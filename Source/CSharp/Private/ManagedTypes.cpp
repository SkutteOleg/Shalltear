#include "ManagedTypes.h"

#include "ManagedBehavior.h"

#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Reflection/Reflect.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"

#include <new>
#include <unordered_map>
#include <vector>

namespace Shalltear::CSharpImpl::ManagedTypes {

	namespace {

		// owner of C# types (image base would also sweep package's C++ types)
		const char _TOKEN = 0;

		std::vector<ManagedType*> _types;
		std::vector<ManagedType*> _retired;
		std::unordered_map<const TypeInfo*, ManagedType*> _byInfo;
		ManagedBehavior* _live = nullptr;
		TypeObserver _observer;
		u64 _hookCrossings = 0;

		std::string_view View(SltString text) {
			return text.ptr && text.length > 0 ? std::string_view(text.ptr, static_cast<size_t>(text.length)) : std::string_view{};
		}

		Object* Create(const TypeInfo* info, MemTag tag) {
			const auto found = _byInfo.find(info);
			if (found == _byInfo.end()) {
				return nullptr;
			}
			void* memory = Allocate(info->size, tag, info->alignment);
			return new (memory) ManagedBehavior(*found->second);
		}

	} // anonymous namespace

	i32 Register(const SltTypeDesc& desc) {
		const std::string_view name = View(desc.name);
		const StringHash hash(name);
		if (name.empty() || hash.GetValue() != desc.nameHash) {
			SLT_LOG_ERROR("C#", "C# type '{}' has name hash {:016x}, engine computes {:016x}. Rebuild the scripts", name, desc.nameHash,
						  hash.GetValue());
			return SLT_ERR_REFUSED;
		}

		const TypeInfo* parent = Behavior::GetReflectType();
		if (desc.parentHash != 0) {
			parent = TypeRegistry::FindType(StringHash(desc.parentHash));
			if (!parent || parent->ownerModule != &_TOKEN) {
				SLT_LOG_ERROR("C#", "C# type '{}' has unregistered parent. Rebuild the scripts", name);
				return SLT_ERR_REFUSED;
			}
		}

		ManagedType* type = New<ManagedType>(MemTag::Scripting);
		type->name = name;
		type->managedTypeId = desc.managedTypeId;
		type->hookMask = desc.hookMask;

		const size_t dot = type->name.rfind('.');
		ReflectBuilder builder(hash, sizeof(ManagedBehavior), alignof(ManagedBehavior));
		builder.SetParent(parent);
		builder.SetFlags(TypeFlagComponent | ((desc.flags & SLT_TYPE_ABSTRACT) ? TypeFlagAbstract : 0u));
		builder.SetCreateTypedFn(&Create);
		builder.SetDisplayName(type->name.c_str() + (dot == str::npos ? 0 : dot + 1));
		builder.AddAttribute(BehaviorTicksAttribute, Variant(static_cast<i64>(desc.tickMask)));
		type->info = builder.FinalizeOwned(&_TOKEN);
		if (!type->info) {
			SLT_LOG_ERROR("C#", "C# component '{}' has a name already used by engine. Rename the class (or give it a namespace)", name);
			Delete(type);
			return SLT_ERR_REFUSED;
		}

		_types.push_back(type);
		_byInfo[type->info] = type;
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
		_byInfo.clear();
		if (orphans > 0) {
			SLT_LOG_ERROR("C#", "{} C# component(s) alive at script unload (hooks stopped)", orphans);
		}
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

	u64 GetHookCrossings() {
		return _hookCrossings;
	}

	void CountHookCrossing() {
		++_hookCrossings;
	}

} // namespace Shalltear::CSharpImpl::ManagedTypes
