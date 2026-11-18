#include "HostLibrary.h"

#include <dlfcn.h>

namespace Shalltear::CSharpImpl {

	void* OpenHostLibrary(const str& utf8Path, str& outError) {
		void* image = ::dlopen(utf8Path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (!image) {
			const char* reason = ::dlerror();
			outError = reason ? reason : "dlopen failed";
		}
		return image;
	}

	void* FindHostSymbol(void* library, const char* name) {
		return ::dlsym(library, name);
	}

} // namespace Shalltear::CSharpImpl
