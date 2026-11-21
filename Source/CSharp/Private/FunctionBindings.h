#pragma once

#include "Shalltear/Core/Types.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Shalltear::CSharpImpl::FunctionBindings {

	// out[0] = thunk, out[1] = context
	void Find(std::string_view name, std::string_view signature, std::span<int64_t> out);

	void Export(std::string_view name, std::string_view signature, u64 id);

	void Poll();

	void Reset();

} // namespace Shalltear::CSharpImpl::FunctionBindings
