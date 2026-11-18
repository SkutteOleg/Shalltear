#include "TestsPCH.h"

#if defined(SHALLTEAR_SHARED)

#include "CSharpRuntimeFixture.h"
#include "CSharpSettings.h"
#include "EditorTestHooks.h"
#include "ScopedSettingsStore.h"
#include "TestPlatform.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Settings.h"

#include "Shalltear/Editor/ChildProcess.h"
#include "Shalltear/Editor/CompileStatus.h"
#include "Shalltear/Editor/EditorConsole.h"
#include "Shalltear/Editor/SettingsPresentation.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace Shalltear;
using namespace Shalltear::CSharpEditor;
using namespace Shalltear::CSharpTests;

namespace {

	namespace fs = std::filesystem;

	constexpr const char* AUTHORED_SCRIPT = "using Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Spinner : Behavior {\n\tpublic float speed = 90f;\n}\n";

	constexpr const char* AUTHORED_BROKEN =
		"using Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Broken : Behavior {\n\tprotected override void OnUpdate() {\n"
		"\t\tint unused;\n\t\tMissing.Call();\n\t}\n}\n";

	char _authoringOwner = 0;

	class CSharpAuthoringTest : public CSharpRuntimeFixture {
	protected:
		const EditorTestHooks* _editor = nullptr;
		fs::path _root;
		fs::path _project;
		CSharpAuthoring::Options _options;
		str _sdk;

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

			_root = fs::temp_directory_path() /
					("shalltear_csharp_authoring_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			_project = _root / "Space Game";
			fs::create_directories(_project / "Assets" / "Scripts");
		}

		void TearDown() override {
			if (_editor) {
				_editor->shutdownAuthoring();
			}
			if (_options.buildServers) {
				TestPlatform::RunProcess(QuoteCommandArgument((fs::path(_sdk) / _editor->muxerName).generic_string()) + " build-server shutdown", 60000);
			}
			CompileStatus::UnregisterOwnedBy(&_authoringOwner);
			EditorConsole::Terminate();
			std::error_code ec;
			fs::remove_all(_root, ec);
			FileSystem::Unmount("engine");
			CSharpRuntimeFixture::TearDown();
		}

		bool Prepare(bool watchSources, bool buildServers = false, f64 debounceSeconds = 0.25) {
			_sdk = _hooks->findSDKRoot();
			if (_sdk.empty()) {
				return false;
			}
			CSharpProjectFiles::Options files = _editor->getDefaultProjectOptions(_project.generic_string(), _sdk);
			files.projectName = "Space Game";
			CSharpProjectFiles::Result written;
			if (!_editor->writeProjectFiles(files, written)) {
				return false;
			}
			_options.projectPath = _project.generic_string();
			_options.projectFile = "Space_Game.Scripts.csproj";
			_options.sdkRoot = _sdk;
			_options.buildServers = buildServers;
			_options.watchSources = watchSources;
			_options.openBuild = false;
			_options.debounceSeconds = debounceSeconds;
			_editor->beginAuthoring(_options, &_authoringOwner);
			return true;
		}

		void Put(const str& relative, const str& text) {
			const fs::path file = _project / relative;
			fs::create_directories(file.parent_path());
			std::ofstream(file, std::ios::binary) << text;
		}

		bool PumpUntil(u32 compiles, f64 seconds) {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<f64>(seconds);
			while (std::chrono::steady_clock::now() < deadline) {
				_editor->pumpAuthoring();
				const CSharpCompile& compile = _editor->getAuthoringCompile();
				if (compile.GetCompileCount() + compile.GetRestoreCount() >= compiles && !compile.IsBusy()) {
					return true;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			return false;
		}

		void PumpFor(f64 seconds) {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<f64>(seconds);
			while (std::chrono::steady_clock::now() < deadline) {
				_editor->pumpAuthoring();
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
		}

		static const CompileStatus::SourceView* FindSource(const Vector<CompileStatus::SourceView>& sources, std::string_view name) {
			for (const CompileStatus::SourceView& source : sources) {
				if (source.name == name) {
					return &source;
				}
			}
			return nullptr;
		}

		fs::path Output() const { return _project / "Library" / "CSharp" / "bin" / "Space_Game.Scripts.dll"; }

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

		fs::path CompilerServer() const {
			const str suffix = str(_editor->muxerName).substr(std::string_view("dotnet").size());
			return fs::path(_sdk) / "sdk" / _editor->getPin().sdkVersion / "Roslyn" / "bincore" / ("VBCSCompiler" + suffix);
		}

		bool ServerRuns() const {
			const auto runs = [](const fs::path& image) { return !std::fstream(image, std::ios::in | std::ios::out | std::ios::binary).is_open(); };
			return runs(CompilerServer()) || runs(fs::path(_sdk) / _editor->muxerName);
		}
	};

} // anonymous namespace

TEST_F(CSharpAuthoringTest, SavedScriptCompilesAfterTheDebounce) {
	if (!Prepare(false, false, 2.0)) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", AUTHORED_SCRIPT);
	const CSharpCompile& compile = _editor->getAuthoringCompile();

	_editor->notifySourceChanged();
	_editor->pumpAuthoring();
	EXPECT_FALSE(compile.IsBusy());
	std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	_editor->notifySourceChanged();
	const auto renewed = std::chrono::steady_clock::now();
	std::this_thread::sleep_for(std::chrono::milliseconds(1200));
	_editor->pumpAuthoring();
	if (std::chrono::steady_clock::now() - renewed < std::chrono::milliseconds(1900)) {
		EXPECT_FALSE(compile.IsBusy());
		EXPECT_EQ(compile.GetCompileCount() + compile.GetRestoreCount(), 0u);
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	_editor->pumpAuthoring();
	EXPECT_TRUE(compile.IsBusy());
	ASSERT_TRUE(PumpUntil(2, 180.0));
	EXPECT_EQ(compile.GetState(), CompileState::Succeeded) << _log.Delivered().Joined();
	EXPECT_TRUE(fs::is_regular_file(Output()));

	PumpFor(0.8);
	EXPECT_EQ(compile.GetCompileCount(), 1u);
}

TEST_F(CSharpAuthoringTest, WatcherSeesScriptsAndTheUserProps) {
	if (!Prepare(true)) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	const CSharpCompile& compile = _editor->getAuthoringCompile();
	Put("Assets/Scripts/Notes.txt", "not a script");
	Put("Library/Scratch.cs", "not under Assets");
	Put("Assets/CSharp.props", "<Project />\n");
	PumpFor(1.2);
	EXPECT_EQ(compile.GetCompileCount() + compile.GetRestoreCount(), 0u);

	Put("Assets/Scripts/Spinner.cs", AUTHORED_SCRIPT);
	ASSERT_TRUE(PumpUntil(2, 180.0));
	EXPECT_EQ(compile.GetState(), CompileState::Succeeded) << _log.Delivered().Joined();
	const u32 compiles = compile.GetCompileCount();
	const u32 restores = compile.GetRestoreCount();
	PumpFor(1.2);
	EXPECT_EQ(compile.GetCompileCount(), compiles);

	Put("CSharp.props", "<Project>\n\t<PropertyGroup>\n\t\t<DefineConstants>$(DefineConstants);MINE</DefineConstants>\n\t</PropertyGroup>\n</Project>\n");
	ASSERT_TRUE(PumpUntil(compiles + restores + 2, 180.0));
	EXPECT_EQ(compile.GetState(), CompileState::Succeeded);
	EXPECT_EQ(compile.GetRestoreCount(), restores + 1);

	Put("Assets/Scripts/Loud.CS", "namespace Space_Game;\n\npublic sealed class Loud {\n}\n");
	ASSERT_TRUE(PumpUntil(compiles + restores + 3, 180.0));
	EXPECT_EQ(compile.GetCompileCount(), compiles + 2);
}

TEST_F(CSharpAuthoringTest, AutoCompileOffWaitsForARequest) {
	ScopedSettingsStore store{_root / "settings"};
	_editor->declareSettings(&_authoringOwner);
	CSharpUserSettings* preferences = UserSettings();
	ASSERT_NE(preferences, nullptr);
	preferences->autoCompile = false;
	if (!Prepare(false)) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", AUTHORED_SCRIPT);
	const CSharpCompile& compile = _editor->getAuthoringCompile();

	_editor->notifySourceChanged();
	PumpFor(0.9);
	EXPECT_EQ(compile.GetCompileCount() + compile.GetRestoreCount(), 0u);

	preferences->autoCompile = true;
	ASSERT_TRUE(PumpUntil(2, 180.0));
	EXPECT_EQ(compile.GetState(), CompileState::Succeeded) << _log.Delivered().Joined();
	EXPECT_EQ(compile.GetCompileCount(), 1u);

	preferences->autoCompile = false;
	_editor->notifySourceChanged();
	PumpFor(0.6);
	EXPECT_EQ(compile.GetCompileCount(), 1u);
	CompileStatus::RequestCompile();
	EXPECT_TRUE(compile.IsBusy());
	ASSERT_TRUE(PumpUntil(3, 180.0));
	EXPECT_EQ(compile.GetCompileCount(), 2u);

	preferences->autoCompile = true;
	PumpFor(0.8);
	EXPECT_EQ(compile.GetCompileCount(), 2u);
	SettingsPresentation::UnregisterOwnedBy(&_authoringOwner);
}

TEST_F(CSharpAuthoringTest, CodeOptimizationPicksTheConfiguration) {
	ScopedSettingsStore store{_root / "settings"};
	_editor->declareSettings(&_authoringOwner);
	if (!Prepare(false)) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", str(AUTHORED_SCRIPT) + "#if DEBUG\n#warning built for debugging\n#endif\n");
	const CSharpCompile& compile = _editor->getAuthoringCompile();
	CompileStatus::RequestCompile();
	ASSERT_TRUE(PumpUntil(2, 180.0));
	ASSERT_EQ(compile.GetState(), CompileState::Succeeded) << _log.Delivered().Joined();
	EXPECT_EQ(compile.GetWarningCount(), 1u);

	ASSERT_NE(UserSettings(), nullptr);
	UserSettings()->codeOptimization = ScriptOptimization::Release;
	CompileStatus::RequestCompile();
	ASSERT_TRUE(PumpUntil(3, 180.0));
	EXPECT_EQ(compile.GetState(), CompileState::Succeeded);
	EXPECT_EQ(compile.GetWarningCount(), 0u);
	SettingsPresentation::UnregisterOwnedBy(&_authoringOwner);
}

TEST_F(CSharpAuthoringTest, AOTAnalysisSettingTurnsTheAnalyzersOn) {
	ScopedSettingsStore store{_root / "settings"};
	_editor->declareSettings(&_authoringOwner);
	if (!Prepare(false)) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Days.cs", "using System;\nusing Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Days : Behavior {\n\tpublic int count;\n\n"
								  "\tprotected override void OnUpdate() {\n\t\tcount = Enum.GetValues(typeof(DayOfWeek)).Length;\n\t}\n}\n");
	const CSharpCompile& compile = _editor->getAuthoringCompile();
	CompileStatus::RequestCompile();
	ASSERT_TRUE(PumpUntil(2, 180.0));
	ASSERT_EQ(compile.GetState(), CompileState::Succeeded) << _log.Delivered().Joined();
	EXPECT_EQ(compile.GetWarningCount(), 0u);

	ASSERT_NE(UserSettings(), nullptr);
	UserSettings()->aotAnalysis = true;
	CompileStatus::RequestCompile();
	ASSERT_TRUE(PumpUntil(3, 180.0));
	EXPECT_EQ(compile.GetState(), CompileState::Succeeded);
	ASSERT_EQ(compile.GetWarningCount(), 1u);
	EXPECT_EQ(compile.GetDiagnostics()[0].code, "IL3050");
	SettingsPresentation::UnregisterOwnedBy(&_authoringOwner);
}

TEST_F(CSharpAuthoringTest, ShutdownEndsCompileAndPendingSave) {
	if (!Prepare(false)) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", AUTHORED_SCRIPT);
	const CSharpCompile& compile = _editor->getAuthoringCompile();
	CompileStatus::RequestCompile();
	EXPECT_TRUE(compile.IsBusy());
	EXPECT_TRUE(_editor->isAuthoringActive());
	_editor->notifySourceChanged();
	_editor->shutdownAuthoring();
	EXPECT_FALSE(compile.IsBusy());
	EXPECT_EQ(compile.GetState(), CompileState::Idle);
	EXPECT_FALSE(_editor->isAuthoringActive());

	_editor->beginAuthoring(_options, &_authoringOwner);
	PumpFor(0.8);
	EXPECT_FALSE(compile.IsBusy());
	EXPECT_EQ(compile.GetRestoreCount(), 1u);
}

TEST_F(CSharpAuthoringTest, RestoreStampCarriesThePin) {
	if (!Prepare(false)) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", AUTHORED_SCRIPT);
	CompileStatus::RequestCompile();
	ASSERT_TRUE(PumpUntil(2, 180.0));
	ASSERT_EQ(_editor->getAuthoringCompile().GetState(), CompileState::Succeeded) << _log.Delivered().Joined();
	_editor->shutdownAuthoring();

	CompileOptions bare;
	bare.projectPath = _options.projectPath;
	bare.projectFile = _options.projectFile;
	bare.muxer = (fs::path(_sdk) / _editor->muxerName).generic_string();
	bare.buildServers = false;
	_editor->configureCompile(bare);
	ASSERT_TRUE(_editor->requestCompile());
	EXPECT_EQ(_editor->getCompile().GetState(), CompileState::Configuring);
	_editor->cancelCompile();
}

TEST_F(CSharpAuthoringTest, StatusSourceReportsTheCompile) {
	Vector<CompileStatus::SourceView> sources{GetMemoryResource(MemTag::Editor)};
	CompileStatus::GetSources(sources);
	EXPECT_EQ(FindSource(sources, "C#"), nullptr);
	if (!Prepare(false)) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	CompileStatus::GetSources(sources);
	const CompileStatus::SourceView* source = FindSource(sources, "C#");
	ASSERT_NE(source, nullptr);
	EXPECT_EQ(source->heading, "Space_Game.Scripts");
	EXPECT_EQ(source->state.compile, CompileState::Idle);
	EXPECT_NE(source->state.typeOwner, nullptr);

	_editor->beginAuthoring(_options, &_authoringOwner);
	CompileStatus::GetSources(sources);
	EXPECT_EQ(std::count_if(sources.begin(), sources.end(), [](const CompileStatus::SourceView& view) { return view.name == "C#"; }), 1);

	Put("Assets/Scripts/Broken.cs", AUTHORED_BROKEN);
	PumpFor(0.6);
	EXPECT_EQ(_editor->getAuthoringCompile().GetCompileCount() + _editor->getAuthoringCompile().GetRestoreCount(), 0u);
	CompileStatus::RequestCompile();
	CompileStatus::GetSources(sources);
	EXPECT_EQ(FindSource(sources, "C#")->state.compile, CompileState::Configuring);
	ASSERT_TRUE(PumpUntil(2, 180.0));
	CompileStatus::GetSources(sources);
	source = FindSource(sources, "C#");
	EXPECT_EQ(source->state.compile, CompileState::Failed);
	EXPECT_EQ(source->state.errors, 1u);
	EXPECT_EQ(source->state.warnings, 1u);
	EXPECT_EQ(CompileStatus::Combine().compile, CompileState::Failed);
	EXPECT_EQ(CompileStatus::Combine().errors, 1u);

	_editor->shutdownAuthoring();
	CompileStatus::GetSources(sources);
	EXPECT_EQ(FindSource(sources, "C#"), nullptr);
	_editor->notifySourceChanged();
	PumpFor(0.8);
	EXPECT_EQ(_editor->getAuthoringCompile().GetCompileCount(), 1u);
}

TEST_F(CSharpAuthoringTest, BuildServersOutliveCompileStopAtShutdown) {
	if (!Prepare(false, true)) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_TRUE(fs::is_regular_file(CompilerServer())) << CompilerServer();
	Put("Assets/Scripts/Spinner.cs", AUTHORED_SCRIPT);
	const CSharpCompile& compile = _editor->getAuthoringCompile();
	CompileStatus::RequestCompile();
	ASSERT_TRUE(PumpUntil(2, 180.0));
	ASSERT_EQ(compile.GetState(), CompileState::Succeeded) << _log.Delivered().Joined();
	EXPECT_TRUE(ServerRuns());

	Put("Assets/Scripts/Spinner.cs", str(AUTHORED_SCRIPT) + "\n");
	CompileStatus::RequestCompile();
	ASSERT_TRUE(PumpUntil(3, 180.0));
	EXPECT_EQ(compile.GetState(), CompileState::Succeeded);
	EXPECT_TRUE(ServerRuns());

	_editor->shutdownAuthoring();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	bool runs = ServerRuns();
	while (runs && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		runs = ServerRuns();
	}
	EXPECT_FALSE(runs);
}

TEST_F(CSharpAuthoringTest, CompileWithoutBuildServersLeavesNone) {
	if (!Prepare(false)) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_TRUE(fs::is_regular_file(CompilerServer())) << CompilerServer();
	if (ServerRuns()) {
		GTEST_SKIP() << "a compiler server of this SDK already runs on this machine";
	}
	Put("Assets/Scripts/Spinner.cs", AUTHORED_SCRIPT);
	CompileStatus::RequestCompile();
	ASSERT_TRUE(PumpUntil(2, 180.0));
	ASSERT_EQ(_editor->getAuthoringCompile().GetState(), CompileState::Succeeded) << _log.Delivered().Joined();
	EXPECT_FALSE(ServerRuns());
}

#endif
