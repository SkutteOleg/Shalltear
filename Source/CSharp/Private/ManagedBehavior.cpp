#include "ManagedBehavior.h"

#include "ScriptRuntime.h"

#include "Shalltear/Core/Profiler.h"

namespace Shalltear::CSharpImpl {

	ManagedBehavior::ManagedBehavior(ManagedType& type) : _type(type) {
		ManagedTypes::Link(*this);
		++_type.liveCount;
		ShadowStorage::Construct(this, _type.inits);
		if (ScriptRuntime::IsAttached()) {
			_instance = ScriptRuntime::GetManaged().createInstance(type.managedTypeId, GetInstanceId());
		}
		if (_instance && _type.hasFields) {
			Push();
			if (_type.normalizes) {
				ScriptRuntime::GetManaged().pullFields(_instance, this, GetInstanceId());
			}
		}
		if (_instance && ManagedTypes::IsRestoring()) {
			_dirty = true;
			ManagedTypes::LinkDirty(*this);
		}
	}

	ManagedBehavior::~ManagedBehavior() {
		ReleaseInstance();
		ShadowStorage::Destroy(this, _type.inits);
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
#ifndef SHALLTEAR_CONFIG_RELEASE
		// must match SLT_HOOK_* in ScriptBridge.h
		static const u32 sites[] = {
			Profiler::RegisterSite("C# OnAttach"),
			Profiler::RegisterSite("C# OnDetach"),
			Profiler::RegisterSite("C# OnEnable"),
			Profiler::RegisterSite("C# OnDisable"),
			Profiler::RegisterSite("C# OnDestroy"),
			Profiler::RegisterSite("C# OnUpdate"),
			Profiler::RegisterSite("C# OnFixedUpdate"),
			Profiler::RegisterSite("C# OnLateUpdate"),
		};
		const Profiler::ScopedZone zone(sites[hook]);
#endif
		if (hook > SLT_HOOK_DESTROY) {
			ScriptRuntime::GetManaged().invokeHook(_instance, hook);
			return;
		}
		++ManagedTypes::GetLifecycleDepth();
		ScriptRuntime::GetManaged().invokeHook(_instance, hook);
		--ManagedTypes::GetLifecycleDepth();
	}

} // namespace Shalltear::CSharpImpl
