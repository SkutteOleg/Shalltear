#include "TestsPCH.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/UI/DragDrop.h"

#include "Shalltear/Editor/ComponentDrops.h"
#include "Shalltear/Editor/EditorDragData.h"
#include "Shalltear/Editor/EditorExtensions.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/EditorSelection.h"
#include "Shalltear/Editor/UndoSystem.h"

#include <atomic>
#include <chrono>
#include <filesystem>

using namespace Shalltear;

namespace {

	class DropProbe : public Component {
	public:
		SLT_REFLECT_COMPONENT(DropProbe, Component)
		SLT_REFLECT_FIELD(weight)
		SLT_END_REFLECT()

		f32 weight = 1.0f;
	};

	SLT_REGISTER_TYPE(DropProbe)

	const StringHash ProbeAssetType{"DropProbeScript"};
	const Guid ProbeAsset{7, 11};

	class ComponentDropsTest : public ::testing::Test {
	protected:
		std::filesystem::path _root;
		Handle<Scene> _scene;
		Handle<SceneObject> _object;
		char _owner = 0;
		u32 _resolved = 0;
		const TypeInfo* _answer = DropProbe::GetReflectType();

		void SetUp() override {
			static std::atomic<u64> counter{0};
			const u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() / ("shalltear_componentdrops_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
			std::filesystem::create_directories(_root / "Assets");
			FileSystem::Mount("project", Path(_root.generic_string()));
			AssetDatabase::Initialize();
			TypeRegistry::InitializePrimitives();
			SceneSystem::Initialize();
			EditorSceneOps::Initialize();
			EditorSelection::Initialize();
			UndoSystem::Initialize();
			_scene = SceneSystem::CreateScene("Fixture");
			_object = SceneSystem::CreateObject("Holder", _scene);
		}

		void TearDown() override {
			ComponentDrops::Terminate();
			UndoSystem::Terminate();
			EditorSelection::Terminate();
			EditorSceneOps::Terminate();
			SceneSystem::Terminate();
			AssetDatabase::Terminate();
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		bool RegisterProbe() {
			return ComponentDrops::Register(
				ProbeAssetType,
				[this](const Guid& asset) {
					_resolved += asset == ProbeAsset ? 1u : 100u;
					return _answer;
				},
				&_owner);
		}

		static UI::DragPayload Payload(StringHash assetType, Guid asset = ProbeAsset) {
			AssetDragData data;
			data.guid = asset;
			data.assetTypeHash = assetType.GetValue();
			UI::DragPayload payload;
			payload.type = "Asset"_hash;
			payload.data = Variant(data);
			return payload;
		}
	};

} // anonymous namespace

TEST_F(ComponentDropsTest, RegisteredAssetAddsComponentInOneUndo) {
	ASSERT_TRUE(RegisterProbe());
	const UI::DragPayload payload = Payload(ProbeAssetType);
	EXPECT_TRUE(ComponentDrops::Accepts(payload));
	EXPECT_EQ(_resolved, 0u);

	Handle<Component> added = ComponentDrops::Drop(payload, _object);
	ASSERT_TRUE(added.IsValid());
	EXPECT_EQ(_resolved, 1u);
	EXPECT_EQ(added->GetType(), DropProbe::GetReflectType());
	ASSERT_EQ(_object->GetComponentCount(), 1u);
	EXPECT_EQ(_object->GetComponentAt(0), added);
	EXPECT_EQ(UndoSystem::GetUndoDepth(), 1u);

	ASSERT_TRUE(UndoSystem::Undo());
	SceneSystem::EndFrame();
	EXPECT_EQ(_object->GetComponentCount(), 0u);
	ASSERT_TRUE(UndoSystem::Redo());
	EXPECT_EQ(_object->GetComponentCount(), 1u);
}

TEST_F(ComponentDropsTest, OtherPayloadsAreLeftAlone) {
	ASSERT_TRUE(RegisterProbe());
	EXPECT_FALSE(ComponentDrops::Accepts(Payload(StringHash("MaterialAsset"))));
	EXPECT_FALSE(ComponentDrops::Accepts(Payload(ProbeAssetType, Guid{})));
	UI::DragPayload foreign = Payload(ProbeAssetType);
	foreign.type = "SceneObject"_hash;
	EXPECT_FALSE(ComponentDrops::Accepts(foreign));
	UI::DragPayload empty;
	empty.type = "Asset"_hash;
	EXPECT_FALSE(ComponentDrops::Accepts(empty));

	EXPECT_FALSE(ComponentDrops::Drop(Payload(StringHash("MaterialAsset")), _object).IsValid());
	EXPECT_FALSE(ComponentDrops::Drop(foreign, _object).IsValid());
	EXPECT_FALSE(ComponentDrops::Drop(Payload(ProbeAssetType), {}).IsValid());
	EXPECT_EQ(_resolved, 0u);
	EXPECT_EQ(_object->GetComponentCount(), 0u);
	EXPECT_EQ(UndoSystem::GetUndoDepth(), 0u);
}

TEST_F(ComponentDropsTest, AssetWithoutAnAttachableComponentAddsNothing) {
	ASSERT_TRUE(RegisterProbe());
	_answer = nullptr;
	EXPECT_TRUE(ComponentDrops::Accepts(Payload(ProbeAssetType)));
	EXPECT_FALSE(ComponentDrops::Drop(Payload(ProbeAssetType), _object).IsValid());
	EXPECT_EQ(_resolved, 1u);
	_answer = Component::GetReflectType();
	EXPECT_FALSE(ComponentDrops::Drop(Payload(ProbeAssetType), _object).IsValid());
	_answer = Scene::GetReflectType();
	EXPECT_FALSE(ComponentDrops::Drop(Payload(ProbeAssetType), _object).IsValid());
	EXPECT_EQ(_object->GetComponentCount(), 0u);
	EXPECT_EQ(UndoSystem::GetUndoDepth(), 0u);
}

TEST_F(ComponentDropsTest, TypeTheRegistryLostReturnsNoComponent) {
	ASSERT_TRUE(RegisterProbe());
	ASSERT_TRUE(ComponentDrops::Drop(Payload(ProbeAssetType), _object).IsValid());
	TypeInfo gone = *DropProbe::GetReflectType();
	gone.name = StringHash("DropProbeGone");
	_answer = &gone;
	EXPECT_FALSE(ComponentDrops::Drop(Payload(ProbeAssetType), _object).IsValid());
	EXPECT_EQ(_object->GetComponentCount(), 1u);
}

TEST_F(ComponentDropsTest, RegistrationUniqueAndSweptWithOwner) {
	char other = 0;
	ASSERT_TRUE(RegisterProbe());
	EXPECT_FALSE(RegisterProbe());
	EXPECT_FALSE(ComponentDrops::Register(StringHash("Second"), [](const Guid&) { return DropProbe::GetReflectType(); }, nullptr));
	EXPECT_FALSE(ComponentDrops::Register(StringHash("Third"), {}, &other));
	EXPECT_TRUE(ComponentDrops::Register(StringHash("Fourth"), [](const Guid&) { return DropProbe::GetReflectType(); }, &other));

	EXPECT_EQ(EditorExtensions::Unload(&_owner), 1u);
	EXPECT_FALSE(ComponentDrops::Accepts(Payload(ProbeAssetType)));
	EXPECT_TRUE(ComponentDrops::Accepts(Payload(StringHash("Fourth"))));
	EXPECT_EQ(ComponentDrops::UnregisterOwnedBy(nullptr), 0u);
	EXPECT_EQ(ComponentDrops::UnregisterOwnedBy(&other), 1u);
	EXPECT_FALSE(ComponentDrops::Accepts(Payload(StringHash("Fourth"))));
}
