#include "TestsPCH.h"

#if defined(SHALLTEAR_SHARED)

#include "CSharpRuntimeFixture.h"
#include "EditorTestHooks.h"
#include "TestPlatform.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Serialization/JSON.h"

#include "Shalltear/Editor/ChildProcess.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

using namespace Shalltear;
using namespace Shalltear::CSharpEditor;
using namespace Shalltear::CSharpTests;

namespace {

	namespace fs = std::filesystem;

	constexpr const char* PATHS_FILE = "Library/CSharp/Generated/Shalltear.Paths.props";
	constexpr const char* PROJECT_FILE = "Space_Game.Scripts.csproj";
	constexpr const char* SOLUTION_FILE = "Space_Game.slnx";

	char _environmentOwner = 0;

	class CSharpProjectFilesTest : public CSharpRuntimeFixture {
	protected:
		const EditorTestHooks* _editor = nullptr;
		fs::path _root;
		fs::path _project;
		CSharpProjectFiles::Options _options;

		void SetUp() override {
			CSharpRuntimeFixture::SetUp();
			const auto get = reinterpret_cast<GetEditorTestHooksFn>(
				ModuleSystem::FindSymbol(ModuleSystem::GetNativeHandle(_package), EDITOR_TEST_HOOKS_SYMBOL));
			ASSERT_NE(get, nullptr);
			_editor = get();
			const fs::path engineAssets = DeviceLossTest::RepoDir("EngineAssets");
			ASSERT_FALSE(engineAssets.empty());
			FileSystem::Mount("engine", Path(engineAssets.generic_string()));
			_root = fs::temp_directory_path() /
					("shalltear_csharp_files_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			_project = _root / "Space Game";
			fs::create_directories(_project);
			_options.projectPath = _project.generic_string();
			_options.projectName = "Space Game";
			_options.dotnetRoot = (_root / "sdks" / "dotnet").generic_string();
			_options.engineRoot = (_root / "R&D" / "engine").generic_string();
			_options.managedDirectory = (_root / "R&D" / "engine" / "bin" / "CSharp").generic_string();
			_options.packagesRoot = (_root / "packages").generic_string();
			_options.pin.sdkVersion = "10.0.999";
			_options.pin.runtimeVersion = "10.0.99";
			_options.pin.targetFramework = "net10.0";
		}

		void TearDown() override {
			ChildEnvironment::UnregisterOwnedBy(&_environmentOwner);
			std::error_code ec;
			fs::remove_all(_root, ec);
			FileSystem::Unmount("engine");
			CSharpRuntimeFixture::TearDown();
		}

		str Read(const str& relative) {
			std::ifstream file(_project / relative, std::ios::binary);
			std::ostringstream text;
			text << file.rdbuf();
			return text.str();
		}

		void Put(const str& relative, const str& text) {
			const fs::path file = _project / relative;
			fs::create_directories(file.parent_path());
			std::ofstream(file, std::ios::binary) << text;
		}

		std::vector<str> Write() {
			CSharpProjectFiles::Result result;
			EXPECT_TRUE(_editor->writeProjectFiles(_options, result));
			std::vector<str> written(result.written.begin(), result.written.end());
			std::sort(written.begin(), written.end());
			return written;
		}

		static str Escaped(str text) {
			for (size_t at = text.find('&'); at != str::npos; at = text.find('&', at + 1)) {
				text.replace(at, 1, "&amp;");
			}
			return text;
		}

		static bool HasLine(const str& text, std::string_view wanted) {
			std::istringstream lines(text);
			for (str line; std::getline(lines, line);) {
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				if (line == wanted) {
					return true;
				}
			}
			return false;
		}

		static bool IsIgnored(const str& ignoreText, const str& relative) {
			std::istringstream lines(ignoreText);
			for (str line; std::getline(lines, line);) {
				if (line.size() < 2 || line.front() != '/') {
					continue;
				}
				const str pattern = line.substr(1);
				if (pattern.back() == '/') {
					if (relative.starts_with(pattern)) {
						return true;
					}
				} else if (pattern.front() == '*') {
					if (relative.find('/') == str::npos && relative.ends_with(pattern.substr(1))) {
						return true;
					}
				} else if (relative == pattern) {
					return true;
				}
			}
			return false;
		}

		std::vector<str> RunIn(const fs::path& directory, const str& commandLine, i32& outExitCode) {
			std::vector<str> lines;
			outExitCode = -1;
			ChildProcess child;
			if (!child.Start(commandLine, directory.generic_string())) {
				ADD_FAILURE() << "child didn't start: " << commandLine;
				return lines;
			}
			Vector<str> drained{GetMemoryResource(MemTag::Editor)};
			for (u32 attempt = 0; attempt < 60000; ++attempt) {
				drained.clear();
				child.DrainLines(drained);
				lines.insert(lines.end(), drained.begin(), drained.end());
				if (child.TryFinish(outExitCode)) {
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			drained.clear();
			child.DrainLines(drained);
			lines.insert(lines.end(), drained.begin(), drained.end());
			return lines;
		}

		static str Joined(const std::vector<str>& lines) {
			str joined;
			for (const str& line : lines) {
				joined += line;
				joined += '\n';
			}
			return joined;
		}

		str FindOtherInstall(const str& pinnedRoot) {
			std::error_code ec;
			const fs::path pinned = fs::weakly_canonical(fs::path(pinnedRoot), ec);
			std::istringstream entries(TestPlatform::GetEnvironment("PATH"));
			for (str entry; std::getline(entries, entry, TestPlatform::GetSearchPathSeparator());) {
				if (entry.empty() || !fs::is_regular_file(fs::path(entry) / _editor->muxerName, ec)) {
					continue;
				}
				if (fs::weakly_canonical(fs::path(entry), ec) == pinned) {
					continue;
				}
				for (const fs::directory_entry& host : fs::directory_iterator(fs::path(entry) / "host" / "fxr", ec)) {
					const str version = host.path().filename().generic_string();
					u32 major = 0;
					std::from_chars(version.data(), version.data() + version.size(), major);
					if (major >= 10) {
						return (fs::path(entry) / _editor->muxerName).generic_string();
					}
				}
			}
			return {};
		}

		bool UseRealToolchain() {
			const str sdk = _hooks->findSDKRoot();
			if (sdk.empty()) {
				return false;
			}
			_options = _editor->getDefaultProjectOptions(_project.generic_string(), sdk);
			_options.projectName = "Space Game";
			return true;
		}
	};

} // anonymous namespace

TEST_F(CSharpProjectFilesTest, RendersEveryFile) {
	const std::vector<str> expected = {".gitignore", "CSharp.props", PATHS_FILE, PROJECT_FILE, SOLUTION_FILE, "global.json"};
	EXPECT_EQ(Write(), expected);
	for (const str& file : expected) {
		EXPECT_FALSE(Read(file).empty()) << file;
		EXPECT_EQ(Read(file).find("{{"), str::npos) << file;
	}

	const str project = Read(PROJECT_FILE);
	EXPECT_NE(project.find("<AssemblyName>Space_Game.Scripts</AssemblyName>"), str::npos);
	EXPECT_NE(project.find("<RootNamespace>Space_Game</RootNamespace>"), str::npos);
	EXPECT_NE(project.find("<Compile Include=\"Assets/**/*.cs\" />"), str::npos);
	EXPECT_LT(project.find("<BaseIntermediateOutputPath>"), project.find("Sdk.props"));
	EXPECT_LT(project.find("Shalltear.CSharp.props"), project.find("Project=\"CSharp.props\""));
	EXPECT_NE(Read(SOLUTION_FILE).find("<Project Path=\"Space_Game.Scripts.csproj\" />"), str::npos);

	const str paths = Read(PATHS_FILE);
	EXPECT_NE(paths.find("<ShalltearSDKRoot>" + Escaped(_options.engineRoot) + "</ShalltearSDKRoot>"), str::npos);
	EXPECT_NE(paths.find("<ShalltearManagedDir>" + Escaped(_options.managedDirectory) + "/</ShalltearManagedDir>"), str::npos);
	EXPECT_NE(paths.find("<ShalltearDotNetRoot>" + _options.dotnetRoot + "</ShalltearDotNetRoot>"), str::npos);
	EXPECT_NE(paths.find("<ShalltearPackagesRoot>" + _options.packagesRoot + "</ShalltearPackagesRoot>"), str::npos);
	EXPECT_NE(paths.find("<ShalltearDotNetSDKVersion>10.0.999</ShalltearDotNetSDKVersion>"), str::npos);
	EXPECT_NE(paths.find("<ShalltearDotNetRuntimeVersion>10.0.99</ShalltearDotNetRuntimeVersion>"), str::npos);
	EXPECT_NE(paths.find("<ShalltearTargetFramework>net10.0</ShalltearTargetFramework>"), str::npos);
	EXPECT_NE(paths.find("$(MSBuildProjectDirectory)/Library/CSharp/IDE/</ShalltearBuildRoot>"), str::npos);
	EXPECT_NE(paths.find("<ShalltearAllowUnsafeBlocks>false</ShalltearAllowUnsafeBlocks>"), str::npos);
	EXPECT_NE(paths.find("<ShalltearInvariantGlobalization>true</ShalltearInvariantGlobalization>"), str::npos);

	const str global = Read("global.json");
	EXPECT_TRUE(global.starts_with(CSharpProjectFiles::GeneratedMark));
	EXPECT_NE(global.find("\"version\": \"10.0.999\""), str::npos);
	EXPECT_NE(global.find("\"rollForward\": \"disable\""), str::npos);
	EXPECT_NE(global.find("\"paths\": [\"" + _options.dotnetRoot + "\", \"$host$\"]"), str::npos);

	const str ignored = Read(".gitignore");
	for (const char* line : {"/Library/", "/*.Scripts.csproj", "/*.slnx", "/global.json"}) {
		EXPECT_TRUE(HasLine(ignored, line)) << line;
	}

	_options.projectName = "3d \"quoted\" <game>";
	_options.dotnetRoot = "/opt/sdk \"x\"\\y";
	Write();
	EXPECT_NE(Read("_3d__quoted___game_.Scripts.csproj").find("<AssemblyName>_3d__quoted___game_.Scripts</AssemblyName>"), str::npos);
	EXPECT_NE(Read("global.json").find("\"paths\": [\"/opt/sdk \\\"x\\\"\\\\y\", \"$host$\"]"), str::npos);
}

TEST_F(CSharpProjectFilesTest, MissingRootsWriteNothing) {
	for (str CSharpProjectFiles::Options::* root : {&CSharpProjectFiles::Options::projectPath, &CSharpProjectFiles::Options::engineRoot,
													&CSharpProjectFiles::Options::dotnetRoot, &CSharpProjectFiles::Options::managedDirectory}) {
		CSharpProjectFiles::Options options = _options;
		options.*root = {};
		CSharpProjectFiles::Result result;
		EXPECT_FALSE(_editor->writeProjectFiles(options, result));
		EXPECT_TRUE(result.written.empty());
	}
	EXPECT_TRUE(fs::is_empty(_project));
}

TEST_F(CSharpProjectFilesTest, RegenerationIsIdempotent) {
	const std::vector<str> first = Write();
	std::vector<fs::file_time_type> times;
	for (const str& file : first) {
		times.push_back(fs::last_write_time(_project / file));
	}

	EXPECT_TRUE(Write().empty());
	for (size_t index = 0; index < first.size(); ++index) {
		EXPECT_EQ(fs::last_write_time(_project / first[index]), times[index]) << first[index];
	}

	_options.allowUnsafeCode = true;
	_options.invariantGlobalization = false;
	EXPECT_EQ(Write(), std::vector<str>{PATHS_FILE});
	const str paths = Read(PATHS_FILE);
	EXPECT_NE(paths.find("<ShalltearAllowUnsafeBlocks>true</ShalltearAllowUnsafeBlocks>"), str::npos);
	EXPECT_NE(paths.find("<ShalltearInvariantGlobalization>false</ShalltearInvariantGlobalization>"), str::npos);

	fs::remove(_project / SOLUTION_FILE);
	EXPECT_EQ(Write(), std::vector<str>{SOLUTION_FILE});
}

TEST_F(CSharpProjectFilesTest, UserPropsNeverOverwritten) {
	Write();
	const str mine = "<Project>\n\t<PropertyGroup>\n\t\t<DefineConstants>$(DefineConstants);MINE</DefineConstants>\n\t</PropertyGroup>\n</Project>\n";
	Put("CSharp.props", mine);
	_options.allowUnsafeCode = true;
	_options.pin.sdkVersion = "10.0.998";
	const std::vector<str> expected = {PATHS_FILE, "global.json"};
	EXPECT_EQ(Write(), expected);
	EXPECT_EQ(Read("CSharp.props"), mine);
}

TEST_F(CSharpProjectFilesTest, IgnoreLinesAreAddedOnce) {
	Put(".gitignore", "/Library/\n/Builds/");
	Write();
	EXPECT_EQ(Read(".gitignore"), "/Library/\n/Builds/\n/*.Scripts.csproj\n/*.slnx\n/global.json\n/.vscode/launch.json\n");

	Put(".gitignore", "/Library/\n");
	_options.allowUnsafeCode = true;
	Write();
	EXPECT_EQ(Read(".gitignore"), "/Library/\n");

	fs::remove(_project / "CSharp.props");
	Put(".gitignore", "/global.json\r\n/Builds/\r\n");
	const std::vector<str> expected = {".gitignore", "CSharp.props"};
	EXPECT_EQ(Write(), expected);
	EXPECT_EQ(Read(".gitignore"), "/global.json\r\n/Builds/\r\n/Library/\r\n/*.Scripts.csproj\r\n/*.slnx\r\n/.vscode/launch.json\r\n");
}

TEST_F(CSharpProjectFilesTest, MachinePathsOnlyInIgnoredFiles) {
	const std::vector<str> written = Write();
	const str ignoreText = Read(".gitignore");
	const str machine = _root.generic_string();
	const str escaped = Escaped(machine);
	u32 holding = 0;
	for (const str& file : written) {
		const str text = Read(file);
		const bool holds = text.find(machine) != str::npos || text.find(escaped) != str::npos ||
						   text.find(_root.filename().generic_string()) != str::npos;
		holding += holds ? 1 : 0;
		EXPECT_TRUE(!holds || IsIgnored(ignoreText, file)) << file;
	}
	EXPECT_EQ(holding, 2u);
	EXPECT_TRUE(IsIgnored(ignoreText, PATHS_FILE));
	EXPECT_TRUE(IsIgnored(ignoreText, "global.json"));
	EXPECT_TRUE(IsIgnored(ignoreText, PROJECT_FILE));
	EXPECT_TRUE(IsIgnored(ignoreText, SOLUTION_FILE));
	EXPECT_FALSE(IsIgnored(ignoreText, "CSharp.props"));
	EXPECT_FALSE(IsIgnored(ignoreText, ".gitignore"));
	EXPECT_FALSE(IsIgnored(ignoreText, "Assets/Scripts/Spinner.cs"));
}

TEST_F(CSharpProjectFilesTest, UserGlobalJSONIsLeftAlone) {
	const str mine = "{\n\t\"sdk\": { \"version\": \"9.0.100\" }\n}\n";
	Put("global.json", mine);
	const std::vector<str> written = Write();
	EXPECT_EQ(std::count(written.begin(), written.end(), "global.json"), 0);
	EXPECT_EQ(Read("global.json"), mine);

	fs::remove(_project / "global.json");
	EXPECT_EQ(Write(), std::vector<str>{"global.json"});
	const str generated = Read("global.json");
	ASSERT_TRUE(generated.starts_with(CSharpProjectFiles::GeneratedMark));

	_options.pin.sdkVersion = "10.0.998";
	EXPECT_EQ(Write(), (std::vector<str>{PATHS_FILE, "global.json"}));
	EXPECT_NE(Read("global.json").find("\"version\": \"10.0.998\""), str::npos);

	const str unmarked = Read("global.json").substr(Read("global.json").find('\n') + 1);
	Put("global.json", unmarked);
	_options.pin.sdkVersion = "10.0.997";
	EXPECT_EQ(Write(), std::vector<str>{PATHS_FILE});
	EXPECT_EQ(Read("global.json"), unmarked);
}

TEST_F(CSharpProjectFilesTest, GlobalJSONNamesPinAndRoot) {
	if (!UseRealToolchain()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	const str installed = FindOtherInstall(_options.dotnetRoot);
	if (installed.empty()) {
		GTEST_SKIP() << "no other dotnet 10+ on PATH";
	}
	Write();
	i32 exitCode = -1;
	const std::vector<str> lines = RunIn(_project, QuoteCommandArgument(installed) + " --version", exitCode);
	EXPECT_EQ(exitCode, 0) << Joined(lines);
	ASSERT_FALSE(lines.empty());
	EXPECT_EQ(lines.back(), _options.pin.sdkVersion) << Joined(lines);

	Put("global.json", str(CSharpProjectFiles::GeneratedMark) +
						   "\n{ \"sdk\": { \"version\": \"" + _options.pin.sdkVersion + "\", \"rollForward\": \"disable\" } }\n");
	const std::vector<str> without = RunIn(_project, QuoteCommandArgument(installed) + " --version", exitCode);
	EXPECT_NE(exitCode, 0) << Joined(without);
}

TEST_F(CSharpProjectFilesTest, BuildWithoutARootWritesUnderIDE) {
	if (!UseRealToolchain()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Put("Assets/Scripts/Spinner.cs", "using Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Spinner : Behavior {\n\tpublic float speed = 90f;\n"
									 "\n\tprotected override void OnUpdate() {\n\t\tspeed += Time.DeltaTime;\n\t}\n}\n");
	Write();
	fs::create_directories(_root / "feed");
	ChildEnvironment::Set("NUGET_PACKAGES", (_root / "global-packages").generic_string(), &_environmentOwner);
	ChildEnvironment::Set("DOTNET_CLI_TELEMETRY_OPTOUT", "1", &_environmentOwner);
	ChildEnvironment::Set("DOTNET_NOLOGO", "1", &_environmentOwner);

	i32 exitCode = -1;
	const str muxer = (fs::path(_options.dotnetRoot) / _editor->muxerName).generic_string();
	const std::vector<str> lines = RunIn(_project,
										 QuoteCommandArgument(muxer) + " build " + SOLUTION_FILE +
											 " --disable-build-servers -nologo -v q -p:RestoreSources=" +
											 QuoteCommandArgument((_root / "feed").generic_string()),
										 exitCode);
	ASSERT_EQ(exitCode, 0) << Joined(lines);
	EXPECT_TRUE(fs::is_regular_file(_project / "Library/CSharp/IDE/bin/Space_Game.Scripts.dll")) << Joined(lines);
	EXPECT_NE(Read("Library/CSharp/IDE/bin/Space_Game.Scripts.dll").find("TypeTable"), str::npos);
	EXPECT_TRUE(fs::is_regular_file(_project / "Library/CSharp/IDE/bin/Space_Game.Scripts.pdb"));
	EXPECT_TRUE(fs::is_directory(_project / "Library/CSharp/IDE/obj"));
	EXPECT_FALSE(fs::exists(_project / "Library/CSharp/IDE/bin/Shalltear.Runtime.dll"));
	EXPECT_FALSE(fs::exists(_project / "Library/CSharp/obj"));
	EXPECT_FALSE(fs::exists(_project / "Library/CSharp/bin"));
	EXPECT_FALSE(fs::exists(_project / "obj"));
	EXPECT_FALSE(fs::exists(_project / "bin"));
	EXPECT_EQ(Joined(lines).find("warning"), str::npos) << Joined(lines);
}

TEST_F(CSharpProjectFilesTest, PackageAssembliesAreNamedInPaths) {
	Write();
	EXPECT_NE(Read(PATHS_FILE).find("<ShalltearPackageAssemblies></ShalltearPackageAssemblies>"), str::npos);
	_options.packageAssemblies.push_back("Shalltear.Audio");
	_options.packageAssemblies.push_back("Shalltear.Physics3D");
	EXPECT_EQ(Write(), (std::vector<str>{PATHS_FILE}));
	EXPECT_NE(Read(PATHS_FILE).find("<ShalltearPackageAssemblies>Shalltear.Audio;Shalltear.Physics3D</ShalltearPackageAssemblies>"), str::npos);
}

TEST_F(CSharpProjectFilesTest, BuildSeesOnlyEnabledPackages) {
	if (!UseRealToolchain()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(_options.packageAssemblies.size(), 1u);
	EXPECT_EQ(_options.packageAssemblies[0], "Shalltear.Physics3D");
	Put("Assets/Scripts/Bumper.cs", "using Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Bumper : Behavior, IPhysicsListener {\n"
									"\tpublic Rigidbody? body;\n\n\tpublic void OnCollisionEnter(in Collision collision) {\n"
									"\t\tbody = collision.Rigidbody;\n\t}\n}\n");
	Write();
	fs::create_directories(_root / "feed");
	ChildEnvironment::Set("NUGET_PACKAGES", (_root / "global-packages").generic_string(), &_environmentOwner);
	ChildEnvironment::Set("DOTNET_CLI_TELEMETRY_OPTOUT", "1", &_environmentOwner);
	ChildEnvironment::Set("DOTNET_NOLOGO", "1", &_environmentOwner);
	const str muxer = (fs::path(_options.dotnetRoot) / _editor->muxerName).generic_string();
	const str build = QuoteCommandArgument(muxer) + " build " + SOLUTION_FILE + " --disable-build-servers -nologo -v q -p:RestoreSources=" +
					  QuoteCommandArgument((_root / "feed").generic_string());

	i32 exitCode = -1;
	std::vector<str> lines = RunIn(_project, build, exitCode);
	ASSERT_EQ(exitCode, 0) << Joined(lines);
	EXPECT_NE(Read("Library/CSharp/IDE/bin/Space_Game.Scripts.dll").find("Physics3DBindings"), str::npos);
	EXPECT_FALSE(fs::exists(_project / "Library/CSharp/IDE/bin/Shalltear.Physics3D.dll"));

	_options.packageAssemblies.clear();
	Write();
	lines = RunIn(_project, build, exitCode);
	EXPECT_NE(exitCode, 0);
	EXPECT_NE(Joined(lines).find("error CS0246"), str::npos) << Joined(lines);
	EXPECT_NE(Joined(lines).find("'Rigidbody'"), str::npos) << Joined(lines);
}

TEST_F(CSharpProjectFilesTest, ProjectWithoutModuleGetsAttachConfiguration) {
	_options.editorExecutable = "C:/Engine/bin/Shalltear.exe";
	const std::vector<str> written = Write();
	EXPECT_EQ(std::count(written.begin(), written.end(), ".vscode/launch.json"), 1);
	const str launch = Read(".vscode/launch.json");
	ASSERT_TRUE(launch.starts_with(CSharpProjectFiles::GeneratedMark)) << launch;
	const str body = launch.substr(launch.find('\n') + 1);
	LinearAllocator arena(body.size() + 64, MemTag::Editor);
	const JSONValue root = JSONParse(body, arena);
	ASSERT_TRUE(root.IsObject()) << launch;
	EXPECT_EQ(root["version"].AsString(), "0.2.0");
	const JSONValue& configurations = root["configurations"];
	ASSERT_EQ(configurations.Size(), 1u);
	const JSONValue& attach = configurations[size_t{0}];
	EXPECT_EQ(attach["name"].AsString(), "Attach C# (editor)");
	EXPECT_EQ(attach["type"].AsString(), "coreclr");
	EXPECT_EQ(attach["request"].AsString(), "attach");
	EXPECT_EQ(attach["processName"].AsString(), "Shalltear.exe");
	EXPECT_NE(launch.find("\t\t{\n\t\t\t\"name\": \"Attach C# (editor)\",\n"), str::npos) << launch;
	EXPECT_TRUE(HasLine(Read(".gitignore"), "/.vscode/launch.json"));

	EXPECT_TRUE(Write().empty());
	_options.editorExecutable = "/opt/shalltear/bin/Shalltear";
	EXPECT_EQ(Write(), std::vector<str>{".vscode/launch.json"});
	EXPECT_NE(Read(".vscode/launch.json").find("\"processName\": \"Shalltear\"\n"), str::npos);
}

TEST_F(CSharpProjectFilesTest, UserLaunchFileIsLeftAlone) {
	const str mine = "{\n\t\"version\": \"0.2.0\",\n\t\"configurations\": []\n}\n";
	Put(".vscode/launch.json", mine);
	_options.editorExecutable = "C:/Engine/bin/Shalltear.exe";
	const std::vector<str> written = Write();
	EXPECT_EQ(std::count(written.begin(), written.end(), ".vscode/launch.json"), 0);
	EXPECT_EQ(Read(".vscode/launch.json"), mine);
}

TEST_F(CSharpProjectFilesTest, ModuleProjectLeavesLaunchFileToEditor) {
	Put("CMakeLists.txt", "cmake_minimum_required(VERSION 3.25)\n");
	fs::create_directories(_project / "Source");
	ASSERT_TRUE(ScriptProjectOps::HasScriptProject(_options.projectPath));
	_options.editorExecutable = "C:/Engine/bin/Shalltear.exe";
	const std::vector<str> written = Write();
	EXPECT_FALSE(written.empty());
	EXPECT_FALSE(fs::exists(_project / ".vscode" / "launch.json"));

	static char owner = 0;
	_editor->registerAttachConfiguration(&owner);
	ScriptProjectOps::ScaffoldOptions scaffold;
	scaffold.moduleName = "SpaceGame";
	scaffold.sdkRoot = "C:/Sdk";
	scaffold.editorExecutable = "C:/Sdk/Bin/Shalltear" SLT_TEST_EXE_SUFFIX;
	ScriptProjectOps::ScaffoldResult regenerated;
	const bool wrote = ScriptProjectOps::RegenerateProjectFiles(_options.projectPath, scaffold, regenerated);
	const str launch = Read(".vscode/launch.json");
	EXPECT_EQ(ScriptProjectOps::UnregisterDebugConfigurationsOwnedBy(&owner), 1u);
	ASSERT_TRUE(wrote);

	LinearAllocator arena(launch.size() + 64, MemTag::Editor);
	const JSONValue root = JSONParse(launch, arena);
	ASSERT_TRUE(root.IsObject()) << launch;
	const JSONValue& configurations = root["configurations"];
	ASSERT_EQ(configurations.Size(), 2u) << launch;
	EXPECT_EQ(configurations[size_t{0}]["request"].AsString(), "launch");
	EXPECT_EQ(configurations[size_t{1}]["name"].AsString(), "Attach C# (editor)");
	EXPECT_EQ(configurations[size_t{1}]["type"].AsString(), "coreclr");
	const str process = fs::path(ScriptProjectOps::GetEditorExecutablePath()).filename().generic_string();
	EXPECT_FALSE(process.empty());
	EXPECT_EQ(configurations[size_t{1}]["processName"].AsString(), process);
}

#endif
