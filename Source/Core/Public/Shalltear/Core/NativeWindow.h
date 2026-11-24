#pragma once

#include "Shalltear/Core/Types.h"

namespace Shalltear {

	enum class NativeWindowSystem : u8 {
		None = 0,
		Win32,
		Xlib,
		Wayland,
		WebCanvas,
	};

	struct NativeWindowHandle {
		NativeWindowSystem system = NativeWindowSystem::None;
		// HINSTANCE, Display*, wl_display*
		void* display = nullptr;
		// HWND, Window (an XID), wl_surface*
		void* window = nullptr;
	};

} // namespace Shalltear
