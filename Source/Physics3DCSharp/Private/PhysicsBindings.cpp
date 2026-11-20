#include "PhysicsBindings.h"

#include "BindingSupport.h"
#include "ManagedTypes.h"
#include "PackageBindings.h"

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Scene/SceneObject.h"

#include "Shalltear/Physics3D/PhysicsListener.h"

namespace Shalltear::CSharpImpl::PhysicsBindings {

	namespace {

		constexpr i32 PHASE_ENTER = 0;
		constexpr i32 PHASE_STAY = 1;
		constexpr i32 PHASE_EXIT = 2;

		struct Entries {
			void (*collision)(intptr_t instance, int32_t phase, const SltCollision* collision) = nullptr;
			void (*trigger)(intptr_t instance, int32_t phase, uint64_t other) = nullptr;
			void (*jointBreak)(intptr_t instance, float breakForce) = nullptr;
			void (*controllerHit)(intptr_t instance, const SltControllerColliderHit* hit) = nullptr;
		};

		Entries _entries;
		i32 _slot = -1;
		Vector<Handle<Collider>> _overlaps;

		SltContactPoint FromEngine(const PhysicsContactPoint& contact) {
			return SltContactPoint{contact.point, contact.normal, contact.separation, contact.normalImpulse};
		}

		class Relay final : public PhysicsListener {
		public:
			intptr_t target = 0;

			void OnCollisionEnter(const Collision& collision) override { Send(PHASE_ENTER, collision); }
			void OnCollisionStay(const Collision& collision) override { Send(PHASE_STAY, collision); }
			void OnCollisionExit(const Collision& collision) override { Send(PHASE_EXIT, collision); }
			void OnTriggerEnter(Handle<Collider> other) override { Send(PHASE_ENTER, other); }
			void OnTriggerStay(Handle<Collider> other) override { Send(PHASE_STAY, other); }
			void OnTriggerExit(Handle<Collider> other) override { Send(PHASE_EXIT, other); }

			void OnJointBreak(f32 breakForce) override {
				const intptr_t instance = target;
				ManagedTypes::FlushDirty();
				_entries.jointBreak(instance, breakForce);
			}

			void OnControllerColliderHit(const ControllerColliderHit& hit) override {
				const intptr_t instance = target;
				const SltControllerColliderHit sent{IdOf(hit.controller), IdOf(hit.collider), IdOf(hit.object), IdOf(hit.rigidbody),
													hit.point, hit.normal, hit.moveDirection, hit.moveLength};
				ManagedTypes::FlushDirty();
				_entries.controllerHit(instance, &sent);
			}
		private:
			void Send(i32 phase, const Collision& collision) const {
				const intptr_t instance = target;
				SltCollision sent{};
				sent.colliderId = IdOf(collision.collider);
				sent.objectId = IdOf(collision.object);
				sent.rigidbodyId = IdOf(collision.rigidbody);
				sent.relativeVelocity = collision.relativeVelocity;
				sent.contactCount = static_cast<int32_t>(collision.contactCount);
				sent.contact0 = FromEngine(collision.contacts[0]);
				sent.contact1 = FromEngine(collision.contacts[1]);
				sent.contact2 = FromEngine(collision.contacts[2]);
				sent.contact3 = FromEngine(collision.contacts[3]);
				ManagedTypes::FlushDirty();
				_entries.collision(instance, phase, &sent);
			}

			void Send(i32 phase, const Handle<Collider>& other) const {
				const intptr_t instance = target;
				ManagedTypes::FlushDirty();
				_entries.trigger(instance, phase, IdOf(other));
			}
		};

		Relay _relay;

		PhysicsListener* Resolve(Component& component) {
			if (_slot < 0) {
				return nullptr;
			}
			const intptr_t instance = PackageBindings::FindListener(component, _slot);
			if (!instance) {
				return nullptr;
			}
			_relay.target = instance;
			return &_relay;
		}

		PhysicsQueryFilter MakeFilter(std::string_view categories, PhysicsCategoryMatch collidesWithMode, std::string_view collidesWith,
									  PhysicsQueryTriggers triggers) {
			PhysicsQueryFilter filter;
			filter.categories = categories;
			filter.collidesWithMode = collidesWithMode;
			filter.collidesWith = collidesWith;
			filter.triggerInteraction = triggers;
			return filter;
		}

		bool Report(bool found, const RaycastHit& hit, std::span<SltRaycastHit> out) {
			if (found && !out.empty()) {
				out[0] = SltRaycastHit{IdOf(hit.collider), IdOf(hit.object), IdOf(hit.rigidbody), hit.point, hit.normal, hit.distance};
			}
			return found;
		}

	} // anonymous namespace

	bool CastRay(const Vec3& origin, const Vec3& direction, f32 maxDistance, std::string_view categories, PhysicsCategoryMatch collidesWithMode,
				 std::string_view collidesWith, PhysicsQueryTriggers triggers, std::span<SltRaycastHit> hit) {
		RaycastHit found;
		return Report(Physics::Raycast(Ray(origin, direction), found, maxDistance, MakeFilter(categories, collidesWithMode, collidesWith, triggers)), found,
					  hit);
	}

	bool CastSphere(const Vec3& origin, const Vec3& direction, f32 radius, f32 maxDistance, std::string_view categories,
					PhysicsCategoryMatch collidesWithMode, std::string_view collidesWith, PhysicsQueryTriggers triggers, std::span<SltRaycastHit> hit) {
		RaycastHit found;
		return Report(Physics::SphereCast(Ray(origin, direction), radius, found, maxDistance,
										  MakeFilter(categories, collidesWithMode, collidesWith, triggers)),
					  found, hit);
	}

	bool CastCapsule(const Vec3& point1, const Vec3& point2, f32 radius, const Vec3& direction, f32 maxDistance, std::string_view categories,
					 PhysicsCategoryMatch collidesWithMode, std::string_view collidesWith, PhysicsQueryTriggers triggers, std::span<SltRaycastHit> hit) {
		RaycastHit found;
		return Report(Physics::CapsuleCast(point1, point2, radius, direction, found, maxDistance,
										   MakeFilter(categories, collidesWithMode, collidesWith, triggers)),
					  found, hit);
	}

	i32 FindOverlaps(const Vec3& center, f32 radius, std::string_view categories, PhysicsCategoryMatch collidesWithMode, std::string_view collidesWith,
					 PhysicsQueryTriggers triggers, std::span<u64> ids) {
		_overlaps.clear();
		Physics::OverlapSphere(center, radius, _overlaps, MakeFilter(categories, collidesWithMode, collidesWith, triggers));
		for (size_t index = 0; index < _overlaps.size() && index < ids.size(); ++index) {
			ids[index] = IdOf(_overlaps[index]);
		}
		return static_cast<i32>(_overlaps.size());
	}

	i32 AttachEvents(i64 collision, i64 trigger, i64 jointBreak, i64 controllerHit) {
		_entries.collision = reinterpret_cast<decltype(Entries::collision)>(static_cast<intptr_t>(collision));
		_entries.trigger = reinterpret_cast<decltype(Entries::trigger)>(static_cast<intptr_t>(trigger));
		_entries.jointBreak = reinterpret_cast<decltype(Entries::jointBreak)>(static_cast<intptr_t>(jointBreak));
		_entries.controllerHit = reinterpret_cast<decltype(Entries::controllerHit)>(static_cast<intptr_t>(controllerHit));
		_slot = PackageBindings::ReserveListenerSlot("Physics3D");
		return _slot;
	}

	void Register() {
		Physics::AddListenerResolver(&Resolve);
	}

	void Unregister() {
		Physics::RemoveListenerResolver(&Resolve);
		_entries = {};
		_slot = -1;
		_overlaps = {};
	}

} // namespace Shalltear::CSharpImpl::PhysicsBindings
