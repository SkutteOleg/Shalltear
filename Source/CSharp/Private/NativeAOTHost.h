#pragma once

#include "Shalltear/Core/Types.h"

namespace Shalltear::CSharpImpl::NativeAOTHost {

	inline constexpr const char* ENTRY_SYMBOL = "ShalltearScriptsInitialize";

	// library never unloaded, unsupported by NativeAOT
	bool Start(const str& libraryPath, str& outError);

} // namespace Shalltear::CSharpImpl::NativeAOTHost
