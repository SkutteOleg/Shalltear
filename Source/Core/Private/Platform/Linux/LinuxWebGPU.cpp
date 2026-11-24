#include "Graphics/WebGPU/WebGPUPlatform.h"

#include "Platform/Library.h"

#include "Shalltear/Core/Log.h"

#include <cstdint>

namespace Shalltear::RHI::WebGPUPlatform {

	namespace {

		void* _dawnLibrary = nullptr;

	} // anonymous namespace

	bool LoadProcs(WebGPUProcs& out) {
		if (!_dawnLibrary) {
			_dawnLibrary = Platform::OpenLibrary("libwebgpu_dawn.so");
			if (!_dawnLibrary) {
				SLT_LOG_ERROR("Graphics", "WebGPU: failed to load libwebgpu_dawn.so");
				return false;
			}
		}

		bool ok = true;
#define SLT_WGPU_LOAD(PascalName, SymbolName)                                                                 \
	out.SymbolName = reinterpret_cast<WGPUProc##PascalName>(Platform::FindSymbol(_dawnLibrary, #SymbolName)); \
	if (!out.SymbolName) {                                                                                    \
		SLT_LOG_ERROR("Graphics", "WebGPU: missing symbol '{}' in libwebgpu_dawn.so", #SymbolName);           \
		ok = false;                                                                                           \
	}
		SLT_WGPU_FUNCS(SLT_WGPU_LOAD)
#undef SLT_WGPU_LOAD
		return ok;
	}

	bool SupportsWaitAny() {
		return true;
	}

	WGPUSurface CreateSurface(const WebGPUProcs& wgpu, WGPUInstance instance, const NativeWindowHandle& window) {
		if (!window.window || !window.display) {
			return nullptr;
		}
		if (window.system == NativeWindowSystem::Xlib) {
			WGPUSurfaceSourceXlibWindow xlibSource{};
			xlibSource.chain.sType = WGPUSType_SurfaceSourceXlibWindow;
			xlibSource.display = window.display;
			xlibSource.window = static_cast<u64>(reinterpret_cast<std::uintptr_t>(window.window));
			WGPUSurfaceDescriptor surfaceDesc{};
			surfaceDesc.nextInChain = &xlibSource.chain;
			return wgpu.wgpuInstanceCreateSurface(instance, &surfaceDesc);
		}
		if (window.system == NativeWindowSystem::Wayland) {
			WGPUSurfaceSourceWaylandSurface waylandSource{};
			waylandSource.chain.sType = WGPUSType_SurfaceSourceWaylandSurface;
			waylandSource.display = window.display;
			waylandSource.surface = window.window;
			WGPUSurfaceDescriptor surfaceDesc{};
			surfaceDesc.nextInChain = &waylandSource.chain;
			return wgpu.wgpuInstanceCreateSurface(instance, &surfaceDesc);
		}
		return nullptr;
	}

} // namespace Shalltear::RHI::WebGPUPlatform
