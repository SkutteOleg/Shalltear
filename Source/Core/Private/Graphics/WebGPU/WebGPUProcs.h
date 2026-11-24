#pragma once

#include "Shalltear/Core/Types.h"

#include <webgpu/webgpu.h>

#include <cstring>
#include <string_view>

namespace Shalltear::RHI {

#define SLT_WGPU_FUNCS(X)                                                                             \
	X(CreateInstance, wgpuCreateInstance)                                                             \
	X(InstanceRelease, wgpuInstanceRelease)                                                           \
	X(InstanceProcessEvents, wgpuInstanceProcessEvents)                                               \
	X(InstanceWaitAny, wgpuInstanceWaitAny)                                                           \
	X(InstanceRequestAdapter, wgpuInstanceRequestAdapter)                                             \
	X(InstanceCreateSurface, wgpuInstanceCreateSurface)                                               \
	X(AdapterRelease, wgpuAdapterRelease)                                                             \
	X(AdapterGetInfo, wgpuAdapterGetInfo)                                                             \
	X(AdapterInfoFreeMembers, wgpuAdapterInfoFreeMembers)                                             \
	X(AdapterGetLimits, wgpuAdapterGetLimits)                                                         \
	X(AdapterHasFeature, wgpuAdapterHasFeature)                                                       \
	X(AdapterRequestDevice, wgpuAdapterRequestDevice)                                                 \
	X(DeviceRelease, wgpuDeviceRelease)                                                               \
	X(DeviceDestroy, wgpuDeviceDestroy)                                                               \
	X(DeviceGetQueue, wgpuDeviceGetQueue)                                                             \
	X(DeviceGetLimits, wgpuDeviceGetLimits)                                                           \
	X(DeviceHasFeature, wgpuDeviceHasFeature)                                                         \
	X(DeviceCreateBuffer, wgpuDeviceCreateBuffer)                                                     \
	X(DeviceCreateTexture, wgpuDeviceCreateTexture)                                                   \
	X(DeviceCreateSampler, wgpuDeviceCreateSampler)                                                   \
	X(DeviceCreateBindGroupLayout, wgpuDeviceCreateBindGroupLayout)                                   \
	X(DeviceCreateBindGroup, wgpuDeviceCreateBindGroup)                                               \
	X(DeviceCreatePipelineLayout, wgpuDeviceCreatePipelineLayout)                                     \
	X(DeviceCreateShaderModule, wgpuDeviceCreateShaderModule)                                         \
	X(DeviceCreateRenderPipeline, wgpuDeviceCreateRenderPipeline)                                     \
	X(DeviceCreateComputePipeline, wgpuDeviceCreateComputePipeline)                                   \
	X(DeviceCreateCommandEncoder, wgpuDeviceCreateCommandEncoder)                                     \
	X(QueueRelease, wgpuQueueRelease)                                                                 \
	X(QueueSubmit, wgpuQueueSubmit)                                                                   \
	X(QueueWriteBuffer, wgpuQueueWriteBuffer)                                                         \
	X(QueueWriteTexture, wgpuQueueWriteTexture)                                                       \
	X(QueueOnSubmittedWorkDone, wgpuQueueOnSubmittedWorkDone)                                         \
	X(BufferRelease, wgpuBufferRelease)                                                               \
	X(BufferDestroy, wgpuBufferDestroy)                                                               \
	X(BufferMapAsync, wgpuBufferMapAsync)                                                             \
	X(BufferGetConstMappedRange, wgpuBufferGetConstMappedRange)                                       \
	X(BufferGetMappedRange, wgpuBufferGetMappedRange)                                                 \
	X(BufferUnmap, wgpuBufferUnmap)                                                                   \
	X(TextureRelease, wgpuTextureRelease)                                                             \
	X(TextureDestroy, wgpuTextureDestroy)                                                             \
	X(TextureCreateView, wgpuTextureCreateView)                                                       \
	X(TextureViewRelease, wgpuTextureViewRelease)                                                     \
	X(SamplerRelease, wgpuSamplerRelease)                                                             \
	X(BindGroupLayoutRelease, wgpuBindGroupLayoutRelease)                                             \
	X(BindGroupRelease, wgpuBindGroupRelease)                                                         \
	X(PipelineLayoutRelease, wgpuPipelineLayoutRelease)                                               \
	X(ShaderModuleRelease, wgpuShaderModuleRelease)                                                   \
	X(RenderPipelineRelease, wgpuRenderPipelineRelease)                                               \
	X(ComputePipelineRelease, wgpuComputePipelineRelease)                                             \
	X(CommandEncoderRelease, wgpuCommandEncoderRelease)                                               \
	X(CommandEncoderBeginRenderPass, wgpuCommandEncoderBeginRenderPass)                               \
	X(CommandEncoderBeginComputePass, wgpuCommandEncoderBeginComputePass)                             \
	X(CommandEncoderCopyBufferToBuffer, wgpuCommandEncoderCopyBufferToBuffer)                         \
	X(CommandEncoderCopyBufferToTexture, wgpuCommandEncoderCopyBufferToTexture)                       \
	X(CommandEncoderCopyTextureToBuffer, wgpuCommandEncoderCopyTextureToBuffer)                       \
	X(CommandEncoderCopyTextureToTexture, wgpuCommandEncoderCopyTextureToTexture)                     \
	X(CommandEncoderInsertDebugMarker, wgpuCommandEncoderInsertDebugMarker)                           \
	X(CommandEncoderFinish, wgpuCommandEncoderFinish)                                                 \
	X(CommandBufferRelease, wgpuCommandBufferRelease)                                                 \
	X(RenderPassEncoderRelease, wgpuRenderPassEncoderRelease)                                         \
	X(RenderPassEncoderSetPipeline, wgpuRenderPassEncoderSetPipeline)                                 \
	X(RenderPassEncoderSetBindGroup, wgpuRenderPassEncoderSetBindGroup)                               \
	X(RenderPassEncoderSetViewport, wgpuRenderPassEncoderSetViewport)                                 \
	X(RenderPassEncoderSetScissorRect, wgpuRenderPassEncoderSetScissorRect)                           \
	X(RenderPassEncoderSetIndexBuffer, wgpuRenderPassEncoderSetIndexBuffer)                           \
	X(RenderPassEncoderDraw, wgpuRenderPassEncoderDraw)                                               \
	X(RenderPassEncoderDrawIndexed, wgpuRenderPassEncoderDrawIndexed)                                 \
	X(RenderPassEncoderDrawIndirect, wgpuRenderPassEncoderDrawIndirect)                               \
	X(RenderPassEncoderEnd, wgpuRenderPassEncoderEnd)                                                 \
	X(ComputePassEncoderRelease, wgpuComputePassEncoderRelease)                                       \
	X(ComputePassEncoderSetPipeline, wgpuComputePassEncoderSetPipeline)                               \
	X(ComputePassEncoderSetBindGroup, wgpuComputePassEncoderSetBindGroup)                             \
	X(ComputePassEncoderDispatchWorkgroups, wgpuComputePassEncoderDispatchWorkgroups)                 \
	X(ComputePassEncoderDispatchWorkgroupsIndirect, wgpuComputePassEncoderDispatchWorkgroupsIndirect) \
	X(ComputePassEncoderEnd, wgpuComputePassEncoderEnd)                                               \
	X(SurfaceRelease, wgpuSurfaceRelease)                                                             \
	X(SurfaceConfigure, wgpuSurfaceConfigure)                                                         \
	X(SurfaceUnconfigure, wgpuSurfaceUnconfigure)                                                     \
	X(SurfaceGetCapabilities, wgpuSurfaceGetCapabilities)                                             \
	X(SurfaceCapabilitiesFreeMembers, wgpuSurfaceCapabilitiesFreeMembers)                             \
	X(SurfaceGetCurrentTexture, wgpuSurfaceGetCurrentTexture)                                         \
	X(SurfacePresent, wgpuSurfacePresent)

	struct WebGPUProcs {
#define SLT_WGPU_MEMBER(PascalName, SymbolName) WGPUProc##PascalName SymbolName = nullptr;
		SLT_WGPU_FUNCS(SLT_WGPU_MEMBER)
#undef SLT_WGPU_MEMBER
	};

	inline WGPUStringView ToWGPUString(const char* text) {
		if (!text) {
			return {nullptr, 0};
		}
		return {text, std::strlen(text)};
	}

	inline WGPUStringView ToWGPUString(std::string_view text) {
		return {text.data(), text.size()};
	}

} // namespace Shalltear::RHI
