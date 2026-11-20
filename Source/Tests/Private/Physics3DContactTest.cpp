#include "TestsPCH.h"

#include "TestPlatform.h"

#include "Shalltear/Core/Frame/FrameStages.h"
#include "Shalltear/Core/Module.h"
#include "Shalltear/Core/Module/ModuleAPI.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/Settings.h"
#include "Shalltear/Core/Time.h"

#include <atomic>
#include <chrono>
#include <filesystem>

#ifndef SHALLTEAR_SHARED
#include "Shalltear/Physics3D/Rigidbody.h"
SLT_DECLARE_LINKED_PACKAGE(Physics3D)
#endif

using namespace Shalltear;

namespace {

	constexpr const char* PACKAGE_TARGET = "Physics3D";
	constexpr const char* LISTENER_MODULE_DLL = "ShalltearPhysicsListenerModule" SLT_TEST_DLL_SUFFIX;

	constexpr const char* RIGIDBODY_TYPE_NAME = "Rigidbody";
	constexpr const char* SPHERE_COLLIDER_TYPE_NAME = "SphereCollider";
	constexpr const char* BOX_COLLIDER_TYPE_NAME = "BoxCollider";
	constexpr const char* PROBE_TYPE_NAME = "ContactProbe";

	constexpr f32 FIXED_STEP = 1.0f / 60.0f;

	const FieldInfo* FindTypeField(const TypeInfo* type, std::string_view fieldName) {
		StringHash wanted(fieldName);
		for (const TypeInfo* level = type; level; level = level->parent) {
			for (u32 index = 0; index < level->fieldCount; ++index) {
				if (level->fields[index].name == wanted) {
					return &level->fields[index];
				}
			}
		}
		return nullptr;
	}

	template <typename T>
	bool ReadField(const TypeInfo* type, const void* instance, std::string_view fieldName, T& out) {
		const FieldInfo* field = FindTypeField(type, fieldName);
		if (!field || !instance || !field->addrFn) {
			return false;
		}
		out = *static_cast<const T*>(field->addrFn(const_cast<void*>(instance)));
		return true;
	}

	template <typename T>
	bool WriteField(const TypeInfo* type, void* instance, std::string_view fieldName, const T& value) {
		const FieldInfo* field = FindTypeField(type, fieldName);
		if (!field || !instance || !field->addrFn) {
			return false;
		}
		*static_cast<T*>(field->addrFn(instance)) = value;
		return true;
	}

	class Physics3DContactFixture : public ::testing::Test {
	protected:
		std::filesystem::path _root;
		SettingsPaths _paths;
		Handle<Scene> _scene;
		const TypeInfo* _rigidbodyType = nullptr;
		const TypeInfo* _sphereType = nullptr;
		const TypeInfo* _boxType = nullptr;
		const TypeInfo* _probeType = nullptr;
		ModuleHandle _listenerModule;
#ifdef SHALLTEAR_SHARED
		ModuleHandle _module;
#else
		char _token = 0;
#endif

		void SetUp() override {
			static std::atomic<u64> counter{0};
			u64 unique = counter.fetch_add(1);
			u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
			_root = std::filesystem::temp_directory_path() /
					("shalltear_physics3d_contact_" + std::to_string(stamp) + "_" + std::to_string(unique));
			std::filesystem::create_directories(_root);
			_paths.project = Path((_root / "ProjectSettings.ini").generic_string());
			_paths.user = Path((_root / "UserSettings.ini").generic_string());
			_paths.userProject = Path((_root / "UserProjectSettings.ini").generic_string());

			TypeRegistry::InitializePrimitives();
			Settings::Initialize(_paths);
			SceneSystem::Initialize();
			ModuleSystem::Initialize();

			ASSERT_TRUE(StandUp());

			_listenerModule = ModuleSystem::Load(Path(LISTENER_MODULE_DLL));
			ASSERT_TRUE(ModuleSystem::IsLoaded(_listenerModule));

			_rigidbodyType = TypeRegistry::FindType(StringHash(RIGIDBODY_TYPE_NAME));
			_sphereType = TypeRegistry::FindType(StringHash(SPHERE_COLLIDER_TYPE_NAME));
			_boxType = TypeRegistry::FindType(StringHash(BOX_COLLIDER_TYPE_NAME));
			_probeType = TypeRegistry::FindType(StringHash(PROBE_TYPE_NAME));
			ASSERT_NE(_rigidbodyType, nullptr);
			ASSERT_NE(_sphereType, nullptr);
			ASSERT_NE(_boxType, nullptr);
			ASSERT_NE(_probeType, nullptr);

			_scene = SceneSystem::CreateScene("physics-contact");
			ASSERT_TRUE(_scene.IsValid());
#ifndef SHALLTEAR_SHARED
			if (!TestPlatform::StaticModulesShareTypeInfo()) {
				GTEST_SKIP() << "listener module has its copy of the interface's type info in a static tree";
			}
#endif
		}

		void TearDown() override {
			SceneSystem::Terminate();
#ifndef SHALLTEAR_SHARED
			FrameStages::UnregisterOwnedBy(&_token);
#endif
			ModuleSystem::Terminate();
			Settings::Terminate();
			std::error_code ec;
			std::filesystem::remove_all(_root, ec);
		}

		bool StandUp() {
#ifdef SHALLTEAR_SHARED
			_module = ModuleSystem::Load(Path(str(PACKAGE_TARGET) + SLT_TEST_DLL_SUFFIX), ModuleLoadPolicy::InPlace);
			return ModuleSystem::IsLoaded(_module);
#else
			SLT_CALL_LINKED_PACKAGE(Physics3D, &_token);
			return true;
#endif
		}

		Handle<SceneObject> MakeSphere(std::string_view name, const Vec3& position, f32 radius, bool dynamicBody,
									   bool listener, bool trigger = false, bool frozen = true) {
			Handle<SceneObject> object = SceneSystem::CreateObject(str(name), _scene);
			if (!object.IsValid()) {
				return {};
			}
			object->SetActive(false);
			object->SetWorldPosition(position);

			if (dynamicBody) {
				Handle<Component> body = object->AddComponent(_rigidbodyType);
				if (!body.Get()) {
					return {};
				}
				WriteField(_rigidbodyType, body.Get(), "_useGravity", false);
				if (frozen) {
					WriteField(_rigidbodyType, body.Get(), "_freezePositionX", true);
					WriteField(_rigidbodyType, body.Get(), "_freezePositionY", true);
					WriteField(_rigidbodyType, body.Get(), "_freezePositionZ", true);
				}
			}
			if (listener) {
				object->AddComponent(_probeType);
			}

			Handle<Component> collider = object->AddComponent(_sphereType);
			if (!collider.Get()) {
				return {};
			}
			WriteField(_sphereType, collider.Get(), "_radius", radius);
			WriteField(_sphereType, collider.Get(), "_isTrigger", trigger);
			object->SetActive(true);
			return object;
		}

		Handle<SceneObject> MakeKinematicSphere(std::string_view name, const Vec3& position, f32 radius,
												bool listener) {
			Handle<SceneObject> object = SceneSystem::CreateObject(str(name), _scene);
			if (!object.IsValid()) {
				return {};
			}
			object->SetActive(false);
			object->SetWorldPosition(position);
			Handle<Component> body = object->AddComponent(_rigidbodyType);
			WriteField(_rigidbodyType, body.Get(), "_isKinematic", true);
			WriteField(_rigidbodyType, body.Get(), "_useGravity", false);
			if (listener) {
				object->AddComponent(_probeType);
			}
			Handle<Component> collider = object->AddComponent(_sphereType);
			WriteField(_sphereType, collider.Get(), "_radius", radius);
			object->SetActive(true);
			return object;
		}

		void* ProbeOn(Handle<SceneObject> object) const {
			SceneObject* resolved = object.Get();
			if (!resolved) {
				return nullptr;
			}
			for (u32 index = 0; index < resolved->GetComponentCount(); ++index) {
				Handle<Component> component = resolved->GetComponentAt(index);
				Component* raw = component.Get();
				if (raw && raw->GetType() == _probeType) {
					return raw;
				}
			}
			return nullptr;
		}

		i32 CountOn(Handle<SceneObject> object, std::string_view fieldName) const {
			void* probe = ProbeOn(object);
			if (!probe) {
				return -1;
			}
			i32 value = -1;
			return ReadField(_probeType, probe, fieldName, value) ? value : -1;
		}

		Handle<SceneObject> MakePlatform(std::string_view name, const Vec3& position, const Vec3& size,
										 bool carry) {
			Handle<SceneObject> object = SceneSystem::CreateObject(str(name), _scene);
			if (!object.IsValid()) {
				return {};
			}
			object->SetActive(false);
			object->SetWorldPosition(position);
			Handle<Component> body = object->AddComponent(_rigidbodyType);
			WriteField(_rigidbodyType, body.Get(), "_isKinematic", true);
			WriteField(_rigidbodyType, body.Get(), "_carryRiders", carry);
			Handle<Component> collider = object->AddComponent(_boxType);
			WriteField(_boxType, collider.Get(), "_size", size);
			object->SetActive(true);
			return object;
		}

		Handle<SceneObject> MakeCrate(std::string_view name, const Vec3& position, const Vec3& size, bool carry,
									  bool pinnedAndWeightless = false) {
			Handle<SceneObject> object = SceneSystem::CreateObject(str(name), _scene);
			if (!object.IsValid()) {
				return {};
			}
			object->SetActive(false);
			object->SetWorldPosition(position);
			Handle<Component> body = object->AddComponent(_rigidbodyType);
			WriteField(_rigidbodyType, body.Get(), "_mass", 70.0f);
			WriteField(_rigidbodyType, body.Get(), "_carryRiders", carry);
			if (pinnedAndWeightless) {
				WriteField(_rigidbodyType, body.Get(), "_useGravity", false);
				WriteField(_rigidbodyType, body.Get(), "_freezePositionX", true);
				WriteField(_rigidbodyType, body.Get(), "_freezePositionZ", true);
			}
			Handle<Component> collider = object->AddComponent(_boxType);
			WriteField(_boxType, collider.Get(), "_size", size);
			object->SetActive(true);
			return object;
		}

		static f32 DriveAlongX(Handle<SceneObject> platform, const Vec3& base, u32 steps) {
			f32 velocity = 0.0f;
			f32 x = 0.0f;
			for (u32 step = 0; step < steps; ++step) {
				const f32 seconds = static_cast<f32>(step) * FIXED_STEP;
				f32 acceleration = 0.0f;
				if (seconds >= 1.0f && seconds < 1.5f) {
					acceleration = 8.0f;
				} else if (seconds >= 2.0f && seconds < 3.0f) {
					acceleration = -8.0f;
				} else if (seconds >= 3.0f && seconds < 3.5f) {
					acceleration = 8.0f;
				}
				velocity += acceleration * FIXED_STEP;
				x += velocity * FIXED_STEP;
				platform->SetWorldPosition(Vec3{base.x + x, base.y, base.z});
				RunFixedSteps(1);
			}
			return base.x + x;
		}

		static void RunFixedSteps(u32 count) {
			FrameTime clockState;
			clockState.fixedDeltaTime = FIXED_STEP;
			clockState.maximumDeltaTime = 1000.0f;
			Time::FrameScope clock{clockState};
			Time::BeginFrame(FIXED_STEP);

			FrameStageArgs args;
			for (u32 index = 0; index < count; ++index) {
				FrameStages::Run(FrameStage::FixedUpdate, args);
			}
		}
	};

} // anonymous namespace

TEST_F(Physics3DContactFixture, ListenerCompiledIntoModuleDLLReceivesMessages) {
	Handle<SceneObject> floor = MakeSphere("Floor", Vec3{}, 0.5f, false, false);
	Handle<SceneObject> box = MakeSphere("Box", Vec3{0.0f, 0.6f, 0.0f}, 0.5f, true, true);
	ASSERT_TRUE(floor.IsValid());
	ASSERT_TRUE(box.IsValid());

	RunFixedSteps(1);

	EXPECT_EQ(CountOn(box, "collisionEnter"), 1);

	void* probe = ProbeOn(box);
	ASSERT_NE(probe, nullptr);
	str other;
	ASSERT_TRUE(ReadField(_probeType, probe, "lastOtherName", other));
	EXPECT_EQ(other, "Floor");
}

TEST_F(Physics3DContactFixture, EnterOnceThenStay) {
	Handle<SceneObject> floor = MakeSphere("Floor", Vec3{}, 0.5f, false, false);
	Handle<SceneObject> box = MakeSphere("Box", Vec3{0.0f, 0.6f, 0.0f}, 0.5f, true, true);
	ASSERT_TRUE(floor.IsValid());
	ASSERT_TRUE(box.IsValid());

	RunFixedSteps(1);
	EXPECT_EQ(CountOn(box, "collisionEnter"), 1);
	EXPECT_EQ(CountOn(box, "collisionStay"), 0);

	RunFixedSteps(3);
	EXPECT_EQ(CountOn(box, "collisionEnter"), 1);
	EXPECT_EQ(CountOn(box, "collisionStay"), 3);
	EXPECT_EQ(CountOn(box, "collisionExit"), 0);
}

TEST_F(Physics3DContactFixture, ExitArrivesOnceWhenThePairSeparates) {
	Handle<SceneObject> floor = MakeSphere("Floor", Vec3{}, 0.5f, false, false);
	Handle<SceneObject> box = MakeSphere("Box", Vec3{0.0f, 0.6f, 0.0f}, 0.5f, true, true);
	ASSERT_TRUE(floor.IsValid());
	ASSERT_TRUE(box.IsValid());

	RunFixedSteps(2);
	ASSERT_EQ(CountOn(box, "collisionEnter"), 1);

	box->SetWorldPosition(Vec3{0.0f, 20.0f, 0.0f});
	RunFixedSteps(2);

	EXPECT_EQ(CountOn(box, "collisionExit"), 1);
	const i32 stayAtSeparation = CountOn(box, "collisionStay");
	RunFixedSteps(2);
	EXPECT_EQ(CountOn(box, "collisionStay"), stayAtSeparation);
	EXPECT_EQ(CountOn(box, "collisionExit"), 1);
}

TEST_F(Physics3DContactFixture, BothSidesOfOnePairAreMessaged) {
	Handle<SceneObject> floor = MakeSphere("Floor", Vec3{}, 0.5f, false, true);
	Handle<SceneObject> box = MakeSphere("Box", Vec3{0.0f, 0.6f, 0.0f}, 0.5f, true, true);
	ASSERT_TRUE(floor.IsValid());
	ASSERT_TRUE(box.IsValid());

	RunFixedSteps(1);

	EXPECT_EQ(CountOn(box, "collisionEnter"), 1);
	EXPECT_EQ(CountOn(floor, "collisionEnter"), 1);

	void* boxProbe = ProbeOn(box);
	void* floorProbe = ProbeOn(floor);
	ASSERT_NE(boxProbe, nullptr);
	ASSERT_NE(floorProbe, nullptr);
	str seenByBox;
	str seenByFloor;
	ASSERT_TRUE(ReadField(_probeType, boxProbe, "lastOtherName", seenByBox));
	ASSERT_TRUE(ReadField(_probeType, floorProbe, "lastOtherName", seenByFloor));
	EXPECT_EQ(seenByBox, "Floor");
	EXPECT_EQ(seenByFloor, "Box");
}

TEST_F(Physics3DContactFixture, TheNormalPointsAwayFromEachListener) {
	Handle<SceneObject> floor = MakeSphere("Floor", Vec3{}, 0.5f, false, true);
	Handle<SceneObject> box = MakeSphere("Box", Vec3{0.0f, 0.6f, 0.0f}, 0.5f, true, true);
	ASSERT_TRUE(floor.IsValid());
	ASSERT_TRUE(box.IsValid());

	RunFixedSteps(1);

	void* boxProbe = ProbeOn(box);
	void* floorProbe = ProbeOn(floor);
	ASSERT_NE(boxProbe, nullptr);
	ASSERT_NE(floorProbe, nullptr);

	i32 boxContacts = 0;
	ASSERT_TRUE(ReadField(_probeType, boxProbe, "lastContactCount", boxContacts));
	ASSERT_GT(boxContacts, 0);

	f32 boxNormalY = 0.0f;
	f32 floorNormalY = 0.0f;
	ASSERT_TRUE(ReadField(_probeType, boxProbe, "lastNormalY", boxNormalY));
	ASSERT_TRUE(ReadField(_probeType, floorProbe, "lastNormalY", floorNormalY));

	EXPECT_LT(boxNormalY, 0.0f);
	EXPECT_GT(floorNormalY, 0.0f);
}

TEST_F(Physics3DContactFixture, StaticColliderIsNamedWithNoRigidbody) {
	Handle<SceneObject> floor = MakeSphere("Floor", Vec3{}, 0.5f, false, false);
	Handle<SceneObject> box = MakeSphere("Box", Vec3{0.0f, 0.6f, 0.0f}, 0.5f, true, true);
	ASSERT_TRUE(floor.IsValid());
	ASSERT_TRUE(box.IsValid());

	RunFixedSteps(1);

	void* probe = ProbeOn(box);
	ASSERT_NE(probe, nullptr);
	bool hadRigidbody = true;
	ASSERT_TRUE(ReadField(_probeType, probe, "lastHadRigidbody", hadRigidbody));
	EXPECT_FALSE(hadRigidbody);

	Handle<SceneObject> other = MakeSphere("Other", Vec3{0.0f, 0.55f, 0.0f}, 0.5f, true, false);
	ASSERT_TRUE(other.IsValid());
	RunFixedSteps(1);
	ASSERT_TRUE(ReadField(_probeType, probe, "lastHadRigidbody", hadRigidbody));
	EXPECT_TRUE(hadRigidbody);
}

TEST_F(Physics3DContactFixture, TriggerReportsBothSidesAndNoCollision) {
	Handle<SceneObject> volume = MakeSphere("Volume", Vec3{}, 0.5f, false, true, true);
	Handle<SceneObject> visitor = MakeSphere("Visitor", Vec3{0.0f, 0.4f, 0.0f}, 0.5f, true, true);
	ASSERT_TRUE(volume.IsValid());
	ASSERT_TRUE(visitor.IsValid());

	RunFixedSteps(1);
	EXPECT_EQ(CountOn(volume, "triggerEnter"), 1);
	EXPECT_EQ(CountOn(visitor, "triggerEnter"), 1);
	EXPECT_EQ(CountOn(volume, "collisionEnter"), 0);
	EXPECT_EQ(CountOn(visitor, "collisionEnter"), 0);

	RunFixedSteps(3);
	EXPECT_EQ(CountOn(volume, "triggerEnter"), 1);
	EXPECT_EQ(CountOn(volume, "triggerStay"), 3);

	visitor->SetWorldPosition(Vec3{0.0f, 20.0f, 0.0f});
	RunFixedSteps(2);
	EXPECT_EQ(CountOn(volume, "triggerExit"), 1);
	EXPECT_EQ(CountOn(visitor, "triggerExit"), 1);
}

TEST_F(Physics3DContactFixture, ResolverSpeaksForOtherComponents) {
#ifndef SHALLTEAR_SHARED
	GTEST_SKIP() << "listener module registers with its copy of Physics 3D in a static tree";
#endif
	const TypeInfo* resolved = TypeRegistry::FindType(StringHash("ResolvedProbe"));
	ASSERT_NE(resolved, nullptr);
	const auto count = [resolved](const Handle<Component>& probe, std::string_view field) {
		i32 value = -1;
		ReadField(resolved, probe.Get(), field, value);
		return value;
	};

	Handle<SceneObject> floor = MakeSphere("Floor", Vec3{}, 0.5f, false, false);
	Handle<SceneObject> box = MakeSphere("Box", Vec3{0.0f, 0.6f, 0.0f}, 0.5f, true, false);
	ASSERT_TRUE(floor.IsValid());
	ASSERT_TRUE(box.IsValid());
	Handle<Component> probe = box->AddComponent(resolved);
	ASSERT_TRUE(probe.IsValid());

	RunFixedSteps(3);
	EXPECT_EQ(count(probe, "collisionEnter"), 1);
	EXPECT_EQ(count(probe, "collisionStay"), 2);

	box->SetWorldPosition(Vec3{0.0f, 20.0f, 0.0f});
	RunFixedSteps(2);
	EXPECT_EQ(count(probe, "collisionExit"), 1);

	Handle<SceneObject> volume = MakeSphere("Volume", Vec3{10.0f, 0.0f, 0.0f}, 0.5f, false, false, true);
	Handle<SceneObject> visitor = MakeSphere("Visitor", Vec3{10.0f, 0.4f, 0.0f}, 0.5f, true, false);
	ASSERT_TRUE(volume.IsValid());
	ASSERT_TRUE(visitor.IsValid());
	Handle<Component> heard = volume->AddComponent(resolved);
	ASSERT_TRUE(heard.IsValid());
	RunFixedSteps(1);
	EXPECT_EQ(count(heard, "triggerEnter"), 1);
	EXPECT_EQ(count(heard, "collisionEnter"), 0);
	EXPECT_EQ(count(probe, "triggerEnter"), 0);
}

TEST_F(Physics3DContactFixture, TriggerHearsVisitorThatIsNotListening) {
	Handle<SceneObject> volume = MakeSphere("Volume", Vec3{}, 0.5f, false, true, true);
	Handle<SceneObject> visitor = MakeSphere("Visitor", Vec3{0.0f, 0.4f, 0.0f}, 0.5f, true, false);
	ASSERT_TRUE(volume.IsValid());
	ASSERT_TRUE(visitor.IsValid());

	RunFixedSteps(1);
	EXPECT_EQ(CountOn(volume, "triggerEnter"), 1);

	void* probe = ProbeOn(volume);
	ASSERT_NE(probe, nullptr);
	str other;
	ASSERT_TRUE(ReadField(_probeType, probe, "lastOtherName", other));
	EXPECT_EQ(other, "Visitor");
}

TEST_F(Physics3DContactFixture, StaticPairIsSilent) {
	Handle<SceneObject> wallA = MakeSphere("WallA", Vec3{}, 0.5f, false, true);
	Handle<SceneObject> wallB = MakeSphere("WallB", Vec3{0.0f, 0.6f, 0.0f}, 0.5f, false, true);
	ASSERT_TRUE(wallA.IsValid());
	ASSERT_TRUE(wallB.IsValid());

	RunFixedSteps(2);
	EXPECT_EQ(CountOn(wallA, "collisionEnter"), 0);
	EXPECT_EQ(CountOn(wallB, "collisionEnter"), 0);

	Handle<SceneObject> liftA = MakeKinematicSphere("LiftA", Vec3{10.0f, 0.0f, 0.0f}, 0.5f, true);
	Handle<SceneObject> liftB = MakeKinematicSphere("LiftB", Vec3{10.0f, 0.6f, 0.0f}, 0.5f, true);
	ASSERT_TRUE(liftA.IsValid());
	ASSERT_TRUE(liftB.IsValid());
	RunFixedSteps(2);
	EXPECT_EQ(CountOn(liftA, "collisionEnter"), 0);

	Handle<SceneObject> crate = MakeSphere("Crate", Vec3{0.0f, 0.55f, 0.0f}, 0.5f, true, true);
	ASSERT_TRUE(crate.IsValid());
	RunFixedSteps(1);
	EXPECT_GT(CountOn(crate, "collisionEnter"), 0);
}

TEST_F(Physics3DContactFixture, DestroyEndsPair) {
	Handle<SceneObject> floor = MakeSphere("Floor", Vec3{}, 0.5f, false, true);
	Handle<SceneObject> box = MakeSphere("Box", Vec3{0.0f, 0.6f, 0.0f}, 0.5f, true, false);
	ASSERT_TRUE(floor.IsValid());
	ASSERT_TRUE(box.IsValid());

	RunFixedSteps(1);
	ASSERT_EQ(CountOn(floor, "collisionEnter"), 1);

	SceneSystem::Destroy(box);
	SceneSystem::EndFrame();
	RunFixedSteps(1);

	EXPECT_EQ(CountOn(floor, "collisionExit"), 1);

	const i32 exitCount = CountOn(floor, "collisionExit");
	const i32 stayCount = CountOn(floor, "collisionStay");
	RunFixedSteps(3);
	EXPECT_EQ(CountOn(floor, "collisionExit"), exitCount);
	EXPECT_EQ(CountOn(floor, "collisionStay"), stayCount);
}

TEST_F(Physics3DContactFixture, DisablingAColliderEndsItsPair) {
	Handle<SceneObject> floor = MakeSphere("Floor", Vec3{}, 0.5f, false, true);
	Handle<SceneObject> box = MakeSphere("Box", Vec3{0.0f, 0.6f, 0.0f}, 0.5f, true, false);
	ASSERT_TRUE(floor.IsValid());
	ASSERT_TRUE(box.IsValid());

	RunFixedSteps(1);
	ASSERT_EQ(CountOn(floor, "collisionEnter"), 1);

	box->SetActive(false);
	RunFixedSteps(1);
	EXPECT_EQ(CountOn(floor, "collisionExit"), 1);

	const i32 stayCount = CountOn(floor, "collisionStay");
	RunFixedSteps(3);
	EXPECT_EQ(CountOn(floor, "collisionStay"), stayCount);
}

TEST_F(Physics3DContactFixture, StayStopsOnceTheBodySleeps) {
	Handle<SceneObject> floor = MakeSphere("Floor", Vec3{}, 0.5f, false, false);
	Handle<SceneObject> box = MakeSphere("Box", Vec3{0.0f, 0.6f, 0.0f}, 0.5f, true, true);
	ASSERT_TRUE(floor.IsValid());
	ASSERT_TRUE(box.IsValid());

	RunFixedSteps(1);
	ASSERT_EQ(CountOn(box, "collisionEnter"), 1);

	RunFixedSteps(60);
	const i32 settled = CountOn(box, "collisionStay");
	ASSERT_GT(settled, 0);

	RunFixedSteps(20);
	EXPECT_EQ(CountOn(box, "collisionStay"), settled);
}

TEST_F(Physics3DContactFixture, StaticTriggerAgainstStaticGeometryIsSilent) {
	Handle<SceneObject> volume = MakeSphere("Volume", Vec3{}, 0.5f, false, true, true);
	Handle<SceneObject> wall = MakeSphere("Wall", Vec3{0.0f, 0.4f, 0.0f}, 0.5f, false, true);
	ASSERT_TRUE(volume.IsValid());
	ASSERT_TRUE(wall.IsValid());

	RunFixedSteps(2);
	EXPECT_EQ(CountOn(volume, "triggerEnter"), 0);
	EXPECT_EQ(CountOn(wall, "triggerEnter"), 0);

	Handle<SceneObject> visitor = MakeSphere("Visitor", Vec3{0.0f, 0.45f, 0.0f}, 0.5f, true, false);
	ASSERT_TRUE(visitor.IsValid());
	RunFixedSteps(1);
	EXPECT_EQ(CountOn(volume, "triggerEnter"), 1);
}

TEST_F(Physics3DContactFixture, TurningLastDynamicBodyKinematicSilencesPair) {
	Handle<SceneObject> floor = MakeSphere("Floor", Vec3{}, 0.5f, false, true);
	Handle<SceneObject> box = MakeSphere("Box", Vec3{0.0f, 0.6f, 0.0f}, 0.5f, true, true);
	ASSERT_TRUE(floor.IsValid());
	ASSERT_TRUE(box.IsValid());

	RunFixedSteps(2);
	const i32 stayBefore = CountOn(floor, "collisionStay");
	ASSERT_GT(stayBefore, 0);

	SceneObject* resolved = box.Get();
	ASSERT_NE(resolved, nullptr);
	void* body = nullptr;
	for (u32 index = 0; index < resolved->GetComponentCount(); ++index) {
		Component* component = resolved->GetComponentAt(index).Get();
		if (component && component->GetType() == _rigidbodyType) {
			body = component;
		}
	}
	ASSERT_NE(body, nullptr);
	ASSERT_TRUE(WriteField(_rigidbodyType, body, "_isKinematic", true));

	RunFixedSteps(3);
	EXPECT_EQ(CountOn(floor, "collisionStay"), stayBefore);
}

TEST_F(Physics3DContactFixture, DisabledListenerStillHears) {
	Handle<SceneObject> floor = MakeSphere("Floor", Vec3{}, 0.5f, false, false);
	Handle<SceneObject> box = MakeSphere("Box", Vec3{0.0f, 0.6f, 0.0f}, 0.5f, true, true);
	ASSERT_TRUE(floor.IsValid());
	ASSERT_TRUE(box.IsValid());

	SceneObject* resolved = box.Get();
	ASSERT_NE(resolved, nullptr);
	Handle<Component> probe;
	for (u32 index = 0; index < resolved->GetComponentCount(); ++index) {
		Handle<Component> component = resolved->GetComponentAt(index);
		if (component.Get() && component.Get()->GetType() == _probeType) {
			probe = component;
		}
	}
	ASSERT_TRUE(probe.IsValid());

	probe.Get()->SetEnabled(false);
	RunFixedSteps(2);
	EXPECT_EQ(CountOn(box, "collisionEnter"), 1);
	const i32 stayWhileDisabled = CountOn(box, "collisionStay");
	EXPECT_GT(stayWhileDisabled, 0);

	SceneSystem::Destroy(probe);
	RunFixedSteps(2);
	EXPECT_EQ(CountOn(box, "collisionStay"), stayWhileDisabled);
}

#ifdef SHALLTEAR_PHYSICS_BACKEND_STUB
#define SLT_SKIP_WITHOUT_CONTACT_RESPONSE() GTEST_SKIP() << "stub backend has no contact response"
#else
#define SLT_SKIP_WITHOUT_CONTACT_RESPONSE() ((void)0)
#endif

TEST_F(Physics3DContactFixture, RiderTracksPlatform) {
	SLT_SKIP_WITHOUT_CONTACT_RESPONSE();

	Handle<SceneObject> carried = MakePlatform("Carried", Vec3{}, Vec3{6.0f, 0.5f, 6.0f}, true);
	Handle<SceneObject> rider = MakeCrate("Rider", Vec3{0.0f, 0.80f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, false);
	ASSERT_TRUE(carried.IsValid());
	ASSERT_TRUE(rider.IsValid());

	const f32 travelled = DriveAlongX(carried, Vec3{}, 240);
	const f32 riderOffset = rider->GetWorldPosition().x - travelled;
	EXPECT_GT(rider->GetWorldPosition().y, 0.0f);
	EXPECT_LT(Math::Abs(riderOffset), 0.05f);

	Handle<SceneObject> plain = MakePlatform("Plain", Vec3{0.0f, 0.0f, 40.0f}, Vec3{6.0f, 0.5f, 6.0f}, false);
	Handle<SceneObject> slider = MakeCrate("Slider", Vec3{0.0f, 0.80f, 40.0f}, Vec3{1.0f, 1.0f, 1.0f}, false);
	ASSERT_TRUE(plain.IsValid());
	ASSERT_TRUE(slider.IsValid());

	const f32 plainTravelled = DriveAlongX(plain, Vec3{0.0f, 0.0f, 40.0f}, 240);
	const f32 sliderOffset = slider->GetWorldPosition().x - plainTravelled;
	EXPECT_GT(Math::Abs(sliderOffset), 0.3f);
	EXPECT_LT(Math::Abs(riderOffset) * 5.0f, Math::Abs(sliderOffset));
}

TEST_F(Physics3DContactFixture, OnlyTopRiderCarried) {
	SLT_SKIP_WITHOUT_CONTACT_RESPONSE();

	Handle<SceneObject> carrying = MakePlatform("Carrying", Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.5f, 2.0f, 4.0f}, true);
	Handle<SceneObject> plain = MakePlatform("Plain", Vec3{0.0f, 1.0f, 40.0f}, Vec3{0.5f, 2.0f, 4.0f}, false);
	ASSERT_TRUE(carrying.IsValid());
	ASSERT_TRUE(plain.IsValid());

	Handle<SceneObject> onTop = MakeCrate("OnTop", Vec3{0.0f, 2.51f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, false);
	ASSERT_TRUE(onTop.IsValid());

	Handle<SceneObject> besideCarrying =
		MakeCrate("BesideCarrying", Vec3{0.749f, 1.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, false, true);
	Handle<SceneObject> besidePlain =
		MakeCrate("BesidePlain", Vec3{0.749f, 1.0f, 40.0f}, Vec3{1.0f, 1.0f, 1.0f}, false, true);
	ASSERT_TRUE(besideCarrying.IsValid());
	ASSERT_TRUE(besidePlain.IsValid());
	besideCarrying->AddComponent(_probeType);

	RunFixedSteps(10);
	const f32 carryingStartY = besideCarrying->GetWorldPosition().y;
	const f32 plainStartY = besidePlain->GetWorldPosition().y;
	const f32 onTopStartY = onTop->GetWorldPosition().y;

	f32 y = 1.0f;
	f32 rise = 0.0f;
	for (u32 step = 0; step < 30; ++step) {
		rise += 4.0f * FIXED_STEP;
		y += rise * FIXED_STEP;
		carrying->SetWorldPosition(Vec3{0.0f, y, 0.0f});
		plain->SetWorldPosition(Vec3{0.0f, y, 40.0f});
		RunFixedSteps(1);
	}

	void* probe = ProbeOn(besideCarrying);
	ASSERT_NE(probe, nullptr);
	str other;
	ASSERT_TRUE(ReadField(_probeType, probe, "lastOtherName", other));
	EXPECT_EQ(other, "Carrying");
	i32 sideContacts = 0;
	ASSERT_TRUE(ReadField(_probeType, probe, "lastContactCount", sideContacts));
	EXPECT_GT(sideContacts, 0);

	for (u32 step = 0; step < 60; ++step) {
		carrying->SetWorldPosition(Vec3{0.0f, y, 0.0f});
		plain->SetWorldPosition(Vec3{0.0f, y, 40.0f});
		RunFixedSteps(1);
	}

	EXPECT_GT(onTop->GetWorldPosition().y - onTopStartY, 0.3f);

	const f32 carriedRise = besideCarrying->GetWorldPosition().y - carryingStartY;
	const f32 frictionRise = besidePlain->GetWorldPosition().y - plainStartY;
	EXPECT_LT(carriedRise - frictionRise, 0.05f);
}

TEST_F(Physics3DContactFixture, RiderKeepsPlatformsVelocityWhenItLeaves) {
	SLT_SKIP_WITHOUT_CONTACT_RESPONSE();

	Handle<SceneObject> platform = MakePlatform("Platform", Vec3{}, Vec3{6.0f, 0.5f, 6.0f}, true);
	Handle<SceneObject> rider = MakeCrate("Rider", Vec3{0.0f, 0.80f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, false);
	ASSERT_TRUE(platform.IsValid());
	ASSERT_TRUE(rider.IsValid());

	f32 x = 0.0f;
	f32 velocity = 0.0f;
	for (u32 step = 0; step < 120; ++step) {
		if (velocity < 4.0f) {
			velocity += 8.0f * FIXED_STEP;
		}
		x += velocity * FIXED_STEP;
		platform->SetWorldPosition(Vec3{x, 0.0f, 0.0f});
		RunFixedSteps(1);
	}
	ASSERT_GT(rider->GetWorldPosition().x, 1.0f);

	SceneSystem::Destroy(platform);
	SceneSystem::EndFrame();

	const f32 launchX = rider->GetWorldPosition().x;
	RunFixedSteps(30);
	const f32 travelled = rider->GetWorldPosition().x - launchX;

	EXPECT_GT(travelled, 1.5f);
	EXPECT_LT(travelled, 2.5f);
}

TEST_F(Physics3DContactFixture, CrateLandsOnPlatform) {
	SLT_SKIP_WITHOUT_CONTACT_RESPONSE();

	Handle<SceneObject> platform = MakePlatform("Platform", Vec3{}, Vec3{6.0f, 0.5f, 6.0f}, false);
	Handle<SceneObject> crate = MakeCrate("Crate", Vec3{0.0f, 0.80f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, false);
	ASSERT_TRUE(platform.IsValid());
	ASSERT_TRUE(crate.IsValid());

	RunFixedSteps(60);

	const Vec3 rest = crate->GetWorldPosition();
	EXPECT_NEAR(rest.y, 0.75f, 0.01f);
	EXPECT_LT(Math::Abs(rest.x) + Math::Abs(rest.z), 0.005f);
}

#ifndef SHALLTEAR_SHARED

TEST_F(Physics3DContactFixture, KinematicPlatformTakesNoPush) {
	SLT_SKIP_WITHOUT_CONTACT_RESPONSE();

	Handle<SceneObject> platform = MakePlatform("Platform", Vec3{}, Vec3{6.0f, 0.5f, 6.0f}, false);
	ASSERT_TRUE(platform.IsValid());
	SceneObject* resolved = platform.Get();
	ASSERT_NE(resolved, nullptr);
	Rigidbody* body = nullptr;
	for (u32 index = 0; index < resolved->GetComponentCount(); ++index) {
		Component* component = resolved->GetComponentAt(index).Get();
		if (component && component->GetType() == _rigidbodyType) {
			body = static_cast<Rigidbody*>(component);
		}
	}
	ASSERT_NE(body, nullptr);
	body->SetKinematic(false);
	body->SetKinematic(true);

	Handle<SceneObject> crate = MakeCrate("Crate", Vec3{0.0f, 0.80f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, false);
	ASSERT_TRUE(crate.IsValid());

	RunFixedSteps(60);

	ASSERT_LT(Math::Abs(platform->GetWorldPosition().y), 1e-4f);
	const Vec3 rest = crate->GetWorldPosition();
	EXPECT_NEAR(rest.y, 0.75f, 0.01f);
	EXPECT_LT(Math::Abs(rest.x) + Math::Abs(rest.z), 0.005f);
}

TEST_F(Physics3DContactFixture, DynamicBodyReportsCollisions) {
	Handle<SceneObject> floor = MakeSphere("Floor", Vec3{}, 0.5f, false, true);
	Handle<SceneObject> ball = MakeKinematicSphere("Ball", Vec3{0.0f, 0.6f, 0.0f}, 0.5f, true);
	ASSERT_TRUE(floor.IsValid());
	ASSERT_TRUE(ball.IsValid());

	RunFixedSteps(3);
	ASSERT_EQ(CountOn(floor, "collisionEnter"), 0);

	SceneObject* resolved = ball.Get();
	ASSERT_NE(resolved, nullptr);
	Rigidbody* body = nullptr;
	for (u32 index = 0; index < resolved->GetComponentCount(); ++index) {
		Component* component = resolved->GetComponentAt(index).Get();
		if (component && component->GetType() == _rigidbodyType) {
			body = static_cast<Rigidbody*>(component);
		}
	}
	ASSERT_NE(body, nullptr);
	body->SetKinematic(false);

	RunFixedSteps(3);
	EXPECT_GT(CountOn(floor, "collisionEnter"), 0);
}

#endif
