#include "TestsPCH.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Graphics/RenderSystem.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/UI/Context.h"
#include "Shalltear/Core/UI/Docking.h"
#include "Shalltear/Core/UI/RenderList.h"
#include "Shalltear/Core/UI/UISystem.h"
#include "Shalltear/Core/Window.h"

#include "Shalltear/Editor/EditorConsole.h"
#include "Shalltear/Editor/EditorFonts.h"
#include "Shalltear/Editor/EditorIcons.h"
#include "Shalltear/Editor/EditorLayoutOps.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/EditorSelection.h"
#include "Shalltear/Editor/EditorTheme.h"
#include "Shalltear/Editor/PanelManager.h"
#include "Shalltear/Editor/PropertyDrawers.h"
#include "Shalltear/Editor/UndoSystem.h"

using namespace Shalltear;

namespace {

	constexpr Vec2I ClientSize = {1600, 900};
	constexpr f32 CONTENT_SCALE = 1.0f;
	constexpr f32 FRAME_DT = 1.0f / 60.0f;

	constexpr u32 SETTLE_FRAMES = 30;

	constexpr u64 ELEMENTS_VISITED_BUDGET = 144;
	constexpr u64 QUADS_BUDGET = 634;
	constexpr u64 DRAW_CALLS_BUDGET = 77;

	std::filesystem::path FindRepoDir(const char* relative) {
		std::error_code ec;
		std::filesystem::path probe = std::filesystem::current_path(ec);
		for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
			std::filesystem::path candidate = probe / relative;
			if (std::filesystem::exists(candidate, ec)) {
				return candidate;
			}
			std::filesystem::path parent = probe.parent_path();
			if (parent == probe) {
				break;
			}
			probe = parent;
		}
		return {};
	}

	struct FrameCost {
		u64 elementsVisited = 0;
		u64 quads = 0;
		u64 drawCalls = 0;

		bool operator==(const FrameCost&) const = default;
	};

	class EditorIdleFrameCostTest : public ::testing::Test {
	protected:
		Handle<Window> _window;
		UI::RenderList _list;

		void SetUp() override {
			TypeRegistry::InitializePrimitives();
			std::filesystem::path engineAssets = FindRepoDir("EngineAssets");
			ASSERT_FALSE(engineAssets.empty()) << std::filesystem::current_path().string();
			FileSystem::Mount("engine", Path(engineAssets.string()));
			AssetDatabase::Initialize();
			SceneSystem::Initialize();
			WindowSystem::Initialize();
			RenderSystem::Initialize();
			UISystem::Initialize();
			UISystem::SetContextInitializer(
				[](UI::Context& context) { EditorTheme::ApplyToContext(context); });

			JobSystem::Initialize();
			LogManager::Initialize({false, false});
			LogManager::SetGlobalLevel(LogLevel::Trace);
			EditorConsole::Initialize();
			EditorSceneOps::Initialize();
			EditorSelection::Initialize();
			UndoSystem::Initialize();
			PropertyDrawers::Initialize();
			PanelManager::Initialize();
			RegisterBuiltInPanels();
			if (!RenderSystem::IsRenderingAvailable()) {
				GTEST_SKIP() << "no Vulkan device available";
			}

			WindowDesc desc;
			desc.title = "EditorIdleFrameCost";
			desc.clientSize = ClientSize;
			desc.flags = WindowFlags::Hidden;
			_window = WindowSystem::Create(desc);
			ASSERT_TRUE(_window.IsValid());
			ASSERT_NE(UISystem::CreateDockManager(_window), nullptr);
			PanelManager::SetDefaultWindow(_window);

			UI::Context* context = UISystem::GetContext(_window);
			ASSERT_NE(context, nullptr);
			context->SetContentScale(CONTENT_SCALE);
			context->Resize(ClientSize);

			EditorLayoutOps::BuildDefaultLayout(_window);
			for (u32 frame = 0; frame < SETTLE_FRAMES; ++frame) {
				Frame();
			}
		}

		void TearDown() override {
			PanelManager::Terminate();
			PropertyDrawers::Terminate();
			UndoSystem::Terminate();
			EditorSelection::Terminate();
			EditorSceneOps::Terminate();
			EditorConsole::Terminate();
			UISystem::SetContextInitializer(nullptr);
			EditorIcons::Shutdown();
			EditorFonts::Shutdown();
			UISystem::Terminate();
			RenderSystem::Terminate();
			WindowSystem::Terminate();
			SceneSystem::Terminate();
			AssetDatabase::Terminate();
			LogManager::Terminate();
			JobSystem::Terminate();
			FileSystem::Unmount("engine");
		}

		UI::Context& Context() { return *UISystem::GetContext(_window); }

		FrameCost Frame() {
			PanelManager::TickAll(FRAME_DT);
			UISystem::Update();

			UI::Context& context = Context();
			context.ResetPaintStats();
			_list.Clear();
			context.BuildRenderList(_list);

			FrameCost cost;
			cost.elementsVisited = context.GetPaintStats().elementsVisited;
			cost.quads = _list.quads.size();
			for (const UI::UICommand& command : _list.commands) {
				if (command.kind == UI::UICommandKind::Quads && command.quadCount > 0) {
					++cost.drawCalls;
				}
			}
			return cost;
		}

		u32 CountTree() {
			u32 count = 0;
			CountSubtree(Context(), Context().GetRoot(), count);
			return count;
		}

		static void CountSubtree(UI::Context& context, UI::ElementHandle handle, u32& count) {
			const UI::Element* element = context.GetElement(handle);
			if (!element) {
				return;
			}
			++count;
			for (u32 child = element->firstChild; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle childHandle = context.MakeHandle(child);
				CountSubtree(context, childHandle, count);
				child = context.GetElement(childHandle)->nextSibling;
			}
		}
	};

	void ExpectTheLayoutIsReallyThere(const FrameCost& cost, u32 treeSize) {
		EXPECT_EQ(PanelManager::GetOpenPanelCount(), 6u);
		EXPECT_GT(treeSize, 80u);
		EXPECT_GT(cost.quads, 300u);
		EXPECT_GT(cost.drawCalls, 20u);
	}

} // anonymous namespace

TEST_F(EditorIdleFrameCostTest, DefaultLayoutIdleFrameCost) {
	FrameCost cost = Frame();
	ExpectTheLayoutIsReallyThere(cost, CountTree());
	RecordProperty("elementsVisited", static_cast<int>(cost.elementsVisited));
	RecordProperty("quads", static_cast<int>(cost.quads));
	RecordProperty("drawCalls", static_cast<int>(cost.drawCalls));

	EXPECT_LE(cost.elementsVisited, ELEMENTS_VISITED_BUDGET);
	EXPECT_LE(cost.quads, QUADS_BUDGET);
	EXPECT_LE(cost.drawCalls, DRAW_CALLS_BUDGET);
}

TEST_F(EditorIdleFrameCostTest, IdleFrameCostIsSteady) {
	FrameCost first = Frame();
	u32 firstTree = CountTree();
	ExpectTheLayoutIsReallyThere(first, firstTree);
	std::vector<UI::UIQuad> firstQuads(_list.quads.begin(), _list.quads.end());

	for (u32 frame = 1; frame < 60; ++frame) {
		FrameCost cost = Frame();
		ASSERT_EQ(CountTree(), firstTree);
		ASSERT_EQ(cost, first);
		ASSERT_EQ(_list.quads.size(), firstQuads.size());
		ASSERT_TRUE(std::equal(firstQuads.begin(), firstQuads.end(), _list.quads.begin(),
							   [](const UI::UIQuad& a, const UI::UIQuad& b) {
								   return std::memcmp(&a, &b, sizeof(UI::UIQuad)) == 0;
							   }));
	}
}

TEST_F(EditorIdleFrameCostTest, HiddenLogLinesCostNothing) {
	u32 startCount = EditorConsole::GetBufferedCount();
	FrameCost before = Frame();
	ExpectTheLayoutIsReallyThere(before, CountTree());
	u32 treeBefore = CountTree();
	for (u32 line = 0; line < 1000; ++line) {
		SLT_LOG_INFO("EditorIdleFrameCost", "a log line nobody is looking at, number {}", line);
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (EditorConsole::GetBufferedCount() < startCount + 1000 &&
		   std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	u32 delivered = EditorConsole::GetBufferedCount();
	ASSERT_GE(delivered, startCount + 1000);

	FrameCost after = Frame();
	EXPECT_EQ(after, before);
	EXPECT_EQ(CountTree(), treeBefore);
}
