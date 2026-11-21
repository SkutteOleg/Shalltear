#include "TestsPCH.h"

#include "Shalltear/Core/Asset/PakFormat.h"

#include <cstring>

using namespace Shalltear;

TEST(PakOverlayFooterTest, LayoutAndMagicAreFixed) {
	PakOverlayFooter footer{};
	std::memcpy(footer.magic, PAK_OVERLAY_MAGIC, sizeof(PAK_OVERLAY_MAGIC));
	footer.exeSize = 1024;
	footer.pakOffset = 1024;
	footer.pakSize = 4096;

	EXPECT_EQ(sizeof(PakOverlayFooter), 32u);
	EXPECT_EQ(std::memcmp(footer.magic, "SLTPKOV1", 8), 0);
}

#if defined(SHALLTEAR_SHARED)

#include "CSharpRuntimeFixture.h"
#include "EditorTestHooks.h"
#include "ScopedSettingsStore.h"
#include "TestPlatform.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/BuildInfo.h"
#include "Shalltear/Core/Config.h"
#include "Shalltear/Core/FileSystem.h"

#include "Shalltear/Editor/BuildProfile.h"
#include "Shalltear/Editor/ScriptProjectOps.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>

using namespace Shalltear::CSharpEditor;
using namespace Shalltear::CSharpTests;

namespace {

	namespace fs = std::filesystem;

	constexpr const char* SINGLE_FILE_PROBE_SCRIPT = R"(using Shalltear;

namespace SingleFileDemo;

public sealed class Probe : Behavior {
	int _frames;

	protected override void OnEnable() {
		Log.Info("SingleFileProbe", "SHALLTEAR_SINGLE_FILE_ENABLED owner=" + Owner.Name);
	}

	protected override void OnUpdate() {
		if (++_frames == 2) {
			Log.Info("SingleFileProbe", "SHALLTEAR_SINGLE_FILE_TICK frame=" + _frames);
		}
	}
}
)";

	str ReadFileBytes(const fs::path& file) {
		std::ifstream stream(file, std::ios::binary);
		return str(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
	}

	str ReadCacheEntry(const fs::path& buildTree, std::string_view key) {
		const str text = ReadFileBytes(buildTree / "CMakeCache.txt");
		const size_t at = text.find(key);
		if (at == str::npos) {
			return {};
		}
		const size_t eq = text.find('=', at);
		if (eq == str::npos) {
			return {};
		}
		size_t end = text.find('\n', eq);
		if (end == str::npos) {
			end = text.size();
		}
		str value = text.substr(eq + 1, end - eq - 1);
		while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
			value.pop_back();
		}
		return value;
	}

	str FindStaticTreeWithCSharp(str& outConfig) {
		const fs::path tree = fs::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path().parent_path();
		const str expectedCommit = std::format("set(SHALLTEAR_GIT_COMMIT \"{}\")", BuildInfo::GetGitCommit());
		std::error_code ec;
		str fallbackTree;
		str fallbackConfig;
		for (const fs::directory_entry& entry : fs::directory_iterator(tree.parent_path(), ec)) {
			if (!entry.is_directory(ec) || entry.path() == tree) {
				continue;
			}
			const str package = ReadFileBytes(entry.path() / "ShalltearConfig.cmake");
			if (package.find("set(SHALLTEAR_LINK_MODE \"STATIC\")") == str::npos ||
				package.find(expectedCommit) == str::npos) {
				continue;
			}
			if (!fs::is_regular_file(entry.path() / "lib" / "CSharp.a", ec) &&
				!fs::is_regular_file(entry.path() / "lib" / "libCSharp.a", ec)) {
				continue;
			}
			const str cfg = ReadCacheEntry(entry.path(), "CMAKE_BUILD_TYPE:");
			if (cfg == "Release") {
				outConfig = "Release";
				return entry.path().generic_string();
			}
			if (fallbackTree.empty() && !cfg.empty()) {
				fallbackTree = entry.path().generic_string();
				fallbackConfig = cfg;
			}
		}
		outConfig = fallbackConfig;
		return fallbackTree;
	}

	str SingleFileSceneText() {
		return std::format(
			"{{\n"
			"\t\"_type\": \"Scene\",\n"
			"\t\"formatVersion\": 1,\n"
			"\t\"engineHash\": \"{:016x}\",\n"
			"\t\"name\": \"Main\",\n"
			"\t\"nextLocalId\": 5,\n"
			"\t\"objects\": [\n"
			"\t\t{{\n"
			"\t\t\t\"id\": 1,\n"
			"\t\t\t\"parent\": 0,\n"
			"\t\t\t\"type\": \"{:016x}\",\n"
			"\t\t\t\"fields\": {{\"_type\": \"SceneObject\", \"name\": \"Camera\", \"_activeSelf\": true, "
			"\"_localPosition\": [0, 0, 0], \"_localRotation\": [0, 0, 0, 1], \"_localScale\": [1, 1, 1]}},\n"
			"\t\t\t\"components\": [{{\"id\": 2, \"type\": \"{:016x}\", \"fields\": {{\"_type\": \"Camera\", \"_enabledSelf\": true}}}}]\n"
			"\t\t}},\n"
			"\t\t{{\n"
			"\t\t\t\"id\": 3,\n"
			"\t\t\t\"parent\": 0,\n"
			"\t\t\t\"type\": \"{:016x}\",\n"
			"\t\t\t\"fields\": {{\"_type\": \"SceneObject\", \"name\": \"Runner\", \"_activeSelf\": true, "
			"\"_localPosition\": [0, 0, 0], \"_localRotation\": [0, 0, 0, 1], \"_localScale\": [1, 1, 1]}},\n"
			"\t\t\t\"components\": [\n"
			"\t\t\t\t{{\n"
			"\t\t\t\t\t\"id\": 4,\n"
			"\t\t\t\t\t\"type\": \"{:016x}\",\n"
			"\t\t\t\t\t\"fields\": {{\"_type\": \"SingleFileDemo.Probe\", \"_enabledSelf\": true}}\n"
			"\t\t\t\t}}\n"
			"\t\t\t]\n"
			"\t\t}}\n"
			"\t]\n"
			"}}\n",
			BuildInfo::GetEngineHash(), StringHash("SceneObject").GetValue(), StringHash("Camera").GetValue(),
			StringHash("SceneObject").GetValue(), StringHash("SingleFileDemo.Probe").GetValue());
	}

	class DotNetToolchainBundledTierTest : public CSharpRuntimeFixture {
	protected:
		const EditorTestHooks* _editor = nullptr;
		fs::path _root;
		DotNetToolchain::Pin _pin;

		void SetUp() override {
			CSharpRuntimeFixture::SetUp();
			const auto get = reinterpret_cast<GetEditorTestHooksFn>(
				ModuleSystem::FindSymbol(ModuleSystem::GetNativeHandle(_package), EDITOR_TEST_HOOKS_SYMBOL));
			ASSERT_NE(get, nullptr);
			_editor = get();
			_root = fs::temp_directory_path() /
					("slt_dotnet_bundled_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			fs::create_directories(_root);
			_pin.sdkVersion = "10.0.999";
			_pin.runtimeVersion = "10.0.99";
			_pin.targetFramework = "net10.0";
			_pin.hostRid = "test-x64";
		}

		void TearDown() override {
			std::error_code ec;
			fs::remove_all(_root, ec);
			CSharpRuntimeFixture::TearDown();
		}

		str MakeFakeSDK(const fs::path& root) {
			fs::create_directories(root / "sdk" / _pin.sdkVersion);
			fs::create_directories(root / "host" / "fxr" / _pin.runtimeVersion);
			std::ofstream(root / _editor->muxerName, std::ios::binary) << "muxer";
			std::ofstream(root / "sdk" / _pin.sdkVersion / "dotnet.dll", std::ios::binary) << "sdk";
			std::ofstream(root / "host" / "fxr" / _pin.runtimeVersion / _editor->hostLibraryName, std::ios::binary) << "fxr";
			return root.generic_string();
		}
	};

	struct SingleFileSuite {
		fs::path root;
		fs::path project;
		str config;
		str skip;
		str editor;
		std::unique_ptr<ScopedUserDataDirectory> userData;
		int singleBuildExit = -1;
		str singleBuildLog;
		int singleRunExit = -1;
		str singleRunLog;
		int sidecarBuildExit = -1;
		str sidecarBuildLog;
		int sidecarRunExit = -1;
		str sidecarRunLog;
	};

	SingleFileSuite* _singleSuite = nullptr;

	class CSharpSingleFilePlayerTest : public ::testing::Test {
	protected:
		static void SetUpTestSuite() {
			_singleSuite = new SingleFileSuite();
			SingleFileSuite& suite = *_singleSuite;
			if (std::string_view(SLT_TEST_HOST_TARGET) != "Windows") {
				suite.skip = "single-file player test targets Windows host";
				return;
			}
			const str staticTree = FindStaticTreeWithCSharp(suite.config);
			if (staticTree.empty()) {
				suite.skip = "no sibling static tree with C# package";
				return;
			}
			suite.editor = Path(ScriptProjectOps::GetEditorExecutablePath()).GetDirectory() + "/Shalltear" SLT_TEST_EXE_SUFFIX;
			std::error_code ec;
			if (!fs::is_regular_file(fs::path(suite.editor), ec)) {
				suite.skip = "no editor executable";
				return;
			}

			suite.root = fs::temp_directory_path() /
						 ("slt_cs_singlefile_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			suite.project = suite.root / "SingleFileDemo";
			fs::create_directories(suite.project / "Assets" / "Scripts");
			fs::create_directories(suite.project / "Assets" / "Scenes");

			suite.userData = std::make_unique<ScopedUserDataDirectory>(suite.root / "UserData");
			fs::create_directories(suite.root / "UserData" / "Shalltear");
			std::ofstream(suite.root / "UserData" / "Shalltear" / "UserSettings.ini", std::ios::binary)
				<< "[Editor/Build]\nstaticSDKRoot = " << staticTree << "\n";

			Config projectFile;
			projectFile.SetString("Project", "name", "SingleFileDemo");
			ASSERT_TRUE(projectFile.Save(Path(suite.project.generic_string() + "/Project.ini")));
			Config settings;
			settings.SetString("Packages", "enabled", "dev.shalltear.csharp");
			ASSERT_TRUE(settings.Save(Path(suite.project.generic_string() + "/ProjectSettings.ini")));

			std::ofstream(suite.project / "Assets" / "Scripts" / "Probe.cs", std::ios::binary) << SINGLE_FILE_PROBE_SCRIPT;
			std::ofstream(suite.project / "Assets" / "Scenes" / "Main.scene", std::ios::binary) << SingleFileSceneText();

			for (const bool singleFile : {true, false}) {
				BuildProfile profile;
				profile.name = singleFile ? "SingleFile" : "Sidecar";
				profile.target = "Windows";
				profile.SetList("scenes", Vector<str>{"Assets/Scenes/Main.scene"});
				profile.SetString("configuration", suite.config);
				profile.SetString("singleFile", singleFile ? "true" : "false");
				ASSERT_TRUE(BuildProfileOps::SaveProfile(
					BuildProfileOps::GetProfilePath(suite.project.generic_string(), profile.name), profile));
			}

			FileSystem::Mount("project", Path(suite.project.generic_string()));
			AssetDatabase::Initialize();
			AssetDatabase::ScanProjectDirectory();
			const bool indexed = AssetDatabase::LookupGuid(Path("project://Assets/Scenes/Main.scene")).IsValid();
			AssetDatabase::Terminate();
			FileSystem::Unmount("project");
			ASSERT_TRUE(indexed);

			suite.singleBuildLog = TestPlatform::RunCommand(
				"\"" + suite.editor + "\" --project \"" + suite.project.generic_string() + "\" --build SingleFile",
				suite.singleBuildExit);
			if (suite.singleBuildExit == 0) {
				const fs::path builtExe = suite.project / "Builds" / "SingleFile" / ("SingleFileDemo" SLT_TEST_PLAYER_SUFFIX);
				const fs::path isolatedDir = suite.root / "Isolated";
				fs::create_directories(isolatedDir);
				const fs::path isolatedExe = isolatedDir / builtExe.filename();
				fs::copy_file(builtExe, isolatedExe, fs::copy_options::overwrite_existing, ec);
				suite.singleRunLog = TestPlatform::RunCommand("\"" + isolatedExe.generic_string() + "\" --frames 5", suite.singleRunExit);
			}

			suite.sidecarBuildLog = TestPlatform::RunCommand(
				"\"" + suite.editor + "\" --project \"" + suite.project.generic_string() + "\" --build Sidecar",
				suite.sidecarBuildExit);
			if (suite.sidecarBuildExit == 0) {
				const fs::path builtExe = suite.project / "Builds" / "Sidecar" / ("SingleFileDemo" SLT_TEST_PLAYER_SUFFIX);
				suite.sidecarRunLog = TestPlatform::RunCommand("\"" + builtExe.generic_string() + "\" --frames 5", suite.sidecarRunExit);
			}
		}

		static void TearDownTestSuite() {
			if (_singleSuite) {
				_singleSuite->userData.reset();
				std::error_code ec;
				fs::remove_all(_singleSuite->root, ec);
				delete _singleSuite;
				_singleSuite = nullptr;
			}
		}

		void SetUp() override {
			ASSERT_NE(_singleSuite, nullptr);
			if (!_singleSuite->skip.empty()) {
				GTEST_SKIP() << _singleSuite->skip;
			}
		}
	};

} // anonymous namespace

TEST_F(DotNetToolchainBundledTierTest, BundledSitsBetweenOverrideAndCache) {
	DotNetToolchain::DiscoveryOptions options;
	options.overrideRoot = MakeFakeSDK(_root / "override");
	options.bundledRoot = MakeFakeSDK(_root / "Toolchain" / "DotNet");
	options.cacheRoot = (_root / "cache").generic_string();
	MakeFakeSDK(_root / "cache" / _pin.sdkVersion);

	DotNetToolchain::Info info = _editor->discover(_pin, options);
	EXPECT_EQ(info.tier, DotNetToolchain::Tier::Override);

	options.overrideRoot.clear();
	info = _editor->discover(_pin, options);
	EXPECT_EQ(info.tier, DotNetToolchain::Tier::Bundled);
	EXPECT_EQ(info.root, options.bundledRoot);

	// also accepts nested <bundledRoot>/<sdkVersion>
	const fs::path nestedBase = _root / "nested-bundled";
	const str nested = MakeFakeSDK(nestedBase / _pin.sdkVersion);
	options.bundledRoot = nestedBase.generic_string();
	info = _editor->discover(_pin, options);
	EXPECT_EQ(info.tier, DotNetToolchain::Tier::Bundled);
	EXPECT_EQ(info.root, nested);

	options.bundledRoot = (_root / "absent-bundled").generic_string();
	info = _editor->discover(_pin, options);
	EXPECT_EQ(info.tier, DotNetToolchain::Tier::Cache);

	const str described = _editor->describe(_pin, options);
	EXPECT_NE(described.find("absent-bundled"), str::npos) << described;
}

TEST_F(CSharpSingleFilePlayerTest, SingleFileExecutableRunsIsolatedWithZeroSidecars) {
	ASSERT_EQ(_singleSuite->singleBuildExit, 0) << _singleSuite->singleBuildLog;

	const fs::path staging = _singleSuite->project / "Builds" / "SingleFile";
	const fs::path exe = staging / ("SingleFileDemo" SLT_TEST_PLAYER_SUFFIX);
	ASSERT_TRUE(fs::is_regular_file(exe));
	EXPECT_FALSE(fs::exists(staging / "GameScripts.dll"));
	EXPECT_FALSE(fs::exists(staging / "Data" / "Content.slpak"));
	EXPECT_FALSE(fs::exists(staging / "libc++.dll"));
	EXPECT_FALSE(fs::exists(staging / "libunwind.dll"));

	// verify SLTPKOV1 overlay footer appended to executable
	const str exeBytes = ReadFileBytes(exe);
	ASSERT_GE(exeBytes.size(), sizeof(PakOverlayFooter));
	PakOverlayFooter footer{};
	std::memcpy(&footer, exeBytes.data() + exeBytes.size() - sizeof(PakOverlayFooter), sizeof(PakOverlayFooter));
	EXPECT_EQ(std::memcmp(footer.magic, PAK_OVERLAY_MAGIC, sizeof(PAK_OVERLAY_MAGIC)), 0);
	EXPECT_GT(footer.exeSize, 0u);
	EXPECT_GT(footer.pakSize, 0u);

	EXPECT_EQ(_singleSuite->singleRunExit, 0) << _singleSuite->singleRunLog;
	EXPECT_NE(_singleSuite->singleRunLog.find("SHALLTEAR_SINGLE_FILE_ENABLED owner=Runner"), str::npos) << _singleSuite->singleRunLog;
	EXPECT_NE(_singleSuite->singleRunLog.find("SHALLTEAR_SINGLE_FILE_TICK frame=2"), str::npos) << _singleSuite->singleRunLog;
}

TEST_F(CSharpSingleFilePlayerTest, SidecarModeProducesGameScriptsLibraryAndRuns) {
	ASSERT_EQ(_singleSuite->sidecarBuildExit, 0) << _singleSuite->sidecarBuildLog;

	const fs::path staging = _singleSuite->project / "Builds" / "Sidecar";
	EXPECT_TRUE(fs::is_regular_file(staging / ("SingleFileDemo" SLT_TEST_PLAYER_SUFFIX)));
	EXPECT_TRUE(fs::is_regular_file(staging / "GameScripts.dll"));
	EXPECT_TRUE(fs::is_regular_file(staging / "Data" / "Content.slpak"));
	EXPECT_EQ(_singleSuite->sidecarRunExit, 0) << _singleSuite->sidecarRunLog;
	EXPECT_NE(_singleSuite->sidecarRunLog.find("SHALLTEAR_SINGLE_FILE_ENABLED owner=Runner"), str::npos) << _singleSuite->sidecarRunLog;
	EXPECT_NE(_singleSuite->sidecarRunLog.find("SHALLTEAR_SINGLE_FILE_TICK frame=2"), str::npos) << _singleSuite->sidecarRunLog;
}

#endif
