#pragma once

#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/Math/Vector.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/Physics3D/Colliders.h"
#include "Shalltear/Physics3D/Rigidbody.h"

namespace Shalltear {

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

		[[nodiscard]] const PhysicsContactPoint& GetContact(u32 index) const noexcept {
			return contacts[index < MAX_CONTACTS ? index : 0];
		}

		[[nodiscard]] const PhysicsContactPoint& GetContact(i32 index) const noexcept {
			return contacts[(index >= 0 && static_cast<u32>(index) < MAX_CONTACTS) ? static_cast<u32>(index) : 0];
		}
	};

	class PhysicsListener {
	public:
		virtual ~PhysicsListener() = default;

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
	};

} // namespace Shalltear
