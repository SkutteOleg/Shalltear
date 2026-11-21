#pragma once

#include "Shalltear/Core/Types.h"

#include <string_view>

namespace Shalltear::CSharpImpl::ModuleThunkBindings {

	u64 GetTable(std::string_view moduleName) noexcept;
	u32 GetCount(std::string_view moduleName) noexcept;
	u64 GetEpoch() noexcept;
	u64 GetEpochAddress() noexcept;

} // namespace Shalltear::CSharpImpl::ModuleThunkBindings
