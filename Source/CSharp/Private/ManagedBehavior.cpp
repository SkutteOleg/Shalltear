#include "ManagedBehavior.h"

#include "ScriptRuntime.h"

namespace Shalltear::CSharpImpl {

	ManagedBehavior::ManagedBehavior(ManagedType& type) : _type(type) {
		ManagedTypes::Link(*this);
		++_type.liveCount;
		if (ScriptRuntime::IsAttached()) {
			_instance = ScriptRuntime::GetManaged().createInstance(type.managedTypeId, GetInstanceId());
		}
	}

	ManagedBehavior::~ManagedBehavior() {
		ReleaseInstance();
		--_type.liveCount;
		ManagedTypes::Unlink(*this);
	}

	void ManagedBehavior::ReleaseInstance() {
		if (_instance && ScriptRuntime::IsAttached()) {
			ScriptRuntime::GetManaged().destroyInstance(_instance);
		}
		_instance = 0;
	}

	void ManagedBehavior::Invoke(i32 hook) {
		if (!_instance || !(_type.hookMask & (1u << hook))) {
			return;
		}
#ifdef SLT_CSHARP_TEST_HOOKS
		ManagedTypes::CountHookCrossing();
#endif
		ScriptRuntime::GetManaged().invokeHook(_instance, hook);
	}

} // namespace Shalltear::CSharpImpl
