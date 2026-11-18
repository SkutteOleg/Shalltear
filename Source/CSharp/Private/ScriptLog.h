#pragma once

#include "Shalltear/Core/Log.h"

#include <string_view>

namespace Shalltear::CSharpImpl {

	void WriteLog(LogLevel level, std::string_view category, std::string_view message);

} // namespace Shalltear::CSharpImpl
