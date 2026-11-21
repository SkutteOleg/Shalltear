#include "TestsPCH.h"

#if defined(SHALLTEAR_SHARED)

#include "CSharpPackageFixtures.h"
#include "CSharpRuntimeFixture.h"
#include "EditorTestHooks.h"
#include "TestPlatform.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"

#include "Shalltear/Editor/ChildProcess.h"
#include "Shalltear/Editor/CompileStatus.h"
#include "Shalltear/Editor/EditorConsole.h"

#include <algorithm>
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

	constexpr const char* DEFAULT_ASSEMBLY = "Space_Game.Scripts";

	constexpr const char* HEALTH = "using Shalltear;\n\nnamespace Game.Core;\n\npublic class Health : Behavior {\n\tpublic int points = 5;\n}\n";
	constexpr const char* REPLICATED = "namespace Game.Net;\n\npublic class Replicated : Game.Core.Health {\n\tpublic int channel = 2;\n}\n";
	constexpr const char* REPLICATED_EDITED =
		"namespace Game.Net;\n\npublic class Replicated : Game.Core.Health {\n\tpublic int channel = 2;\n\tpublic int lag = 3;\n}\n";
	constexpr const char* GADGET = "#if TOOLS\nusing Shalltear;\n\nnamespace Game.Tools;\n\npublic sealed class Gadget : Behavior {}\n#endif\n";
	constexpr const char* PLAYER = "namespace Space_Game;\n\npublic sealed class Player : Game.Net.Replicated {}\n";
	constexpr const char* BONUS = "using Shalltear;\n\nnamespace Game.Extra;\n\npublic sealed class Bonus : Behavior {}\n";
	constexpr const char* GREETS = "using Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Greets : Behavior {\n\tprotected override void OnEnable() {\n"
								   "\t\tLog.Info(\"Greets\", Fixture.Greeter.Hello.To(\"editor\"));\n\t}\n}\n";

	char _assembliesOwner = 0;

	class CSharpAssembliesTest : public CSharpRuntimeFixture {
	protected:
		const EditorTestHooks* _editor = nullptr;
		fs::path _root;
		fs::path _project;
		str _sdk;

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
			_root = fs::temp_directory_path() / ("slt_cs_assemblies_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			_project = _root / "Space Game";
			fs::create_directories(_project / "Assets");
		}

		void TearDown() override {
			if (_editor) {
				_editor->shutdownAuthoring();
			}
			CompileStatus::UnregisterOwnedBy(&_assembliesOwner);
			EditorConsole::Terminate();
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

		str Read(const str& relative) {
			std::ifstream file(_project / relative, std::ios::binary);
			std::ostringstream text;
			text << file.rdbuf();
			return text.str();
		}

		CSharpProjectFiles::Options MakeOptions() {
			CSharpProjectFiles::Options options;
			options.projectPath = _project.generic_string();
			options.projectName = "Space Game";
			options.dotnetRoot = (_root / "sdks" / "dotnet").generic_string();
			options.engineRoot = (_root / "engine").generic_string();
			options.managedDirectory = (_root / "engine" / "bin" / "CSharp").generic_string();
			options.packagesRoot = (_root / "packages").generic_string();
			options.pin.sdkVersion = "10.0.999";
			options.pin.runtimeVersion = "10.0.99";
			options.pin.targetFramework = "net10.0";
			options.assemblies = _editor->scanAssemblies(options.projectPath, DEFAULT_ASSEMBLY);
			return options;
		}

		bool Prepare() {
			_sdk = _hooks->findSDKRoot();
			if (_sdk.empty()) {
				return false;
			}
			CSharpProjectFiles::Result written;
			if (!_editor->writeProjectFiles(_editor->getDefaultProjectOptions(_project.generic_string(), _sdk), written)) {
				return false;
			}
			CSharpAuthoring::Options options;
			options.projectPath = _project.generic_string();
			options.projectFile = "Space_Game.Scripts.csproj";
			options.sdkRoot = _sdk;
			options.buildServers = false;
			options.watchSources = false;
			options.openBuild = false;
			options.debounceSeconds = 0.0;
			_editor->beginAuthoring(options, &_assembliesOwner);
			return true;
		}

		bool CompileAgain(f64 seconds = 240.0) {
			const u32 wanted = _editor->getAuthoringCompile().GetRequestCount() + 1;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<f64>(seconds);
			while (std::chrono::steady_clock::now() < deadline) {
				_editor->pumpAuthoring();
				const CSharpCompile& compile = _editor->getAuthoringCompile();
				if (compile.GetRequestCount() >= wanted && !compile.IsBusy()) {
					_editor->pumpAuthoring();
					return compile.GetState() == CompileState::Succeeded;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			return false;
		}

		str Said() {
			str said = _log.Delivered().Joined();
			Vector<ConsoleEntry> entries;
			EditorConsole::ReadSince(0, entries);
			for (const ConsoleEntry& entry : entries) {
				said += "\n" + entry.message;
			}
			return said;
		}

		fs::path Output(std::string_view assembly) const { return _project / "Library" / "CSharp" / "bin" / (str(assembly) + ".dll"); }

		static bool HasProblem(const CSharpAssemblies::Set& set, std::string_view file, std::string_view text) {
			return std::any_of(set.problems.begin(), set.problems.end(), [&](const CSharpAssemblies::Problem& problem) {
				return problem.file == file && problem.message.find(text) != str::npos;
			});
		}
	};

} // anonymous namespace

TEST_F(CSharpAssembliesTest, DefinitionsSplitTheProjectFiles) {
	Put("Assets/Core/Core.slassembly", "{\"assemblyName\": \"Game.Core\"}");
	Put("Assets/Core/Tools/Tools.slassembly", "{\"assemblyName\": \"Game.Tools\", \"references\": [\"Game.Core\"], \"editorOnly\": true, \"defines\": [\"TOOLS\", \"EXTRA\"]}");
	Put("Assets/Net & Co/Net.slassembly", "{\"assemblyName\": \"Game.Net\", \"references\": [\"Game.Core\"], \"platforms\": [\"Windows\", \"WindowsServer\"]}");
	Put("Mine.Scripts.csproj", "<Project />\n");
	CSharpProjectFiles::Options options = MakeOptions();
	ASSERT_EQ(options.assemblies.definitions.size(), 3u);
	EXPECT_TRUE(options.assemblies.problems.empty());
	EXPECT_EQ(options.assemblies.definitions[0].name, "Game.Core");
	EXPECT_EQ(options.assemblies.definitions[0].nestedFolders, (Vector<str>{"Assets/Core/Tools"}));
	CSharpProjectFiles::Result written;
	ASSERT_TRUE(_editor->writeProjectFiles(options, written));

	const str project = Read("Space_Game.Scripts.csproj");
	EXPECT_NE(project.find("<Compile Include=\"Assets/**/*.cs\" Exclude=\"Assets/Core/**/*.cs;Assets/Core/Tools/**/*.cs;Assets/Net &amp; Co/**/*.cs\" />"), str::npos)
		<< project;
	EXPECT_NE(project.find("<ProjectReference Include=\"Game.Core.Scripts.csproj\" Private=\"false\" />"), str::npos) << project;
	EXPECT_NE(project.find("<ProjectReference Include=\"Game.Net.Scripts.csproj\" Private=\"false\" Condition=\"'$(ShalltearTargetPlatform)' == '' Or "
						   "'$(ShalltearTargetPlatform)' == 'Windows' Or '$(ShalltearTargetPlatform)' == 'WindowsServer'\" />"),
			  str::npos)
		<< project;
	EXPECT_EQ(project.find("Game.Tools"), str::npos) << project;

	const str core = Read("Game.Core.Scripts.csproj");
	EXPECT_NE(core.find("<AssemblyName>Game.Core</AssemblyName>"), str::npos) << core;
	EXPECT_NE(core.find("<Compile Include=\"Assets/Core/**/*.cs\" Exclude=\"Assets/Core/Tools/**/*.cs\" />"), str::npos) << core;
	EXPECT_NE(core.find("obj/Game.Core/"), str::npos) << core;
	EXPECT_EQ(core.find("ProjectReference"), str::npos) << core;
	const str tools = Read("Game.Tools.Scripts.csproj");
	EXPECT_NE(tools.find("<DefineConstants>$(DefineConstants);TOOLS;EXTRA</DefineConstants>"), str::npos) << tools;
	EXPECT_NE(tools.find("<ProjectReference Include=\"Game.Core.Scripts.csproj\" Private=\"false\" />"), str::npos) << tools;
	const str solution = Read("Space_Game.slnx");
	for (const char* listed : {"Space_Game.Scripts.csproj", "Game.Core.Scripts.csproj", "Game.Tools.Scripts.csproj", "Game.Net.Scripts.csproj"}) {
		EXPECT_NE(solution.find(std::format("<Project Path=\"{}\" />", listed)), str::npos) << listed;
	}

	fs::remove(_project / "Assets" / "Net & Co" / "Net.slassembly");
	options = MakeOptions();
	ASSERT_EQ(options.assemblies.definitions.size(), 2u);
	ASSERT_TRUE(_editor->writeProjectFiles(options, written));
	EXPECT_FALSE(fs::exists(_project / "Game.Net.Scripts.csproj"));
	EXPECT_TRUE(fs::exists(_project / "Game.Core.Scripts.csproj"));
	EXPECT_TRUE(fs::exists(_project / "Mine.Scripts.csproj"));
	EXPECT_EQ(Read("Space_Game.Scripts.csproj").find("Game.Net"), str::npos);

	fs::remove(_project / "Assets" / "Core" / "Core.slassembly");
	fs::remove(_project / "Assets" / "Core" / "Tools" / "Tools.slassembly");
	options = MakeOptions();
	ASSERT_TRUE(_editor->writeProjectFiles(options, written));
	EXPECT_NE(Read("Space_Game.Scripts.csproj").find("<Compile Include=\"Assets/**/*.cs\" />\n\t</ItemGroup>"), str::npos);
	EXPECT_FALSE(fs::exists(_project / "Game.Core.Scripts.csproj"));
}

TEST_F(CSharpAssembliesTest, DefinitionMistakesAreReported) {
	Put("Assets/A/Good.slassembly", "{\"assemblyName\": \"Game.Good\"}");
	Put("Assets/B/Unnamed Kit.slassembly", "{}");
	Put("Assets/C/Spaced.slassembly", "{\"assemblyName\": \"Not A Name\"}");
	Put("Assets/D/Taken.slassembly", "{\"assemblyName\": \"Game.Good\"}");
	Put("Assets/E/Engine.slassembly", "{\"assemblyName\": \"Shalltear.Runtime\"}");
	Put("Assets/F/Default.slassembly", std::format("{{\"assemblyName\": \"{}\"}}", DEFAULT_ASSEMBLY));
	Put("Assets/G/Lost.slassembly", "{\"assemblyName\": \"Game.Lost\", \"references\": [\"Game.Nowhere\"]}");
	Put("Assets/H/Tools.slassembly", "{\"assemblyName\": \"Game.Tools\", \"editorOnly\": true}");
	Put("Assets/I/Leaky.slassembly", "{\"assemblyName\": \"Game.Leaky\", \"references\": [\"Game.Tools\"]}");
	Put("Assets/J/Ring1.slassembly", "{\"assemblyName\": \"Game.Ring1\", \"references\": [\"Game.Ring2\"]}");
	Put("Assets/K/Ring2.slassembly", "{\"assemblyName\": \"Game.Ring2\", \"references\": [\"Game.Ring1\"]}");
	Put("Assets/L/Broken.slassembly", "not json");
	Put("Assets/M/First.slassembly", "{\"assemblyName\": \"Game.First\"}");
	Put("Assets/M/Second.slassembly", "{\"assemblyName\": \"Game.Second\"}");
	Put("Assets/N/Symbols.slassembly", "{\"assemblyName\": \"Game.Symbols\", \"defines\": [\"NOT-A-SYMBOL\"]}");
	Put("Assets/O/Self.slassembly", "{\"assemblyName\": \"Game.Self\", \"references\": [\"Game.Self\"]}");

	const CSharpAssemblies::Set set = _editor->scanAssemblies(_project.generic_string(), DEFAULT_ASSEMBLY);
	Vector<str> names;
	for (const CSharpAssemblies::Definition& definition : set.definitions) {
		names.push_back(definition.name);
	}
	std::sort(names.begin(), names.end());
	EXPECT_EQ(names, (Vector<str>{"Game.First", "Game.Good", "Game.Tools", "UnnamedKit"}));
	EXPECT_TRUE(HasProblem(set, "Assets/C/Spaced.slassembly", "has assembly name 'Not A Name' (ignored). Use letters, digits, _ and ."));
	EXPECT_TRUE(HasProblem(set, "Assets/D/Taken.slassembly", "has assembly name 'Game.Good' (taken by 'Assets/A/Good.slassembly'). Rename one"));
	EXPECT_TRUE(HasProblem(set, "Assets/E/Engine.slassembly", "has assembly name 'Shalltear.Runtime' (taken by engine). Rename it"));
	EXPECT_TRUE(HasProblem(set, "Assets/F/Default.slassembly", "(taken by engine). Rename it"));
	EXPECT_TRUE(HasProblem(set, "Assets/G/Lost.slassembly", "references 'Game.Nowhere' (no such assembly definition)"));
	EXPECT_TRUE(HasProblem(set, "Assets/I/Leaky.slassembly", "references editor-only 'Game.Tools' (ignored)"));
	EXPECT_TRUE(HasProblem(set, "Assets/J/Ring1.slassembly", "is part of a reference cycle (ignored)"));
	EXPECT_TRUE(HasProblem(set, "Assets/K/Ring2.slassembly", "is part of a reference cycle (ignored)"));
	EXPECT_TRUE(HasProblem(set, "Assets/L/Broken.slassembly", "isn't a JSON object (ignored)"));
	EXPECT_TRUE(HasProblem(set, "Assets/M/Second.slassembly", "shares its folder with 'Assets/M/First.slassembly' (ignored)"));
	EXPECT_TRUE(HasProblem(set, "Assets/N/Symbols.slassembly", "defines 'NOT-A-SYMBOL' (ignored)"));
	EXPECT_TRUE(HasProblem(set, "Assets/O/Self.slassembly", "references itself (ignored)"));
	EXPECT_EQ(set.problems.size(), 12u);
}

TEST_F(CSharpAssembliesTest, DefinitionsCompileAndLoadTogether) {
	SceneSystem::Initialize();
	ASSERT_TRUE(Attach()) << _error;
	Put("Assets/Core/Core.slassembly", "{\"assemblyName\": \"Game.Core\"}");
	Put("Assets/Core/Health.cs", HEALTH);
	Put("Assets/Net/Net.slassembly", "{\"assemblyName\": \"Game.Net\", \"references\": [\"Game.Core\"]}");
	Put("Assets/Net/Replicated.cs", REPLICATED);
	Put("Assets/Tools/Tools.slassembly", "{\"assemblyName\": \"Game.Tools\", \"editorOnly\": true, \"defines\": [\"TOOLS\"]}");
	Put("Assets/Tools/Gadget.cs", GADGET);
	Put("Assets/Player.cs", PLAYER);
	if (!Prepare()) {
		SceneSystem::Terminate();
		GTEST_SKIP() << "pinned .NET SDK not found";
	}

	_editor->notifySourceChanged();
	ASSERT_TRUE(CompileAgain()) << Said();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded) << Said();
	EXPECT_EQ(_editor->getReloadCount(), 1u);
	for (const char* type : {"Game.Core.Health", "Game.Net.Replicated", "Game.Tools.Gadget", "Space_Game.Player"}) {
		EXPECT_NE(TypeRegistry::FindType(StringHash(type)), nullptr) << type;
	}
	const TypeInfo* player = TypeRegistry::FindType(StringHash("Space_Game.Player"));
	ASSERT_NE(player, nullptr);
	EXPECT_TRUE(TypeRegistry::IsDerivedFrom(player, TypeRegistry::FindType(StringHash("Game.Core.Health"))));

	const fs::file_time_type core = fs::last_write_time(Output("Game.Core"));
	const fs::file_time_type net = fs::last_write_time(Output("Game.Net"));
	const fs::file_time_type tools = fs::last_write_time(Output("Game.Tools"));
	const fs::file_time_type main = fs::last_write_time(Output(DEFAULT_ASSEMBLY));
	Put("Assets/Net/Replicated.cs", REPLICATED_EDITED);
	_editor->notifySourceChanged();
	ASSERT_TRUE(CompileAgain()) << Said();
	EXPECT_EQ(fs::last_write_time(Output("Game.Core")), core);
	EXPECT_EQ(fs::last_write_time(Output("Game.Tools")), tools);
	EXPECT_NE(fs::last_write_time(Output("Game.Net")), net);
	EXPECT_NE(fs::last_write_time(Output(DEFAULT_ASSEMBLY)), main);
	EXPECT_EQ(_editor->getReloadCount(), 2u);
	EXPECT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded) << Said();
	EXPECT_NE(TypeRegistry::FindType(StringHash("Space_Game.Player")), nullptr);

	Put("Assets/Extra/Extra.slassembly", "{\"assemblyName\": \"Game.Extra\"}");
	Put("Assets/Extra/Bonus.cs", BONUS);
	_editor->notifyDefinitionsChanged();
	ASSERT_TRUE(CompileAgain()) << Said();
	EXPECT_TRUE(fs::exists(_project / "Game.Extra.Scripts.csproj"));
	EXPECT_EQ(_editor->getReloadCount(), 3u);
	EXPECT_NE(TypeRegistry::FindType(StringHash("Game.Extra.Bonus")), nullptr) << Said();
	EXPECT_NE(TypeRegistry::FindType(StringHash("Game.Core.Health")), nullptr);

	Put("Assets/Extra/Extra.slassembly", "{\"assemblyName\": \"Not A Name\"}");
	_editor->notifyDefinitionsChanged();
	ASSERT_TRUE(CompileAgain()) << Said();
	EXPECT_NE(Said().find("'Assets/Extra/Extra.slassembly' has assembly name 'Not A Name' (ignored)"), str::npos) << Said();
	EXPECT_FALSE(fs::exists(_project / "Game.Extra.Scripts.csproj"));
	EXPECT_NE(TypeRegistry::FindType(StringHash("Game.Extra.Bonus")), nullptr) << Said();

	_editor->shutdownAuthoring();
	_hooks->detach();
	SceneSystem::Terminate();
}

TEST_F(CSharpAssembliesTest, PackageReferenceRestoresAndLoads) {
	SceneSystem::Initialize();
	ASSERT_TRUE(Attach()) << _error;
	PackageFeed feed;
	feed.root = _root / "nuget";
	if (!PackFixturePackage(feed, "Fixture.Greeter")) {
		SceneSystem::Terminate();
		if (feed.log.find("not found") != str::npos) {
			GTEST_SKIP() << feed.log;
		}
		FAIL() << feed.log;
	}
	Put("CSharp.props", feed.MakeUserProps("Fixture.Greeter"));
	Put("Assets/Scripts/Greets.cs", GREETS);
	ASSERT_TRUE(Prepare());
	_editor->notifySourceChanged();
	ASSERT_TRUE(CompileAgain()) << Said();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded) << Said();
	EXPECT_TRUE(fs::is_regular_file(Output("Fixture.Greeter")));

	const TypeInfo* greets = TypeRegistry::FindType(StringHash("Space_Game.Greets"));
	ASSERT_NE(greets, nullptr) << Said();
	Handle<Scene> world = SceneSystem::CreateScene("World");
	Handle<SceneObject> object = SceneSystem::CreateObject("Host", world);
	ASSERT_TRUE(object->AddComponent(greets).IsValid());
	EXPECT_EQ(_log.Delivered().Count("hello editor from a package"), 1u) << Said();
	SceneSystem::UnloadScene(world);
	SceneSystem::EndFrame();

	_editor->shutdownAuthoring();
	_hooks->detach();
	SceneSystem::Terminate();
}

#endif
