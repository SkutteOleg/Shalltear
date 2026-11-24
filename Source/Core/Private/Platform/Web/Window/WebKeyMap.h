#pragma once

#include "Shalltear/Core/Input/InputCodes.h"

#include <string_view>

namespace Shalltear::Web {

	Key KeyFromWebCode(std::string_view code);

} // namespace Shalltear::Web
