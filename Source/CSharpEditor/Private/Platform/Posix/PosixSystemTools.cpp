#include "SystemTools.h"

namespace Shalltear::CSharpEditor {

	std::filesystem::path MakeLongPath(const str& path) {
		return std::filesystem::path(path);
	}

	str FindSystemTool(std::string_view name) {
		return str(name);
	}

} // namespace Shalltear::CSharpEditor
