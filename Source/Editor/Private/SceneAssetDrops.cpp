#include "Shalltear/Editor/SceneAssetDrops.h"

#include "EditorSceneBuild.h"

#include "Shalltear/Core/Asset/Asset.h"
#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Reflection/Primitives.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Reflection/Walk.h"
#include "Shalltear/Core/Scene/Component.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "Shalltear/Core/UI/DragDrop.h"
#include "Shalltear/Editor/EditorDragData.h"
#include "Shalltear/Editor/EditorSelection.h"

#include <algorithm>
#include <cstring>

namespace Shalltear {

	namespace {

		struct Entry {
			StringHash assetType;
			StringHash component;
			StringHash assetField;
			const void* owner = nullptr;
		};

		struct DropsState {
			Vector<Entry> entries{GetMemoryResource(MemTag::Editor)};
		};

		DropsState* _state = nullptr;

		DropsState& State() {
			if (!_state) {
				_state = New<DropsState>(MemTag::Editor);
			}
			return *_state;
		}

		const Entry* Find(StringHash assetType) {
			if (!_state) {
				return nullptr;
			}
			auto found = std::find_if(_state->entries.begin(), _state->entries.end(), [assetType](const Entry& entry) { return entry.assetType == assetType; });
			return found == _state->entries.end() ? nullptr : &*found;
		}

		const Entry* Dragged(const UI::DragPayload& payload, Guid& outAsset) {
			if (payload.type != "Asset"_hash || !payload.data.Is<AssetDragData>()) {
				return nullptr;
			}
			const AssetDragData& data = payload.data.As<AssetDragData>();
			const Entry* entry = data.guid.IsValid() ? Find(StringHash{data.assetTypeHash}) : nullptr;
			if (entry) {
				outAsset = data.guid;
			}
			return entry;
		}

		str NameOf(const Guid& asset) {
			const str path = AssetDatabase::LookupPath(asset).ToString();
			const size_t slash = path.find_last_of("/\\");
			str name = slash == str::npos ? path : path.substr(slash + 1);
			const size_t dot = name.find_last_of('.');
			if (dot != str::npos && dot > 0) {
				name.resize(dot);
			}
			return name.empty() ? str("New Object") : name;
		}

		void PointAt(Handle<SceneObject> object, StringHash component, StringHash assetField, const Guid& asset) {
			SceneObject* live = object.Get();
			const TypeInfo* type = TypeRegistry::FindType(component);
			const FieldInfo* field = type ? FindField(type, assetField) : nullptr;
			if (!live || !field || !(field->flags & FieldFlagHandle) || !field->addrFn || field->size != sizeof(u64)) {
				SLT_LOG_WARN("Editor", "a dropped asset wasn't assigned (its registered field isn't an asset handle on its component)");
				return;
			}
			Asset* loaded = AssetDatabase::LoadAssetByGuid(asset);
			const TypeInfo* pointee = TypeRegistry::FindType(HandlePointeeName(*field->fieldType));
			if (!loaded || !pointee || !TypeRegistry::IsDerivedFrom(loaded->GetType(), pointee)) {
				SLT_LOG_WARN("Editor", "a dropped asset wasn't assigned ('{}' didn't load as what its component's field holds)",
							 AssetDatabase::LookupPath(asset).ToString());
				return;
			}
			for (u32 index = 0; index < live->GetComponentCount(); ++index) {
				Component* held = live->GetComponentAt(index).Get();
				if (held && TypeRegistry::IsDerivedFrom(held->GetType(), type)) {
					const u64 bits = loaded->GetInstanceId();
					SyncFieldsBeforeRead(held, held->GetType());
					std::memcpy(field->addrFn(held), &bits, sizeof(bits));
					SyncFieldsAfterWrite(held, held->GetType());
					return;
				}
			}
		}

	} // anonymous namespace

	namespace SceneAssetDrops {

		bool Register(StringHash assetType, StringHash component, StringHash assetField, const void* owner) {
			if (!owner) {
				SLT_LOG_WARN("Editor", "ignoring an asset drop with no owner to sweep it with");
				return false;
			}
			if (Find(assetType)) {
				SLT_LOG_WARN("Editor", "ignoring an asset drop: its asset type is registered already");
				return false;
			}
			State().entries.push_back({assetType, component, assetField, owner});
			return true;
		}

		u32 UnregisterOwnedBy(const void* owner) {
			if (!_state || !owner) {
				return 0;
			}
			return static_cast<u32>(std::erase_if(_state->entries, [owner](const Entry& entry) { return entry.owner == owner; }));
		}

		bool Accepts(const UI::DragPayload& payload, Handle<Scene> scene) {
			Guid asset;
			return scene.IsValid() && Dragged(payload, asset) != nullptr;
		}

		Handle<SceneObject> Drop(const UI::DragPayload& payload, Handle<Scene> scene, Handle<SceneObject> parent, const Vec3* worldPosition) {
			Guid asset;
			const Entry* entry = scene.IsValid() ? Dragged(payload, asset) : nullptr;
			if (!entry) {
				return {};
			}
			const StringHash component = entry->component;
			const StringHash field = entry->assetField;
			const bool placed = worldPosition != nullptr;
			const Vec3 at = placed ? *worldPosition : Vec3{};
			Handle<SceneObject> created =
				EditorSceneBuild::Push(NameOf(asset), scene, parent, component, [component, field, asset, placed, at](Handle<SceneObject> object) {
					if (placed && object.IsValid()) {
						object.Get()->SetWorldPosition(at);
					}
					PointAt(object, component, field, asset);
				});
			if (created.IsValid()) {
				EditorSelection::Select(Handle<Object>(static_cast<Object*>(created.Get())));
			}
			return created;
		}

		void Terminate() {
			if (_state) {
				Delete(_state);
				_state = nullptr;
			}
		}

	} // namespace SceneAssetDrops

} // namespace Shalltear
