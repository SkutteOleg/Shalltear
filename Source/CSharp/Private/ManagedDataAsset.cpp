#include "ManagedDataAsset.h"

#include "ScriptRuntime.h"
#include "ShadowStorage.h"

namespace Shalltear::CSharpImpl {

	ManagedDataAsset::ManagedDataAsset(ManagedType& type) : _type(type) {
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

	ManagedDataAsset::~ManagedDataAsset() {
		ReleaseInstance();
		ShadowStorage::Destroy(this, _type.inits);
		--_type.liveCount;
		ManagedTypes::Unlink(*this);
	}

	void ManagedDataAsset::ReleaseInstance() {
		if (_dirty) {
			ManagedTypes::UnlinkDirty(*this);
			_dirty = false;
		}
		if (_instance && ScriptRuntime::IsAttached()) {
			ScriptRuntime::GetManaged().destroyInstance(_instance);
		}
		_instance = 0;
	}

	void ManagedDataAsset::SyncFields(void* instance, FieldSync phase) {
		ManagedDataAsset& proxy = *static_cast<ManagedDataAsset*>(instance);
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

	void ManagedDataAsset::Push() {
		ScriptRuntime::GetManaged().pushFields(_instance, this);
	}

	void ManagedDataAsset::Pull() {
		ManagedTypes::UnlinkDirty(*this);
		_dirty = false;
		ScriptRuntime::GetManaged().pullFields(_instance, this, GetInstanceId());
	}

} // namespace Shalltear::CSharpImpl
