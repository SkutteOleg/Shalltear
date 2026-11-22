#include "TestsPCH.h"

#if defined(SHALLTEAR_SHARED)

#include "CSharpRuntimeFixture.h"
#include "EditorTestHooks.h"
#include "TestPlatform.h"

#include "Shalltear/Core/BuildInfo.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"

#include "Shalltear/Editor/EditorConsole.h"
#include "Shalltear/Editor/ScriptProjectOps.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <thread>

using namespace Shalltear;
using namespace Shalltear::CSharpEditor;
using namespace Shalltear::CSharpTests;

namespace {

	namespace fs = std::filesystem;

	constexpr const char* MIGRATION_SCRIPT = R"(using System;
using Shalltear;

namespace MigrationTests;

[FormerlySerializedAs("MigrationTests.OldMode")]
public enum DriveMode {
	Standard = 0,
	[FormerlySerializedAs("Turbo")]
	Boosted = 1,
}

[Serializable]
public struct EngineSpec {
	[FormerlySerializedAs("oldTorque")]
	public float torque;
}

public sealed class RequiredBase : Behavior {
	public int tag = 11;
}

[FormerlySerializedAs("MigrationTests.LegacyVehicle")]
[RequireComponent(typeof(RequiredBase))]
[DisallowMultipleComponent]
[AddComponentMenu("Gameplay/Vehicle Controller", 10)]
public sealed class VehicleController : Behavior {
	[FormerlySerializedAs("oldSpeed")]
	[Space(12f)]
	public float topSpeed = 10f;

	[Multiline(4)]
	public string summary = "";

	[TextArea(2, 6)]
	public string notes = "";

	[Reflect]
	[FormerlySerializedAs("oldBoost")]
	public float BoostCapacity { get; set; } = 7.5f;

	public DriveMode mode = DriveMode.Standard;
	public EngineSpec spec;
}
)";

	constexpr const char* INVALID_COMPUTED_PROPERTY = R"(using Shalltear;

namespace MigrationTests;

public sealed class BadPropertyScript : Behavior {
	[Reflect]
	public float Computed => 42f;
}
)";

	str ReadTextFile(const fs::path& file) {
		std::ifstream stream(file, std::ios::binary);
		return str(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
	}

	class CSharpMigrationTest : public CSharpRuntimeFixture {
	protected:
		const EditorTestHooks* _editor = nullptr;
		fs::path _root;
		fs::path _project;

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
			ASSERT_TRUE(Attach()) << _error;

			_root = fs::temp_directory_path() /
					("slt_cs_migrate_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			_project = _root / "MigrateGame";
			fs::create_directories(_project / "Assets" / "Scripts");
		}

		void TearDown() override {
			if (_editor) {
				_editor->shutdownReload();
			}
			SceneSystem::Terminate();
			EditorConsole::Terminate();
			FileSystem::Unmount("engine");
			std::error_code ec;
			fs::remove_all(_root, ec);
			CSharpRuntimeFixture::TearDown();
		}

		CompileState CompileScript(const str& fileName, const str& source, fs::path& outAssembly) {
			const str sdk = _hooks->findSDKRoot();
			if (sdk.empty()) {
				return CompileState::Failed;
			}
			CSharpProjectFiles::Options files = _editor->getDefaultProjectOptions(_project.generic_string(), sdk);
			files.projectName = "MigrateGame";
			CSharpProjectFiles::Result written;
			if (!_editor->writeProjectFiles(files, written)) {
				return CompileState::Failed;
			}
			std::error_code ec;
			fs::remove_all(_project / "Assets" / "Scripts", ec);
			fs::create_directories(_project / "Assets" / "Scripts");
			std::ofstream(_project / "Assets" / "Scripts" / fileName, std::ios::binary) << source;
			CompileOptions options;
			options.projectPath = _project.generic_string();
			options.projectFile = "MigrateGame.Scripts.csproj";
			options.muxer = (fs::path(sdk) / _editor->muxerName).generic_string();
			options.pinStamp = "migrate";
			options.buildServers = false;
			_editor->configureCompile(options);
			if (!_editor->requestCompile()) {
				return CompileState::Failed;
			}
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
			while (_editor->getCompile().IsBusy() && std::chrono::steady_clock::now() < deadline) {
				_editor->pumpCompile();
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			outAssembly = fs::path(_editor->getCompileOutput());
			return _editor->getCompile().GetState();
		}
	};

} // anonymous namespace

TEST_F(CSharpMigrationTest, FormerlySerializedAsAutoPropertiesAndAttributes) {
	if (_hooks->findSDKRoot().empty()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	fs::path assembly;
	ASSERT_EQ(CompileScript("VehicleController.cs", MIGRATION_SCRIPT, assembly), CompileState::Succeeded)
		<< _log.Delivered().Joined();

	const str pathStr = assembly.generic_string();
	const SltString paths[] = {SltString{pathStr.data(), static_cast<int32_t>(pathStr.size())}};
	ASSERT_EQ(_hooks->getManaged().loadUserAssemblies(paths, 1), SLT_OK);

	const TypeInfo* vehicleType = TypeRegistry::FindType(StringHash("MigrationTests.VehicleController"));
	ASSERT_NE(vehicleType, nullptr);

	const FieldInfo* boostField = FindField(vehicleType, StringHash("BoostCapacity"));
	ASSERT_NE(boostField, nullptr);
	const FieldInfo* speedField = FindField(vehicleType, StringHash("topSpeed"));
	ASSERT_NE(speedField, nullptr);
	const FieldInfo* summaryField = FindField(vehicleType, StringHash("summary"));
	ASSERT_NE(summaryField, nullptr);
	const FieldInfo* notesField = FindField(vehicleType, StringHash("notes"));
	ASSERT_NE(notesField, nullptr);

	EXPECT_NE(FindTypeAttribute(vehicleType, StringHash("DisallowMultipleComponent")), nullptr);
	EXPECT_NE(FindTypeAttribute(vehicleType, StringHash("RequireComponent")), nullptr);
	const AttributeInfo* menuAttr = FindTypeAttribute(vehicleType, StringHash("AddComponentMenu"));
	ASSERT_NE(menuAttr, nullptr);
	EXPECT_EQ(menuAttr->value.As<str>(), "Gameplay/Vehicle Controller");

	EXPECT_NE(FindFieldAttribute(speedField, StringHash("Space")), nullptr);
	EXPECT_NE(FindFieldAttribute(summaryField, StringHash("Multiline")), nullptr);
	EXPECT_NE(FindFieldAttribute(notesField, StringHash("TextArea")), nullptr);
	const AttributeInfo* formerBoost = FindFieldAttribute(boostField, StringHash("FormerlySerializedAs"));
	ASSERT_NE(formerBoost, nullptr);
	EXPECT_EQ(formerBoost->value.As<str>(), "oldBoost");

	// verify [Reflect] auto-property default on live component
	Handle<Scene> scene = SceneSystem::CreateScene("MigrationWorld");
	Handle<SceneObject> actor = SceneSystem::CreateObject("Car", scene);
	ASSERT_TRUE(actor.IsValid());
	Handle<Component> first = actor->AddComponent(vehicleType);
	ASSERT_TRUE(first.IsValid());

	Variant initialBoost;
	ASSERT_TRUE(GetField(first.Get(), vehicleType, StringHash("BoostCapacity"), &initialBoost));
	EXPECT_FLOAT_EQ(initialBoost.As<f32>(), 7.5f);

	// verify round-trip loading from legacy serialized type/field/enum names
	const str legacyScene = std::format(
		"{{\n"
		"\t\"_type\": \"Scene\",\n"
		"\t\"formatVersion\": 1,\n"
		"\t\"engineHash\": \"{:016x}\",\n"
		"\t\"name\": \"LegacyWorld\",\n"
		"\t\"nextLocalId\": 3,\n"
		"\t\"objects\": [\n"
		"\t\t{{\n"
		"\t\t\t\"id\": 1,\n"
		"\t\t\t\"parent\": 0,\n"
		"\t\t\t\"type\": \"{:016x}\",\n"
		"\t\t\t\"fields\": {{\"_type\": \"SceneObject\", \"name\": \"OldCar\", \"_activeSelf\": true, "
		"\"_localPosition\": [0, 0, 0], \"_localRotation\": [0, 0, 0, 1], \"_localScale\": [1, 1, 1]}},\n"
		"\t\t\t\"components\": [\n"
		"\t\t\t\t{{\n"
		"\t\t\t\t\t\"id\": 2,\n"
		"\t\t\t\t\t\"type\": \"{:016x}\",\n"
		"\t\t\t\t\t\"fields\": {{\n"
		"\t\t\t\t\t\t\"_type\": \"MigrationTests.LegacyVehicle\",\n"
		"\t\t\t\t\t\t\"_enabledSelf\": true,\n"
		"\t\t\t\t\t\t\"oldSpeed\": 88.5,\n"
		"\t\t\t\t\t\t\"oldBoost\": 42.25,\n"
		"\t\t\t\t\t\t\"mode\": \"Turbo\",\n"
		"\t\t\t\t\t\t\"spec\": {{\"oldTorque\": 333.0}}\n"
		"\t\t\t\t\t}}\n"
		"\t\t\t\t}}\n"
		"\t\t\t]\n"
		"\t\t}}\n"
		"\t]\n"
		"}}\n",
		BuildInfo::GetEngineHash(), StringHash("SceneObject").GetValue(), StringHash("MigrationTests.LegacyVehicle").GetValue());

	const fs::path legacyScenePath = _root / "LegacyWorld.scene";
	std::ofstream(legacyScenePath, std::ios::binary) << legacyScene;
	Handle<Scene> migrated = SceneSystem::LoadScene(Path(legacyScenePath.generic_string()));
	ASSERT_TRUE(migrated.IsValid());

	Handle<SceneObject> oldCar = migrated->FindObjectByName("OldCar");
	ASSERT_TRUE(oldCar.IsValid());
	Handle<Component> migratedComp;
	for (u32 i = 0; i < oldCar->GetComponentCount(); ++i) {
		Handle<Component> candidate = oldCar->GetComponentAt(i);
		if (candidate.IsValid() && candidate->GetType() == vehicleType) {
			migratedComp = candidate;
			break;
		}
	}
	ASSERT_TRUE(migratedComp.IsValid());

	Variant loadedSpeed;
	ASSERT_TRUE(GetField(migratedComp.Get(), vehicleType, StringHash("topSpeed"), &loadedSpeed));
	EXPECT_FLOAT_EQ(loadedSpeed.As<f32>(), 88.5f);

	Variant loadedBoost;
	ASSERT_TRUE(GetField(migratedComp.Get(), vehicleType, StringHash("BoostCapacity"), &loadedBoost));
	EXPECT_FLOAT_EQ(loadedBoost.As<f32>(), 42.25f);
}

TEST_F(CSharpMigrationTest, NonAutoPropertyWithReflectReportsDiagnostic) {
	if (_hooks->findSDKRoot().empty()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	fs::path assembly;
	EXPECT_EQ(CompileScript("BadPropertyScript.cs", INVALID_COMPUTED_PROPERTY, assembly), CompileState::Failed);

	bool sawDiagnostic = false;
	for (const CSharpDiagnostics::Diagnostic& diag : _editor->getCompile().GetDiagnostics()) {
		if (diag.code == "SLT1007" || diag.message.find("SLT1007") != str::npos) {
			sawDiagnostic = true;
			break;
		}
	}
	EXPECT_TRUE(sawDiagnostic) << _log.Delivered().Joined();
}

TEST_F(CSharpMigrationTest, RoslynRefactorRenamesClassAndInsertsFormerlySerializedAs) {
	const str sdk = _hooks->findSDKRoot();
	if (sdk.empty()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	const fs::path managedDir = fs::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path() / "CSharp";
	const fs::path refactorDll = managedDir / "Refactor" / "Shalltear.Refactor.dll";
	ASSERT_TRUE(fs::is_regular_file(refactorDll)) << refactorDll;

	const fs::path fileA = _project / "Assets" / "Scripts" / "Booster.cs";
	const fs::path fileB = _project / "Assets" / "Scripts" / "Caller.cs";
	std::ofstream(fileA, std::ios::binary)
		<< "using System.Numerics;\nusing Shalltear;\n\nnamespace Demo;\n\npublic sealed class Booster : Behavior {\n"
		<< "\t// Booster comment stays\n\tpublic string label = \"Booster\";\n\tpublic Vector3 ExtraOffset() => Vector3.UnitY;\n}\n";
	std::ofstream(fileB, std::ios::binary)
		<< "using Shalltear;\n\nnamespace Demo;\n\npublic sealed class Caller : Behavior {\n\tpublic Booster? target;\n}\n";

	const fs::path dotnet = fs::path(sdk) / _editor->muxerName;
	TestPlatform::SetEnvironment("DOTNET_ROOT", sdk);
	TestPlatform::SetEnvironment("DOTNET_CLI_TELEMETRY_OPTOUT", "1");
	TestPlatform::SetEnvironment("DOTNET_NOLOGO", "1");
	TestPlatform::SetEnvironment("DOTNET_MULTILEVEL_LOOKUP", "0");

	int exitCode = -1;
	const str command = "\"" + dotnet.generic_string() + "\" \"" + refactorDll.generic_string() + "\" --file \"" +
						fileA.generic_string() + "\" --old-name Booster --new-name TurboPad --project-root \"" +
						_project.generic_string() + "\" --runtime-dir \"" + managedDir.generic_string() + "\"";
	const str output = TestPlatform::RunCommand(command, exitCode);
	ASSERT_EQ(exitCode, 0) << output;

	const str updatedA = ReadTextFile(fileA);
	const str updatedB = ReadTextFile(fileB);
	EXPECT_NE(updatedA.find("[FormerlySerializedAs(\"Demo.Booster\")]"), str::npos) << updatedA;
	EXPECT_NE(updatedA.find("class TurboPad : Behavior"), str::npos) << updatedA;
	EXPECT_NE(updatedA.find("// Booster comment stays"), str::npos) << updatedA;
	EXPECT_NE(updatedA.find("\"Booster\""), str::npos) << updatedA;
	EXPECT_NE(updatedB.find("public TurboPad? target;"), str::npos) << updatedB;
}

TEST_F(CSharpMigrationTest, CppViewEmitterHandlesCollisionsTypedHandlesAndEnums) {
	const str sdk = _hooks->findSDKRoot();
	if (sdk.empty()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	const fs::path managedDir = fs::path(ScriptProjectOps::GetEditorExecutablePath()).parent_path() / "CSharp";
	const fs::path refactorDll = managedDir / "Refactor" / "Shalltear.Refactor.dll";
	ASSERT_TRUE(fs::is_regular_file(refactorDll)) << refactorDll;

	std::ofstream(_project / "Assets" / "Scripts" / "AIState.cs", std::ios::binary)
		<< "using Shalltear;\n\nnamespace Game.AI;\n\npublic sealed class State : Behavior {\n\tpublic int priority;\n}\n";
	std::ofstream(_project / "Assets" / "Scripts" / "UIState.cs", std::ios::binary)
		<< "using Shalltear;\n\nnamespace Game.UI;\n\npublic sealed class State : Behavior {\n\tpublic bool visible;\n}\n";
	std::ofstream(_project / "Assets" / "Scripts" / "TargetViewProbe.cs", std::ios::binary)
		<< "using Shalltear;\n\nnamespace Demo;\n\npublic enum ProbeMode : byte { Idle = 0, Active = 1 }\n\n"
		<< "public sealed class TargetViewProbe : Behavior {\n"
		<< "\tpublic SceneObject? trackedObject;\n"
		<< "\tpublic Mesh? customMesh;\n"
		<< "\tpublic ProbeMode mode;\n"
		<< "\t[Callable]\n"
		<< "\tpublic SceneObject? EchoTarget(SceneObject? input) => input;\n"
		<< "}\n";

	const fs::path viewsDir = _project / "Library" / "CSharp" / "Generated" / "Views";
	const fs::path dotnet = fs::path(sdk) / _editor->muxerName;
	TestPlatform::SetEnvironment("DOTNET_ROOT", sdk);
	TestPlatform::SetEnvironment("DOTNET_CLI_TELEMETRY_OPTOUT", "1");
	TestPlatform::SetEnvironment("DOTNET_NOLOGO", "1");
	TestPlatform::SetEnvironment("DOTNET_MULTILEVEL_LOOKUP", "0");

	int exitCode = -1;
	const str command = "\"" + dotnet.generic_string() + "\" \"" + refactorDll.generic_string() +
						"\" --emit-views --project-root \"" + _project.generic_string() +
						"\" --out-dir \"" + viewsDir.generic_string() +
						"\" --runtime-dir \"" + managedDir.generic_string() + "\"";
	const str output = TestPlatform::RunCommand(command, exitCode);
	ASSERT_EQ(exitCode, 0) << output;

	// colliding short name "State" must emit qualified headers + namespaces instead of overwriting State.generated.h
	EXPECT_FALSE(fs::exists(viewsDir / "State.generated.h"));
	ASSERT_TRUE(fs::is_regular_file(viewsDir / "Game.AI.State.generated.h"));
	ASSERT_TRUE(fs::is_regular_file(viewsDir / "Game.UI.State.generated.h"));
	EXPECT_NE(ReadTextFile(viewsDir / "Game.AI.State.generated.h").find("namespace Shalltear::CSharpViews::Game::AI"), str::npos);
	EXPECT_NE(ReadTextFile(viewsDir / "Game.UI.State.generated.h").find("namespace Shalltear::CSharpViews::Game::UI"), str::npos);

	// unique class keeps ergonomic short filename with typed Handle<T> and enum fields
	const fs::path probeHeader = viewsDir / "TargetViewProbe.generated.h";
	ASSERT_TRUE(fs::is_regular_file(probeHeader));
	const str probeText = ReadTextFile(probeHeader);
	EXPECT_NE(probeText.find("#include \"Shalltear/Core/Graphics/MeshAsset.h\""), str::npos) << probeText;
	EXPECT_NE(probeText.find("::Shalltear::Handle<::Shalltear::SceneObject> GetTrackedObject()"), str::npos) << probeText;
	EXPECT_NE(probeText.find("::Shalltear::Handle<::Shalltear::MeshAsset> GetCustomMesh()"), str::npos) << probeText;
	EXPECT_NE(probeText.find("::Shalltear::u8 GetMode()"), str::npos) << probeText;
	EXPECT_NE(probeText.find("void SetMode(const ::Shalltear::u8& next)"), str::npos) << probeText;
	EXPECT_NE(probeText.find("::Shalltear::Handle<::Shalltear::SceneObject> EchoTarget(::Shalltear::Handle<::Shalltear::SceneObject> input)"), str::npos)
		<< probeText;
}

#endif
