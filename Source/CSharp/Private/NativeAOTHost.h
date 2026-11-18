#pragma once

#include "HostLibrary.h"

#include "Shalltear/Core/Types.h"

namespace Shalltear::CSharpImpl::NativeAOTHost {

	inline constexpr const char* ENTRY_SYMBOL = "ShalltearScriptsInitialize";

	inline str GetPlayerLibraryName() {
		return "GameScripts" + str(GetSharedLibrarySuffix());
	}

	// library never unloaded, unsupported by NativeAOT
	bool Start(const str& libraryPath, str& outError);

	void StartPlayer();

} // namespace Shalltear::CSharpImpl::NativeAOTHost
