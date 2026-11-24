#pragma once

#include "Graphics/DeviceLossPolicy.h"
#include "Graphics/PresentPolicy.h"
#include "Graphics/WebGPU/WebGPUProcs.h"

#include "Shalltear/Core/Assert.h"
#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Graphics/RHICommandList.h"
#include "Shalltear/Core/Graphics/RHIDevice.h"
#include "Shalltear/Core/Graphics/RHISwapChain.h"
#include "Shalltear/Core/Graphics/RHITypes.h"
#include "Shalltear/Core/Log.h"

#include <chrono>
#include <memory>
#include <thread>

namespace Shalltear::RHI {

	template <typename TResource>
	struct ResourcePool {
		struct Slot {
			TResource resource{};
			u32 generation = 1;
			bool alive = false;
		};

		Vector<Slot> slots{GetMemoryResource(MemTag::Graphics)};
		Vector<u32> freeIndices{GetMemoryResource(MemTag::Graphics)};
		u32 liveCount = 0;

		ResourcePool() {
			slots.emplace_back();
		}

		std::pair<u32, u32> Acquire() {
			u32 index;
			if (!freeIndices.empty()) {
				index = freeIndices.back();
				freeIndices.pop_back();
			} else {
				index = static_cast<u32>(slots.size());
				slots.emplace_back();
			}
			Slot& slot = slots[index];
			slot.resource = TResource{};
			slot.alive = true;
			++liveCount;
			return {index, slot.generation};
		}

		void Release(u32 index) {
			Slot& slot = slots[index];
			SLT_CORE_ASSERT(slot.alive);
			slot.alive = false;
			++slot.generation;
			--liveCount;
			freeIndices.push_back(index);
		}

		template <typename THandle>
		TResource* Resolve(THandle handle) {
			if (handle.index == 0 || handle.index >= slots.size()) {
				return nullptr;
			}
			Slot& slot = slots[handle.index];
			if (!slot.alive || slot.generation != handle.generation) {
				return nullptr;
			}
			return &slot.resource;
		}
	};

	inline constexpr u64 WEBGPU_TRANSIENT_HEAP_TAG = 1ull << 32;
	inline constexpr u64 WEBGPU_VERTEX_HEAP_TAG = 2ull << 32;
	inline constexpr u32 WEBGPU_PUSH_STRIDE = 256u;

	struct BufferResource {
		WGPUBuffer buffer = nullptr;
		u64 size = 0;
		u64 gpuSize = 0;
		BufferUsage usage = BufferUsage::None;
		MemoryAccess access = MemoryAccess::GPUOnly;
		bool inVertexHeap = false;
		bool isTransientRing = false;
		u64 heapOffset = 0;
		u64 heapAlignedSize = 0;
		std::shared_ptr<Vector<u8>> cpuShadow;
		mutable bool cpuDirty = false;
		u64 deviceAddress = 0;
		ResourceState state = ResourceState::Undefined;
	};

	struct TextureResource {
		WGPUTexture texture = nullptr;
		WGPUTextureView view = nullptr;
		// 1 2D view per slice/mip at mip * arrayLayers + layer
		Vector<WGPUTextureView> layerViews{GetMemoryResource(MemTag::Graphics)};
		Vec2I size{};
		Format format = Format::Unknown;
		Format gpuFormat = Format::Unknown;
		bool decodedFallback = false;
		TextureUsage usage = TextureUsage::None;
		u32 mipLevels = 1;
		u32 samples = 1;
		u32 arrayLayers = 1;
		u32 depth = 1;
		TextureDimension dimension = TextureDimension::Texture2D;
		u32 tableIndex = TABLE_NONE;
		bool external = false;
		ResourceState state = ResourceState::Undefined;

		WGPUTextureView AttachmentView(u32 layer, u32 mip = 0) const {
			const size_t index = static_cast<size_t>(mip) * arrayLayers + layer;
			return index < layerViews.size() ? layerViews[index] : view;
		}
	};

	struct SamplerResource {
		WGPUSampler sampler = nullptr;
		u32 tableIndex = TABLE_NONE;
		SamplerDesc desc;
	};

	struct GraphicsPipelineVariant {
		FrontFace frontFace = FrontFace::Clockwise;
		i32 depthBias = 0;
		f32 depthBiasSlope = 0.0f;
		WGPURenderPipeline pipeline = nullptr;
	};

	struct PipelineResource {
		bool isCompute = false;
		WGPUComputePipeline computePipeline = nullptr;
		WGPUPipelineLayout layout = nullptr;
		WGPUBindGroupLayout setLayouts[MAX_DESCRIPTOR_SETS] = {};
		bool setHasBindings[MAX_DESCRIPTOR_SETS] = {};
		u64 setBindingMask[MAX_DESCRIPTOR_SETS] = {};
		u32 setBindingCount[MAX_DESCRIPTOR_SETS] = {};
		u32 setCount = 0;
		bool bindsTable = false;
		bool tableWritable = false;
		Vector<DescriptorBindingDesc> bindings{GetMemoryResource(MemTag::Graphics)};
		u32 pushConstantSize = 0;

		WGPUShaderModule vertexModule = nullptr;
		WGPUShaderModule pixelModule = nullptr;
		Vector<SpecializationConstant> pixelConstants{GetMemoryResource(MemTag::Graphics)};
		BlendMode blend = BlendMode::Opaque;
		DepthMode depth = DepthMode::TestWrite;
		CullMode cull = CullMode::Back;
		PrimitiveTopology topology = PrimitiveTopology::TriangleList;
		f32 depthBiasConstant = 0.0f;
		f32 depthBiasSlope = 0.0f;
		bool dynamicDepthBias = false;
		Format colorFormats[MAX_COLOR_ATTACHMENTS] = {};
		u32 colorFormatCount = 0;
		Format depthFormat = Format::Unknown;
		u32 samples = 1;
		DepthCompare depthCompare = DepthCompare::NearerOrEqual;
		Vector<GraphicsPipelineVariant> variants{GetMemoryResource(MemTag::Graphics)};
	};

	struct VertexHeapBlock {
		u64 offset = 0;
		u64 size = 0;
	};

	struct VertexHeap {
		WGPUBuffer buffer = nullptr;
		u64 capacity = 0;
		u64 highWater = 256;
		u64 version = 1;
		Vector<VertexHeapBlock> freeBlocks{GetMemoryResource(MemTag::Graphics)};
	};

	struct TextureAtlasSlot {
		bool used = false;
		u32 x = 0;
		u32 y = 0;
		u32 width = 0;
		u32 height = 0;
	};

	struct TextureAtlasTable {
		static constexpr u32 ATLAS_2D_SIZE = 2048;
		static constexpr u32 ATLAS_2D_MAX_SLOTS = 256;
		static constexpr u32 ATLAS_2D_MAX_TILE = 512;
		static constexpr u32 ATLAS_CUBE_SIZE = 128;
		static constexpr u32 ATLAS_CUBE_SLOTS = 16;

		WGPUBindGroupLayout graphicsLayout = nullptr;
		WGPUBindGroupLayout computeReadWriteLayout = nullptr;
		WGPUBindGroup emptyBindGroup = nullptr;
		WGPUBuffer atlasMetaBuffer = nullptr;
		WGPUTexture atlas2D = nullptr;
		WGPUTextureView atlas2DView = nullptr;
		WGPUTexture atlasCube = nullptr;
		WGPUTextureView atlasCubeView = nullptr;
		Vector<WGPUTextureView> atlasCubeFaceViews{GetMemoryResource(MemTag::Graphics)};
		WGPUSampler atlasSampler = nullptr;

		TextureAtlasSlot slots2D[ATLAS_2D_MAX_SLOTS] = {};
		u32 shelfX = 0;
		u32 shelfY = 0;
		u32 shelfHeight = 0;
		bool cubeUsed[ATLAS_CUBE_SLOTS] = {};

		WGPUShaderModule blitShader = nullptr;
		WGPUBindGroupLayout blitLayout = nullptr;
		WGPUPipelineLayout blitPipelineLayout = nullptr;
		WGPUSampler blitSampler = nullptr;
		HashMap<u32, WGPURenderPipeline> blitPipelines{GetMemoryResource(MemTag::Graphics)};
	};

	struct PendingReadback {
		WGPUBuffer staging = nullptr;
		std::shared_ptr<Vector<u8>> dstShadow;
		u64 dstOffset = 0;
		u32 tightRowBytes = 0;
		u32 paddedRowBytes = 0;
		u32 rowCount = 1;
		WGPUFuture future{};
		bool submitted = false;
		bool mapped = false;
		bool failed = false;
	};

	struct DeletionQueue {
		Vector<WGPUBuffer> buffers{GetMemoryResource(MemTag::Graphics)};
		Vector<WGPUTexture> textures{GetMemoryResource(MemTag::Graphics)};
		Vector<WGPUTextureView> textureViews{GetMemoryResource(MemTag::Graphics)};
		Vector<WGPUSampler> samplers{GetMemoryResource(MemTag::Graphics)};
		Vector<WGPURenderPipeline> renderPipelines{GetMemoryResource(MemTag::Graphics)};
		Vector<WGPUComputePipeline> computePipelines{GetMemoryResource(MemTag::Graphics)};
		Vector<WGPUPipelineLayout> pipelineLayouts{GetMemoryResource(MemTag::Graphics)};
		Vector<WGPUShaderModule> shaderModules{GetMemoryResource(MemTag::Graphics)};
		Vector<WGPUBindGroup> bindGroups{GetMemoryResource(MemTag::Graphics)};
		Vector<VertexHeapBlock> vertexHeapReleases{GetMemoryResource(MemTag::Graphics)};
		struct AtlasRelease {
			bool isCube = false;
			u32 slot = 0;
		};
		Vector<AtlasRelease> atlasReleases{GetMemoryResource(MemTag::Graphics)};

		bool IsEmpty() const {
			return buffers.empty() && textures.empty() && textureViews.empty() && samplers.empty() &&
				   renderPipelines.empty() && computePipelines.empty() && pipelineLayouts.empty() &&
				   shaderModules.empty() && bindGroups.empty() && vertexHeapReleases.empty() && atlasReleases.empty();
		}
	};

	struct ResolvedWrite {
		u64 object = 0;
		u64 offset = 0;
		u64 range = 0;
		u32 binding = 0;
		DescriptorType type = DescriptorType::UniformBuffer;
	};

	struct FrameBindGroupCache {
		static constexpr u32 CACHE_SLOTS = 2048;
		static constexpr u32 CACHE_MAX_PROBES = 16;
		static constexpr u32 MAX_CACHED_GROUPS = 1536;
		static constexpr u32 CACHE_EMPTY = 0xFFFFFFFFu;

		struct Entry {
			u64 hash = 0;
			WGPUBindGroupLayout layout = nullptr;
			u64 opaque = 0;
			u32 firstWrite = 0;
			u32 writeCount = 0;
		};

		u32 slots[CACHE_SLOTS];
		Vector<Entry> entries{GetMemoryResource(MemTag::Graphics)};
		Vector<ResolvedWrite> writes{GetMemoryResource(MemTag::Graphics)};
		Vector<WGPUBindGroup> ownedGroups{GetMemoryResource(MemTag::Graphics)};
		u64 epoch = 0;
		bool hasExternal = false;

		FrameBindGroupCache() {
			Reset();
		}

		void Reset() {
			for (u32& slot : slots) {
				slot = CACHE_EMPTY;
			}
			entries.clear();
			writes.clear();
			hasExternal = false;
		}
	};

	struct CommandListImpl {
		DeviceImpl* device = nullptr;
		WGPUCommandEncoder encoder = nullptr;
		WGPURenderPassEncoder renderPass = nullptr;
		WGPUComputePassEncoder computePass = nullptr;
		u32 boundPipelineIndex = 0;
		WGPURenderPipeline boundRenderPipeline = nullptr;
		WGPUComputePipeline boundComputePipeline = nullptr;
		bool inRenderPass = false;
		bool pipelineRefused = false;
		bool graphicsVariantDirty = false;
		bool renderSet3Dirty = false;
		FrontFace frontFace = FrontFace::Clockwise;
		i32 depthBias = 0;
		f32 depthBiasSlope = 0.0f;
		u32 currentPushOffset = 0;
		u32 lastPushSize = 0;
		bool set3Bound = false;
		bool set3BoundReadWrite = false;
		WGPUBindGroup boundRenderSets[MAX_DESCRIPTOR_SETS] = {};
		u32 boundRenderSet3Offset = 0xFFFFFFFFu;
		WGPUBindGroup boundComputeSets[MAX_DESCRIPTOR_SETS] = {};
		WGPUBindGroup boundComputeActiveSets[MAX_DESCRIPTOR_SETS] = {};
		u32 boundComputeSet3Offset = 0xFFFFFFFFu;
		WGPUBuffer boundIndexBuffer = nullptr;
		WGPUIndexFormat boundIndexFormat = WGPUIndexFormat_Undefined;
		u64 boundIndexOffset = 0;
		u64 boundIndexSize = 0;
		bool viewportBound = false;
		i32 boundViewportX = 0;
		i32 boundViewportY = 0;
		i32 boundViewportW = 0;
		i32 boundViewportH = 0;
		bool scissorBound = false;
		u32 boundScissorX = 0;
		u32 boundScissorY = 0;
		u32 boundScissorW = 0;
		u32 boundScissorH = 0;
		RectI passArea{};
		Format passColorFormats[MAX_COLOR_ATTACHMENTS] = {};
		u32 passColorCount = 0;
		Format passDepthFormat = Format::Unknown;
		u32 passSamples = 1;

		void ResetRenderPassCache() {
			boundRenderPipeline = nullptr;
			graphicsVariantDirty = false;
			renderSet3Dirty = false;
			set3Bound = false;
			boundRenderSet3Offset = 0xFFFFFFFFu;
			for (WGPUBindGroup& bg : boundRenderSets) {
				bg = nullptr;
			}
			boundIndexBuffer = nullptr;
			boundIndexFormat = WGPUIndexFormat_Undefined;
			boundIndexOffset = 0;
			boundIndexSize = 0;
			viewportBound = false;
			scissorBound = false;
		}

		void ResetComputePassCache() {
			boundComputePipeline = nullptr;
			set3Bound = false;
			boundComputeSet3Offset = 0xFFFFFFFFu;
			for (WGPUBindGroup& bg : boundComputeActiveSets) {
				bg = nullptr;
			}
		}
	};

	struct FrameContext {
		CommandList commandList;
		CommandListImpl commandListImpl;
		FrameBindGroupCache bindGroups;
		DeletionQueue deletionQueue;

		WGPUBuffer transientBuffer = nullptr;
		BufferHandle transientHandle;
		Vector<u8> transientCPU{GetMemoryResource(MemTag::Graphics)};
		u64 transientCapacity = 0;
		u64 transientUsed = 256;

		WGPUBuffer pushBuffer = nullptr;
		Vector<u8> pushCPU{GetMemoryResource(MemTag::Graphics)};
		u64 pushCapacity = 0;
		u64 pushUsed = 0;

		WGPUBindGroup set3GraphicsGroup = nullptr;
		WGPUBindGroup set3ComputeReadWriteGroup = nullptr;
		u64 set3HeapVersion = 0;

		Vector<std::shared_ptr<PendingReadback>> pendingReadbacks{GetMemoryResource(MemTag::Graphics)};
		WGPUFuture workDoneFuture{};
		bool hasWorkDoneFuture = false;
	};

	struct SwapChainImpl {
		DeviceImpl* device = nullptr;
		NativeWindowHandle window;
		WGPUSurface surface = nullptr;
		Format format = Format::Unknown;
		WGPUTextureFormat wgpuFormat = WGPUTextureFormat_Undefined;
		Vec2I size{};
		bool vsync = true;
		bool configured = false;
		bool needsRecreate = false;
		bool acquiredThisFrame = false;
		TextureHandle currentHandle;
		WGPUTexture currentTexture = nullptr;
	};

	struct DeviceImpl {
		WebGPUProcs wgpu{};
		WGPUInstance instance = nullptr;
		WGPUAdapter adapter = nullptr;
		WGPUDevice device = nullptr;
		WGPUQueue queue = nullptr;
		WGPULimits limits{};

		Vector<AdapterInfo> adapters{GetMemoryResource(MemTag::Graphics)};
		i32 selectedAdapter = -1;
		DeviceDesc desc;
		bool validation = false;
		bool headless = false;
		bool textureCompressionBC = false;
		bool float32Filterable = false;
		bool lost = false;
		bool lossSimulated = false;
		bool deadPipelineWarned = false;
		str rebuildFailure;
		u32 failedRebuilds = 0;
		u32 simulatedFailures = 0;
		LossHistory losses;
		std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
		std::chrono::steady_clock::time_point lostAt{};
		std::chrono::steady_clock::time_point nextRebuild{};

		ResourcePool<BufferResource> buffers;
		ResourcePool<TextureResource> textures;
		ResourcePool<SamplerResource> samplers;
		ResourcePool<PipelineResource> pipelines;

		HashMap<u64, WGPUBindGroupLayout> setLayoutCache{GetMemoryResource(MemTag::Graphics)};
		VertexHeap vertexHeap;
		TextureAtlasTable table;
		u64 resourceEpoch = 1;

		FrameContext frames[FRAMES_IN_FLIGHT];
		u32 frameIndex = 0;
		bool inFrame = false;

		Vector<SwapChain*> swapChains{GetMemoryResource(MemTag::Graphics)};
		std::thread::id mainThread;

		bool CreateCore(bool rebuild);
		void DestroyCore();
		bool CreateObjects();
		void ReleaseObjects();
		bool RestoreObjects();
		bool InitVertexHeap(u64 initialCapacity);
		void DestroyVertexHeap();
		u64 AllocateVertexHeap(u64 size, u64& outAlignedSize);
		void FreeVertexHeap(u64 offset, u64 alignedSize);
		bool GrowVertexHeap(u64 requiredCapacity);
		bool CreateTable();
		void DestroyTable();
		void EnsureFrameSet3BindGroups(FrameContext& frame);
		void BlitTextureToAtlas2D(TextureResource& src, u32 slot);
		void BlitCubeToAtlasCube(TextureResource& src, u32 slot);
		WGPURenderPipeline GetOrCreateBlitPipeline(WGPUTextureFormat targetFormat);
		bool CreateBufferObject(BufferResource& resource, const char* debugName);
		bool CreateTextureObjects(TextureResource& resource, const char* debugName);
		bool CreateSamplerObject(SamplerResource& resource, const char* debugName);
		void FlushCPUToGPUBuffers();
		void ReleaseBindGroupCache(FrameBindGroupCache& cache);
		void FlushDeletionQueue(DeletionQueue& queue);
		DeletionQueue& DeletionQueueForDestroy();
		void DrainFrameReadbacks(FrameContext& frame, bool wait);
		void WaitFuture(WGPUFuture future);
		void MarkLost(const char* reason);
		WGPUBindGroupLayout GetOrCreateSetLayout(std::span<const WGPUBindGroupLayoutEntry> entries);
		WGPURenderPipeline GetOrCreateGraphicsVariant(PipelineResource& pipeline, FrontFace frontFace, i32 depthBias, f32 depthBiasSlope);
		void CloseActiveComputePass(CommandListImpl& cmd);
		void EnsureActiveComputePass(CommandListImpl& cmd);
		void CmdGenerateMips(WGPUCommandEncoder encoder, TextureResource& resource);
	};

#define SLT_RHI_ASSERT_MAIN_THREAD(impl) \
	SLT_CORE_ASSERT_MSG(std::this_thread::get_id() == (impl)->mainThread, "RHI calls are main-thread-only in v1")

	WGPUTextureFormat ToWGPUFormat(Format format);
	Format FromWGPUFormat(WGPUTextureFormat format);
	bool IsDepthFormat(Format format);
	WGPUTextureUsage ToWGPUTextureUsage(TextureUsage usage, Format gpuFormat, u32 samples);
	u64 AlignUp(u64 value, u64 alignment);

	void ReleaseSwapChainObjects(SwapChainImpl& impl);
	void RestoreSwapChainObjects(SwapChainImpl& impl);

} // namespace Shalltear::RHI
