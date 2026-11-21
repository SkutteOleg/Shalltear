#include "TestsPCH.h"

#if defined(SHALLTEAR_SHARED)

#include "CSharpPackageFixtures.h"
#include "CSharpRuntimeFixture.h"
#include "CSharpScenarioUtil.h"
#include "EditorTestHooks.h"
#include "TestPlatform.h"
#include "UITestUtil.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Module/ScriptFunctions.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"

#include "Shalltear/Editor/ChildProcess.h"
#include "Shalltear/Editor/CompileStatus.h"
#include "Shalltear/Editor/EditorConsole.h"
#include "Shalltear/Editor/EditorMenus.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

using namespace Shalltear;
using namespace Shalltear::CSharpEditor;
using namespace Shalltear::CSharpTests;

namespace NativeTypesFixture {

	enum class Gear : u8 {
		Low = 1,
		High = 2,
	};

} // namespace NativeTypesFixture

SLT_REFLECT_ENUM(::NativeTypesFixture::Gear)
SLT_ENUM_VALUE(Low)
SLT_ENUM_VALUE(High)
SLT_END_ENUM()

class NativeProbeGauge final : public Component {
public:
	SLT_REFLECT_COMPONENT(NativeProbeGauge, Component)
	SLT_REFLECT_FIELD(level)
	SLT_END_REFLECT()

	i32 level = 0;
};

class NativeProbeEngine final : public Component {
public:
	SLT_REFLECT_COMPONENT(NativeProbeEngine, Component)
	SLT_REFLECT_FIELD(power)
	SLT_REFLECT_FIELD(_cylinders)
	SLT_REFLECT_FIELD(label)
	SLT_REFLECT_FIELD(running)
	SLT_REFLECT_FIELD(mount)
	SLT_REFLECT_FIELD(paint)
	SLT_REFLECT_FIELD(gear)
	SLT_REFLECT_FIELD(anchor)
	SLT_REFLECT_FIELD(dial)
	SLT_REFLECT_FIELD(serial, SLT_READONLY)
	SLT_REFLECT_ARRAY(ratios)
	SLT_REFLECT_METHOD(Rev)
	SLT_REFLECT_METHOD(RevBy)
	SLT_END_REFLECT()

	void Rev() { ++revs; }
	void RevBy(i32 count) { revs += count; }

	f32 power = 1.5f;
	i32 _cylinders = 4;
	str label = "v8";
	bool running = false;
	Vec3 mount{1.0f, 2.0f, 3.0f};
	Color paint{0.25f, 0.5f, 0.75f, 1.0f};
	NativeTypesFixture::Gear gear = NativeTypesFixture::Gear::Low;
	Handle<SceneObject> anchor;
	Handle<NativeProbeGauge> dial;
	i32 serial = 40;
	Vector<f32> ratios;
	i32 revs = 0;
};

SLT_REGISTER_TYPE(NativeProbeGauge)
SLT_REGISTER_TYPE(NativeProbeEngine)

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

	constexpr const char* DRIVER =
		"using Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Driver : Behavior {\n\tpublic NativeProbeEngine? engine;\n"
		"\tpublic NativeProbeGauge? gauge;\n\n\tprotected override void OnEnable() {\n"
		"\t\tNativeProbeEngine found = Owner.GetComponent<NativeProbeEngine>()!;\n\t\tfound.Power *= 2f;\n\t\tfound.Cylinders += 2;\n"
		"\t\tfound.Label += \"!\";\n\t\tfound.Running = !found.Running;\n\t\tfound.Mount += new Vec3(1f, 0f, 0f);\n"
		"\t\tfound.Paint = new Color(found.Paint.g, found.Paint.r, 0f, 1f);\n"
		"\t\tfound.Gear = found.Gear == Gear.Low ? Gear.High : Gear.Low;\n\t\tfound.Anchor = Owner;\n"
		"\t\tfound.Rev();\n\t\tengine = found;\n\t\tgauge = Owner.GetComponent<NativeProbeGauge>()!;\n\t\tfound.Dial = gauge;\n"
		"\t\tgauge.Level = found.Serial + (found.Dial == gauge ? 1 : 0) + (found.Anchor == Owner ? 1 : 0);\n\t}\n}\n";
	constexpr const char* HALVES = "using Shalltear;\n\nnamespace Game.Calls;\n\npublic static class Halves {\n\t[ExportToNative(\"Game.Calls.Half\")]\n"
								   "\tinternal static int Half(int value) {\n\t\treturn value / 2;\n\t}\n}\n";
	constexpr const char* IDLE = "using Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Idle : Behavior {}\n";

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

TEST_F(CSharpAssembliesTest, EditorOnlyAssemblyReachesEditorAPI) {
	SceneSystem::Initialize();
	ASSERT_TRUE(Attach()) << _error;
	_editor->registerEditorBindings();
	Put("Assets/Tools/Tools.slassembly", "{\"assemblyName\": \"Game.Tools\", \"editorOnly\": true}");
	Put("Assets/Tools/Menu.cs", R"(using Shalltear;
using Shalltear.Editor;

namespace Game.Tools;

static class Menu {
	[EditorLoad]
	internal static void Add() {
		EditorMenusMenuItemDesc item = new() { Label = "From Tools" };
		item.SetOnSelect(static context => Log.Info("Tools", "tools item selected"));
		EditorMenus.Register("ToolsProbe", item);
	}
}
)");
	Put("Assets/Bonus.cs", BONUS);
	if (!Prepare()) {
		SceneSystem::Terminate();
		GTEST_SKIP() << "pinned .NET SDK not found";
	}

	_editor->notifySourceChanged();
	ASSERT_TRUE(CompileAgain()) << Said();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded) << Said();
	UITest::MockFontSystem fonts;
	UI::Context context(UI::ContextDesc{}, &fonts);
	Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
	EditorMenus::AppendContributed("ToolsProbe", context, items);
	ASSERT_EQ(items.size(), 1u) << Said();
	EXPECT_EQ(items[0].label, "From Tools");
	items[0].onSelect();
	EXPECT_EQ(_log.Delivered().Count("tools item selected"), 1u) << Said();
	items.clear();

	Put("Assets/Bonus.cs", str(BONUS) + "\nstatic class Reach { static object item = new Shalltear.Editor.EditorMenusMenuItemDesc(); }\n");
	_editor->notifySourceChanged();
	EXPECT_FALSE(CompileAgain()) << Said();
	EXPECT_NE(Said().find("CS0234"), str::npos) << Said();

	_editor->shutdownAuthoring();
	EditorMenus::Terminate();
	SceneSystem::Terminate();
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

TEST_F(CSharpAssembliesTest, WrappersRenderComponentMembers) {
	const TypeInfo* types[] = {NativeProbeEngine::GetReflectType(), NativeProbeGauge::GetReflectType()};
	const str text = _editor->renderNativeTypes(types);
	const auto power = std::format("0x{:016x}UL", StringHash("power").GetValue());
	const auto serial = std::format("0x{:016x}UL", StringHash("serial").GetValue());
	for (const str& expected : {
			 str("public enum Gear : int {\n\t\tLow = 1,\n\t\tHigh = 2,\n\t}"),
			 str("[global::Shalltear.Interop.EngineType(\"NativeProbeEngine\")]\n\tpublic sealed class NativeProbeEngine : global::Shalltear.Interop.NativeComponent {"),
			 std::format("const ulong TYPE = 0x{:016x}UL;", StringHash("NativeProbeEngine").GetValue()),
			 std::format("public float Power {{\n\t\t\tget => GetSingle(TYPE, {});\n\t\t\tset => Set(TYPE, {}, value);\n\t\t}}", power, power),
			 str("public int Cylinders {"),
			 str("public string Label {"),
			 str("public bool Running {"),
			 str("public global::Shalltear.Vec3 Mount {"),
			 str("public global::Shalltear.Color Paint {"),
			 str("public global::Gear Gear {"),
			 str("public global::Shalltear.SceneObject? Anchor {"),
			 str("public global::NativeProbeGauge? Dial {"),
			 std::format("public int Serial {{\n\t\t\tget => (int)GetInteger(TYPE, {}, {});\n\t\t}}", serial, static_cast<i32>(SLT_KIND_I32)),
			 str("public void Rev() {"),
			 str("public int Level {"),
			 str("static partial void RegisterNativeTypes() {\n\t\t\tglobal::NativeProbeEngine.RegisterWrapper();\n\t\t\tglobal::NativeProbeGauge.RegisterWrapper();\n"),
		 }) {
		EXPECT_NE(text.find(expected), str::npos) << expected << "\n"
												  << text;
	}
	for (const char* absent : {"Ratios", "RevBy", "Revs"}) {
		EXPECT_EQ(text.find(absent), str::npos) << absent;
	}
	EXPECT_TRUE(_editor->renderNativeTypes({}).empty());

	const str project = _project.generic_string();
	EXPECT_TRUE(_editor->writeNativeTypes(project, types));
	EXPECT_FALSE(_editor->writeNativeTypes(project, types));
	Put("Assets/Core/Core.slassembly", "{\"assemblyName\": \"Game.Core\"}");
	Put("Assets/Taken/Taken.slassembly", "{\"assemblyName\": \"Space_Game.Native\"}");
	CSharpProjectFiles::Options options = MakeOptions();
	ASSERT_EQ(options.assemblies.definitions.size(), 2u);
	EXPECT_EQ(options.assemblies.definitions[0].name, "Space_Game.Native");
	EXPECT_EQ(options.assemblies.definitions[1].references, (Vector<str>{"Space_Game.Native"}));
	EXPECT_TRUE(HasProblem(options.assemblies, "Assets/Taken/Taken.slassembly", "(taken by engine). Rename it"));
	CSharpProjectFiles::Result written;
	ASSERT_TRUE(_editor->writeProjectFiles(options, written));
	const str native = Read("Space_Game.Native.Scripts.csproj");
	EXPECT_NE(native.find("<Compile Include=\"Library/CSharp/Generated/Native/**/*.cs\" />"), str::npos) << native;
	EXPECT_NE(Read("Game.Core.Scripts.csproj").find("<ProjectReference Include=\"Space_Game.Native.Scripts.csproj\" Private=\"false\" />"), str::npos);
	const str main = Read("Space_Game.Scripts.csproj");
	EXPECT_NE(main.find("<ProjectReference Include=\"Space_Game.Native.Scripts.csproj\" Private=\"false\" />"), str::npos) << main;
	EXPECT_NE(main.find("<Compile Include=\"Assets/**/*.cs\" Exclude=\"Assets/Core/**/*.cs\" />"), str::npos) << main;

	EXPECT_TRUE(_editor->writeNativeTypes(project, {}));
	EXPECT_FALSE(fs::exists(_project / "Library" / "CSharp" / "Generated" / "Native" / "NativeTypes.g.cs"));
	options = MakeOptions();
	ASSERT_EQ(options.assemblies.definitions.size(), 1u);
	EXPECT_TRUE(options.assemblies.definitions[0].references.empty());
	ASSERT_TRUE(_editor->writeProjectFiles(options, written));
	EXPECT_FALSE(fs::exists(_project / "Space_Game.Native.Scripts.csproj"));
}

TEST_F(CSharpAssembliesTest, WrappersReachProjectComponents) {
	SceneSystem::Initialize();
	ASSERT_TRUE(Attach()) << _error;
	Put("CMakeLists.txt", "");
	fs::create_directories(_project / "Source");
	const str project = _project.generic_string();
	const TypeInfo* types[] = {NativeProbeEngine::GetReflectType(), NativeProbeGauge::GetReflectType()};
	ASSERT_TRUE(_editor->writeNativeTypes(project, types));
	Put("Assets/Driver.cs", DRIVER);
	Put("Assets/Calls/Calls.slassembly", "{\"assemblyName\": \"Game.Calls\"}");
	Put("Assets/Calls/Halves.cs", HALVES);
	if (!Prepare()) {
		SceneSystem::Terminate();
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	_editor->notifySourceChanged();
	ASSERT_TRUE(CompileAgain()) << Said();
	ASSERT_EQ(_editor->getReloadOutcome(), CSharpReload::Outcome::Loaded) << Said();
	EXPECT_TRUE(fs::is_regular_file(Output("Space_Game.Native")));
	const TypeInfo* driverType = TypeRegistry::FindType(StringHash("Space_Game.Driver"));
	ASSERT_NE(driverType, nullptr) << Said();
	EXPECT_EQ(ScriptFunctions::Find<i32(i32)>("Game.Calls.Half")(42), 21) << Said();
	{
		CSharpScenarios::World world;
		Handle<SceneObject> car = SceneSystem::CreateObject("Car", world.scene);
		Handle<NativeProbeEngine> engine = car->AddComponent<NativeProbeEngine>();
		Handle<NativeProbeGauge> gauge = car->AddComponent<NativeProbeGauge>();
		Handle<Component> driver = car->AddComponent(driverType);
		ASSERT_TRUE(engine.IsValid() && gauge.IsValid() && driver.IsValid());
		EXPECT_FLOAT_EQ(engine->power, 3.0f) << Said();
		EXPECT_EQ(engine->_cylinders, 6);
		EXPECT_EQ(engine->label, "v8!");
		EXPECT_TRUE(engine->running);
		EXPECT_EQ(engine->mount, Vec3(2.0f, 2.0f, 3.0f));
		EXPECT_EQ(engine->paint, Color(0.5f, 0.25f, 0.0f, 1.0f));
		EXPECT_EQ(engine->gear, NativeTypesFixture::Gear::High);
		EXPECT_EQ(engine->anchor, car);
		EXPECT_EQ(engine->dial, gauge);
		EXPECT_EQ(engine->revs, 1);
		EXPECT_EQ(gauge->level, 42);
		EXPECT_EQ(CSharpScenarios::Read<Handle<NativeProbeEngine>>(driver, "engine"), engine);
		EXPECT_EQ(CSharpScenarios::Read<Handle<NativeProbeGauge>>(driver, "gauge"), gauge);
		const FieldInfo* slot = FindField(driverType, StringHash("engine"));
		ASSERT_NE(slot, nullptr);
		EXPECT_EQ(TypeRegistry::FindType(HandlePointeeName(*slot->fieldType)), NativeProbeEngine::GetReflectType());
	}

	str stale = _editor->renderNativeTypes(types);
	const str power = std::format("0x{:016x}UL", StringHash("power").GetValue());
	for (size_t at = stale.find(power); at != str::npos; at = stale.find(power, at)) {
		stale.replace(at, power.size(), "0x0000000000000001UL");
	}
	Put("Library/CSharp/Generated/Native/NativeTypes.g.cs", stale);
	_editor->notifySourceChanged();
	ASSERT_TRUE(CompileAgain()) << Said();
	ASSERT_EQ(_editor->getReloadCount(), 2u) << Said();
	{
		CSharpScenarios::World world;
		Handle<SceneObject> car = SceneSystem::CreateObject("Car", world.scene);
		Handle<NativeProbeEngine> engine = car->AddComponent<NativeProbeEngine>();
		car->AddComponent<NativeProbeGauge>();
		car->AddComponent(TypeRegistry::FindType(StringHash("Space_Game.Driver")));
		ASSERT_TRUE(engine.IsValid());
		EXPECT_FLOAT_EQ(engine->power, 1.5f);
		EXPECT_EQ(engine->_cylinders, 4);
		EXPECT_NE(Said().find("C++ field is gone. Compile the C# scripts again"), str::npos) << Said();
	}

	str renamed = _editor->renderNativeTypes(types);
	const size_t level = renamed.find("public int Level {");
	ASSERT_NE(level, str::npos);
	renamed.replace(level, 18, "public int Depth {");
	Put("Library/CSharp/Generated/Native/NativeTypes.g.cs", renamed);
	_editor->notifySourceChanged();
	EXPECT_FALSE(CompileAgain());
	EXPECT_NE(Said().find("'NativeProbeGauge' does not contain a definition for 'Level'"), str::npos) << Said();

	ASSERT_TRUE(_editor->writeNativeTypes(project, {}));
	Put("Assets/Driver.cs", IDLE);
	_editor->notifyDefinitionsChanged();
	ASSERT_TRUE(CompileAgain()) << Said();
	EXPECT_FALSE(fs::exists(_project / "Space_Game.Native.Scripts.csproj"));
	EXPECT_NE(TypeRegistry::FindType(StringHash("Space_Game.Idle")), nullptr) << Said();

	_editor->shutdownAuthoring();
	_hooks->detach();
	SceneSystem::Terminate();
}

#endif
