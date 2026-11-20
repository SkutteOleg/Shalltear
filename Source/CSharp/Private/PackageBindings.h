#pragma once

#include "ScriptAPI.h"

#include "Shalltear/Core/Scene/Component.h"
#include "Shalltear/Core/Types.h"

#include <cstdint>
#include <string_view>

namespace Shalltear::CSharpImpl::PackageBindings {

	// table must outlive its registration
	SHALLTEAR_CSHARP_API void Add(std::string_view name, const void* table, i32 size, u64 layoutHash);
	SHALLTEAR_CSHARP_API void Remove(std::string_view name);

	// refusals thrown as std::runtime_error
	const void* Find(std::string_view name, u64 layoutHash, i32 size);

	u32 GetCount();
	std::string_view GetName(u32 index);

	SHALLTEAR_CSHARP_API i32 ReserveListenerSlot(std::string_view name);

	// GC handle owned by proxy
	SHALLTEAR_CSHARP_API intptr_t FindListener(Component& component, i32 slot) noexcept;

} // namespace Shalltear::CSharpImpl::PackageBindings
