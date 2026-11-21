#pragma once

#include "Shalltear/Core/Types.h"

namespace Shalltear::CSharpStaticHost {

	using StaticEntryFn = i32 (*)(const void*, void*);

	void SetEntry(StaticEntryFn entry);

} // namespace Shalltear::CSharpStaticHost
