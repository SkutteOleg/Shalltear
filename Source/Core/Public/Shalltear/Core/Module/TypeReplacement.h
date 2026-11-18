#pragma once

#include "Shalltear/Core/CoreAPI.h"
#include "Shalltear/Core/Types.h"

namespace Shalltear::TypeReplacement {

	SHALLTEAR_CORE_API bool CanReplace();

	// swap should unregister owner's types + may register replacements
	SHALLTEAR_CORE_API bool Replace(const void* owner, bool (*swap)(void* user), void* user);

} // namespace Shalltear::TypeReplacement
