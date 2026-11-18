#ifndef SLT_WITH_EDITOR

#include "NativeAOTHost.h"

#include "HostLibrary.h"
#include "ScriptRuntime.h"

#include <format>

namespace Shalltear::CSharpImpl::NativeAOTHost {

	bool Start(const str& libraryPath, str& outError) {
		str reason;
		void* library = OpenHostLibrary(libraryPath, reason);
		if (!library) {
			outError = std::format("{} didn't load ({})", libraryPath, reason);
			return false;
		}
		const auto initialize = reinterpret_cast<SltInitializeFn>(FindHostSymbol(library, ENTRY_SYMBOL));
		if (!initialize) {
			outError = std::format("{} doesn't export {}", libraryPath, ENTRY_SYMBOL);
			return false;
		}
		return ScriptRuntime::Attach(initialize, outError);
	}

} // namespace Shalltear::CSharpImpl::NativeAOTHost

#endif
