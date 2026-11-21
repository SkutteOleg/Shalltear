#include "TestsPCH.h"

#if defined(SHALLTEAR_SHARED)

#include "CSharpRuntimeFixture.h"
#include "CSharpScenarioUtil.h"
#include "CSharpSettings.h"
#include "EditorTestHooks.h"
#include "ScopedSettingsStore.h"
#include "TestPlatform.h"
#include "UITestUtil.h"

#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Module/ScriptFunctions.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/Scene/UnresolvedComponent.h"
#include "Shalltear/Core/Settings.h"
#include "Shalltear/Core/UI/DragDrop.h"
#include "Shalltear/Core/UI/UISystem.h"

#include "Shalltear/Editor/BuildPipeline.h"
#include "Shalltear/Editor/BuildTarget.h"
#include "Shalltear/Editor/CompileStatus.h"
#include "Shalltear/Editor/EditTarget.h"
#include "Shalltear/Editor/EditorConsole.h"
#include "Shalltear/Editor/EditorEvents.h"
#include "Shalltear/Editor/EditorMenus.h"
#include "Shalltear/Editor/EditorPanel.h"
#include "Shalltear/Editor/PanelManager.h"
#include "Shalltear/Editor/SettingsPresentation.h"
#include "Shalltear/Editor/TypeNameOps.h"
#include "Shalltear/Editor/UndoSystem.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <thread>

using namespace Shalltear;
using namespace Shalltear::CSharpEditor;
using namespace Shalltear::CSharpTests;

namespace {

	namespace fs = std::filesystem;

	constexpr const char* SPINNER_FIRST = "using Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Spinner : Behavior {\n\tpublic float speed = 90f;\n"
										  "\tpublic int count = 3;\n\tpublic string label = \"fresh\";\n\tpublic SceneObject? target;\n}\n";

	constexpr const char* SPINNER_SECOND = "using Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Spinner : Behavior {\n"
										   "\tpublic string label = \"second\";\n\tpublic int bonus = 42;\n\tpublic float speed = 5f;\n\tpublic SceneObject? target;\n}\n";

	constexpr const char* WHIRLER = "using Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Whirler : Behavior {\n\tpublic float speed = 5f;\n}\n";

	constexpr const char* PLAYER_ONLY = "namespace Space_Game;\n\npublic static class PlayerOnly {\n#if SHALLTEAR_PLAYER\n\tpublic static int Broken() {\n"
										"\t\treturn missing;\n\t}\n#endif\n}\n";

	constexpr const char* BOOT_REFUSED = "namespace Space_Game;\n\nstatic class Boot {\n\t[System.Runtime.CompilerServices.ModuleInitializer]\n"
										 "\tinternal static void Start() {\n\t\tthrow new System.InvalidOperationException(\"boot refused\");\n\t}\n}\n";

	constexpr const char* SAVER = "using Shalltear;\n\nnamespace Space_Game;\n\n[KeepName]\npublic enum SaveSlot {\n\tFirst,\n\tSecond,\n}\n\n"
								  "public sealed class Saver : Behavior {\n\tprotected override void OnEnable() {\n\t\tLog.Info(\"Game\", SaveSlot.Second.ToString());\n"
								  "\t}\n}\n";

	str ReadAll(const fs::path& file) {
		std::ifstream stream(file, std::ios::binary);
		return str(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
	}

	bool HoldsWord(const str& bytes, std::string_view word) {
		str wide;
		for (const char c : word) {
			wide.push_back(c);
			wide.push_back('\0');
		}
		for (const str& needle : {str(word), wide}) {
			const size_t step = needle.size() == word.size() ? 1 : 2;
			for (size_t at = bytes.find(needle); at != str::npos; at = bytes.find(needle, at + 1)) {
				const size_t end = at + needle.size();
				const auto joined = [&](size_t index) { return std::isalnum(static_cast<unsigned char>(bytes[index])) || bytes[index] == '_'; };
				if (!(at >= step && joined(at - step)) && !(end < bytes.size() && joined(end))) {
					return true;
				}
			}
		}
		return false;
	}

	constexpr const char* GREETER = "using Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Greeter : Behavior {\n"
									"\tprotected override void OnEnable() {\n\t\tLog.Info(\"Game\", \"greeter on \" + Owner.Name);\n\t}\n}\n";

	constexpr const char* LEAKER = "using System.Threading;\nusing Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Leaker : Behavior {\n"
								   "\tprotected override void OnEnable() {\n\t\tnew Thread(Wait) { IsBackground = true }.Start();\n\t}\n\n"
								   "\tstatic void Wait() {\n\t\twhile (true) {\n\t\t\tThread.Sleep(1000);\n\t\t}\n\t}\n}\n";

	constexpr const char* THROWER = "using System;\nusing System.Runtime.CompilerServices;\n\nnamespace Space_Game;\n\nstatic class Thrower {\n"
									"#pragma warning disable CA2255\n\t[ModuleInitializer]\n\tinternal static void Run() {\n"
									"\t\tthrow new InvalidOperationException(\"thrown at load\");\n\t}\n}\n";

	constexpr const char* BASE_SCRIPT = "using Shalltear;\n\nnamespace Extras;\n\npublic abstract class BaseScript : Behavior {\n\tpublic float inherited = 1f;\n}\n";

	constexpr const char* NEEDY = "using Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Needy : Extras.BaseScript {\n\tpublic float mood = 1f;\n}\n";

	constexpr const char* COUNTER = "using Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Counter : Behavior {\n\tpublic static int Total;\n\n"
									"\tprotected override void OnUpdate() {\n\t\t++Total;\n\t\tLog.Info(\"Game\", \"counter at \" + Total);\n\t}\n}\n";

	constexpr const char* PAIR = "using Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Finder : Behavior {\n"
								 "\tprotected override void OnEnable() {\n\t\tTarget? target = Owner.GetComponent<Target>();\n\t\tif (target) {\n"
								 "\t\t\tLog.Info(\"Game\", \"enabled with \" + target!.Owner.Name);\n\t\t}\n\t}\n\n"
								 "\tprotected override void OnUpdate() {\n\t\tTarget found = Owner.GetComponent<Target>()!;\n"
								 "\t\tLog.Info(\"Game\", \"found on \" + found.Owner.Name);\n\t}\n}\n\npublic sealed class Target : Behavior {\n}\n";

	fs::path _suiteRoot;
	bool _variantsBuilt = false;
	bool _toolchainMissing = false;

	class ReloadListener : public Object {
	public:
		std::vector<str> seen;

		ReloadListener() {
			EditorEvents::ModuleReloadBegan.Subscribe(EventBus::GetGlobal(), Handle<ReloadListener>(this), &ReloadListener::OnBegan);
			EditorEvents::ModuleReloadEnded.Subscribe(EventBus::GetGlobal(), Handle<ReloadListener>(this), &ReloadListener::OnEnded);
		}
		~ReloadListener() override { EventBus::GetGlobal().UnsubscribeAll(GetInstanceId()); }

		void OnBegan(const EditorModuleReloadBeganEvent& event) {
			seen.push_back(std::format("began {} drag {}", event.moduleBase == token ? "token" : "other", UISystem::IsDragPayloadActive()));
		}
		void OnEnded(const EditorModuleReloadEndedEvent& event) {
			seen.push_back(std::format("ended {} {} drag {} named {}", event.moduleBase == token ? "token" : "other", event.succeeded,
									   UISystem::IsDragPayloadActive(), TypeNameOps::Lookup(StringHash("Space_Game.Whirler"))));
		}

		const void* token = nullptr;
	};

	class CSharpReloadTest : public CSharpRuntimeFixture {
	protected:
		const EditorTestHooks* _editor = nullptr;
		fs::path _root;
		fs::path _live;
		Handle<Scene> _world;

		static void TearDownTestSuite() {
			std::error_code ec;
			fs::remove_all(_suiteRoot, ec);
			_suiteRoot.clear();
			_variantsBuilt = false;
		}

		void SetUp() override {
			CSharpRuntimeFixture::SetUp();
			const auto get = reinterpret_cast<GetEditorTestHooksFn>(
				ModuleSystem::FindSymbol(ModuleSystem::GetNativeHandle(_package), EDITOR_TEST_HOOKS_SYMBOL));
			ASSERT_NE(get, nullptr);
			_editor = get();
			const fs::path engineAssets = DeviceLossTest::RepoDir("EngineAssets");
			ASSERT_FALSE(engineAssets.empty());
			FileSystem::Mount("engine", Path(engineAssets.generic_string()));
			EditorConsole::Initialize();
			SceneSystem::Initialize();
			_hooks->installComponentMenu();
			ASSERT_TRUE(Attach()) << _error;

			if (!_variantsBuilt && !_toolchainMissing) {
				BuildVariants();
			}
			_root = fs::temp_directory_path() / ("shalltear_csharp_reload_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			_live = _root / "live" / "Space_Game.Scripts.dll";
			fs::create_directories(_live.parent_path());
			_world = SceneSystem::CreateScene("World");
			_editor->beginReload(_live.generic_string());
		}

		void TearDown() override {
			if (_editor) {
				_editor->shutdownReload();
			}
			SceneSystem::Terminate();
			EditorConsole::Terminate();
			FileSystem::Unmount("project");
			std::error_code ec;
			fs::remove_all(_root, ec);
			FileSystem::Unmount("engine");
			CSharpRuntimeFixture::TearDown();
		}

		void BuildVariants() {
			const str sdk = _hooks->findSDKRoot();
			if (sdk.empty()) {
				_toolchainMissing = true;
				return;
			}
			_suiteRoot = fs::temp_directory_path() / ("shalltear_csharp_reload_suite_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			const fs::path project = _suiteRoot / "Space Game";
			fs::create_directories(project / "Assets" / "Scripts");
			CSharpProjectFiles::Options files = _editor->getDefaultProjectOptions(project.generic_string(), sdk);
			files.projectName = "Space Game";
			CSharpProjectFiles::Result written;
			ASSERT_TRUE(_editor->writeProjectFiles(files, written));
			CompileOptions options;
			options.projectPath = project.generic_string();
			options.projectFile = "Space_Game.Scripts.csproj";
			options.muxer = (fs::path(sdk) / _editor->muxerName).generic_string();
			options.pinStamp = "reload";
			options.buildServers = false;
			_editor->configureCompile(options);

			const std::pair<const char*, std::vector<std::pair<const char*, const char*>>> variants[] = {
				{"first", {{"Spinner.cs", SPINNER_FIRST}, {"Greeter.cs", GREETER}, {"Counter.cs", COUNTER}}},
				{"second", {{"Spinner.cs", SPINNER_SECOND}, {"Greeter.cs", GREETER}, {"Counter.cs", COUNTER}}},
				{"renamed", {{"Whirler.cs", WHIRLER}, {"Greeter.cs", GREETER}, {"Counter.cs", COUNTER}}},
				{"leaky", {{"Leaker.cs", LEAKER}, {"Greeter.cs", GREETER}}},
				{"pair", {{"Pair.cs", PAIR}}},
				{"thrower", {{"Whirler.cs", WHIRLER}, {"Greeter.cs", GREETER}, {"Counter.cs", COUNTER}, {"Thrower.cs", THROWER}}},
			};
			for (const auto& [name, sources] : variants) {
				std::error_code ec;
				fs::remove_all(project / "Assets" / "Scripts", ec);
				fs::create_directories(project / "Assets" / "Scripts");
				for (const auto& [file, text] : sources) {
					std::ofstream(project / "Assets" / "Scripts" / file, std::ios::binary) << text;
				}
				ASSERT_TRUE(_editor->requestCompile());
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
				while (_editor->getCompile().IsBusy() && std::chrono::steady_clock::now() < deadline) {
					_editor->pumpCompile();
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
				ASSERT_EQ(_editor->getCompile().GetState(), CompileState::Succeeded) << name << "\n"
																					 << _log.Delivered().Joined();
				fs::copy_file(fs::path(_editor->getCompileOutput()), _suiteRoot / (str(name) + ".dll"), fs::copy_options::overwrite_existing);
			}
			BuildNeedyVariant(sdk, options);
			_variantsBuilt = true;
		}

		void BuildNeedyVariant(const str& sdk, CompileOptions options) {
			const fs::path project = fs::path(options.projectPath);
			const fs::path extras = _suiteRoot / "Extras";
			fs::create_directories(extras / "Assets" / "Scripts");
			CSharpProjectFiles::Options files = _editor->getDefaultProjectOptions(extras.generic_string(), sdk);
			files.projectName = "Extras";
			CSharpProjectFiles::Result written;
			ASSERT_TRUE(_editor->writeProjectFiles(files, written));
			std::ofstream(extras / "Assets" / "Scripts" / "BaseScript.cs", std::ios::binary) << BASE_SCRIPT;
			CompileOptions library = options;
			library.projectPath = extras.generic_string();
			library.projectFile = "Extras.Scripts.csproj";
			ASSERT_TRUE(CompileNow(library)) << _log.Delivered().Joined();
			const str extrasOutput = _editor->getCompileOutput();

			std::error_code ec;
			fs::remove_all(project / "Assets" / "Scripts", ec);
			fs::create_directories(project / "Assets" / "Scripts");
			std::ofstream(project / "Assets" / "Scripts" / "Needy.cs", std::ios::binary) << NEEDY;
			std::ofstream(project / "Assets" / "Scripts" / "Greeter.cs", std::ios::binary) << GREETER;
			std::ofstream(project / "Assets" / "Scripts" / "Counter.cs", std::ios::binary) << COUNTER;
			std::ofstream(project / "CSharp.props", std::ios::binary)
				<< "<Project>\n\t<ItemGroup>\n\t\t<Reference Include=\"Extras.Scripts\" HintPath=\"" << extrasOutput << "\" Private=\"false\" />\n\t</ItemGroup>\n</Project>\n";
			ASSERT_TRUE(CompileNow(options)) << _log.Delivered().Joined();
			fs::copy_file(fs::path(_editor->getCompileOutput()), _suiteRoot / "needy.dll", fs::copy_options::overwrite_existing);
		}

		bool CompileNow(const CompileOptions& options) {
			_editor->configureCompile(options);
			if (!_editor->requestCompile()) {
				return false;
			}
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
			while (_editor->getCompile().IsBusy() && std::chrono::steady_clock::now() < deadline) {
				_editor->pumpCompile();
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			return _editor->getCompile().GetState() == CompileState::Succeeded;
		}

		bool Ready() const { return _variantsBuilt; }

		void Stage(const char* variant) { fs::copy_file(_suiteRoot / (str(variant) + ".dll"), _live, fs::copy_options::overwrite_existing); }

		CSharpReload::Outcome Reload(const char* variant) {
			Stage(variant);
			const u32 before = _editor->getReloadCount();
			_editor->requestReload();
			_editor->pumpReload();
			return _editor->getReloadCount() == before ? CSharpReload::Outcome::None : _editor->getReloadOutcome();
		}

		static const TypeInfo* Type(const char* name) { return TypeRegistry::FindType(StringHash(str("Space_Game.") + name)); }

		Handle<Component> Add(const char* typeName, const char* objectName) {
			const TypeInfo* type = Type(typeName);
			return type ? SceneSystem::CreateObject(objectName, _world)->AddComponent(type) : Handle<Component>();
		}

		static void SetPlaying(bool playing) {
			EditorEvents::PlayModeChanged.Publish(EventBus::GetGlobal(), {playing ? PlayModeState::Playing : PlayModeState::Editing});
		}
	};

} // anonymous namespace

TEST_F(CSharpReloadTest, FieldValuesSurvive) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	EXPECT_FALSE(_editor->areScriptsLoaded());
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded) << _log.Delivered().Joined();
	EXPECT_TRUE(_editor->areScriptsLoaded());
	Handle<Component> spinner = Add("Spinner", "Holder");
	ASSERT_TRUE(spinner.IsValid());
	EXPECT_FLOAT_EQ(CSharpScenarios::Read<f32>(spinner, "speed"), 90.0f);
	CSharpScenarios::Write<f32>(spinner, "speed", 7.5f);
	CSharpScenarios::Write<str>(spinner, "label", str("kept"));
	const u64 id = spinner.GetInstanceId();

	ASSERT_EQ(Reload("second"), CSharpReload::Outcome::Loaded) << _log.Delivered().Joined();
	ASSERT_TRUE(spinner.IsValid());
	EXPECT_EQ(spinner.GetInstanceId(), id);
	EXPECT_EQ(spinner->GetType(), Type("Spinner"));
	EXPECT_FLOAT_EQ(CSharpScenarios::Read<f32>(spinner, "speed"), 7.5f);
	EXPECT_EQ(CSharpScenarios::Read<str>(spinner, "label"), "kept");
	EXPECT_EQ(_editor->getReloadMigratedCount(), 1u);
	EXPECT_GT(_editor->getReloadMilliseconds(), 0.0);
	EXPECT_EQ(_log.Delivered().Count("C# scripts loaded in"), 1u);
	EXPECT_EQ(_log.Delivered().Count("C# reloaded in"), 1u);
	EXPECT_EQ(_log.Delivered().Count("(1 component(s) migrated)"), 1u);
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Warn), 0u) << _log.Delivered().Joined();
}

TEST_F(CSharpReloadTest, AddedFieldGetsDefaultRemovedFieldDropped) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	Handle<Component> spinner = Add("Spinner", "Holder");
	CSharpScenarios::Write<i32>(spinner, "count", 9);
	EXPECT_EQ(FindField(Type("Spinner"), "bonus"_hash), nullptr);

	ASSERT_EQ(Reload("second"), CSharpReload::Outcome::Loaded);
	ASSERT_TRUE(spinner.IsValid());
	EXPECT_EQ(CSharpScenarios::Read<i32>(spinner, "bonus"), 42);
	EXPECT_EQ(FindField(Type("Spinner"), "count"_hash), nullptr);
	EXPECT_FLOAT_EQ(CSharpScenarios::Read<f32>(spinner, "speed"), 90.0f);

	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	EXPECT_EQ(CSharpScenarios::Read<i32>(spinner, "count"), 3);
}

TEST_F(CSharpReloadTest, HandlesResolveAfter) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	Handle<Component> spinner = Add("Spinner", "Holder");
	Handle<SceneObject> aim = SceneSystem::CreateObject("Aim", _world);
	CSharpScenarios::Write<Handle<SceneObject>>(spinner, "target", aim);
	Handle<SceneObject> holder = _world->FindObjectByName("Holder");

	ASSERT_EQ(Reload("second"), CSharpReload::Outcome::Loaded);
	ASSERT_TRUE(spinner.IsValid());
	EXPECT_EQ(CSharpScenarios::Read<Handle<SceneObject>>(spinner, "target"), aim);
	ASSERT_EQ(holder->GetComponentCount(), 1u);
	EXPECT_EQ(holder->GetComponentAt(0), spinner);
}

TEST_F(CSharpReloadTest, RenamedClassBecomesUnresolvedThenReturns) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	Handle<Component> spinner = Add("Spinner", "Holder");
	CSharpScenarios::Write<f32>(spinner, "speed", 7.5f);
	Handle<SceneObject> holder = _world->FindObjectByName("Holder");

	ASSERT_EQ(Reload("renamed"), CSharpReload::Outcome::Loaded);
	EXPECT_EQ(Type("Spinner"), nullptr);
	EXPECT_NE(Type("Whirler"), nullptr);
	ASSERT_EQ(holder->GetComponentCount(), 1u);
	const Component* preserved = holder->GetComponentAt(0).Get();
	ASSERT_NE(preserved, nullptr);
	ASSERT_EQ(preserved->GetType(), UnresolvedComponent::GetReflectType());
	EXPECT_EQ(static_cast<const UnresolvedComponent*>(preserved)->GetUnresolvedType(), StringHash("Space_Game.Spinner"));

	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	ASSERT_EQ(holder->GetComponentCount(), 1u);
	Handle<Component> back = holder->GetComponentAt(0);
	ASSERT_EQ(back->GetType(), Type("Spinner"));
	EXPECT_FLOAT_EQ(CSharpScenarios::Read<f32>(back, "speed"), 7.5f);
}

TEST_F(CSharpReloadTest, TwoReloadsInARow) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	Handle<Component> spinner = Add("Spinner", "Holder");
	CSharpScenarios::Write<f32>(spinner, "speed", 7.5f);
	ASSERT_EQ(Reload("second"), CSharpReload::Outcome::Loaded);
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	ASSERT_TRUE(spinner.IsValid());
	EXPECT_FLOAT_EQ(CSharpScenarios::Read<f32>(spinner, "speed"), 7.5f);
	EXPECT_EQ(_editor->getReloadCount(), 4u);
	EXPECT_EQ(_log.Delivered().Count("still referenced after unload"), 0u);
}

TEST_F(CSharpReloadTest, BrokenAssemblyRollsBack) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	Handle<Component> spinner = Add("Spinner", "Holder");
	CSharpScenarios::Write<f32>(spinner, "speed", 7.5f);

	std::ofstream(_live, std::ios::binary | std::ios::trunc) << "not an assembly";
	_editor->requestReload();
	_editor->pumpReload();
	EXPECT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::RolledBack);
	EXPECT_TRUE(_editor->areScriptsLoaded());
	ASSERT_TRUE(spinner.IsValid());
	EXPECT_EQ(spinner->GetType(), Type("Spinner"));
	EXPECT_FLOAT_EQ(CSharpScenarios::Read<f32>(spinner, "speed"), 7.5f);
	EXPECT_EQ(_log.Delivered().Count("previous build loaded again"), 1u) << _log.Delivered().Joined();

	ASSERT_EQ(Reload("second"), CSharpReload::Outcome::Loaded);
	EXPECT_FLOAT_EQ(CSharpScenarios::Read<f32>(spinner, "speed"), 7.5f);
}

TEST_F(CSharpReloadTest, FirstBrokenAssemblyLoadsNothing) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	std::ofstream(_live, std::ios::binary | std::ios::trunc) << "not an assembly";
	_editor->requestReload();
	_editor->pumpReload();
	EXPECT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Failed);
	EXPECT_FALSE(_editor->areScriptsLoaded());
	EXPECT_EQ(Type("Spinner"), nullptr);
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	EXPECT_NE(Type("Spinner"), nullptr);
}

TEST_F(CSharpReloadTest, HeldDuringPlayAppliedOnStop) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	Handle<Component> spinner = Add("Spinner", "Holder");
	SetPlaying(true);
	EXPECT_EQ(Reload("second"), CSharpReload::Outcome::None);
	EXPECT_TRUE(_editor->isReloadHeld());
	EXPECT_EQ(FindField(Type("Spinner"), "bonus"_hash), nullptr);
	_editor->pumpReload();
	EXPECT_EQ(_log.Delivered().Count("new scripts compiled during Play"), 1u);

	SetPlaying(false);
	EXPECT_TRUE(_editor->isReloadHeld());
	_editor->pumpReload();
	EXPECT_FALSE(_editor->isReloadHeld());
	EXPECT_NE(FindField(Type("Spinner"), "bonus"_hash), nullptr);
	EXPECT_EQ(_editor->getReloadCount(), 2u);
	_editor->pumpReload();
	EXPECT_EQ(_editor->getReloadCount(), 2u);
	EXPECT_TRUE(spinner.IsValid());
}

TEST_F(CSharpReloadTest, ReloadNowAppliesDuringPlay) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	SetPlaying(true);
	EXPECT_EQ(Reload("second"), CSharpReload::Outcome::None);
	ASSERT_TRUE(_editor->isReloadHeld());

	_editor->reloadNow();
	EXPECT_FALSE(_editor->isReloadHeld());
	EXPECT_NE(FindField(Type("Spinner"), "bonus"_hash), nullptr);
	EXPECT_EQ(_editor->getReloadCount(), 2u);
	SetPlaying(false);
	_editor->pumpReload();
	EXPECT_EQ(_editor->getReloadCount(), 2u);
}

TEST_F(CSharpReloadTest, HeldWhileABuildRuns) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	_editor->setBuildRunning(true);
	EXPECT_EQ(Reload("second"), CSharpReload::Outcome::None);
	EXPECT_TRUE(_editor->isReloadHeld());
	EXPECT_EQ(_log.Delivered().Count("new scripts compiled during a build"), 1u);
	_editor->reloadNow();
	EXPECT_TRUE(_editor->isReloadHeld());
	EXPECT_EQ(_editor->getReloadCount(), 1u);

	_editor->setBuildRunning(false);
	_editor->pumpReload();
	EXPECT_FALSE(_editor->isReloadHeld());
	EXPECT_EQ(_editor->getReloadCount(), 2u);
	EXPECT_NE(FindField(Type("Spinner"), "bonus"_hash), nullptr);

	BuildPipeline pipeline;
	ASSERT_EQ(BuildPipeline::GetActive(), &pipeline);
	BuildRequest request;
	request.projectPath = _root.generic_string();
	request.profiles.push_back("Default");
	str refusal;
	ASSERT_TRUE(pipeline.Request(request, refusal)) << refusal;
	EXPECT_EQ(Reload("first"), CSharpReload::Outcome::None);
	EXPECT_TRUE(_editor->isReloadHeld());
	EXPECT_EQ(_log.Delivered().Count("new scripts compiled during a build"), 2u);
	pipeline.Cancel();
	pipeline.PumpMainThread();
	ASSERT_FALSE(pipeline.IsBusy());
	_editor->pumpReload();
	EXPECT_FALSE(_editor->isReloadHeld());
	EXPECT_EQ(_editor->getReloadCount(), 3u);
}

TEST_F(CSharpReloadTest, BeginAndShutdownForgetTheLastSession) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	Stage("second");
	_editor->requestReload();
	_editor->shutdownReload();
	EXPECT_FALSE(_editor->areScriptsLoaded());
	EXPECT_EQ(_editor->getReloadCount(), 0u);
	_editor->requestReload();
	_editor->beginReload(_live.generic_string());
	_editor->pumpReload();
	EXPECT_EQ(_editor->getReloadCount(), 0u);
	EXPECT_FALSE(_editor->areScriptsLoaded());
	EXPECT_EQ(FindField(Type("Spinner"), "bonus"_hash), nullptr);

	ASSERT_EQ(Reload("second"), CSharpReload::Outcome::Loaded);
	EXPECT_EQ(_editor->getReloadCount(), 1u);
	_editor->beginReload(_live.generic_string());
	EXPECT_EQ(_editor->getReloadCount(), 0u);
	EXPECT_FALSE(_editor->areScriptsLoaded());
	EXPECT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::None);
	EditorEvents::PlayStarting.Publish(EventBus::GetGlobal(), {});
	EXPECT_EQ(_editor->getReloadCount(), 0u);
}

TEST_F(CSharpReloadTest, PausedPlayStillHolds) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	EditorEvents::PlayModeChanged.Publish(EventBus::GetGlobal(), {PlayModeState::Paused});
	EXPECT_EQ(Reload("second"), CSharpReload::Outcome::None);
	EXPECT_TRUE(_editor->isReloadHeld());
	EXPECT_EQ(_log.Delivered().Count("new scripts compiled during Play"), 1u);
	SetPlaying(false);
	_editor->pumpReload();
	EXPECT_EQ(_editor->getReloadCount(), 2u);
}

TEST_F(CSharpReloadTest, PlayStartLoadsABuildThatWaited) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	EditorEvents::PlayStarting.Publish(EventBus::GetGlobal(), {});
	EXPECT_EQ(_editor->getReloadCount(), 0u);
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Warn), 0u) << _log.Delivered().Joined();

	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	Stage("second");
	_editor->requestReload();
	EditorEvents::PlayStarting.Publish(EventBus::GetGlobal(), {});
	EXPECT_EQ(_editor->getReloadCount(), 2u);
	EXPECT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded);
	EXPECT_NE(FindField(Type("Spinner"), "bonus"_hash), nullptr);
	_editor->pumpReload();
	EXPECT_EQ(_editor->getReloadCount(), 2u);
}

TEST_F(CSharpReloadTest, LoadThatFailsHalfwayRollsBackCleanly) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	Handle<Component> spinner = Add("Spinner", "Holder");
	Handle<Component> counter = Add("Counter", "Clock");
	CSharpScenarios::Write<f32>(spinner, "speed", 7.5f);

	Stage("needy");
	_editor->requestReload();
	_editor->pumpReload();
	EXPECT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::RolledBack) << _log.Delivered().Joined();
	EXPECT_EQ(_log.Delivered().Count("Extras.Scripts"), 1u) << _log.Delivered().Joined();
	EXPECT_EQ(_log.Delivered().Count("already used by engine"), 0u) << _log.Delivered().Joined();
	EXPECT_EQ(Type("Needy"), nullptr);
	ASSERT_TRUE(spinner.IsValid());
	ASSERT_TRUE(counter.IsValid());
	EXPECT_EQ(spinner->GetType(), Type("Spinner"));
	EXPECT_EQ(counter->GetType(), Type("Counter"));
	EXPECT_FLOAT_EQ(CSharpScenarios::Read<f32>(spinner, "speed"), 7.5f);
	CSharpScenarios::RunFrames(1);
	EXPECT_EQ(_log.Delivered().Count("counter at 1"), 1u);
}

TEST_F(CSharpReloadTest, ThrowingModuleInitializerRollsBack) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	Handle<Component> spinner = Add("Spinner", "Holder");
	CSharpScenarios::Write<f32>(spinner, "speed", 7.5f);

	Stage("thrower");
	_editor->requestReload();
	_editor->pumpReload();
	EXPECT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::RolledBack) << _log.Delivered().Joined();
	EXPECT_EQ(_log.Delivered().Count("thrown at load"), 1u) << _log.Delivered().Joined();
	EXPECT_EQ(_log.Delivered().Count("already used by engine"), 0u) << _log.Delivered().Joined();
	EXPECT_EQ(Type("Whirler"), nullptr);
	ASSERT_TRUE(spinner.IsValid());
	EXPECT_EQ(spinner->GetType(), Type("Spinner"));
	EXPECT_FLOAT_EQ(CSharpScenarios::Read<f32>(spinner, "speed"), 7.5f);
	ASSERT_EQ(Reload("second"), CSharpReload::Outcome::Loaded);
	EXPECT_FLOAT_EQ(CSharpScenarios::Read<f32>(spinner, "speed"), 7.5f);
}

TEST_F(CSharpReloadTest, FieldlessScriptKeepsOwnerAfterReload) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	Handle<Component> greeter = Add("Greeter", "Porch");
	ASSERT_TRUE(greeter.IsValid());
	EXPECT_EQ(_log.Delivered().Count("greeter on Porch"), 1u);

	ASSERT_EQ(Reload("second"), CSharpReload::Outcome::Loaded);
	EXPECT_EQ(_log.Delivered().Count("greeter on Porch"), 2u) << _log.Delivered().Joined();
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Warn), 0u) << _log.Delivered().Joined();
}

TEST_F(CSharpReloadTest, RestoredScriptPulledOnceForId) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	ASSERT_TRUE(Add("Counter", "Clock").IsValid());
	CSharpScenarios::RunFrames(2);
	const u64 pulls = _hooks->getCounters().pulls;
	CSharpScenarios::RunFrames(2);
	EXPECT_EQ(_hooks->getCounters().pulls, pulls);

	ASSERT_EQ(Reload("second"), CSharpReload::Outcome::Loaded);
	EXPECT_EQ(_hooks->getCounters().pulls, pulls + 1);
	ASSERT_TRUE(Add("Greeter", "Porch").IsValid());
	CSharpScenarios::RunFrames(3);
	EXPECT_EQ(_hooks->getCounters().pulls, pulls + 1);
	EXPECT_EQ(_log.Delivered().Count("counter at 3"), 2u) << _log.Delivered().Joined();
}

TEST_F(CSharpReloadTest, ScriptFoundRightAfterReloadKnowsOwner) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(Reload("pair"), CSharpReload::Outcome::Loaded) << _log.Delivered().Joined();
	Handle<SceneObject> holder = SceneSystem::CreateObject("Holder", _world);
	ASSERT_TRUE(holder->AddComponent(Type("Finder")).IsValid());
	ASSERT_TRUE(holder->AddComponent(Type("Target")).IsValid());
	CSharpScenarios::RunFrames(1);
	EXPECT_EQ(_log.Delivered().Count("found on Holder"), 1u) << _log.Delivered().Joined();

	EXPECT_EQ(_log.Delivered().Count("enabled with Holder"), 0u);
	ASSERT_EQ(Reload("pair"), CSharpReload::Outcome::Loaded);
	EXPECT_EQ(_log.Delivered().Count("enabled with Holder"), 1u) << _log.Delivered().Joined();
	CSharpScenarios::RunFrames(1);
	EXPECT_EQ(_log.Delivered().Count("found on Holder"), 2u) << _log.Delivered().Joined();
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Warn), 0u) << _log.Delivered().Joined();
}

TEST_F(CSharpReloadTest, PlayStartsFromFreshStatics) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	Handle<Component> counter = Add("Counter", "Clock");
	ASSERT_TRUE(counter.IsValid());
	CSharpScenarios::RunFrames(3);
	EXPECT_EQ(_log.Delivered().Count("counter at 3"), 1u) << _log.Delivered().Joined();

	EditorEvents::PlayStarting.Publish(EventBus::GetGlobal(), {});
	EXPECT_EQ(_editor->getReloadCount(), 2u);
	ASSERT_TRUE(counter.IsValid());
	CSharpScenarios::RunFrames(2);
	EXPECT_EQ(_log.Delivered().Count("counter at 1"), 2u);
	EXPECT_EQ(_log.Delivered().Count("counter at 2"), 2u);
	EXPECT_EQ(_log.Delivered().Count("counter at 4"), 0u);
}

TEST_F(CSharpReloadTest, RefusedReloadWaitsPlayStartsWithoutReset) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	Handle<Component> counter = Add("Counter", "Clock");
	CSharpScenarios::RunFrames(2);
	SceneSystem::Destroy(SceneSystem::CreateObject("Doomed", _world));

	EXPECT_EQ(Reload("second"), CSharpReload::Outcome::None);
	EXPECT_FALSE(_editor->isReloadHeld());
	_editor->reloadNow();
	EXPECT_EQ(_editor->getReloadCount(), 1u);
	EditorEvents::PlayStarting.Publish(EventBus::GetGlobal(), {});
	EXPECT_EQ(_editor->getReloadCount(), 1u);
	EXPECT_EQ(_log.Delivered().Count("script statics not reset for Play (scene busy). Press Play again"), 1u) << _log.Delivered().Joined();
	EXPECT_EQ(FindField(Type("Spinner"), "bonus"_hash), nullptr);

	SceneSystem::EndFrame();
	_editor->pumpReload();
	EXPECT_EQ(_editor->getReloadCount(), 2u);
	EXPECT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded);
	EXPECT_NE(FindField(Type("Spinner"), "bonus"_hash), nullptr);
	EXPECT_TRUE(counter.IsValid());
	_editor->pumpReload();
	EXPECT_EQ(_editor->getReloadCount(), 2u);
}

TEST_F(CSharpReloadTest, ReloadNowWithNothingNewResetsStatics) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	_editor->reloadNow();
	EXPECT_EQ(_editor->getReloadCount(), 0u);
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	Handle<Component> counter = Add("Counter", "Clock");
	CSharpScenarios::RunFrames(2);
	EXPECT_EQ(_log.Delivered().Count("counter at 2"), 1u);

	std::ofstream(_live, std::ios::binary | std::ios::trunc) << "not an assembly";
	_editor->reloadNow();
	EXPECT_EQ(_editor->getReloadCount(), 2u);
	EXPECT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded);
	ASSERT_TRUE(counter.IsValid());
	CSharpScenarios::RunFrames(1);
	EXPECT_EQ(_log.Delivered().Count("counter at 1"), 2u);
	EXPECT_EQ(_log.Delivered().Count("counter at 3"), 0u);
}

TEST_F(CSharpReloadTest, ModuleAndScriptsReloadIndependently) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ModuleHandle module = ModuleSystem::Load(Path("ShalltearHotReloadModuleV1" SLT_TEST_DLL_SUFFIX));
	ASSERT_TRUE(ModuleSystem::IsLoaded(module));
	const TypeInfo* nativeType = TypeRegistry::FindType(StringHash("HotReloadBehavior"));
	ASSERT_NE(nativeType, nullptr);
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	Handle<SceneObject> holder = SceneSystem::CreateObject("Holder", _world);
	Handle<Component> counter = holder->AddComponent(Type("Counter"));
	Handle<Component> spinner = holder->AddComponent(Type("Spinner"));
	Handle<Component> native = holder->AddComponent(nativeType);
	CSharpScenarios::Write<f32>(spinner, "speed", 7.5f);
	CSharpScenarios::Write<i32>(native, "counter", 40);
	CSharpScenarios::RunFrames(1);
	EXPECT_EQ(CSharpScenarios::Read<i32>(native, "counter"), 41);
	EXPECT_EQ(_log.Delivered().Count("counter at 1"), 1u);

	const Component* nativeBefore = native.Get();
	const i32 enables = CSharpScenarios::Read<i32>(native, "enableCount");
	const void* image = ModuleSystem::GetNativeHandle(module);
	ASSERT_EQ(Reload("second"), CSharpReload::Outcome::Loaded);
	EXPECT_EQ(native.Get(), nativeBefore);
	EXPECT_EQ(native->GetType(), nativeType);
	EXPECT_EQ(ModuleSystem::GetNativeHandle(module), image);
	EXPECT_EQ(CSharpScenarios::Read<i32>(native, "enableCount"), enables);
	EXPECT_EQ(CSharpScenarios::Read<i32>(native, "counter"), 41);
	CSharpScenarios::RunFrames(1);
	EXPECT_EQ(CSharpScenarios::Read<i32>(native, "counter"), 42);
	EXPECT_EQ(_log.Delivered().Count("counter at 1"), 2u);

	const Component* spinnerBefore = spinner.Get();
	const Component* counterBefore = counter.Get();
	const u32 reloads = _editor->getReloadCount();
	Path second{"ShalltearHotReloadModuleV2" SLT_TEST_DLL_SUFFIX};
	ASSERT_TRUE(ModuleSystem::Reload(module, &second));
	EXPECT_EQ(spinner.Get(), spinnerBefore);
	EXPECT_EQ(counter.Get(), counterBefore);
	EXPECT_EQ(_editor->getReloadCount(), reloads);
	EXPECT_FLOAT_EQ(CSharpScenarios::Read<f32>(spinner, "speed"), 7.5f);
	EXPECT_EQ(CSharpScenarios::Read<i32>(spinner, "bonus"), 42);
	ASSERT_TRUE(native.IsValid());
	EXPECT_EQ(CSharpScenarios::Read<i32>(native, "counter"), 42);
	EXPECT_NE(FindField(native->GetType(), "added"_hash), nullptr);
	CSharpScenarios::RunFrames(1);
	EXPECT_EQ(CSharpScenarios::Read<i32>(native, "counter"), 43);
	EXPECT_EQ(_log.Delivered().Count("counter at 2"), 1u) << _log.Delivered().Joined();
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Warn), 0u) << _log.Delivered().Joined();
}

TEST_F(CSharpReloadTest, ReloadIsAnnouncedUnderTheScriptToken) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	FileSystem::Mount("project", Path(_root.generic_string()));
	TypeNameOps::Refresh();
	ASSERT_EQ(TypeNameOps::Lookup(StringHash("Space_Game.Whirler")), "");
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	EXPECT_EQ(TypeNameOps::Lookup(StringHash("Space_Game.Spinner")), "Spinner");
	ReloadListener listener;
	listener.token = Type("Spinner")->ownerModule;
	ASSERT_TRUE(TypeRegistry::IsOwnerToken(listener.token));

	UISystem::Initialize();
	UI::DragPayload payload;
	payload.type = "Probe"_hash;
	UISystem::BeginDragPayload(payload);
	ASSERT_TRUE(UISystem::IsDragPayloadActive());
	ASSERT_EQ(Reload("renamed"), CSharpReload::Outcome::Loaded);
	ASSERT_EQ(listener.seen.size(), 2u);
	EXPECT_EQ(listener.seen[0], "began token drag true");
	EXPECT_EQ(listener.seen[1], "ended token true drag false named Whirler");
	EXPECT_EQ(TypeNameOps::Lookup(StringHash("Space_Game.Spinner")), "Spinner");

	std::ofstream(_live, std::ios::binary | std::ios::trunc) << "not an assembly";
	_editor->requestReload();
	_editor->pumpReload();
	ASSERT_EQ(listener.seen.size(), 4u);
	EXPECT_EQ(listener.seen[2], "began token drag false");
	EXPECT_EQ(listener.seen[3], "ended token false drag false named Whirler");
	UISystem::Terminate();
}

TEST_F(CSharpReloadTest, LeakedContextReportedReloadGoesOn) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(Reload("leaky"), CSharpReload::Outcome::Loaded);
	ASSERT_TRUE(Add("Leaker", "Holder").IsValid());
	EXPECT_EQ(_log.Delivered().Count("still referenced after unload"), 0u);

	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	EXPECT_EQ(_log.Delivered().Count("previous scripts are still referenced after unload (memory not freed)"), 1u) << _log.Delivered().Joined();
	EXPECT_EQ(Type("Leaker"), nullptr);
	EXPECT_NE(Type("Spinner"), nullptr);
	ASSERT_EQ(Reload("second"), CSharpReload::Outcome::Loaded);
	EXPECT_EQ(_log.Delivered().Count("still referenced after unload"), 1u);
}

TEST_F(CSharpReloadTest, UndoSurvivesReload) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	UndoSystem::Initialize();
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	Handle<Component> spinner = Add("Spinner", "Holder");
	UndoSystem::Push(MakeUnique<PropertySetCommand>(EditTarget{spinner}, "speed"_hash, Variant(90.0f), Variant(8.0f), "Speed"));
	EXPECT_FLOAT_EQ(CSharpScenarios::Read<f32>(spinner, "speed"), 8.0f);
	ASSERT_TRUE(UndoSystem::CanUndo());

	ASSERT_EQ(Reload("second"), CSharpReload::Outcome::Loaded);
	EXPECT_FLOAT_EQ(CSharpScenarios::Read<f32>(spinner, "speed"), 8.0f);
	ASSERT_TRUE(UndoSystem::CanUndo());
	ASSERT_TRUE(UndoSystem::Undo());
	EXPECT_FLOAT_EQ(CSharpScenarios::Read<f32>(spinner, "speed"), 90.0f);
	ASSERT_TRUE(UndoSystem::Redo());
	EXPECT_FLOAT_EQ(CSharpScenarios::Read<f32>(spinner, "speed"), 8.0f);
	UndoSystem::Terminate();
}

TEST_F(CSharpReloadTest, OldContextIsCollected) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	Add("Spinner", "Holder");
	Add("Counter", "Clock");
	CSharpScenarios::RunFrames(2);
	for (u32 round = 0; round < 4; ++round) {
		ASSERT_EQ(Reload(round % 2 == 0 ? "second" : "first"), CSharpReload::Outcome::Loaded);
	}
	EXPECT_EQ(_hooks->unloadUserAssemblies(), SLT_OK);
	EXPECT_EQ(_log.Delivered().Count("still referenced after unload"), 0u) << _log.Delivered().Joined();
}

TEST_F(CSharpReloadTest, MemoryFlatOverTenReloads) {
	if (!Ready()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	for (u32 index = 0; index < 50; ++index) {
		Add("Spinner", "Holder");
	}
	ASSERT_EQ(Reload("second"), CSharpReload::Outcome::Loaded);
	ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	const size_t scene = MemoryManager::GetBytesForTag(MemTag::Scene);
	const size_t core = MemoryManager::GetBytesForTag(MemTag::Core);
	for (u32 round = 0; round < 5; ++round) {
		ASSERT_EQ(Reload("second"), CSharpReload::Outcome::Loaded);
		ASSERT_EQ(Reload("first"), CSharpReload::Outcome::Loaded);
	}
	EXPECT_EQ(MemoryManager::GetBytesForTag(MemTag::Scene), scene);
	EXPECT_EQ(MemoryManager::GetBytesForTag(MemTag::Core), core);
	EXPECT_EQ(_editor->getReloadMigratedCount(), 50u);
}

TEST_F(CSharpReloadTest, SceneHandlersDropAtUnload) {
	const fs::path conformance = fs::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path() / "CSharp" / "Shalltear.Conformance.dll";
	ASSERT_TRUE(fs::is_regular_file(conformance));
	fs::copy_file(conformance, _live, fs::copy_options::overwrite_existing);
	_editor->requestReload();
	_editor->pumpReload();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded);
	str message;
	ASSERT_EQ(Run("SceneProbe.KeepsListening", message), SLT_OK) << message;
	_hooks->takeRecords();
	SceneSystem::CreateScene("First");
	EXPECT_EQ(_hooks->takeRecords(), (std::vector<str>{"heard First"}));

	_editor->requestReload();
	_editor->pumpReload();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded);
	ASSERT_EQ(_editor->getReloadCount(), 2u);
	SceneSystem::CreateScene("Second");
	EXPECT_TRUE(_hooks->takeRecords().empty());
	EXPECT_EQ(_log.Delivered().Count("still referenced after unload"), 0u);
}

TEST_F(CSharpReloadTest, PendingAwaitsDropAtUnload) {
	const fs::path conformance = fs::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path() / "CSharp" / "Shalltear.Conformance.dll";
	ASSERT_TRUE(fs::is_regular_file(conformance));
	fs::copy_file(conformance, _live, fs::copy_options::overwrite_existing);
	_editor->requestReload();
	_editor->pumpReload();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded);
	str message;
	ASSERT_EQ(Run("AwaitProbe.LeavesPendingWork", message), SLT_OK) << message;

	_editor->requestReload();
	_editor->pumpReload();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded);
	ASSERT_EQ(_editor->getReloadCount(), 2u);
	EXPECT_EQ(_log.Delivered().Count("still referenced after unload"), 0u);
	_hooks->takeRecords();
	ASSERT_EQ(Run("AwaitProbe.StepsFrames", message), SLT_OK) << message;
	EXPECT_TRUE(_hooks->takeRecords().empty());
}

TEST_F(CSharpReloadTest, ExportsRebindAcrossReload) {
	const fs::path conformance = fs::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path() / "CSharp" / "Shalltear.Conformance.dll";
	ASSERT_TRUE(fs::is_regular_file(conformance));
	fs::copy_file(conformance, _live, fs::copy_options::overwrite_existing);
	ScriptFunction<i32(i32)> triple("ConformanceTriple");
	EXPECT_FALSE(triple.IsBound());
	_editor->requestReload();
	_editor->pumpReload();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded);
	ASSERT_TRUE(triple.IsBound());
	EXPECT_EQ(triple(5), 15);

	_editor->requestReload();
	_editor->pumpReload();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded);
	ASSERT_EQ(_editor->getReloadCount(), 2u);
	EXPECT_EQ(triple(6), 18);
	EXPECT_EQ(_log.Delivered().Count("is already exported"), 0u) << _log.Delivered().Joined();
	EXPECT_EQ(_log.Delivered().Count("still referenced after unload"), 0u);
	_hooks->unloadUserAssemblies();
	EXPECT_FALSE(triple.IsBound());
}

TEST_F(CSharpReloadTest, AnimationHandlersDropAtUnload) {
	const fs::path conformance = fs::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path() / "CSharp" / "Shalltear.Conformance.dll";
	ASSERT_TRUE(fs::is_regular_file(conformance));
	fs::copy_file(conformance, _live, fs::copy_options::overwrite_existing);
	_editor->requestReload();
	_editor->pumpReload();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded);
	str message;
	ASSERT_EQ(Run("AnimationProbe.KeepsListening", message), SLT_OK) << message;
	ASSERT_EQ(Run("AnimationProbe.CountsKeepersSubscribers", message), SLT_OK) << message;
	EXPECT_EQ(_hooks->takeRecords(), (std::vector<str>{"subscribers 1"}));

	_editor->requestReload();
	_editor->pumpReload();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded);
	ASSERT_EQ(Run("AnimationProbe.CountsKeepersSubscribers", message), SLT_OK) << message;
	EXPECT_EQ(_hooks->takeRecords(), (std::vector<str>{"subscribers 0"}));
	EXPECT_EQ(_log.Delivered().Count("still referenced after unload"), 0u);
}

TEST_F(CSharpReloadTest, DataAssetsSurviveReload) {
	const fs::path conformance = fs::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path() / "CSharp" / "Shalltear.Conformance.dll";
	ASSERT_TRUE(fs::is_regular_file(conformance));
	fs::copy_file(conformance, _live, fs::copy_options::overwrite_existing);
	_editor->requestReload();
	_editor->pumpReload();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded);
	std::optional<CSharpConformance::StagedAssets> assets(std::in_place);
	ASSERT_TRUE(assets->IsStaged());
	str message;
	ASSERT_EQ(Run("DataAssetProbe.EditsSoup", message), SLT_OK) << message;
	const std::vector<str> edited = _hooks->takeRecords();
	ASSERT_EQ(edited.size(), 1u);

	_editor->requestReload();
	_editor->pumpReload();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded);
	ASSERT_EQ(Run("DataAssetProbe.ReportsSoup", message), SLT_OK) << message;
	EXPECT_EQ(_hooks->takeRecords(), (std::vector<str>{"servings 9 " + edited[0]}));
	EXPECT_EQ(_log.Delivered().Count("still referenced after unload"), 0u);
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Error), 0u) << _log.Delivered().Joined();
}

TEST_F(CSharpReloadTest, UIHandlersDropAtUnload) {
	const fs::path conformance = fs::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path() / "CSharp" / "Shalltear.Conformance.dll";
	ASSERT_TRUE(fs::is_regular_file(conformance));
	fs::copy_file(conformance, _live, fs::copy_options::overwrite_existing);
	_editor->requestReload();
	_editor->pumpReload();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded);
	std::optional<CSharpConformance::StagedAssets> assets(std::in_place);
	ASSERT_TRUE(assets->IsStaged());
	CSharpConformance::InputRig input(_hooks->setDriver);
	CSharpConformance::UIRig ui;
	ASSERT_TRUE(ui.IsReady());
	str message;
	ASSERT_EQ(Run("UIProbe.KeepsAHandler", message), SLT_OK) << message;
	ASSERT_EQ(Run("UIProbe.ClicksKeepersButton", message), SLT_OK) << message;
	ASSERT_EQ(Run("UIProbe.ReportsKeepersHandler", message), SLT_OK) << message;
	EXPECT_EQ(_hooks->takeRecords(), (std::vector<str>{"kept handler ran", "handler 1"}));

	_editor->requestReload();
	_editor->pumpReload();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded);
	ASSERT_EQ(Run("UIProbe.ClicksKeepersButton", message), SLT_OK) << message;
	ASSERT_EQ(Run("UIProbe.ReportsKeepersHandler", message), SLT_OK) << message;
	EXPECT_EQ(_hooks->takeRecords(), (std::vector<str>{"handler 0"}));
	EXPECT_EQ(_log.Delivered().Count("still referenced after unload"), 0u);
	const std::vector<Handle<Scene>> loaded(SceneSystem::GetLoadedScenes().begin(), SceneSystem::GetLoadedScenes().end());
	for (const Handle<Scene>& scene : loaded) {
		SceneSystem::UnloadScene(scene);
	}
	SceneSystem::EndFrame();
}

TEST_F(CSharpReloadTest, EditorRegistrationsEndAtUnload) {
	const fs::path conformance = fs::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path() / "CSharp" / "Shalltear.Conformance.Editor.dll";
	ASSERT_TRUE(fs::is_regular_file(conformance));
	_editor->registerEditorBindings();
	fs::copy_file(conformance, _live, fs::copy_options::overwrite_existing);
	_editor->requestReload();
	_editor->pumpReload();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded);
	EXPECT_EQ(_hooks->takeRecords(), (std::vector<str>{"editor load"}));
	str message;
	ASSERT_EQ(Run("EditorProbe.AddsAMenuItem", message), SLT_OK) << message;

	UITest::MockFontSystem fonts;
	UI::Context context(UI::ContextDesc{}, &fonts);
	Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
	EditorMenus::AppendContributed("Probe", context, items);
	ASSERT_EQ(items.size(), 1u);
	EXPECT_EQ(items[0].label, "Probe Item");
	EXPECT_EQ(items[0].shortcut, "Ctrl+9");
	EXPECT_TRUE(items[0].enabled);
	items[0].onSelect();
	EXPECT_EQ(_hooks->takeRecords(), (std::vector<str>{"probe item selected"}));
	items.clear();

	_editor->requestReload();
	_editor->pumpReload();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded);
	EXPECT_EQ(_hooks->takeRecords(), (std::vector<str>{"editor load"}));
	EditorMenus::AppendContributed("Probe", context, items);
	EXPECT_TRUE(items.empty());
	EXPECT_EQ(_log.Delivered().Count("still referenced after unload"), 0u);
	EditorMenus::Terminate();
}

TEST_F(CSharpReloadTest, PanelTypesEndAtUnload) {
	const fs::path conformance = fs::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path() / "CSharp" / "Shalltear.Conformance.Editor.dll";
	ASSERT_TRUE(fs::is_regular_file(conformance));
	_editor->registerEditorBindings();
	fs::copy_file(conformance, _live, fs::copy_options::overwrite_existing);
	_editor->requestReload();
	_editor->pumpReload();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded);
	EXPECT_EQ(_hooks->takeRecords(), (std::vector<str>{"editor load"}));
	str message;
	ASSERT_EQ(Run("EditorProbe.RegistersAPanel", message), SLT_OK) << message;

	const TypeInfo* type = TypeRegistry::FindType(StringHash("Conformance.ProbePanel"));
	ASSERT_NE(type, nullptr);
	EXPECT_TRUE(TypeRegistry::IsDerivedFrom(type, EditorPanel::GetReflectType()));
	EXPECT_EQ(PanelManager::GetPanelTypeName(type), "Probe Panel");
	Object* const made = TypeRegistry::CreateObjectInstance(type, MemTag::Editor);
	ASSERT_NE(made, nullptr);
	EXPECT_EQ(_hooks->takeRecords(), (std::vector<str>{"panel wired"}));
	EditorPanel* const panel = static_cast<EditorPanel*>(made);
	EXPECT_EQ(panel->GetType(), type);
	EXPECT_EQ(panel->GetTitleBase(), "Probe");
	UITest::MockFontSystem fonts;
	UI::Context context(UI::ContextDesc{}, &fonts);
	panel->BuildContent(context, context.GetRoot());
	EXPECT_EQ(_hooks->takeRecords(), (std::vector<str>{"panel built"}));
	TypeRegistry::DestroyObjectInstance(made);

	_editor->requestReload();
	_editor->pumpReload();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded);
	EXPECT_EQ(_hooks->takeRecords(), (std::vector<str>{"editor load"}));
	EXPECT_EQ(TypeRegistry::FindType(StringHash("Conformance.ProbePanel")), nullptr);
	EXPECT_EQ(_log.Delivered().Count("still referenced after unload"), 0u);
}

TEST_F(CSharpReloadTest, InputMapsEndAtUnload) {
	const fs::path conformance = fs::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path() / "CSharp" / "Shalltear.Conformance.dll";
	ASSERT_TRUE(fs::is_regular_file(conformance));
	fs::copy_file(conformance, _live, fs::copy_options::overwrite_existing);
	_editor->requestReload();
	_editor->pumpReload();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded);
	str message;
	ASSERT_EQ(Run("InputProbe.KeepsAMap", message), SLT_OK) << message;
	EXPECT_EQ(_hooks->getInputMapCount(), 1);

	_editor->requestReload();
	_editor->pumpReload();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded);
	EXPECT_EQ(_hooks->getInputMapCount(), 0);
	EXPECT_EQ(_log.Delivered().Count("still referenced after unload"), 0u);
}

TEST_F(CSharpReloadTest, ConformanceSurvivesTenReloads) {
	const fs::path conformance = fs::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path() / "CSharp" / "Shalltear.Conformance.dll";
	ASSERT_TRUE(fs::is_regular_file(conformance));
	fs::copy_file(conformance, _live, fs::copy_options::overwrite_existing);
	const Time::FrameScope frame(CSharpConformance::MakeFrame());
	std::optional<CSharpConformance::StagedAssets> assets;
	CSharpConformance::InputRig input(_hooks->setDriver);
	ASSERT_TRUE(input.IsReady());
	CSharpConformance::UIRig ui;
	ASSERT_TRUE(ui.IsReady());
	for (u32 round = 0; round < 10; ++round) {
		const auto begin = std::chrono::steady_clock::now();
		_editor->requestReload();
		_editor->pumpReload();
		ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded) << round;
		ASSERT_EQ(_editor->getReloadCount(), round + 1);
		if (!assets) {
			assets.emplace();
			ASSERT_TRUE(assets->IsStaged());
		}
		const SltManagedAPI& managed = _hooks->getManaged();
		const int32_t count = managed.getConformanceTestCount();
		ASSERT_GT(count, 50) << round;
		u32 failed = 0;
		for (int32_t index = 0; index < count; ++index) {
			SltString name{};
			ASSERT_EQ(managed.getConformanceTestName(index, &name), SLT_OK);
			const str label(CSharpConformance::View(name));
			if (!CSharpConformance::IsInPassSet(label)) {
				continue;
			}
			SltString message{};
			if (managed.runConformanceTest(index, &message) != SLT_OK) {
				++failed;
				ADD_FAILURE() << "round " << round << " " << label << ": " << CSharpConformance::View(message);
			}
		}
		EXPECT_EQ(failed, 0u);
		EXPECT_LT(std::chrono::duration<f64>(std::chrono::steady_clock::now() - begin).count(), 60.0) << round;
		SceneSystem::EndFrame();
	}
	EXPECT_EQ(_log.Delivered().Count("still referenced after unload"), 0u);
	const std::vector<Handle<Scene>> loaded(SceneSystem::GetLoadedScenes().begin(), SceneSystem::GetLoadedScenes().end());
	for (const Handle<Scene>& scene : loaded) {
		SceneSystem::UnloadScene(scene);
	}
	SceneSystem::EndFrame();
	assets.reset();
}

namespace {

	char _sessionOwner = 0;

	class CSharpSessionTest : public CSharpRuntimeFixture {
	protected:
		const EditorTestHooks* _editor = nullptr;
		fs::path _root;
		fs::path _project;
		CSharpAuthoring::Options _options;
		Handle<Scene> _world;

		void SetUp() override {
			CSharpRuntimeFixture::SetUp();
			const auto get = reinterpret_cast<GetEditorTestHooksFn>(
				ModuleSystem::FindSymbol(ModuleSystem::GetNativeHandle(_package), EDITOR_TEST_HOOKS_SYMBOL));
			ASSERT_NE(get, nullptr);
			_editor = get();
			const fs::path engineAssets = DeviceLossTest::RepoDir("EngineAssets");
			ASSERT_FALSE(engineAssets.empty());
			FileSystem::Mount("engine", Path(engineAssets.generic_string()));
			EditorConsole::Initialize();
			SceneSystem::Initialize();
			_hooks->installComponentMenu();
			ASSERT_TRUE(Attach()) << _error;
			_root = fs::temp_directory_path() / ("shalltear_csharp_session_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			_project = _root / "Space Game";
			fs::create_directories(_project / "Assets" / "Scripts");
			_world = SceneSystem::CreateScene("World");
		}

		void TearDown() override {
			if (_editor) {
				_editor->shutdownAuthoring();
			}
			CompileStatus::UnregisterOwnedBy(&_sessionOwner);
			SceneSystem::Terminate();
			EditorConsole::Terminate();
			std::error_code ec;
			fs::remove_all(_root, ec);
			FileSystem::Unmount("project");
			FileSystem::Unmount("engine");
			BuildTargetRegistry::Shutdown();
			CSharpRuntimeFixture::TearDown();
		}

		bool Prepare() {
			const str sdk = _hooks->findSDKRoot();
			if (sdk.empty()) {
				return false;
			}
			CSharpProjectFiles::Options files = _editor->getDefaultProjectOptions(_project.generic_string(), sdk);
			files.projectName = "Space Game";
			CSharpProjectFiles::Result written;
			if (!_editor->writeProjectFiles(files, written)) {
				return false;
			}
			_options.projectPath = _project.generic_string();
			_options.projectFile = "Space_Game.Scripts.csproj";
			_options.sdkRoot = sdk;
			_options.buildServers = false;
			_options.watchSources = false;
			_options.openBuild = true;
			return true;
		}

		void Begin() { _editor->beginAuthoring(_options, &_sessionOwner); }

		void Put(const str& relative, const str& text) {
			const fs::path file = _project / relative;
			fs::create_directories(file.parent_path());
			std::ofstream(file, std::ios::binary) << text;
		}

		bool PumpUntilReloads(u32 reloads, f64 seconds) {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<f64>(seconds);
			while (std::chrono::steady_clock::now() < deadline) {
				_editor->pumpAuthoring();
				if (_editor->getReloadCount() >= reloads && !_editor->getAuthoringCompile().IsBusy()) {
					return true;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			return false;
		}

		bool PumpUntilIdle(f64 seconds) {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<f64>(seconds);
			_editor->pumpAuthoring();
			while (_editor->getAuthoringCompile().IsBusy() && std::chrono::steady_clock::now() < deadline) {
				_editor->pumpAuthoring();
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			_editor->pumpAuthoring();
			return !_editor->getAuthoringCompile().IsBusy();
		}

		static const TypeInfo* Type(const char* name) { return TypeRegistry::FindType(StringHash(str("Space_Game.") + name)); }

		fs::path Stamp() const { return _project / "Library" / "CSharp" / "bin" / "Space_Game.Scripts.stamp"; }

		static CSharpUserSettings* UserSettings() {
			Vector<SettingsCategoryInfo> categories{GetMemoryResource(MemTag::Editor)};
			Settings::GetCategories(categories);
			for (const SettingsCategoryInfo& category : categories) {
				if (category.path == CSharpSettings::UserCategory) {
					return static_cast<CSharpUserSettings*>(category.live);
				}
			}
			return nullptr;
		}

		void Close() {
			_editor->shutdownAuthoring();
			EXPECT_EQ(_hooks->unloadUserAssemblies(), SLT_OK);
		}

		BuildStepStatus RunStep(std::string_view name, BuildContext& context, f64 seconds, const std::function<bool()>& until = {}) {
			Vector<BuildStepDesc> steps{GetMemoryResource(MemTag::Debug)};
			BuildStepRegistry::GetSteps(steps);
			const auto step = std::find_if(steps.begin(), steps.end(), [name](const BuildStepDesc& each) { return each.name == name; });
			if (step == steps.end()) {
				ADD_FAILURE() << "no build step named " << name;
				return BuildStepStatus::Failed;
			}
			_profile.name = "Windows";
			context.profile = &_profile;
			context.target = BuildTargetRegistry::Find(SLT_TEST_HOST_TARGET);
			context.projectRoot = _project.generic_string();
			if (context.configuration.empty()) {
				context.configuration = "Debug";
			}
			BuildStepStatus status = BuildStepStatus::Running;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<f64>(seconds);
			while (status == BuildStepStatus::Running && std::chrono::steady_clock::now() < deadline) {
				status = step->fn(context);
				if (until && until()) {
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			return status;
		}

		ResolvedProfile _profile;
	};

} // anonymous namespace

TEST_F(CSharpSessionTest, SavedScriptCompiledLoadedReloadedWithValues) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", SPINNER_FIRST);
	Begin();
	EXPECT_TRUE(_editor->isHoldingReports());
	EXPECT_TRUE(_editor->getAuthoringCompile().IsBusy());
	ASSERT_TRUE(PumpUntilReloads(1, 180.0)) << _log.Delivered().Joined();
	EXPECT_FALSE(_editor->isHoldingReports());
	EXPECT_TRUE(_editor->areScriptsLoaded());
	ASSERT_TRUE(fs::is_regular_file(Stamp()));
	const fs::file_time_type stamped = fs::last_write_time(Stamp());
	for (int pump = 0; pump < 5; ++pump) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		_editor->pumpAuthoring();
	}
	EXPECT_EQ(fs::last_write_time(Stamp()), stamped);
	const TypeInfo* type = Type("Spinner");
	ASSERT_NE(type, nullptr);
	Handle<Component> spinner = SceneSystem::CreateObject("Holder", _world)->AddComponent(type);
	CSharpScenarios::Write<f32>(spinner, "speed", 7.5f);

	Put("Assets/Scripts/Spinner.cs", SPINNER_SECOND);
	_editor->notifySourceChanged();
	ASSERT_TRUE(PumpUntilReloads(2, 180.0)) << _log.Delivered().Joined();
	ASSERT_TRUE(spinner.IsValid());
	EXPECT_FLOAT_EQ(CSharpScenarios::Read<f32>(spinner, "speed"), 7.5f);
	EXPECT_EQ(CSharpScenarios::Read<i32>(spinner, "bonus"), 42);

	CompileStatus::RequestCompile();
	ASSERT_TRUE(PumpUntilIdle(180.0));
	EXPECT_EQ(_editor->getReloadCount(), 2u);
	EXPECT_EQ(_log.Delivered().Count("C# scripts loaded in"), 1u);
	EXPECT_EQ(_log.Delivered().Count("C# reloaded in"), 1u);
}

TEST_F(CSharpSessionTest, HeldReloadShownAppliedByReloadNow) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", SPINNER_FIRST);
	Begin();
	ASSERT_TRUE(PumpUntilReloads(1, 180.0)) << _log.Delivered().Joined();
	EXPECT_FALSE(CompileStatus::Combine().reloadHeld);

	EditorEvents::PlayModeChanged.Publish(EventBus::GetGlobal(), {PlayModeState::Playing});
	Put("Assets/Scripts/Spinner.cs", SPINNER_SECOND);
	CompileStatus::RequestCompile();
	ASSERT_TRUE(PumpUntilIdle(180.0));
	EXPECT_EQ(_editor->getAuthoringCompile().GetState(), CompileState::Succeeded) << _log.Delivered().Joined();
	EXPECT_EQ(_editor->getReloadCount(), 1u);
	EXPECT_TRUE(CompileStatus::Combine().reloadHeld);
	EXPECT_EQ(FindField(Type("Spinner"), "bonus"_hash), nullptr);

	CompileStatus::RequestReload();
	EXPECT_EQ(_editor->getReloadCount(), 2u);
	EXPECT_FALSE(CompileStatus::Combine().reloadHeld);
	EXPECT_NE(FindField(Type("Spinner"), "bonus"_hash), nullptr);
	EditorEvents::PlayModeChanged.Publish(EventBus::GetGlobal(), {PlayModeState::Editing});
	ASSERT_TRUE(PumpUntilIdle(10.0));
	EXPECT_EQ(_editor->getReloadCount(), 2u);
}

TEST_F(CSharpSessionTest, FailedCompileKeepsTheLoadedScripts) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", SPINNER_FIRST);
	Begin();
	ASSERT_TRUE(PumpUntilReloads(1, 180.0)) << _log.Delivered().Joined();
	Handle<Component> spinner = SceneSystem::CreateObject("Holder", _world)->AddComponent(Type("Spinner"));
	const Component* before = spinner.Get();

	Put("Assets/Scripts/Spinner.cs", str(SPINNER_FIRST) + "broken\n");
	CompileStatus::RequestCompile();
	ASSERT_TRUE(PumpUntilIdle(180.0));
	EXPECT_EQ(_editor->getAuthoringCompile().GetState(), CompileState::Failed);
	EXPECT_EQ(_editor->getReloadCount(), 1u);
	EXPECT_EQ(spinner.Get(), before);
	EXPECT_NE(Type("Spinner"), nullptr);
}

TEST_F(CSharpSessionTest, OpenLoadsCurrentBuildBeforeScene) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", SPINNER_FIRST);
	Begin();
	ASSERT_TRUE(PumpUntilReloads(1, 180.0)) << _log.Delivered().Joined();
	const str stamped = FileSystem::ReadTextFile(Path(Stamp().generic_string()), MemTag::Editor);
	ASSERT_FALSE(stamped.empty());
	_editor->shutdownAuthoring();
	EXPECT_EQ(_hooks->unloadUserAssemblies(), SLT_OK);
	ASSERT_EQ(Type("Spinner"), nullptr);
	const u32 compiles = _editor->getAuthoringCompile().GetCompileCount();
	const u32 restores = _editor->getAuthoringCompile().GetRestoreCount();

	Begin();
	EXPECT_NE(Type("Spinner"), nullptr);
	EXPECT_TRUE(_editor->areScriptsLoaded());
	EXPECT_FALSE(_editor->isHoldingReports());
	EXPECT_FALSE(_editor->getAuthoringCompile().IsBusy());
	ASSERT_TRUE(PumpUntilIdle(10.0));
	EXPECT_EQ(_editor->getAuthoringCompile().GetCompileCount(), compiles);
	EXPECT_EQ(_editor->getAuthoringCompile().GetRestoreCount(), restores);
	EXPECT_EQ(_editor->getReloadCount(), 1u);
	EXPECT_EQ(FileSystem::ReadTextFile(Path(Stamp().generic_string()), MemTag::Editor), stamped);

	CompileStatus::RequestCompile();
	ASSERT_TRUE(PumpUntilIdle(180.0));
	EXPECT_EQ(_editor->getAuthoringCompile().GetCompileCount(), compiles + 1);
	EXPECT_EQ(_editor->getReloadCount(), 1u);
}

TEST_F(CSharpSessionTest, OpenTellsStaleBuildFromCurrent) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", SPINNER_FIRST);
	Begin();
	ASSERT_TRUE(PumpUntilReloads(1, 180.0)) << _log.Delivered().Joined();
	Close();

	Put("Assets/Textures/readme.txt", "no script");
	Begin();
	EXPECT_FALSE(_editor->getAuthoringCompile().IsBusy());
	EXPECT_TRUE(_editor->areScriptsLoaded());
	Close();

	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	str sameSize(SPINNER_FIRST);
	sameSize.replace(sameSize.find("90f"), 3, "91f");
	Put("Assets/Scripts/Spinner.cs", sameSize);
	Begin();
	EXPECT_TRUE(_editor->getAuthoringCompile().IsBusy());
	ASSERT_TRUE(PumpUntilReloads(1, 180.0)) << _log.Delivered().Joined();
	Close();

	Put("CSharp.props", "<Project>\n\t<PropertyGroup>\n\t\t<DefineConstants>$(DefineConstants);EXTRA</DefineConstants>\n\t</PropertyGroup>\n</Project>\n");
	Begin();
	EXPECT_TRUE(_editor->getAuthoringCompile().IsBusy());
	ASSERT_TRUE(PumpUntilReloads(1, 180.0)) << _log.Delivered().Joined();
	Close();

	ASSERT_TRUE(fs::remove(_project / "Library" / "CSharp" / "bin" / "Space_Game.Scripts.dll"));
	Begin();
	EXPECT_TRUE(_editor->getAuthoringCompile().IsBusy());
	ASSERT_TRUE(PumpUntilReloads(1, 180.0)) << _log.Delivered().Joined();
	EXPECT_TRUE(_editor->areScriptsLoaded());
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Error), 0u) << _log.Delivered().Joined();
}

TEST_F(CSharpSessionTest, AnotherHoldOutlivesTheSession) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", SPINNER_FIRST);
	Begin();
	ASSERT_TRUE(PumpUntilReloads(1, 180.0)) << _log.Delivered().Joined();
	SceneSystem::CreateObject("Holder", _world)->AddComponent(Type("Spinner"));
	Buffer saved;
	ASSERT_TRUE(SceneSystem::SaveSceneToMemory(_world, saved));
	SceneSystem::UnloadScene(_world);
	SceneSystem::EndFrame();
	Close();

	SceneSystem::HoldUnresolvedComponentReports();
	Begin();
	EXPECT_FALSE(_editor->isHoldingReports());
	ASSERT_TRUE(PumpUntilIdle(10.0));
	Close();
	ASSERT_TRUE(SceneSystem::LoadSceneFromMemory(saved).IsValid());
	EXPECT_EQ(_log.Delivered().Count("unloaded type(s)"), 0u) << _log.Delivered().Joined();
	SceneSystem::ReleaseUnresolvedComponentReports();
	EXPECT_EQ(_log.Delivered().Count("loaded with 1 component row(s) of 1 unloaded type(s)"), 1u) << _log.Delivered().Joined();
}

TEST_F(CSharpSessionTest, ShutdownDuringCompileReleasesReportHold) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", SPINNER_FIRST);
	Begin();
	ASSERT_TRUE(PumpUntilReloads(1, 180.0)) << _log.Delivered().Joined();
	SceneSystem::CreateObject("Holder", _world)->AddComponent(Type("Spinner"));
	Buffer saved;
	ASSERT_TRUE(SceneSystem::SaveSceneToMemory(_world, saved));
	SceneSystem::UnloadScene(_world);
	SceneSystem::EndFrame();
	Close();

	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	Put("Assets/Scripts/Spinner.cs", SPINNER_SECOND);
	Begin();
	ASSERT_TRUE(_editor->isHoldingReports());
	Close();
	EXPECT_FALSE(_editor->isHoldingReports());
	ASSERT_TRUE(SceneSystem::LoadSceneFromMemory(saved).IsValid());
	EXPECT_EQ(_log.Delivered().Count("loaded with 1 component row(s) of 1 unloaded type(s)"), 1u) << _log.Delivered().Joined();
}

TEST_F(CSharpSessionTest, OpenCompilesGoneScriptOrMovedConfiguration) {
	ScopedSettingsStore store{_root / "settings"};
	_editor->declareSettings(&_sessionOwner);
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", SPINNER_FIRST);
	Put("Assets/Scripts/Greeter.cs", GREETER);
	Begin();
	ASSERT_TRUE(PumpUntilReloads(1, 180.0)) << _log.Delivered().Joined();
	ASSERT_NE(Type("Greeter"), nullptr);
	Close();

	fs::remove(_project / "Assets" / "Scripts" / "Greeter.cs");
	Begin();
	EXPECT_TRUE(_editor->isHoldingReports());
	EXPECT_TRUE(_editor->getAuthoringCompile().IsBusy());
	ASSERT_TRUE(PumpUntilReloads(1, 180.0)) << _log.Delivered().Joined();
	EXPECT_EQ(Type("Greeter"), nullptr);
	EXPECT_NE(Type("Spinner"), nullptr);
	Close();

	Begin();
	EXPECT_FALSE(_editor->getAuthoringCompile().IsBusy());
	EXPECT_NE(Type("Spinner"), nullptr);
	Close();

	ASSERT_NE(UserSettings(), nullptr);
	UserSettings()->codeOptimization = ScriptOptimization::Release;
	Begin();
	EXPECT_TRUE(_editor->getAuthoringCompile().IsBusy());
	ASSERT_TRUE(PumpUntilReloads(1, 180.0)) << _log.Delivered().Joined();
	EXPECT_NE(Type("Spinner"), nullptr);
	Close();

	Begin();
	EXPECT_FALSE(_editor->getAuthoringCompile().IsBusy());
	EXPECT_NE(Type("Spinner"), nullptr);
	SettingsPresentation::UnregisterOwnedBy(&_sessionOwner);
}

TEST_F(CSharpSessionTest, PlayerBuildStartsWithCurrentScripts) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", SPINNER_FIRST);
	Begin();
	ASSERT_TRUE(PumpUntilReloads(1, 180.0)) << _log.Delivered().Joined();

	Put("Assets/Scripts/Spinner.cs", SPINNER_SECOND);
	_editor->setBuildRunning(true);
	BuildContext current;
	EXPECT_EQ(RunStep("csharp-load", current, 180.0), BuildStepStatus::Succeeded) << _log.Delivered().Joined();
	EXPECT_TRUE(current.GetIssues().empty());
	EXPECT_EQ(_editor->getReloadCount(), 2u);
	EXPECT_NE(FindField(Type("Spinner"), "bonus"_hash), nullptr);
	EXPECT_FALSE(CompileStatus::Combine().reloadHeld);

	Put("Assets/Scripts/Spinner.cs", SPINNER_FIRST);
	CompileStatus::RequestCompile();
	ASSERT_TRUE(PumpUntilIdle(180.0));
	EXPECT_EQ(_editor->getReloadCount(), 2u);
	EXPECT_TRUE(CompileStatus::Combine().reloadHeld);
	_editor->setBuildRunning(false);
	ASSERT_TRUE(PumpUntilReloads(3, 30.0));
	EXPECT_EQ(FindField(Type("Spinner"), "bonus"_hash), nullptr);

	const u32 compiles = _editor->getAuthoringCompile().GetCompileCount();
	BuildContext unchanged;
	EXPECT_EQ(RunStep("csharp-load", unchanged, 180.0), BuildStepStatus::Succeeded);
	EXPECT_EQ(_editor->getAuthoringCompile().GetCompileCount(), compiles + 1);
	EXPECT_EQ(_editor->getReloadCount(), 3u);

	Put("Assets/Scripts/Spinner.cs", str(SPINNER_FIRST) + "broken\n");
	BuildContext broken;
	EXPECT_EQ(RunStep("csharp-load", broken, 180.0), BuildStepStatus::Failed);
	ASSERT_EQ(broken.GetIssues().size(), 1u);
	EXPECT_NE(broken.GetIssues()[0].message.find("C# scripts don't compile"), str::npos) << broken.GetIssues()[0].message;
	EXPECT_NE(Type("Spinner"), nullptr);
	EXPECT_EQ(_editor->getReloadCount(), 3u);
}

TEST_F(CSharpSessionTest, PlayerBuildDuringPlayKeepsRunningScripts) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", SPINNER_FIRST);
	Begin();
	ASSERT_TRUE(PumpUntilReloads(1, 180.0)) << _log.Delivered().Joined();

	EditorEvents::PlayModeChanged.Publish(EventBus::GetGlobal(), {PlayModeState::Playing});
	Put("Assets/Scripts/Spinner.cs", SPINNER_SECOND);
	const u32 compiles = _editor->getAuthoringCompile().GetCompileCount();
	BuildContext context;
	EXPECT_EQ(RunStep("csharp-load", context, 60.0), BuildStepStatus::Succeeded) << _log.Delivered().Joined();
	EXPECT_TRUE(context.GetIssues().empty());
	EXPECT_EQ(_editor->getAuthoringCompile().GetCompileCount(), compiles);
	EXPECT_EQ(_editor->getReloadCount(), 1u);
	EXPECT_EQ(FindField(Type("Spinner"), "bonus"_hash), nullptr);
	EditorEvents::PlayModeChanged.Publish(EventBus::GetGlobal(), {PlayModeState::Editing});
}

TEST_F(CSharpSessionTest, PlayerBuildWaitsUntilSceneTakesReload) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", SPINNER_FIRST);
	Begin();
	ASSERT_TRUE(PumpUntilReloads(1, 180.0)) << _log.Delivered().Joined();

	Put("Assets/Scripts/Spinner.cs", SPINNER_SECOND);
	SceneSystem::Destroy(SceneSystem::CreateObject("Doomed", _world));
	const u32 compiles = _editor->getAuthoringCompile().GetCompileCount();
	const auto compiled = [&] { return _editor->getAuthoringCompile().GetCompileCount() > compiles && !_editor->getAuthoringCompile().IsBusy(); };
	BuildContext context;
	EXPECT_EQ(RunStep("csharp-load", context, 180.0, compiled), BuildStepStatus::Running) << _log.Delivered().Joined();
	EXPECT_EQ(_editor->getAuthoringCompile().GetState(), CompileState::Succeeded);
	EXPECT_EQ(_editor->getReloadCount(), 1u);
	EXPECT_EQ(FindField(Type("Spinner"), "bonus"_hash), nullptr);

	SceneSystem::EndFrame();
	EXPECT_EQ(RunStep("csharp-load", context, 30.0), BuildStepStatus::Succeeded) << _log.Delivered().Joined();
	EXPECT_TRUE(context.GetIssues().empty());
	EXPECT_EQ(_editor->getReloadCount(), 2u);
	EXPECT_NE(FindField(Type("Spinner"), "bonus"_hash), nullptr);
}

TEST_F(CSharpSessionTest, PlayerBuildRefusesStaleScripts) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", SPINNER_FIRST);
	Begin();
	ASSERT_TRUE(PumpUntilReloads(1, 180.0)) << _log.Delivered().Joined();

	Put("Assets/Scripts/Spinner.cs", SPINNER_SECOND);
	Put("Assets/Scripts/Boot.cs", BOOT_REFUSED);
	CompileStatus::RequestCompile();
	ASSERT_TRUE(PumpUntilIdle(180.0));
	ASSERT_EQ(_editor->getAuthoringCompile().GetState(), CompileState::Succeeded) << _log.Delivered().Joined();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::RolledBack) << _log.Delivered().Joined();
	EditorEvents::PlayStarting.Publish(EventBus::GetGlobal(), {});
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded);
	EXPECT_TRUE(_editor->areScriptsLoaded());
	EXPECT_EQ(_editor->getReloadCount(), 3u);

	BuildContext refused;
	EXPECT_EQ(RunStep("csharp-load", refused, 180.0), BuildStepStatus::Failed);
	ASSERT_EQ(refused.GetIssues().size(), 1u);
	EXPECT_EQ(refused.GetIssues()[0].message, "C# scripts couldn't be loaded (errors in log above)");
	EXPECT_EQ(_editor->getReloadCount(), 3u);
	EXPECT_EQ(FindField(Type("Spinner"), "bonus"_hash), nullptr);

	ASSERT_TRUE(fs::remove(_project / "Assets" / "Scripts" / "Boot.cs"));
	BuildContext fixed;
	EXPECT_EQ(RunStep("csharp-load", fixed, 180.0), BuildStepStatus::Succeeded) << _log.Delivered().Joined();
	EXPECT_TRUE(fixed.GetIssues().empty());
	EXPECT_EQ(_editor->getReloadCount(), 4u);
	EXPECT_NE(FindField(Type("Spinner"), "bonus"_hash), nullptr);
}

TEST_F(CSharpSessionTest, ReleasePlayerCompileRenamesScriptNames) {
	if (!TestPlatform::BuildsCSharpPlayers()) {
		GTEST_SKIP() << "C# players build for Windows only";
	}
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", SPINNER_FIRST);
	Put("Assets/Scripts/Saver.cs", SAVER);
	Begin();
	ASSERT_TRUE(PumpUntilReloads(1, 180.0)) << _log.Delivered().Joined();

	BuildContext context;
	context.configuration = "Release";
	context.intermediateDir = (_project / "Library" / "Player" / "Windows").generic_string();
	context.stagingDir = (_project / "Builds" / "Windows").generic_string();
	ASSERT_EQ(RunStep("csharp-validate", context, 10.0), BuildStepStatus::Succeeded);
	ASSERT_EQ(RunStep("csharp-compile", context, 300.0), BuildStepStatus::Succeeded) << _log.Delivered().Joined();
	ASSERT_EQ(RunStep("csharp-stage", context, 10.0), BuildStepStatus::Succeeded);
	const str library = ReadAll(_project / "Builds" / "Windows" / ("GameScripts" SLT_TEST_DLL_SUFFIX));
	ASSERT_FALSE(library.empty());
	EXPECT_FALSE(HoldsWord(library, "Spinner"));
	EXPECT_FALSE(HoldsWord(library, "Saver"));
	EXPECT_TRUE(HoldsWord(library, "SaveSlot"));
	EXPECT_EQ(_log.Delivered().Count("C# script names renamed: "), 1u) << _log.Delivered().Joined();
	EXPECT_EQ(_log.Delivered().Count("C# script name kept: type 'Space_Game.SaveSlot' ([KeepName])"), 1u) << _log.Delivered().Joined();
	const str map = ReadAll(_project / "Library" / "Player" / "Windows" / "symbols" / "GameScripts.map");
	EXPECT_NE(map.find("type Space_Game.Scripts Space_Game.Spinner -> "), str::npos) << map;

	BuildContext development;
	development.configuration = "Debug";
	development.intermediateDir = context.intermediateDir;
	development.stagingDir = context.stagingDir;
	ASSERT_EQ(RunStep("csharp-validate", development, 10.0), BuildStepStatus::Succeeded);
	ASSERT_EQ(RunStep("csharp-compile", development, 300.0), BuildStepStatus::Succeeded) << _log.Delivered().Joined();
	ASSERT_EQ(RunStep("csharp-stage", development, 10.0), BuildStepStatus::Succeeded);
	EXPECT_TRUE(HoldsWord(ReadAll(_project / "Builds" / "Windows" / ("GameScripts" SLT_TEST_DLL_SUFFIX)), "Spinner"));
	EXPECT_EQ(_log.Delivered().Count("C# script names renamed: "), 1u) << _log.Delivered().Joined();
	EXPECT_FALSE(fs::exists(_project / "Library" / "Player" / "Windows" / "symbols" / "GameScripts.map"));
}

TEST_F(CSharpSessionTest, SettingOffShipsTheNamesAsWritten) {
	if (!TestPlatform::BuildsCSharpPlayers()) {
		GTEST_SKIP() << "C# players build for Windows only";
	}
	fs::create_directories(_root / "store");
	std::ofstream(_root / "store" / "ProjectSettings.ini", std::ios::binary) << "[C#]\nrenameScriptNames = false\n";
	ScopedSettingsStore store{_root / "store"};
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", SPINNER_FIRST);
	Put("Assets/Scripts/Saver.cs", SAVER);
	Begin();
	ASSERT_TRUE(PumpUntilReloads(1, 180.0)) << _log.Delivered().Joined();

	const fs::path player = _project / "Library" / "CSharp" / "Player" / "Windows";
	for (const char* configuration : {"Release", "Debug"}) {
		BuildContext context;
		context.configuration = configuration;
		ASSERT_EQ(RunStep("csharp-validate", context, 10.0), BuildStepStatus::Succeeded);
		ASSERT_EQ(RunStep("csharp-compile", context, 300.0), BuildStepStatus::Succeeded) << _log.Delivered().Joined();
		const str library = ReadAll(player / "publish" / ("GameScripts" SLT_TEST_DLL_SUFFIX));
		EXPECT_TRUE(HoldsWord(library, "Spinner")) << configuration;
		EXPECT_TRUE(HoldsWord(library, "Saver")) << configuration;
		EXPECT_FALSE(fs::exists(player / "obj" / "rename")) << configuration;
	}
	EXPECT_EQ(_log.Delivered().Count("C# script names renamed: "), 0u) << _log.Delivered().Joined();
}

TEST_F(CSharpSessionTest, PlayerCompileFailureNamesLinePerBuild) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", SPINNER_FIRST);
	Put("Assets/Scripts/PlayerOnly.cs", PLAYER_ONLY);
	Begin();
	ASSERT_TRUE(PumpUntilReloads(1, 180.0)) << _log.Delivered().Joined();

	FileSystem::Mount("project", Path(_project.generic_string()));
	const fs::path source = _project / "Assets" / "Scripts" / "PlayerOnly.cs";
	for (u32 build = 1; build <= 2; ++build) {
		BuildContext context;
		ASSERT_EQ(RunStep("csharp-validate", context, 10.0), BuildStepStatus::Succeeded);
		EXPECT_EQ(RunStep("csharp-compile", context, 180.0), BuildStepStatus::Failed);
		ASSERT_EQ(context.GetIssues().size(), 1u);
		EXPECT_EQ(context.GetIssues()[0].message, "C# script compile failed (1 error(s) in log above)");
		Vector<ConsoleEntry> rows{GetMemoryResource(MemTag::Editor)};
		EditorConsole::ReadSince(0, rows);
		u32 named = 0;
		for (const ConsoleEntry& row : rows) {
			if (row.category != "C#" || !row.file.ends_with("PlayerOnly.cs")) {
				continue;
			}
			++named;
			EXPECT_TRUE(fs::equivalent(fs::path(row.file), source)) << row.file;
			EXPECT_EQ(row.level, LogLevel::Error);
			EXPECT_EQ(row.line, 6u);
			EXPECT_TRUE(row.clickable);
			EXPECT_NE(row.message.find("CS0103: "), str::npos) << row.message;
		}
		EXPECT_EQ(named, build);
	}

	fs::remove(source);
	Put("global.json", "{\"sdk\": {\"version\": \"99.0.100\", \"rollForward\": \"disable\"}}\n");
	BuildContext unreachable;
	ASSERT_EQ(RunStep("csharp-validate", unreachable, 10.0), BuildStepStatus::Succeeded);
	EXPECT_EQ(RunStep("csharp-compile", unreachable, 180.0), BuildStepStatus::Failed);
	ASSERT_EQ(unreachable.GetIssues().size(), 1u);
	const str& said = unreachable.GetIssues()[0].message;
	EXPECT_TRUE(said.starts_with("C# script compile failed with exit code ")) << said;
	EXPECT_NE(said.find("Requested SDK version: 99.0.100"), str::npos) << said;
}

TEST_F(CSharpSessionTest, OpenHoldsReportsWhileCompiling) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", SPINNER_FIRST);
	Begin();
	ASSERT_TRUE(PumpUntilReloads(1, 180.0)) << _log.Delivered().Joined();
	SceneSystem::CreateObject("Holder", _world)->AddComponent(Type("Spinner"));
	Buffer saved;
	ASSERT_TRUE(SceneSystem::SaveSceneToMemory(_world, saved));
	SceneSystem::UnloadScene(_world);
	SceneSystem::EndFrame();
	Close();

	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	Put("Assets/Scripts/Spinner.cs", SPINNER_SECOND);
	Begin();
	EXPECT_EQ(Type("Spinner"), nullptr);
	EXPECT_TRUE(_editor->isHoldingReports());
	EXPECT_TRUE(_editor->getAuthoringCompile().IsBusy());
	Handle<Scene> opened = SceneSystem::LoadSceneFromMemory(saved);
	ASSERT_TRUE(opened.IsValid());
	EXPECT_EQ(_log.Delivered().Count("unloaded type(s)"), 0u);
	ASSERT_TRUE(PumpUntilReloads(1, 180.0)) << _log.Delivered().Joined();
	EXPECT_FALSE(_editor->isHoldingReports());
	ASSERT_NE(Type("Spinner"), nullptr);
	EXPECT_NE(FindField(Type("Spinner"), "bonus"_hash), nullptr);
	Handle<SceneObject> holder = opened->FindObjectByName("Holder");
	ASSERT_TRUE(holder.IsValid());
	ASSERT_EQ(holder->GetComponentCount(), 1u);
	EXPECT_EQ(holder->GetComponentAt(0)->GetType(), Type("Spinner"));
	EXPECT_EQ(_log.Delivered().Count("unloaded type(s)"), 0u) << _log.Delivered().Joined();
	SceneSystem::UnloadScene(opened);
	SceneSystem::EndFrame();
	Close();

	Put("Assets/Scripts/Spinner.cs", str(SPINNER_SECOND) + "broken\n");
	Begin();
	EXPECT_TRUE(_editor->isHoldingReports());
	ASSERT_TRUE(SceneSystem::LoadSceneFromMemory(saved).IsValid());
	EXPECT_EQ(_log.Delivered().Count("unloaded type(s)"), 0u);
	ASSERT_TRUE(PumpUntilIdle(180.0));
	EXPECT_EQ(_editor->getAuthoringCompile().GetState(), CompileState::Failed);
	EXPECT_FALSE(_editor->isHoldingReports());
	EXPECT_EQ(_editor->getReloadCount(), 0u);
	EXPECT_EQ(_log.Delivered().Count("loaded with 1 component row(s) of 1 unloaded type(s)"), 1u) << _log.Delivered().Joined();
}

#endif
