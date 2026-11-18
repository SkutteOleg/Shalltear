#include "TestsPCH.h"

#if defined(SHALLTEAR_SHARED)

#include "CSharpRuntimeFixture.h"
#include "CSharpScenarioUtil.h"
#include "EditorTestHooks.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/Scene/UnresolvedComponent.h"

#include "Shalltear/Editor/CompileStatus.h"
#include "Shalltear/Editor/EditorConsole.h"
#include "Shalltear/Editor/TypeNameOps.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace Shalltear;
using namespace Shalltear::CSharpEditor;
using namespace Shalltear::CSharpTests;

namespace {

	namespace fs = std::filesystem;

	constexpr const char* THROWER = "using System;\nusing Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Thrower : Behavior {\n"
									"\tprotected override void OnUpdate() {\n\t\tFail(Owner.Name);\n\t}\n\n"
									"\tstatic void Fail(string who) {\n\t\tthrow new InvalidOperationException(\"thrown by \" + who);\n\t}\n}\n";

	constexpr const char* TALKER = "using Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Talker : Behavior {\n"
								   "\tprotected override void OnUpdate() {\n\t\tfor (int index = 0; index < 2; ++index) {\n"
								   "\t\t\tLog.Info(\"Game\", \"talker says \" + index);\n\t\t}\n\t\tLog.Warn(\"Game\", \"talker warns\");\n\t}\n}\n";

	constexpr const char* KEEPER = "using Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Keeper : Behavior {\n\tpublic float speed = 90f;\n"
								   "\tpublic string label = \"fresh\";\n}\n";

	constexpr const char* FILLER = "namespace Space_Game;\n\npublic sealed class Filler {\n}\n";

	constexpr const char* TOUCHER = "using System;\nusing Shalltear;\n\nnamespace Space_Game;\n\nstatic class Fragile {\n"
									"\tinternal static readonly int value;\n\n\tstatic Fragile() {\n\t\tvalue = Fail();\n\t}\n\n"
									"\tstatic int Fail() {\n\t\tthrow new InvalidOperationException(\"broken at start\");\n\t}\n}\n\n"
									"public sealed class Toucher : Behavior {\n\tprotected override void OnUpdate() {\n"
									"\t\tLog.Info(\"Game\", \"value \" + Fragile.value);\n\t}\n}\n";

	constexpr const char* ASKER = "using System.Collections.Generic;\nusing Shalltear;\n\nnamespace Space_Game;\n\n"
								  "public sealed class Asker : Behavior {\n\tprotected override void OnUpdate() {\n"
								  "\t\tList<Behavior>? none = null;\n\t\tOwner.GetComponents(none!);\n\t}\n}\n";

	char _errorOwner = 0;

	class CSharpErrorFixture : public CSharpRuntimeFixture {
	protected:
		const EditorTestHooks* _editor = nullptr;
		fs::path _root;
		fs::path _project;
		CSharpAuthoring::Options _options;
		Handle<Scene> _world;

		void SetUp() override {
			CSharpRuntimeFixture::SetUp();
			const auto get = reinterpret_cast<GetEditorTestHooksFn>(
				ModuleSystem::FindSymbol(ModuleSystem::GetNativeHandle(_package), EDITOR_TEST_HOOKS_SYMBOL));
			ASSERT_NE(get, nullptr);
			_editor = get();
			const fs::path engineAssets = DeviceLossTest::RepoDir("EngineAssets");
			ASSERT_FALSE(engineAssets.empty());
			FileSystem::Mount("engine", Path(engineAssets.generic_string()));
			_root = fs::temp_directory_path() / ("shalltear_csharp_errors_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			_project = _root / "Space Game";
			fs::create_directories(_project / "Assets" / "Scripts");
			FileSystem::Mount("project", Path(_project.generic_string()));
			EditorConsole::Initialize();
			SceneSystem::Initialize();
			_hooks->installComponentMenu();
			ASSERT_TRUE(Attach()) << _error;
			_world = SceneSystem::CreateScene("World");
		}

		void TearDown() override {
			if (_editor) {
				_editor->shutdownAuthoring();
			}
			CompileStatus::UnregisterOwnedBy(&_errorOwner);
			SceneSystem::Terminate();
			EditorConsole::Terminate();
			FileSystem::Unmount("project");
			std::error_code ec;
			fs::remove_all(_root, ec);
			FileSystem::Unmount("engine");
			CSharpRuntimeFixture::TearDown();
		}

		void Put(const str& relative, const str& text) {
			const fs::path file = _project / relative;
			fs::create_directories(file.parent_path());
			std::ofstream(file, std::ios::binary) << text;
		}

		bool Compile() {
			const str sdk = _hooks->findSDKRoot();
			if (sdk.empty()) {
				return false;
			}
			if (_options.projectPath.empty()) {
				CSharpProjectFiles::Options files = _editor->getDefaultProjectOptions(_project.generic_string(), sdk);
				files.projectName = "Space Game";
				CSharpProjectFiles::Result written;
				if (!_editor->writeProjectFiles(files, written)) {
					return false;
				}
				_options.projectPath = _project.generic_string();
				_options.projectFile = "Space_Game.Scripts.csproj";
				_options.sdkRoot = sdk;
				_options.buildServers = false;
				_options.watchSources = false;
				_options.openBuild = true;
				_editor->beginAuthoring(_options, &_errorOwner);
			} else {
				CompileStatus::RequestCompile();
			}
			const u32 reloads = _editor->getReloadCount();
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
			while (std::chrono::steady_clock::now() < deadline) {
				_editor->pumpAuthoring();
				const CSharpCompile& compile = _editor->getAuthoringCompile();
				if (!compile.IsBusy() && (_editor->getReloadCount() > reloads || compile.GetState() == CompileState::Failed)) {
					return true;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			return false;
		}

		static const TypeInfo* Type(const char* name) { return TypeRegistry::FindType(StringHash(str("Space_Game.") + name)); }

		Handle<Component> Add(const char* typeName, const char* objectName) {
			const TypeInfo* type = Type(typeName);
			return type ? SceneSystem::CreateObject(objectName, _world)->AddComponent(type) : Handle<Component>();
		}

		Vector<ConsoleEntry> Rows(std::string_view holding) {
			_log.Delivered();
			Vector<ConsoleEntry> all{GetMemoryResource(MemTag::Editor)};
			EditorConsole::ReadSince(0, all);
			Vector<ConsoleEntry> matching{GetMemoryResource(MemTag::Editor)};
			for (ConsoleEntry& row : all) {
				if (row.message.find(holding) != str::npos) {
					matching.push_back(std::move(row));
				}
			}
			return matching;
		}

		bool IsScript(const ConsoleEntry& row, const char* file) const {
			std::error_code ec;
			return !row.file.empty() && fs::equivalent(fs::path(row.file), _project / "Assets" / "Scripts" / file, ec);
		}
	};

	using CSharpExceptionTest = CSharpErrorFixture;
	using CSharpLogTest = CSharpErrorFixture;

} // anonymous namespace

TEST_F(CSharpExceptionTest, LoggedWithOrigin) {
	Put("Assets/Scripts/Thrower.cs", THROWER);
	if (!Compile()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_TRUE(Add("Thrower", "Bomb").IsValid()) << _log.Delivered().Joined();
	CSharpScenarios::RunFrames(1);

	const Vector<ConsoleEntry> rows = Rows("thrown by Bomb");
	ASSERT_EQ(rows.size(), 1u) << _log.Delivered().Joined();
	const ConsoleEntry& row = rows[0];
	EXPECT_EQ(row.level, LogLevel::Error);
	EXPECT_EQ(row.category, "C#");
	EXPECT_TRUE(row.message.starts_with("System.InvalidOperationException: thrown by Bomb")) << row.message;
	EXPECT_NE(row.message.find("at Space_Game.Thrower.Fail(String who)"), str::npos) << row.message;
	EXPECT_NE(row.message.find("at Space_Game.Thrower.OnUpdate()"), str::npos) << row.message;
	EXPECT_NE(row.message.find("Thrower.cs:line 12"), str::npos) << row.message;
	EXPECT_TRUE(IsScript(row, "Thrower.cs")) << row.file;
	EXPECT_EQ(row.line, 12u);
	EXPECT_TRUE(row.clickable);
}

TEST_F(CSharpExceptionTest, FrameContinues) {
	Put("Assets/Scripts/Thrower.cs", THROWER);
	Put("Assets/Scripts/Talker.cs", TALKER);
	if (!Compile()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Handle<Component> thrower = Add("Thrower", "Bomb");
	Handle<Component> talker = SceneSystem::CreateObject("Guide", _world)->AddComponent(Type("Talker"));
	ASSERT_TRUE(thrower.IsValid());
	ASSERT_TRUE(talker.IsValid());
	CSharpScenarios::RunFrames(3);

	EXPECT_EQ(Rows("thrown by Bomb").size(), 3u);
	EXPECT_EQ(Rows("talker warns").size(), 3u);
	EXPECT_TRUE(thrower->IsEnabledSelf());
	EXPECT_TRUE(thrower->IsEffectivelyEnabled());
	EXPECT_TRUE(thrower.IsValid());
}

TEST_F(CSharpExceptionTest, MissingClassKeepsData) {
	Put("Assets/Scripts/Keeper.cs", KEEPER);
	if (!Compile()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	Handle<Component> keeper = Add("Keeper", "Holder");
	ASSERT_TRUE(keeper.IsValid());
	CSharpScenarios::Write<f32>(keeper, "speed", 7.5f);
	CSharpScenarios::Write<str>(keeper, "label", str("kept"));
	Handle<SceneObject> holder = _world->FindObjectByName("Holder");
	const StringHash name("Space_Game.Keeper");

	fs::remove(_project / "Assets" / "Scripts" / "Keeper.cs");
	Put("Assets/Scripts/Filler.cs", FILLER);
	ASSERT_TRUE(Compile());
	ASSERT_EQ(_editor->getAuthoringCompile().GetState(), CompileState::Succeeded) << _log.Delivered().Joined();
	EXPECT_EQ(Type("Keeper"), nullptr);
	ASSERT_EQ(holder->GetComponentCount(), 1u);
	const Component* preserved = holder->GetComponentAt(0).Get();
	ASSERT_NE(preserved, nullptr);
	ASSERT_EQ(preserved->GetType(), UnresolvedComponent::GetReflectType());
	EXPECT_EQ(static_cast<const UnresolvedComponent*>(preserved)->GetUnresolvedType(), name);
	EXPECT_EQ(TypeNameOps::Lookup(name), "Keeper");
	EXPECT_NE(TypeNameOps::MissingTypeLabel(name).find("Keeper"), str::npos) << TypeNameOps::MissingTypeLabel(name);

	Put("Assets/Scripts/Keeper.cs", KEEPER);
	ASSERT_TRUE(Compile());
	ASSERT_EQ(holder->GetComponentCount(), 1u);
	Handle<Component> back = holder->GetComponentAt(0);
	ASSERT_EQ(back->GetType(), Type("Keeper"));
	EXPECT_FLOAT_EQ(CSharpScenarios::Read<f32>(back, "speed"), 7.5f);
	EXPECT_EQ(CSharpScenarios::Read<str>(back, "label"), "kept");
}

TEST_F(CSharpLogTest, RowCarriesItsCallSite) {
	Put("Assets/Scripts/Talker.cs", TALKER);
	if (!Compile()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_TRUE(Add("Talker", "Guide").IsValid());
	CSharpScenarios::RunFrames(1);

	const Vector<ConsoleEntry> said = Rows("talker says");
	ASSERT_EQ(said.size(), 2u) << _log.Delivered().Joined();
	for (const ConsoleEntry& row : said) {
		EXPECT_EQ(row.level, LogLevel::Info);
		EXPECT_EQ(row.category, "Game");
		EXPECT_TRUE(IsScript(row, "Talker.cs")) << row.file;
		EXPECT_EQ(row.line, 8u);
		EXPECT_TRUE(row.clickable);
	}
	EXPECT_EQ(said[0].message, "talker says 0");
	EXPECT_EQ(said[1].message, "talker says 1");
	const Vector<ConsoleEntry> warned = Rows("talker warns");
	ASSERT_EQ(warned.size(), 1u);
	EXPECT_EQ(warned[0].level, LogLevel::Warn);
	EXPECT_TRUE(IsScript(warned[0], "Talker.cs"));
	EXPECT_EQ(warned[0].line, 10u);
}

TEST_F(CSharpLogTest, OriginSurvivesAReload) {
	Put("Assets/Scripts/Talker.cs", TALKER);
	if (!Compile()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_TRUE(Add("Talker", "Guide").IsValid());
	CSharpScenarios::RunFrames(1);

	Put("Assets/Scripts/Talker.cs", str("// moved down\n// by 2 lines\n") + TALKER);
	ASSERT_TRUE(Compile());
	ASSERT_EQ(_editor->getAuthoringCompile().GetState(), CompileState::Succeeded) << _log.Delivered().Joined();
	CSharpScenarios::RunFrames(1);

	const Vector<ConsoleEntry> warned = Rows("talker warns");
	ASSERT_EQ(warned.size(), 2u) << _log.Delivered().Joined();
	EXPECT_TRUE(IsScript(warned[0], "Talker.cs")) << warned[0].file;
	EXPECT_EQ(warned[0].line, 10u);
	EXPECT_TRUE(IsScript(warned[1], "Talker.cs")) << warned[1].file;
	EXPECT_EQ(warned[1].line, 12u);
}

TEST_F(CSharpLogTest, SecondLogFromSiteReadsNoPdb) {
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	str message;
	EXPECT_EQ(Run("LogTests.OneSiteResolvesOnce", message), SLT_OK) << message;
	EXPECT_EQ(Rows("from one site").size(), 3u);
}

TEST_F(CSharpExceptionTest, InnerExceptionGivesTheOrigin) {
	Put("Assets/Scripts/Toucher.cs", TOUCHER);
	if (!Compile()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_TRUE(Add("Toucher", "Probe").IsValid()) << _log.Delivered().Joined();
	CSharpScenarios::RunFrames(1);

	const Vector<ConsoleEntry> rows = Rows("broken at start");
	ASSERT_EQ(rows.size(), 1u) << _log.Delivered().Joined();
	const ConsoleEntry& row = rows[0];
	EXPECT_TRUE(row.message.starts_with("System.TypeInitializationException: ")) << row.message;
	EXPECT_NE(row.message.find("> System.InvalidOperationException: broken at start\n"), str::npos) << row.message;
	const size_t inner = row.message.find("at Space_Game.Fragile.Fail()");
	const size_t end = row.message.find("End of inner exception stack trace");
	const size_t outer = row.message.find("at Space_Game.Toucher.OnUpdate()");
	ASSERT_NE(inner, str::npos) << row.message;
	ASSERT_NE(end, str::npos) << row.message;
	ASSERT_NE(outer, str::npos) << row.message;
	EXPECT_LT(inner, end);
	EXPECT_LT(end, outer);
	EXPECT_TRUE(IsScript(row, "Toucher.cs")) << row.file;
	EXPECT_EQ(row.line, 14u);
}

TEST_F(CSharpExceptionTest, EngineFramesAreSkipped) {
	Put("Assets/Scripts/Asker.cs", ASKER);
	if (!Compile()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_TRUE(Add("Asker", "Probe").IsValid()) << _log.Delivered().Joined();
	CSharpScenarios::RunFrames(1);

	const Vector<ConsoleEntry> rows = Rows("System.ArgumentNullException");
	ASSERT_EQ(rows.size(), 1u) << _log.Delivered().Joined();
	EXPECT_NE(rows[0].message.find("at Shalltear.SceneObject.GetComponents"), str::npos) << rows[0].message;
	EXPECT_TRUE(IsScript(rows[0], "Asker.cs")) << rows[0].file;
	EXPECT_EQ(rows[0].line, 9u);
	EXPECT_TRUE(rows[0].clickable);
}

TEST_F(CSharpLogTest, OriginsAreForgottenWhenScriptsUnload) {
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	str message;
	ASSERT_EQ(Run("LogProbe.WritesEachLevel", message), SLT_OK) << message;
	EXPECT_EQ(Run("LogProbe.SitesStartEmpty", message), CASE_FAILED) << message;
	ASSERT_EQ(_hooks->unloadUserAssemblies(), SLT_OK);
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	EXPECT_EQ(Run("LogProbe.SitesStartEmpty", message), SLT_OK) << message;
}

TEST_F(CSharpLogTest, RowBelowLevelTakesNoCallSite) {
	ASSERT_TRUE(LoadConformance()) << _log.Delivered().Joined();
	str message;
	const LogLevel before = LogManager::GetGlobalLevel();
	LogManager::SetGlobalLevel(LogLevel::Fatal);
	const i32 quiet = Run("LogProbe.WritesEachLevel", message);
	LogManager::SetGlobalLevel(before);
	ASSERT_EQ(quiet, SLT_OK) << message;
	EXPECT_EQ(Run("LogProbe.SitesStartEmpty", message), SLT_OK) << message;
	EXPECT_TRUE(Rows("from C#").empty());
	ASSERT_EQ(Run("LogProbe.WritesEachLevel", message), SLT_OK) << message;
	EXPECT_EQ(Run("LogProbe.SitesStartEmpty", message), CASE_FAILED) << message;
}

TEST_F(CSharpLogTest, OriginTextIsInternedOnce) {
	const str path = (_project / "Assets" / "Scripts" / "Talker.cs").generic_string();
	const char* first = _hooks->internLogOrigin(path);
	ASSERT_NE(first, nullptr);
	EXPECT_EQ(str(first), path);
	EXPECT_NE(first, path.c_str());
	for (u32 index = 0; index < 2000; ++index) {
		const str other = path + std::to_string(index);
		EXPECT_EQ(str(_hooks->internLogOrigin(other)), other);
	}
	EXPECT_EQ(_hooks->internLogOrigin(str(path)), first);
	EXPECT_EQ(str(first), path);
	EXPECT_EQ(str(_hooks->internLogOrigin("")), "");
}

#endif
