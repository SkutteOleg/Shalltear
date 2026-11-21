#include "TestsPCH.h"

#include "TestPlatform.h"

#include "Shalltear/Core/Config.h"
#include "Shalltear/Core/Module.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/StringUtil.h"

#include "Shalltear/Editor/CompileService.h"
#include "Shalltear/Editor/CppToolchain.h"
#include "Shalltear/Editor/PackageOps.h"
#include "Shalltear/Editor/ScriptProjectOps.h"

#include "StaleSDK.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

using namespace Shalltear;

namespace {

	constexpr f64 COMPILE_TIMEOUT_SECONDS = 300.0;

	str ReadWholeFile(const std::filesystem::path& path) {
		std::ifstream stream(path, std::ios::binary);
		return str(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
	}

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

	class ScriptCompileIntegrationTest : public ::testing::Test {
	protected:
		std::filesystem::path _root;
		str _config;

		void SetUp() override {
			_sdkRoot = ScriptProjectOps::GetSDKRoot();
			if (_sdkRoot.empty()) {
				GTEST_SKIP() << "no Shalltear CMake package found";
			}

			CppToolchain::DiscoveryOptions options;
			options.sdkRoot = _sdkRoot;
			CppToolchain::ToolchainInfo toolchain = CppToolchain::Discover(options);
			if (!toolchain.IsComplete()) {
				GTEST_SKIP() << "toolchain incomplete:\n"
							 << CppToolchain::Describe(toolchain);
			}
			_cmakePath = toolchain.cmake.path;
			_compilerPath = toolchain.compiler.path;
			_ninjaPath = toolchain.ninja.path;
			_toolchainFile = toolchain.sdkToolchainFile;

			static std::atomic<u64> counter{0};
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
					("shalltear_compile_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
			std::filesystem::create_directories(_root);
			FileSystem::Mount("project", Path(_root.generic_string()));

			std::filesystem::path engineAssets = RepoDir("EngineAssets");
			ASSERT_FALSE(engineAssets.empty());
			FileSystem::Mount("engine", Path(engineAssets.generic_string()));

			_config = str(ScriptProjectOps::GetActiveConfigName());
			SceneSystem::Initialize();
			ModuleSystem::Initialize();
		}

		void TearDown() override {
			if (IsSkipped()) {
				return;
			}
			SceneSystem::Terminate();
			ModuleSystem::Terminate();
			FileSystem::Unmount("engine");
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		str Root() const { return _root.generic_string(); }
		const str& SDKRoot() const { return _sdkRoot; }

		void WriteFile(const str& relative, const str& contents) {
			std::filesystem::path target = _root / relative;
			std::error_code ec;
			std::filesystem::create_directories(target.parent_path(), ec);
			std::ofstream file(target, std::ios::binary | std::ios::trunc);
			file << contents;
		}

		bool RunCompile(CompileService& service, std::initializer_list<const char*> enabledPackages = {}) {
			CompileRequest request;
			request.projectPath = Root();
			for (const char* package : enabledPackages) {
				request.enabledPackages.emplace_back(package);
			}
			if (!service.RequestCompile(request)) {
				return false;
			}
			auto deadline = std::chrono::steady_clock::now() +
							std::chrono::duration<f64>(COMPILE_TIMEOUT_SECONDS);
			while (service.IsBusy() && std::chrono::steady_clock::now() < deadline) {
				service.PumpMainThread();
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
			service.PumpMainThread();
			return service.GetState() == CompileState::Succeeded;
		}

		bool RunCompileRecording(CompileService& service, str& outStates) {
			CompileRequest request;
			request.projectPath = Root();
			if (!service.RequestCompile(request)) {
				return false;
			}
			CompileState last = CompileState::Idle;
			bool any = false;
			auto record = [&] {
				CompileState state = service.GetState();
				if (any && state == last) {
					return;
				}
				last = state;
				any = true;
				if (!outStates.empty()) {
					outStates += ">";
				}
				switch (state) {
				case CompileState::Idle:
					outStates += "Idle";
					break;
				case CompileState::Configuring:
					outStates += "Configuring";
					break;
				case CompileState::Compiling:
					outStates += "Compiling";
					break;
				case CompileState::Succeeded:
					outStates += "Succeeded";
					break;
				case CompileState::Failed:
					outStates += "Failed";
					break;
				}
			};
			record();
			auto deadline = std::chrono::steady_clock::now() +
							std::chrono::duration<f64>(COMPILE_TIMEOUT_SECONDS);
			while (service.IsBusy() && std::chrono::steady_clock::now() < deadline) {
				service.PumpMainThread();
				record();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			service.PumpMainThread();
			record();
			return service.GetState() == CompileState::Succeeded;
		}

		str Diagnostics(const CompileService& service) const {
			str text;
			for (const CompilerDiagnostic& diagnostic : service.GetDiagnostics()) {
				text += diagnostic.raw + "\n";
			}
			return text;
		}

		str DiagnosticsOnOneLine(const CompileService& service) const {
			str text;
			for (char c : Diagnostics(service)) {
				const bool space = c == ' ' || c == '\t' || c == '\r' || c == '\n';
				if (!space) {
					text += c;
				} else if (!text.empty() && text.back() != ' ') {
					text += ' ';
				}
			}
			return text;
		}

		str RunCMake(const str& arguments, i32* outExitCode = nullptr) const {
			return RunTool(_cmakePath, arguments, outExitCode);
		}

		str RunTool(const str& toolPath, const str& arguments, i32* outExitCode = nullptr) const {
			int exitCode = 0;
			str output = TestPlatform::RunCommand("\"" + toolPath + "\" " + arguments, exitCode);
			if (outExitCode) {
				*outExitCode = exitCode;
			}
			return output;
		}

		const str& CompilerPath() const { return _compilerPath; }
		const str& NinjaPath() const { return _ninjaPath; }
		str ToolchainFileArgument() const {
			return _toolchainFile.empty() ? str() : " -DCMAKE_TOOLCHAIN_FILE=\"" + _toolchainFile + "\"";
		}

		str FabricateStaleSDK(std::string_view commit, str& outPackageDirectory, std::string_view version = {}) const {
			std::filesystem::path suggested = _root / "StaleSDK";
			std::filesystem::path resolved = suggested / "Shalltear";
			if (!StaleSDK::Fabricate(SDKRoot(), resolved, commit, version)) {
				return {};
			}
			outPackageDirectory = resolved.generic_string();
			return suggested.generic_string();
		}
	private:
		str _sdkRoot;
		str _cmakePath;
		str _compilerPath;
		str _ninjaPath;
		str _toolchainFile;
	};

} // anonymous namespace

TEST_F(ScriptCompileIntegrationTest, CompileReloadMigratesState) {
	Config projectFile;
	projectFile.SetString("Project", "name", "CompileProbe");
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "CompileProbe";
	options.sdkRoot = SDKRoot();
	options.editorExecutable = ScriptProjectOps::GetEditorExecutablePath();
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));
	ASSERT_TRUE(ScriptProjectOps::HasScriptProject(Root()));

	CompileService service;
	service.RefreshToolchain();
	bool compiled = RunCompile(service);
	if (!compiled) {
		str diagnostics;
		for (const CompilerDiagnostic& diagnostic : service.GetDiagnostics()) {
			diagnostics += diagnostic.raw + "\n";
		}
		FAIL() << "scaffolded project didn't compile:\n"
			   << diagnostics;
	}

	str binary = ScriptProjectOps::GetModuleBinaryPath(Root(), _config);
	ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(binary))) << binary;

	ModuleHandle module = ModuleSystem::Load(Path(binary));
	ASSERT_TRUE(ModuleSystem::IsLoaded(module));

	const TypeInfo* type = TypeRegistry::FindType(StringHash("ExampleComponent"));
	ASSERT_NE(type, nullptr);

	Handle<Scene> scene = SceneSystem::CreateScene("CompileProbeScene");
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", scene);
	Handle<Component> component = object.Get()->AddComponent(type);
	ASSERT_TRUE(component.IsValid());

	Variant value(7.5f);
	ASSERT_TRUE(SetField(component.Get(), type, StringHash("degreesPerSecond"), &value));

	str header = FileSystem::ReadTextFile(Path(Root() + "/Source/ExampleComponent.h"), MemTag::Editor);
	ASSERT_FALSE(header.empty());
	ASSERT_NE(header.find("f32 degreesPerSecond = 60.0f;"), str::npos) << header;
	header = StringUtil::Replace(header, "SLT_REFLECT_FIELD(degreesPerSecond)",
								 "SLT_REFLECT_FIELD(degreesPerSecond)\n\tSLT_REFLECT_FIELD(jumpHeight)");
	header = StringUtil::Replace(header, "f32 degreesPerSecond = 60.0f;",
								 "f32 degreesPerSecond = 60.0f;\n\tf32 jumpHeight = 2.0f;");
	WriteFile("Source/ExampleComponent.h", header);

	ASSERT_TRUE(RunCompile(service));
	ASSERT_TRUE(ModuleSystem::Reload(module));

	const TypeInfo* reloaded = TypeRegistry::FindType(StringHash("ExampleComponent"));
	ASSERT_NE(reloaded, nullptr);
	ASSERT_TRUE(component.IsValid());

	Variant migrated;
	ASSERT_TRUE(GetField(component.Get(), reloaded, StringHash("degreesPerSecond"), &migrated));
	const f32* migratedValue = migrated.TryGet<f32>();
	ASSERT_NE(migratedValue, nullptr);
	EXPECT_FLOAT_EQ(*migratedValue, 7.5f);

	Variant added;
	ASSERT_TRUE(GetField(component.Get(), reloaded, StringHash("jumpHeight"), &added));
	const f32* addedValue = added.TryGet<f32>();
	ASSERT_NE(addedValue, nullptr);
	EXPECT_FLOAT_EQ(*addedValue, 2.0f);
}

TEST_F(ScriptCompileIntegrationTest, UmbrellaIsPrecompiled) {
	Config projectFile;
	projectFile.SetString("Project", "name", "PchProbe");
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "PchProbe";
	options.sdkRoot = SDKRoot();
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));

	WriteFile("Source/NoIncludesAtAll.cpp", R"(class Bare : public Behavior {
public:
	SLT_REFLECT_COMPONENT(Bare, Behavior)
	SLT_REFLECT_FIELD(speed)
	SLT_END_REFLECT()

	f32 speed = 1.0f;
protected:
	void OnUpdate() override { speed += Time::GetDeltaTime(); }
};

SLT_REGISTER_TYPE(Bare)
)");

	CompileService service;
	service.RefreshToolchain();
	if (!RunCompile(service)) {
		FAIL() << Diagnostics(service);
	}

	std::filesystem::path compileDirectory(ScriptProjectOps::GetCompileDirectory(Root(), _config));
	std::filesystem::path precompiled =
		compileDirectory / "CMakeFiles" / "PchProbe.dir" / "cmake_pch.hxx.pch";
	EXPECT_TRUE(std::filesystem::exists(precompiled));

	str database = ReadWholeFile(compileDirectory / "compile_commands.json");
	ASSERT_FALSE(database.empty());
	EXPECT_NE(database.find("-include-pch"), str::npos);
	EXPECT_NE(database.find("NoIncludesAtAll.cpp"), str::npos);

	str binary = ScriptProjectOps::GetModuleBinaryPath(Root(), _config);
	ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(binary))) << binary;
	ModuleHandle module = ModuleSystem::Load(Path(binary));
	ASSERT_TRUE(ModuleSystem::IsLoaded(module));
	EXPECT_NE(TypeRegistry::FindType(StringHash("Bare")), nullptr);
}

TEST_F(ScriptCompileIntegrationTest, TheScaffoldBuildsWithPrecompiledHeadersDisabled) {
	Config projectFile;
	projectFile.SetString("Project", "name", "NoPchProbe");
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "NoPchProbe";
	options.sdkRoot = SDKRoot();
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));

	str binaryDirectory = Root() + "/Library/Compile/NoPCH";
	i32 exitCode = -1;
	str configured = RunCMake("-S \"" + Root() + "\" -B \"" + binaryDirectory + "\" -G Ninja" +
								  " -DCMAKE_BUILD_TYPE=" + _config + " -DCMAKE_CXX_COMPILER=\"" + CompilerPath() + "\"" +
								  " -DCMAKE_MAKE_PROGRAM=\"" + NinjaPath() + "\"" + ToolchainFileArgument() + " -DShalltear_ROOT=\"" + SDKRoot() + "\"" +
								  " -DSHALLTEAR_EXPECT_ENGINE_IDENTITY=\"" + CppToolchain::GetEngineIdentity() + "\"" +
								  " -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON",
							  &exitCode);
	ASSERT_EQ(exitCode, 0) << configured;

	str database = ReadWholeFile(std::filesystem::path(binaryDirectory) / "compile_commands.json");
	ASSERT_FALSE(database.empty());
	ASSERT_EQ(database.find("cmake_pch"), str::npos) << database;

	str built = RunCMake("--build \"" + binaryDirectory + "\"", &exitCode);
	EXPECT_EQ(exitCode, 0) << built;

	WriteFile("Source/LeansOnTheCache.cpp", "class Leaning : public Behavior {};\n");
	str control = RunCMake("--build \"" + binaryDirectory + "\"", &exitCode);
	EXPECT_NE(exitCode, 0) << control;
	EXPECT_NE(control.find("Behavior"), str::npos) << control;
}

namespace {

	str LspMessage(const str& json) {
		return "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json;
	}

	str JSONString(const str& text) {
		str out = "\"";
		for (char c : text) {
			switch (c) {
			case '"':
				out += "\\\"";
				break;
			case '\\':
				out += "\\\\";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\r':
				out += "\\r";
				break;
			case '\t':
				out += "\\t";
				break;
			default:
				out += c;
				break;
			}
		}
		return out + "\"";
	}

	str FileUri(const str& path) {
		str uri = "file:///";
		for (char c : path) {
			if (c == '\\') {
				uri += '/';
			} else if (c == ' ') {
				uri += "%20";
			} else {
				uri += c;
			}
		}
		return uri;
	}

	str DidOpen(const str& path, const str& text) {
		return LspMessage("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":" +
						  JSONString(FileUri(path)) + ",\"languageId\":\"cpp\",\"version\":1,\"text\":" + JSONString(text) + "}}}");
	}

	str PublishedLine(const str& output, const str& fileSuffix) {
		str found;
		for (size_t begin = 0; begin < output.size();) {
			size_t end = output.find('\n', begin);
			if (end == str::npos) {
				end = output.size();
			}
			str line = output.substr(begin, end - begin);
			if (line.find("textDocument/publishDiagnostics") != str::npos && line.find(fileSuffix + "\"") != str::npos) {
				found = line;
			}
			begin = end + 1;
		}
		return found;
	}

} // anonymous namespace

TEST_F(ScriptCompileIntegrationTest, ScaffoldHasNoUnusedInclude) {
	std::filesystem::path clangd = std::filesystem::path(CompilerPath()).parent_path() / "clangd" SLT_TEST_EXE_SUFFIX;
	if (!std::filesystem::exists(clangd)) {
		GTEST_SKIP() << "no clangd at " << clangd.generic_string();
	}

	Config projectFile;
	projectFile.SetString("Project", "name", "ClangdProbe");
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "ClangdProbe";
	options.sdkRoot = SDKRoot();
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));

	CompileService service;
	service.RefreshToolchain();
	ASSERT_TRUE(RunCompile(service, {"dev.shalltear.audio"})) << Diagnostics(service);

	str component = Root() + "/Source/ExampleComponent.h";
	str unused = Root() + "/Source/UnusedInclude.h";
	WriteFile("Source/UnusedInclude.h", "#pragma once\n#include \"Shalltear/Core/Log.h\"\n");
#ifdef SHALLTEAR_SHARED
	str packageOnly = Root() + "/Source/PackageOnly.h";
	WriteFile("Source/PackageOnly.h", "#pragma once\n#include \"Shalltear.h\"\n\nstruct PackageOnly {\n\tAudioClip* clip = nullptr;\n};\n");
#endif

	str requests;
	requests += LspMessage("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":" +
						   JSONString(FileUri(Root())) + ",\"capabilities\":{}}}");
	requests += LspMessage("{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");
	requests += DidOpen(component, ReadWholeFile(std::filesystem::path(component)));
	requests += DidOpen(unused, ReadWholeFile(std::filesystem::path(unused)));
#ifdef SHALLTEAR_SHARED
	requests += DidOpen(packageOnly, ReadWholeFile(std::filesystem::path(packageOnly)));
#endif
	requests += LspMessage("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shutdown\",\"params\":null}");
	requests += LspMessage("{\"jsonrpc\":\"2.0\",\"method\":\"exit\",\"params\":null}");
	WriteFile("Library/clangd-requests.txt", requests);

	i32 exitCode = -1;
	str output = RunTool(clangd.generic_string(), "--sync --log=error < \"" + Root() + "/Library/clangd-requests.txt\"", &exitCode);
	EXPECT_EQ(exitCode, 0) << output;

	str published = PublishedLine(output, "Source/ExampleComponent.h");
	ASSERT_FALSE(published.empty()) << output;
	EXPECT_NE(published.find("\"diagnostics\":[]"), str::npos) << published;

	str control = PublishedLine(output, "Source/UnusedInclude.h");
	ASSERT_FALSE(control.empty()) << output;
	EXPECT_NE(control.find("unused-includes"), str::npos) << control;

#ifdef SHALLTEAR_SHARED
	str packageUser = PublishedLine(output, "Source/PackageOnly.h");
	ASSERT_FALSE(packageUser.empty()) << output;
	EXPECT_NE(packageUser.find("\"diagnostics\":[]"), str::npos) << packageUser;
#endif
}

TEST_F(ScriptCompileIntegrationTest, ModulePathMatchesCompile) {
	Config projectFile;
	projectFile.SetString("Project", "name", "PathProbe");
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "PathProbe";
	options.sdkRoot = SDKRoot();
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));

	str expected = ScriptProjectOps::GetModuleBinaryPath(Root(), _config);
	EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(expected)));

	CompileService service;
	service.RefreshToolchain();
	ASSERT_TRUE(RunCompile(service));

	EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(expected))) << expected;
}

TEST_F(ScriptCompileIntegrationTest, NoOpCompileClearsStaleness) {
	Config projectFile;
	projectFile.SetString("Project", "name", "StaleProbe");
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "StaleProbe";
	options.sdkRoot = SDKRoot();
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));

	WriteFile("Source/Unreferenced.h", "#pragma once\n// nothing includes this\n");

	CompileService service;
	service.RefreshToolchain();
	ASSERT_TRUE(RunCompile(service));

	std::filesystem::path binary(ScriptProjectOps::GetModuleBinaryPath(Root(), _config));
	std::filesystem::path stamp(ScriptProjectOps::GetModuleCompileStampPath(Root(), _config));
	ASSERT_TRUE(std::filesystem::exists(binary)) << binary.generic_string();
	ASSERT_TRUE(std::filesystem::exists(stamp)) << stamp.generic_string();
	ASSERT_FALSE(ScriptProjectOps::IsModuleBinaryOutOfDate(Root(), _config));

	std::filesystem::file_time_type linkedAt = std::filesystem::last_write_time(binary);

	std::this_thread::sleep_for(std::chrono::milliseconds(1100));
	WriteFile("Source/Unreferenced.h", "#pragma once\n// still nothing includes this\n");

	str reason;
	ASSERT_TRUE(ScriptProjectOps::IsModuleBinaryOutOfDate(Root(), _config, &reason));
	EXPECT_NE(reason.find("Unreferenced.h"), str::npos) << reason << "'";

	ASSERT_TRUE(RunCompile(service));

	ASSERT_EQ(std::filesystem::last_write_time(binary), linkedAt);

	EXPECT_FALSE(ScriptProjectOps::IsModuleBinaryOutOfDate(Root(), _config));
}

TEST_F(ScriptCompileIntegrationTest, GeneratedPresetsConfigureWithNoEnvironmentSetup) {
	Config projectFile;
	projectFile.SetString("Project", "name", "PresetProbe");
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "PresetProbe";
	options.sdkRoot = SDKRoot();
	options.editorExecutable = ScriptProjectOps::GetEditorExecutablePath();
	options.enabledPackages = Vector<str>{str("dev.shalltear.physics3d")};
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));

	const str previous = TestPlatform::GetEnvironment("SHALLTEAR_SDK");
	TestPlatform::SetEnvironment("SHALLTEAR_SDK", "");

	const str previousPath = TestPlatform::GetEnvironment("PATH");
	TestPlatform::SetEnvironment("PATH", TestPlatform::GetSystemSearchPath());

	str listed = RunCMake("-S \"" + Root() + "\" --list-presets");
	str configured = RunCMake("-S \"" + Root() + "\" --preset " + _config + "-Local");

	TestPlatform::SetEnvironment("PATH", previousPath);
	TestPlatform::SetEnvironment("SHALLTEAR_SDK", previous);

	EXPECT_NE(listed.find("\"Debug-Local\""), str::npos) << listed;
	EXPECT_NE(listed.find("\"Release-Local\""), str::npos) << listed;
	EXPECT_EQ(std::count(listed.begin(), listed.end(), '"'), 4) << listed;

	str userPresets = ReadWholeFile(std::filesystem::path(Root()) / "CMakeUserPresets.json");
	str compilerDirectory = std::filesystem::path(CompilerPath()).parent_path().generic_string();
	str ninjaDirectory = std::filesystem::path(NinjaPath()).parent_path().generic_string();
	const str separator(1, TestPlatform::GetSearchPathSeparator());
	EXPECT_NE(userPresets.find(compilerDirectory + separator), str::npos) << userPresets;
	EXPECT_NE(userPresets.find(ninjaDirectory + separator), str::npos) << userPresets;

	EXPECT_NE(configured.find("Build files have been written"), str::npos) << configured;
	std::filesystem::path compileDirectory = ScriptProjectOps::GetCompileDirectory(Root(), _config);
	EXPECT_TRUE(std::filesystem::exists(compileDirectory / "CMakeCache.txt")) << configured;

	std::filesystem::path database = compileDirectory / "compile_commands.json";
	ASSERT_TRUE(std::filesystem::exists(database));
	std::ifstream file(database);
	str contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	EXPECT_NE(contents.find("ExampleComponent.cpp"), str::npos);
	EXPECT_NE(contents.find("Core/Public"), str::npos) << contents.substr(0, 600);
#ifdef SHALLTEAR_SHARED
	EXPECT_NE(contents.find("Physics3D/Public"), str::npos) << contents.substr(0, 600);
#endif
}

namespace {

	constexpr const char* PHYSICS_CALLER = R"(#include "Shalltear.h"

extern "C" SLT_API_EXPORT u32 LinkProbeOverlapCount() {
	Vector<Handle<Collider>> results;
	return Physics::OverlapSphere(Vec3{0.0f, 0.0f, 0.0f}, 1.0f, results);
}
)";

} // anonymous namespace

#ifdef SHALLTEAR_SHARED

TEST_F(ScriptCompileIntegrationTest, EnabledPackageLinksImplicitly) {
	Config projectFile;
	projectFile.SetString("Project", "name", "LinkProbe");
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "LinkProbe";
	options.sdkRoot = SDKRoot();
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));
	const str cmakeLists = ReadWholeFile(_root / "CMakeLists.txt");
	ASSERT_EQ(cmakeLists.find("Physics3D"), str::npos) << cmakeLists;

	Vector<PackageOps::PackageManifest> catalogue;
	Vector<str> diagnostics;
	PackageOps::LoadBuiltInCatalogue(catalogue, diagnostics);
	ASSERT_NE(PackageOps::FindPackage(catalogue, "dev.shalltear.physics3d"), nullptr);
	const Path binary(ScriptProjectOps::GetModuleBinaryPath(Root(), _config));
	auto importedPackages = [&] {
		Vector<str> imported;
		EXPECT_TRUE(PackageOps::FindUnloadedPackageImports(binary, catalogue, {}, imported));
		str joined;
		for (const str& name : imported) {
			joined += (joined.empty() ? "" : ",") + name;
		}
		return joined;
	};

	CompileService service;
	service.RefreshToolchain();

	ASSERT_TRUE(RunCompile(service, {"dev.shalltear.physics3d"})) << Diagnostics(service);
	EXPECT_EQ(importedPackages(), "");

	WriteFile("Source/LinkProbePhysics.cpp", PHYSICS_CALLER);
	ASSERT_TRUE(RunCompile(service, {"dev.shalltear.physics3d"})) << Diagnostics(service);
	EXPECT_EQ(importedPackages(), "dev.shalltear.physics3d");

	EXPECT_FALSE(RunCompile(service, {}));
	EXPECT_NE(Diagnostics(service).find("undeclared identifier 'Physics'"), str::npos) << Diagnostics(service);

	ASSERT_TRUE(RunCompile(service, {"dev.shalltear.physics3d"})) << Diagnostics(service);
	EXPECT_EQ(importedPackages(), "dev.shalltear.physics3d");
}

#endif

TEST_F(ScriptCompileIntegrationTest, DisabledPackageLinkRefused) {
	Config projectFile;
	projectFile.SetString("Project", "name", "LinkRefusalProbe");
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "LinkRefusalProbe";
	options.sdkRoot = SDKRoot();
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));
	const str scaffoldedLists = ReadWholeFile(_root / "CMakeLists.txt");
	WriteFile("CMakeLists.txt", scaffoldedLists + "\ntarget_link_libraries(LinkRefusalProbe PRIVATE Shalltear::Physics3D)\n");

	CompileService service;
	service.RefreshToolchain();
#ifdef SHALLTEAR_SHARED
	EXPECT_FALSE(RunCompile(service, {}));
	EXPECT_NE(Diagnostics(service).find("Shalltear::Physics3D"), str::npos) << Diagnostics(service);
	EXPECT_NE(DiagnosticsOnOneLine(service).find("dev.shalltear.physics3d is not enabled"), str::npos) << Diagnostics(service);

	EXPECT_TRUE(RunCompile(service, {"dev.shalltear.physics3d"})) << Diagnostics(service);
#else
	EXPECT_FALSE(RunCompile(service, {"dev.shalltear.physics3d"}));
	EXPECT_NE(Diagnostics(service).find("STATIC"), str::npos) << Diagnostics(service);

	WriteFile("CMakeLists.txt", scaffoldedLists);
	EXPECT_TRUE(RunCompile(service, {"dev.shalltear.physics3d"})) << Diagnostics(service);
#endif
}

TEST_F(ScriptCompileIntegrationTest, GeneratedPackageListIsModulesLinkLine) {
	Config projectFile;
	projectFile.SetString("Project", "name", "PackageListProbe");
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "PackageListProbe";
	options.sdkRoot = SDKRoot();
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));

	const std::filesystem::path list =
		std::filesystem::path(ScriptProjectOps::GetCompileDirectory(Root(), _config)) / "ShalltearGenerated" / "ShalltearPackages.h";
	auto included = [&] {
		str joined;
		std::ifstream file(list, std::ios::binary);
		for (str line; std::getline(file, line);) {
			if (line.starts_with("#include ")) {
				joined += (joined.empty() ? "" : ",") + line.substr(9);
			}
		}
		return joined;
	};

	CompileService service;
	service.RefreshToolchain();
	ASSERT_TRUE(RunCompile(service, {"dev.shalltear.audio", "dev.shalltear.samplepackage", "dev.shalltear.physics3d"}))
		<< Diagnostics(service);
	ASSERT_TRUE(std::filesystem::exists(list)) << list.generic_string();
#ifdef SHALLTEAR_SHARED
	EXPECT_EQ(included(), "\"Shalltear/Audio.h\",\"Shalltear/Physics3D.h\"");
#else
	EXPECT_EQ(included(), "");
#endif

	ASSERT_TRUE(RunCompile(service, {"dev.shalltear.physics3d"})) << Diagnostics(service);
#ifdef SHALLTEAR_SHARED
	EXPECT_EQ(included(), "\"Shalltear/Physics3D.h\"");
#else
	EXPECT_EQ(included(), "");
#endif
}

TEST_F(ScriptCompileIntegrationTest, ForeignEnginePackageRefused) {
	Config projectFile;
	projectFile.SetString("Project", "name", "StaleSDKProbe");
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "StaleSDKProbe";
	options.sdkRoot = SDKRoot();
	options.editorExecutable = ScriptProjectOps::GetEditorExecutablePath();
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));

	constexpr const char* OTHER_COMMIT = "0bad0bad0bad";
	str packageDirectory;
	str staleRoot = FabricateStaleSDK(OTHER_COMMIT, packageDirectory);
	if (staleRoot.empty()) {
		GTEST_SKIP();
	}

	str ours = CppToolchain::GetEngineIdentity();
	ASSERT_NE(ours.find('|'), str::npos) << ours;
	ASSERT_EQ(ours.find(OTHER_COMMIT), str::npos);

	auto configure = [&](const str& binaryDirectory, const str& identity) {
		return RunCMake("-S \"" + Root() + "\" -B \"" + Root() + "/Library/Compile/" + binaryDirectory + "\" -G Ninja" +
						" -DCMAKE_BUILD_TYPE=" + _config + " -DCMAKE_CXX_COMPILER=\"" + CompilerPath() + "\"" +
						" -DCMAKE_MAKE_PROGRAM=\"" + NinjaPath() + "\"" + ToolchainFileArgument() + " -DShalltear_ROOT=\"" + staleRoot + "\"" +
						" -DSHALLTEAR_EXPECT_ENGINE_IDENTITY=\"" + identity + "\"");
	};

	str matching = ours.substr(0, ours.find('|') + 1) + OTHER_COMMIT;
	str control = configure("StaleMatched", matching);
	ASSERT_NE(control.find("Build files have been written"), str::npos) << control;

	str output = configure("StaleRefused", ours);

	EXPECT_EQ(output.find("Build files have been written"), str::npos) << output;
	EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(Root()) / "Library/Compile/StaleRefused/build.ninja"))
		<< output;

	EXPECT_NE(output.find(OTHER_COMMIT), str::npos) << output;
	EXPECT_NE(output.find(ours), str::npos) << output;

	EXPECT_NE(output.find(packageDirectory), str::npos) << output;
	EXPECT_NE(packageDirectory, staleRoot);
}

TEST_F(ScriptCompileIntegrationTest, ForeignAbiSdkRefused) {
	Config projectFile;
	projectFile.SetString("Project", "name", "TargetABIProbe");
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "TargetABIProbe";
	options.sdkRoot = SDKRoot();
	options.editorExecutable = ScriptProjectOps::GetEditorExecutablePath();
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));

	str sourceConfig = FileSystem::ReadTextFile(Path(SDKRoot() + "/ShalltearConfig.cmake"), MemTag::Editor);
	str ourTarget;
	ASSERT_TRUE(StaleSDK::GetCMakeString(sourceConfig, "SHALLTEAR_COMPILER_TARGET", ourTarget));
	str ourCommit;
	ASSERT_TRUE(StaleSDK::GetCMakeString(sourceConfig, "SHALLTEAR_GIT_COMMIT", ourCommit));

	constexpr const char* OTHER_TARGET = "x86_64-pc-windows-msvc";
	ASSERT_NE(ourTarget, OTHER_TARGET);

	auto fabricate = [&](const char* directory, std::string_view target, str& outRoot) {
		std::filesystem::path suggested = std::filesystem::path(Root()) / directory;
		outRoot = suggested.generic_string();
		return StaleSDK::Fabricate(SDKRoot(), suggested / "Shalltear", ourCommit, {}, target);
	};

	str ours = CppToolchain::GetEngineIdentity();
	auto configure = [&](const str& binaryDirectory, const str& packageRoot) {
		return RunCMake("-S \"" + Root() + "\" -B \"" + Root() + "/Library/Compile/" + binaryDirectory + "\" -G Ninja" +
						" -DCMAKE_BUILD_TYPE=" + _config + " -DCMAKE_CXX_COMPILER=\"" + CompilerPath() + "\"" +
						" -DCMAKE_MAKE_PROGRAM=\"" + NinjaPath() + "\"" + ToolchainFileArgument() + " -DShalltear_ROOT=\"" + packageRoot + "\"" +
						" -DSHALLTEAR_EXPECT_ENGINE_IDENTITY=\"" + ours + "\"");
	};

	str matchedRoot;
	if (!fabricate("MatchedTargetSDK", ourTarget, matchedRoot)) {
		GTEST_SKIP();
	}
	str control = configure("MatchedTarget", matchedRoot);
	ASSERT_NE(control.find("Build files have been written"), str::npos) << control;

	str otherRoot;
	ASSERT_TRUE(fabricate("OtherTargetSDK", OTHER_TARGET, otherRoot));
	str output = configure("OtherTarget", otherRoot);

	EXPECT_EQ(output.find("Build files have been written"), str::npos) << output;
	EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(Root()) / "Library/Compile/OtherTarget/build.ninja"))
		<< output;

	EXPECT_NE(output.find(OTHER_TARGET), str::npos) << output;
	EXPECT_NE(output.find(ourTarget), str::npos) << output;
}

TEST_F(ScriptCompileIntegrationTest, ScaffoldedProjectConfiguresAgainstLaterEngineVersion) {
	Config projectFile;
	projectFile.SetString("Project", "name", "VersionBumpProbe");
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "VersionBumpProbe";
	options.sdkRoot = SDKRoot();
	options.editorExecutable = ScriptProjectOps::GetEditorExecutablePath();
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));

	str ours = CppToolchain::GetEngineIdentity();
	size_t separator = ours.find('|');
	ASSERT_NE(separator, str::npos) << ours;
	constexpr const char* LATER_VERSION = "9.9.9";
	ASSERT_EQ(ours.find(LATER_VERSION), str::npos);
	str commit = ours.substr(separator + 1);
	str later = str(LATER_VERSION) + ours.substr(separator);

	str packageDirectory;
	str staleRoot = FabricateStaleSDK(commit, packageDirectory, LATER_VERSION);
	if (staleRoot.empty()) {
		GTEST_SKIP();
	}

	auto configure = [&](const str& binaryDirectory) {
		return RunCMake("-S \"" + Root() + "\" -B \"" + Root() + "/Library/Compile/" + binaryDirectory + "\" -G Ninja" +
						" -DCMAKE_BUILD_TYPE=" + _config + " -DCMAKE_CXX_COMPILER=\"" + CompilerPath() + "\"" +
						" -DCMAKE_MAKE_PROGRAM=\"" + NinjaPath() + "\"" + ToolchainFileArgument() + " -DShalltear_ROOT=\"" + staleRoot + "\"" +
						" -DSHALLTEAR_EXPECT_ENGINE_IDENTITY=\"" + later + "\"");
	};

	str output = configure("VersionBumped");
	EXPECT_NE(output.find("Build files have been written"), str::npos) << output;

	str cmake = FileSystem::ReadTextFile(Path(Root() + "/CMakeLists.txt"), MemTag::Editor);
	constexpr std::string_view Versionless = "find_package(Shalltear REQUIRED)";
	size_t at = cmake.find(Versionless);
	ASSERT_NE(at, str::npos) << cmake;
	WriteFile("CMakeLists.txt", cmake.substr(0, at) + "find_package(Shalltear " + ours.substr(0, separator) +
									" REQUIRED)" + cmake.substr(at + Versionless.size()));

	str control = configure("VersionPinned");
	EXPECT_EQ(control.find("Build files have been written"), str::npos) << control;
	EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(Root()) / "Library/Compile/VersionPinned/build.ninja"))
		<< control;
}

TEST_F(ScriptCompileIntegrationTest, StaleTreeReconfigured) {
	Config projectFile;
	projectFile.SetString("Project", "name", "StaleCacheProbe");
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "StaleCacheProbe";
	options.sdkRoot = SDKRoot();
	options.editorExecutable = ScriptProjectOps::GetEditorExecutablePath();
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));

	CompileService service;
	service.RefreshToolchain();

	str states;
	ASSERT_TRUE(RunCompileRecording(service, states)) << Diagnostics(service);

	EXPECT_EQ(states, "Configuring>Compiling>Succeeded");

	str compileDirectory = ScriptProjectOps::GetCompileDirectory(Root(), _config);
	Path cache(compileDirectory + "/CMakeCache.txt");
	ASSERT_TRUE(FileSystem::Exists(cache));

	constexpr const char* OTHER_COMMIT = "0bad0bad0bad";
	str ours = CppToolchain::GetEngineIdentity();
	ASSERT_EQ(ours.find(OTHER_COMMIT), str::npos);
	str fossil = ours.substr(0, ours.find('|') + 1) + OTHER_COMMIT;

	str text = FileSystem::ReadTextFile(cache, MemTag::Editor);
	size_t at = text.find("SHALLTEAR_EXPECT_ENGINE_IDENTITY:");
	ASSERT_NE(at, str::npos);
	size_t equals = text.find('=', at);
	ASSERT_NE(equals, str::npos);
	size_t endOfLine = text.find('\n', equals);
	ASSERT_NE(endOfLine, str::npos);
	text = text.substr(0, equals + 1) + fossil + text.substr(endOfLine);
	ASSERT_TRUE(FileSystem::WriteTextFile(cache, text));

	str refused = RunCMake("--build \"" + compileDirectory + "\"");
	ASSERT_NE(refused.find(OTHER_COMMIT), str::npos) << refused;

	ASSERT_TRUE(RunCompile(service)) << Diagnostics(service);
	EXPECT_TRUE(FileSystem::Exists(Path(ScriptProjectOps::GetModuleBinaryPath(Root(), _config))));

	str healed = FileSystem::ReadTextFile(cache, MemTag::Editor);
	EXPECT_EQ(healed.find(OTHER_COMMIT), str::npos);
	EXPECT_NE(healed.find(ours), str::npos);
}

TEST_F(ScriptCompileIntegrationTest, CacheFromMovedEngineReconfigured) {
	Config projectFile;
	projectFile.SetString("Project", "name", "MovedEngineProbe");
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "MovedEngineProbe";
	options.sdkRoot = SDKRoot();
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));

	const std::filesystem::path otherDriver = TestPlatform::MakeCompilerAlias(CompilerPath(), Root());
	ASSERT_TRUE(std::filesystem::exists(otherDriver)) << otherDriver.generic_string();
	const str compileDirectory = ScriptProjectOps::GetCompileDirectory(Root(), _config);
	i32 exitCode = -1;
	str configured = RunCMake("-S \"" + Root() + "\" -B \"" + compileDirectory + "\" -G Ninja" +
								  " -DCMAKE_BUILD_TYPE=" + _config + " -DCMAKE_CXX_COMPILER=\"" + otherDriver.generic_string() + "\"" +
								  " -DCMAKE_MAKE_PROGRAM=\"" + NinjaPath() + "\"" + ToolchainFileArgument() + " -DShalltear_ROOT=\"" + SDKRoot() + "\"" +
								  " -DSHALLTEAR_EXPECT_ENGINE_IDENTITY=\"" + CppToolchain::GetEngineIdentity() + "\"",
							  &exitCode);
	ASSERT_EQ(exitCode, 0) << configured;

	CompileService service;
	service.RefreshToolchain();
	ASSERT_TRUE(RunCompile(service)) << Diagnostics(service);
	EXPECT_TRUE(FileSystem::Exists(Path(ScriptProjectOps::GetModuleBinaryPath(Root(), _config))));
	str cache = FileSystem::ReadTextFile(Path(compileDirectory + "/CMakeCache.txt"), MemTag::Editor);
	EXPECT_EQ(cache.find(otherDriver.filename().generic_string()), str::npos);
}

TEST_F(ScriptCompileIntegrationTest, StarterComponentTurnsTarget) {
	Config projectFile;
	projectFile.SetString("Project", "name", "StarterProbe");
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "StarterProbe";
	options.sdkRoot = SDKRoot();
	options.editorExecutable = ScriptProjectOps::GetEditorExecutablePath();
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));

	CompileService service;
	service.RefreshToolchain();
	ASSERT_TRUE(RunCompile(service)) << Diagnostics(service);
	ModuleHandle module = ModuleSystem::Load(Path(ScriptProjectOps::GetModuleBinaryPath(Root(), _config)));
	ASSERT_TRUE(ModuleSystem::IsLoaded(module));
	const TypeInfo* type = TypeRegistry::FindType(StringHash("ExampleComponent"));
	ASSERT_NE(type, nullptr);

	Handle<Scene> scene = SceneSystem::CreateScene("StarterProbeScene");
	Handle<SceneObject> owner = SceneSystem::CreateObject("Owner", scene);
	Handle<SceneObject> elsewhere = SceneSystem::CreateObject("Elsewhere", scene);
	Handle<Component> component = owner.Get()->AddComponent(type);
	ASSERT_TRUE(component.IsValid());

	FrameTime clock;
	clock.deltaTime = 0.5f;
	clock.unscaledDeltaTime = 0.5f;
	auto tick = [&clock]() {
		Time::FrameScope scope{clock};
		SceneSystem::UpdateAll();
	};

	const Quat rest = owner.Get()->GetLocalRotation();
	tick();
	EXPECT_NEAR(Quat::Angle(rest, owner.Get()->GetLocalRotation()), 30.0f, 0.5f);
	EXPECT_NEAR(Quat::Angle(Quat::Identity(), elsewhere.Get()->GetLocalRotation()), 0.0f, 1.0e-3f);

	Variant wired(elsewhere);
	ASSERT_TRUE(SetField(component.Get(), type, StringHash("target"), &wired));
	const Quat ownerBefore = owner.Get()->GetLocalRotation();
	tick();
	EXPECT_NEAR(Quat::Angle(Quat::Identity(), elsewhere.Get()->GetLocalRotation()), 30.0f, 0.5f);
	EXPECT_NEAR(Quat::Angle(ownerBefore, owner.Get()->GetLocalRotation()), 0.0f, 1.0e-3f);

	SceneSystem::Destroy(elsewhere);
	SceneSystem::EndFrame();
	const Quat ownerAgain = owner.Get()->GetLocalRotation();
	tick();
	EXPECT_GT(Quat::Angle(ownerAgain, owner.Get()->GetLocalRotation()), 1.0f);
}

TEST_F(ScriptCompileIntegrationTest, CMakeMessageArrivesWhole) {
	WriteFile("CMakeLists.txt",
			  "cmake_minimum_required(VERSION 3.20)\n"
			  "project(Probe NONE)\n"
			  "message(WARNING \"the probe's test framework is missing at "
			  "'C:/some/long/path/that/wraps/External/Submodules/googletest' (set PROBE_GTEST_DIR)\")\n"
			  "message(FATAL_ERROR \"stop here, before a compiler is needed\")\n");

	CompileService service;
	EXPECT_FALSE(RunCompile(service));
	ASSERT_EQ(service.GetState(), CompileState::Failed);
	EXPECT_EQ(service.GetWarningCount(), 1u) << Diagnostics(service);
	EXPECT_EQ(service.GetErrorCount(), 1u) << Diagnostics(service);

	const CompilerDiagnostic* warning = nullptr;
	const CompilerDiagnostic* error = nullptr;
	for (const CompilerDiagnostic& diagnostic : service.GetDiagnostics()) {
		if (diagnostic.severity == CompilerDiagnosticSeverity::Warning) {
			warning = &diagnostic;
		} else if (diagnostic.severity == CompilerDiagnosticSeverity::Error) {
			error = &diagnostic;
		}
	}
	ASSERT_NE(warning, nullptr) << Diagnostics(service);
	ASSERT_NE(error, nullptr) << Diagnostics(service);

	EXPECT_NE(warning->message.find("the probe's test framework is missing at "
									"'C:/some/long/path/that/wraps/External/Submodules/googletest' (set PROBE_GTEST_DIR)"),
			  str::npos)
		<< warning->message;
	EXPECT_EQ(warning->message.find('\n'), str::npos) << warning->message;
	EXPECT_EQ(warning->line, 3u);
	EXPECT_NE(error->message.find("stop here, before a compiler is needed"), str::npos) << error->message;
	EXPECT_EQ(error->line, 4u);
}
