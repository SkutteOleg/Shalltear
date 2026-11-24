#include "Graphics/WebGPU/WebGPUInternal.h"

#include "Graphics/WebGPU/WebGPUBCDecode.h"
#include "Graphics/WebGPU/WebGPUPlatform.h"

#include "Shalltear/Core/Graphics/TextureCodecs.h"

#include <algorithm>
#include <cstring>

namespace Shalltear::RHI {

	namespace {

		WGPUBufferUsage ToWGPUBufferUsage(BufferUsage usage, MemoryAccess access) {
			WGPUBufferUsage flags = WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc;
			if (HasFlag(usage, BufferUsage::Vertex) || HasFlag(usage, BufferUsage::Storage)) {
				flags |= WGPUBufferUsage_Storage;
			}
			if (HasFlag(usage, BufferUsage::Index)) {
				flags |= WGPUBufferUsage_Index;
			}
			if (HasFlag(usage, BufferUsage::Uniform)) {
				flags |= WGPUBufferUsage_Uniform;
			}
			if (HasFlag(usage, BufferUsage::Indirect)) {
				flags |= WGPUBufferUsage_Indirect;
			}
			(void)access;
			return flags;
		}

		WGPUAddressMode ToWGPUAddressMode(AddressMode mode) {
			switch (mode) {
			case AddressMode::ClampToEdge:
				return WGPUAddressMode_ClampToEdge;
			case AddressMode::MirroredRepeat:
				return WGPUAddressMode_MirrorRepeat;
			case AddressMode::Repeat:
			default:
				return WGPUAddressMode_Repeat;
			}
		}

		WGPUTextureViewDimension ToWGPUViewDimension(TextureDimension dim) {
			switch (dim) {
			case TextureDimension::Texture2DArray:
				return WGPUTextureViewDimension_2DArray;
			case TextureDimension::Texture3D:
				return WGPUTextureViewDimension_3D;
			case TextureDimension::Cube:
				return WGPUTextureViewDimension_Cube;
			case TextureDimension::CubeArray:
				return WGPUTextureViewDimension_CubeArray;
			case TextureDimension::Texture2D:
			case TextureDimension::Count:
			default:
				return WGPUTextureViewDimension_2D;
			}
		}

		u32 FormatRowBytes(Format format, u32 width) {
			switch (format) {
			case Format::BC5Unorm:
			case Format::BC7Unorm:
			case Format::BC7SRGB:
			case Format::BC6HUFloat:
				return ((width + 3u) / 4u) * 16u;
			case Format::R8Unorm:
				return width;
			case Format::RGBA8Unorm:
			case Format::RGBA8SRGB:
			case Format::BGRA8Unorm:
			case Format::BGRA8SRGB:
			case Format::RG16Float:
			case Format::R32Uint:
			case Format::D32Float:
				return width * 4u;
			case Format::RGBA16Float:
				return width * 8u;
			case Format::RGBA32Float:
				return width * 16u;
			default:
				return 0u;
			}
		}

		u32 FormatRowsPerSlice(Format format, u32 height) {
			return IsBlockCompressed(format) ? ((height + 3u) / 4u) : height;
		}

	} // anonymous namespace

	BufferHandle Device::CreateBufferResource(const BufferDesc& desc) {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		if (desc.size == 0) {
			SLT_LOG_ERROR("Graphics", "CreateBufferResource: 0 size ('{}')", desc.debugName ? desc.debugName : "<unnamed>");
			return {};
		}

		BufferResource created;
		created.size = desc.size;
		const u64 minAlign = HasFlag(desc.usage, BufferUsage::Uniform) ? 16 : 4;
		created.gpuSize = AlignUp(std::max<u64>(desc.size, minAlign), minAlign);
		created.usage = desc.usage;
		created.access = desc.access;
		if (desc.access == MemoryAccess::CPUToGPU || desc.access == MemoryAccess::Readback) {
			created.cpuShadow = std::make_shared<Vector<u8>>(GetMemoryResource(MemTag::Graphics));
			created.cpuShadow->assign(static_cast<size_t>(created.gpuSize), 0);
		}
		if (!impl->lost && !impl->CreateBufferObject(created, desc.debugName) && !impl->lost) {
			return {};
		}

		auto [index, generation] = impl->buffers.Acquire();
		impl->buffers.slots[index].resource = std::move(created);
		return BufferHandle{index, generation};
	}

	bool DeviceImpl::CreateBufferObject(BufferResource& resource, const char* debugName) {
		if (HasFlag(resource.usage, BufferUsage::DeviceAddress)) {
			u64 alignedSize = 0;
			const u64 offset = AllocateVertexHeap(resource.gpuSize, alignedSize);
			if (offset == 0) {
				return false;
			}
			resource.inVertexHeap = true;
			resource.heapOffset = offset;
			resource.heapAlignedSize = alignedSize;
			resource.buffer = nullptr;
			resource.deviceAddress = WEBGPU_VERTEX_HEAP_TAG | offset;
			return true;
		}

		WGPUBufferDescriptor desc{};
		desc.label = ToWGPUString(debugName);
		desc.size = resource.gpuSize;
		desc.usage = ToWGPUBufferUsage(resource.usage, resource.access);
		WGPUBuffer buf = wgpu.wgpuDeviceCreateBuffer(device, &desc);
		if (!buf) {
			SLT_LOG_ERROR("Graphics", "WebGPU CreateBufferResource failed ('{}')", debugName ? debugName : "<unnamed>");
			return false;
		}
		resource.buffer = buf;
		resource.inVertexHeap = false;
		resource.deviceAddress = 0;
		return true;
	}

	void Device::DestroyResource(BufferHandle handle) {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		BufferResource* resource = impl->buffers.Resolve(handle);
		if (!resource) {
			if (handle.IsValid()) {
				SLT_LOG_WARN("Graphics", "DestroyResource: stale buffer handle ({}, gen {})", handle.index, handle.generation);
			}
			return;
		}
		DeletionQueue& queue = impl->DeletionQueueForDestroy();
		if (resource->inVertexHeap) {
			queue.vertexHeapReleases.push_back({resource->heapOffset, resource->heapAlignedSize});
		} else if (resource->buffer) {
			queue.buffers.push_back(resource->buffer);
		}
		*resource = BufferResource{};
		impl->buffers.Release(handle.index);
		++impl->resourceEpoch;
	}

	u64 Device::GetBufferDeviceAddress(BufferHandle buffer) const {
		BufferResource* resource = _impl->buffers.Resolve(buffer);
		if (!resource || resource->deviceAddress == 0) {
			if (!_impl->lost) {
				SLT_LOG_WARN("Graphics", "GetBufferDeviceAddress: invalid handle or missing DeviceAddress usage");
			}
			return 0;
		}
		return resource->deviceAddress;
	}

	void* Device::GetMappedPointer(BufferHandle buffer) const {
		BufferResource* resource = _impl->buffers.Resolve(buffer);
		if (!resource || !resource->cpuShadow) {
			return nullptr;
		}
		if (resource->access == MemoryAccess::CPUToGPU) {
			resource->cpuDirty = true;
		}
		return resource->cpuShadow->data();
	}

	TextureHandle Device::CreateTextureResource(const TextureDesc& desc) {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		if (desc.size.x <= 0 || desc.size.y <= 0 || desc.format == Format::Unknown) {
			SLT_LOG_ERROR("Graphics", "CreateTextureResource: invalid desc ('{}')", desc.debugName ? desc.debugName : "<unnamed>");
			return {};
		}

		const TextureDimension dimension = ResolveTextureDimension(desc);
		if (desc.dimension) {
			const char* contradiction = nullptr;
			if (desc.cube && dimension != TextureDimension::Cube && dimension != TextureDimension::CubeArray) {
				contradiction = "its cube flag";
			} else if (desc.cubeArray && dimension != TextureDimension::CubeArray) {
				contradiction = "its cube-array flag";
			} else if (desc.depth > 1 && dimension != TextureDimension::Texture3D) {
				contradiction = "its depth";
			} else if (desc.arrayLayers > 1 && (dimension == TextureDimension::Texture2D || dimension == TextureDimension::Texture3D)) {
				contradiction = "its layers";
			}
			if (contradiction) {
				SLT_LOG_ERROR("Graphics", "CreateTextureResource: a {} texture contradicts {} ('{}')", TextureDimensionName(dimension),
							  contradiction, desc.debugName ? desc.debugName : "<unnamed>");
				return {};
			}
		}

		const bool cube = dimension == TextureDimension::Cube || dimension == TextureDimension::CubeArray;
		const bool volume = dimension == TextureDimension::Texture3D;
		if (desc.depth > 1 && !volume) {
			return {};
		}
		const bool cubeArray = dimension == TextureDimension::CubeArray;
		u32 arrayLayers = cube ? (cubeArray ? std::max(6u, desc.arrayLayers) : 6u) : std::max(1u, desc.arrayLayers);
		if (cubeArray && arrayLayers % 6 != 0) {
			arrayLayers = ((arrayLayers + 5u) / 6u) * 6u;
		}
		const u32 depth = std::max(1u, desc.depth);
		if (volume && (arrayLayers > 1 || desc.samples > 1 || desc.mipLevels > 1 ||
					   HasFlag(desc.usage, TextureUsage::RenderTarget) || HasFlag(desc.usage, TextureUsage::DepthStencil))) {
			return {};
		}
		if (IsBlockCompressed(desc.format) &&
			(HasFlag(desc.usage, TextureUsage::RenderTarget) || HasFlag(desc.usage, TextureUsage::DepthStencil) || desc.samples > 1 || volume)) {
			return {};
		}
		const u32 samples = std::max(1u, desc.samples);
		if (samples > 1 && (desc.mipLevels > 1 || arrayLayers > 1)) {
			return {};
		}

		TextureResource created;
		created.size = desc.size;
		created.format = desc.format;
		if (IsBlockCompressed(desc.format) && !impl->textureCompressionBC) {
			created.gpuFormat = TextureCodecs::DecodedFormat(desc.format);
			created.decodedFallback = true;
		} else {
			created.gpuFormat = desc.format;
			created.decodedFallback = false;
		}
		created.usage = desc.usage;
		created.mipLevels = std::max(1u, desc.mipLevels);
		created.samples = samples;
		created.arrayLayers = arrayLayers;
		created.depth = depth;
		created.dimension = dimension;
		if (!impl->lost && !impl->CreateTextureObjects(created, desc.debugName) && !impl->lost) {
			return {};
		}

		auto [index, generation] = impl->textures.Acquire();
		impl->textures.slots[index].resource = std::move(created);
		return TextureHandle{index, generation};
	}

	bool DeviceImpl::CreateTextureObjects(TextureResource& resource, const char* debugName) {
		WGPUTextureDescriptor texDesc{};
		texDesc.label = ToWGPUString(debugName);
		texDesc.dimension = resource.dimension == TextureDimension::Texture3D ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
		texDesc.size = {static_cast<u32>(resource.size.x), static_cast<u32>(resource.size.y),
						resource.dimension == TextureDimension::Texture3D ? resource.depth : resource.arrayLayers};
		texDesc.format = ToWGPUFormat(resource.gpuFormat);
		texDesc.mipLevelCount = resource.mipLevels;
		texDesc.sampleCount = resource.samples;
		texDesc.usage = ToWGPUTextureUsage(resource.usage, resource.gpuFormat, resource.samples);

		WGPUTexture texture = wgpu.wgpuDeviceCreateTexture(device, &texDesc);
		if (!texture) {
			SLT_LOG_ERROR("Graphics", "WebGPU CreateTextureResource failed ('{}')", debugName ? debugName : "<unnamed>");
			return false;
		}

		const WGPUTextureAspect aspect = IsDepthFormat(resource.gpuFormat) ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All;

		WGPUTextureViewDescriptor viewDesc{};
		viewDesc.format = texDesc.format;
		viewDesc.dimension = ToWGPUViewDimension(resource.dimension);
		viewDesc.baseMipLevel = 0;
		viewDesc.mipLevelCount = resource.mipLevels;
		viewDesc.baseArrayLayer = 0;
		viewDesc.arrayLayerCount = resource.dimension == TextureDimension::Texture3D ? 1u : resource.arrayLayers;
		viewDesc.aspect = aspect;
		WGPUTextureView view = wgpu.wgpuTextureCreateView(texture, &viewDesc);
		if (!view) {
			wgpu.wgpuTextureDestroy(texture);
			wgpu.wgpuTextureRelease(texture);
			return false;
		}

		const bool needLevelViews = resource.dimension != TextureDimension::Texture3D &&
									(resource.arrayLayers > 1 || resource.mipLevels > 1);
		Vector<WGPUTextureView> layerViews{GetMemoryResource(MemTag::Graphics)};
		if (needLevelViews) {
			layerViews.reserve(static_cast<size_t>(resource.mipLevels) * resource.arrayLayers);
			for (u32 level = 0; level < resource.mipLevels; ++level) {
				for (u32 layer = 0; layer < resource.arrayLayers; ++layer) {
					WGPUTextureViewDescriptor lvDesc{};
					lvDesc.format = texDesc.format;
					lvDesc.dimension = WGPUTextureViewDimension_2D;
					lvDesc.baseMipLevel = level;
					lvDesc.mipLevelCount = 1;
					lvDesc.baseArrayLayer = layer;
					lvDesc.arrayLayerCount = 1;
					lvDesc.aspect = aspect;
					layerViews.push_back(wgpu.wgpuTextureCreateView(texture, &lvDesc));
				}
			}
		}

		resource.texture = texture;
		resource.view = view;
		resource.layerViews.assign(layerViews.begin(), layerViews.end());
		return true;
	}

	void Device::DestroyResource(TextureHandle handle) {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		TextureResource* resource = impl->textures.Resolve(handle);
		if (!resource) {
			if (handle.IsValid()) {
				SLT_LOG_WARN("Graphics", "DestroyResource: stale texture handle ({}, gen {})", handle.index, handle.generation);
			}
			return;
		}
		DeletionQueue& queue = impl->DeletionQueueForDestroy();
		if (resource->tableIndex != TABLE_NONE) {
			queue.atlasReleases.push_back({resource->dimension == TextureDimension::Cube, resource->tableIndex});
		}
		if (resource->view) {
			queue.textureViews.push_back(resource->view);
		}
		for (WGPUTextureView lv : resource->layerViews) {
			if (lv) {
				queue.textureViews.push_back(lv);
			}
		}
		if (!resource->external && resource->texture) {
			queue.textures.push_back(resource->texture);
		}
		const bool wasExternal = resource->external;
		*resource = TextureResource{};
		impl->textures.Release(handle.index);
		if (!wasExternal) {
			++impl->resourceEpoch;
		}
	}

	bool Device::IsAlive(TextureHandle texture) const {
		return _impl->textures.Resolve(texture) != nullptr;
	}

	Vec2I Device::GetTextureSize(TextureHandle texture) const {
		TextureResource* resource = _impl->textures.Resolve(texture);
		return resource ? resource->size : Vec2I{};
	}

	Format Device::GetTextureFormat(TextureHandle texture) const {
		TextureResource* resource = _impl->textures.Resolve(texture);
		return resource ? resource->format : Format::Unknown;
	}

	TextureUsage Device::GetTextureUsage(TextureHandle texture) const {
		TextureResource* resource = _impl->textures.Resolve(texture);
		return resource ? resource->usage : TextureUsage::None;
	}

	ResourceState Device::GetTextureState(TextureHandle texture) const {
		TextureResource* resource = _impl->textures.Resolve(texture);
		return resource ? resource->state : ResourceState::Undefined;
	}

	SamplerHandle Device::CreateSamplerResource(const SamplerDesc& desc) {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);

		SamplerResource created;
		created.desc = desc;
		created.desc.debugName = nullptr;
		if (!impl->lost && !impl->CreateSamplerObject(created, desc.debugName) && !impl->lost) {
			return {};
		}

		auto [index, generation] = impl->samplers.Acquire();
		impl->samplers.slots[index].resource = created;
		return SamplerHandle{index, generation};
	}

	bool DeviceImpl::CreateSamplerObject(SamplerResource& resource, const char* debugName) {
		const SamplerDesc& desc = resource.desc;
		const WGPUAddressMode address = ToWGPUAddressMode(desc.addressMode);
		const WGPUFilterMode filter = desc.filter == FilterMode::Linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
		const WGPUMipmapFilterMode mipFilter =
			desc.mipFilter == FilterMode::Linear ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;

		WGPUSamplerDescriptor sDesc{};
		sDesc.label = ToWGPUString(debugName);
		sDesc.addressModeU = address;
		sDesc.addressModeV = address;
		sDesc.addressModeW = address;
		sDesc.magFilter = filter;
		sDesc.minFilter = filter;
		sDesc.mipmapFilter = mipFilter;
		sDesc.lodMinClamp = 0.0f;
		sDesc.lodMaxClamp = 32.0f;
		sDesc.compare = desc.comparison ? WGPUCompareFunction_GreaterEqual : WGPUCompareFunction_Undefined;
		// WebGPU forbids maxAnisotropy > 1 unless all filters are linear
		const bool canAniso = desc.filter == FilterMode::Linear && desc.mipFilter == FilterMode::Linear;
		sDesc.maxAnisotropy = canAniso ? std::max<u16>(1u, static_cast<u16>(std::min<u8>(desc.maxAnisotropy, 16))) : 1u;

		WGPUSampler sampler = wgpu.wgpuDeviceCreateSampler(device, &sDesc);
		if (!sampler) {
			SLT_LOG_ERROR("Graphics", "WebGPU CreateSamplerResource failed");
			return false;
		}
		resource.sampler = sampler;
		return true;
	}

	void Device::DestroyResource(SamplerHandle handle) {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		SamplerResource* resource = impl->samplers.Resolve(handle);
		if (!resource) {
			if (handle.IsValid()) {
				SLT_LOG_WARN("Graphics", "DestroyResource: stale sampler handle ({}, gen {})", handle.index, handle.generation);
			}
			return;
		}
		DeletionQueue& queue = impl->DeletionQueueForDestroy();
		if (resource->sampler) {
			queue.samplers.push_back(resource->sampler);
		}
		*resource = SamplerResource{};
		impl->samplers.Release(handle.index);
		++impl->resourceEpoch;
	}

	void Device::DestroyResource(PipelineHandle handle) {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		PipelineResource* resource = impl->pipelines.Resolve(handle);
		if (!resource) {
			if (handle.IsValid()) {
				SLT_LOG_WARN("Graphics", "DestroyResource: stale pipeline handle ({}, gen {})", handle.index, handle.generation);
			}
			return;
		}
		DeletionQueue& queue = impl->DeletionQueueForDestroy();
		for (GraphicsPipelineVariant& v : resource->variants) {
			if (v.pipeline) {
				queue.renderPipelines.push_back(v.pipeline);
			}
		}
		if (resource->computePipeline) {
			queue.computePipelines.push_back(resource->computePipeline);
		}
		if (resource->layout) {
			queue.pipelineLayouts.push_back(resource->layout);
		}
		if (resource->vertexModule) {
			queue.shaderModules.push_back(resource->vertexModule);
		}
		if (resource->pixelModule) {
			queue.shaderModules.push_back(resource->pixelModule);
		}
		*resource = PipelineResource{};
		impl->pipelines.Release(handle.index);
		++impl->resourceEpoch;
	}

	void Device::UploadBuffer(BufferHandle target, std::span<const u8> data, u64 offset) {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		BufferResource* resource = impl->buffers.Resolve(target);
		if (!resource || data.empty()) {
			return;
		}
		if (offset + data.size() > resource->size) {
			SLT_LOG_ERROR("Graphics", "UploadBuffer: range {}..{} exceeds buffer size {}", offset, offset + data.size(), resource->size);
			return;
		}
		if (resource->cpuShadow) {
			std::memcpy(resource->cpuShadow->data() + offset, data.data(), data.size());
		}
		if (impl->lost) {
			return;
		}

		WGPUBuffer dst = resource->inVertexHeap ? impl->vertexHeap.buffer : resource->buffer;
		const u64 base = resource->inVertexHeap ? resource->heapOffset : 0;
		if (!dst) {
			return;
		}

		if ((offset % 4) == 0 && (data.size() % 4) == 0) {
			impl->wgpu.wgpuQueueWriteBuffer(impl->queue, dst, base + offset, data.data(), data.size());
		} else {
			const u64 alignedOffset = offset & ~3ull;
			const u64 prefix = offset - alignedOffset;
			const u64 alignedSize = AlignUp(prefix + data.size(), 4);
			Vector<u8> padded{GetMemoryResource(MemTag::Graphics)};
			padded.assign(static_cast<size_t>(alignedSize), 0);
			std::memcpy(padded.data() + prefix, data.data(), data.size());
			impl->wgpu.wgpuQueueWriteBuffer(impl->queue, dst, base + alignedOffset, padded.data(), padded.size());
		}
	}

	void Device::UploadTexture(TextureHandle target, std::span<const u8> data, u32 mipLevel, u32 layer) {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		TextureResource* resource = impl->textures.Resolve(target);
		if (!resource || data.empty()) {
			return;
		}
		if (mipLevel >= resource->mipLevels || layer >= resource->arrayLayers) {
			SLT_LOG_ERROR("Graphics", "UploadTexture: mip {} layer {} out of range", mipLevel, layer);
			return;
		}

		const u32 mipWidth = static_cast<u32>(std::max(1, resource->size.x >> mipLevel));
		const u32 mipHeight = static_cast<u32>(std::max(1, resource->size.y >> mipLevel));
		const u32 mipDepth = resource->depth;
		const u64 expected = FormatLevelBytes(resource->format, Vec2I{static_cast<i32>(mipWidth), static_cast<i32>(mipHeight)}) * mipDepth;
		if (data.size() != expected) {
			SLT_LOG_ERROR("Graphics", "UploadTexture: {} bytes provided, mip {} expects {}", data.size(), mipLevel, expected);
			return;
		}
		if (impl->lost || !resource->texture) {
			resource->state = ResourceState::ShaderResource;
			return;
		}

		Vector<u8> decoded{GetMemoryResource(MemTag::Graphics)};
		std::span<const u8> uploadSpan = data;
		if (resource->decodedFallback) {
			const u64 decodedBytes = FormatLevelBytes(resource->gpuFormat, Vec2I{static_cast<i32>(mipWidth), static_cast<i32>(mipHeight)});
			decoded.resize(static_cast<size_t>(decodedBytes));
			if (!WebGPU::DecodeBlockCompressedLevel(resource->format, data, Vec2I{static_cast<i32>(mipWidth), static_cast<i32>(mipHeight)},
													{decoded.data(), decoded.size()})) {
				SLT_LOG_ERROR("Graphics", "WebGPU UploadTexture: CPU BC decode failed");
				return;
			}
			uploadSpan = {decoded.data(), decoded.size()};
		}

		const u32 bytesPerRow = FormatRowBytes(resource->gpuFormat, mipWidth);
		const u32 rowsPerImage = FormatRowsPerSlice(resource->gpuFormat, mipHeight);
		const u32 copyWidth = IsBlockCompressed(resource->gpuFormat) ? static_cast<u32>(AlignUp(mipWidth, 4)) : mipWidth;
		const u32 copyHeight = IsBlockCompressed(resource->gpuFormat) ? static_cast<u32>(AlignUp(mipHeight, 4)) : mipHeight;

		WGPUTexelCopyTextureInfo dst{};
		dst.texture = resource->texture;
		dst.mipLevel = mipLevel;
		dst.origin = {0, 0, resource->dimension == TextureDimension::Texture3D ? 0u : layer};
		dst.aspect = IsDepthFormat(resource->gpuFormat) ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All;

		WGPUTexelCopyBufferLayout layout{};
		layout.offset = 0;
		layout.bytesPerRow = bytesPerRow;
		layout.rowsPerImage = rowsPerImage;

		WGPUExtent3D extent{copyWidth, copyHeight, mipDepth};
		impl->wgpu.wgpuQueueWriteTexture(impl->queue, &dst, uploadSpan.data(), uploadSpan.size(), &layout, &extent);
		resource->state = ResourceState::ShaderResource;

		// Keep atlas mirror up-to-date if the texture was already registered in Set 3
		if (resource->tableIndex != TABLE_NONE && mipLevel == 0) {
			if (resource->dimension == TextureDimension::Texture2D) {
				impl->BlitTextureToAtlas2D(*resource, resource->tableIndex);
			} else if (resource->dimension == TextureDimension::Cube && layer == 5) {
				impl->BlitCubeToAtlasCube(*resource, resource->tableIndex);
			}
		}
	}

	void Device::UploadTextureRegion(TextureHandle target, std::span<const u8> data, RectI region, u32 mipLevel) {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		TextureResource* resource = impl->textures.Resolve(target);
		if (!resource || data.empty() || mipLevel >= resource->mipLevels || resource->depth > 1 || IsBlockCompressed(resource->format)) {
			return;
		}
		const i32 mipWidth = std::max(1, resource->size.x >> mipLevel);
		const i32 mipHeight = std::max(1, resource->size.y >> mipLevel);
		if (region.size.x <= 0 || region.size.y <= 0 || region.position.x < 0 || region.position.y < 0 ||
			region.position.x + region.size.x > mipWidth || region.position.y + region.size.y > mipHeight) {
			return;
		}
		if (data.size() != FormatLevelBytes(resource->format, region.size)) {
			return;
		}
		if (impl->lost || !resource->texture) {
			resource->state = ResourceState::ShaderResource;
			return;
		}

		WGPUTexelCopyTextureInfo dst{};
		dst.texture = resource->texture;
		dst.mipLevel = mipLevel;
		dst.origin = {static_cast<u32>(region.position.x), static_cast<u32>(region.position.y), 0};
		dst.aspect = IsDepthFormat(resource->gpuFormat) ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All;

		WGPUTexelCopyBufferLayout layout{};
		layout.offset = 0;
		layout.bytesPerRow = FormatRowBytes(resource->gpuFormat, static_cast<u32>(region.size.x));
		layout.rowsPerImage = static_cast<u32>(region.size.y);

		WGPUExtent3D extent{static_cast<u32>(region.size.x), static_cast<u32>(region.size.y), 1};
		impl->wgpu.wgpuQueueWriteTexture(impl->queue, &dst, data.data(), data.size(), &layout, &extent);
		resource->state = ResourceState::ShaderResource;
	}

	void DeviceImpl::CmdGenerateMips(WGPUCommandEncoder encoder, TextureResource& resource) {
		if (lost || resource.mipLevels <= 1 || !resource.texture || IsDepthFormat(resource.gpuFormat) || IsBlockCompressed(resource.gpuFormat)) {
			resource.state = ResourceState::ShaderResource;
			return;
		}
		WGPURenderPipeline blitPipe = GetOrCreateBlitPipeline(ToWGPUFormat(resource.gpuFormat));
		if (!blitPipe || !table.blitLayout || !table.blitSampler) {
			resource.state = ResourceState::ShaderResource;
			return;
		}

		for (u32 level = 1; level < resource.mipLevels; ++level) {
			for (u32 layer = 0; layer < resource.arrayLayers; ++layer) {
				WGPUTextureView srcView = resource.AttachmentView(layer, level - 1);
				WGPUTextureView dstView = resource.AttachmentView(layer, level);
				if (!srcView || !dstView) {
					continue;
				}
				WGPUBindGroupEntry entries[2]{};
				entries[0].binding = 0;
				entries[0].textureView = srcView;
				entries[1].binding = 1;
				entries[1].sampler = table.blitSampler;

				WGPUBindGroupDescriptor bgDesc{};
				bgDesc.layout = table.blitLayout;
				bgDesc.entryCount = 2;
				bgDesc.entries = entries;
				WGPUBindGroup bg = wgpu.wgpuDeviceCreateBindGroup(device, &bgDesc);

				WGPURenderPassColorAttachment color{};
				color.view = dstView;
				color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
				color.loadOp = WGPULoadOp_Clear;
				color.storeOp = WGPUStoreOp_Store;

				WGPURenderPassDescriptor passDesc{};
				passDesc.colorAttachmentCount = 1;
				passDesc.colorAttachments = &color;

				WGPURenderPassEncoder pass = wgpu.wgpuCommandEncoderBeginRenderPass(encoder, &passDesc);
				wgpu.wgpuRenderPassEncoderSetPipeline(pass, blitPipe);
				wgpu.wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
				wgpu.wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
				wgpu.wgpuRenderPassEncoderEnd(pass);
				wgpu.wgpuRenderPassEncoderRelease(pass);
				wgpu.wgpuBindGroupRelease(bg);
			}
		}
		resource.state = ResourceState::ShaderResource;
	}

	void Device::GenerateMips(TextureHandle target) {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		TextureResource* resource = impl->textures.Resolve(target);
		if (!resource || resource->mipLevels <= 1) {
			return;
		}
		SLT_CORE_ASSERT_MSG(!IsDepthFormat(resource->format), "GenerateMips on a depth texture");
		if (IsBlockCompressed(resource->format) || impl->lost) {
			return;
		}

		WGPUCommandEncoderDescriptor encDesc{};
		WGPUCommandEncoder enc = impl->wgpu.wgpuDeviceCreateCommandEncoder(impl->device, &encDesc);
		impl->CmdGenerateMips(enc, *resource);
		WGPUCommandBufferDescriptor cbDesc{};
		WGPUCommandBuffer cb = impl->wgpu.wgpuCommandEncoderFinish(enc, &cbDesc);
		impl->wgpu.wgpuQueueSubmit(impl->queue, 1, &cb);
		impl->wgpu.wgpuCommandBufferRelease(cb);
		impl->wgpu.wgpuCommandEncoderRelease(enc);
	}

	void Device::ReadbackTexture(TextureHandle source, std::span<u8> out, u32 mipLevel, u32 layer) {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		TextureResource* resource = impl->textures.Resolve(source);
		if (!resource || layer >= resource->arrayLayers || mipLevel >= resource->mipLevels) {
			return;
		}
		const u32 mipWidth = static_cast<u32>(std::max(1, resource->size.x >> mipLevel));
		const u32 mipHeight = static_cast<u32>(std::max(1, resource->size.y >> mipLevel));
		const u32 mipDepth = resource->depth;
		const u64 expected = FormatLevelBytes(resource->gpuFormat, Vec2I{static_cast<i32>(mipWidth), static_cast<i32>(mipHeight)}) * mipDepth;
		if (out.size() < expected) {
			return;
		}
		const ResourceState after = HasFlag(resource->usage, TextureUsage::Sampled) ? ResourceState::ShaderResource : ResourceState::TransferSrc;
		if (impl->lost || !resource->texture || !WebGPUPlatform::SupportsWaitAny()) {
			std::memset(out.data(), 0, static_cast<size_t>(expected));
			resource->state = after;
			return;
		}

		const u32 tightRowBytes = FormatRowBytes(resource->gpuFormat, mipWidth);
		const u32 paddedRowBytes = static_cast<u32>(AlignUp(tightRowBytes, 256u));
		const u32 rowsPerImage = FormatRowsPerSlice(resource->gpuFormat, mipHeight);
		const u64 stagingSize = static_cast<u64>(paddedRowBytes) * rowsPerImage * mipDepth;

		WGPUBufferDescriptor stDesc{};
		stDesc.size = stagingSize;
		stDesc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
		WGPUBuffer staging = impl->wgpu.wgpuDeviceCreateBuffer(impl->device, &stDesc);
		if (!staging) {
			std::memset(out.data(), 0, static_cast<size_t>(expected));
			return;
		}

		WGPUCommandEncoderDescriptor encDesc{};
		WGPUCommandEncoder enc = impl->wgpu.wgpuDeviceCreateCommandEncoder(impl->device, &encDesc);
		WGPUTexelCopyTextureInfo srcInfo{};
		srcInfo.texture = resource->texture;
		srcInfo.mipLevel = mipLevel;
		srcInfo.origin = {0, 0, resource->dimension == TextureDimension::Texture3D ? 0u : layer};
		srcInfo.aspect = IsDepthFormat(resource->gpuFormat) ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All;

		WGPUTexelCopyBufferInfo dstInfo{};
		dstInfo.buffer = staging;
		dstInfo.layout.offset = 0;
		dstInfo.layout.bytesPerRow = paddedRowBytes;
		dstInfo.layout.rowsPerImage = rowsPerImage;

		const u32 copyWidth = IsBlockCompressed(resource->gpuFormat) ? static_cast<u32>(AlignUp(mipWidth, 4)) : mipWidth;
		const u32 copyHeight = IsBlockCompressed(resource->gpuFormat) ? static_cast<u32>(AlignUp(mipHeight, 4)) : mipHeight;
		WGPUExtent3D extent{copyWidth, copyHeight, mipDepth};
		impl->wgpu.wgpuCommandEncoderCopyTextureToBuffer(enc, &srcInfo, &dstInfo, &extent);
		WGPUCommandBufferDescriptor cbDesc{};
		WGPUCommandBuffer cb = impl->wgpu.wgpuCommandEncoderFinish(enc, &cbDesc);
		impl->wgpu.wgpuQueueSubmit(impl->queue, 1, &cb);
		impl->wgpu.wgpuCommandBufferRelease(cb);
		impl->wgpu.wgpuCommandEncoderRelease(enc);

		bool mappedOk = false;
		WGPUBufferMapCallbackInfo mapCb{};
		mapCb.mode = WGPUCallbackMode_WaitAnyOnly;
		mapCb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* ud1, void*) {
			*static_cast<bool*>(ud1) = (status == WGPUMapAsyncStatus_Success);
		};
		mapCb.userdata1 = &mappedOk;
		WGPUFuture future = impl->wgpu.wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, static_cast<size_t>(stagingSize), mapCb);
		impl->WaitFuture(future);

		if (mappedOk) {
			const void* ptr = impl->wgpu.wgpuBufferGetConstMappedRange(staging, 0, static_cast<size_t>(stagingSize));
			if (ptr) {
				const u8* srcBytes = static_cast<const u8*>(ptr);
				const u32 totalRows = rowsPerImage * mipDepth;
				for (u32 row = 0; row < totalRows; ++row) {
					std::memcpy(out.data() + static_cast<size_t>(row) * tightRowBytes,
								srcBytes + static_cast<size_t>(row) * paddedRowBytes,
								tightRowBytes);
				}
			}
			impl->wgpu.wgpuBufferUnmap(staging);
		} else {
			std::memset(out.data(), 0, static_cast<size_t>(expected));
		}
		impl->wgpu.wgpuBufferDestroy(staging);
		impl->wgpu.wgpuBufferRelease(staging);
		resource->state = after;
	}

	void Device::ReadbackBuffer(BufferHandle source, std::span<u8> out, u64 offset) {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		BufferResource* resource = impl->buffers.Resolve(source);
		if (!resource || out.empty() || offset + out.size() > resource->size || !HasFlag(resource->usage, BufferUsage::TransferSrc)) {
			return;
		}
		if (impl->lost || !WebGPUPlatform::SupportsWaitAny()) {
			std::memset(out.data(), 0, out.size());
			resource->state = ResourceState::TransferSrc;
			return;
		}

		WGPUBuffer srcBuf = resource->inVertexHeap ? impl->vertexHeap.buffer : resource->buffer;
		const u64 base = resource->inVertexHeap ? resource->heapOffset : 0;
		const u64 alignedOffset = (base + offset) & ~3ull;
		const u64 lead = (base + offset) - alignedOffset;
		const u64 copySize = AlignUp(lead + out.size(), 4);

		WGPUBufferDescriptor stDesc{};
		stDesc.size = copySize;
		stDesc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
		WGPUBuffer staging = impl->wgpu.wgpuDeviceCreateBuffer(impl->device, &stDesc);
		if (!staging) {
			std::memset(out.data(), 0, out.size());
			return;
		}

		WGPUCommandEncoderDescriptor encDesc{};
		WGPUCommandEncoder enc = impl->wgpu.wgpuDeviceCreateCommandEncoder(impl->device, &encDesc);
		impl->wgpu.wgpuCommandEncoderCopyBufferToBuffer(enc, srcBuf, alignedOffset, staging, 0, copySize);
		WGPUCommandBufferDescriptor cbDesc{};
		WGPUCommandBuffer cb = impl->wgpu.wgpuCommandEncoderFinish(enc, &cbDesc);
		impl->wgpu.wgpuQueueSubmit(impl->queue, 1, &cb);
		impl->wgpu.wgpuCommandBufferRelease(cb);
		impl->wgpu.wgpuCommandEncoderRelease(enc);

		bool mappedOk = false;
		WGPUBufferMapCallbackInfo mapCb{};
		mapCb.mode = WGPUCallbackMode_WaitAnyOnly;
		mapCb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* ud1, void*) {
			*static_cast<bool*>(ud1) = (status == WGPUMapAsyncStatus_Success);
		};
		mapCb.userdata1 = &mappedOk;
		WGPUFuture future = impl->wgpu.wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, static_cast<size_t>(copySize), mapCb);
		impl->WaitFuture(future);

		if (mappedOk) {
			const void* ptr = impl->wgpu.wgpuBufferGetConstMappedRange(staging, 0, static_cast<size_t>(copySize));
			if (ptr) {
				std::memcpy(out.data(), static_cast<const u8*>(ptr) + lead, out.size());
			}
			impl->wgpu.wgpuBufferUnmap(staging);
		} else {
			std::memset(out.data(), 0, out.size());
		}
		impl->wgpu.wgpuBufferDestroy(staging);
		impl->wgpu.wgpuBufferRelease(staging);
		resource->state = ResourceState::TransferSrc;
	}

} // namespace Shalltear::RHI
