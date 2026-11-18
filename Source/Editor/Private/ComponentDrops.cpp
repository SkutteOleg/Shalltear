#include "Shalltear/Editor/ComponentDrops.h"

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Reflection/TypeInfo.h"
#include "Shalltear/Core/Scene/Component.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "Shalltear/Core/UI/DragDrop.h"

#include "Shalltear/Editor/EditorCommands.h"
#include "Shalltear/Editor/EditorDragData.h"
#include "Shalltear/Editor/UndoSystem.h"

#include <algorithm>

namespace Shalltear {

	namespace {

		struct Entry {
			StringHash assetType;
			ComponentDrops::ResolveFn resolve;
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

	} // anonymous namespace

	namespace ComponentDrops {

		bool Register(StringHash assetType, ResolveFn resolve, const void* owner) {
			if (!resolve) {
				return false;
			}
			if (!owner) {
				SLT_LOG_WARN("Editor", "ignoring a component drop with no owner to sweep it with");
				return false;
			}
			if (Find(assetType)) {
				SLT_LOG_WARN("Editor", "ignoring a component drop: its asset type is registered already");
				return false;
			}
			State().entries.push_back({assetType, std::move(resolve), owner});
			return true;
		}

		u32 UnregisterOwnedBy(const void* owner) {
			if (!_state || !owner) {
				return 0;
			}
			return static_cast<u32>(std::erase_if(_state->entries, [owner](const Entry& entry) { return entry.owner == owner; }));
		}

		bool Accepts(const UI::DragPayload& payload) {
			Guid asset;
			return Dragged(payload, asset) != nullptr;
		}

		Handle<Component> Drop(const UI::DragPayload& payload, Handle<SceneObject> object) {
			Guid asset;
			const Entry* entry = object.IsValid() ? Dragged(payload, asset) : nullptr;
			if (!entry) {
				return {};
			}
			const ResolveFn resolve = entry->resolve;
			const TypeInfo* type = resolve(asset);
			if (!type || !(type->flags & TypeFlagComponent) || (type->flags & TypeFlagAbstract)) {
				return {};
			}
			const u32 before = object->GetComponentCount();
			UndoSystem::Push(MakeUnique<AddComponentCommand>(object, type->name));
			const SceneObject* held = object.Get();
			return held && held->GetComponentCount() > before ? held->GetComponentAt(held->GetComponentCount() - 1) : Handle<Component>();
		}

		void Terminate() {
			if (_state) {
				Delete(_state);
				_state = nullptr;
			}
		}

	} // namespace ComponentDrops

} // namespace Shalltear
