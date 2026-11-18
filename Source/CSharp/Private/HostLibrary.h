#pragma once

#include "Shalltear/Core/Types.h"

namespace Shalltear::CSharpImpl {

	// never closed
	void* OpenHostLibrary(const str& utf8Path, str& outError);

	void* FindHostSymbol(void* library, const char* name);

} // namespace Shalltear::CSharpImpl
