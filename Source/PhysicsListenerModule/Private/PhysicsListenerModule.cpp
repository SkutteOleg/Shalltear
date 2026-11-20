#include "Shalltear/Core/Module/ModuleAPI.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene/Behavior.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/Physics3D/PhysicsListener.h"

namespace PhysicsListenerModuleImpl {

	using namespace Shalltear;

	class ContactProbe : public Behavior, public PhysicsListener {
	public:
		SLT_REFLECT_COMPONENT(ContactProbe, Behavior)
		SLT_REFLECT_FIELD(collisionEnter)
		SLT_REFLECT_FIELD(collisionStay)
		SLT_REFLECT_FIELD(collisionExit)
		SLT_REFLECT_FIELD(triggerEnter)
		SLT_REFLECT_FIELD(triggerStay)
		SLT_REFLECT_FIELD(triggerExit)
		SLT_REFLECT_FIELD(lastOtherName)
		SLT_REFLECT_FIELD(lastContactCount)
		SLT_REFLECT_FIELD(lastNormalY)
		SLT_REFLECT_FIELD(lastHadRigidbody)
		SLT_REFLECT_FIELD(jointBreak)
		SLT_REFLECT_FIELD(lastBreakForce)
		SLT_REFLECT_FIELD(controllerHit)
		SLT_END_REFLECT()

		i32 collisionEnter = 0;
		i32 collisionStay = 0;
		i32 collisionExit = 0;
		i32 triggerEnter = 0;
		i32 triggerStay = 0;
		i32 triggerExit = 0;

		i32 jointBreak = 0;

		str lastOtherName;
		i32 lastContactCount = 0;
		f32 lastNormalY = 0.0f;
		bool lastHadRigidbody = false;

		f32 lastBreakForce = 0.0f;

		i32 controllerHit = 0;

		void OnCollisionEnter(const Collision& collision) override {
			++collisionEnter;
			Record(collision);
		}

		void OnCollisionStay(const Collision& collision) override {
			++collisionStay;
			Record(collision);
		}

		void OnCollisionExit(const Collision& collision) override {
			++collisionExit;
			Record(collision);
		}

		void OnTriggerEnter(Handle<Collider> other) override {
			++triggerEnter;
			RecordOther(other);
		}

		void OnTriggerStay(Handle<Collider> other) override {
			++triggerStay;
			RecordOther(other);
		}

		void OnTriggerExit(Handle<Collider> other) override {
			++triggerExit;
			RecordOther(other);
		}

		void OnJointBreak(f32 breakForce) override {
			++jointBreak;
			lastBreakForce = breakForce;
		}

		void OnControllerColliderHit(const ControllerColliderHit& hit) override {
			++controllerHit;
			RecordOther(hit.collider);
		}
	private:
		void RecordOther(Handle<Collider> other) {
			Collider* resolved = other.Get();
			SceneObject* owner = resolved ? resolved->GetOwner().Get() : nullptr;
			lastOtherName = owner ? owner->name : str();
		}

		void Record(const Collision& collision) {
			RecordOther(collision.collider);
			lastContactCount = static_cast<i32>(collision.contactCount);
			lastNormalY = collision.contactCount > 0 ? collision.contacts[0].normal.y : 0.0f;
			lastHadRigidbody = collision.rigidbody.GetInstanceId() != 0;
		}
	};

	class ResolvedProbe : public Behavior {
	public:
		SLT_REFLECT_COMPONENT(ResolvedProbe, Behavior)
		SLT_REFLECT_FIELD(collisionEnter)
		SLT_REFLECT_FIELD(collisionStay)
		SLT_REFLECT_FIELD(collisionExit)
		SLT_REFLECT_FIELD(triggerEnter)
		SLT_REFLECT_FIELD(jointBreak)
		SLT_END_REFLECT()

		i32 collisionEnter = 0;
		i32 collisionStay = 0;
		i32 collisionExit = 0;
		i32 triggerEnter = 0;
		i32 jointBreak = 0;
	};

	class ResolvedRelay final : public PhysicsListener {
	public:
		ResolvedProbe* target = nullptr;

		void OnCollisionEnter(const Collision&) override { ++target->collisionEnter; }
		void OnCollisionStay(const Collision&) override { ++target->collisionStay; }
		void OnCollisionExit(const Collision&) override { ++target->collisionExit; }
		void OnTriggerEnter(Handle<Collider>) override { ++target->triggerEnter; }
		void OnJointBreak(f32) override { ++target->jointBreak; }
	};

	ResolvedRelay _relay;

	PhysicsListener* Resolve(Component& component) {
		if (component.GetType() != ResolvedProbe::GetReflectType()) {
			return nullptr;
		}
		_relay.target = static_cast<ResolvedProbe*>(&component);
		return &_relay;
	}

	void Register(const void*) {
		Physics::AddListenerResolver(&Resolve);
	}

	void Shutdown() {
		Physics::RemoveListenerResolver(&Resolve);
	}

} // namespace PhysicsListenerModuleImpl

SLT_REGISTER_TYPE(PhysicsListenerModuleImpl::ContactProbe)
SLT_REGISTER_TYPE(PhysicsListenerModuleImpl::ResolvedProbe)

SLT_IMPLEMENT_MODULE_WITH_SHUTDOWN(PhysicsListenerModule, &PhysicsListenerModuleImpl::Shutdown)
SLT_IMPLEMENT_CORE_EXTENSIONS(&PhysicsListenerModuleImpl::Register)
