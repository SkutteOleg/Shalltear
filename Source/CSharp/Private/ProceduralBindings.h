#pragma once

#include "Shalltear/Core/Types.h"

#include <string_view>

namespace Shalltear::CSharpImpl::ProceduralBindings {

	u64 CreateTransient(u64 typeHash, std::string_view name);
	void DestroyTransient(u64 id);

} // namespace Shalltear::CSharpImpl::ProceduralBindings
