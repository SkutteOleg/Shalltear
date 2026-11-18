#pragma once

#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Types.h"

#include <string_view>

namespace Shalltear::CSharpImpl {

	i32 GetLogLevel() noexcept;

	void WriteLog(i32 level, std::string_view category, std::string_view message, std::string_view file, i32 line);

	// valid until package unloads
	const char* InternLogOrigin(std::string_view file);

} // namespace Shalltear::CSharpImpl
