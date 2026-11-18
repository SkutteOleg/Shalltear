#include "ManagedBehavior.h"

#include "ScriptRuntime.h"

namespace Shalltear::CSharpImpl {

	ManagedBehavior::ManagedBehavior(ManagedType& type) : _type(type) {
		ManagedTypes::Link(*this);
		++_type.liveCount;
		if (ScriptRuntime::IsAttached()) {
			_instance = ScriptRuntime::GetManaged().createInstance(type.managedTypeId, GetInstanceId());
		}
		if (_instance && _type.hasFields) {
			Push();
		}
	}

	ManagedBehavior::~ManagedBehavior() {
		ReleaseInstance();
		--_type.liveCount;
		ManagedTypes::Unlink(*this);
	}

	void ManagedBehavior::ReleaseInstance() {
		if (_dirty) {
			ManagedTypes::UnlinkDirty(*this);
			_dirty = false;
		}
		if (_instance && ScriptRuntime::IsAttached()) {
			ScriptRuntime::GetManaged().destroyInstance(_instance);
		}
		_instance = 0;
	}

	void ManagedBehavior::SyncFields(void* instance, FieldSync phase) {
		ManagedBehavior& proxy = *static_cast<ManagedBehavior*>(instance);
		if (!proxy._instance || proxy._dirty) {
			return;
		}
		if (phase == FieldSync::BeforeRead) {
			proxy.Push();
			return;
		}
		proxy._dirty = true;
		ManagedTypes::LinkDirty(proxy);
	}

	void ManagedBehavior::Push() {
#ifdef SLT_CSHARP_TEST_HOOKS
		++ManagedTypes::GetCounters().pushes;
#endif
		ScriptRuntime::GetManaged().pushFields(_instance, this);
	}

	void ManagedBehavior::Pull() {
		ManagedTypes::UnlinkDirty(*this);
		_dirty = false;
#ifdef SLT_CSHARP_TEST_HOOKS
		++ManagedTypes::GetCounters().pulls;
#endif
		ScriptRuntime::GetManaged().pullFields(_instance, this, GetInstanceId());
	}

	void ManagedBehavior::Invoke(i32 hook) {
		if (!_instance || !(_type.hookMask & (1u << hook))) {
			return;
		}
		ManagedTypes::FlushDirty();
#ifdef SLT_CSHARP_TEST_HOOKS
		++ManagedTypes::GetCounters().hookCrossings;
#endif
		ScriptRuntime::GetManaged().invokeHook(_instance, hook);
	}

} // namespace Shalltear::CSharpImpl
