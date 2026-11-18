#include "TestsPCH.h"

#if defined(SHALLTEAR_SHARED)

#include "CSharpRuntimeFixture.h"
#include "EditorTestHooks.h"
#include "ScopedSettingsStore.h"
#include "TestPlatform.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/BuildInfo.h"
#include "Shalltear/Core/Config.h"

#include "Shalltear/Editor/BuildPipeline.h"
#include "Shalltear/Editor/BuildProfile.h"
#include "Shalltear/Editor/BuildTarget.h"
#include "Shalltear/Editor/EditorExtensions.h"
#include "Shalltear/Editor/ScriptProjectOps.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>

using namespace Shalltear;
using namespace Shalltear::CSharpTests;

namespace {

	namespace fs = std::filesystem;

	constexpr const char* LIBRARY = "GameScripts" SLT_TEST_DLL_SUFFIX;

	constexpr const char* PROBE_MARKER = "SHALLTEAR_CSHARP_PROBE";
	constexpr const char* SOURCE_MARKER = "SOURCE_LINE_MARKER";
	constexpr const char* TOOLTIP_MARKER = "TOOLTIP_MARKER";

	constexpr const char* TALKER =
		"using System;\nusing System.Collections.Generic;\nusing System.Globalization;\nusing System.Runtime.CompilerServices;\nusing Shalltear;\n\n"
		"namespace Scripted;\n\n"
		"public enum Gear {\n\tLow = 2,\n\tHigh = 5,\n}\n\n[Serializable]\npublic struct Tuning {\n\tpublic float grip;\n\tpublic int laps;\n}\n\n"
		"// SOURCE_LINE_MARKER this comment must never ship\npublic sealed class Talker : Behavior {\n"
		"\t[Tooltip(\"TOOLTIP_MARKER shown in the Inspector only\")]\n\tpublic float speed = 1.5f;\n\tpublic Gear gear = Gear.Low;\n"
		"\tpublic string label = \"unset\";\n\tpublic Tuning tuning;\n\tpublic List<int> steps = new();\n\tpublic float turnRateDegrees = 30f;\n"
		"\tint _frames;\n\n\tstatic string Where([CallerFilePath] string file = \"\") {\n\t\treturn file;\n\t}\n\n"
		"\tprotected override void OnEnable() {\n"
		"\t\tLog.Info(\"PlayerProbe\", \"SHALLTEAR_CSHARP_PROBE enabled speed=\" + speed.ToString(CultureInfo.InvariantCulture) + \" gear=\" + (int)gear +\n"
		"\t\t\t\t\t\t\t\t\" label=\" + label + \" grip=\" + tuning.grip.ToString(CultureInfo.InvariantCulture) + \" laps=\" + tuning.laps +\n"
		"\t\t\t\t\t\t\t\t\" steps=\" + steps.Count + \" owner=\" + Owner.Name);\n\t}\n\n"
		"\tprotected override void OnUpdate() {\n\t\tif (++_frames == 2) {\n"
		"\t\t\tLog.Info(\"PlayerProbe\", \"SHALLTEAR_CSHARP_PROBE frame \" + _frames + \" where=\" + Where() + \" turn=\" + turnRateDegrees);\n"
		"\t\t}\n\t}\n}\n";

	constexpr const char* PLAYER_ONLY = "namespace Scripted;\n\npublic static class PlayerOnly {\n#if SHALLTEAR_PLAYER\n\tpublic static int Broken() {\n"
										"\t\treturn missing;\n\t}\n#endif\n}\n";

	constexpr const char* NOTICE = "namespace Scripted;\n\npublic static class Notice {\n#if SHALLTEAR_PLAYER\n#warning player builds only\n#endif\n}\n";

	constexpr const char* POINTER = "namespace Scripted;\n\npublic static class Pointer {\n\tpublic static unsafe int Read(int* at) {\n\t\treturn *at;\n\t}\n}\n";

	str Hex(std::string_view name) {
		return std::format("{:016x}", StringHash(name).GetValue());
	}

	str SceneObjectRow(u32 id, std::string_view name, const str& components) {
		return std::format("\t\t{{\n\t\t\t\"id\": {},\n\t\t\t\"parent\": 0,\n\t\t\t\"type\": \"{}\",\n\t\t\t\"fields\": {{\"_type\": \"SceneObject\", \"name\": \"{}\", "
						   "\"_activeSelf\": true, \"_localPosition\": [0, 0, 0], \"_localRotation\": [0, 0, 0, 1], \"_localScale\": [1, 1, 1]}},\n"
						   "\t\t\t\"components\": [{}]\n\t\t}}",
						   id, Hex("SceneObject"), name, components);
	}

	str SceneText() {
		const str camera = std::format("{{\"id\": 2, \"type\": \"{}\", \"fields\": {{\"_type\": \"Camera\", \"_enabledSelf\": true}}}}", Hex("Camera"));
		const str talker = std::format("{{\"id\": 4, \"type\": \"{}\", \"fields\": {{\"_type\": \"Scripted.Talker\", \"_enabledSelf\": true, \"speed\": 4.25, "
									   "\"gear\": \"High\", \"label\": \"from scene\", \"tuning\": {{\"grip\": 0.5, \"laps\": 3}}, \"steps\": [1, 2, 3]}}}}",
									   Hex("Scripted.Talker"));
		return std::format("{{\n\t\"_type\": \"Scene\",\n\t\"formatVersion\": 1,\n\t\"engineHash\": \"{:016x}\",\n\t\"name\": \"Main\",\n\t\"nextLocalId\": 5,\n"
						   "\t\"objects\": [\n{},\n{}\n\t]\n}}\n",
						   BuildInfo::GetEngineHash(), SceneObjectRow(1, "Camera", camera), SceneObjectRow(3, "Guide", talker));
	}

	str ReadBytes(const fs::path& file) {
		std::ifstream stream(file, std::ios::binary);
		return str(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
	}

	str Wide(std::string_view text) {
		str wide;
		for (const char c : text) {
			wide.push_back(c);
			wide.push_back('\0');
		}
		return wide;
	}

	bool Holds(const str& bytes, std::string_view text) {
		return bytes.find(text) != str::npos || bytes.find(Wide(text)) != str::npos;
	}

	str ReadCacheValue(const fs::path& tree, std::string_view key) {
		std::ifstream stream(tree / "CMakeCache.txt");
		for (str line; std::getline(stream, line);) {
			if (line.starts_with(key)) {
				const size_t equals = line.find('=');
				return equals == str::npos ? str() : line.substr(equals + 1);
			}
		}
		return {};
	}

	str FindStaticSibling(std::string_view config) {
		const fs::path tree = fs::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path().parent_path();
		std::error_code ec;
		for (const fs::directory_entry& entry : fs::directory_iterator(tree.parent_path(), ec)) {
			if (!entry.is_directory(ec) || entry.path() == tree) {
				continue;
			}
			const str package = ReadBytes(entry.path() / "ShalltearConfig.cmake");
			if (package.find("set(SHALLTEAR_LINK_MODE \"STATIC\")") == str::npos || ReadCacheValue(entry.path(), "CMAKE_BUILD_TYPE:") != config) {
				continue;
			}
			if (fs::is_regular_file(entry.path() / "lib" / "CSharp.a", ec)) {
				return entry.path().generic_string();
			}
		}
		return {};
	}

	bool HeldOpen(const fs::path& image) {
		return !std::fstream(image, std::ios::in | std::ios::out | std::ios::binary).is_open();
	}

	Vector<fs::path> FindBuildServerImages() {
		Vector<fs::path> images{GetMemoryResource(MemTag::Debug)};
		const fs::path engine = fs::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path().parent_path().parent_path();
		std::error_code ec;
		for (const fs::directory_entry& root : fs::directory_iterator(engine / "External" / "Prebuilt" / "dotnet", ec)) {
			const fs::path muxer = root.path() / ("dotnet" SLT_TEST_EXE_SUFFIX);
			if (!fs::is_regular_file(muxer, ec)) {
				continue;
			}
			images.push_back(muxer);
			for (const fs::directory_entry& sdk : fs::directory_iterator(root.path() / "sdk", ec)) {
				const fs::path server = sdk.path() / "Roslyn" / "bincore" / ("VBCSCompiler" SLT_TEST_EXE_SUFFIX);
				if (fs::is_regular_file(server, ec)) {
					images.push_back(server);
				}
			}
		}
		return images;
	}

	bool AnyHeldOpen(const Vector<fs::path>& images) {
		return std::any_of(images.begin(), images.end(), [](const fs::path& image) { return HeldOpen(image); });
	}

	struct Suite {
		fs::path root;
		fs::path project;
		str config;
		str skip;
		str editor;
		Vector<fs::path> serverImages{GetMemoryResource(MemTag::Debug)};
		bool serverRanBefore = false;
		bool serverRanAfter = false;
		std::unique_ptr<ScopedUserDataDirectory> userData;
		int buildExit = -1;
		str buildLog;
		str playerLog;
		int playerExit = -1;
	};

	Suite* _suite = nullptr;

	class CSharpPlayerBuildTest : public ::testing::Test {
	protected:
		static void SetUpTestSuite() {
			_suite = new Suite();
			Suite& suite = *_suite;
			if (!TestPlatform::BuildsCSharpPlayers()) {
				suite.skip = "C# players build for Windows only";
				return;
			}
			suite.config = str(ScriptProjectOps::GetActiveConfigName());
			const str staticTree = FindStaticSibling(suite.config);
			if (staticTree.empty()) {
				suite.skip = "no sibling static " + suite.config + " tree with the C# package";
				return;
			}
			suite.editor = Path(ScriptProjectOps::GetEditorExecutablePath()).GetDirectory() + "/Shalltear" SLT_TEST_EXE_SUFFIX;
			std::error_code ec;
			if (!fs::is_regular_file(fs::path(suite.editor), ec)) {
				suite.skip = "no editor exe (" + suite.editor + ")";
				return;
			}

			suite.root = fs::temp_directory_path() / ("slt_cs_player_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			suite.project = suite.root / "Scripted Game";
			fs::create_directories(suite.project / "Assets" / "Scripts");
			fs::create_directories(suite.project / "Assets" / "Scenes");
			suite.userData = std::make_unique<ScopedUserDataDirectory>(suite.root / "UserData");
			fs::create_directories(suite.root / "UserData" / "Shalltear");
			std::ofstream(suite.root / "UserData" / "Shalltear" / "UserSettings.ini", std::ios::binary) << "[Editor/Build]\nstaticSDKRoot = " << staticTree << "\n";

			Config projectFile;
			projectFile.SetString("Project", "name", "ScriptedGame");
			ASSERT_TRUE(projectFile.Save(Path(suite.project.generic_string() + "/Project.ini")));
			Config settings;
			settings.SetString("Packages", "enabled", "dev.shalltear.csharp");
			settings.SetBool("C#", "allowUnsafeCode", true);
			ASSERT_TRUE(settings.Save(Path(suite.project.generic_string() + "/ProjectSettings.ini")));
			std::ofstream(suite.project / "Assets" / "Scripts" / "Talker.cs", std::ios::binary) << TALKER;
			std::ofstream(suite.project / "Assets" / "Scripts" / "Notice.cs", std::ios::binary) << NOTICE;
			std::ofstream(suite.project / "Assets" / "Scripts" / "Pointer.cs", std::ios::binary) << POINTER;
			std::ofstream(suite.project / "Assets" / "Scenes" / "Main.scene", std::ios::binary) << SceneText();
			for (const char* target : {"Windows", "WindowsServer"}) {
				BuildProfile profile;
				profile.name = target;
				profile.target = target;
				profile.SetList("scenes", Vector<str>{"Assets/Scenes/Main.scene"});
				profile.SetString("configuration", suite.config);
				ASSERT_TRUE(BuildProfileOps::SaveProfile(BuildProfileOps::GetProfilePath(suite.project.generic_string(), target), profile));
			}

			FileSystem::Mount("project", Path(suite.project.generic_string()));
			AssetDatabase::Initialize();
			AssetDatabase::ScanProjectDirectory();
			const bool indexed = AssetDatabase::LookupGuid(Path("project://Assets/Scenes/Main.scene")).IsValid();
			AssetDatabase::Terminate();
			FileSystem::Unmount("project");
			ASSERT_TRUE(indexed);

			suite.serverImages = FindBuildServerImages();
			suite.serverRanBefore = AnyHeldOpen(suite.serverImages);
			suite.buildLog = Build("Windows", suite.buildExit);
			suite.serverRanAfter = AnyHeldOpen(suite.serverImages);
			if (suite.buildExit == 0) {
				suite.playerLog = TestPlatform::RunCommand("\"" + Player("Windows").generic_string() + "\" --frames 8", suite.playerExit);
			}
		}

		static void TearDownTestSuite() {
			if (_suite) {
				_suite->userData.reset();
				std::error_code ec;
				fs::remove_all(_suite->root, ec);
				delete _suite;
				_suite = nullptr;
			}
		}

		void SetUp() override {
			ASSERT_NE(_suite, nullptr);
			if (!_suite->skip.empty()) {
				GTEST_SKIP() << _suite->skip;
			}
			ASSERT_EQ(_suite->buildExit, 0) << _suite->buildLog;
		}

		static str Build(const char* profile, int& outExitCode) {
			return TestPlatform::RunCommand("\"" + _suite->editor + "\" --project \"" + _suite->project.generic_string() + "\" --build " + profile, outExitCode);
		}

		static fs::path Staging(const char* profile) {
			return _suite->project / "Builds" / profile;
		}

		static fs::path Player(const char* profile) {
			return Staging(profile) / ("ScriptedGame" SLT_TEST_PLAYER_SUFFIX);
		}

		static fs::path Library(const char* profile) {
			return Staging(profile) / LIBRARY;
		}
	};

	class CSharpBuildStepsTest : public CSharpRuntimeFixture {
	protected:
		void TearDown() override {
			BuildStepRegistry::Shutdown();
			BuildTargetRegistry::Shutdown();
			CSharpRuntimeFixture::TearDown();
		}

		static const BuildStepDesc* Find(const Vector<BuildStepDesc>& steps, std::string_view name) {
			const auto found = std::find_if(steps.begin(), steps.end(), [name](const BuildStepDesc& step) { return step.name == name; });
			return found == steps.end() ? nullptr : &*found;
		}

		static size_t IndexOf(const Vector<BuildStepDesc>& steps, std::string_view name) {
			const BuildStepDesc* step = Find(steps, name);
			return step ? static_cast<size_t>(step - steps.data()) : steps.size();
		}
	};

} // anonymous namespace

TEST_F(CSharpPlayerBuildTest, CSharpPlayerRunsComponent) {
	EXPECT_NE(_suite->buildLog.find("C# scripts compiled in"), str::npos) << _suite->buildLog;
	EXPECT_EQ(_suite->buildLog.find("isn't loaded"), str::npos) << _suite->buildLog;
	ASSERT_TRUE(fs::is_regular_file(Library("Windows"))) << _suite->buildLog;
	ASSERT_EQ(_suite->playerExit, 0) << _suite->playerLog;
	EXPECT_NE(_suite->playerLog.find(str(PROBE_MARKER) + " enabled speed=4.25 gear=5 label=from scene grip=0.5 laps=3 steps=3 owner=Guide"), str::npos)
		<< _suite->playerLog;
	const size_t frame = _suite->playerLog.find(str(PROBE_MARKER) + " frame 2 where=.");
	ASSERT_NE(frame, str::npos) << _suite->playerLog;
	const str line = _suite->playerLog.substr(frame, _suite->playerLog.find('\n', frame) - frame);
	EXPECT_NE(line.find("Talker.cs turn=30"), str::npos) << line;
	EXPECT_EQ(line.find("Scripted Game"), str::npos) << line;
	EXPECT_EQ(_suite->playerLog.find("[ERROR]"), str::npos) << _suite->playerLog;
}

TEST_F(CSharpPlayerBuildTest, CSharpTypesRegisteredBeforeBootScene) {
	const str& log = _suite->playerLog;
	const size_t started = log.find(str(LIBRARY) + " started in ");
	const size_t scenes = log.find("1 scene(s)");
	const size_t enabled = log.find(str(PROBE_MARKER) + " enabled");
	ASSERT_NE(started, str::npos) << log;
	ASSERT_NE(scenes, str::npos) << log;
	ASSERT_NE(enabled, str::npos) << log;
	EXPECT_NE(log.find(" ms (1 script type(s))", started), str::npos) << log;
	EXPECT_LT(started, scenes);
	EXPECT_LT(scenes, enabled);
	EXPECT_EQ(log.find("unloaded type"), str::npos) << log;
}

TEST_F(CSharpPlayerBuildTest, CSharpPlayerShipsNoIL) {
	const str& log = _suite->buildLog;
	const size_t compiled = log.find("C# scripts compiled in");
	const size_t gated = log.find(" staged file(s) have no managed code");
	ASSERT_NE(compiled, str::npos) << log;
	ASSERT_NE(gated, str::npos) << log;
	EXPECT_LT(compiled, gated);
	u32 files = 0;
	std::error_code ec;
	for (const fs::directory_entry& entry : fs::recursive_directory_iterator(Staging("Windows"), ec)) {
		files += entry.is_regular_file(ec) ? 1u : 0u;
	}
	EXPECT_NE(log.find(std::format(": {} staged file(s) have no managed code", files)), str::npos) << log;
}

TEST_F(CSharpPlayerBuildTest, BatchBuildLeavesNoBuildServer) {
	if (_suite->serverImages.empty() || _suite->serverRanBefore) {
		GTEST_SKIP() << "pinned .NET SDK is elsewhere, or one of its build servers ran before the build";
	}
	EXPECT_FALSE(_suite->serverRanAfter);
	EXPECT_FALSE(AnyHeldOpen(_suite->serverImages));
}

TEST_F(CSharpPlayerBuildTest, PlayerOnlyWarningCountedInBuildLog) {
	const str& log = _suite->buildLog;
	const size_t warned = log.find("Notice.cs:5:");
	ASSERT_NE(warned, str::npos) << log;
	const size_t row = log.rfind('\n', warned);
	EXPECT_NE(log.substr(row == str::npos ? 0 : row, warned - (row == str::npos ? 0 : row)).find("[WARN]"), str::npos) << log;
	EXPECT_NE(log.find("CS1030: ", warned), str::npos) << log;
	EXPECT_NE(log.find("C# scripts compiled with 1 warning(s) (in log above)"), str::npos) << log;
	EXPECT_EQ(log.find("[ERROR]"), str::npos) << log;
}

TEST_F(CSharpPlayerBuildTest, UnsafeScriptBuildsWhenProjectAllows) {
	EXPECT_EQ(_suite->buildLog.find("CS0227"), str::npos) << _suite->buildLog;
	const str paths = ReadBytes(_suite->project / "Library" / "CSharp" / "Generated" / "Shalltear.Paths.props");
	EXPECT_NE(paths.find("<ShalltearAllowUnsafeBlocks>true</ShalltearAllowUnsafeBlocks>"), str::npos) << paths;
}

TEST_F(CSharpPlayerBuildTest, LibraryWithoutEntryNamedInPlayerLog) {
	const char* windows = std::getenv("SystemRoot");
	ASSERT_NE(windows, nullptr);
	const fs::path other = fs::path(windows) / "System32" / "version.dll";
	ASSERT_TRUE(fs::is_regular_file(other));
	const fs::path library = Library("Windows");
	const fs::path aside = fs::path(library).replace_extension(".aside");
	std::error_code ec;
	fs::rename(library, aside, ec);
	ASSERT_FALSE(ec) << ec.message();
	fs::copy_file(other, library, ec);
	int exitCode = -1;
	const str log = TestPlatform::RunCommand("\"" + Player("Windows").generic_string() + "\" --frames 2", exitCode);
	fs::remove(library, ec);
	fs::rename(aside, library, ec);
	ASSERT_FALSE(ec) << ec.message();
	const size_t said = log.find(str(LIBRARY) + " doesn't export ShalltearScriptsInitialize");
	ASSERT_NE(said, str::npos) << log;
	const size_t row = log.rfind('\n', said);
	EXPECT_NE(log.substr(row == str::npos ? 0 : row, said - (row == str::npos ? 0 : row)).find("[ERROR]"), str::npos) << log;
	EXPECT_EQ(log.find(str(LIBRARY) + " started in "), str::npos) << log;
}

TEST_F(CSharpPlayerBuildTest, CSharpPlayerCarriesNoSourceOrPath) {
	const str projectPath = _suite->project.generic_string();
	str backslashed = projectPath;
	std::replace(backslashed.begin(), backslashed.end(), '/', '\\');
	const str engine = fs::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path().parent_path().parent_path().generic_string();
	str engineBackslashed = engine;
	std::replace(engineBackslashed.begin(), engineBackslashed.end(), '/', '\\');

	u32 files = 0;
	bool literal = false;
	std::error_code ec;
	for (const fs::directory_entry& entry : fs::recursive_directory_iterator(Staging("Windows"), ec)) {
		if (!entry.is_regular_file(ec)) {
			continue;
		}
		++files;
		const str name = entry.path().filename().generic_string();
		EXPECT_NE(entry.path().extension().generic_string(), ".cs") << name;
		EXPECT_NE(name, "GameScripts.pdb");
		EXPECT_EQ(name.find("Shalltear.Runtime"), str::npos) << name;
		EXPECT_EQ(name.find(".Scripts."), str::npos) << name;
		EXPECT_EQ(name.find(".runtimeconfig."), str::npos) << name;
		EXPECT_EQ(name.find(".deps."), str::npos) << name;
		const str bytes = ReadBytes(entry.path());
		for (const std::string_view banned : {std::string_view(SOURCE_MARKER), std::string_view(TOOLTIP_MARKER)}) {
			EXPECT_FALSE(Holds(bytes, banned)) << name << " holds " << banned;
		}
		if (name != LIBRARY) {
			continue;
		}
		for (const std::string_view banned : {std::string_view(projectPath), std::string_view(backslashed), std::string_view(engine),
											  std::string_view(engineBackslashed), std::string_view("Scripted Game")}) {
			EXPECT_FALSE(Holds(bytes, banned)) << name << " holds " << banned;
		}
		literal = Holds(bytes, str(PROBE_MARKER) + " enabled speed=");
		EXPECT_EQ(Holds(bytes, "turnRateDegrees"), _suite->config != "Release");
	}
	EXPECT_GT(files, 3u);
	EXPECT_TRUE(literal);
	EXPECT_TRUE(fs::is_regular_file(_suite->project / "Library" / "Player" / "Windows" / "symbols" / "GameScripts.pdb"));
}

TEST_F(CSharpPlayerBuildTest, CSharpRebuildRepeatsTheLibrary) {
	const str first = ReadBytes(Library("Windows"));
	ASSERT_FALSE(first.empty());
	std::error_code ec;
	fs::remove_all(_suite->project / "Library" / "CSharp" / "Player" / "Windows", ec);
	fs::remove(Library("Windows"), ec);
	int exitCode = -1;
	const str log = Build("Windows", exitCode);
	ASSERT_EQ(exitCode, 0) << log;
	const str second = ReadBytes(Library("Windows"));
	ASSERT_EQ(second.size(), first.size());
	EXPECT_TRUE(second == first);
}

TEST_F(CSharpPlayerBuildTest, CSharpServerRunsComponent) {
	int exitCode = -1;
	const str log = Build("WindowsServer", exitCode);
	ASSERT_EQ(exitCode, 0) << log;
	ASSERT_TRUE(fs::is_regular_file(Library("WindowsServer"))) << log;
	const fs::path scripts = fs::path("Library") / "CSharp" / "Player";
	const str built = ReadBytes(_suite->project / scripts / "WindowsServer" / "scripts" / "bin" / "ScriptedGame.Scripts.dll");
	ASSERT_FALSE(built.empty());
	EXPECT_TRUE(built == ReadBytes(_suite->project / scripts / "Windows" / "scripts" / "bin" / "ScriptedGame.Scripts.dll"));
	const fs::path server = Staging("WindowsServer") / ("ScriptedGameServer" SLT_TEST_PLAYER_SUFFIX);
	const str serverLog = TestPlatform::RunCommand("\"" + server.generic_string() + "\" --frames 4", exitCode);
	ASSERT_EQ(exitCode, 0) << serverLog;
	EXPECT_NE(serverLog.find(str(PROBE_MARKER) + " enabled speed=4.25 gear=5"), str::npos) << serverLog;
	EXPECT_NE(serverLog.find(str(PROBE_MARKER) + " frame 2"), str::npos) << serverLog;
}

TEST_F(CSharpPlayerBuildTest, PlayerOnlyErrorFailsBuildWithLine) {
	const fs::path script = _suite->project / "Assets" / "Scripts" / "PlayerOnly.cs";
	std::ofstream(script, std::ios::binary) << PLAYER_ONLY;
	int exitCode = -1;
	const str log = Build("Windows", exitCode);
	std::error_code ec;
	fs::remove(script, ec);
	EXPECT_NE(exitCode, 0) << log;
	EXPECT_NE(log.find("PlayerOnly.cs:6:"), str::npos) << log;
	EXPECT_NE(log.find("CS0103: "), str::npos) << log;
	EXPECT_NE(log.find("C# script compile failed (1 error(s) in log above)"), str::npos) << log;
	EXPECT_NE(log.find("the 'csharp-compile' step failed"), str::npos) << log;
	EXPECT_EQ(log.find("C# scripts don't compile"), str::npos) << log;
}

TEST_F(CSharpBuildStepsTest, StepsJoinAtLoadLeaveWithExtensions) {
	Vector<BuildStepDesc> steps{GetMemoryResource(MemTag::Debug)};
	BuildStepRegistry::GetSteps(steps);
	const BuildStepDesc* validate = Find(steps, "csharp-validate");
	const BuildStepDesc* load = Find(steps, "csharp-load");
	const BuildStepDesc* compile = Find(steps, "csharp-compile");
	const BuildStepDesc* stage = Find(steps, "csharp-stage");
	const BuildStepDesc* gate = Find(steps, "csharp-no-il");
	ASSERT_TRUE(validate && load && compile && stage && gate);
	EXPECT_EQ(validate->phase, BuildPhase::Validate);
	EXPECT_EQ(load->phase, BuildPhase::PreBuild);
	EXPECT_EQ(compile->phase, BuildPhase::Compile);
	EXPECT_EQ(stage->phase, BuildPhase::Stage);
	EXPECT_EQ(gate->phase, BuildPhase::PostBuild);
	EXPECT_LT(IndexOf(steps, "validate"), IndexOf(steps, "csharp-validate"));
	EXPECT_LT(IndexOf(steps, "csharp-load"), IndexOf(steps, "cook-content"));
	EXPECT_LT(IndexOf(steps, "csharp-compile"), IndexOf(steps, "csharp-stage"));
	EXPECT_LT(IndexOf(steps, "post-build-command"), IndexOf(steps, "csharp-no-il"));
	EXPECT_EQ(IndexOf(steps, "csharp-no-il"), steps.size() - 1);

	const void* owner = validate->owner;
	ASSERT_NE(owner, nullptr);
	EXPECT_EQ(owner, ModuleSystem::GetNativeHandle(_package));
	EXPECT_EQ(gate->owner, owner);
	EXPECT_GE(EditorExtensions::Unload(owner), 5u);
	BuildStepRegistry::GetSteps(steps);
	for (const char* name : {"csharp-validate", "csharp-load", "csharp-compile", "csharp-stage", "csharp-no-il"}) {
		EXPECT_EQ(Find(steps, name), nullptr) << name;
	}
	EXPECT_NE(Find(steps, "cook-content"), nullptr);
}

TEST_F(CSharpBuildStepsTest, TargetWithoutCSharpRefusedWithReason) {
	Vector<BuildStepDesc> steps{GetMemoryResource(MemTag::Debug)};
	BuildStepRegistry::GetSteps(steps);
	const BuildStepDesc* validate = Find(steps, "csharp-validate");
	ASSERT_NE(validate, nullptr);
	ResolvedProfile profile;
	profile.name = "Elsewhere";
	BuildContext context;
	context.profile = &profile;
	context.target = BuildTargetRegistry::Find("Linux");
	ASSERT_NE(context.target, nullptr);
	EXPECT_EQ(validate->fn(context), BuildStepStatus::Failed);
	ASSERT_EQ(context.GetIssues().size(), 1u);
	EXPECT_EQ(context.GetIssues()[0].severity, BuildIssueSeverity::Error);
	EXPECT_NE(context.GetIssues()[0].message.find("C# scripts don't build for Linux yet. Build this profile for Windows"), str::npos)
		<< context.GetIssues()[0].message;

	BuildContext server;
	server.profile = &profile;
	server.target = BuildTargetRegistry::Find("LinuxServer");
	ASSERT_NE(server.target, nullptr);
	EXPECT_EQ(validate->fn(server), BuildStepStatus::Failed);
}

TEST_F(CSharpBuildStepsTest, MissingSDKFailsValidationWithItsVersion) {
	const auto get = reinterpret_cast<CSharpEditor::GetEditorTestHooksFn>(
		ModuleSystem::FindSymbol(ModuleSystem::GetNativeHandle(_package), CSharpEditor::EDITOR_TEST_HOOKS_SYMBOL));
	ASSERT_NE(get, nullptr);
	const CSharpEditor::EditorTestHooks* editor = get();
	Vector<BuildStepDesc> steps{GetMemoryResource(MemTag::Debug)};
	BuildStepRegistry::GetSteps(steps);
	const BuildStepDesc* validate = Find(steps, "csharp-validate");
	ASSERT_NE(validate, nullptr);
	const fs::path root = fs::temp_directory_path() / ("slt_cs_bare_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	ScopedSettingsStore store{root};
	ResolvedProfile profile;
	profile.name = "Bare";
	BuildContext context;
	context.profile = &profile;
	context.target = BuildTargetRegistry::Find("Windows");
	editor->hideSDK(true);
	const BuildStepStatus status = validate->fn(context);
	editor->hideSDK(false);
	EXPECT_EQ(status, BuildStepStatus::Failed);
	ASSERT_EQ(context.GetIssues().size(), 1u);
	EXPECT_EQ(context.GetIssues()[0].message, std::format(".NET SDK {} wasn't found. Open project in editor once (downloads it)", editor->getPin().sdkVersion));
	std::error_code ec;
	fs::remove_all(root, ec);
}

TEST_F(CSharpBuildStepsTest, StageWithoutLibraryFailsWithPath) {
	Vector<BuildStepDesc> steps{GetMemoryResource(MemTag::Debug)};
	BuildStepRegistry::GetSteps(steps);
	const BuildStepDesc* stage = Find(steps, "csharp-stage");
	ASSERT_NE(stage, nullptr);
	const fs::path root = fs::temp_directory_path() / ("slt_cs_stage_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	ResolvedProfile profile;
	profile.name = "Empty";
	BuildContext context;
	context.profile = &profile;
	context.target = BuildTargetRegistry::Find("Windows");
	context.projectRoot = root.generic_string();
	context.stagingDir = (root / "Builds" / "Empty").generic_string();
	context.intermediateDir = (root / "Library" / "Player" / "Empty").generic_string();
	EXPECT_EQ(stage->fn(context), BuildStepStatus::Failed);
	ASSERT_EQ(context.GetIssues().size(), 1u);
	EXPECT_NE(context.GetIssues()[0].message.find("Library/CSharp/Player/Empty/publish/GameScripts"), str::npos) << context.GetIssues()[0].message;

	const fs::path publish = root / "Library" / "CSharp" / "Player" / "Empty" / "publish";
	fs::create_directories(publish);
	std::ofstream(publish / LIBRARY, std::ios::binary) << "library";
	std::ofstream(publish / "GameScripts.pdb", std::ios::binary) << "symbols";
	std::ofstream(publish / "Scripted.Scripts.pdb", std::ios::binary) << "managed symbols";
	BuildContext staged;
	staged.profile = &profile;
	staged.target = context.target;
	staged.projectRoot = context.projectRoot;
	staged.stagingDir = context.stagingDir;
	staged.intermediateDir = context.intermediateDir;
	EXPECT_EQ(stage->fn(staged), BuildStepStatus::Succeeded);
	EXPECT_TRUE(staged.GetIssues().empty());
	EXPECT_EQ(ReadBytes(root / "Builds" / "Empty" / LIBRARY), "library");
	EXPECT_EQ(ReadBytes(root / "Library" / "Player" / "Empty" / "symbols" / "GameScripts.pdb"), "symbols");
	u32 staging = 0;
	std::error_code ec;
	for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root / "Builds", ec)) {
		staging += entry.is_regular_file(ec) ? 1u : 0u;
	}
	EXPECT_EQ(staging, 1u);

	std::ofstream(publish / LIBRARY, std::ios::binary) << "library built again";
	EXPECT_EQ(stage->fn(staged), BuildStepStatus::Succeeded);
	EXPECT_EQ(ReadBytes(root / "Builds" / "Empty" / LIBRARY), "library built again");
	fs::remove_all(root, ec);
}

TEST_F(CSharpBuildStepsTest, ManagedFileInStagingFailsTheBuild) {
	Vector<BuildStepDesc> steps{GetMemoryResource(MemTag::Debug)};
	BuildStepRegistry::GetSteps(steps);
	const BuildStepDesc* gate = Find(steps, "csharp-no-il");
	ASSERT_NE(gate, nullptr);
	const fs::path root = fs::temp_directory_path() / ("slt_cs_gate_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	const fs::path staging = root / "Builds" / "Gated";
	fs::create_directories(staging / "Data");
	std::ofstream(staging / "Game.exe", std::ios::binary) << "player";
	std::ofstream(staging / "GameScripts.pdb", std::ios::binary) << "symbols";
	ResolvedProfile profile;
	profile.name = "Gated";
	const auto run = [&](const char* configuration, BuildContext& context) {
		context.profile = &profile;
		context.target = BuildTargetRegistry::Find("Windows");
		context.stagingDir = staging.generic_string();
		context.configuration = configuration;
		BuildStepStatus status = BuildStepStatus::Running;
		for (u32 pump = 0; pump < 64 && status == BuildStepStatus::Running; ++pump) {
			status = gate->fn(context);
		}
		return status;
	};

	BuildContext development;
	EXPECT_EQ(run("Debug", development), BuildStepStatus::Succeeded);
	EXPECT_TRUE(development.GetIssues().empty());
	EXPECT_EQ(_log.Delivered().Count("2 staged file(s) have no managed code"), 1u) << _log.Delivered().Joined();

	BuildContext release;
	EXPECT_EQ(run("Release", release), BuildStepStatus::Failed);
	ASSERT_EQ(release.GetIssues().size(), 1u);
	EXPECT_EQ(release.GetIssues()[0].severity, BuildIssueSeverity::Error);
	EXPECT_EQ(release.GetIssues()[0].message, "'GameScripts.pdb' has debug symbols (not for Release). Remove it from build output");

	std::error_code ec;
	fs::remove(staging / "GameScripts.pdb", ec);
	const fs::path managed = fs::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path() / "CSharp";
	fs::copy_file(managed / "Shalltear.Runtime.dll", staging / "Data" / "Mod.dll", ec);
	fs::copy_file(managed / "Shalltear.Generators.dll", staging / "Data" / "Tool.bin", ec);
	BuildContext planted;
	EXPECT_EQ(run("Release", planted), BuildStepStatus::Failed);
	ASSERT_EQ(planted.GetIssues().size(), 2u);
	EXPECT_EQ(planted.GetIssues()[0].message, "'Data/Mod.dll' has managed code. Remove it from build output");
	EXPECT_EQ(planted.GetIssues()[1].message, "'Data/Tool.bin' has managed code. Remove it from build output");
	EXPECT_EQ(_log.Delivered().Count("have no managed code"), 1u);

	fs::remove_all(staging / "Data", ec);
	BuildContext clean;
	EXPECT_EQ(run("Release", clean), BuildStepStatus::Succeeded);
	EXPECT_EQ(_log.Delivered().Count("1 staged file(s) have no managed code"), 1u) << _log.Delivered().Joined();
	fs::remove_all(root, ec);
}

#endif
