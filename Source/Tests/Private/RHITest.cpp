#include "TestsPCH.h"

#include "TestPlatform.h"

#include "Shalltear/Core/Graphics/RHICommandList.h"
#include "Shalltear/Core/Graphics/RHIDevice.h"
#include "Shalltear/Core/Graphics/RHISwapChain.h"
#include "Shalltear/Core/Graphics/RHITypes.h"
#include "Shalltear/Core/Graphics/ShaderAsset.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Window.h"

#include "ZoneProbe.h"

#include <array>
#include <cstring>
#include <filesystem>

using namespace Shalltear;

namespace {

	class LayerLineSink : public ILogSink {
	public:
		static constexpr const char* SENTINEL = "rhi-layer-sentinel";

		void ReceiveLog(const LogEvent& event) override {
			std::lock_guard lock(_mutex);
			if (event.message.find(SENTINEL) != str::npos) {
				_armed = true;
				return;
			}
			if (_armed) {
				_messages.push_back(str(event.message));
			}
		}
		void Flush() override {}

		bool Armed() const {
			std::lock_guard lock(_mutex);
			return _armed;
		}
		u32 CountMatching(const char* text) const {
			std::lock_guard lock(_mutex);
			u32 count = 0;
			for (const str& message : _messages) {
				count += message.find(text) != str::npos ? 1u : 0u;
			}
			return count;
		}
	private:
		mutable std::mutex _mutex;
		bool _armed = false;
		std::vector<str> _messages;
	};

	class ScopedLogCapture {
	public:
		ScopedLogCapture() : _sink(std::make_shared<LayerLineSink>()) {
			_ownsJobSystem = JobSystem::GetWorkerCount() == 0;
			if (_ownsJobSystem) {
				JobSystem::Initialize();
			}
			LogManager::Initialize({false, false});
			LogManager::SetGlobalLevel(LogLevel::Trace);
			LogManager::AddSink(_sink);
			for (int attempt = 0; attempt < 200 && !_sink->Armed(); ++attempt) {
				SLT_LOG_INFO("Graphics", "{}", LayerLineSink::SENTINEL);
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
		}
		~ScopedLogCapture() {
			LogManager::RemoveSink(_sink);
			LogManager::Terminate();
			if (_ownsJobSystem) {
				JobSystem::Terminate();
			}
		}

		ScopedLogCapture(const ScopedLogCapture&) = delete;
		ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;

		bool Armed() const { return _sink->Armed(); }

		bool Settle(const char* marker) {
			SLT_LOG_INFO("Graphics", "{}", marker);
			for (int attempt = 0; attempt < 400 && _sink->CountMatching(marker) == 0; ++attempt) {
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			return _sink->CountMatching(marker) == 1;
		}

		u32 CountMatching(const char* text) const { return _sink->CountMatching(text); }
	private:
		std::shared_ptr<LayerLineSink> _sink;
		bool _ownsJobSystem = false;
	};

	class RHIFixture : public ::testing::Test {
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

	std::filesystem::path RepoDir(const char* relative) {
		std::error_code ec;
		std::filesystem::path probe = std::filesystem::current_path(ec);
		for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
			std::filesystem::path candidate = probe / relative;
			if (std::filesystem::exists(candidate, ec)) {
				return candidate;
			}
			std::filesystem::path parent = probe.parent_path();
			if (parent == probe) {
				break;
			}
			probe = parent;
		}
		return {};
	}

	bool ImportFixtureShader(ShaderAsset& shader, const char* fileName) {
		std::filesystem::path source = RepoDir("Source/Tests/Fixtures/Graphics") / fileName;
		return !source.empty() && shader.Import(Path(source.generic_string()));
	}

	RHI::PipelineHandle MakeFixturePipeline(RHI::Device& device, const ShaderAsset& shader, const char* debugName,
											RHI::Format colorFormat, RHI::Format depthFormat, u32 samples) {
		RHI::GraphicsPipelineDesc desc;
		desc.vertexSpirv = {shader.variants[SHADER_BASE_VARIANT].vertexSpirv.data(), shader.variants[SHADER_BASE_VARIANT].vertexSpirv.size()};
		desc.pixelSpirv = {shader.variants[SHADER_BASE_VARIANT].pixelSpirv.data(), shader.variants[SHADER_BASE_VARIANT].pixelSpirv.size()};
		desc.bytecodeFormat = shader.bytecodeFormat;
		desc.bindings = {shader.bindings.data(), shader.bindings.size()};
		desc.pushConstantSize = shader.pushConstantSize;
		desc.blend = shader.renderState.blend;
		desc.depth = shader.renderState.depth;
		desc.cull = shader.renderState.cull;
		desc.colorFormats[0] = colorFormat;
		desc.colorFormatCount = 1;
		desc.depthFormat = depthFormat;
		desc.samples = samples;
		desc.debugName = debugName;
		return device.CreateGraphicsPipeline(desc);
	}

	Vector<u8> MakePixelBuffer(i32 width, i32 height) {
		Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
		pixels.assign(static_cast<size_t>(width) * height * 4, 0);
		return pixels;
	}

	u8 RedAt(const Vector<u8>& pixels, i32 width, i32 x, i32 y) {
		return pixels[(static_cast<size_t>(y) * width + x) * 4];
	}

	void FlushDeferredDeletes(RHI::Device& device) {
		for (int i = 0; i < 2; ++i) {
			device.BeginFrame();
			device.EndFrame();
		}
		device.WaitIdle();
	}

	struct ComputePush {
		u32 mode = 0;
		u32 count = 0;
		u32 base = 0;
		u32 spin = 0;
		u32 mask = 0;
	};

	RHI::PipelineHandle MakeKernelPipeline(RHI::Device& device, const ShaderAsset& kernel) {
		const Vector<u8>& spirv = kernel.variants[SHADER_BASE_VARIANT].computeSpirv;
		RHI::ComputePipelineDesc desc;
		desc.spirv = {spirv.data(), spirv.size()};
		desc.bytecodeFormat = kernel.bytecodeFormat;
		desc.bindings = {kernel.bindings.data(), kernel.bindings.size()};
		desc.pushConstantSize = kernel.pushConstantSize;
		desc.debugName = "ComputeFixture";
		return device.CreateComputePipeline(desc);
	}

	RHI::BufferHandle MakeKernelBuffer(RHI::Device& device, u32 count, RHI::BufferUsage extra = RHI::BufferUsage::None) {
		RHI::BufferDesc desc;
		desc.size = static_cast<u64>(count) * 4;
		desc.usage = RHI::BufferUsage::Storage | RHI::BufferUsage::TransferSrc | RHI::BufferUsage::TransferDst | extra;
		RHI::BufferHandle buffer = device.CreateBufferResource(desc);
		Vector<u8> zeros{GetMemoryResource(MemTag::Graphics)};
		zeros.assign(desc.size, 0);
		device.UploadBuffer(buffer, {zeros.data(), zeros.size()});
		return buffer;
	}

	void BindKernel(RHI::Device& device, RHI::CommandList& cmd, RHI::PipelineHandle pipeline, RHI::BufferHandle values,
					u64 size, const ComputePush& push) {
		cmd.SetPipeline(pipeline);
		cmd.BindTransientSet(0, device.CreateTransientSet(pipeline, 0).WriteStorageBuffer(0, values, 0, size).Commit());
		cmd.PushConstants({reinterpret_cast<const u8*>(&push), sizeof(push)});
	}

	Vector<u32> ReadValues(RHI::Device& device, RHI::BufferHandle buffer, u32 count, u32 first = 0) {
		Vector<u32> out{GetMemoryResource(MemTag::Graphics)};
		out.assign(count, 0xFFFFFFFFu);
		device.ReadbackBuffer(buffer, {reinterpret_cast<u8*>(out.data()), out.size() * 4}, static_cast<u64>(first) * 4);
		return out;
	}

} // anonymous namespace

TEST_F(RHIFixture, SelectsAnAdapterAndReportsIt) {
	EXPECT_FALSE(_device->GetAdapters().empty());
	EXPECT_FALSE(_device->GetSelectedAdapter().name.empty());
	EXPECT_GE(_device->GetSelectedAdapter().apiVersion, 0x00403000u);
}

TEST_F(RHIFixture, VideoMemoryQueryAnswers) {
	RHI::VideoMemoryInfo info = _device->GetVideoMemoryInfo();
	EXPECT_GT(info.budgetBytes, 0u);
}

TEST_F(RHIFixture, HandleRecyclingBumpsGenerations) {
	RHI::BufferDesc desc;
	desc.size = 256;
	desc.usage = RHI::BufferUsage::Storage;
	desc.debugName = "recycleA";

	RHI::BufferHandle first = _device->CreateBufferResource(desc);
	ASSERT_TRUE(first.IsValid());
	_device->DestroyResource(first);

	RHI::BufferHandle second = _device->CreateBufferResource(desc);
	ASSERT_TRUE(second.IsValid());
	EXPECT_EQ(second.index, first.index);
	EXPECT_NE(second.generation, first.generation);

	_device->DestroyResource(second);
}

TEST_F(RHIFixture, StaleHandlesAreLoggedNoOps) {
	RHI::BufferDesc desc;
	desc.size = 64;
	desc.usage = RHI::BufferUsage::Uniform;
	RHI::BufferHandle handle = _device->CreateBufferResource(desc);
	ASSERT_TRUE(handle.IsValid());
	_device->DestroyResource(handle);

	_device->DestroyResource(handle);
	EXPECT_EQ(_device->GetBufferDeviceAddress(handle), 0u);
	EXPECT_EQ(_device->GetMappedPointer(handle), nullptr);
	EXPECT_FALSE(RHI::BufferHandle{}.IsValid());
	EXPECT_FALSE(RHI::TextureHandle{}.IsValid());
}

TEST_F(RHIFixture, LiveCountsTrackCreation) {
	u32 baseline = _device->GetLiveBufferCount();
	RHI::BufferDesc desc;
	desc.size = 128;
	desc.usage = RHI::BufferUsage::Storage;
	RHI::BufferHandle handle = _device->CreateBufferResource(desc);
	EXPECT_EQ(_device->GetLiveBufferCount(), baseline + 1);
	_device->DestroyResource(handle);
	EXPECT_EQ(_device->GetLiveBufferCount(), baseline);
}

TEST_F(RHIFixture, TextureUploadReadbackRoundTrips) {
	RHI::TextureDesc desc;
	desc.size = {4, 4};
	desc.format = RHI::Format::RGBA8Unorm;
	desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst | RHI::TextureUsage::TransferSrc;
	desc.debugName = "roundtrip";
	RHI::TextureHandle texture = _device->CreateTextureResource(desc);
	ASSERT_TRUE(texture.IsValid());

	std::array<u8, 4 * 4 * 4> pixels{};
	for (size_t i = 0; i < pixels.size(); ++i) {
		pixels[i] = static_cast<u8>(i * 3 + 1);
	}
	_device->UploadTexture(texture, pixels);

	std::array<u8, 4 * 4 * 4> readback{};
	_device->ReadbackTexture(texture, readback);
	EXPECT_EQ(pixels, readback);

	_device->DestroyResource(texture);
}

TEST_F(RHIFixture, VolumeRoundTripsSlices) {
	constexpr u32 W = 4, H = 4, D = 3;
	RHI::TextureDesc desc;
	desc.size = {static_cast<i32>(W), static_cast<i32>(H)};
	desc.depth = D;
	desc.format = RHI::Format::RGBA8Unorm;
	desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst | RHI::TextureUsage::TransferSrc;
	desc.debugName = "volume";
	RHI::TextureHandle volume = _device->CreateTextureResource(desc);
	ASSERT_TRUE(volume.IsValid());

	std::array<u8, W * H * D * 4> pixels{};
	for (u32 z = 0; z < D; ++z) {
		for (u32 i = 0; i < W * H * 4; ++i) {
			pixels[z * W * H * 4 + i] = static_cast<u8>((z + 1) * 60);
		}
	}
	_device->UploadTexture(volume, pixels);

	std::array<u8, W * H * D * 4> readback{};
	readback.fill(0xAB);
	_device->ReadbackTexture(volume, readback);
	EXPECT_EQ(pixels, readback);
	EXPECT_EQ(readback[0], 60);
	EXPECT_EQ(readback[W * H * 4], 120);
	EXPECT_EQ(readback[2 * W * H * 4], 180);

	_device->DestroyResource(volume);
}

TEST_F(RHIFixture, VolumeRefusesShapesItCannotCombineWith) {
	auto volumeWith = [this](auto&& mutate) {
		RHI::TextureDesc desc;
		desc.size = {4, 4};
		desc.depth = 2;
		desc.format = RHI::Format::RGBA8Unorm;
		desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst;
		desc.debugName = "refused";
		mutate(desc);
		return _device->CreateTextureResource(desc);
	};

	EXPECT_FALSE(volumeWith([](RHI::TextureDesc& d) { d.arrayLayers = 2; }).IsValid());
	EXPECT_FALSE(volumeWith([](RHI::TextureDesc& d) { d.cube = true; }).IsValid());
	EXPECT_FALSE(volumeWith([](RHI::TextureDesc& d) { d.samples = 4; }).IsValid());
	EXPECT_FALSE(volumeWith([](RHI::TextureDesc& d) { d.mipLevels = 2; }).IsValid());
	EXPECT_FALSE(volumeWith([](RHI::TextureDesc& d) { d.usage = d.usage | RHI::TextureUsage::RenderTarget; }).IsValid());

	RHI::TextureHandle ok = volumeWith([](RHI::TextureDesc&) {});
	EXPECT_TRUE(ok.IsValid());
	_device->DestroyResource(ok);
}

TEST_F(RHIFixture, APartialUploadRefusesAVolume) {
	RHI::TextureDesc desc;
	desc.size = {4, 4};
	desc.depth = 2;
	desc.format = RHI::Format::RGBA8Unorm;
	desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst | RHI::TextureUsage::TransferSrc;
	desc.debugName = "partial";
	RHI::TextureHandle volume = _device->CreateTextureResource(desc);
	ASSERT_TRUE(volume.IsValid());

	std::array<u8, 4 * 4 * 2 * 4> zeroed{};
	_device->UploadTexture(volume, zeroed);

	std::array<u8, 2 * 2 * 4> patch{};
	patch.fill(0xFF);
	_device->UploadTextureRegion(volume, patch, RectI{{0, 0}, {2, 2}});

	std::array<u8, 4 * 4 * 2 * 4> readback{};
	readback.fill(0xAB);
	_device->ReadbackTexture(volume, readback);
	EXPECT_EQ(readback, zeroed);

	_device->DestroyResource(volume);
}

TEST_F(RHIFixture, MipGenerationAveragesDown) {
	RHI::TextureDesc desc;
	desc.size = {4, 4};
	desc.format = RHI::Format::RGBA8Unorm;
	desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst | RHI::TextureUsage::TransferSrc;
	desc.mipLevels = 3;
	RHI::TextureHandle texture = _device->CreateTextureResource(desc);
	ASSERT_TRUE(texture.IsValid());

	std::array<u8, 4 * 4 * 4> pixels{};
	for (size_t i = 0; i < pixels.size(); i += 4) {
		pixels[i] = 200;
		pixels[i + 1] = 100;
		pixels[i + 2] = 50;
		pixels[i + 3] = 255;
	}
	_device->UploadTexture(texture, pixels);
	_device->GenerateMips(texture);

	std::array<u8, 1 * 1 * 4> tail{};
	_device->ReadbackTexture(texture, tail, 2);
	EXPECT_NEAR(tail[0], 200, 1);
	EXPECT_NEAR(tail[1], 100, 1);
	EXPECT_NEAR(tail[2], 50, 1);
	EXPECT_EQ(tail[3], 255);

	_device->DestroyResource(texture);
}

TEST_F(RHIFixture, AnArraysMipChainReducesEveryLayer) {
	RHI::TextureDesc desc;
	desc.size = {4, 4};
	desc.format = RHI::Format::RGBA8Unorm;
	desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst | RHI::TextureUsage::TransferSrc;
	desc.mipLevels = 3;
	desc.arrayLayers = 2;
	RHI::TextureHandle texture = _device->CreateTextureResource(desc);
	ASSERT_TRUE(texture.IsValid());

	const u8 colors[2][4] = {{200, 40, 0, 255}, {0, 90, 220, 255}};
	for (u32 layer = 0; layer < 2; ++layer) {
		std::array<u8, 4 * 4 * 4> pixels{};
		for (size_t i = 0; i < pixels.size(); i += 4) {
			std::memcpy(&pixels[i], colors[layer], 4);
		}
		_device->UploadTexture(texture, pixels, 0, layer);
	}
	_device->GenerateMips(texture);

	for (u32 layer = 0; layer < 2; ++layer) {
		std::array<u8, 1 * 1 * 4> tail{};
		tail.fill(0xAB);
		_device->ReadbackTexture(texture, tail, 2, layer);
		EXPECT_NEAR(tail[0], colors[layer][0], 1) << "layer " << layer;
		EXPECT_NEAR(tail[1], colors[layer][1], 1) << "layer " << layer;
		EXPECT_NEAR(tail[2], colors[layer][2], 1) << "layer " << layer;
	}
	_device->DestroyResource(texture);
}

TEST_F(RHIFixture, DimensionRefusesContradictions) {
	auto make = [this](RHI::TextureDimension dimension, auto&& mutate) {
		RHI::TextureDesc desc;
		desc.size = {2, 2};
		desc.format = RHI::Format::RGBA8Unorm;
		desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst | RHI::TextureUsage::TransferSrc;
		desc.dimension = dimension;
		desc.debugName = "stated";
		mutate(desc);
		return _device->CreateTextureResource(desc);
	};
	auto roundTrips = [this](RHI::TextureHandle texture, u32 layer) {
		std::array<u8, 2 * 2 * 4> pixels{};
		for (size_t i = 0; i < pixels.size(); ++i) {
			pixels[i] = static_cast<u8>(17 * (i + 1));
		}
		_device->UploadTexture(texture, pixels, 0, layer);
		std::array<u8, 2 * 2 * 4> readback{};
		readback.fill(0xAB);
		_device->ReadbackTexture(texture, readback, 0, layer);
		return pixels == readback;
	};

	RHI::TextureHandle array = make(RHI::TextureDimension::Texture2DArray, [](RHI::TextureDesc&) {});
	ASSERT_TRUE(array.IsValid());
	EXPECT_TRUE(roundTrips(array, 0));
	_device->DestroyResource(array);
	RHI::TextureHandle volume = make(RHI::TextureDimension::Texture3D, [](RHI::TextureDesc&) {});
	ASSERT_TRUE(volume.IsValid());
	EXPECT_TRUE(roundTrips(volume, 0));
	_device->DestroyResource(volume);
	RHI::TextureHandle cube = make(RHI::TextureDimension::Cube, [](RHI::TextureDesc&) {});
	ASSERT_TRUE(cube.IsValid());
	EXPECT_TRUE(roundTrips(cube, 5));
	_device->DestroyResource(cube);

	EXPECT_FALSE(make(RHI::TextureDimension::Texture2D, [](RHI::TextureDesc& d) { d.arrayLayers = 2; }).IsValid());
	EXPECT_FALSE(make(RHI::TextureDimension::Texture3D, [](RHI::TextureDesc& d) { d.arrayLayers = 2; }).IsValid());
	EXPECT_FALSE(make(RHI::TextureDimension::Texture2DArray, [](RHI::TextureDesc& d) { d.depth = 2; }).IsValid());
	EXPECT_FALSE(make(RHI::TextureDimension::Texture2DArray, [](RHI::TextureDesc& d) { d.cube = true; }).IsValid());
	EXPECT_FALSE(make(RHI::TextureDimension::Cube, [](RHI::TextureDesc& d) {
					 d.cube = true;
					 d.cubeArray = true;
				 }).IsValid());
	EXPECT_FALSE(make(RHI::TextureDimension::Texture3D, [](RHI::TextureDesc& d) { d.mipLevels = 2; }).IsValid());
}

TEST(RHIFormatTest, LevelCountedInBlocksOrTexels) {
	EXPECT_EQ(RHI::FormatLevelBytes(RHI::Format::BC7SRGB, {8, 8}), 64u);
	EXPECT_EQ(RHI::FormatLevelBytes(RHI::Format::BC7Unorm, {2, 2}), 16u);
	EXPECT_EQ(RHI::FormatLevelBytes(RHI::Format::BC5Unorm, {1, 1}), 16u);
	EXPECT_EQ(RHI::FormatLevelBytes(RHI::Format::BC5Unorm, {5, 3}), 32u);
	EXPECT_EQ(RHI::FormatLevelBytes(RHI::Format::BC6HUFloat, {8, 8}), 64u);
	EXPECT_EQ(RHI::FormatLevelBytes(RHI::Format::BC6HUFloat, {1, 1}), 16u);
	EXPECT_EQ(RHI::FormatLevelBytes(RHI::Format::RGBA8SRGB, {5, 3}), 60u);
	EXPECT_EQ(RHI::FormatLevelBytes(RHI::Format::RGBA16Float, {5, 3}), 120u);
	EXPECT_EQ(RHI::FormatLevelBytes(RHI::Format::R8Unorm, {5, 3}), 15u);
	EXPECT_EQ(RHI::FormatLevelBytes(RHI::Format::Unknown, {4, 4}), 0u);
	EXPECT_TRUE(RHI::IsBlockCompressed(RHI::Format::BC5Unorm));
	EXPECT_TRUE(RHI::IsBlockCompressed(RHI::Format::BC6HUFloat));
	EXPECT_FALSE(RHI::IsBlockCompressed(RHI::Format::RGBA8Unorm));
	EXPECT_FALSE(RHI::IsBlockCompressed(RHI::Format::RGBA16Float));
	EXPECT_TRUE(RHI::IsSRGB(RHI::Format::BC7SRGB));
	EXPECT_FALSE(RHI::IsSRGB(RHI::Format::BC7Unorm));
	EXPECT_FALSE(RHI::IsSRGB(RHI::Format::BC5Unorm));
	EXPECT_FALSE(RHI::IsSRGB(RHI::Format::BC6HUFloat));
}

TEST_F(RHIFixture, BlockChainRoundTrips) {
	constexpr u32 LEVELS = 4;
	for (RHI::Format format : {RHI::Format::BC7Unorm, RHI::Format::BC6HUFloat}) {
		RHI::TextureDesc desc;
		desc.size = {8, 8};
		desc.format = format;
		desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst | RHI::TextureUsage::TransferSrc;
		desc.mipLevels = LEVELS;
		desc.debugName = "blocks";
		RHI::TextureHandle texture = _device->CreateTextureResource(desc);
		ASSERT_TRUE(texture.IsValid()) << static_cast<u32>(format);

		Vector<u8> levels[LEVELS];
		for (u32 level = 0; level < LEVELS; ++level) {
			const i32 edge = 8 >> level;
			levels[level].resize(RHI::FormatLevelBytes(desc.format, {edge, edge}));
			for (size_t i = 0; i < levels[level].size(); ++i) {
				levels[level][i] = static_cast<u8>(level * 61 + i * 7 + 3);
			}
			_device->UploadTexture(texture, {levels[level].data(), levels[level].size()}, level);
		}
		for (u32 level = 0; level < LEVELS; ++level) {
			Vector<u8> readback{GetMemoryResource(MemTag::Graphics)};
			readback.assign(levels[level].size(), 0xAB);
			_device->ReadbackTexture(texture, {readback.data(), readback.size()}, level);
			EXPECT_TRUE(readback == levels[level]) << "format " << static_cast<u32>(format) << " level " << level;
		}
		EXPECT_EQ(levels[0].size(), 64u);
		EXPECT_EQ(levels[3].size(), 16u);
		_device->DestroyResource(texture);
	}
}

TEST_F(RHIFixture, BlockFormatRefusesInvalid) {
	auto blocksWith = [this](auto&& mutate) {
		RHI::TextureDesc desc;
		desc.size = {8, 8};
		desc.format = RHI::Format::BC5Unorm;
		desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst | RHI::TextureUsage::TransferSrc;
		desc.debugName = "refusedBlocks";
		mutate(desc);
		return _device->CreateTextureResource(desc);
	};
	EXPECT_FALSE(blocksWith([](RHI::TextureDesc& d) { d.usage = d.usage | RHI::TextureUsage::RenderTarget; }).IsValid());
	EXPECT_FALSE(blocksWith([](RHI::TextureDesc& d) { d.samples = 4; }).IsValid());
	EXPECT_FALSE(blocksWith([](RHI::TextureDesc& d) { d.depth = 2; }).IsValid());

	RHI::TextureHandle texture = blocksWith([](RHI::TextureDesc& d) { d.mipLevels = 2; });
	ASSERT_TRUE(texture.IsValid());
	std::array<u8, 64> top{};
	std::array<u8, 16> tail{};
	top.fill(0x5A);
	tail.fill(0xC3);
	_device->UploadTexture(texture, top, 0);
	_device->UploadTexture(texture, tail, 1);
	_device->GenerateMips(texture);
	std::array<u8, 16> patch{};
	_device->UploadTextureRegion(texture, patch, RectI{{0, 0}, {4, 4}});

	std::array<u8, 64> topBack{};
	std::array<u8, 16> tailBack{};
	_device->ReadbackTexture(texture, topBack, 0);
	_device->ReadbackTexture(texture, tailBack, 1);
	EXPECT_EQ(topBack, top);
	EXPECT_EQ(tailBack, tail);
	_device->DestroyResource(texture);
}

TEST_F(RHIFixture, BufferUploadReachesGPUMemory) {
	RHI::BufferDesc gpuDesc;
	gpuDesc.size = 64;
	gpuDesc.usage = RHI::BufferUsage::Storage | RHI::BufferUsage::TransferDst | RHI::BufferUsage::TransferSrc;
	RHI::BufferHandle gpu = _device->CreateBufferResource(gpuDesc);
	RHI::BufferDesc readDesc;
	readDesc.size = 64;
	readDesc.usage = RHI::BufferUsage::TransferDst;
	readDesc.access = RHI::MemoryAccess::Readback;
	RHI::BufferHandle readback = _device->CreateBufferResource(readDesc);
	ASSERT_TRUE(gpu.IsValid() && readback.IsValid());

	std::array<u8, 64> data{};
	for (size_t i = 0; i < data.size(); ++i) {
		data[i] = static_cast<u8>(255 - i);
	}
	_device->UploadBuffer(gpu, data);

	RHI::CommandList* cmd = _device->BeginFrame();
	cmd->CopyBufferRegion(gpu, readback, 0, 0, 64);
	_device->EndFrame();
	_device->WaitIdle();

	const u8* mapped = static_cast<const u8*>(_device->GetMappedPointer(readback));
	ASSERT_NE(mapped, nullptr);
	EXPECT_EQ(std::memcmp(mapped, data.data(), 64), 0);

	_device->DestroyResource(gpu);
	_device->DestroyResource(readback);
}

TEST_F(RHIFixture, QueuedUploadsLandFirst) {
	constexpr u64 SIZE = 64;
	constexpr int COUNT = 3;
	std::array<RHI::BufferHandle, COUNT> gpu{};
	std::array<std::array<u8, SIZE>, COUNT> payloads{};

	for (int i = 0; i < COUNT; ++i) {
		RHI::BufferDesc desc;
		desc.size = SIZE;
		desc.usage = RHI::BufferUsage::Storage | RHI::BufferUsage::TransferDst | RHI::BufferUsage::TransferSrc;
		gpu[i] = _device->CreateBufferResource(desc);
		ASSERT_TRUE(gpu[i].IsValid());
		payloads[i].fill(static_cast<u8>(0xA0 + i));
		_device->UploadBuffer(gpu[i], payloads[i]);
	}

	RHI::BufferDesc readDesc;
	readDesc.size = SIZE * COUNT;
	readDesc.usage = RHI::BufferUsage::TransferDst;
	readDesc.access = RHI::MemoryAccess::Readback;
	RHI::BufferHandle readback = _device->CreateBufferResource(readDesc);
	ASSERT_TRUE(readback.IsValid());

	RHI::CommandList* cmd = _device->BeginFrame();
	for (int i = 0; i < COUNT; ++i) {
		cmd->CopyBufferRegion(gpu[i], readback, 0, static_cast<u64>(i) * SIZE, SIZE);
	}
	_device->EndFrame();
	_device->WaitIdle();

	const u8* mapped = static_cast<const u8*>(_device->GetMappedPointer(readback));
	ASSERT_NE(mapped, nullptr);
	for (int i = 0; i < COUNT; ++i) {
		EXPECT_EQ(std::memcmp(mapped + static_cast<size_t>(i) * SIZE, payloads[i].data(), SIZE), 0) << "buffer " << i;
	}

	for (RHI::BufferHandle handle : gpu) {
		_device->DestroyResource(handle);
	}
	_device->DestroyResource(readback);
}

TEST_F(RHIFixture, DestroyingBufferCancelsOnlyQueuedUpload) {
	RHI::BufferDesc desc;
	desc.size = 64;
	desc.usage = RHI::BufferUsage::Storage | RHI::BufferUsage::TransferDst | RHI::BufferUsage::TransferSrc;
	RHI::BufferHandle doomed = _device->CreateBufferResource(desc);
	RHI::BufferHandle survivor = _device->CreateBufferResource(desc);
	ASSERT_TRUE(doomed.IsValid() && survivor.IsValid());

	std::array<u8, 64> doomedBytes{};
	doomedBytes.fill(0x11);
	std::array<u8, 64> survivorBytes{};
	survivorBytes.fill(0x22);
	_device->UploadBuffer(doomed, doomedBytes);
	_device->UploadBuffer(survivor, survivorBytes);

	_device->DestroyResource(doomed);

	RHI::BufferDesc readDesc;
	readDesc.size = 64;
	readDesc.usage = RHI::BufferUsage::TransferDst;
	readDesc.access = RHI::MemoryAccess::Readback;
	RHI::BufferHandle readback = _device->CreateBufferResource(readDesc);

	RHI::CommandList* cmd = _device->BeginFrame();
	cmd->CopyBufferRegion(survivor, readback, 0, 0, 64);
	_device->EndFrame();
	_device->WaitIdle();

	const u8* mapped = static_cast<const u8*>(_device->GetMappedPointer(readback));
	ASSERT_NE(mapped, nullptr);
	EXPECT_EQ(std::memcmp(mapped, survivorBytes.data(), 64), 0);

	_device->DestroyResource(survivor);
	_device->DestroyResource(readback);
}

TEST_F(RHIFixture, InFrameUploadReadByFrame) {
	RHI::BufferDesc desc;
	desc.size = 64;
	desc.usage = RHI::BufferUsage::Storage | RHI::BufferUsage::TransferDst | RHI::BufferUsage::TransferSrc;
	RHI::BufferHandle gpu = _device->CreateBufferResource(desc);
	RHI::BufferDesc readDesc;
	readDesc.size = 64;
	readDesc.usage = RHI::BufferUsage::TransferDst;
	readDesc.access = RHI::MemoryAccess::Readback;
	RHI::BufferHandle readEarly = _device->CreateBufferResource(readDesc);
	RHI::BufferHandle readLate = _device->CreateBufferResource(readDesc);
	ASSERT_TRUE(gpu.IsValid() && readEarly.IsValid() && readLate.IsValid());

	std::array<u8, 64> payload{};
	payload.fill(0x5E);

	RHI::CommandList* cmd = _device->BeginFrame();
	cmd->CopyBufferRegion(gpu, readEarly, 0, 0, 64);
	_device->UploadBuffer(gpu, payload);
	cmd->CopyBufferRegion(gpu, readLate, 0, 0, 64);
	_device->EndFrame();
	_device->WaitIdle();

	const u8* early = static_cast<const u8*>(_device->GetMappedPointer(readEarly));
	const u8* late = static_cast<const u8*>(_device->GetMappedPointer(readLate));
	ASSERT_TRUE(early && late);
	EXPECT_EQ(std::memcmp(early, payload.data(), 64), 0);
	EXPECT_EQ(std::memcmp(late, payload.data(), 64), 0);

	_device->DestroyResource(gpu);
	_device->DestroyResource(readEarly);
	_device->DestroyResource(readLate);
}

namespace {

	RHI::TextureHandle MakeRegionPage(RHI::Device& device, i32 side) {
		RHI::TextureDesc desc;
		desc.size = {side, side};
		desc.format = RHI::Format::R8Unorm;
		desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst | RHI::TextureUsage::TransferSrc;
		desc.debugName = "RegionPage";
		RHI::TextureHandle page = device.CreateTextureResource(desc);
		if (page.IsValid()) {
			std::vector<u8> zero(static_cast<size_t>(side) * side, 0);
			device.UploadTexture(page, zero);
		}
		return page;
	}

	void UploadFilled(RHI::Device& device, RHI::TextureHandle page, RectI region, u8 value) {
		std::vector<u8> bytes(static_cast<size_t>(region.size.x) * region.size.y, value);
		device.UploadTextureRegion(page, bytes, region);
	}

	std::vector<u8> ReadPage(RHI::Device& device, RHI::TextureHandle page, i32 side) {
		std::vector<u8> texels(static_cast<size_t>(side) * side, 0xEE);
		device.ReadbackTexture(page, texels);
		return texels;
	}

} // anonymous namespace

TEST_F(RHIFixture, RegionsLandInCallOrder) {
	constexpr i32 SIDE = 16;
	RHI::TextureHandle page = MakeRegionPage(*_device, SIDE);
	ASSERT_TRUE(page.IsValid());
	UploadFilled(*_device, page, RectI{2, 2, 8, 8}, 0x11);
	UploadFilled(*_device, page, RectI{6, 6, 8, 8}, 0x22);
	UploadFilled(*_device, page, RectI{0, 14, 4, 2}, 0x33);

	const std::vector<u8> texels = ReadPage(*_device, page, SIDE);
	auto at = [&](i32 x, i32 y) { return texels[static_cast<size_t>(y) * SIDE + x]; };
	EXPECT_EQ(at(3, 3), 0x11);
	EXPECT_EQ(at(7, 7), 0x22);
	EXPECT_EQ(at(12, 12), 0x22);
	EXPECT_EQ(at(1, 15), 0x33);
	EXPECT_EQ(at(15, 0), 0x00);

	_device->DestroyResource(page);
}

TEST_F(RHIFixture, WholeUploadOverwritesRegion) {
	constexpr i32 SIDE = 16;
	RHI::TextureHandle page = MakeRegionPage(*_device, SIDE);
	ASSERT_TRUE(page.IsValid());
	UploadFilled(*_device, page, RectI{4, 4, 4, 4}, 0xAA);
	std::vector<u8> whole(static_cast<size_t>(SIDE) * SIDE, 0x55);
	_device->UploadTexture(page, whole);
	_device->BeginFrame();
	_device->EndFrame();
	_device->WaitIdle();
	EXPECT_EQ(ReadPage(*_device, page, SIDE)[5 * SIDE + 5], 0x55);

	UploadFilled(*_device, page, RectI{4, 4, 4, 4}, 0xAA);
	_device->BeginFrame();
	_device->EndFrame();
	_device->WaitIdle();
	const std::vector<u8> texels = ReadPage(*_device, page, SIDE);
	EXPECT_EQ(texels[5 * SIDE + 5], 0xAA);
	EXPECT_EQ(texels[0], 0x55);

	_device->DestroyResource(page);
}

TEST_F(RHIFixture, DestroyedTextureDropsItsRegions) {
	constexpr i32 SIDE = 8;
	RHI::TextureHandle doomed = MakeRegionPage(*_device, SIDE);
	RHI::TextureHandle survivor = MakeRegionPage(*_device, SIDE);
	ASSERT_TRUE(doomed.IsValid() && survivor.IsValid());
	UploadFilled(*_device, survivor, RectI{0, 0, 4, 4}, 0x21);
	UploadFilled(*_device, doomed, RectI{0, 0, 8, 8}, 0x7F);
	UploadFilled(*_device, survivor, RectI{4, 4, 4, 4}, 0x42);
	_device->DestroyResource(doomed);

	_device->BeginFrame();
	_device->EndFrame();
	_device->WaitIdle();
	const std::vector<u8> texels = ReadPage(*_device, survivor, SIDE);
	EXPECT_EQ(texels[1 * SIDE + 1], 0x21);
	EXPECT_EQ(texels[5 * SIDE + 5], 0x42);
	EXPECT_EQ(texels[1 * SIDE + 6], 0x00);

	_device->DestroyResource(survivor);
}

TEST_F(RHIFixture, RegionsShareFrameSubmit) {
#if defined(SHALLTEAR_CONFIG_RELEASE)
	GTEST_SKIP() << "zones compiled out in Release";
#else
	constexpr i32 SIDE = 64;
	RHI::TextureHandle page = MakeRegionPage(*_device, SIDE);
	ASSERT_TRUE(page.IsValid());
	{
		ZoneProbe::ZoneScope zones;
		for (i32 i = 0; i < 32; ++i) {
			UploadFilled(*_device, page, RectI{(i % 8) * 8, (i / 8) * 8, 8, 8}, static_cast<u8>(i + 1));
		}
		_device->BeginFrame();
		for (i32 i = 32; i < 64; ++i) {
			UploadFilled(*_device, page, RectI{(i % 8) * 8, (i / 8) * 8, 8, 8}, static_cast<u8>(i + 1));
		}
		_device->EndFrame();
		Profiler::NextFrame();
		EXPECT_EQ(ZoneProbe::ZoneCalls("RHI.ImmediateSubmit"), 0u);

		_device->WaitIdle();
		const std::vector<u8> texels = ReadPage(*_device, page, SIDE);
		Profiler::NextFrame();
		EXPECT_EQ(ZoneProbe::ZoneCalls("RHI.ImmediateSubmit"), 1u);
		for (i32 i = 0; i < 64; ++i) {
			EXPECT_EQ(texels[static_cast<size_t>((i / 8) * 8 + 3) * SIDE + (i % 8) * 8 + 3], static_cast<u8>(i + 1)) << "region " << i;
		}
	}
	_device->DestroyResource(page);
#endif
}

TEST_F(RHIFixture, FramePacingCyclesWithoutWork) {
	for (int i = 0; i < 5; ++i) {
		EXPECT_FALSE(_device->IsInFrame());
		_device->BeginFrame();
		EXPECT_TRUE(_device->IsInFrame());
		_device->EndFrame();
	}
	_device->WaitIdle();
}

TEST_F(RHIFixture, MidFrameDestructionIsDeferred) {
	RHI::BufferDesc desc;
	desc.size = 128;
	desc.usage = RHI::BufferUsage::Storage | RHI::BufferUsage::TransferSrc | RHI::BufferUsage::TransferDst;
	RHI::BufferHandle a = _device->CreateBufferResource(desc);
	RHI::BufferHandle b = _device->CreateBufferResource(desc);
	std::array<u8, 128> payload{};
	payload.fill(0x5A);
	_device->UploadBuffer(a, payload);

	RHI::CommandList* cmd = _device->BeginFrame();
	cmd->CopyBufferRegion(a, b, 0, 0, 128);
	_device->DestroyResource(a);
	EXPECT_FALSE(a.IsValid() && _device->GetMappedPointer(a) != nullptr);
	_device->EndFrame();
	_device->WaitIdle();

	_device->DestroyResource(b);
	_device->BeginFrame();
	_device->EndFrame();
	_device->BeginFrame();
	_device->EndFrame();
	_device->WaitIdle();
}

TEST_F(RHIFixture, TransientAllocationsComeFromTheRing) {
	_device->BeginFrame();
	RHI::TransientAllocation first = _device->AllocateTransient(256);
	RHI::TransientAllocation second = _device->AllocateTransient(256);
	ASSERT_TRUE(first.IsValid() && second.IsValid());
	EXPECT_EQ(first.buffer.index, second.buffer.index);
	EXPECT_GE(second.offset, first.offset + 256);
	std::memset(first.cpu, 0xAB, 256);
	_device->EndFrame();
	_device->WaitIdle();
}

TEST_F(RHIFixture, SampleCountPerUsage) {
	u32 color = _device->GetSupportedSampleCounts(RHI::Format::RGBA8Unorm,
												  RHI::TextureUsage::RenderTarget | RHI::TextureUsage::TransferSrc);
	u32 depth = _device->GetSupportedSampleCounts(RHI::Format::D32Float, RHI::TextureUsage::DepthStencil);
	EXPECT_TRUE(color & 1u);
	EXPECT_TRUE(depth & 1u);
	EXPECT_EQ(color & ~0x7Fu, 0u);
	EXPECT_EQ(_device->GetSupportedSampleCounts(RHI::Format::Unknown, RHI::TextureUsage::RenderTarget), 1u);
}

TEST_F(RHIFixture, MultisampledEdgeResolvesBetweenItsNeighbors) {
	constexpr i32 SIZE = 32;
	constexpr u32 SAMPLES = 4;
	RHI::TextureUsage colorUsage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::TransferSrc;
	if ((_device->GetSupportedSampleCounts(RHI::Format::RGBA8Unorm, colorUsage) & SAMPLES) == 0) {
		GTEST_SKIP();
	}

	ShaderAsset shader;
	if (!ImportFixtureShader(shader, "MsaaTriangle.hlsl")) {
		GTEST_SKIP() << "dxcompiler.dll unavailable and shader cache cold";
	}
	RHI::PipelineHandle multisampled = MakeFixturePipeline(*_device, shader, "MsaaTriangle", RHI::Format::RGBA8Unorm, RHI::Format::Unknown, SAMPLES);
	RHI::PipelineHandle single = MakeFixturePipeline(*_device, shader, "MsaaTriangle1x", RHI::Format::RGBA8Unorm, RHI::Format::Unknown, 1);
	ASSERT_TRUE(multisampled.IsValid());
	ASSERT_TRUE(single.IsValid());

	auto renderAt = [&](u32 samples, RHI::PipelineHandle pipeline, Vector<u8>& out) {
		RHI::TextureDesc msaaDesc;
		msaaDesc.size = {SIZE, SIZE};
		msaaDesc.format = RHI::Format::RGBA8Unorm;
		msaaDesc.usage = colorUsage;
		msaaDesc.samples = samples;
		msaaDesc.debugName = "MsaaEdgeColor";
		RHI::TextureHandle target = _device->CreateTextureResource(msaaDesc);
		ASSERT_TRUE(target.IsValid());

		RHI::TextureDesc resolveDesc = msaaDesc;
		resolveDesc.samples = 1;
		resolveDesc.usage = RHI::TextureUsage::TransferDst | RHI::TextureUsage::TransferSrc;
		resolveDesc.debugName = "MsaaEdgeResolve";
		RHI::TextureHandle resolved = samples > 1 ? _device->CreateTextureResource(resolveDesc) : RHI::TextureHandle{};

		RHI::CommandList* cmd = _device->BeginFrame();
		cmd->TransitionTexture(target, RHI::ResourceState::RenderTarget);
		RHI::RenderPassDesc::Attachment attachment;
		attachment.texture = target;
		attachment.load = RHI::LoadOp::Clear;
		attachment.clearColor = Color::Black();
		RHI::RenderPassDesc pass;
		pass.colorAttachments = {&attachment, 1};
		cmd->BeginRenderPass(pass);
		cmd->SetPipeline(pipeline);
		cmd->Draw(3);
		cmd->EndRenderPass();
		if (samples > 1) {
			cmd->TransitionTexture(target, RHI::ResourceState::TransferSrc);
			cmd->TransitionTexture(resolved, RHI::ResourceState::TransferDst);
			cmd->ResolveTexture(target, resolved);
		}
		_device->EndFrame();
		_device->WaitIdle();

		_device->ReadbackTexture(samples > 1 ? resolved : target, {out.data(), out.size()});

		_device->DestroyResource(target);
		if (resolved.IsValid()) {
			_device->DestroyResource(resolved);
		}
	};

	Vector<u8> aliased = MakePixelBuffer(SIZE, SIZE);
	Vector<u8> antialiased = MakePixelBuffer(SIZE, SIZE);
	renderAt(1, single, aliased);
	renderAt(SAMPLES, multisampled, antialiased);

	u32 blendedAtOne = 0;
	u32 blendedAtFour = 0;
	for (i32 y = 0; y < SIZE; ++y) {
		for (i32 x = 0; x < SIZE; ++x) {
			u8 one = RedAt(aliased, SIZE, x, y);
			u8 four = RedAt(antialiased, SIZE, x, y);
			blendedAtOne += (one != 0 && one != 255) ? 1 : 0;
			blendedAtFour += (four != 0 && four != 255) ? 1 : 0;
		}
	}
	EXPECT_EQ(blendedAtOne, 0u);
	EXPECT_GT(blendedAtFour, 0u);

	_device->DestroyResource(multisampled);
	_device->DestroyResource(single);
	FlushDeferredDeletes(*_device);
}

TEST_F(RHIFixture, CubeFacesRoundTrip) {
	constexpr i32 SIZE = 8;
	constexpr u32 FACES = 6;
	RHI::TextureDesc desc;
	desc.size = {SIZE, SIZE};
	desc.format = RHI::Format::RGBA8Unorm;
	desc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferSrc;
	desc.cube = true;
	desc.debugName = "CubeFaces";
	RHI::TextureHandle cube = _device->CreateTextureResource(desc);
	ASSERT_TRUE(cube.IsValid());

	RHI::CommandList* cmd = _device->BeginFrame();
	cmd->TransitionTexture(cube, RHI::ResourceState::RenderTarget);
	for (u32 face = 0; face < FACES; ++face) {
		RHI::RenderPassDesc::Attachment attachment;
		attachment.texture = cube;
		attachment.layer = face;
		attachment.load = RHI::LoadOp::Clear;
		attachment.clearColor = Color{static_cast<f32>(face + 1) / 8.0f, 0.0f, 0.0f, 1.0f};
		RHI::RenderPassDesc pass;
		pass.colorAttachments = {&attachment, 1};
		cmd->BeginRenderPass(pass);
		cmd->EndRenderPass();
	}
	_device->EndFrame();
	_device->WaitIdle();

	Vector<u8> pixels = MakePixelBuffer(SIZE, SIZE);
	for (u32 face = 0; face < FACES; ++face) {
		std::fill(pixels.begin(), pixels.end(), u8{0});
		_device->ReadbackTexture(cube, {pixels.data(), pixels.size()}, 0, face);
		u8 expected = static_cast<u8>((face + 1) * 255 / 8);
		for (i32 y = 0; y < SIZE; ++y) {
			for (i32 x = 0; x < SIZE; ++x) {
				ASSERT_NEAR(RedAt(pixels, SIZE, x, y), expected, 1) << "face " << face << " texel " << x << "," << y;
			}
		}
	}

	std::fill(pixels.begin(), pixels.end(), u8{0x7F});
	_device->ReadbackTexture(cube, {pixels.data(), pixels.size()}, 0, FACES);
	EXPECT_EQ(pixels[0], 0x7F);

	_device->DestroyResource(cube);
	FlushDeferredDeletes(*_device);
}

namespace {

	constexpr i32 LEVEL_SIZE = 16;
	constexpr u32 LEVEL_COUNT = 3;

	u8 LevelRed(u32 face, u32 level) {
		return static_cast<u8>(10 + face * 30 + level * 7);
	}

	RHI::TextureHandle MakeLevelCube(RHI::Device& device, RHI::TextureUsage extra) {
		RHI::TextureDesc desc;
		desc.size = {LEVEL_SIZE, LEVEL_SIZE};
		desc.format = RHI::Format::RGBA8Unorm;
		desc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferSrc | extra;
		desc.mipLevels = LEVEL_COUNT;
		desc.cube = true;
		desc.debugName = "LevelCube";
		return device.CreateTextureResource(desc);
	}

	void ClearLevel(RHI::CommandList& cmd, RHI::TextureHandle cube, u32 face, u32 level, u8 red) {
		RHI::RenderPassDesc::Attachment attachment;
		attachment.texture = cube;
		attachment.layer = face;
		attachment.mip = level;
		attachment.load = RHI::LoadOp::Clear;
		attachment.clearColor = Color{static_cast<f32>(red) / 255.0f, 0.0f, 0.0f, 1.0f};
		RHI::RenderPassDesc pass;
		pass.colorAttachments = {&attachment, 1};
		cmd.BeginRenderPass(pass);
		cmd.EndRenderPass();
	}

} // anonymous namespace

TEST_F(RHIFixture, PassDrawsNamedLevel) {
	RHI::TextureHandle cube = MakeLevelCube(*_device, RHI::TextureUsage::None);
	ASSERT_TRUE(cube.IsValid());

	RHI::CommandList* cmd = _device->BeginFrame();
	cmd->TransitionTexture(cube, RHI::ResourceState::RenderTarget);
	for (u32 face = 0; face < 6; ++face) {
		for (u32 level = 0; level < LEVEL_COUNT; ++level) {
			ClearLevel(*cmd, cube, face, level, LevelRed(face, level));
		}
	}
	_device->EndFrame();
	_device->WaitIdle();

	for (u32 face = 0; face < 6; ++face) {
		for (u32 level = 0; level < LEVEL_COUNT; ++level) {
			const i32 side = LEVEL_SIZE >> level;
			Vector<u8> pixels = MakePixelBuffer(side, side);
			_device->ReadbackTexture(cube, {pixels.data(), pixels.size()}, level, face);
			for (i32 y = 0; y < side; ++y) {
				for (i32 x = 0; x < side; ++x) {
					ASSERT_NEAR(RedAt(pixels, side, x, y), LevelRed(face, level), 1)
						<< "face " << face << " level " << level << " texel " << x << "," << y;
				}
			}
		}
	}

	_device->DestroyResource(cube);
	FlushDeferredDeletes(*_device);
}

TEST_F(RHIFixture, AFramesMipChainReducesEveryLayer) {
	RHI::TextureHandle cube = MakeLevelCube(*_device, RHI::TextureUsage::TransferDst);
	ASSERT_TRUE(cube.IsValid());

	RHI::CommandList* cmd = _device->BeginFrame();
	cmd->TransitionTexture(cube, RHI::ResourceState::RenderTarget);
	for (u32 face = 0; face < 6; ++face) {
		for (u32 level = 1; level < LEVEL_COUNT; ++level) {
			ClearLevel(*cmd, cube, face, level, 1);
		}
		ClearLevel(*cmd, cube, face, 0, LevelRed(face, 0));
	}
	cmd->GenerateMips(cube);
	_device->EndFrame();
	_device->WaitIdle();

	for (u32 face = 0; face < 6; ++face) {
		const i32 side = LEVEL_SIZE >> (LEVEL_COUNT - 1);
		Vector<u8> pixels = MakePixelBuffer(side, side);
		_device->ReadbackTexture(cube, {pixels.data(), pixels.size()}, LEVEL_COUNT - 1, face);
		EXPECT_NEAR(RedAt(pixels, side, 0, 0), LevelRed(face, 0), 1) << "face " << face;
		EXPECT_NEAR(RedAt(pixels, side, side - 1, side - 1), LevelRed(face, 0), 1) << "face " << face;
	}

	_device->DestroyResource(cube);
	FlushDeferredDeletes(*_device);
}

TEST_F(RHIFixture, CopyTakesLevelAndLayerAskedFor) {
	RHI::TextureHandle cube = MakeLevelCube(*_device, RHI::TextureUsage::None);
	ASSERT_TRUE(cube.IsValid());
	constexpr u32 FACE = 4;
	constexpr u32 LEVEL = 1;
	constexpr i32 SIDE = LEVEL_SIZE >> LEVEL;
	RHI::BufferDesc readDesc;
	readDesc.size = static_cast<u64>(SIDE) * SIDE * 4 + 16;
	readDesc.usage = RHI::BufferUsage::TransferDst;
	readDesc.access = RHI::MemoryAccess::Readback;
	RHI::BufferHandle readback = _device->CreateBufferResource(readDesc);
	ASSERT_TRUE(readback.IsValid());
	u8* sentinel = static_cast<u8*>(_device->GetMappedPointer(readback));
	ASSERT_NE(sentinel, nullptr);
	std::memset(sentinel, 0xAB, static_cast<size_t>(readDesc.size));

	RHI::CommandList* cmd = _device->BeginFrame();
	cmd->TransitionTexture(cube, RHI::ResourceState::RenderTarget);
	for (u32 face = 0; face < 6; ++face) {
		for (u32 level = 0; level < LEVEL_COUNT; ++level) {
			ClearLevel(*cmd, cube, face, level, LevelRed(face, level));
		}
	}
	cmd->TransitionTexture(cube, RHI::ResourceState::TransferSrc);
	cmd->CopyTextureToBuffer(cube, readback, 0, LEVEL, FACE);
	_device->EndFrame();
	_device->WaitIdle();

	const u8* mapped = static_cast<const u8*>(_device->GetMappedPointer(readback));
	ASSERT_NE(mapped, nullptr);
	for (i32 texel = 0; texel < SIDE * SIDE; ++texel) {
		ASSERT_NEAR(mapped[texel * 4], LevelRed(FACE, LEVEL), 1) << "texel " << texel;
	}
	EXPECT_EQ(mapped[SIDE * SIDE * 4], 0xABu);

	std::memset(sentinel, 0xAB, static_cast<size_t>(readDesc.size));
	cmd = _device->BeginFrame();
	cmd->CopyTextureToBuffer(cube, readback, 0, LEVEL_COUNT, FACE);
	_device->EndFrame();
	_device->WaitIdle();
	EXPECT_EQ(mapped[0], 0xABu);

	_device->DestroyResource(readback);
	_device->DestroyResource(cube);
	FlushDeferredDeletes(*_device);
}

TEST_F(RHIFixture, DepthPassFeedsComparison) {
	constexpr i32 SIZE = 16;
	constexpr f32 STORED_DEPTH = 0.75f;

	RHI::TextureDesc depthDesc;
	depthDesc.size = {SIZE, SIZE};
	depthDesc.format = RHI::Format::D32Float;
	depthDesc.usage = RHI::TextureUsage::DepthStencil | RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferSrc;
	depthDesc.debugName = "CompareDepth";
	RHI::TextureHandle depth = _device->CreateTextureResource(depthDesc);
	ASSERT_TRUE(depth.IsValid());

	RHI::TextureDesc colorDesc;
	colorDesc.size = {SIZE, SIZE};
	colorDesc.format = RHI::Format::RGBA8Unorm;
	colorDesc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::TransferSrc;
	colorDesc.debugName = "CompareResult";
	RHI::TextureHandle color = _device->CreateTextureResource(colorDesc);
	ASSERT_TRUE(color.IsValid());

	ShaderAsset shader;
	if (!ImportFixtureShader(shader, "DepthCompare.hlsl")) {
		_device->DestroyResource(depth);
		_device->DestroyResource(color);
		FlushDeferredDeletes(*_device);
		GTEST_SKIP() << "dxcompiler.dll unavailable and shader cache cold";
	}
	RHI::PipelineHandle pipeline = MakeFixturePipeline(*_device, shader, "DepthCompare", RHI::Format::RGBA8Unorm, RHI::Format::Unknown, 1);
	ASSERT_TRUE(pipeline.IsValid());

	RHI::SamplerDesc samplerDesc;
	samplerDesc.filter = RHI::FilterMode::Linear;
	samplerDesc.addressMode = RHI::AddressMode::ClampToEdge;
	samplerDesc.comparison = true;
	samplerDesc.debugName = "ShadowCompare";
	RHI::SamplerHandle comparison = _device->CreateSamplerResource(samplerDesc);
	ASSERT_TRUE(comparison.IsValid());

	RHI::CommandList* cmd = _device->BeginFrame();

	cmd->TransitionTexture(depth, RHI::ResourceState::DepthWrite);
	RHI::RenderPassDesc::DepthAttachment depthAttachment;
	depthAttachment.texture = depth;
	depthAttachment.load = RHI::LoadOp::Clear;
	depthAttachment.clearDepth = STORED_DEPTH;
	RHI::RenderPassDesc depthPass;
	depthPass.depth = &depthAttachment;
	cmd->BeginRenderPass(depthPass);
	cmd->EndRenderPass();

	cmd->TransitionTexture(depth, RHI::ResourceState::ShaderResource);
	cmd->TransitionTexture(color, RHI::ResourceState::RenderTarget);
	RHI::RenderPassDesc::Attachment colorAttachment;
	colorAttachment.texture = color;
	colorAttachment.load = RHI::LoadOp::Clear;
	colorAttachment.clearColor = Color::Red();
	RHI::RenderPassDesc colorPass;
	colorPass.colorAttachments = {&colorAttachment, 1};
	cmd->BeginRenderPass(colorPass);
	cmd->SetPipeline(pipeline);
	RHI::TransientSet set = _device->CreateTransientSet(pipeline, 1)
								.WriteTexture(0, depth)
								.WriteSampler(1, comparison)
								.Commit();
	cmd->BindTransientSet(1, set);
	cmd->Draw(3);
	cmd->EndRenderPass();

	_device->EndFrame();
	_device->WaitIdle();

	Vector<u8> depthBytes = MakePixelBuffer(SIZE, SIZE);
	_device->ReadbackTexture(depth, {depthBytes.data(), depthBytes.size()});
	const f32* depthTexels = reinterpret_cast<const f32*>(depthBytes.data());
	EXPECT_NEAR(depthTexels[0], STORED_DEPTH, 1e-5f);
	EXPECT_NEAR(depthTexels[SIZE * SIZE - 1], STORED_DEPTH, 1e-5f);

	Vector<u8> result = MakePixelBuffer(SIZE, SIZE);
	_device->ReadbackTexture(color, {result.data(), result.size()});
	EXPECT_EQ(RedAt(result, SIZE, 2, SIZE / 2), 0);
	EXPECT_EQ(RedAt(result, SIZE, SIZE - 1, SIZE / 2), 255);

	_device->DestroyResource(comparison);
	_device->DestroyResource(pipeline);
	_device->DestroyResource(depth);
	_device->DestroyResource(color);
	FlushDeferredDeletes(*_device);
}

TEST_F(RHIFixture, DepthPipelineHasNoPixelStage) {
	constexpr i32 SIZE = 16;
	constexpr f32 TRIANGLE_DEPTH = 0.5f;

	ShaderAsset shader;
	if (!ImportFixtureShader(shader, "DepthOnlyTriangle.hlsl")) {
		GTEST_SKIP() << "dxcompiler.dll unavailable and shader cache cold";
	}
	const ShaderVariant& base = shader.variants[SHADER_BASE_VARIANT];

	RHI::GraphicsPipelineDesc desc;
	desc.vertexSpirv = {base.vertexSpirv.data(), base.vertexSpirv.size()};
	desc.bytecodeFormat = shader.bytecodeFormat;
	desc.bindings = {shader.bindings.data(), shader.bindings.size()};
	desc.pushConstantSize = shader.pushConstantSize;
	desc.depth = RHI::DepthMode::TestWrite;
	desc.cull = RHI::CullMode::Off;
	desc.colorFormatCount = 0;
	desc.depthFormat = RHI::Format::D32Float;
	desc.debugName = "DepthOnlyTriangle";
	RHI::PipelineHandle pipeline = _device->CreateGraphicsPipeline(desc);
	ASSERT_TRUE(pipeline.IsValid());

	RHI::GraphicsPipelineDesc withColor = desc;
	withColor.colorFormats[0] = RHI::Format::RGBA8Unorm;
	withColor.colorFormatCount = 1;
	withColor.debugName = "DepthOnlyTriangleWithColor";
	EXPECT_FALSE(_device->CreateGraphicsPipeline(withColor).IsValid());

	RHI::TextureDesc depthDesc;
	depthDesc.size = {SIZE, SIZE};
	depthDesc.format = RHI::Format::D32Float;
	depthDesc.usage = RHI::TextureUsage::DepthStencil | RHI::TextureUsage::TransferSrc;
	depthDesc.debugName = "DepthOnlyMap";
	RHI::TextureHandle depth = _device->CreateTextureResource(depthDesc);
	ASSERT_TRUE(depth.IsValid());

	RHI::CommandList* cmd = _device->BeginFrame();
	cmd->TransitionTexture(depth, RHI::ResourceState::DepthWrite);
	RHI::RenderPassDesc::DepthAttachment depthAttachment;
	depthAttachment.texture = depth;
	depthAttachment.load = RHI::LoadOp::Clear;
	depthAttachment.clearDepth = 0.0f;
	RHI::RenderPassDesc pass;
	pass.depth = &depthAttachment;
	cmd->BeginRenderPass(pass);
	cmd->SetPipeline(pipeline);
	cmd->Draw(3);
	cmd->EndRenderPass();
	_device->EndFrame();
	_device->WaitIdle();

	Vector<u8> bytes = MakePixelBuffer(SIZE, SIZE);
	_device->ReadbackTexture(depth, {bytes.data(), bytes.size()});
	const f32* texels = reinterpret_cast<const f32*>(bytes.data());
	u32 covered = 0;
	u32 clear = 0;
	for (i32 i = 0; i < SIZE * SIZE; ++i) {
		covered += std::abs(texels[i] - TRIANGLE_DEPTH) < 1e-5f ? 1 : 0;
		clear += texels[i] == 0.0f ? 1 : 0;
	}
	EXPECT_EQ(covered + clear, static_cast<u32>(SIZE * SIZE));
	EXPECT_GT(covered, static_cast<u32>(SIZE * SIZE) * 2 / 5);
	EXPECT_GT(clear, static_cast<u32>(SIZE * SIZE) * 2 / 5);
	EXPECT_NEAR(texels[(SIZE - 2) * SIZE + 1], TRIANGLE_DEPTH, 1e-5f);
	EXPECT_EQ(texels[1 * SIZE + (SIZE - 2)], 0.0f);

	_device->DestroyResource(pipeline);
	_device->DestroyResource(depth);
	FlushDeferredDeletes(*_device);
}

TEST_F(RHIFixture, DepthComparePassesFragmentsNameSays) {
	constexpr i32 SIZE = 16;
	constexpr f32 TRIANGLE_DEPTH = 0.5f;
	ShaderAsset shader;
	if (!ImportFixtureShader(shader, "DepthOnlyTriangle.hlsl")) {
		GTEST_SKIP() << "dxcompiler.dll unavailable and shader cache cold";
	}
	const ShaderVariant& base = shader.variants[SHADER_BASE_VARIANT];
	RHI::TextureDesc depthDesc;
	depthDesc.size = {SIZE, SIZE};
	depthDesc.format = RHI::Format::D32Float;
	depthDesc.usage = RHI::TextureUsage::DepthStencil | RHI::TextureUsage::TransferSrc;
	depthDesc.debugName = "DepthCompareMap";
	RHI::TextureHandle depth = _device->CreateTextureResource(depthDesc);
	ASSERT_TRUE(depth.IsValid());

	using C = RHI::DepthCompare;
	struct Row {
		C compare;
		f32 clear;
		bool passes;
	};
	const Row rows[] = {
		{C::NearerOrEqual, 0.25f, true},
		{C::NearerOrEqual, 0.75f, false},
		{C::NearerOrEqual, 0.5f, true},
		{C::Nearer, 0.25f, true},
		{C::Nearer, 0.5f, false},
		{C::Farther, 0.75f, true},
		{C::Farther, 0.25f, false},
		{C::FartherOrEqual, 0.5f, true},
		{C::FartherOrEqual, 0.25f, false},
		{C::Equal, 0.5f, true},
		{C::Equal, 0.25f, false},
		{C::NotEqual, 0.25f, true},
		{C::NotEqual, 0.5f, false},
		{C::Always, 0.75f, true},
	};
	for (const Row& row : rows) {
		RHI::GraphicsPipelineDesc desc;
		desc.vertexSpirv = {base.vertexSpirv.data(), base.vertexSpirv.size()};
		desc.bytecodeFormat = shader.bytecodeFormat;
		desc.bindings = {shader.bindings.data(), shader.bindings.size()};
		desc.pushConstantSize = shader.pushConstantSize;
		desc.depth = RHI::DepthMode::TestWrite;
		desc.depthCompare = row.compare;
		desc.cull = RHI::CullMode::Off;
		desc.depthFormat = RHI::Format::D32Float;
		desc.debugName = "DepthCompareTriangle";
		RHI::PipelineHandle pipeline = _device->CreateGraphicsPipeline(desc);
		ASSERT_TRUE(pipeline.IsValid()) << RHI::DepthCompareName(row.compare);

		RHI::CommandList* cmd = _device->BeginFrame();
		cmd->TransitionTexture(depth, RHI::ResourceState::DepthWrite);
		RHI::RenderPassDesc::DepthAttachment depthAttachment;
		depthAttachment.texture = depth;
		depthAttachment.load = RHI::LoadOp::Clear;
		depthAttachment.clearDepth = row.clear;
		RHI::RenderPassDesc pass;
		pass.depth = &depthAttachment;
		cmd->BeginRenderPass(pass);
		cmd->SetPipeline(pipeline);
		cmd->Draw(3);
		cmd->EndRenderPass();
		_device->EndFrame();
		_device->WaitIdle();

		Vector<u8> bytes = MakePixelBuffer(SIZE, SIZE);
		_device->ReadbackTexture(depth, {bytes.data(), bytes.size()});
		const f32* texels = reinterpret_cast<const f32*>(bytes.data());
		EXPECT_NEAR(texels[(SIZE - 2) * SIZE + 1], row.passes ? TRIANGLE_DEPTH : row.clear, 1e-5f) << RHI::DepthCompareName(row.compare) << ' ' << row.clear;
		EXPECT_NEAR(texels[1 * SIZE + (SIZE - 2)], row.clear, 1e-5f);
		_device->DestroyResource(pipeline);
	}
	_device->DestroyResource(depth);
	FlushDeferredDeletes(*_device);
}

TEST_F(RHIFixture, FrontFaceLastsUntilNextPipeline) {
	constexpr i32 SIZE = 16;
	constexpr f32 TRIANGLE_DEPTH = 0.5f;
	ShaderAsset shader;
	if (!ImportFixtureShader(shader, "DepthOnlyTriangle.hlsl")) {
		GTEST_SKIP() << "dxcompiler.dll unavailable and shader cache cold";
	}
	const ShaderVariant& base = shader.variants[SHADER_BASE_VARIANT];
	RHI::GraphicsPipelineDesc desc;
	desc.vertexSpirv = {base.vertexSpirv.data(), base.vertexSpirv.size()};
	desc.bytecodeFormat = shader.bytecodeFormat;
	desc.bindings = {shader.bindings.data(), shader.bindings.size()};
	desc.pushConstantSize = shader.pushConstantSize;
	desc.depth = RHI::DepthMode::TestWrite;
	desc.cull = RHI::CullMode::Back;
	desc.depthFormat = RHI::Format::D32Float;
	desc.debugName = "FrontFaceTriangle";
	RHI::PipelineHandle pipeline = _device->CreateGraphicsPipeline(desc);
	ASSERT_TRUE(pipeline.IsValid());

	RHI::TextureDesc depthDesc;
	depthDesc.size = {SIZE, SIZE};
	depthDesc.format = RHI::Format::D32Float;
	depthDesc.usage = RHI::TextureUsage::DepthStencil | RHI::TextureUsage::TransferSrc;
	depthDesc.debugName = "FrontFaceMap";
	RHI::TextureHandle depth = _device->CreateTextureResource(depthDesc);
	ASSERT_TRUE(depth.IsValid());

	auto depthAfter = [&](auto&& record) {
		RHI::CommandList* cmd = _device->BeginFrame();
		cmd->TransitionTexture(depth, RHI::ResourceState::DepthWrite);
		RHI::RenderPassDesc::DepthAttachment depthAttachment;
		depthAttachment.texture = depth;
		depthAttachment.load = RHI::LoadOp::Clear;
		depthAttachment.clearDepth = 0.0f;
		RHI::RenderPassDesc pass;
		pass.depth = &depthAttachment;
		cmd->BeginRenderPass(pass);
		record(*cmd);
		cmd->Draw(3);
		cmd->EndRenderPass();
		_device->EndFrame();
		_device->WaitIdle();
		Vector<u8> bytes = MakePixelBuffer(SIZE, SIZE);
		_device->ReadbackTexture(depth, {bytes.data(), bytes.size()});
		return reinterpret_cast<const f32*>(bytes.data())[(SIZE - 2) * SIZE + 1];
	};

	using F = RHI::FrontFace;
	EXPECT_EQ(depthAfter([&](RHI::CommandList& cmd) { cmd.SetPipeline(pipeline); }), 0.0f);
	EXPECT_NEAR(depthAfter([&](RHI::CommandList& cmd) {
					cmd.SetPipeline(pipeline);
					cmd.SetFrontFace(F::CounterClockwise);
				}),
				TRIANGLE_DEPTH, 1e-5f);
	EXPECT_EQ(depthAfter([&](RHI::CommandList& cmd) {
				  cmd.SetFrontFace(F::CounterClockwise);
				  cmd.SetPipeline(pipeline);
			  }),
			  0.0f);
	EXPECT_EQ(depthAfter([&](RHI::CommandList& cmd) {
				  cmd.SetPipeline(pipeline);
				  cmd.SetFrontFace(F::CounterClockwise);
				  cmd.SetFrontFace(F::Clockwise);
			  }),
			  0.0f);
	EXPECT_NEAR(depthAfter([&](RHI::CommandList& cmd) {
					cmd.SetPipeline(pipeline);
					cmd.SetFrontFace(F::CounterClockwise);
					cmd.SetPipeline(pipeline);
					cmd.SetFrontFace(F::CounterClockwise);
				}),
				TRIANGLE_DEPTH, 1e-5f);

	_device->DestroyResource(pipeline);
	_device->DestroyResource(depth);
	FlushDeferredDeletes(*_device);
}

TEST_F(RHIFixture, KernelDispatchWritesWhatBufferReadsBack) {
	ShaderAsset kernel;
	if (!ImportFixtureShader(kernel, "ComputeFixture.hlsl")) {
		GTEST_SKIP() << "dxcompiler.dll unavailable and shader cache cold";
	}
	ASSERT_TRUE(kernel.IsKernel());
	ASSERT_EQ(kernel.threadGroupSize.x, 64);
	RHI::PipelineHandle pipeline = MakeKernelPipeline(*_device, kernel);
	ASSERT_TRUE(pipeline.IsValid());

	constexpr u32 COUNT = 1000;
	RHI::BufferHandle values = MakeKernelBuffer(*_device, COUNT);
	RHI::CommandList* cmd = _device->BeginFrame();
	cmd->TransitionBuffer(values, RHI::ResourceState::UnorderedAccess);
	BindKernel(*_device, *cmd, pipeline, values, COUNT * 4, {0, COUNT, 7});
	cmd->Dispatch((COUNT + 63) / 64);
	_device->EndFrame();
	_device->WaitIdle();

	Vector<u32> out = ReadValues(*_device, values, COUNT);
	u32 wrong = 0;
	for (u32 i = 0; i < COUNT; ++i) {
		wrong += out[i] == 7 + i ? 0u : 1u;
	}
	EXPECT_EQ(wrong, 0u) << "first: " << out[0] << ", last: " << out[COUNT - 1];
	Vector<u32> tail = ReadValues(*_device, values, 4, 100);
	EXPECT_EQ(tail[0], 107u);
	EXPECT_EQ(tail[3], 110u);

	_device->DestroyResource(pipeline);
	_device->DestroyResource(values);
	FlushDeferredDeletes(*_device);
}

TEST_F(RHIFixture, IndirectDispatchRunsGroupCountKernelWrote) {
	ShaderAsset kernel;
	if (!ImportFixtureShader(kernel, "ComputeFixture.hlsl")) {
		GTEST_SKIP() << "dxcompiler.dll unavailable and shader cache cold";
	}
	RHI::PipelineHandle pipeline = MakeKernelPipeline(*_device, kernel);
	ASSERT_TRUE(pipeline.IsValid());
	constexpr u32 COUNT = 256;

	for (u32 groups : {2u, 0u}) {
		RHI::BufferHandle arguments = MakeKernelBuffer(*_device, 3, RHI::BufferUsage::Indirect);
		RHI::BufferHandle values = MakeKernelBuffer(*_device, COUNT);
		RHI::CommandList* cmd = _device->BeginFrame();
		cmd->TransitionBuffer(arguments, RHI::ResourceState::UnorderedAccess);
		BindKernel(*_device, *cmd, pipeline, arguments, 12, {2, groups});
		cmd->Dispatch(1);
		cmd->TransitionBuffer(arguments, RHI::ResourceState::IndirectArgument);
		cmd->TransitionBuffer(values, RHI::ResourceState::UnorderedAccess);
		BindKernel(*_device, *cmd, pipeline, values, COUNT * 4, {0, COUNT, 1});
		cmd->DispatchIndirect(arguments);
		_device->EndFrame();
		_device->WaitIdle();

		Vector<u32> out = ReadValues(*_device, values, COUNT);
		const u32 ran = groups * 64;
		u32 wrong = 0;
		for (u32 i = 0; i < COUNT; ++i) {
			wrong += out[i] == (i < ran ? 1 + i : 0u) ? 0u : 1u;
		}
		EXPECT_EQ(wrong, 0u) << groups << ' ' << ran - (ran > 0 ? 1 : 0) << " is " << out[ran > 0 ? ran - 1 : 0] << ", element " << ran << " is " << out[ran];
		_device->DestroyResource(arguments);
		_device->DestroyResource(values);
	}
	_device->DestroyResource(pipeline);
	FlushDeferredDeletes(*_device);
}

TEST_F(RHIFixture, IndirectDrawRunsVertexCountKernelWrote) {
	constexpr i32 WIDTH = 16;
	constexpr i32 HEIGHT = 4;
	ShaderAsset kernel;
	ShaderAsset columns;
	if (!ImportFixtureShader(kernel, "ComputeFixture.hlsl") || !ImportFixtureShader(columns, "IndirectColumns.hlsl")) {
		GTEST_SKIP() << "dxcompiler.dll unavailable and shader cache cold";
	}
	RHI::PipelineHandle kernelPipeline = MakeKernelPipeline(*_device, kernel);
	RHI::PipelineHandle drawPipeline = MakeFixturePipeline(*_device, columns, "IndirectColumns", RHI::Format::RGBA8Unorm,
														   RHI::Format::Unknown, 1);
	ASSERT_TRUE(kernelPipeline.IsValid());
	ASSERT_TRUE(drawPipeline.IsValid());

	RHI::TextureDesc targetDesc;
	targetDesc.size = {WIDTH, HEIGHT};
	targetDesc.format = RHI::Format::RGBA8Unorm;
	targetDesc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::TransferSrc;
	targetDesc.debugName = "IndirectColumns";
	RHI::TextureHandle target = _device->CreateTextureResource(targetDesc);
	ASSERT_TRUE(target.IsValid());

	for (u32 drawn : {5u, 0u}) {
		RHI::BufferHandle arguments = MakeKernelBuffer(*_device, 4, RHI::BufferUsage::Indirect);
		RHI::CommandList* cmd = _device->BeginFrame();
		cmd->TransitionBuffer(arguments, RHI::ResourceState::UnorderedAccess);
		BindKernel(*_device, *cmd, kernelPipeline, arguments, 16, {3, drawn * 6});
		cmd->Dispatch(1);
		cmd->TransitionBuffer(arguments, RHI::ResourceState::IndirectArgument);
		cmd->TransitionTexture(target, RHI::ResourceState::RenderTarget);
		RHI::RenderPassDesc::Attachment attachment;
		attachment.texture = target;
		attachment.load = RHI::LoadOp::Clear;
		attachment.clearColor = Color::Black();
		RHI::RenderPassDesc pass;
		pass.colorAttachments = {&attachment, 1};
		cmd->BeginRenderPass(pass);
		cmd->SetPipeline(drawPipeline);
		cmd->DrawIndirect(arguments);
		cmd->EndRenderPass();
		_device->EndFrame();
		_device->WaitIdle();

		Vector<u8> pixels = MakePixelBuffer(WIDTH, HEIGHT);
		_device->ReadbackTexture(target, {pixels.data(), pixels.size()});
		u32 wrong = 0;
		for (i32 x = 0; x < WIDTH; ++x) {
			const u8 expected = static_cast<u32>(x) < drawn ? 255 : 0;
			wrong += RedAt(pixels, WIDTH, x, HEIGHT / 2) == expected ? 0u : 1u;
		}
		EXPECT_EQ(wrong, 0u) << drawn << ' ' << (drawn > 0 ? drawn - 1 : 0) << " reads " << static_cast<u32>(RedAt(pixels, WIDTH, drawn > 0 ? static_cast<i32>(drawn) - 1 : 0, HEIGHT / 2)) << ", column " << drawn << " reads " << static_cast<u32>(RedAt(pixels, WIDTH, static_cast<i32>(drawn), HEIGHT / 2));
		_device->DestroyResource(arguments);
	}
	_device->DestroyResource(target);
	_device->DestroyResource(drawPipeline);
	_device->DestroyResource(kernelPipeline);
	FlushDeferredDeletes(*_device);
}

TEST_F(RHIFixture, BarrierOrdersKernelWrites) {
	ShaderAsset kernel;
	if (!ImportFixtureShader(kernel, "ComputeFixture.hlsl")) {
		GTEST_SKIP() << "dxcompiler.dll unavailable and shader cache cold";
	}
	RHI::PipelineHandle pipeline = MakeKernelPipeline(*_device, kernel);
	ASSERT_TRUE(pipeline.IsValid());
	constexpr u32 COUNT = 1u << 16;
	RHI::BufferHandle values = MakeKernelBuffer(*_device, COUNT);

	RHI::CommandList* cmd = _device->BeginFrame();
	cmd->TransitionBuffer(values, RHI::ResourceState::UnorderedAccess);
	BindKernel(*_device, *cmd, pipeline, values, COUNT * 4, {0, COUNT, 1, 2000});
	cmd->Dispatch(COUNT / 64);
	cmd->UnorderedAccessBarrier(values);
	BindKernel(*_device, *cmd, pipeline, values, COUNT * 4, {1, COUNT});
	cmd->Dispatch(COUNT / 64);
	_device->EndFrame();
	_device->WaitIdle();

	Vector<u32> out = ReadValues(*_device, values, COUNT);
	u32 wrong = 0;
	for (u32 i = 0; i < COUNT; ++i) {
		wrong += out[i] == 2 * (1 + i) ? 0u : 1u;
	}
	EXPECT_EQ(wrong, 0u) << "of " << COUNT << ' ' << out[0];

	_device->DestroyResource(pipeline);
	_device->DestroyResource(values);
	FlushDeferredDeletes(*_device);
}

TEST(RHIValidationTest, LoadedLayerIsLogged) {
	JobSystem::Initialize();
	LogManager::Initialize({false, false});
	LogManager::SetGlobalLevel(LogLevel::Trace);
	auto sink = std::make_shared<LayerLineSink>();
	LogManager::AddSink(sink);
	for (int attempt = 0; attempt < 200 && !sink->Armed(); ++attempt) {
		SLT_LOG_INFO("Graphics", "{}", LayerLineSink::SENTINEL);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	const bool armed = sink->Armed();

	RHI::DeviceDesc desc;
	desc.headless = true;
	desc.enableValidation = true;
	RHI::Device* validated = RHI::Device::Create(desc);
	const bool layerLoaded = TestPlatform::FindLoadedLibrary("VkLayer_khronos_validation" SLT_TEST_DLL_SUFFIX) != nullptr;
	if (validated) {
		RHI::Device::Destroy(validated);
	}
	desc.enableValidation = false;
	RHI::Device* plain = RHI::Device::Create(desc);
	if (plain) {
		RHI::Device::Destroy(plain);
	}

	LogManager::Terminate();
	JobSystem::Terminate();
	ASSERT_TRUE(armed);
	if (!validated || !plain) {
		GTEST_SKIP() << "no Vulkan ICD available";
	}
	ASSERT_EQ(sink->CountMatching("Selected adapter"), 2u);
	EXPECT_EQ(sink->CountMatching("validation layer on"), layerLoaded ? 1u : 0u);
}

TEST(RHISwapChainTest, CreateResizeDestroyAgainstHiddenWindow) {
	RHI::DeviceDesc deviceDesc;
	RHI::Device* device = RHI::Device::Create(deviceDesc);
	if (!device) {
		GTEST_SKIP() << "no Vulkan device that can present";
	}

	WindowSystem::Initialize();
	{
		WindowDesc windowDesc;
		windowDesc.title = "RHISwapChainTest";
		windowDesc.clientSize = {320, 240};
		windowDesc.flags = WindowFlags::Resizable | WindowFlags::Hidden;
		Handle<Window> window = WindowSystem::Create(windowDesc);
		ASSERT_TRUE(window.IsValid());

		NativeWindowHandle native = window.Get()->GetNativeHandle();
		RHI::NativeSwapChainDesc swapDesc;
		swapDesc.window = native;
		swapDesc.sizePx = window.Get()->GetClientSize();
		RHI::SwapChain* swapChain = device->CreateSwapChainForWindow(swapDesc);
		ASSERT_NE(swapChain, nullptr);
		EXPECT_NE(swapChain->GetFormat(), RHI::Format::Unknown);

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
		if (backbuffer.IsValid()) {
			swapChain->Present();
		}

		swapChain->Recreate({256, 192}, true);
		device->DestroySwapChain(swapChain);

		WindowSystem::RequestDestroy(window);
		WindowSystem::PumpEvents();
	}
	WindowSystem::Terminate();
	RHI::Device::Destroy(device);
}

TEST(RHISwapChainTest, AcquiredImageBarrierWaitsForAcquire) {
	ScopedLogCapture capture;
	ASSERT_TRUE(capture.Armed());

	const str enables = TestPlatform::GetEnvironment("VK_LAYER_ENABLES");
	TestPlatform::SetEnvironment("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT");
	RHI::DeviceDesc deviceDesc;
	deviceDesc.enableValidation = true;
	RHI::Device* device = RHI::Device::Create(deviceDesc);
	TestPlatform::SetEnvironment("VK_LAYER_ENABLES", enables);
	if (!device) {
		GTEST_SKIP() << "no Vulkan device that can present";
	}
	if (!TestPlatform::FindLoadedLibrary("VkLayer_khronos_validation" SLT_TEST_DLL_SUFFIX)) {
		RHI::Device::Destroy(device);
		GTEST_SKIP() << "validation layer not installed";
	}

	RHI::BufferDesc bufferDesc;
	bufferDesc.size = 256;
	bufferDesc.usage = RHI::BufferUsage::TransferSrc | RHI::BufferUsage::TransferDst;
	const RHI::BufferHandle source = device->CreateBufferResource(bufferDesc);
	const RHI::BufferHandle target = device->CreateBufferResource(bufferDesc);
	RHI::CommandList* control = device->BeginFrame();
	control->TransitionBuffer(source, RHI::ResourceState::TransferSrc);
	control->TransitionBuffer(target, RHI::ResourceState::TransferDst);
	control->CopyBufferRegion(source, target, 0, 0, bufferDesc.size);
	control->CopyBufferRegion(source, target, 0, 0, bufferDesc.size);
	device->EndFrame();
	device->WaitIdle();
	const bool controlSettled = capture.Settle("rhi-hazard-control");
	const u32 controlHazards = capture.CountMatching("WRITE_AFTER_WRITE");

	u32 presented = 0;
	WindowSystem::Initialize();
	{
		WindowDesc windowDesc;
		windowDesc.title = "RHIAcquireSyncTest";
		windowDesc.clientSize = {160, 120};
		windowDesc.flags = WindowFlags::Hidden;
		Handle<Window> window = WindowSystem::Create(windowDesc);
		RHI::SwapChain* swapChain = nullptr;
		if (window.IsValid()) {
			RHI::NativeSwapChainDesc swapDesc;
			swapDesc.window = window.Get()->GetNativeHandle();
			swapDesc.sizePx = window.Get()->GetClientSize();
			swapChain = device->CreateSwapChainForWindow(swapDesc);
		}
		for (u32 frame = 0; swapChain && frame < 4; ++frame) {
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
			if (backbuffer.IsValid()) {
				swapChain->Present();
				++presented;
			}
		}
		device->WaitIdle();
		if (swapChain) {
			device->DestroySwapChain(swapChain);
		}
		if (window.IsValid()) {
			WindowSystem::RequestDestroy(window);
			WindowSystem::PumpEvents();
		}
	}
	WindowSystem::Terminate();
	device->DestroyResource(source);
	device->DestroyResource(target);
	RHI::Device::Destroy(device);
	const bool framesSettled = capture.Settle("rhi-hazard-frames");

	ASSERT_TRUE(controlSettled);
	ASSERT_TRUE(framesSettled);
	if (controlHazards == 0) {
		GTEST_SKIP() << "synchronization validation not active";
	}
	ASSERT_GT(presented, 0u);
	EXPECT_EQ(capture.CountMatching("WRITE_AFTER_READ"), 0u);
}

TEST_F(RHIFixture, InlineMSAAResolveWritesResolvedColor) {
	if ((_device->GetSupportedSampleCounts(RHI::Format::RGBA8Unorm, RHI::TextureUsage::RenderTarget) & 4u) == 0) {
		GTEST_SKIP() << "4x MSAA not supported for RGBA8Unorm";
	}
	RHI::TextureDesc msaaDesc;
	msaaDesc.size = {8, 8};
	msaaDesc.format = RHI::Format::RGBA8Unorm;
	msaaDesc.usage = RHI::TextureUsage::RenderTarget;
	msaaDesc.samples = 4;
	msaaDesc.debugName = "InlineResolveMS";
	RHI::TextureHandle msaa = _device->CreateTextureResource(msaaDesc);
	ASSERT_TRUE(msaa.IsValid());

	RHI::TextureDesc resolveDesc;
	resolveDesc.size = {8, 8};
	resolveDesc.format = RHI::Format::RGBA8Unorm;
	resolveDesc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::TransferSrc;
	resolveDesc.debugName = "InlineResolveTarget";
	RHI::TextureHandle resolved = _device->CreateTextureResource(resolveDesc);
	ASSERT_TRUE(resolved.IsValid());

	RHI::CommandList* cmd = _device->BeginFrame();
	ASSERT_NE(cmd, nullptr);
	cmd->TransitionTexture(msaa, RHI::ResourceState::RenderTarget);
	cmd->TransitionTexture(resolved, RHI::ResourceState::RenderTarget);

	RHI::RenderPassDesc::Attachment color;
	color.texture = msaa;
	color.load = RHI::LoadOp::Clear;
	color.store = RHI::StoreOp::DontCare;
	color.clearColor = Color(0.25f, 0.5f, 0.75f, 1.0f);
	RHI::RenderPassDesc pass;
	pass.colorAttachments = {&color, 1};
	pass.resolveTargets = {&resolved, 1};
	cmd->BeginRenderPass(pass);
	cmd->EndRenderPass();
	cmd->TransitionTexture(resolved, RHI::ResourceState::TransferSrc);

	_device->EndFrame();
	_device->WaitIdle();

	Vector<u8> pixels = MakePixelBuffer(8, 8);
	_device->ReadbackTexture(resolved, pixels);
	EXPECT_NEAR(pixels[0], 64, 2);
	EXPECT_NEAR(pixels[1], 128, 2);
	EXPECT_NEAR(pixels[2], 191, 2);
	EXPECT_EQ(pixels[3], 255);

	_device->DestroyResource(resolved);
	_device->DestroyResource(msaa);
}
