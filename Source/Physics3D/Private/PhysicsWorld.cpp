#include "PhysicsWorld.h"

#include "PhysicsSettings.h"
#include "PhysicsJointMath.h"

#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Math/MathUtil.h"
#include "Shalltear/Core/Object.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/Time.h"

#include "Shalltear/Physics3D/CharacterController.h"
#include "Shalltear/Physics3D/Colliders.h"
#include "Shalltear/Physics3D/Joints.h"
#include "Shalltear/Physics3D/PhysicsListener.h"
#include "Shalltear/Physics3D/Rigidbody.h"

#include <algorithm>
#include <thread>

namespace Shalltear {

	namespace {

		u32 HardwareThreads() {
			static const u32 count = std::thread::hardware_concurrency();
			return count;
		}

		constexpr f32 SECONDARY_AXIS_EPSILON_SQUARED = 1.0e-6f;

		PhysicsBackend::WorldParams ToBackendParams(const PhysicsSettings& settings) {
			PhysicsBackend::WorldParams params;
			params.gravityX = settings.gravityX;
			params.gravityY = settings.gravityY;
			params.gravityZ = settings.gravityZ;
			params.bounceThreshold = settings.bounceThreshold;
			params.hitEventThreshold = settings.hitEventThreshold;
			params.contactHertz = settings.contactHertz;
			params.contactDampingRatio = settings.contactDampingRatio;
			params.contactSpeed = settings.contactSpeed;
			params.maxLinearSpeed = settings.maxLinearSpeed;
			params.enableSleeping = settings.enableSleeping;
			params.enableContinuous = settings.enableContinuous;
			params.workerCount = PhysicsBackend::ResolveWorkerCount(settings.workerCount, HardwareThreads());
			return params;
		}

		PhysicsBackend::BodyParams ToBackendParams(const Rigidbody& body, const Vec3& position, const Quat& rotation,
												   const Vec3& initialLinearVelocity, const Vec3& initialAngularVelocity,
												   u64 owner) {
			PhysicsBackend::BodyParams params;
			params.position = position;
			params.rotation = rotation;
			params.linearVelocity = initialLinearVelocity;
			params.angularVelocity = initialAngularVelocity;
			params.centerOfMass = body.GetAutomaticCenterOfMass() ? Vec3{} : body.GetCenterOfMass();
			params.mass = body.GetMass();
			params.linearDamping = body.GetLinearDamping();
			params.angularDamping = body.GetAngularDamping();
			params.gravityScale = body.GetUseGravity() ? 1.0f : 0.0f;
			params.sleepThreshold = body.GetSleepThreshold();
			params.kind = body.IsKinematic() ? PhysicsBackend::BodyKind::Kinematic : PhysicsBackend::BodyKind::Dynamic;
			params.freezePositionX = body.GetFreezePositionX();
			params.freezePositionY = body.GetFreezePositionY();
			params.freezePositionZ = body.GetFreezePositionZ();
			params.freezeRotationX = body.GetFreezeRotationX();
			params.freezeRotationY = body.GetFreezeRotationY();
			params.freezeRotationZ = body.GetFreezeRotationZ();
			params.isBullet = body.GetCollisionDetectionMode() == RigidbodyCollisionDetection::ContinuousDynamic;
			params.owner = owner;
			return params;
		}

	} // anonymous namespace

	namespace {

		PhysicsWorld _world;

	} // anonymous namespace

	PhysicsWorld& GetPhysicsWorld() {
		return _world;
	}

	PhysicsWorld::~PhysicsWorld() {
		Shutdown();
	}

	bool PhysicsWorld::Initialize(const PhysicsSettings& settings) {
		PhysicsBackend::InstallHostServices();

		_settings = &settings;

		if (_islandCount > 0) {
			return true;
		}

		_appliedCategories = settings.categories;
		_categories.Rebuild(_appliedCategories);

		_applied = ToBackendParams(settings);

		_reportedWorkerCount = _applied.workerCount;

#if defined(SLT_WITH_EDITOR)
		PhysicsBackend::EnableDebugDraw();
#endif

		PhysicsBackend::WorldHandle world;
		if (!PhysicsBackend::CreateWorld(_applied, world)) {
			SLT_LOG_ERROR("Physics", "the {} backend couldn't create a world", PhysicsBackend::Name());
			return false;
		}

		_islands[0].id = PHYSICS_DEFAULT_ISLAND;
		_islands[0].world = world;
		_islandCount = 1;
		_worldIsPristine = true;
		return true;
	}

	void PhysicsWorld::BeginPopulation() {
		if (_worldIsPristine) {
			_worldIsPristine = false;
			return;
		}

		if (_islandCount == 0 || !_bodies.empty() || !_colliders.empty() || !_staticBodies.empty() ||
			!_joints.empty() || !_controllers.empty() || _worldAnchorUsers > 0) {
			return;
		}

		for (u32 index = 0; index < _islandCount; ++index) {
			PhysicsBackend::DestroyWorld(_islands[index].world);
			_islands[index].world = {};

			PhysicsBackend::WorldHandle world;
			if (!PhysicsBackend::CreateWorld(_applied, world)) {
				SLT_LOG_ERROR("Physics", "the {} backend couldn't recreate a drained world", PhysicsBackend::Name());
				_islandCount = 0;
				return;
			}
			_islands[index].world = world;
		}

		_touching.clear();
		_riders.clear();
		_brokenJoints.clear();
		_jointReports.clear();
		_worldAnchor = {};

		_worldIsPristine = false;
	}

#if defined(SLT_WITH_EDITOR)

	void PhysicsWorld::DrawDebug(const PhysicsBackend::DebugDrawOptions& options, const PhysicsBackend::DebugDrawSink& sink) const {
		for (u32 island = 0; island < _islandCount; ++island) {
			PhysicsBackend::DrawWorld(_islands[island].world, options, sink);
		}
	}

	namespace {

		bool IsUnderAny(SceneObject& node, std::span<const Handle<SceneObject>> roots) {
			for (Handle<SceneObject> level(&node); level.IsValid(); level = level->GetParent()) {
				for (const Handle<SceneObject>& root : roots) {
					if (root == level) {
						return true;
					}
				}
			}
			return false;
		}

	} // anonymous namespace

	void PhysicsWorld::SetHeldBodies(std::span<const Handle<SceneObject>> objects) {
		ReleaseHeldBodies();
		_heldSimulated = false;
		if (objects.empty() || _islandCount == 0) {
			return;
		}

		for (const Handle<Rigidbody>& handle : _bodies) {
			Rigidbody* body = handle.Get();
			if (!body || body->_bodyBits == 0) {
				continue;
			}
			if (body->IsKinematic()) {
				continue;
			}
			SceneObject* owner = body->GetOwner().Get();
			if (!owner || !IsUnderAny(*owner, objects)) {
				continue;
			}

			_heldBodies.push_back(handle);
		}
	}

	void PhysicsWorld::ReleaseHeldBodies() {
		for (const Handle<Rigidbody>& handle : _heldBodies) {
			Rigidbody* body = handle.Get();
			if (!body || body->_bodyBits == 0) {
				continue;
			}
			const PhysicsBackend::BodyHandle backendBody{body->_bodyBits};

			PhysicsBackend::SetBodyLinearVelocity(backendBody, Vec3{});
			PhysicsBackend::SetBodyAngularVelocity(backendBody, Vec3{});

			Vec3 position;
			Quat rotation;
			if (_heldSimulated) {
				PhysicsBackend::GetBodyTransform(backendBody, position, rotation);
				WriteTransform(*body, position, rotation);
			} else if (SceneObject* owner = body->GetOwner().Get()) {
				position = owner->GetWorldPosition();
				rotation = owner->GetWorldRotation();
				PhysicsBackend::SetBodyTransform(backendBody, position, rotation);
				body->_syncedPosition = position;
				body->_syncedRotation = rotation;
			} else {
				continue;
			}

			body->_stepPosition = position;
			body->_stepRotation = rotation;
			body->_previousStepPosition = position;
			body->_previousStepRotation = rotation;
		}
		_heldBodies.clear();
	}

#endif

	bool PhysicsWorld::SceneOwnsTransform(Rigidbody& body) const {
		if (body.IsKinematic()) {
			return true;
		}
#if defined(SLT_WITH_EDITOR)
		if (!_heldBodies.empty()) {
			const Handle<Rigidbody> handle(&body);
			return std::find(_heldBodies.begin(), _heldBodies.end(), handle) != _heldBodies.end();
		}
#endif
		return false;
	}

	void PhysicsWorld::Shutdown() {
		// no logging (runs during image teardown)
		for (u32 index = 0; index < _islandCount; ++index) {
			PhysicsBackend::DestroyWorld(_islands[index].world);
			_islands[index] = Island{};
		}
		_islandCount = 0;
		_settings = nullptr;
		_bodies.clear();
		_colliders.clear();
		_controllers.clear();
		_staticBodies.clear();
		_shapePoints.clear();
		_shapeIndices.clear();
		_appliedCategories.clear();
		_interpolatingCount = 0;
	}

	void PhysicsWorld::AddBody(Rigidbody& body) {
		if (body._bodyBits != 0) {
			return;
		}

		Handle<SceneObject> ownerHandle = body.GetOwner();
		SceneObject* owner = ownerHandle.Get();
		if (!owner) {
			return;
		}

		Scene* scene = body.GetScene().Get();
		if (!scene || scene->GetKind() != SceneKind::World) {
			return;
		}

		BeginPopulation();

		_bodies.push_back(Handle<Rigidbody>(&body));

		if (_islandCount == 0) {
			return;
		}

		const Vec3 position = owner->GetWorldPosition();
		const Quat rotation = owner->GetWorldRotation();

		PhysicsBackend::BodyHandle handle;
		const PhysicsBackend::BodyParams params = ToBackendParams(body, position, rotation, body._pendingLinearVelocity,
																  body._pendingAngularVelocity, ownerHandle.GetInstanceId());
		if (!PhysicsBackend::CreateBody(_islands[0].world, params, handle)) {
			SLT_LOG_ERROR("Physics", "the {} backend couldn't create a body for '{}'", PhysicsBackend::Name(), owner->name);
			return;
		}

		body._bodyBits = handle.bits;
		body._pendingLinearVelocity = Vec3{};
		body._pendingAngularVelocity = Vec3{};

		body._syncedPosition = position;
		body._syncedRotation = rotation;
		body._stepPosition = position;
		body._stepRotation = rotation;
		body._previousStepPosition = position;
		body._previousStepRotation = rotation;

		RehomeColliders();

		RehomeJoints();
	}

	void PhysicsWorld::RemoveBody(Rigidbody& body) {
		if (body._bodyBits != 0) {
			const PhysicsBackend::BodyHandle handle{body._bodyBits};
			for (const Handle<Collider>& colliderHandle : _colliders) {
				Collider* collider = colliderHandle.Get();
				if (collider && collider->_ownerBodyBits == handle.bits) {
					collider->_shapeBits = 0;
					collider->_ownerBodyBits = 0;
				}
			}
			PhysicsBackend::DestroyBody(handle);
			body._bodyBits = 0;
		}

		const Handle<Rigidbody> registryHandle(&body);
		auto it = std::find(_bodies.begin(), _bodies.end(), registryHandle);
		if (it != _bodies.end()) {
			_bodies.erase(it);
		}

		RehomeColliders();
		RehomeJoints();
	}

	Rigidbody* PhysicsWorld::FindOwningBody(const Component& component) {
		for (Handle<SceneObject> node = component.GetOwner(); node.IsValid(); node = node->GetParent()) {
			if (Handle<Rigidbody> found = node->GetComponent<Rigidbody>()) {
				Rigidbody* body = found.Get();
				if (body && body->_bodyBits != 0) {
					return body;
				}
			}
		}
		return nullptr;
	}

	PhysicsSurface PhysicsWorld::SurfaceFrom(const PhysicsMaterial& material) {
		PhysicsSurface surface;
		surface.friction = material.friction;
		surface.bounciness = material.bounciness;
		surface.rollingResistance = material.rollingResistance;
		surface.combineBits = PackPhysicsMaterialCombine(material.frictionCombine, material.bounceCombine);
		return surface;
	}

	PhysicsSurface PhysicsWorld::SurfaceOf(const Collider& collider) {
		if (const PhysicsMaterial* material = collider.GetMaterial().Get()) {
			return SurfaceFrom(*material);
		}
		const PhysicsMaterial defaults;
		return SurfaceFrom(defaults);
	}

	void PhysicsWorld::AddCollider(Collider& collider) {
		const Handle<Collider> handle(&collider);
		if (std::find(_colliders.begin(), _colliders.end(), handle) != _colliders.end()) {
			return;
		}

		if (!collider.GetOwner().IsValid()) {
			return;
		}

		Scene* scene = collider.GetScene().Get();
		if (!scene || scene->GetKind() != SceneKind::World) {
			return;
		}

		BeginPopulation();

		_colliders.push_back(handle);
		AttachCollider(collider);
	}

	void PhysicsWorld::RemoveCollider(Collider& collider) {
		DetachCollider(collider);

		const Handle<Collider> handle(&collider);
		auto it = std::find(_colliders.begin(), _colliders.end(), handle);
		if (it != _colliders.end()) {
			_colliders.erase(it);
		}
	}

	void PhysicsWorld::AttachCollider(Collider& collider) {
		if (collider._shapeBits != 0 || _islandCount == 0) {
			return;
		}

		SceneObject* owner = collider.GetOwner().Get();
		if (!owner) {
			return;
		}

		Rigidbody* rigidbody = FindOwningBody(collider);
		PhysicsBackend::BodyHandle body;
		Vec3 bodyPosition;
		Quat bodyRotation;
		if (rigidbody) {
			SceneObject* bodyOwner = rigidbody->GetOwner().Get();
			if (!bodyOwner) {
				return;
			}
			body = PhysicsBackend::BodyHandle{rigidbody->_bodyBits};
			bodyPosition = bodyOwner->GetWorldPosition();
			bodyRotation = bodyOwner->GetWorldRotation();
		} else {
			body = AcquireStaticBody(collider.GetOwner());
			if (body.IsNull()) {
				return;
			}
			bodyPosition = owner->GetWorldPosition();
			bodyRotation = owner->GetWorldRotation();
		}

		SyncCategoryVocabulary();

		PhysicsShapeRequest request;
		request.isTrigger = collider.IsTrigger();
		request.surface = SurfaceOf(collider);

		const PhysicsCategoryFilter filter = ResolveFilterFor(collider);
		request.categoryBits = filter.categoryBits;
		request.maskBits = filter.maskBits;
		request.groupIndex = filter.groupIndex;
		request.owner = Handle<Collider>(&collider).GetInstanceId();
		request.pointScratch = &_shapePoints;
		request.indexScratch = &_shapeIndices;

		if (!collider.DescribeShape(owner->GetLossyScale(), request)) {
			SLT_LOG_WARN("Physics", "'{}' has a collider with no usable geometry",
						 owner->name);
			if (!rigidbody) {
				ReleaseStaticBody(body);
			}
			return;
		}

		if (request.kind == PhysicsShapeKind::TriangleMesh &&
			!PhysicsBackend::SupportsTriangleMeshOn(rigidbody ? (rigidbody->IsKinematic() ? PhysicsBackend::BodyKind::Kinematic
																						  : PhysicsBackend::BodyKind::Dynamic)
															  : PhysicsBackend::BodyKind::Static)) {
			SLT_LOG_WARN("Physics", "'{}' has a non-convex MeshCollider under a Rigidbody (triangle mesh contacts are static only). Turn Convex on or remove the Rigidbody",
						 owner->name);
			if (!rigidbody) {
				ReleaseStaticBody(body);
			}
			return;
		}

		const Quat bodyRotationInverse = bodyRotation.Inverse();
		const Vec3 colliderPosition = owner->GetWorldPosition();
		const Quat colliderRotation = owner->GetWorldRotation();
		const Quat relativeRotation = bodyRotationInverse * colliderRotation;
		const Vec3 relativeOffset = bodyRotationInverse * (colliderPosition - bodyPosition);

		request.localRotation = relativeRotation;
		request.localPosition = relativeOffset + relativeRotation * request.localPosition;
		request.capsulePoint0 = relativeOffset + relativeRotation * request.capsulePoint0;
		request.capsulePoint1 = relativeOffset + relativeRotation * request.capsulePoint1;

		PhysicsBackend::ShapeHandle shape;
		if (!PhysicsBackend::CreateShape(body, request, shape)) {
			SLT_LOG_ERROR("Physics", "the {} backend couldn't create a collider shape for '{}'", PhysicsBackend::Name(), owner->name);
			if (!rigidbody) {
				ReleaseStaticBody(body);
			}
			return;
		}

		collider._shapeBits = shape.bits;
		collider._ownerBodyBits = body.bits;
		TrackSurface(collider, request.surface);

		PhysicsBackend::SetShapeEvents(shape, rigidbody != nullptr && !rigidbody->IsKinematic(), true);

		if (rigidbody) {
			RefreshMass(*rigidbody);
		}
	}

	void PhysicsWorld::DetachCollider(Collider& collider) {
		if (collider._shapeBits == 0) {
			collider._ownerBodyBits = 0;
			return;
		}

		PhysicsBackend::DestroyShape(PhysicsBackend::ShapeHandle{collider._shapeBits});
		const PhysicsBackend::BodyHandle body{collider._ownerBodyBits};
		collider._shapeBits = 0;
		collider._ownerBodyBits = 0;

		for (const Handle<Rigidbody>& bodyHandle : _bodies) {
			Rigidbody* rigidbody = bodyHandle.Get();
			if (rigidbody && rigidbody->_bodyBits == body.bits) {
				RefreshMass(*rigidbody);
				return;
			}
		}
		ReleaseStaticBody(body);
	}

	void PhysicsWorld::RehomeColliders() {
		for (size_t index = 0; index < _colliders.size(); ++index) {
			Collider* collider = _colliders[index].Get();
			if (!collider) {
				continue;
			}
			Rigidbody* wanted = FindOwningBody(*collider);
			const u64 wantedBits = wanted ? wanted->_bodyBits : 0;
			if (collider->_shapeBits != 0 && collider->_ownerBodyBits == wantedBits) {
				continue;
			}
			DetachCollider(*collider);
			AttachCollider(*collider);
		}
	}

	u32 PhysicsWorld::GetShapeCountOnBodyOf(const Collider& collider) const {
		if (collider._ownerBodyBits == 0) {
			return 0;
		}
		return PhysicsBackend::GetBodyShapeCount(PhysicsBackend::BodyHandle{collider._ownerBodyBits});
	}

	void PhysicsWorld::RefreshMass(Rigidbody& body) {
		if (body._bodyBits == 0) {
			return;
		}
		const PhysicsBackend::BodyHandle handle{body._bodyBits};

		PhysicsBackend::MassParams params;
		params.mass = body.GetMass();
		params.centerOfMass = body.GetCenterOfMass();
		params.automaticCenterOfMass = body.GetAutomaticCenterOfMass();
		params.inertiaTensor = body.GetInertiaTensor();
		params.inertiaTensorRotation = body.GetInertiaTensorRotation();
		params.automaticInertiaTensor = body.GetAutomaticInertiaTensor();
		params.hasShapes = PhysicsBackend::GetBodyShapeCount(handle) > 0;
		PhysicsBackend::RefreshBodyMass(handle, params);

		RebuildJointsOn(body);
	}

	void PhysicsWorld::SyncCategoryVocabulary() {
		if (!_settings || _settings->categories == _appliedCategories) {
			return;
		}

		_appliedCategories = _settings->categories;
		if (!_categories.Rebuild(_appliedCategories)) {
			return;
		}
		if (_categories.OverflowCount() > 0) {
			SLT_LOG_WARN("Physics", "the project declares {} categories and this build resolves the 1st {}",
						 _categories.Size() + _categories.OverflowCount(), PHYSICS_CATEGORY_CAPACITY);
		}
		ReapplyCategoryFilters();
	}

	void PhysicsWorld::ReapplyCategoryFilters() {
		for (const Handle<Collider>& handle : _colliders) {
			Collider* collider = handle.Get();
			if (!collider || collider->_shapeBits == 0) {
				continue;
			}
			const PhysicsCategoryFilter filter = ResolveFilterFor(*collider);
			PhysicsBackend::SetShapeFilter(PhysicsBackend::ShapeHandle{collider->_shapeBits},
										   filter.categoryBits, filter.maskBits, filter.groupIndex);
		}
		for (const Handle<CharacterController>& handle : _controllers) {
			CharacterController* controller = handle.Get();
			if (controller && controller->_bodyBits != 0) {
				ResolveControllerFilter(*controller);
			}
		}
	}

	void PhysicsWorld::ReconcileSurfaces() {
		for (size_t index = 0; index < _surfaces.size();) {
			SurfaceWatch& watch = _surfaces[index];
			const PhysicsMaterial* material = watch.material.Get();
			if (!material) {
				_surfaces[index] = _surfaces.back();
				_surfaces.pop_back();
				continue;
			}

			const PhysicsSurface wanted = SurfaceFrom(*material);
			if (wanted == watch.applied) {
				++index;
				continue;
			}
			watch.applied = wanted;

			for (const Handle<Collider>& handle : _colliders) {
				Collider* collider = handle.Get();
				if (!collider || collider->_shapeBits == 0 || collider->GetMaterial() != watch.material) {
					continue;
				}
				PhysicsBackend::SetShapeSurface(PhysicsBackend::ShapeHandle{collider->_shapeBits}, wanted);
			}
			for (const Handle<CharacterController>& handle : _controllers) {
				CharacterController* controller = handle.Get();
				if (!controller || controller->_shapeBits == 0 || controller->GetMaterial() != watch.material) {
					continue;
				}
				PhysicsBackend::SetShapeSurface(PhysicsBackend::ShapeHandle{controller->_shapeBits}, wanted);
			}
			++index;
		}
	}

	void PhysicsWorld::TrackSurface(const Collider& collider, const PhysicsSurface& applied) {
		if (!collider.GetMaterial().IsValid()) {
			return;
		}
		for (const SurfaceWatch& watch : _surfaces) {
			if (watch.material == collider.GetMaterial()) {
				return;
			}
		}
		_surfaces.push_back(SurfaceWatch{collider.GetMaterial(), applied});
	}

	PhysicsCategoryFilter PhysicsWorld::ResolveFilterFor(const Collider& collider) const {
		u32 unknown = 0;
		const PhysicsCategoryFilter filter = ResolveCategoryFilter(_categories, collider.GetCategories(),
																   collider.GetCollidesWithMode(), collider.GetCollidesWith(),
																   unknown);
		if (unknown > 0) {
			const SceneObject* owner = collider.GetOwner().Get();
			SLT_LOG_WARN("Physics", "'{}' uses {} categor{} the project doesn't declare (ignored)",
						 owner ? owner->name : str("<destroyed>"), unknown, unknown == 1 ? "y" : "ies");
		}
		if (filter.categoryBits == 0) {
			const SceneObject* owner = collider.GetOwner().Get();
			SLT_LOG_WARN("Physics", "'{}' resolved to no categories from '{}' (nothing will collide with it)",
						 owner ? owner->name : str("<destroyed>"), collider.GetCategories());
		}
		return filter;
	}

	PhysicsBackend::QueryFilter PhysicsWorld::ResolveQueryFilterFor(const PhysicsQueryFilter& filter) const {
		u32 unknown = 0;
		const PhysicsCategoryFilter resolved = ResolveQueryFilter(_categories, filter.categories,
																  filter.collidesWithMode, filter.collidesWith,
																  unknown);
		if (unknown > 0) {
			SLT_LOG_WARN("Physics", "a query uses {} categor{} the project doesn't declare (is '{}', accepts '{}'). Ignored",
						 unknown, unknown == 1 ? "y" : "ies", filter.categories, filter.collidesWith);
		}

		PhysicsBackend::QueryFilter out;
		out.categoryBits = resolved.categoryBits;
		out.maskBits = resolved.maskBits;
		out.hitTriggers = filter.triggerInteraction == PhysicsQueryTriggers::UseGlobal
							  ? (_settings ? _settings->queriesHitTriggers : true)
							  : filter.triggerInteraction == PhysicsQueryTriggers::Collide;
		return out;
	}

	namespace {

		constexpr f32 PHYSICS_CARRY_UP_DOT = 0.7071068f;

		Vec3 CarrierUp(const Rigidbody& carrier) {
			const SceneObject* owner = carrier.GetOwner().Get();
			if (!owner) {
				return Vec3{0.0f, 1.0f, 0.0f};
			}
			return owner->GetWorldRotation() * Vec3{0.0f, 1.0f, 0.0f};
		}

		bool IsController(const Collider* collider) {
			return dynamic_cast<const CharacterController*>(collider) != nullptr;
		}

	} // anonymous namespace

	void PhysicsWorld::NoteRider(Rigidbody& rider, Rigidbody& carrier, const Vec3& anchor) {
		const u64 riderId = Handle<Rigidbody>(&rider).GetInstanceId();
		const u64 carrierId = Handle<Rigidbody>(&carrier).GetInstanceId();

		for (Rider& entry : _riders) {
			if (entry.rider.GetInstanceId() != riderId) {
				continue;
			}
			if (entry.carrier.GetInstanceId() == carrierId) {
				entry.refreshed = true;
				entry.anchor = anchor;
			}
			return;
		}

		Rider entry;
		entry.rider = Handle<Rigidbody>(&rider);
		entry.carrier = Handle<Rigidbody>(&carrier);
		entry.anchor = anchor;
		entry.refreshed = true;
		_riders.push_back(entry);
	}

	void PhysicsWorld::CarryRiders() {
		if (_riders.empty()) {
			return;
		}

		for (size_t index = 0; index < _riders.size();) {
			Rigidbody* rider = _riders[index].rider.Get();
			Rigidbody* carrier = _riders[index].carrier.Get();
			if (!_riders[index].refreshed || !rider || !carrier || rider->_bodyBits == 0 ||
				carrier->_bodyBits == 0) {
				_riders.erase(_riders.begin() + static_cast<ptrdiff_t>(index));
				continue;
			}
			_riders[index].refreshed = false;
			_riders[index].applied = false;
			++index;
		}
		if (_riders.empty()) {
			return;
		}

		size_t remaining = _riders.size();
		bool progress = true;
		while (remaining > 0 && progress) {
			progress = false;
			for (size_t index = 0; index < _riders.size(); ++index) {
				if (_riders[index].applied) {
					continue;
				}
				const u64 carrierId = _riders[index].carrier.GetInstanceId();
				bool carrierWaiting = false;
				for (const Rider& other : _riders) {
					if (!other.applied && other.rider.GetInstanceId() == carrierId) {
						carrierWaiting = true;
						break;
					}
				}
				if (carrierWaiting) {
					continue;
				}
				ApplyCarry(_riders[index]);
				_riders[index].applied = true;
				--remaining;
				progress = true;
			}
		}

		if (remaining > 0) {
			SLT_LOG_WARN("Physics", "{} rider(s) form a carry cycle and weren't carried this step",
						 remaining);
		}
	}

	void PhysicsWorld::ApplyCarry(Rider& entry) {
		Rigidbody* rider = entry.rider.Get();
		Rigidbody* carrier = entry.carrier.Get();
		SceneObject* riderOwner = rider ? rider->GetOwner().Get() : nullptr;
		if (!rider || !carrier || !riderOwner) {
			return;
		}

		const PhysicsBackend::BodyHandle riderBody{rider->_bodyBits};
		const PhysicsBackend::BodyHandle carrierBody{carrier->_bodyBits};

		const Vec3 pointVelocity = PhysicsBackend::GetBodyPointVelocity(carrierBody, entry.anchor);
		const Vec3 angularVelocity = PhysicsBackend::GetBodyAngularVelocity(carrierBody);

		if (entry.stamped) {
			const Vec3 deltaLinear = pointVelocity - entry.pointVelocity;
			const Vec3 deltaAngular = angularVelocity - entry.angularVelocity;
			if (deltaLinear.LengthSquared() > 0.0f || deltaAngular.LengthSquared() > 0.0f) {
				PhysicsBackend::SetBodyLinearVelocity(
					riderBody, PhysicsBackend::GetBodyLinearVelocity(riderBody) + deltaLinear);
				PhysicsBackend::SetBodyAngularVelocity(
					riderBody, PhysicsBackend::GetBodyAngularVelocity(riderBody) + deltaAngular);
			}
		}

		entry.pointVelocity = pointVelocity;
		entry.angularVelocity = angularVelocity;
		entry.stamped = true;
	}

	void PhysicsWorld::OnTouchEvent(void* user, const PhysicsBackend::TouchEvent& event) {
		PhysicsWorld& world = *static_cast<PhysicsWorld*>(user);
		const bool sensor = event.kind == PhysicsBackend::TouchKind::Sensor;

		if (event.begin) {
			if (event.shapeOwnerA == 0 || event.shapeOwnerB == 0) {
				return;
			}
			if (world.FindTouch(event.shapeOwnerA, event.shapeOwnerB, sensor)) {
				return;
			}
			TouchPair pair;
			pair.ownerA = event.shapeOwnerA;
			pair.ownerB = event.shapeOwnerB;
			pair.sensor = sensor;
			pair.fresh = true;
			world._touching.push_back(pair);
			return;
		}

		if (TouchPair* pair = world.FindTouch(event.shapeOwnerA, event.shapeOwnerB, sensor)) {
			pair->ended = true;
		}
	}

	PhysicsWorld::TouchPair* PhysicsWorld::FindTouch(u64 ownerA, u64 ownerB, bool sensor) {
		if (ownerA == 0 || ownerB == 0) {
			return nullptr;
		}
		for (TouchPair& pair : _touching) {
			if (pair.sensor != sensor) {
				continue;
			}
			if ((pair.ownerA == ownerA && pair.ownerB == ownerB) ||
				(pair.ownerA == ownerB && pair.ownerB == ownerA)) {
				return &pair;
			}
		}
		return nullptr;
	}

	namespace {

		PhysicsListenerResolver _listenerResolvers[Physics::MAX_LISTENER_RESOLVERS] = {};
		u32 _listenerResolverCount = 0;

		PhysicsListener* FindListener(Component* component) {
			if (!component) {
				return nullptr;
			}
			if (PhysicsListener* listener = dynamic_cast<PhysicsListener*>(component)) {
				return listener;
			}
			for (u32 index = 0; index < _listenerResolverCount; ++index) {
				if (PhysicsListener* listener = _listenerResolvers[index](*component)) {
					return listener;
				}
			}
			return nullptr;
		}

	} // anonymous namespace

	void Physics::AddListenerResolver(PhysicsListenerResolver resolver) {
		RemoveListenerResolver(resolver);
		SLT_CORE_ASSERT_MSG(_listenerResolverCount < MAX_LISTENER_RESOLVERS, "too many physics listener resolvers");
		if (resolver && _listenerResolverCount < MAX_LISTENER_RESOLVERS) {
			_listenerResolvers[_listenerResolverCount++] = resolver;
		}
	}

	void Physics::RemoveListenerResolver(PhysicsListenerResolver resolver) {
		for (u32 index = 0; index < _listenerResolverCount; ++index) {
			if (_listenerResolvers[index] == resolver) {
				_listenerResolvers[index] = _listenerResolvers[--_listenerResolverCount];
				return;
			}
		}
	}

	u32 PhysicsWorld::CollectListeners(SceneObject& object) {
		_listenerScratch.clear();
		const u32 count = object.GetComponentCount();
		for (u32 index = 0; index < count; ++index) {
			Handle<Component> handle = object.GetComponentAt(index);
			Component* component = handle.Get();
			if (!component || component->IsPendingDestroy()) {
				continue;
			}
			if (FindListener(component)) {
				_listenerScratch.push_back(handle);
			}
		}
		return static_cast<u32>(_listenerScratch.size());
	}

	void PhysicsWorld::DispatchToSide(TouchPhase phase, bool sensor, Collider& self, Rigidbody* selfBody,
									  Handle<Collider> other, Rigidbody* otherBody,
									  const PhysicsBackend::ContactGeometry* geometry, bool flipNormal) {
		SceneObject* owner = self.GetOwner().Get();
		if (!owner) {
			return;
		}
		if (CollectListeners(*owner) == 0) {
			return;
		}

		if (sensor) {
			const u32 count = static_cast<u32>(_listenerScratch.size());
			for (u32 index = 0; index < count; ++index) {
				if (index >= _listenerScratch.size()) {
					break;
				}
				Component* component = _listenerScratch[index].Get();
				PhysicsListener* listener = FindListener(component);
				if (!listener) {
					continue;
				}
				switch (phase) {
				case TouchPhase::Enter:
					listener->OnTriggerEnter(other);
					break;
				case TouchPhase::Stay:
					listener->OnTriggerStay(other);
					break;
				case TouchPhase::Exit:
					listener->OnTriggerExit(other);
					break;
				}
			}
			return;
		}

		Collision collision;
		collision.collider = other;
		if (Collider* resolved = other.Get()) {
			collision.object = resolved->GetOwner();
		}
		collision.rigidbody = Handle<Rigidbody>(otherBody);

		const Vec3 otherVelocity = otherBody ? otherBody->GetLinearVelocity() : Vec3{};
		const Vec3 selfVelocity = selfBody ? selfBody->GetLinearVelocity() : Vec3{};
		collision.relativeVelocity = otherVelocity - selfVelocity;

		if (geometry) {
			const u32 points = geometry->pointCount < Collision::MAX_CONTACTS ? geometry->pointCount
																			  : Collision::MAX_CONTACTS;
			const Vec3 normal = flipNormal ? Vec3{-geometry->normal.x, -geometry->normal.y, -geometry->normal.z}
										   : geometry->normal;
			for (u32 index = 0; index < points; ++index) {
				collision.contacts[index].point = geometry->point[index];
				collision.contacts[index].normal = normal;
				collision.contacts[index].separation = geometry->separation[index];
				collision.contacts[index].normalImpulse = geometry->normalImpulse[index];
			}
			collision.contactCount = points;
		}

		const u32 count = static_cast<u32>(_listenerScratch.size());
		for (u32 index = 0; index < count; ++index) {
			if (index >= _listenerScratch.size()) {
				break;
			}
			Component* component = _listenerScratch[index].Get();
			PhysicsListener* listener = FindListener(component);
			if (!listener) {
				continue;
			}
			switch (phase) {
			case TouchPhase::Enter:
				listener->OnCollisionEnter(collision);
				break;
			case TouchPhase::Stay:
				listener->OnCollisionStay(collision);
				break;
			case TouchPhase::Exit:
				listener->OnCollisionExit(collision);
				break;
			}
		}
	}

	void PhysicsWorld::DispatchContacts() {
		if (_islandCount == 0) {
			return;
		}

		// per step (skipped read never recovers)
		for (u32 index = 0; index < _islandCount; ++index) {
			PhysicsBackend::DrainTouchEvents(_islands[index].world, &PhysicsWorld::OnTouchEvent, this);
		}

		if (_touching.empty()) {
			return;
		}

		// only the drain adds to _touching
		for (size_t index = 0; index < _touching.size();) {
			TouchPair pair = _touching[index];

			Handle<Collider> handleA = ResolveShapeCollider(pair.ownerA);
			Handle<Collider> handleB = ResolveShapeCollider(pair.ownerB);
			Collider* colliderA = handleA.Get();
			Collider* colliderB = handleB.Get();

			const bool goneA = !colliderA || colliderA->_shapeBits == 0;
			const bool goneB = !colliderB || colliderB->_shapeBits == 0;
			const bool leaving = pair.ended || goneA || goneB;

			Rigidbody* bodyA = colliderA ? FindOwningBody(*colliderA) : nullptr;
			Rigidbody* bodyB = colliderB ? FindOwningBody(*colliderB) : nullptr;

			const bool dynamicA = bodyA && !bodyA->IsKinematic();
			const bool dynamicB = bodyB && !bodyB->IsKinematic();

			const bool controllerInPair = pair.sensor && ((!bodyA && IsController(colliderA)) || (!bodyB && IsController(colliderB)));

			const bool eligible = pair.sensor ? (bodyA != nullptr || bodyB != nullptr || controllerInPair) : (dynamicA || dynamicB);

			const bool awake = (bodyA && !bodyA->IsSleeping()) || (bodyB && !bodyB->IsSleeping()) || controllerInPair;

			const bool wantEnter = pair.fresh && eligible;
			const bool wantExit = leaving && pair.announced;
			const bool wantStay = !pair.fresh && !leaving && eligible && awake;

			const bool carrierInPair =
				_carrierCount > 0 && !leaving && !pair.sensor &&
				((bodyA && bodyA->GetCarryRiders()) || (bodyB && bodyB->GetCarryRiders()));

			if (wantEnter || wantStay || carrierInPair) {
				PhysicsBackend::ContactGeometry geometry;
				bool haveGeometry = false;
				if (!pair.sensor && !goneA && !goneB) {
					haveGeometry = PhysicsBackend::GetContactGeometry(
						PhysicsBackend::ShapeHandle{colliderA->_shapeBits},
						PhysicsBackend::ShapeHandle{colliderB->_shapeBits}, geometry);
				}

				if (wantEnter || wantStay) {
					const TouchPhase phase = wantEnter ? TouchPhase::Enter : TouchPhase::Stay;
					if (colliderA) {
						DispatchToSide(phase, pair.sensor, *colliderA, bodyA, handleB, bodyB,
									   haveGeometry ? &geometry : nullptr, false);
					}
					if (colliderB) {
						DispatchToSide(phase, pair.sensor, *colliderB, bodyB, handleA, bodyA,
									   haveGeometry ? &geometry : nullptr, true);
					}
					_touching[index].announced = true;
					_touching[index].fresh = false;
				}

				if (haveGeometry && carrierInPair) {
					const Vec3 anchor = geometry.point[0];
					if (bodyA && bodyA->GetCarryRiders() && bodyB && !bodyB->IsKinematic() &&
						Vec3::Dot(geometry.normal, CarrierUp(*bodyA)) >= PHYSICS_CARRY_UP_DOT) {
						NoteRider(*bodyB, *bodyA, anchor);
					}
					const Vec3 towardA{-geometry.normal.x, -geometry.normal.y, -geometry.normal.z};
					if (bodyB && bodyB->GetCarryRiders() && bodyA && !bodyA->IsKinematic() &&
						Vec3::Dot(towardA, CarrierUp(*bodyB)) >= PHYSICS_CARRY_UP_DOT) {
						NoteRider(*bodyA, *bodyB, anchor);
					}
				}
			}

			if (wantExit) {
				if (colliderA) {
					DispatchToSide(TouchPhase::Exit, pair.sensor, *colliderA, bodyA, handleB, bodyB, nullptr, false);
				}
				if (colliderB) {
					DispatchToSide(TouchPhase::Exit, pair.sensor, *colliderB, bodyB, handleA, bodyA, nullptr, true);
				}
			}

			if (leaving) {
				_touching.erase(_touching.begin() + static_cast<ptrdiff_t>(index));
				continue;
			}
			++index;
		}
	}

	Handle<Collider> PhysicsWorld::ResolveShapeCollider(u64 shapeOwner) {
		if (shapeOwner == 0) {
			return {};
		}
		Object* object = ObjectRegistry::Resolve(shapeOwner);
		if (!object) {
			return {};
		}
		if (!TypeRegistry::IsDerivedFrom(object->GetType(), Collider::GetReflectType())) {
			return {};
		}
		return Handle<Collider>(static_cast<Collider*>(object));
	}

	bool PhysicsWorld::FillHit(const PhysicsBackend::QueryHit& source, RaycastHit& hit) {
		hit = RaycastHit{};
		Handle<Collider> collider = ResolveShapeCollider(source.shapeOwner);
		Collider* resolved = collider.Get();
		if (!resolved) {
			return false;
		}

		hit.collider = collider;
		hit.object = resolved->GetOwner();
		hit.rigidbody = Handle<Rigidbody>(FindOwningBody(*resolved));
		hit.point = source.point;
		hit.normal = source.normal;
		hit.distance = source.distance;
		return true;
	}

	bool PhysicsWorld::CastRay(const Ray& ray, f32 maxDistance, const PhysicsQueryFilter& filter, RaycastHit& hit) {
		hit = RaycastHit{};
		if (_islandCount == 0 || !(maxDistance > 0.0f)) {
			return false;
		}
		SyncCategoryVocabulary();

		PhysicsBackend::QueryHit found;
		if (!PhysicsBackend::CastRay(_islands[PHYSICS_DEFAULT_ISLAND].world, ray.origin, ray.direction * maxDistance,
									 ResolveQueryFilterFor(filter), found)) {
			return false;
		}
		return FillHit(found, hit);
	}

	bool PhysicsWorld::CastShape(const Vec3* points, u32 pointCount, f32 radius, const Vec3& direction,
								 f32 maxDistance, const PhysicsQueryFilter& filter, RaycastHit& hit) {
		hit = RaycastHit{};
		if (_islandCount == 0 || !(maxDistance > 0.0f) || !(radius > 0.0f)) {
			return false;
		}
		SyncCategoryVocabulary();

		PhysicsBackend::QueryHit found;
		if (!PhysicsBackend::CastShape(_islands[PHYSICS_DEFAULT_ISLAND].world, points, pointCount, radius,
									   direction * maxDistance, ResolveQueryFilterFor(filter), found)) {
			return false;
		}
		return FillHit(found, hit);
	}

	u32 PhysicsWorld::OverlapShape(const Vec3* points, u32 pointCount, f32 radius,
								   const PhysicsQueryFilter& filter, Vector<Handle<Collider>>& results) {
		results.clear();
		if (_islandCount == 0 || !(radius > 0.0f)) {
			return 0;
		}
		SyncCategoryVocabulary();

		struct Sink {
			Vector<Handle<Collider>>* results = nullptr;
		} sink{&results};

		return PhysicsBackend::OverlapShape(
			_islands[PHYSICS_DEFAULT_ISLAND].world, points, pointCount, radius, ResolveQueryFilterFor(filter),
			[](void* user, u64 shapeOwner) {
				Handle<Collider> collider = ResolveShapeCollider(shapeOwner);
				if (collider.IsValid()) {
					static_cast<Sink*>(user)->results->push_back(collider);
				}
			},
			&sink);
	}

	PhysicsBackend::BodyHandle PhysicsWorld::AcquireStaticBody(Handle<SceneObject> object) {
		SceneObject* owner = object.Get();
		if (!owner || _islandCount == 0) {
			return {};
		}

		for (StaticBody& entry : _staticBodies) {
			if (entry.object == object) {
				++entry.colliderCount;
				return entry.body;
			}
		}

		const Vec3 position = owner->GetWorldPosition();
		const Quat rotation = owner->GetWorldRotation();

		PhysicsBackend::BodyParams params;
		params.kind = PhysicsBackend::BodyKind::Static;
		params.position = position;
		params.rotation = rotation;
		params.owner = object.GetInstanceId();

		PhysicsBackend::BodyHandle body;
		if (!PhysicsBackend::CreateBody(_islands[0].world, params, body)) {
			SLT_LOG_ERROR("Physics", "the {} backend couldn't create a static body for '{}'", PhysicsBackend::Name(), owner->name);
			return {};
		}

		StaticBody entry;
		entry.object = object;
		entry.body = body;
		entry.colliderCount = 1;
		entry.syncedPosition = position;
		entry.syncedRotation = rotation;
		_staticBodies.push_back(entry);
		return body;
	}

	void PhysicsWorld::ReleaseStaticBody(PhysicsBackend::BodyHandle body) {
		for (size_t index = 0; index < _staticBodies.size(); ++index) {
			if (_staticBodies[index].body != body) {
				continue;
			}
			if (--_staticBodies[index].colliderCount > 0) {
				return;
			}
			PhysicsBackend::DestroyBody(body);
			_staticBodies.erase(_staticBodies.begin() + static_cast<ptrdiff_t>(index));
			return;
		}
	}

	void PhysicsWorld::AddJoint(Joint& joint) {
		const Handle<Joint> handle(&joint);
		if (std::find(_joints.begin(), _joints.end(), handle) != _joints.end()) {
			return;
		}
		if (!joint.GetOwner().IsValid()) {
			return;
		}

		Scene* scene = joint.GetScene().Get();
		if (!scene || scene->GetKind() != SceneKind::World) {
			return;
		}

		BeginPopulation();

		_joints.push_back(handle);
		AttachJoint(joint);
	}

	void PhysicsWorld::RemoveJoint(Joint& joint) {
		DetachJoint(joint, false);

		const Handle<Joint> handle(&joint);
		auto it = std::find(_joints.begin(), _joints.end(), handle);
		if (it != _joints.end()) {
			_joints.erase(it);
		}
	}

	Quat PhysicsWorld::RotationBetween(const Vec3& from, const Vec3& to) {
		const Vec3 a = from.Normalized();
		const Vec3 b = to.Normalized();
		const f32 alignment = Vec3::Dot(a, b);

		if (alignment > 0.999999f) {
			return Quat::Identity();
		}

		if (alignment < -0.999999f) {
			const Vec3 reference = Math::Abs(a.x) < 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
			const Vec3 perpendicular = Vec3::Cross(a, reference).Normalized();
			return Quat{perpendicular.x, perpendicular.y, perpendicular.z, 0.0f};
		}

		const Vec3 axis = Vec3::Cross(a, b);
		Quat rotation{axis.x, axis.y, axis.z, 1.0f + alignment};
		return rotation.Normalized();
	}

	PhysicsBackend::BodyHandle PhysicsWorld::AcquireWorldAnchor() {
		if (_islandCount == 0) {
			return {};
		}
		if (_worldAnchorUsers > 0 && PhysicsBackend::IsBodyValid(_worldAnchor)) {
			++_worldAnchorUsers;
			return _worldAnchor;
		}

		PhysicsBackend::BodyParams params;
		params.kind = PhysicsBackend::BodyKind::Static;

		params.owner = 0;

		if (!PhysicsBackend::CreateBody(_islands[0].world, params, _worldAnchor)) {
			SLT_LOG_ERROR("Physics", "the {} backend couldn't create the world anchor body", PhysicsBackend::Name());
			_worldAnchor = {};
			return {};
		}
		_worldAnchorUsers = 1;
		return _worldAnchor;
	}

	void PhysicsWorld::ReleaseWorldAnchor() {
		if (_worldAnchorUsers == 0) {
			return;
		}
		if (--_worldAnchorUsers > 0) {
			return;
		}
		PhysicsBackend::DestroyBody(_worldAnchor);
		_worldAnchor = {};
	}

	bool PhysicsWorld::BuildJointParams(Joint& joint, PhysicsBackend::JointParams& params, bool& anchoredToWorld,
										JointReport& report) {
		anchoredToWorld = false;
		report = JointReport::None;

		SceneObject* owner = joint.GetOwner().Get();
		if (!owner) {
			return false;
		}

		Rigidbody* selfBody = FindOwningBody(joint);
		if (!selfBody) {
			report = JointReport::NoBody;
			return false;
		}
		SceneObject* selfOwner = selfBody->GetOwner().Get();
		if (!selfOwner) {
			return false;
		}

		PhysicsJointRequest request;
		if (!joint.DescribeJoint(request)) {
			return false;
		}

		Rigidbody* connected = joint.GetConnectedBody().Get();
		PhysicsBackend::BodyHandle bodyA;
		SceneObject* anchorOwner = nullptr;
		if (connected && connected->_bodyBits != 0) {
			bodyA = PhysicsBackend::BodyHandle{connected->_bodyBits};
			anchorOwner = connected->GetOwner().Get();
			if (!anchorOwner) {
				return false;
			}
		} else {
			bodyA = AcquireWorldAnchor();
			if (bodyA.IsNull()) {
				return false;
			}
			anchoredToWorld = true;
		}

		params.kind = static_cast<PhysicsBackend::JointKind>(request.kind);
		params.bodyA = bodyA;
		params.bodyB = PhysicsBackend::BodyHandle{selfBody->_bodyBits};
		params.collideConnected = joint.GetEnableCollision();
		params.owner = Handle<Joint>(&joint).GetInstanceId();

		const Vec3 selfPosition = selfOwner->GetWorldPosition();
		const Quat selfRotation = selfOwner->GetWorldRotation();
		const Quat selfRotationInverse = selfRotation.Inverse();

		const Quat jointRelativeRotation = selfRotationInverse * owner->GetWorldRotation();
		const Vec3 jointRelativeOffset = selfRotationInverse * (owner->GetWorldPosition() - selfPosition);

		params.localAnchorB = jointRelativeOffset + jointRelativeRotation * joint.GetAnchor();

		const Vec3 axisInBody = jointRelativeRotation * request.axis;

		const Vec3 axisInWorld = (selfRotation * axisInBody).Normalized();

		Quat worldFrame = RotationBetween(request.canonicalAxis, axisInWorld);

		if (request.secondaryAxis.LengthSquared() > 0.0f) {
			const Vec3 secondaryInWorld = (selfRotation * (jointRelativeRotation * request.secondaryAxis)).Normalized();

			const Vec3 alongAxis = axisInWorld * Vec3::Dot(secondaryInWorld, axisInWorld);
			const Vec3 acrossAxis = secondaryInWorld - alongAxis;
			if (acrossAxis.LengthSquared() > SECONDARY_AXIS_EPSILON_SQUARED) {
				const Vec3 secondaryNow = worldFrame * request.canonicalSecondaryAxis;
				worldFrame = RotationBetween(secondaryNow, acrossAxis.Normalized()) * worldFrame;
			} else if (request.enableConeLimit && request.coneAngleX != request.coneAngleY) {
				SLT_LOG_WARN("Physics", "'{}' has a joint swing axis parallel to its twist axis (swing limits follow the twist axis only)",
							 owner->name);
			}
		}

		params.localRotationB = selfRotationInverse * worldFrame;

		const Vec3 anchorWorld = selfPosition + selfRotation * params.localAnchorB;

		if (anchorOwner) {
			const Quat connectedRotation = anchorOwner->GetWorldRotation();
			const Quat connectedRotationInverse = connectedRotation.Inverse();

			params.localAnchorA = joint.GetAutoConfigureConnectedAnchor()
									  ? connectedRotationInverse * (anchorWorld - anchorOwner->GetWorldPosition())
									  : joint.GetConnectedAnchor();
			params.localRotationA = connectedRotationInverse * worldFrame;
		} else {
			params.localAnchorA = joint.GetAutoConfigureConnectedAnchor() ? anchorWorld : joint.GetConnectedAnchor();
			params.localRotationA = worldFrame;
		}

		params.enableSpring = request.enableSpring;
		params.springTarget = request.springTarget;

		params.targetRotation = request.targetRotation;

		const bool convertedSpring =
			request.kind != PhysicsJointKind::Weld && request.kind != PhysicsJointKind::Spherical;
		if (convertedSpring) {
			joint._springEffectiveMass =
				request.angularSpring
					? EffectiveMassOf(PhysicsBackend::GetBodyInverseInertiaAbout(params.bodyA, axisInWorld),
									  PhysicsBackend::GetBodyInverseInertiaAbout(params.bodyB, axisInWorld))
					: EffectiveMassOf(PhysicsBackend::GetBodyInverseMass(params.bodyA),
									  PhysicsBackend::GetBodyInverseMass(params.bodyB));
		}
		if (request.enableSpring && convertedSpring) {
			const PhysicsSpringTuning tuning =
				ConvertSpring(request.springStiffness, request.springDamper, joint._springEffectiveMass);
			params.springHertz = tuning.hertz;
			params.springDampingRatio = tuning.dampingRatio;
		}
		if (request.kind == PhysicsJointKind::Spherical) {
			params.springStiffness = request.springStiffness;
			params.springDamping = request.springDamper;
		}

		params.enableLimit = request.enableLimit;
		if (request.enableLimit) {
			params.lowerLimit = request.lowerLimit;
			params.upperLimit = request.upperLimit;
			if (request.angularLimit) {
				if (ClampJointAngularLimit(params.lowerLimit, params.upperLimit)) {
					SLT_LOG_WARN("Physics", "'{}' has a joint angle limit outside ±{:.1f}° (the simulation's reach) -> now {:.1f}° to {:.1f}°",
								 owner->name, Math::RadToDeg(JOINT_ANGULAR_LIMIT),
								 Math::RadToDeg(params.lowerLimit), Math::RadToDeg(params.upperLimit));
				}
			}
		}

		params.enableConeLimit = request.enableConeLimit;
		if (request.enableConeLimit) {
			params.coneAngleX = request.coneAngleX;
			params.coneAngleY = request.coneAngleY;
			if (ClampJointConeAngles(params.coneAngleX, params.coneAngleY)) {
				SLT_LOG_WARN("Physics", "'{}' has a joint swing limit outside 0°..{:.1f}° (the simulation's reach) -> now {:.1f}° and {:.1f}°",
							 owner->name, Math::RadToDeg(JOINT_CONE_LIMIT),
							 Math::RadToDeg(params.coneAngleX), Math::RadToDeg(params.coneAngleY));
			}
		}

		// without inertia the damn solver skips spring/motor/limit
		if (request.enableSpring || request.enableMotor || request.enableLimit || request.enableConeLimit) {
			const f32 inertiaAvailable = PhysicsBackend::GetBodyInverseInertiaAbout(params.bodyA, axisInWorld) +
										 PhysicsBackend::GetBodyInverseInertiaAbout(params.bodyB, axisInWorld);
			if (inertiaAvailable <= 0.0f) {
				report = JointReport::NoInertia;
			}
		}

		params.enableMotor = request.enableMotor;
		params.motorSpeed = request.motorSpeed;
		params.maxMotorEffort = request.maxMotorEffort;

		params.linearHertz = request.linearHertz;
		params.linearDampingRatio = request.linearDampingRatio;
		params.angularHertz = request.angularHertz;
		params.angularDampingRatio = request.angularDampingRatio;

		if (request.restLengthFromAnchors) {
			const Vec3 anchorAWorld = anchorOwner
										  ? anchorOwner->GetWorldPosition() + anchorOwner->GetWorldRotation() * params.localAnchorA
										  : params.localAnchorA;
			params.springTarget = Math::Max(JOINT_MIN_LENGTH, (anchorWorld - anchorAWorld).Length());
		}

		params.enableLimit = params.enableLimit || request.enableLengthRange;
		if (request.enableLengthRange) {
			params.minLength = Math::Max(JOINT_MIN_LENGTH, params.springTarget + request.minLengthOffset);
			params.maxLength = Math::Max(params.minLength, params.springTarget + request.maxLengthOffset);
		} else if (request.kind == PhysicsJointKind::Distance) {
			params.minLength = JOINT_MIN_LENGTH;
			params.maxLength = Math::Max(JOINT_MIN_LENGTH, params.springTarget);
		}

		params.forceThreshold = Math::Max(0.0f, joint.GetBreakForce());
		params.torqueThreshold = Math::Max(0.0f, joint.GetBreakTorque());
		return true;
	}

	void PhysicsWorld::AttachJoint(Joint& joint) {
		if (joint._jointBits != 0 || _islandCount == 0) {
			return;
		}

		PhysicsBackend::JointParams params;
		bool anchoredToWorld = false;
		JointReport report = JointReport::None;
		if (!BuildJointParams(joint, params, anchoredToWorld, report)) {
			if (anchoredToWorld) {
				ReleaseWorldAnchor();
			}
			NoteJointReport(joint, report);
			return;
		}

		PhysicsBackend::JointHandle handle;
		if (!PhysicsBackend::CreateJoint(_islands[0].world, params, handle)) {
			SceneObject* owner = joint.GetOwner().Get();
			SLT_LOG_ERROR("Physics", "the {} backend couldn't create a joint for '{}'", PhysicsBackend::Name(),
						  owner ? owner->name : str("(destroyed)"));
			if (anchoredToWorld) {
				ReleaseWorldAnchor();
			}
			return;
		}

		joint._jointBits = handle.bits;
		joint._anchoredToWorld = anchoredToWorld;
		NoteJointReport(joint, report);
	}

	void PhysicsWorld::PushJointProperty(const Joint& joint, PhysicsBackend::JointProperty property,
										 const PhysicsBackend::PropertyValue& value) {
		if (!joint.IsInSimulation()) {
			return;
		}
		const PhysicsBackend::JointHandle handle{joint._jointBits};
		PhysicsBackend::SetJointProperty(handle, property, value);
	}

	void PhysicsWorld::PushJointSpring(const Joint& joint, bool armed, f32 stiffness, f32 damper) {
		if (!joint.IsInSimulation()) {
			return;
		}
		const PhysicsBackend::JointHandle handle{joint._jointBits};
		// disarm before zeroing hertz (or the solver replays its last impulse)
		PhysicsBackend::SetJointProperty(handle, PhysicsBackend::JointProperty::EnableSpring,
										 PhysicsBackend::PropertyValue::Flag(armed));
		const PhysicsSpringTuning tuning = ConvertSpring(stiffness, damper, joint._springEffectiveMass);
		PhysicsBackend::SetJointProperty(handle, PhysicsBackend::JointProperty::SpringHertz,
										 PhysicsBackend::PropertyValue::Scalar(tuning.hertz));
		PhysicsBackend::SetJointProperty(handle, PhysicsBackend::JointProperty::SpringDampingRatio,
										 PhysicsBackend::PropertyValue::Scalar(tuning.dampingRatio));
	}

	void PhysicsWorld::PushJointDrive(const Joint& joint, bool armed, f32 stiffness, f32 damper) {
		if (!joint.IsInSimulation()) {
			return;
		}
		const PhysicsBackend::JointHandle handle{joint._jointBits};
		PhysicsBackend::SetJointProperty(handle, PhysicsBackend::JointProperty::EnableSpring,
										 PhysicsBackend::PropertyValue::Flag(armed));
		PhysicsBackend::SetJointProperty(handle, PhysicsBackend::JointProperty::SpringConstants,
										 PhysicsBackend::PropertyValue::Pair(stiffness, damper));
	}

	void PhysicsWorld::PushJointAngularLimits(const Joint& joint, PhysicsBackend::JointProperty property, f32 lower, f32 upper) {
		if (!joint.IsInSimulation()) {
			return;
		}
		const PhysicsBackend::JointHandle handle{joint._jointBits};
		if (ClampJointAngularLimit(lower, upper)) {
			const SceneObject* owner = joint.GetOwner().Get();
			SLT_LOG_WARN("Physics", "'{}' wrote a joint angle limit outside ±{:.1f}° (the simulation's reach) -> now {:.1f}° to {:.1f}°",
						 owner ? owner->name : str("<destroyed>"), Math::RadToDeg(JOINT_ANGULAR_LIMIT),
						 Math::RadToDeg(lower), Math::RadToDeg(upper));
		}
		PhysicsBackend::SetJointProperty(handle, property, PhysicsBackend::PropertyValue::Pair(lower, upper));
	}

	void PhysicsWorld::PushJointConeAngles(const Joint& joint, f32 angleX, f32 angleY) {
		if (!joint.IsInSimulation()) {
			return;
		}
		const PhysicsBackend::JointHandle handle{joint._jointBits};
		if (ClampJointConeAngles(angleX, angleY)) {
			const SceneObject* owner = joint.GetOwner().Get();
			SLT_LOG_WARN("Physics", "'{}' wrote a joint swing limit outside 0°..{:.1f}° (the simulation's reach) -> now {:.1f}° and {:.1f}°",
						 owner ? owner->name : str("<destroyed>"), Math::RadToDeg(JOINT_CONE_LIMIT),
						 Math::RadToDeg(angleX), Math::RadToDeg(angleY));
		}
		PhysicsBackend::SetJointProperty(handle, PhysicsBackend::JointProperty::ConeAngles,
										 PhysicsBackend::PropertyValue::Pair(angleX, angleY));
	}

	void PhysicsWorld::PushJointLengthRange(const Joint& joint, f32 minOffset, f32 maxOffset) {
		if (!joint.IsInSimulation()) {
			return;
		}
		const PhysicsBackend::JointHandle handle{joint._jointBits};
		const f32 rest = PhysicsBackend::GetJointProperty(handle, PhysicsBackend::JointProperty::SpringTarget).AsScalar();

		const bool enabled = minOffset != 0.0f || maxOffset != 0.0f;
		f32 minLength = JOINT_MIN_LENGTH;
		f32 maxLength = Math::Max(JOINT_MIN_LENGTH, rest);
		if (enabled) {
			minLength = Math::Max(JOINT_MIN_LENGTH, rest + minOffset);
			maxLength = Math::Max(minLength, rest + maxOffset);
		}
		PhysicsBackend::SetJointProperty(handle, PhysicsBackend::JointProperty::LengthRange,
										 PhysicsBackend::PropertyValue::Pair(minLength, maxLength));
		PhysicsBackend::SetJointProperty(handle, PhysicsBackend::JointProperty::EnableLimit,
										 PhysicsBackend::PropertyValue::Flag(enabled));
	}

	PhysicsBackend::PropertyValue PhysicsWorld::ReadJointProperty(const Joint& joint,
																  PhysicsBackend::JointProperty property) const {
		if (!joint.IsInSimulation()) {
			return PhysicsBackend::PropertyValue{};
		}
		return PhysicsBackend::GetJointProperty(PhysicsBackend::JointHandle{joint._jointBits}, property);
	}

	void PhysicsWorld::PushBodyProperty(const Rigidbody& body, PhysicsBackend::BodyProperty property,
										const PhysicsBackend::PropertyValue& value) {
		if (body._bodyBits == 0) {
			return;
		}
		PhysicsBackend::SetBodyProperty(PhysicsBackend::BodyHandle{body._bodyBits}, property, value);
	}

	void PhysicsWorld::PushBodyKind(Rigidbody& body) {
		if (body._bodyBits == 0) {
			return;
		}
		const PhysicsBackend::BodyHandle handle{body._bodyBits};
		const bool kinematic = body.IsKinematic();
		PhysicsBackend::SetBodyKind(handle, kinematic ? PhysicsBackend::BodyKind::Kinematic
													  : PhysicsBackend::BodyKind::Dynamic);

		RefreshMass(body);

		for (const Handle<Collider>& colliderHandle : _colliders) {
			Collider* collider = colliderHandle.Get();
			if (!collider || collider->_shapeBits == 0 || collider->_ownerBodyBits != body._bodyBits) {
				continue;
			}
			PhysicsBackend::SetShapeEvents(PhysicsBackend::ShapeHandle{collider->_shapeBits}, !kinematic, true);
		}
	}

	PhysicsBackend::PropertyValue PhysicsWorld::ReadBodyProperty(const Rigidbody& body,
																 PhysicsBackend::BodyProperty property) const {
		if (body._bodyBits == 0) {
			return PhysicsBackend::PropertyValue{};
		}
		return PhysicsBackend::GetBodyProperty(PhysicsBackend::BodyHandle{body._bodyBits}, property);
	}

	PhysicsBackend::BodyKind PhysicsWorld::ReadBodyKind(const Rigidbody& body) const {
		if (body._bodyBits == 0) {
			return PhysicsBackend::BodyKind::Static;
		}
		return PhysicsBackend::GetBodyKind(PhysicsBackend::BodyHandle{body._bodyBits});
	}

	f32 PhysicsWorld::ReadBodyInverseMass(const Rigidbody& body) const {
		if (body._bodyBits == 0) {
			return 0.0f;
		}
		return PhysicsBackend::GetBodyInverseMass(PhysicsBackend::BodyHandle{body._bodyBits});
	}

	void PhysicsWorld::RebuildShape(Collider& collider) {
		if (auto* controller = dynamic_cast<CharacterController*>(&collider)) {
			if (controller->_shapeBits != 0) {
				PhysicsBackend::DestroyShape(PhysicsBackend::ShapeHandle{controller->_shapeBits});
				controller->_shapeBits = 0;
				controller->_ownerBodyBits = 0;
			}
			AttachControllerShape(*controller);
			return;
		}
		if (collider._shapeBits == 0) {
			return;
		}
		DetachCollider(collider);
		AttachCollider(collider);
	}

	void PhysicsWorld::PushColliderSurface(Collider& collider) {
		if (collider._shapeBits == 0) {
			return;
		}
		const PhysicsSurface surface = SurfaceOf(collider);
		PhysicsBackend::SetShapeSurface(PhysicsBackend::ShapeHandle{collider._shapeBits}, surface);
		TrackSurface(collider, surface);
	}

	void PhysicsWorld::PushColliderFilter(Collider& collider) {
		if (auto* controller = dynamic_cast<CharacterController*>(&collider)) {
			if (controller->_bodyBits != 0) {
				SyncCategoryVocabulary();
				ResolveControllerFilter(*controller);
			}
			return;
		}
		if (collider._shapeBits == 0) {
			return;
		}
		SyncCategoryVocabulary();
		const PhysicsCategoryFilter filter = ResolveFilterFor(collider);
		PhysicsBackend::SetShapeFilter(PhysicsBackend::ShapeHandle{collider._shapeBits},
									   filter.categoryBits, filter.maskBits, filter.groupIndex);
	}

	void PhysicsWorld::DetachJoint(Joint& joint, bool wakeAttached) {
		if (joint._jointBits == 0) {
			return;
		}
		PhysicsBackend::DestroyJoint(PhysicsBackend::JointHandle{joint._jointBits}, wakeAttached);
		joint._jointBits = 0;
		if (joint._anchoredToWorld) {
			ReleaseWorldAnchor();
			joint._anchoredToWorld = false;
		}
	}

	void PhysicsWorld::RehomeJoints() {
		for (size_t index = 0; index < _joints.size(); ++index) {
			Joint* joint = _joints[index].Get();
			if (!joint) {
				continue;
			}

			if (joint->_jointBits != 0 &&
				!PhysicsBackend::IsJointValid(PhysicsBackend::JointHandle{joint->_jointBits})) {
				DetachJoint(*joint, false);
			}

			if (joint->_jointBits == 0) {
				AttachJoint(*joint);
			}
		}
	}

	void PhysicsWorld::NoteJointReport(Joint& joint, JointReport what) {
		if (what == JointReport::None) {
			return;
		}
		PendingJointReport record;
		record.joint = Handle<Joint>(&joint);
		record.jointBits = joint._jointBits;
		record.what = what;
		_jointReports.push_back(record);
	}

	void PhysicsWorld::DrainJointReports() {
		for (const PendingJointReport& record : _jointReports) {
			Joint* joint = record.joint.Get();
			if (!joint || joint->_jointBits != record.jointBits) {
				continue;
			}
			SceneObject* owner = joint->GetOwner().Get();
			const str name = owner ? owner->name : str("(destroyed)");
			switch (record.what) {
			case JointReport::NoBody:
				SLT_LOG_WARN("Physics", "'{}' has a joint with no Rigidbody on it or an ancestor", name);
				break;
			case JointReport::NoInertia:
				SLT_LOG_WARN("Physics", "'{}' has a joint with a spring, motor or limit and neither body has rotational inertia. Add a collider to 1 of them", name);
				break;
			case JointReport::None:
				break;
			}
		}
		_jointReports.clear();
	}

	void PhysicsWorld::RebuildJointsOn(Rigidbody& body) {
		for (size_t index = 0; index < _joints.size(); ++index) {
			Joint* joint = _joints[index].Get();
			if (!joint || joint->_jointBits == 0) {
				continue;
			}

			if (FindOwningBody(*joint) != &body && joint->GetConnectedBody().Get() != &body) {
				continue;
			}
			DetachJoint(*joint, false);
			AttachJoint(*joint);
		}
	}

	Handle<Joint> PhysicsWorld::ResolveJointOwner(u64 owner) {
		if (owner == 0) {
			return {};
		}
		Object* object = ObjectRegistry::Resolve(owner);
		if (!object) {
			return {};
		}
		if (!TypeRegistry::IsDerivedFrom(object->GetType(), Joint::GetReflectType())) {
			return {};
		}
		return Handle<Joint>(static_cast<Joint*>(object));
	}

	void PhysicsWorld::OnJointEvent(void* user, u64 owner) {
		auto* world = static_cast<PhysicsWorld*>(user);
		world->_brokenJoints.push_back(owner);
	}

	void PhysicsWorld::DispatchJointBreaks() {
		_brokenJoints.clear();
		for (u32 index = 0; index < _islandCount; ++index) {
			PhysicsBackend::DrainJointEvents(_islands[index].world, &OnJointEvent, this);
		}
		if (_brokenJoints.empty()) {
			return;
		}

		for (u64 owner : _brokenJoints) {
			Handle<Joint> handle = ResolveJointOwner(owner);
			Joint* joint = handle.Get();
			if (!joint || joint->_jointBits == 0) {
				continue;
			}

			// before the destroy (break event has no force)
			const f32 force = PhysicsBackend::GetJointForce(PhysicsBackend::JointHandle{joint->_jointBits});

			DetachJoint(*joint, true);

			SceneObject* owningObject = joint->GetOwner().Get();
			if (owningObject) {
				const u32 count = CollectListeners(*owningObject);
				for (u32 listenerIndex = 0; listenerIndex < count; ++listenerIndex) {
					Component* component = _listenerScratch[listenerIndex].Get();
					if (PhysicsListener* listener = FindListener(component)) {
						listener->OnJointBreak(force);
					}
				}
			}

			SceneSystem::Destroy(Handle<Component>(joint));
		}
	}

	void PhysicsWorld::Step(f32 deltaSeconds) {
		if (_islandCount == 0 || !_settings) {
			return;
		}

#if defined(SLT_WITH_EDITOR)
		_heldSimulated = _heldSimulated || !_heldBodies.empty();
#endif

		if (!_jointReports.empty()) {
			DrainJointReports();
		}

		PhysicsBackend::WorldParams wanted = ToBackendParams(*_settings);

		if (wanted.workerCount != _reportedWorkerCount) {
			_reportedWorkerCount = wanted.workerCount;
			if (wanted.workerCount != _applied.workerCount) {
				SLT_LOG_WARN("Physics", "workerCount resolves to {} but the simulation runs on {}. Restart to apply it",
							 wanted.workerCount, _applied.workerCount);
			}
		}

		wanted.workerCount = _applied.workerCount;

		if (wanted != _applied) {
			for (u32 index = 0; index < _islandCount; ++index) {
				PhysicsBackend::UpdateWorld(_islands[index].world, wanted);
			}
			_applied = wanted;
		}

		SyncCategoryVocabulary();

		ReconcileSurfaces();

		PushTransforms(deltaSeconds);

		CarryRiders();

		i32 substeps = _settings->substeps;
		if (substeps < 1) {
			substeps = 1;
		}
		for (u32 index = 0; index < _islandCount; ++index) {
			PhysicsBackend::StepWorld(_islands[index].world, deltaSeconds, substeps);
		}

		PullTransforms();

		DispatchContacts();

		// after contacts (a contact listener may destroy objects)
		DispatchJointBreaks();
	}

	void PhysicsWorld::PushTransforms(f32 deltaSeconds) {
		_interpolatingCount = 0;
		_carrierCount = 0;

		for (StaticBody& entry : _staticBodies) {
			SceneObject* owner = entry.object.Get();
			if (!owner) {
				continue;
			}
			const Vec3 position = owner->GetWorldPosition();
			const Quat rotation = owner->GetWorldRotation();
			if (position == entry.syncedPosition && rotation == entry.syncedRotation) {
				continue;
			}
			PhysicsBackend::SetBodyTransform(entry.body, position, rotation);
			entry.syncedPosition = position;
			entry.syncedRotation = rotation;
		}

		for (const Handle<CharacterController>& handle : _controllers) {
			if (CharacterController* controller = handle.Get()) {
				PlaceController(*controller);
			}
		}

		for (const Handle<Rigidbody>& handle : _bodies) {
			Rigidbody* body = handle.Get();
			if (!body || body->_bodyBits == 0) {
				continue;
			}
			SceneObject* owner = body->GetOwner().Get();
			if (!owner) {
				continue;
			}

			const bool sceneOwned = SceneOwnsTransform(*body);
			if (body->GetInterpolation() != RigidbodyInterpolation::None && !sceneOwned) {
				++_interpolatingCount;
			}
			if (body->GetCarryRiders()) {
				++_carrierCount;
			}

			const PhysicsBackend::BodyHandle backendBody{body->_bodyBits};
			const Vec3 position = owner->GetWorldPosition();
			const Quat rotation = owner->GetWorldRotation();

			if (sceneOwned) {
				// per step (stale target should still move body)
				PhysicsBackend::SetBodyTargetTransform(backendBody, position, rotation, deltaSeconds);
				body->_syncedPosition = position;
				body->_syncedRotation = rotation;
				continue;
			}

			if (position == body->_syncedPosition && rotation == body->_syncedRotation) {
				continue;
			}

			PhysicsBackend::SetBodyTransform(backendBody, position, rotation);
			body->_syncedPosition = position;
			body->_syncedRotation = rotation;
			body->_stepPosition = position;
			body->_stepRotation = rotation;
			body->_previousStepPosition = position;
			body->_previousStepRotation = rotation;
		}
	}

	void PhysicsWorld::PullTransforms() {
		for (const Handle<Rigidbody>& handle : _bodies) {
			Rigidbody* body = handle.Get();
			if (!body || body->_bodyBits == 0) {
				continue;
			}

			if (SceneOwnsTransform(*body)) {
				continue;
			}

			const PhysicsBackend::BodyHandle backendBody{body->_bodyBits};
			if (!PhysicsBackend::IsBodyAwake(backendBody)) {
				continue;
			}

			Vec3 position;
			Quat rotation;
			PhysicsBackend::GetBodyTransform(backendBody, position, rotation);

			body->_previousStepPosition = body->_stepPosition;
			body->_previousStepRotation = body->_stepRotation;
			body->_stepPosition = position;
			body->_stepRotation = rotation;

			WriteTransform(*body, position, rotation);
		}
	}

	void PhysicsWorld::Interpolate(f32 alpha) {
		if (_interpolatingCount == 0) {
			return;
		}

		const f32 fixedStep = Time::GetFixedDeltaTime();

		for (const Handle<Rigidbody>& handle : _bodies) {
			Rigidbody* body = handle.Get();
			if (!body || body->_bodyBits == 0 || SceneOwnsTransform(*body)) {
				continue;
			}
			if (body->GetInterpolation() == RigidbodyInterpolation::None) {
				continue;
			}

			Vec3 position;
			Quat rotation;
			if (body->GetInterpolation() == RigidbodyInterpolation::Interpolate) {
				position = Vec3::Lerp(body->_previousStepPosition, body->_stepPosition, alpha);
				rotation = Quat::Slerp(body->_previousStepRotation, body->_stepRotation, alpha);
			} else {
				const PhysicsBackend::BodyHandle backendBody{body->_bodyBits};
				const Vec3 velocity = PhysicsBackend::GetBodyLinearVelocity(backendBody);
				const Vec3 spin = PhysicsBackend::GetBodyAngularVelocity(backendBody);
				const f32 ahead = alpha * fixedStep;
				position = body->_stepPosition + velocity * ahead;
				if (spin.LengthSquared() > 0.0f) {
					const Quat spinQuat(spin.x, spin.y, spin.z, 0.0f);
					rotation = (body->_stepRotation + spinQuat * body->_stepRotation * (0.5f * ahead)).Normalized();
				} else {
					rotation = body->_stepRotation;
				}
			}

			WriteTransform(*body, position, rotation);
		}
	}

	void PhysicsWorld::AddController(CharacterController& controller) {
		const Handle<CharacterController> handle(&controller);
		if (std::find(_controllers.begin(), _controllers.end(), handle) != _controllers.end()) {
			return;
		}
		if (!controller.GetOwner().IsValid()) {
			return;
		}

		Scene* scene = controller.GetScene().Get();
		if (!scene || scene->GetKind() != SceneKind::World) {
			return;
		}

		BeginPopulation();

		_controllers.push_back(handle);
		AttachController(controller);
	}

	void PhysicsWorld::RemoveController(CharacterController& controller) {
		DetachController(controller);

		const Handle<CharacterController> handle(&controller);
		auto it = std::find(_controllers.begin(), _controllers.end(), handle);
		if (it != _controllers.end()) {
			_controllers.erase(it);
		}
	}

	void PhysicsWorld::AttachController(CharacterController& controller) {
		if (controller._bodyBits != 0 || _islandCount == 0) {
			return;
		}
		SceneObject* owner = controller.GetOwner().Get();
		if (!owner) {
			return;
		}

		const Vec3 position = owner->GetWorldPosition();

		PhysicsBackend::BodyParams params;
		params.kind = PhysicsBackend::BodyKind::Kinematic;
		params.position = position;
		params.gravityScale = 0.0f;
		params.owner = controller.GetOwner().GetInstanceId();

		PhysicsBackend::BodyHandle body;
		if (!PhysicsBackend::CreateBody(_islands[0].world, params, body)) {
			SLT_LOG_ERROR("Physics", "the {} backend couldn't create a body for '{}'", PhysicsBackend::Name(), owner->name);
			return;
		}

		controller._bodyBits = body.bits;
		controller._syncedPosition = position;
		controller._reportedOutside = false;

		SyncCategoryVocabulary();
		ResolveControllerFilter(controller);
		AttachControllerShape(controller);
	}

	void PhysicsWorld::DetachController(CharacterController& controller) {
		if (controller._shapeBits != 0) {
			PhysicsBackend::DestroyShape(PhysicsBackend::ShapeHandle{controller._shapeBits});
			controller._shapeBits = 0;
		}
		controller._ownerBodyBits = 0;
		if (controller._bodyBits != 0) {
			PhysicsBackend::DestroyBody(PhysicsBackend::BodyHandle{controller._bodyBits});
			controller._bodyBits = 0;
		}
		controller._carrier = {};
	}

	void PhysicsWorld::AttachControllerShape(CharacterController& controller) {
		if (controller._shapeBits != 0 || controller._bodyBits == 0 || !controller.GetDetectCollisions()) {
			return;
		}
		SceneObject* owner = controller.GetOwner().Get();
		if (!owner) {
			return;
		}

		PhysicsShapeRequest request;
		request.isTrigger = controller.IsTrigger();
		request.surface = SurfaceOf(controller);
		request.categoryBits = controller._queryCategoryBits;
		request.maskBits = controller._queryMaskBits;
		request.owner = Handle<Collider>(&controller).GetInstanceId();

		if (!controller.DescribeShape(owner->GetLossyScale(), request)) {
			SLT_LOG_WARN("Physics", "'{}' has a CharacterController with no usable geometry", owner->name);
			return;
		}

		const PhysicsBackend::BodyHandle body{controller._bodyBits};
		PhysicsBackend::ShapeHandle shape;
		if (!PhysicsBackend::CreateShape(body, request, shape)) {
			SLT_LOG_ERROR("Physics", "the {} backend couldn't create a collider shape for '{}'", PhysicsBackend::Name(), owner->name);
			return;
		}

		controller._shapeBits = shape.bits;
		controller._ownerBodyBits = body.bits;
		TrackSurface(controller, request.surface);
		PhysicsBackend::SetShapeEvents(shape, false, true);
	}

	void PhysicsWorld::ResolveControllerFilter(CharacterController& controller) {
		const PhysicsCategoryFilter filter = ResolveFilterFor(controller);
		controller._queryCategoryBits = filter.categoryBits;
		controller._queryMaskBits = filter.maskBits;
		if (controller._shapeBits != 0) {
			PhysicsBackend::SetShapeFilter(PhysicsBackend::ShapeHandle{controller._shapeBits},
										   filter.categoryBits, filter.maskBits, filter.groupIndex);
		}
	}

	u32 PhysicsWorld::CollideController(const CharacterController& controller, const Vec3& position,
										const PhysicsBackend::MoverCapsule& capsule, PhysicsBackend::MoverPlane* planes,
										u32 capacity) {
		if (_islandCount == 0 || capacity == 0) {
			return 0;
		}
		SyncCategoryVocabulary();

		PhysicsBackend::QueryFilter filter;
		filter.categoryBits = controller._queryCategoryBits;
		filter.maskBits = controller._queryMaskBits;
		filter.hitTriggers = false;

		struct Sink {
			PhysicsBackend::MoverPlane* planes = nullptr;
			u32 capacity = 0;
			u32 count = 0;
		} sink{planes, capacity, 0};

		PhysicsBackend::CollideMover(
			_islands[PHYSICS_DEFAULT_ISLAND].world, position, capsule, filter,
			PhysicsBackend::ShapeHandle{controller._shapeBits},
			[](void* user, const PhysicsBackend::MoverPlane& plane) {
				Sink& target = *static_cast<Sink*>(user);
				if (target.count < target.capacity) {
					target.planes[target.count++] = plane;
				}
			},
			&sink);
		return sink.count;
	}

	f32 PhysicsWorld::CastController(const CharacterController& controller, const Vec3& position,
									 const PhysicsBackend::MoverCapsule& capsule, const Vec3& translation) {
		if (_islandCount == 0) {
			return 1.0f;
		}

		PhysicsBackend::QueryFilter filter;
		filter.categoryBits = controller._queryCategoryBits;
		filter.maskBits = controller._queryMaskBits;
		filter.hitTriggers = false;

		return PhysicsBackend::CastMover(_islands[PHYSICS_DEFAULT_ISLAND].world, position, capsule, translation, filter,
										 PhysicsBackend::ShapeHandle{controller._shapeBits});
	}

	bool PhysicsWorld::ProbeController(const CharacterController& controller, const Vec3& origin, const Vec3& translation,
									   Vec3& normal) {
		if (_islandCount == 0) {
			return false;
		}

		PhysicsBackend::QueryFilter filter;
		filter.categoryBits = controller._queryCategoryBits;
		filter.maskBits = controller._queryMaskBits;
		filter.hitTriggers = false;

		PhysicsBackend::QueryHit hit;
		if (!PhysicsBackend::CastRay(_islands[PHYSICS_DEFAULT_ISLAND].world, origin, translation, filter, hit)) {
			return false;
		}
		if (ResolveShapeCollider(hit.shapeOwner).Get() == static_cast<const Collider*>(&controller)) {
			return false;
		}
		normal = hit.normal;
		return true;
	}

	void PhysicsWorld::PlaceController(CharacterController& controller) {
		if (controller._bodyBits == 0) {
			return;
		}
		SceneObject* owner = controller.GetOwner().Get();
		if (!owner) {
			return;
		}
		const Vec3 position = owner->GetWorldPosition();
		if (position == controller._syncedPosition) {
			return;
		}
		PhysicsBackend::SetBodyTransform(PhysicsBackend::BodyHandle{controller._bodyBits}, position, Quat::Identity());
		controller._syncedPosition = position;
	}

	Handle<Rigidbody> PhysicsWorld::FindCarrier(u64 shapeOwner, const Vec3& normal) const {
		Collider* collider = ResolveShapeCollider(shapeOwner).Get();
		Rigidbody* body = collider ? FindOwningBody(*collider) : nullptr;
		if (!body || !body->GetCarryRiders() || Vec3::Dot(normal, CarrierUp(*body)) < PHYSICS_CARRY_UP_DOT) {
			return {};
		}
		return Handle<Rigidbody>(body);
	}

	void PhysicsWorld::DispatchControllerHits(Handle<CharacterController> controller,
											  std::span<const ControllerContact> contacts) {
		for (const ControllerContact& contact : contacts) {
			CharacterController* self = controller.Get();
			if (!self || self->IsPendingDestroy()) {
				return;
			}
			const Handle<SceneObject> owner = self->GetOwner();

			ControllerColliderHit hit;
			hit.controller = controller;
			hit.collider = ResolveShapeCollider(contact.shapeOwner);
			Collider* collider = hit.collider.Get();
			if (!collider) {
				continue;
			}
			hit.object = collider->GetOwner();
			hit.rigidbody = Handle<Rigidbody>(FindOwningBody(*collider));
			hit.point = contact.point;
			hit.normal = contact.normal;
			hit.moveDirection = contact.direction;
			hit.moveLength = contact.travelled;

			for (u32 index = 0;; ++index) {
				SceneObject* object = owner.Get();
				if (!object || index >= object->GetComponentCount()) {
					break;
				}
				Component* component = object->GetComponentAt(index).Get();
				if (!component || component->IsPendingDestroy()) {
					continue;
				}
				if (PhysicsListener* listener = FindListener(component)) {
					listener->OnControllerColliderHit(hit);
				}
			}
		}
	}

	void PhysicsWorld::WriteTransform(Rigidbody& body, const Vec3& position, const Quat& rotation) {
		SceneObject* owner = body.GetOwner().Get();
		if (!owner) {
			return;
		}

		owner->SetWorldPosition(position);
		owner->SetWorldRotation(rotation);

		body._syncedPosition = owner->GetWorldPosition();
		body._syncedRotation = owner->GetWorldRotation();
	}

} // namespace Shalltear
