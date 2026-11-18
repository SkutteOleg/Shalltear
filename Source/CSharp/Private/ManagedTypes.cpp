#include "ManagedTypes.h"

#include "FieldKinds.h"
#include "ManagedBehavior.h"

#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Reflection/Primitives.h"
#include "Shalltear/Core/Reflection/Reflect.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"

#include <array>
#include <cstring>
#include <new>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Shalltear::CSharpImpl::ManagedTypes {

	namespace {

		// owner of C# types (image base would also sweep package's C++ types)
		const char _TOKEN = 0;

		constexpr size_t SLOT_BYTES = 8;
		constexpr size_t THUNK_COUNT = 1024;
		constexpr u32 FIRST_SLOT = static_cast<u32>((sizeof(ManagedBehavior) + 15) / 16 * 2);
		static_assert(FIRST_SLOT + SLT_FIELD_SLOT_LIMIT <= THUNK_COUNT);

		template <size_t N>
		void* AtSlot(void* instance) {
			return static_cast<u8*>(instance) + N * SLOT_BYTES;
		}

		template <size_t... N>
		constexpr std::array<void* (*)(void*), sizeof...(N)> MakeThunks(std::index_sequence<N...>) {
			return {&AtSlot<N>...};
		}

		// no offset passed to addrFn -> 1 function per slot
		constexpr auto SlotThunks = MakeThunks(std::make_index_sequence<THUNK_COUNT>{});

		std::vector<ManagedType*> _types;
		std::vector<ManagedType*> _byIndex;
		std::vector<ManagedType*> _retired;
		std::unordered_map<const TypeInfo*, ManagedType*> _byInfo;
		ManagedBehavior* _live = nullptr;
		TypeObserver _observer;
		Counters _counters;
		i32 _lifecycleDepth = 0;

		std::string_view View(SltString text) {
			return text.ptr && text.length > 0 ? std::string_view(text.ptr, static_cast<size_t>(text.length)) : std::string_view{};
		}

		Object* Create(const TypeInfo* info, MemTag tag) {
			const auto found = _byInfo.find(info);
			if (found == _byInfo.end()) {
				return nullptr;
			}
			void* memory = Allocate(info->size, tag, info->alignment);
			std::memset(static_cast<u8*>(memory) + sizeof(ManagedBehavior), 0, info->size - sizeof(ManagedBehavior));
			return new (memory) ManagedBehavior(*found->second);
		}

		void AddField(ReflectBuilder& builder, const SltFieldDesc& field, const FieldKindInfo& kind, const char* displayName) {
			u32 flags = FieldFlagDefault;
			if (field.flags & SLT_FIELD_HIDDEN) {
				flags &= ~FieldFlagEditorVisible;
			}
			if (field.flags & SLT_FIELD_READ_ONLY) {
				flags |= FieldFlagReadOnly;
			}
			if (field.flags & SLT_FIELD_TRANSIENT) {
				flags |= FieldFlagTransient;
			}

			AttributeInfo attributes[3];
			u32 count = 0;
			if (field.flags & SLT_FIELD_RANGED) {
				attributes[count++] = {StringHash("Range"), Variant(AttrRangeData{field.rangeMin, field.rangeMax})};
			}
			if (!View(field.tooltip).empty()) {
				attributes[count++] = {StringHash("Tooltip"), Variant(str(View(field.tooltip)))};
			}
			if (!View(field.header).empty()) {
				attributes[count++] = {StringHash("Header"), Variant(str(View(field.header)))};
			}

			const StringHash name(field.nameHash);
			void* (*const address)(void*) = SlotThunks[static_cast<size_t>(field.offset) / SLOT_BYTES];
			// initializer_list can't be built from a runtime count
			switch (count) {
			case 0:
				builder.AddField(name, displayName, kind.type, kind.size, kind.alignment, nullptr, nullptr, address, flags, {});
				break;
			case 1:
				builder.AddField(name, displayName, kind.type, kind.size, kind.alignment, nullptr, nullptr, address, flags, {attributes[0]});
				break;
			case 2:
				builder.AddField(name, displayName, kind.type, kind.size, kind.alignment, nullptr, nullptr, address, flags,
								 {attributes[0], attributes[1]});
				break;
			default:
				builder.AddField(name, displayName, kind.type, kind.size, kind.alignment, nullptr, nullptr, address, flags,
								 {attributes[0], attributes[1], attributes[2]});
				break;
			}
		}

	} // anonymous namespace

	ManagedBehavior* dirtyHead = nullptr;

	i32 Register(const SltTypeDesc& desc) {
		const std::string_view name = View(desc.name);
		const StringHash hash(name);
		if (name.empty() || hash.GetValue() != desc.nameHash) {
			SLT_LOG_ERROR("C#", "C# type '{}' has name hash {:016x}, engine computes {:016x}. Rebuild the scripts", name, desc.nameHash,
						  hash.GetValue());
			return SLT_ERR_REFUSED;
		}

		if (desc.managedTypeId < 0) {
			SLT_LOG_ERROR("C#", "C# type '{}' has id {}. Rebuild the scripts", name, desc.managedTypeId);
			return SLT_ERR_REFUSED;
		}

		const TypeInfo* parent = Behavior::GetReflectType();
		const ManagedType* managedParent = nullptr;
		if (desc.parentHash != 0) {
			parent = TypeRegistry::FindType(StringHash(desc.parentHash));
			const auto found = parent ? _byInfo.find(parent) : _byInfo.end();
			if (found == _byInfo.end()) {
				SLT_LOG_ERROR("C#", "C# type '{}' has unregistered parent. Rebuild the scripts", name);
				return SLT_ERR_REFUSED;
			}
			managedParent = found->second;
		}

		u32 slot = managedParent ? managedParent->slotEnd : FIRST_SLOT;
		for (i32 index = 0; index < desc.fieldCount; ++index) {
			SltFieldDesc& field = desc.fields[index];
			const FieldKindInfo* kind = FieldKinds::Find(field.kind);
			if (!kind) {
				SLT_LOG_ERROR("C#", "C# type '{}' has a field of unknown kind {}. Rebuild the scripts", name, field.kind);
				return SLT_ERR_REFUSED;
			}
			slot += kind->alignment > SLOT_BYTES ? slot & 1u : 0u;
			field.offset = static_cast<int32_t>(slot * SLOT_BYTES);
			slot += static_cast<u32>((kind->size + SLOT_BYTES - 1) / SLOT_BYTES);
		}
		if (slot > FIRST_SLOT + SLT_FIELD_SLOT_LIMIT) {
			SLT_LOG_ERROR("C#", "C# type '{}' has too many serialized fields ({} slot(s), max {}). Remove some", name, slot - FIRST_SLOT,
						  SLT_FIELD_SLOT_LIMIT);
			return SLT_ERR_REFUSED;
		}

		ManagedType* type = New<ManagedType>(MemTag::Scripting);
		type->name = name;
		type->parent = managedParent;
		type->managedTypeId = desc.managedTypeId;
		type->hookMask = desc.hookMask;
		type->slotEnd = slot;
		type->hasFields = desc.fieldCount > 0 || (managedParent && managedParent->hasFields);

		const size_t dot = type->name.rfind('.');
		ReflectBuilder builder(hash, (static_cast<size_t>(slot) * SLOT_BYTES + 15) / 16 * 16, 16);
		builder.SetParent(parent);
		builder.SetFlags(TypeFlagComponent | ((desc.flags & SLT_TYPE_ABSTRACT) ? TypeFlagAbstract : 0u));
		builder.SetCreateTypedFn(&Create);
		if (type->hasFields) {
			builder.SetFieldSyncFn(&ManagedBehavior::SyncFields);
		}
		builder.SetDisplayName(type->name.c_str() + (dot == str::npos ? 0 : dot + 1));
		builder.AddAttribute(BehaviorTicksAttribute, Variant(static_cast<i64>(desc.tickMask)));
		for (i32 index = 0; index < desc.fieldCount; ++index) {
			const SltFieldDesc& field = desc.fields[index];
			const std::string_view fieldName = View(field.name);
			AddField(builder, field, *FieldKinds::Find(field.kind), fieldName.empty() ? nullptr : type->fieldNames.emplace_back(fieldName).c_str());
		}
		type->info = builder.FinalizeOwned(&_TOKEN);
		if (!type->info) {
			SLT_LOG_ERROR("C#", "C# component '{}' has a name already used by engine. Rename the class (or give it a namespace)", name);
			Delete(type);
			return SLT_ERR_REFUSED;
		}

		_types.push_back(type);
		_byInfo[type->info] = type;
		if (_byIndex.size() <= static_cast<size_t>(desc.managedTypeId)) {
			_byIndex.resize(static_cast<size_t>(desc.managedTypeId) + 1);
		}
		_byIndex[static_cast<size_t>(desc.managedTypeId)] = type;
		if (_observer.registered) {
			_observer.registered(type->info, type->name, &_TOKEN);
		}
		return SLT_OK;
	}

	void ReleaseAll() {
		u32 orphans = 0;
		for (ManagedBehavior* proxy = _live; proxy; proxy = proxy->next) {
			proxy->ReleaseInstance();
			++orphans;
		}
		if (_observer.released) {
			_observer.released(&_TOKEN);
		}
		for (ManagedType* type : _types) {
			if (type->liveCount > 0) {
				// TypeInfo still read by live proxies (never freed)
				TypeRegistry::Unregister(type->info->name);
				_retired.push_back(type);
			}
		}
		TypeRegistry::UnregisterOwnedBy(&_TOKEN);
		for (ManagedType* type : _types) {
			if (type->liveCount == 0) {
				Delete(type);
			}
		}
		_types.clear();
		_byIndex.clear();
		_byInfo.clear();
		if (orphans > 0) {
			SLT_LOG_ERROR("C#", "{} C# component(s) alive at script unload (hooks stopped)", orphans);
		}
	}

	const ManagedType* FindByIndex(i32 scriptIndex) {
		const size_t id = static_cast<size_t>(scriptIndex) - 1;
		return id < _byIndex.size() ? _byIndex[id] : nullptr;
	}

	const void* GetOwnerToken() {
		return &_TOKEN;
	}

	void SetTypeObserver(const TypeObserver& observer) {
		_observer = observer;
	}

	void Link(ManagedBehavior& proxy) {
		proxy.next = _live;
		if (_live) {
			_live->previous = &proxy;
		}
		_live = &proxy;
	}

	void Unlink(ManagedBehavior& proxy) {
		if (proxy.previous) {
			proxy.previous->next = proxy.next;
		} else {
			_live = proxy.next;
		}
		if (proxy.next) {
			proxy.next->previous = proxy.previous;
		}
	}

	void LinkDirty(ManagedBehavior& proxy) {
		proxy.dirtyPrevious = nullptr;
		proxy.dirtyNext = dirtyHead;
		if (dirtyHead) {
			dirtyHead->dirtyPrevious = &proxy;
		}
		dirtyHead = &proxy;
	}

	void UnlinkDirty(ManagedBehavior& proxy) {
		if (proxy.dirtyPrevious) {
			proxy.dirtyPrevious->dirtyNext = proxy.dirtyNext;
		} else {
			dirtyHead = proxy.dirtyNext;
		}
		if (proxy.dirtyNext) {
			proxy.dirtyNext->dirtyPrevious = proxy.dirtyPrevious;
		}
	}

	void PullDirty() {
		while (dirtyHead) {
			dirtyHead->Pull();
		}
	}

	Counters& GetCounters() {
		return _counters;
	}

	i32& GetLifecycleDepth() {
		return _lifecycleDepth;
	}

} // namespace Shalltear::CSharpImpl::ManagedTypes
