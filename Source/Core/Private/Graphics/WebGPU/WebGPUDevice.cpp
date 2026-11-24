#include "Graphics/WebGPU/WebGPUInternal.h"

#include "Graphics/WebGPU/WebGPUPlatform.h"

#include "Shalltear/Core/Memory.h"

#include <algorithm>
#include <cstring>

namespace Shalltear::RHI {

	u64 AlignUp(u64 value, u64 alignment) {
		return alignment > 1 ? ((value + alignment - 1) & ~(alignment - 1)) : value;
	}

	bool IsDepthFormat(Format format) {
		return format == Format::D32Float;
	}

	WGPUTextureFormat ToWGPUFormat(Format format) {
		switch (format) {
		case Format::R8Unorm:
			return WGPUTextureFormat_R8Unorm;
		case Format::RGBA8Unorm:
			return WGPUTextureFormat_RGBA8Unorm;
		case Format::RGBA8SRGB:
			return WGPUTextureFormat_RGBA8UnormSrgb;
		case Format::BGRA8Unorm:
			return WGPUTextureFormat_BGRA8Unorm;
		case Format::BGRA8SRGB:
			return WGPUTextureFormat_BGRA8UnormSrgb;
		case Format::RGBA16Float:
			return WGPUTextureFormat_RGBA16Float;
		case Format::RG16Float:
			return WGPUTextureFormat_RG16Float;
		case Format::R32Uint:
			return WGPUTextureFormat_R32Uint;
		case Format::D32Float:
			return WGPUTextureFormat_Depth32Float;
		case Format::BC5Unorm:
			return WGPUTextureFormat_BC5RGUnorm;
		case Format::BC7Unorm:
			return WGPUTextureFormat_BC7RGBAUnorm;
		case Format::BC7SRGB:
			return WGPUTextureFormat_BC7RGBAUnormSrgb;
		case Format::BC6HUFloat:
			return WGPUTextureFormat_BC6HRGBUfloat;
		case Format::RGBA32Float:
			return WGPUTextureFormat_RGBA32Float;
		default:
			return WGPUTextureFormat_Undefined;
		}
	}

	Format FromWGPUFormat(WGPUTextureFormat format) {
		switch (format) {
		case WGPUTextureFormat_R8Unorm:
			return Format::R8Unorm;
		case WGPUTextureFormat_RGBA8Unorm:
			return Format::RGBA8Unorm;
		case WGPUTextureFormat_RGBA8UnormSrgb:
			return Format::RGBA8SRGB;
		case WGPUTextureFormat_BGRA8Unorm:
			return Format::BGRA8Unorm;
		case WGPUTextureFormat_BGRA8UnormSrgb:
			return Format::BGRA8SRGB;
		case WGPUTextureFormat_RGBA16Float:
			return Format::RGBA16Float;
		case WGPUTextureFormat_RG16Float:
			return Format::RG16Float;
		case WGPUTextureFormat_R32Uint:
			return Format::R32Uint;
		case WGPUTextureFormat_Depth32Float:
			return Format::D32Float;
		case WGPUTextureFormat_BC5RGUnorm:
			return Format::BC5Unorm;
		case WGPUTextureFormat_BC7RGBAUnorm:
			return Format::BC7Unorm;
		case WGPUTextureFormat_BC7RGBAUnormSrgb:
			return Format::BC7SRGB;
		case WGPUTextureFormat_BC6HRGBUfloat:
			return Format::BC6HUFloat;
		case WGPUTextureFormat_RGBA32Float:
			return Format::RGBA32Float;
		default:
			return Format::Unknown;
		}
	}

	WGPUTextureUsage ToWGPUTextureUsage(TextureUsage usage, Format gpuFormat, u32 samples) {
		WGPUTextureUsage flags = WGPUTextureUsage_None;
		if (HasFlag(usage, TextureUsage::Sampled)) {
			flags |= WGPUTextureUsage_TextureBinding;
		}
		if (HasFlag(usage, TextureUsage::RenderTarget) || HasFlag(usage, TextureUsage::DepthStencil)) {
			flags |= WGPUTextureUsage_RenderAttachment;
		}
		if (HasFlag(usage, TextureUsage::TransferSrc)) {
			flags |= WGPUTextureUsage_CopySrc;
		}
		if (HasFlag(usage, TextureUsage::TransferDst)) {
			flags |= WGPUTextureUsage_CopyDst;
		}
		// RenderAttachment is also needed for ResolveTexture and GPU GenerateMips on non-block color textures
		if (!IsBlockCompressed(gpuFormat) && !IsDepthFormat(gpuFormat) && samples == 1 &&
			(HasFlag(usage, TextureUsage::TransferDst) || HasFlag(usage, TextureUsage::Sampled))) {
			flags |= WGPUTextureUsage_RenderAttachment;
		}
		return flags;
	}

	namespace {

		constexpr u64 DEFAULT_TRANSIENT_RING_BYTES = 16ull * 1024 * 1024;
		constexpr u64 DEFAULT_PUSH_RING_BYTES = 4ull * 1024 * 1024;
		constexpr u64 DEFAULT_VERTEX_HEAP_BYTES = 32ull * 1024 * 1024;

		// Process-wide WebGPU instance avoids Dawn D3D12 teardown/reinit issues across Device recreate cycles
		WGPUInstance _sharedInstance = nullptr;

		void OnUncapturedError(WGPUDevice const*, WGPUErrorType type, WGPUStringView message, void*, void*) {
			const std::string_view text = message.data ? std::string_view(message.data, message.length) : std::string_view("<no message>");
			const char* kind = type == WGPUErrorType_Validation	   ? "Validation"
							   : type == WGPUErrorType_OutOfMemory ? "OutOfMemory"
																   : "Internal";
			SLT_LOG_ERROR("WebGPU", "[{}] {}", kind, text);
		}

		void OnDeviceLost(WGPUDevice const*, WGPUDeviceLostReason reason, WGPUStringView message, void* userdata1, void*) {
			if (reason == WGPUDeviceLostReason_Destroyed || reason == WGPUDeviceLostReason_CallbackCancelled) {
				return;
			}
			const std::string_view text = message.data ? std::string_view(message.data, message.length) : std::string_view("device lost");
			SLT_LOG_ERROR("WebGPU", "device lost: {}", text);
			if (DeviceImpl* impl = static_cast<DeviceImpl*>(userdata1)) {
				impl->MarkLost("WebGPUDeviceLost");
			}
		}

		void OnQueueWorkDone(WGPUQueueWorkDoneStatus, WGPUStringView, void* userdata1, void*) {
			if (bool* done = static_cast<bool*>(userdata1)) {
				*done = true;
			}
		}

		void OnBufferMapped(WGPUMapAsyncStatus status, WGPUStringView, void* userdata1, void*) {
			auto* box = static_cast<std::shared_ptr<PendingReadback>*>(userdata1);
			if (!box) {
				return;
			}
			std::shared_ptr<PendingReadback> pending = *box;
			delete box;
			if (!pending) {
				return;
			}
			if (status == WGPUMapAsyncStatus_Success) {
				pending->mapped = true;
			} else {
				pending->failed = true;
			}
		}

	} // anonymous namespace

	void DeviceImpl::WaitFuture(WGPUFuture future) {
		if (!instance || future.id == 0) {
			return;
		}
		if (WebGPUPlatform::SupportsWaitAny()) {
			WGPUFutureWaitInfo waitInfo{};
			waitInfo.future = future;
			wgpu.wgpuInstanceWaitAny(instance, 1, &waitInfo, UINT64_MAX);
		} else {
			wgpu.wgpuInstanceProcessEvents(instance);
		}
	}

	void DeviceImpl::MarkLost(const char* reason) {
		if (lost) {
			return;
		}
		lost = true;
		lossSimulated = reason == nullptr;
		const auto now = std::chrono::steady_clock::now();
		lostAt = now;
		if (lossSimulated) {
			nextRebuild = now;
			SLT_LOG_WARN("Graphics", "WebGPU device loss simulated");
			return;
		}
		const u32 inWindow = losses.Note(static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(now - created).count()));
		const u64 delayMs = RebuildDelayMs(inWindow);
		nextRebuild = now + std::chrono::milliseconds(delayMs);
		SLT_LOG_ERROR("Graphics", "WebGPU device lost ({}, {} loss(es) in last {} s). Rebuilding in {} s",
					  reason, inWindow, LOSS_WINDOW_MS / 1000, delayMs / 1000);
	}

	bool DeviceImpl::InitVertexHeap(u64 initialCapacity) {
		const u64 cap = std::min(AlignUp(initialCapacity, BINDING_OFFSET_ALIGNMENT),
								 limits.maxStorageBufferBindingSize ? limits.maxStorageBufferBindingSize : DEFAULT_VERTEX_HEAP_BYTES);
		WGPUBufferDescriptor desc{};
		desc.label = ToWGPUString("ShalltearVertexHeap");
		desc.size = cap;
		desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst |
					 WGPUBufferUsage_Index | WGPUBufferUsage_Indirect;
		vertexHeap.buffer = wgpu.wgpuDeviceCreateBuffer(device, &desc);
		if (!vertexHeap.buffer) {
			return false;
		}
		vertexHeap.capacity = cap;
		vertexHeap.highWater = BINDING_OFFSET_ALIGNMENT;
		vertexHeap.freeBlocks.clear();
		++vertexHeap.version;
		return true;
	}

	void DeviceImpl::DestroyVertexHeap() {
		if (vertexHeap.buffer) {
			wgpu.wgpuBufferDestroy(vertexHeap.buffer);
			wgpu.wgpuBufferRelease(vertexHeap.buffer);
			vertexHeap.buffer = nullptr;
		}
		vertexHeap.capacity = 0;
		vertexHeap.highWater = BINDING_OFFSET_ALIGNMENT;
		vertexHeap.freeBlocks.clear();
	}

	bool DeviceImpl::GrowVertexHeap(u64 requiredCapacity) {
		const u64 maxBinding = limits.maxStorageBufferBindingSize ? limits.maxStorageBufferBindingSize : (128ull * 1024 * 1024);
		u64 nextCap = std::max(vertexHeap.capacity * 2, AlignUp(requiredCapacity, BINDING_OFFSET_ALIGNMENT));
		if (nextCap > maxBinding) {
			if (requiredCapacity > maxBinding) {
				SLT_LOG_ERROR("Graphics", "WebGPU: vertex heap request {} B exceeds maxStorageBufferBindingSize {} B",
							  requiredCapacity, maxBinding);
				return false;
			}
			nextCap = maxBinding;
		}

		WGPUBufferDescriptor desc{};
		desc.label = ToWGPUString("ShalltearVertexHeap");
		desc.size = nextCap;
		desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst |
					 WGPUBufferUsage_Index | WGPUBufferUsage_Indirect;
		WGPUBuffer nextBuffer = wgpu.wgpuDeviceCreateBuffer(device, &desc);
		if (!nextBuffer) {
			return false;
		}

		if (vertexHeap.buffer && vertexHeap.highWater > 0) {
			WGPUCommandEncoderDescriptor encDesc{};
			WGPUCommandEncoder enc = wgpu.wgpuDeviceCreateCommandEncoder(device, &encDesc);
			wgpu.wgpuCommandEncoderCopyBufferToBuffer(enc, vertexHeap.buffer, 0, nextBuffer, 0, vertexHeap.highWater);
			WGPUCommandBufferDescriptor cbDesc{};
			WGPUCommandBuffer cb = wgpu.wgpuCommandEncoderFinish(enc, &cbDesc);
			wgpu.wgpuQueueSubmit(queue, 1, &cb);
			wgpu.wgpuCommandBufferRelease(cb);
			wgpu.wgpuCommandEncoderRelease(enc);
		}

		if (vertexHeap.buffer) {
			DeletionQueueForDestroy().buffers.push_back(vertexHeap.buffer);
		}
		vertexHeap.buffer = nextBuffer;
		vertexHeap.capacity = nextCap;
		++vertexHeap.version;
		return true;
	}

	u64 DeviceImpl::AllocateVertexHeap(u64 size, u64& outAlignedSize) {
		outAlignedSize = AlignUp(std::max<u64>(size, 4), BINDING_OFFSET_ALIGNMENT);
		for (size_t i = 0; i < vertexHeap.freeBlocks.size(); ++i) {
			if (vertexHeap.freeBlocks[i].size >= outAlignedSize) {
				const u64 offset = vertexHeap.freeBlocks[i].offset;
				if (vertexHeap.freeBlocks[i].size == outAlignedSize) {
					vertexHeap.freeBlocks.erase(vertexHeap.freeBlocks.begin() + static_cast<std::ptrdiff_t>(i));
				} else {
					vertexHeap.freeBlocks[i].offset += outAlignedSize;
					vertexHeap.freeBlocks[i].size -= outAlignedSize;
				}
				return offset;
			}
		}
		if (vertexHeap.highWater + outAlignedSize > vertexHeap.capacity) {
			if (!GrowVertexHeap(vertexHeap.highWater + outAlignedSize)) {
				return 0;
			}
		}
		const u64 offset = vertexHeap.highWater;
		vertexHeap.highWater += outAlignedSize;
		return offset;
	}

	void DeviceImpl::FreeVertexHeap(u64 offset, u64 alignedSize) {
		if (offset == 0 || alignedSize == 0) {
			return;
		}
		vertexHeap.freeBlocks.push_back({offset, alignedSize});
		std::sort(vertexHeap.freeBlocks.begin(), vertexHeap.freeBlocks.end(),
				  [](const VertexHeapBlock& a, const VertexHeapBlock& b) { return a.offset < b.offset; });
		size_t out = 0;
		for (size_t i = 0; i < vertexHeap.freeBlocks.size(); ++i) {
			if (out > 0 && vertexHeap.freeBlocks[out - 1].offset + vertexHeap.freeBlocks[out - 1].size == vertexHeap.freeBlocks[i].offset) {
				vertexHeap.freeBlocks[out - 1].size += vertexHeap.freeBlocks[i].size;
			} else {
				vertexHeap.freeBlocks[out++] = vertexHeap.freeBlocks[i];
			}
		}
		vertexHeap.freeBlocks.resize(out);
	}

	DeletionQueue& DeviceImpl::DeletionQueueForDestroy() {
		if (inFrame) {
			return frames[frameIndex].deletionQueue;
		}
		return frames[(frameIndex + FRAMES_IN_FLIGHT - 1) % FRAMES_IN_FLIGHT].deletionQueue;
	}

	void DeviceImpl::FlushDeletionQueue(DeletionQueue& q) {
		for (const VertexHeapBlock& block : q.vertexHeapReleases) {
			FreeVertexHeap(block.offset, block.size);
		}
		for (const DeletionQueue::AtlasRelease& release : q.atlasReleases) {
			if (release.isCube) {
				if (release.slot < TextureAtlasTable::ATLAS_CUBE_SLOTS) {
					table.cubeUsed[release.slot] = false;
				}
			} else if (release.slot < TextureAtlasTable::ATLAS_2D_MAX_SLOTS) {
				table.slots2D[release.slot].used = false;
			}
		}
		for (WGPUBindGroup bg : q.bindGroups) {
			if (bg) {
				wgpu.wgpuBindGroupRelease(bg);
			}
		}
		for (WGPUBuffer buf : q.buffers) {
			if (buf) {
				wgpu.wgpuBufferDestroy(buf);
				wgpu.wgpuBufferRelease(buf);
			}
		}
		for (WGPUTextureView view : q.textureViews) {
			if (view) {
				wgpu.wgpuTextureViewRelease(view);
			}
		}
		for (WGPUTexture tex : q.textures) {
			if (tex) {
				wgpu.wgpuTextureDestroy(tex);
				wgpu.wgpuTextureRelease(tex);
			}
		}
		for (WGPUSampler samp : q.samplers) {
			if (samp) {
				wgpu.wgpuSamplerRelease(samp);
			}
		}
		for (WGPURenderPipeline pipe : q.renderPipelines) {
			if (pipe) {
				wgpu.wgpuRenderPipelineRelease(pipe);
			}
		}
		for (WGPUComputePipeline pipe : q.computePipelines) {
			if (pipe) {
				wgpu.wgpuComputePipelineRelease(pipe);
			}
		}
		for (WGPUPipelineLayout lay : q.pipelineLayouts) {
			if (lay) {
				wgpu.wgpuPipelineLayoutRelease(lay);
			}
		}
		for (WGPUShaderModule mod : q.shaderModules) {
			if (mod) {
				wgpu.wgpuShaderModuleRelease(mod);
			}
		}
		q.buffers.clear();
		q.textures.clear();
		q.textureViews.clear();
		q.samplers.clear();
		q.renderPipelines.clear();
		q.computePipelines.clear();
		q.pipelineLayouts.clear();
		q.shaderModules.clear();
		q.bindGroups.clear();
		q.vertexHeapReleases.clear();
		q.atlasReleases.clear();
	}

	void DeviceImpl::DrainFrameReadbacks(FrameContext& frame, bool wait) {
		for (size_t i = 0; i < frame.pendingReadbacks.size();) {
			std::shared_ptr<PendingReadback>& pending = frame.pendingReadbacks[i];
			if (!pending || !pending->staging) {
				frame.pendingReadbacks.erase(frame.pendingReadbacks.begin() + static_cast<std::ptrdiff_t>(i));
				continue;
			}
			if (wait && !pending->mapped && !pending->failed && pending->submitted) {
				WaitFuture(pending->future);
			}
			if (pending->mapped) {
				const u64 mapSize = static_cast<u64>(pending->paddedRowBytes) * pending->rowCount;
				const void* mapped = wgpu.wgpuBufferGetConstMappedRange(pending->staging, 0, static_cast<size_t>(mapSize));
				if (mapped && pending->dstShadow) {
					const u8* srcBytes = static_cast<const u8*>(mapped);
					u8* dstBytes = pending->dstShadow->data() + pending->dstOffset;
					for (u32 row = 0; row < pending->rowCount; ++row) {
						const u64 writeEnd = pending->dstOffset + static_cast<u64>(row + 1) * pending->tightRowBytes;
						if (writeEnd <= pending->dstShadow->size()) {
							std::memcpy(dstBytes + static_cast<u64>(row) * pending->tightRowBytes,
										srcBytes + static_cast<u64>(row) * pending->paddedRowBytes,
										pending->tightRowBytes);
						}
					}
				}
				wgpu.wgpuBufferUnmap(pending->staging);
				wgpu.wgpuBufferDestroy(pending->staging);
				wgpu.wgpuBufferRelease(pending->staging);
				pending->staging = nullptr;
				frame.pendingReadbacks.erase(frame.pendingReadbacks.begin() + static_cast<std::ptrdiff_t>(i));
				continue;
			}
			if (pending->failed) {
				wgpu.wgpuBufferDestroy(pending->staging);
				wgpu.wgpuBufferRelease(pending->staging);
				pending->staging = nullptr;
				frame.pendingReadbacks.erase(frame.pendingReadbacks.begin() + static_cast<std::ptrdiff_t>(i));
				continue;
			}
			++i;
		}
	}

	void DeviceImpl::FlushCPUToGPUBuffers() {
		if (lost || !queue) {
			return;
		}
		for (u32 i = 1; i < buffers.slots.size(); ++i) {
			if (!buffers.slots[i].alive) {
				continue;
			}
			BufferResource& res = buffers.slots[i].resource;
			if (res.access == MemoryAccess::CPUToGPU && !res.isTransientRing && res.cpuShadow && !res.cpuShadow->empty()) {
				WGPUBuffer target = res.inVertexHeap ? vertexHeap.buffer : res.buffer;
				const u64 offset = res.inVertexHeap ? res.heapOffset : 0;
				if (target) {
					wgpu.wgpuQueueWriteBuffer(queue, target, offset, res.cpuShadow->data(), res.gpuSize);
				}
			}
		}
	}

	bool DeviceImpl::CreateCore(bool rebuild) {
		if (!WebGPUPlatform::LoadProcs(wgpu)) {
			rebuildFailure = "WebGPU library load failed";
			return false;
		}

		if (!_sharedInstance) {
			WGPUInstanceFeatureName instFeatures[] = {WGPUInstanceFeatureName_TimedWaitAny};
			WGPUInstanceLimits instLimits{};
			instLimits.timedWaitAnyMaxCount = 64;
			WGPUInstanceDescriptor instDesc{};
			if (WebGPUPlatform::SupportsWaitAny()) {
				instDesc.requiredFeatureCount = 1;
				instDesc.requiredFeatures = instFeatures;
				instDesc.requiredLimits = &instLimits;
			}
			_sharedInstance = wgpu.wgpuCreateInstance(&instDesc);
		}
		instance = _sharedInstance;
		if (!instance) {
			rebuildFailure = "wgpuCreateInstance failed";
			return false;
		}

		if (WGPUDevice preinitialized = WebGPUPlatform::TryGetPreinitializedDevice()) {
			device = preinitialized;
			queue = wgpu.wgpuDeviceGetQueue(device);
			wgpu.wgpuDeviceGetLimits(device, &limits);
			textureCompressionBC = wgpu.wgpuDeviceHasFeature(device, WGPUFeatureName_TextureCompressionBC) != 0;
			float32Filterable = wgpu.wgpuDeviceHasFeature(device, WGPUFeatureName_Float32Filterable) != 0;
			if (!rebuild) {
				AdapterInfo adInfo;
				adInfo.name = "WebGPU Browser Adapter";
				adInfo.discrete = true;
				adInfo.apiVersion = 1;
				adapters.clear();
				adapters.push_back(std::move(adInfo));
				selectedAdapter = 0;
			}
			SLT_LOG_INFO("Graphics", "WebGPU device ready (preinitialized): '{}' (BC={}, float32Filter={})",
						 adapters.empty() ? str("WebGPU") : adapters[0].name, textureCompressionBC, float32Filterable);
			return true;
		}

		struct AdapterResult {
			WGPUAdapter adapter = nullptr;
			WGPURequestAdapterStatus status = WGPURequestAdapterStatus_Error;
			str message;
		} adapterResult;

		WGPURequestAdapterOptions adapterOpts{};
		adapterOpts.featureLevel = WGPUFeatureLevel_Core;
		adapterOpts.powerPreference = WGPUPowerPreference_HighPerformance;
		WGPURequestAdapterCallbackInfo adapterCb{};
		adapterCb.mode = WebGPUPlatform::SupportsWaitAny() ? WGPUCallbackMode_WaitAnyOnly : WGPUCallbackMode_AllowProcessEvents;
		adapterCb.callback = [](WGPURequestAdapterStatus status, WGPUAdapter ad, WGPUStringView msg, void* ud1, void*) {
			auto* r = static_cast<AdapterResult*>(ud1);
			r->status = status;
			r->adapter = ad;
			if (msg.data && msg.length > 0) {
				r->message.assign(msg.data, msg.length);
			}
		};
		adapterCb.userdata1 = &adapterResult;

		WGPUFuture adapterFuture = wgpu.wgpuInstanceRequestAdapter(instance, &adapterOpts, adapterCb);
		WaitFuture(adapterFuture);
		if (adapterResult.status != WGPURequestAdapterStatus_Success || !adapterResult.adapter) {
			rebuildFailure = adapterResult.message.empty() ? "no WebGPU adapter available" : adapterResult.message;
			return false;
		}
		adapter = adapterResult.adapter;

		WGPUAdapterInfo info{};
		wgpu.wgpuAdapterGetInfo(adapter, &info);
		AdapterInfo adInfo;
		if (info.device.data && info.device.length > 0) {
			adInfo.name.assign(info.device.data, info.device.length);
		} else if (info.description.data && info.description.length > 0) {
			adInfo.name.assign(info.description.data, info.description.length);
		} else {
			adInfo.name = "WebGPU Adapter";
		}
		adInfo.discrete = info.adapterType == WGPUAdapterType_DiscreteGPU;
		adInfo.apiVersion = 1;
		wgpu.wgpuAdapterInfoFreeMembers(info);

		if (!rebuild) {
			adapters.clear();
			adapters.push_back(std::move(adInfo));
			selectedAdapter = 0;
		}

		WGPULimits adapterLimits{};
		wgpu.wgpuAdapterGetLimits(adapter, &adapterLimits);
		adapterLimits.nextInChain = nullptr;

		Vector<WGPUFeatureName> wantedFeatures{GetMemoryResource(MemTag::Graphics)};
		if (wgpu.wgpuAdapterHasFeature(adapter, WGPUFeatureName_TextureCompressionBC)) {
			wantedFeatures.push_back(WGPUFeatureName_TextureCompressionBC);
		}
		if (wgpu.wgpuAdapterHasFeature(adapter, WGPUFeatureName_Float32Filterable)) {
			wantedFeatures.push_back(WGPUFeatureName_Float32Filterable);
		}

		struct DeviceResult {
			WGPUDevice device = nullptr;
			WGPURequestDeviceStatus status = WGPURequestDeviceStatus_Error;
			str message;
		} deviceResult;

		WGPUDeviceDescriptor deviceDesc{};
		deviceDesc.label = ToWGPUString("ShalltearWebGPUDevice");
		deviceDesc.requiredFeatureCount = wantedFeatures.size();
		deviceDesc.requiredFeatures = wantedFeatures.data();
		deviceDesc.requiredLimits = &adapterLimits;
		deviceDesc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
		deviceDesc.deviceLostCallbackInfo.callback = &OnDeviceLost;
		deviceDesc.deviceLostCallbackInfo.userdata1 = this;
		deviceDesc.uncapturedErrorCallbackInfo.callback = &OnUncapturedError;

		WGPURequestDeviceCallbackInfo deviceCb{};
		deviceCb.mode = WebGPUPlatform::SupportsWaitAny() ? WGPUCallbackMode_WaitAnyOnly : WGPUCallbackMode_AllowProcessEvents;
		deviceCb.callback = [](WGPURequestDeviceStatus status, WGPUDevice dev, WGPUStringView msg, void* ud1, void*) {
			auto* r = static_cast<DeviceResult*>(ud1);
			r->status = status;
			r->device = dev;
			if (msg.data && msg.length > 0) {
				r->message.assign(msg.data, msg.length);
			}
		};
		deviceCb.userdata1 = &deviceResult;

		WGPUFuture deviceFuture = wgpu.wgpuAdapterRequestDevice(adapter, &deviceDesc, deviceCb);
		WaitFuture(deviceFuture);
		if (deviceResult.status != WGPURequestDeviceStatus_Success || !deviceResult.device) {
			rebuildFailure = deviceResult.message.empty() ? "wgpuAdapterRequestDevice failed" : deviceResult.message;
			return false;
		}

		device = deviceResult.device;
		queue = wgpu.wgpuDeviceGetQueue(device);
		wgpu.wgpuDeviceGetLimits(device, &limits);
		textureCompressionBC = wgpu.wgpuDeviceHasFeature(device, WGPUFeatureName_TextureCompressionBC) != 0;
		float32Filterable = wgpu.wgpuDeviceHasFeature(device, WGPUFeatureName_Float32Filterable) != 0;

		SLT_LOG_INFO("Graphics", "WebGPU device ready: '{}' (BC={}, float32Filter={})",
					 adapters.empty() ? str("WebGPU") : adapters[0].name, textureCompressionBC, float32Filterable);
		return true;
	}

	bool DeviceImpl::CreateObjects() {
		if (!InitVertexHeap(DEFAULT_VERTEX_HEAP_BYTES)) {
			return false;
		}
		if (!CreateTable()) {
			return false;
		}

		const u64 maxStorage = limits.maxStorageBufferBindingSize ? limits.maxStorageBufferBindingSize : DEFAULT_TRANSIENT_RING_BYTES;
		const u64 transientCap = std::min(DEFAULT_TRANSIENT_RING_BYTES, maxStorage);
		const u64 pushCap = std::min(DEFAULT_PUSH_RING_BYTES, maxStorage);

		for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
			FrameContext& f = frames[i];
			f.commandList._impl = &f.commandListImpl;
			f.commandListImpl.device = this;

			WGPUBufferDescriptor tDesc{};
			tDesc.label = ToWGPUString("ShalltearTransientRing");
			tDesc.size = transientCap;
			tDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
			f.transientBuffer = wgpu.wgpuDeviceCreateBuffer(device, &tDesc);
			if (!f.transientBuffer) {
				return false;
			}
			f.transientCapacity = transientCap;
			f.transientUsed = BINDING_OFFSET_ALIGNMENT;
			f.transientCPU.assign(static_cast<size_t>(transientCap), 0);

			auto [index, gen] = buffers.Acquire();
			BufferResource& tRes = buffers.slots[index].resource;
			tRes.buffer = f.transientBuffer;
			tRes.size = transientCap;
			tRes.gpuSize = transientCap;
			tRes.usage = BufferUsage::Uniform | BufferUsage::Storage | BufferUsage::TransferSrc | BufferUsage::DeviceAddress;
			tRes.access = MemoryAccess::CPUToGPU;
			tRes.isTransientRing = true;
			tRes.deviceAddress = WEBGPU_TRANSIENT_HEAP_TAG;
			f.transientHandle = BufferHandle{index, gen};

			WGPUBufferDescriptor pDesc{};
			pDesc.label = ToWGPUString("ShalltearPushRing");
			pDesc.size = pushCap;
			pDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
			f.pushBuffer = wgpu.wgpuDeviceCreateBuffer(device, &pDesc);
			if (!f.pushBuffer) {
				return false;
			}
			f.pushCapacity = pushCap;
			f.pushUsed = 0;
			f.pushCPU.assign(static_cast<size_t>(pushCap), 0);

			EnsureFrameSet3BindGroups(f);
		}
		return true;
	}

	void DeviceImpl::ReleaseObjects() {
		for (SwapChain* swapChain : swapChains) {
			ReleaseSwapChainObjects(*swapChain->_impl);
		}

		for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
			FrameContext& f = frames[i];
			DrainFrameReadbacks(f, !lost);
			if (f.commandListImpl.computePass) {
				wgpu.wgpuComputePassEncoderEnd(f.commandListImpl.computePass);
				wgpu.wgpuComputePassEncoderRelease(f.commandListImpl.computePass);
				f.commandListImpl.computePass = nullptr;
			}
			if (f.commandListImpl.renderPass) {
				wgpu.wgpuRenderPassEncoderEnd(f.commandListImpl.renderPass);
				wgpu.wgpuRenderPassEncoderRelease(f.commandListImpl.renderPass);
				f.commandListImpl.renderPass = nullptr;
			}
			if (f.commandListImpl.encoder) {
				wgpu.wgpuCommandEncoderRelease(f.commandListImpl.encoder);
				f.commandListImpl.encoder = nullptr;
			}
			for (WGPUBindGroup bg : f.bindGroups.ownedGroups) {
				if (bg) {
					wgpu.wgpuBindGroupRelease(bg);
				}
			}
			f.bindGroups.ownedGroups.clear();
			f.bindGroups.Reset();
			FlushDeletionQueue(f.deletionQueue);

			if (f.set3GraphicsGroup) {
				wgpu.wgpuBindGroupRelease(f.set3GraphicsGroup);
				f.set3GraphicsGroup = nullptr;
			}
			if (f.set3ComputeReadWriteGroup) {
				wgpu.wgpuBindGroupRelease(f.set3ComputeReadWriteGroup);
				f.set3ComputeReadWriteGroup = nullptr;
			}
			if (f.transientHandle.IsValid()) {
				if (BufferResource* tRes = buffers.Resolve(f.transientHandle)) {
					*tRes = BufferResource{};
					buffers.Release(f.transientHandle.index);
				}
				f.transientHandle = {};
			}
			if (f.transientBuffer) {
				wgpu.wgpuBufferDestroy(f.transientBuffer);
				wgpu.wgpuBufferRelease(f.transientBuffer);
				f.transientBuffer = nullptr;
			}
			if (f.pushBuffer) {
				wgpu.wgpuBufferDestroy(f.pushBuffer);
				wgpu.wgpuBufferRelease(f.pushBuffer);
				f.pushBuffer = nullptr;
			}
			f.transientCPU.clear();
			f.pushCPU.clear();
			f.hasWorkDoneFuture = false;
		}

		for (u32 i = 1; i < pipelines.slots.size(); ++i) {
			if (!pipelines.slots[i].alive) {
				continue;
			}
			PipelineResource& p = pipelines.slots[i].resource;
			for (GraphicsPipelineVariant& v : p.variants) {
				if (v.pipeline) {
					wgpu.wgpuRenderPipelineRelease(v.pipeline);
				}
			}
			p.variants.clear();
			if (p.computePipeline) {
				wgpu.wgpuComputePipelineRelease(p.computePipeline);
				p.computePipeline = nullptr;
			}
			if (p.layout) {
				wgpu.wgpuPipelineLayoutRelease(p.layout);
				p.layout = nullptr;
			}
			if (p.vertexModule) {
				wgpu.wgpuShaderModuleRelease(p.vertexModule);
				p.vertexModule = nullptr;
			}
			if (p.pixelModule) {
				wgpu.wgpuShaderModuleRelease(p.pixelModule);
				p.pixelModule = nullptr;
			}
		}

		for (u32 i = 1; i < buffers.slots.size(); ++i) {
			if (!buffers.slots[i].alive) {
				continue;
			}
			BufferResource& b = buffers.slots[i].resource;
			if (!b.inVertexHeap && b.buffer) {
				wgpu.wgpuBufferDestroy(b.buffer);
				wgpu.wgpuBufferRelease(b.buffer);
			}
			b.buffer = nullptr;
			b.deviceAddress = 0;
		}

		for (u32 i = 1; i < textures.slots.size(); ++i) {
			if (!textures.slots[i].alive) {
				continue;
			}
			TextureResource& t = textures.slots[i].resource;
			for (WGPUTextureView lv : t.layerViews) {
				if (lv) {
					wgpu.wgpuTextureViewRelease(lv);
				}
			}
			t.layerViews.clear();
			if (t.view) {
				wgpu.wgpuTextureViewRelease(t.view);
				t.view = nullptr;
			}
			if (!t.external && t.texture) {
				wgpu.wgpuTextureDestroy(t.texture);
				wgpu.wgpuTextureRelease(t.texture);
			}
			t.texture = nullptr;
			t.tableIndex = TABLE_NONE;
		}

		for (u32 i = 1; i < samplers.slots.size(); ++i) {
			if (!samplers.slots[i].alive) {
				continue;
			}
			SamplerResource& s = samplers.slots[i].resource;
			if (s.sampler) {
				wgpu.wgpuSamplerRelease(s.sampler);
				s.sampler = nullptr;
			}
			s.tableIndex = TABLE_NONE;
		}

		for (auto& [hash, layout] : setLayoutCache) {
			if (layout) {
				wgpu.wgpuBindGroupLayoutRelease(layout);
			}
		}
		setLayoutCache.clear();

		DestroyTable();
		DestroyVertexHeap();
	}

	bool DeviceImpl::RestoreObjects() {
		if (!CreateObjects()) {
			return false;
		}
		for (u32 i = 1; i < buffers.slots.size(); ++i) {
			if (!buffers.slots[i].alive || buffers.slots[i].resource.isTransientRing) {
				continue;
			}
			if (!CreateBufferObject(buffers.slots[i].resource, nullptr)) {
				return false;
			}
		}
		for (u32 i = 1; i < textures.slots.size(); ++i) {
			if (!textures.slots[i].alive || textures.slots[i].resource.external) {
				continue;
			}
			if (!CreateTextureObjects(textures.slots[i].resource, nullptr)) {
				return false;
			}
		}
		for (u32 i = 1; i < samplers.slots.size(); ++i) {
			if (!samplers.slots[i].alive) {
				continue;
			}
			if (!CreateSamplerObject(samplers.slots[i].resource, nullptr)) {
				return false;
			}
		}
		for (SwapChain* swapChain : swapChains) {
			RestoreSwapChainObjects(*swapChain->_impl);
		}
		return true;
	}

	void DeviceImpl::DestroyCore() {
		if (queue) {
			wgpu.wgpuQueueRelease(queue);
			queue = nullptr;
		}
		if (device) {
			if (adapter) {
				wgpu.wgpuDeviceDestroy(device);
			}
			wgpu.wgpuDeviceRelease(device);
			device = nullptr;
		}
		if (adapter) {
			wgpu.wgpuAdapterRelease(adapter);
			adapter = nullptr;
		}
		instance = nullptr;
	}

	Device* Device::Create(const DeviceDesc& desc) {
		DeviceImpl* impl = New<DeviceImpl>(MemTag::Graphics);
		impl->desc = desc;
		impl->validation = desc.enableValidation;
		impl->headless = desc.headless;
		impl->mainThread = std::this_thread::get_id();

		if (!impl->CreateCore(false) || !impl->CreateObjects()) {
			SLT_LOG_ERROR("Graphics", "WebGPU Device::Create failed: {}", impl->rebuildFailure);
			impl->ReleaseObjects();
			impl->DestroyCore();
			Delete(impl);
			return nullptr;
		}

		Device* dev = New<Device>(MemTag::Graphics);
		dev->_impl = impl;
		return dev;
	}

	void Device::Destroy(Device* device) {
		if (!device) {
			return;
		}
		DeviceImpl* impl = device->_impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);

		if (!impl->lost) {
			device->WaitIdle();
		}

		if (!impl->swapChains.empty()) {
			SLT_LOG_ERROR("Graphics", "{} swapchain(s) still alive at device teardown. Destroying", impl->swapChains.size());
			while (!impl->swapChains.empty()) {
				device->DestroySwapChain(impl->swapChains.back());
			}
		}

		impl->ReleaseObjects();

		auto reportLeaks = [](const char* kind, u32 count) {
			if (count > 0) {
				SLT_LOG_ERROR("Graphics", "{} {} object(s) leaked at device teardown", count, kind);
			}
		};
		reportLeaks("buffer", impl->buffers.liveCount);
		reportLeaks("texture", impl->textures.liveCount);
		reportLeaks("sampler", impl->samplers.liveCount);
		reportLeaks("pipeline", impl->pipelines.liveCount);
		SLT_CORE_ASSERT_MSG(
			impl->buffers.liveCount == 0 && impl->textures.liveCount == 0 &&
				impl->samplers.liveCount == 0 && impl->pipelines.liveCount == 0,
			"GPU resources leaked at device teardown (see log)");

		impl->DestroyCore();
		Delete(impl);
		Delete(device);
	}

	std::span<const AdapterInfo> Device::GetAdapters() const {
		return {_impl->adapters.data(), _impl->adapters.size()};
	}

	const AdapterInfo& Device::GetSelectedAdapter() const {
		return _impl->adapters[_impl->selectedAdapter >= 0 ? static_cast<size_t>(_impl->selectedAdapter) : 0];
	}

	DeviceCapabilities Device::GetCapabilities() const {
		DeviceCapabilities caps;
		caps.bufferDeviceAddress = false;
		caps.bindlessTextureTable = false;
		caps.pushConstants = false;
		caps.dynamicFrontFace = false;
		caps.dynamicDepthBias = false;
		caps.wireframeFill = false;
		caps.persistentBufferMapping = false;
		caps.syncReadback = WebGPUPlatform::SupportsWaitAny();
		caps.textureCompressionBC = _impl->textureCompressionBC;
		caps.maxSampleCount = 4;
		caps.shaderFormat = ShaderBytecodeFormat::WGSL;
		return caps;
	}

	u32 Device::GetSupportedSampleCounts(Format format, TextureUsage) const {
		if (format == Format::Unknown || IsBlockCompressed(format)) {
			return 1u;
		}
		if (format == Format::RGBA32Float) {
			return 1u;
		}
		return 1u | 4u;
	}

	VideoMemoryInfo Device::GetVideoMemoryInfo() const {
		return {};
	}

	u32 Device::GetLiveBufferCount() const { return _impl->buffers.liveCount; }
	u32 Device::GetLiveTextureCount() const { return _impl->textures.liveCount; }
	u32 Device::GetLiveSamplerCount() const { return _impl->samplers.liveCount; }
	u32 Device::GetLivePipelineCount() const { return _impl->pipelines.liveCount; }

	void Device::WaitIdle() {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		if (impl->lost || !impl->queue) {
			return;
		}
		impl->FlushCPUToGPUBuffers();

		if (!WebGPUPlatform::SupportsWaitAny()) {
			impl->wgpu.wgpuInstanceProcessEvents(impl->instance);
			for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
				impl->DrainFrameReadbacks(impl->frames[i], false);
			}
			return;
		}

		bool done = false;
		WGPUQueueWorkDoneCallbackInfo cb{};
		cb.mode = WebGPUPlatform::SupportsWaitAny() ? WGPUCallbackMode_WaitAnyOnly : WGPUCallbackMode_AllowProcessEvents;
		cb.callback = &OnQueueWorkDone;
		cb.userdata1 = &done;
		WGPUFuture future = impl->wgpu.wgpuQueueOnSubmittedWorkDone(impl->queue, cb);
		impl->WaitFuture(future);

		for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
			impl->DrainFrameReadbacks(impl->frames[i], true);
		}
	}

	void Device::LoadPipelineCache(std::span<const u8>) {}

	size_t Device::SavePipelineCache(std::span<u8>) const {
		return 0;
	}

	bool Device::IsLost() const {
		return _impl->lost;
	}

	u32 Device::GetFailedRebuildCount() const {
		return _impl->failedRebuilds;
	}

	void Device::SimulateLoss(u32 failedRebuilds) {
		SLT_RHI_ASSERT_MAIN_THREAD(_impl);
		_impl->simulatedFailures = failedRebuilds;
		_impl->MarkLost(nullptr);
	}

	bool Device::Recover() {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		SLT_CORE_ASSERT_MSG(!impl->inFrame, "Recover called inside a frame");
		if (!impl->lost) {
			return true;
		}
		const auto started = std::chrono::steady_clock::now();
		if (started < impl->nextRebuild) {
			return false;
		}

		impl->ReleaseObjects();
		impl->DestroyCore();
		impl->lost = false;
		const auto lostAt = impl->lostAt;
		bool rebuilt = false;
		if (impl->simulatedFailures > 0) {
			--impl->simulatedFailures;
			impl->rebuildFailure = "simulated";
		} else {
			rebuilt = impl->CreateCore(true) && impl->RestoreObjects();
		}
		if (!rebuilt) {
			impl->ReleaseObjects();
			impl->DestroyCore();
			impl->lost = true;
			impl->lostAt = lostAt;
			impl->nextRebuild = started + std::chrono::milliseconds(REBUILD_RETRY_MS);
			++impl->failedRebuilds;
			return false;
		}
		impl->failedRebuilds = 0;
		impl->deadPipelineWarned = false;
		return true;
	}

	CommandList* Device::BeginFrame() {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		SLT_CORE_ASSERT_MSG(!impl->inFrame, "BeginFrame called twice without EndFrame");

		FrameContext& frame = impl->frames[impl->frameIndex];
		if (!impl->lost) {
			if (frame.hasWorkDoneFuture) {
				impl->WaitFuture(frame.workDoneFuture);
				frame.hasWorkDoneFuture = false;
			}
			impl->DrainFrameReadbacks(frame, WebGPUPlatform::SupportsWaitAny());
			for (WGPUBindGroup bg : frame.bindGroups.ownedGroups) {
				if (bg) {
					impl->wgpu.wgpuBindGroupRelease(bg);
				}
			}
			frame.bindGroups.ownedGroups.clear();
			frame.bindGroups.Reset();
			impl->FlushDeletionQueue(frame.deletionQueue);

			WGPUCommandEncoderDescriptor encDesc{};
			frame.commandListImpl.encoder = impl->wgpu.wgpuDeviceCreateCommandEncoder(impl->device, &encDesc);
			impl->EnsureFrameSet3BindGroups(frame);
		}

		frame.transientUsed = BINDING_OFFSET_ALIGNMENT;
		frame.pushUsed = 0;

		CommandListImpl& cmd = frame.commandListImpl;
		cmd.boundPipelineIndex = 0;
		cmd.boundRenderPipeline = nullptr;
		cmd.inRenderPass = false;
		cmd.pipelineRefused = false;
		cmd.frontFace = FrontFace::Clockwise;
		cmd.depthBias = 0;
		cmd.depthBiasSlope = 0.0f;
		cmd.currentPushOffset = 0;
		cmd.set3Bound = false;
		cmd.set3BoundReadWrite = false;

		impl->inFrame = true;
		return &frame.commandList;
	}

	void Device::EndFrame() {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		SLT_CORE_ASSERT_MSG(impl->inFrame, "EndFrame without BeginFrame");

		FrameContext& frame = impl->frames[impl->frameIndex];
		SLT_CORE_ASSERT_MSG(!frame.commandListImpl.inRenderPass, "EndFrame inside an open render pass");

		if (impl->lost) {
			impl->inFrame = false;
			return;
		}

		impl->CloseActiveComputePass(frame.commandListImpl);

		// Flush CPU staging buffers before queue submit so commands see all written data
		if (frame.transientUsed > BINDING_OFFSET_ALIGNMENT && frame.transientBuffer) {
			const u64 bytes = AlignUp(frame.transientUsed, 4);
			impl->wgpu.wgpuQueueWriteBuffer(impl->queue, frame.transientBuffer, 0, frame.transientCPU.data(), bytes);
		}
		if (frame.pushUsed > 0 && frame.pushBuffer) {
			const u64 bytes = AlignUp(frame.pushUsed, 4);
			impl->wgpu.wgpuQueueWriteBuffer(impl->queue, frame.pushBuffer, 0, frame.pushCPU.data(), bytes);
		}
		impl->FlushCPUToGPUBuffers();

		WGPUCommandBufferDescriptor cbDesc{};
		WGPUCommandBuffer cb = impl->wgpu.wgpuCommandEncoderFinish(frame.commandListImpl.encoder, &cbDesc);
		impl->wgpu.wgpuCommandEncoderRelease(frame.commandListImpl.encoder);
		frame.commandListImpl.encoder = nullptr;

		impl->wgpu.wgpuQueueSubmit(impl->queue, 1, &cb);
		impl->wgpu.wgpuCommandBufferRelease(cb);

		// Kick off async map for any readbacks recorded this frame
		for (std::shared_ptr<PendingReadback>& pending : frame.pendingReadbacks) {
			if (!pending || !pending->staging || pending->submitted) {
				continue;
			}
			pending->submitted = true;
			const size_t mapSize = static_cast<size_t>(pending->paddedRowBytes) * pending->rowCount;
			auto* box = new std::shared_ptr<PendingReadback>(pending);
			WGPUBufferMapCallbackInfo mapCb{};
			mapCb.mode = WebGPUPlatform::SupportsWaitAny() ? WGPUCallbackMode_WaitAnyOnly : WGPUCallbackMode_AllowProcessEvents;
			mapCb.callback = &OnBufferMapped;
			mapCb.userdata1 = box;
			pending->future = impl->wgpu.wgpuBufferMapAsync(pending->staging, WGPUMapMode_Read, 0, mapSize, mapCb);
		}

		if (WebGPUPlatform::SupportsWaitAny()) {
			WGPUQueueWorkDoneCallbackInfo doneCb{};
			doneCb.mode = WGPUCallbackMode_WaitAnyOnly;
			doneCb.callback = &OnQueueWorkDone;
			frame.workDoneFuture = impl->wgpu.wgpuQueueOnSubmittedWorkDone(impl->queue, doneCb);
			frame.hasWorkDoneFuture = true;
		} else {
			impl->wgpu.wgpuInstanceProcessEvents(impl->instance);
		}

		impl->inFrame = false;
		impl->frameIndex = (impl->frameIndex + 1) % FRAMES_IN_FLIGHT;
	}

	bool Device::IsInFrame() const {
		return _impl->inFrame;
	}

	TransientAllocation Device::AllocateTransient(u64 size, u64 alignment) {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		SLT_CORE_ASSERT_MSG(impl->inFrame, "AllocateTransient outside a frame");
		SLT_CORE_ASSERT(size > 0 && IsPowerOfTwo(alignment));

		FrameContext& frame = impl->frames[impl->frameIndex];
		const u64 offset = AlignUp(frame.transientUsed, alignment);
		if (offset + size > frame.transientCapacity) {
			SLT_LOG_ERROR("Graphics", "WebGPU: transient ring exhausted ({} + {} > {} B)", offset, size, frame.transientCapacity);
			return {};
		}
		frame.transientUsed = offset + size;

		TransientAllocation alloc;
		alloc.buffer = frame.transientHandle;
		alloc.offset = offset;
		alloc.cpu = frame.transientCPU.data() + offset;
		return alloc;
	}

} // namespace Shalltear::RHI
