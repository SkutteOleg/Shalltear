#pragma once

#include "Graphics/WebGPU/WebGPUProcs.h"

#include "Shalltear/Core/NativeWindow.h"

namespace Shalltear::RHI::WebGPUPlatform {

	bool LoadProcs(WebGPUProcs& out);
	bool SupportsWaitAny();
	WGPUDevice TryGetPreinitializedDevice();
	WGPUTextureFormat GetPreferredSurfaceFormat();
	WGPUSurface CreateSurface(const WebGPUProcs& wgpu, WGPUInstance instance, const NativeWindowHandle& window);

} // namespace Shalltear::RHI::WebGPUPlatform
