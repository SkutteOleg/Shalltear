#include "TestsPCH.h"

#include "TestPlatform.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Frame/FrameStages.h"
#include "Shalltear/Core/Module.h"
#include "Shalltear/Core/Module/ModuleAPI.h"
#include "Shalltear/Core/Object.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/Settings.h"
#include "Shalltear/Core/Time.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <filesystem>

#ifndef SHALLTEAR_SHARED
#include "PhysicsJointMath.h"
#include "PhysicsWorld.h"

#include "Shalltear/Core/Scene/Behavior.h"
#include "Shalltear/Physics3D/Joints.h"
SLT_DECLARE_LINKED_PACKAGE(Physics3D)
#endif

using namespace Shalltear;

namespace {

	constexpr const char* PACKAGE_TARGET = "Physics3D";
	constexpr const char* LISTENER_MODULE_DLL = "ShalltearPhysicsListenerModule" SLT_TEST_DLL_SUFFIX;

	constexpr const char* RIGIDBODY_TYPE_NAME = "Rigidbody";
	constexpr const char* PROBE_TYPE_NAME = "ContactProbe";
	constexpr const char* JOINT_TYPE_NAME = "Joint";
	constexpr const char* HINGE_TYPE_NAME = "HingeJoint";
	constexpr const char* FIXED_TYPE_NAME = "FixedJoint";
	constexpr const char* SPRING_TYPE_NAME = "SpringJoint";
	constexpr const char* CHARACTER_TYPE_NAME = "CharacterJoint";
	constexpr const char* PRISMATIC_TYPE_NAME = "PrismaticJoint";

	constexpr f32 FIXED_STEP = 1.0f / 60.0f;

	constexpr f32 GRAVITY = 9.81f;

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

	const FieldInfo* FindOwnField(const TypeInfo* type, std::string_view fieldName) {
		StringHash wanted(fieldName);
		for (u32 index = 0; index < type->fieldCount; ++index) {
			if (type->fields[index].name == wanted) {
				return &type->fields[index];
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

	class JointCapturingSink : public ILogSink {
	public:
		void ReceiveLog(const LogEvent& event) override {
			std::lock_guard<std::mutex> lock(_mutex);
			_messages.push_back(str(event.message));
		}
		void Flush() override {}

		u32 CountMatching(std::string_view needle) const {
			std::lock_guard<std::mutex> lock(_mutex);
			u32 count = 0;
			for (const str& message : _messages) {
				if (message.find(needle) != str::npos) {
					++count;
				}
			}
			return count;
		}
	private:
		mutable std::mutex _mutex;
		std::vector<str> _messages;
	};

#ifndef SHALLTEAR_SHARED

	constexpr f32 DRIVE_SPRING = 400.0f;
	constexpr f32 DRIVE_DAMPER = 20.0f;

	constexpr u32 CHAIN_LINKS = 3;
	constexpr u32 SETTLE_STEPS = 60;
	constexpr u32 TRACK_STEPS = 240;
	constexpr f32 TRACK_TARGET_DEGREES = 20.0f;

	constexpr f32 MOVING_SPEED = 0.05f;

	class PoseDriver : public Behavior {
	public:
		Vector<Handle<CharacterJoint>> joints;
		Handle<Rigidbody> watched;

		i32 ticks = 0;
		i32 switchTick = -1;
		i32 writeTick = 0;
		i32 firstMovingTick = 0;

		SLT_REFLECT_COMPONENT(PoseDriver, Behavior)
		SLT_END_REFLECT()
	protected:
		void OnFixedUpdate() override {
			++ticks;
			if (writeTick != 0 && firstMovingTick == 0 && watched.IsValid() &&
				watched->GetAngularVelocity().Length() > MOVING_SPEED) {
				firstMovingTick = ticks;
			}

			const bool driving = switchTick >= 0;
			const Quat target =
				Quat::AngleAxis(driving ? TRACK_TARGET_DEGREES : 0.0f, Vec3{0.0f, 0.0f, 1.0f});
			for (Handle<CharacterJoint>& joint : joints) {
				if (joint.IsValid()) {
					joint->SetTargetRotation(target);
				}
			}
			if (driving && writeTick == 0) {
				writeTick = ticks;
			}
		}
	};

	SLT_REGISTER_TYPE(PoseDriver)

#endif

	class Physics3DJointFixture : public ::testing::Test {
	protected:
		std::filesystem::path _root;
		SettingsPaths _paths;
		Handle<Scene> _scene;
		const TypeInfo* _rigidbodyType = nullptr;
		const TypeInfo* _probeType = nullptr;
		const TypeInfo* _hingeType = nullptr;
		const TypeInfo* _fixedType = nullptr;
		const TypeInfo* _springType = nullptr;
		const TypeInfo* _characterType = nullptr;
		const TypeInfo* _prismaticType = nullptr;
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
					("shalltear_physics3d_joint_" + std::to_string(stamp) + "_" + std::to_string(unique));
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
			_probeType = TypeRegistry::FindType(StringHash(PROBE_TYPE_NAME));
			_hingeType = TypeRegistry::FindType(StringHash(HINGE_TYPE_NAME));
			_fixedType = TypeRegistry::FindType(StringHash(FIXED_TYPE_NAME));
			_springType = TypeRegistry::FindType(StringHash(SPRING_TYPE_NAME));
			_characterType = TypeRegistry::FindType(StringHash(CHARACTER_TYPE_NAME));
			_prismaticType = TypeRegistry::FindType(StringHash(PRISMATIC_TYPE_NAME));
			ASSERT_NE(_rigidbodyType, nullptr);
			ASSERT_NE(_probeType, nullptr);
			ASSERT_NE(_hingeType, nullptr);
			ASSERT_NE(_fixedType, nullptr);
			ASSERT_NE(_springType, nullptr);
			ASSERT_NE(_characterType, nullptr);
			ASSERT_NE(_prismaticType, nullptr);

			_scene = SceneSystem::CreateScene("physics-joint");
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

		Handle<SceneObject> MakeJointed(std::string_view name, const Vec3& position, const TypeInfo* jointType,
										Handle<Component>& outJoint, bool listener = false, f32 mass = 1.0f) {
			Handle<SceneObject> object = SceneSystem::CreateObject(str(name), _scene);
			if (!object.IsValid()) {
				return {};
			}
			object->SetActive(false);
			object->SetWorldPosition(position);

			Handle<Component> body = object->AddComponent(_rigidbodyType);
			if (!body.Get()) {
				return {};
			}
			WriteField(_rigidbodyType, body.Get(), "_mass", mass);

			if (listener) {
				object->AddComponent(_probeType);
			}

			outJoint = object->AddComponent(jointType);
			if (!outJoint.Get()) {
				return {};
			}
			return object;
		}

		static void Activate(Handle<SceneObject> object) {
			object->SetActive(true);
		}

		Handle<SceneObject> MakePlainBody(std::string_view name, const Vec3& position) {
			Handle<SceneObject> object = SceneSystem::CreateObject(str(name), _scene);
			if (!object.IsValid()) {
				return {};
			}
			object->SetActive(false);
			object->SetWorldPosition(position);
			object->AddComponent(_rigidbodyType);
			object->SetActive(true);
			return object;
		}

		static Vector<f32> FurthestSwings(const Vector<Handle<SceneObject>>& bodies, u32 steps) {
			Vector<Quat> built;
			Vector<f32> furthest;
			for (const Handle<SceneObject>& body : bodies) {
				built.push_back(body->GetWorldRotation());
				furthest.push_back(0.0f);
			}
			for (u32 step = 0; step < steps; ++step) {
				RunFixedSteps(1);
				for (u32 index = 0; index < bodies.size(); ++index) {
					furthest[index] = Math::Max(furthest[index], Quat::Angle(built[index], bodies[index]->GetWorldRotation()));
				}
			}
			return furthest;
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

#ifndef SHALLTEAR_SHARED
		Handle<Rigidbody> FindBody(Handle<SceneObject> object) const {
			return Handle<Rigidbody>(static_cast<Rigidbody*>(ComponentOn(object, _rigidbodyType).Get()));
		}

		void ArmBreakAndWake(Handle<SceneObject> object, f32 breakForce, f32 breakTorque) {
			auto* joint = static_cast<Joint*>(ComponentOn(object, Joint::GetReflectType()).Get());
			ASSERT_NE(joint, nullptr);
			joint->SetBreakForce(breakForce);
			joint->SetBreakTorque(breakTorque);
			Handle<Rigidbody> body = FindBody(object);
			ASSERT_TRUE(body.IsValid());
			body->WakeUp();
		}
#endif

		Handle<Component> ComponentOn(Handle<SceneObject> object, const TypeInfo* type) const {
			SceneObject* resolved = object.Get();
			if (!resolved) {
				return {};
			}
			for (u32 index = 0; index < resolved->GetComponentCount(); ++index) {
				Handle<Component> component = resolved->GetComponentAt(index);
				Component* raw = component.Get();
				if (raw && TypeRegistry::IsDerivedFrom(raw->GetType(), type)) {
					return component;
				}
			}
			return {};
		}

		i32 ProbeCount(Handle<SceneObject> object, std::string_view field) const {
			Handle<Component> probe = ComponentOn(object, _probeType);
			i32 value = -1;
			ReadField(_probeType, probe.Get(), field, value);
			return value;
		}
	};

} // anonymous namespace

TEST_F(Physics3DJointFixture, JointsCarryNamesAndDefaults) {
	Handle<SceneObject> object = SceneSystem::CreateObject("joints", _scene);
	ASSERT_TRUE(object.IsValid());
	object->SetActive(false);

	const TypeInfo* base = TypeRegistry::FindType(StringHash(JOINT_TYPE_NAME));
	ASSERT_NE(base, nullptr);
	EXPECT_NE(base->flags & TypeFlagAbstract, 0u);
	for (const TypeInfo* type : {_hingeType, _fixedType, _springType, _characterType, _prismaticType}) {
		EXPECT_TRUE(TypeRegistry::IsDerivedFrom(type, base)) << type->displayName;
	}

	Handle<Component> hinge = object->AddComponent(_hingeType);
	ASSERT_NE(hinge.Get(), nullptr);

	Vec3 anchor{1.0f, 1.0f, 1.0f};
	ASSERT_TRUE(ReadField(_hingeType, hinge.Get(), "_anchor", anchor));
	EXPECT_FLOAT_EQ(anchor.x, 0.0f);
	EXPECT_FLOAT_EQ(anchor.y, 0.0f);
	EXPECT_FLOAT_EQ(anchor.z, 0.0f);

	bool autoConfigure = false;
	ASSERT_TRUE(ReadField(_hingeType, hinge.Get(), "_autoConfigureConnectedAnchor", autoConfigure));
	EXPECT_TRUE(autoConfigure);

	bool enableCollision = true;
	ASSERT_TRUE(ReadField(_hingeType, hinge.Get(), "_enableCollision", enableCollision));
	EXPECT_FALSE(enableCollision);

	f32 breakForce = 0.0f;
	ASSERT_TRUE(ReadField(_hingeType, hinge.Get(), "_breakForce", breakForce));
	EXPECT_TRUE(std::isfinite(breakForce));
	EXPECT_GT(breakForce, 1.0e30f);

	f32 breakTorque = 0.0f;
	ASSERT_TRUE(ReadField(_hingeType, hinge.Get(), "_breakTorque", breakTorque));
	EXPECT_FLOAT_EQ(breakTorque, breakForce);

	const FieldInfo* connected = FindTypeField(_hingeType, "_connectedBody");
	ASSERT_NE(connected, nullptr);
	EXPECT_NE(connected->flags & FieldFlagHandle, 0u);

	Vec3 axis{};
	ASSERT_TRUE(ReadField(_hingeType, hinge.Get(), "_axis", axis));
	EXPECT_FLOAT_EQ(axis.x, 1.0f);
	EXPECT_FLOAT_EQ(axis.y, 0.0f);
	EXPECT_FLOAT_EQ(axis.z, 0.0f);

	for (const char* name : {"_useSpring", "_useMotor", "_useLimits"}) {
		bool on = true;
		ASSERT_TRUE(ReadField(_hingeType, hinge.Get(), name, on)) << "missing " << name;
		EXPECT_FALSE(on) << name;
	}

	for (const char* absent : {"bounciness", "bounceMinVelocity", "contactDistance",
							   "freeSpin", "useAcceleration", "extendedLimits",
							   "massScale", "connectedMassScale", "enablePreprocessing"}) {
		EXPECT_EQ(FindTypeField(_hingeType, absent), nullptr) << absent;
	}

	EXPECT_EQ(_fixedType->fieldCount, 0u);

	Handle<Component> spring = object->AddComponent(_springType);
	ASSERT_NE(spring.Get(), nullptr);
	f32 springConstant = 0.0f;
	ASSERT_TRUE(ReadField(_springType, spring.Get(), "_spring", springConstant));
	EXPECT_GT(springConstant, 0.0f);
	for (const char* name : {"_minDistance", "_maxDistance"}) {
		f32 value = 1.0f;
		ASSERT_TRUE(ReadField(_springType, spring.Get(), name, value));
		EXPECT_FLOAT_EQ(value, 0.0f);
	}
	EXPECT_EQ(FindTypeField(_springType, "tolerance"), nullptr);

	Handle<Component> character = object->AddComponent(_characterType);
	ASSERT_NE(character.Get(), nullptr);
	EXPECT_NE(FindOwnField(_characterType, "_lowTwistLimit"), nullptr);
	EXPECT_NE(FindOwnField(_characterType, "_highTwistLimit"), nullptr);

	for (const char* name : {"_swingAxis", "_swing1Limit", "_swing2Limit"}) {
		EXPECT_NE(FindOwnField(_characterType, name), nullptr) << name;
	}
	EXPECT_EQ(FindTypeField(_characterType, "_swingLimit"), nullptr);

	Vec3 swingAxis{};
	ASSERT_TRUE(ReadField(_characterType, character.Get(), "_swingAxis", swingAxis));
	EXPECT_FLOAT_EQ(swingAxis.x, 0.0f);
	EXPECT_FLOAT_EQ(swingAxis.y, 1.0f);
	EXPECT_FLOAT_EQ(swingAxis.z, 0.0f);

	for (const char* absent : {"swingLimitSpring", "twistLimitSpring", "enableProjection"}) {
		EXPECT_EQ(FindTypeField(_characterType, absent), nullptr) << absent;
	}

	for (const char* name : {"_useDrive", "_driveSpring", "_driveDamper", "_targetRotation"}) {
		EXPECT_NE(FindOwnField(_characterType, name), nullptr) << name;
	}
	bool useDrive = true;
	ASSERT_TRUE(ReadField(_characterType, character.Get(), "_useDrive", useDrive));
	EXPECT_FALSE(useDrive);
	Quat target{1.0f, 1.0f, 1.0f, 1.0f};
	ASSERT_TRUE(ReadField(_characterType, character.Get(), "_targetRotation", target));
	EXPECT_FLOAT_EQ(target.w, 1.0f);
	EXPECT_FLOAT_EQ(target.x, 0.0f);

	for (const char* absent : {"driveMaxTorque", "_driveMaxTorque",
							   "targetAngularVelocity", "_targetAngularVelocity"}) {
		EXPECT_EQ(FindTypeField(_characterType, absent), nullptr);
	}

	Handle<Component> prismatic = object->AddComponent(_prismaticType);
	ASSERT_NE(prismatic.Get(), nullptr);
	for (const char* name : {"_axis", "_useSpring", "_spring", "_damper", "_springTargetPosition",
							 "_useMotor", "_motorTargetVelocity", "_motorForce",
							 "_useLimits", "_limitMin", "_limitMax"}) {
		EXPECT_NE(FindOwnField(_prismaticType, name), nullptr) << name;
	}
}

TEST_F(Physics3DJointFixture, JointSceneRoundTrips) {
	Handle<SceneObject> anchorObject = MakePlainBody("anchor", Vec3{0.0f, 5.0f, 0.0f});
	ASSERT_TRUE(anchorObject.IsValid());
	Handle<Component> anchorBody = ComponentOn(anchorObject, _rigidbodyType);
	ASSERT_NE(anchorBody.Get(), nullptr);

	Handle<SceneObject> object = SceneSystem::CreateObject("every joint", _scene);
	ASSERT_TRUE(object.IsValid());
	object->SetActive(false);
	object->SetWorldPosition(Vec3{1.0f, 2.0f, 3.0f});
	object->AddComponent(_rigidbodyType);

	Handle<Component> hinge = object->AddComponent(_hingeType);
	ASSERT_TRUE(WriteField(_hingeType, hinge.Get(), "_axis", Vec3{0.0f, 1.0f, 0.0f}));
	ASSERT_TRUE(WriteField(_hingeType, hinge.Get(), "_anchor", Vec3{0.25f, -0.5f, 0.0f}));
	ASSERT_TRUE(WriteField(_hingeType, hinge.Get(), "_useLimits", true));
	ASSERT_TRUE(WriteField(_hingeType, hinge.Get(), "_limitMin", -45.0f));
	ASSERT_TRUE(WriteField(_hingeType, hinge.Get(), "_limitMax", 90.0f));
	ASSERT_TRUE(WriteField(_hingeType, hinge.Get(), "_breakForce", 250.0f));
	ASSERT_TRUE(WriteField(_hingeType, hinge.Get(), "_connectedBody", anchorBody.GetInstanceId()));

	Handle<Component> fixedJoint = object->AddComponent(_fixedType);
	ASSERT_TRUE(WriteField(_fixedType, fixedJoint.Get(), "_enableCollision", true));

	Handle<Component> spring = object->AddComponent(_springType);
	ASSERT_TRUE(WriteField(_springType, spring.Get(), "_spring", 42.5f));
	ASSERT_TRUE(WriteField(_springType, spring.Get(), "_maxDistance", 1.5f));

	Handle<Component> character = object->AddComponent(_characterType);
	ASSERT_TRUE(WriteField(_characterType, character.Get(), "_useSwingLimit", true));
	ASSERT_TRUE(WriteField(_characterType, character.Get(), "_swing1Limit", 30.0f));
	ASSERT_TRUE(WriteField(_characterType, character.Get(), "_swing2Limit", 12.0f));
	ASSERT_TRUE(WriteField(_characterType, character.Get(), "_swingAxis", Vec3{0.0f, 0.0f, 1.0f}));
	ASSERT_TRUE(WriteField(_characterType, character.Get(), "_useDrive", true));
	ASSERT_TRUE(WriteField(_characterType, character.Get(), "_driveSpring", 750.0f));
	ASSERT_TRUE(WriteField(_characterType, character.Get(), "_driveDamper", 25.0f));
	ASSERT_TRUE(WriteField(_characterType, character.Get(), "_targetRotation",
						   Quat::AngleAxis(35.0f, Vec3{0.0f, 1.0f, 0.0f})));

	Handle<Component> prismatic = object->AddComponent(_prismaticType);
	ASSERT_TRUE(WriteField(_prismaticType, prismatic.Get(), "_useMotor", true));
	ASSERT_TRUE(WriteField(_prismaticType, prismatic.Get(), "_motorTargetVelocity", 2.0f));

	object->SetActive(true);

	const Path scenePath((_root / "joints.scene").generic_string());
	ASSERT_TRUE(SceneSystem::SaveScene(_scene, scenePath));
	const str first = FileSystem::ReadTextFile(scenePath);
	ASSERT_FALSE(first.empty());

	Handle<Scene> reloaded = SceneSystem::LoadScene(scenePath);
	ASSERT_TRUE(reloaded.IsValid());

	std::filesystem::create_directories(_root / "again");
	const Path secondPath((_root / "again" / "joints.scene").generic_string());
	ASSERT_TRUE(SceneSystem::SaveScene(reloaded, secondPath));

	EXPECT_EQ(FileSystem::ReadTextFile(secondPath), first);

	for (const char* name : {"HingeJoint", "FixedJoint", "SpringJoint", "CharacterJoint", "PrismaticJoint"}) {
		EXPECT_NE(first.find(name), str::npos) << name;
	}
	EXPECT_NE(first.find("\"_connectedBody\""), str::npos);
	EXPECT_NE(first.find("_breakForce"), str::npos);
	EXPECT_NE(first.find("\"_targetRotation\""), str::npos);
	EXPECT_NE(first.find("\"_driveSpring\""), str::npos);
	EXPECT_NE(first.find("\"_swing1Limit\""), str::npos);
	EXPECT_NE(first.find("\"_swing2Limit\""), str::npos);
	EXPECT_NE(first.find("\"_swingAxis\""), str::npos);

	struct Found {
		Handle<SceneObject> jointed;
		Handle<SceneObject> anchor;
	} found;
	reloaded->ForEachObject(
		[](SceneObject& node, void* user) {
			auto& out = *static_cast<Found*>(user);
			if (node.name == "every joint") {
				out.jointed = Handle<SceneObject>(&node);
			} else if (node.name == "anchor") {
				out.anchor = Handle<SceneObject>(&node);
			}
		},
		&found);
	Handle<SceneObject> reloadedJointed = found.jointed;
	Handle<SceneObject> reloadedAnchor = found.anchor;
	ASSERT_TRUE(reloadedJointed.IsValid());
	ASSERT_TRUE(reloadedAnchor.IsValid());

	Handle<Component> reloadedHinge = ComponentOn(reloadedJointed, _hingeType);
	ASSERT_NE(reloadedHinge.Get(), nullptr);
	u64 referenceId = 0;
	ASSERT_TRUE(ReadField(_hingeType, reloadedHinge.Get(), "_connectedBody", referenceId));
	ASSERT_NE(referenceId, 0u);
	auto* reference = static_cast<Component*>(ObjectRegistry::Resolve(referenceId));
	ASSERT_NE(reference, nullptr);
	EXPECT_EQ(reference->GetOwner().GetInstanceId(), reloadedAnchor.GetInstanceId());

	EXPECT_NE(first.find("-45"), str::npos);
}

TEST_F(Physics3DJointFixture, ResolvedComponentHearsJointBreak) {
#ifndef SHALLTEAR_SHARED
	GTEST_SKIP() << "listener module registers with its copy of Physics 3D in a static tree";
#endif
	const TypeInfo* resolved = TypeRegistry::FindType(StringHash("ResolvedProbe"));
	ASSERT_NE(resolved, nullptr);
	Handle<Component> joint;
	Handle<SceneObject> object = MakeJointed("hanging", Vec3{0.0f, -1.0f, 0.0f}, _fixedType, joint, false, 1.0f);
	ASSERT_TRUE(object.IsValid());
	Handle<Component> probe = object->AddComponent(resolved);
	ASSERT_TRUE(probe.IsValid());
	ASSERT_TRUE(WriteField(_hingeType, joint.Get(), "_breakForce", GRAVITY * 0.5f));
	Activate(object);

	RunFixedSteps(4);

	i32 breaks = -1;
	ASSERT_TRUE(ReadField(resolved, probe.Get(), "jointBreak", breaks));
	EXPECT_EQ(breaks, 1);
}

TEST_F(Physics3DJointFixture, JointBreaksAtThresholdAndReportsForce) {
	Handle<Component> joint;
	Handle<SceneObject> object = MakeJointed("hanging", Vec3{0.0f, -1.0f, 0.0f}, _fixedType, joint,
											 true, 1.0f);
	ASSERT_TRUE(object.IsValid());

	ASSERT_TRUE(WriteField(_hingeType, joint.Get(), "_breakForce", GRAVITY * 0.5f));
	Activate(object);

	EXPECT_EQ(ProbeCount(object, "jointBreak"), 0);

	RunFixedSteps(4);

	EXPECT_EQ(ProbeCount(object, "jointBreak"), 1);

	Handle<Component> probe = ComponentOn(object, _probeType);
	f32 reported = 0.0f;
	ASSERT_TRUE(ReadField(_probeType, probe.Get(), "lastBreakForce", reported));
	EXPECT_GT(reported, GRAVITY * 0.5f);

	Component* broken = joint.Get();
	ASSERT_NE(broken, nullptr);
	EXPECT_TRUE(broken->IsPendingDestroy());
	SceneSystem::EndFrame();
	EXPECT_EQ(joint.Get(), nullptr);

	RunFixedSteps(10);
	EXPECT_EQ(ProbeCount(object, "jointBreak"), 1);
}

TEST_F(Physics3DJointFixture, JointBelowThresholdDoesNotBreak) {
	Handle<Component> joint;
	Handle<SceneObject> object = MakeJointed("holding", Vec3{0.0f, -1.0f, 0.0f}, _fixedType, joint,
											 true, 1.0f);
	ASSERT_TRUE(object.IsValid());
	ASSERT_TRUE(WriteField(_hingeType, joint.Get(), "_breakForce", GRAVITY * 100.0f));
	Activate(object);

	RunFixedSteps(30);

	EXPECT_EQ(ProbeCount(object, "jointBreak"), 0);
	EXPECT_NE(joint.Get(), nullptr);
}

TEST_F(Physics3DJointFixture, JointWithDefaultThresholdsNeverBreaks) {
	Handle<Component> joint;
	Handle<SceneObject> object = MakeJointed("permanent", Vec3{0.0f, -1.0f, 0.0f}, _fixedType, joint,
											 true, 1000.0f);
	ASSERT_TRUE(object.IsValid());
	Activate(object);

	RunFixedSteps(60);

	EXPECT_EQ(ProbeCount(object, "jointBreak"), 0);
	EXPECT_NE(joint.Get(), nullptr);
}

#ifndef SHALLTEAR_SHARED

#ifndef SHALLTEAR_PHYSICS_BACKEND_STUB

TEST_F(Physics3DJointFixture, SpringHingeBreaksAtLoadTorque) {
	const TypeInfo* boxType = TypeRegistry::FindType(StringHash("BoxCollider"));
	ASSERT_NE(boxType, nullptr);

	constexpr f32 LEVER = 1.0f;
	const f32 loadTorque = GRAVITY * LEVER;

	auto makeFlap = [&](const char* name, const Vec3& position) {
		Handle<Component> jointComponent;
		Handle<SceneObject> flap = MakeJointed(name, position, _hingeType, jointComponent, true);
		EXPECT_TRUE(flap.IsValid());
		EXPECT_NE(flap->AddComponent(boxType).Get(), nullptr);
		EXPECT_TRUE(WriteField(_hingeType, jointComponent.Get(), "_axis", Vec3{0.0f, 0.0f, 1.0f}));
		EXPECT_TRUE(WriteField(_hingeType, jointComponent.Get(), "_anchor", Vec3{-LEVER, 0.0f, 0.0f}));
		EXPECT_TRUE(WriteField(_hingeType, jointComponent.Get(), "_useSpring", true));
		EXPECT_TRUE(WriteField(_hingeType, jointComponent.Get(), "_spring", 400.0f));
		EXPECT_TRUE(WriteField(_hingeType, jointComponent.Get(), "_damper", 20.0f));
		Activate(flap);
		return flap;
	};

	Handle<SceneObject> weak = makeFlap("weak flap", Vec3{0.0f, 4.0f, 0.0f});
	Handle<SceneObject> strong = makeFlap("strong flap", Vec3{8.0f, 4.0f, 0.0f});
	ASSERT_TRUE(weak.IsValid());
	ASSERT_TRUE(strong.IsValid());
	RunFixedSteps(120);

	ArmBreakAndWake(weak, Joint::NEVER_BREAKS, loadTorque * 0.8f);
	ArmBreakAndWake(strong, Joint::NEVER_BREAKS, loadTorque * 1.25f);
	RunFixedSteps(10);

	EXPECT_EQ(ProbeCount(weak, "jointBreak"), 1);
	EXPECT_EQ(ProbeCount(strong, "jointBreak"), 0);
}

TEST_F(Physics3DJointFixture, SpringPrismaticBreaksAtLoadForce) {
	const TypeInfo* boxType = TypeRegistry::FindType(StringHash("BoxCollider"));
	ASSERT_NE(boxType, nullptr);

	const f32 loadForce = GRAVITY;

	auto makeLift = [&](const char* name, const Vec3& position) {
		Handle<Component> jointComponent;
		Handle<SceneObject> lift = MakeJointed(name, position, _prismaticType, jointComponent, true);
		EXPECT_TRUE(lift.IsValid());
		EXPECT_NE(lift->AddComponent(boxType).Get(), nullptr);
		EXPECT_TRUE(WriteField(_prismaticType, jointComponent.Get(), "_axis", Vec3{0.0f, 1.0f, 0.0f}));
		EXPECT_TRUE(WriteField(_prismaticType, jointComponent.Get(), "_useSpring", true));
		EXPECT_TRUE(WriteField(_prismaticType, jointComponent.Get(), "_spring", 400.0f));
		EXPECT_TRUE(WriteField(_prismaticType, jointComponent.Get(), "_damper", 20.0f));
		Activate(lift);
		return lift;
	};

	Handle<SceneObject> weak = makeLift("weak lift", Vec3{0.0f, 4.0f, 0.0f});
	Handle<SceneObject> strong = makeLift("strong lift", Vec3{8.0f, 4.0f, 0.0f});
	ASSERT_TRUE(weak.IsValid());
	ASSERT_TRUE(strong.IsValid());
	RunFixedSteps(120);

	ArmBreakAndWake(weak, loadForce * 0.8f, Joint::NEVER_BREAKS);
	ArmBreakAndWake(strong, loadForce * 1.25f, Joint::NEVER_BREAKS);
	RunFixedSteps(10);

	EXPECT_EQ(ProbeCount(weak, "jointBreak"), 1);
	EXPECT_EQ(ProbeCount(strong, "jointBreak"), 0);
}

TEST_F(Physics3DJointFixture, DriveIntoConeBreaksAtNetTorque) {
	const TypeInfo* boxType = TypeRegistry::FindType(StringHash("BoxCollider"));
	ASSERT_NE(boxType, nullptr);

	constexpr f32 LEVER = 1.0f;
	constexpr f32 CONE_DEGREES = 10.0f;
	const f32 loadTorque = GRAVITY * LEVER * std::cos(Math::DegToRad(CONE_DEGREES));

	auto makeArm = [&](const char* name, const Vec3& position) {
		Handle<Component> jointComponent;
		Handle<SceneObject> arm = MakeJointed(name, position, _characterType, jointComponent, true);
		EXPECT_TRUE(arm.IsValid());
		EXPECT_NE(arm->AddComponent(boxType).Get(), nullptr);
		EXPECT_TRUE(WriteField(_characterType, jointComponent.Get(), "_anchor", Vec3{-LEVER, 0.0f, 0.0f}));
		EXPECT_TRUE(WriteField(_characterType, jointComponent.Get(), "_axis", Vec3{1.0f, 0.0f, 0.0f}));
		EXPECT_TRUE(WriteField(_characterType, jointComponent.Get(), "_swingAxis", Vec3{0.0f, 0.0f, 1.0f}));
		EXPECT_TRUE(WriteField(_characterType, jointComponent.Get(), "_useSwingLimit", true));
		EXPECT_TRUE(WriteField(_characterType, jointComponent.Get(), "_swing1Limit", CONE_DEGREES));
		EXPECT_TRUE(WriteField(_characterType, jointComponent.Get(), "_swing2Limit", CONE_DEGREES));
		EXPECT_TRUE(WriteField(_characterType, jointComponent.Get(), "_useDrive", true));
		EXPECT_TRUE(WriteField(_characterType, jointComponent.Get(), "_driveSpring", DRIVE_SPRING));
		EXPECT_TRUE(WriteField(_characterType, jointComponent.Get(), "_driveDamper", DRIVE_DAMPER));
		EXPECT_TRUE(WriteField(_characterType, jointComponent.Get(), "_targetRotation",
							   Quat::AngleAxis(-3.0f * CONE_DEGREES, Vec3{1.0f, 0.0f, 0.0f})));
		Activate(arm);
		return arm;
	};

	Handle<SceneObject> weak = makeArm("weak arm", Vec3{0.0f, 4.0f, 0.0f});
	Handle<SceneObject> strong = makeArm("strong arm", Vec3{8.0f, 4.0f, 0.0f});
	ASSERT_TRUE(weak.IsValid());
	ASSERT_TRUE(strong.IsValid());
	RunFixedSteps(120);

	ArmBreakAndWake(weak, Joint::NEVER_BREAKS, loadTorque * 0.8f);
	ArmBreakAndWake(strong, Joint::NEVER_BREAKS, loadTorque * 1.25f);
	RunFixedSteps(10);

	EXPECT_EQ(ProbeCount(weak, "jointBreak"), 1);
	EXPECT_EQ(ProbeCount(strong, "jointBreak"), 0);
}

#endif

TEST_F(Physics3DJointFixture, UnconnectedJointAnchorsToWorld) {
	Handle<Component> joint;
	Handle<SceneObject> object = MakeJointed("hanging", Vec3{0.0f, 4.0f, 0.0f}, _fixedType, joint);
	ASSERT_TRUE(object.IsValid());
	Activate(object);

	auto* live = static_cast<Joint*>(joint.Get());
	ASSERT_NE(live, nullptr);
	EXPECT_TRUE(live->IsInSimulation());
	EXPECT_EQ(GetPhysicsWorld().GetJointCount(), 1u);

	const f32 startY = object->GetWorldPosition().y;
	RunFixedSteps(30);
	EXPECT_NEAR(object->GetWorldPosition().y, startY, 0.05f);
}

TEST_F(Physics3DJointFixture, WorldAnchorSurvivesJointLoss) {
	Handle<Component> firstJoint;
	Handle<SceneObject> first = MakeJointed("first", Vec3{0.0f, 4.0f, 0.0f}, _fixedType, firstJoint);
	ASSERT_TRUE(first.IsValid());
	Activate(first);

	Handle<Component> secondJoint;
	Handle<SceneObject> second = MakeJointed("second", Vec3{2.0f, 4.0f, 0.0f}, _fixedType, secondJoint);
	ASSERT_TRUE(second.IsValid());
	Activate(second);

	ASSERT_EQ(GetPhysicsWorld().GetJointCount(), 2u);

	SceneSystem::Destroy(first);
	RunFixedSteps(1);
	EXPECT_EQ(GetPhysicsWorld().GetJointCount(), 1u);

	const f32 startY = second->GetWorldPosition().y;
	RunFixedSteps(30);
	EXPECT_NEAR(second->GetWorldPosition().y, startY, 0.05f);
}

TEST_F(Physics3DJointFixture, BodylessJointNotBuilt) {
	Handle<SceneObject> object = SceneSystem::CreateObject("bare", _scene);
	ASSERT_TRUE(object.IsValid());
	object->SetActive(false);
	Handle<Component> joint = object->AddComponent(_fixedType);
	ASSERT_NE(joint.Get(), nullptr);
	object->SetActive(true);

	EXPECT_EQ(GetPhysicsWorld().GetJointCount(), 1u);
	EXPECT_FALSE(static_cast<Joint*>(joint.Get())->IsInSimulation());
}

TEST_F(Physics3DJointFixture, JointClaimedByNewBody) {
	Handle<SceneObject> object = SceneSystem::CreateObject("late body", _scene);
	ASSERT_TRUE(object.IsValid());
	object->SetActive(false);
	Handle<Component> joint = object->AddComponent(_fixedType);
	object->SetActive(true);

	ASSERT_FALSE(static_cast<Joint*>(joint.Get())->IsInSimulation());

	object->AddComponent(_rigidbodyType);

	EXPECT_TRUE(static_cast<Joint*>(joint.Get())->IsInSimulation());
}

TEST_F(Physics3DJointFixture, JointOnChildUsesBodyOnAncestor) {
	Handle<SceneObject> parent = MakePlainBody("parent", Vec3{0.0f, 4.0f, 0.0f});
	ASSERT_TRUE(parent.IsValid());

	Handle<SceneObject> child = SceneSystem::CreateObject("child", _scene, parent);
	ASSERT_TRUE(child.IsValid());
	child->SetActive(false);
	Handle<Component> joint = child->AddComponent(_fixedType);
	child->SetActive(true);

	EXPECT_TRUE(static_cast<Joint*>(joint.Get())->IsInSimulation());
}

TEST_F(Physics3DJointFixture, DisablingJointsBodyLeavesNoStaleJoint) {
	Handle<Component> joint;
	Handle<SceneObject> object = MakeJointed("hanging", Vec3{0.0f, 4.0f, 0.0f}, _fixedType, joint);
	ASSERT_TRUE(object.IsValid());
	Activate(object);
	ASSERT_TRUE(static_cast<Joint*>(joint.Get())->IsInSimulation());

	Handle<Component> body = ComponentOn(object, _rigidbodyType);
	ASSERT_NE(body.Get(), nullptr);
	body.Get()->SetEnabled(false);

	EXPECT_FALSE(static_cast<Joint*>(joint.Get())->IsInSimulation());

	body.Get()->SetEnabled(true);
	EXPECT_TRUE(static_cast<Joint*>(joint.Get())->IsInSimulation());
}

TEST(Physics3DSpringConversion, TheConstantBecomesTheSimulationsFrequency) {
	const PhysicsSpringTuning tuning = ConvertSpring(100.0f, 0.0f, 1.0f);
	EXPECT_NEAR(tuning.hertz, 10.0f / Math::TAU, 1.0e-5f);
	EXPECT_FLOAT_EQ(tuning.dampingRatio, 0.0f);

	const PhysicsSpringTuning heavier = ConvertSpring(100.0f, 0.0f, 4.0f);
	EXPECT_NEAR(heavier.hertz, tuning.hertz * 0.5f, 1.0e-5f);

	const PhysicsSpringTuning critical = ConvertSpring(100.0f, 20.0f, 1.0f);
	EXPECT_NEAR(critical.dampingRatio, 1.0f, 1.0e-5f);
	EXPECT_NEAR(ConvertSpring(100.0f, 10.0f, 1.0f).dampingRatio, 0.5f, 1.0e-5f);

	EXPECT_FLOAT_EQ(ConvertSpring(0.0f, 5.0f, 1.0f).hertz, 0.0f);
	EXPECT_FLOAT_EQ(ConvertSpring(100.0f, 5.0f, 0.0f).hertz, 0.0f);
}

TEST(Physics3DSpringConversion, EffectiveMassIsReducedMassOfPair) {
	EXPECT_FLOAT_EQ(EffectiveMassOf(1.0f, 0.0f), 1.0f);
	EXPECT_FLOAT_EQ(EffectiveMassOf(0.5f, 0.5f), 1.0f);
	EXPECT_FLOAT_EQ(EffectiveMassOf(0.0f, 0.0f), 0.0f);
}

TEST(Physics3DJointLimits, LimitNarrowedToSupported) {
	f32 lower = -Math::PI;
	f32 upper = Math::PI;
	EXPECT_TRUE(ClampJointAngularLimit(lower, upper));
	EXPECT_NEAR(lower, -JOINT_ANGULAR_LIMIT, 1.0e-6f);
	EXPECT_NEAR(upper, JOINT_ANGULAR_LIMIT, 1.0e-6f);
	EXPECT_LT(Math::RadToDeg(upper), 180.0f);

	f32 insideLower = Math::DegToRad(-45.0f);
	f32 insideUpper = Math::DegToRad(90.0f);
	EXPECT_FALSE(ClampJointAngularLimit(insideLower, insideUpper));
	EXPECT_NEAR(Math::RadToDeg(insideLower), -45.0f, 1.0e-3f);
	EXPECT_NEAR(Math::RadToDeg(insideUpper), 90.0f, 1.0e-3f);

	f32 invertedLower = Math::DegToRad(60.0f);
	f32 invertedUpper = Math::DegToRad(-60.0f);
	ClampJointAngularLimit(invertedLower, invertedUpper);
	EXPECT_LT(invertedLower, invertedUpper);

	f32 coneX = Math::DegToRad(185.0f);
	f32 coneY = Math::DegToRad(200.0f);
	EXPECT_TRUE(ClampJointConeAngles(coneX, coneY));
	EXPECT_NEAR(Math::RadToDeg(coneX), 178.2f, 1.0e-1f);
	EXPECT_NEAR(Math::RadToDeg(coneY), 178.2f, 1.0e-1f);

	f32 shoulderX = Math::DegToRad(130.0f);
	f32 shoulderY = Math::DegToRad(50.0f);
	EXPECT_FALSE(ClampJointConeAngles(shoulderX, shoulderY));
	EXPECT_NEAR(Math::RadToDeg(shoulderX), 130.0f, 1.0e-3f);
	EXPECT_NEAR(Math::RadToDeg(shoulderY), 50.0f, 1.0e-3f);

	f32 wideX = Math::DegToRad(60.0f);
	f32 narrowY = Math::DegToRad(10.0f);
	EXPECT_FALSE(ClampJointConeAngles(wideX, narrowY));
	EXPECT_NEAR(Math::RadToDeg(wideX), 60.0f, 1.0e-3f);
	EXPECT_NEAR(Math::RadToDeg(narrowY), 10.0f, 1.0e-3f);
}

TEST_F(Physics3DJointFixture, LimitsFromBuildPose) {
	const TypeInfo* boxType = TypeRegistry::FindType(StringHash("BoxCollider"));
	ASSERT_NE(boxType, nullptr);

	constexpr Vec3 HingeAxis{0.0f, 1.0f, 0.0f};
	constexpr f32 LIMIT_DEGREES = 2.0f;

	for (const f32 roll : {0.0f, 30.0f, 90.0f, 180.0f}) {
		Handle<SceneObject> post = SceneSystem::CreateObject("post", _scene);
		ASSERT_TRUE(post.IsValid());
		post->SetActive(false);
		post->SetWorldPosition(Vec3{roll, 0.0f, 0.0f});
		post->SetWorldRotation(Quat::AngleAxis(roll, HingeAxis));
		Handle<Component> postBody = post->AddComponent(_rigidbodyType);
		ASSERT_NE(postBody.Get(), nullptr);
		ASSERT_TRUE(WriteField(_rigidbodyType, postBody.Get(), "_isKinematic", true));
		ASSERT_NE(post->AddComponent(boxType).Get(), nullptr);
		post->SetActive(true);

		Handle<Component> jointComponent;
		Handle<SceneObject> door =
			MakeJointed("door", Vec3{roll + 1.0f, 0.0f, 0.0f}, _hingeType, jointComponent);
		ASSERT_TRUE(door.IsValid());
		ASSERT_NE(door->AddComponent(boxType).Get(), nullptr);
		ASSERT_TRUE(WriteField(_hingeType, jointComponent.Get(), "_axis", HingeAxis));
		ASSERT_TRUE(WriteField(_hingeType, jointComponent.Get(), "_anchor", Vec3{-1.0f, 0.0f, 0.0f}));
		ASSERT_TRUE(WriteField(_hingeType, jointComponent.Get(), "_useLimits", true));
		ASSERT_TRUE(WriteField(_hingeType, jointComponent.Get(), "_limitMin", -LIMIT_DEGREES));
		ASSERT_TRUE(WriteField(_hingeType, jointComponent.Get(), "_limitMax", LIMIT_DEGREES));
		ASSERT_TRUE(WriteField(_hingeType, jointComponent.Get(), "_connectedBody",
							   Handle<Rigidbody>(static_cast<Rigidbody*>(postBody.Get())).GetInstanceId()));
		Activate(door);

		auto* hinge = static_cast<HingeJoint*>(jointComponent.Get());
		ASSERT_NE(hinge, nullptr);
		ASSERT_TRUE(hinge->IsInSimulation());

		const Quat builtIn = door->GetWorldRotation();
		RunFixedSteps(180);

		const f32 moved = Quat::Angle(builtIn, door->GetWorldRotation());
		EXPECT_LT(moved, LIMIT_DEGREES * 2.0f);
	}
}

TEST_F(Physics3DJointFixture, InertialessDriveReported) {
	JobSystem::Initialize();
	LogManager::Initialize({false, false});
	LogManager::SetGlobalLevel(LogLevel::Trace);
	auto sink = std::make_shared<JointCapturingSink>();
	LogManager::AddSink(sink);

	Handle<Component> joint;
	Handle<SceneObject> object = MakeJointed("door", Vec3{}, _hingeType, joint);
	ASSERT_TRUE(object.IsValid());
	ASSERT_TRUE(WriteField(_hingeType, joint.Get(), "_useLimits", true));
	ASSERT_TRUE(WriteField(_hingeType, joint.Get(), "_limitMax", 90.0f));
	Activate(object);

	Handle<Component> quietJoint;
	Handle<SceneObject> quiet = MakeJointed("door with a body", Vec3{4.0f, 0.0f, 0.0f}, _hingeType, quietJoint);
	ASSERT_TRUE(quiet.IsValid());
	ASSERT_TRUE(WriteField(_hingeType, quietJoint.Get(), "_useLimits", true));
	ASSERT_TRUE(WriteField(_hingeType, quietJoint.Get(), "_limitMax", 90.0f));
	const TypeInfo* boxType = TypeRegistry::FindType(StringHash("BoxCollider"));
	ASSERT_NE(boxType, nullptr);
	quiet->AddComponent(boxType);
	Activate(quiet);

	Handle<Component> driveJoint;
	Handle<SceneObject> limb = MakeJointed("undriven limb", Vec3{8.0f, 0.0f, 0.0f}, _characterType, driveJoint);
	ASSERT_TRUE(limb.IsValid());
	ASSERT_TRUE(WriteField(_characterType, driveJoint.Get(), "_useDrive", true));
	ASSERT_TRUE(WriteField(_characterType, driveJoint.Get(), "_driveSpring", 500.0f));
	Activate(limb);

	RunFixedSteps(1);

	LogManager::Terminate();

	ASSERT_GT(sink->CountMatching(""), 0u);
	EXPECT_EQ(sink->CountMatching("rotational inertia"), 2u);

	EXPECT_EQ(sink->CountMatching("'door'"), 1u);
	EXPECT_EQ(sink->CountMatching("'undriven limb'"), 1u);

	sink.reset();
	JobSystem::Terminate();
}

TEST_F(Physics3DJointFixture, LiveJointFieldsReachSimulation) {
	using Property = PhysicsBackend::JointProperty;

	Handle<Component> hingeComponent;
	Handle<SceneObject> door = MakeJointed("door", Vec3{0.0f, 4.0f, 0.0f}, _hingeType, hingeComponent);
	ASSERT_TRUE(door.IsValid());
	const TypeInfo* shapeType = TypeRegistry::FindType(StringHash("BoxCollider"));
	ASSERT_NE(shapeType, nullptr);
	ASSERT_NE(door->AddComponent(shapeType).Get(), nullptr);
	Activate(door);
	auto* hinge = static_cast<HingeJoint*>(hingeComponent.Get());
	ASSERT_NE(hinge, nullptr);
	ASSERT_TRUE(hinge->IsInSimulation());

	PhysicsWorld& world = GetPhysicsWorld();

	hinge->SetBreakForce(250.0f);
	EXPECT_FLOAT_EQ(world.ReadJointProperty(*hinge, Property::ForceThreshold).AsScalar(), 250.0f);
	hinge->SetBreakTorque(75.0f);
	EXPECT_FLOAT_EQ(world.ReadJointProperty(*hinge, Property::TorqueThreshold).AsScalar(), 75.0f);
	hinge->SetEnableCollision(true);
	EXPECT_TRUE(world.ReadJointProperty(*hinge, Property::CollideConnected).AsFlag());

	hinge->SetUseMotor(true);
	EXPECT_TRUE(world.ReadJointProperty(*hinge, Property::EnableMotor).AsFlag());
	hinge->SetMotorTargetVelocity(90.0f);
	EXPECT_NEAR(world.ReadJointProperty(*hinge, Property::MotorSpeed).AsScalar(), Math::DegToRad(90.0f), 1e-5f);
	hinge->SetMotorForce(12.5f);
	EXPECT_FLOAT_EQ(world.ReadJointProperty(*hinge, Property::MaxMotorEffort).AsScalar(), 12.5f);

	hinge->SetUseLimits(true);
	EXPECT_TRUE(world.ReadJointProperty(*hinge, Property::EnableLimit).AsFlag());
	hinge->SetLimitMin(-30.0f);
	hinge->SetLimitMax(60.0f);
	const Vec3 limits = world.ReadJointProperty(*hinge, Property::Limits).AsVector();
	EXPECT_NEAR(limits.x, Math::DegToRad(-30.0f), 1e-5f);
	EXPECT_NEAR(limits.y, Math::DegToRad(60.0f), 1e-5f);

	hinge->SetUseSpring(true);
	EXPECT_FALSE(world.ReadJointProperty(*hinge, Property::EnableSpring).AsFlag());
	hinge->SetSpringTargetPosition(45.0f);
	EXPECT_NEAR(world.ReadJointProperty(*hinge, Property::SpringTarget).AsScalar(), Math::DegToRad(45.0f), 1e-5f);
	hinge->SetSpring(500.0f);
	const f32 hertz = world.ReadJointProperty(*hinge, Property::SpringHertz).AsScalar();
	EXPECT_GT(hertz, 0.0f);
	EXPECT_NE(hertz, 500.0f);
	EXPECT_TRUE(world.ReadJointProperty(*hinge, Property::EnableSpring).AsFlag());
	hinge->SetDamper(20.0f);
	EXPECT_GT(world.ReadJointProperty(*hinge, Property::SpringDampingRatio).AsScalar(), 0.0f);

	Handle<Component> slideComponent;
	Handle<SceneObject> lift = MakeJointed("lift", Vec3{4.0f, 4.0f, 0.0f}, _prismaticType, slideComponent);
	ASSERT_TRUE(lift.IsValid());
	Activate(lift);
	auto* slide = static_cast<PrismaticJoint*>(slideComponent.Get());
	ASSERT_NE(slide, nullptr);

	slide->SetUseMotor(true);
	slide->SetMotorTargetVelocity(2.0f);
	EXPECT_FLOAT_EQ(world.ReadJointProperty(*slide, Property::MotorSpeed).AsScalar(), 2.0f);
	slide->SetUseLimits(true);
	slide->SetLimitMin(-1.5f);
	slide->SetLimitMax(3.0f);
	const Vec3 travel = world.ReadJointProperty(*slide, Property::Limits).AsVector();
	EXPECT_FLOAT_EQ(travel.x, -1.5f);
	EXPECT_FLOAT_EQ(travel.y, 3.0f);
	slide->SetSpringTargetPosition(0.75f);
	EXPECT_FLOAT_EQ(world.ReadJointProperty(*slide, Property::SpringTarget).AsScalar(), 0.75f);

	Handle<Component> ragdollComponent;
	Handle<SceneObject> limb = MakeJointed("limb", Vec3{8.0f, 4.0f, 0.0f}, _characterType, ragdollComponent);
	ASSERT_TRUE(limb.IsValid());
	ASSERT_NE(limb->AddComponent(shapeType).Get(), nullptr);
	Activate(limb);
	auto* ragdoll = static_cast<CharacterJoint*>(ragdollComponent.Get());
	ASSERT_NE(ragdoll, nullptr);

	ragdoll->SetUseSwingLimit(true);
	EXPECT_TRUE(world.ReadJointProperty(*ragdoll, Property::EnableConeLimit).AsFlag());

	ragdoll->SetSwing1Limit(35.0f);
	ragdoll->SetSwing2Limit(20.0f);
	const Vec3 cone = world.ReadJointProperty(*ragdoll, Property::ConeAngles).AsVector();
	EXPECT_NEAR(cone.x, Math::DegToRad(35.0f), 1e-5f);
	EXPECT_NEAR(cone.y, Math::DegToRad(20.0f), 1e-5f);
	ragdoll->SetUseTwistLimit(true);
	EXPECT_TRUE(world.ReadJointProperty(*ragdoll, Property::EnableTwistLimit).AsFlag());
	ragdoll->SetLowTwistLimit(-20.0f);
	ragdoll->SetHighTwistLimit(20.0f);
	const Vec3 twist = world.ReadJointProperty(*ragdoll, Property::TwistLimits).AsVector();
	EXPECT_NEAR(twist.x, Math::DegToRad(-20.0f), 1e-5f);
	EXPECT_NEAR(twist.y, Math::DegToRad(20.0f), 1e-5f);

	ragdoll->SetUseDrive(true);
	EXPECT_FALSE(world.ReadJointProperty(*ragdoll, Property::EnableSpring).AsFlag());
	ragdoll->SetDriveSpring(800.0f);
	EXPECT_TRUE(world.ReadJointProperty(*ragdoll, Property::EnableSpring).AsFlag());
	ragdoll->SetDriveDamper(30.0f);
	const Vec3 drive = world.ReadJointProperty(*ragdoll, Property::SpringConstants).AsVector();
	EXPECT_FLOAT_EQ(drive.x, 800.0f);
	EXPECT_FLOAT_EQ(drive.y, 30.0f);

	const Quat authored = Quat::AngleAxis(40.0f, Vec3{0.0f, 1.0f, 0.0f});
	ragdoll->SetTargetRotation(authored);
	const Quat arrived = world.ReadJointProperty(*ragdoll, Property::TargetRotation).AsRotation();
	EXPECT_NEAR(arrived.x, authored.x, 1e-5f);
	EXPECT_NEAR(arrived.y, authored.y, 1e-5f);
	EXPECT_NEAR(arrived.z, authored.z, 1e-5f);
	EXPECT_NEAR(arrived.w, authored.w, 1e-5f);
	EXPECT_FLOAT_EQ(ragdoll->GetTargetRotation().y, authored.y);

	EXPECT_FLOAT_EQ(world.ReadJointProperty(*ragdoll, Property::SpringTarget).AsScalar(), 0.0f);
	EXPECT_FLOAT_EQ(world.ReadJointProperty(*hinge, Property::TargetRotation).AsRotation().w, 0.0f);
}

TEST_F(Physics3DJointFixture, SpringRangeFromRestLength) {
	using Property = PhysicsBackend::JointProperty;

	Handle<SceneObject> anchor = MakePlainBody("anchor", Vec3{0.0f, 6.0f, 0.0f});
	ASSERT_TRUE(anchor.IsValid());

	Handle<Component> springComponent;
	Handle<SceneObject> hook = MakeJointed("hook", Vec3{0.0f, 3.0f, 0.0f}, _springType, springComponent);
	ASSERT_TRUE(hook.IsValid());
	auto* spring = static_cast<SpringJoint*>(springComponent.Get());
	ASSERT_NE(spring, nullptr);
	Handle<Component> anchorBody = ComponentOn(anchor, _rigidbodyType);
	ASSERT_NE(anchorBody.Get(), nullptr);
	spring->SetConnectedBody(Handle<Rigidbody>(static_cast<Rigidbody*>(anchorBody.Get())));
	spring->SetAutoConfigureConnectedAnchor(false);
	spring->SetConnectedAnchor(Vec3{});
	Activate(hook);

	PhysicsWorld& world = GetPhysicsWorld();
	const f32 rest = world.ReadJointProperty(*spring, Property::SpringTarget).AsScalar();
	EXPECT_NEAR(rest, 3.0f, 1e-3f);

	spring->SetMinDistance(-2.0f);
	spring->SetMaxDistance(-2.0f);
	const Vec3 range = world.ReadJointProperty(*spring, Property::LengthRange).AsVector();
	EXPECT_NEAR(range.x, rest - 2.0f, 1e-3f);
	EXPECT_NEAR(range.y, rest - 2.0f, 1e-3f);
	EXPECT_TRUE(world.ReadJointProperty(*spring, Property::EnableLimit).AsFlag());

	spring->SetMinDistance(0.0f);
	spring->SetMaxDistance(0.0f);
	EXPECT_FALSE(world.ReadJointProperty(*spring, Property::EnableLimit).AsFlag());
}

TEST_F(Physics3DJointFixture, RuntimeLimitClamped) {
	using Property = PhysicsBackend::JointProperty;

	Handle<Component> jointComponent;
	Handle<SceneObject> door = MakeJointed("over-authored door", Vec3{}, _hingeType, jointComponent);
	ASSERT_TRUE(door.IsValid());
	Activate(door);
	auto* hinge = static_cast<HingeJoint*>(jointComponent.Get());
	ASSERT_NE(hinge, nullptr);

	JobSystem::Initialize();
	LogManager::Initialize({false, false});
	LogManager::SetGlobalLevel(LogLevel::Trace);
	auto sink = std::make_shared<JointCapturingSink>();
	LogManager::AddSink(sink);

	hinge->SetUseLimits(true);
	hinge->SetLimitMax(300.0f);

	LogManager::Terminate();
	EXPECT_GT(sink->CountMatching("over-authored door"), 0u);
	sink.reset();
	JobSystem::Terminate();

	const Vec3 limits = GetPhysicsWorld().ReadJointProperty(*hinge, Property::Limits).AsVector();
	EXPECT_LT(limits.y, Math::DegToRad(300.0f));
	EXPECT_NEAR(limits.y, JOINT_ANGULAR_LIMIT, 1e-4f);
}

TEST_F(Physics3DJointFixture, OfflineJointSetterWritesMember) {
	using Property = PhysicsBackend::JointProperty;

	Handle<Component> jointComponent;
	Handle<SceneObject> door = MakeJointed("dark door", Vec3{}, _hingeType, jointComponent);
	ASSERT_TRUE(door.IsValid());
	auto* hinge = static_cast<HingeJoint*>(jointComponent.Get());
	ASSERT_NE(hinge, nullptr);
	ASSERT_FALSE(hinge->IsInSimulation());

	hinge->SetUseMotor(true);
	hinge->SetMotorTargetVelocity(120.0f);
	hinge->SetBreakForce(400.0f);
	EXPECT_TRUE(hinge->GetUseMotor());
	EXPECT_FLOAT_EQ(hinge->GetMotorTargetVelocity(), 120.0f);
	EXPECT_FLOAT_EQ(GetPhysicsWorld().ReadJointProperty(*hinge, Property::MotorSpeed).AsScalar(), 0.0f);

	Activate(door);
	ASSERT_TRUE(hinge->IsInSimulation());
	EXPECT_NEAR(GetPhysicsWorld().ReadJointProperty(*hinge, Property::MotorSpeed).AsScalar(),
				Math::DegToRad(120.0f), 1e-5f);
	EXPECT_FLOAT_EQ(GetPhysicsWorld().ReadJointProperty(*hinge, Property::ForceThreshold).AsScalar(), 400.0f);

	Handle<Component> ragdollComponent;
	Handle<SceneObject> limb = MakeJointed("dark limb", Vec3{8.0f, 0.0f, 0.0f}, _characterType, ragdollComponent);
	ASSERT_TRUE(limb.IsValid());
	auto* ragdoll = static_cast<CharacterJoint*>(ragdollComponent.Get());
	ASSERT_NE(ragdoll, nullptr);
	const Quat authored = Quat::AngleAxis(28.0f, Vec3{0.0f, 1.0f, 0.0f});
	ragdoll->SetTargetRotation(authored);
	ragdoll->SetUseDrive(true);
	ragdoll->SetDriveSpring(300.0f);
	ASSERT_FALSE(ragdoll->IsInSimulation());

	Activate(limb);
	ASSERT_TRUE(ragdoll->IsInSimulation());
	const Quat built = GetPhysicsWorld().ReadJointProperty(*ragdoll, Property::TargetRotation).AsRotation();
	EXPECT_NEAR(built.y, authored.y, 1e-5f);
	EXPECT_NEAR(built.w, authored.w, 1e-5f);
	EXPECT_TRUE(GetPhysicsWorld().ReadJointProperty(*ragdoll, Property::EnableSpring).AsFlag());
}

#ifndef SHALLTEAR_PHYSICS_BACKEND_STUB

TEST_F(Physics3DJointFixture, RuntimeMotorTurnsDoor) {
	Handle<Component> jointComponent;
	Handle<SceneObject> door = MakeJointed("powered door", Vec3{0.0f, 4.0f, 0.0f}, _hingeType, jointComponent);
	ASSERT_TRUE(door.IsValid());
	const TypeInfo* boxType = TypeRegistry::FindType(StringHash("BoxCollider"));
	ASSERT_NE(boxType, nullptr);
	Handle<Component> collider = door->AddComponent(boxType);
	ASSERT_NE(collider.Get(), nullptr);
	auto* hinge = static_cast<HingeJoint*>(jointComponent.Get());
	ASSERT_NE(hinge, nullptr);
	hinge->SetAxis(Vec3{0.0f, 1.0f, 0.0f});
	Activate(door);
	ASSERT_TRUE(hinge->IsInSimulation());

	RunFixedSteps(30);
	const Quat still = door->GetWorldRotation();

	hinge->SetUseMotor(true);
	hinge->SetMotorTargetVelocity(180.0f);
	hinge->SetMotorForce(500.0f);
	RunFixedSteps(30);

	const Quat opened = door->GetWorldRotation();
	const f32 turned = Quat::Angle(still, opened);
	EXPECT_GT(turned, 5.0f);

	hinge->SetUseMotor(false);
	hinge->SetUseLimits(true);
	hinge->SetLimitMin(-1.0f);
	hinge->SetLimitMax(1.0f);
	RunFixedSteps(120);
	EXPECT_LT(Quat::Angle(still, door->GetWorldRotation()), 15.0f);
}

TEST_F(Physics3DJointFixture, ZeroSpringDropsLoad) {
	const TypeInfo* boxType = TypeRegistry::FindType(StringHash("BoxCollider"));
	ASSERT_NE(boxType, nullptr);

	Handle<Component> flapJoint;
	Handle<SceneObject> flap = MakeJointed("sprung flap", Vec3{0.0f, 4.0f, 0.0f}, _hingeType, flapJoint);
	ASSERT_TRUE(flap.IsValid());
	ASSERT_NE(flap->AddComponent(boxType).Get(), nullptr);
	ASSERT_TRUE(WriteField(_hingeType, flapJoint.Get(), "_axis", Vec3{0.0f, 0.0f, 1.0f}));
	ASSERT_TRUE(WriteField(_hingeType, flapJoint.Get(), "_anchor", Vec3{-1.0f, 0.0f, 0.0f}));
	ASSERT_TRUE(WriteField(_hingeType, flapJoint.Get(), "_useSpring", true));
	ASSERT_TRUE(WriteField(_hingeType, flapJoint.Get(), "_spring", 400.0f));
	ASSERT_TRUE(WriteField(_hingeType, flapJoint.Get(), "_damper", 20.0f));
	Activate(flap);
	auto* hinge = static_cast<HingeJoint*>(flapJoint.Get());
	ASSERT_NE(hinge, nullptr);

	const Quat flapPose = flap->GetWorldRotation();
	RunFixedSteps(120);
	ASSERT_LT(Quat::Angle(flapPose, flap->GetWorldRotation()), 15.0f);

	hinge->SetSpring(0.0f);
	EXPECT_FALSE(GetPhysicsWorld().ReadJointProperty(*hinge, PhysicsBackend::JointProperty::EnableSpring).AsFlag());
	RunFixedSteps(120);
	EXPECT_GT(Quat::Angle(flapPose, flap->GetWorldRotation()), 45.0f);

	Handle<Component> liftJoint;
	Handle<SceneObject> lift = MakeJointed("sprung lift", Vec3{8.0f, 4.0f, 0.0f}, _prismaticType, liftJoint);
	ASSERT_TRUE(lift.IsValid());
	ASSERT_NE(lift->AddComponent(boxType).Get(), nullptr);
	ASSERT_TRUE(WriteField(_prismaticType, liftJoint.Get(), "_axis", Vec3{0.0f, 1.0f, 0.0f}));
	ASSERT_TRUE(WriteField(_prismaticType, liftJoint.Get(), "_useSpring", true));
	ASSERT_TRUE(WriteField(_prismaticType, liftJoint.Get(), "_spring", 400.0f));
	ASSERT_TRUE(WriteField(_prismaticType, liftJoint.Get(), "_damper", 20.0f));
	Activate(lift);
	auto* slide = static_cast<PrismaticJoint*>(liftJoint.Get());
	ASSERT_NE(slide, nullptr);

	const f32 liftHeight = lift->GetWorldPosition().y;
	RunFixedSteps(120);
	ASSERT_LT(liftHeight - lift->GetWorldPosition().y, 0.5f);

	slide->SetSpring(0.0f);
	EXPECT_FALSE(GetPhysicsWorld().ReadJointProperty(*slide, PhysicsBackend::JointProperty::EnableSpring).AsFlag());
	RunFixedSteps(120);
	EXPECT_GT(liftHeight - lift->GetWorldPosition().y, 2.0f);
}

TEST_F(Physics3DJointFixture, ZeroSpringJointReleases) {
	const TypeInfo* boxType = TypeRegistry::FindType(StringHash("BoxCollider"));
	ASSERT_NE(boxType, nullptr);

	Handle<Component> releasedJoint;
	Handle<SceneObject> released = MakeJointed("released bob", Vec3{0.0f, 4.0f, 0.0f}, _springType, releasedJoint);
	ASSERT_TRUE(released.IsValid());
	ASSERT_NE(released->AddComponent(boxType).Get(), nullptr);
	ASSERT_TRUE(WriteField(_springType, releasedJoint.Get(), "_spring", 400.0f));
	ASSERT_TRUE(WriteField(_springType, releasedJoint.Get(), "_damper", 20.0f));
	Activate(released);

	Handle<Component> heldJoint;
	Handle<SceneObject> held = MakeJointed("held bob", Vec3{8.0f, 4.0f, 0.0f}, _springType, heldJoint);
	ASSERT_TRUE(held.IsValid());
	ASSERT_NE(held->AddComponent(boxType).Get(), nullptr);
	ASSERT_TRUE(WriteField(_springType, heldJoint.Get(), "_spring", 400.0f));
	ASSERT_TRUE(WriteField(_springType, heldJoint.Get(), "_damper", 20.0f));
	Activate(held);

	Handle<SceneObject> loose = SceneSystem::CreateObject("loose bob", _scene);
	ASSERT_TRUE(loose.IsValid());
	loose->SetActive(false);
	loose->SetWorldPosition(Vec3{16.0f, 4.0f, 0.0f});
	Handle<Component> looseBody = loose->AddComponent(_rigidbodyType);
	ASSERT_NE(looseBody.Get(), nullptr);
	ASSERT_TRUE(WriteField(_rigidbodyType, looseBody.Get(), "_mass", 1.0f));
	ASSERT_NE(loose->AddComponent(boxType).Get(), nullptr);

	RunFixedSteps(120);
	const f32 releasedRest = released->GetWorldPosition().y;
	const f32 heldRest = held->GetWorldPosition().y;
	ASSERT_GT(releasedRest, 3.0f);

	auto* spring = static_cast<SpringJoint*>(releasedJoint.Get());
	ASSERT_NE(spring, nullptr);
	spring->SetSpring(0.0f);
	Activate(loose);
	const f32 looseRest = loose->GetWorldPosition().y;

	RunFixedSteps(120);
	const f32 releasedDrop = releasedRest - released->GetWorldPosition().y;
	const f32 looseDrop = looseRest - loose->GetWorldPosition().y;
	ASSERT_GT(looseDrop, 15.0f);
	EXPECT_NEAR(releasedDrop, looseDrop, 0.5f);
	EXPECT_LT(heldRest - held->GetWorldPosition().y, 0.5f);
}

TEST_F(Physics3DJointFixture, DrivenJointHoldsPose) {
	const TypeInfo* boxType = TypeRegistry::FindType(StringHash("BoxCollider"));
	ASSERT_NE(boxType, nullptr);

	Handle<Component> limpJoint;
	Handle<SceneObject> limp = MakeJointed("limp arm", Vec3{0.0f, 4.0f, 0.0f}, _characterType, limpJoint);
	ASSERT_TRUE(limp.IsValid());
	ASSERT_NE(limp->AddComponent(boxType).Get(), nullptr);
	ASSERT_TRUE(WriteField(_characterType, limpJoint.Get(), "_anchor", Vec3{-1.0f, 0.0f, 0.0f}));
	Activate(limp);

	Handle<Component> heldJoint;
	Handle<SceneObject> held = MakeJointed("held arm", Vec3{8.0f, 4.0f, 0.0f}, _characterType, heldJoint);
	ASSERT_TRUE(held.IsValid());
	ASSERT_NE(held->AddComponent(boxType).Get(), nullptr);
	ASSERT_TRUE(WriteField(_characterType, heldJoint.Get(), "_anchor", Vec3{-1.0f, 0.0f, 0.0f}));
	ASSERT_TRUE(WriteField(_characterType, heldJoint.Get(), "_useDrive", true));
	ASSERT_TRUE(WriteField(_characterType, heldJoint.Get(), "_driveSpring", DRIVE_SPRING));
	ASSERT_TRUE(WriteField(_characterType, heldJoint.Get(), "_driveDamper", DRIVE_DAMPER));
	Activate(held);

	auto* drive = static_cast<CharacterJoint*>(heldJoint.Get());
	ASSERT_NE(drive, nullptr);
	ASSERT_TRUE(drive->IsInSimulation());

	const Quat authoredPose = held->GetWorldRotation();
	RunFixedSteps(120);

	const f32 heldError = Quat::Angle(authoredPose, held->GetWorldRotation());
	const f32 limpError = Quat::Angle(authoredPose, limp->GetWorldRotation());
	EXPECT_LT(heldError, 15.0f);
	EXPECT_GT(limpError, 45.0f);

	drive->SetDriveSpring(0.0f);
	RunFixedSteps(120);
	EXPECT_GT(Quat::Angle(authoredPose, held->GetWorldRotation()), 45.0f);
}

TEST_F(Physics3DJointFixture, DriveIsTorquePerRadian) {
	const TypeInfo* boxType = TypeRegistry::FindType(StringHash("BoxCollider"));
	ASSERT_NE(boxType, nullptr);

	constexpr f32 BAR_MASS = 1.0f;
	constexpr f32 LEVER = 0.5f;
	constexpr f32 PACKAGE_GRAVITY = 9.81f;
	const Vec3 barSize{0.1f, 1.0f, 0.1f};

	auto settle = [&](const Vec3& axis, f32 spring) {
		Handle<Component> jointComponent;
		Handle<SceneObject> bar =
			MakeJointed("bar", Vec3{0.0f, 4.0f, 0.0f}, _characterType, jointComponent, false, BAR_MASS);
		EXPECT_TRUE(bar.IsValid());
		Handle<Component> box = bar->AddComponent(boxType);
		EXPECT_NE(box.Get(), nullptr);
		EXPECT_TRUE(WriteField(boxType, box.Get(), "_size", barSize));
		EXPECT_TRUE(WriteField(_characterType, jointComponent.Get(), "_anchor", Vec3{0.0f, 0.0f, LEVER}));
		EXPECT_TRUE(WriteField(_characterType, jointComponent.Get(), "_axis", axis));
		EXPECT_TRUE(WriteField(_characterType, jointComponent.Get(), "_useDrive", true));
		EXPECT_TRUE(WriteField(_characterType, jointComponent.Get(), "_driveSpring", spring));
		EXPECT_TRUE(WriteField(_characterType, jointComponent.Get(), "_driveDamper", 0.0f));
		bar->SetWorldRotation(Quat::AngleAxis(90.0f, Vec3{0.0f, 0.0f, 1.0f}));
		Activate(bar);

		const Quat authored = bar->GetWorldRotation();
		RunFixedSteps(900);
		const f32 settled = Quat::Angle(authored, bar->GetWorldRotation());
		SceneSystem::Destroy(bar);
		RunFixedSteps(1);
		return settled;
	};

	constexpr f32 SOFT = 400.0f;
	const f32 torque = BAR_MASS * PACKAGE_GRAVITY * LEVER;
	const f32 softPredicted = Math::RadToDeg(torque / SOFT);

	const Vec3 axes[] = {Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
	for (const Vec3& axis : axes) {
		const f32 softSettled = settle(axis, SOFT);
		EXPECT_NEAR(softSettled, softPredicted, softPredicted * 0.15f);

		const f32 stiffSettled = settle(axis, SOFT * 4.0f);
		EXPECT_NEAR(stiffSettled, softSettled * 0.25f, softSettled * 0.05f);
	}
}

TEST_F(Physics3DJointFixture, DrivenChainTracksPose) {
	const TypeInfo* boxType = TypeRegistry::FindType(StringHash("BoxCollider"));
	ASSERT_NE(boxType, nullptr);

	Handle<Rigidbody> root;
	Vector<Handle<CharacterJoint>> chain;
	Vector<Handle<SceneObject>> links;
	Handle<Rigidbody> parentBody;
	for (u32 index = 0; index < CHAIN_LINKS; ++index) {
		const f32 height = 3.0f - static_cast<f32>(index);
		Handle<Component> jointComponent;
		Handle<SceneObject> link = MakeJointed("link", Vec3{0.0f, height, 0.0f}, _characterType, jointComponent);
		ASSERT_TRUE(link.IsValid());
		ASSERT_NE(link->AddComponent(boxType).Get(), nullptr);
		ASSERT_TRUE(WriteField(_characterType, jointComponent.Get(), "_axis", Vec3{0.0f, 1.0f, 0.0f}));
		ASSERT_TRUE(WriteField(_characterType, jointComponent.Get(), "_anchor", Vec3{0.0f, 0.5f, 0.0f}));
		ASSERT_TRUE(WriteField(_characterType, jointComponent.Get(), "_useDrive", true));
		ASSERT_TRUE(WriteField(_characterType, jointComponent.Get(), "_driveSpring", DRIVE_SPRING));
		ASSERT_TRUE(WriteField(_characterType, jointComponent.Get(), "_driveDamper", DRIVE_DAMPER));
		if (parentBody.IsValid()) {
			ASSERT_TRUE(WriteField(_characterType, jointComponent.Get(), "_connectedBody",
								   parentBody.GetInstanceId()));
		}
		Activate(link);
		auto* joint = static_cast<CharacterJoint*>(jointComponent.Get());
		ASSERT_NE(joint, nullptr);
		ASSERT_TRUE(joint->IsInSimulation());
		chain.push_back(Handle<CharacterJoint>(joint));
		links.push_back(link);
		parentBody = FindBody(link);
		ASSERT_TRUE(parentBody.IsValid());
	}

	Handle<SceneObject> scripted = SceneSystem::CreateObject("pose driver", _scene);
	ASSERT_TRUE(scripted.IsValid());
	Handle<PoseDriver> driver = scripted->AddComponent<PoseDriver>();
	ASSERT_TRUE(driver.IsValid());
	driver->joints = chain;
	driver->watched = FindBody(links[0]);
	ASSERT_TRUE(driver->watched.IsValid());

	RunFixedSteps(SETTLE_STEPS);
	ASSERT_GT(driver->ticks, 0);
	driver->switchTick = driver->ticks;
	RunFixedSteps(TRACK_STEPS);

	ASSERT_GT(driver->firstMovingTick, 0);
	const i32 lag = driver->firstMovingTick - driver->writeTick - 1;
	EXPECT_EQ(lag, 0);

	const Quat commanded = Quat::AngleAxis(TRACK_TARGET_DEGREES * 3.0f, Vec3{0.0f, 1.0f, 0.0f});
	const f32 trackingError = Quat::Angle(commanded, links[CHAIN_LINKS - 1]->GetWorldRotation());
	EXPECT_LT(trackingError, TRACK_TARGET_DEGREES);
}

TEST_F(Physics3DJointFixture, AnAsymmetricSwingLimitIsEnforcedAsymmetrically) {
	const TypeInfo* boxType = TypeRegistry::FindType(StringHash("BoxCollider"));
	ASSERT_NE(boxType, nullptr);

	constexpr f32 WIDE_DEGREES = 60.0f;
	constexpr f32 NARROW_DEGREES = 10.0f;

	auto makeArm = [&](const char* name, const Vec3& position, const Vec3& swingAxis) {
		Handle<Component> jointComponent;
		Handle<SceneObject> arm = MakeJointed(name, position, _characterType, jointComponent);
		EXPECT_TRUE(arm.IsValid());
		EXPECT_NE(arm->AddComponent(boxType).Get(), nullptr);
		EXPECT_TRUE(WriteField(_characterType, jointComponent.Get(), "_anchor", Vec3{-1.0f, 0.0f, 0.0f}));
		EXPECT_TRUE(WriteField(_characterType, jointComponent.Get(), "_axis", Vec3{1.0f, 0.0f, 0.0f}));
		EXPECT_TRUE(WriteField(_characterType, jointComponent.Get(), "_swingAxis", swingAxis));
		EXPECT_TRUE(WriteField(_characterType, jointComponent.Get(), "_useSwingLimit", true));
		EXPECT_TRUE(WriteField(_characterType, jointComponent.Get(), "_swing1Limit", WIDE_DEGREES));
		EXPECT_TRUE(WriteField(_characterType, jointComponent.Get(), "_swing2Limit", NARROW_DEGREES));
		Activate(arm);
		return arm;
	};

	Handle<SceneObject> wide = makeArm("wide arm", Vec3{0.0f, 4.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f});
	Handle<SceneObject> narrow = makeArm("narrow arm", Vec3{8.0f, 4.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
	ASSERT_TRUE(wide.IsValid());
	ASSERT_TRUE(narrow.IsValid());

	const Vector<f32> reach = FurthestSwings({wide, narrow}, 240);
	const f32 wideSwing = reach[0];
	const f32 narrowSwing = reach[1];

	EXPECT_NEAR(wideSwing, WIDE_DEGREES, 5.0f);
	EXPECT_NEAR(narrowSwing, NARROW_DEGREES, 5.0f);

	EXPECT_GT(wideSwing - narrowSwing, 30.0f);
}

TEST_F(Physics3DJointFixture, WideSwingLimitEnforced) {
	const TypeInfo* boxType = TypeRegistry::FindType(StringHash("BoxCollider"));
	ASSERT_NE(boxType, nullptr);

	constexpr f32 SHOULDER_DEGREES = 130.0f;

	Handle<Component> jointComponent;
	Handle<SceneObject> arm = MakeJointed("shoulder", Vec3{0.0f, 4.0f, 0.0f}, _characterType, jointComponent);
	ASSERT_TRUE(arm.IsValid());
	ASSERT_NE(arm->AddComponent(boxType).Get(), nullptr);
	ASSERT_TRUE(WriteField(_characterType, jointComponent.Get(), "_anchor", Vec3{-1.0f, 0.0f, 0.0f}));
	ASSERT_TRUE(WriteField(_characterType, jointComponent.Get(), "_axis", Vec3{1.0f, 0.0f, 0.0f}));
	ASSERT_TRUE(WriteField(_characterType, jointComponent.Get(), "_swingAxis", Vec3{0.0f, 0.0f, 1.0f}));
	ASSERT_TRUE(WriteField(_characterType, jointComponent.Get(), "_useSwingLimit", true));
	ASSERT_TRUE(WriteField(_characterType, jointComponent.Get(), "_swing1Limit", SHOULDER_DEGREES));
	ASSERT_TRUE(WriteField(_characterType, jointComponent.Get(), "_swing2Limit", SHOULDER_DEGREES));
	Activate(arm);

	auto* shoulder = static_cast<CharacterJoint*>(jointComponent.Get());
	ASSERT_NE(shoulder, nullptr);
	ASSERT_TRUE(shoulder->IsInSimulation());

	const Vec3 stored = GetPhysicsWorld().ReadJointProperty(*shoulder, PhysicsBackend::JointProperty::ConeAngles).AsVector();
	EXPECT_NEAR(Math::RadToDeg(stored.x), SHOULDER_DEGREES, 0.5f);
	EXPECT_NEAR(Math::RadToDeg(stored.y), SHOULDER_DEGREES, 0.5f);

	const f32 swing = FurthestSwings({arm}, 240)[0];
	EXPECT_GT(swing, 90.0f);
	EXPECT_LT(swing, SHOULDER_DEGREES + 10.0f);
}

TEST_F(Physics3DJointFixture, SecondSwingLimitClamped) {
	const TypeInfo* boxType = TypeRegistry::FindType(StringHash("BoxCollider"));
	ASSERT_NE(boxType, nullptr);

	Handle<Component> jointComponent;
	Handle<SceneObject> arm = MakeJointed("over-authored shoulder", Vec3{0.0f, 4.0f, 0.0f}, _characterType, jointComponent);
	ASSERT_TRUE(arm.IsValid());
	ASSERT_NE(arm->AddComponent(boxType).Get(), nullptr);
	ASSERT_TRUE(WriteField(_characterType, jointComponent.Get(), "_useSwingLimit", true));
	Activate(arm);

	auto* shoulder = static_cast<CharacterJoint*>(jointComponent.Get());
	ASSERT_NE(shoulder, nullptr);
	ASSERT_TRUE(shoulder->IsInSimulation());

	JobSystem::Initialize();
	LogManager::Initialize({false, false});
	LogManager::SetGlobalLevel(LogLevel::Trace);
	auto sink = std::make_shared<JointCapturingSink>();
	LogManager::AddSink(sink);

	shoulder->SetSwing2Limit(250.0f);

	LogManager::Terminate();
	EXPECT_GT(sink->CountMatching("over-authored shoulder"), 0u);
	sink.reset();
	JobSystem::Terminate();

	const Vec3 stored = GetPhysicsWorld().ReadJointProperty(*shoulder, PhysicsBackend::JointProperty::ConeAngles).AsVector();
	EXPECT_NEAR(Math::RadToDeg(stored.y), 178.2f, 0.2f);
	EXPECT_NEAR(Math::RadToDeg(stored.x), 0.0f, 1.0e-3f);
}

#endif

#endif
