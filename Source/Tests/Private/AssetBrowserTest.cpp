#include "TestsPCH.h"

#include "UITestUtil.h"
#include "CookHarness.h"

#include "Shalltear/Core/Asset.h"
#include "Shalltear/Core/Graphics/MaterialAsset.h"
#include "Shalltear/Core/Graphics/MeshRenderer.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/UI/UISystem.h"
#include "Shalltear/Core/Window.h"

#include "Shalltear/Editor/AssetBrowserOps.h"
#include "Shalltear/Editor/AssetOpeners.h"
#include "Shalltear/Editor/EditorAssetWatch.h"
#include "Shalltear/Editor/EditorExtensions.h"
#include "Shalltear/Editor/EditorDragData.h"
#include "Shalltear/Editor/EditorEvents.h"
#include "Shalltear/Editor/EditorPreviews.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/EditorSelection.h"
#include "Shalltear/Editor/EditorTheme.h"
#include "Shalltear/Editor/PanelManager.h"
#include "Shalltear/Editor/UndoSystem.h"

#include <atomic>
#include <chrono>
#include <filesystem>

using namespace Shalltear;
using namespace UITest;

namespace {

	SceneObjectDragData DraggedObjects(std::initializer_list<Handle<SceneObject>> objects) {
		SceneObjectDragData data;
		for (Handle<SceneObject> object : objects) {
			data.objects.push_back(object.GetInstanceId());
		}
		return data;
	}

	class AssetBrowserFixture : public ::testing::Test {
	protected:
		std::filesystem::path _root;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			u64 unique = counter.fetch_add(1);
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() /
					("shalltear_browser_" + std::to_string(stamp) + "_" + std::to_string(unique));
			std::filesystem::create_directories(_root / "Assets");

			FileSystem::Mount("project", Path(_root.generic_string()));
			AssetDatabase::Initialize();
		}

		void TearDown() override {
			AssetDatabase::Terminate();
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		void WriteSource(const str& virtualPath, const str& content) {
			Path resolved = FileSystem::Resolve(Path(virtualPath));
			std::filesystem::path fsPath(resolved.ToString());
			std::error_code ec;
			std::filesystem::create_directories(fsPath.parent_path(), ec);
			FileSystem::WriteTextFile(resolved, content);
		}

		static const AssetBrowserOps::Entry* Find(const Vector<AssetBrowserOps::Entry>& entries, const str& relPath) {
			for (const AssetBrowserOps::Entry& entry : entries) {
				if (entry.relPath == relPath) {
					return &entry;
				}
			}
			return nullptr;
		}
	};

} // namespace

TEST_F(AssetBrowserFixture, ScanTreeSkipsSidecarsAndTheIndex) {
	WriteSource("project://Assets/a.txt", "a");
	WriteSource("project://Assets/a.txt.meta", "{}");
	WriteSource("project://Assets/.assets.db", "{}");
	WriteSource("project://Assets/Sub/b.txt", "b");

	Vector<AssetBrowserOps::Entry> entries{GetMemoryResource(MemTag::Editor)};
	ASSERT_TRUE(AssetBrowserOps::ScanTree(entries));

	EXPECT_NE(Find(entries, "Assets/a.txt"), nullptr);
	EXPECT_NE(Find(entries, "Assets/Sub"), nullptr);
	EXPECT_NE(Find(entries, "Assets/Sub/b.txt"), nullptr);
	EXPECT_EQ(Find(entries, "Assets/a.txt.meta"), nullptr);
	EXPECT_EQ(Find(entries, "Assets/.assets.db"), nullptr);
}

TEST_F(AssetBrowserFixture, ScanTreeShowsOnlyTheContentRoot) {
	WriteSource("project://Assets/keep.txt", "keep");
	WriteSource("project://Project.ini", "[Project]\nname=X\n");
	WriteSource("project://CMakeLists.txt", "cmake_minimum_required(VERSION 3.25)");
	WriteSource("project://.gitignore", "Library/");
	WriteSource("project://.vscode/settings.json", "{}");
	WriteSource("project://.git/objects/ab/cdef", "binary");
	WriteSource("project://Library/ShaderCache/blob.bin", "x");
	WriteSource("project://Source/Player.cpp", "int main(){}");

	Vector<AssetBrowserOps::Entry> entries{GetMemoryResource(MemTag::Editor)};
	ASSERT_TRUE(AssetBrowserOps::ScanTree(entries));

	ASSERT_EQ(entries.size(), 2u);
	EXPECT_EQ(entries[0].relPath, "Assets");
	EXPECT_TRUE(entries[0].IsDirectory());
	EXPECT_EQ(entries[1].relPath, "Assets/keep.txt");
	for (const AssetBrowserOps::Entry& entry : entries) {
		EXPECT_TRUE(entry.relPath == "Assets" || entry.relPath.starts_with("Assets/")) << entry.relPath;
	}
}

TEST_F(AssetBrowserFixture, NoContentRootScansEmpty) {
	std::error_code ec;
	std::filesystem::remove_all(_root / "Assets", ec);
	WriteSource("project://Project.ini", "[Project]\nname=X\n");

	Vector<AssetBrowserOps::Entry> entries{GetMemoryResource(MemTag::Editor)};
	EXPECT_TRUE(AssetBrowserOps::ScanTree(entries));
	EXPECT_TRUE(entries.empty());
}

TEST_F(AssetBrowserFixture, ScanTreeOrdersDirectoriesFirstDepthFirst) {
	WriteSource("project://Assets/zebra.txt", "z");
	WriteSource("project://Assets/Alpha/inner.txt", "i");
	WriteSource("project://Assets/Alpha/Deep/leaf.txt", "l");
	WriteSource("project://Assets/beta.txt", "b");

	Vector<AssetBrowserOps::Entry> entries{GetMemoryResource(MemTag::Editor)};
	ASSERT_TRUE(AssetBrowserOps::ScanTree(entries));
	ASSERT_EQ(entries.size(), 7u);

	EXPECT_EQ(entries[0].relPath, "Assets");
	EXPECT_TRUE(entries[0].IsDirectory());
	EXPECT_EQ(entries[0].depth, 0u);
	EXPECT_EQ(entries[1].relPath, "Assets/Alpha");
	EXPECT_TRUE(entries[1].IsDirectory());
	EXPECT_EQ(entries[1].depth, 1u);
	EXPECT_EQ(entries[2].relPath, "Assets/Alpha/Deep");
	EXPECT_EQ(entries[2].depth, 2u);
	EXPECT_EQ(entries[3].relPath, "Assets/Alpha/Deep/leaf.txt");
	EXPECT_EQ(entries[3].depth, 3u);
	EXPECT_EQ(entries[4].relPath, "Assets/Alpha/inner.txt");
	EXPECT_EQ(entries[5].relPath, "Assets/beta.txt");
	EXPECT_EQ(entries[6].relPath, "Assets/zebra.txt");
}

TEST_F(AssetBrowserFixture, FolderListsChildrenSubfoldersFirst) {
	WriteSource("project://Assets/zebra.txt", "z");
	WriteSource("project://Assets/Alpha/inner.txt", "i");
	WriteSource("project://Assets/Alpha/Deep/leaf.txt", "l");
	WriteSource("project://Assets/Al/own.txt", "o");
	WriteSource("project://Assets/beta.txt", "b");

	Vector<AssetBrowserOps::Entry> tree{GetMemoryResource(MemTag::Editor)};
	ASSERT_TRUE(AssetBrowserOps::ScanTree(tree));
	Vector<AssetBrowserOps::Entry> contents{GetMemoryResource(MemTag::Editor)};
	auto names = [&contents]() {
		str joined;
		for (const AssetBrowserOps::Entry& entry : contents) {
			joined += (joined.empty() ? "" : ",") + entry.name;
		}
		return joined;
	};

	AssetBrowserOps::FolderContents(tree, "Assets", "", contents);
	EXPECT_EQ(names(), "Al,Alpha,beta.txt,zebra.txt");
	AssetBrowserOps::FolderContents(tree, "Assets/Al", "", contents);
	EXPECT_EQ(names(), "own.txt");
	AssetBrowserOps::FolderContents(tree, "Assets/Alpha", "", contents);
	EXPECT_EQ(names(), "Deep,inner.txt");
	AssetBrowserOps::FolderContents(tree, "Assets", "AL", contents);
	EXPECT_EQ(names(), "Al,Alpha");
	AssetBrowserOps::FolderContents(tree, "Assets", "e", contents);
	EXPECT_EQ(names(), "beta.txt,zebra.txt");
	AssetBrowserOps::FolderContents(tree, "", "", contents);
	EXPECT_TRUE(contents.empty());

	std::reverse(tree.begin(), tree.end());
	AssetBrowserOps::FolderContents(tree, "Assets", "", contents);
	ASSERT_EQ(contents.size(), 4u);
	EXPECT_TRUE(contents[0].IsDirectory());
	EXPECT_TRUE(contents[1].IsDirectory());
	EXPECT_FALSE(contents[2].IsDirectory());
	EXPECT_FALSE(contents[3].IsDirectory());
}

TEST_F(AssetBrowserFixture, ScanTreeUnmountedReturnsFalse) {
	FileSystem::Unmount("project");

	Vector<AssetBrowserOps::Entry> entries{GetMemoryResource(MemTag::Editor)};
	EXPECT_FALSE(AssetBrowserOps::ScanTree(entries));
	EXPECT_TRUE(entries.empty());
	EXPECT_FALSE(AssetBrowserOps::IsProjectMounted());

	FileSystem::Mount("project", Path(_root.generic_string()));
}

TEST_F(AssetBrowserFixture, CreateFolderValidatesNames) {
	EXPECT_TRUE(AssetBrowserOps::CreateFolder(Path("project://Assets"), "Textures"));
	EXPECT_TRUE(FileSystem::IsDirectory(Path("project://Assets/Textures")));

	EXPECT_FALSE(AssetBrowserOps::CreateFolder(Path("project://Assets"), ""));
	EXPECT_FALSE(AssetBrowserOps::CreateFolder(Path("project://Assets"), "a/b"));
	EXPECT_FALSE(AssetBrowserOps::CreateFolder(Path("project://Assets"), ".."));
	EXPECT_FALSE(AssetBrowserOps::CreateFolder(Path("project://Assets"), "Textures"));
}

TEST_F(AssetBrowserFixture, RenameMovesMetaAndKeepsGuid) {
	WriteSource("project://Assets/a.txt", "content");
	AssetDatabase::ScanProjectDirectory();
	Guid original = AssetDatabase::LookupGuid(Path("project://Assets/a.txt"));
	ASSERT_TRUE(original.IsValid());

	ASSERT_TRUE(AssetBrowserOps::RenameEntry(Path("project://Assets/a.txt"), "b.txt"));

	EXPECT_FALSE(FileSystem::Exists(Path("project://Assets/a.txt")));
	EXPECT_FALSE(FileSystem::Exists(Path("project://Assets/a.txt.meta")));
	EXPECT_TRUE(FileSystem::Exists(Path("project://Assets/b.txt")));
	EXPECT_TRUE(FileSystem::Exists(Path("project://Assets/b.txt.meta")));

	EXPECT_EQ(AssetDatabase::LookupGuid(Path("project://Assets/b.txt")), original);
	EXPECT_FALSE(AssetDatabase::LookupGuid(Path("project://Assets/a.txt")).IsValid());
}

TEST(AssetBrowserRenameTest, RenameEditsNameAndKeepsExtension) {
	EXPECT_EQ(AssetBrowserOps::RenameStem("Ground.mat"), "Ground");
	EXPECT_EQ(AssetBrowserOps::RenameStem("README"), "README");
	EXPECT_EQ(AssetBrowserOps::RenameStem(".gitignore"), ".gitignore");
	EXPECT_EQ(AssetBrowserOps::RenameStem("archive.tar.gz"), "archive.tar");

	EXPECT_EQ(AssetBrowserOps::RenamedFileName("old.mat", "Ground"), "Ground.mat");
	EXPECT_EQ(AssetBrowserOps::RenamedFileName("old.mat", "Ground.mat"), "Ground.mat");
	EXPECT_EQ(AssetBrowserOps::RenamedFileName("old.mat", "Ground.MAT"), "Ground.mat");
	EXPECT_EQ(AssetBrowserOps::RenamedFileName("old.mat", "Ground.txt"), "Ground.txt.mat");
	EXPECT_EQ(AssetBrowserOps::RenamedFileName("old.mat", "mat"), "mat.mat");
	EXPECT_EQ(AssetBrowserOps::RenamedFileName("old.mat", ".mat"), "");
	EXPECT_EQ(AssetBrowserOps::RenamedFileName("README", "NOTES"), "NOTES");
	EXPECT_EQ(AssetBrowserOps::RenamedFileName("archive.tar.gz", "backup"), "backup.gz");
}

TEST_F(AssetBrowserFixture, TypedPathResolvesAnySpelling) {
	WriteSource("project://Assets/Shaders/Lit.hlsl", "x");
	WriteSource("project://Assets/Materials/keep.txt", "k");
	Vector<AssetBrowserOps::Entry> entries{GetMemoryResource(MemTag::Editor)};
	ASSERT_TRUE(AssetBrowserOps::ScanTree(entries));
	AssetBrowserOps::Entry link;
	link.kind = AssetBrowserOps::EntryKind::Link;
	link.relPath = "Assets/Shaders/Lit.hlsl";
	entries.insert(entries.begin(), link);

	const AssetBrowserOps::Entry* file = AssetBrowserOps::ResolveTypedPath(entries, "Assets/Shaders/Lit.hlsl");
	ASSERT_NE(file, nullptr);
	EXPECT_EQ(file->kind, AssetBrowserOps::EntryKind::File);

	auto resolve = [&](std::string_view typed) {
		const AssetBrowserOps::Entry* entry = AssetBrowserOps::ResolveTypedPath(entries, typed);
		return entry ? entry->relPath : str("<nothing>");
	};
	EXPECT_EQ(resolve("Assets/Shaders"), "Assets/Shaders");
	EXPECT_EQ(resolve("assets\\shaders\\"), "Assets/Shaders");
	EXPECT_EQ(resolve("  ./Assets//Shaders/  "), "Assets/Shaders");
	EXPECT_EQ(resolve("project://Assets/Shaders"), "Assets/Shaders");
	EXPECT_EQ(resolve("Assets/Shaders/../Materials"), "Assets/Materials");
	EXPECT_EQ(resolve("Assets/Nope"), "<nothing>");
	EXPECT_EQ(resolve(""), "<nothing>");
}

TEST(AssetBrowserHistoryTest, NewVisitDropsForwardHistory) {
	AssetBrowserOps::FolderHistory history;
	auto all = [](std::string_view) { return true; };
	EXPECT_TRUE(history.Current().empty());
	EXPECT_TRUE(history.PeekBack(all).empty());

	history.Visit("Assets");
	history.Visit("Assets/Sub");
	history.Visit("Assets/Sub");
	history.Visit("Assets/Sub/Deep");
	EXPECT_EQ(history.Current(), "Assets/Sub/Deep");
	EXPECT_TRUE(history.PeekForward(all).empty());

	EXPECT_EQ(history.Back(all), "Assets/Sub");
	EXPECT_EQ(history.Back(all), "Assets");
	EXPECT_TRUE(history.Back(all).empty());
	EXPECT_EQ(history.Current(), "Assets");
	EXPECT_EQ(history.Forward(all), "Assets/Sub");

	history.Visit("Assets/Other");
	EXPECT_TRUE(history.PeekForward(all).empty());
	EXPECT_EQ(history.Back(all), "Assets/Sub");
	EXPECT_EQ(history.Back(all), "Assets");
}

TEST(AssetBrowserHistoryTest, StepSkipsGoneAndShownFolders) {
	AssetBrowserOps::FolderHistory history;
	history.Visit("Assets");
	history.Visit("Assets/Gone");
	history.Visit("Assets");
	history.Visit("Assets/Here");
	auto exists = [](std::string_view folder) { return folder != "Assets/Gone"; };

	EXPECT_EQ(history.PeekBack(exists), "Assets");
	EXPECT_EQ(history.Current(), "Assets/Here");
	EXPECT_EQ(history.Back(exists), "Assets");
	EXPECT_TRUE(history.Back(exists).empty());
	EXPECT_EQ(history.Forward(exists), "Assets/Here");
}

TEST(AssetBrowserHistoryTest, RenameKeepsVisitsHistoryHolds50) {
	AssetBrowserOps::FolderHistory history;
	history.Visit("Assets/Old");
	history.Visit("Assets/Old/Inner");
	history.Visit("Assets/Older");
	history.Remap("Assets/Old", "Assets/New");
	auto all = [](std::string_view) { return true; };
	EXPECT_EQ(history.Current(), "Assets/Older");
	EXPECT_EQ(history.Back(all), "Assets/New/Inner");
	EXPECT_EQ(history.Back(all), "Assets/New");

	AssetBrowserOps::FolderHistory capped;
	for (u32 i = 0; i < AssetBrowserOps::FolderHistory::CAPACITY + 10; ++i) {
		capped.Visit(std::format("Assets/F{}", i));
	}
	u32 steps = 0;
	while (!capped.Back(all).empty()) {
		++steps;
	}
	EXPECT_EQ(steps, AssetBrowserOps::FolderHistory::CAPACITY - 1);
	EXPECT_EQ(capped.Current(), "Assets/F10");
}

TEST_F(AssetBrowserFixture, RenameRejectsCollisionAndBadNames) {
	WriteSource("project://Assets/a.txt", "a");
	WriteSource("project://Assets/b.txt", "b");

	EXPECT_FALSE(AssetBrowserOps::RenameEntry(Path("project://Assets/a.txt"), "b.txt"));
	EXPECT_FALSE(AssetBrowserOps::RenameEntry(Path("project://Assets/a.txt"), "x/y.txt"));
	EXPECT_FALSE(AssetBrowserOps::RenameEntry(Path("project://Assets/missing.txt"), "c.txt"));
	EXPECT_TRUE(FileSystem::Exists(Path("project://Assets/a.txt")));
}

TEST_F(AssetBrowserFixture, DeleteRemovesFileMetaAndIndexEntry) {
	WriteSource("project://Assets/doomed.txt", "x");
	AssetDatabase::ScanProjectDirectory();
	ASSERT_TRUE(AssetDatabase::LookupGuid(Path("project://Assets/doomed.txt")).IsValid());

	ASSERT_TRUE(AssetBrowserOps::DeleteEntry(Path("project://Assets/doomed.txt")));

	EXPECT_FALSE(FileSystem::Exists(Path("project://Assets/doomed.txt")));
	EXPECT_FALSE(FileSystem::Exists(Path("project://Assets/doomed.txt.meta")));
	EXPECT_FALSE(AssetDatabase::LookupGuid(Path("project://Assets/doomed.txt")).IsValid());
}

TEST_F(AssetBrowserFixture, RenameDirectoryKeepsChildGuids) {
	WriteSource("project://Assets/Old/child.txt", "c");
	AssetDatabase::ScanProjectDirectory();
	Guid childGuid = AssetDatabase::LookupGuid(Path("project://Assets/Old/child.txt"));
	ASSERT_TRUE(childGuid.IsValid());

	ASSERT_TRUE(AssetBrowserOps::RenameEntry(Path("project://Assets/Old"), "New"));

	EXPECT_EQ(AssetDatabase::LookupGuid(Path("project://Assets/New/child.txt")), childGuid);
	EXPECT_FALSE(AssetDatabase::LookupGuid(Path("project://Assets/Old/child.txt")).IsValid());
	EXPECT_EQ(AssetDatabase::LookupPath(childGuid).ToString(), "project://Assets/New/child.txt");
}

TEST_F(AssetBrowserFixture, RenamingLoadedAssetMovesNextSave) {
	Guid guid = AssetDatabase::CreateAsset(Path("project://Assets/New Panel Settings.slpanel"), StringHash("UIPanelSettings"));
	ASSERT_TRUE(guid.IsValid());
	auto* settings = static_cast<UIPanelSettings*>(AssetDatabase::LoadAssetByGuid(guid));
	ASSERT_NE(settings, nullptr);

	ASSERT_TRUE(AssetBrowserOps::RenameEntry(Path("project://Assets/New Panel Settings.slpanel"), "Game.slpanel"));
	EXPECT_EQ(settings->virtualPath, "project://Assets/Game.slpanel");

	settings->fontSizePx = 28.0f;
	AssetDatabase::SaveAsset(settings);

	EXPECT_FALSE(FileSystem::Exists(Path("project://Assets/New Panel Settings.slpanel")));
	EXPECT_FALSE(FileSystem::Exists(Path("project://Assets/New Panel Settings.slpanel.meta")));
	EXPECT_EQ(AssetDatabase::LookupPath(guid).ToString(), "project://Assets/Game.slpanel");
	str written = FileSystem::ReadTextFile(Path("project://Assets/Game.slpanel"));
	EXPECT_NE(written.find("\"fontSizePx\": 28"), str::npos) << written;

	AssetDatabase::UnloadAsset(settings);
}

TEST_F(AssetBrowserFixture, DeleteDirectoryRemovesSubtree) {
	WriteSource("project://Assets/Gone/a.txt", "a");
	WriteSource("project://Assets/Gone/Deep/b.txt", "b");
	AssetDatabase::ScanProjectDirectory();

	ASSERT_TRUE(AssetBrowserOps::DeleteEntry(Path("project://Assets/Gone")));

	EXPECT_FALSE(FileSystem::Exists(Path("project://Assets/Gone")));
	EXPECT_FALSE(AssetDatabase::LookupGuid(Path("project://Assets/Gone/a.txt")).IsValid());
	EXPECT_FALSE(AssetDatabase::LookupGuid(Path("project://Assets/Gone/Deep/b.txt")).IsValid());
}

TEST(AssetDragDataTest, VariantRoundTripsAndDeepCopies) {
	AssetDragData data;
	data.guid = Guid{0x1234567890abcdefull, 0xfedcba0987654321ull};
	data.assetTypeHash = StringHash("TextureAsset").GetValue();

	Variant held{data};
	ASSERT_TRUE(held.Is<AssetDragData>());

	Variant copy = held;
	AssetDragData out = copy.As<AssetDragData>();
	EXPECT_EQ(out.guid, data.guid);
	EXPECT_EQ(out.assetTypeHash, data.assetTypeHash);
}

namespace {

	class BrowserAssetComponent : public Component {
	public:
		SLT_REFLECT_COMPONENT(BrowserAssetComponent, Component)
		SLT_REFLECT_FIELD(textRef)
		SLT_END_REFLECT()

		BrowserAssetComponent() = default;

		Handle<TextAsset> textRef;
	};

	class AssetBrowserPanelTest : public AssetBrowserFixture {
	protected:
		void SetUp() override {
			AssetBrowserFixture::SetUp();
			TypeRegistry::InitializePrimitives();
			RegisterBuiltInPanels();
			Asset::GetReflectType();
			TextAsset::GetReflectType();
			BrowserAssetComponent::GetReflectType();
			SceneSystem::Initialize();
			EditorSceneOps::Initialize();
			EditorSelection::Initialize();
			UndoSystem::Initialize();
			PanelManager::Initialize();
		}
		void TearDown() override {
			PanelManager::Terminate();
			UndoSystem::Terminate();
			EditorSelection::Terminate();
			EditorSceneOps::Terminate();
			SceneSystem::Terminate();
			AssetBrowserFixture::TearDown();
		}

		static void ClickAt(UI::Context& context, EditorPanel& panel, Vec2 point, KeyMod mods = KeyMod::None) {
			InputEvent event{};
			event.button = MouseButton::Left;
			event.mods = mods;
			event.mousePosPx = {static_cast<i32>(point.x), static_cast<i32>(point.y)};
			event.type = InputEventType::MouseMove;
			context.InjectInput(event);
			event.type = InputEventType::MouseDown;
			context.InjectInput(event);
			event.type = InputEventType::MouseUp;
			context.InjectInput(event);
			context.Update();
			panel.OnTick(0.016f);
			context.Update();
		}

		static void ClickWithoutTick(UI::Context& context, Vec2 point) {
			InputEvent event{};
			event.button = MouseButton::Left;
			event.mousePosPx = {static_cast<i32>(point.x), static_cast<i32>(point.y)};
			event.type = InputEventType::MouseMove;
			context.InjectInput(event);
			event.type = InputEventType::MouseDown;
			context.InjectInput(event);
			event.type = InputEventType::MouseUp;
			context.InjectInput(event);
			context.Update();
		}

		static void RightClickAt(UI::Context& context, EditorPanel& panel, Vec2 point) {
			InputEvent event{};
			event.button = MouseButton::Right;
			event.mousePosPx = {static_cast<i32>(point.x), static_cast<i32>(point.y)};
			event.type = InputEventType::MouseMove;
			context.InjectInput(event);
			event.type = InputEventType::MouseDown;
			context.InjectInput(event);
			event.type = InputEventType::MouseUp;
			context.InjectInput(event);
			context.Update();
			panel.OnTick(0.016f);
			context.Update();
		}

		static UI::ElementHandle MenuRowNamed(UI::Context& context, UI::ElementHandle popup, const str& label) {
			const UI::Element* element = context.GetElement(popup);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				if (context.HasClass(handle, StringHash("slt-menu-item")) && HasText(context, handle, label)) {
					return handle;
				}
				child = context.GetElement(handle)->nextSibling;
			}
			return {};
		}

		static bool RowIsSelected(UI::Context& context, UI::ElementHandle row) {
			return context.HasClass(row, StringHash("slt-selected"));
		}

		static UI::ElementHandle ButtonNamed(UI::Context& context, UI::ElementHandle from, const str& label) {
			if (context.HasClass(from, StringHash("slt-button")) && HasText(context, from, label)) {
				return from;
			}
			const UI::Element* element = context.GetElement(from);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				UI::ElementHandle found = ButtonNamed(context, handle, label);
				if (found.IsValid()) {
					return found;
				}
				child = context.GetElement(handle)->nextSibling;
			}
			return {};
		}

		static void DoubleClick(UI::Context& context, EditorPanel& panel, Vec2 point) {
			for (int click = 0; click < 2; ++click) {
				InputEvent event{};
				event.button = MouseButton::Left;
				event.mousePosPx = {static_cast<i32>(point.x), static_cast<i32>(point.y)};
				event.type = InputEventType::MouseMove;
				context.InjectInput(event);
				event.type = InputEventType::MouseDown;
				context.InjectInput(event);
				event.type = InputEventType::MouseUp;
				context.InjectInput(event);
				context.Update();
				panel.OnTick(0.016f);
				context.Update();
			}
		}

		static Vec2 CenterOf(UI::Context& context, UI::ElementHandle handle) {
			const UI::Rect& box = context.GetLayout(handle).borderBox;
			return {box.x + box.w * 0.5f, box.y + box.h * 0.5f};
		}

		f32 browserIconSize = 0.0f;

		Handle<EditorPanel> OpenBrowser() {
			Handle<EditorPanel> panel =
				PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("AssetBrowserPanel")));
			if (panel.IsValid()) {
				SetIconSize(panel.Get(), browserIconSize);
			}
			return panel;
		}

		static bool SetIconSize(EditorPanel* panel, f32 value) {
			Variant boxed{value};
			return SetField(panel, panel->GetType(), StringHash("iconSize"), &boxed);
		}

		static bool HasText(UI::Context& context, UI::ElementHandle from, const str& text) {
			if (context.GetTextString(from) == text) {
				return true;
			}
			const UI::Element* element = context.GetElement(from);
			if (!element) {
				return false;
			}
			for (u32 child = element->firstChild; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				if (HasText(context, handle, text)) {
					return true;
				}
				child = context.GetElement(handle)->nextSibling;
			}
			return false;
		}

		static InputEvent TextChar(char32_t codepoint) {
			InputEvent event{};
			event.type = InputEventType::Text;
			event.codepoint = codepoint;
			return event;
		}

		static UI::ElementHandle FindClassed(UI::Context& context, UI::ElementHandle from, StringHash className) {
			if (context.HasClass(from, className)) {
				return from;
			}
			const UI::Element* element = context.GetElement(from);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				UI::ElementHandle found = FindClassed(context, handle, className);
				if (found.IsValid()) {
					return found;
				}
				child = context.GetElement(handle)->nextSibling;
			}
			return {};
		}

		static void CountCells(UI::Context& context, UI::ElementHandle from, u32& cells, u32& withCustomDraw) {
			if (context.HasClass(from, StringHash("ed-asset-cell"))) {
				++cells;
			}
			if (context.Events(from).customDraw && context.HasClass(from, StringHash("ed-asset-cell"))) {
				++withCustomDraw;
			}
			const UI::Element* element = context.GetElement(from);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				CountCells(context, handle, cells, withCustomDraw);
				child = context.GetElement(handle)->nextSibling;
			}
		}

		static void CollectCellRects(UI::Context& context, UI::ElementHandle from, Vector<UI::Rect>& out) {
			if (context.HasClass(from, StringHash("ed-asset-cell"))) {
				out.push_back(context.GetLayout(from).borderBox);
			}
			const UI::Element* element = context.GetElement(from);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				CollectCellRects(context, handle, out);
				child = context.GetElement(handle)->nextSibling;
			}
		}

		static UI::ElementHandle FindScrollRootHoldingCells(UI::Context& context, UI::ElementHandle from) {
			u32 cells = 0;
			u32 ignored = 0;
			if (context.HasClass(from, StringHash("slt-scroll-viewport"))) {
				CountCells(context, from, cells, ignored);
				if (cells > 0) {
					return from;
				}
			}
			const UI::Element* element = context.GetElement(from);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				UI::ElementHandle found = FindScrollRootHoldingCells(context, handle);
				if (found.IsValid()) {
					return found;
				}
				child = context.GetElement(handle)->nextSibling;
			}
			return {};
		}

		static void CountCellsOnScreen(UI::Context& context, UI::ElementHandle from, const UI::Rect& window,
									   u32& built, u32& onScreen) {
			if (context.HasClass(from, StringHash("ed-asset-cell"))) {
				++built;
				UI::Rect clipped = UI::RectIntersect(context.GetLayout(from).borderBox, window);
				if (clipped.w > 0.0f && clipped.h > 0.0f) {
					++onScreen;
				}
			}
			const UI::Element* element = context.GetElement(from);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				CountCellsOnScreen(context, handle, window, built, onScreen);
				child = context.GetElement(handle)->nextSibling;
			}
		}

		static bool IsFileEntry(UI::Context& context, UI::ElementHandle handle) {
			return context.HasClass(handle, StringHash("slt-tree-row")) ||
				   context.HasClass(handle, StringHash("ed-asset-cell"));
		}

		static UI::ElementHandle FindFileRow(UI::Context& context, UI::ElementHandle from, const str& name) {
			if (IsFileEntry(context, from) && HasText(context, from, name)) {
				return from;
			}
			const UI::Element* element = context.GetElement(from);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				UI::ElementHandle found = FindFileRow(context, handle, name);
				if (found.IsValid()) {
					return found;
				}
				child = context.GetElement(handle)->nextSibling;
			}
			return {};
		}

		static UI::ElementHandle FindDropTarget(UI::Context& context, UI::ElementHandle from,
												const UI::DragPayload& payload) {
			if (context.Events(from).onDragOver && context.Events(from).onDragOver(payload)) {
				return from;
			}
			const UI::Element* element = context.GetElement(from);
			if (!element) {
				return {};
			}
			for (u32 child = element->firstChild; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				UI::ElementHandle found = FindDropTarget(context, handle, payload);
				if (context.IsAlive(found)) {
					return found;
				}
				child = context.GetElement(handle)->nextSibling;
			}
			return {};
		}

		static UI::ElementHandle FilePane(UI::Context& context, UI::ElementHandle root) {
			UI::ElementHandle band = UITest::FindByClass(context, root, StringHash("slt-splitter"), 0);
			const UI::Element* element = context.IsAlive(band) ? context.GetElement(band) : nullptr;
			return element && element->nextSibling != UI::INVALID_UI_INDEX ? context.MakeHandle(element->nextSibling)
																		   : UI::ElementHandle{};
		}
		static UI::ElementHandle FilePaneEntry(UI::Context& context, UI::ElementHandle root, const str& name) {
			UI::ElementHandle pane = FilePane(context, root);
			return context.IsAlive(pane) ? FindFileRow(context, pane, name) : UI::ElementHandle{};
		}
		static UI::ElementHandle TreePane(UI::Context& context, UI::ElementHandle root) {
			UI::ElementHandle band = UITest::FindByClass(context, root, StringHash("slt-splitter"), 0);
			const UI::Element* element = context.IsAlive(band) ? context.GetElement(band) : nullptr;
			const UI::Element* split = element && element->parent != UI::INVALID_UI_INDEX
										   ? context.GetElement(context.MakeHandle(element->parent))
										   : nullptr;
			return split ? context.MakeHandle(split->firstChild) : UI::ElementHandle{};
		}
		static UI::ElementHandle TreeRow(UI::Context& context, UI::ElementHandle root, const str& name) {
			UI::ElementHandle tree = TreePane(context, root);
			return context.IsAlive(tree) ? FindFileRow(context, tree, name) : UI::ElementHandle{};
		}
		static bool PressFilePaneEntry(UI::Context& context, EditorPanel& panel, UI::ElementHandle root,
									   const str& name) {
			UI::ElementHandle entry = FilePaneEntry(context, root, name);
			if (!context.IsAlive(entry)) {
				return false;
			}
			ClickAt(context, panel, CenterOf(context, entry));
			return true;
		}
		str SelectedFolderOf(EditorPanel* panel) {
			Variant value;
			return GetField(panel, panel->GetType(), StringHash("selectedFolder"), &value) ? value.As<str>() : str();
		}
		static void PressKey(UI::Context& context, EditorPanel& panel, Key key) {
			InputEvent event{};
			event.type = InputEventType::KeyDown;
			event.key = key;
			context.InjectInput(event);
			context.Update();
			panel.OnTick(0.016f);
			context.Update();
		}
	};

	class AssetBrowserGestureTest : public AssetBrowserPanelTest, public ::testing::WithParamInterface<f32> {
	protected:
		void SetUp() override {
			AssetBrowserPanelTest::SetUp();
			browserIconSize = GetParam();
		}
	};

	struct PanelBuild {
		MockFontSystem fonts;
		UI::Context context;
		UI::ElementHandle root;

		explicit PanelBuild(UI::ContextDesc desc) : context(desc, &fonts) {
			root = context.CreateElement();
			context.AppendChild(context.GetRoot(), root);
		}
	};

	void BuildAndSettle(EditorPanel& panel, struct PanelBuild& build);

	UI::ContextDesc BrowserContextDesc() {
		UI::ContextDesc desc;
		desc.sizePx = {800, 600};
		return desc;
	}

	void BuildAndSettle(EditorPanel& panel, PanelBuild& build) {
		panel.BuildContent(build.context, build.root);
		build.context.Update();
		panel.OnTick(0.016f);
		build.context.Update();
	}

} // anonymous namespace

TEST_F(AssetBrowserPanelTest, TreeRowsReserveCaretAndPicture) {
	WriteSource("project://Assets/Textures/Deep/x.txt", "x");
	WriteSource("project://Assets/Empty/keep.txt", "k");
	WriteSource("project://Assets/readme.txt", "r");

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::ElementHandle branch = FindFileRow(build.context, build.root, "Textures");
	UI::ElementHandle leaf = FindFileRow(build.context, build.root, "Empty");
	UI::ElementHandle file = FindFileRow(build.context, build.root, "readme.txt");
	ASSERT_TRUE(branch.IsValid() && leaf.IsValid() && file.IsValid());

	const auto leading = [&build](UI::ElementHandle row) {
		Vector<UI::ElementHandle> out{GetMemoryResource(MemTag::Editor)};
		const UI::Element* pod = build.context.GetElement(row);
		for (u32 child = pod ? pod->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
			UI::ElementHandle handle = build.context.MakeHandle(child);
			if (!build.context.HasClass(handle, StringHash("slt-tree-indent"))) {
				out.push_back(handle);
			}
			child = build.context.GetElement(handle)->nextSibling;
		}
		return out;
	};

	for (UI::ElementHandle row : {branch, leaf, file}) {
		Vector<UI::ElementHandle> slots = leading(row);
		ASSERT_GE(slots.size(), 3u);
		EXPECT_TRUE(build.context.HasClass(slots[0], StringHash("slt-tree-arrow")));
		EXPECT_TRUE(build.context.HasClass(slots[2], StringHash("slt-tree-label")));
		EXPECT_NE(slots[1].index, slots[0].index);
		EXPECT_FALSE(build.context.HasClass(slots[1], StringHash("slt-tree-arrow")));
	}

	EXPECT_EQ(leading(branch).size(), leading(leaf).size());

	const auto boxOf = [&build](UI::ElementHandle element) { return build.context.GetLayout(element).borderBox; };
	EXPECT_FLOAT_EQ(boxOf(leading(branch)[2]).x, boxOf(leading(leaf)[2]).x);
	EXPECT_FLOAT_EQ(boxOf(leading(branch)[1]).x, boxOf(leading(leaf)[1]).x);
}

TEST_F(AssetBrowserPanelTest, PanesMirrorTheProjectTree) {
	WriteSource("project://Assets/Textures/tex.txt", "t");
	WriteSource("project://Assets/readme.txt", "r");
	WriteSource("project://Assets/readme.txt.meta", "{}");

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());

	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	EXPECT_TRUE(HasText(build.context, build.root, "Assets"));
	EXPECT_FALSE(HasText(build.context, build.root, "Project"));
	EXPECT_TRUE(HasText(build.context, build.root, "Textures"));
	EXPECT_TRUE(HasText(build.context, build.root, "readme.txt"));
	EXPECT_FALSE(HasText(build.context, build.root, "tex.txt"));
	EXPECT_FALSE(HasText(build.context, build.root, "readme.txt.meta"));
}

TEST_F(AssetBrowserPanelTest, BandDragResizesPaneRemembered) {
	WriteSource("project://Assets/readme.txt", "r");

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::ElementHandle band = UITest::FindByClass(build.context, build.root, StringHash("slt-splitter"), 0);
	ASSERT_TRUE(band.IsValid());
	const UI::Rect box = build.context.GetLayout(band).borderBox;
	Vec2 grab{box.x + box.w * 0.5f, box.y + box.h * 0.5f};

	InputEvent event{};
	event.button = MouseButton::Left;
	event.mousePosPx = {static_cast<i32>(grab.x), static_cast<i32>(grab.y)};
	event.type = InputEventType::MouseMove;
	build.context.InjectInput(event);
	event.type = InputEventType::MouseDown;
	build.context.InjectInput(event);
	build.context.Update();

	event.type = InputEventType::MouseMove;
	event.mousePosPx = {static_cast<i32>(grab.x) + 60, static_cast<i32>(grab.y)};
	build.context.InjectInput(event);
	build.context.Update();

	EXPECT_NEAR(build.context.GetLayout(band).borderBox.x, box.x + 60.0f, 1.0f);

	PanelBuild second{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), second);
	UI::ElementHandle rebuiltBand = UITest::FindByClass(second.context, second.root, StringHash("slt-splitter"), 0);
	ASSERT_TRUE(rebuiltBand.IsValid());
	EXPECT_NEAR(second.context.GetLayout(rebuiltBand).borderBox.x, box.x + 60.0f, 1.0f);
}

TEST_F(AssetBrowserPanelTest, PanesHideFilesOutsideContentRoot) {
	WriteSource("project://Assets/readme.txt", "r");
	WriteSource("project://Project.ini", "[Project]\nname=X\n");
	WriteSource("project://CMakeLists.txt", "cmake_minimum_required(VERSION 3.25)");
	WriteSource("project://.vscode/settings.json", "{}");
	WriteSource("project://.git/config", "[core]");

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());

	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	EXPECT_TRUE(HasText(build.context, build.root, "readme.txt"));
	EXPECT_FALSE(HasText(build.context, build.root, ".vscode"));
	EXPECT_FALSE(HasText(build.context, build.root, ".git"));
	EXPECT_FALSE(HasText(build.context, build.root, "Project.ini"));
	EXPECT_FALSE(HasText(build.context, build.root, "CMakeLists.txt"));
}

TEST_F(AssetBrowserPanelTest, SelectedFolderDrivesTheFileList) {
	WriteSource("project://Assets/Textures/tex.txt", "t");
	WriteSource("project://Assets/readme.txt", "r");

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());

	Variant folder{str("Assets/Textures")};
	ASSERT_TRUE(SetField(panel.Get(), panel.Get()->GetType(), StringHash("selectedFolder"), &folder));

	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	EXPECT_TRUE(HasText(build.context, build.root, "tex.txt"));
	EXPECT_FALSE(HasText(build.context, build.root, "readme.txt"));
}

TEST_F(AssetBrowserPanelTest, StaleFolderFallsBackToRoot) {
	WriteSource("project://Assets/readme.txt", "r");

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	Variant folder{str("Deleted/Elsewhere")};
	ASSERT_TRUE(SetField(panel.Get(), panel.Get()->GetType(), StringHash("selectedFolder"), &folder));

	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	Variant after;
	ASSERT_TRUE(GetField(panel.Get(), panel.Get()->GetType(), StringHash("selectedFolder"), &after));
	EXPECT_EQ(after.As<str>(), "Assets");
	EXPECT_TRUE(HasText(build.context, build.root, "readme.txt"));
}

TEST_F(AssetBrowserPanelTest, PreContentRootLayoutPathIsRepaired) {
	WriteSource("project://Assets/Textures/tex.txt", "t");

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	Variant folder{str("Textures")};
	ASSERT_TRUE(SetField(panel.Get(), panel.Get()->GetType(), StringHash("selectedFolder"), &folder));

	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	Variant after;
	ASSERT_TRUE(GetField(panel.Get(), panel.Get()->GetType(), StringHash("selectedFolder"), &after));
	EXPECT_EQ(after.As<str>(), "Assets");
}

TEST_P(AssetBrowserGestureTest, InspectorAssetHandleAcceptsTheBrowserPayload) {
	WriteSource("project://Assets/notes.txt", "hi");
	AssetDatabase::ScanProjectDirectory();
	Guid guid = AssetDatabase::LookupGuid(Path("project://Assets/notes.txt"));
	ASSERT_TRUE(guid.IsValid());

	Handle<Scene> scene = SceneSystem::CreateScene("Fixture");
	Handle<SceneObject> object = SceneSystem::CreateObject("Subject", scene);
	Handle<BrowserAssetComponent> component = object.Get()->AddComponent<BrowserAssetComponent>();
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> inspector = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("InspectorPanel")));
	ASSERT_TRUE(inspector.IsValid());
	PanelBuild build{BrowserContextDesc()};
	inspector.Get()->BuildContent(build.context, build.root);
	build.context.Update();

	UI::DragPayload payload;
	payload.type = "Asset"_hash;
	payload.data = Variant(AssetDragData{guid, StringHash("TextAsset").GetValue()});

	UI::ElementHandle row = FindDropTarget(build.context, build.root, payload);
	ASSERT_TRUE(build.context.IsAlive(row));

	UI::DragPayload mismatched = payload;
	mismatched.data = Variant(AssetDragData{guid, StringHash("TextureAsset").GetValue()});
	EXPECT_FALSE(build.context.Events(row).onDragOver(mismatched));

	build.context.Events(row).onDrop(payload, {});
	ASSERT_TRUE(component.Get()->textRef.IsValid());
	EXPECT_EQ(component.Get()->textRef.Get()->assetId, guid);

	UndoSystem::Undo();
	EXPECT_FALSE(component.Get()->textRef.IsValid());
	UndoSystem::Redo();
	EXPECT_TRUE(component.Get()->textRef.IsValid());
}

TEST_F(AssetBrowserPanelTest, UnmountedProjectShowsTheMountHint) {
	FileSystem::Unmount("project");

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());

	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	EXPECT_FALSE(HasText(build.context, build.root, "Project"));

	FileSystem::Mount("project", Path(_root.generic_string()));
	WriteSource("project://Assets/late.txt", "l");
	panel.Get()->OnTick(2.0f);

	UI::ElementHandle second = build.context.CreateElement();
	build.context.AppendChild(build.context.GetRoot(), second);
	panel.Get()->BuildContent(build.context, second);
	build.context.Update();
	EXPECT_TRUE(HasText(build.context, second, "late.txt"));
}

TEST_F(AssetBrowserFixture, MakeUniqueNameSuffixesBeforeTheExtension) {
	EXPECT_EQ(AssetBrowserOps::MakeUniqueName(Path("project://Assets"), "New Material.mat"), "New Material.mat");

	WriteSource("project://Assets/New Material.mat", "{}");
	EXPECT_EQ(AssetBrowserOps::MakeUniqueName(Path("project://Assets"), "New Material.mat"), "New Material 1.mat");

	WriteSource("project://Assets/New Material 1.mat", "{}");
	EXPECT_EQ(AssetBrowserOps::MakeUniqueName(Path("project://Assets"), "New Material.mat"), "New Material 2.mat");

	WriteSource("project://Assets/Notes", "x");
	EXPECT_EQ(AssetBrowserOps::MakeUniqueName(Path("project://Assets"), "Notes"), "Notes 1");
}

TEST_F(AssetBrowserFixture, CreateAssetUniquifiesInFolder) {
	ASSERT_TRUE(AssetBrowserOps::CreateFolder(Path("project://Assets"), "Materials"));

	str relPath;
	Guid first = AssetBrowserOps::CreateAsset(Path("project://Assets/Materials"), StringHash("MaterialAsset"), {}, &relPath);
	ASSERT_TRUE(first.IsValid());
	EXPECT_EQ(relPath, "Assets/Materials/New Material.mat");
	EXPECT_TRUE(FileSystem::Exists(Path("project://Assets/Materials/New Material.mat")));

	Guid second = AssetBrowserOps::CreateAsset(Path("project://Assets/Materials"), StringHash("MaterialAsset"), {}, &relPath);
	ASSERT_TRUE(second.IsValid());
	EXPECT_NE(second, first);
	EXPECT_EQ(relPath, "Assets/Materials/New Material 1.mat");
}

TEST_F(AssetBrowserFixture, DefaultCreateFolderIsContentRoot) {
	EXPECT_EQ(AssetBrowserOps::GetDefaultCreateFolder().ToString(), Path("project://Assets").ToString());

	str relPath;
	Guid guid = AssetBrowserOps::CreateAsset(AssetBrowserOps::GetDefaultCreateFolder(),
											 StringHash("MaterialAsset"), {}, &relPath);
	ASSERT_TRUE(guid.IsValid());
	EXPECT_EQ(relPath, "Assets/New Material.mat");
}

TEST_F(AssetBrowserFixture, NoContentRootNoCreateFolder) {
	std::error_code ec;
	std::filesystem::remove_all(_root / "Assets", ec);

	EXPECT_TRUE(AssetBrowserOps::GetDefaultCreateFolder().ToString().empty());
	str relPath;
	EXPECT_FALSE(AssetBrowserOps::CreateAsset(AssetBrowserOps::GetDefaultCreateFolder(),
											  StringHash("MaterialAsset"), {}, &relPath)
					 .IsValid());
}

TEST_F(AssetBrowserFixture, CreateAtRootHandlesSchemeOnlyPath) {
	str relPath;
	Guid guid = AssetBrowserOps::CreateAsset(Path("project://Assets"), StringHash("MaterialAsset"), {}, &relPath);
	ASSERT_TRUE(guid.IsValid());
	EXPECT_EQ(relPath, "Assets/New Material.mat");
	EXPECT_EQ(AssetDatabase::LookupGuid(Path("project://Assets/New Material.mat")), guid);
}

TEST_F(AssetBrowserFixture, MoveKeepsGuidRefusesDangerous) {
	ASSERT_TRUE(AssetBrowserOps::CreateFolder(Path("project://Assets"), "Src"));
	ASSERT_TRUE(AssetBrowserOps::CreateFolder(Path("project://Assets"), "Dst"));
	ASSERT_TRUE(AssetBrowserOps::CreateFolder(Path("project://Assets/Src"), "Inner"));
	WriteSource("project://Assets/Src/note.txt", "hello");

	Asset* asset = AssetDatabase::LoadAsset(Path("project://Assets/Src/note.txt"));
	ASSERT_NE(asset, nullptr);
	Guid guid = asset->assetId;
	AssetDatabase::UnloadAsset(asset);

	ASSERT_TRUE(AssetBrowserOps::MoveEntry(Path("project://Assets/Src/note.txt"), Path("project://Assets/Dst")));
	EXPECT_TRUE(FileSystem::Exists(Path("project://Assets/Dst/note.txt")));
	EXPECT_FALSE(FileSystem::Exists(Path("project://Assets/Src/note.txt")));
	EXPECT_TRUE(FileSystem::Exists(Path("project://Assets/Dst/note.txt.meta")));
	EXPECT_EQ(AssetDatabase::LookupGuid(Path("project://Assets/Dst/note.txt")), guid);
	EXPECT_FALSE(AssetDatabase::LookupGuid(Path("project://Assets/Src/note.txt")).IsValid());

	EXPECT_FALSE(AssetBrowserOps::MoveEntry(Path("project://Assets/Src"), Path("project://Assets/Src/Inner")));
	EXPECT_TRUE(FileSystem::Exists(Path("project://Assets/Src/Inner")));

	WriteSource("project://Assets/Src/note.txt", "second");
	EXPECT_FALSE(AssetBrowserOps::MoveEntry(Path("project://Assets/Src/note.txt"), Path("project://Assets/Dst")));
	EXPECT_TRUE(FileSystem::Exists(Path("project://Assets/Src/note.txt")));
}

TEST_F(AssetBrowserFixture, MoveEntryIntoParentIsNoOp) {
	ASSERT_TRUE(AssetBrowserOps::CreateFolder(Path("project://Assets"), "Here"));
	WriteSource("project://Assets/Here/a.txt", "a");
	EXPECT_FALSE(AssetBrowserOps::MoveEntry(Path("project://Assets/Here/a.txt"), Path("project://Assets/Here")));
	EXPECT_TRUE(FileSystem::Exists(Path("project://Assets/Here/a.txt")));
}

TEST_F(AssetBrowserFixture, DuplicateEntryMintsAFreshGuid) {
	WriteSource("project://Assets/note.txt", "original");
	Asset* asset = AssetDatabase::LoadAsset(Path("project://Assets/note.txt"));
	ASSERT_NE(asset, nullptr);
	Guid original = asset->assetId;
	AssetDatabase::UnloadAsset(asset);

	str created = AssetBrowserOps::DuplicateEntry(Path("project://Assets/note.txt"));
	ASSERT_EQ(created, "Assets/note 1.txt");
	EXPECT_TRUE(FileSystem::Exists(Path("project://Assets/note 1.txt")));

	Guid copy = AssetDatabase::LookupGuid(Path("project://Assets/note 1.txt"));
	EXPECT_TRUE(copy.IsValid());
	EXPECT_NE(copy, original);
	EXPECT_EQ(AssetDatabase::LookupGuid(Path("project://Assets/note.txt")), original);
}

TEST_F(AssetBrowserFixture, DuplicateDirectoryDropsTheCopiedSidecars) {
	ASSERT_TRUE(AssetBrowserOps::CreateFolder(Path("project://Assets"), "Pack"));
	WriteSource("project://Assets/Pack/a.txt", "a");
	Asset* asset = AssetDatabase::LoadAsset(Path("project://Assets/Pack/a.txt"));
	ASSERT_NE(asset, nullptr);
	Guid original = asset->assetId;
	AssetDatabase::UnloadAsset(asset);

	str created = AssetBrowserOps::DuplicateEntry(Path("project://Assets/Pack"));
	ASSERT_EQ(created, "Assets/Pack 1");
	EXPECT_TRUE(FileSystem::Exists(Path("project://Assets/Pack 1/a.txt")));
	Guid copy = AssetDatabase::LookupGuid(Path("project://Assets/Pack 1/a.txt"));
	EXPECT_TRUE(copy.IsValid());
	EXPECT_NE(copy, original);
}

TEST_F(AssetBrowserFixture, ImportCopiesExternalFile) {
	std::filesystem::path outside = _root.parent_path() /
									("shalltear_external_" + _root.filename().string() + ".txt");
	FileSystem::WriteTextFile(Path(outside.generic_string()), "external");

	str created = AssetBrowserOps::ImportExternalFile(Path(outside.generic_string()), Path("project://Assets"));
	ASSERT_FALSE(created.empty());
	EXPECT_TRUE(FileSystem::Exists(Path("project://" + created)));
	EXPECT_TRUE(std::filesystem::exists(outside));
	EXPECT_TRUE(AssetDatabase::LookupGuid(Path("project://" + created)).IsValid());

	std::error_code ec;
	std::filesystem::remove(outside, ec);
}

TEST_F(AssetBrowserFixture, FilterIsCaseInsensitiveAndSubstring) {
	EXPECT_TRUE(AssetBrowserOps::MatchesFilter("BrickWall.png", ""));
	EXPECT_TRUE(AssetBrowserOps::MatchesFilter("BrickWall.png", "wall"));
	EXPECT_TRUE(AssetBrowserOps::MatchesFilter("BrickWall.png", "BRICK"));
	EXPECT_TRUE(AssetBrowserOps::MatchesFilter("BrickWall.png", ".png"));
	EXPECT_FALSE(AssetBrowserOps::MatchesFilter("BrickWall.png", "stone"));
	EXPECT_FALSE(AssetBrowserOps::MatchesFilter("a", "abc"));
}

TEST_P(AssetBrowserGestureTest, PressedRowStaysDraggable) {
	WriteSource("project://Assets/one.txt", "1");
	WriteSource("project://Assets/two.txt", "2");

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::ElementHandle row = FindFileRow(build.context, build.root, "one.txt");
	ASSERT_TRUE(row.IsValid());
	const UI::Rect& box = build.context.GetLayout(row).borderBox;
	Vec2 point{box.x + box.w * 0.5f, box.y + box.h * 0.5f};
	ASSERT_EQ(build.context.HitTest(point), row);

	int drags = 0;
	std::function<void(Vec2)> launch = build.context.Events(row).onPressDrag;
	ASSERT_TRUE(static_cast<bool>(launch));
	build.context.Events(row).onPressDrag = [&drags, launch](Vec2 at) {
		++drags;
		launch(at);
	};

	InputEvent down{};
	down.type = InputEventType::MouseDown;
	down.button = MouseButton::Left;
	down.mousePosPx = {static_cast<i32>(point.x), static_cast<i32>(point.y)};
	build.context.InjectInput(down);
	build.context.Update();

	panel.Get()->OnTick(0.016f);
	build.context.Update();
	ASSERT_TRUE(build.context.IsAlive(row));
	EXPECT_FALSE(build.context.HasClass(row, StringHash("slt-selected")));

	InputEvent move{};
	move.type = InputEventType::MouseMove;
	move.mousePosPx = {static_cast<i32>(point.x) + 12, static_cast<i32>(point.y) + 3};
	build.context.InjectInput(move);
	build.context.Update();
	EXPECT_EQ(drags, 1);

	InputEvent back{};
	back.type = InputEventType::MouseMove;
	back.mousePosPx = {static_cast<i32>(point.x), static_cast<i32>(point.y)};
	build.context.InjectInput(back);
	InputEvent up{};
	up.type = InputEventType::MouseUp;
	up.button = MouseButton::Left;
	up.mousePosPx = {static_cast<i32>(point.x), static_cast<i32>(point.y)};
	build.context.InjectInput(up);
	build.context.Update();
	panel.Get()->OnTick(0.016f);
	build.context.Update();
	ASSERT_TRUE(build.context.IsAlive(row));
	EXPECT_TRUE(build.context.HasClass(row, StringHash("slt-selected")));
}

TEST_P(AssetBrowserGestureTest, DoubleClickingAPrefabStagesIt) {
	Handle<Scene> world = SceneSystem::CreateScene("World");
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", world);
	Guid guid = SceneSystem::SavePrefab(unit, Path("project://Assets/unit.prefab"));
	ASSERT_TRUE(guid.IsValid());

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::ElementHandle row = FindFileRow(build.context, build.root, "unit.prefab");
	ASSERT_TRUE(build.context.IsAlive(row));
	DoubleClick(build.context, *panel.Get(), CenterOf(build.context, row));

	ASSERT_EQ(SceneSystem::GetLoadedScenes().size(), 2u);
	Handle<Scene> opened = EditorSceneOps::GetPrefabStage();
	ASSERT_TRUE(opened.IsValid());
	EXPECT_EQ(EditorSceneOps::GetPrefabTarget(opened), guid);
	EXPECT_EQ(opened->GetRootCount(), 1u);
	EXPECT_TRUE(world->FindObjectByName("Unit").IsValid());
	EXPECT_EQ(SceneSystem::GetPrimaryScene().GetInstanceId(), opened.GetInstanceId());
	Vector<Handle<Scene>> shown{GetMemoryResource(MemTag::Editor)};
	EditorSceneOps::GetStageScenes(shown);
	ASSERT_EQ(shown.size(), 1u);
	EXPECT_EQ(shown[0].GetInstanceId(), opened.GetInstanceId());

	BuildAndSettle(*panel.Get(), build);
	UI::ElementHandle again = FindFileRow(build.context, build.root, "unit.prefab");
	ASSERT_TRUE(build.context.IsAlive(again));
	DoubleClick(build.context, *panel.Get(), CenterOf(build.context, again));
	EXPECT_EQ(SceneSystem::GetLoadedScenes().size(), 2u);
	EXPECT_EQ(EditorSceneOps::GetPrefabStage().GetInstanceId(), opened.GetInstanceId());

	EXPECT_TRUE(EditorSceneOps::LeavePrefabStage());
	SceneSystem::EndFrame();
	EXPECT_FALSE(EditorSceneOps::GetPrefabStage().IsValid());
	ASSERT_EQ(SceneSystem::GetLoadedScenes().size(), 1u);
	EXPECT_EQ(SceneSystem::GetPrimaryScene().GetInstanceId(), world.GetInstanceId());
}

TEST_P(AssetBrowserGestureTest, DoubleClickingSceneStillReplacesWorld) {
	Handle<Scene> source = SceneSystem::CreateScene("Source");
	SceneSystem::CreateObject("Arrival", source);
	ASSERT_TRUE(SceneSystem::SaveScene(source, Path("project://Assets/second.scene")));
	SceneSystem::UnloadScene(source);
	SceneSystem::EndFrame();
	Handle<Scene> world = SceneSystem::CreateScene("World");
	SceneSystem::SetPrimaryScene(world);
	SceneSystem::CreateObject("Doomed", world);

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::ElementHandle row = FindFileRow(build.context, build.root, "second.scene");
	ASSERT_TRUE(build.context.IsAlive(row));
	DoubleClick(build.context, *panel.Get(), CenterOf(build.context, row));

	ASSERT_EQ(SceneSystem::GetLoadedScenes().size(), 1u);
	Handle<Scene> loaded = SceneSystem::GetLoadedScenes()[0];
	EXPECT_TRUE(loaded->FindObjectByName("Arrival").IsValid());
	EXPECT_FALSE(EditorSceneOps::GetPrefabTarget(loaded).IsValid());
	EXPECT_FALSE(world.IsValid());
}

TEST_P(AssetBrowserGestureTest, DoubleClickOpensInRegisteredOpener) {
	WriteSource("project://Assets/notes.txt", "a text asset, which nothing opened before");
	const Guid guid = AssetDatabase::EnsureAssetIdentity(Path("project://Assets/notes.txt"));
	ASSERT_TRUE(guid.IsValid());

	static const int image = 0;
	static const int otherImage = 0;
	struct Release {
		~Release() { AssetOpeners::Terminate(); }
	} release;
	Guid opened{};
	u32 opens = 0;
	ASSERT_TRUE(AssetOpeners::Register(".TXT", [&](const Guid& asset) {
		opened = asset;
		++opens; }, &image));
	EXPECT_FALSE(AssetOpeners::Register(".txt", [&](const Guid&) { opens += 100; }, &otherImage));

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	UI::ElementHandle row = FindFileRow(build.context, build.root, "notes.txt");
	ASSERT_TRUE(build.context.IsAlive(row));
	DoubleClick(build.context, *panel.Get(), CenterOf(build.context, row));
	EXPECT_EQ(opens, 1u);
	EXPECT_EQ(opened, guid);

	EXPECT_GE(EditorExtensions::Unload(&image), 1u);
	BuildAndSettle(*panel.Get(), build);
	UI::ElementHandle again = FindFileRow(build.context, build.root, "notes.txt");
	ASSERT_TRUE(build.context.IsAlive(again));
	DoubleClick(build.context, *panel.Get(), CenterOf(build.context, again));
	EXPECT_EQ(opens, 1u);
}

TEST_P(AssetBrowserGestureTest, ObjectDropOnFilesMakesPrefab) {
	Handle<Scene> scene = SceneSystem::CreateScene("Level");
	Handle<SceneObject> unit = SceneSystem::CreateObject("Turret", scene);
	SceneSystem::CreateObject("Barrel", scene, unit);

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::DragPayload payload;
	payload.type = "SceneObject"_hash;
	payload.data = Variant(DraggedObjects({unit}));
	payload.display = "Turret";

	UI::ElementHandle pane = FindDropTarget(build.context, build.root, payload);
	ASSERT_TRUE(build.context.IsAlive(pane));
	build.context.Events(pane).onDrop(payload, {});

	EXPECT_TRUE(std::filesystem::exists(_root / "Assets" / "Turret.prefab"));
	EXPECT_TRUE(SceneSystem::IsPrefabLinked(unit));
	EXPECT_TRUE(EditorSelection::HasAssetSelection());
	EXPECT_EQ(EditorSelection::GetSelectedAsset(), AssetDatabase::LookupGuid(Path("project://Assets/Turret.prefab")));

	BuildAndSettle(*panel.Get(), build);
	UI::ElementHandle row = FindFileRow(build.context, build.root, "Turret.prefab");
	ASSERT_TRUE(build.context.IsAlive(row));
	EXPECT_TRUE(build.context.HasClass(row, StringHash("slt-selected")));
}

TEST_F(AssetBrowserPanelTest, FilePaneRefusesInstanceInterior) {
	Handle<Scene> scene = SceneSystem::CreateScene("Level");
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", scene);
	Handle<SceneObject> body = SceneSystem::CreateObject("Body", scene, unit);
	ASSERT_TRUE(AssetBrowserOps::CreatePrefabFromObject(unit, Path("project://Assets")).IsValid());
	ASSERT_TRUE(SceneSystem::IsPrefabLinked(body));

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::DragPayload payload;
	payload.type = "SceneObject"_hash;
	payload.data = Variant(DraggedObjects({body}));
	payload.display = "Body";
	EXPECT_FALSE(build.context.IsAlive(FindDropTarget(build.context, build.root, payload)));

	payload.data = Variant(DraggedObjects({unit}));
	payload.display = "Unit";
	EXPECT_TRUE(build.context.IsAlive(FindDropTarget(build.context, build.root, payload)));
}

TEST_F(AssetBrowserPanelTest, DroppingSeveralObjectsMakesPrefabOfEach) {
	Handle<Scene> scene = SceneSystem::CreateScene("Level");
	Handle<SceneObject> left = SceneSystem::CreateObject("Left", scene);
	Handle<SceneObject> right = SceneSystem::CreateObject("Right", scene);

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::DragPayload payload;
	payload.type = "SceneObject"_hash;
	payload.data = Variant(DraggedObjects({left, right}));
	UI::ElementHandle pane = FindDropTarget(build.context, build.root, payload);
	ASSERT_TRUE(build.context.IsAlive(pane));
	build.context.Events(pane).onDrop(payload, {});
	build.context.Update();

	EXPECT_EQ(build.context.GetOpenPopupCount(), 0u);
	EXPECT_TRUE(std::filesystem::exists(_root / "Assets" / "Left.prefab"));
	EXPECT_TRUE(std::filesystem::exists(_root / "Assets" / "Right.prefab"));
	EXPECT_TRUE(SceneSystem::IsPrefabLinked(left));
	EXPECT_TRUE(SceneSystem::IsPrefabLinked(right));
}

TEST_F(AssetBrowserPanelTest, InstanceRunAsksOnceMakesEach) {
	Handle<Scene> world = SceneSystem::CreateScene("World");
	Handle<SceneObject> unit = SceneSystem::CreateObject("Turret", world);
	Handle<SceneObject> plain = SceneSystem::CreateObject("Crate", world);
	ASSERT_TRUE(EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Prefabs/Turret.prefab")).IsValid());

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::DragPayload payload;
	payload.type = "SceneObject"_hash;
	payload.data = Variant(DraggedObjects({unit, plain}));
	UI::ElementHandle pane = FindDropTarget(build.context, build.root, payload);
	ASSERT_TRUE(build.context.IsAlive(pane));
	build.context.Events(pane).onDrop(payload, {});
	build.context.Update();
	ASSERT_EQ(build.context.GetOpenPopupCount(), 1u);
	EXPECT_FALSE(std::filesystem::exists(_root / "Assets" / "Crate.prefab"));

	UI::ElementHandle create = ButtonNamed(build.context, build.context.GetOpenPopup(0), "Create");
	ASSERT_TRUE(build.context.IsAlive(create));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, create));
	EXPECT_TRUE(std::filesystem::exists(_root / "Assets" / "Crate.prefab"));
	EXPECT_TRUE(std::filesystem::exists(_root / "Assets" / "Turret.prefab"));
}

TEST_F(AssetBrowserPanelTest, RunHoldingInstancesInteriorRowIsRefused) {
	Handle<Scene> scene = SceneSystem::CreateScene("Level");
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", scene);
	Handle<SceneObject> body = SceneSystem::CreateObject("Body", scene, unit);
	Handle<SceneObject> crate = SceneSystem::CreateObject("Crate", scene);
	ASSERT_TRUE(AssetBrowserOps::CreatePrefabFromObject(unit, Path("project://Assets")).IsValid());

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::DragPayload payload;
	payload.type = "SceneObject"_hash;
	payload.data = Variant(DraggedObjects({crate, body}));
	EXPECT_FALSE(build.context.IsAlive(FindDropTarget(build.context, build.root, payload)));
	payload.data = Variant(DraggedObjects({crate}));
	EXPECT_TRUE(build.context.IsAlive(FindDropTarget(build.context, build.root, payload)));
}

TEST_P(AssetBrowserGestureTest, RowMenuVariantLandsBesideBase) {
	Handle<Scene> world = SceneSystem::CreateScene("World");
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", world);
	Guid base = SceneSystem::SavePrefab(unit, Path("project://Assets/Unit.prefab"));
	ASSERT_TRUE(base.IsValid());

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::ElementHandle row = FindFileRow(build.context, build.root, "Unit.prefab");
	ASSERT_TRUE(build.context.IsAlive(row));
	RightClickAt(build.context, *panel.Get(), CenterOf(build.context, row));
	ASSERT_EQ(build.context.GetOpenPopupCount(), 1u);
	UI::ElementHandle item = MenuRowNamed(build.context, build.context.GetOpenPopup(0), "Create Prefab Variant");
	ASSERT_TRUE(build.context.IsAlive(item));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, item));

	EXPECT_TRUE(std::filesystem::exists(_root / "Assets" / "Unit Variant.prefab"));
	Guid variant = AssetDatabase::LookupGuid(Path("project://Assets/Unit Variant.prefab"));
	ASSERT_TRUE(variant.IsValid());
	EXPECT_NE(variant, base);
	PrefabAsset* asset = static_cast<PrefabAsset*>(AssetDatabase::LoadAssetByGuid(variant));
	ASSERT_NE(asset, nullptr);
	EXPECT_EQ(asset->GetBasePrefab(), base);
	EXPECT_TRUE(asset->GetOverrides().IsEmpty());
	AssetDatabase::UnloadAsset(asset);

	EXPECT_EQ(EditorSelection::GetSelectedAsset(), variant);
	BuildAndSettle(*panel.Get(), build);
	UI::ElementHandle created = FindFileRow(build.context, build.root, "Unit Variant.prefab");
	ASSERT_TRUE(build.context.IsAlive(created));
	EXPECT_TRUE(RowIsSelected(build.context, created));
}

TEST_P(AssetBrowserGestureTest, VariantItemInertOnNonPrefab) {
	WriteSource("project://Assets/notes.txt", "n");
	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	RightClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "notes.txt")));
	ASSERT_EQ(build.context.GetOpenPopupCount(), 1u);
	UI::ElementHandle item = MenuRowNamed(build.context, build.context.GetOpenPopup(0), "Create Prefab Variant");
	ASSERT_TRUE(build.context.IsAlive(item));
	EXPECT_FALSE(static_cast<bool>(build.context.Events(item).onClick));
}

TEST_P(AssetBrowserGestureTest, InstanceRootDropOffersVariant) {
	Handle<Scene> world = SceneSystem::CreateScene("World");
	Handle<SceneObject> unit = SceneSystem::CreateObject("Turret", world);
	Guid base = EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Prefabs/Turret.prefab"));
	ASSERT_TRUE(base.IsValid());
	unit->SetLocalPosition({1.0f, 2.0f, 3.0f});

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::DragPayload payload;
	payload.type = "SceneObject"_hash;
	payload.data = Variant(DraggedObjects({unit}));
	payload.display = "Turret";
	UI::ElementHandle pane = FindDropTarget(build.context, build.root, payload);
	ASSERT_TRUE(build.context.IsAlive(pane));
	build.context.Events(pane).onDrop(payload, {});
	build.context.Update();

	EXPECT_FALSE(std::filesystem::exists(_root / "Assets" / "Turret.prefab"));
	ASSERT_EQ(build.context.GetOpenPopupCount(), 1u);
	UI::ElementHandle choose = ButtonNamed(build.context, build.context.GetOpenPopup(0), "Prefab Variant");
	ASSERT_TRUE(build.context.IsAlive(choose));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, choose));

	EXPECT_TRUE(std::filesystem::exists(_root / "Assets" / "Turret.prefab"));
	Guid variant = AssetDatabase::LookupGuid(Path("project://Assets/Turret.prefab"));
	ASSERT_TRUE(variant.IsValid());
	EXPECT_NE(variant, base);
	PrefabAsset* asset = static_cast<PrefabAsset*>(AssetDatabase::LoadAssetByGuid(variant));
	ASSERT_NE(asset, nullptr);
	EXPECT_EQ(asset->GetBasePrefab(), base);
	EXPECT_FALSE(asset->GetOverrides().IsEmpty());
	AssetDatabase::UnloadAsset(asset);
	EXPECT_TRUE(SceneSystem::IsPrefabInstanceRoot(unit));
	EXPECT_EQ(EditorSelection::GetSelectedAsset(), variant);
	BuildAndSettle(*panel.Get(), build);
	UI::ElementHandle created = FindFileRow(build.context, build.root, "Turret.prefab");
	ASSERT_TRUE(build.context.IsAlive(created));
	EXPECT_TRUE(RowIsSelected(build.context, created));
}

TEST_F(AssetBrowserPanelTest, CancellingTheInstanceDropWritesNothing) {
	Handle<Scene> world = SceneSystem::CreateScene("World");
	Handle<SceneObject> unit = SceneSystem::CreateObject("Turret", world);
	Guid base = EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Prefabs/Turret.prefab"));
	ASSERT_TRUE(base.IsValid());

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::DragPayload payload;
	payload.type = "SceneObject"_hash;
	payload.data = Variant(DraggedObjects({unit}));
	payload.display = "Turret";
	UI::ElementHandle pane = FindDropTarget(build.context, build.root, payload);
	ASSERT_TRUE(build.context.IsAlive(pane));
	build.context.Events(pane).onDrop(payload, {});
	build.context.Update();
	ASSERT_EQ(build.context.GetOpenPopupCount(), 1u);
	UI::ElementHandle cancel = ButtonNamed(build.context, build.context.GetOpenPopup(0), "Cancel");
	ASSERT_TRUE(build.context.IsAlive(cancel));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, cancel));

	EXPECT_EQ(build.context.GetOpenPopupCount(), 0u);
	EXPECT_FALSE(std::filesystem::exists(_root / "Assets" / "Turret.prefab"));
	EXPECT_TRUE(SceneSystem::IsPrefabInstanceRoot(unit));
	EXPECT_EQ(AssetDatabase::LookupGuid(Path("project://Assets/Prefabs/Turret.prefab")), base);
}

TEST_F(AssetBrowserPanelTest, ProducedRowTakesKeyboard) {
	Handle<Scene> world = SceneSystem::CreateScene("World");
	Handle<SceneObject> unit = SceneSystem::CreateObject("Turret", world);

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	build.context.ClearFocus();
	ASSERT_FALSE(panel.Get()->OnCommand(PanelCommand::Delete));

	UI::DragPayload payload;
	payload.type = "SceneObject"_hash;
	payload.data = Variant(DraggedObjects({unit}));
	payload.display = "Turret";
	UI::ElementHandle pane = FindDropTarget(build.context, build.root, payload);
	ASSERT_TRUE(build.context.IsAlive(pane));
	build.context.Events(pane).onDrop(payload, {});
	build.context.Update();
	panel.Get()->OnTick(0.016f);
	build.context.Update();
	ASSERT_TRUE(std::filesystem::exists(_root / "Assets" / "Turret.prefab"));

	EXPECT_TRUE(panel.Get()->OnCommand(PanelCommand::Delete));
	EXPECT_GT(build.context.GetOpenPopupCount(), 0u);
}

TEST(AssetBrowserOpenPolicyTest, OnlyShaderSourceOpensExternally) {
	EXPECT_TRUE(AssetBrowserOps::OpensInExternalEditor(".hlsl"));
	EXPECT_TRUE(AssetBrowserOps::OpensInExternalEditor(".hlsli"));
	EXPECT_TRUE(AssetBrowserOps::OpensInExternalEditor(".HLSL"));
	EXPECT_TRUE(AssetBrowserOps::OpensInExternalEditor(".cs"));
	EXPECT_TRUE(AssetBrowserOps::OpensInExternalEditor(".CS"));

	EXPECT_FALSE(AssetBrowserOps::OpensInExternalEditor(".scene"));
	EXPECT_FALSE(AssetBrowserOps::OpensInExternalEditor(".prefab"));
	EXPECT_FALSE(AssetBrowserOps::OpensInExternalEditor(".mat"));
	EXPECT_FALSE(AssetBrowserOps::OpensInExternalEditor(".png"));
	EXPECT_FALSE(AssetBrowserOps::OpensInExternalEditor(""));
}

TEST_P(AssetBrowserGestureTest, ShaderRowHasOpenActionMaterialNot) {
	FileSystem::WriteTextFile(Path("project://Assets/Custom.hlsl"), "// a shader\n");
	FileSystem::WriteTextFile(Path("project://Assets/Some.mat"), "{}\n");
	AssetDatabase::ScanProjectDirectory();

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::ElementHandle shaderRow = FindFileRow(build.context, build.root, "Custom.hlsl");
	ASSERT_TRUE(build.context.IsAlive(shaderRow));
	EXPECT_TRUE(static_cast<bool>(build.context.Events(shaderRow).onDoubleClick));

	UI::ElementHandle materialRow = FindFileRow(build.context, build.root, "Some.mat");
	ASSERT_TRUE(build.context.IsAlive(materialRow));
	EXPECT_FALSE(static_cast<bool>(build.context.Events(materialRow).onDoubleClick));
}

namespace {

	void WriteFourRows() {
		for (const char* name : {"a.txt", "b.txt", "c.txt", "d.txt"}) {
			FileSystem::WriteTextFile(Path(str("project://Assets/") + name), "x");
		}
		AssetDatabase::ScanProjectDirectory();
	}

	Guid GuidOfRow(const char* name) {
		return AssetDatabase::LookupGuid(Path(str("project://Assets/") + name));
	}

} // namespace

TEST_P(AssetBrowserGestureTest, CtrlClickAddsToSelection) {
	WriteFourRows();
	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::ElementHandle rowA = FindFileRow(build.context, build.root, "a.txt");
	UI::ElementHandle rowC = FindFileRow(build.context, build.root, "c.txt");
	ASSERT_TRUE(build.context.IsAlive(rowA) && build.context.IsAlive(rowC));

	ClickAt(build.context, *panel.Get(), CenterOf(build.context, rowA));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, rowC), KeyMod::Ctrl);

	EXPECT_EQ(EditorSelection::GetAssetSelectionCount(), 2u);
	EXPECT_EQ(EditorSelection::GetSelectedAsset(), GuidOfRow("c.txt"));
	EXPECT_TRUE(RowIsSelected(build.context, rowA));
	EXPECT_TRUE(RowIsSelected(build.context, rowC));
	EXPECT_FALSE(RowIsSelected(build.context, FindFileRow(build.context, build.root, "b.txt")));
}

TEST_P(AssetBrowserGestureTest, CtrlClickRemovesSelectedRow) {
	WriteFourRows();
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::ElementHandle rowA = FindFileRow(build.context, build.root, "a.txt");
	UI::ElementHandle rowB = FindFileRow(build.context, build.root, "b.txt");
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, rowA));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, rowB), KeyMod::Ctrl);
	ASSERT_EQ(EditorSelection::GetAssetSelectionCount(), 2u);

	ClickAt(build.context, *panel.Get(), CenterOf(build.context, rowA), KeyMod::Ctrl);
	EXPECT_EQ(EditorSelection::GetAssetSelectionCount(), 1u);
	EXPECT_FALSE(RowIsSelected(build.context, rowA));
	EXPECT_TRUE(RowIsSelected(build.context, rowB));
}

TEST_P(AssetBrowserGestureTest, ShiftClickSpansFromAnchor) {
	WriteFourRows();
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	ClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "a.txt")));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "c.txt")), KeyMod::Shift);

	EXPECT_EQ(EditorSelection::GetAssetSelectionCount(), 3u);
	EXPECT_TRUE(RowIsSelected(build.context, FindFileRow(build.context, build.root, "b.txt")));
	EXPECT_FALSE(RowIsSelected(build.context, FindFileRow(build.context, build.root, "d.txt")));
	EXPECT_EQ(EditorSelection::GetSelectedAsset(), GuidOfRow("c.txt"));
}

TEST_P(AssetBrowserGestureTest, ShiftClickSpansSameRunDraggedUpwards) {
	WriteFourRows();
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	ClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "c.txt")));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "a.txt")), KeyMod::Shift);

	EXPECT_EQ(EditorSelection::GetAssetSelectionCount(), 3u);
	EXPECT_TRUE(RowIsSelected(build.context, FindFileRow(build.context, build.root, "b.txt")));
	EXPECT_EQ(EditorSelection::GetSelectedAsset(), GuidOfRow("a.txt"));
}

TEST_P(AssetBrowserGestureTest, SecondShiftClickRespansFromAnchor) {
	WriteFourRows();
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	ClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "a.txt")));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "d.txt")), KeyMod::Shift);
	ASSERT_EQ(EditorSelection::GetAssetSelectionCount(), 4u);

	ClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "b.txt")), KeyMod::Shift);
	EXPECT_EQ(EditorSelection::GetAssetSelectionCount(), 2u);
	EXPECT_FALSE(RowIsSelected(build.context, FindFileRow(build.context, build.root, "d.txt")));
}

TEST_P(AssetBrowserGestureTest, PlainClickCollapsesSelection) {
	WriteFourRows();
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	ClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "a.txt")));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "d.txt")), KeyMod::Shift);
	ASSERT_EQ(EditorSelection::GetAssetSelectionCount(), 4u);

	ClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "b.txt")));
	EXPECT_EQ(EditorSelection::GetAssetSelectionCount(), 1u);
	EXPECT_EQ(EditorSelection::GetSelectedAsset(), GuidOfRow("b.txt"));
	EXPECT_FALSE(RowIsSelected(build.context, FindFileRow(build.context, build.root, "a.txt")));
}

TEST_P(AssetBrowserGestureTest, RowMenuActsOnSelection) {
	WriteFourRows();
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	ClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "a.txt")));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "c.txt")), KeyMod::Shift);
	ASSERT_EQ(EditorSelection::GetAssetSelectionCount(), 3u);

	RightClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "b.txt")));
	ASSERT_EQ(build.context.GetOpenPopupCount(), 1u);
	UI::ElementHandle menu = build.context.GetOpenPopup(0);

	EXPECT_TRUE(build.context.IsAlive(MenuRowNamed(build.context, menu, "Delete 3 Items")));
	UI::ElementHandle rename = MenuRowNamed(build.context, menu, "Rename");
	ASSERT_TRUE(build.context.IsAlive(rename));
	EXPECT_FALSE(static_cast<bool>(build.context.Events(rename).onClick));
	EXPECT_EQ(EditorSelection::GetAssetSelectionCount(), 3u);
}

TEST_P(AssetBrowserGestureTest, RowMenuOutsideSelectionActsOnRow) {
	WriteFourRows();
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	ClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "a.txt")));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "b.txt")), KeyMod::Ctrl);
	ASSERT_EQ(EditorSelection::GetAssetSelectionCount(), 2u);

	RightClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "d.txt")));
	ASSERT_EQ(build.context.GetOpenPopupCount(), 1u);
	UI::ElementHandle menu = build.context.GetOpenPopup(0);

	EXPECT_FALSE(build.context.IsAlive(MenuRowNamed(build.context, menu, "Delete 2 Items")));
	UI::ElementHandle rename = MenuRowNamed(build.context, menu, "Rename");
	ASSERT_TRUE(build.context.IsAlive(rename));
	EXPECT_TRUE(static_cast<bool>(build.context.Events(rename).onClick));
	EXPECT_EQ(EditorSelection::GetAssetSelectionCount(), 2u);
}

TEST_P(AssetBrowserGestureTest, UnimportedFilesSelectNoAsset) {
	FileSystem::WriteTextFile(Path("project://Assets/keep.txt"), "x");
	FileSystem::WriteTextFile(Path("project://Assets/notes.unclaimed"), "x");
	AssetDatabase::ScanProjectDirectory();
	ASSERT_FALSE(AssetDatabase::IsAssetPath(Path("project://Assets/notes.unclaimed")));

	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	ClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "keep.txt")));
	ASSERT_TRUE(EditorSelection::HasAssetSelection());

	UI::ElementHandle other = FindFileRow(build.context, build.root, "notes.unclaimed");
	ASSERT_TRUE(build.context.IsAlive(other));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, other));

	EXPECT_TRUE(RowIsSelected(build.context, other));
	EXPECT_FALSE(EditorSelection::HasAssetSelection());
}

TEST_P(AssetBrowserGestureTest, CSharpFilesSelectAssetGuid) {
	FileSystem::WriteTextFile(Path("project://Assets/Spinner.cs"), "using Shalltear;\n");
	AssetDatabase::ScanProjectDirectory();
	Guid expected = AssetDatabase::LookupGuid(Path("project://Assets/Spinner.cs"));
	ASSERT_TRUE(expected.IsValid());

	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::ElementHandle row = FindFileRow(build.context, build.root, "Spinner.cs");
	ASSERT_TRUE(build.context.IsAlive(row));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, row));

	EXPECT_TRUE(RowIsSelected(build.context, row));
	EXPECT_TRUE(EditorSelection::HasAssetSelection());
	EXPECT_EQ(EditorSelection::GetSelectedAsset(), expected);
}

TEST_P(AssetBrowserGestureTest, BrowsersShareSelection) {
	WriteFourRows();
	Handle<EditorPanel> first = OpenBrowser();
	Handle<EditorPanel> second = OpenBrowser();
	ASSERT_TRUE(first.IsValid() && second.IsValid());
	ASSERT_NE(first.GetInstanceId(), second.GetInstanceId());

	PanelBuild buildA{BrowserContextDesc()};
	first.Get()->BuildContent(buildA.context, buildA.root);
	buildA.context.Update();
	PanelBuild buildB{BrowserContextDesc()};
	second.Get()->BuildContent(buildB.context, buildB.root);
	buildB.context.Update();

	ClickAt(buildA.context, *first.Get(), CenterOf(buildA.context, FindFileRow(buildA.context, buildA.root, "a.txt")));
	ClickAt(buildA.context, *first.Get(), CenterOf(buildA.context, FindFileRow(buildA.context, buildA.root, "c.txt")), KeyMod::Shift);
	ASSERT_EQ(EditorSelection::GetAssetSelectionCount(), 3u);

	second.Get()->OnTick(0.016f);
	buildB.context.Update();
	EXPECT_TRUE(RowIsSelected(buildB.context, FindFileRow(buildB.context, buildB.root, "b.txt")));

	ClickAt(buildB.context, *second.Get(), CenterOf(buildB.context, FindFileRow(buildB.context, buildB.root, "d.txt")));
	first.Get()->OnTick(0.016f);
	buildA.context.Update();
	EXPECT_EQ(EditorSelection::GetAssetSelectionCount(), 1u);
	EXPECT_FALSE(RowIsSelected(buildA.context, FindFileRow(buildA.context, buildA.root, "a.txt")));
}

TEST_P(AssetBrowserGestureTest, BrowserOpenedIntoExistingSelectionShowsIt) {
	WriteFourRows();
	EditorSelection::SelectAsset(GuidOfRow("c.txt"));

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	EXPECT_TRUE(RowIsSelected(build.context, FindFileRow(build.context, build.root, "c.txt")));
	EXPECT_FALSE(RowIsSelected(build.context, FindFileRow(build.context, build.root, "a.txt")));
}

TEST_P(AssetBrowserGestureTest, MultiDeleteRemovesAllClearsSelection) {
	WriteFourRows();
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	ClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "a.txt")));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "b.txt")), KeyMod::Shift);
	ASSERT_EQ(EditorSelection::GetAssetSelectionCount(), 2u);

	RightClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "b.txt")));
	ASSERT_EQ(build.context.GetOpenPopupCount(), 1u);
	UI::ElementHandle deleteRow = MenuRowNamed(build.context, build.context.GetOpenPopup(0), "Delete 2 Items");
	ASSERT_TRUE(build.context.IsAlive(deleteRow));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, deleteRow));

	ASSERT_EQ(build.context.GetOpenPopupCount(), 1u);
	UI::ElementHandle confirm = ButtonNamed(build.context, build.context.GetOpenPopup(0), "Delete");
	ASSERT_TRUE(build.context.IsAlive(confirm));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, confirm));

	EXPECT_FALSE(FileSystem::Exists(Path("project://Assets/a.txt")));
	EXPECT_FALSE(FileSystem::Exists(Path("project://Assets/b.txt")));
	EXPECT_TRUE(FileSystem::Exists(Path("project://Assets/c.txt")));
	EXPECT_EQ(EditorSelection::GetAssetSelectionCount(), 0u);
}

TEST_P(AssetBrowserGestureTest, RenameMovesFileRowFollows) {
	WriteFourRows();
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	ClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "a.txt")));
	RightClickAt(build.context, *panel.Get(), CenterOf(build.context, FindFileRow(build.context, build.root, "a.txt")));
	ASSERT_EQ(build.context.GetOpenPopupCount(), 1u);
	UI::ElementHandle rename = MenuRowNamed(build.context, build.context.GetOpenPopup(0), "Rename");
	ASSERT_TRUE(build.context.IsAlive(rename));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, rename));

	ASSERT_EQ(build.context.GetOpenPopupCount(), 1u);
	UI::ElementHandle card = build.context.GetOpenPopup(0);
	UI::ElementHandle field = FindClassed(build.context, card, StringHash("slt-textfield"));
	ASSERT_TRUE(build.context.IsAlive(field));
	for (char32_t ch : {U'z', U'z', U'.', U't', U'x', U't'}) {
		build.context.InjectInput(TextChar(ch));
	}
	build.context.Update();

	UI::ElementHandle ok = ButtonNamed(build.context, card, "OK");
	ASSERT_TRUE(build.context.IsAlive(ok));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, ok));
	panel.Get()->OnTick(0.016f);
	build.context.Update();

	EXPECT_FALSE(FileSystem::Exists(Path("project://Assets/a.txt")));
	EXPECT_TRUE(FileSystem::Exists(Path("project://Assets/zz.txt")));
	EXPECT_TRUE(FindFileRow(build.context, build.root, "zz.txt").IsValid());
	EXPECT_FALSE(FindFileRow(build.context, build.root, "a.txt").IsValid());
}

TEST_P(AssetBrowserGestureTest, SubfolderOnlyFolderListsFoldersFirst) {
	WriteSource("project://Assets/aaa.txt", "a");
	WriteSource("project://Assets/Parent/Alpha/a.txt", "a");
	WriteSource("project://Assets/Parent/Beta/b.txt", "b");
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::ElementHandle folder = FilePaneEntry(build.context, build.root, "Parent");
	UI::ElementHandle file = FilePaneEntry(build.context, build.root, "aaa.txt");
	ASSERT_TRUE(build.context.IsAlive(folder));
	ASSERT_TRUE(build.context.IsAlive(file));
	const UI::Rect f = build.context.GetLayout(folder).borderBox;
	const UI::Rect a = build.context.GetLayout(file).borderBox;
	EXPECT_TRUE(f.y + 0.5f < a.y || (std::abs(f.y - a.y) < 0.5f && f.x < a.x));

	Variant parent{str("Assets/Parent")};
	ASSERT_TRUE(SetField(panel.Get(), panel.Get()->GetType(), StringHash("selectedFolder"), &parent));
	PanelBuild second{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), second);
	EXPECT_TRUE(second.context.IsAlive(FilePaneEntry(second.context, second.root, "Alpha")));
	EXPECT_TRUE(second.context.IsAlive(FilePaneEntry(second.context, second.root, "Beta")));
	EXPECT_FALSE(HasText(second.context, second.root, "(empty folder)"));
}

TEST_P(AssetBrowserGestureTest, FolderDoubleClickOpensTreeFollows) {
	WriteSource("project://Assets/top.txt", "t");
	WriteSource("project://Assets/Sub/inner.txt", "i");
	AssetDatabase::ScanProjectDirectory();
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	ASSERT_TRUE(PressFilePaneEntry(build.context, *panel.Get(), build.root, "top.txt"));
	ASSERT_EQ(EditorSelection::GetAssetSelectionCount(), 1u);

	UI::ElementHandle sub = FilePaneEntry(build.context, build.root, "Sub");
	ASSERT_TRUE(build.context.IsAlive(sub));
	DoubleClick(build.context, *panel.Get(), CenterOf(build.context, sub));
	panel.Get()->OnTick(0.016f);
	build.context.Update();

	EXPECT_EQ(SelectedFolderOf(panel.Get()), "Assets/Sub");
	EXPECT_TRUE(build.context.IsAlive(FilePaneEntry(build.context, build.root, "inner.txt")));
	EXPECT_FALSE(build.context.IsAlive(FilePaneEntry(build.context, build.root, "top.txt")));
	UI::ElementHandle treeRow = TreeRow(build.context, build.root, "Sub");
	ASSERT_TRUE(build.context.IsAlive(treeRow));
	EXPECT_TRUE(RowIsSelected(build.context, treeRow));
	EXPECT_EQ(EditorSelection::GetAssetSelectionCount(), 0u);
}

TEST_P(AssetBrowserGestureTest, EnterOnAFolderEntryOpensIt) {
	WriteSource("project://Assets/top.txt", "t");
	WriteSource("project://Assets/Sub/inner.txt", "i");
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	ASSERT_TRUE(PressFilePaneEntry(build.context, *panel.Get(), build.root, "Sub"));
	ASSERT_EQ(SelectedFolderOf(panel.Get()), "Assets");
	PressKey(build.context, *panel.Get(), Key::Enter);
	panel.Get()->OnTick(0.016f);
	build.context.Update();
	EXPECT_EQ(SelectedFolderOf(panel.Get()), "Assets/Sub");
	EXPECT_TRUE(build.context.IsAlive(FilePaneEntry(build.context, build.root, "inner.txt")));
}

TEST_P(AssetBrowserGestureTest, AssetDropOnFolderMovesKeepsSelected) {
	WriteSource("project://Assets/notes.txt", "n");
	WriteSource("project://Assets/Sub/inner.txt", "i");
	AssetDatabase::ScanProjectDirectory();
	Guid notes = AssetDatabase::LookupGuid(Path("project://Assets/notes.txt"));
	Guid inner = AssetDatabase::LookupGuid(Path("project://Assets/Sub/inner.txt"));
	ASSERT_TRUE(notes.IsValid());
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	ASSERT_TRUE(PressFilePaneEntry(build.context, *panel.Get(), build.root, "notes.txt"));
	ASSERT_EQ(EditorSelection::GetSelectedAsset(), notes);

	UI::ElementHandle sub = FilePaneEntry(build.context, build.root, "Sub");
	ASSERT_TRUE(build.context.IsAlive(sub));
	Vec2 over = CenterOf(build.context, sub);

	UI::DragPayload already;
	already.type = "Asset"_hash;
	already.data = Variant(AssetDragData{inner, StringHash("TextAsset").GetValue()});
	EXPECT_FALSE(build.context.IsAlive(build.context.UpdateDragTarget(already, over)));
	build.context.ClearDragTarget();

	UI::DragPayload payload;
	payload.type = "Asset"_hash;
	payload.data = Variant(AssetDragData{notes, StringHash("TextAsset").GetValue()});
	EXPECT_EQ(build.context.UpdateDragTarget(payload, over), sub);
	ASSERT_TRUE(build.context.DeliverDrop(payload, over));
	panel.Get()->OnTick(0.016f);
	build.context.Update();

	EXPECT_FALSE(FileSystem::Exists(Path("project://Assets/notes.txt")));
	EXPECT_TRUE(FileSystem::Exists(Path("project://Assets/Sub/notes.txt")));
	EXPECT_EQ(AssetDatabase::LookupGuid(Path("project://Assets/Sub/notes.txt")), notes);
	EXPECT_EQ(EditorSelection::GetSelectedAsset(), notes);
	EXPECT_EQ(SelectedFolderOf(panel.Get()), "Assets");
}

TEST_P(AssetBrowserGestureTest, DelAndF2ActOnFolder) {
	WriteSource("project://Assets/top.txt", "t");
	WriteSource("project://Assets/Sub/inner.txt", "i");
	WriteSource("project://Assets/Doomed/gone.txt", "g");
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	ASSERT_TRUE(PressFilePaneEntry(build.context, *panel.Get(), build.root, "Doomed"));
	ASSERT_TRUE(panel.Get()->OnCommand(PanelCommand::Delete));
	build.context.Update();
	ASSERT_EQ(build.context.GetOpenPopupCount(), 1u);
	UI::ElementHandle card = build.context.GetOpenPopup(0);
	EXPECT_TRUE(HasText(build.context, card, "Delete folder 'Assets/Doomed' and all its contents?"));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, ButtonNamed(build.context, card, "Delete")));
	EXPECT_FALSE(FileSystem::Exists(Path("project://Assets/Doomed/gone.txt")));
	EXPECT_TRUE(FileSystem::Exists(Path("project://Assets/top.txt")));
	EXPECT_EQ(SelectedFolderOf(panel.Get()), "Assets");

	ASSERT_TRUE(PressFilePaneEntry(build.context, *panel.Get(), build.root, "Sub"));
	ASSERT_TRUE(panel.Get()->OnCommand(PanelCommand::Rename));
	build.context.Update();
	ASSERT_EQ(build.context.GetOpenPopupCount(), 1u);
	card = build.context.GetOpenPopup(0);
	UI::ElementHandle field = FindClassed(build.context, card, StringHash("slt-textfield"));
	ASSERT_TRUE(build.context.IsAlive(field));
	for (char32_t ch : {U'K', U'e', U'p', U't'}) {
		build.context.InjectInput(TextChar(ch));
	}
	build.context.Update();
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, ButtonNamed(build.context, card, "OK")));
	EXPECT_TRUE(FileSystem::Exists(Path("project://Assets/Kept/inner.txt")));
	EXPECT_FALSE(FileSystem::Exists(Path("project://Assets/Sub/inner.txt")));
}

TEST_P(AssetBrowserGestureTest, F2FieldTakesTypingWithoutClick) {
	WriteSource("project://Assets/old.txt", "o");
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	ASSERT_TRUE(PressFilePaneEntry(build.context, *panel.Get(), build.root, "old.txt"));
	ASSERT_TRUE(panel.Get()->OnCommand(PanelCommand::Rename));
	build.context.Update();
	ASSERT_EQ(build.context.GetOpenPopupCount(), 1u);
	UI::ElementHandle card = build.context.GetOpenPopup(0);
	for (char32_t ch : std::u32string_view(U"new.txt")) {
		build.context.InjectInput(TextChar(ch));
	}
	build.context.Update();
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, ButtonNamed(build.context, card, "OK")));
	EXPECT_TRUE(FileSystem::Exists(Path("project://Assets/new.txt")));
	EXPECT_FALSE(FileSystem::Exists(Path("project://Assets/old.txt")));
}

TEST_P(AssetBrowserGestureTest, F2RenamesNameAndKeepsExtension) {
	WriteSource("project://Assets/old.txt", "o");
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	ASSERT_TRUE(PressFilePaneEntry(build.context, *panel.Get(), build.root, "old.txt"));
	ASSERT_TRUE(panel.Get()->OnCommand(PanelCommand::Rename));
	build.context.Update();
	ASSERT_EQ(build.context.GetOpenPopupCount(), 1u);
	UI::ElementHandle card = build.context.GetOpenPopup(0);
	UI::ElementHandle field = FindClassed(build.context, card, StringHash("slt-textfield"));
	ASSERT_TRUE(build.context.IsAlive(field));
	EXPECT_EQ(build.context.GetTextString(field), "old");
	for (char32_t ch : std::u32string_view(U"fresh")) {
		build.context.InjectInput(TextChar(ch));
	}
	build.context.Update();
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, ButtonNamed(build.context, card, "OK")));
	EXPECT_TRUE(FileSystem::Exists(Path("project://Assets/fresh.txt")));
	EXPECT_FALSE(FileSystem::Exists(Path("project://Assets/fresh")));
	EXPECT_FALSE(FileSystem::Exists(Path("project://Assets/old.txt")));
}

TEST_P(AssetBrowserGestureTest, PathBarBackForwardRetrace) {
	WriteSource("project://Assets/top.txt", "t");
	WriteSource("project://Assets/Sub/Deep/inner.txt", "i");
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	ASSERT_EQ(SelectedFolderOf(panel.Get()), "Assets");

	auto step = [&](u32 index) {
		return UITest::FindByClass(build.context, FilePane(build.context, build.root), EditorTheme::Class::ToolButton, index);
	};
	auto offered = [&](UI::ElementHandle button) {
		return build.context.IsAlive(button) && !build.context.HasClass(button, EditorTheme::Class::Disabled);
	};
	ASSERT_TRUE(build.context.IsAlive(step(1)));
	EXPECT_FALSE(offered(step(0)));
	EXPECT_FALSE(offered(step(1)));

	ClickAt(build.context, *panel.Get(), CenterOf(build.context, TreeRow(build.context, build.root, "Sub")));
	ASSERT_EQ(SelectedFolderOf(panel.Get()), "Assets/Sub");
	UI::ElementHandle deep = FilePaneEntry(build.context, build.root, "Deep");
	ASSERT_TRUE(build.context.IsAlive(deep));
	DoubleClick(build.context, *panel.Get(), CenterOf(build.context, deep));
	ASSERT_EQ(SelectedFolderOf(panel.Get()), "Assets/Sub/Deep");
	EXPECT_TRUE(offered(step(0)));
	EXPECT_FALSE(offered(step(1)));

	ClickAt(build.context, *panel.Get(), CenterOf(build.context, step(0)));
	EXPECT_EQ(SelectedFolderOf(panel.Get()), "Assets/Sub");
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, step(0)));
	EXPECT_EQ(SelectedFolderOf(panel.Get()), "Assets");
	EXPECT_FALSE(offered(step(0)));
	EXPECT_TRUE(offered(step(1)));

	ClickAt(build.context, *panel.Get(), CenterOf(build.context, step(1)));
	EXPECT_EQ(SelectedFolderOf(panel.Get()), "Assets/Sub");
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, TreeRow(build.context, build.root, "Assets")));
	ASSERT_EQ(SelectedFolderOf(panel.Get()), "Assets");
	EXPECT_FALSE(offered(step(1)));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, step(0)));
	EXPECT_EQ(SelectedFolderOf(panel.Get()), "Assets/Sub");
}

namespace {

	void ShowFolder(EditorPanel& panel, UI::Context& context, const str& folder) {
		Variant value{folder};
		ASSERT_TRUE(SetField(&panel, panel.GetType(), StringHash("selectedFolder"), &value));
		panel.OnTick(0.016f);
		context.Update();
	}

} // namespace

TEST_P(AssetBrowserGestureTest, CrumbShowsFolderAndSeparatorListsSubfolders) {
	WriteSource("project://Assets/Sub/Deep/inner.txt", "i");
	WriteSource("project://Assets/Other/x.txt", "x");
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	ASSERT_NO_FATAL_FAILURE(ShowFolder(*panel.Get(), build.context, "Assets/Sub/Deep"));

	auto crumb = [&](const str& name) {
		UI::ElementHandle pane = FilePane(build.context, build.root);
		for (u32 index = 0;; ++index) {
			UI::ElementHandle found = UITest::FindByClass(build.context, pane, EditorTheme::Class::PathCrumb, index);
			if (!build.context.IsAlive(found) || build.context.GetTextString(found) == name) {
				return found;
			}
		}
	};
	ASSERT_TRUE(build.context.IsAlive(crumb("Assets")) && build.context.IsAlive(crumb("Sub")) && build.context.IsAlive(crumb("Deep")));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, crumb("Assets")));
	EXPECT_EQ(SelectedFolderOf(panel.Get()), "Assets");

	ASSERT_NO_FATAL_FAILURE(ShowFolder(*panel.Get(), build.context, "Assets/Sub/Deep"));
	UI::ElementHandle separator = UITest::FindByClass(build.context, FilePane(build.context, build.root), EditorTheme::Class::ToolButton, 2);
	ASSERT_TRUE(build.context.IsAlive(separator));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, separator));
	ASSERT_EQ(build.context.GetOpenPopupCount(), 1u);
	UI::ElementHandle menu = build.context.GetOpenPopup(0);
	UI::ElementHandle sub = MenuRowNamed(build.context, menu, "Sub");
	UI::ElementHandle other = MenuRowNamed(build.context, menu, "Other");
	ASSERT_TRUE(build.context.IsAlive(sub) && build.context.IsAlive(other));
	EXPECT_TRUE(FindClassed(build.context, sub, StringHash("slt-check-mark")).IsValid());
	EXPECT_FALSE(FindClassed(build.context, other, StringHash("slt-check-mark")).IsValid());
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, other));
	EXPECT_EQ(SelectedFolderOf(panel.Get()), "Assets/Other");
}

TEST_P(AssetBrowserGestureTest, OnlyEmptyBarPartAcceptsTyping) {
	WriteSource("project://Assets/Sub/inner.txt", "i");
	Handle<EditorPanel> panel = OpenBrowser();
	FontAsset* font = New<FontAsset>(MemTag::Asset);
	UI::ContextDesc desc = BrowserContextDesc();
	desc.tooltipFont = Handle<FontAsset>(font);
	desc.tooltipDelaySeconds = 0.0f;
	PanelBuild build{desc};
	BuildAndSettle(*panel.Get(), build);
	ASSERT_NO_FATAL_FAILURE(ShowFolder(*panel.Get(), build.context, "Assets/Sub"));

	UI::ElementHandle back = UITest::FindByClass(build.context, FilePane(build.context, build.root), EditorTheme::Class::ToolButton, 0);
	const UI::Element* bar = build.context.GetElement(build.context.MakeHandle(build.context.GetElement(back)->parent));
	const UI::Rect strip = build.context.GetLayout(build.context.MakeHandle(bar->lastChild)).borderBox;
	const str typing = UITest::TooltipAt(build.context, {strip.x + strip.w - 4.0f, strip.y + strip.h * 0.5f});
	ASSERT_FALSE(typing.empty());

	u32 crumbs = 0;
	for (;; ++crumbs) {
		UI::ElementHandle crumb = UITest::FindByClass(build.context, FilePane(build.context, build.root), EditorTheme::Class::PathCrumb, crumbs);
		if (!build.context.IsAlive(crumb)) {
			break;
		}
		EXPECT_NE(UITest::TooltipAt(build.context, CenterOf(build.context, crumb)), typing) << "the crumb \"" << build.context.GetTextString(crumb);
	}
	EXPECT_EQ(crumbs, 2u);
	Delete(font);
}

TEST_P(AssetBrowserGestureTest, TypedPathNavigatesOrRefusesInPlace) {
	WriteSource("project://Assets/Sub/Deep/inner.txt", "i");
	AssetDatabase::ScanProjectDirectory();
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	auto field = [&]() { return FindClassed(build.context, FilePane(build.context, build.root), StringHash("slt-textfield")); };
	auto openField = [&]() {
		UI::ElementHandle back = UITest::FindByClass(build.context, FilePane(build.context, build.root), EditorTheme::Class::ToolButton, 0);
		const UI::Element* bar = build.context.GetElement(build.context.MakeHandle(build.context.GetElement(back)->parent));
		UI::ElementHandle strip = build.context.MakeHandle(bar->lastChild);
		const UI::Rect box = build.context.GetLayout(strip).borderBox;
		ClickAt(build.context, *panel.Get(), {box.x + box.w - 4.0f, box.y + box.h * 0.5f});
		return field();
	};
	auto type = [&](std::u32string_view text) {
		for (char32_t ch : text) {
			build.context.InjectInput(TextChar(ch));
		}
		build.context.Update();
		PressKey(build.context, *panel.Get(), Key::Enter);
	};

	UI::ElementHandle typing = openField();
	ASSERT_TRUE(build.context.IsAlive(typing));
	EXPECT_EQ(build.context.GetFocused(), typing);
	EXPECT_EQ(build.context.GetTextString(typing), "Assets");
	type(U"assets/sub/deep");
	EXPECT_EQ(SelectedFolderOf(panel.Get()), "Assets/Sub/Deep");
	EXPECT_FALSE(build.context.IsAlive(field()));

	ASSERT_NO_FATAL_FAILURE(ShowFolder(*panel.Get(), build.context, "Assets"));
	ASSERT_TRUE(build.context.IsAlive(openField()));
	type(U"Assets/Sub/Deep/inner.txt");
	EXPECT_EQ(SelectedFolderOf(panel.Get()), "Assets/Sub/Deep");
	EXPECT_EQ(EditorSelection::GetSelectedAsset(), AssetDatabase::LookupGuid(Path("project://Assets/Sub/Deep/inner.txt")));

	ASSERT_TRUE(build.context.IsAlive(openField()));
	type(U"Assets/Nope");
	typing = field();
	ASSERT_TRUE(build.context.IsAlive(typing));
	EXPECT_TRUE(build.context.HasClass(typing, EditorTheme::Class::FieldRefused));
	EXPECT_EQ(build.context.GetFocused(), typing);
	EXPECT_EQ(build.context.GetTextString(typing), "Assets/Nope");
	EXPECT_EQ(SelectedFolderOf(panel.Get()), "Assets/Sub/Deep");

	build.context.InjectInput(TextChar(U'x'));
	build.context.Update();
	EXPECT_FALSE(build.context.HasClass(typing, EditorTheme::Class::FieldRefused));
	PressKey(build.context, *panel.Get(), Key::Escape);
	panel.Get()->OnTick(0.016f);
	build.context.Update();
	EXPECT_FALSE(build.context.IsAlive(field()));
	EXPECT_EQ(SelectedFolderOf(panel.Get()), "Assets/Sub/Deep");

	ASSERT_TRUE(build.context.IsAlive(openField()));
	for (char32_t ch : std::u32string_view(U"Assets")) {
		build.context.InjectInput(TextChar(ch));
	}
	build.context.Update();
	const UI::Rect tree = build.context.GetLayout(TreePane(build.context, build.root)).borderBox;
	ClickAt(build.context, *panel.Get(), {tree.x + 8.0f, tree.y + tree.h - 8.0f});
	panel.Get()->OnTick(0.016f);
	build.context.Update();
	EXPECT_FALSE(build.context.IsAlive(field()));
	EXPECT_EQ(SelectedFolderOf(panel.Get()), "Assets/Sub/Deep");
}

TEST_P(AssetBrowserGestureTest, ChangingFolderLeavesTheEditorsSelectionAlone) {
	WriteSource("project://Assets/top.txt", "t");
	WriteSource("project://Assets/Sub/inner.txt", "i");
	AssetDatabase::ScanProjectDirectory();
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	ASSERT_TRUE(PressFilePaneEntry(build.context, *panel.Get(), build.root, "top.txt"));
	const Guid top = AssetDatabase::LookupGuid(Path("project://Assets/top.txt"));
	ASSERT_TRUE(top.IsValid());
	ASSERT_EQ(EditorSelection::GetSelectedAsset(), top);
	UI::ElementHandle sub = TreeRow(build.context, build.root, "Sub");
	ASSERT_TRUE(build.context.IsAlive(sub));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, sub));
	ASSERT_EQ(SelectedFolderOf(panel.Get()), "Assets/Sub");
	EXPECT_EQ(EditorSelection::GetSelectedAsset(), top);

	Handle<Scene> scene = SceneSystem::CreateScene("Level");
	Handle<SceneObject> object = SceneSystem::CreateObject("Level", scene);
	EditorSelection::Select(Handle<Object>(object.Get()));
	UI::ElementHandle assets = TreeRow(build.context, build.root, "Assets");
	ASSERT_TRUE(build.context.IsAlive(assets));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, assets));
	ASSERT_EQ(SelectedFolderOf(panel.Get()), "Assets");
	EXPECT_EQ(EditorSelection::GetActive().GetInstanceId(), object.GetInstanceId());
	SceneSystem::UnloadScene(scene);
	SceneSystem::EndFrame();
}

TEST_P(AssetBrowserGestureTest, ReturningToFolderLightsAssetStillSelected) {
	WriteSource("project://Assets/top.txt", "t");
	WriteSource("project://Assets/Sub/inner.txt", "i");
	AssetDatabase::ScanProjectDirectory();
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	ASSERT_TRUE(PressFilePaneEntry(build.context, *panel.Get(), build.root, "top.txt"));
	const Guid top = AssetDatabase::LookupGuid(Path("project://Assets/top.txt"));
	ASSERT_TRUE(top.IsValid());
	ASSERT_EQ(EditorSelection::GetSelectedAsset(), top);

	ClickAt(build.context, *panel.Get(), CenterOf(build.context, TreeRow(build.context, build.root, "Sub")));
	ASSERT_EQ(SelectedFolderOf(panel.Get()), "Assets/Sub");
	ASSERT_FALSE(build.context.IsAlive(FilePaneEntry(build.context, build.root, "top.txt")));
	ClickAt(build.context, *panel.Get(), CenterOf(build.context, TreeRow(build.context, build.root, "Assets")));
	ASSERT_EQ(SelectedFolderOf(panel.Get()), "Assets");

	UI::ElementHandle entry = FilePaneEntry(build.context, build.root, "top.txt");
	ASSERT_TRUE(build.context.IsAlive(entry));
	EXPECT_TRUE(RowIsSelected(build.context, entry));
	EXPECT_EQ(EditorSelection::GetSelectedAsset(), top);
}

TEST_P(AssetBrowserGestureTest, EmptySpaceClickDeselectsSameFrame) {
	WriteSource("project://Assets/a.txt", "a");
	AssetDatabase::ScanProjectDirectory();
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	ASSERT_TRUE(PressFilePaneEntry(build.context, *panel.Get(), build.root, "a.txt"));
	UI::ElementHandle entry = FilePaneEntry(build.context, build.root, "a.txt");
	ASSERT_TRUE(RowIsSelected(build.context, entry));
	ASSERT_EQ(EditorSelection::GetAssetSelectionCount(), 1u);

	UI::ElementHandle viewport =
		UITest::FindByClass(build.context, FilePane(build.context, build.root), StringHash("slt-scroll-viewport"), 0);
	ASSERT_TRUE(build.context.IsAlive(viewport));
	const UI::Rect& window = build.context.GetLayout(viewport).content;
	const Vec2 empty{window.x + window.w * 0.5f, window.y + window.h - 6.0f};
	ASSERT_EQ(build.context.HitTest(empty), viewport);

	ClickWithoutTick(build.context, empty);
	EXPECT_FALSE(RowIsSelected(build.context, entry));
	EXPECT_EQ(EditorSelection::GetAssetSelectionCount(), 0u);

	Handle<Scene> scene = SceneSystem::CreateScene("Level");
	Handle<SceneObject> object = SceneSystem::CreateObject("Level", scene);
	EditorSelection::Select(Handle<Object>(object.Get()));
	ClickWithoutTick(build.context, empty);
	EXPECT_EQ(EditorSelection::GetSelectionCount(), 0u);
	SceneSystem::UnloadScene(scene);
	SceneSystem::EndFrame();

	ASSERT_TRUE(PressFilePaneEntry(build.context, *panel.Get(), build.root, "a.txt"));
	UI::ElementHandle treeViewport =
		UITest::FindByClass(build.context, TreePane(build.context, build.root), StringHash("slt-scroll-viewport"), 0);
	ASSERT_TRUE(build.context.IsAlive(treeViewport));
	const UI::Rect& treeWindow = build.context.GetLayout(treeViewport).content;
	const Vec2 treeEmpty{treeWindow.x + treeWindow.w * 0.5f, treeWindow.y + treeWindow.h - 6.0f};
	ASSERT_EQ(build.context.HitTest(treeEmpty), treeViewport);
	ClickWithoutTick(build.context, treeEmpty);
	EXPECT_EQ(SelectedFolderOf(panel.Get()), "Assets");
	EXPECT_EQ(EditorSelection::GetAssetSelectionCount(), 1u);
}

TEST_F(AssetBrowserPanelTest, AssetDropOnTreeFolderMoves) {
	WriteSource("project://Assets/notes.txt", "n");
	WriteSource("project://Assets/Sub/inner.txt", "i");
	AssetDatabase::ScanProjectDirectory();
	Guid notes = AssetDatabase::LookupGuid(Path("project://Assets/notes.txt"));
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::ElementHandle row = TreeRow(build.context, build.root, "Sub");
	ASSERT_TRUE(build.context.IsAlive(row));
	UI::DragPayload payload;
	payload.type = "Asset"_hash;
	payload.data = Variant(AssetDragData{notes, StringHash("TextAsset").GetValue()});
	EXPECT_EQ(build.context.UpdateDragTarget(payload, CenterOf(build.context, row)), row);
	ASSERT_TRUE(build.context.DeliverDrop(payload, CenterOf(build.context, row)));
	EXPECT_TRUE(FileSystem::Exists(Path("project://Assets/Sub/notes.txt")));
	EXPECT_EQ(AssetDatabase::LookupGuid(Path("project://Assets/Sub/notes.txt")), notes);
}

TEST_P(AssetBrowserGestureTest, ModeParameterReallyChangesWhatIsBuilt) {
	WriteFourRows();
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::ElementHandle entry = FindFileRow(build.context, build.root, "a.txt");
	ASSERT_TRUE(entry.IsValid());
	if (AssetBrowserOps::SliderIsGrid(GetParam())) {
		EXPECT_TRUE(build.context.HasClass(entry, StringHash("ed-asset-cell")));
		EXPECT_FALSE(build.context.HasClass(entry, StringHash("slt-tree-row")));
	} else {
		EXPECT_TRUE(build.context.HasClass(entry, StringHash("slt-tree-row")));
		EXPECT_FALSE(build.context.HasClass(entry, StringHash("ed-asset-cell")));
	}
}

INSTANTIATE_TEST_SUITE_P(BothLayoutModes, AssetBrowserGestureTest,
						 ::testing::Values(0.0f, 0.5f),
						 [](const ::testing::TestParamInfo<f32>& info) {
							 return info.param <= 0.0f ? "List" : "Grid";
						 });

TEST(AssetGridLayoutTest, LeftmostStopIsHittableList) {
	EXPECT_FALSE(AssetBrowserOps::SliderIsGrid(0.0f));
	EXPECT_EQ(AssetBrowserOps::CellSizeForSlider(0.0f, 50.0f, 100.0f, 20.0f).widthPx, 0.0f);
	EXPECT_FALSE(AssetBrowserOps::SliderIsGrid(0.01f));
	EXPECT_FALSE(AssetBrowserOps::SliderIsGrid(0.05f));
	EXPECT_TRUE(AssetBrowserOps::SliderIsGrid(0.1f));
	EXPECT_FALSE(AssetBrowserOps::SliderIsGrid(AssetBrowserOps::SLIDER_SMALLEST_GRID - 0.001f));
	EXPECT_TRUE(AssetBrowserOps::SliderIsGrid(AssetBrowserOps::SLIDER_SMALLEST_GRID));
	EXPECT_FLOAT_EQ(AssetBrowserOps::CellSizeForSlider(AssetBrowserOps::SLIDER_SMALLEST_GRID, 50.0f, 100.0f, 20.0f).widthPx,
					50.0f);
}

TEST(AssetGridLayoutTest, SliderScalesSquarePlusLabel) {
	AssetBrowserOps::GridCellSize small = AssetBrowserOps::CellSizeForSlider(0.08f, 50.0f, 100.0f, 20.0f);
	AssetBrowserOps::GridCellSize large = AssetBrowserOps::CellSizeForSlider(1.0f, 50.0f, 100.0f, 20.0f);
	EXPECT_GT(large.widthPx, small.widthPx);
	EXPECT_FLOAT_EQ(small.heightPx - small.widthPx, 20.0f);
	EXPECT_FLOAT_EQ(large.heightPx - large.widthPx, 20.0f);
	EXPECT_LE(large.widthPx, 100.0f);
	EXPECT_GE(small.widthPx, 50.0f);
}

TEST(AssetGridLayoutTest, NarrowPaneShowsOneColumn) {
	EXPECT_EQ(AssetBrowserOps::GridColumnsFor(400.0f, 100.0f), 4u);
	EXPECT_EQ(AssetBrowserOps::GridColumnsFor(449.0f, 100.0f), 4u);
	EXPECT_EQ(AssetBrowserOps::GridColumnsFor(30.0f, 100.0f), 1u);
	EXPECT_EQ(AssetBrowserOps::GridColumnsFor(0.0f, 100.0f), 1u);
	EXPECT_EQ(AssetBrowserOps::GridColumnsFor(400.0f, 0.0f), 1u);
}

TEST(AssetGridLayoutTest, OnScreenBoundedByViewport) {
	AssetBrowserOps::GridWindow window = AssetBrowserOps::VisibleGridWindow(4000, 4, 100.0f, 0.0f, 250.0f);
	EXPECT_EQ(window.firstIndex, 0u);
	EXPECT_EQ(window.count, 12u);

	AssetBrowserOps::GridWindow bigger = AssetBrowserOps::VisibleGridWindow(40000, 4, 100.0f, 0.0f, 250.0f);
	EXPECT_EQ(bigger.count, window.count);
}

TEST(AssetGridLayoutTest, ScrollKeepsLastTouchedRow) {
	AssetBrowserOps::GridWindow window = AssetBrowserOps::VisibleGridWindow(100, 4, 100.0f, 150.0f, 250.0f);
	EXPECT_EQ(window.firstIndex, 4u);
	EXPECT_EQ(window.count, 12u);

	AssetBrowserOps::GridWindow tail = AssetBrowserOps::VisibleGridWindow(6, 4, 100.0f, 0.0f, 250.0f);
	EXPECT_EQ(tail.firstIndex, 0u);
	EXPECT_EQ(tail.count, 6u);

	EXPECT_EQ(AssetBrowserOps::VisibleGridWindow(6, 4, 100.0f, 900.0f, 250.0f).count, 0u);
	EXPECT_EQ(AssetBrowserOps::VisibleGridWindow(0, 4, 100.0f, 0.0f, 250.0f).count, 0u);
	EXPECT_EQ(AssetBrowserOps::VisibleGridWindow(10, 4, 0.0f, 0.0f, 250.0f).count, 0u);
	EXPECT_EQ(AssetBrowserOps::VisibleGridWindow(10, 4, 100.0f, 0.0f, 0.0f).count, 0u);
}

TEST_F(AssetBrowserPanelTest, TheBrowserDefaultsToTheGrid) {
	Handle<EditorPanel> fresh =
		PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("AssetBrowserPanel")));
	ASSERT_TRUE(fresh.IsValid());
	Variant stored;
	ASSERT_TRUE(GetField(fresh.Get(), fresh.Get()->GetType(), StringHash("iconSize"), &stored));
	EXPECT_TRUE(AssetBrowserOps::SliderIsGrid(stored.As<f32>()));
	const f32 minSide = EditorTheme::GetMetric(EditorTheme::MetricToken::BrowserCellMin);
	EXPECT_FLOAT_EQ(AssetBrowserOps::CellSizeForSlider(stored.As<f32>(), minSide,
													   EditorTheme::GetMetric(EditorTheme::MetricToken::BrowserCellMax), 0.0f)
						.widthPx,
					minSide);
}

TEST_F(AssetBrowserPanelTest, LeftmostSliderStopShowsList) {
	WriteFourRows();
	browserIconSize = 0.6f;
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	ASSERT_TRUE(build.context.HasClass(FindFileRow(build.context, build.root, "a.txt"),
									   StringHash("ed-asset-cell")));

	UI::ElementHandle track = FindClassed(build.context, build.root, StringHash("slt-slider-track"));
	ASSERT_TRUE(build.context.IsAlive(track));
	const UI::Rect& box = build.context.GetLayout(track).content;
	ClickAt(build.context, *panel.Get(),
			{box.x + 1.0f, box.y + box.h * 0.5f});

	ASSERT_GT(build.context.GetLayout(track).content.w, 0.0f);

	UI::ElementHandle entry = FindFileRow(build.context, build.root, "a.txt");
	ASSERT_TRUE(entry.IsValid());
	EXPECT_TRUE(build.context.HasClass(entry, StringHash("slt-tree-row")));
}

TEST_F(AssetBrowserPanelTest, GridFillsPaneWidth) {
	for (u32 i = 0; i < 24; ++i) {
		WriteSource("project://Assets/f" + std::to_string(i) + ".txt", "x");
	}
	browserIconSize = 0.08f;
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	Vector<UI::Rect> cells{GetMemoryResource(MemTag::Editor)};
	CollectCellRects(build.context, build.root, cells);
	ASSERT_EQ(cells.size(), 24u);

	u32 sharingTheFirstTop = 0;
	u32 sharingTheFirstLeft = 0;
	for (const UI::Rect& cell : cells) {
		if (std::abs(cell.y - cells[0].y) < 0.5f) {
			++sharingTheFirstTop;
		}
		if (std::abs(cell.x - cells[0].x) < 0.5f) {
			++sharingTheFirstLeft;
		}
	}
	EXPECT_GT(sharingTheFirstTop, 1u);
	EXPECT_GT(sharingTheFirstLeft, 1u);
}

TEST_F(AssetBrowserPanelTest, ResizeKeepsPressedCellAlive) {
	for (u32 i = 0; i < 24; ++i) {
		WriteSource("project://Assets/f" + std::to_string(i) + ".txt", "x");
	}
	browserIconSize = 0.08f;
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::ElementHandle cell = FindFileRow(build.context, build.root, "f0.txt");
	ASSERT_TRUE(build.context.IsAlive(cell));
	Vec2 point = CenterOf(build.context, cell);

	InputEvent event{};
	event.button = MouseButton::Left;
	event.mousePosPx = {static_cast<i32>(point.x), static_cast<i32>(point.y)};
	event.type = InputEventType::MouseMove;
	build.context.InjectInput(event);
	event.type = InputEventType::MouseDown;
	build.context.InjectInput(event);
	build.context.Update();

	build.context.Resize({420, 600});
	build.context.Update();
	panel.Get()->OnTick(0.016f);
	build.context.Update();
	EXPECT_TRUE(build.context.IsAlive(cell));

	event.type = InputEventType::MouseUp;
	build.context.InjectInput(event);
	build.context.Update();
	panel.Get()->OnTick(0.016f);
	build.context.Update();

	Vector<UI::Rect> cells{GetMemoryResource(MemTag::Editor)};
	CollectCellRects(build.context, build.root, cells);
	ASSERT_FALSE(cells.empty());
	u32 sharingTheFirstTop = 0;
	for (const UI::Rect& box : cells) {
		if (std::abs(box.y - cells[0].y) < 0.5f) {
			++sharingTheFirstTop;
		}
	}
	EXPECT_LT(sharingTheFirstTop, 24u);
}

TEST_F(AssetBrowserPanelTest, NoGridCellCarriesACustomDraw) {
	WriteFourRows();
	browserIconSize = 0.6f;
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	u32 cells = 0;
	u32 withCustomDraw = 0;
	CountCells(build.context, build.root, cells, withCustomDraw);
	ASSERT_EQ(cells, 4u);
	EXPECT_EQ(withCustomDraw, 0u);
}

TEST_F(AssetBrowserPanelTest, GridShowsFewerCellsThanFolder) {
	for (u32 i = 0; i < 120; ++i) {
		WriteSource("project://Assets/f" + std::to_string(i) + ".txt", "x");
	}
	AssetDatabase::ScanProjectDirectory();
	browserIconSize = 1.0f;
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::ElementHandle scrollRoot = FindScrollRootHoldingCells(build.context, build.root);
	ASSERT_TRUE(build.context.IsAlive(scrollRoot));
	const UI::Rect& window = build.context.GetLayout(scrollRoot).content;

	u32 built = 0;
	u32 onScreen = 0;
	CountCellsOnScreen(build.context, build.root, window, built, onScreen);
	EXPECT_EQ(built, 120u);
	EXPECT_GT(onScreen, 0u);
	EXPECT_LT(onScreen, built);
}

namespace {

	class AssetBrowserGridPreviewTest : public AssetBrowserPanelTest {
	protected:
		void SetUp() override {
			AssetBrowserPanelTest::SetUp();
			EditorPreviews::Initialize();
			EditorPreviews::RegisterBuiltIns();
			EditorPreviewStore::Initialize();
			browserIconSize = 1.0f;
		}
		void TearDown() override {
			EditorPreviewStore::Terminate();
			EditorPreviews::Terminate();
			AssetBrowserPanelTest::TearDown();
		}

		static u32 GuidsTheStoreWasAskedAbout() {
			return EditorPreviewStore::GetCachedCount() + EditorPreviewStore::GetPendingCount();
		}
	};

} // anonymous namespace

TEST_F(AssetBrowserGridPreviewTest, OnlyVisibleCellsAsked) {
	for (u32 i = 0; i < 120; ++i) {
		WriteSource("project://Assets/f" + std::to_string(i) + ".txt", "x");
	}
	AssetDatabase::ScanProjectDirectory();
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	for (int frame = 0; frame < 2; ++frame) {
		panel.Get()->OnTick(0.016f);
		build.context.Update();
	}

	UI::ElementHandle scrollRoot = FindScrollRootHoldingCells(build.context, build.root);
	ASSERT_TRUE(build.context.IsAlive(scrollRoot));
	u32 built = 0;
	u32 onScreen = 0;
	CountCellsOnScreen(build.context, build.root, build.context.GetLayout(scrollRoot).content, built, onScreen);
	ASSERT_EQ(built, 120u);
	ASSERT_GT(onScreen, 0u);

	u32 asked = GuidsTheStoreWasAskedAbout();
	EXPECT_GT(asked, 0u);
	EXPECT_LT(asked, built);
	EXPECT_EQ(asked, onScreen);
}

TEST_F(AssetBrowserGridPreviewTest, ScrollAsksNewCells) {
	for (u32 i = 0; i < 120; ++i) {
		WriteSource("project://Assets/f" + std::to_string(i) + ".txt", "x");
	}
	AssetDatabase::ScanProjectDirectory();
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	for (int frame = 0; frame < 2; ++frame) {
		panel.Get()->OnTick(0.016f);
		build.context.Update();
	}
	u32 beforeScroll = GuidsTheStoreWasAskedAbout();
	ASSERT_GT(beforeScroll, 0u);

	UI::ElementHandle scrollRoot = FindScrollRootHoldingCells(build.context, build.root);
	ASSERT_TRUE(build.context.IsAlive(scrollRoot));
	build.context.SetScrollOffset(scrollRoot, {0.0f, 4000.0f});
	build.context.Update();
	panel.Get()->OnTick(0.016f);
	build.context.Update();

	EXPECT_GT(GuidsTheStoreWasAskedAbout(), beforeScroll);
}

TEST_F(AssetBrowserGridPreviewTest, ListModeAsksPreviewStoreForNothing) {
	for (u32 i = 0; i < 20; ++i) {
		WriteSource("project://Assets/f" + std::to_string(i) + ".txt", "x");
	}
	AssetDatabase::ScanProjectDirectory();
	browserIconSize = 0.0f;
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	panel.Get()->OnTick(0.016f);
	build.context.Update();

	EXPECT_EQ(GuidsTheStoreWasAskedAbout(), 0u);
}

TEST_F(AssetBrowserGridPreviewTest, GridAsksWithinCacheCapacity) {
	for (u32 i = 0; i < 120; ++i) {
		WriteSource("project://Assets/f" + std::to_string(i) + ".txt", "x");
	}
	AssetDatabase::ScanProjectDirectory();
	EditorPreviewStore::SetCapacity(3);
	browserIconSize = 0.08f;

	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	panel.Get()->OnTick(0.016f);
	build.context.Update();

	UI::ElementHandle scrollRoot = FindScrollRootHoldingCells(build.context, build.root);
	ASSERT_TRUE(build.context.IsAlive(scrollRoot));
	u32 built = 0;
	u32 onScreen = 0;
	CountCellsOnScreen(build.context, build.root, build.context.GetLayout(scrollRoot).content, built, onScreen);
	ASSERT_GT(onScreen, 3u);

	EXPECT_LE(GuidsTheStoreWasAskedAbout(), EditorPreviewStore::GetCapacity());
}

TEST_F(AssetBrowserGridPreviewTest, DefaultCapacityHoldsFullWindow) {
	Variant stored;
	Handle<EditorPanel> fresh =
		PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("AssetBrowserPanel")));
	ASSERT_TRUE(fresh.IsValid());
	ASSERT_TRUE(GetField(fresh.Get(), fresh.Get()->GetType(), StringHash("iconSize"), &stored));

	AssetBrowserOps::GridCellSize cell = AssetBrowserOps::CellSizeForSlider(
		stored.As<f32>(), EditorTheme::GetMetric(EditorTheme::MetricToken::BrowserCellMin),
		EditorTheme::GetMetric(EditorTheme::MetricToken::BrowserCellMax),
		EditorTheme::GetMetric(EditorTheme::MetricToken::BrowserCellLabel));
	ASSERT_GT(cell.widthPx, 0.0f);

	constexpr f32 PANE_WIDTH = 1536.0f - 210.0f;
	constexpr f32 PANE_HEIGHT = 864.0f;
	u32 columns = AssetBrowserOps::GridColumnsFor(PANE_WIDTH, cell.widthPx);
	u32 fullWindow = AssetBrowserOps::VisibleGridWindow(100000, columns, cell.heightPx, 0.0f, PANE_HEIGHT).count;
	ASSERT_GT(fullWindow, 0u);

	EXPECT_GE(EditorPreviewStore::GetCapacity(), fullWindow);
}

TEST(AssetGridLayoutTest, SliderAsksFewBadgeSizes) {
	using EditorTheme::MetricToken;
	const f32 minSide = EditorTheme::GetMetric(MetricToken::BrowserCellMin);
	const f32 maxSide = EditorTheme::GetMetric(MetricToken::BrowserCellMax);
	const f32 labelBand = EditorTheme::GetMetric(MetricToken::BrowserCellLabel);
	const f32 pad = EditorTheme::GetMetric(MetricToken::BrowserCellPad);

	std::vector<f32> distinct;
	f32 previous = 0.0f;
	for (u32 step = 0; step <= 1000; ++step) {
		const f32 slider = static_cast<f32>(step) / 1000.0f;
		if (!AssetBrowserOps::SliderIsGrid(slider)) {
			continue;
		}
		const f32 well = AssetBrowserOps::CellSizeForSlider(slider, minSide, maxSide, labelBand).widthPx - pad * 2.0f;
		const f32 glyph = AssetBrowserOps::BadgeGlyphSizeFor(well);

		EXPECT_LE(glyph, well) << slider;
		EXPECT_GE(glyph, previous) << slider;
		previous = glyph;
		if (std::find(distinct.begin(), distinct.end(), glyph) == distinct.end()) {
			distinct.push_back(glyph);
		}
	}

	EXPECT_GT(distinct.size(), 1u);
	EXPECT_LE(distinct.size(), 8u);
}

TEST(AssetGridLayoutTest, FoldOutButtonHas3Sizes) {
	using EditorTheme::MetricToken;
	const f32 minSide = EditorTheme::GetMetric(MetricToken::BrowserCellMin);
	const f32 maxSide = EditorTheme::GetMetric(MetricToken::BrowserCellMax);
	const f32 labelBand = EditorTheme::GetMetric(MetricToken::BrowserCellLabel);
	const f32 small = EditorTheme::GetMetric(MetricToken::BrowserFoldButtonSmall);
	const f32 medium = EditorTheme::GetMetric(MetricToken::BrowserFoldButtonMedium);
	const f32 large = EditorTheme::GetMetric(MetricToken::BrowserFoldButtonLarge);

	std::vector<f32> distinct;
	f32 previous = 0.0f;
	for (u32 step = 0; step <= 1000; ++step) {
		const f32 slider = static_cast<f32>(step) / 1000.0f;
		if (!AssetBrowserOps::SliderIsGrid(slider)) {
			continue;
		}
		const f32 side = AssetBrowserOps::CellSizeForSlider(slider, minSide, maxSide, labelBand).widthPx;
		const f32 button = AssetBrowserOps::FoldButtonSizeFor(side, maxSide, small, medium, large);
		EXPECT_GE(button, previous) << slider;
		previous = button;
		if (std::find(distinct.begin(), distinct.end(), button) == distinct.end()) {
			distinct.push_back(button);
		}
	}
	EXPECT_GT(distinct.size(), 1u);
	EXPECT_LE(distinct.size(), 3u);
}

TEST(AssetGridLayoutTest, FoldOutBandOnePiecePerRow) {
	Vector<AssetBrowserOps::FoldOutSegment> segments{GetMemoryResource(MemTag::Editor)};
	auto run = [&segments](std::initializer_list<u32> depths, u32 columns) {
		Vector<u32> list(depths.begin(), depths.end(), GetMemoryResource(MemTag::Editor));
		AssetBrowserOps::FoldOutBandSegments(list, columns, segments);
	};

	run({0, 0, 1, 1, 1, 1, 0, 0, 1}, 4);
	ASSERT_EQ(segments.size(), 3u);
	EXPECT_EQ(segments[0].first, 2u);
	EXPECT_EQ(segments[0].last, 3u);
	EXPECT_FALSE(segments[0].fromAbove);
	EXPECT_TRUE(segments[0].continuesBelow);
	EXPECT_EQ(segments[1].first, 4u);
	EXPECT_EQ(segments[1].last, 5u);
	EXPECT_TRUE(segments[1].fromAbove);
	EXPECT_FALSE(segments[1].continuesBelow);
	EXPECT_EQ(segments[2].first, 8u);
	EXPECT_FALSE(segments[2].fromAbove);

	run({0, 1, 1, 0, 0}, 3);
	ASSERT_EQ(segments.size(), 1u);
	EXPECT_EQ(segments[0].last, 2u);
	EXPECT_FALSE(segments[0].continuesBelow);

	run({0, 0, 0}, 3);
	EXPECT_TRUE(segments.empty());
	run({1, 1}, 0);
	EXPECT_TRUE(segments.empty());
}

TEST_F(AssetBrowserPanelTest, TypeBadgeAndPreviewOccupySameWell) {
	WriteFourRows();
	browserIconSize = 0.6f;
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	UI::ElementHandle cell = FindFileRow(build.context, build.root, "a.txt");
	ASSERT_TRUE(cell.IsValid());
	ASSERT_TRUE(build.context.HasClass(cell, StringHash("ed-asset-cell")));

	UI::ElementHandle square = FindByClass(build.context, cell, StringHash("ed-asset-cell-well"), 0);
	ASSERT_TRUE(build.context.IsAlive(square));
	const UI::Element* element = build.context.GetElement(square);
	ASSERT_NE(element->firstChild, UI::INVALID_UI_INDEX);
	UI::ElementHandle image = build.context.MakeHandle(element->firstChild);
	UI::ElementHandle badge = build.context.MakeHandle(build.context.GetElement(image)->nextSibling);
	ASSERT_TRUE(build.context.IsAlive(badge));

	UI::Style imageStyle = build.context.GetStyle(image);
	UI::Style badgeStyle = build.context.GetStyle(badge);
	ASSERT_FALSE(imageStyle.width.IsAuto());
	EXPECT_EQ(badgeStyle.width, imageStyle.width);
	EXPECT_EQ(badgeStyle.height, imageStyle.height);
	EXPECT_EQ(imageStyle.width, imageStyle.height);
}

TEST_F(AssetBrowserGridPreviewTest, StoreAnswerReachesCellAtBuild) {
	EditorPreviews::RegisterProducer(StringHash("TextAsset"),
									 [](const AssetPreviewRequest&, AssetPreviewImage& out) {
										 static constexpr u8 PIXELS[2 * 2 * 4] = {
											 255, 0, 0, 255, 0, 255, 0, 255,
											 0, 0, 255, 255, 255, 255, 255, 255};
										 out.size = {2, 2};
										 out.pixelsFormat = RHI::Format::RGBA8Unorm;
										 out.pixels = {PIXELS, sizeof(PIXELS)};
										 return true;
									 });
	WriteSource("project://Assets/a.txt", "a");
	AssetDatabase::ScanProjectDirectory();

	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	for (int frame = 0; frame < 2; ++frame) {
		panel.Get()->OnTick(0.016f);
		build.context.Update();
	}
	EditorPreviewStore::Drain();

	const Guid guid = AssetDatabase::LookupGuid(Path("project://Assets/a.txt"));
	AssetPreviewEntry held;
	ASSERT_TRUE(EditorPreviewStore::Peek(guid, held));

	WriteSource("project://Assets/b.txt", "b");
	AssetDatabase::ScanProjectDirectory();
	const u32 pendingBefore = EditorPreviewStore::GetPendingCount();
	AssetPreviewEntry missing;
	EXPECT_FALSE(EditorPreviewStore::Peek(AssetDatabase::LookupGuid(Path("project://Assets/b.txt")), missing));
	EXPECT_EQ(EditorPreviewStore::GetPendingCount(), pendingBefore);

	EditorPreviewStore::Request(AssetDatabase::LookupGuid(Path("project://Assets/b.txt")), missing);
	EditorPreviewStore::Drain();
	ASSERT_EQ(EditorPreviewStore::GetCachedCount(), 2u);
	for (u32 poll = 0; poll < 4; ++poll) {
		EXPECT_TRUE(EditorPreviewStore::Peek(guid, held));
	}

	WriteSource("project://Assets/c.txt", "c");
	AssetDatabase::ScanProjectDirectory();
	EditorPreviewStore::SetCapacity(2);
	EditorPreviewStore::Request(AssetDatabase::LookupGuid(Path("project://Assets/c.txt")), missing);
	EditorPreviewStore::Drain();

	AssetPreviewEntry survivor;
	EXPECT_FALSE(EditorPreviewStore::Peek(guid, survivor));
	EXPECT_TRUE(EditorPreviewStore::Peek(AssetDatabase::LookupGuid(Path("project://Assets/b.txt")), survivor));
}

namespace {

	class AssetBrowserGridDeviceTest : public AssetBrowserGridPreviewTest {
	protected:
		void SetUp() override {
			AssetBrowserGridPreviewTest::SetUp();
			WindowSystem::Initialize();
			RenderSystem::Initialize();
		}
		void TearDown() override {
			EditorPreviewStore::Terminate();
			RenderSystem::Terminate();
			WindowSystem::Terminate();
			AssetBrowserGridPreviewTest::TearDown();
		}
	};

} // anonymous namespace

TEST_F(AssetBrowserGridDeviceTest, EveryPictureAndBadgeCastsTileShadow) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	WriteSource("project://Assets/solid.txt", "s");
	WriteSource("project://Assets/clear.txt", "c");
	AssetDatabase::ScanProjectDirectory();
	const Guid solid = AssetDatabase::LookupGuid(Path("project://Assets/solid.txt"));
	ASSERT_TRUE(solid.IsValid());
	EditorPreviews::RegisterProducer(StringHash("TextAsset"), [solid](const AssetPreviewRequest& request,
																	  AssetPreviewImage& out) {
		static constexpr u8 SOLID[2 * 2 * 4] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
		static constexpr u8 CLEAR[2 * 2 * 4] = {255, 0, 0, 0, 0, 255, 0, 0, 0, 0, 255, 0, 255, 255, 255, 255};
		out.size = {2, 2};
		out.pixelsFormat = RHI::Format::RGBA8Unorm;
		out.pixels = request.guid == solid ? std::span<const u8>(SOLID) : std::span<const u8>(CLEAR);
		return true;
	});

	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	EditorTheme::ApplyToContext(build.context);
	BuildAndSettle(*panel.Get(), build);
	for (int frame = 0; frame < 4; ++frame) {
		panel.Get()->OnTick(0.016f);
		build.context.Update();
		EditorPreviewStore::Drain();
	}
	panel.Get()->OnTick(0.016f);
	build.context.Update();

	auto picture = [&](const char* name) {
		UI::ElementHandle cell = FindFileRow(build.context, build.root, name);
		UI::ElementHandle square = build.context.IsAlive(cell) ? FindByClass(build.context, cell, StringHash("ed-asset-cell-well"), 0) : UI::ElementHandle{};
		const UI::Element* element = build.context.IsAlive(square) ? build.context.GetElement(square) : nullptr;
		return build.context.MakeHandle(element ? element->firstChild : UI::INVALID_UI_INDEX);
	};
	UI::ElementHandle solidPicture = picture("solid.txt");
	UI::ElementHandle clearPicture = picture("clear.txt");
	ASSERT_TRUE(build.context.IsAlive(solidPicture) && build.context.IsAlive(clearPicture));
	ASSERT_EQ(build.context.GetStyle(solidPicture).display, UI::Display::Flex);
	ASSERT_EQ(build.context.GetStyle(clearPicture).display, UI::Display::Flex);
	const StringHash shadow("ed-asset-cell-picture");
	EXPECT_TRUE(build.context.HasClass(solidPicture, shadow));
	EXPECT_TRUE(build.context.HasClass(clearPicture, shadow));
	const UI::Element* solidElement = build.context.GetElement(solidPicture);
	ASSERT_NE(solidElement, nullptr);
	UI::ElementHandle badge = build.context.MakeHandle(solidElement->nextSibling);
	ASSERT_TRUE(build.context.IsAlive(badge));
	EXPECT_TRUE(build.context.HasClass(badge, shadow));

	EXPECT_GT(build.context.GetComputedStyle(clearPicture).dropShadow.color.a, 0.0f);
	EXPECT_EQ(build.context.GetComputedStyle(clearPicture).boxShadowCount, 0u);
}

TEST_F(AssetBrowserGridDeviceTest, SliderDragKeepsPreview) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	EditorPreviews::RegisterProducer(StringHash("TextAsset"),
									 [](const AssetPreviewRequest&, AssetPreviewImage& out) {
										 static constexpr u8 PIXELS[2 * 2 * 4] = {
											 255, 0, 0, 255, 0, 255, 0, 255,
											 0, 0, 255, 255, 255, 255, 255, 255};
										 out.size = {2, 2};
										 out.pixelsFormat = RHI::Format::RGBA8Unorm;
										 out.pixels = {PIXELS, sizeof(PIXELS)};
										 return true;
									 });
	WriteSource("project://Assets/a.txt", "a");
	AssetDatabase::ScanProjectDirectory();

	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	for (int frame = 0; frame < 4; ++frame) {
		panel.Get()->OnTick(0.016f);
		build.context.Update();
		EditorPreviewStore::Drain();
	}
	panel.Get()->OnTick(0.016f);
	build.context.Update();

	auto pictureShown = [&]() {
		UI::ElementHandle cell = FindFileRow(build.context, build.root, "a.txt");
		EXPECT_TRUE(cell.IsValid());
		const UI::Element* element = build.context.GetElement(cell);
		UI::ElementHandle image = build.context.MakeHandle(element ? element->firstChild : UI::INVALID_UI_INDEX);
		return build.context.IsAlive(image) && build.context.GetStyle(image).display == UI::Display::Flex;
	};
	ASSERT_TRUE(pictureShown());

	UI::ElementHandle track = FindClassed(build.context, build.root, StringHash("slt-slider-track"));
	ASSERT_TRUE(build.context.IsAlive(track));
	const UI::Rect& box = build.context.GetLayout(track).content;
	ASSERT_GT(box.w, 0.0f);
	ClickAt(build.context, *panel.Get(), {box.x + box.w * 0.5f, box.y + box.h * 0.5f});

	EXPECT_TRUE(pictureShown());
}

TEST_F(AssetBrowserGridDeviceTest, EditedAssetKeepsOldCellPicture) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	EditorPreviews::RegisterProducer(StringHash("TextAsset"),
									 [](const AssetPreviewRequest&, AssetPreviewImage& out) {
										 static constexpr u8 PIXELS[2 * 2 * 4] = {
											 255, 0, 0, 255, 0, 255, 0, 255,
											 0, 0, 255, 255, 255, 255, 255, 255};
										 out.size = {2, 2};
										 out.pixelsFormat = RHI::Format::RGBA8Unorm;
										 out.pixels = {PIXELS, sizeof(PIXELS)};
										 return true;
									 });
	WriteSource("project://Assets/a.txt", "a");
	AssetDatabase::ScanProjectDirectory();
	const Guid guid = AssetDatabase::LookupGuid(Path("project://Assets/a.txt"));
	ASSERT_TRUE(guid.IsValid());

	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	for (int frame = 0; frame < 4; ++frame) {
		panel.Get()->OnTick(0.016f);
		build.context.Update();
		EditorPreviewStore::Drain();
	}
	panel.Get()->OnTick(0.016f);
	build.context.Update();

	auto picture = [&]() {
		UI::ElementHandle cell = FindFileRow(build.context, build.root, "a.txt");
		UI::ElementHandle square = build.context.IsAlive(cell) ? FindByClass(build.context, cell, StringHash("ed-asset-cell-well"), 0) : UI::ElementHandle{};
		const UI::Element* element = build.context.IsAlive(square) ? build.context.GetElement(square) : nullptr;
		return build.context.MakeHandle(element ? element->firstChild : UI::INVALID_UI_INDEX);
	};
	auto pictureShown = [&]() {
		UI::ElementHandle image = picture();
		return build.context.IsAlive(image) && build.context.GetStyle(image).display == UI::Display::Flex;
	};
	ASSERT_TRUE(pictureShown());
	const RHI::TextureHandle before = build.context.GetStyle(picture()).background.image;

	EditorPreviewStore::Invalidate(guid);
	for (int frame = 0; frame < 3; ++frame) {
		panel.Get()->OnTick(0.016f);
		build.context.Update();
		EXPECT_TRUE(pictureShown());
	}
	EditorPreviewStore::Drain();
	panel.Get()->OnTick(0.016f);
	build.context.Update();
	EXPECT_TRUE(pictureShown());
	EXPECT_NE(build.context.GetStyle(picture()).background.image, before);
}

TEST_F(AssetBrowserPanelTest, SideGapRightClickOpensMenu) {
	WriteSource("project://Assets/a.txt", "a");
	AssetDatabase::ScanProjectDirectory();
	browserIconSize = 0.5f;
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	panel.Get()->OnTick(0.016f);
	build.context.Update();

	UI::ElementHandle cell = FindFileRow(build.context, build.root, "a.txt");
	ASSERT_TRUE(cell.IsValid());
	UI::ElementHandle viewport = FindScrollRootHoldingCells(build.context, build.root);
	ASSERT_TRUE(build.context.IsAlive(viewport));

	const UI::Rect& cellBox = build.context.GetLayout(cell).borderBox;
	const UI::Rect& window = build.context.GetLayout(viewport).content;
	const f32 gapLeft = cellBox.x + cellBox.w;
	const f32 gapRight = window.x + window.w;
	ASSERT_GT(gapRight - gapLeft, 8.0f);

	RightClickAt(build.context, *panel.Get(),
				 {(gapLeft + gapRight) * 0.5f, cellBox.y + cellBox.h * 0.5f});

	ASSERT_EQ(build.context.GetOpenPopupCount(), 1u);
	EXPECT_TRUE(MenuRowNamed(build.context, build.context.GetOpenPopup(0), "New Folder").IsValid());
}

TEST_F(AssetBrowserPanelTest, CellResizeKeepsColumnsSameFrame) {
	for (u32 i = 0; i < 24; ++i) {
		WriteSource("project://Assets/f" + std::to_string(i) + ".txt", "x");
	}
	AssetDatabase::ScanProjectDirectory();
	browserIconSize = 0.5f;
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	for (int frame = 0; frame < 3; ++frame) {
		panel.Get()->OnTick(0.016f);
		build.context.Update();
	}

	auto topRow = [&](u32& columns, f32& cellWidth) {
		Vector<UI::Rect> rects{GetMemoryResource(MemTag::Editor)};
		CollectCellRects(build.context, build.root, rects);
		columns = 0;
		cellWidth = 0.0f;
		for (const UI::Rect& rect : rects) {
			if (!rects.empty() && rect.y == rects[0].y) {
				++columns;
				cellWidth = rect.w;
			}
		}
	};

	u32 columnsBefore = 0;
	f32 widthBefore = 0.0f;
	topRow(columnsBefore, widthBefore);
	ASSERT_GT(columnsBefore, 1u);

	UI::ElementHandle track = FindClassed(build.context, build.root, StringHash("slt-slider-track"));
	ASSERT_TRUE(build.context.IsAlive(track));
	const UI::Rect& box = build.context.GetLayout(track).content;
	ASSERT_GT(box.w, 0.0f);
	ClickAt(build.context, *panel.Get(), {box.x + box.w * 0.5f + 1.0f, box.y + box.h * 0.5f});

	u32 columnsAfter = 0;
	f32 widthAfter = 0.0f;
	topRow(columnsAfter, widthAfter);
	ASSERT_NE(widthAfter, widthBefore);
	EXPECT_GT(columnsAfter, 1u);
	UI::ElementHandle viewport = FindScrollRootHoldingCells(build.context, build.root);
	ASSERT_TRUE(build.context.IsAlive(viewport));
	f32 paneWidthLogical = build.context.GetLayout(viewport).content.w / build.context.GetContentScale();
	EXPECT_EQ(columnsAfter, AssetBrowserOps::GridColumnsFor(paneWidthLogical, widthAfter / build.context.GetContentScale()));
}

TEST_F(AssetBrowserPanelTest, GridDrawsColumnsOnFirstFrame) {
	for (u32 i = 0; i < 24; ++i) {
		WriteSource("project://Assets/f" + std::to_string(i) + ".txt", "x");
	}
	AssetDatabase::ScanProjectDirectory();
	browserIconSize = 0.0f;
	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	for (int frame = 0; frame < 3; ++frame) {
		panel.Get()->OnTick(0.016f);
		build.context.Update();
	}
	ASSERT_TRUE(build.context.HasClass(FindFileRow(build.context, build.root, "f0.txt"),
									   StringHash("slt-tree-row")));

	UI::ElementHandle track = FindClassed(build.context, build.root, StringHash("slt-slider-track"));
	ASSERT_TRUE(build.context.IsAlive(track));
	const UI::Rect& box = build.context.GetLayout(track).content;
	ASSERT_GT(box.w, 0.0f);
	ClickAt(build.context, *panel.Get(), {box.x + box.w * 0.25f, box.y + box.h * 0.5f});

	Vector<UI::Rect> rects{GetMemoryResource(MemTag::Editor)};
	CollectCellRects(build.context, build.root, rects);
	ASSERT_FALSE(rects.empty());
	u32 columns = 0;
	for (const UI::Rect& rect : rects) {
		if (rect.y == rects[0].y) {
			++columns;
		}
	}
	EXPECT_GT(columns, 1u);
}

namespace {

	class SubAssetProbeImporter : public IAssetImporter {
	public:
		Guid textureGuid;

		StringHash GetAssetType() const override { return StringHash("TextAsset"); }

		Asset* Import(const Path& sourcePath) override {
			TextAsset* asset = New<TextAsset>(MemTag::Asset);
			asset->Import(sourcePath);
			return asset;
		}

		Asset* ImportWithSettings(const Path& sourcePath, const Path& virtualPath, const JSONValue& settings,
								  AssetImportContext& context) override {
			(void)virtualPath;
			(void)settings;
			str text = FileSystem::ReadTextFile(sourcePath, MemTag::Core);
			size_t cursor = 0;
			while (cursor < text.size()) {
				size_t lineEnd = text.find('\n', cursor);
				str key(text.substr(cursor, (lineEnd == str::npos ? text.size() : lineEnd) - cursor));
				cursor = (lineEnd == str::npos) ? text.size() : lineEnd + 1;
				while (!key.empty() && (key.back() == '\r' || key.back() == ' ')) {
					key.pop_back();
				}
				if (key.empty()) {
					continue;
				}
				if (key.starts_with("Materials/")) {
					MaterialAsset* material = New<MaterialAsset>(MemTag::Asset);
					if (textureGuid.IsValid()) {
						material->SetTexture("baseColor", textureGuid);
					}
					context.AddProduct(key, material);
				} else {
					TextAsset* mesh = New<TextAsset>(MemTag::Asset);
					mesh->content = key;
					context.AddProduct(key, mesh);
				}
			}
			return Import(sourcePath);
		}
	};

	class AssetBrowserSubAssetTest : public AssetBrowserPanelTest {
	protected:
		SubAssetProbeImporter _importer;

		void SetUp() override {
			AssetBrowserPanelTest::SetUp();
			MaterialAsset::GetReflectType();
			AssetImporterRegistry::Register(StringHash("SubProbeAsset"), &_importer, {".sub"});
		}
		void TearDown() override {
			AssetImporterRegistry::Unregister(&_importer);
			AssetBrowserPanelTest::TearDown();
		}

		void ImportSource(const str& virtualPath) {
			Asset* asset = AssetDatabase::LoadAsset(Path(virtualPath));
			ASSERT_NE(asset, nullptr) << virtualPath;
			AssetDatabase::UnloadAsset(asset);
			AssetDatabase::CollectGarbage();
		}

		static void CollectAll(UI::Context& context, UI::ElementHandle from, Vector<UI::ElementHandle>& out) {
			out.push_back(from);
			const UI::Element* element = context.GetElement(from);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				CollectAll(context, handle, out);
				child = context.GetElement(handle)->nextSibling;
			}
		}

		static UI::ElementHandle RowNamed(UI::Context& context, UI::ElementHandle from, const str& label) {
			if (context.HasClass(from, StringHash("slt-tree-row")) && HasText(context, from, label)) {
				return from;
			}
			const UI::Element* element = context.GetElement(from);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = context.MakeHandle(child);
				UI::ElementHandle found = RowNamed(context, handle, label);
				if (found.IsValid()) {
					return found;
				}
				child = context.GetElement(handle)->nextSibling;
			}
			return {};
		}
	};

} // namespace

TEST_F(AssetBrowserSubAssetTest, ScanProductsOneRowEach) {
	WriteSource("project://Assets/rig.sub", "Meshes/Body\nMaterials/Skin\n");
	ImportSource("project://Assets/rig.sub");

	Vector<AssetBrowserOps::Entry> rows{GetMemoryResource(MemTag::Editor)};
	AssetBrowserOps::ScanProducts("Assets/rig.sub", 1, rows);

	ASSERT_EQ(rows.size(), 2u);
	EXPECT_EQ(rows[0].kind, AssetBrowserOps::EntryKind::Product);
	EXPECT_EQ(rows[0].name, "Body");
	EXPECT_EQ(rows[0].productKey, "Meshes/Body");
	EXPECT_EQ(rows[0].sourceRelPath, "Assets/rig.sub");
	EXPECT_EQ(rows[0].depth, 2u);
	EXPECT_TRUE(rows[0].relPath.empty());
	EXPECT_TRUE(rows[0].guid.IsValid());
	EXPECT_NE(rows[0].id, rows[1].id);
	EXPECT_EQ(rows[1].name, "Skin");

	WriteSource("project://Assets/plain.txt", "no products");
	AssetDatabase::ScanProjectDirectory();
	AssetBrowserOps::ScanProducts("Assets/plain.txt", 0, rows);
	EXPECT_TRUE(rows.empty());
}

TEST_F(AssetBrowserSubAssetTest, ScanLinksAnswersProjectFilesProductReferences) {
	WriteSource("project://Assets/Textures/brick.txt", "pretend png");
	AssetDatabase::ScanProjectDirectory();
	_importer.textureGuid = AssetDatabase::LookupGuid(Path("project://Assets/Textures/brick.txt"));
	ASSERT_TRUE(_importer.textureGuid.IsValid());

	WriteSource("project://Assets/rig.sub", "Meshes/Body\nMaterials/Skin\nMaterials/Trim\n");
	ImportSource("project://Assets/rig.sub");

	Vector<AssetBrowserOps::Entry> links{GetMemoryResource(MemTag::Editor)};
	AssetBrowserOps::ScanLinks("Assets/rig.sub", 1, links);

	ASSERT_EQ(links.size(), 1u);
	EXPECT_EQ(links[0].kind, AssetBrowserOps::EntryKind::Link);
	EXPECT_EQ(links[0].relPath, "Assets/Textures/brick.txt");
	EXPECT_EQ(links[0].guid, _importer.textureGuid);
	EXPECT_EQ(links[0].sourceRelPath, "Assets/rig.sub");
	EXPECT_NE(links[0].id, AssetBrowserOps::EntryKeyForPath("Assets/Textures/brick.txt"));
}

TEST_F(AssetBrowserSubAssetTest, ScanLinksEmptyWithoutReferences) {
	WriteSource("project://Assets/rig.sub", "Materials/Skin\n");
	ImportSource("project://Assets/rig.sub");

	Vector<AssetBrowserOps::Entry> links{GetMemoryResource(MemTag::Editor)};
	AssetBrowserOps::ScanLinks("Assets/rig.sub", 1, links);
	EXPECT_TRUE(links.empty());
}

TEST_F(AssetBrowserSubAssetTest, RefusalsNameSourceAndNextAction) {
	WriteSource("project://Assets/rig.sub", "Meshes/Body\nMaterials/Skin\n");
	ImportSource("project://Assets/rig.sub");
	Vector<AssetBrowserOps::Entry> rows{GetMemoryResource(MemTag::Editor)};
	AssetBrowserOps::ScanProducts("Assets/rig.sub", 0, rows);
	ASSERT_EQ(rows.size(), 2u);
	const AssetBrowserOps::Entry& mesh = rows[0];
	const AssetBrowserOps::Entry& material = rows[1];

	for (AssetBrowserOps::EntryCommand command : {AssetBrowserOps::EntryCommand::Rename,
												  AssetBrowserOps::EntryCommand::Delete,
												  AssetBrowserOps::EntryCommand::Move,
												  AssetBrowserOps::EntryCommand::Duplicate}) {
		str refusal = AssetBrowserOps::RefusalFor(mesh, command);
		ASSERT_FALSE(refusal.empty());
		EXPECT_NE(refusal.find("rig.sub"), str::npos) << refusal;
		EXPECT_NE(refusal.find("Extract"), str::npos) << refusal;
	}

	EXPECT_FALSE(AssetBrowserOps::RefusalFor(mesh, AssetBrowserOps::EntryCommand::Extract).empty());
	EXPECT_TRUE(AssetBrowserOps::RefusalFor(material, AssetBrowserOps::EntryCommand::Extract).empty());

	AssetBrowserOps::Entry file;
	file.kind = AssetBrowserOps::EntryKind::File;
	file.name = "plain.txt";
	file.relPath = "Assets/plain.txt";
	EXPECT_TRUE(AssetBrowserOps::RefusalFor(file, AssetBrowserOps::EntryCommand::Delete).empty());
	EXPECT_FALSE(AssetBrowserOps::RefusalFor(file, AssetBrowserOps::EntryCommand::Extract).empty());
}

TEST_F(AssetBrowserSubAssetTest, LinkRefusesCommandsNamesFile) {
	WriteSource("project://Assets/Textures/brick.txt", "pretend png");
	AssetDatabase::ScanProjectDirectory();
	_importer.textureGuid = AssetDatabase::LookupGuid(Path("project://Assets/Textures/brick.txt"));
	WriteSource("project://Assets/rig.sub", "Materials/Skin\n");
	ImportSource("project://Assets/rig.sub");

	Vector<AssetBrowserOps::Entry> links{GetMemoryResource(MemTag::Editor)};
	AssetBrowserOps::ScanLinks("Assets/rig.sub", 0, links);
	ASSERT_EQ(links.size(), 1u);

	for (AssetBrowserOps::EntryCommand command : {AssetBrowserOps::EntryCommand::Rename,
												  AssetBrowserOps::EntryCommand::Delete,
												  AssetBrowserOps::EntryCommand::Move,
												  AssetBrowserOps::EntryCommand::Duplicate,
												  AssetBrowserOps::EntryCommand::Extract}) {
		EXPECT_FALSE(AssetBrowserOps::RefusalFor(links[0], command).empty());
	}
	str refusal = AssetBrowserOps::RefusalFor(links[0], AssetBrowserOps::EntryCommand::Delete);
	EXPECT_NE(refusal.find("Assets/Textures"), str::npos) << refusal;
}

TEST_F(AssetBrowserSubAssetTest, ExtractWritesIntoFolderMigrationReads) {
	WriteSource("project://Assets/rig.sub", "Materials/Skin\n");
	ImportSource("project://Assets/rig.sub");
	Vector<AssetBrowserOps::Entry> rows{GetMemoryResource(MemTag::Editor)};
	AssetBrowserOps::ScanProducts("Assets/rig.sub", 0, rows);
	ASSERT_EQ(rows.size(), 1u);
	Guid before = rows[0].guid;

	str created = AssetBrowserOps::ExtractProduct(rows[0]);
	ASSERT_EQ(created, "Assets/rig.sub.materials/Skin.mat");
	EXPECT_EQ(AssetDatabase::LookupGuid(Path("project://Assets/rig.sub.materials/Skin.mat")), before);

	AssetBrowserOps::ScanProducts("Assets/rig.sub", 0, rows);
	ASSERT_EQ(rows.size(), 1u);
	EXPECT_EQ(rows[0].extractedRelPath, "Assets/rig.sub.materials/Skin.mat");
	EXPECT_FALSE(AssetBrowserOps::RefusalFor(rows[0], AssetBrowserOps::EntryCommand::Extract).empty());
	str onExtracted = AssetBrowserOps::RefusalFor(rows[0], AssetBrowserOps::EntryCommand::Delete);
	EXPECT_FALSE(onExtracted.empty());
	EXPECT_NE(onExtracted.find("Assets/rig.sub.materials/Skin.mat"), str::npos) << onExtracted;
}

TEST_F(AssetBrowserSubAssetTest, ExpandingModelRowShowsProductsBeneathIt) {
	WriteSource("project://Assets/rig.sub", "Meshes/Body\nMeshes/Visor\n");
	ImportSource("project://Assets/rig.sub");

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	UI::Context& context = build.context;
	UI::ElementHandle sourceRow = RowNamed(context, build.root, "rig.sub");
	ASSERT_TRUE(sourceRow.IsValid());
	EXPECT_FALSE(RowNamed(context, build.root, "Body").IsValid());

	UI::ElementHandle arrow = FindClassed(context, sourceRow, StringHash("slt-tree-arrow"));
	ASSERT_TRUE(arrow.IsValid());
	ClickAt(context, *panel.Get(), CenterOf(context, arrow));
	context.Update();
	panel.Get()->OnTick(0.016f);
	context.Update();

	EXPECT_TRUE(RowNamed(context, build.root, "Body").IsValid());
	EXPECT_TRUE(RowNamed(context, build.root, "Visor").IsValid());

	sourceRow = RowNamed(context, build.root, "rig.sub");
	ASSERT_TRUE(sourceRow.IsValid());
	arrow = FindClassed(context, sourceRow, StringHash("slt-tree-arrow"));
	ASSERT_TRUE(arrow.IsValid());
	ClickAt(context, *panel.Get(), CenterOf(context, arrow));
	context.Update();
	panel.Get()->OnTick(0.016f);
	context.Update();
	EXPECT_FALSE(RowNamed(context, build.root, "Body").IsValid());
}

TEST_F(AssetBrowserSubAssetTest, FileThatProducesNothingHasNoDisclosure) {
	WriteSource("project://Assets/plain.txt", "no products");
	AssetDatabase::ScanProjectDirectory();

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	UI::Context& context = build.context;
	UI::ElementHandle row = RowNamed(context, build.root, "plain.txt");
	ASSERT_TRUE(row.IsValid());
	UI::ElementHandle arrow = FindClassed(context, row, StringHash("slt-tree-arrow"));
	if (arrow.IsValid()) {
		EXPECT_EQ(context.Events(arrow).onClick, nullptr);
	}
}

TEST_F(AssetBrowserSubAssetTest, SelectingAProductRowPublishesItsGuid) {
	WriteSource("project://Assets/rig.sub", "Meshes/Body\n");
	ImportSource("project://Assets/rig.sub");
	Guid body;
	{
		Vector<AssetBrowserOps::Entry> rows{GetMemoryResource(MemTag::Editor)};
		AssetBrowserOps::ScanProducts("Assets/rig.sub", 0, rows);
		ASSERT_EQ(rows.size(), 1u);
		body = rows[0].guid;
	}

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	UI::Context& context = build.context;

	UI::ElementHandle sourceRow = RowNamed(context, build.root, "rig.sub");
	ASSERT_TRUE(sourceRow.IsValid());
	UI::ElementHandle arrow = FindClassed(context, sourceRow, StringHash("slt-tree-arrow"));
	ASSERT_TRUE(arrow.IsValid());
	ClickAt(context, *panel.Get(), CenterOf(context, arrow));
	context.Update();
	panel.Get()->OnTick(0.016f);
	context.Update();

	UI::ElementHandle productRow = RowNamed(context, build.root, "Body");
	ASSERT_TRUE(productRow.IsValid());
	ClickAt(context, *panel.Get(), CenterOf(context, productRow));
	context.Update();

	EXPECT_EQ(EditorSelection::GetSelectedAsset(), body);
	EXPECT_TRUE(RowIsSelected(context, RowNamed(context, build.root, "Body")));
}

TEST_F(AssetBrowserSubAssetTest, HandlePickerOffersProducts) {
	WriteSource("project://Assets/rig.sub", "Materials/Skin\n");
	ImportSource("project://Assets/rig.sub");
	Vector<AssetBrowserOps::Entry> rows{GetMemoryResource(MemTag::Editor)};
	AssetBrowserOps::ScanProducts("Assets/rig.sub", 0, rows);
	ASSERT_EQ(rows.size(), 1u);
	ASSERT_TRUE(AssetDatabase::LookupPath(rows[0].guid).ToString().empty());

	Handle<Scene> scene = SceneSystem::CreateScene("PickerFixture");
	Handle<SceneObject> object = SceneSystem::CreateObject("Renderer", scene);
	ASSERT_NE(object.Get()->AddComponent<MeshRenderer>().Get(), nullptr);
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> inspector =
		PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("InspectorPanel")));
	ASSERT_TRUE(inspector.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*inspector.Get(), build);

	Vector<UI::ElementHandle> all{GetMemoryResource(MemTag::Editor)};
	CollectAll(build.context, build.root, all);
	UI::DragPayload probe;
	probe.type = "Component"_hash;
	Asset* product = AssetDatabase::LoadAssetByGuid(rows[0].guid);
	ASSERT_NE(product, nullptr);
	probe.data = Variant(product->GetInstanceId());
	UI::ElementHandle field;
	for (UI::ElementHandle element : all) {
		if (build.context.HasClass(element, EditorTheme::Class::ObjectField) &&
			build.context.Events(element).onDragOver && build.context.Events(element).onDragOver(probe)) {
			field = element;
			break;
		}
	}
	AssetDatabase::UnloadAsset(product);
	ASSERT_TRUE(field.IsValid());

	UI::ElementHandle button = FindByClass(build.context, field, EditorTheme::Class::ObjectFieldButton, 0);
	ASSERT_TRUE(button.IsValid());
	ClickElement(build.context, button);
	build.context.Update();

	EXPECT_TRUE(HasText(build.context, build.context.GetRoot(), "Assets/rig.sub : Materials/Skin"));
	EXPECT_FALSE(HasText(build.context, build.context.GetRoot(), "(no candidates)"));

	SceneSystem::UnloadScene(scene);
	SceneSystem::EndFrame();
}

namespace {

	class AssetBrowserBuiltInPickerTest : public AssetBrowserSubAssetTest {
	protected:
		void SetUp() override {
			std::filesystem::path engineAssets = CookHarness::FindRepoDir("EngineAssets");
			if (engineAssets.empty()) {
				GTEST_SKIP() << "EngineAssets not found";
			}
			FileSystem::Mount("engine", Path(engineAssets.generic_string()));
			_engineMounted = true;
			AssetBrowserSubAssetTest::SetUp();
			MeshAsset::GetReflectType();
			MeshRenderer::GetReflectType();
		}
		void TearDown() override {
			if (_engineMounted) {
				AssetBrowserSubAssetTest::TearDown();
				FileSystem::Unmount("engine");
			}
		}

		bool OpenPickerFor(PanelBuild& build, Asset* payloadAsset) {
			Vector<UI::ElementHandle> all{GetMemoryResource(MemTag::Editor)};
			CollectAll(build.context, build.root, all);
			UI::DragPayload probe;
			probe.type = "Component"_hash;
			probe.data = Variant(payloadAsset->GetInstanceId());
			for (UI::ElementHandle element : all) {
				if (build.context.HasClass(element, EditorTheme::Class::ObjectField) &&
					build.context.Events(element).onDragOver && build.context.Events(element).onDragOver(probe)) {
					UI::ElementHandle button = FindByClass(build.context, element, EditorTheme::Class::ObjectFieldButton, 0);
					if (!button.IsValid()) {
						return false;
					}
					ClickElement(build.context, button);
					build.context.Update();
					return true;
				}
			}
			return false;
		}

		bool _engineMounted = false;
	};

} // namespace

TEST_F(AssetBrowserBuiltInPickerTest, HandlePickerAssignsBuiltIns) {
	const Guid cube = AssetDatabase::LookupGuid(Path("engine://Meshes/Cube.primitive"));
	const Guid sphere = AssetDatabase::LookupGuid(Path("engine://Meshes/Sphere.primitive"));
	ASSERT_TRUE(cube.IsValid());
	ASSERT_TRUE(sphere.IsValid());

	Handle<Scene> scene = SceneSystem::CreateScene("PickerFixture");
	Handle<SceneObject> object = SceneSystem::CreateObject("Renderer", scene);
	Handle<MeshRenderer> renderer = object.Get()->AddComponent<MeshRenderer>();
	ASSERT_NE(renderer.Get(), nullptr);
	Asset* cubeMesh = AssetDatabase::LoadAssetByGuid(cube);
	ASSERT_NE(cubeMesh, nullptr);
	renderer->SetMesh(Handle<MeshAsset>(static_cast<MeshAsset*>(cubeMesh)));
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> inspector = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("InspectorPanel")));
	ASSERT_TRUE(inspector.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*inspector.Get(), build);
	ASSERT_TRUE(OpenPickerFor(build, cubeMesh));

	EXPECT_FALSE(HasText(build.context, build.context.GetRoot(), "(no candidates)"));
	UI::ElementHandle sphereRow = FindByText(build.context, build.context.GetRoot(), "engine://Meshes/Sphere.primitive");
	ASSERT_TRUE(sphereRow.IsValid());

	ClickElement(build.context, sphereRow);
	build.context.Update();
	MeshAsset* assigned = renderer->GetMesh().Get();
	ASSERT_NE(assigned, nullptr);
	EXPECT_EQ(assigned->assetId, sphere);

	SceneSystem::UnloadScene(scene);
	SceneSystem::EndFrame();
}

TEST_F(AssetBrowserBuiltInPickerTest, ProjectAssetsBeforeBuiltIns) {
	WriteSource("project://Assets/rig.sub", "Materials/Skin\n");
	ImportSource("project://Assets/rig.sub");

	Vector<AssetBrowserOps::AssignableAsset> offered{GetMemoryResource(MemTag::Editor)};
	AssetBrowserOps::CollectAssignableAssets(StringHash("MaterialAsset"), offered);
	size_t project = offered.size();
	size_t builtIn = offered.size();
	for (size_t i = 0; i < offered.size(); ++i) {
		if (offered[i].display == "Assets/rig.sub : Materials/Skin") {
			project = i;
		}
		if (offered[i].display == "engine://Materials/Default.mat") {
			builtIn = i;
		}
	}
	ASSERT_LT(project, offered.size());
	ASSERT_LT(builtIn, offered.size());
	EXPECT_LT(project, builtIn);
}

TEST_F(AssetBrowserSubAssetTest, ASelectedProductSurvivesARescan) {
	WriteSource("project://Assets/rig.sub", "Meshes/Body\n");
	ImportSource("project://Assets/rig.sub");

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	UI::Context& context = build.context;

	UI::ElementHandle sourceRow = RowNamed(context, build.root, "rig.sub");
	ASSERT_TRUE(sourceRow.IsValid());
	ClickAt(context, *panel.Get(), CenterOf(context, FindClassed(context, sourceRow, StringHash("slt-tree-arrow"))));
	context.Update();
	panel.Get()->OnTick(0.016f);
	context.Update();
	UI::ElementHandle productRow = RowNamed(context, build.root, "Body");
	ASSERT_TRUE(productRow.IsValid());
	ClickAt(context, *panel.Get(), CenterOf(context, productRow));
	context.Update();
	Guid body = EditorSelection::GetSelectedAsset();
	ASSERT_TRUE(body.IsValid());

	EditorAssetWatch::Initialize();
	EditorAssetWatch::PollChanges();
	WriteSource("project://Assets/arrival.txt", "anything at all");
	EditorAssetWatch::NotifyChanged(Path("project://Assets/arrival.txt"));
	EditorAssetWatch::PollChanges();
	ASSERT_FALSE(EditorAssetWatch::GetChangedThisFrame().empty());
	panel.Get()->OnTick(0.6f);
	context.Update();
	EditorAssetWatch::Terminate();

	BuildAndSettle(*panel.Get(), build);
	ASSERT_TRUE(RowNamed(build.context, build.root, "arrival.txt").IsValid());

	EXPECT_EQ(EditorSelection::GetSelectedAsset(), body);
	EXPECT_TRUE(RowIsSelected(build.context, RowNamed(build.context, build.root, "Body")));
}

TEST_F(AssetBrowserSubAssetTest, FileCommandSkipsLinkedFile) {
	WriteSource("project://Assets/Textures/brick.txt", "pretend png");
	WriteSource("project://Assets/plain.txt", "an ordinary file");
	AssetDatabase::ScanProjectDirectory();
	_importer.textureGuid = AssetDatabase::LookupGuid(Path("project://Assets/Textures/brick.txt"));
	ASSERT_TRUE(_importer.textureGuid.IsValid());
	WriteSource("project://Assets/rig.sub", "Materials/Skin\n");
	ImportSource("project://Assets/rig.sub");

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	UI::Context& context = build.context;

	UI::ElementHandle sourceRow = RowNamed(context, build.root, "rig.sub");
	ASSERT_TRUE(sourceRow.IsValid());
	ClickAt(context, *panel.Get(), CenterOf(context, FindClassed(context, sourceRow, StringHash("slt-tree-arrow"))));
	context.Update();
	panel.Get()->OnTick(0.016f);
	context.Update();

	UI::ElementHandle linkRow = RowNamed(context, build.root, "brick.txt");
	ASSERT_TRUE(linkRow.IsValid());
	ClickAt(context, *panel.Get(), CenterOf(context, linkRow));
	context.Update();
	ASSERT_EQ(EditorSelection::GetSelectedAsset(), _importer.textureGuid);

	auto textureFileCount = [&]() {
		u32 count = 0;
		std::error_code ec;
		for (const auto& item : std::filesystem::directory_iterator(
				 std::filesystem::path(FileSystem::Resolve(Path("project://Assets/Textures")).ToString()), ec)) {
			(void)item;
			++count;
		}
		return count;
	};
	const u32 before = textureFileCount();
	ASSERT_GT(before, 0u);

	ASSERT_TRUE(panel.Get()->OnCommand(PanelCommand::Duplicate));
	context.Update();
	panel.Get()->OnTick(0.016f);
	EXPECT_EQ(textureFileCount(), before);

	ASSERT_TRUE(panel.Get()->OnCommand(PanelCommand::Delete));
	context.Update();
	EXPECT_EQ(context.GetOpenPopupCount(), 0u);
	EXPECT_TRUE(FileSystem::Exists(FileSystem::Resolve(Path("project://Assets/Textures/brick.txt"))));

	UI::ElementHandle fileRow = RowNamed(context, build.root, "plain.txt");
	ASSERT_TRUE(fileRow.IsValid());
	ClickAt(context, *panel.Get(), CenterOf(context, fileRow));
	context.Update();
	panel.Get()->OnCommand(PanelCommand::Duplicate);
	context.Update();
	panel.Get()->OnTick(0.016f);
	EXPECT_TRUE(FileSystem::Exists(FileSystem::Resolve(Path("project://Assets/plain 1.txt"))));
}

TEST_F(AssetBrowserSubAssetTest, TheGridExpandsAModelToo) {
	browserIconSize = 0.5f;
	WriteSource("project://Assets/Textures/brick.txt", "pretend png");
	AssetDatabase::ScanProjectDirectory();
	_importer.textureGuid = AssetDatabase::LookupGuid(Path("project://Assets/Textures/brick.txt"));
	WriteSource("project://Assets/rig.sub", "Meshes/Body\nMaterials/Skin\n");
	WriteSource("project://Assets/zz.txt", "the file after the model");
	ImportSource("project://Assets/rig.sub");

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	UI::Context& context = build.context;

	UI::ElementHandle button = FindClassed(context, build.root, StringHash("ed-asset-fold-button"));
	ASSERT_TRUE(button.IsValid());
	ClickAt(context, *panel.Get(), CenterOf(context, button));
	context.Update();
	panel.Get()->OnTick(0.016f);
	context.Update();

	const char* order[] = {"rig.sub", "Body", "Skin", "brick.txt", "zz.txt"};
	UI::Rect previous{};
	for (const char* name : order) {
		UI::ElementHandle cell = FilePaneEntry(context, build.root, name);
		ASSERT_TRUE(context.IsAlive(cell)) << name;
		const UI::Rect box = context.GetLayout(cell).borderBox;
		if (name != order[0]) {
			EXPECT_TRUE(box.y > previous.y + 0.5f || (std::abs(box.y - previous.y) < 0.5f && box.x > previous.x)) << name;
		}
		previous = box;
	}

	button = FindClassed(context, build.root, StringHash("ed-asset-fold-button"));
	ASSERT_TRUE(button.IsValid());
	ClickAt(context, *panel.Get(), CenterOf(context, button));
	context.Update();
	panel.Get()->OnTick(0.016f);
	context.Update();
	EXPECT_FALSE(context.IsAlive(FilePaneEntry(context, build.root, "Body")));
}

TEST_F(AssetBrowserSubAssetTest, ScanLinksIgnoresForeignReference) {
	_importer.textureGuid = AssetDatabase::GenerateGuid();
	WriteSource("project://Assets/rig.sub", "Materials/Skin\n");
	ImportSource("project://Assets/rig.sub");

	Vector<AssetBrowserOps::Entry> links{GetMemoryResource(MemTag::Editor)};
	AssetBrowserOps::ScanLinks("Assets/rig.sub", 0, links);
	EXPECT_TRUE(links.empty());
}

TEST_F(AssetBrowserSubAssetTest, ExtractedProductIsNoLink) {
	WriteSource("project://Assets/rig.sub", "Materials/Skin\nMaterials/Trim\n");
	ImportSource("project://Assets/rig.sub");

	Vector<AssetBrowserOps::Entry> rows{GetMemoryResource(MemTag::Editor)};
	AssetBrowserOps::ScanProducts("Assets/rig.sub", 0, rows);
	ASSERT_EQ(rows.size(), 2u);
	ASSERT_FALSE(AssetBrowserOps::ExtractProduct(rows[0]).empty());
	ASSERT_FALSE(AssetDatabase::LookupPath(rows[0].guid).ToString().empty());

	_importer.textureGuid = rows[0].guid;
	ImportSource("project://Assets/rig.sub");

	Vector<AssetBrowserOps::Entry> links{GetMemoryResource(MemTag::Editor)};
	AssetBrowserOps::ScanLinks("Assets/rig.sub", 0, links);
	for (const AssetBrowserOps::Entry& link : links) {
		EXPECT_NE(link.guid, rows[0].guid);
	}
}

TEST_F(AssetBrowserSubAssetTest, InspectorNamesSourceAndKeyOfProduct) {
	WriteSource("project://Assets/rig.sub", "Meshes/Body\n");
	ImportSource("project://Assets/rig.sub");
	Vector<AssetBrowserOps::Entry> rows{GetMemoryResource(MemTag::Editor)};
	AssetBrowserOps::ScanProducts("Assets/rig.sub", 0, rows);
	ASSERT_EQ(rows.size(), 1u);

	Vector<Guid> selection{GetMemoryResource(MemTag::Editor)};
	selection.push_back(rows[0].guid);
	EditorSelection::SelectAssets(selection);

	Handle<EditorPanel> inspector =
		PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("InspectorPanel")));
	ASSERT_TRUE(inspector.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*inspector.Get(), build);

	EXPECT_TRUE(HasText(build.context, build.root, "project://Assets/rig.sub"));
	EXPECT_TRUE(HasText(build.context, build.root, "Meshes/Body"));
	EXPECT_FALSE(HasText(build.context, build.root, "(not in database)"));
}

TEST_F(AssetBrowserSubAssetTest, InspectorReportsVanishedProduct) {
	WriteSource("project://Assets/rig.sub", "Meshes/Body\n");
	ImportSource("project://Assets/rig.sub");
	Vector<AssetBrowserOps::Entry> rows{GetMemoryResource(MemTag::Editor)};
	AssetBrowserOps::ScanProducts("Assets/rig.sub", 0, rows);
	ASSERT_EQ(rows.size(), 1u);
	Guid held = rows[0].guid;

	WriteSource("project://Assets/rig.sub", "Meshes/Torso\n");
	ImportSource("project://Assets/rig.sub");
	AssetBrowserOps::ScanProducts("Assets/rig.sub", 0, rows);
	ASSERT_EQ(rows.size(), 2u);

	Vector<Guid> selection{GetMemoryResource(MemTag::Editor)};
	selection.push_back(held);
	EditorSelection::SelectAssets(selection);

	Handle<EditorPanel> inspector =
		PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("InspectorPanel")));
	ASSERT_TRUE(inspector.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*inspector.Get(), build);

	EXPECT_TRUE(HasText(build.context, build.root, "no longer in 'rig.sub'"));
}

TEST_F(AssetBrowserSubAssetTest, HandleFieldNamesProduct) {
	WriteSource("project://Assets/rig.sub", "Materials/Skin\n");
	ImportSource("project://Assets/rig.sub");
	Vector<AssetBrowserOps::Entry> rows{GetMemoryResource(MemTag::Editor)};
	AssetBrowserOps::ScanProducts("Assets/rig.sub", 0, rows);
	ASSERT_EQ(rows.size(), 1u);

	Asset* product = AssetDatabase::LoadAssetByGuid(rows[0].guid);
	ASSERT_NE(product, nullptr);
	ASSERT_TRUE(AssetDatabase::LookupPath(product->assetId).ToString().empty());

	Handle<Scene> scene = SceneSystem::CreateScene("CaptionFixture");
	Handle<SceneObject> object = SceneSystem::CreateObject("Renderer", scene);
	ASSERT_NE(object.Get()->AddComponent<MeshRenderer>().Get(), nullptr);
	EditorSelection::Select(Handle<Object>(object.Get()));

	Handle<EditorPanel> inspector =
		PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("InspectorPanel")));
	ASSERT_TRUE(inspector.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*inspector.Get(), build);

	Vector<UI::ElementHandle> all{GetMemoryResource(MemTag::Editor)};
	CollectAll(build.context, build.root, all);
	bool dropped = false;
	UI::DragPayload payload;
	payload.type = "Component"_hash;
	payload.data = Variant(product->GetInstanceId());
	for (UI::ElementHandle element : all) {
		if (!build.context.HasClass(element, EditorTheme::Class::ObjectField)) {
			continue;
		}
		UI::ElementEvents& events = build.context.Events(element);
		if (events.onDrop && events.onDragOver && events.onDragOver(payload)) {
			events.onDrop(payload, {});
			dropped = true;
			break;
		}
	}
	ASSERT_TRUE(dropped);

	inspector.Get()->OnTick(0.016f);
	build.context.Update();
	inspector.Get()->OnTick(0.016f);
	build.context.Update();

	EXPECT_TRUE(HasText(build.context, build.root, "Materials/Skin"));
	EXPECT_FALSE(HasText(build.context, build.root, "MaterialAsset"));

	AssetDatabase::UnloadAsset(product);
	SceneSystem::UnloadScene(scene);
	SceneSystem::EndFrame();
}

TEST_F(AssetBrowserSubAssetTest, ModelGrowsArrowWhenGainingProducts) {
	WriteSource("project://Assets/rig.sub", "Meshes/Body\n");
	AssetDatabase::ScanProjectDirectory();

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	UI::Context& context = build.context;
	{
		UI::ElementHandle row = RowNamed(context, build.root, "rig.sub");
		ASSERT_TRUE(row.IsValid());
		UI::ElementHandle arrow = FindClassed(context, row, StringHash("slt-tree-arrow"));
		if (arrow.IsValid()) {
			EXPECT_EQ(context.Events(arrow).onClick, nullptr);
		}
	}

	ImportSource("project://Assets/rig.sub");
	{
		Vector<AssetBrowserOps::Entry> after{GetMemoryResource(MemTag::Editor)};
		AssetBrowserOps::ScanProducts("Assets/rig.sub", 0, after);
		ASSERT_FALSE(after.empty());
	}

	panel.Get()->OnTick(0.6f);
	context.Update();

	UI::ElementHandle row = RowNamed(context, build.root, "rig.sub");
	ASSERT_TRUE(row.IsValid());
	UI::ElementHandle arrow = FindClassed(context, row, StringHash("slt-tree-arrow"));
	ASSERT_TRUE(arrow.IsValid());
	ASSERT_NE(context.Events(arrow).onClick, nullptr);

	ClickAt(context, *panel.Get(), CenterOf(context, arrow));
	context.Update();
	panel.Get()->OnTick(0.016f);
	context.Update();
	EXPECT_TRUE(RowNamed(context, build.root, "Body").IsValid());
}

TEST_F(AssetBrowserPanelTest, PingNavigatesAndMarksRow) {
	WriteSource("project://Assets/Textures/tex.txt", "t");
	WriteSource("project://Assets/readme.txt", "r");
	AssetDatabase::ScanProjectDirectory();
	Guid guid = AssetDatabase::LookupGuid(Path("project://Assets/Textures/tex.txt"));
	ASSERT_TRUE(guid.IsValid());

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());

	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);
	EXPECT_TRUE(HasText(build.context, build.root, "readme.txt"));
	EXPECT_FALSE(HasText(build.context, build.root, "tex.txt"));

	EditorEvents::PingRequested.Publish(EventBus::GetGlobal(), {{.asset = guid}});
	PanelManager::TickAll(0.016f);
	build.context.Update();

	Variant folder;
	ASSERT_TRUE(GetField(panel.Get(), panel.Get()->GetType(), StringHash("selectedFolder"), &folder));
	EXPECT_EQ(folder.As<str>(), "Assets/Textures");
	EXPECT_TRUE(HasText(build.context, build.root, "tex.txt"));

	UI::ElementHandle marked = FindByClass(build.context, build.root, EditorTheme::Class::RowPing, 0);
	ASSERT_TRUE(marked.IsValid());
	EXPECT_TRUE(HasText(build.context, marked, "tex.txt"));
}

TEST_F(AssetBrowserPanelTest, APingRevealsWithoutChangingTheSelection) {
	WriteSource("project://Assets/Textures/tex.txt", "t");
	WriteSource("project://Assets/readme.txt", "r");
	AssetDatabase::ScanProjectDirectory();
	Guid pinged = AssetDatabase::LookupGuid(Path("project://Assets/Textures/tex.txt"));
	Guid held = AssetDatabase::LookupGuid(Path("project://Assets/readme.txt"));
	ASSERT_TRUE(pinged.IsValid());
	ASSERT_TRUE(held.IsValid());

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	const Guid selection[] = {held};
	EditorSelection::SelectAssets({selection, 1});
	ASSERT_EQ(EditorSelection::GetAssetSelectionCount(), 1u);

	EditorEvents::PingRequested.Publish(EventBus::GetGlobal(), {{.asset = pinged}});
	PanelManager::TickAll(0.016f);
	build.context.Update();

	ASSERT_EQ(EditorSelection::GetAssetSelectionCount(), 1u);
	EXPECT_EQ(EditorSelection::GetSelectedAsset(), held);
}

TEST_F(AssetBrowserPanelTest, PingMarkComesOffWhenMomentPasses) {
	WriteSource("project://Assets/readme.txt", "r");
	AssetDatabase::ScanProjectDirectory();
	Guid guid = AssetDatabase::LookupGuid(Path("project://Assets/readme.txt"));
	ASSERT_TRUE(guid.IsValid());

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	EditorEvents::PingRequested.Publish(EventBus::GetGlobal(), {{.asset = guid}});
	PanelManager::TickAll(0.016f);
	build.context.Update();
	UI::ElementHandle marked = FindByClass(build.context, build.root, EditorTheme::Class::RowPing, 0);
	ASSERT_TRUE(marked.IsValid());

	for (u32 frame = 0; frame < 8; ++frame) {
		PanelManager::TickAll(0.25f);
		build.context.Update();
	}
	EXPECT_TRUE(build.context.IsAlive(marked));
	EXPECT_FALSE(build.context.HasClass(marked, EditorTheme::Class::RowPing));
}

TEST_F(AssetBrowserPanelTest, PingForHiddenAssetMovesNothing) {
	WriteSource("project://Assets/readme.txt", "r");
	AssetDatabase::ScanProjectDirectory();

	Handle<EditorPanel> panel = OpenBrowser();
	ASSERT_TRUE(panel.IsValid());
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	Variant before;
	ASSERT_TRUE(GetField(panel.Get(), panel.Get()->GetType(), StringHash("selectedFolder"), &before));

	EditorEvents::PingRequested.Publish(EventBus::GetGlobal(), {{.asset = Guid{0xDEAD, 0xBEEF}}});
	PanelManager::TickAll(0.016f);
	build.context.Update();

	Variant after;
	ASSERT_TRUE(GetField(panel.Get(), panel.Get()->GetType(), StringHash("selectedFolder"), &after));
	EXPECT_EQ(after.As<str>(), before.As<str>());
	EXPECT_FALSE(FindByClass(build.context, build.root, EditorTheme::Class::RowPing, 0).IsValid());
}

TEST_F(AssetBrowserPanelTest, BothOpenBrowsersAnswerOnePing) {
	WriteSource("project://Assets/Textures/tex.txt", "t");
	AssetDatabase::ScanProjectDirectory();
	Guid guid = AssetDatabase::LookupGuid(Path("project://Assets/Textures/tex.txt"));
	ASSERT_TRUE(guid.IsValid());

	Handle<EditorPanel> first = OpenBrowser();
	Handle<EditorPanel> second = OpenBrowser();
	ASSERT_TRUE(first.IsValid());
	ASSERT_TRUE(second.IsValid());
	PanelBuild buildA{BrowserContextDesc()};
	PanelBuild buildB{BrowserContextDesc()};
	BuildAndSettle(*first.Get(), buildA);
	BuildAndSettle(*second.Get(), buildB);

	EditorEvents::PingRequested.Publish(EventBus::GetGlobal(), {{.asset = guid}});
	PanelManager::TickAll(0.016f);
	buildA.context.Update();
	buildB.context.Update();

	for (EditorPanel* panel : {first.Get(), second.Get()}) {
		Variant folder;
		ASSERT_TRUE(GetField(panel, panel->GetType(), StringHash("selectedFolder"), &folder));
		EXPECT_EQ(folder.As<str>(), "Assets/Textures");
	}
	EXPECT_TRUE(FindByClass(buildA.context, buildA.root, EditorTheme::Class::RowPing, 0).IsValid());
	EXPECT_TRUE(FindByClass(buildB.context, buildB.root, EditorTheme::Class::RowPing, 0).IsValid());
}

TEST_F(AssetBrowserFixture, VariantToldFromBaseByPath) {
	SceneSystem::Initialize();
	EditorSceneOps::Initialize();
	{
		Handle<Scene> scene = SceneSystem::CreateScene("Authoring");
		Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", scene);
		Guid base = EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Unit.prefab"));
		ASSERT_TRUE(base.IsValid());
		Guid variant = EditorSceneOps::CreatePrefabVariantOf(base, Path("project://Assets/Unit Variant.prefab"));
		ASSERT_TRUE(variant.IsValid());

		EXPECT_FALSE(AssetBrowserOps::IsPrefabVariant("Assets/Unit.prefab"));
		EXPECT_TRUE(AssetBrowserOps::IsPrefabVariant("Assets/Unit Variant.prefab"));
		EXPECT_TRUE(AssetBrowserOps::IsPrefabVariant("Assets/Unit Variant.prefab"));
		EXPECT_FALSE(AssetBrowserOps::IsPrefabVariant("Assets/Missing.prefab"));
		EXPECT_FALSE(AssetBrowserOps::IsPrefabVariant("Assets/Unit.prefab.meta"));
		EXPECT_FALSE(AssetBrowserOps::IsPrefabVariant(""));

		AssetDatabase::CollectGarbage();
		SceneSystem::UnloadScene(scene);
		SceneSystem::EndFrame();
	}
	EditorSceneOps::Terminate();
	SceneSystem::Terminate();
}

TEST_F(AssetBrowserPanelTest, OnlyVariantCellCarriesMark) {
	browserIconSize = 0.6f;
	SceneSystem::Initialize();
	EditorSceneOps::Initialize();
	Handle<Scene> scene = SceneSystem::CreateScene("Authoring");
	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", scene);
	Guid base = EditorSceneOps::CreatePrefabFrom(unit, Path("project://Assets/Unit.prefab"));
	ASSERT_TRUE(base.IsValid());
	ASSERT_TRUE(EditorSceneOps::CreatePrefabVariantOf(base, Path("project://Assets/Unit Variant.prefab")).IsValid());
	SceneSystem::UnloadScene(scene);
	SceneSystem::EndFrame();
	AssetDatabase::ScanProjectDirectory();

	Handle<EditorPanel> panel = OpenBrowser();
	PanelBuild build{BrowserContextDesc()};
	BuildAndSettle(*panel.Get(), build);

	Vector<UI::ElementHandle> stack{GetMemoryResource(MemTag::Editor)};
	Vector<UI::ElementHandle> cells{GetMemoryResource(MemTag::Editor)};
	stack.push_back(build.root);
	while (!stack.empty()) {
		UI::ElementHandle at = stack.back();
		stack.pop_back();
		if (build.context.HasClass(at, StringHash("ed-asset-cell"))) {
			cells.push_back(at);
		}
		const UI::Element* element = build.context.GetElement(at);
		for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX; child != UI::INVALID_UI_INDEX;) {
			UI::ElementHandle handle = build.context.MakeHandle(child);
			stack.push_back(handle);
			child = build.context.GetElement(handle)->nextSibling;
		}
	}

	u32 prefabCells = 0;
	u32 withMark = 0;
	for (UI::ElementHandle cell : cells) {
		bool isVariantCell = HasText(build.context, cell, "Unit Variant.prefab");
		bool isBaseCell = HasText(build.context, cell, "Unit.prefab");
		if (!isVariantCell && !isBaseCell) {
			continue;
		}
		++prefabCells;
		UI::ElementHandle mark = FindClassed(build.context, cell, StringHash("ed-asset-cell-variant"));
		bool marked = build.context.IsAlive(mark);
		withMark += marked ? 1u : 0u;
		EXPECT_EQ(marked, isVariantCell);
		if (marked) {
			EXPECT_EQ(build.context.GetStyle(mark).display, UI::Display::None);
		}
	}
	EXPECT_EQ(prefabCells, 2u);
	EXPECT_EQ(withMark, 1u);

	EditorSceneOps::Terminate();
	SceneSystem::Terminate();
}

TEST_F(AssetBrowserPanelTest, CaptionCentredUnderIcon) {
	WriteSource("project://Assets/alpha.txt", "a");
	WriteSource("project://Assets/beta.txt", "b");

	Handle<EditorPanel> panel = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("AssetBrowserPanel")));
	ASSERT_TRUE(panel.IsValid());

	UI::ContextDesc desc;
	desc.sizePx = {1280, 210};
	PanelBuild build{desc};
	EditorTheme::ApplyToContext(build.context);
	panel.Get()->BuildContent(build.context, build.root);
	build.context.Update();
	panel.Get()->OnTick(0.016f);
	build.context.Update();

	UI::ElementHandle cell = FindFileRow(build.context, build.root, "alpha.txt");
	ASSERT_TRUE(build.context.IsAlive(cell));
	ASSERT_TRUE(build.context.HasClass(cell, StringHash("ed-asset-cell")));

	UI::ElementHandle label = FindByClass(build.context, cell, StringHash("ed-asset-cell-label"), 0);
	ASSERT_TRUE(build.context.IsAlive(label));
	EXPECT_EQ(build.context.GetTextString(label), "alpha.txt");

	const UI::Rect& cellBox = build.context.GetLayout(cell).borderBox;
	const UI::Rect& labelBox = build.context.GetLayout(label).borderBox;

	EXPECT_GT(labelBox.w, 0.0f);
	EXPECT_NEAR(labelBox.x + labelBox.w * 0.5f, cellBox.x + cellBox.w * 0.5f, 1.0f);
	EXPECT_GT(labelBox.h, 0.0f);

	EXPECT_GE(labelBox.y, cellBox.y);
	EXPECT_LE(labelBox.y + labelBox.h, cellBox.y + cellBox.h + 0.5f);

	UI::ElementHandle icon = FindByClass(build.context, cell, StringHash("ed-asset-cell-badge"), 0);
	if (build.context.IsAlive(icon)) {
		EXPECT_GE(labelBox.y, build.context.GetLayout(icon).borderBox.y);
	}

	EXPECT_LE(cellBox.y + cellBox.h, 210.0f);

	PanelManager::ClosePanel(panel);
}

TEST_F(AssetBrowserPanelTest, SelectedCellMarksSquareAndName) {
	WriteSource("project://Assets/alpha.txt", "a");
	Handle<EditorPanel> panel = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("AssetBrowserPanel")));
	ASSERT_TRUE(panel.IsValid());
	UI::ContextDesc desc;
	desc.sizePx = {1280, 400};
	PanelBuild build{desc};
	EditorTheme::ApplyToContext(build.context);
	panel.Get()->BuildContent(build.context, build.root);
	build.context.Update();
	panel.Get()->OnTick(0.016f);
	build.context.Update();

	UI::ElementHandle cell = FindFileRow(build.context, build.root, "alpha.txt");
	ASSERT_TRUE(build.context.HasClass(cell, StringHash("ed-asset-cell")));
	UI::ElementHandle square = FindByClass(build.context, cell, StringHash("ed-asset-cell-well"), 0);
	UI::ElementHandle label = FindByClass(build.context, cell, StringHash("ed-asset-cell-label"), 0);
	ASSERT_TRUE(build.context.IsAlive(square) && build.context.IsAlive(label));
	auto paints = [&](UI::ElementHandle element) {
		const UI::Brush& fill = build.context.GetComputedStyle(element).background;
		return fill.type != UI::BrushType::None && !(fill.type == UI::BrushType::Solid && fill.solid.a == 0.0f);
	};
	ASSERT_FALSE(paints(square));

	const UI::Rect at = build.context.GetLayout(cell).borderBox;
	InputEvent over{};
	over.type = InputEventType::MouseMove;
	over.mousePosPx = {static_cast<i32>(at.x + at.w * 0.5f), static_cast<i32>(at.y + at.h * 0.5f)};
	build.context.InjectInput(over);
	build.context.Update();
	EXPECT_TRUE(paints(square));
	EXPECT_FALSE(paints(cell));

	ClickElement(build.context, cell);
	panel.Get()->OnTick(0.016f);
	build.context.Update();
	ASSERT_TRUE(build.context.HasClass(cell, StringHash("slt-selected")));

	EXPECT_FALSE(paints(cell));
	EXPECT_TRUE(paints(square));
	EXPECT_TRUE(paints(label));
	const UI::Rect squareBox = build.context.GetLayout(square).borderBox;
	const UI::Rect labelBox = build.context.GetLayout(label).borderBox;
	const UI::Rect cellBox = build.context.GetLayout(cell).borderBox;
	EXPECT_NEAR(squareBox.w, squareBox.h, 0.5f);
	EXPECT_LT(squareBox.h, cellBox.h);
	EXPECT_LT(labelBox.w, cellBox.w);
	EXPECT_GE(labelBox.y, squareBox.y + squareBox.h - 0.5f);

	InputEvent away{};
	away.type = InputEventType::MouseMove;
	away.mousePosPx = {static_cast<i32>(cellBox.x + cellBox.w + 400.0f), static_cast<i32>(cellBox.y + cellBox.h * 0.5f)};
	build.context.InjectInput(away);
	build.context.Update();
	ASSERT_FALSE(HasFlag(build.context.GetElement(cell)->flags, UI::ElementFlags::Hovered));
	EXPECT_FALSE(paints(cell));
	EXPECT_TRUE(paints(square));

	PanelManager::ClosePanel(panel);
}
