#include "TestsPCH.h"

#if defined(SHALLTEAR_SHARED)

#include "CSharpRuntimeFixture.h"
#include "CSharpScenarioUtil.h"
#include "EditorTestHooks.h"
#include "GameModuleHost.h"
#include "TestPlatform.h"
#include "UITestUtil.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Asset/AssetImporter.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/UI/DragDrop.h"

#include "Shalltear/Editor/AssetBrowserOps.h"
#include "Shalltear/Editor/AssetOpeners.h"
#include "Shalltear/Editor/CompileStatus.h"
#include "Shalltear/Editor/ComponentDrops.h"
#include "Shalltear/Editor/EditorConsole.h"
#include "Shalltear/Editor/EditorDragData.h"
#include "Shalltear/Editor/EditorExtensions.h"
#include "Shalltear/Editor/EditorMenus.h"
#include "Shalltear/Editor/EditorPreviews.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/EditorSelection.h"
#include "Shalltear/Editor/PropertyDrawers.h"
#include "Shalltear/Editor/UndoSystem.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

using namespace Shalltear;
using namespace Shalltear::CSharpEditor;
using namespace Shalltear::CSharpTests;

namespace {

	namespace fs = std::filesystem;

	constexpr const char* SPINNER = "using Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Spinner : Behavior {\n\tpublic float speed = 90f;\n}\n\n"
									"public sealed class Helper : Behavior {\n}\n\npublic abstract class Base : Behavior {\n}\n";

	constexpr const char* LONE_SPINNER = "using Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Spinner : Behavior {\n\tpublic float speed = 5f;\n}\n";

	constexpr const char* SOLO = "using Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Other : Behavior {\n}\n";

	constexpr const char* PAIR = "using Shalltear;\n\nnamespace Space_Game;\n\npublic sealed class Left : Behavior {\n}\n\npublic sealed class Right : Behavior {\n}\n";

	constexpr const char* NOTES = "namespace Space_Game;\n\npublic sealed class Notes {\n\tpublic int pages;\n}\n";

	constexpr const char* SHAPE = "using Shalltear;\n\nnamespace Space_Game;\n\npublic abstract class Shape : Behavior {\n}\n\npublic sealed class Circle : Shape {\n}\n";

	char _scriptOwner = 0;

	class CSharpScriptAssetTest : public CSharpRuntimeFixture {
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
			_root = fs::temp_directory_path() / ("shalltear_csharp_scripts_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			_project = _root / "Space Game";
			fs::create_directories(_project / "Assets" / "Scripts");
			std::ofstream(_project / "Project.ini", std::ios::binary) << "[Project]\nname = Space Game\n";
			FileSystem::Mount("project", Path(_project.generic_string()));
			AssetDatabase::Initialize();
			_editor->registerScriptImporter(&_scriptOwner);
			EditorConsole::Initialize();
			SceneSystem::Initialize();
			EditorSceneOps::Initialize();
			EditorSelection::Initialize();
			UndoSystem::Initialize();
			PropertyDrawers::Initialize();
			_hooks->installComponentMenu();
			ASSERT_TRUE(Attach()) << _error;
			_world = SceneSystem::CreateScene("World");
		}

		void TearDown() override {
			if (_editor) {
				_editor->shutdownAuthoring();
			}
			EditorExtensions::Unload(&_scriptOwner);
			AssetImporterRegistry::UnregisterOwnedBy(&_scriptOwner);
			ComponentDrops::Terminate();
			AssetOpeners::Terminate();
			EditorMenus::Terminate();
			PropertyDrawers::Terminate();
			UndoSystem::Terminate();
			EditorSelection::Terminate();
			EditorSceneOps::Terminate();
			SceneSystem::Terminate();
			EditorConsole::Terminate();
			AssetDatabase::Terminate();
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

		str Read(const str& relative) const {
			std::ifstream file(_project / relative, std::ios::binary);
			return str(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
		}

		static Guid Script(const char* name) {
			return AssetDatabase::LookupGuid(Path(str("project://Assets/Scripts/") + name));
		}

		bool Compile() {
			const str sdk = _hooks->findSDKRoot();
			if (sdk.empty()) {
				return false;
			}
			CSharpProjectFiles::Options files = _editor->getDefaultProjectOptions(_project.generic_string(), sdk);
			CSharpProjectFiles::Result written;
			if (!_editor->writeProjectFiles(files, written)) {
				return false;
			}
			_editor->shutdownAuthoring();
			EXPECT_EQ(_hooks->unloadUserAssemblies(), SLT_OK);
			_options.projectPath = _project.generic_string();
			_options.projectFile = "Space_Game.Scripts.csproj";
			_options.sdkRoot = sdk;
			_options.buildServers = false;
			_options.watchSources = false;
			_options.openBuild = true;
			_editor->beginAuthoring(_options, &_scriptOwner);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
			while (std::chrono::steady_clock::now() < deadline) {
				_editor->pumpAuthoring();
				if (!_editor->getAuthoringCompile().IsBusy() && (_editor->getReloadCount() > 0 || _editor->getAuthoringCompile().GetState() == CompileState::Failed)) {
					return true;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			return false;
		}

		Vector<str> Components(const Guid& script) {
			Vector<const TypeInfo*> types{GetMemoryResource(MemTag::Editor)};
			_editor->getScriptComponents(script, types);
			Vector<str> names{GetMemoryResource(MemTag::Editor)};
			for (const TypeInfo* type : types) {
				names.push_back(type->displayName);
			}
			return names;
		}

		static UI::DragPayload Dragged(const Guid& script) {
			AssetDragData data;
			data.guid = script;
			data.assetTypeHash = StringHash("CSharpScriptAsset").GetValue();
			UI::DragPayload payload;
			payload.type = "Asset"_hash;
			payload.data = Variant(data);
			return payload;
		}
	};

} // anonymous namespace

TEST_F(CSharpScriptAssetTest, ScanGivesScriptsTheirType) {
	Put("Assets/Scripts/Spinner.cs", SPINNER);
	Put("Assets/Scripts/Notes.cs", NOTES);
	AssetDatabase::ScanProjectDirectory();

	const Guid spinner = Script("Spinner.cs");
	ASSERT_TRUE(spinner.IsValid());
	ASSERT_TRUE(Script("Notes.cs").IsValid());
	const str sidecar = Read("Assets/Scripts/Spinner.cs.meta");
	EXPECT_NE(sidecar.find(std::format("{:016x}", StringHash("CSharpScriptAsset").GetValue())), str::npos) << sidecar;
	Vector<Guid> scripts{GetMemoryResource(MemTag::Editor)};
	AssetDatabase::FindAssets(StringHash("CSharpScriptAsset"), scripts);
	EXPECT_EQ(scripts.size(), 2u);

	Asset* loaded = AssetDatabase::LoadAssetByGuid(spinner);
	ASSERT_NE(loaded, nullptr);
	EXPECT_EQ(loaded->GetType()->name, StringHash("CSharpScriptAsset"));
	EXPECT_EQ(loaded->GetAssetTypeId(), StringHash("CSharpScriptAsset"));
	AssetDatabase::UnloadAsset(loaded);
	EXPECT_TRUE(Components(spinner).empty());
}

TEST_F(CSharpScriptAssetTest, ListsItsComponentsAfterCompile) {
	Put("Assets/Scripts/Spinner.cs", SPINNER);
	Put("Assets/Scripts/Nested/Solo.cs", SOLO);
	Put("Assets/Scripts/Notes.cs", NOTES);
	AssetDatabase::ScanProjectDirectory();
	if (!Compile()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(_editor->getAuthoringCompile().GetState(), CompileState::Succeeded) << _log.Delivered().Joined();

	const Vector<str> spinner = Components(Script("Spinner.cs"));
	ASSERT_EQ(spinner.size(), 3u);
	EXPECT_NE(std::find(spinner.begin(), spinner.end(), "Spinner"), spinner.end());
	EXPECT_NE(std::find(spinner.begin(), spinner.end(), "Helper"), spinner.end());
	EXPECT_NE(std::find(spinner.begin(), spinner.end(), "Base"), spinner.end());
	const Vector<str> solo = Components(AssetDatabase::LookupGuid(Path("project://Assets/Scripts/Nested/Solo.cs")));
	ASSERT_EQ(solo.size(), 1u);
	EXPECT_EQ(solo[0], "Other");
	EXPECT_TRUE(Components(Script("Notes.cs")).empty());
	EXPECT_TRUE(Components(Guid{}).empty());
	EXPECT_TRUE(Components(Guid{5, 5}).empty());

	Put("Assets/Scripts/Spinner.cs", LONE_SPINNER);
	ASSERT_TRUE(Compile());
	ASSERT_EQ(_editor->getAuthoringCompile().GetState(), CompileState::Succeeded) << _log.Delivered().Joined();
	const Vector<str> after = Components(Script("Spinner.cs"));
	ASSERT_EQ(after.size(), 1u);
	EXPECT_EQ(after[0], "Spinner");
}

TEST_F(CSharpScriptAssetTest, CreateWritesMatchingClass) {
	const Path folder("project://Assets/Scripts");
	EXPECT_TRUE(_editor->isValidScriptName("Mover"));
	EXPECT_TRUE(_editor->isValidScriptName("_mover2"));
	for (const char* refused : {"", "9lives", "has space", "dash-ed", "class", "object", "string", "event", "namespace"}) {
		EXPECT_FALSE(_editor->isValidScriptName(refused)) << refused;
		EXPECT_FALSE(_editor->createScript(folder, refused, CSharpScripts::Template::Component).IsValid()) << refused;
	}
	EXPECT_EQ(_log.Delivered().Count("can't be a C# class name. Use letters/digits/_ (no leading digit, no keyword)"), 9u);

	const Guid mover = _editor->createScript(folder, "Mover", CSharpScripts::Template::Component);
	ASSERT_TRUE(mover.IsValid()) << _log.Delivered().Joined();
	EXPECT_EQ(Script("Mover.cs"), mover);
	const str written = Read("Assets/Scripts/Mover.cs");
	EXPECT_NE(written.find("namespace Space_Game;"), str::npos) << written;
	EXPECT_NE(written.find("public sealed class Mover : Behavior {"), str::npos) << written;
	EXPECT_EQ(written.find("{{"), str::npos) << written;
	EXPECT_EQ(written.find('\r'), str::npos);
	Asset* loaded = AssetDatabase::LoadAssetByGuid(mover);
	ASSERT_NE(loaded, nullptr);
	EXPECT_EQ(loaded->GetAssetTypeId(), StringHash("CSharpScriptAsset"));
	AssetDatabase::UnloadAsset(loaded);

	EXPECT_FALSE(_editor->createScript(folder, "Mover", CSharpScripts::Template::Component).IsValid());
	EXPECT_EQ(_log.Delivered().Count("'Mover.cs' already exists. Pick another name"), 1u);
	EXPECT_EQ(Read("Assets/Scripts/Mover.cs"), written);

	const Guid ledger = _editor->createScript(Path("project://Assets/Data/Deep"), "Ledger", CSharpScripts::Template::Class);
	ASSERT_TRUE(ledger.IsValid()) << _log.Delivered().Joined();
	const str plain = Read("Assets/Data/Deep/Ledger.cs");
	EXPECT_NE(plain.find("public sealed class Ledger {"), str::npos) << plain;
	EXPECT_EQ(plain.find("Behavior"), str::npos);
	const Guid rooted = _editor->createScript(Path{}, "Rooted", CSharpScripts::Template::Class);
	ASSERT_TRUE(rooted.IsValid());
	EXPECT_TRUE(fs::is_regular_file(_project / "Assets" / "Rooted.cs"));

	if (!Compile()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(_editor->getAuthoringCompile().GetState(), CompileState::Succeeded) << _log.Delivered().Joined();
	EXPECT_EQ(_editor->getAuthoringCompile().GetWarningCount(), 0u) << _log.Delivered().Joined();
	const Vector<str> made = Components(mover);
	ASSERT_EQ(made.size(), 1u);
	EXPECT_EQ(made[0], "Mover");
	EXPECT_TRUE(Components(ledger).empty());
	const TypeInfo* type = TypeRegistry::FindType(StringHash("Space_Game.Mover"));
	ASSERT_NE(type, nullptr);
	EXPECT_NE(FindField(type, "degreesPerSecond"_hash), nullptr);
	EXPECT_NE(FindField(type, "target"_hash), nullptr);

	const u32 compiles = _editor->getAuthoringCompile().GetCompileCount();
	const u32 reloads = _editor->getReloadCount();
	const Guid late = _editor->createScript(folder, "Late", CSharpScripts::Template::Component);
	ASSERT_TRUE(late.IsValid());
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
	while (std::chrono::steady_clock::now() < deadline && _editor->getReloadCount() == reloads) {
		_editor->pumpAuthoring();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	EXPECT_EQ(_editor->getAuthoringCompile().GetCompileCount(), compiles + 1) << _log.Delivered().Joined();
	const Vector<str> added = Components(late);
	ASSERT_EQ(added.size(), 1u) << _log.Delivered().Joined();
	EXPECT_EQ(added[0], "Late");
}

TEST_F(CSharpScriptAssetTest, DropAddsTheMatchingComponent) {
	Put("Assets/Scripts/Spinner.cs", SPINNER);
	Put("Assets/Scripts/Solo.cs", SOLO);
	Put("Assets/Scripts/Shape.cs", SHAPE);
	AssetDatabase::ScanProjectDirectory();
	_editor->registerScriptEditor(&_scriptOwner);
	if (!Compile()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(_editor->getAuthoringCompile().GetState(), CompileState::Succeeded) << _log.Delivered().Joined();
	EXPECT_EQ(_editor->resolveScriptComponent(Script("Shape.cs")), TypeRegistry::FindType(StringHash("Space_Game.Circle")));

	Handle<SceneObject> object = SceneSystem::CreateObject("Holder", _world);
	const UI::DragPayload spinner = Dragged(Script("Spinner.cs"));
	ASSERT_TRUE(ComponentDrops::Accepts(spinner));
	Handle<Component> added = ComponentDrops::Drop(spinner, object);
	ASSERT_TRUE(added.IsValid()) << _log.Delivered().Joined();
	EXPECT_EQ(added->GetType(), TypeRegistry::FindType(StringHash("Space_Game.Spinner")));
	EXPECT_EQ(_editor->resolveScriptComponent(Script("Spinner.cs")), added->GetType());

	Handle<Component> other = ComponentDrops::Drop(Dragged(Script("Solo.cs")), object);
	ASSERT_TRUE(other.IsValid());
	EXPECT_EQ(other->GetType(), TypeRegistry::FindType(StringHash("Space_Game.Other")));
	EXPECT_EQ(object->GetComponentCount(), 2u);
	ASSERT_TRUE(UndoSystem::Undo());
	ASSERT_TRUE(UndoSystem::Undo());
	SceneSystem::EndFrame();
	EXPECT_EQ(object->GetComponentCount(), 0u);
	EXPECT_EQ(_log.Delivered().CountAtLeast(LogLevel::Warn), 0u) << _log.Delivered().Joined();

	UITest::MockFontSystem fonts;
	UI::Context context(UI::ContextDesc{}, &fonts);
	Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
	EditorMenus::AppendContributed("Scripts", context, items);
	ASSERT_EQ(items.size(), 3u);
	EXPECT_EQ(items[1].label, "Compile C#");
	EXPECT_TRUE(items[1].enabled);
	CompileStatus::RequestCompile();
	_editor->pumpAuthoring();
	ASSERT_TRUE(_editor->getAuthoringCompile().IsBusy());
	items.clear();
	EditorMenus::AppendContributed("Scripts", context, items);
	ASSERT_EQ(items.size(), 3u);
	EXPECT_FALSE(items[1].enabled);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
	while (std::chrono::steady_clock::now() < deadline && _editor->getAuthoringCompile().IsBusy()) {
		_editor->pumpAuthoring();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

TEST_F(CSharpScriptAssetTest, DropRefusesAmbiguousFile) {
	Put("Assets/Scripts/Pair.cs", PAIR);
	Put("Assets/Scripts/Notes.cs", NOTES);
	AssetDatabase::ScanProjectDirectory();
	_editor->registerScriptEditor(&_scriptOwner);
	Handle<SceneObject> object = SceneSystem::CreateObject("Holder", _world);
	EXPECT_FALSE(ComponentDrops::Drop(Dragged(Script("Pair.cs")), object).IsValid());
	EXPECT_EQ(_log.Delivered().Count("'Pair.cs' isn't loaded yet (C# scripts still starting). Drop it again in a moment"), 1u) << _log.Delivered().Joined();

	if (!Compile()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	ASSERT_EQ(_editor->getAuthoringCompile().GetState(), CompileState::Succeeded) << _log.Delivered().Joined();
	EXPECT_TRUE(ComponentDrops::Accepts(Dragged(Script("Pair.cs"))));
	EXPECT_FALSE(ComponentDrops::Drop(Dragged(Script("Pair.cs")), object).IsValid());
	EXPECT_EQ(_log.Delivered().Count("'Pair.cs' has 2 components, none named 'Pair'. Pick one in Add Component"), 1u) << _log.Delivered().Joined();
	EXPECT_FALSE(ComponentDrops::Drop(Dragged(Script("Notes.cs")), object).IsValid());
	EXPECT_EQ(_log.Delivered().Count("'Notes.cs' has no component to add. Derive a class from Behavior in it"), 1u);
	EXPECT_EQ(object->GetComponentCount(), 0u);
	EXPECT_EQ(UndoSystem::GetUndoDepth(), 0u);

	Put("Assets/Scripts/Pair.cs", str(PAIR) + "broken\n");
	ASSERT_TRUE(Compile());
	ASSERT_EQ(_editor->getAuthoringCompile().GetState(), CompileState::Failed);
	EXPECT_FALSE(ComponentDrops::Drop(Dragged(Script("Pair.cs")), object).IsValid());
	EXPECT_EQ(_log.Delivered().Count("'Pair.cs' isn't compiled (scripts have errors). Fix them, drop it again"), 1u) << _log.Delivered().Joined();

	CompileStatus::RequestCompile();
	_editor->pumpAuthoring();
	ASSERT_TRUE(_editor->getAuthoringCompile().IsBusy());
	EXPECT_FALSE(ComponentDrops::Drop(Dragged(Script("Pair.cs")), object).IsValid());
	EXPECT_EQ(_log.Delivered().Count("'Pair.cs' is still compiling. Drop it again when compile ends"), 1u) << _log.Delivered().Joined();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
	while (std::chrono::steady_clock::now() < deadline && _editor->getAuthoringCompile().IsBusy()) {
		_editor->pumpAuthoring();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

TEST_F(CSharpScriptAssetTest, OpenerComposesEditorCommand) {
	Put("Assets/Scripts/Spinner.cs", SPINNER);
	Put("Assets/Scripts/Solo.cs", SOLO);
	AssetDatabase::ScanProjectDirectory();
	const fs::path receipt = _root / "opened.txt";
	GameModuleHost host;
	GameModuleHost::Config config;
	config.externalEditorCommand = TestPlatform::GetEditorProbeCommand(receipt.generic_string());
	host.Initialize(config, nullptr);

	EXPECT_FALSE(AssetOpeners::Has(".cs"));
	EXPECT_FALSE(AssetOpeners::Open(Path("project://Assets/Scripts/Spinner.cs")));
	_editor->registerScriptEditor(&_scriptOwner);
	ASSERT_TRUE(AssetOpeners::Has(".cs"));
	ASSERT_TRUE(AssetOpeners::Open(Path("project://Assets/Scripts/Spinner.cs")));

	str seen;
	for (int attempt = 0; attempt < 200 && seen.find(']') == str::npos; ++attempt) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		std::ifstream file(receipt, std::ios::binary);
		seen.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	}
	const str expected = FileSystem::Resolve(Path("project://Assets/Scripts/Spinner.cs")).ToString();
	EXPECT_NE(seen.find("[" + expected + "] [1]"), str::npos) << seen;

	const AssetInspectorFn* draw = PropertyDrawers::FindAssetInspector(StringHash("CSharpScriptAsset"));
	ASSERT_NE(draw, nullptr);
	UITest::MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {500, 900};
	UI::Context context(desc, &fonts);
	UI::ElementHandle root = context.CreateElement();
	context.AppendChild(context.GetRoot(), root);
	const Guid solo = Script("Solo.cs");
	Asset* asset = AssetDatabase::LoadAssetByGuid(solo);
	ASSERT_NE(asset, nullptr);
	(*draw)(context, root, solo, *asset);
	context.Update();
	UI::ElementHandle open = UITest::FindByText(context, root, "Open");
	ASSERT_TRUE(open.IsValid());
	UITest::ClickElement(context, open);
	for (int attempt = 0; attempt < 200 && seen.find("Solo.cs] [1]") == str::npos; ++attempt) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		std::ifstream file(receipt, std::ios::binary);
		seen.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	}
	EXPECT_NE(seen.find("Solo.cs] [1]"), str::npos) << seen;
	AssetDatabase::UnloadAsset(asset);
	host.Terminate();
	CompileStatus::UnregisterOwnedBy(&host);
}

TEST_F(CSharpScriptAssetTest, InspectorShowsComponentsAndSource) {
	Put("Assets/Scripts/Spinner.cs", SPINNER);
	Put("Assets/Scripts/Notes.cs", NOTES);
	str tall = "namespace Space_Game;\n\npublic sealed class Tall {\n";
	for (u32 line = 4; line <= 60; ++line) {
		tall += "\t// row " + std::to_string(line) + " of the file\n";
	}
	Put("Assets/Scripts/Tall.cs", tall + "}\n");
	AssetDatabase::ScanProjectDirectory();
	_editor->registerScriptEditor(&_scriptOwner);
	if (!Compile()) {
		GTEST_SKIP() << "pinned .NET SDK not found";
	}
	const AssetInspectorFn* draw = PropertyDrawers::FindAssetInspector(StringHash("CSharpScriptAsset"));
	ASSERT_NE(draw, nullptr);

	const auto rows = [&](const char* file, std::initializer_list<const char*> present, std::initializer_list<const char*> absent) {
		UITest::MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {500, 900};
		UI::Context context(desc, &fonts);
		UI::ElementHandle root = context.CreateElement();
		context.AppendChild(context.GetRoot(), root);
		const Guid guid = Script(file);
		Asset* asset = AssetDatabase::LoadAssetByGuid(guid);
		ASSERT_NE(asset, nullptr);
		(*draw)(context, root, guid, *asset);
		context.Update();
		for (const char* text : present) {
			EXPECT_TRUE(UITest::FindByText(context, root, text).IsValid()) << file << ": " << text;
		}
		for (const char* text : absent) {
			EXPECT_FALSE(UITest::FindByText(context, root, text).IsValid()) << file << ": " << text;
		}
		AssetDatabase::UnloadAsset(asset);
	};
	rows("Spinner.cs", {"Spinner", "Helper", "Base"}, {"None compiled from this file."});
	rows("Notes.cs", {"None compiled from this file."}, {"Spinner", "Helper", "Base"});

	const auto shown = [&](const char* file, std::initializer_list<const char*> present, std::initializer_list<const char*> absent) {
		UITest::MockFontSystem fonts;
		UI::ContextDesc desc;
		desc.sizePx = {500, 900};
		UI::Context context(desc, &fonts);
		UI::ElementHandle root = context.CreateElement();
		context.AppendChild(context.GetRoot(), root);
		const Guid guid = Script(file);
		Asset* asset = AssetDatabase::LoadAssetByGuid(guid);
		ASSERT_NE(asset, nullptr);
		(*draw)(context, root, guid, *asset);
		context.Update();
		for (const char* text : present) {
			EXPECT_TRUE(UITest::FindByTextContaining(context, root, text).IsValid()) << file << ": " << text;
		}
		for (const char* text : absent) {
			EXPECT_FALSE(UITest::FindByTextContaining(context, root, text).IsValid()) << file << ": " << text;
		}
		AssetDatabase::UnloadAsset(asset);
	};
	shown("Spinner.cs", {"C# Script", "Open", "Components", "Helper", "Base", "Source", "public sealed class Spinner : Behavior {"}, {"None compiled from this file."});
	shown("Notes.cs", {"Components", "None compiled from this file.", "public sealed class Notes {", " public int pages;"}, {"Helper", "\tpublic int pages;", "..."});
	shown("Tall.cs", {"// row 40 of the file\n...", " // row 4 of the file"}, {"// row 41 of the file", "\t// row"});
}

TEST_F(CSharpScriptAssetTest, EditorPartsAreSweptWithTheirOwner) {
	const fs::path receipt = _root / "opened.txt";
	GameModuleHost host;
	GameModuleHost::Config config;
	config.externalEditorCommand = TestPlatform::GetEditorProbeCommand(receipt.generic_string());
	host.Initialize(config, nullptr);
	_editor->registerScriptEditor(&_scriptOwner);
	Vector<AssetBrowserOps::CreateMenuEntry> entries{GetMemoryResource(MemTag::Editor)};
	AssetBrowserOps::GetCreateMenuEntries(entries);
	u32 named = 0;
	for (const AssetBrowserOps::CreateMenuEntry& entry : entries) {
		named += entry.asksName && (entry.label == "C# Script" || entry.label == "C# Class") ? 1u : 0u;
	}
	EXPECT_EQ(named, 2u);
	EXPECT_NE(PropertyDrawers::FindAssetInspector(StringHash("CSharpScriptAsset")), nullptr);
	EXPECT_EQ(EditorPreviews::BadgeForFileName("Spinner.cs").icon.glyph, EditorIcons::Glyph::AssetScript.glyph);
	UITest::MockFontSystem fonts;
	UI::ContextDesc desc;
	desc.sizePx = {800, 600};
	UI::Context context(desc, &fonts);
	Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
	EditorMenus::AppendContributed("Scripts", context, items);
	ASSERT_EQ(items.size(), 3u);
	EXPECT_EQ(items[0].label, "New C# Script...");
	EXPECT_TRUE(items[0].separatorBefore);
	EXPECT_EQ(items[1].label, "Compile C#");
	EXPECT_FALSE(items[1].enabled);
	EXPECT_EQ(items[2].label, "Open C# Project");
	EXPECT_TRUE(items[2].enabled);
	items.clear();

	const Guid made = AssetBrowserOps::RunCreateEntry("C# Script", Path("project://Assets/Scripts"), "Jumper");
	ASSERT_TRUE(made.IsValid()) << _log.Delivered().Joined();
	EXPECT_NE(Read("Assets/Scripts/Jumper.cs").find("class Jumper : Behavior"), str::npos);
	str seen;
	for (int attempt = 0; attempt < 200 && seen.find(']') == str::npos; ++attempt) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		std::ifstream file(receipt, std::ios::binary);
		seen.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	}
	EXPECT_NE(seen.find("Jumper.cs] [1]"), str::npos) << seen;
	EXPECT_FALSE(AssetBrowserOps::RunCreateEntry("C# Class", Path("project://Assets/Scripts"), "class").IsValid());
	ASSERT_TRUE(AssetBrowserOps::RunCreateEntry("C# Class", Path("project://Assets/Scripts"), "Ledger").IsValid()) << _log.Delivered().Joined();
	EXPECT_NE(Read("Assets/Scripts/Ledger.cs").find("public sealed class Ledger {"), str::npos);
	EXPECT_EQ(Read("Assets/Scripts/Ledger.cs").find("Behavior"), str::npos);

	EditorMenus::AppendContributed("Scripts", context, items);
	ASSERT_EQ(items.size(), 3u);
	items[0].onSelect();
	context.Update();
	ASSERT_EQ(context.GetOpenPopupCount(), 1u);
	UI::ElementHandle accept = UITest::FindByText(context, context.GetOpenPopup(0), "Create");
	ASSERT_TRUE(accept.IsValid());
	UITest::ClickElement(context, accept);
	EXPECT_NE(Read("Assets/Scripts/NewScript.cs").find("class NewScript : Behavior"), str::npos);
	EXPECT_FALSE(fs::exists(_project / "Assets" / "NewScript.cs"));
	items.clear();

	EXPECT_GE(EditorExtensions::Unload(&_scriptOwner), 8u);
	AssetBrowserOps::GetCreateMenuEntries(entries);
	for (const AssetBrowserOps::CreateMenuEntry& entry : entries) {
		EXPECT_FALSE(entry.asksName) << entry.label;
	}
	EXPECT_FALSE(AssetOpeners::Has(".cs"));
	EditorMenus::AppendContributed("Scripts", context, items);
	EXPECT_TRUE(items.empty());
	EXPECT_EQ(PropertyDrawers::FindAssetInspector(StringHash("CSharpScriptAsset")), nullptr);
	EXPECT_FALSE(ComponentDrops::Accepts(Dragged(made)));
	host.Terminate();
	CompileStatus::UnregisterOwnedBy(&host);
}

#endif
