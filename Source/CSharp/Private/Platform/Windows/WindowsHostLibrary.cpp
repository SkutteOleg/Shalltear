#include "HostLibrary.h"

#include "Shalltear/Core/StringUtil.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <format>

namespace Shalltear::CSharpImpl {

	void* OpenHostLibrary(const str& utf8Path, str& outError) {
		HMODULE image = ::LoadLibraryW(StringUtil::UTF8ToWide(utf8Path).c_str());
		if (!image) {
			outError = std::format("error {}", ::GetLastError());
		}
		return image;
	}

	void* FindHostSymbol(void* library, const char* name) {
		return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(library), name));
	}

	std::string_view GetSharedLibrarySuffix() {
		return ".dll";
	}

} // namespace Shalltear::CSharpImpl
