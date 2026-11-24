#include "Graphics/PresentPolicy.h"

#include "Graphics/WebGPU/WebGPUInternal.h"
#include "Graphics/WebGPU/WebGPUPlatform.h"

#include "Shalltear/Core/Memory.h"

#include <algorithm>

namespace Shalltear::RHI {

	namespace {

		void ReleaseCurrentSurfaceTexture(SwapChainImpl& impl) {
			DeviceImpl& d = *impl.device;
			if (impl.currentHandle.IsValid()) {
				if (TextureResource* res = d.textures.Resolve(impl.currentHandle)) {
					if (res->view) {
						d.wgpu.wgpuTextureViewRelease(res->view);
					}
					*res = TextureResource{};
					d.textures.Release(impl.currentHandle.index);
				}
				impl.currentHandle = {};
			}
			if (impl.currentTexture) {
				d.wgpu.wgpuTextureRelease(impl.currentTexture);
				impl.currentTexture = nullptr;
			}
		}

		bool ConfigureSurface(SwapChainImpl& impl, Vec2I sizePx, bool vsync) {
			DeviceImpl& d = *impl.device;
			if (!impl.surface || sizePx.x <= 0 || sizePx.y <= 0 || d.lost) {
				return false;
			}

			WGPUSurfaceCapabilities caps{};
			d.wgpu.wgpuSurfaceGetCapabilities(impl.surface, d.adapter, &caps);

			WGPUTextureFormat chosen = WGPUTextureFormat_BGRA8Unorm;
			if (caps.formatCount > 0 && caps.formats) {
				chosen = caps.formats[0];
				for (size_t i = 0; i < caps.formatCount; ++i) {
					if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm) {
						chosen = WGPUTextureFormat_BGRA8Unorm;
						break;
					}
					if (caps.formats[i] == WGPUTextureFormat_RGBA8Unorm) {
						chosen = WGPUTextureFormat_RGBA8Unorm;
					}
				}
			}

			WGPUPresentMode presentMode = WGPUPresentMode_Fifo;
			if (!vsync && caps.presentModeCount > 0 && caps.presentModes) {
				for (size_t i = 0; i < caps.presentModeCount; ++i) {
					if (caps.presentModes[i] == WGPUPresentMode_Mailbox) {
						presentMode = WGPUPresentMode_Mailbox;
						break;
					}
					if (caps.presentModes[i] == WGPUPresentMode_Immediate) {
						presentMode = WGPUPresentMode_Immediate;
					}
				}
			}

			WGPUCompositeAlphaMode alphaMode = WGPUCompositeAlphaMode_Opaque;
			if (caps.alphaModeCount > 0 && caps.alphaModes) {
				alphaMode = caps.alphaModes[0];
				for (size_t i = 0; i < caps.alphaModeCount; ++i) {
					if (caps.alphaModes[i] == WGPUCompositeAlphaMode_Opaque) {
						alphaMode = WGPUCompositeAlphaMode_Opaque;
						break;
					}
				}
			}

			const WGPUTextureUsage extraCopy =
				(caps.usages & WGPUTextureUsage_CopySrc) ? WGPUTextureUsage_CopySrc : WGPUTextureUsage_None;
			const WGPUTextureUsage extraDst =
				(caps.usages & WGPUTextureUsage_CopyDst) ? WGPUTextureUsage_CopyDst : WGPUTextureUsage_None;

			d.wgpu.wgpuSurfaceCapabilitiesFreeMembers(caps);

			WGPUSurfaceConfiguration config{};
			config.device = d.device;
			config.format = chosen;
			config.usage = WGPUTextureUsage_RenderAttachment | extraCopy | extraDst;
			config.width = static_cast<u32>(sizePx.x);
			config.height = static_cast<u32>(sizePx.y);
			config.alphaMode = alphaMode;
			config.presentMode = presentMode;
			d.wgpu.wgpuSurfaceConfigure(impl.surface, &config);

			impl.wgpuFormat = chosen;
			impl.format = FromWGPUFormat(chosen);
			impl.size = sizePx;
			impl.vsync = vsync;
			impl.configured = true;
			impl.needsRecreate = false;
			return true;
		}

	} // anonymous namespace

	void ReleaseSwapChainObjects(SwapChainImpl& impl) {
		DeviceImpl& d = *impl.device;
		impl.acquiredThisFrame = false;
		ReleaseCurrentSurfaceTexture(impl);
		if (impl.surface) {
			if (impl.configured && d.device) {
				d.wgpu.wgpuSurfaceUnconfigure(impl.surface);
			}
			d.wgpu.wgpuSurfaceRelease(impl.surface);
			impl.surface = nullptr;
		}
		impl.configured = false;
	}

	void RestoreSwapChainObjects(SwapChainImpl& impl) {
		DeviceImpl& d = *impl.device;
		impl.surface = WebGPUPlatform::CreateSurface(d.wgpu, d.instance, impl.window);
		if (!impl.surface || !ConfigureSurface(impl, impl.size, impl.vsync)) {
			impl.needsRecreate = true;
		}
	}

	FrameSync SyncFrame(std::span<SwapChain* const>, f64, u64) {
		return {};
	}

	SwapChain* Device::CreateSwapChainForWindow(const NativeSwapChainDesc& desc) {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);

		WGPUSurface surface = impl->lost ? nullptr : WebGPUPlatform::CreateSurface(impl->wgpu, impl->instance, desc.window);
		if (!impl->lost && !surface) {
			SLT_LOG_ERROR("Graphics", "WebGPU CreateSwapChainForWindow: surface creation failed");
			return nullptr;
		}

		SwapChainImpl* swapImpl = New<SwapChainImpl>(MemTag::Graphics);
		swapImpl->device = impl;
		swapImpl->window = desc.window;
		swapImpl->surface = surface;
		swapImpl->size = desc.sizePx;
		swapImpl->vsync = desc.vsync;
		if (impl->lost || !ConfigureSurface(*swapImpl, desc.sizePx, desc.vsync)) {
			swapImpl->needsRecreate = true;
		}

		SwapChain* swapChain = New<SwapChain>(MemTag::Graphics);
		swapChain->_impl = swapImpl;
		impl->swapChains.push_back(swapChain);
		return swapChain;
	}

	void Device::DestroySwapChain(SwapChain* swapChain) {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		if (!swapChain) {
			return;
		}
		if (!impl->lost) {
			WaitIdle();
		}
		ReleaseSwapChainObjects(*swapChain->_impl);
		auto it = std::find(impl->swapChains.begin(), impl->swapChains.end(), swapChain);
		if (it != impl->swapChains.end()) {
			impl->swapChains.erase(it);
		}
		Delete(swapChain->_impl);
		Delete(swapChain);
	}

	TextureHandle SwapChain::AcquireNextImage() {
		SwapChainImpl& impl = *_impl;
		DeviceImpl& d = *impl.device;
		SLT_RHI_ASSERT_MAIN_THREAD(&d);
		SLT_CORE_ASSERT_MSG(d.inFrame, "AcquireNextImage outside BeginFrame/EndFrame");
		SLT_CORE_ASSERT_MSG(!impl.acquiredThisFrame, "AcquireNextImage called twice in one frame");

		if (d.lost || !impl.surface) {
			return {};
		}
		if (impl.needsRecreate || !impl.configured) {
			if (!ConfigureSurface(impl, impl.size, impl.vsync)) {
				return {};
			}
		}

		ReleaseCurrentSurfaceTexture(impl);

		WGPUSurfaceTexture surfaceTex{};
		d.wgpu.wgpuSurfaceGetCurrentTexture(impl.surface, &surfaceTex);
		if (surfaceTex.status == WGPUSurfaceGetCurrentTextureStatus_Outdated ||
			surfaceTex.status == WGPUSurfaceGetCurrentTextureStatus_Lost) {
			if (surfaceTex.texture) {
				d.wgpu.wgpuTextureRelease(surfaceTex.texture);
			}
			if (!ConfigureSurface(impl, impl.size, impl.vsync)) {
				return {};
			}
			d.wgpu.wgpuSurfaceGetCurrentTexture(impl.surface, &surfaceTex);
		}
		if ((surfaceTex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
			 surfaceTex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) ||
			!surfaceTex.texture) {
			if (surfaceTex.texture) {
				d.wgpu.wgpuTextureRelease(surfaceTex.texture);
			}
			impl.needsRecreate = true;
			return {};
		}
		if (surfaceTex.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
			impl.needsRecreate = true;
		}

		WGPUTextureViewDescriptor viewDesc{};
		viewDesc.format = impl.wgpuFormat;
		viewDesc.dimension = WGPUTextureViewDimension_2D;
		viewDesc.mipLevelCount = 1;
		viewDesc.arrayLayerCount = 1;
		viewDesc.aspect = WGPUTextureAspect_All;
		WGPUTextureView view = d.wgpu.wgpuTextureCreateView(surfaceTex.texture, &viewDesc);

		auto [index, gen] = d.textures.Acquire();
		TextureResource& res = d.textures.slots[index].resource;
		res.texture = surfaceTex.texture;
		res.view = view;
		res.size = impl.size;
		res.format = impl.format;
		res.gpuFormat = impl.format;
		res.usage = TextureUsage::RenderTarget | TextureUsage::TransferDst | TextureUsage::TransferSrc;
		res.external = true;
		res.state = ResourceState::Undefined;

		impl.currentTexture = surfaceTex.texture;
		impl.currentHandle = TextureHandle{index, gen};
		impl.acquiredThisFrame = true;
		return impl.currentHandle;
	}

	void SwapChain::Present() {
		SwapChainImpl& impl = *_impl;
		DeviceImpl& d = *impl.device;
		SLT_RHI_ASSERT_MAIN_THREAD(&d);
		SLT_CORE_ASSERT_MSG(!d.inFrame, "Present must be called after EndFrame");
		if (!impl.acquiredThisFrame) {
			return;
		}
		impl.acquiredThisFrame = false;
		if (!d.lost && impl.surface && WebGPUPlatform::SupportsWaitAny()) {
			d.wgpu.wgpuSurfacePresent(impl.surface);
		}
		ReleaseCurrentSurfaceTexture(impl);
	}

	void SwapChain::Recreate(Vec2I newSizePx, bool vsync) {
		SwapChainImpl& impl = *_impl;
		DeviceImpl& d = *impl.device;
		SLT_RHI_ASSERT_MAIN_THREAD(&d);
		SLT_CORE_ASSERT_MSG(!d.inFrame, "Recreate mid-frame is illegal");

		impl.size = newSizePx;
		impl.vsync = vsync;
		ReleaseCurrentSurfaceTexture(impl);
		if (d.lost || (!impl.surface && !(impl.surface = WebGPUPlatform::CreateSurface(d.wgpu, d.instance, impl.window))) ||
			!ConfigureSurface(impl, newSizePx, vsync)) {
			impl.needsRecreate = true;
		}
	}

	Format SwapChain::GetFormat() const {
		return _impl->format;
	}

	Vec2I SwapChain::GetSize() const {
		return _impl->size;
	}

	bool SwapChain::GetVSync() const {
		return _impl->vsync;
	}

} // namespace Shalltear::RHI
