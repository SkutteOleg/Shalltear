#include "Shalltear/Core/Reflection/TypeRegistry.h"

#include "Shalltear/Core/Assert.h"
#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Module/ScriptFunctions.h"
#include "Shalltear/Core/Object.h"
#include "Shalltear/Core/Reflection/Walk.h"

#include "Module/EngineStateTable.h"
#include "Platform/Library.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <shared_mutex>

namespace Shalltear {

	namespace {

		constexpr u16 INITIAL_CLASS_CAPACITY = 256;

		constexpr u16 INVALID_CLASS_ID = 0xFFFF;

		constexpr size_t INITIAL_OWNER_TOKEN_CAPACITY = 64;

		struct IsDerivedMatrix {
			u8* data = nullptr;
			u16 dimension = 0;

			void Initialize(u16 initialDim) {
				Resize(initialDim);
			}

			void Destroy() {
				if (data) {
					Shalltear::Free(data);
					data = nullptr;
					dimension = 0;
				}
			}

			void GrowIfNeeded(u16 newDim) {
				if (newDim <= dimension)
					return;

				u16 target = 1;
				while (target < newDim)
					target = static_cast<u16>(target << 1);
				if (target < newDim)
					target = newDim;
				Resize(target);
			}

			void Resize(u16 newDim) {
				size_t newBytes = (static_cast<size_t>(newDim) * newDim + 7) / 8;
				u8* newData = static_cast<u8*>(Shalltear::Allocate(newBytes, MemTag::Core, 16));
				std::memset(newData, 0, newBytes);

				if (data) {
					for (u16 c = 0; c < dimension; ++c) {
						for (u16 p = 0; p < dimension; ++p) {
							if (GetBit(c, p)) {
								size_t newIdx = static_cast<size_t>(c) * newDim + p;
								newData[newIdx >> 3] |= static_cast<u8>(1u << (newIdx & 7));
							}
						}
					}
					Shalltear::Free(data);
				}

				data = newData;
				dimension = newDim;
			}

			bool GetBit(u16 child, u16 parent) const {
				if (child >= dimension || parent >= dimension)
					return false;
				size_t idx = static_cast<size_t>(child) * dimension + parent;
				return (data[idx >> 3] & static_cast<u8>(1u << (idx & 7))) != 0;
			}

			void SetBit(u16 child, u16 parent) {
				size_t idx = static_cast<size_t>(child) * dimension + parent;
				data[idx >> 3] |= static_cast<u8>(1u << (idx & 7));
			}

			void ClearRow(u16 classId) {
				if (classId >= dimension)
					return;
				for (u16 p = 0; p < dimension; ++p) {
					size_t idx = static_cast<size_t>(classId) * dimension + p;
					data[idx >> 3] &= static_cast<u8>(~(1u << (idx & 7)));
				}
			}

			void ClearColumn(u16 classId) {
				if (classId >= dimension)
					return;
				for (u16 c = 0; c < dimension; ++c) {
					size_t idx = static_cast<size_t>(c) * dimension + classId;
					data[idx >> 3] &= static_cast<u8>(~(1u << (idx & 7)));
				}
			}
		};

		struct ScriptFunctionEntry {
			str name;
			str signature;
			ScriptFunctions::Thunk thunk = nullptr;
			u64 context = 0;
			const void* owner = nullptr;
		};

		struct RegistryData {
			HashMap<StringHash, TypeInfo*> byName{GetMemoryResource(MemTag::Core)};

			HashMap<StringHash, TypeInfo*> byFormerName{GetMemoryResource(MemTag::Core)};

			HashMap<StringHash, ScriptFunctionEntry> functions{GetMemoryResource(MemTag::Core)};

			Vector<TypeInfo*> byClassId{GetMemoryResource(MemTag::Core)};

			Vector<u16> freeClassIds{GetMemoryResource(MemTag::Core)};

			Vector<const void*> ownerTokens{GetMemoryResource(MemTag::Core)};

			IsDerivedMatrix matrix;

			u16 nextClassId = 0;

			std::atomic<u32> epoch{0};

			std::atomic<u32> functionEpoch{0};

			mutable std::shared_mutex mutex;

			bool primitivesInitialized = false;

			RegistryData() {
				byClassId.reserve(INITIAL_CLASS_CAPACITY);
				ownerTokens.reserve(INITIAL_OWNER_TOKEN_CAPACITY);
				matrix.Initialize(INITIAL_CLASS_CAPACITY);
			}

			~RegistryData() {
				matrix.Destroy();
			}
		};

		RegistryData* _boundData = nullptr;

		RegistryData& GetData() noexcept {
			if (_boundData)
				return *_boundData;
			static RegistryData data;
			return data;
		}

	} // anonymous namespace

	const TypeInfo* TypeRegistry::Register(TypeInfo* typeInfo, const void* ownerHint) {
		SLT_CORE_ASSERT_MSG(typeInfo != nullptr, "TypeRegistry::Register: null TypeInfo");
		if (!typeInfo)
			return nullptr;
		return RegisterAs(typeInfo, ownerHint ? ownerHint : __builtin_return_address(0), false);
	}

	const TypeInfo* TypeRegistry::RegisterOwned(TypeInfo* typeInfo, const void* ownerToken) {
		SLT_CORE_ASSERT_MSG(typeInfo != nullptr && ownerToken != nullptr, "TypeRegistry::RegisterOwned: null argument");
		if (!typeInfo || !ownerToken)
			return nullptr;
		return RegisterAs(typeInfo, ownerToken, true);
	}

	bool TypeRegistry::IsOwnerToken(const void* owner) {
		auto& data = GetData();
		std::shared_lock lock(data.mutex);
		return std::find(data.ownerTokens.begin(), data.ownerTokens.end(), owner) != data.ownerTokens.end();
	}

	const TypeInfo* TypeRegistry::RegisterAs(TypeInfo* typeInfo, const void* owner, bool ownerIsToken) {
		auto& data = GetData();
		std::unique_lock lock(data.mutex);

		auto existing = data.byName.find(typeInfo->name);
		if (existing != data.byName.end()) {
			return ownerIsToken ? nullptr : existing->second;
		}

		typeInfo->ownerModule = ownerIsToken ? owner : Platform::ImageBaseOf(owner);
		if (ownerIsToken && std::find(data.ownerTokens.begin(), data.ownerTokens.end(), owner) == data.ownerTokens.end()) {
			data.ownerTokens.push_back(owner);
		}

		typeInfo->moduleEpoch = data.epoch.load(std::memory_order_relaxed);

		u16 classId = INVALID_CLASS_ID;
		if (!data.freeClassIds.empty()) {
			classId = data.freeClassIds.back();
			data.freeClassIds.pop_back();
		} else {
			SLT_CORE_ASSERT_MSG(data.nextClassId < INVALID_CLASS_ID,
								"TypeRegistry: exceeded u16 ClassId capacity (65,535 types)");
			classId = data.nextClassId++;
		}

		if (classId >= data.matrix.dimension) {
			data.matrix.GrowIfNeeded(static_cast<u16>(classId + 1));
		}
		if (classId >= data.byClassId.size()) {
			data.byClassId.resize(classId + 1, nullptr);
		}

		typeInfo->classId = classId;
		data.byClassId[classId] = typeInfo;
		data.byName[typeInfo->name] = typeInfo;
		data.byFormerName.erase(typeInfo->name);

		if (typeInfo->attributes && typeInfo->attributeCount > 0) {
			const StringHash formerAttrKey("FormerlySerializedAs");
			for (u32 i = 0; i < typeInfo->attributeCount; ++i) {
				const AttributeInfo& attr = typeInfo->attributes[i];
				if (attr.key == formerAttrKey && attr.value.Is<str>()) {
					const str& oldName = attr.value.As<str>();
					if (!oldName.empty()) {
						const StringHash formerHash(oldName);
						if (formerHash != StringHash{} && formerHash != typeInfo->name && !data.byName.contains(formerHash)) {
							data.byFormerName[formerHash] = typeInfo;
						}
					}
				}
			}
		}

		const TypeInfo* ancestor = typeInfo;
		while (ancestor) {
			if (ancestor->classId < data.matrix.dimension) {
				data.matrix.SetBit(classId, ancestor->classId);
			}
			ancestor = ancestor->parent;
		}

		return typeInfo;
	}

	namespace {

		void RemoveEntryLocked(RegistryData& data, TypeInfo* info) {
			u16 classId = info->classId;

			data.matrix.ClearRow(classId);
			data.matrix.ClearColumn(classId);

			if (classId < data.byClassId.size())
				data.byClassId[classId] = nullptr;

			for (auto formerIt = data.byFormerName.begin(); formerIt != data.byFormerName.end();) {
				if (formerIt->second == info) {
					formerIt = data.byFormerName.erase(formerIt);
				} else {
					++formerIt;
				}
			}

			data.freeClassIds.push_back(classId);
		}

		void FreeHeapMetadata(TypeInfo* info) {
			AttributeInfo* attrPool = nullptr;
			size_t poolCount = 0;
			for (u32 i = 0; i < info->fieldCount; ++i) {
				if (info->fields[i].attrCount > 0) {
					if (!attrPool) {
						attrPool = const_cast<AttributeInfo*>(info->fields[i].attributes);
					}
					poolCount += info->fields[i].attrCount;
				}
			}
			if (attrPool) {
				// destructors run before the dispatch sweep
				for (size_t i = 0; i < poolCount; ++i) {
					attrPool[i].~AttributeInfo();
				}
				Shalltear::Free(attrPool);
			}
			if (info->fields) {
				Shalltear::Free(const_cast<FieldInfo*>(info->fields));
			}
			if (info->methods) {
				Shalltear::Free(const_cast<MethodInfo*>(info->methods));
			}
			if (info->attributes) {
				for (u32 i = 0; i < info->attributeCount; ++i) {
					const_cast<AttributeInfo*>(&info->attributes[i])->~AttributeInfo();
				}
				Shalltear::Free(const_cast<AttributeInfo*>(info->attributes));
			}
			Delete(info);
		}

	} // anonymous namespace

	void TypeRegistry::Unregister(StringHash name) {
		auto& data = GetData();
		std::unique_lock lock(data.mutex);

		auto it = data.byName.find(name);
		if (it == data.byName.end())
			return;

		RemoveEntryLocked(data, it->second);
		data.byName.erase(it);
	}

	u32 TypeRegistry::UnregisterOwnedBy(const void* imageBase) {
		if (!imageBase)
			return 0;

		auto& data = GetData();

		Vector<TypeInfo*> swept{GetMemoryResource(MemTag::Core)};
		{
			std::unique_lock lock(data.mutex);

			data.epoch.fetch_add(1, std::memory_order_relaxed);
			if (std::erase_if(data.functions, [imageBase](const auto& entry) { return entry.second.owner == imageBase; }) > 0) {
				data.functionEpoch.fetch_add(1, std::memory_order_relaxed);
			}
			for (auto it = data.byName.begin(); it != data.byName.end();) {
				TypeInfo* info = it->second;
				if (info->ownerModule == imageBase) {
					RemoveEntryLocked(data, info);
					it = data.byName.erase(it);
					swept.push_back(info);
				} else {
					++it;
				}
			}
		}

		for (TypeInfo* info : swept) {
			SLT_LOG_TRACE("Reflection", "module sweep unregistered type {:#018x}", info->name.GetValue());
			if (info->flags & TypeFlagHeapMetadata) {
				FreeHeapMetadata(info);
			}
		}
		return static_cast<u32>(swept.size());
	}

	const TypeInfo* TypeRegistry::FindType(StringHash name) {
		auto& data = GetData();
		std::shared_lock lock(data.mutex);

		auto it = data.byName.find(name);
		if (it != data.byName.end()) {
			return it->second;
		}
		auto formerIt = data.byFormerName.find(name);
		return (formerIt != data.byFormerName.end()) ? formerIt->second : nullptr;
	}

	bool ScriptFunctions::Register(std::string_view name, std::string_view signature, Thunk thunk, u64 context, const void* ownerToken) {
		if (name.empty() || !thunk) {
			return false;
		}
		auto& data = GetData();
		const void* owner = ownerToken ? ownerToken : Platform::ImageBaseOf(reinterpret_cast<const void*>(thunk));
		std::unique_lock lock(data.mutex);
		const auto found = data.functions.find(StringHash(name));
		if (found != data.functions.end()) {
			const ScriptFunctionEntry& held = found->second;
			return held.thunk == thunk && held.context == context && held.name == name && held.signature == signature;
		}
		data.functions.emplace(StringHash(name), ScriptFunctionEntry{str(name), str(signature), thunk, context, owner});
		data.functionEpoch.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	ScriptFunctions::Found ScriptFunctions::FindRaw(std::string_view name, str* outSignature) {
		auto& data = GetData();
		std::shared_lock lock(data.mutex);
		const auto found = data.functions.find(StringHash(name));
		if (found == data.functions.end() || found->second.name != name) {
			if (outSignature) {
				outSignature->clear();
			}
			return {};
		}
		if (outSignature) {
			*outSignature = found->second.signature;
		}
		return {found->second.thunk, found->second.context};
	}

	u32 ScriptFunctions::GetEpoch() noexcept {
		return GetData().functionEpoch.load(std::memory_order_relaxed);
	}

	void ScriptFunctionDetail::ReportThrow(const char* what) noexcept {
		try {
			SLT_LOG_ERROR("Script", "exported function threw: {}", what ? what : "");
		} catch (...) {
		}
	}

	void ScriptFunctionDetail::ReportTaken(std::string_view name) noexcept {
		try {
			SLT_LOG_WARN("Script", "SLT_SCRIPT_EXPORT: '{}' is exported twice (2nd ignored). Rename one", name);
		} catch (...) {
		}
	}

	std::string_view ScriptFunctionDetail::KeepText(str&& text) {
		// glibc won't unmap a lib with live thread_local destructors
		[[clang::no_destroy]] thread_local str kept;
		kept = std::move(text);
		return kept;
	}

	const FieldInfo* FieldRef::Resolve() {
		auto& data = GetData();

		// read the epoch before resolving and never after
		const u32 current = data.epoch.load(std::memory_order_relaxed);

		if (cached && epoch == current) {
			return cached;
		}

		const TypeInfo* type = TypeRegistry::FindType(ownerType);
		cached = type ? FindField(type, field) : nullptr;
		epoch = current;
		return cached;
	}

	bool TypeRegistry::IsDerivedFrom(const TypeInfo* child, const TypeInfo* parent) {
		if (!child || !parent)
			return false;

		auto& data = GetData();
		std::shared_lock lock(data.mutex);

		return data.matrix.GetBit(child->classId, parent->classId);
	}

	u32 TypeRegistry::GetTypeCount() {
		auto& data = GetData();
		std::shared_lock lock(data.mutex);
		return static_cast<u32>(data.byName.size());
	}

	void TypeRegistry::ForEachType(void (*visitor)(const TypeInfo*, void*), void* user) {
		if (!visitor)
			return;

		auto& data = GetData();
		Vector<const TypeInfo*> snapshot{GetMemoryResource(MemTag::Core)};
		{
			std::shared_lock lock(data.mutex);
			snapshot.reserve(data.byName.size());
			for (const auto& [name, info] : data.byName) {
				snapshot.push_back(info);
			}
		}
		for (const TypeInfo* info : snapshot) {
			visitor(info, user);
		}
	}

	Object* TypeRegistry::CreateObjectInstance(const TypeInfo* type, MemTag tag) {
		if (!type || (type->flags & TypeFlagAbstract) || !type->createFn) {
			return nullptr;
		}
		if (type->flags & TypeFlagTypedCreate) {
			return reinterpret_cast<Object* (*)(const TypeInfo*, MemTag)>(type->createFn)(type, tag);
		}
		return type->createFn(tag);
	}

	void TypeRegistry::DestroyObjectInstance(Object* object) {
		Delete(object);
	}

	namespace {
		TypeRegistrarNode* _pendingHead = nullptr;
		TypeRegistrarNode* _pendingTail = nullptr;
	} // anonymous namespace

	TypeRegistrarNode::TypeRegistrarNode(const TypeInfo* (*fn)()) : registerFn(fn), next(nullptr) {
		TypeRegistry::AddPendingRegistration(this);
	}

	void TypeRegistry::AddPendingRegistration(TypeRegistrarNode* node) {
		if (!node) {
			return;
		}

		for (TypeRegistrarNode* existing = _pendingHead; existing; existing = existing->next) {
			if (existing == node) {
				return;
			}
		}

		node->next = nullptr;
		if (_pendingTail) {
			_pendingTail->next = node;
		} else {
			_pendingHead = node;
		}
		_pendingTail = node;
	}

	void TypeRegistry::RegisterPendingTypes() {
		for (TypeRegistrarNode* node = _pendingHead; node; node = node->next) {
			if (node->registerFn) {
				node->registerFn();
			}
		}
	}

	u32 TypeRegistry::UnlinkPendingRegistrationsIn(const void* imageBase) {
		if (!imageBase)
			return 0;

		const Platform::ImageSpan image = Platform::GetImageSpan(imageBase);
		u32 removed = 0;
		TypeRegistrarNode** link = &_pendingHead;
		TypeRegistrarNode* previous = nullptr;
		while (*link) {
			if (image.Contains(*link)) {
				*link = (*link)->next;
				++removed;
			} else {
				previous = *link;
				link = &(*link)->next;
			}
		}
		_pendingTail = previous;
		return removed;
	}

	extern void RegisterEnginePrimitives();

	void TypeRegistry::InitializePrimitives() {
		auto& data = GetData();
		bool firstCall = false;
		{
			std::unique_lock lock(data.mutex);
			if (!data.primitivesInitialized) {
				data.primitivesInitialized = true;
				firstCall = true;
			}
		}
		if (firstCall) {
			RegisterEnginePrimitives();
		}

		RegisterPendingTypes();
	}

	namespace ModuleInternal {

		void CaptureTypeRegistryEngineState(EngineStateTable& table) {
			table.typeRegistry = &GetData();
		}

		void BindTypeRegistryEngineState(const EngineStateTable& table) {
			_boundData = static_cast<RegistryData*>(table.typeRegistry);
		}

	} // namespace ModuleInternal

} // namespace Shalltear
