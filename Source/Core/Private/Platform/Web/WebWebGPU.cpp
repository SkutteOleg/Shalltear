#include "Graphics/WebGPU/WebGPUPlatform.h"

#include "Shalltear/Core/Log.h"

#include <emscripten/emscripten.h>

namespace Shalltear::RHI::WebGPUPlatform {

	bool LoadProcs(WebGPUProcs& out) {
#define SLT_WGPU_BIND(PascalName, SymbolName) out.SymbolName = &::SymbolName;
		SLT_WGPU_FUNCS(SLT_WGPU_BIND)
#undef SLT_WGPU_BIND
		return true;
	}

	bool SupportsWaitAny() {
		return false;
	}

	WGPUDevice TryGetPreinitializedDevice() {
		const int hasDevice = EM_ASM_INT({
			return (typeof Module != = 'undefined' && Module['preinitializedWebGPUDevice']) ? 1 : 0;
		});
		if (!hasDevice) {
			SLT_LOG_ERROR("Graphics", "WebGPU: Module.preinitializedWebGPUDevice is not set");
			return nullptr;
		}
		return emscripten_webgpu_get_device();
	}

	WGPUTextureFormat GetPreferredSurfaceFormat() {
		const int isRGBA = EM_ASM_INT({
			if (typeof navigator == = 'undefined' || !navigator.gpu) {
				return 0;
			}
			return navigator.gpu.getPreferredCanvasFormat() == = 'rgba8unorm' ? 1 : 0;
		});
		return isRGBA ? WGPUTextureFormat_RGBA8Unorm : WGPUTextureFormat_BGRA8Unorm;
	}

	WGPUSurface CreateSurface(const WebGPUProcs& wgpu, WGPUInstance instance, const NativeWindowHandle& window) {
		if (window.system != NativeWindowSystem::WebCanvas) {
			return nullptr;
		}
		const char* selector = window.window ? static_cast<const char*>(window.window) : "#canvas";
		WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvasSource = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
		canvasSource.selector = ToWGPUString(selector);

		WGPUSurfaceDescriptor surfaceDesc{};
		surfaceDesc.nextInChain = &canvasSource.chain;
		return wgpu.wgpuInstanceCreateSurface(instance, &surfaceDesc);
	}

} // namespace Shalltear::RHI::WebGPUPlatform
