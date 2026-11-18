#include "TestsPCH.h"

#include "CSharpFieldScenarios.h"

#if defined(SHALLTEAR_SHARED)

#include "CSharpRuntimeFixture.h"
#include "CSharpScenarioUtil.h"
#include "TestPlatform.h"

#include "Shalltear/Core/Serialization/TextSerializer.h"

#include <format>
#include <fstream>
#include <optional>

using namespace Shalltear;
using namespace Shalltear::CSharpTests;

namespace {

	class CSharpFieldTest : public CSharpRuntimeFixture {
	protected:
		std::optional<CSharpScenarios::FieldProject> _project;

		void SetUp() override {
			CSharpRuntimeFixture::SetUp();
			_project.emplace();
			ASSERT_TRUE(Attach()) << _error;
			ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
			_hooks->takeRecords();
		}

		void TearDown() override {
			_project.reset();
			CSharpRuntimeFixture::TearDown();
		}

		void RunScenario(std::string_view name) {
			for (const CSharpScenarios::FieldScenario& scenario : CSharpScenarios::GetFieldScenarios()) {
				if (name == scenario.name) {
					scenario.run({_hooks->takeRecords, _hooks->getCounters});
					EXPECT_EQ(_log.Delivered().Count("Exception"), 0u) << _log.Delivered().Joined();
					return;
				}
			}
			ADD_FAILURE() << "no scenario " << name;
		}

		bool Logged(const char* text) {
			return _log.Delivered().Count(text) > 0;
		}
	};

	class CSharpGeneratorTest : public CSharpRuntimeFixture {
	protected:
		std::filesystem::path _root;

		void SetUp() override {
			CSharpRuntimeFixture::SetUp();
			_root = std::filesystem::temp_directory_path() /
					("shalltear_csharp_generator_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			std::filesystem::create_directories(_root);
		}

		void TearDown() override {
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
			CSharpRuntimeFixture::TearDown();
		}

		str Build(const str& source, int& outExitCode, const str& extra = {}) {
			const std::filesystem::path managed = std::filesystem::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path() / "CSharp";
			const str version = _hooks->runtimeVersion;
			const str framework = "net" + version.substr(0, version.rfind('.'));
			std::ofstream(_root / "Scratch.csproj", std::ios::binary)
				<< "<Project Sdk=\"Microsoft.NET.Sdk\">\n\t<PropertyGroup>\n\t\t<TargetFramework>" << framework
				<< "</TargetFramework>\n\t\t<Nullable>enable</Nullable>\n\t\t<NuGetAudit>false</NuGetAudit>\n\t\t<RestorePackagesPath>"
				<< (_root / "packages").generic_string() << "</RestorePackagesPath>\n\t</PropertyGroup>\n\t<ItemGroup>\n"
				<< "\t\t<Reference Include=\"Shalltear.Runtime\" HintPath=\"" << (managed / "Shalltear.Runtime.dll").generic_string()
				<< "\" Private=\"false\" />\n\t\t<Analyzer Include=\"" << (managed / "Shalltear.Generators.dll").generic_string()
				<< "\" />\n\t</ItemGroup>\n"
				<< extra << "</Project>\n";
			std::ofstream(_root / "Scripts.cs", std::ios::binary) << source;

			const str sdkRoot = _hooks->findSDKRoot();
			TestPlatform::SetEnvironment("DOTNET_ROOT", sdkRoot);
			TestPlatform::SetEnvironment("DOTNET_CLI_TELEMETRY_OPTOUT", "1");
			TestPlatform::SetEnvironment("DOTNET_NOLOGO", "1");
			TestPlatform::SetEnvironment("DOTNET_CLI_UI_LANGUAGE", "en-us");
			const std::filesystem::path dotnet = std::filesystem::path(sdkRoot) / (str("dotnet") + SLT_TEST_EXE_SUFFIX);
			return TestPlatform::RunCommand("\"" + dotnet.string() + "\" msbuild \"" + (_root / "Scratch.csproj").string() +
												"\" -restore -nologo -v:q -tl:off -clp:NoSummary -nodeReuse:false -p:UseSharedCompilation=false",
											outExitCode);
		}
	};

} // anonymous namespace

TEST_F(CSharpGeneratorTest, ScriptPathsProjectRelativeAbsentInPlayer) {
	const std::filesystem::path outside = _root.parent_path() / (_root.filename().string() + "_outside");
	std::filesystem::create_directories(outside);
	std::ofstream(outside / "Far.cs", std::ios::binary) << "using Shalltear;\nnamespace Scratch;\npublic sealed class Far : Behavior {\n}\n";
	const str source = "using Shalltear;\nnamespace Scratch;\npublic sealed class Near : Behavior {\n}\n";
	const str items = "\t<ItemGroup>\n\t\t<CompilerVisibleProperty Include=\"MSBuildProjectDirectory\" />\n\t\t<Compile Include=\"" +
					  (outside / "Far.cs").generic_string() + "\" />\n\t</ItemGroup>\n";
	const auto table = [&]() {
		str text;
		std::error_code ec;
		for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(_root / "obj", ec)) {
			if (entry.path().filename() == "TypeTable.g.cs") {
				std::ifstream file(entry.path(), std::ios::binary);
				text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			}
		}
		return text;
	};

	int exitCode = 0;
	str output = Build(source, exitCode, items + "\t<PropertyGroup>\n\t\t<EmitCompilerGeneratedFiles>true</EmitCompilerGeneratedFiles>\n\t</PropertyGroup>\n");
	ASSERT_EQ(exitCode, 0) << output;
	const str editor = table();
	EXPECT_NE(editor.find("Scratch.Near(), \"Scripts.cs\""), str::npos) << editor;
	EXPECT_NE(editor.find("Scratch.Far(), null"), str::npos) << editor;

	std::error_code ec;
	std::filesystem::remove_all(_root / "obj", ec);
	output = Build(source, exitCode,
				   items + "\t<PropertyGroup>\n\t\t<EmitCompilerGeneratedFiles>true</EmitCompilerGeneratedFiles>\n"
						   "\t\t<DefineConstants>$(DefineConstants);SHALLTEAR_PLAYER</DefineConstants>\n\t</PropertyGroup>\n");
	ASSERT_EQ(exitCode, 0) << output;
	const str player = table();
	EXPECT_NE(player.find("Scratch.Near(), null"), str::npos) << player;
	EXPECT_EQ(player.find("Scripts.cs"), str::npos) << player;
	std::filesystem::remove_all(outside, ec);
}

TEST_F(CSharpGeneratorTest, ReleasePlayerTableHashedLoadsSceneText) {
	const str source = "using Shalltear;\nnamespace Scratch;\npublic enum Gear { Low = 2, High = 5 }\n[System.Serializable]\npublic struct Tuning {\n\tpublic float grip;\n}\n"
					   "public sealed class Racer : Behavior {\n\t[Tooltip(\"how fast\")]\n\tpublic float speed = 1.5f;\n"
					   "\t[Header(\"Driving\")]\n\tpublic Gear gear = Gear.Low;\n\tpublic Tuning tuning;\n\tpublic Racer rival;\n"
					   "\tpublic System.Collections.Generic.List<Racer> pack = new();\n}\n";
	const str head = "\t<ItemGroup>\n\t\t<CompilerVisibleProperty Include=\"ShalltearScriptNames\" />\n\t</ItemGroup>\n"
					 "\t<PropertyGroup>\n\t\t<EmitCompilerGeneratedFiles>true</EmitCompilerGeneratedFiles>\n";
	const auto table = [&]() {
		str text;
		std::error_code ec;
		for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(_root / "obj", ec)) {
			if (entry.path().filename() == "TypeTable.g.cs") {
				std::ifstream file(entry.path(), std::ios::binary);
				text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			}
		}
		return text;
	};
	const char* names[] = {"\"Scratch.Racer\"",
						   "\"Scratch.Gear\"",
						   "\"Scratch.Tuning\"",
						   "\"speed\"",
						   "\"grip\"",
						   "\"High\"",
						   "\"Scratch.Racer.rival\"",
						   "\"Scratch.Racer.pack\""};

	int exitCode = 0;
	str output = Build(source, exitCode, head + "\t</PropertyGroup>\n");
	ASSERT_EQ(exitCode, 0) << output;
	const str editor = table();
	for (const char* name : names) {
		EXPECT_NE(editor.find(name), str::npos) << name;
	}
	EXPECT_NE(editor.find("how fast"), str::npos) << editor;
	EXPECT_NE(editor.find("\"Driving\""), str::npos) << editor;
	EXPECT_NE(editor.find("static class TypeTable"), str::npos);
	EXPECT_EQ(editor.find("public static class TypeTable"), str::npos);
	EXPECT_NE(editor.find("internal static void Register()"), str::npos);

	std::error_code ec;
	std::filesystem::remove_all(_root / "obj", ec);
	output = Build(source, exitCode, head + "\t\t<DefineConstants>$(DefineConstants);SHALLTEAR_PLAYER</DefineConstants>\n\t</PropertyGroup>\n");
	ASSERT_EQ(exitCode, 0) << output;
	const str development = table();
	for (const char* name : names) {
		EXPECT_NE(development.find(name), str::npos) << name;
	}
	EXPECT_EQ(development.find("how fast"), str::npos) << development;
	EXPECT_EQ(development.find("\"Driving\""), str::npos) << development;
	EXPECT_NE(development.find("public static class TypeTable"), str::npos);
	EXPECT_NE(development.find("public static void Register()"), str::npos);

	std::filesystem::remove_all(_root / "obj", ec);
	output = Build(source, exitCode,
				   head + "\t\t<DefineConstants>$(DefineConstants);SHALLTEAR_PLAYER</DefineConstants>\n"
						  "\t\t<ShalltearScriptNames>hashed</ShalltearScriptNames>\n\t</PropertyGroup>\n");
	ASSERT_EQ(exitCode, 0) << output;
	const str release = table();
	ASSERT_FALSE(release.empty());
	for (const char* name : names) {
		EXPECT_EQ(release.find(name), str::npos) << name;
	}
	EXPECT_EQ(release.find("how fast"), str::npos) << release;
	EXPECT_EQ(release.find("\"Driving\""), str::npos) << release;
	EXPECT_NE(release.find(std::format("{:016x}UL", StringHash("Scratch.Racer").GetValue())), str::npos) << release;

	str library;
	for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(_root / "bin", ec)) {
		if (entry.path().filename() == "Scratch.dll") {
			library = entry.path().generic_string();
		}
	}
	ASSERT_FALSE(library.empty());
	ASSERT_TRUE(Attach()) << _error;
	SceneSystem::Initialize();
	const SltString paths[] = {SltString{library.data(), static_cast<int32_t>(library.size())}};
	ASSERT_EQ(_hooks->getManaged().loadUserAssemblies(paths, 1), SLT_OK) << _log.Delivered().Joined();
	const TypeInfo* racer = TypeRegistry::FindType(StringHash("Scratch.Racer"));
	ASSERT_NE(racer, nullptr) << _log.Delivered().Joined();
	EXPECT_EQ(str(racer->displayName), std::format("{:016x}", StringHash("Scratch.Racer").GetValue()));
	const FieldInfo* speed = FindField(racer, StringHash("speed"));
	ASSERT_NE(speed, nullptr);
	EXPECT_EQ(speed->displayName, nullptr);
	const TypeInfo* gear = TypeRegistry::FindType(StringHash("Scratch.Gear"));
	ASSERT_NE(gear, nullptr);
	ASSERT_EQ(gear->enumConstantCount, 2u);
	EXPECT_EQ(gear->enumConstants[1].displayName, nullptr);
	{
		Handle<Scene> scene = SceneSystem::CreateScene("Track");
		Handle<Component> made = SceneSystem::CreateObject("Car", scene)->AddComponent(racer);
		ASSERT_TRUE(made.IsValid()) << _log.Delivered().Joined();
		EXPECT_FLOAT_EQ(CSharpScenarios::Read<f32>(made, "speed"), 1.5f);
		EXPECT_EQ(CSharpScenarios::Read<i64>(made, "gear"), 2);
		ASSERT_TRUE(TextSerializer::Deserialize(made.Get(), racer, R"({"speed": 4.25, "gear": "High", "tuning": {"grip": 0.5}})"));
		EXPECT_FLOAT_EQ(CSharpScenarios::Read<f32>(made, "speed"), 4.25f);
		EXPECT_EQ(CSharpScenarios::Read<i64>(made, "gear"), 5);
		const str written = TextSerializer::Serialize(made.Get(), racer);
		EXPECT_NE(written.find("0.5"), str::npos) << written;
		SceneSystem::UnloadScene(scene);
		SceneSystem::EndFrame();
	}
	EXPECT_EQ(_hooks->unloadUserAssemblies(), SLT_OK);
	EXPECT_EQ(TypeRegistry::FindType(StringHash("Scratch.Racer")), nullptr);
	SceneSystem::Terminate();
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Warn), 0u) << _log.Delivered().Joined();
}

TEST_F(CSharpGeneratorTest, PlayerTableExistsWithoutComponents) {
	const str source = "namespace Scratch;\npublic sealed class Helper {\n\tpublic int uses;\n}\n";
	const str head = "\t<PropertyGroup>\n\t\t<EmitCompilerGeneratedFiles>true</EmitCompilerGeneratedFiles>\n";
	const auto table = [&]() {
		str text;
		std::error_code ec;
		for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(_root / "obj", ec)) {
			if (entry.path().filename() == "TypeTable.g.cs") {
				std::ifstream file(entry.path(), std::ios::binary);
				text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			}
		}
		return text;
	};

	int exitCode = 0;
	str output = Build(source, exitCode, head + "\t</PropertyGroup>\n");
	ASSERT_EQ(exitCode, 0) << output;
	EXPECT_TRUE(table().empty());

	std::error_code ec;
	std::filesystem::remove_all(_root / "obj", ec);
	output = Build(source, exitCode, head + "\t\t<DefineConstants>$(DefineConstants);SHALLTEAR_PLAYER</DefineConstants>\n\t</PropertyGroup>\n");
	ASSERT_EQ(exitCode, 0) << output;
	const str player = table();
	EXPECT_NE(player.find("public static class TypeTable"), str::npos) << player;
	EXPECT_NE(player.find("public static void Register()"), str::npos) << player;
	EXPECT_EQ(player.find("Register<"), str::npos) << player;
}

TEST_F(CSharpGeneratorTest, TooManyFieldsFailTheCompile) {
	str source = "using Shalltear;\nnamespace Scratch;\npublic sealed class Small : Behavior {\n\tpublic float speed;\n}\n"
				 "public sealed class Huge : Behavior {\n";
	for (u32 index = 0; index < 321; ++index) {
		source += "\tpublic Vec4 plane" + std::to_string(index) + ";\n";
	}
	source += "}\npublic class Half : Behavior {\n";
	for (u32 index = 0; index < 200; ++index) {
		source += "\tpublic Quat turn" + std::to_string(index) + ";\n";
	}
	source += "}\npublic sealed class Heir : Half {\n";
	for (u32 index = 0; index < 121; ++index) {
		source += "\tpublic Vec4 plane" + std::to_string(index) + ";\n";
	}
	source += "}\n";

	int exitCode = 0;
	const str output = Build(source, exitCode);
	EXPECT_NE(exitCode, 0) << output;
	EXPECT_NE(output.find("error SLT1004: 'Scratch.Huge' has too many serialized fields (963 of 960 slots). Remove some."), str::npos) << output;
	EXPECT_NE(output.find("error SLT1004: 'Scratch.Heir' has too many serialized fields (963 of 960 slots). Remove some."), str::npos) << output;
	EXPECT_EQ(output.find("Scratch.Small"), str::npos) << output;
	EXPECT_EQ(output.find("Scratch.Half"), str::npos) << output;
}

TEST_F(CSharpGeneratorTest, FieldsWithinTheSpanCompile) {
	str source = "using Shalltear;\nnamespace Scratch;\npublic sealed class Wide : Behavior {\n\t[Reflect] Quat hidden;\n";
	for (u32 index = 0; index < 319; ++index) {
		source += "\tpublic Vec4 plane" + std::to_string(index) + ";\n";
	}
	source += "\tpublic Quat Sum() => hidden;\n}\n";

	int exitCode = 0;
	const str output = Build(source, exitCode);
	EXPECT_EQ(exitCode, 0) << output;
	EXPECT_EQ(output.find("SLT1004"), str::npos) << output;
	EXPECT_TRUE(std::filesystem::exists(_root / "bin" / "Debug")) << output;
}

TEST_F(CSharpGeneratorTest, NullOperatorsOnEngineObjectsWarn) {
	const str source = "using Shalltear;\nnamespace Scratch;\npublic sealed class Seeker : Behavior {\n"
					   "\tSceneObject? _target;\n\tobject? _plain;\n"
					   "\tpublic string? A() => _target?.Name;\n"
					   "\tpublic SceneObject B(SceneObject other) => _target ?? other;\n"
					   "\tpublic void C(SceneObject other) { _target ??= other; }\n"
					   "\tpublic bool D() => _target is null;\n"
					   "\tpublic bool E() => _target is not null;\n"
					   "\tpublic bool F(Seeker? other) => other?.D() ?? false;\n"
					   "\tpublic bool G<T>(T? found) where T : Component => found is null;\n"
					   "\tpublic bool H() => _target == null || !_target || _plain is null || _plain?.ToString() == null;\n"
					   "\tpublic bool I(SceneObject other) => other is SceneObject same && (_plain ?? same) != null;\n"
					   "\tpublic bool J(EngineObject? any) => any is null;\n"
					   "}\n";

	int exitCode = 0;
	const str output = Build(source, exitCode);
	EXPECT_EQ(exitCode, 0) << output;
	const auto warned = [&output](u32 line, std::string_view used, std::string_view type) {
		const str position = std::format("Scripts.cs({},", line);
		const str message = std::format("warning SLT2001: '{}' misses a destroyed {}. Compare with == null.", used, type);
		for (size_t at = output.find(position); at != str::npos; at = output.find(position, at + 1)) {
			const size_t end = output.find('\n', at);
			if (std::string_view(output).substr(at, end - at).find(message) != std::string_view::npos) {
				return true;
			}
		}
		return false;
	};
	EXPECT_TRUE(warned(6, "?.", "SceneObject")) << output;
	EXPECT_TRUE(warned(7, "??", "SceneObject")) << output;
	EXPECT_TRUE(warned(8, "??=", "SceneObject")) << output;
	EXPECT_TRUE(warned(9, "is null", "SceneObject")) << output;
	EXPECT_TRUE(warned(10, "is not null", "SceneObject")) << output;
	EXPECT_TRUE(warned(11, "?.", "Seeker")) << output;
	EXPECT_TRUE(warned(12, "is null", "T")) << output;
	EXPECT_EQ(output.find("Scripts.cs(13,"), str::npos) << output;
	EXPECT_EQ(output.find("Scripts.cs(14,"), str::npos) << output;
	EXPECT_TRUE(warned(15, "is null", "EngineObject")) << output;
}

TEST_F(CSharpGeneratorTest, PublicFieldsOfUnsupportedTypesSkipped) {
	const str source =
		"using System;\n"
		"using System.Collections.Generic;\n"
		"using Shalltear;\n"
		"namespace Scratch;\n"
		"[Serializable] public sealed class Node { public int id; public Node? next; }\n"
		"[Serializable] public struct Bare { int kept; public Bare(int value) { kept = value; } public readonly int Kept => kept; }\n"
		"public enum Mood { Calm, Loud }\n"
		"public sealed class Mixed : Behavior {\n"
		"\tpublic Dictionary<int, int>? map;\n"
		"\tpublic List<List<int>>? nested;\n"
		"\tpublic int[,]? grid;\n"
		"\tpublic Bare bare;\n"
		"\tpublic Action? callback;\n"
		"\tpublic object? anything;\n"
		"\t[NonSerialized] public int ignored;\n"
		"\tpublic static int shared;\n"
		"\tpublic readonly int frozen = 2;\n"
		"\tpublic const int FIXED = 3;\n"
		"\tpublic Node? chain;\n"
		"\tpublic List<Mood>? moods;\n"
		"\tpublic string? title;\n"
		"}\n";

	int exitCode = 0;
	const str output = Build(source, exitCode);
	EXPECT_EQ(exitCode, 0) << output;
	EXPECT_EQ(output.find("SLT1002"), str::npos) << output;
	EXPECT_EQ(output.find("warning"), str::npos) << output;
}

TEST_F(CSharpGeneratorTest, ReflectOnUnsupportedTypeFailsCompile) {
	const str source =
		"using System;\n"
		"using System.Collections.Generic;\n"
		"using Shalltear;\n"
		"namespace Scratch;\n"
		"[Serializable] public sealed class Node { public int id; public Node? next; }\n"
		"[Serializable] public struct Bare { int kept; public Bare(int value) { kept = value; } public readonly int Kept => kept; }\n"
		"public enum Mood { Calm, Loud }\n"
		"public sealed class Mixed : Behavior {\n"
		"\t[Reflect] Dictionary<int, int>? wanted;\n"
		"\t[Reflect] Action? callback;\n"
		"\t[Reflect] List<List<int>>? nested;\n"
		"\t[Reflect] string? fine;\n"
		"\tpublic int Use() => (wanted?.Count ?? 0) + (callback == null ? 0 : 1) + (nested?.Count ?? 0) + (fine?.Length ?? 0);\n"
		"}\n";

	int exitCode = 0;
	const str output = Build(source, exitCode);
	EXPECT_NE(exitCode, 0) << output;
	const auto refused = [&output](u32 line, std::string_view field, std::string_view type) {
		const str position = std::format("Scripts.cs({},", line);
		const str message = std::format("error SLT1002: '{}' has [Reflect] but type '{}' can't be serialized. Use a supported type (or remove [Reflect]).",
										field, type);
		for (size_t at = output.find(position); at != str::npos; at = output.find(position, at + 1)) {
			const size_t end = output.find('\n', at);
			if (std::string_view(output).substr(at, end - at).find(message) != std::string_view::npos) {
				return true;
			}
		}
		return false;
	};
	EXPECT_TRUE(refused(9, "Scratch.Mixed.wanted", "Dictionary<int, int>?")) << output;
	EXPECT_TRUE(refused(10, "Scratch.Mixed.callback", "Action?")) << output;
	EXPECT_TRUE(refused(11, "Scratch.Mixed.nested", "List<List<int>>?")) << output;
	EXPECT_EQ(output.find("'Scratch.Mixed.fine'"), str::npos) << output;
}

#elif defined(SLT_TEST_CSHARP_AOT)

#include "CSharpConformance.h"
#include "TestHelperProcess.h"
#include "TestPlatform.h"

#include "BindingProbe.h"
#include "NativeAOTHost.h"
#include "ScriptRuntime.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Reflection.h"

#include <cstdio>
#include <filesystem>

using namespace Shalltear;

int Shalltear::TestHelpers::RunCSharpAOTFields(const char* library) {
	JobSystem::Initialize();
	LogManager::Initialize({true, false});
	TypeRegistry::RegisterPendingTypes();
	int result = 0;
	{
		const CSharpScenarios::FieldProject project;
		str error;
		if (!library || !CSharpImpl::NativeAOTHost::Start(library, error)) {
			std::printf("START FAILED %s\n", error.c_str());
			result = 2;
		} else {
			const testing::TestResult& failures = testing::UnitTest::GetInstance()->ad_hoc_test_result();
			for (const CSharpScenarios::FieldScenario& scenario : CSharpScenarios::GetFieldScenarios()) {
				CSharpImpl::BindingProbe::TakeRecords();
				const int before = failures.total_part_count();
				scenario.run({&CSharpImpl::BindingProbe::TakeRecords, &CSharpImpl::ManagedTypes::GetCounters});
				std::printf("\nSCENARIO %s %s\n", scenario.name, failures.total_part_count() == before ? "ok" : "FAILED");
				std::fflush(stdout);
			}
		}
	}
	CSharpImpl::ScriptRuntime::Detach();
	LogManager::Terminate();
	JobSystem::Terminate();
	std::printf("DONE\n");
	return result;
}

namespace {

	class CSharpFieldTest : public ::testing::Test {
	protected:
		static const str& Output() {
			static const str output = [] {
				const std::filesystem::path library =
					std::filesystem::path(FileSystem::GetExecutablePath().ToString()).parent_path() / "CSharp" / "aot" / "Shalltear.ConformanceAOT.dll";
				int exitCode = -1;
				str text = TestPlatform::RunCommand("\"" + FileSystem::GetExecutablePath().ToString() + "\" " + TestHelpers::CSHARP_AOT_FIELDS_FLAG +
														" \"" + library.generic_string() + "\"",
													exitCode);
				return "\n" + text + "\nEXIT " + std::to_string(exitCode) + "\n";
			}();
			return output;
		}

		void RunScenario(std::string_view name) {
			const str& output = Output();
			ASSERT_NE(output.find("\nDONE"), str::npos) << output;
			ASSERT_NE(output.find("\nEXIT 0\n"), str::npos) << output;
			EXPECT_NE(output.find("\nSCENARIO " + str(name) + " ok"), str::npos) << output;
		}

		bool Logged(const char* text) {
			return Output().find(text) != str::npos;
		}
	};

} // anonymous namespace

#endif

#if defined(SHALLTEAR_SHARED) || defined(SLT_TEST_CSHARP_AOT)

TEST_F(CSharpFieldTest, DefaultsComeFromInitializers) {
	RunScenario("DefaultsComeFromInitializers");
}

TEST_F(CSharpFieldTest, AttributesReachFieldInfo) {
	RunScenario("AttributesReachFieldInfo");
}

TEST_F(CSharpFieldTest, SceneRoundTrip) {
	RunScenario("SceneRoundTrip");
}

TEST_F(CSharpFieldTest, BoxedEditReachesScript) {
	RunScenario("BoxedEditReachesScript");
}

TEST_F(CSharpFieldTest, ScriptChangeReachesReflection) {
	RunScenario("ScriptChangeReachesReflection");
}

TEST_F(CSharpFieldTest, PrefabOverrideRoundTrip) {
	RunScenario("PrefabOverrideRoundTrip");
}

TEST_F(CSharpFieldTest, InstantiateCopiesFields) {
	RunScenario("InstantiateCopiesFields");
}

TEST_F(CSharpFieldTest, InheritedFieldsFollowParents) {
	RunScenario("InheritedFieldsFollowParents");
}

TEST_F(CSharpFieldTest, SyncsStayBounded) {
	RunScenario("SyncsStayBounded");
}

TEST_F(CSharpFieldTest, InspectorEditReachesScript) {
	RunScenario("InspectorEditReachesScript");
}

TEST_F(CSharpFieldTest, InspectorShowsAValueTheScriptChanged) {
	RunScenario("InspectorShowsAValueTheScriptChanged");
}

TEST_F(CSharpFieldTest, InspectorRowsEditScriptFields) {
	RunScenario("InspectorRowsEditScriptFields");
}

TEST_F(CSharpFieldTest, PlaySnapshotKeepsValues) {
	RunScenario("PlaySnapshotKeepsValues");
}

TEST_F(CSharpFieldTest, DescribeHolder) {
	RunScenario("DescribeHolder");
}

TEST_F(CSharpFieldTest, KindDefaultsReachBothSides) {
	RunScenario("KindDefaultsReachBothSides");
}

TEST_F(CSharpFieldTest, KindsSurviveSaveAndLoad) {
	RunScenario("KindsSurviveSaveAndLoad");
}

TEST_F(CSharpFieldTest, KindsSurviveInstantiate) {
	RunScenario("KindsSurviveInstantiate");
}

TEST_F(CSharpFieldTest, EditorWritesReachEveryKind) {
	RunScenario("EditorWritesReachEveryKind");
}

TEST_F(CSharpFieldTest, UnchangedStringIsNotPushed) {
	RunScenario("UnchangedStringIsNotPushed");
}

TEST_F(CSharpFieldTest, InheritedTextFieldsSyncApart) {
	RunScenario("InheritedTextFieldsSyncApart");
}

TEST_F(CSharpFieldTest, NullClassValueIsReplaced) {
	RunScenario("NullClassValueIsReplaced");
}

TEST_F(CSharpFieldTest, WideValueStaysAlignedInAList) {
	RunScenario("WideValueStaysAlignedInAList");
}

TEST_F(CSharpFieldTest, WrongComponentTypePullsNull) {
	RunScenario("WrongComponentTypePullsNull");
	EXPECT_TRUE(Logged("'Shalltear.Conformance.Pointer.aimed' got Silent (read as null). Assign a Carrier"));
	EXPECT_TRUE(Logged("'Shalltear.Conformance.Pointer.aimed' got another object (read as null). Assign a Carrier"));
	EXPECT_TRUE(Logged("'Shalltear.Conformance.Pointer.many' got Silent (read as null). Assign a Carrier"));
}

TEST_F(CSharpFieldTest, ReferenceToDestroyedObjectReadsNull) {
	RunScenario("ReferenceToDestroyedObjectReadsNull");
}

TEST_F(CSharpFieldTest, KindOverridesSurvivePrefabReload) {
	RunScenario("KindOverridesSurvivePrefabReload");
}

TEST_F(CSharpFieldTest, InspectorDrawsEveryKind) {
	RunScenario("InspectorDrawsEveryKind");
}

TEST_F(CSharpFieldTest, EnumShowsConstantNames) {
	RunScenario("EnumShowsConstantNames");
}

TEST_F(CSharpFieldTest, ReferencesArePickedInInspector) {
	RunScenario("ReferencesArePickedInInspector");
}

TEST_F(CSharpFieldTest, ListResizesFromInspector) {
	RunScenario("ListResizesFromInspector");
}

TEST_F(CSharpFieldTest, NestedValueRoundTrip) {
	RunScenario("NestedValueRoundTrip");
}

#ifndef SHALLTEAR_SHARED

TEST_F(CSharpFieldTest, AnimationClipDrivesField) {
	RunScenario("AnimationClipDrivesField");
}

#endif

#endif
