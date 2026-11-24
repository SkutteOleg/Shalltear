#include "HostLibrary.h"

namespace Shalltear::CSharpImpl {

	void* OpenHostLibrary(const str&, str& outError) {
		outError = "dynamic script libraries are not supported on Web";
		return nullptr;
	}

	void* FindHostSymbol(void*, const char*) {
		return nullptr;
	}

	std::string_view GetSharedLibrarySuffix() {
		return ".wasm";
	}

} // namespace Shalltear::CSharpImpl
