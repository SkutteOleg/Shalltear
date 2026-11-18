#include "TestsPCH.h"

#include "UITestUtil.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Asset/AssetImporter.h"
#include "Shalltear/Core/Graphics.h"
#include "Shalltear/Core/Graphics/PrimitiveMesh.h"
#include "Shalltear/Core/Reflection/Walk.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/UI/UIDocument.h"
#include "Shalltear/Core/UI/UIPanelRenderer.h"

#include "Shalltear/Editor/ComponentDrops.h"
#include "Shalltear/Editor/EditorDragData.h"
#include "Shalltear/Editor/EditorGizmos.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/EditorSelection.h"
#include "Shalltear/Editor/EditorTheme.h"
#include "Shalltear/Editor/EditorViewportOps.h"
#include "Shalltear/Editor/PanelManager.h"
#include "Shalltear/Editor/SceneAnnotations.h"
#include "Shalltear/Editor/SceneAssetDrops.h"
#include "Shalltear/Editor/SceneViewOverlays.h"
#include "Shalltear/Editor/UndoSystem.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace Shalltear;
using namespace UITest;

namespace {

	class SceneViewDropTest : public ::testing::Test {
	protected:
		Handle<Scene> _scene;
		std::filesystem::path _projectRoot;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_projectRoot = std::filesystem::temp_directory_path() /
						   ("shalltear_sceneviewdrop_" + std::to_string(stamp) + "_" +
							std::to_string(counter.fetch_add(1)));
			std::filesystem::create_directories(_projectRoot / "Assets");
			FileSystem::Mount("project", Path(_projectRoot.generic_string()));
			AssetDatabase::Initialize();

			TypeRegistry::InitializePrimitives();
			RegisterBuiltInPanels();
			SceneSystem::Initialize();
			EditorSceneOps::Initialize();
			EditorSelection::Initialize();
			UndoSystem::Initialize();
			SceneAnnotations::Initialize();
			PanelManager::Initialize();
			_scene = SceneSystem::CreateScene("Fixture");
		}

		void TearDown() override {
			PanelManager::Terminate();
			SceneAnnotations::Terminate();
			UndoSystem::Terminate();
			EditorSelection::Terminate();
			EditorSceneOps::Terminate();
			SceneSystem::Terminate();
			AssetDatabase::Terminate();
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_projectRoot, ec);
		}

		AssetDragData StageModelAsset() {
			std::error_code ec;
			std::filesystem::path source;
			std::filesystem::path probe = std::filesystem::current_path(ec);
			for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
				std::filesystem::path candidate = probe / "Source/Tests/Fixtures/Graphics/hierarchy.gltf";
				if (std::filesystem::exists(candidate, ec)) {
					source = candidate;
					break;
				}
				std::filesystem::path parent = probe.parent_path();
				if (parent == probe) {
					break;
				}
				probe = parent;
			}
			AssetDragData data;
			if (source.empty()) {
				return data;
			}
			std::filesystem::copy_file(source, _projectRoot / "Assets" / "hierarchy.gltf",
									   std::filesystem::copy_options::overwrite_existing, ec);
			if (ec) {
				return data;
			}
			AssetDatabase::ScanProjectDirectory();
			data.guid = AssetDatabase::LookupGuid(Path("project://Assets/hierarchy.gltf"));
			IAssetImporter* importer = AssetImporterRegistry::FindByExtension(".gltf");
			data.assetTypeHash = importer ? importer->GetAssetType().GetValue() : 0;
			return data;
		}

		static UI::DragPayload AssetPayload(const AssetDragData& data) {
			UI::DragPayload payload;
			payload.type = "Asset"_hash;
			payload.data = Variant(data);
			return payload;
		}
	};

	struct SceneViewRig {
		MockFontSystem fonts;
		UI::Context context;
		UI::ElementHandle root;
		Handle<EditorPanel> panel;

		explicit SceneViewRig(Vec3 eye) : context(Desc(), &fonts) {
			EditorTheme::ApplyToContext(context);
			panel = PanelManager::OpenPanel(TypeRegistry::FindType(StringHash("SceneViewPanel")));

			if (Object* object = panel.Get()) {
				Variant position(eye);
				Variant pitch(90.0f);
				Variant yaw(0.0f);
				SetField(object, object->GetType(), "cameraPosition"_hash, &position);
				SetField(object, object->GetType(), "cameraPitchDeg"_hash, &pitch);
				SetField(object, object->GetType(), "cameraYawDeg"_hash, &yaw);
			}

			UI::Style fill;
			fill.width = UI::Length::Percent(1.0f);
			fill.height = UI::Length::Percent(1.0f);
			root = context.CreateElement();
			context.AppendChild(context.GetRoot(), root);
			context.SetStyle(root, fill);
			panel.Get()->BuildContent(context, root);
			context.Update();
		}

		static UI::ContextDesc Desc() {
			UI::ContextDesc desc;
			desc.sizePx = {600, 400};
			return desc;
		}

		UI::ElementHandle DropTarget() const {
			UI::ElementHandle found;
			Walk(root, found);
			return found;
		}

		Vec2 CenterOfTarget() const {
			const UI::Rect& box = context.GetLayout(DropTarget()).borderBox;
			return {box.x + box.w * 0.5f, box.y + box.h * 0.5f};
		}
	private:
		void Walk(UI::ElementHandle from, UI::ElementHandle& out) const {
			if (out.IsValid()) {
				return;
			}
			if (const_cast<UI::Context&>(context).Events(from).onDrop) {
				out = from;
				return;
			}
			const UI::Element* element = context.GetElement(from);
			for (u32 child = element ? element->firstChild : UI::INVALID_UI_INDEX;
				 child != UI::INVALID_UI_INDEX;) {
				UI::ElementHandle handle = const_cast<UI::Context&>(context).MakeHandle(child);
				Walk(handle, out);
				child = context.GetElement(handle)->nextSibling;
			}
		}
	};

	u32 RootCount(Handle<Scene> scene) {
		Scene* resolved = scene.Get();
		return resolved ? resolved->GetRootCount() : 0u;
	}

} // namespace

TEST_F(SceneViewDropTest, ModelDropAtCursor) {
	AssetDragData data = StageModelAsset();
	if (!data.guid.IsValid()) {
		GTEST_SKIP() << "hierarchy.gltf fixture not found";
	}
	ASSERT_EQ(StringHash{data.assetTypeHash}, StringHash("PrefabAsset"));

	SceneViewRig rig(Vec3(3.0f, 5.0f, -2.0f));
	ASSERT_TRUE(rig.DropTarget().IsValid());
	ASSERT_EQ(RootCount(_scene), 0u);

	ASSERT_TRUE(rig.context.DeliverDrop(AssetPayload(data), rig.CenterOfTarget()));

	ASSERT_EQ(RootCount(_scene), 1u);
	Handle<SceneObject> placed = _scene->GetRoot(0);
	ASSERT_TRUE(placed.IsValid());

	u32 objects = 0;
	_scene->ForEachObject([](SceneObject&, void* user) { ++*static_cast<u32*>(user); }, &objects);
	EXPECT_EQ(objects, 5u);

	Vec3 world = placed.Get()->GetWorldPosition();
	EXPECT_NEAR(world.x, 3.0f, 1e-3f);
	EXPECT_NEAR(world.y, 0.0f, 1e-3f);
	EXPECT_NEAR(world.z, -2.0f, 1e-3f);
}

TEST_F(SceneViewDropTest, DropsLandWhereAimed) {
	AssetDragData data = StageModelAsset();
	if (!data.guid.IsValid()) {
		GTEST_SKIP() << "hierarchy.gltf fixture not found";
	}

	SceneViewRig rig(Vec3(0.0f, 5.0f, 0.0f));
	const UI::Rect& box = rig.context.GetLayout(rig.DropTarget()).borderBox;
	Vec2 center{box.x + box.w * 0.5f, box.y + box.h * 0.5f};
	Vec2 offCenter{box.x + box.w * 0.25f, box.y + box.h * 0.5f};

	ASSERT_TRUE(rig.context.DeliverDrop(AssetPayload(data), center));
	ASSERT_TRUE(rig.context.DeliverDrop(AssetPayload(data), offCenter));
	ASSERT_EQ(RootCount(_scene), 2u);

	Vec3 first = _scene->GetRoot(0).Get()->GetWorldPosition();
	Vec3 second = _scene->GetRoot(1).Get()->GetWorldPosition();
	EXPECT_NEAR(first.x, 0.0f, 1e-3f);
	EXPECT_LT(second.x, first.x - 0.5f);
	EXPECT_NEAR(second.y, 0.0f, 1e-3f);
}

TEST_F(SceneViewDropTest, DropLandsOnObject) {
	AssetDragData data = StageModelAsset();
	if (!data.guid.IsValid()) {
		GTEST_SKIP() << "hierarchy.gltf fixture not found";
	}

	MeshAsset* mesh = New<MeshAsset>(MemTag::Asset);
	BuildPrimitiveMesh(PrimitiveShape::Cube, *mesh);
	Handle<SceneObject> floorBox = SceneSystem::CreateObject("Box", _scene);
	floorBox.Get()->SetLocalPosition(Vec3(0.0f, 0.5f, 0.0f));
	floorBox.Get()->AddComponent<MeshRenderer>().Get()->SetMesh(Handle<MeshAsset>(mesh));

	SceneViewRig rig(Vec3(0.0f, 5.0f, 0.0f));
	ASSERT_TRUE(rig.context.DeliverDrop(AssetPayload(data), rig.CenterOfTarget()));

	ASSERT_EQ(RootCount(_scene), 2u);
	Vec3 world = _scene->GetRoot(1).Get()->GetWorldPosition();
	EXPECT_NEAR(world.y, 1.0f, 1e-3f);

	SceneSystem::Destroy(floorBox);
	SceneSystem::EndFrame();
	Delete(mesh);
}

TEST_F(SceneViewDropTest, ComponentAssetJoinsObjectUnderCursor) {
	static char owner = 0;
	MeshAsset* mesh = New<MeshAsset>(MemTag::Asset);
	BuildPrimitiveMesh(PrimitiveShape::Cube, *mesh);
	Handle<SceneObject> box = SceneSystem::CreateObject("Box", _scene);
	box.Get()->SetLocalPosition(Vec3(0.0f, 0.5f, 0.0f));
	box.Get()->AddComponent<MeshRenderer>().Get()->SetMesh(Handle<MeshAsset>(mesh));
	ASSERT_TRUE(ComponentDrops::Register(StringHash("ProbeScript"), [](const Guid&) { return Light::GetReflectType(); }, &owner));
	AssetDragData data;
	data.guid = Guid{3, 9};
	data.assetTypeHash = StringHash("ProbeScript").GetValue();

	SceneViewRig rig(Vec3(0.0f, 5.0f, 0.0f));
	const UI::Rect& area = rig.context.GetLayout(rig.DropTarget()).borderBox;
	ASSERT_TRUE(rig.context.DeliverDrop(AssetPayload(data), {area.x + 4.0f, area.y + area.h - 4.0f}));
	EXPECT_EQ(box.Get()->GetComponentCount(), 1u);
	EXPECT_EQ(RootCount(_scene), 1u);

	ASSERT_TRUE(rig.context.DeliverDrop(AssetPayload(data), rig.CenterOfTarget()));
	ASSERT_EQ(box.Get()->GetComponentCount(), 2u);
	EXPECT_TRUE(box.Get()->GetComponent<Light>().IsValid());
	EXPECT_EQ(RootCount(_scene), 1u);
	ASSERT_TRUE(UndoSystem::Undo());
	SceneSystem::EndFrame();
	EXPECT_EQ(box.Get()->GetComponentCount(), 1u);

	EXPECT_EQ(ComponentDrops::UnregisterOwnedBy(&owner), 1u);
	EXPECT_FALSE(rig.context.DeliverDrop(AssetPayload(data), rig.CenterOfTarget()));
	SceneSystem::Destroy(box);
	SceneSystem::EndFrame();
	Delete(mesh);
}

TEST_F(SceneViewDropTest, TheDropIsASingleUndoStep) {
	AssetDragData data = StageModelAsset();
	if (!data.guid.IsValid()) {
		GTEST_SKIP() << "hierarchy.gltf fixture not found";
	}

	SceneViewRig rig(Vec3(3.0f, 5.0f, -2.0f));
	ASSERT_TRUE(rig.context.DeliverDrop(AssetPayload(data), rig.CenterOfTarget()));
	ASSERT_EQ(RootCount(_scene), 1u);

	ASSERT_TRUE(UndoSystem::Undo());
	SceneSystem::EndFrame();
	EXPECT_EQ(RootCount(_scene), 0u);

	ASSERT_TRUE(UndoSystem::Redo());
	SceneSystem::EndFrame();
	ASSERT_EQ(RootCount(_scene), 1u);
	Vec3 world = _scene->GetRoot(0).Get()->GetWorldPosition();
	EXPECT_NEAR(world.x, 3.0f, 1e-3f);
	EXPECT_NEAR(world.z, -2.0f, 1e-3f);
}

TEST_F(SceneViewDropTest, AnAssetOfAnotherTypeIsRefused) {
	AssetDragData data = StageModelAsset();
	if (!data.guid.IsValid()) {
		GTEST_SKIP() << "hierarchy.gltf fixture not found";
	}
	SceneViewRig rig(Vec3(0.0f, 5.0f, 0.0f));
	Vec2 point = rig.CenterOfTarget();

	ASSERT_TRUE(rig.context.DeliverDrop(AssetPayload(data), point));
	ASSERT_EQ(RootCount(_scene), 1u);

	AssetDragData wrongType = data;
	wrongType.assetTypeHash = StringHash("MaterialAsset").GetValue();
	EXPECT_FALSE(rig.context.DeliverDrop(AssetPayload(wrongType), point));
	EXPECT_EQ(RootCount(_scene), 1u);
}

TEST_F(SceneViewDropTest, DropIsRefusedWhilePrefabIsStaged) {
	AssetDragData data = StageModelAsset();
	if (!data.guid.IsValid()) {
		GTEST_SKIP() << "hierarchy.gltf fixture not found";
	}

	Handle<SceneObject> unit = SceneSystem::CreateObject("Unit", _scene);
	Guid prefab = SceneSystem::SavePrefab(unit, Path("project://Assets/unit.prefab"));
	ASSERT_TRUE(prefab.IsValid());
	SceneSystem::Destroy(unit);
	SceneSystem::EndFrame();

	ASSERT_TRUE(EditorSceneOps::OpenPrefab(prefab).IsValid());
	Handle<Scene> stage = EditorSceneOps::GetPrefabStage();
	ASSERT_TRUE(stage.IsValid());
	u32 stageRootsBefore = RootCount(stage);

	SceneViewRig rig(Vec3(0.0f, 5.0f, 0.0f));
	EXPECT_FALSE(rig.context.DeliverDrop(AssetPayload(data), rig.CenterOfTarget()));
	EXPECT_EQ(RootCount(stage), stageRootsBefore);
	EXPECT_EQ(RootCount(_scene), 0u);
}

TEST_F(SceneViewDropTest, RegisteredDropIsOneUndo) {
	{
		std::ofstream file(_projectRoot / "Assets" / "Hud.slxml", std::ios::binary);
		file << "<ui version=\"1\">\n\t<panel id=\"root\"/>\n</ui>\n";
	}
	AssetDatabase::ScanProjectDirectory();
	AssetDragData data;
	data.guid = AssetDatabase::LookupGuid(Path("project://Assets/Hud.slxml"));
	IAssetImporter* importer = AssetImporterRegistry::FindByExtension(".slxml");
	data.assetTypeHash = importer ? importer->GetAssetType().GetValue() : 0;
	ASSERT_TRUE(data.guid.IsValid());
	char owner = 0;
	ASSERT_TRUE(SceneAssetDrops::Register(StringHash("UIDocumentAsset"), StringHash("UIPanelRenderer"), StringHash("_document"), &owner));
	auto documentOf = [](Handle<SceneObject> object) {
		Handle<UIPanelRenderer> renderer = object.IsValid() ? object->GetComponent<UIPanelRenderer>() : Handle<UIPanelRenderer>{};
		const UIDocumentAsset* document = renderer.IsValid() ? renderer->GetDocument().Get() : nullptr;
		return document ? document->assetId : Guid{};
	};

	SceneViewRig rig(Vec3(3.0f, 5.0f, -2.0f));
	ASSERT_TRUE(rig.context.DeliverDrop(AssetPayload(data), rig.CenterOfTarget()));
	ASSERT_EQ(RootCount(_scene), 1u);
	Handle<SceneObject> made = _scene->GetRoot(0);
	EXPECT_EQ(made->name, "Hud");
	EXPECT_EQ(documentOf(made), data.guid);
	EXPECT_EQ(EditorSelection::GetActive().GetInstanceId(), made.GetInstanceId());
	const Vec3 world = made->GetWorldPosition();
	EXPECT_NEAR(world.x, 3.0f, 1e-3f);
	EXPECT_NEAR(world.z, -2.0f, 1e-3f);

	ASSERT_TRUE(UndoSystem::Undo());
	SceneSystem::EndFrame();
	EXPECT_EQ(RootCount(_scene), 0u);
	ASSERT_TRUE(UndoSystem::Redo());
	SceneSystem::EndFrame();
	ASSERT_EQ(RootCount(_scene), 1u);
	EXPECT_NEAR(_scene->GetRoot(0)->GetWorldPosition().x, 3.0f, 1e-3f);
	EXPECT_EQ(documentOf(_scene->GetRoot(0)), data.guid);

	EXPECT_EQ(SceneAssetDrops::UnregisterOwnedBy(&owner), 1u);
	EXPECT_FALSE(rig.context.DeliverDrop(AssetPayload(data), rig.CenterOfTarget()));
	SceneAssetDrops::Terminate();
}

TEST_F(SceneViewDropTest, OverlayActsOnSelection) {
	char owner = 0;
	u32 builds = 0;
	u32 refreshes = 0;
	u32 acted = 0;
	UI::ElementHandle button;
	ASSERT_TRUE(SceneViewOverlays::Register(
		"Probe Controls", StringHash("Light"),
		[&](UI::Context& context, UI::ElementHandle body, std::span<const Handle<Component>> components) -> SceneViewOverlays::RefreshFn {
			++builds;
			EXPECT_EQ(components.size(), 2u);
			button = context.CreateElement();
			context.AppendChild(body, button);
			UI::Style size;
			size.width = UI::Length::Pixel(40.0f);
			size.height = UI::Length::Pixel(20.0f);
			context.SetStyle(button, size);
			const u32 count = static_cast<u32>(components.size());
			context.Events(button).onClick = [&acted, count]() { acted += count; };
			return [&refreshes](UI::Context&) { ++refreshes; };
		},
		&owner));
	EXPECT_FALSE(SceneViewOverlays::Register("Probe Controls", StringHash("Light"), [](UI::Context&, UI::ElementHandle, std::span<const Handle<Component>>) { return SceneViewOverlays::RefreshFn{}; }, &owner));

	Handle<SceneObject> a = SceneSystem::CreateObject("A", _scene);
	Handle<SceneObject> b = SceneSystem::CreateObject("B", _scene);
	Handle<SceneObject> plain = SceneSystem::CreateObject("Plain", _scene);
	a->AddComponent<Light>();
	b->AddComponent<Light>();
	EditorSelection::Select(Handle<Object>(static_cast<Object*>(a.Get())));
	EditorSelection::AddToSelection(Handle<Object>(static_cast<Object*>(b.Get())));

	SceneViewRig first(Vec3(0.0f, 5.0f, 0.0f));
	SceneViewRig second(Vec3(0.0f, 5.0f, 0.0f));
	auto tick = [](SceneViewRig& rig) {
		rig.panel.Get()->OnTick(1.0f / 60.0f);
		rig.context.Update();
	};
	auto card = [](SceneViewRig& rig, u32 skip) { return FindByClass(rig.context, rig.root, EditorTheme::Class::SceneOverlayCard, skip); };
	tick(first);
	tick(second);
	EXPECT_EQ(builds, 2u);
	ASSERT_TRUE(first.context.IsAlive(card(first, 0)));
	EXPECT_TRUE(second.context.IsAlive(card(second, 0)));
	EXPECT_FALSE(first.context.IsAlive(card(first, 1)));
	ASSERT_TRUE(second.context.IsAlive(FindByText(second.context, card(second, 0), "Probe Controls")));

	ClickElement(second.context, button);
	EXPECT_EQ(acted, 2u);

	const u32 refreshed = refreshes;
	tick(first);
	tick(second);
	EXPECT_EQ(refreshes, refreshed + 2);
	EXPECT_EQ(builds, 2u);

	EditorSelection::Select(Handle<Object>(static_cast<Object*>(plain.Get())));
	tick(first);
	EXPECT_FALSE(first.context.IsAlive(card(first, 0)));

	EditorSelection::Select(Handle<Object>(static_cast<Object*>(a.Get())));
	EditorSelection::AddToSelection(Handle<Object>(static_cast<Object*>(b.Get())));
	tick(first);
	ASSERT_TRUE(first.context.IsAlive(card(first, 0)));
	EXPECT_GE(SceneViewOverlays::UnregisterOwnedBy(&owner), 2u);
	const u32 swept = refreshes;
	tick(first);
	tick(second);
	EXPECT_EQ(refreshes, swept);
	EXPECT_FALSE(first.context.IsAlive(card(first, 0)));
	EXPECT_FALSE(second.context.IsAlive(card(second, 0)));
	SceneViewOverlays::Terminate();
}

TEST_F(SceneViewDropTest, GizmoCursorIsWorldRay) {
	EditorGizmos::Initialize();
	SceneViewRig rig(Vec3(0.0f, 5.0f, 0.0f));

	char token = 0;
	GizmoDrawContext seen;
	u32 calls = 0;
	ASSERT_TRUE(EditorGizmos::RegisterSceneGizmo(StringHash("CursorProbe"), [&](GizmoDrawContext& context) {
													 seen = context;
													 ++calls; }, &token));

	InputEvent away{};
	away.type = InputEventType::MouseMove;
	away.mousePosPx = {-40, -40};
	rig.context.InjectInput(away);
	rig.context.Update();
	rig.panel.Get()->OnSubmitView(1.0f / 60.0f);
	ASSERT_GT(calls, 0u);
	EXPECT_FALSE(seen.cursorInView);

	const Vec2 center = rig.CenterOfTarget();
	InputEvent over{};
	over.type = InputEventType::MouseMove;
	over.mousePosPx = {static_cast<i32>(center.x), static_cast<i32>(center.y)};
	rig.context.InjectInput(over);
	rig.context.Update();
	rig.panel.Get()->OnSubmitView(1.0f / 60.0f);

	EXPECT_TRUE(seen.cursorInView);
	EXPECT_NEAR(seen.cursorRay.origin.y, 5.0f, 0.5f);
	EXPECT_NEAR(seen.cursorRay.direction.y, -1.0f, 0.05f);

	EXPECT_EQ(EditorGizmos::UnregisterOwnedBy(&token), 1u);
	EditorGizmos::Terminate();
}
