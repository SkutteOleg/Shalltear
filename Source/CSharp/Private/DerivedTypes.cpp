#include "DerivedTypes.h"

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/StringHash.h"

#include <algorithm>

namespace Shalltear::CSharpImpl::DerivedTypes {

	namespace {

		struct Derived {
			str displayName;
			const TypeInfo* info = nullptr;
			Wire wire;
			Vector<void*> live;
		};

		const char _TOKEN = 0;
		Vector<Derived*> _types;
		Vector<Derived*> _retired;

		Derived* Find(const Vector<Derived*>& types, const TypeInfo* info) {
			const auto found = std::ranges::find_if(types, [info](const Derived* type) { return type->info == info; });
			return found != types.end() ? *found : nullptr;
		}

	} // anonymous namespace

	const void* GetOwnerToken() {
		return &_TOKEN;
	}

	const TypeInfo* Register(
		std::string_view name,
		const TypeInfo* parent,
		size_t size,
		size_t alignment,
		Object* (*create)(const TypeInfo*, MemTag),
		Wire wire,
		const Describe& describe) {
		if (name.empty()) {
			SLT_LOG_ERROR("C#", "C# type derived from '{}' has no name. Pass a name to Register", parent->displayName ? parent->displayName : "");
			return nullptr;
		}
		Derived* const made = New<Derived>(MemTag::Scripting);
		made->displayName = name.substr(name.find_last_of('.') + 1);
		made->wire = std::move(wire);
		ReflectBuilder builder(StringHash(name), size, alignment);
		builder.SetParent(parent);
		builder.SetFlags(parent->flags & (TypeFlagComponent | TypeFlagAsset));
		builder.SetCreateTypedFn(create);
		builder.SetDisplayName(made->displayName.c_str());
		if (describe) {
			describe(&builder);
		}
		made->info = builder.FinalizeOwned(&_TOKEN);
		if (!made->info) {
			SLT_LOG_ERROR("C#", "C# type '{}' has a name already used by engine. Rename the type (or give it a namespace)", name);
			Delete(made);
			return nullptr;
		}
		_types.push_back(made);
		return made->info;
	}

	void Created(const TypeInfo* type, void* proxy) {
		if (Derived* const found = Find(_types, type)) {
			found->live.push_back(proxy);
			if (found->wire) {
				found->wire(proxy);
			}
		}
	}

	void Destroyed(const TypeInfo* type, void* proxy) {
		if (!type) {
			return;
		}
		Derived* found = Find(_types, type);
		if (!found) {
			found = Find(_retired, type);
		}
		if (found) {
			std::erase(found->live, proxy);
		}
	}

	bool HasLiveObjects() {
		return std::ranges::any_of(_types, [](const Derived* type) { return !type->live.empty(); });
	}

	void Unload() {
		for (Derived* type : _types) {
			type->wire = nullptr;
			if (!type->live.empty()) {
				// TypeInfo still read by live objects (never freed)
				TypeRegistry::Unregister(type->info->name);
				_retired.push_back(type);
			}
		}
		TypeRegistry::UnregisterOwnedBy(&_TOKEN);
		for (Derived* type : _types) {
			if (type->live.empty()) {
				Delete(type);
			}
		}
		_types.clear();
	}

} // namespace Shalltear::CSharpImpl::DerivedTypes
