#pragma once

#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/StringHash.h"

namespace Shalltear {

	struct ImageUnloadingEvent {
		const void* image = nullptr;
	};

	namespace ModuleEvents {
		inline constexpr Event<ImageUnloadingEvent> ImageUnloading{"Module.ImageUnloading"_hash};
		inline constexpr Event<> ExtensionsRegistered{"Module.ExtensionsRegistered"_hash};
	} // namespace ModuleEvents

} // namespace Shalltear
