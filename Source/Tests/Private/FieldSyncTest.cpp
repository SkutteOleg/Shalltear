#include "TestsPCH.h"

#include "Shalltear/Core/Asset.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Math.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/Serialization/TextSerializer.h"

#include "Shalltear/Editor/EditTarget.h"
#include "Shalltear/Editor/EditorCommands.h"
#include "Shalltear/Editor/UndoSystem.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

#ifndef SHALLTEAR_SHARED
#include "Binder.h"

#include "Shalltear/Animation/AnimationClip.h"
#endif

using namespace Shalltear;

namespace {

	char _syncToken = 0;

	struct Mirror {
		f32 speed = 2.5f;
		i32 count = 3;
		Vec3 offset{1.0f, 2.0f, 3.0f};
		u64 target = 0;
	};

	class SyncProbe;
	std::vector<SyncProbe*> _dirtyProbes;

	class SyncProbe final : public Behavior {
	public:
		~SyncProbe() override { std::erase(_dirtyProbes, this); }

		const TypeInfo* GetType() const override { return type; }

		void Push() {
			shadowSpeed = mirror.speed;
			shadowCount = mirror.count;
			shadowOffset = mirror.offset;
			shadowTarget = Handle<SceneObject>(static_cast<SceneObject*>(ObjectRegistry::Resolve(mirror.target)));
			++pushes;
		}

		void Pull() {
			mirror.speed = shadowSpeed;
			mirror.count = shadowCount;
			mirror.offset = shadowOffset;
			mirror.target = shadowTarget.GetInstanceId();
			dirty = false;
			++pulls;
		}

		const TypeInfo* type = nullptr;
		Mirror mirror;
		f32 shadowSpeed = 0.0f;
		i32 shadowCount = 0;
		Vec3 shadowOffset;
		Handle<SceneObject> shadowTarget;
		bool dirty = false;
		u32 pushes = 0;
		u32 pulls = 0;
		str calls;
	};

	void FlushProbes() {
		std::vector<SyncProbe*> pending;
		pending.swap(_dirtyProbes);
		for (SyncProbe* probe : pending) {
			probe->Pull();
		}
	}

	void SyncProbeFields(void* instance, FieldSync phase) {
		SyncProbe& probe = *static_cast<SyncProbe*>(instance);
		probe.calls += phase == FieldSync::BeforeRead ? 'r' : 'w';
		if (probe.dirty) {
			return;
		}
		if (phase == FieldSync::BeforeRead) {
			probe.Push();
			return;
		}
		probe.dirty = true;
		_dirtyProbes.push_back(&probe);
	}

	Object* CreateSyncProbe(const TypeInfo* type, MemTag tag) {
		SyncProbe* probe = New<SyncProbe>(tag);
		probe->type = type;
		probe->Push();
		probe->pushes = 0;
		return probe;
	}

	template <typename T>
	void AddShadowField(ReflectBuilder& builder, const char* name, void* (*address)(void*)) {
		builder.AddField(StringHash(name), name, GetStaticType<T>(), sizeof(T), alignof(T), nullptr, nullptr, address, FieldFlagDefault, {});
	}

	const TypeInfo* RegisterSyncProbe() {
		ReflectBuilder builder(StringHash("SyncProbe"), sizeof(SyncProbe), alignof(SyncProbe));
		builder.SetDisplayName("SyncProbe");
		builder.SetParent(Behavior::GetReflectType());
		builder.SetFlags(TypeFlagComponent);
		builder.SetCreateTypedFn(&CreateSyncProbe);
		builder.SetFieldSyncFn(&SyncProbeFields);
		AddShadowField<f32>(builder, "speed", +[](void* instance) -> void* { return &static_cast<SyncProbe*>(instance)->shadowSpeed; });
		AddShadowField<i32>(builder, "count", +[](void* instance) -> void* { return &static_cast<SyncProbe*>(instance)->shadowCount; });
		AddShadowField<Vec3>(builder, "offset", +[](void* instance) -> void* { return &static_cast<SyncProbe*>(instance)->shadowOffset; });
		AddShadowField<Handle<SceneObject>>(builder, "target", +[](void* instance) -> void* { return &static_cast<SyncProbe*>(instance)->shadowTarget; });
		return builder.FinalizeOwned(&_syncToken);
	}

	u64 Bits(f32 value) {
		u64 bits = 0;
		std::memcpy(&bits, &value, sizeof(value));
		return bits;
	}

	class FieldSyncTest : public ::testing::Test {
	protected:
		std::filesystem::path _root;
		const TypeInfo* _type = nullptr;
		Handle<Scene> _scene;

		void SetUp() override {
			static std::atomic<u64> counter{0};
			const u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() /
					("shalltear_fieldsync_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
			std::filesystem::create_directories(_root);
			FileSystem::Mount("project", Path(_root.generic_string()));
			AssetDatabase::Initialize();
			SceneSystem::Initialize();
			TypeRegistry::InitializePrimitives();
			_type = RegisterSyncProbe();
			ASSERT_NE(_type, nullptr);
			_scene = SceneSystem::CreateScene("Sync");
		}

		void TearDown() override {
			SceneSystem::Terminate();
			_dirtyProbes.clear();
			TypeRegistry::UnregisterOwnedBy(&_syncToken);
			AssetDatabase::CollectGarbage();
			AssetDatabase::Terminate();
			FileSystem::Unmount("project");
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		SyncProbe* AddProbe(const char* objectName, Handle<SceneObject>* outObject = nullptr) {
			Handle<SceneObject> object = SceneSystem::CreateObject(objectName, _scene);
			if (outObject) {
				*outObject = object;
			}
			return static_cast<SyncProbe*>(object->AddComponent(_type).Get());
		}

		SyncProbe* ProbeOf(Handle<Scene> scene, const char* objectName) {
			Handle<SceneObject> object = scene->FindObjectByName(objectName);
			if (!object.IsValid()) {
				return nullptr;
			}
			for (u32 index = 0; index < object->GetComponentCount(); ++index) {
				Component* component = object->GetComponentAt(index).Get();
				if (component && component->GetType() == _type) {
					return static_cast<SyncProbe*>(component);
				}
			}
			return nullptr;
		}
	};

} // anonymous namespace

TEST_F(FieldSyncTest, SaveSyncsBeforeRead) {
	SyncProbe* probe = AddProbe("Holder");
	ASSERT_NE(probe, nullptr);
	probe->mirror.speed = 7.0f;
	probe->mirror.count = -4;
	probe->calls.clear();

	Buffer snapshot;
	ASSERT_TRUE(SceneSystem::SaveSceneToMemory(_scene, snapshot));
	EXPECT_EQ(probe->calls, "r");
	EXPECT_EQ(probe->pushes, 1u);
	EXPECT_FLOAT_EQ(probe->shadowSpeed, 7.0f);

	probe->mirror.speed = 12.5f;
	probe->calls.clear();
	const str state = TextSerializer::Serialize(probe, _type);
	EXPECT_EQ(probe->calls, "r");
	EXPECT_NE(state.find("12.5"), str::npos) << state;
	EXPECT_NE(state.find("-4"), str::npos) << state;
	EXPECT_FALSE(probe->dirty);
}

TEST_F(FieldSyncTest, LoadSyncsAfterWrite) {
	SyncProbe* probe = AddProbe("Holder");
	ASSERT_NE(probe, nullptr);
	probe->mirror.speed = 7.0f;
	probe->mirror.offset = Vec3(4.0f, 5.0f, 6.0f);

	Buffer snapshot;
	ASSERT_TRUE(SceneSystem::SaveSceneToMemory(_scene, snapshot));
	SceneSystem::UnloadScene(_scene);
	SceneSystem::EndFrame();
	Handle<Scene> restored = SceneSystem::LoadSceneFromMemory(snapshot);
	ASSERT_TRUE(restored.IsValid());

	SyncProbe* loaded = ProbeOf(restored, "Holder");
	ASSERT_NE(loaded, nullptr);
	EXPECT_NE(loaded->calls.find('w'), str::npos) << loaded->calls;
	EXPECT_TRUE(loaded->dirty);
	EXPECT_FLOAT_EQ(loaded->mirror.speed, 2.5f);
	FlushProbes();
	EXPECT_EQ(loaded->pulls, 1u);
	EXPECT_FLOAT_EQ(loaded->mirror.speed, 7.0f);
	EXPECT_FLOAT_EQ(loaded->mirror.offset.y, 5.0f);
	EXPECT_EQ(loaded->mirror.count, 3);

	SyncProbe* sink = static_cast<SyncProbe*>(restored->FindObjectByName("Holder")->AddComponent(_type).Get());
	ASSERT_NE(sink, nullptr);
	sink->mirror.count = 40;
	sink->calls.clear();
	ASSERT_TRUE(TextSerializer::Deserialize(sink, _type, "{\"speed\": 1.5}"));
	EXPECT_EQ(sink->calls, "rw");
	FlushProbes();
	EXPECT_FLOAT_EQ(sink->mirror.speed, 1.5f);
	EXPECT_EQ(sink->mirror.count, 40);
}

TEST_F(FieldSyncTest, InspectorEditIsReadModifyWrite) {
	UndoSystem::Initialize();
	SyncProbe* probe = AddProbe("Holder");
	ASSERT_NE(probe, nullptr);
	const EditTarget target{Handle<Component>(probe)};

	probe->mirror.count = 50;
	probe->calls.clear();
	UndoSystem::Push(MakeUnique<SetRawFieldCommand>(target, "speed"_hash, Bits(2.5f), Bits(9.0f), 4));
	ASSERT_NE(probe->calls.find('w'), str::npos);
	EXPECT_LT(probe->calls.find('r'), probe->calls.find('w')) << probe->calls;
	FlushProbes();
	EXPECT_FLOAT_EQ(probe->mirror.speed, 9.0f);
	EXPECT_EQ(probe->mirror.count, 50);

	probe->mirror.count = 60;
	ASSERT_TRUE(UndoSystem::Undo());
	FlushProbes();
	EXPECT_FLOAT_EQ(probe->mirror.speed, 2.5f);
	EXPECT_EQ(probe->mirror.count, 60);

	probe->mirror.speed = 11.0f;
	const FieldInfo* speed = FindField(_type, "speed"_hash);
	ASSERT_NE(speed, nullptr);
	Object* resolved = target.Resolve();
	ASSERT_EQ(resolved, probe);
	EXPECT_FLOAT_EQ(*static_cast<const f32*>(speed->addrFn(resolved)), 11.0f);
	UndoSystem::Terminate();
}

TEST_F(FieldSyncTest, BoxedAccessReachesAFieldWithoutAccessors) {
	SyncProbe* probe = AddProbe("Holder");
	ASSERT_NE(probe, nullptr);
	probe->mirror.count = 8;

	Variant fast(9.0f);
	ASSERT_TRUE(SetField(probe, _type, "speed"_hash, &fast));
	FlushProbes();
	EXPECT_FLOAT_EQ(probe->mirror.speed, 9.0f);
	EXPECT_EQ(probe->mirror.count, 8);

	probe->mirror.speed = 11.0f;
	Variant read;
	ASSERT_TRUE(GetField(probe, _type, "speed"_hash, &read));
	ASSERT_TRUE(read.Is<f32>());
	EXPECT_FLOAT_EQ(read.As<f32>(), 11.0f);

	Variant wrong(5);
	EXPECT_FALSE(SetField(probe, _type, "speed"_hash, &wrong));
	FlushProbes();
	EXPECT_FLOAT_EQ(probe->mirror.speed, 11.0f);

	probe->mirror.offset = Vec3(7.0f, 8.0f, 9.0f);
	u32 boxed = 0;
	ForEachField(
		probe, _type,
		[&boxed](const FieldInfo* field, const Variant* value) {
			if (field->name == "offset"_hash) {
				ASSERT_TRUE(value->Is<Vec3>());
				EXPECT_FLOAT_EQ(value->As<Vec3>().z, 9.0f);
				++boxed;
			}
		},
		FieldWalkFlagNone);
	EXPECT_EQ(boxed, 1u);
}

TEST_F(FieldSyncTest, PrefabOverrideReachesAFieldWithoutAccessors) {
	Handle<SceneObject> source;
	SyncProbe* authored = AddProbe("Unit", &source);
	ASSERT_NE(authored, nullptr);
	authored->mirror.count = 30;
	const Guid prefab = SceneSystem::SavePrefab(source, Path("project://unit.prefab"));
	ASSERT_TRUE(prefab.IsValid());
	SceneSystem::Destroy(source);
	SceneSystem::EndFrame();

	Handle<SceneObject> instance = SceneSystem::Instantiate(prefab, _scene);
	ASSERT_TRUE(instance.IsValid());
	FlushProbes();
	SyncProbe* live = ProbeOf(_scene, "Unit");
	ASSERT_NE(live, nullptr);
	EXPECT_EQ(live->mirror.count, 30);
	live->mirror.speed = 42.0f;

	ASSERT_TRUE(SceneSystem::SaveScene(_scene, Path("project://override.scene")));
	SceneSystem::UnloadScene(_scene);
	SceneSystem::EndFrame();
	Handle<Scene> reloaded = SceneSystem::LoadScene(Path("project://override.scene"));
	ASSERT_TRUE(reloaded.IsValid());
	FlushProbes();

	SyncProbe* loaded = ProbeOf(reloaded, "Unit");
	ASSERT_NE(loaded, nullptr);
	EXPECT_FLOAT_EQ(loaded->mirror.speed, 42.0f);
	EXPECT_EQ(loaded->mirror.count, 30);
	EXPECT_TRUE(loaded->calls.ends_with("rw")) << loaded->calls;
}

TEST_F(FieldSyncTest, HandleFieldIsPulledAfterFixups) {
	Handle<SceneObject> other = SceneSystem::CreateObject("Other", _scene);
	SyncProbe* probe = AddProbe("Holder");
	ASSERT_NE(probe, nullptr);
	probe->mirror.target = other.GetInstanceId();
	const u64 oldTarget = other.GetInstanceId();

	Buffer snapshot;
	ASSERT_TRUE(SceneSystem::SaveSceneToMemory(_scene, snapshot));
	ASSERT_TRUE(SceneSystem::SaveScene(_scene, Path("project://handles.scene"), true));
	SceneSystem::UnloadScene(_scene);
	SceneSystem::EndFrame();

	Handle<Scene> restored = SceneSystem::LoadSceneFromMemory(snapshot);
	ASSERT_TRUE(restored.IsValid());
	FlushProbes();
	SyncProbe* loaded = ProbeOf(restored, "Holder");
	ASSERT_NE(loaded, nullptr);
	EXPECT_NE(loaded->mirror.target, 0u);
	EXPECT_NE(loaded->mirror.target, oldTarget);
	EXPECT_EQ(loaded->mirror.target, restored->FindObjectByName("Other").GetInstanceId());
	SceneSystem::UnloadScene(restored);
	SceneSystem::EndFrame();

	JobSystem::Initialize();
	SceneSystem::SetAsyncLoadBudget(0.0);
	Handle<SceneLoadOperation> operation = SceneSystem::LoadSceneAsync(Path("project://handles.scene"));
	ASSERT_TRUE(operation.IsValid());
	u32 flushedMidLoad = 0;
	const auto start = std::chrono::steady_clock::now();
	while (!operation->IsDone() && std::chrono::steady_clock::now() - start < std::chrono::seconds(30)) {
		SceneSystem::BeginFrame();
		flushedMidLoad += !operation->IsDone() && !_dirtyProbes.empty() ? 1u : 0u;
		FlushProbes();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	SceneSystem::SetAsyncLoadBudget(0.002);
	ASSERT_TRUE(operation->IsDone());
	Handle<Scene> streamed = operation->GetScene();
	ASSERT_TRUE(streamed.IsValid());
	FlushProbes();
	EXPECT_GE(flushedMidLoad, 1u);

	SyncProbe* late = ProbeOf(streamed, "Holder");
	ASSERT_NE(late, nullptr);
	EXPECT_NE(late->mirror.target, 0u);
	EXPECT_EQ(late->mirror.target, streamed->FindObjectByName("Other").GetInstanceId());
	SceneSystem::Terminate();
	SceneSystem::Initialize();
	JobSystem::Terminate();
}

#ifndef SHALLTEAR_SHARED

TEST_F(FieldSyncTest, AnimatedWriteSyncs) {
	SyncProbe* probe = AddProbe("Holder");
	ASSERT_NE(probe, nullptr);
	probe->mirror.count = 77;
	probe->mirror.offset = Vec3(1.0f, 2.0f, 3.0f);
	probe->calls.clear();

	ASSERT_TRUE(AnimationImpl::WriteAnimatedField(probe, FindField(_type, "speed"_hash), Vec4(5.0f, 0.0f, 0.0f, 0.0f), ANIMATION_WHOLE_VALUE));
	EXPECT_EQ(probe->calls, "rw");
	EXPECT_EQ(probe->pushes, 1u);
	ASSERT_TRUE(AnimationImpl::WriteAnimatedField(probe, FindField(_type, "offset"_hash), Vec4(9.0f, 0.0f, 0.0f, 0.0f), 1));
	EXPECT_EQ(probe->pushes, 1u);
	FlushProbes();
	EXPECT_EQ(probe->pulls, 1u);
	EXPECT_FLOAT_EQ(probe->mirror.speed, 5.0f);
	EXPECT_EQ(probe->mirror.count, 77);
	EXPECT_FLOAT_EQ(probe->mirror.offset.x, 1.0f);
	EXPECT_FLOAT_EQ(probe->mirror.offset.y, 9.0f);
	EXPECT_FLOAT_EQ(probe->mirror.offset.z, 3.0f);
}

#endif
