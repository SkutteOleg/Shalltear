#include "ManagedTypes.h"

#include "FieldKinds.h"
#include "ManagedBehavior.h"

#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Reflection/Primitives.h"
#include "Shalltear/Core/Reflection/Reflect.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Reflection/VariantDispatch.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <initializer_list>
#include <map>
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

		constexpr VariantVTable HandleVTable = {
			[](void* dst, const void* src) { std::memcpy(dst, src, sizeof(u64)); },
			[](void* dst, void* src) { std::memcpy(dst, src, sizeof(u64)); },
			[](void*) {},
			sizeof(u64),
			alignof(u64)};

		struct ManagedEnum {
			TypeInfo info{};
			str displayName;
			std::deque<str> names;
			std::vector<EnumConstantInfo> constants;
		};

		struct ManagedValue {
			const TypeInfo* info = nullptr;
			str name;
			std::deque<str> fieldNames;
			std::vector<ShadowInit> inits;
			u32 size = 0;
			u32 alignment = 0;
		};

		struct HandleType {
			TypeInfo info{};
			bool registered = false;
		};

		struct ListType {
			ContainerInfo container{};
			ElementOps ops;
		};

		struct Element {
			const TypeInfo* type = nullptr;
			u32 size = 0;
			u32 alignment = 0;
			u32 flags = 0;
			bool text = false;
			const std::vector<ShadowInit>* inits = nullptr;
		};

		struct Laid {
			Element element;
			const ContainerInfo* container = nullptr;
		};

		std::vector<ManagedType*> _types;
		std::vector<ManagedType*> _byIndex;
		std::vector<ManagedType*> _retired;
		std::unordered_map<const TypeInfo*, ManagedType*> _byInfo;
		std::unordered_map<u64, ManagedEnum*> _enums;
		std::unordered_map<u64, ManagedValue*> _values;
		std::unordered_map<u64, HandleType*> _handles;
		std::map<std::pair<i32, u64>, ListType*> _lists;
		ManagedBehavior* _live = nullptr;
		TypeObserver _observer;
		Counters _counters;
		i32 _lifecycleDepth = 0;
		bool _restoring = false;

		std::string_view View(SltString text) {
			return text.ptr && text.length > 0 ? std::string_view(text.ptr, static_cast<size_t>(text.length)) : std::string_view{};
		}

		const char* ShortName(const str& fullName) {
			const size_t dot = fullName.rfind('.');
			return fullName.c_str() + (dot == str::npos ? 0 : dot + 1);
		}

		bool NameMatches(std::string_view kind, std::string_view name, u64 nameHash) {
			if (name.empty() && nameHash != 0) {
				return true;
			}
			const StringHash hash(name);
			if (!name.empty() && hash.GetValue() == nameHash) {
				return true;
			}
			SLT_LOG_ERROR("C#", "C# {} '{}' has name hash {:016x}, engine computes {:016x}. Rebuild the scripts", kind, name, nameHash, hash.GetValue());
			return false;
		}

		str LabelOf(std::string_view name, u64 nameHash) {
			return name.empty() ? std::format("{:016x}", nameHash) : str(name);
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

		const TypeInfo* HandleFor(u64 pointeeHash) {
			if (const TypeInfo* engine = FieldKinds::FindEngineHandle(pointeeHash)) {
				return engine;
			}
			HandleType*& slot = _handles[pointeeHash];
			if (!slot) {
				slot = New<HandleType>(MemTag::Scripting);
				TypeInfo pointee{};
				pointee.name = StringHash(pointeeHash);
				slot->info.name = HandlePointeeName(pointee);
				slot->info.size = sizeof(u64);
				slot->info.alignment = alignof(u64);
				slot->info.flags = TypeFlagPrimitive | TypeFlagHandle | TypeFlagTriviallyCopyable;
				slot->registered = TypeRegistry::RegisterOwned(&slot->info, &_TOKEN) != nullptr;
				if (slot->registered) {
					VariantDispatch::Register(slot->info.name, &HandleVTable);
				}
			}
			return &slot->info;
		}

		bool Describe(std::string_view owner, const SltFieldDesc& field, Element& out) {
			if (const FieldKindInfo* plain = FieldKinds::Find(field.kind)) {
				out = {plain->type, plain->size, plain->alignment};
				return true;
			}
			switch (field.kind) {
			case SLT_KIND_STRING: {
				const FieldKindInfo& text = FieldKinds::GetText();
				out = {text.type, text.size, text.alignment};
				out.text = true;
				return true;
			}
			case SLT_KIND_ENUM:
				if (const auto found = _enums.find(field.typeHash); found != _enums.end()) {
					const TypeInfo& type = found->second->info;
					out = {&type, static_cast<u32>(type.size), static_cast<u32>(type.alignment)};
					return true;
				}
				break;
			case SLT_KIND_OBJECT:
				out = {HandleFor(field.typeHash), sizeof(u64), alignof(u64)};
				return true;
			case SLT_KIND_VALUE:
				if (const auto found = _values.find(field.typeHash); found != _values.end()) {
					const ManagedValue& value = *found->second;
					out = {value.info, value.size, value.alignment, FieldFlagSubobject, false, &value.inits};
					return true;
				}
				break;
			default:
				break;
			}
			SLT_LOG_ERROR("C#", "C# type '{}' has a field of unknown kind {} (type {:016x}). Rebuild the scripts", owner, field.kind, field.typeHash);
			return false;
		}

		const ListType& ListFor(const SltFieldDesc& field, const Element& element) {
			ListType*& slot = _lists[{field.kind, field.kind < SLT_KIND_ENUM ? 0 : field.typeHash}];
			if (!slot) {
				slot = New<ListType>(MemTag::Scripting);
				slot->ops.size = element.size;
				slot->ops.alignment = element.alignment;
				slot->ops.text = element.text;
				if (element.inits) {
					slot->ops.inits = *element.inits;
				}
				ShadowStorage::FillContainer(slot->container, element.type, element.size);
			}
			return *slot;
		}

		bool LayOut(std::string_view owner, SltFieldDesc* fields, i32 count, u32& slot, u32& alignment, std::vector<ShadowInit>& inits,
					std::vector<Laid>& laid) {
			for (i32 index = 0; index < count; ++index) {
				SltFieldDesc& field = fields[index];
				Laid entry;
				if (!Describe(owner, field, entry.element)) {
					return false;
				}
				const bool list = (field.flags & SLT_FIELD_LIST) != 0;
				const u32 size = list ? static_cast<u32>(sizeof(ShadowList)) : entry.element.size;
				const u32 fieldAlignment = list ? static_cast<u32>(alignof(ShadowList)) : entry.element.alignment;
				slot += fieldAlignment > SLOT_BYTES ? slot & 1u : 0u;
				const u32 offset = static_cast<u32>(slot * SLOT_BYTES);
				field.offset = static_cast<int32_t>(offset);
				slot += static_cast<u32>((size + SLOT_BYTES - 1) / SLOT_BYTES);
				alignment = std::max(alignment, fieldAlignment);
				if (list) {
					const ListType& type = ListFor(field, entry.element);
					entry.container = &type.container;
					inits.push_back({offset, &type.ops});
				} else {
					if (entry.element.text) {
						inits.push_back({offset, nullptr});
					}
					if (entry.element.inits) {
						for (const ShadowInit& init : *entry.element.inits) {
							inits.push_back({offset + init.offset, init.list});
						}
					}
				}
				laid.push_back(entry);
			}
			return true;
		}

		void AddField(ReflectBuilder& builder, const SltFieldDesc& field, const Laid& laid, const char* displayName) {
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
			const auto add = [&](std::initializer_list<AttributeInfo> list) {
				if (laid.container) {
					builder.AddContainerField(name, displayName, sizeof(ShadowList), alignof(ShadowList), address, flags, laid.container, list);
				} else {
					builder.AddField(name, displayName, laid.element.type, laid.element.size, laid.element.alignment, nullptr, nullptr, address,
									 flags | laid.element.flags, list);
				}
			};
			// initializer_list can't be built from a runtime count
			switch (count) {
			case 0:
				add({});
				break;
			case 1:
				add({attributes[0]});
				break;
			case 2:
				add({attributes[0], attributes[1]});
				break;
			default:
				add({attributes[0], attributes[1], attributes[2]});
				break;
			}
		}

	} // anonymous namespace

	ManagedBehavior* dirtyHead = nullptr;

	i32 Register(const SltTypeDesc& desc) {
		if (!NameMatches("type", View(desc.name), desc.nameHash)) {
			return SLT_ERR_REFUSED;
		}
		const str name = LabelOf(View(desc.name), desc.nameHash);
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

		ManagedType* type = New<ManagedType>(MemTag::Scripting);
		if (managedParent) {
			type->inits = managedParent->inits;
		}
		u32 slot = managedParent ? managedParent->slotEnd : FIRST_SLOT;
		u32 alignment = 16;
		std::vector<Laid> laid;
		if (!LayOut(name, desc.fields, desc.fieldCount, slot, alignment, type->inits, laid)) {
			Delete(type);
			return SLT_ERR_REFUSED;
		}
		if (slot > FIRST_SLOT + SLT_FIELD_SLOT_LIMIT) {
			SLT_LOG_ERROR("C#", "C# type '{}' has too many serialized fields ({} slot(s), max {}). Remove some", name, slot - FIRST_SLOT,
						  SLT_FIELD_SLOT_LIMIT);
			Delete(type);
			return SLT_ERR_REFUSED;
		}

		type->name = name;
		type->script = View(desc.script);
		type->parent = managedParent;
		type->managedTypeId = desc.managedTypeId;
		type->hookMask = desc.hookMask;
		type->listenerMask = desc.listenerMask;
		type->slotEnd = slot;
		type->hasFields = desc.fieldCount > 0 || (managedParent && managedParent->hasFields);
		type->normalizes = (desc.flags & SLT_TYPE_NORMALIZE) != 0 || (managedParent && managedParent->normalizes);

		ReflectBuilder builder(StringHash(desc.nameHash), (static_cast<size_t>(slot) * SLOT_BYTES + 15) / 16 * 16, 16);
		builder.SetParent(parent);
		builder.SetFlags(TypeFlagComponent | ((desc.flags & SLT_TYPE_ABSTRACT) ? TypeFlagAbstract : 0u));
		builder.SetCreateTypedFn(&Create);
		if (type->hasFields) {
			builder.SetFieldSyncFn(&ManagedBehavior::SyncFields);
		}
		builder.SetDisplayName(ShortName(type->name));
		builder.AddAttribute(BehaviorTicksAttribute, Variant(static_cast<i64>(desc.tickMask)));
		for (i32 index = 0; index < desc.fieldCount; ++index) {
			const SltFieldDesc& field = desc.fields[index];
			const std::string_view fieldName = View(field.name);
			AddField(builder, field, laid[static_cast<size_t>(index)], fieldName.empty() ? nullptr : type->fieldNames.emplace_back(fieldName).c_str());
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

	i32 RegisterEnum(const SltEnumDesc& desc) {
		if (!NameMatches("enum", View(desc.name), desc.nameHash)) {
			return SLT_ERR_REFUSED;
		}
		const str name = LabelOf(View(desc.name), desc.nameHash);
		if (desc.size != 1 && desc.size != 2 && desc.size != 4 && desc.size != 8) {
			SLT_LOG_ERROR("C#", "C# enum '{}' has size {}. Rebuild the scripts", name, desc.size);
			return SLT_ERR_REFUSED;
		}

		ManagedEnum* made = New<ManagedEnum>(MemTag::Scripting);
		made->displayName = name;
		made->constants.reserve(static_cast<size_t>(std::max(desc.constantCount, 0)));
		for (i32 index = 0; index < desc.constantCount; ++index) {
			const SltEnumConstant& constant = desc.constants[index];
			if (!NameMatches("enum constant", View(constant.name), constant.nameHash)) {
				Delete(made);
				return SLT_ERR_REFUSED;
			}
			const str& text = made->names.emplace_back(View(constant.name));
			made->constants.push_back({StringHash(constant.nameHash), text.empty() ? nullptr : text.c_str(), constant.value});
		}
		made->info.name = StringHash(desc.nameHash);
		made->info.size = static_cast<size_t>(desc.size);
		made->info.alignment = static_cast<size_t>(desc.size);
		made->info.flags = TypeFlagPrimitive | TypeFlagEnum | TypeFlagTriviallyCopyable;
		made->info.displayName = ShortName(made->displayName);
		made->info.enumConstants = made->constants.data();
		made->info.enumConstantCount = static_cast<u32>(made->constants.size());
		if (!TypeRegistry::RegisterOwned(&made->info, &_TOKEN)) {
			SLT_LOG_ERROR("C#", "C# enum '{}' has a name already used by engine. Rename the enum (or give it a namespace)", name);
			Delete(made);
			return SLT_ERR_REFUSED;
		}
		_enums[desc.nameHash] = made;
		return SLT_OK;
	}

	i32 RegisterValue(SltValueDesc& desc) {
		if (!NameMatches("value", View(desc.name), desc.nameHash)) {
			return SLT_ERR_REFUSED;
		}
		const str name = LabelOf(View(desc.name), desc.nameHash);

		ManagedValue* made = New<ManagedValue>(MemTag::Scripting);
		made->name = name;
		u32 slot = 0;
		u32 alignment = SLOT_BYTES;
		std::vector<Laid> laid;
		if (!LayOut(name, desc.fields, desc.fieldCount, slot, alignment, made->inits, laid)) {
			Delete(made);
			return SLT_ERR_REFUSED;
		}
		if (slot == 0 || slot > SLT_FIELD_SLOT_LIMIT) {
			SLT_LOG_ERROR("C#", "C# value '{}' has {} slot(s) of serialized fields (1-{} allowed)", name, slot, SLT_FIELD_SLOT_LIMIT);
			Delete(made);
			return SLT_ERR_REFUSED;
		}
		made->alignment = alignment;
		made->size = static_cast<u32>((slot * SLOT_BYTES + alignment - 1) / alignment * alignment);

		ReflectBuilder builder(StringHash(desc.nameHash), made->size, made->alignment);
		builder.SetDisplayName(ShortName(made->name));
		for (i32 index = 0; index < desc.fieldCount; ++index) {
			const SltFieldDesc& field = desc.fields[index];
			const std::string_view fieldName = View(field.name);
			AddField(builder, field, laid[static_cast<size_t>(index)], fieldName.empty() ? nullptr : made->fieldNames.emplace_back(fieldName).c_str());
		}
		made->info = builder.FinalizeOwned(&_TOKEN);
		if (!made->info) {
			SLT_LOG_ERROR("C#", "C# type '{}' has a name already used by engine. Rename it (or give it a namespace)", name);
			Delete(made);
			return SLT_ERR_REFUSED;
		}
		desc.size = static_cast<int32_t>(made->size);
		_values[desc.nameHash] = made;
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
		if (orphans > 0) {
			// still read as field types of kept TypeInfos
			for (const auto& [hash, value] : _values) {
				TypeRegistry::Unregister(value->info->name);
			}
		}
		TypeRegistry::UnregisterOwnedBy(&_TOKEN);
		for (ManagedType* type : _types) {
			if (type->liveCount == 0) {
				Delete(type);
			}
		}
		for (const auto& [hash, handle] : _handles) {
			if (handle->registered) {
				VariantDispatch::Unregister(handle->info.name);
			}
		}
		if (orphans == 0) {
			for (const auto& [hash, made] : _enums) {
				Delete(made);
			}
			for (const auto& [hash, made] : _values) {
				Delete(made);
			}
			for (const auto& [hash, made] : _handles) {
				Delete(made);
			}
			for (const auto& [key, made] : _lists) {
				Delete(made);
			}
		}
		_types.clear();
		_byIndex.clear();
		_byInfo.clear();
		_enums.clear();
		_values.clear();
		_handles.clear();
		_lists.clear();
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

	void ForEachType(void (*visit)(const ManagedType& type, void* user), void* user) {
		for (const ManagedType* type : _types) {
			visit(*type, user);
		}
	}

	u32 GetLiveCount() {
		u32 count = 0;
		for (const ManagedBehavior* proxy = _live; proxy; proxy = proxy->next) {
			++count;
		}
		return count;
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

	void SetRestoring(bool restoring) {
		_restoring = restoring;
	}

	bool IsRestoring() {
		return _restoring;
	}

	Counters& GetCounters() {
		return _counters;
	}

	i32& GetLifecycleDepth() {
		return _lifecycleDepth;
	}

} // namespace Shalltear::CSharpImpl::ManagedTypes
