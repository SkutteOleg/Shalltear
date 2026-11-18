#include "Shalltear/Editor/EditTarget.h"

#include "Shalltear/Core/Asset/Asset.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Reflection/Walk.h"

#include "Shalltear/Editor/EditorAssetOps.h"
#include "Shalltear/Editor/EditorSceneOps.h"

#include <algorithm>
#include <vector>

namespace Shalltear {

	namespace {

		u32 _readScopes = 0;
		std::vector<u64> _syncedInScope;

	} // anonymous namespace

	EditTarget::ReadScope::ReadScope() {
		++_readScopes;
	}

	EditTarget::ReadScope::~ReadScope() {
		if (--_readScopes == 0) {
			_syncedInScope.clear();
		}
	}

	void EditTarget::SyncForRead(Object& object) {
		if (_readScopes > 0) {
			const u64 id = object.GetInstanceId();
			if (std::find(_syncedInScope.begin(), _syncedInScope.end(), id) != _syncedInScope.end()) {
				return;
			}
			_syncedInScope.push_back(id);
		}
		SyncFieldsBeforeRead(&object, object.GetType());
	}

	EditTarget EditTarget::PrefabRow(const Guid& prefabId, u64 rowKey) {
		EditTarget target;
		if (!prefabId.IsValid() || rowKey == 0) {
			return target;
		}
		target._prefabId = prefabId;
		target._rowKey = rowKey;
		return target;
	}

	Object* EditTarget::Find() const {
		if (_rowKey != 0) {
			return EditorSceneOps::ResolvePrefabRow(_prefabId, _rowKey);
		}
		return _liveId != 0 ? ObjectRegistry::Resolve(_liveId) : nullptr;
	}

	Object* EditTarget::Resolve() const {
		Object* object = Find();
		if (object) {
			SyncForRead(*object);
		}
		return object;
	}

	void EditTarget::NotifyWritten() const {
		Object* object = Find();
		if (object) {
			SyncFieldsAfterWrite(object, object->GetType());
		}
		if (_rowKey != 0) {
			EditorSceneOps::MarkPrefabPreviewEdited(_prefabId);
			return;
		}
		if (object && TypeRegistry::IsDerivedFrom(object->GetType(), Asset::GetReflectType())) {
			EditorAssetOps::MarkDirty(Handle<Asset>(static_cast<Asset*>(object)));
		}
	}

	bool EditTarget::operator==(const EditTarget& other) const {
		return _liveId == other._liveId && _rowKey == other._rowKey && _prefabId == other._prefabId;
	}

} // namespace Shalltear
