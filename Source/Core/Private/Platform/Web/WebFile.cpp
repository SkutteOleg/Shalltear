#include "Platform/NativeFile.h"

#include <sys/stat.h>

namespace Shalltear::Platform {

	u64 GetFileCreationTimeUnix(const str& utf8Path) {
		struct stat status{};
		if (::stat(utf8Path.c_str(), &status) != 0 || status.st_mtime <= 0) {
			return 0;
		}
		return static_cast<u64>(status.st_mtime);
	}

	DirectoryWatcher* StartDirectoryWatcher(const Path&, FileWatcher::Callback) {
		return nullptr;
	}

	void StopDirectoryWatcher(DirectoryWatcher*) {
	}

} // namespace Shalltear::Platform
