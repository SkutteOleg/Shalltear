#include "UI/SystemFontFallback.h"

namespace Shalltear::UI {

	struct SystemFontFallback::Impl {};

	SystemFontFallback::~SystemFontFallback() = default;

	const SystemFontFile* SystemFontFallback::Find(u32, u16, bool) {
		return nullptr;
	}

} // namespace Shalltear::UI
