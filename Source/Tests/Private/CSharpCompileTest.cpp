#include "TestsPCH.h"

#if defined(SHALLTEAR_SHARED)

#include "CSharpRuntimeFixture.h"
#include "EditorTestHooks.h"

#include "Shalltear/Core/FileSystem.h"

#include "Shalltear/Editor/ChildProcess.h"
#include "Shalltear/Editor/EditorConsole.h"
#include "Shalltear/Editor/EditorEvents.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace Shalltear;
using namespace Shalltear::CSharpEditor;
using namespace Shalltear::CSharpTests;

namespace {

	namespace fs = std::filesystem;

	constexpr const char* GOOD_SCRIPT = "using Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Spinner : Behavior {\n\tpublic float speed = 90f;\n"
										"\n\tprotected override void OnUpdate() {\n\t\tspeed += Time.DeltaTime;\n\t}\n}\n";

	constexpr const char* BROKEN_SCRIPT =
		"using Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Broken : Behavior {\n\tprotected override void OnUpdate() {\n"
		"\t\tint unused;\n\t\tMissing.Call();\n\t}\n}\n";

	constexpr const char* REFLECTING_SCRIPT =
		"using System;\nusing Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Finder : Behavior {\n"
		"\tpublic string wanted = \"System.String\";\n\tpublic int found;\n\n\tprotected override void OnUpdate() {\n"
		"\t\tfound += Type.GetType(wanted) is null ? 0 : 1;\n\t\tfound += Enum.GetValues(typeof(DayOfWeek)).Length;\n"
		"\t\tfound += typeof(Finder).Assembly.Location.Length;\n\t}\n}\n";

	class CompileStateListener : public Object {
	public:
		std::vector<EditorCompileStateChangedEvent> events;

		CompileStateListener() {
			EditorEvents::CompileStateChanged.Subscribe(EventBus::GetGlobal(), Handle<CompileStateListener>(this), &CompileStateListener::OnChanged);
		}
		~CompileStateListener() override { EventBus::GetGlobal().UnsubscribeAll(GetInstanceId()); }

		void OnChanged(const EditorCompileStateChangedEvent& event) { events.push_back(event); }

		str States() const {
			str states;
			for (const EditorCompileStateChangedEvent& event : events) {
				states += std::to_string(event.state) + " ";
			}
			return states;
		}
	};

	str Expected(std::initializer_list<CompileState> states) {
		str text;
		for (const CompileState state : states) {
			text += std::to_string(static_cast<u8>(state)) + " ";
		}
		return text;
	}

	class CSharpCompileTest : public CSharpRuntimeFixture {
	protected:
		const EditorTestHooks* _editor = nullptr;
		fs::path _root;
		fs::path _project;
		str _sdk;
		CompileOptions _options;
		CompileStateListener _listener;

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
					("shalltear_csharp_compile_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			_project = _root / "Space Game";
			fs::create_directories(_project);
		}

		void TearDown() override {
			if (_editor) {
				_editor->cancelCompile();
			}
			EditorConsole::Terminate();
			std::error_code ec;
			fs::remove_all(_root, ec);
			FileSystem::Unmount("engine");
			CSharpRuntimeFixture::TearDown();
		}

		bool Prepare() {
			_sdk = _hooks->findSDKRoot();
			if (_sdk.empty()) {
				return false;
			}
			if (!WriteFiles(false)) {
				return false;
			}
			_options.projectPath = _project.generic_string();
			_options.projectFile = "Space_Game.Scripts.csproj";
			_options.muxer = (fs::path(_sdk) / _editor->muxerName).generic_string();
			_options.pinStamp = "pin 1";
			_options.buildServers = false;
			_editor->configureCompile(_options);
			return true;
		}

		bool WriteFiles(bool allowUnsafeCode) {
			CSharpProjectFiles::Options files = _editor->getDefaultProjectOptions(_project.generic_string(), _sdk);
			files.projectName = "Space Game";
			files.allowUnsafeCode = allowUnsafeCode;
			CSharpProjectFiles::Result written;
			return _editor->writeProjectFiles(files, written);
		}

		void Put(const str& relative, const str& text) {
			const fs::path file = _project / relative;
			fs::create_directories(file.parent_path());
			std::ofstream(file, std::ios::binary) << text;
		}

		CompileState Finish() {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
			while (_editor->getCompile().IsBusy() && std::chrono::steady_clock::now() < deadline) {
				_editor->pumpCompile();
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			return _editor->getCompile().GetState();
		}

		CompileState Compile() {
			EXPECT_TRUE(_editor->requestCompile());
			return Finish();
		}

		str BuildOutsideTheEditor() {
			ChildProcess child;
			const str command = QuoteCommandArgument(_options.muxer) + " build " + _options.projectFile +
								" --disable-build-servers -nologo -v q --no-restore -p:ShalltearBuildRoot=Library/CSharp/";
			if (!child.Start(command, _options.projectPath)) {
				return "child didn't start";
			}
			str output;
			Vector<str> drained{GetMemoryResource(MemTag::Editor)};
			i32 exitCode = -1;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
			for (bool finished = false; !finished && std::chrono::steady_clock::now() < deadline;) {
				finished = child.TryFinish(exitCode);
				drained.clear();
				child.DrainLines(drained);
				for (const str& line : drained) {
					output += line + "\n";
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			return output + "exit " + std::to_string(exitCode) + "\n";
		}

		static Vector<ConsoleEntry> Rows() {
			Vector<ConsoleEntry> rows{GetMemoryResource(MemTag::Editor)};
			EditorConsole::ReadSince(0, rows);
			return rows;
		}

		fs::path Output() const { return _project / "Library" / "CSharp" / "bin" / "Space_Game.Scripts.dll"; }
	};

} // anonymous namespace

TEST_F(CSharpCompileTest, SuccessProducesAssembly) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", GOOD_SCRIPT);
	EXPECT_EQ(_editor->getCompile().GetState(), CompileState::Idle);
	ASSERT_EQ(Compile(), CompileState::Succeeded) << _log.Delivered().Joined();
	EXPECT_EQ(_listener.States(), Expected({CompileState::Configuring, CompileState::Compiling, CompileState::Succeeded}));

	const CSharpCompile& compile = _editor->getCompile();
	EXPECT_EQ(compile.GetErrorCount(), 0u);
	EXPECT_EQ(compile.GetWarningCount(), 0u);
	EXPECT_EQ(compile.GetRestoreCount(), 1u);
	EXPECT_EQ(compile.GetCompileCount(), 1u);
	EXPECT_GT(compile.GetLastDurationSeconds(), 0.0);
	EXPECT_TRUE(fs::is_regular_file(Output()));
	EXPECT_TRUE(fs::is_regular_file(Output().replace_extension(".pdb")));
	EXPECT_EQ(_editor->getCompileOutput(), Output().generic_string());
	EXPECT_FALSE(fs::exists(_project / "Library" / "CSharp" / "IDE"));
	EXPECT_FALSE(fs::exists(_project / "obj"));
	EXPECT_FALSE(fs::exists(_project / "bin"));
	EXPECT_EQ(_log.Delivered().Count("compile succeeded in"), 1u);
}

TEST_F(CSharpCompileTest, ErrorReachesConsoleWithOrigin) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	DeviceLossTest::ScopedEnvironment language("DOTNET_CLI_UI_LANGUAGE", "de");
	Put("CSharp.props", "<Project>\n\t<PropertyGroup>\n\t\t<GenerateFullPaths>false</GenerateFullPaths>\n\t</PropertyGroup>\n</Project>\n");
	Put("Assets/Scripts/Spinner.cs", GOOD_SCRIPT);
	Put("Assets/Scripts/Broken.cs", BROKEN_SCRIPT);
	ASSERT_EQ(Compile(), CompileState::Failed);
	ASSERT_FALSE(_listener.events.empty());
	EXPECT_EQ(_listener.events.back().state, static_cast<u8>(CompileState::Failed));
	EXPECT_EQ(_listener.events.back().errorCount, 1u);
	EXPECT_EQ(_listener.events.back().warningCount, 1u);

	const CSharpCompile& compile = _editor->getCompile();
	EXPECT_EQ(compile.GetErrorCount(), 1u);
	EXPECT_EQ(compile.GetWarningCount(), 1u);
	ASSERT_EQ(compile.GetDiagnostics().size(), 2u);
	EXPECT_FALSE(fs::exists(Output()));

	const fs::path source = _project / "Assets" / "Scripts" / "Broken.cs";
	u32 errors = 0;
	u32 warnings = 0;
	for (const ConsoleEntry& row : Rows()) {
		EXPECT_EQ(row.message.find("failed with exit code"), str::npos) << row.message;
		if (row.category != "C#" || !row.file.ends_with("Broken.cs")) {
			continue;
		}
		ASSERT_TRUE(fs::path(row.file).is_absolute()) << row.file;
		EXPECT_TRUE(fs::equivalent(fs::path(row.file), source)) << row.file;
		EXPECT_TRUE(row.clickable);
		if (row.level == LogLevel::Error) {
			++errors;
			EXPECT_EQ(row.line, 8u);
			EXPECT_EQ(row.column, 3u);
			EXPECT_NE(row.message.find("CS0103: The name 'Missing' does not exist in the current context"), str::npos) << row.message;
			EXPECT_NE(row.message.find(":8:3: "), str::npos) << row.message;
		} else if (row.level == LogLevel::Warn) {
			++warnings;
			EXPECT_EQ(row.line, 7u);
			EXPECT_NE(row.message.find("CS0168"), str::npos) << row.message;
		}
	}
	EXPECT_EQ(errors, 1u);
	EXPECT_EQ(warnings, 1u);
	EXPECT_EQ(_log.Delivered().Count("compile FAILED after"), 1u);

	fs::remove(source);
	ASSERT_EQ(Compile(), CompileState::Succeeded) << _log.Delivered().Joined();
	EXPECT_EQ(compile.GetErrorCount(), 0u);
	EXPECT_EQ(compile.GetWarningCount(), 0u);
	EXPECT_TRUE(compile.GetDiagnostics().empty());
	EXPECT_TRUE(fs::is_regular_file(Output()));
}

TEST_F(CSharpCompileTest, LinkerAnalyzersStayOutOfEditorCompile) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Finder.cs", REFLECTING_SCRIPT);
	const CSharpCompile& compile = _editor->getCompile();
	ASSERT_EQ(Compile(), CompileState::Succeeded) << _log.Delivered().Joined();
	EXPECT_EQ(compile.GetWarningCount(), 0u);

	Put("Assets/Scripts/Finder.cs", str(REFLECTING_SCRIPT) + "\n");
	const str outside = BuildOutsideTheEditor();
	EXPECT_NE(outside.find("warning IL2057"), str::npos) << outside;
	EXPECT_NE(outside.find("warning IL3050"), str::npos) << outside;
	EXPECT_NE(outside.find("warning IL3000"), str::npos) << outside;
	EXPECT_NE(outside.find("exit 0\n"), str::npos) << outside;

	Put("CSharp.props", "<Project>\n\t<PropertyGroup>\n\t\t<EnableTrimAnalyzer>true</EnableTrimAnalyzer>\n\t</PropertyGroup>\n</Project>\n");
	Put("Assets/Scripts/Finder.cs", REFLECTING_SCRIPT);
	ASSERT_EQ(Compile(), CompileState::Succeeded) << _log.Delivered().Joined();
	ASSERT_GE(compile.GetWarningCount(), 1u);
	bool trim = false;
	for (const CSharpDiagnostics::Diagnostic& diagnostic : compile.GetDiagnostics()) {
		trim = trim || diagnostic.code == "IL2057";
		EXPECT_TRUE(diagnostic.code.starts_with("IL2")) << diagnostic.code;
	}
	EXPECT_TRUE(trim);

	fs::remove(_project / "CSharp.props");
	_options.aotAnalysis = true;
	_editor->configureCompile(_options);
	ASSERT_EQ(Compile(), CompileState::Succeeded) << _log.Delivered().Joined();
	str codes;
	for (const CSharpDiagnostics::Diagnostic& diagnostic : compile.GetDiagnostics()) {
		codes += diagnostic.code + " ";
	}
	EXPECT_EQ(compile.GetWarningCount(), 3u) << codes;
	for (const char* code : {"IL2057", "IL3050", "IL3000"}) {
		EXPECT_NE(codes.find(code), str::npos) << codes;
	}
}

TEST_F(CSharpCompileTest, RestoreRunsOnlyWhenItsInputsChange) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", GOOD_SCRIPT);
	const CSharpCompile& compile = _editor->getCompile();
	ASSERT_EQ(Compile(), CompileState::Succeeded) << _log.Delivered().Joined();
	EXPECT_EQ(compile.GetRestoreCount(), 1u);

	Put("Assets/Scripts/Spinner.cs", str(GOOD_SCRIPT) + "\n");
	ASSERT_EQ(Compile(), CompileState::Succeeded);
	EXPECT_EQ(compile.GetRestoreCount(), 1u);
	EXPECT_EQ(compile.GetCompileCount(), 2u);

	Put("CSharp.props", "<Project>\n\t<PropertyGroup>\n\t\t<DefineConstants>$(DefineConstants);MINE</DefineConstants>\n\t</PropertyGroup>\n</Project>\n");
	ASSERT_EQ(Compile(), CompileState::Succeeded);
	EXPECT_EQ(compile.GetRestoreCount(), 2u);

	_options.pinStamp = "pin 2";
	_editor->configureCompile(_options);
	ASSERT_EQ(Compile(), CompileState::Succeeded);
	EXPECT_EQ(compile.GetRestoreCount(), 3u);

	fs::remove(_project / "Library" / "CSharp" / "obj" / "project.assets.json");
	ASSERT_EQ(Compile(), CompileState::Succeeded);
	EXPECT_EQ(compile.GetRestoreCount(), 4u);

	ASSERT_EQ(Compile(), CompileState::Succeeded);
	EXPECT_EQ(compile.GetRestoreCount(), 4u);
	EXPECT_EQ(compile.GetCompileCount(), 6u);

	ASSERT_TRUE(WriteFiles(true));
	ASSERT_EQ(Compile(), CompileState::Succeeded);
	EXPECT_EQ(compile.GetRestoreCount(), 5u);

	const str project = FileSystem::ReadTextFile(Path((_project / "Space_Game.Scripts.csproj").generic_string()));
	ASSERT_FALSE(project.empty());
	Put("Space_Game.Scripts.csproj", project + "\n");
	ASSERT_EQ(Compile(), CompileState::Succeeded);
	EXPECT_EQ(compile.GetRestoreCount(), 6u);
}

TEST_F(CSharpCompileTest, UnwritableStampRestoresEachTime) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", GOOD_SCRIPT);
	const fs::path stamp = _project / "Library" / "CSharp" / "obj" / "Shalltear.Restore.stamp";
	fs::create_directories(stamp);
	const CSharpCompile& compile = _editor->getCompile();
	ASSERT_EQ(Compile(), CompileState::Succeeded) << _log.Delivered().Joined();
	const str warning = "couldn't write '" + stamp.generic_string() + "' (restore will repeat)";
	EXPECT_EQ(_log.Delivered().Count(warning.c_str()), 1u) << _log.Delivered().Joined();
	ASSERT_EQ(Compile(), CompileState::Succeeded);
	EXPECT_EQ(compile.GetRestoreCount(), 2u);
	EXPECT_EQ(_log.Delivered().Count(warning.c_str()), 2u);

	fs::remove(stamp);
	ASSERT_EQ(Compile(), CompileState::Succeeded);
	ASSERT_EQ(Compile(), CompileState::Succeeded);
	EXPECT_EQ(compile.GetRestoreCount(), 3u);
}

TEST_F(CSharpCompileTest, WarningsOutliveACompileThatChangesNothing) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", str(GOOD_SCRIPT) + "#warning still here\n");
	const CSharpCompile& compile = _editor->getCompile();
	const auto pushed = []() {
		u32 rows = 0;
		for (const ConsoleEntry& row : Rows()) {
			rows += row.category == "C#" && row.message.find("CS1030") != str::npos ? 1u : 0u;
		}
		return rows;
	};
	ASSERT_EQ(Compile(), CompileState::Succeeded) << _log.Delivered().Joined();
	EXPECT_EQ(compile.GetWarningCount(), 1u);
	EXPECT_EQ(pushed(), 1u);

	ASSERT_EQ(Compile(), CompileState::Succeeded);
	EXPECT_EQ(compile.GetWarningCount(), 1u);
	ASSERT_EQ(compile.GetDiagnostics().size(), 1u);
	EXPECT_EQ(compile.GetDiagnostics()[0].code, "CS1030");
	EXPECT_EQ(pushed(), 1u);
	EXPECT_EQ(_listener.events.back().warningCount, 1u);

	Put("Assets/Scripts/Spinner.cs", GOOD_SCRIPT);
	ASSERT_EQ(Compile(), CompileState::Succeeded);
	EXPECT_EQ(compile.GetWarningCount(), 0u);
	EXPECT_TRUE(compile.GetDiagnostics().empty());

	ASSERT_EQ(Compile(), CompileState::Succeeded);
	EXPECT_EQ(compile.GetWarningCount(), 0u);
}

TEST_F(CSharpCompileTest, FailedRestoreIsReportedAndRunsAgain) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", GOOD_SCRIPT);
	fs::create_directories(_root / "feed");
	Put("CSharp.props", "<Project>\n\t<PropertyGroup>\n\t\t<RestoreSources>" + (_root / "feed").generic_string() +
							"</RestoreSources>\n\t</PropertyGroup>\n\t<ItemGroup>\n\t\t<PackageReference Include=\"Shalltear.No.Such.Package\" "
							"Version=\"1.0.0\" />\n\t</ItemGroup>\n</Project>\n");
	const CSharpCompile& compile = _editor->getCompile();
	ASSERT_EQ(Compile(), CompileState::Failed);
	EXPECT_EQ(compile.GetRestoreCount(), 1u);
	EXPECT_EQ(compile.GetCompileCount(), 0u);
	ASSERT_FALSE(compile.GetDiagnostics().empty());
	EXPECT_EQ(compile.GetDiagnostics()[0].code, "NU1101");
	EXPECT_EQ(compile.GetErrorCount(), static_cast<u32>(compile.GetDiagnostics().size()));
	EXPECT_EQ(_listener.States(), Expected({CompileState::Configuring, CompileState::Failed}));
	EXPECT_EQ(_log.Delivered().Count("restore FAILED after"), 1u) << _log.Delivered().Joined();
	EXPECT_EQ(_log.Delivered().Count("compile FAILED after"), 0u);

	ASSERT_EQ(Compile(), CompileState::Failed);
	EXPECT_EQ(compile.GetRestoreCount(), 2u);
	ASSERT_FALSE(compile.GetDiagnostics().empty());
	EXPECT_EQ(compile.GetDiagnostics()[0].code, "NU1101");

	fs::remove(_project / "CSharp.props");
	ASSERT_EQ(Compile(), CompileState::Succeeded) << _log.Delivered().Joined();
	EXPECT_EQ(compile.GetRestoreCount(), 3u);
	EXPECT_EQ(compile.GetErrorCount(), 0u);
}

TEST_F(CSharpCompileTest, FailureWithoutDiagnosticsStillCountsAnError) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", GOOD_SCRIPT);
	Put("global.json", "{ \"sdk\": { \"version\": \"99.9.999\", \"rollForward\": \"disable\" } }\n");
	const CSharpCompile& compile = _editor->getCompile();
	ASSERT_EQ(Compile(), CompileState::Failed);
	EXPECT_EQ(compile.GetErrorCount(), 1u);
	EXPECT_TRUE(compile.GetDiagnostics().empty());
	EXPECT_GT(compile.GetLastDurationSeconds(), 0.0);

	u32 rows = 0;
	for (const ConsoleEntry& row : Rows()) {
		rows += row.level == LogLevel::Error && row.category == "C#" && row.message.starts_with("restore failed with exit code ") &&
						row.message.find("\nRequested SDK version: 99.9.999\n") != str::npos && !row.clickable
					? 1u
					: 0u;
	}
	EXPECT_EQ(rows, 1u);
	EXPECT_EQ(_log.Delivered().Count("restore FAILED after"), 1u);
}

TEST_F(CSharpCompileTest, ARequestWhileBusyRunsOnceMore) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", GOOD_SCRIPT);
	EXPECT_TRUE(_editor->requestCompile());
	EXPECT_TRUE(_editor->getCompile().IsBusy());
	EXPECT_TRUE(_editor->requestCompile());
	EXPECT_TRUE(_editor->requestCompile());
	ASSERT_EQ(Finish(), CompileState::Succeeded) << _log.Delivered().Joined();
	EXPECT_EQ(_editor->getCompile().GetCompileCount(), 1u);
	_editor->pumpCompile();
	EXPECT_TRUE(_editor->getCompile().IsBusy());
	ASSERT_EQ(Finish(), CompileState::Succeeded);
	_editor->pumpCompile();
	EXPECT_FALSE(_editor->getCompile().IsBusy());
	EXPECT_EQ(_editor->getCompile().GetCompileCount(), 2u);
	EXPECT_EQ(_editor->getCompile().GetRestoreCount(), 1u);
}

TEST_F(CSharpCompileTest, CancelStopsTheBuild) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", GOOD_SCRIPT);
	EXPECT_TRUE(_editor->requestCompile());
	EXPECT_TRUE(_editor->requestCompile());
	_editor->cancelCompile();
	EXPECT_EQ(_editor->getCompile().GetState(), CompileState::Idle);
	EXPECT_EQ(_listener.States(), Expected({CompileState::Configuring, CompileState::Idle}));
	_editor->pumpCompile();
	EXPECT_FALSE(_editor->getCompile().IsBusy());
	EXPECT_FALSE(fs::exists(Output()));

	ASSERT_EQ(Compile(), CompileState::Succeeded) << _log.Delivered().Joined();
	EXPECT_TRUE(fs::is_regular_file(Output()));
}

TEST_F(CSharpCompileTest, MissingToolchainFailsWithoutAChild) {
	if (!Prepare()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	_options.muxer = (_root / "nowhere" / "dotnet").generic_string();
	_editor->configureCompile(_options);
	EXPECT_FALSE(_editor->requestCompile());
	EXPECT_EQ(_editor->getCompile().GetState(), CompileState::Failed);
	EXPECT_EQ(_editor->getCompile().GetErrorCount(), 1u);
	EXPECT_EQ(_listener.States(), Expected({CompileState::Configuring, CompileState::Failed}));
	EXPECT_EQ(_listener.events.back().errorCount, 1u);
	const str said = "couldn't start '" + _options.muxer + "' for restore. Reopen the project";
	u32 rows = 0;
	for (const ConsoleEntry& row : Rows()) {
		rows += row.level == LogLevel::Error && row.message == said ? 1u : 0u;
	}
	EXPECT_EQ(rows, 1u);

	_options.muxer.clear();
	_editor->configureCompile(_options);
	EXPECT_FALSE(_editor->requestCompile());
	EXPECT_EQ(_editor->getCompile().GetRestoreCount(), 1u);
}

#endif
