#pragma once

#include "ScriptAPI.h"

#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Object.h"
#include "Shalltear/Core/Reflection/Reflect.h"
#include "Shalltear/Core/Reflection/TypeInfo.h"
#include "Shalltear/Core/Types.h"

#include <functional>
#include <string_view>
#include <utility>

namespace Shalltear::CSharpImpl::DerivedTypes {

	using Wire = std::function<void(void*)>;
	using Describe = std::function<void(ReflectBuilder*)>;

	SHALLTEAR_CSHARP_API const void* GetOwnerToken();

	SHALLTEAR_CSHARP_API const TypeInfo* Register(
		std::string_view name,
		const TypeInfo* parent,
		size_t size,
		size_t alignment,
		Object* (*create)(const TypeInfo*, MemTag),
		Wire wire,
		const Describe& describe);

	SHALLTEAR_CSHARP_API void Created(const TypeInfo* type, void* proxy);
	SHALLTEAR_CSHARP_API void Destroyed(const TypeInfo* type, void* proxy);

	bool HasLiveObjects();
	void Unload();

	template <typename TProxy>
	Object* Create(const TypeInfo* type, MemTag tag) {
		TProxy* const made = New<TProxy>(tag);
		made->derivedType = type;
		Created(type, made);
		return made;
	}

	template <typename TProxy, typename TBase>
	const TypeInfo* Register(std::string_view name, std::function<void(TProxy*)> wire, const Describe& describe) {
		return Register(
			name,
			TBase::GetReflectType(),
			sizeof(TProxy),
			alignof(TProxy),
			&Create<TProxy>,
			[wire = std::move(wire)](void* proxy) {
				if (wire) {
					wire(static_cast<TProxy*>(proxy));
				}
			},
			describe);
	}

} // namespace Shalltear::CSharpImpl::DerivedTypes
