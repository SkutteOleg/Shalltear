#include "Graphics/WebGPU/WebGPUPlatform.h"

#include "Platform/Library.h"

#include "Shalltear/Core/Log.h"

namespace Shalltear::RHI::WebGPUPlatform {

	namespace {

		void* _dawnLibrary = nullptr;

	} // anonymous namespace

	bool LoadProcs(WebGPUProcs& out) {
		if (!_dawnLibrary) {
			_dawnLibrary = Platform::OpenLibrary("webgpu_dawn.dll");
			if (!_dawnLibrary) {
				SLT_LOG_ERROR("Graphics", "WebGPU: failed to load webgpu_dawn.dll");
				return false;
			}
		}

		bool ok = true;
#define SLT_WGPU_LOAD(PascalName, SymbolName)                                                                 \
	out.SymbolName = reinterpret_cast<WGPUProc##PascalName>(Platform::FindSymbol(_dawnLibrary, #SymbolName)); \
	if (!out.SymbolName) {                                                                                    \
		SLT_LOG_ERROR("Graphics", "WebGPU: missing symbol '{}' in webgpu_dawn.dll", #SymbolName);             \
		ok = false;                                                                                           \
	}
		SLT_WGPU_FUNCS(SLT_WGPU_LOAD)
#undef SLT_WGPU_LOAD
		return ok;
	}

	bool SupportsWaitAny() {
		return true;
	}

	WGPUDevice TryGetPreinitializedDevice() {
		return nullptr;
	}

	WGPUTextureFormat GetPreferredSurfaceFormat() {
		return WGPUTextureFormat_BGRA8Unorm;
	}

	WGPUSurface CreateSurface(const WebGPUProcs& wgpu, WGPUInstance instance, const NativeWindowHandle& window) {
		if (window.system != NativeWindowSystem::Win32 || !window.window || !window.display) {
			return nullptr;
		}
		WGPUSurfaceSourceWindowsHWND hwndSource{};
		hwndSource.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
		hwndSource.hinstance = window.display;
		hwndSource.hwnd = window.window;

		WGPUSurfaceDescriptor surfaceDesc{};
		surfaceDesc.nextInChain = &hwndSource.chain;
		return wgpu.wgpuInstanceCreateSurface(instance, &surfaceDesc);
	}

} // namespace Shalltear::RHI::WebGPUPlatform
