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
#include "LinuxLibraryReader.h"
#include "ScopedSettingsStore.h"
#include "TestPlatform.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/BuildInfo.h"
#include "Shalltear/Core/Config.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/Settings.h"

#include "Shalltear/Editor/BuildPipeline.h"
#include "Shalltear/Editor/BuildProfile.h"
#include "Shalltear/Editor/BuildTarget.h"
#include "Shalltear/Editor/EditorConsole.h"
#include "Shalltear/Editor/ScriptProjectOps.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <thread>

using namespace Shalltear::CSharpEditor;
using namespace Shalltear::CSharpTests;

namespace {

	namespace fs = std::filesystem;

	constexpr const char* SCRIPTS_LIBRARY = "GameScripts" SLT_TEST_DLL_SUFFIX;

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

	bool IsLinuxTargetPackage(const str& packageText) {
		constexpr std::string_view Marker = "set(SHALLTEAR_COMPILER_TARGET \"";
		const size_t at = packageText.find(Marker);
		if (at == str::npos) {
			return false;
		}
		const size_t start = at + Marker.size();
		const size_t end = packageText.find('"', start);
		const std::string_view triple = end == str::npos ? std::string_view(packageText).substr(start)
														 : std::string_view(packageText).substr(start, end - start);
		return triple.find("linux") != std::string_view::npos;
	}

	str FindStaticTreeWithCSharp(str& outConfig, bool wantLinux = std::string_view(SLT_TEST_HOST_TARGET) == "Linux") {
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
				package.find(expectedCommit) == str::npos || IsLinuxTargetPackage(package) != wantLinux) {
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

	void VerifyLinuxElfSystemLibraries(const fs::path& elfBinary) {
		TestHelpers::LibraryNeeds needs;
		ASSERT_TRUE(TestHelpers::ReadLibraryNeeds(elfBinary, needs)) << elfBinary.generic_string();
		ASSERT_FALSE(needs.libraries.empty());
		for (const str& name : needs.libraries) {
			const bool system = name == "libc.so.6" || name == "libm.so.6" || name == "libdl.so.2" ||
								name == "libpthread.so.0" || name == "librt.so.1" || name == "libgcc_s.so.1" ||
								name == "ld-linux-x86-64.so.2";
			EXPECT_TRUE(system) << name;
		}
		EXPECT_EQ(needs.glibcMajor, 2u);
		EXPECT_GT(needs.glibcMinor, 0u);
		EXPECT_LE(needs.glibcMinor, 31u);
	}

	str SingleFileSceneText(std::string_view behaviorType = "SingleFileDemo.Probe") {
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
			"\t\t\t\t\t\"fields\": {{\"_type\": \"{}\", \"_enabledSelf\": true}}\n"
			"\t\t\t\t}}\n"
			"\t\t\t]\n"
			"\t\t}}\n"
			"\t]\n"
			"}}\n",
			BuildInfo::GetEngineHash(), StringHash("SceneObject").GetValue(), StringHash("Camera").GetValue(),
			StringHash("SceneObject").GetValue(), StringHash(behaviorType).GetValue(), behaviorType);
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
		int singleNoticesExit = -1;
		str singleNoticesOutput;
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
			if (std::string_view(SLT_TEST_HOST_TARGET) != "Windows" && std::string_view(SLT_TEST_HOST_TARGET) != "Linux") {
				suite.skip = "single-file player test targets Windows or Linux host";
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
				profile.target = SLT_TEST_HOST_TARGET;
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
				const fs::path noticesDir = suite.root / "IsolatedNotices";
				fs::create_directories(noticesDir);
				const fs::path noticesExe = noticesDir / builtExe.filename();
				fs::copy_file(builtExe, noticesExe, fs::copy_options::overwrite_existing, ec);
				suite.singleNoticesOutput = TestPlatform::RunCommand(
					"\"" + noticesExe.generic_string() + "\" --third-party-notices", suite.singleNoticesExit);

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

	class CSharpMixedSingleFilePipelineTest : public CSharpRuntimeFixture {
	protected:
		fs::path _root;
		fs::path _project;
		str _config;
		str _skip;
		std::unique_ptr<ScopedUserDataDirectory> _userData;
		bool _mounted = false;

		static fs::path FindEngineAssetsDir() {
			std::error_code ec;
			fs::path probe = fs::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path();
			for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
				if (fs::is_directory(probe / "EngineAssets", ec)) {
					return probe / "EngineAssets";
				}
				const fs::path parent = probe.parent_path();
				if (parent == probe) {
					break;
				}
				probe = parent;
			}
			return {};
		}

		void SetUp() override {
			CSharpRuntimeFixture::SetUp();
			if (std::string_view(SLT_TEST_HOST_TARGET) != "Windows" && std::string_view(SLT_TEST_HOST_TARGET) != "Linux") {
				_skip = "single-file player test targets Windows or Linux host";
				return;
			}
			const str staticTree = FindStaticTreeWithCSharp(_config);
			if (staticTree.empty()) {
				_skip = "no sibling static tree with C# package";
				return;
			}
			const fs::path engineAssets = FindEngineAssetsDir();
			if (engineAssets.empty()) {
				_skip = "EngineAssets directory not found";
				return;
			}

			_root = fs::temp_directory_path() /
					("slt_cs_mixed_single_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			_project = _root / "MixedSingleFile";
			fs::create_directories(_project / "Source");
			fs::create_directories(_project / "Assets" / "Scripts");
			fs::create_directories(_project / "Assets" / "Scenes");

			_userData = std::make_unique<ScopedUserDataDirectory>(_root / "UserData");
			fs::create_directories(_root / "UserData" / "Shalltear");
			std::ofstream(_root / "UserData" / "Shalltear" / "UserSettings.ini", std::ios::binary)
				<< "[Editor/Build]\nstaticSDKRoot = " << staticTree << "\n";

			Config projectFile;
			projectFile.SetString("Project", "name", "MixedSingleFile");
			ASSERT_TRUE(projectFile.Save(Path(_project.generic_string() + "/Project.ini")));
			Config settings;
			settings.SetString("Packages", "enabled", "dev.shalltear.csharp");
			ASSERT_TRUE(settings.Save(Path(_project.generic_string() + "/ProjectSettings.ini")));

			FileSystem::Mount("engine", Path(engineAssets.generic_string()));
			FileSystem::Mount("project", Path(_project.generic_string()));
			_mounted = true;

			EditorConsole::Initialize();
			Settings::Initialize(Settings::GetDefaultPaths());
			AssetDatabase::Initialize();
			SceneSystem::Initialize();
			SceneSystem::SetPlaying(false);
		}

		void TearDown() override {
			// unload CSharp module while SceneSystem/Settings/EditorConsole are still alive
			CSharpRuntimeFixture::TearDown();
			if (_mounted) {
				BuildTargetRegistry::Shutdown();
				SceneSystem::Terminate();
				AssetDatabase::Terminate();
				Settings::Terminate();
				EditorConsole::Terminate();
				FileSystem::Unmount("project");
				FileSystem::Unmount("engine");
				_mounted = false;
			}
			_userData.reset();
			if (!_root.empty()) {
				std::error_code ec;
				fs::remove_all(_root, ec);
			}
		}

		static str RunPipelineBuild(const str& projectRoot, const str& profileName, bool& outSucceeded) {
			BuildPipeline pipeline;
			BuildRequest request;
			request.projectPath = projectRoot;
			request.profiles.push_back(profileName);
			str refusal;
			if (!pipeline.Request(request, refusal)) {
				outSucceeded = false;
				return "refused: " + refusal;
			}
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
			while (pipeline.IsBusy() && std::chrono::steady_clock::now() < deadline) {
				pipeline.PumpMainThread();
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			pipeline.PumpMainThread();
			if (pipeline.IsBusy()) {
				pipeline.Cancel();
				outSucceeded = false;
				return "build timed out on step: " + pipeline.GetStepName();
			}
			const BuildResult& result = pipeline.GetResult();
			outSucceeded = result.AllSucceeded();
			str summary;
			for (const BuildProfileResult& entry : result.profiles) {
				summary += entry.profile + ": " + entry.reason + "\n";
				for (const BuildIssue& issue : entry.issues) {
					summary += "  issue [" + issue.key + "]: " + issue.message + "\n";
				}
			}
			return summary;
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
	EXPECT_FALSE(fs::exists(staging / SCRIPTS_LIBRARY));
	EXPECT_FALSE(fs::exists(staging / "Data" / "Content.slpak"));
	EXPECT_FALSE(fs::exists(staging / "libc++.dll"));
	EXPECT_FALSE(fs::exists(staging / "libunwind.dll"));
	EXPECT_FALSE(fs::exists(staging / "THIRD-PARTY-NOTICES.txt"));
	EXPECT_FALSE(fs::exists(staging / "THIRD-PARTY-NOTICES-DotNet.txt"));

	// staging folder holds strictly the single executable
	u32 stagedFiles = 0;
	std::error_code ec;
	for (const fs::directory_entry& entry : fs::recursive_directory_iterator(staging, ec)) {
		if (entry.is_regular_file(ec)) {
			++stagedFiles;
		}
	}
	EXPECT_EQ(stagedFiles, 1u);

	// verify SLTPKOV1 overlay footer appended to executable
	const str exeBytes = ReadFileBytes(exe);
	ASSERT_GE(exeBytes.size(), sizeof(PakOverlayFooter));
	PakOverlayFooter footer{};
	std::memcpy(&footer, exeBytes.data() + exeBytes.size() - sizeof(PakOverlayFooter), sizeof(PakOverlayFooter));
	EXPECT_EQ(std::memcmp(footer.magic, PAK_OVERLAY_MAGIC, sizeof(PAK_OVERLAY_MAGIC)), 0);
	EXPECT_GT(footer.exeSize, 0u);
	EXPECT_GT(footer.pakSize, 0u);

	if (std::string_view(SLT_TEST_HOST_TARGET) == "Linux") {
		VerifyLinuxElfSystemLibraries(exe);
	}

	// --third-party-notices prints embedded engine + .NET notices cleanly without creating files
	EXPECT_EQ(_singleSuite->singleNoticesExit, 0) << _singleSuite->singleNoticesOutput;
	EXPECT_NE(_singleSuite->singleNoticesOutput.find("THIRD-PARTY NOTICES FOR SHALLTEAR"), str::npos);
	EXPECT_NE(_singleSuite->singleNoticesOutput.find("by NativeAOT"), str::npos);
	EXPECT_NE(_singleSuite->singleNoticesOutput.find("SingleFileDemo" SLT_TEST_PLAYER_SUFFIX), str::npos);
	EXPECT_FALSE(fs::exists(_singleSuite->root / "IsolatedNotices" / "THIRD-PARTY-NOTICES.txt"));
	EXPECT_FALSE(fs::exists(_singleSuite->root / "IsolatedNotices" / "Logs"));

	EXPECT_EQ(_singleSuite->singleRunExit, 0) << _singleSuite->singleRunLog;
	EXPECT_NE(_singleSuite->singleRunLog.find("SHALLTEAR_SINGLE_FILE_ENABLED owner=Runner"), str::npos) << _singleSuite->singleRunLog;
	EXPECT_NE(_singleSuite->singleRunLog.find("SHALLTEAR_SINGLE_FILE_TICK frame=2"), str::npos) << _singleSuite->singleRunLog;

	// first launch unpacks THIRD-PARTY-NOTICES.txt beside the executable
	const fs::path unpackedNotices = _singleSuite->root / "Isolated" / "THIRD-PARTY-NOTICES.txt";
	ASSERT_TRUE(fs::is_regular_file(unpackedNotices));
	const str unpackedText = ReadFileBytes(unpackedNotices);
	EXPECT_NE(unpackedText.find("THIRD-PARTY NOTICES FOR SHALLTEAR"), str::npos);
	EXPECT_NE(unpackedText.find("by NativeAOT"), str::npos);
	EXPECT_NE(unpackedText.find("SingleFileDemo" SLT_TEST_PLAYER_SUFFIX), str::npos);
}

TEST_F(CSharpSingleFilePlayerTest, SidecarModeProducesGameScriptsLibraryAndRuns) {
	ASSERT_EQ(_singleSuite->sidecarBuildExit, 0) << _singleSuite->sidecarBuildLog;

	const fs::path staging = _singleSuite->project / "Builds" / "Sidecar";
	EXPECT_TRUE(fs::is_regular_file(staging / ("SingleFileDemo" SLT_TEST_PLAYER_SUFFIX)));
	EXPECT_TRUE(fs::is_regular_file(staging / SCRIPTS_LIBRARY));
	EXPECT_TRUE(fs::is_regular_file(staging / "Data" / "Content.slpak"));
	EXPECT_EQ(_singleSuite->sidecarRunExit, 0) << _singleSuite->sidecarRunLog;
	EXPECT_NE(_singleSuite->sidecarRunLog.find("SHALLTEAR_SINGLE_FILE_ENABLED owner=Runner"), str::npos) << _singleSuite->sidecarRunLog;
	EXPECT_NE(_singleSuite->sidecarRunLog.find("SHALLTEAR_SINGLE_FILE_TICK frame=2"), str::npos) << _singleSuite->sidecarRunLog;
}

TEST_F(CSharpSingleFilePlayerTest, LinuxSingleFileCrossBuildProducesSelfContainedElf) {
	if (std::string_view(SLT_TEST_HOST_TARGET) != "Windows") {
		GTEST_SKIP() << "cross-build test runs from a Windows host";
	}
	str linuxConfig;
	const str linuxStaticTree = FindStaticTreeWithCSharp(linuxConfig, true);
	if (linuxStaticTree.empty()) {
		GTEST_SKIP() << "no sibling Linux static tree with C# package";
	}

	BuildProfile profile;
	profile.name = "LinuxSingleFile";
	profile.target = "Linux";
	profile.SetList("scenes", Vector<str>{"Assets/Scenes/Main.scene"});
	profile.SetString("configuration", linuxConfig);
	profile.SetString("singleFile", "true");
	ASSERT_TRUE(BuildProfileOps::SaveProfile(
		BuildProfileOps::GetProfilePath(_singleSuite->project.generic_string(), profile.name), profile));

	ScopedUserDataDirectory linuxUserData(_singleSuite->root / "LinuxUserData");
	fs::create_directories(_singleSuite->root / "LinuxUserData" / "Shalltear");
	std::ofstream(_singleSuite->root / "LinuxUserData" / "Shalltear" / "UserSettings.ini", std::ios::binary)
		<< "[Editor/Build]\nstaticSDKRoot = " << linuxStaticTree << "\n";

	int buildExit = -1;
	const str buildLog = TestPlatform::RunCommand(
		"\"" + _singleSuite->editor + "\" --project \"" + _singleSuite->project.generic_string() + "\" --build LinuxSingleFile",
		buildExit);
	ASSERT_EQ(buildExit, 0) << buildLog;

	const fs::path staging = _singleSuite->project / "Builds" / "LinuxSingleFile";
	const fs::path linuxExe = staging / "SingleFileDemo.x86_64";
	ASSERT_TRUE(fs::is_regular_file(linuxExe)) << buildLog;
	EXPECT_FALSE(fs::exists(staging / "GameScripts.so"));
	EXPECT_FALSE(fs::exists(staging / "Data" / "Content.slpak"));
	EXPECT_FALSE(fs::exists(staging / "THIRD-PARTY-NOTICES.txt"));
	EXPECT_FALSE(fs::exists(staging / "THIRD-PARTY-NOTICES-DotNet.txt"));

	u32 stagedFiles = 0;
	std::error_code ec;
	for (const fs::directory_entry& entry : fs::recursive_directory_iterator(staging, ec)) {
		if (entry.is_regular_file(ec)) {
			++stagedFiles;
		}
	}
	EXPECT_EQ(stagedFiles, 1u);

	const str exeBytes = ReadFileBytes(linuxExe);
	ASSERT_GE(exeBytes.size(), sizeof(PakOverlayFooter));
	PakOverlayFooter footer{};
	std::memcpy(&footer, exeBytes.data() + exeBytes.size() - sizeof(PakOverlayFooter), sizeof(PakOverlayFooter));
	EXPECT_EQ(std::memcmp(footer.magic, PAK_OVERLAY_MAGIC, sizeof(PAK_OVERLAY_MAGIC)), 0);
	EXPECT_GT(footer.exeSize, 0u);
	EXPECT_GT(footer.pakSize, 0u);

	VerifyLinuxElfSystemLibraries(linuxExe);

	// verify embedded notices inside the cross-compiled Linux overlay pak
	AssetDatabase::Initialize();
	str mountError;
	ASSERT_TRUE(AssetDatabase::MountPackage(Path(linuxExe.generic_string()), mountError)) << mountError;
	str embeddedNotices;
	ASSERT_TRUE(AssetDatabase::ReadPackageDocument(PakDocumentKind::ThirdPartyNotices, embeddedNotices));
	AssetDatabase::Terminate();
	EXPECT_NE(embeddedNotices.find("THIRD-PARTY NOTICES FOR SHALLTEAR"), str::npos);
	EXPECT_NE(embeddedNotices.find("by NativeAOT"), str::npos);
	EXPECT_NE(embeddedNotices.find("SingleFileDemo.x86_64"), str::npos);
}

TEST_F(CSharpMixedSingleFilePipelineTest, MixedCppHeaderBindingStaysInSyncWithoutEditorBuild) {
	if (!_skip.empty()) {
		GTEST_SKIP() << _skip;
	}

	std::ofstream(_project / "CMakeLists.txt", std::ios::binary)
		<< "cmake_minimum_required(VERSION 3.24)\n"
		   "project(MixedSingleFile LANGUAGES CXX)\n"
		   "find_package(Shalltear CONFIG REQUIRED)\n"
		   "shalltear_add_module(MixedSingleFile SOURCES Source/MixedModule.cpp)\n";

	std::ofstream(_project / "Source" / "NativeMath.h", std::ios::binary)
		<< "#pragma once\n"
		   "namespace NativeMath {\n"
		   "\tint ComputeScore(int base);\n"
		   "}\n";

	std::ofstream(_project / "Source" / "MixedModule.cpp", std::ios::binary)
		<< "#include \"Shalltear.h\"\n"
		   "#include \"NativeMath.h\"\n"
		   "#include \"MixedProbe.generated.h\"\n"
		   "namespace NativeMath {\n"
		   "\tint ComputeScore(int base) { return base + 10; }\n"
		   "}\n"
		   "static_assert(sizeof(Shalltear::CSharpViews::MixedProbe) > 0);\n"
		   "SLT_IMPLEMENT_MODULE(MixedSingleFile)\n";

	std::ofstream(_project / "Assets" / "Scripts" / "MixedProbe.cs", std::ios::binary)
		<< "using Shalltear;\n"
		   "namespace MixedDemo;\n"
		   "public sealed partial class MixedProbe : Behavior {\n"
		   "\tpublic int lastScore;\n"
		   "\tprotected override void OnEnable() {\n"
		   "\t\tlastScore = NativeMath.Native.ComputeScore(5);\n"
		   "\t\tLog.Info(\"MixedProbe\", \"SHALLTEAR_MIXED_SCORE=\" + lastScore);\n"
		   "\t}\n"
		   "}\n";

	std::ofstream(_project / "Assets" / "Scenes" / "Main.scene", std::ios::binary) << SingleFileSceneText("MixedDemo.MixedProbe");

	BuildProfile profile;
	profile.name = "SingleFile";
	profile.target = SLT_TEST_HOST_TARGET;
	profile.SetList("scenes", Vector<str>{"Assets/Scenes/Main.scene"});
	profile.SetString("configuration", _config);
	profile.SetString("singleFile", "true");
	ASSERT_TRUE(BuildProfileOps::SaveProfile(BuildProfileOps::GetProfilePath(_project.generic_string(), profile.name), profile));

	AssetDatabase::ScanProjectDirectory();
	ASSERT_TRUE(AssetDatabase::LookupGuid(Path("project://Assets/Scenes/Main.scene")).IsValid());

	// build 1: clean project, no prior Editor C++ compile, no pre-existing UserBindings.g.cs
	bool ok = false;
	const str build1Summary = RunPipelineBuild(_project.generic_string(), "SingleFile", ok);
	ASSERT_TRUE(ok) << build1Summary << "\n"
					<< _log.Delivered().Joined();

	const fs::path userBindings = _project / "Library" / "CSharp" / "Generated" / "Native" / "UserBindings.g.cs";
	ASSERT_TRUE(fs::is_regular_file(userBindings));
	EXPECT_NE(ReadFileBytes(userBindings).find("ComputeScore"), str::npos);
	EXPECT_FALSE(fs::exists(_project / "Library" / "Binaries"));

	std::error_code ec;
	const fs::path builtExe = _project / "Builds" / "SingleFile" / ("MixedSingleFile" SLT_TEST_PLAYER_SUFFIX);
	ASSERT_TRUE(fs::is_regular_file(builtExe));
	const fs::path iso1 = _root / "IsolatedMixed1";
	fs::create_directories(iso1);
	const fs::path isoExe1 = iso1 / builtExe.filename();
	fs::copy_file(builtExe, isoExe1, fs::copy_options::overwrite_existing, ec);
	ASSERT_FALSE(ec) << ec.message();

	int runExit = -1;
	const str run1Log = TestPlatform::RunCommand("\"" + isoExe1.generic_string() + "\" --frames 3", runExit);
	EXPECT_EQ(runExit, 0) << run1Log;
	EXPECT_NE(run1Log.find("SHALLTEAR_MIXED_SCORE=15"), str::npos) << run1Log;

	// build 2: mutate bound C++ header and rebuild in the same Editor session without compiling Editor C++
	std::ofstream(_project / "Source" / "NativeMath.h", std::ios::binary)
		<< "#pragma once\n"
		   "namespace NativeMath {\n"
		   "\tint ComputeScore(int base);\n"
		   "\tint ComputeBonus(int base, int extra);\n"
		   "}\n";

	std::ofstream(_project / "Source" / "MixedModule.cpp", std::ios::binary)
		<< "#include \"Shalltear.h\"\n"
		   "#include \"NativeMath.h\"\n"
		   "#include \"MixedProbe.generated.h\"\n"
		   "namespace NativeMath {\n"
		   "\tint ComputeScore(int base) { return base + 10; }\n"
		   "\tint ComputeBonus(int base, int extra) { return base * 2 + extra; }\n"
		   "}\n"
		   "static_assert(sizeof(Shalltear::CSharpViews::MixedProbe) > 0);\n"
		   "SLT_IMPLEMENT_MODULE(MixedSingleFile)\n";

	std::ofstream(_project / "Assets" / "Scripts" / "MixedProbe.cs", std::ios::binary)
		<< "using Shalltear;\n"
		   "namespace MixedDemo;\n"
		   "public sealed partial class MixedProbe : Behavior {\n"
		   "\tpublic int lastScore;\n"
		   "\tprotected override void OnEnable() {\n"
		   "\t\tlastScore = NativeMath.Native.ComputeBonus(7, 3);\n"
		   "\t\tLog.Info(\"MixedProbe\", \"SHALLTEAR_MIXED_BONUS=\" + lastScore);\n"
		   "\t}\n"
		   "}\n";

	ok = false;
	const str build2Summary = RunPipelineBuild(_project.generic_string(), "SingleFile", ok);
	ASSERT_TRUE(ok) << build2Summary << "\n"
					<< _log.Delivered().Joined();
	EXPECT_NE(ReadFileBytes(userBindings).find("ComputeBonus"), str::npos);

	const fs::path iso2 = _root / "IsolatedMixed2";
	fs::create_directories(iso2);
	const fs::path isoExe2 = iso2 / builtExe.filename();
	fs::copy_file(builtExe, isoExe2, fs::copy_options::overwrite_existing, ec);
	ASSERT_FALSE(ec) << ec.message();

	runExit = -1;
	const str run2Log = TestPlatform::RunCommand("\"" + isoExe2.generic_string() + "\" --frames 3", runExit);
	EXPECT_EQ(runExit, 0) << run2Log;
	EXPECT_NE(run2Log.find("SHALLTEAR_MIXED_BONUS=17"), str::npos) << run2Log;
}

#endif
