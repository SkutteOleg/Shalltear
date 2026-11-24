#include "Platform/Library.h"

#include "Shalltear/Core/Module/ModuleAPI.h"

#include <cstring>

extern "C" void ShalltearRegisterCoreExtensions(void* image) __attribute__((weak));

namespace Shalltear::Platform {

	namespace {

		u8 _executableImageMarker = 0;

	} // anonymous namespace

	void* OpenLibrary(const str&, str* outError) {
		if (outError) {
			*outError = "dynamic libraries are unsupported on Web";
		}
		return nullptr;
	}

	void CloseLibrary(void*) {
	}

	bool ClaimLibraryFile(const str&) {
		return true;
	}

	void ReleaseLibraryFile(const str&) {
	}

	void* FindSymbol(const void* imageBase, const char* name) {
		if (!imageBase || !name || imageBase != &_executableImageMarker) {
			return nullptr;
		}
		if (std::strcmp(name, SLT_CORE_EXTENSION_REGISTER_SYMBOL) == 0) {
			return reinterpret_cast<void*>(&ShalltearRegisterCoreExtensions);
		}
		return nullptr;
	}

	void* GetExecutableImage() {
		return &_executableImageMarker;
	}

	void* ImageBaseOf(const void* address) {
		return address ? &_executableImageMarker : nullptr;
	}

	ImageSpan GetImageSpan(const void*) {
		return {};
	}

	std::string_view GetSharedLibrarySuffix() {
		return ".wasm";
	}

	str GetSystemLibraryFileName(std::string_view stem) {
		return "lib" + str(stem) + ".wasm";
	}

	std::string_view GetDebugSymbolsSuffix() {
		return {};
	}

} // namespace Shalltear::Platform
