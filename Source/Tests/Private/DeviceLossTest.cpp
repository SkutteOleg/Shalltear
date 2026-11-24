#include "TestsPCH.h"

#include "DeviceLossHarness.h"

#include "Graphics/DeviceLossPolicy.h"

#include "Shalltear/Core/Graphics/RHICommandList.h"
#include "Shalltear/Core/Graphics/RHIDevice.h"
#include "Shalltear/Core/Graphics/RHISwapChain.h"
#include "Shalltear/Core/Graphics/RHITypes.h"
#include "Shalltear/Core/Graphics/ShaderAsset.h"
#include "Shalltear/Core/Window.h"

#include <bit>
#include <cstring>

using namespace Shalltear;
using DeviceLossTest::LogSink;
using DeviceLossTest::ScopedLog;
using DeviceLossTest::ScopedMissingDriver;

namespace {

	bool ImportFixture(ShaderAsset& shader, const char* fileName) {
		const std::filesystem::path directory = DeviceLossTest::RepoDir("Source/Tests/Fixtures/Graphics");
		return !directory.empty() && shader.Import(Path((directory / fileName).generic_string()));
	}

	RHI::PipelineHandle MakeColumnsPipeline(RHI::Device& device, const ShaderAsset& shader) {
		const ShaderVariant& base = shader.variants[SHADER_BASE_VARIANT];
		RHI::GraphicsPipelineDesc desc;
		desc.vertexSpirv = {base.vertexSpirv.data(), base.vertexSpirv.size()};
		desc.pixelSpirv = {base.pixelSpirv.data(), base.pixelSpirv.size()};
		desc.bytecodeFormat = shader.bytecodeFormat;
		desc.bindings = {shader.bindings.data(), shader.bindings.size()};
		desc.pushConstantSize = shader.pushConstantSize;
		desc.blend = shader.renderState.blend;
		desc.depth = shader.renderState.depth;
		desc.cull = shader.renderState.cull;
		desc.colorFormats[0] = RHI::Format::RGBA8Unorm;
		desc.colorFormatCount = 1;
		desc.debugName = "LossColumns";
		return device.CreateGraphicsPipeline(desc);
	}

	RHI::PipelineHandle MakeKernel(RHI::Device& device, const ShaderAsset& kernel) {
		const Vector<u8>& spirv = kernel.variants[SHADER_BASE_VARIANT].computeSpirv;
		RHI::ComputePipelineDesc desc;
		desc.spirv = {spirv.data(), spirv.size()};
		desc.bytecodeFormat = kernel.bytecodeFormat;
		desc.bindings = {kernel.bindings.data(), kernel.bindings.size()};
		desc.pushConstantSize = kernel.pushConstantSize;
		desc.debugName = "LossKernel";
		return device.CreateComputePipeline(desc);
	}

	constexpr i32 WIDTH = 16;
	constexpr i32 HEIGHT = 4;

	RHI::TextureHandle MakeTarget(RHI::Device& device) {
		RHI::TextureDesc desc;
		desc.size = {WIDTH, HEIGHT};
		desc.format = RHI::Format::RGBA8Unorm;
		desc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::TransferSrc | RHI::TextureUsage::Sampled;
		desc.debugName = "LossTarget";
		return device.CreateTextureResource(desc);
	}

	u32 DrawColumns(RHI::Device& device, RHI::PipelineHandle pipeline, RHI::TextureHandle target, u32 columns) {
		RHI::CommandList* cmd = device.BeginFrame();
		cmd->Breadcrumb("LossColumns");
		cmd->TransitionTexture(target, RHI::ResourceState::RenderTarget);
		RHI::RenderPassDesc::Attachment attachment;
		attachment.texture = target;
		attachment.load = RHI::LoadOp::Clear;
		attachment.clearColor = Color::Black();
		RHI::RenderPassDesc pass;
		pass.colorAttachments = {&attachment, 1};
		cmd->BeginRenderPass(pass);
		cmd->SetPipeline(pipeline);
		cmd->Draw(columns * 6);
		cmd->EndRenderPass();
		device.EndFrame();
		device.WaitIdle();

		Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
		pixels.assign(static_cast<size_t>(WIDTH) * HEIGHT * 4, 0x55);
		device.ReadbackTexture(target, {pixels.data(), pixels.size()});
		u32 white = 0;
		for (i32 x = 0; x < WIDTH; ++x) {
			white += pixels[(static_cast<size_t>(HEIGHT / 2) * WIDTH + x) * 4] == 255 ? 1u : 0u;
		}
		return white;
	}

	Vector<u8> Ramp(size_t bytes, u8 seed) {
		Vector<u8> out{GetMemoryResource(MemTag::Graphics)};
		out.resize(bytes);
		for (size_t i = 0; i < bytes; ++i) {
			out[i] = static_cast<u8>(seed + i * 7);
		}
		return out;
	}

	RHI::TextureHandle MakeSampled(RHI::Device& device, i32 size) {
		RHI::TextureDesc desc;
		desc.size = {size, size};
		desc.format = RHI::Format::RGBA8Unorm;
		desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst | RHI::TextureUsage::TransferSrc;
		desc.debugName = "LossSampled";
		return device.CreateTextureResource(desc);
	}

	RHI::BufferHandle MakeStorage(RHI::Device& device, u64 bytes) {
		RHI::BufferDesc desc;
		desc.size = bytes;
		desc.usage = RHI::BufferUsage::Storage | RHI::BufferUsage::DeviceAddress | RHI::BufferUsage::TransferSrc | RHI::BufferUsage::TransferDst;
		desc.debugName = "LossStorage";
		return device.CreateBufferResource(desc);
	}

	void FlushDeletes(RHI::Device& device) {
		for (int i = 0; i < 2; ++i) {
			device.BeginFrame();
			device.EndFrame();
		}
		device.WaitIdle();
	}

	class DeviceLossFixture : public ::testing::Test {
	protected:
		RHI::Device* _device = nullptr;

		void SetUp() override {
			RHI::DeviceDesc desc;
			desc.headless = true;
			_device = RHI::Device::Create(desc);
			if (!_device) {
				GTEST_SKIP() << "no Vulkan ICD available";
			}
		}

		void TearDown() override {
			if (_device) {
				RHI::Device::Destroy(_device);
			}
		}
	};

} // anonymous namespace

TEST(DeviceLossPolicyTest, FirstLossRebuildsAtOnce) {
	EXPECT_EQ(RHI::RebuildDelayMs(1), 0u);
	EXPECT_GT(RHI::RebuildDelayMs(2), 0u);
	EXPECT_GE(RHI::RebuildDelayMs(3), RHI::RebuildDelayMs(2));
	EXPECT_EQ(RHI::RebuildDelayMs(3), RHI::RebuildDelayMs(4));
}

TEST(DeviceLossPolicyTest, HistoryCountsWindowOnly) {
	RHI::LossHistory history;
	EXPECT_EQ(history.Note(1'000), 1u);
	EXPECT_EQ(history.Note(2'000), 2u);
	EXPECT_EQ(history.Note(3'000), 3u);
	EXPECT_EQ(history.Note(4'000), 4u);
	EXPECT_EQ(history.Note(4'000 + RHI::LOSS_WINDOW_MS - 1), 2u);
	EXPECT_EQ(history.Note(10 * RHI::LOSS_WINDOW_MS), 1u);
}

TEST(DeviceLossPolicyTest, RelapsingDeviceStaysUnderResetLimit) {
	constexpr u32 BUGCHECK_RESETS = 5;
	for (u64 hangMs : {0ull, 100ull, 2'500ull, 10'000ull, 59'000ull}) {
		RHI::LossHistory history;
		std::vector<u64> losses;
		u64 now = 0;
		while (now < 20 * RHI::LOSS_WINDOW_MS) {
			losses.push_back(now);
			now += RHI::RebuildDelayMs(history.Note(now)) + hangMs + 1;
		}
		ASSERT_GT(losses.size(), 10u) << hangMs;
		u32 worst = 0;
		for (size_t first = 0; first < losses.size(); ++first) {
			u32 inWindow = 0;
			for (size_t i = first; i < losses.size() && losses[i] - losses[first] < RHI::LOSS_WINDOW_MS; ++i) {
				++inWindow;
			}
			worst = std::max(worst, inWindow);
		}
		EXPECT_LT(worst, BUGCHECK_RESETS - 1) << hangMs;
	}
}

TEST(DeviceLossPolicyTest, FailedRebuildLogStaysShort) {
	const u32 hour = static_cast<u32>(3'600'000 / RHI::REBUILD_RETRY_MS);
	u32 lines = 0;
	u32 last = 0;
	for (u32 failed = 1; failed <= hour; ++failed) {
		if (!RHI::LogsFailedRebuild(failed)) {
			continue;
		}
		EXPECT_LE(failed, std::max(1u, last * 2));
		last = failed;
		++lines;
	}
	EXPECT_GT(lines, 8u);
	EXPECT_LT(lines, 16u);
	EXPECT_GT(last * 2, hour);
}

TEST_F(DeviceLossFixture, HandlesSurviveRebuild) {
	constexpr i32 SIZE = 8;
	RHI::TextureHandle texture = MakeSampled(*_device, SIZE);
	RHI::BufferHandle storage = MakeStorage(*_device, 4096);
	RHI::BufferDesc mappedDesc;
	mappedDesc.size = 256;
	mappedDesc.usage = RHI::BufferUsage::Storage;
	mappedDesc.access = RHI::MemoryAccess::CPUToGPU;
	RHI::BufferHandle mapped = _device->CreateBufferResource(mappedDesc);
	RHI::SamplerDesc samplerDesc;
	samplerDesc.addressMode = RHI::AddressMode::ClampToEdge;
	RHI::SamplerHandle sampler = _device->CreateSamplerResource(samplerDesc);
	ASSERT_TRUE(texture.IsValid() && storage.IsValid() && mapped.IsValid() && sampler.IsValid());

	const Vector<u8> pixels = Ramp(static_cast<size_t>(SIZE) * SIZE * 4, 3);
	_device->UploadTexture(texture, {pixels.data(), pixels.size()});
	const u32 textureSlot = _device->GetTableIndex(texture);
	const u32 samplerSlot = _device->GetTableIndex(sampler);
	const u32 buffersBefore = _device->GetLiveBufferCount();
	const u32 texturesBefore = _device->GetLiveTextureCount();
	const u32 samplersBefore = _device->GetLiveSamplerCount();

	ASSERT_FALSE(_device->IsLost());
	_device->SimulateLoss();
	ASSERT_TRUE(_device->IsLost());
	EXPECT_TRUE(_device->IsAlive(texture));
	EXPECT_EQ(_device->GetTextureSize(texture), Vec2I(SIZE, SIZE));

	ASSERT_TRUE(_device->Recover());
	ASSERT_FALSE(_device->IsLost());
	EXPECT_TRUE(_device->Recover());
	EXPECT_EQ(_device->GetLiveBufferCount(), buffersBefore);
	EXPECT_EQ(_device->GetLiveTextureCount(), texturesBefore);
	EXPECT_EQ(_device->GetLiveSamplerCount(), samplersBefore);
	EXPECT_TRUE(_device->IsAlive(texture));
	EXPECT_EQ(_device->GetTextureFormat(texture), RHI::Format::RGBA8Unorm);
	EXPECT_EQ(_device->GetTextureState(texture), RHI::ResourceState::ShaderResource);
	EXPECT_EQ(_device->GetTableIndex(texture), textureSlot);
	EXPECT_EQ(_device->GetTableIndex(sampler), samplerSlot);
	EXPECT_NE(_device->GetBufferDeviceAddress(storage), 0u);
	EXPECT_NE(_device->GetMappedPointer(mapped), nullptr);

	const Vector<u8> second = Ramp(pixels.size(), 91);
	_device->UploadTexture(texture, {second.data(), second.size()});
	Vector<u8> read{GetMemoryResource(MemTag::Graphics)};
	read.assign(second.size(), 0);
	_device->ReadbackTexture(texture, {read.data(), read.size()});
	EXPECT_EQ(std::memcmp(read.data(), second.data(), second.size()), 0);

	const Vector<u8> bytes = Ramp(4096, 17);
	_device->UploadBuffer(storage, {bytes.data(), bytes.size()});
	Vector<u8> readBytes{GetMemoryResource(MemTag::Graphics)};
	readBytes.assign(bytes.size(), 0);
	_device->ReadbackBuffer(storage, {readBytes.data(), readBytes.size()});
	EXPECT_EQ(std::memcmp(readBytes.data(), bytes.data(), bytes.size()), 0);

	_device->DestroyResource(texture);
	_device->DestroyResource(storage);
	_device->DestroyResource(mapped);
	_device->DestroyResource(sampler);
	FlushDeletes(*_device);
}

TEST_F(DeviceLossFixture, LostDeviceIgnoresWork) {
	ShaderAsset columns;
	if (!ImportFixture(columns, "IndirectColumns.hlsl")) {
		GTEST_SKIP() << "dxcompiler.dll unavailable and shader cache cold";
	}
	RHI::PipelineHandle pipeline = MakeColumnsPipeline(*_device, columns);
	RHI::TextureHandle target = MakeTarget(*_device);
	ASSERT_TRUE(pipeline.IsValid() && target.IsValid());
	ASSERT_EQ(DrawColumns(*_device, pipeline, target, 5), 5u);

	ScopedLog log;
	_device->SimulateLoss();
	for (int frame = 0; frame < 3; ++frame) {
		EXPECT_EQ(DrawColumns(*_device, pipeline, target, 5), 0u);
	}
	RHI::CommandList* cmd = _device->BeginFrame();
	RHI::TransientAllocation transient = _device->AllocateTransient(64ull * 1024 * 1024);
	ASSERT_TRUE(transient.IsValid());
	std::memset(transient.cpu, 0xAB, 64ull * 1024 * 1024);
	cmd->SetPipeline(pipeline);
	EXPECT_FALSE(_device->CreateTransientSet(pipeline, 0).Commit().IsValid());
	_device->EndFrame();

	RHI::TextureHandle sampled = MakeSampled(*_device, 4);
	RHI::BufferHandle storage = MakeStorage(*_device, 64);
	ASSERT_TRUE(sampled.IsValid() && storage.IsValid());
	const Vector<u8> pixels = Ramp(4 * 4 * 4, 1);
	_device->UploadTexture(sampled, {pixels.data(), pixels.size()});
	_device->UploadTextureRegion(sampled, {pixels.data(), 4}, RectI{0, 0, 1, 1});
	_device->GenerateMips(sampled);
	_device->UploadBuffer(storage, {pixels.data(), pixels.size()});
	EXPECT_EQ(_device->GetBufferDeviceAddress(storage), 0u);
	Vector<u8> read{GetMemoryResource(MemTag::Graphics)};
	read.assign(pixels.size(), 0x77);
	_device->ReadbackTexture(sampled, {read.data(), read.size()});
	EXPECT_EQ(read[0], 0);
	read.assign(pixels.size(), 0x77);
	_device->ReadbackBuffer(storage, {read.data(), read.size()});
	EXPECT_EQ(read[5], 0);
	EXPECT_FALSE(MakeColumnsPipeline(*_device, columns).IsValid());
	_device->WaitIdle();
	_device->GetVideoMemoryInfo();

	const LogSink& seen = log.Delivered();
	EXPECT_EQ(seen.Count("device loss simulated"), 1u) << seen.Joined();
	EXPECT_EQ(seen.CountAtLeast(LogLevel::Error), 0u) << seen.Joined();
	EXPECT_EQ(seen.Count("failed"), 0u) << seen.Joined();

	ASSERT_TRUE(_device->Recover());
	EXPECT_EQ(_device->GetTextureState(sampled), RHI::ResourceState::ShaderResource);
	EXPECT_NE(_device->GetBufferDeviceAddress(storage), 0u);
	_device->UploadTexture(sampled, {pixels.data(), pixels.size()});
	read.assign(pixels.size(), 0);
	_device->ReadbackTexture(sampled, {read.data(), read.size()});
	EXPECT_EQ(std::memcmp(read.data(), pixels.data(), pixels.size()), 0);

	_device->DestroyResource(pipeline);
	_device->DestroyResource(target);
	_device->DestroyResource(sampled);
	_device->DestroyResource(storage);
	FlushDeletes(*_device);
}

TEST_F(DeviceLossFixture, PipelineDrawsNothingUntilRecreated) {
	ShaderAsset columns;
	if (!ImportFixture(columns, "IndirectColumns.hlsl")) {
		GTEST_SKIP() << "dxcompiler.dll unavailable and shader cache cold";
	}
	RHI::PipelineHandle pipeline = MakeColumnsPipeline(*_device, columns);
	RHI::TextureHandle target = MakeTarget(*_device);
	ASSERT_TRUE(pipeline.IsValid() && target.IsValid());
	ASSERT_EQ(DrawColumns(*_device, pipeline, target, 7), 7u);

	_device->SimulateLoss();
	ASSERT_TRUE(_device->Recover());

	ScopedLog log;
	EXPECT_EQ(DrawColumns(*_device, pipeline, target, 7), 0u);
	EXPECT_EQ(DrawColumns(*_device, pipeline, target, 7), 0u);
	EXPECT_EQ(log.Delivered().Count("from before device rebuild"), 1u);

	RHI::PipelineHandle rebuilt = MakeColumnsPipeline(*_device, columns);
	ASSERT_TRUE(rebuilt.IsValid());
	EXPECT_EQ(DrawColumns(*_device, rebuilt, target, 7), 7u);
	EXPECT_EQ(DrawColumns(*_device, rebuilt, target, 3), 3u);

	_device->DestroyResource(pipeline);
	_device->DestroyResource(rebuilt);
	_device->DestroyResource(target);
	FlushDeletes(*_device);
}

TEST_F(DeviceLossFixture, KernelRunsAfterRebuild) {
	ShaderAsset kernel;
	if (!ImportFixture(kernel, "ComputeFixture.hlsl")) {
		GTEST_SKIP() << "dxcompiler.dll unavailable and shader cache cold";
	}
	constexpr u32 COUNT = 256;
	RHI::BufferHandle values = MakeStorage(*_device, COUNT * 4);
	ASSERT_TRUE(values.IsValid());

	struct Push {
		u32 mode = 0;
		u32 count = COUNT;
		u32 base = 0;
		u32 spin = 0;
		u32 mask = 0;
	};
	auto run = [&](u32 base) {
		RHI::PipelineHandle pipeline = MakeKernel(*_device, kernel);
		const Vector<u8> zeros = Ramp(COUNT * 4, 0);
		_device->UploadBuffer(values, {zeros.data(), zeros.size()});
		RHI::CommandList* cmd = _device->BeginFrame();
		cmd->TransitionBuffer(values, RHI::ResourceState::UnorderedAccess);
		cmd->SetPipeline(pipeline);
		cmd->BindTransientSet(0, _device->CreateTransientSet(pipeline, 0).WriteStorageBuffer(0, values, 0, COUNT * 4).Commit());
		Push push;
		push.base = base;
		cmd->PushConstants({reinterpret_cast<const u8*>(&push), sizeof(push)});
		cmd->Dispatch(COUNT / 64);
		_device->EndFrame();
		_device->WaitIdle();
		Vector<u32> out{GetMemoryResource(MemTag::Graphics)};
		out.assign(COUNT, 0xFFFFFFFFu);
		_device->ReadbackBuffer(values, {reinterpret_cast<u8*>(out.data()), out.size() * 4});
		u32 wrong = 0;
		for (u32 i = 0; i < COUNT; ++i) {
			wrong += out[i] == base + i ? 0u : 1u;
		}
		_device->DestroyResource(pipeline);
		return wrong;
	};

	ASSERT_EQ(run(11), 0u);
	for (u32 round = 0; round < 3; ++round) {
		_device->SimulateLoss();
		ASSERT_TRUE(_device->Recover());
		EXPECT_EQ(run(100 + round), 0u) << round;
	}

	_device->DestroyResource(values);
	FlushDeletes(*_device);
}

TEST_F(DeviceLossFixture, LossInsideFrameEndsIt) {
	ShaderAsset columns;
	if (!ImportFixture(columns, "IndirectColumns.hlsl")) {
		GTEST_SKIP() << "dxcompiler.dll unavailable and shader cache cold";
	}
	RHI::PipelineHandle pipeline = MakeColumnsPipeline(*_device, columns);
	RHI::TextureHandle target = MakeTarget(*_device);
	ASSERT_TRUE(pipeline.IsValid() && target.IsValid());
	ASSERT_EQ(DrawColumns(*_device, pipeline, target, 2), 2u);

	RHI::CommandList* cmd = _device->BeginFrame();
	cmd->TransitionTexture(target, RHI::ResourceState::RenderTarget);
	RHI::RenderPassDesc::Attachment attachment;
	attachment.texture = target;
	RHI::RenderPassDesc pass;
	pass.colorAttachments = {&attachment, 1};
	cmd->BeginRenderPass(pass);
	cmd->SetPipeline(pipeline);
	_device->SimulateLoss();
	cmd->Draw(6);
	RHI::TextureHandle late = MakeSampled(*_device, 4);
	cmd->EndRenderPass();
	cmd->TransitionTexture(late, RHI::ResourceState::TransferDst);
	RHI::TransientAllocation transient = _device->AllocateTransient(256);
	EXPECT_TRUE(transient.IsValid());
	_device->EndFrame();
	EXPECT_FALSE(_device->IsInFrame());

	ASSERT_TRUE(_device->Recover());
	EXPECT_EQ(_device->GetTextureState(late), RHI::ResourceState::TransferDst);
	RHI::PipelineHandle rebuilt = MakeColumnsPipeline(*_device, columns);
	EXPECT_EQ(DrawColumns(*_device, rebuilt, target, 9), 9u);

	_device->DestroyResource(pipeline);
	_device->DestroyResource(rebuilt);
	_device->DestroyResource(target);
	_device->DestroyResource(late);
	FlushDeletes(*_device);
}

TEST_F(DeviceLossFixture, QueuedWorkDiesWithDevice) {
	RHI::BufferHandle storage = MakeStorage(*_device, 256);
	RHI::BufferHandle doomed = MakeStorage(*_device, 256);
	RHI::TextureHandle sampled = MakeSampled(*_device, 4);
	ASSERT_TRUE(storage.IsValid() && doomed.IsValid() && sampled.IsValid());
	const Vector<u8> bytes = Ramp(256, 5);
	_device->UploadBuffer(storage, {bytes.data(), bytes.size()});
	_device->UploadTextureRegion(sampled, {bytes.data(), 4}, RectI{1, 1, 1, 1});
	_device->DestroyResource(doomed);
	const u32 buffers = _device->GetLiveBufferCount();

	_device->SimulateLoss();
	ASSERT_TRUE(_device->Recover());
	EXPECT_EQ(_device->GetLiveBufferCount(), buffers);

	const Vector<u8> fresh = Ramp(256, 77);
	_device->UploadBuffer(storage, {fresh.data(), fresh.size()});
	Vector<u8> read{GetMemoryResource(MemTag::Graphics)};
	read.assign(256, 0);
	_device->ReadbackBuffer(storage, {read.data(), read.size()});
	EXPECT_EQ(std::memcmp(read.data(), fresh.data(), fresh.size()), 0);

	_device->DestroyResource(storage);
	_device->DestroyResource(sampled);
	FlushDeletes(*_device);
}

TEST_F(DeviceLossFixture, LostDeviceTearsDown) {
	RHI::BufferHandle storage = MakeStorage(*_device, 256);
	RHI::TextureHandle sampled = MakeSampled(*_device, 4);
	ASSERT_TRUE(storage.IsValid() && sampled.IsValid());
	_device->SimulateLoss();
	RHI::BufferHandle late = MakeStorage(*_device, 256);
	_device->DestroyResource(storage);
	_device->DestroyResource(sampled);
	_device->DestroyResource(late);
	EXPECT_EQ(_device->GetLiveBufferCount(), 0u);
	EXPECT_EQ(_device->GetLiveTextureCount(), 0u);
}

TEST_F(DeviceLossFixture, RebuildWaitsForMissingDriver) {
	RHI::TextureHandle sampled = MakeSampled(*_device, 4);
	RHI::BufferDesc mappedDesc;
	mappedDesc.size = 256;
	mappedDesc.usage = RHI::BufferUsage::Storage;
	mappedDesc.access = RHI::MemoryAccess::CPUToGPU;
	RHI::BufferHandle storage = _device->CreateBufferResource(mappedDesc);
	ASSERT_TRUE(sampled.IsValid() && storage.IsValid());
	ASSERT_NE(_device->GetMappedPointer(storage), nullptr);
	const Vector<u8> pixels = Ramp(4 * 4 * 4, 9);
	_device->UploadTexture(sampled, {pixels.data(), pixels.size()});

	ScopedLog log;
	ScopedMissingDriver missing;
	_device->SimulateLoss();
	if (_device->Recover()) {
		_device->DestroyResource(sampled);
		_device->DestroyResource(storage);
		GTEST_SKIP() << "Vulkan loader ignores driver overrides in this process";
	}
	ASSERT_TRUE(_device->IsLost());
	EXPECT_FALSE(_device->Recover());

	RHI::CommandList* cmd = _device->BeginFrame();
	cmd->TransitionTexture(sampled, RHI::ResourceState::TransferSrc);
	RHI::TransientAllocation transient = _device->AllocateTransient(1024);
	EXPECT_TRUE(transient.IsValid());
	_device->EndFrame();
	RHI::TextureHandle late = MakeSampled(*_device, 4);
	EXPECT_TRUE(late.IsValid());
	_device->UploadTexture(late, {pixels.data(), pixels.size()});
	EXPECT_EQ(_device->GetMappedPointer(storage), nullptr);
	EXPECT_EQ(_device->GetVideoMemoryInfo().budgetBytes, 0u);
	EXPECT_EQ(_device->GetSupportedSampleCounts(RHI::Format::RGBA8Unorm, RHI::TextureUsage::RenderTarget), 1u);
	_device->WaitIdle();
	const u32 failed = _device->GetFailedRebuildCount();
	EXPECT_GE(failed, 1u);

	missing.Restore();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	bool rebuilt = false;
	u32 attempts = 0;
	while (!rebuilt && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		rebuilt = _device->Recover();
		++attempts;
	}
	ASSERT_TRUE(rebuilt);
	EXPECT_GT(attempts, 5u);
	EXPECT_EQ(_device->GetFailedRebuildCount(), 0u);
	const LogSink& seen = log.Delivered();
	EXPECT_EQ(seen.Count("couldn't rebuild device (vkCreateInstance returned "), static_cast<u32>(std::bit_width(failed))) << seen.Joined();
	EXPECT_EQ(seen.Count("device rebuilt"), 1u) << seen.Joined();
	EXPECT_EQ(seen.CountAtLeast(LogLevel::Error), 0u) << seen.Joined();

	EXPECT_EQ(_device->GetTextureState(sampled), RHI::ResourceState::TransferSrc);
	_device->UploadTexture(late, {pixels.data(), pixels.size()});
	Vector<u8> read{GetMemoryResource(MemTag::Graphics)};
	read.assign(pixels.size(), 0);
	_device->ReadbackTexture(late, {read.data(), read.size()});
	EXPECT_EQ(std::memcmp(read.data(), pixels.data(), pixels.size()), 0);
	_device->BeginFrame();
	EXPECT_TRUE(_device->AllocateTransient(1024).buffer.IsValid());
	_device->EndFrame();

	_device->DestroyResource(sampled);
	_device->DestroyResource(storage);
	_device->DestroyResource(late);
	FlushDeletes(*_device);
}

TEST_F(DeviceLossFixture, FailedRebuildsCountUntilRebuilt) {
	RHI::TextureHandle sampled = MakeSampled(*_device, 4);
	ASSERT_TRUE(sampled.IsValid());
	const Vector<u8> pixels = Ramp(4 * 4 * 4, 9);
	EXPECT_EQ(_device->GetFailedRebuildCount(), 0u);

	ScopedLog log;
	constexpr u32 FAILURES = 3;
	_device->SimulateLoss(FAILURES);
	EXPECT_EQ(_device->GetFailedRebuildCount(), 0u);
	EXPECT_FALSE(_device->Recover());
	EXPECT_EQ(_device->GetFailedRebuildCount(), 1u);
	EXPECT_FALSE(_device->Recover());
	EXPECT_EQ(_device->GetFailedRebuildCount(), 1u);

	RHI::CommandList* cmd = _device->BeginFrame();
	cmd->TransitionTexture(sampled, RHI::ResourceState::TransferSrc);
	EXPECT_TRUE(_device->AllocateTransient(1024).IsValid());
	_device->EndFrame();
	_device->WaitIdle();

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	u32 highest = 1;
	bool rebuilt = false;
	while (!rebuilt && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		rebuilt = _device->Recover();
		const u32 failed = _device->GetFailedRebuildCount();
		if (!rebuilt) {
			EXPECT_TRUE(_device->IsLost());
			EXPECT_GE(failed, highest);
			highest = failed;
		}
	}
	ASSERT_TRUE(rebuilt);
	EXPECT_EQ(highest, FAILURES);
	EXPECT_EQ(_device->GetFailedRebuildCount(), 0u);
	const LogSink& seen = log.Delivered();
	EXPECT_EQ(seen.Count("couldn't rebuild device (simulated, 1 attempt(s) in 0 s). Retrying"), 1u) << seen.Joined();
	EXPECT_EQ(seen.Count("couldn't rebuild device (simulated, 2 attempt(s) in "), 1u) << seen.Joined();
	EXPECT_EQ(seen.Count("couldn't rebuild device"), 2u) << seen.Joined();
	EXPECT_EQ(seen.Count("device rebuilt"), 1u) << seen.Joined();
	EXPECT_EQ(seen.CountAtLeast(LogLevel::Error), 0u) << seen.Joined();

	EXPECT_EQ(_device->GetTextureState(sampled), RHI::ResourceState::TransferSrc);
	_device->UploadTexture(sampled, {pixels.data(), pixels.size()});
	Vector<u8> read{GetMemoryResource(MemTag::Graphics)};
	read.assign(pixels.size(), 0);
	_device->ReadbackTexture(sampled, {read.data(), read.size()});
	EXPECT_EQ(std::memcmp(read.data(), pixels.data(), pixels.size()), 0);

	_device->SimulateLoss();
	EXPECT_EQ(_device->GetFailedRebuildCount(), 0u);
	ASSERT_TRUE(_device->Recover());
	EXPECT_EQ(_device->GetFailedRebuildCount(), 0u);

	_device->DestroyResource(sampled);
	FlushDeletes(*_device);
}

TEST(DeviceLossTeardownTest, DeviceWithoutDriverTearsDown) {
	RHI::DeviceDesc desc;
	desc.headless = true;
	RHI::Device* device = RHI::Device::Create(desc);
	if (!device) {
		GTEST_SKIP() << "no Vulkan ICD available";
	}
	RHI::Device::Destroy(device);
	const size_t idleBytes = MemoryManager::GetBytesForTag(MemTag::Graphics);

	device = RHI::Device::Create(desc);
	ASSERT_NE(device, nullptr);
	RHI::BufferHandle storage = MakeStorage(*device, 256);
	bool rebuilt = false;
	{
		ScopedMissingDriver missing;
		device->SimulateLoss();
		rebuilt = device->Recover();
		device->BeginFrame();
		EXPECT_TRUE(device->AllocateTransient(1024).IsValid());
		device->EndFrame();
	}
	device->DestroyResource(storage);
	RHI::Device::Destroy(device);
	EXPECT_EQ(MemoryManager::GetBytesForTag(MemTag::Graphics), idleBytes);
	if (rebuilt) {
		GTEST_SKIP() << "Vulkan loader ignores driver overrides in this process";
	}
}

TEST(DeviceLossSwapChainTest, WindowPresentsAfterRebuild) {
	RHI::DeviceDesc deviceDesc;
	RHI::Device* device = RHI::Device::Create(deviceDesc);
	if (!device) {
		GTEST_SKIP() << "no Vulkan device that can present";
	}

	WindowSystem::Initialize();
	{
		WindowDesc windowDesc;
		windowDesc.title = "DeviceLossSwapChainTest";
		windowDesc.clientSize = {320, 240};
		windowDesc.flags = WindowFlags::Resizable | WindowFlags::Hidden;
		Handle<Window> window = WindowSystem::Create(windowDesc);
		ASSERT_TRUE(window.IsValid());

		NativeWindowHandle native = window.Get()->GetNativeHandle();
		RHI::NativeSwapChainDesc swapDesc;
		swapDesc.window = native;
		swapDesc.sizePx = window.Get()->GetClientSize();
		swapDesc.vsync = false;
		RHI::SwapChain* swapChain = device->CreateSwapChainForWindow(swapDesc);
		ASSERT_NE(swapChain, nullptr);
		const RHI::Format format = swapChain->GetFormat();
		const Vec2I size = swapChain->GetSize();

		auto present = [&]() {
			RHI::CommandList* cmd = device->BeginFrame();
			RHI::TextureHandle backbuffer = swapChain->AcquireNextImage();
			if (backbuffer.IsValid()) {
				cmd->TransitionTexture(backbuffer, RHI::ResourceState::RenderTarget);
				RHI::RenderPassDesc::Attachment attachment;
				attachment.texture = backbuffer;
				attachment.clearColor = Color::Blue();
				RHI::RenderPassDesc pass;
				pass.colorAttachments = {&attachment, 1};
				cmd->BeginRenderPass(pass);
				cmd->EndRenderPass();
				cmd->TransitionTexture(backbuffer, RHI::ResourceState::Present);
			}
			device->EndFrame();
			swapChain->Present();
			return backbuffer.IsValid();
		};

		ASSERT_TRUE(present());
		const u32 textures = device->GetLiveTextureCount();

		device->SimulateLoss();
		EXPECT_FALSE(present());
		swapChain->Recreate({256, 192}, false);
		ASSERT_TRUE(device->Recover());
		EXPECT_EQ(swapChain->GetFormat(), format);
		EXPECT_FALSE(swapChain->GetVSync());
		for (int frame = 0; frame < 4; ++frame) {
			EXPECT_TRUE(present()) << frame;
		}
		EXPECT_EQ(device->GetLiveTextureCount(), textures);

		swapChain->Recreate(size, true);
		EXPECT_TRUE(present());
		EXPECT_TRUE(swapChain->GetVSync());

		device->SimulateLoss();
		device->DestroySwapChain(swapChain);
		swapChain = device->CreateSwapChainForWindow(swapDesc);
		ASSERT_NE(swapChain, nullptr);
		EXPECT_FALSE(present());
		ASSERT_TRUE(device->Recover());
		EXPECT_TRUE(present());
		EXPECT_EQ(device->GetLiveTextureCount(), textures);
		device->DestroySwapChain(swapChain);

		WindowSystem::RequestDestroy(window);
		WindowSystem::PumpEvents();
	}
	WindowSystem::Terminate();
	RHI::Device::Destroy(device);
}

TEST_F(DeviceLossFixture, FaultedAddressLosesDevice) {
	if (TestPlatform::GetEnvironment("SHALLTEAR_TEST_GPU_FAULT").empty()) {
		GTEST_SKIP() << "faults the GPU. Set SHALLTEAR_TEST_GPU_FAULT=1 to run";
	}
	ShaderAsset kernel;
	if (!ImportFixture(kernel, "ComputeMeshFixture.hlsl")) {
		GTEST_SKIP() << "dxcompiler.dll unavailable and shader cache cold";
	}
	constexpr u32 COUNT = 4096;
	RHI::BufferHandle values = MakeStorage(*_device, COUNT * 8);
	RHI::TextureHandle sampled = MakeSampled(*_device, 8);
	ASSERT_TRUE(values.IsValid() && sampled.IsValid());
	const Vector<u8> pixels = Ramp(8 * 8 * 4, 9);
	_device->UploadTexture(sampled, {pixels.data(), pixels.size()});

	struct Push {
		u64 vertices = 0;
		u64 indices = 0;
		u32 count = COUNT;
		u32 padding = 0;
	};
	ScopedLog log;
	RHI::PipelineHandle pipeline = MakeKernel(*_device, kernel);
	ASSERT_TRUE(pipeline.IsValid());
	const auto started = std::chrono::steady_clock::now();
	for (int frame = 0; frame < 600 && !_device->IsLost(); ++frame) {
		RHI::CommandList* cmd = _device->BeginFrame();
		cmd->Breadcrumb("FaultedKernel");
		cmd->TransitionBuffer(values, RHI::ResourceState::UnorderedAccess);
		cmd->SetPipeline(pipeline);
		cmd->BindTransientSet(0, _device->CreateTransientSet(pipeline, 0).WriteStorageBuffer(0, values, 0, COUNT * 8).Commit());
		Push push;
		push.vertices = 0x00007FDEAD000000ull + static_cast<u64>(frame) * 0x10000;
		push.indices = 0x00007FBEEF000000ull + static_cast<u64>(frame) * 0x10000;
		cmd->PushConstants({reinterpret_cast<const u8*>(&push), sizeof(push)});
		cmd->Dispatch(COUNT / 64);
		_device->EndFrame();
		_device->WaitIdle();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	_device->DestroyResource(pipeline);
	if (!_device->IsLost()) {
		_device->DestroyResource(values);
		_device->DestroyResource(sampled);
		GTEST_SKIP() << "driver survives a read of an unmapped address";
	}
	const auto lostAfter = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	bool rebuilt = false;
	while (!rebuilt && std::chrono::steady_clock::now() < deadline) {
		rebuilt = _device->Recover();
		if (!rebuilt) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}
	const LogSink& seen = log.Delivered();
	std::printf("lost after %lld ms\n%s", static_cast<long long>(lostAfter.count()), seen.Joined().c_str());
	ASSERT_TRUE(rebuilt);
	EXPECT_EQ(seen.Count("device lost ("), 1u);
	EXPECT_EQ(seen.Count("device rebuilt"), 1u);

	_device->UploadTexture(sampled, {pixels.data(), pixels.size()});
	Vector<u8> read{GetMemoryResource(MemTag::Graphics)};
	read.assign(pixels.size(), 0);
	_device->ReadbackTexture(sampled, {read.data(), read.size()});
	EXPECT_EQ(std::memcmp(read.data(), pixels.data(), pixels.size()), 0);

	_device->DestroyResource(values);
	_device->DestroyResource(sampled);
	FlushDeletes(*_device);
}
