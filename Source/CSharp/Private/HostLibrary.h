#pragma once

#include "Shalltear/Core/Types.h"

#include <string_view>

namespace Shalltear::CSharpImpl {

	// never closed
	void* OpenHostLibrary(const str& utf8Path, str& outError);

	void* FindHostSymbol(void* library, const char* name);

	std::string_view GetSharedLibrarySuffix();

} // namespace Shalltear::CSharpImpl
