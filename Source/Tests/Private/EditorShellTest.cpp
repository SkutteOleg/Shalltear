#include "TestsPCH.h"

#include "Checkout.h"
#include "UITestUtil.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/BuildInfo.h"
#include "Shalltear/Core/Graphics/RenderSystem.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/Time.h"
#include "Shalltear/Core/UI/Context.h"
#include "Shalltear/Core/UI/UISystem.h"
#include "Shalltear/Core/UI/VectorAsset.h"
#include "Shalltear/Core/Window.h"

#include "Shalltear/Editor/CompileService.h"
#include "Shalltear/Editor/EditorConsole.h"
#include "Shalltear/Editor/EditorFonts.h"
#include "Shalltear/Editor/EditorIcons.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/EditorSelection.h"
#include "Shalltear/Editor/EditorTasks.h"
#include "Shalltear/Editor/EditorTheme.h"
#include "Shalltear/Editor/EngineVersionOps.h"
#include "Shalltear/Editor/PanelManager.h"
#include "Shalltear/Editor/PlayModeController.h"

#include "EditorShell.h"

#include <filesystem>
#include <fstream>

using namespace Shalltear;

namespace {

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

	constexpr f32 ONE_FRAME = 1.0f / 144.0f;

	bool AnyTextContains(UI::Context& context, UI::ElementHandle root, std::string_view needle) {
		if (context.GetTextString(root).find(needle) != std::string_view::npos) {
			return true;
		}
		const UI::Element* pod = context.GetElement(root);
		if (!pod) {
			return false;
		}
		for (u32 child = pod->firstChild; child != UI::INVALID_UI_INDEX;) {
			UI::ElementHandle handle = context.MakeHandle(child);
			if (AnyTextContains(context, handle, needle)) {
				return true;
			}
			child = context.GetElement(handle)->nextSibling;
		}
		return false;
	}

	class EditorShellTest : public ::testing::Test {
	protected:
		Handle<Window> _window;
		PlayModeController _playMode;
		CompileService _compile;
		EditorShell _shell;

		void SetUp() override {
			TypeRegistry::InitializePrimitives();
			std::filesystem::path engineAssets = FindRepoDir("EngineAssets");
			ASSERT_FALSE(engineAssets.empty()) << std::filesystem::current_path().string();
			FileSystem::Mount("engine", Path(engineAssets.string()));
			JobSystem::Initialize();
			LogManager::Initialize({false, false});
			AssetDatabase::Initialize();
			SceneSystem::Initialize();
			WindowSystem::Initialize();
			RenderSystem::Initialize();
			UISystem::Initialize();
			UISystem::SetContextInitializer(
				[](UI::Context& context) { EditorTheme::ApplyToContext(context); });
			EditorConsole::Initialize();
			EditorSceneOps::Initialize();
			EditorSelection::Initialize();
			PanelManager::Initialize();
			RegisterBuiltInPanels();

			WindowDesc desc;
			desc.title = "EditorShellTest";
			desc.clientSize = {1280, 720};
			desc.flags = WindowFlags::Hidden;
			_window = WindowSystem::Create(desc);
			ASSERT_TRUE(_window.IsValid());
			PanelManager::SetDefaultWindow(_window);

			_shell.Initialize(_window, "ShellTest", nullptr, &_playMode, &_compile, {});
			ASSERT_TRUE(_shell.IsBuilt());
			Frame();
		}

		void TearDown() override {
			_shell.Terminate();
			PanelManager::Terminate();
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

		void Frame(f32 delta = ONE_FRAME) {
			_shell.Tick(delta);
			Context().Update();
		}

		bool ChromeSays(std::string_view needle) {
			UI::Context& context = Context();
			return AnyTextContains(context, context.GetRoot(), needle);
		}
	};

} // anonymous namespace

TEST_F(EditorShellTest, PlayStateShowsNextFrame) {
	EXPECT_TRUE(ChromeSays("editing"));
	EXPECT_FALSE(ChromeSays("PLAYING"));

	_playMode.RequestToggle();
	_playMode.PumpMainThread();
	ASSERT_TRUE(_playMode.IsPlaying());

	Frame();
	EXPECT_TRUE(ChromeSays("PLAYING"));
	EXPECT_FALSE(ChromeSays("editing"));

	_playMode.RequestToggle();
	_playMode.PumpMainThread();
	ASSERT_FALSE(_playMode.IsPlaying());
	Frame();
	EXPECT_TRUE(ChromeSays("editing"));
	EXPECT_FALSE(ChromeSays("PLAYING"));
}

TEST_F(EditorShellTest, PauseReachesChromeOnFrameToo) {
	_playMode.RequestToggle();
	_playMode.PumpMainThread();
	Frame();
	ASSERT_TRUE(ChromeSays("PLAYING"));

	_playMode.RequestPause();
	_playMode.PumpMainThread();
	ASSERT_TRUE(_playMode.IsPaused());
	Frame();
	EXPECT_TRUE(ChromeSays("PAUSED"));
	EXPECT_FALSE(ChromeSays("PLAYING"));
}

TEST_F(EditorShellTest, BarCountsAssetSelection) {
	EXPECT_TRUE(ChromeSays("0 selected"));

	Guid assets[2] = {Guid{1, 1}, Guid{2, 2}};
	EditorSelection::SelectAssets({assets, 2});
	Frame();
	EXPECT_TRUE(ChromeSays("2 selected"));

	EditorSelection::Clear();
	Frame();
	EXPECT_TRUE(ChromeSays("0 selected"));
}

TEST_F(EditorShellTest, FrameRateWaitsForWindow) {
	EXPECT_FALSE(ChromeSays("FPS"));
	for (u32 frame = 0; frame < 4; ++frame) {
		Frame(0.1f);
	}
	EXPECT_TRUE(ChromeSays("FPS"));
}

TEST_F(EditorShellTest, StatusBarNamesApiAtOnce) {
	EXPECT_TRUE(ChromeSays("Vulkan"));
}

TEST_F(EditorShellTest, StatusBarShowsBackgroundTasks) {
	UI::Context& context = Context();
	static char owner = 0;
	u32 cancelled = 0;
	EXPECT_FALSE(ChromeSays("Downloading SDK"));

	const u64 task = EditorTasks::Begin({"Downloading SDK", [&cancelled]() { ++cancelled; }}, &owner);
	Frame();
	EXPECT_TRUE(ChromeSays("Downloading SDK"));
	const UI::ElementHandle label = UITest::FindByText(context, context.GetRoot(), "Downloading SDK");
	ASSERT_TRUE(context.IsAlive(label));
	const UI::ElementHandle notice = context.MakeHandle(context.GetElement(label)->parent);
	const UI::ElementHandle track = context.MakeHandle(context.GetElement(label)->nextSibling);
	const UI::ElementHandle fill = context.MakeHandle(context.GetElement(track)->firstChild);
	const auto shown = [&](UI::ElementHandle element) { return HasFlag(context.GetElement(element)->flags, UI::ElementFlags::Visible); };
	EXPECT_TRUE(shown(notice));
	EXPECT_FALSE(shown(track));

	EditorTasks::Report(task, "Downloading SDK (53 of 212 MB)", 0.25f);
	Frame();
	EXPECT_TRUE(ChromeSays("Downloading SDK (53 of 212 MB)"));
	EXPECT_TRUE(shown(track));
	EXPECT_NEAR(context.GetLayout(fill).borderBox.w, context.GetLayout(track).borderBox.w * 0.25f, 1.0f);

	const u64 other = EditorTasks::Begin({"Baking", {}}, &owner);
	Frame();
	EXPECT_TRUE(ChromeSays("Downloading SDK (53 of 212 MB) (+1)"));

	const UI::ElementHandle cancel = UITest::FindByText(context, context.GetRoot(), "Cancel");
	ASSERT_TRUE(context.IsAlive(cancel));
	EXPECT_TRUE(shown(cancel));
	UITest::ClickElement(context, cancel);
	EXPECT_EQ(cancelled, 1u);

	EditorTasks::End(task);
	Frame();
	EXPECT_TRUE(ChromeSays("Baking"));
	EXPECT_FALSE(ChromeSays("Downloading"));
	EXPECT_FALSE(shown(cancel));
	EXPECT_FALSE(shown(track));
	UITest::ClickElement(context, cancel);
	EXPECT_EQ(cancelled, 1u);

	EditorTasks::End(other);
	Frame();
	EXPECT_FALSE(shown(notice));
	EXPECT_EQ(EditorTasks::UnregisterOwnedBy(&owner), 0u);
}

TEST_F(EditorShellTest, TitleNamesCommit) {
	const str stamp = std::format("Shalltear {}", EngineVersionOps::DisplayLabel());
	EXPECT_TRUE(ChromeSays(stamp)) << stamp;
}

TEST_F(EditorShellTest, TitleDoesNotShowToolingVersion) {
	EXPECT_FALSE(ChromeSays(str("Shalltear ") + BuildInfo::GetVersionString()));
}

TEST_F(EditorShellTest, PressableChromeDeclaresCursor) {
	UI::Context& context = Context();
	Vector<str> offenders{GetMemoryResource(MemTag::Editor)};
	UITest::CollectCursorOffenders(context, context.GetRoot(), offenders);
	EXPECT_TRUE(offenders.empty()) << UITest::ReportCursorOffenders(offenders);
}

TEST_F(EditorShellTest, ChromeShowsPressSameFrame) {
	UI::Context& context = Context();
	UITest::PressWalk walk;
	UITest::WalkThePress(context, context.GetRoot(), walk, [this]() { Frame(); });
	EXPECT_GT(walk.inspected, 5u);
	EXPECT_TRUE(walk.Inspected(StringHash("ed-tool-button")));
	EXPECT_TRUE(walk.offenders.empty()) << walk.Report();
}

namespace {

	UI::ElementHandle FindWearing(UI::Context& context, UI::ElementHandle root, EditorIcons::Icon icon) {
		Handle<VectorAsset> glyph = EditorIcons::GetGlyph(icon);
		if (!glyph.IsValid()) {
			return {};
		}
		if (context.HasIcon(root) && context.GetIconAsset(root) == glyph) {
			return root;
		}
		const UI::Element* pod = context.GetElement(root);
		for (u32 child = pod ? pod->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
			UI::ElementHandle handle = context.MakeHandle(child);
			if (UI::ElementHandle found = FindWearing(context, handle, icon); found.IsValid()) {
				return found;
			}
			child = context.GetElement(handle)->nextSibling;
		}
		return {};
	}

} // anonymous namespace

TEST_F(EditorShellTest, FailedCompilePressSkipsConsole) {
	UI::Context& context = Context();
	ASSERT_EQ(PanelManager::GetOpenPanelCount(), 0u);

	std::filesystem::path project = std::filesystem::temp_directory_path() /
									("shalltear_shell_failed_" + std::to_string(Time::GetTicks()));
	std::filesystem::create_directories(project);
	{
		std::ofstream ini(project / "Project.ini", std::ios::binary);
		ini << "[Project]\nname = Failing\n[Script]\ncompileCommand = Z:/no/such/compiler.exe\n";
	}
	CompileRequest request;
	request.projectPath = project.generic_string();
	EXPECT_FALSE(_compile.RequestCompile(request));
	ASSERT_EQ(_compile.GetState(), CompileState::Failed);
	Frame();

	UI::ElementHandle face = FindWearing(context, context.GetRoot(), EditorIcons::Glyph::Compile);
	ASSERT_TRUE(face.IsValid());
	const UI::Element* facePod = context.GetElement(face);
	ASSERT_NE(facePod, nullptr);
	UI::ElementHandle button = context.MakeHandle(facePod->parent);
	ASSERT_TRUE(context.IsAlive(button));

	UITest::ClickElement(context, button);
	Frame();
	EXPECT_EQ(PanelManager::GetOpenPanelCount(), 0u);

	PanelManager::CloseAllPanels();
	Frame();
	std::error_code ec;
	std::filesystem::remove_all(project, ec);
}

namespace {

	UI::ElementHandle RowNamed(UI::Context& context, UI::ElementHandle popup, std::string_view label) {
		const UI::Element* element = context.GetElement(popup);
		for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
			UI::ElementHandle handle = context.MakeHandle(child);
			if (context.HasClass(handle, StringHash("slt-menu-item")) &&
				UITest::FindByText(context, handle, label).IsValid()) {
				return handle;
			}
			child = context.GetElement(handle)->nextSibling;
		}
		return {};
	}

	void ClickWithMods(UI::Context& context, UI::ElementHandle element, KeyMod mods) {
		const UI::Rect rect = context.GetLayout(element).borderBox;
		InputEvent event{};
		event.mods = mods;
		event.button = MouseButton::Left;
		event.mousePosPx = {static_cast<i32>(rect.x + rect.w * 0.5f), static_cast<i32>(rect.y + rect.h * 0.5f)};
		event.type = InputEventType::MouseMove;
		context.InjectInput(event);
		event.type = InputEventType::MouseDown;
		context.InjectInput(event);
		event.type = InputEventType::MouseUp;
		context.InjectInput(event);
		context.Update();
	}

} // anonymous namespace

TEST_F(EditorShellTest, WindowMenuRecyclesPanel) {
	UI::Context& context = Context();
	ASSERT_EQ(PanelManager::GetOpenPanelCount(), 0u);

	auto openWindowMenu = [&]() -> UI::ElementHandle {
		UI::ElementHandle bar = UITest::FindByText(context, context.GetRoot(), "Window");
		EXPECT_TRUE(bar.IsValid());
		UITest::ClickElement(context, bar);
		Frame();
		EXPECT_EQ(context.GetOpenPopupCount(), 1u);
		return context.GetOpenPopup(0);
	};

	UI::ElementHandle menu = openWindowMenu();
	ASSERT_TRUE(menu.IsValid());
	EXPECT_TRUE(RowNamed(context, menu, "Console").IsValid());
	EXPECT_TRUE(UITest::FindByTextContaining(context, menu, "Shift").IsValid());

	UI::ElementHandle row = RowNamed(context, menu, "Console");
	ASSERT_TRUE(row.IsValid());
	UITest::ClickElement(context, row);
	Frame();
	EXPECT_EQ(PanelManager::GetOpenPanelCount(), 1u);

	menu = openWindowMenu();
	row = RowNamed(context, menu, "Console");
	ASSERT_TRUE(row.IsValid());
	UITest::ClickElement(context, row);
	Frame();
	EXPECT_EQ(PanelManager::GetOpenPanelCount(), 1u);

	menu = openWindowMenu();
	row = RowNamed(context, menu, "Console");
	ASSERT_TRUE(row.IsValid());
	ClickWithMods(context, row, KeyMod::Shift);
	Frame();
	EXPECT_EQ(PanelManager::GetOpenPanelCount(), 2u);

	menu = openWindowMenu();
	row = RowNamed(context, menu, "Console");
	ASSERT_TRUE(row.IsValid());
	UITest::ClickElement(context, row);
	Frame();
	EXPECT_EQ(PanelManager::GetOpenPanelCount(), 2u);

	PanelManager::CloseAllPanels();
	Frame();
}

TEST_F(EditorShellTest, CategorizedPanelInSubmenu) {
	UI::Context& context = Context();
	ASSERT_EQ(PanelManager::GetOpenPanelCount(), 0u);

	UI::ElementHandle bar = UITest::FindByText(context, context.GetRoot(), "Window");
	ASSERT_TRUE(bar.IsValid());
	UITest::ClickElement(context, bar);
	Frame();
	ASSERT_EQ(context.GetOpenPopupCount(), 1u);
	UI::ElementHandle menu = context.GetOpenPopup(0);

	EXPECT_TRUE(RowNamed(context, menu, "Hierarchy").IsValid());
	EXPECT_FALSE(RowNamed(context, menu, "Profiler").IsValid());

	UI::ElementHandle analysis = RowNamed(context, menu, "Analysis");
	ASSERT_TRUE(analysis.IsValid());
	UITest::ClickElement(context, analysis);
	Frame();
	ASSERT_EQ(context.GetOpenPopupCount(), 2u);

	UI::ElementHandle profiler = RowNamed(context, context.GetOpenPopup(1), "Profiler");
	ASSERT_TRUE(profiler.IsValid());
	UITest::ClickElement(context, profiler);
	Frame();
	EXPECT_EQ(PanelManager::GetOpenPanelCount(), 1u);

	PanelManager::CloseAllPanels();
	Frame();
}

TEST(EditorPanelCategoryTest, OptionalPanelsDeclareCategory) {
	const std::filesystem::path root = Checkout::Root();
	ASSERT_FALSE(root.empty()) << std::filesystem::current_path().string();
	std::filesystem::path source = root / "Source";
	ASSERT_TRUE(std::filesystem::exists(source)) << source.string();

	const std::initializer_list<std::string_view> defaultLayout{
		"Hierarchy", "Inspector", "Console", "Asset Browser", "Scene View", "Runtime View"};

	Vector<str> offenders{GetMemoryResource(MemTag::Editor)};
	u32 scanned = 0;
	std::error_code ec;
	for (const auto& tree : std::filesystem::directory_iterator(source, ec)) {
		const str treeName = tree.path().filename().string();
		if (!tree.is_directory() || treeName.size() < 6 ||
			treeName.compare(treeName.size() - 6, 6, "Editor") != 0) {
			continue;
		}
		for (const auto& entry : std::filesystem::recursive_directory_iterator(tree.path(), ec)) {
			const str extension = entry.path().extension().string();
			if (extension != ".h" && extension != ".cpp") {
				continue;
			}
			std::ifstream file(entry.path());
			std::stringstream buffer;
			buffer << file.rdbuf();
			const str text = buffer.str();
			for (size_t at = text.find("SLT_EDITOR_PANEL_NAME("); at != str::npos;
				 at = text.find("SLT_EDITOR_PANEL_NAME(", at + 1)) {
				size_t lineStart = text.rfind('\n', at);
				lineStart = (lineStart == str::npos) ? 0 : lineStart + 1;
				if (text.find("//", lineStart) < at) {
					continue;
				}
				size_t open = text.find('"', at);
				size_t close = text.find('"', open + 1);
				if (open == str::npos || close == str::npos) {
					continue;
				}
				const str name = text.substr(open + 1, close - open - 1);
				++scanned;
				if (std::find(defaultLayout.begin(), defaultLayout.end(), std::string_view(name)) !=
					defaultLayout.end()) {
					continue;
				}
				size_t blockEnd = text.find("SLT_END_REFLECT", close);
				size_t category = text.find("SLT_EDITOR_PANEL_CATEGORY(", close);
				if (category == str::npos || (blockEnd != str::npos && category > blockEnd)) {
					offenders.push_back(entry.path().filename().string() + ": " + name);
				}
			}
		}
	}

	ASSERT_GT(scanned, 10u) << "only " << scanned << ' ' << source.string();

	str report;
	for (const str& offender : offenders) {
		report += (report.empty() ? "" : ", ") + offender;
	}
	EXPECT_TRUE(offenders.empty()) << report;
}
