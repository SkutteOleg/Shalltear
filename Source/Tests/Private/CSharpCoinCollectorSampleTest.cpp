#include "TestsPCH.h"

#if defined(SHALLTEAR_SHARED)

#include "CSharpConformance.h"
#include "CSharpRuntimeFixture.h"
#include "Checkout.h"
#include "EditorTestHooks.h"
#include "TestPlatform.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Frame/FrameStages.h"
#include "Shalltear/Core/Input/InputEvent.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/Settings.h"
#include "Shalltear/Core/Time.h"
#include "Shalltear/Core/UI/Context.h"
#include "Shalltear/Core/UI/UIPanelRenderer.h"
#include "Shalltear/Core/Window.h"
#include "Shalltear/Core/Window/FrameLoop.h"

#include "Shalltear/Editor/EditorConsole.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <thread>

using namespace Shalltear;
using namespace Shalltear::CSharpEditor;
using namespace Shalltear::CSharpTests;

namespace {

	namespace fs = std::filesystem;

	constexpr f64 FRAME_SECONDS = 1.0 / 60.0;

	class CSharpCoinCollectorFixture : public CSharpRuntimeFixture {
	protected:
		const EditorTestHooks* _editor = nullptr;
		fs::path _root;
		Handle<Window> _window;
		std::optional<CSharpConformance::UIRig> _ui;

		bool LoadPackages() override { return CSharpConformance::StandUpPackages(nullptr) && CSharpConformance::StandUpAudio(nullptr); }

		void SetUp() override {
			const fs::path sample = Checkout::Find("Samples/CoinCollectorCSharp");
			_root = fs::temp_directory_path() / ("shalltear_csharp_coins_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			std::error_code ec;
			fs::create_directories(_root, ec);
			if (!sample.empty()) {
				for (const char* part : {"Assets", "BuildProfiles"}) {
					fs::copy(sample / part, _root / part, fs::copy_options::recursive, ec);
				}
				for (const char* file : {"Project.ini", "ProjectSettings.ini", "CSharp.props"}) {
					fs::copy_file(sample / file, _root / file, ec);
				}
			}
			const Path settings((_root / "ProjectSettings.ini").generic_string());
			FileSystem::WriteTextFile(settings, FileSystem::ReadTextFile(settings, MemTag::Editor) + "\n[Audio]\ndevice = None\n");
			SettingsPaths paths;
			paths.project = settings;
			paths.user = Path((_root / "UserSettings.ini").generic_string());
			paths.userProject = Path((_root / "UserProjectSettings.ini").generic_string());
			Settings::Initialize(paths);

			CSharpRuntimeFixture::SetUp();
			ASSERT_FALSE(sample.empty());
			const auto get = reinterpret_cast<GetEditorTestHooksFn>(
				ModuleSystem::FindSymbol(ModuleSystem::GetNativeHandle(_package), EDITOR_TEST_HOOKS_SYMBOL));
			ASSERT_NE(get, nullptr);
			_editor = get();
			const fs::path engineAssets = Checkout::Find("EngineAssets");
			ASSERT_FALSE(engineAssets.empty());
			FileSystem::Mount("engine", Path(engineAssets.generic_string()));
			FileSystem::Mount("project", Path(_root.generic_string()));
			EditorConsole::Initialize();
			SceneSystem::Initialize();
			ASSERT_TRUE(Attach()) << _error;
			AssetDatabase::Initialize();
			AssetDatabase::ScanProjectDirectory();

			WindowSystem::Initialize();
			WindowDesc desc;
			desc.title = "CoinCollectorCSharp";
			desc.clientSize = {320, 240};
			desc.flags = WindowFlags::Hidden;
			_window = WindowSystem::Create(desc);
			TestPlatform::PostFocus(_window, true);
			WindowSystem::PumpEvents();
			ASSERT_TRUE(WindowSystem::GetFocusedWindow().IsValid());
			_ui.emplace();
			ASSERT_TRUE(_ui->IsReady());

			const str sdk = _hooks->findSDKRoot();
			if (sdk.empty()) {
				GTEST_SKIP() << "pinned .NET SDK not found";
			}
			CSharpProjectFiles::Result written;
			ASSERT_TRUE(_editor->writeProjectFiles(_editor->getDefaultProjectOptions(_root.generic_string(), sdk), written));
			CompileOptions options;
			options.projectPath = _root.generic_string();
			options.projectFile = "CoinCollectorCSharp.Scripts.csproj";
			options.muxer = (fs::path(sdk) / _editor->muxerName).generic_string();
			options.pinStamp = "coins";
			options.buildServers = false;
			_editor->configureCompile(options);
			ASSERT_TRUE(_editor->requestCompile());
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
			while (_editor->getCompile().IsBusy() && std::chrono::steady_clock::now() < deadline) {
				_editor->pumpCompile();
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			ASSERT_EQ(_editor->getCompile().GetState(), CompileState::Succeeded) << _log.Delivered().Joined();
			_editor->beginReload(_editor->getCompileOutput());
			_editor->requestReload();
			_editor->pumpReload();
			ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded) << _log.Delivered().Joined();
		}

		void TearDown() override {
			if (_editor) {
				_editor->shutdownReload();
			}
			SceneSystem::Terminate();
			while (AssetDatabase::CollectGarbage() > 0) {
			}
			_ui.reset();
			_window = {};
			WindowSystem::Terminate();
			AssetDatabase::Terminate();
			EditorConsole::Terminate();
			CSharpRuntimeFixture::TearDown();
			Settings::Terminate();
			FileSystem::Unmount("project");
			FileSystem::Unmount("engine");
			std::error_code ec;
			fs::remove_all(_root, ec);
		}

		static Guid SceneGuid(const char* name) { return AssetDatabase::LookupGuid(Path(str("project://Assets/Scenes/") + name + ".scene")); }

		static str PrimaryName() {
			Scene* scene = SceneSystem::GetPrimaryScene().Get();
			return scene ? scene->name : str();
		}

		static UIPanelRenderer* Panel(const char* objectName) {
			Scene* scene = SceneSystem::GetPrimaryScene().Get();
			Handle<SceneObject> object = scene ? scene->FindObjectByName(objectName) : Handle<SceneObject>();
			UIPanelRenderer* panel = object.IsValid() ? object->GetComponent<UIPanelRenderer>().Get() : nullptr;
			return panel && panel->GetContext() ? panel : nullptr;
		}

		static str Text(UIPanelRenderer& panel, std::string_view id) { return panel.GetContext()->GetTextString(panel.FindElement(id)); }

		static UI::Display Display(UIPanelRenderer& panel, std::string_view id) { return panel.GetContext()->GetStyle(panel.FindElement(id)).display; }

		static void Click(UIPanelRenderer& panel, std::string_view id) {
			UI::Context* context = panel.GetContext();
			context->Update();
			const UI::Rect rect = context->GetLayout(panel.FindElement(id)).borderBox;
			const Vec2I point{static_cast<i32>(rect.x + rect.w * 0.5f), static_cast<i32>(rect.y + rect.h * 0.5f)};
			for (const InputEventType type : {InputEventType::MouseMove, InputEventType::MouseDown, InputEventType::MouseUp}) {
				InputEvent event{};
				event.type = type;
				event.button = MouseButton::Left;
				event.mousePosPx = point;
				context->InjectInput(event);
			}
			context->Update();
		}

		void RunFrame(FixedTimestep& fixed) {
			WindowSystem::PumpEvents();
			Time::BeginFrame(FRAME_SECONDS);
			SceneSystem::BeginFrame();
			const FrameTime& frame = Time::GetFrame();
			fixed.Configure(frame.fixedDeltaTime, frame.maximumDeltaTime);
			const u32 steps = fixed.Advance(frame.ComputeGameDelta());
			FrameStageArgs args;
			for (FrameStage stage : FRAME_STAGES) {
				if (stage == FrameStage::FixedUpdate) {
					for (u32 i = 0; i < steps; ++i) {
						FrameStages::Run(stage, args);
					}
				} else if (stage != FrameStage::Render) {
					FrameStages::Run(stage, args);
				}
			}
			AssetDatabase::CollectGarbage();
		}

		void RunSeconds(FixedTimestep& fixed, f64 seconds) {
			for (f64 elapsed = 0.0; elapsed < seconds; elapsed += FRAME_SECONDS) {
				RunFrame(fixed);
			}
		}
	};

} // anonymous namespace

TEST_F(CSharpCoinCollectorFixture, MenuLevelsChainBackToMenu) {
	Time::FrameScope clock{FrameTime{}};
	FixedTimestep fixed(Time::GetFrame().fixedDeltaTime, Time::GetFrame().maximumDeltaTime);

	ASSERT_TRUE(SceneSystem::LoadSceneByGuid(SceneGuid("Main")).IsValid());
	RunFrame(fixed);
	UIPanelRenderer* menu = Panel("Menu");
	ASSERT_NE(menu, nullptr);
	Click(*menu, "play");
	RunFrame(fixed);
	ASSERT_EQ(PrimaryName(), "Level1") << _log.Delivered().Joined();

	RunFrame(fixed);
	UIPanelRenderer* hud = Panel("Level");
	ASSERT_NE(hud, nullptr);
	EXPECT_EQ(Text(*hud, "coins"), "Coins: 0 / 5");
	EXPECT_EQ(Text(*hud, "timer"), "Time: 30");
	EXPECT_EQ(Display(*hud, "pause-menu"), UI::Display::None);

	TestPlatform::PostKey(_window, Key::Escape, true);
	RunFrame(fixed);
	TestPlatform::PostKey(_window, Key::Escape, false);
	RunSeconds(fixed, 2.0);
	EXPECT_EQ(Display(*hud, "pause-menu"), UI::Display::Flex);
	EXPECT_EQ(Text(*hud, "timer"), "Time: 30");
	Click(*hud, "resume");
	RunFrame(fixed);
	EXPECT_EQ(Display(*hud, "pause-menu"), UI::Display::None);

	Scene* level = SceneSystem::GetPrimaryScene().Get();
	Handle<SceneObject> player = level->FindObjectByName("Player");
	ASSERT_TRUE(player.IsValid());
	Vector<Handle<SceneObject>> coins{GetMemoryResource(MemTag::Scene)};
	struct Collect {
		Vector<Handle<SceneObject>>* out;
		static void Visit(SceneObject& node, void* user) {
			if (node.name == "Coin") {
				static_cast<Collect*>(user)->out->push_back(Handle<SceneObject>(&node));
			}
		}
	} collect{&coins};
	level->ForEachObject(&Collect::Visit, &collect);
	ASSERT_EQ(coins.size(), 5u);
	const Quat before = coins[0]->GetLocalRotation();
	RunSeconds(fixed, 0.5);
	const Quat after = coins[0]->GetLocalRotation();
	EXPECT_LT(std::abs(before.x * after.x + before.y * after.y + before.z * after.z + before.w * after.w), 0.99f);

	for (size_t i = 0; i < coins.size(); ++i) {
		ASSERT_EQ(PrimaryName(), "Level1") << i << " coins";
		EXPECT_EQ(Text(*hud, "coins"), std::format("Coins: {} / 5", i));
		const Vec3 at = coins[i]->GetLocalPosition();
		player->SetLocalPosition(Vec3{at.x, 1.0f, at.z});
		for (i32 frame = 0; frame < 30 && coins[i].IsValid(); ++frame) {
			RunFrame(fixed);
		}
		EXPECT_FALSE(coins[i].IsValid()) << i;
		RunSeconds(fixed, 0.1);
	}
	EXPECT_EQ(PrimaryName(), "Level2");

	RunSeconds(fixed, 20.0);
	EXPECT_EQ(PrimaryName(), "Level2");
	RunSeconds(fixed, 6.0);
	EXPECT_EQ(PrimaryName(), "Main");
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Warn), 0u) << _log.Delivered().Joined();
}

#endif
