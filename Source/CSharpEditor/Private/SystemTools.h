#pragma once

#include "Shalltear/Core/Types.h"

#include <filesystem>
#include <string_view>

namespace Shalltear::CSharpEditor {

	// system directory's curl/tar (a shell's PATH may have another)
	str FindSystemTool(std::string_view name);

	std::filesystem::path MakeLongPath(const str& path);

} // namespace Shalltear::CSharpEditor
