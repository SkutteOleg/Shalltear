#pragma once

#include "Shalltear/Core/CoreAPI.h"
#include "Shalltear/Core/Graphics/RHITypes.h"
#include "Shalltear/Core/Memory.h"

#include <span>

namespace Shalltear::RHI {

	class CommandList;
	class SwapChain;
	struct DeviceImpl;

	// frame lifetime, never stored
	class SHALLTEAR_CORE_API TransientSetWriter {
	public:
		static constexpr u32 MAX_WRITES = 16;

		TransientSetWriter& WriteUniformBuffer(u32 binding, BufferHandle buffer, u64 offset, u64 range);
		TransientSetWriter& WriteStorageBuffer(u32 binding, BufferHandle buffer, u64 offset, u64 range);
		TransientSetWriter& WriteTexture(u32 binding, TextureHandle texture);
		TransientSetWriter& WriteSampler(u32 binding, SamplerHandle sampler);
		TransientSet Commit();
	private:
		friend class Device;
		friend struct DeviceImpl;
		DeviceImpl* _device = nullptr;
		u32 _pipelineIndex = 0;
		u32 _setIndex = 0;
		struct StagedWrite {
			u32 binding;
			DescriptorType type;
			u64 a;
			u64 b;
			u32 resourceIndex;
			u32 resourceGeneration;
		} _writes[MAX_WRITES];
		u32 _writeCount = 0;
	};

	class SHALLTEAR_CORE_API Device {
	public:
		static Device* Create(const DeviceDesc& desc);
		static void Destroy(Device* device);

		std::span<const AdapterInfo> GetAdapters() const;
		const AdapterInfo& GetSelectedAdapter() const;
		DeviceCapabilities GetCapabilities() const;

		u32 GetSupportedSampleCounts(Format format, TextureUsage usage) const;

		// not CreateBuffer (Win32 macros collide, ugh)
		BufferHandle CreateBufferResource(const BufferDesc& desc);
		TextureHandle CreateTextureResource(const TextureDesc& desc);
		SamplerHandle CreateSamplerResource(const SamplerDesc& desc);
		PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc);
		PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc);

		// GPU object goes at frame end, the handle now
		void DestroyResource(BufferHandle handle);
		void DestroyResource(TextureHandle handle);
		void DestroyResource(SamplerHandle handle);
		void DestroyResource(PipelineHandle handle);

		// valid until resource destroyed (slot reused after frames in flight end)
		u32 GetTableIndex(TextureHandle texture);
		u32 GetTableIndex(SamplerHandle sampler);

		u64 GetBufferDeviceAddress(BufferHandle buffer) const;
		void* GetMappedPointer(BufferHandle buffer) const; // CPUToGPU/Readback only
		bool IsAlive(TextureHandle texture) const;

		Vec2I GetTextureSize(TextureHandle texture) const;
		Format GetTextureFormat(TextureHandle texture) const;
		TextureUsage GetTextureUsage(TextureHandle texture) const;
		ResourceState GetTextureState(TextureHandle texture) const;

		// copy completes with the next submit
		void UploadBuffer(BufferHandle target, std::span<const u8> data, u64 offset = 0);
		void UploadTexture(TextureHandle target, std::span<const u8> data, u32 mipLevel = 0, u32 layer = 0);
		void UploadTextureRegion(TextureHandle target, std::span<const u8> data, RectI region, u32 mipLevel = 0);
		void GenerateMips(TextureHandle target); // mip 0 must be filled
		void ReadbackTexture(TextureHandle source, std::span<u8> out, u32 mipLevel = 0, u32 layer = 0);
		void ReadbackBuffer(BufferHandle source, std::span<u8> out, u64 offset = 0);

		SwapChain* CreateSwapChainForWindow(const NativeSwapChainDesc& desc);
		void DestroySwapChain(SwapChain* swapChain);

		CommandList* BeginFrame();
		void EndFrame();
		bool IsInFrame() const;

		TransientAllocation AllocateTransient(u64 size, u64 alignment = BINDING_OFFSET_ALIGNMENT);

		TransientSetWriter CreateTransientSet(PipelineHandle pipeline, u32 setIndex);

		void LoadPipelineCache(std::span<const u8> blob);
		size_t SavePipelineCache(std::span<u8> out) const;

		VideoMemoryInfo GetVideoMemoryInfo() const;
		u32 GetLiveBufferCount() const;
		u32 GetLiveTextureCount() const;
		u32 GetLiveSamplerCount() const;
		u32 GetLivePipelineCount() const;

		void WaitIdle();

		// while lost handles stay valid + calls ignored
		bool IsLost() const;
		// on success re-upload contents, re-read device addresses + recreate pipelines
		bool Recover();
		u32 GetFailedRebuildCount() const;
		void SimulateLoss(u32 failedRebuilds = 0);
	private:
		Device() = default;
		~Device() = default;
		Device(const Device&) = delete;
		Device& operator=(const Device&) = delete;

		template <typename T, typename... Args>
		friend T* Shalltear::New(MemTag tag, Args&&... args);
		template <typename T>
		friend void Shalltear::Delete(T* p) noexcept;

		DeviceImpl* _impl = nullptr;
	};

} // namespace Shalltear::RHI
