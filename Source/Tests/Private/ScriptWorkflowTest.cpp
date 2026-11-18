#include "TestsPCH.h"

#include "TestPlatform.h"
#include "Checkout.h"

#include "Shalltear/Core/Asset.h"
#include "Shalltear/Core/BuildInfo.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/ProjectLayout.h"
#include "Shalltear/Core/StringUtil.h"

#include "Shalltear/Editor/AssetBrowserOps.h"
#include "Shalltear/Editor/BuildProfile.h"
#include "Shalltear/Editor/ChildProcess.h"
#include "Shalltear/Editor/CompileService.h"
#include "Shalltear/Editor/CppToolchain.h"
#include "Shalltear/Editor/PackageOps.h"
#include "Shalltear/Editor/ProjectOps.h"
#include "Shalltear/Editor/ScriptProjectOps.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

using namespace Shalltear;

namespace {

	std::filesystem::path RepoDir(const char* relative) {
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

	std::vector<std::filesystem::path> PublicTrees(const std::filesystem::path& root) {
		std::vector<std::filesystem::path> trees;
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(root / "Source", ec)) {
			if (std::filesystem::is_directory(entry.path() / "Public", ec)) {
				trees.push_back(entry.path() / "Public");
			}
		}
		return trees;
	}

	class ScopedEnvironment {
	public:
		ScopedEnvironment(const char* name, const str& value) : _name(name), _previous(TestPlatform::GetEnvironment(name)) {
			TestPlatform::SetEnvironment(name, value);
		}
		~ScopedEnvironment() { TestPlatform::SetEnvironment(_name, _previous); }

		ScopedEnvironment(const ScopedEnvironment&) = delete;
		ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;
	private:
		const char* _name;
		str _previous;
	};

	class ScopedPath : public ScopedEnvironment {
	public:
		explicit ScopedPath(const str& value) : ScopedEnvironment("PATH", value) {}
	};

	class CapturingLogSink : public ILogSink {
	public:
		void ReceiveLog(const LogEvent& event) override {
			std::lock_guard<std::mutex> lock(_mutex);
			_messages.push_back(str(event.message));
		}
		void Flush() override {}

		Vector<str> MatchingAfter(std::string_view sentinel, std::string_view needle) const {
			std::lock_guard<std::mutex> lock(_mutex);
			Vector<str> out{GetMemoryResource(MemTag::Editor)};
			bool seen = false;
			for (const str& message : _messages) {
				if (!seen) {
					seen = message == sentinel;
					continue;
				}
				if (message.find(needle) != str::npos) {
					out.push_back(message);
				}
			}
			return out;
		}
	private:
		mutable std::mutex _mutex;
		std::vector<str> _messages;
	};

	class ScriptWorkflowFixture : public ::testing::Test {
	protected:
		std::filesystem::path _root;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			u64 unique = counter.fetch_add(1);
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() /
					("shalltear_script_" + std::to_string(stamp) + "_" + std::to_string(unique));
			std::filesystem::create_directories(_root);
			FileSystem::Mount("project", Path(_root.generic_string()));

			std::filesystem::path engineAssets = RepoDir("EngineAssets");
			if (!engineAssets.empty()) {
				FileSystem::Mount("engine", Path(engineAssets.generic_string()));
			}
		}

		void TearDown() override {
			FileSystem::Unmount("engine");
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		str Root() const { return _root.generic_string(); }

		static void WriteStub(const std::filesystem::path& path) {
			std::filesystem::create_directories(path.parent_path());
			std::ofstream stub(path, std::ios::binary);
			stub << "stub";
		}

		str Read(const str& relative) const {
			std::ifstream file(_root / relative, std::ios::binary);
			return str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		}

		void WriteSource(const str& virtualPath, const str& content) {
			Path resolved = FileSystem::Resolve(Path(virtualPath));
			std::error_code ec;
			std::filesystem::create_directories(std::filesystem::path(resolved.ToString()).parent_path(), ec);
			FileSystem::WriteTextFile(resolved, content);
		}

		bool Exists(const str& relative) const {
			return std::filesystem::exists(_root / relative);
		}
	};

} // namespace

TEST(ProjectLayoutTest, ContentRootsAreNamedAndNonEmpty) {
	std::span<const std::string_view> roots = ProjectLayout::GetContentRoots();
	ASSERT_FALSE(roots.empty());
	EXPECT_EQ(roots[0], "Assets");

	for (std::string_view name : roots) {
		EXPECT_NE(name, "Source");
		EXPECT_NE(name, "Library");
	}
}

TEST(ProjectLayoutTest, ReservedFileNames) {
	EXPECT_TRUE(ProjectLayout::IsReservedFileName(".assets.db"));
	EXPECT_TRUE(ProjectLayout::IsReservedFileName("Main.scene.meta"));
	EXPECT_FALSE(ProjectLayout::IsReservedFileName(".meta"));
	EXPECT_FALSE(ProjectLayout::IsReservedFileName("Main.scene"));
	EXPECT_FALSE(ProjectLayout::IsReservedFileName("metadata.txt"));

	EXPECT_TRUE(ProjectLayout::IsReservedFileName(".ASSETS.DB"));
	EXPECT_TRUE(ProjectLayout::IsReservedFileName("Main.scene.META"));
}

TEST_F(ScriptWorkflowFixture, ScanMintsNoSourceMeta) {
	WriteSource("project://Source/MyGame.cpp", "// module entry point\n");
	WriteSource("project://Source/ExampleComponent.h", "#pragma once\n");
	WriteSource("project://CMakeLists.txt", "project(MyGame CXX)\n");
	WriteSource("project://Assets/note.txt", "content");

	AssetDatabase::Initialize();
	AssetDatabase::ScanProjectDirectory();

	EXPECT_FALSE(Exists("Source/MyGame.cpp.meta"));
	EXPECT_FALSE(Exists("Source/ExampleComponent.h.meta"));
	EXPECT_FALSE(AssetDatabase::LookupGuid(Path("project://Source/MyGame.cpp")).IsValid());

	EXPECT_TRUE(Exists("Assets/note.txt.meta"));
	EXPECT_TRUE(AssetDatabase::LookupGuid(Path("project://Assets/note.txt")).IsValid());

	AssetDatabase::Terminate();
}

TEST_F(ScriptWorkflowFixture, BrowserWalkHidesSourceAndLibrary) {
	WriteSource("project://Source/MyGame.cpp", "// x\n");
	WriteSource("project://Library/Compile/Debug/log.txt", "x");
	WriteSource("project://Assets/note.txt", "content");

	Vector<AssetBrowserOps::Entry> entries{GetMemoryResource(MemTag::Editor)};
	ASSERT_TRUE(AssetBrowserOps::ScanTree(entries));

	bool sawAssets = false;
	for (const AssetBrowserOps::Entry& entry : entries) {
		EXPECT_FALSE(entry.relPath.starts_with("Source")) << entry.relPath;
		EXPECT_FALSE(entry.relPath.starts_with("Library")) << entry.relPath;
		sawAssets = sawAssets || entry.relPath == "Assets/note.txt";
	}
	EXPECT_TRUE(sawAssets);
}

TEST_F(ScriptWorkflowFixture, BrowserWalkKeepsNestedDirectoriesNamedSource) {
	WriteSource("project://Assets/Source/note.txt", "content");

	Vector<AssetBrowserOps::Entry> entries{GetMemoryResource(MemTag::Editor)};
	ASSERT_TRUE(AssetBrowserOps::ScanTree(entries));

	bool found = false;
	for (const AssetBrowserOps::Entry& entry : entries) {
		found = found || entry.relPath == "Assets/Source/note.txt";
	}
	EXPECT_TRUE(found);
}

TEST_F(ScriptWorkflowFixture, ScriptSettingsDefaultWithoutMigration) {
	WriteSource("project://Project.ini", "[Project]\nname = Legacy Game\ncreated = 1\n");

	ScriptProjectOps::ScriptSettings settings = ScriptProjectOps::ReadScriptSettings(Root());
	EXPECT_EQ(settings.moduleName, "LegacyGame");
	EXPECT_TRUE(settings.compileCommand.empty());

	settings.moduleName = "MyGame";
	settings.compileCommand = "ninja -C custom";
	ASSERT_TRUE(ScriptProjectOps::WriteScriptSettings(Root(), settings));

	ScriptProjectOps::ScriptSettings roundTripped = ScriptProjectOps::ReadScriptSettings(Root());
	EXPECT_EQ(roundTripped.moduleName, "MyGame");
	EXPECT_EQ(roundTripped.compileCommand, "ninja -C custom");

	str ini = Read("Project.ini");
	EXPECT_NE(ini.find("Legacy Game"), str::npos);
}

TEST_F(ScriptWorkflowFixture, LegacyBuildCommandIsHonoredOnRead) {
	WriteSource("project://Project.ini",
				"[Project]\nname = MyGame\n[Script]\nmodule = MyGame\nbuildCommand = ninja -C legacy\n");

	ScriptProjectOps::ScriptSettings settings = ScriptProjectOps::ReadScriptSettings(Root());
	EXPECT_EQ(settings.compileCommand, "ninja -C legacy");
	EXPECT_TRUE(settings.compileCommandIsLegacy);

	EXPECT_NE(Read("Project.ini").find("buildCommand"), str::npos);
}

TEST_F(ScriptWorkflowFixture, WritingScriptSettingsRetiresTheLegacyKey) {
	WriteSource("project://Project.ini",
				"[Project]\nname = MyGame\n[Script]\nmodule = MyGame\nbuildCommand = ninja -C legacy\n");

	ScriptProjectOps::ScriptSettings settings = ScriptProjectOps::ReadScriptSettings(Root());
	ASSERT_TRUE(ScriptProjectOps::WriteScriptSettings(Root(), settings));

	str ini = Read("Project.ini");
	EXPECT_NE(ini.find("compileCommand = ninja -C legacy"), str::npos) << ini;
	EXPECT_EQ(ini.find("buildCommand"), str::npos) << ini;

	ScriptProjectOps::ScriptSettings migrated = ScriptProjectOps::ReadScriptSettings(Root());
	EXPECT_EQ(migrated.compileCommand, "ninja -C legacy");
	EXPECT_FALSE(migrated.compileCommandIsLegacy);
}

TEST_F(ScriptWorkflowFixture, BothSpellingsPresentResolveToNewOne) {
	WriteSource("project://Project.ini",
				"[Project]\nname = MyGame\n[Script]\nmodule = MyGame\n"
				"buildCommand = ninja -C old\ncompileCommand = ninja -C new\n");

	ScriptProjectOps::ScriptSettings settings = ScriptProjectOps::ReadScriptSettings(Root());
	EXPECT_EQ(settings.compileCommand, "ninja -C new");
	EXPECT_FALSE(settings.compileCommandIsLegacy);

	ASSERT_TRUE(ScriptProjectOps::WriteScriptSettings(Root(), settings));
	str ini = Read("Project.ini");
	EXPECT_EQ(ini.find("ninja -C old"), str::npos) << ini;
	EXPECT_NE(ini.find("ninja -C new"), str::npos) << ini;
}

TEST_F(ScriptWorkflowFixture, ClearingTheCompileCommandLeavesNeitherSpelling) {
	WriteSource("project://Project.ini",
				"[Project]\nname = MyGame\n[Script]\nmodule = MyGame\nbuildCommand = ninja -C legacy\n");

	ScriptProjectOps::ScriptSettings settings = ScriptProjectOps::ReadScriptSettings(Root());
	settings.compileCommand.clear();
	ASSERT_TRUE(ScriptProjectOps::WriteScriptSettings(Root(), settings));

	str ini = Read("Project.ini");
	EXPECT_EQ(ini.find("buildCommand"), str::npos) << ini;
	EXPECT_EQ(ini.find("compileCommand"), str::npos) << ini;
	EXPECT_TRUE(ScriptProjectOps::ReadScriptSettings(Root()).compileCommand.empty());
}

TEST_F(ScriptWorkflowFixture, DerivedPathsUseOneConfigurationSpelling) {
	WriteSource("project://Project.ini", "[Project]\nname = MyGame\n[Script]\nmodule = MyGame\n");

	std::string_view config = ScriptProjectOps::GetActiveConfigName();
	EXPECT_TRUE(config == "Debug" || config == "Release");

	EXPECT_EQ(ScriptProjectOps::GetSourceDirectory(Root()), Root() + "/Source");
	EXPECT_EQ(ScriptProjectOps::GetCompileDirectory(Root(), config), Root() + "/Library/Compile/" + str(config));
	EXPECT_EQ(ScriptProjectOps::GetModuleBinaryPath(Root(), config),
			  Root() + "/Library/Binaries/" + str(config) + "/MyGame" SLT_TEST_DLL_SUFFIX);

	EXPECT_EQ(ScriptProjectOps::GetCompileDirectory(Root(), config).find("/Binaries/"), str::npos);
}

namespace {

	void StampAt(const std::filesystem::path& path, std::chrono::seconds offsetFromNow) {
		std::error_code ec;
		std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + offsetFromNow, ec);
		ASSERT_FALSE(ec) << path.string() << ": " << ec.message();
	}

} // namespace

TEST_F(ScriptWorkflowFixture, StalenessCoversBuildInputs) {
	using namespace std::chrono_literals;
	std::string_view config = ScriptProjectOps::GetActiveConfigName();

	EXPECT_FALSE(ScriptProjectOps::IsModuleBinaryOutOfDate(Root(), config));
	EXPECT_FALSE(ScriptProjectOps::IsModuleBinaryOutOfDate("", config));

	WriteSource("project://Project.ini", "[Project]\nname = MyGame\n[Script]\nmodule = MyGame\n");
	WriteSource("project://CMakeLists.txt", "project(MyGame CXX)\n");
	WriteSource("project://Source/MyGame.cpp", "// module entry point\n");
	ASSERT_TRUE(ScriptProjectOps::HasScriptProject(Root()));

	str binaryPath = ScriptProjectOps::GetModuleBinaryPath(Root(), config);
	EXPECT_TRUE(ScriptProjectOps::IsModuleBinaryOutOfDate(Root(), config));

	WriteSource(str("project://Library/Binaries/") + str(config) + "/MyGame" SLT_TEST_DLL_SUFFIX, "MZ");
	WriteSource(str("project://Library/Compile/") + str(config) + "/CMakeCache.txt",
				str("//No help, variable specified on the command line.\nSHALLTEAR_EXPECT_ENGINE_IDENTITY:UNINITIALIZED=") + CppToolchain::GetEngineIdentity() + "\n");
	std::filesystem::path binary(binaryPath);
	ASSERT_TRUE(std::filesystem::exists(binary));
	StampAt(_root / "CMakeLists.txt", -60s);
	StampAt(_root / "Source/MyGame.cpp", -60s);
	StampAt(binary, -30s);
	EXPECT_FALSE(ScriptProjectOps::IsModuleBinaryOutOfDate(Root(), config));

	StampAt(_root / "Source/MyGame.cpp", -10s);
	EXPECT_TRUE(ScriptProjectOps::IsModuleBinaryOutOfDate(Root(), config));
	StampAt(_root / "Source/MyGame.cpp", -60s);
	ASSERT_FALSE(ScriptProjectOps::IsModuleBinaryOutOfDate(Root(), config));

	StampAt(_root / "CMakeLists.txt", -10s);
	EXPECT_TRUE(ScriptProjectOps::IsModuleBinaryOutOfDate(Root(), config));
	StampAt(_root / "CMakeLists.txt", -60s);
	ASSERT_FALSE(ScriptProjectOps::IsModuleBinaryOutOfDate(Root(), config));

	WriteSource("project://Source/Gameplay/Enemies/Slime.cpp", "// nested\n");
	StampAt(_root / "Source/Gameplay/Enemies/Slime.cpp", -10s);
	EXPECT_TRUE(ScriptProjectOps::IsModuleBinaryOutOfDate(Root(), config));

	StampAt(_root / "Source/Gameplay/Enemies/Slime.cpp", -60s);
	ASSERT_FALSE(ScriptProjectOps::IsModuleBinaryOutOfDate(Root(), config));
	WriteSource(str("project://Library/Compile/") + str(config) + "/CMakeCache.txt",
				"SHALLTEAR_EXPECT_ENGINE_IDENTITY:UNINITIALIZED=0.0.0|000000000000\n");
	EXPECT_TRUE(ScriptProjectOps::IsModuleBinaryOutOfDate(Root(), config));

	std::filesystem::remove(_root / "Library/Compile" / str(config) / "CMakeCache.txt");
	EXPECT_TRUE(ScriptProjectOps::IsModuleBinaryOutOfDate(Root(), config));
}

TEST_F(ScriptWorkflowFixture, StalenessFromCompileStamp) {
	using namespace std::chrono_literals;
	std::string_view config = ScriptProjectOps::GetActiveConfigName();

	WriteSource("project://Project.ini", "[Project]\nname = MyGame\n[Script]\nmodule = MyGame\n");
	WriteSource("project://CMakeLists.txt", "project(MyGame CXX)\n");
	WriteSource("project://Source/MyGame.cpp", "// module entry point\n");
	WriteSource("project://Source/Tests/MyTest.cpp", "// a suite the module's CMakeLists.txt excludes\n");
	WriteSource(str("project://Library/Binaries/") + str(config) + "/MyGame" SLT_TEST_DLL_SUFFIX, "MZ");
	WriteSource(str("project://Library/Compile/") + str(config) + "/CMakeCache.txt",
				str("//No help, variable specified on the command line.\nSHALLTEAR_EXPECT_ENGINE_IDENTITY:UNINITIALIZED=") + CppToolchain::GetEngineIdentity() + "\n");

	std::filesystem::path binary(ScriptProjectOps::GetModuleBinaryPath(Root(), config));
	std::filesystem::path stamp(ScriptProjectOps::GetModuleCompileStampPath(Root(), config));
	EXPECT_EQ(stamp.parent_path(), binary.parent_path());

	StampAt(_root / "Source/MyGame.cpp", -60s);
	StampAt(_root / "Source/Tests/MyTest.cpp", -60s);
	StampAt(_root / "CMakeLists.txt", -60s);
	StampAt(binary, -30s);
	ASSERT_FALSE(ScriptProjectOps::IsModuleBinaryOutOfDate(Root(), config));
	StampAt(_root / "Source/Tests/MyTest.cpp", -10s);
	EXPECT_TRUE(ScriptProjectOps::IsModuleBinaryOutOfDate(Root(), config));

	WriteSource(str("project://Library/Binaries/") + str(config) + "/MyGame.compiled", "");
	ASSERT_TRUE(std::filesystem::exists(stamp));
	StampAt(stamp, -5s);
	EXPECT_FALSE(ScriptProjectOps::IsModuleBinaryOutOfDate(Root(), config));

	StampAt(_root / "Source/MyGame.cpp", -1s);
	str reason;
	EXPECT_TRUE(ScriptProjectOps::IsModuleBinaryOutOfDate(Root(), config, &reason));
	EXPECT_NE(reason.find("MyGame.cpp"), str::npos) << reason << "'";

	StampAt(_root / "Source/MyGame.cpp", -60s);
	ASSERT_FALSE(ScriptProjectOps::IsModuleBinaryOutOfDate(Root(), config));
	std::filesystem::remove(binary);
	reason.clear();
	EXPECT_TRUE(ScriptProjectOps::IsModuleBinaryOutOfDate(Root(), config, &reason));
	EXPECT_NE(reason.find("MyGame" SLT_TEST_DLL_SUFFIX), str::npos) << "got '" << reason << "'";
}

TEST_F(ScriptWorkflowFixture, CompileCacheFromElsewhereIsStale) {
	std::string_view config = ScriptProjectOps::GetActiveConfigName();
	const str root = TestPlatform::GetAbsoluteRoot();
	const str compiler = root + "Engine/External/Prebuilt/llvm-mingw/bin/x86_64-w64-mingw32-clang++" SLT_TEST_EXE_SUFFIX;
	const str compileDirectory = ScriptProjectOps::GetCompileDirectory(Root(), config);
	auto writeCache = [&](const str& recordedCompiler, const str& home, const str& cacheDirectory) {
		WriteSource(str("project://Library/Compile/") + str(config) + "/CMakeCache.txt",
					"CMAKE_CXX_COMPILER:STRING=" + recordedCompiler + "\nCMAKE_CXX_COMPILER_AR:FILEPATH=" + root + "Other/ar" SLT_TEST_EXE_SUFFIX +
						"\nCMAKE_CACHEFILE_DIR:INTERNAL=" + cacheDirectory + "\nCMAKE_HOME_DIRECTORY:INTERNAL=" + home + "\n");
	};
	auto windowsSpelling = [](const str& path) { return StringUtil::Replace(StringUtil::ToLower(path), "/", "\\"); };

	EXPECT_FALSE(ScriptProjectOps::IsCompileCacheStale(Root(), config, compiler));

	writeCache(StringUtil::Replace(compiler, "/", "\\"), windowsSpelling(Root()) + "\\", windowsSpelling(compileDirectory));
	EXPECT_FALSE(ScriptProjectOps::IsCompileCacheStale(Root(), config, compiler));

	writeCache("x86_64-w64-mingw32-clang++", Root(), compileDirectory);
	EXPECT_FALSE(ScriptProjectOps::IsCompileCacheStale(Root(), config, compiler));

	str reason;
	writeCache(StringUtil::ToUpper(compiler), Root(), compileDirectory);
	EXPECT_TRUE(ScriptProjectOps::IsCompileCacheStale(Root(), config, compiler, &reason));
	EXPECT_NE(reason.find("CMAKE_CXX_COMPILER was '" + root + "ENGINE/"), str::npos) << "got '" << reason << "'";

	reason.clear();
	writeCache(root + "OldEngine/External/Prebuilt/llvm-mingw/bin/x86_64-w64-mingw32-clang++" SLT_TEST_EXE_SUFFIX, Root(), compileDirectory);
	EXPECT_TRUE(ScriptProjectOps::IsCompileCacheStale(Root(), config, compiler, &reason));
	EXPECT_NE(reason.find("CMAKE_CXX_COMPILER was '" + root + "OldEngine/"), str::npos) << "got '" << reason << "'";

	reason.clear();
	writeCache(compiler, root + "Copied/From/Here", compileDirectory);
	EXPECT_TRUE(ScriptProjectOps::IsCompileCacheStale(Root(), config, compiler, &reason));
	EXPECT_NE(reason.find("CMAKE_HOME_DIRECTORY was '" + root + "Copied/From/Here'"), str::npos) << "got '" << reason << "'";

	reason.clear();
	writeCache(compiler, Root(), root + "Copied/From/Here/Library/Compile/Debug");
	EXPECT_TRUE(ScriptProjectOps::IsCompileCacheStale(Root(), config, compiler, &reason));
	EXPECT_NE(reason.find("CMAKE_CACHEFILE_DIR was"), str::npos) << "got '" << reason << "'";
}

TEST_F(ScriptWorkflowFixture, RenderTemplateSubstitutesAndReportsMissing) {
	Vector<ScriptProjectOps::TemplateToken> tokens{GetMemoryResource(MemTag::Editor)};
	tokens.push_back({"MODULE_NAME", "MyGame"});

	str rendered;
	ASSERT_TRUE(ScriptProjectOps::RenderTemplate("Module.cpp", tokens, rendered));
	EXPECT_NE(rendered.find("SLT_IMPLEMENT_MODULE(MyGame)"), str::npos);
	EXPECT_EQ(rendered.find("{{MODULE_NAME}}"), str::npos);

	str missing;
	EXPECT_FALSE(ScriptProjectOps::RenderTemplate("NoSuchTemplate", tokens, missing));
	EXPECT_TRUE(missing.empty());
}

TEST_F(ScriptWorkflowFixture, ScaffoldWritesExpectedSetAndIsIdempotent) {
	WriteSource("project://Project.ini", "[Project]\nname = MyGame\n");

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "MyGame";
	options.sdkRoot = "C:/Sdk";
	options.editorExecutable = "C:/Sdk/Bin/Shalltear" SLT_TEST_EXE_SUFFIX;

	ScriptProjectOps::ScaffoldResult first;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, first));

	for (const char* expected : {"CMakeLists.txt", "CMakePresets.json", "CMakeUserPresets.json",
								 ".gitignore", ".clang-format", ".clangd",
								 ".vscode/settings.json", ".vscode/launch.json", ".vscode/extensions.json",
								 "Source/MyGame.cpp", "Source/ExampleComponent.h", "Source/ExampleComponent.cpp"}) {
		EXPECT_TRUE(Exists(expected)) << expected;
	}
	EXPECT_TRUE(first.skipped.empty());
	EXPECT_TRUE(ScriptProjectOps::HasScriptProject(Root()));
	EXPECT_EQ(ScriptProjectOps::GetModuleName(Root()), "MyGame");

	str cmake = Read("CMakeLists.txt");
	EXPECT_NE(cmake.find("find_package(Shalltear"), str::npos);
	EXPECT_NE(cmake.find("shalltear_add_module(MyGame"), str::npos);

	str cmakeBefore = Read("CMakeLists.txt");
	ScriptProjectOps::ScaffoldResult second;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, second));
	EXPECT_TRUE(second.written.empty());
	EXPECT_FALSE(second.skipped.empty());
	EXPECT_EQ(Read("CMakeLists.txt"), cmakeBefore);
}

TEST_F(ScriptWorkflowFixture, GeneratedSettingsNameClangdBesideCompiler) {
	WriteSource("project://Project.ini", "[Project]\nname = MyGame\n");

	str toolchain = Root() + "/Toolchain/bin";
	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "MyGame";
	options.sdkRoot = Root() + "/SDK";
	options.editorExecutable = options.sdkRoot + "/Bin/Shalltear" SLT_TEST_EXE_SUFFIX;
	options.toolchainDirectory = toolchain;

	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));
	EXPECT_NE(Read(".vscode/settings.json").find("\"clangd.path\": \"clangd\""), str::npos) << Read(".vscode/settings.json");

	str beside = toolchain + "/clangd" SLT_TEST_EXE_SUFFIX;
	WriteStub(std::filesystem::path(beside));
	ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(beside)));

	ScriptProjectOps::ScaffoldResult regenerated;
	ASSERT_TRUE(ScriptProjectOps::RegenerateProjectFiles(Root(), options, regenerated));
	EXPECT_NE(Read(".vscode/settings.json").find(beside), str::npos) << Read(".vscode/settings.json");
}

TEST_F(ScriptWorkflowFixture, GeneratedSettingsNameEngineAssetsEditorMounted) {
	WriteSource("project://Project.ini", "[Project]\nname = MyGame\n");
	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "MyGame";
	options.sdkRoot = Root() + "/SDK";
	options.editorExecutable = Root() + "/Checkout/build/bin/Shalltear" SLT_TEST_EXE_SUFFIX;
	options.toolchainDirectory = Root() + "/Toolchain/bin";

	const str mounted = RepoDir("EngineAssets").generic_string();
	ASSERT_FALSE(mounted.empty());
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));
	str settings = Read(".vscode/settings.json");
	EXPECT_NE(settings.find(mounted + "/Schemas/Layout.xsd"), str::npos) << settings;
	EXPECT_NE(settings.find(mounted + "/Debug/libcxx.natvis"), str::npos) << settings;
	EXPECT_EQ(settings.find("build/bin/EngineAssets"), str::npos) << settings;
}

TEST_F(ScriptWorkflowFixture, GeneratedSettingsNameCMakeEditorResolved) {
	WriteSource("project://Project.ini", "[Project]\nname = MyGame\n");

	str toolchain = Root() + "/Toolchain/bin";
	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "MyGame";
	options.sdkRoot = Root() + "/SDK";
	options.editorExecutable = options.sdkRoot + "/Bin/Shalltear" SLT_TEST_EXE_SUFFIX;
	options.toolchainDirectory = toolchain;

	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));
	EXPECT_NE(Read(".vscode/settings.json").find("\"cmake.cmakePath\": \"\""), str::npos) << Read(".vscode/settings.json");

	str beside = toolchain + "/cmake" SLT_TEST_EXE_SUFFIX;
	WriteStub(std::filesystem::path(beside));
	ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(beside)));

	ScriptProjectOps::ScaffoldResult regenerated;
	ASSERT_TRUE(ScriptProjectOps::RegenerateProjectFiles(Root(), options, regenerated));
	EXPECT_NE(Read(".vscode/settings.json").find("\"cmake.cmakePath\": \"" + beside + "\""), str::npos) << Read(".vscode/settings.json");
}

TEST_F(ScriptWorkflowFixture, RegenerateReportsResult) {
	WriteSource("project://Project.ini", "[Project]\nname = MyGame\n");
	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "MyGame";
	options.sdkRoot = Root() + "/SDK";
	options.editorExecutable = options.sdkRoot + "/Bin/Shalltear" SLT_TEST_EXE_SUFFIX;
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));

	JobSystem::Initialize();
	LogManager::Initialize({false, false});
	LogManager::SetGlobalLevel(LogLevel::Trace);
	auto sink = std::make_shared<CapturingLogSink>();
	LogManager::AddSink(sink);
	constexpr std::string_view Sentinel = "RegeneratingProjectFilesSaysWhatItDid begins";
	SLT_LOG_INFO("Test", "{}", Sentinel);

	options.sdkRoot = Root() + "/OtherSDK";
	ScriptProjectOps::ScaffoldResult rewrote;
	ASSERT_TRUE(ScriptProjectOps::RegenerateProjectFiles(Root(), options, rewrote));
	ScriptProjectOps::ScaffoldResult unchanged;
	ASSERT_TRUE(ScriptProjectOps::RegenerateProjectFiles(Root(), options, unchanged));

	LogManager::Terminate();
	JobSystem::Terminate();

	ASSERT_FALSE(rewrote.written.empty());
	ASSERT_FALSE(sink->MatchingAfter(Sentinel, "").empty());
	Vector<str> rewroteLines = sink->MatchingAfter(Sentinel, "regenerated ");
	ASSERT_EQ(rewroteLines.size(), 1u);
	for (const str& file : rewrote.written) {
		EXPECT_NE(rewroteLines[0].find(file), str::npos) << file << ": " << rewroteLines[0];
	}

	ASSERT_TRUE(unchanged.written.empty());
	EXPECT_EQ(sink->MatchingAfter(Sentinel, "already up to date").size(), 1u);
}

TEST_F(ScriptWorkflowFixture, CMakeListsCarriesNoEngineFact) {
	WriteSource("project://Project.ini", "[Project]\nname = MyGame\n");

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "MyGame";
	ScriptProjectOps::ScaffoldResult result;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, result));

	str cmake = Read("CMakeLists.txt");

	EXPECT_NE(cmake.find("find_package(Shalltear REQUIRED)"), str::npos) << cmake;

	EXPECT_EQ(cmake.find(str("Shalltear ") + BuildInfo::GetVersionString()), str::npos) << cmake;
	EXPECT_EQ(cmake.find(BuildInfo::GetGitCommit()), str::npos) << cmake;
}

TEST_F(ScriptWorkflowFixture, ScaffoldNeverOverwritesAUserFile) {
	WriteSource("project://Project.ini", "[Project]\nname = MyGame\n");
	WriteSource("project://CMakeLists.txt", "# mine, hand-written\n");

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "MyGame";
	ScriptProjectOps::ScaffoldResult result;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, result));

	str cmake = Read("CMakeLists.txt");
	EXPECT_NE(cmake.find("# mine, hand-written"), str::npos);
	EXPECT_EQ(cmake.find("find_package(Shalltear"), str::npos);
	EXPECT_NE(std::find(result.skipped.begin(), result.skipped.end(), str("CMakeLists.txt")), result.skipped.end());
}

TEST_F(ScriptWorkflowFixture, GeneratedPresetsNamesTheResolvedToolchainDirectory) {
	WriteSource("project://Project.ini", "[Project]\nname = MyGame\n");

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "MyGame";
	options.sdkRoot = "C:/Sdk";
	options.toolchainDirectory = "C:/Somewhere/llvm-mingw/bin";
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));

	str presets = Read("CMakeUserPresets.json");
	const str separator(1, TestPlatform::GetSearchPathSeparator());
	EXPECT_NE(presets.find("\"PATH\": \"C:/Somewhere/llvm-mingw/bin" + separator + "$penv{PATH}\""), str::npos) << presets;
	EXPECT_EQ(presets.find("/Toolchain/LLVM/bin"), str::npos) << presets;
}

TEST_F(ScriptWorkflowFixture, PresetsKeepPathWithoutToolchain) {
	if (!TestPlatform::ToolDirectoriesFollowEnvironment()) {
		GTEST_SKIP() << "this host's tool directories are fixed";
	}
	WriteSource("project://Project.ini", "[Project]\nname = MyGame\n");

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "MyGame";
	options.sdkRoot = Root();
	options.toolchainDirectory = "";
	ScriptProjectOps::ScaffoldResult scaffolded;

	str presets;
	{
		ScopedPath scoped("");
		ScopedEnvironment programFiles("ProgramFiles", (_root / "empty-bin").generic_string());
		ScopedEnvironment programFilesX86("ProgramFiles(x86)", (_root / "empty-bin").generic_string());
		ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));
		presets = Read("CMakeUserPresets.json");
	}

	EXPECT_NE(presets.find("\"PATH\": \"$penv{PATH}\""), str::npos) << presets;
	EXPECT_EQ(presets.find(";$penv{PATH}"), str::npos) << presets;
}

TEST_F(ScriptWorkflowFixture, RegenerateTouchesOnlyEngineOwnedSet) {
	WriteSource("project://Project.ini", "[Project]\nname = MyGame\n");

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "MyGame";
	options.sdkRoot = "C:/Sdk";
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));

	str cmakeBefore = Read("CMakeLists.txt");
	str gitignoreBefore = Read(".gitignore");
	str componentBefore = Read("Source/ExampleComponent.h");

	options.sdkRoot = "D:/OtherSdk";
	ScriptProjectOps::ScaffoldResult regenerated;
	ASSERT_TRUE(ScriptProjectOps::RegenerateProjectFiles(Root(), options, regenerated));

	EXPECT_EQ(Read("CMakeLists.txt"), cmakeBefore);
	EXPECT_EQ(Read(".gitignore"), gitignoreBefore);
	EXPECT_EQ(Read("Source/ExampleComponent.h"), componentBefore);
	EXPECT_NE(Read("CMakeUserPresets.json").find("D:/OtherSdk"), str::npos);

	for (const str& written : regenerated.written) {
		EXPECT_TRUE(written == "CMakePresets.json" || written == "CMakeUserPresets.json" ||
					written == ".clangd" || written.starts_with(".vscode/"))
			<< written;
	}
	EXPECT_NE(std::find(regenerated.skipped.begin(), regenerated.skipped.end(), str("CMakePresets.json")),
			  regenerated.skipped.end());
}

TEST_F(ScriptWorkflowFixture, RegenerationHealsProjectRenderedFromOlderTemplate) {
	WriteSource("project://Project.ini", "[Project]\nname = MyGame\n");

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "MyGame";
	options.sdkRoot = "C:/Sdk";
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));
	ASSERT_FALSE(ScriptProjectOps::NeedsProjectFileRegeneration(Root(), options));

	str current = Read("CMakePresets.json");
	WriteSource("project://CMakePresets.json", "{ \"version\": 3, \"configurePresets\": [] }\n");
	EXPECT_TRUE(ScriptProjectOps::NeedsProjectFileRegeneration(Root(), options));

	ScriptProjectOps::ScaffoldResult regenerated;
	ASSERT_TRUE(ScriptProjectOps::RegenerateProjectFiles(Root(), options, regenerated));
	EXPECT_EQ(Read("CMakePresets.json"), current);
	EXPECT_FALSE(ScriptProjectOps::NeedsProjectFileRegeneration(Root(), options));
}

TEST_F(ScriptWorkflowFixture, NeedsRegenerationDetectsAFreshClone) {
	WriteSource("project://Project.ini", "[Project]\nname = MyGame\n");

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "MyGame";
	options.sdkRoot = "C:/Sdk";
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));
	EXPECT_FALSE(ScriptProjectOps::NeedsProjectFileRegeneration(Root(), options));

	std::error_code ec;
	std::filesystem::remove(_root / "CMakeUserPresets.json", ec);
	std::filesystem::remove_all(_root / ".vscode", ec);
	std::filesystem::remove(_root / ".clangd", ec);
	EXPECT_TRUE(ScriptProjectOps::NeedsProjectFileRegeneration(Root(), options));

	ScriptProjectOps::ScaffoldResult regenerated;
	ASSERT_TRUE(ScriptProjectOps::RegenerateProjectFiles(Root(), options, regenerated));
	EXPECT_FALSE(ScriptProjectOps::NeedsProjectFileRegeneration(Root(), options));

	ScriptProjectOps::ScaffoldOptions elsewhere = options;
	elsewhere.sdkRoot = "D:/OtherSdk";
	EXPECT_TRUE(ScriptProjectOps::NeedsProjectFileRegeneration(Root(), elsewhere));
}

TEST_F(ScriptWorkflowFixture, PresetsCarryLoadedPackages) {
	WriteSource("project://Project.ini", "[Project]\nname = MyGame\n");

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "MyGame";
	options.sdkRoot = "C:/Sdk";
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));
	EXPECT_EQ(Read("CMakeUserPresets.json").find("\"SHALLTEAR_ENABLED_PACKAGES\":"), str::npos)
		<< Read("CMakeUserPresets.json");

	options.enabledPackages = Vector<str>{str("dev.shalltear.audio"), str("dev.shalltear.physics3d")};
	EXPECT_TRUE(ScriptProjectOps::NeedsProjectFileRegeneration(Root(), options));
	ScriptProjectOps::ScaffoldResult regenerated;
	ASSERT_TRUE(ScriptProjectOps::RegenerateProjectFiles(Root(), options, regenerated));
	const str presets = Read("CMakeUserPresets.json");
	EXPECT_NE(presets.find("\"cacheVariables\": { \"Shalltear_ROOT\": \"C:/Sdk\", \"CMAKE_CXX_COMPILER\": \"x86_64-w64-mingw32-clang++\", "
						   "\"SHALLTEAR_ENABLED_PACKAGES\": \"dev.shalltear.audio;dev.shalltear.physics3d\" }"),
			  str::npos)
		<< presets;
	EXPECT_FALSE(ScriptProjectOps::NeedsProjectFileRegeneration(Root(), options));

	options.enabledPackages = Vector<str>{str("dev.shalltear.audio")};
	EXPECT_TRUE(ScriptProjectOps::NeedsProjectFileRegeneration(Root(), options));

	options.enabledPackages = Vector<str>{};
	ASSERT_TRUE(ScriptProjectOps::RegenerateProjectFiles(Root(), options, regenerated));
	EXPECT_NE(Read("CMakeUserPresets.json").find("\"SHALLTEAR_ENABLED_PACKAGES\": \"\""), str::npos)
		<< Read("CMakeUserPresets.json");
}

TEST_F(ScriptWorkflowFixture, UserPresetsCarrySDKToolchainFile) {
	WriteSource("project://Project.ini", "[Project]\nname = MyGame\n");
	WriteSource("project://Sdk/ShalltearConfig.cmake",
				"set(SHALLTEAR_TOOLCHAIN_FILE \"${SHALLTEAR_PACKAGE_ROOT}/Toolchains/Linux-x64.cmake\")\n");

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "MyGame";
	options.sdkRoot = Root() + "/Sdk";
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));

	const str presets = Read("CMakeUserPresets.json");
	EXPECT_NE(presets.find("\"CMAKE_CXX_COMPILER\": \"clang++\", \"CMAKE_TOOLCHAIN_FILE\": \"" + Root() + "/Sdk/Toolchains/Linux-x64.cmake\""),
			  str::npos)
		<< presets;
	EXPECT_EQ(Read("CMakePresets.json").find("CMAKE_CXX_COMPILER"), str::npos);
}

TEST_F(ScriptWorkflowFixture, ContentOnlyProjectHasNoScriptProject) {
	WriteSource("project://Project.ini", "[Project]\nname = MyGame\n");
	WriteSource("project://Assets/Scenes/Main.scene", "{}");
	EXPECT_FALSE(ScriptProjectOps::HasScriptProject(Root()));
	ScriptProjectOps::ScaffoldOptions options;
	options.sdkRoot = "C:/Sdk";
	EXPECT_FALSE(ScriptProjectOps::NeedsProjectFileRegeneration(Root(), options));
}

TEST_F(ScriptWorkflowFixture, DetectsVsCodeCommands) {
	str variable;
	const str staged = TestPlatform::StageExternalEditor(_root.generic_string(), variable);
	const str previous = TestPlatform::GetEnvironment(variable.c_str());
	TestPlatform::SetEnvironment(variable.c_str(), staged);

	ScriptProjectOps::ExternalEditor editor = ScriptProjectOps::DetectExternalEditor();
	TestPlatform::SetEnvironment(variable.c_str(), previous);

	EXPECT_EQ(editor.name, "VS Code");

	EXPECT_NE(editor.fileCommand.find("--goto"), str::npos);
	EXPECT_NE(editor.fileCommand.find("{file}:{line}:{column}"), str::npos);

	EXPECT_NE(editor.folderCommand.find("{folder}"), str::npos);
	EXPECT_EQ(editor.folderCommand.find("--goto"), str::npos);
	EXPECT_EQ(editor.folderCommand.find("{file}"), str::npos);
	EXPECT_EQ(editor.folderCommand.find("{line}"), str::npos);
}

TEST_F(ScriptWorkflowFixture, LaunchStripsVariablesThatRedefineChildProgram) {
	std::filesystem::path receipt = _root / "child-saw.txt";
	str unset;
	str command = TestPlatform::GetEnvironmentProbeCommand("ELECTRON_RUN_AS_NODE", receipt.generic_string(), unset);

	TestPlatform::SetEnvironment("ELECTRON_RUN_AS_NODE", "1");
	EXPECT_TRUE(ScriptProjectOps::OpenInExternalEditor(Root(), command, {}, 0, 0));

	str seen = Read("child-saw.txt");
	for (int attempt = 0; attempt < 100 && seen.find('\n') == str::npos; ++attempt) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		seen = Read("child-saw.txt");
	}
	TestPlatform::SetEnvironment("ELECTRON_RUN_AS_NODE", "");

	ASSERT_NE(seen.find('\n'), str::npos) << seen;
	EXPECT_EQ(seen.find("[1]"), str::npos) << seen;
	EXPECT_NE(seen.find(unset), str::npos) << seen;
}

TEST_F(ScriptWorkflowFixture, LaunchCarriesContributedVariables) {
	static char owner = 0;
	std::filesystem::path receipt = _root / "child-saw.txt";
	str unset;
	const str command = TestPlatform::GetEnvironmentProbeCommand("SLT_TEST_EDITOR_TOOLCHAIN", receipt.generic_string(), unset);

	ChildEnvironment::Set("SLT_TEST_EDITOR_TOOLCHAIN", "pinned", &owner);
	EXPECT_TRUE(ScriptProjectOps::OpenInExternalEditor(Root(), command, {}, 0, 0));
	EXPECT_EQ(ChildEnvironment::UnregisterOwnedBy(&owner), 1u);

	str seen = Read("child-saw.txt");
	for (int attempt = 0; attempt < 100 && seen.find('\n') == str::npos; ++attempt) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		seen = Read("child-saw.txt");
	}
	EXPECT_NE(seen.find("[pinned]"), str::npos) << seen;
}

TEST(ScriptClassNameTest, ValidatesCppIdentifiers) {
	EXPECT_TRUE(ScriptProjectOps::IsValidClassName("CharacterController"));
	EXPECT_TRUE(ScriptProjectOps::IsValidClassName("_Private2"));

	EXPECT_FALSE(ScriptProjectOps::IsValidClassName(""));
	EXPECT_FALSE(ScriptProjectOps::IsValidClassName("2Fast"));
	EXPECT_FALSE(ScriptProjectOps::IsValidClassName("Has Space"));
	EXPECT_FALSE(ScriptProjectOps::IsValidClassName("Has-Dash"));
	EXPECT_FALSE(ScriptProjectOps::IsValidClassName("class"));
}

TEST_F(ScriptWorkflowFixture, CreateComponentWritesPairAndRefusesCollision) {
	WriteSource("project://Project.ini", "[Project]\nname = MyGame\n[Script]\nmodule = MyGame\n");

	ScriptProjectOps::ScaffoldResult created;
	ASSERT_TRUE(ScriptProjectOps::CreateComponentSource(Root(), "Gameplay", "CharacterController", created));
	ASSERT_EQ(created.written.size(), 2u);
	EXPECT_EQ(created.written[0], "Source/Gameplay/CharacterController.h");
	EXPECT_EQ(created.written[1], "Source/Gameplay/CharacterController.cpp");

	str header = Read("Source/Gameplay/CharacterController.h");
	EXPECT_NE(header.find("class CharacterController : public Behavior"), str::npos);
	EXPECT_NE(header.find("SLT_REFLECT_COMPONENT(CharacterController"), str::npos);
	EXPECT_NE(header.find("Handle"), str::npos);

	EXPECT_NE(header.find("#include \"Shalltear.h\""), str::npos) << header;
	EXPECT_EQ(header.find("Shalltear::"), str::npos);
	EXPECT_EQ(header.find("Shalltear/Core/"), str::npos);

	str source = Read("Source/Gameplay/CharacterController.cpp");
	EXPECT_EQ(source.find("using namespace Shalltear"), str::npos) << source;

	ScriptProjectOps::ScaffoldResult collision;
	EXPECT_FALSE(ScriptProjectOps::CreateComponentSource(Root(), "Gameplay", "CharacterController", collision));
	EXPECT_TRUE(collision.written.empty());

	ScriptProjectOps::ScaffoldResult escaped;
	EXPECT_FALSE(ScriptProjectOps::CreateComponentSource(Root(), "../../Evil", "Escapee", escaped));
	EXPECT_FALSE(std::filesystem::exists(_root.parent_path().parent_path() / "Evil"));
}

namespace {

	CompilerDiagnostic Parse(std::string_view line) {
		CompilerDiagnostic diagnostic;
		EXPECT_TRUE(CompilerDiagnosticParser::ParseDiagnosticLine(line, diagnostic)) << line;
		return diagnostic;
	}

} // namespace

TEST(CompilerDiagnosticsTest, ParsesClangFileLineColumnForm) {
	CompilerDiagnostic error = Parse("C:/Games/MyGame/Source/CharacterController.cpp:41:9: error: expected ';' after expression");
	EXPECT_EQ(error.severity, CompilerDiagnosticSeverity::Error);
	EXPECT_EQ(error.file, "C:/Games/MyGame/Source/CharacterController.cpp");
	EXPECT_EQ(error.line, 41u);
	EXPECT_EQ(error.column, 9u);
	EXPECT_EQ(error.message, "expected ';' after expression");

	CompilerDiagnostic warning = Parse("/home/dev/Game/Source/Player.h:7:12: warning: unused variable 'x' [-Wunused-variable]");
	EXPECT_EQ(warning.severity, CompilerDiagnosticSeverity::Warning);
	EXPECT_EQ(warning.file, "/home/dev/Game/Source/Player.h");
	EXPECT_EQ(warning.line, 7u);
	EXPECT_EQ(warning.column, 12u);

	CompilerDiagnostic note = Parse("C:/Games/MyGame/Source/Player.h:3:7: note: previous definition is here");
	EXPECT_EQ(note.severity, CompilerDiagnosticSeverity::Note);
}

TEST(CompilerDiagnosticsTest, ParsesFatalErrorWithoutAColumn) {
	CompilerDiagnostic fatal = Parse("C:/Games/MyGame/Source/MyGame.cpp:3: fatal error: 'Shalltear/Core/Nope.h' file not found");
	EXPECT_EQ(fatal.severity, CompilerDiagnosticSeverity::Error);
	EXPECT_EQ(fatal.file, "C:/Games/MyGame/Source/MyGame.cpp");
	EXPECT_EQ(fatal.line, 3u);
	EXPECT_EQ(fatal.column, 0u);
}

TEST(CompilerDiagnosticsTest, ParsesLocationlessToolDiagnostics) {
	CompilerDiagnostic link = Parse("lld-link: error: undefined symbol: public: void __cdecl CharacterController::Missing(void)");
	EXPECT_EQ(link.severity, CompilerDiagnosticSeverity::Error);
	EXPECT_TRUE(link.file.empty());
	EXPECT_NE(link.message.find("undefined symbol"), str::npos);

	CompilerDiagnostic driver = Parse("clang++: error: no such file or directory: 'Missing.cpp'");
	EXPECT_EQ(driver.severity, CompilerDiagnosticSeverity::Error);
	EXPECT_TRUE(driver.file.empty());

	CompilerDiagnostic gnuLink = Parse("ld.lld: error: undefined symbol: CharacterController::Missing()");
	EXPECT_EQ(gnuLink.severity, CompilerDiagnosticSeverity::Error);
	EXPECT_TRUE(gnuLink.file.empty());
	EXPECT_NE(gnuLink.message.find("undefined symbol"), str::npos);
}

TEST(CompilerDiagnosticsTest, ParsesCMakeErrors) {
	CompilerDiagnostic cmake = Parse("CMake Error at C:/Games/MyGame/CMakeLists.txt:8 (shalltear_add_module):");
	EXPECT_EQ(cmake.severity, CompilerDiagnosticSeverity::Error);
	EXPECT_EQ(cmake.file, "C:/Games/MyGame/CMakeLists.txt");
	EXPECT_EQ(cmake.line, 8u);

	CompilerDiagnostic warning = Parse("CMake Warning at C:/Games/MyGame/CMakeLists.txt:2 (project):");
	EXPECT_EQ(warning.severity, CompilerDiagnosticSeverity::Warning);
}

namespace {

	size_t FeedBlock(const std::vector<std::string_view>& lines, CompilerDiagnostic& out) {
		EXPECT_TRUE(CompilerDiagnosticParser::ParseDiagnosticLine(lines[0], out)) << lines[0];
		size_t taken = 1;
		while (taken < lines.size() && CompilerDiagnosticParser::AppendContinuation(out, lines[taken])) {
			++taken;
		}
		return taken;
	}

} // namespace

TEST(CompilerDiagnosticsTest, CMakeMessageRejoined) {
	CompilerDiagnostic warning;
	const size_t taken = FeedBlock({"CMake Warning at CMakeLists.txt:82 (message):",
									"  KlotzchenprufstandTests is not built: no googletest at",
									"  'C:/some/long/path/External/Submodules/googletest' (set KLOTZ_GTEST_DIR)",
									"",
									"",
									"-- Configuring done (0.5s)"},
								   warning);
	EXPECT_EQ(taken, 5u);
	EXPECT_EQ(warning.severity, CompilerDiagnosticSeverity::Warning);
	EXPECT_EQ(warning.file, "CMakeLists.txt");
	EXPECT_EQ(warning.line, 82u);
	EXPECT_EQ(warning.message, "CMake Warning: KlotzchenprufstandTests is not built: no googletest at "
							   "'C:/some/long/path/External/Submodules/googletest' (set KLOTZ_GTEST_DIR)");
	EXPECT_NE(warning.raw.find("(set KLOTZ_GTEST_DIR)"), str::npos);
}

TEST(CompilerDiagnosticsTest, CMakeErrorKeepsLines) {
	CompilerDiagnostic error;
	const size_t taken = FeedBlock(
		{"CMake Error at C:/Users/Some One/Shalltear/build/ShalltearToolchainGuard.cmake:61 (message):",
		 "  Shalltear: 'DrunkenWrestlers' is built for x86_64-pc-windows-msvc, but",
		 "  SDK was built for x86_64-w64-windows-gnu. A module exchanges C++ objects",
		 "  with the engine (the two must target the same ABI) -> a mismatch here",
		 "  LOADS then misbehaves instead of failing.",
		 "",
		 "    Fix: configure with the engine's compiler (the one whose -dumpmachine says x86_64-w64-windows-gnu).",
		 "Call Stack (most recent call first):",
		 "  C:/Users/Some One/Shalltear/build/ShalltearModule.cmake:34 (shalltear_enforce_toolchain)",
		 "  CMakeLists.txt:42 (shalltear_add_module)",
		 "",
		 "",
		 "-- Configuring incomplete, errors occurred!"},
		error);
	EXPECT_EQ(taken, 12u);
	EXPECT_EQ(error.severity, CompilerDiagnosticSeverity::Error);
	EXPECT_EQ(error.file, "C:/Users/Some One/Shalltear/build/ShalltearToolchainGuard.cmake");
	EXPECT_EQ(error.line, 61u);
	EXPECT_EQ(error.message,
			  "CMake Error: Shalltear: 'DrunkenWrestlers' is built for x86_64-pc-windows-msvc, but SDK was "
			  "built for x86_64-w64-windows-gnu. A module exchanges C++ objects with the engine (the two must target "
			  "the same ABI) -> a mismatch here LOADS then misbehaves instead of failing.\n"
			  "  Fix: configure with the engine's compiler (the one whose -dumpmachine says x86_64-w64-windows-gnu).\n"
			  "Call Stack (most recent call first):\n"
			  "C:/Users/Some One/Shalltear/build/ShalltearModule.cmake:34 (shalltear_enforce_toolchain)\n"
			  "CMakeLists.txt:42 (shalltear_add_module)");
}

TEST(CompilerDiagnosticsTest, CMakeMessageFormsParsed) {
	CompilerDiagnostic dev;
	EXPECT_EQ(FeedBlock({"CMake Warning (dev) at CMakeLists.txt:4 (message):", "  an author warning",
						 "This warning is for project developers.  Use -Wno-dev to suppress it.", "",
						 "CMake Deprecation Warning at CMakeLists.txt:5 (message):"},
						dev),
			  4u);
	EXPECT_EQ(dev.severity, CompilerDiagnosticSeverity::Warning);
	EXPECT_EQ(dev.line, 4u);
	EXPECT_EQ(dev.message, "CMake Warning (dev): an author warning\n"
						   "This warning is for project developers.  Use -Wno-dev to suppress it.");

	CompilerDiagnostic deprecation;
	FeedBlock({"CMake Deprecation Warning at CMakeLists.txt:5 (message):", "  a deprecation", "", ""}, deprecation);
	EXPECT_EQ(deprecation.severity, CompilerDiagnosticSeverity::Warning);
	EXPECT_EQ(deprecation.file, "CMakeLists.txt");
	EXPECT_EQ(deprecation.message, "CMake Deprecation Warning: a deprecation");

	CompilerDiagnostic unused;
	FeedBlock({"CMake Warning:", "  Manually-specified variables were not used by the project:", "",
			   "    SHALLTEAR_EXPECT_ENGINE_IDENTITY", "", "", "-- Build files have been written to: C:/x"},
			  unused);
	EXPECT_TRUE(unused.file.empty());
	EXPECT_EQ(unused.message,
			  "CMake Warning: Manually-specified variables were not used by the project:\n  SHALLTEAR_EXPECT_ENGINE_IDENTITY");

	CompilerDiagnostic oneLine = Parse("CMake Error: Could not create named generator Ninja2");
	EXPECT_EQ(oneLine.severity, CompilerDiagnosticSeverity::Error);
	EXPECT_EQ(oneLine.message, "CMake Error: Could not create named generator Ninja2");
}

TEST(CompilerDiagnosticsTest, AClangDiagnosticTakesNoContinuation) {
	CompilerDiagnostic error = Parse("C:/Games/MyGame/Source/Player.cpp:12:5: error: use of undeclared identifier 'x'");
	EXPECT_FALSE(error.hasBody);
	EXPECT_FALSE(CompilerDiagnosticParser::AppendContinuation(error, "   12 |     x = 1;"));
	EXPECT_EQ(error.message, "use of undeclared identifier 'x'");
}

TEST(CompilerDiagnosticsTest, IgnoresOrdinaryProgressOutput) {
	CompilerDiagnostic ignored;
	for (const char* line : {"[1/3] Building CXX object CMakeFiles/MyGame.dir/Source/MyGame.cpp.obj",
							 "-- Configuring done (0.4s)",
							 "ninja: build stopped: subcommand failed.",
							 "",
							 "   "}) {
		EXPECT_FALSE(CompilerDiagnosticParser::ParseDiagnosticLine(line, ignored)) << line;
	}
}

TEST(CompilerDiagnosticsTest, StripsAnsiBeforeParsing) {
	str colored = "\x1b[0;1mC:/Games/MyGame/Source/Player.cpp:12:5: \x1b[0;1;31merror: \x1b[0m\x1b[1muse of undeclared identifier 'foo'\x1b[0m";
	EXPECT_EQ(CompilerDiagnosticParser::StripAnsi(colored),
			  "C:/Games/MyGame/Source/Player.cpp:12:5: error: use of undeclared identifier 'foo'");

	CompilerDiagnostic diagnostic = Parse(colored);
	EXPECT_EQ(diagnostic.file, "C:/Games/MyGame/Source/Player.cpp");
	EXPECT_EQ(diagnostic.line, 12u);
	EXPECT_EQ(diagnostic.column, 5u);
	EXPECT_EQ(diagnostic.message, "use of undeclared identifier 'foo'");
}

TEST_F(ScriptWorkflowFixture, ToolchainDiscoveryReportsMissingAndPresentTools) {
	std::filesystem::create_directories(_root / "fake-bin");
	for (const char* tool : {"cmake" SLT_TEST_EXE_SUFFIX, "ninja" SLT_TEST_EXE_SUFFIX}) {
		std::ofstream stub(_root / "fake-bin" / tool, std::ios::binary);
		stub << "stub";
	}

	CppToolchain::DiscoveryOptions options;
	options.cmakePath = (_root / "fake-bin" / "cmake" SLT_TEST_EXE_SUFFIX).generic_string();
	options.ninjaPath = (_root / "fake-bin" / "ninja" SLT_TEST_EXE_SUFFIX).generic_string();
	options.extraSearchDirectories.push_back((_root / "fake-bin").generic_string());
	options.sdkRoot = Root();
	options.probeVersions = false;

	CppToolchain::ToolchainInfo info = CppToolchain::Discover(options);
	EXPECT_EQ(info.cmake.status, CppToolchain::ToolStatus::Found);
	EXPECT_EQ(info.ninja.status, CppToolchain::ToolStatus::Found);
	EXPECT_EQ(info.cmake.path, options.cmakePath);

	str described = CppToolchain::Describe(info);
	EXPECT_NE(described.find("cmake"), str::npos);
	EXPECT_EQ(described.find("Major versions must match"), str::npos);
}

TEST_F(ScriptWorkflowFixture, ToolchainDescribesMissingToolWithFix) {
	CppToolchain::DiscoveryOptions options;
	options.extraSearchDirectories.push_back((_root / "empty-bin").generic_string());
	options.sdkRoot = Root();
	options.probeVersions = false;

	CppToolchain::ToolchainInfo info;
	info.cmake.name = "cmake";
	info.ninja.name = "ninja";
	info.compiler.name = "clang++";
	EXPECT_FALSE(info.IsComplete());

	str described = CppToolchain::Describe(info);
	EXPECT_NE(described.find("NOT FOUND"), str::npos);
	EXPECT_NE(described.find("install CMake"), str::npos);
	EXPECT_NE(described.find("install Ninja"), str::npos);
	EXPECT_NE(described.find("install LLVM/Clang"), str::npos);
	EXPECT_NE(described.find("Settings"), str::npos);
	EXPECT_NE(described.find("Scripting"), str::npos);
}

TEST_F(ScriptWorkflowFixture, ToolchainPrefersTheBundledToolchainOverPATH) {
	const std::filesystem::path bundle = _root / "fake-sdk" / "Toolchain";
	WriteStub(bundle / "LLVM" / "bin" / "x86_64-w64-mingw32-clang++" SLT_TEST_EXE_SUFFIX);
	WriteStub(bundle / "CMake" / "bin" / "cmake" SLT_TEST_EXE_SUFFIX);
	WriteStub(bundle / "Ninja" / "ninja" SLT_TEST_EXE_SUFFIX);

	const std::filesystem::path onPath = _root / "path-bin";
	WriteStub(onPath / "clang++" SLT_TEST_EXE_SUFFIX);
	WriteStub(onPath / "cmake" SLT_TEST_EXE_SUFFIX);
	WriteStub(onPath / "ninja" SLT_TEST_EXE_SUFFIX);

	CppToolchain::DiscoveryOptions options;
	options.sdkRoot = (_root / "fake-sdk").generic_string();
	options.probeVersions = false;

	CppToolchain::ToolchainInfo info;
	{
		ScopedPath scoped(onPath.generic_string());
		info = CppToolchain::Discover(options);
	}

	EXPECT_EQ(info.compiler.path, (bundle / "LLVM" / "bin" / "x86_64-w64-mingw32-clang++" SLT_TEST_EXE_SUFFIX).generic_string());
	EXPECT_EQ(info.cmake.path, (bundle / "CMake" / "bin" / "cmake" SLT_TEST_EXE_SUFFIX).generic_string());
	EXPECT_EQ(info.ninja.path, (bundle / "Ninja" / "ninja" SLT_TEST_EXE_SUFFIX).generic_string());
}

TEST_F(ScriptWorkflowFixture, ToolchainFindsNestedBundle) {
	const std::filesystem::path install = _root / "fake-sdk";
	const std::filesystem::path bundle = install / "Toolchain";
	WriteStub(bundle / "LLVM" / "bin" / "x86_64-w64-mingw32-clang++" SLT_TEST_EXE_SUFFIX);
	WriteStub(bundle / "CMake" / "bin" / "cmake" SLT_TEST_EXE_SUFFIX);
	WriteStub(bundle / "Ninja" / "ninja" SLT_TEST_EXE_SUFFIX);

	const std::filesystem::path flavor = install / "Static";
	std::filesystem::create_directories(flavor / "Lib" / "cmake" / "Shalltear");
	{
		std::ofstream config(flavor / "Lib" / "cmake" / "Shalltear" / "ShalltearConfig.cmake", std::ios::binary);
		config << "set(SHALLTEAR_COMPILER_ID \"Clang\")\n"
			   << "set(SHALLTEAR_LINK_MODE \"STATIC\")\n";
	}

	CppToolchain::DiscoveryOptions options;
	options.sdkRoot = flavor.generic_string();
	options.extraSearchDirectories.push_back((_root / "empty-bin").generic_string());
	options.probeVersions = false;

	CppToolchain::ToolchainInfo info;
	{
		ScopedPath scoped("");
		info = CppToolchain::Discover(options);
	}

	EXPECT_EQ(info.compiler.path, (bundle / "LLVM" / "bin" / "x86_64-w64-mingw32-clang++" SLT_TEST_EXE_SUFFIX).generic_string());
	EXPECT_EQ(info.cmake.path, (bundle / "CMake" / "bin" / "cmake" SLT_TEST_EXE_SUFFIX).generic_string());
	EXPECT_EQ(info.ninja.path, (bundle / "Ninja" / "ninja" SLT_TEST_EXE_SUFFIX).generic_string());
	EXPECT_TRUE(info.IsComplete());

	EXPECT_EQ(info.sdkRoot, flavor.generic_string());
	EXPECT_EQ(info.sdkLinkMode, "STATIC");
}

TEST_F(ScriptWorkflowFixture, ToolchainFallsBackToPath) {
	const std::filesystem::path onPath = _root / "path-bin";
	WriteStub(onPath / "clang++" SLT_TEST_EXE_SUFFIX);
	WriteStub(onPath / "cmake" SLT_TEST_EXE_SUFFIX);
	WriteStub(onPath / "ninja" SLT_TEST_EXE_SUFFIX);

	CppToolchain::DiscoveryOptions options;
	options.sdkRoot = Root();
	options.probeVersions = false;

	CppToolchain::ToolchainInfo info;
	{
		ScopedPath scoped(onPath.generic_string());
		info = CppToolchain::Discover(options);
	}

	EXPECT_EQ(info.compiler.path, (onPath / "clang++" SLT_TEST_EXE_SUFFIX).generic_string());
	EXPECT_EQ(info.cmake.path, (onPath / "cmake" SLT_TEST_EXE_SUFFIX).generic_string());
	EXPECT_EQ(info.ninja.path, (onPath / "ninja" SLT_TEST_EXE_SUFFIX).generic_string());
}

TEST_F(ScriptWorkflowFixture, ToolchainPrefersRecordedCompiler) {
	const std::filesystem::path recorded = _root / "recorded-bin";
	WriteStub(recorded / "x86_64-w64-mingw32-clang++" SLT_TEST_EXE_SUFFIX);

	const std::filesystem::path onPath = _root / "path-bin";
	WriteStub(onPath / "clang++" SLT_TEST_EXE_SUFFIX);
	WriteStub(onPath / "cmake" SLT_TEST_EXE_SUFFIX);
	WriteStub(onPath / "ninja" SLT_TEST_EXE_SUFFIX);

	WriteSource("project://ShalltearConfig.cmake",
				"set(SHALLTEAR_COMPILER_ID \"Clang\")\n"
				"set(SHALLTEAR_COMPILER_PATH \"" +
					(recorded / "x86_64-w64-mingw32-clang++" SLT_TEST_EXE_SUFFIX).generic_string() + "\")\n");

	CppToolchain::DiscoveryOptions options;
	options.sdkRoot = Root();
	options.probeVersions = false;

	CppToolchain::ToolchainInfo info;
	{
		ScopedPath scoped(onPath.generic_string());
		info = CppToolchain::Discover(options);
	}

	EXPECT_EQ(info.compiler.path, (recorded / "x86_64-w64-mingw32-clang++" SLT_TEST_EXE_SUFFIX).generic_string());
	EXPECT_EQ(info.cmake.path, (onPath / "cmake" SLT_TEST_EXE_SUFFIX).generic_string());
	EXPECT_EQ(info.ninja.path, (onPath / "ninja" SLT_TEST_EXE_SUFFIX).generic_string());
}

TEST_F(ScriptWorkflowFixture, ToolchainFileSDKUsesPlainDriver) {
	const std::filesystem::path recorded = _root / "recorded-bin";
	WriteStub(recorded / "x86_64-w64-mingw32-clang++" SLT_TEST_EXE_SUFFIX);
	WriteStub(recorded / "clang++" SLT_TEST_EXE_SUFFIX);
	WriteStub(recorded / "cmake" SLT_TEST_EXE_SUFFIX);
	WriteStub(recorded / "ninja" SLT_TEST_EXE_SUFFIX);

	WriteSource("project://ShalltearConfig.cmake",
				"set(SHALLTEAR_COMPILER_PATH \"" + (recorded / "clang++" SLT_TEST_EXE_SUFFIX).generic_string() +
					"\")\n"
					"set(SHALLTEAR_TOOLCHAIN_FILE \"${SHALLTEAR_PACKAGE_ROOT}/Lib/cmake/Shalltear/Toolchains/Linux-x64.cmake\")\n");

	CppToolchain::DiscoveryOptions options;
	options.sdkRoot = Root();
	options.probeVersions = false;
	const CppToolchain::ToolchainInfo info = CppToolchain::Discover(options);

	EXPECT_EQ(info.compiler.path, (recorded / "clang++" SLT_TEST_EXE_SUFFIX).generic_string());
	EXPECT_EQ(info.sdkToolchainFile, Root() + "/Lib/cmake/Shalltear/Toolchains/Linux-x64.cmake");
	EXPECT_EQ(CppToolchain::GetCompilerDriverName(info), "clang++");
	EXPECT_EQ(CppToolchain::GetCompilerDriverName(CppToolchain::ToolchainInfo{}), "x86_64-w64-mingw32-clang++");
}

TEST_F(ScriptWorkflowFixture, StaticSDKProbeMatchesTargetTriple) {
	WriteSource("project://Static/ShalltearConfig.cmake",
				"set(SHALLTEAR_COMPILER_TARGET \"probe-unknown-none\")\nset(SHALLTEAR_LINK_MODE \"STATIC\")\n");
	WriteSource("project://Shared/ShalltearConfig.cmake",
				"set(SHALLTEAR_COMPILER_TARGET \"probe-unknown-none\")\nset(SHALLTEAR_LINK_MODE \"SHARED\")\n");

	{
		ScopedEnvironment scoped("SHALLTEAR_SDK_STATIC", Root() + "/Static");
		BuildProfileOps::StaticSDKProbe probe;
		BuildProfileOps::FindStaticSDKRoot(probe, "probe-unknown-none");
		EXPECT_EQ(probe.root, Root() + "/Static");
		EXPECT_EQ(probe.source, "SHALLTEAR_SDK_STATIC");

		BuildProfileOps::FindStaticSDKRoot(probe, "other-unknown-none");
		EXPECT_TRUE(probe.root.empty());
		EXPECT_TRUE(probe.source.empty());
	}

	ScopedEnvironment scoped("SHALLTEAR_SDK_STATIC", Root() + "/Shared");
	BuildProfileOps::StaticSDKProbe probe;
	BuildProfileOps::FindStaticSDKRoot(probe, "probe-unknown-none");
	EXPECT_TRUE(probe.root.empty());
	EXPECT_NE(probe.fix.find("not a STATIC Shalltear package"), str::npos) << probe.fix;
}

TEST_F(ScriptWorkflowFixture, ToolchainIgnoresMissingCompiler) {
	const std::filesystem::path onPath = _root / "path-bin";
	WriteStub(onPath / "clang++" SLT_TEST_EXE_SUFFIX);
	WriteStub(onPath / "cmake" SLT_TEST_EXE_SUFFIX);
	WriteStub(onPath / "ninja" SLT_TEST_EXE_SUFFIX);

	WriteSource("project://ShalltearConfig.cmake",
				"set(SHALLTEAR_COMPILER_PATH \"" + (_root / "gone" / "x86_64-w64-mingw32-clang++" SLT_TEST_EXE_SUFFIX).generic_string() + "\")\n");

	CppToolchain::DiscoveryOptions options;
	options.sdkRoot = Root();
	options.probeVersions = false;

	CppToolchain::ToolchainInfo info;
	{
		ScopedPath scoped(onPath.generic_string());
		info = CppToolchain::Discover(options);
	}

	EXPECT_EQ(info.compiler.path, (onPath / "clang++" SLT_TEST_EXE_SUFFIX).generic_string());
}

TEST_F(ScriptWorkflowFixture, ToolchainReadsSDKIdentityFromPackage) {
	WriteSource("project://ShalltearConfig.cmake",
				"set(SHALLTEAR_COMPILER_ID \"Clang\")\n"
				"set(SHALLTEAR_COMPILER_VERSION \"20.1.8\")\n"
				"set(SHALLTEAR_LINK_MODE \"SHARED\")\n");

	CppToolchain::ToolchainInfo info;
	ASSERT_TRUE(CppToolchain::ReadSDKIdentity(Root(), info));
	EXPECT_EQ(info.sdkCompilerId, "Clang");
	EXPECT_EQ(info.sdkCompilerVersion, "20.1.8");
	EXPECT_EQ(info.sdkLinkMode, "SHARED");

	CppToolchain::ToolchainInfo absent;
	EXPECT_FALSE(CppToolchain::ReadSDKIdentity(Root() + "/nowhere", absent));
}

TEST_F(ScriptWorkflowFixture, CompileServiceRefusesEmptyRequest) {
	CompileService service;
	EXPECT_EQ(service.GetState(), CompileState::Idle);
	EXPECT_FALSE(service.IsBusy());
	EXPECT_TRUE(service.GetDiagnostics().empty());

	CompileRequest request;
	EXPECT_FALSE(service.RequestCompile(request));
	EXPECT_EQ(service.GetState(), CompileState::Idle);

	service.PumpMainThread();
	EXPECT_EQ(service.GetState(), CompileState::Idle);
}

TEST_F(ScriptWorkflowFixture, CompileControlExplainsStates) {
	CompileControlText idle = DescribeCompileState(CompileState::Idle, 0, 0);
	EXPECT_EQ(idle.status, "Compile");
	EXPECT_EQ(idle.tooltip, "Compile");

	EXPECT_EQ(DescribeCompileState(CompileState::Configuring, 0, 0).tooltip, "Configuring...");
	EXPECT_EQ(DescribeCompileState(CompileState::Compiling, 0, 0).tooltip, "Compiling...");

	CompileControlText clean = DescribeCompileState(CompileState::Succeeded, 0, 0);
	EXPECT_EQ(clean.status, "Up to date");
	EXPECT_EQ(clean.tooltip, "Up to date. Click to compile");

	CompileControlText warned = DescribeCompileState(CompileState::Succeeded, 0, 2);
	EXPECT_EQ(warned.status, "Up to date (2 warning(s))");
	EXPECT_EQ(warned.tooltip, "Up to date (2 warning(s)). Click to compile");

	CompileControlText failed = DescribeCompileState(CompileState::Failed, 3, 0);
	EXPECT_EQ(failed.status, "3 error(s)");
	EXPECT_EQ(failed.tooltip, "3 error(s). Click to compile");
	EXPECT_EQ(DescribeCompileState(CompileState::Failed, 3, 1).status, "3 error(s), 1 warning(s)");

	CompileService service;
	EXPECT_EQ(service.GetErrorCount(), 0u);
	EXPECT_EQ(service.GetWarningCount(), 0u);
}

TEST(AggregateHeaderTest, AggregateHeadersExportIncludes) {
	const std::filesystem::path root = Checkout::Root();
	if (root.empty()) {
		GTEST_SKIP() << "checkout not found";
	}
	const std::vector<std::filesystem::path> trees = PublicTrees(root);

	constexpr std::string_view Begin = "// IWYU pragma: begin_exports";
	constexpr std::string_view End = "// IWYU pragma: end_exports";
	std::vector<str> aggregates;
	for (const std::filesystem::path& tree : trees) {
		std::error_code ec;
		for (const auto& entry : std::filesystem::recursive_directory_iterator(tree, ec)) {
			if (!entry.is_regular_file() || entry.path().extension() != ".h") {
				continue;
			}
			std::ifstream file(entry.path(), std::ios::binary);
			str text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

			bool aggregate = true;
			size_t includes = 0;
			size_t firstInclude = str::npos;
			size_t lastInclude = str::npos;
			for (size_t begin = 0; begin < text.size();) {
				size_t end = text.find('\n', begin);
				if (end == str::npos) {
					end = text.size();
				}
				std::string_view line(text.data() + begin, end - begin);
				while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
					line.remove_suffix(1);
				}
				while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
					line.remove_prefix(1);
				}
				if (line.starts_with("#include")) {
					++includes;
					if (firstInclude == str::npos) {
						firstInclude = begin;
					}
					lastInclude = begin;
				} else if (!line.empty() && !line.starts_with("//") && line != "#pragma once" &&
						   !line.starts_with("using namespace") && !line.starts_with("#if") &&
						   !line.starts_with("#elif") && !line.starts_with("#else") && !line.starts_with("#endif")) {
					aggregate = false;
					break;
				}
				begin = end + 1;
			}
			if (!aggregate || includes < 2) {
				continue;
			}

			str relative = std::filesystem::relative(entry.path(), tree, ec).generic_string();
			aggregates.push_back(relative);
			size_t beginAt = text.find(Begin);
			size_t endAt = text.find(End);
			EXPECT_TRUE(beginAt != str::npos && endAt != str::npos && beginAt < firstInclude && endAt > lastInclude);
		}
	}

	EXPECT_NE(std::find(aggregates.begin(), aggregates.end(), str("Shalltear.h")), aggregates.end());
	EXPECT_NE(std::find(aggregates.begin(), aggregates.end(), str("Shalltear/Audio.h")), aggregates.end());
	EXPECT_GE(aggregates.size(), 15u);
}

namespace {

	std::set<str> ReachedFrom(const std::vector<std::filesystem::path>& trees, const str& seed) {
		std::set<str> reached;
		std::vector<str> pending{seed};
		while (!pending.empty()) {
			const str name = pending.back();
			pending.pop_back();
			std::filesystem::path found;
			for (const std::filesystem::path& tree : trees) {
				std::error_code ec;
				if (std::filesystem::is_regular_file(tree / name, ec)) {
					found = tree / name;
					break;
				}
			}
			if (found.empty() || !reached.insert(name).second) {
				continue;
			}
			std::ifstream file(found, std::ios::binary);
			for (str line; std::getline(file, line);) {
				const size_t at = line.find_first_not_of(" \t");
				if (at == str::npos || line.compare(at, 9, "#include ") != 0) {
					continue;
				}
				const size_t open = line.find('"', at);
				const size_t close = open == str::npos ? str::npos : line.find('"', open + 1);
				if (close != str::npos) {
					pending.push_back(line.substr(open + 1, close - open - 1));
				}
			}
		}
		return reached;
	}

	struct EngineFacingHeader {
		const char* header;
		const char* reason;
	};
	constexpr EngineFacingHeader EngineFacing[] = {
		{"Shalltear/Core/Asset/AssetCues.h", "package to package hook"},
		{"Shalltear/Core/Asset/ImportTier.h", "editor only import code"},
		{"Shalltear/Core/Asset/PakFormat.h", "cook container format"},
		{"Shalltear/Core/Graphics/CubeFace.h", "capture convention"},
		{"Shalltear/Core/Graphics/FramePass.h", "package frame GPU work"},
		{"Shalltear/Core/Graphics/ShaderCompileQueue.h", "background compile"},
		{"Shalltear/Core/Graphics/TextureCodecs.h", "import encoders"},
		{"Shalltear/Core/Math/HalfFloat.h", "texel conversion"},
		{"Shalltear/Core/Memory/OffsetPtr.h", "unused draft"},
		{"Shalltear/Core/Module.h", "loader"},
		{"Shalltear/Core/Module/ModuleEvents.h", "image unload events"},
		{"Shalltear/Core/Module/ModuleSystem.h", "loader"},
		{"Shalltear/Core/Module/TypeReplacement.h", "package type swap"},
		{"Shalltear/Core/Profiler.h", "zone instrument"},
		{"Shalltear/Core/ProjectLayout.h", "project root layout"},
		{"Shalltear/Core/Serialization/HandleResolver.h", "serializer"},
		{"Shalltear/Core/Serialization/JSON.h", "sidecar/scene DOM"},
	};

} // anonymous namespace

TEST(UmbrellaHeaderTest, UmbrellaReachesGameHeaders) {
	const std::filesystem::path root = Checkout::Root();
	if (root.empty()) {
		GTEST_SKIP() << "checkout not found";
	}
	const std::vector<std::filesystem::path> trees = PublicTrees(root);

	const std::set<str> reached = ReachedFrom(trees, "Shalltear.h");
	std::set<str> engineFacing;
	for (const EngineFacingHeader& entry : EngineFacing) {
		engineFacing.insert(entry.header);
		EXPECT_TRUE(std::filesystem::is_regular_file(root / "Source/Core/Public" / entry.header));
	}
	const std::filesystem::path core = root / "Source/Core/Public";
	size_t coreHeaders = 0;
	std::error_code ec;
	for (const auto& entry : std::filesystem::recursive_directory_iterator(core, ec)) {
		if (!entry.is_regular_file() || entry.path().extension() != ".h") {
			continue;
		}
		++coreHeaders;
		const str relative = std::filesystem::relative(entry.path(), core, ec).generic_string();
		const bool isReached = reached.contains(relative);
		const bool isListed = engineFacing.contains(relative);
		EXPECT_TRUE(isReached || isListed);
		EXPECT_FALSE(isReached && isListed) << relative;
	}
	EXPECT_GT(coreHeaders, 100u) << core.generic_string();
	EXPECT_TRUE(reached.contains("Shalltear/Core/UI/UIPanelRenderer.h"));

	size_t umbrellas = 0;
	for (const auto& package : std::filesystem::directory_iterator(root / "Source", ec)) {
		const std::filesystem::path manifestFile = package.path() / "package.json";
		if (!std::filesystem::is_regular_file(manifestFile, ec)) {
			continue;
		}
		std::ifstream file(manifestFile, std::ios::binary);
		const str json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		PackageOps::PackageManifest manifest;
		str error;
		ASSERT_TRUE(PackageOps::ParseManifest(json, manifest, error)) << manifestFile.generic_string() << ": " << error;
		const std::filesystem::path publicTree = package.path() / "Public";
		if (manifest.kind == PackageOps::PackageKind::Tool || !std::filesystem::is_directory(publicTree, ec)) {
			continue;
		}

		const str name = package.path().filename().generic_string();
		const str umbrella = "Shalltear/" + name + ".h";
		if (!std::filesystem::is_regular_file(publicTree / umbrella, ec)) {
			ADD_FAILURE();
			continue;
		}
		++umbrellas;
		const std::set<str> packageReached = ReachedFrom(trees, umbrella);
		for (const auto& entry : std::filesystem::recursive_directory_iterator(publicTree / "Shalltear" / name, ec)) {
			if (!entry.is_regular_file() || entry.path().extension() != ".h") {
				continue;
			}
			const str relative = std::filesystem::relative(entry.path(), publicTree, ec).generic_string();
			EXPECT_TRUE(packageReached.contains(relative));
		}
	}
	EXPECT_GE(umbrellas, 5u);
}

TEST_F(ScriptWorkflowFixture, ModuleStatusReportsCauseNotConsequence) {
	using Facts = ScriptProjectOps::ModuleStatusFacts;
	using Status = ScriptProjectOps::ModuleStatus;

	EXPECT_EQ(ScriptProjectOps::ClassifyModuleStatus(Facts{}), Status::NoScriptProject);

	Facts facts;
	facts.hasScriptProject = true;
	EXPECT_EQ(ScriptProjectOps::ClassifyModuleStatus(facts), Status::LoadFailed);

	facts.moduleLoaded = true;
	EXPECT_EQ(ScriptProjectOps::ClassifyModuleStatus(facts), Status::Loaded);

	facts.moduleLoaded = false;
	facts.compileFailed = true;
	EXPECT_EQ(ScriptProjectOps::ClassifyModuleStatus(facts), Status::CompileFailed);

	facts.compiling = true;
	EXPECT_EQ(ScriptProjectOps::ClassifyModuleStatus(facts), Status::Compiling);

	facts.hasScriptProject = false;
	EXPECT_EQ(ScriptProjectOps::ClassifyModuleStatus(facts), Status::NoScriptProject);
}

TEST(ScaffoldTemplateTest, NoTemplateCitesTheEnginesOwnDocumentation) {
	const std::filesystem::path templates = RepoDir("EngineAssets/Templates");
	if (templates.empty()) {
		GTEST_SKIP() << "EngineAssets/Templates not found";
	}

	constexpr std::string_view Section = "\xc2\xa7";
	size_t checked = 0;
	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator(templates, ec)) {
		if (!entry.is_regular_file()) {
			continue;
		}
		std::ifstream file(entry.path(), std::ios::binary);
		str text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		str name = entry.path().filename().generic_string();
		++checked;

		EXPECT_EQ(text.find(".md"), str::npos);
		EXPECT_EQ(text.find(Section), str::npos) << name;
		str lowered = StringUtil::ToLower(text);
		EXPECT_EQ(lowered.find("decision "), str::npos);
	}

	EXPECT_GE(checked, 10u);
}

TEST_F(ScriptWorkflowFixture, ScaffoldedIgnoreCoversDerivedAssetIndex) {
	WriteSource("project://Project.ini", "[Project]\nname = MyGame\n");

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "MyGame";
	options.sdkRoot = "C:/Sdk";
	options.editorExecutable = "C:/Sdk/Bin/Shalltear" SLT_TEST_EXE_SUFFIX;
	ScriptProjectOps::ScaffoldResult result;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, result));

	constexpr std::string_view AssetIndex = ".assets.db";
	ASSERT_TRUE(ProjectLayout::IsReservedFileName(AssetIndex));

	str gitignore = Read(".gitignore");
	EXPECT_NE(gitignore.find(AssetIndex), str::npos) << gitignore;

	EXPECT_NE(gitignore.find("/Library/"), str::npos) << gitignore;
	EXPECT_NE(gitignore.find("/Builds/"), str::npos) << gitignore;
}
