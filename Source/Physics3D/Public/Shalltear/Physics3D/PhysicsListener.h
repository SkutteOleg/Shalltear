#pragma once

#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/Math/Vector.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/Physics3D/CharacterController.h"
#include "Shalltear/Physics3D/Colliders.h"
#include "Shalltear/Physics3D/Physics3DAPI.h"
#include "Shalltear/Physics3D/Rigidbody.h"

namespace Shalltear {

	class Component;
	class SceneObject;

	struct PhysicsContactPoint {
		Vec3 point;

		Vec3 normal;

		// overlapping when negative, speculative when positive
		f32 separation = 0.0f;

		f32 normalImpulse = 0.0f;
	};

	struct Collision {
		static constexpr u32 MAX_CONTACTS = 4;

		Handle<Collider> collider;

		Handle<SceneObject> object;

		Handle<Rigidbody> rigidbody;

		Vec3 relativeVelocity;

		u32 contactCount = 0;

		PhysicsContactPoint contacts[MAX_CONTACTS];
	};

	class SHALLTEAR_PHYSICS3D_API PhysicsListener {
	public:
		// key function (type info must live in 1 lib for dynamic_cast)
		virtual ~PhysicsListener();

		virtual void OnCollisionEnter(const Collision& collision) { (void)collision; }

		// once per fixed step, never for a sleeping body
		virtual void OnCollisionStay(const Collision& collision) { (void)collision; }

		// collision.collider may be null (other may be destroyed)
		virtual void OnCollisionExit(const Collision& collision) { (void)collision; }

		virtual void OnTriggerEnter(Handle<Collider> other) { (void)other; }

		virtual void OnTriggerStay(Handle<Collider> other) { (void)other; }

		virtual void OnTriggerExit(Handle<Collider> other) { (void)other; }

		// joint already gone from the solver
		virtual void OnJointBreak(f32 breakForce) { (void)breakForce; }

		// once per collider per Move, after the move
		virtual void OnControllerColliderHit(const ControllerColliderHit& hit) { (void)hit; }
	};

	// result valid until next resolver call
	using PhysicsListenerResolver = PhysicsListener* (*)(Component& component);

	namespace Physics {

		inline constexpr u32 MAX_LISTENER_RESOLVERS = 4;

		SHALLTEAR_PHYSICS3D_API void AddListenerResolver(PhysicsListenerResolver resolver);
		SHALLTEAR_PHYSICS3D_API void RemoveListenerResolver(PhysicsListenerResolver resolver);

	} // namespace Physics

} // namespace Shalltear
