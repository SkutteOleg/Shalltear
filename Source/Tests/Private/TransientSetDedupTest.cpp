#include "TestsPCH.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Graphics.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Window.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

using namespace Shalltear;

namespace {

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

	class GuardSink : public ILogSink {
	public:
		static constexpr const char* SENTINEL = "guard-sink-sentinel";

		void ReceiveLog(const LogEvent& event) override {
			std::lock_guard lock(_mutex);
			if (event.message.find(SENTINEL) != str::npos) {
				_sentinelSeen = true;
				return;
			}
			if (event.category == "Vulkan") {
				++_validationErrors;
				if (_firstValidation.empty()) {
					_firstValidation = event.message;
				}
			}
			if (event.message.find("not ShaderResource") != str::npos) {
				++_barrierReports;
			}
			if (event.message.find("isn't a multiple of") != str::npos) {
				++_alignmentReports;
			}
		}
		void Flush() override {}

		bool SentinelSeen() const {
			std::lock_guard lock(_mutex);
			return _sentinelSeen;
		}
		u32 ValidationErrors() const {
			std::lock_guard lock(_mutex);
			return _validationErrors;
		}
		str FirstValidation() const {
			std::lock_guard lock(_mutex);
			return _firstValidation;
		}
		u32 BarrierReports() const {
			std::lock_guard lock(_mutex);
			return _barrierReports;
		}
		u32 AlignmentReports() const {
			std::lock_guard lock(_mutex);
			return _alignmentReports;
		}
	private:
		mutable std::mutex _mutex;
		u32 _validationErrors = 0;
		u32 _barrierReports = 0;
		u32 _alignmentReports = 0;
		bool _sentinelSeen = false;
		str _firstValidation;
	};

	class ScopedGuardWatch {
	public:
		ScopedGuardWatch() : _sink(std::make_shared<GuardSink>()) {
			_ownsJobSystem = JobSystem::GetWorkerCount() == 0;
			if (_ownsJobSystem) {
				JobSystem::Initialize();
			}
			LogManager::Initialize({false, false});
			LogManager::SetGlobalLevel(LogLevel::Trace);
			LogManager::AddSink(_sink);
		}
		~ScopedGuardWatch() {
			LogManager::RemoveSink(_sink);
			LogManager::Terminate();
			if (_ownsJobSystem) {
				JobSystem::Terminate();
			}
		}

		bool Settle(f64 timeoutSeconds = 5.0) {
			SLT_LOG_ERROR("Vulkan", "{}", GuardSink::SENTINEL);
			auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<f64>(timeoutSeconds);
			while (std::chrono::steady_clock::now() < deadline) {
				if (_sink->SentinelSeen()) {
					return true;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			return false;
		}

		u32 ValidationErrors() const { return _sink->ValidationErrors(); }
		str FirstValidation() const { return _sink->FirstValidation(); }
		u32 BarrierReports() const { return _sink->BarrierReports(); }
		u32 AlignmentReports() const { return _sink->AlignmentReports(); }
	private:
		std::shared_ptr<GuardSink> _sink;
		bool _ownsJobSystem = false;
	};

	class TransientSetFixture : public ::testing::Test {
	protected:
		bool _mounted = false;
		RHI::Device* _device = nullptr;
		RHI::PipelineHandle _pipeline{};
		RHI::PipelineHandle _litPipeline{};
		RHI::TextureHandle _textureA{};
		RHI::TextureHandle _textureB{};
		RHI::TextureHandle _target{};
		RHI::SamplerHandle _samplerA{};
		RHI::SamplerHandle _samplerB{};
		Asset* _shaderAsset = nullptr;
		Asset* _litAsset = nullptr;
		Vector<RHI::TextureHandle> _textures{GetMemoryResource(MemTag::Graphics)};

		void SetUp() override {
			std::filesystem::path engineAssets = RepoDir("EngineAssets");
			if (!engineAssets.empty()) {
				FileSystem::Mount("engine", Path(engineAssets.generic_string()));
				_mounted = true;
			}
			AssetDatabase::Initialize();
			WindowSystem::Initialize();
			RenderSystem::Initialize();
			if (!RenderSystem::IsRenderingAvailable()) {
				GTEST_SKIP() << "no Vulkan device available";
			}
			_device = RenderSystem::GetDevice();

			_shaderAsset = AssetDatabase::LoadAsset(Path("engine://Shaders/UI.hlsl"));
			_litAsset = AssetDatabase::LoadAsset(Path("engine://Shaders/Lit.hlsl"));
			if (!_shaderAsset || _shaderAsset->GetAssetTypeId() != StringHash("ShaderAsset") ||
				!_litAsset || _litAsset->GetAssetTypeId() != StringHash("ShaderAsset")) {
				GTEST_SKIP() << "engine shaders unavailable (dxcompiler not staged)";
			}
			_pipeline = MakePipeline(static_cast<ShaderAsset*>(_shaderAsset), "TransientSetDedupUIPipeline", RHI::Format::Unknown);
			_litPipeline = MakePipeline(static_cast<ShaderAsset*>(_litAsset), "TransientSetDedupLitPipeline", RHI::Format::D32Float);
			ASSERT_TRUE(_pipeline.IsValid());
			ASSERT_TRUE(_litPipeline.IsValid());

			_textureA = MakeSampledTexture(0xFFu);
			_textureB = MakeSampledTexture(0x40u);
			RHI::TextureDesc targetDesc;
			targetDesc.size = {16, 16};
			targetDesc.format = RHI::Format::RGBA16Float;
			targetDesc.usage = RHI::TextureUsage::RenderTarget;
			targetDesc.debugName = "TransientSetDedupTarget";
			_target = _device->CreateTextureResource(targetDesc);
			_textures.push_back(_target);
			RHI::SamplerDesc samplerDesc;
			_samplerA = _device->CreateSamplerResource(samplerDesc);
			samplerDesc.filter = RHI::FilterMode::Nearest;
			_samplerB = _device->CreateSamplerResource(samplerDesc);
		}

		void TearDown() override {
			if (_device) {
				_device->WaitIdle();
				for (RHI::TextureHandle texture : _textures) {
					_device->DestroyResource(texture);
				}
				_textures.clear();
				if (_samplerA.IsValid()) {
					_device->DestroyResource(_samplerA);
				}
				if (_samplerB.IsValid()) {
					_device->DestroyResource(_samplerB);
				}
				if (_pipeline.IsValid()) {
					_device->DestroyResource(_pipeline);
				}
				if (_litPipeline.IsValid()) {
					_device->DestroyResource(_litPipeline);
				}
				for (int i = 0; i < 3; ++i) {
					_device->BeginFrame();
					_device->EndFrame();
				}
				_device->WaitIdle();
			}
			if (_shaderAsset) {
				AssetDatabase::UnloadAsset(_shaderAsset);
				_shaderAsset = nullptr;
			}
			if (_litAsset) {
				AssetDatabase::UnloadAsset(_litAsset);
				_litAsset = nullptr;
			}
			AssetDatabase::Terminate();
			RenderSystem::Terminate();
			WindowSystem::Terminate();
			if (_mounted) {
				FileSystem::Unmount("engine");
			}
		}

		RHI::PipelineHandle MakePipeline(ShaderAsset* shader, const char* debugName, RHI::Format depthFormat) {
			RHI::GraphicsPipelineDesc desc;
			desc.vertexSpirv = {shader->variants[SHADER_BASE_VARIANT].vertexSpirv.data(), shader->variants[SHADER_BASE_VARIANT].vertexSpirv.size()};
			desc.pixelSpirv = {shader->variants[SHADER_BASE_VARIANT].pixelSpirv.data(), shader->variants[SHADER_BASE_VARIANT].pixelSpirv.size()};
			desc.bytecodeFormat = shader->bytecodeFormat;
			desc.bindings = {shader->bindings.data(), shader->bindings.size()};
			desc.pushConstantSize = shader->pushConstantSize;
			desc.blend = shader->renderState.blend;
			desc.depth = shader->renderState.depth;
			desc.cull = shader->renderState.cull;
			desc.colorFormats[0] = RHI::Format::RGBA16Float;
			desc.colorFormatCount = 1;
			desc.depthFormat = depthFormat;
			desc.debugName = debugName;
			return _device->CreateGraphicsPipeline(desc);
		}

		RHI::TextureHandle MakeSampledTexture(u8 value) {
			RHI::TextureDesc desc;
			desc.size = {1, 1};
			desc.format = RHI::Format::RGBA8Unorm;
			desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst;
			desc.debugName = "TransientSetDedupTexture";
			RHI::TextureHandle texture = _device->CreateTextureResource(desc);
			const u8 pixel[4] = {value, value, value, 255};
			_device->UploadTexture(texture, pixel);
			_textures.push_back(texture);
			return texture;
		}

		void BindInPass(RHI::CommandList& cmd, RHI::TransientSet set) {
			RHI::RenderPassDesc::Attachment color;
			color.texture = _target;
			color.load = RHI::LoadOp::Clear;
			RHI::RenderPassDesc pass;
			pass.colorAttachments = {&color, 1};
			cmd.TransitionTexture(_target, RHI::ResourceState::RenderTarget);
			cmd.BeginRenderPass(pass);
			cmd.SetViewportScissor({0, 0, 16, 16});
			cmd.SetPipeline(_pipeline);
			cmd.BindTransientSet(1, set);
			cmd.EndRenderPass();
		}

		RHI::TransientSet CommitSet(RHI::TextureHandle texture, RHI::SamplerHandle sampler) {
			return _device->CreateTransientSet(_pipeline, 1)
				.WriteTexture(1, texture)
				.WriteSampler(2, sampler)
				.Commit();
		}
	};

} // anonymous namespace

TEST_F(TransientSetFixture, IdenticalContentCommitsOnce) {
	_device->BeginFrame();
	RHI::TransientSet first = CommitSet(_textureA, _samplerA);
	RHI::TransientSet second = CommitSet(_textureA, _samplerA);
	ASSERT_TRUE(first.IsValid() && second.IsValid());
	EXPECT_EQ(first.opaque, second.opaque);
	_device->EndFrame();
	_device->WaitIdle();
}

TEST_F(TransientSetFixture, AlternatingContentCommitsTwoSetsNotSix) {
	_device->BeginFrame();
	RHI::TransientSet a1 = CommitSet(_textureA, _samplerA);
	RHI::TransientSet b1 = CommitSet(_textureB, _samplerA);
	RHI::TransientSet a2 = CommitSet(_textureA, _samplerA);
	RHI::TransientSet b2 = CommitSet(_textureB, _samplerA);
	RHI::TransientSet a3 = CommitSet(_textureA, _samplerA);
	RHI::TransientSet b3 = CommitSet(_textureB, _samplerA);
	EXPECT_EQ(a1.opaque, a2.opaque);
	EXPECT_EQ(a1.opaque, a3.opaque);
	EXPECT_EQ(b1.opaque, b2.opaque);
	EXPECT_EQ(b1.opaque, b3.opaque);
	EXPECT_NE(a1.opaque, b1.opaque);
	_device->EndFrame();
	_device->WaitIdle();
}

TEST_F(TransientSetFixture, ADifferentTextureIsADifferentSet) {
	_device->BeginFrame();
	RHI::TransientSet a = CommitSet(_textureA, _samplerA);
	RHI::TransientSet b = CommitSet(_textureB, _samplerA);
	ASSERT_TRUE(a.IsValid() && b.IsValid());
	EXPECT_NE(a.opaque, b.opaque);
	_device->EndFrame();
	_device->WaitIdle();
}

TEST_F(TransientSetFixture, ADifferentSamplerIsADifferentSet) {
	_device->BeginFrame();
	RHI::TransientSet linear = CommitSet(_textureA, _samplerA);
	RHI::TransientSet nearest = CommitSet(_textureA, _samplerB);
	ASSERT_TRUE(linear.IsValid() && nearest.IsValid());
	EXPECT_NE(linear.opaque, nearest.opaque);
	_device->EndFrame();
	_device->WaitIdle();
}

TEST_F(TransientSetFixture, DifferentBufferRangeIsDifferentSet) {
	_device->BeginFrame();
	RHI::TransientAllocation first = _device->AllocateTransient(256);
	RHI::TransientAllocation second = _device->AllocateTransient(256);
	ASSERT_TRUE(first.IsValid() && second.IsValid());
	ASSERT_EQ(first.buffer.index, second.buffer.index);
	ASSERT_NE(first.offset, second.offset);
	RHI::TransientSet a = _device->CreateTransientSet(_litPipeline, 1)
							  .WriteUniformBuffer(0, first.buffer, first.offset, 256)
							  .WriteTexture(1, _textureA)
							  .WriteSampler(2, _samplerA)
							  .Commit();
	RHI::TransientSet b = _device->CreateTransientSet(_litPipeline, 1)
							  .WriteUniformBuffer(0, second.buffer, second.offset, 256)
							  .WriteTexture(1, _textureA)
							  .WriteSampler(2, _samplerA)
							  .Commit();
	ASSERT_TRUE(a.IsValid() && b.IsValid());
	EXPECT_NE(a.opaque, b.opaque);

	RHI::TransientSet again = _device->CreateTransientSet(_litPipeline, 1)
								  .WriteUniformBuffer(0, first.buffer, first.offset, 256)
								  .WriteTexture(1, _textureA)
								  .WriteSampler(2, _samplerA)
								  .Commit();
	EXPECT_EQ(a.opaque, again.opaque);
	_device->EndFrame();
	_device->WaitIdle();
}

TEST_F(TransientSetFixture, CacheDiesWithFrameThatFilledIt) {
	ScopedGuardWatch watch;

	for (int frame = 0; frame < 4; ++frame) {
		RHI::CommandList* cmd = _device->BeginFrame();
		ASSERT_NE(cmd, nullptr);
		RHI::TransientSet first = CommitSet(_textureA, _samplerA);
		RHI::TransientSet second = CommitSet(_textureA, _samplerA);
		ASSERT_TRUE(first.IsValid());
		EXPECT_EQ(first.opaque, second.opaque) << frame;
		BindInPass(*cmd, first);
		_device->EndFrame();
	}
	_device->WaitIdle();
	ASSERT_TRUE(watch.Settle());
	EXPECT_EQ(watch.ValidationErrors(), 0u) << "validation complained: " << watch.FirstValidation();
}

TEST_F(TransientSetFixture, BarrierGuardFiresEveryCommit) {
	RHI::TextureDesc desc;
	desc.size = {1, 1};
	desc.format = RHI::Format::RGBA8Unorm;
	desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst;
	desc.debugName = "TransientSetDedupUntransitioned";
	RHI::TextureHandle cold = _device->CreateTextureResource(desc);
	_textures.push_back(cold);
	ASSERT_EQ(_device->GetTextureState(cold), RHI::ResourceState::Undefined);

	ScopedGuardWatch watch;
	_device->BeginFrame();
	RHI::TransientSet first = CommitSet(cold, _samplerA);
	RHI::TransientSet second = CommitSet(cold, _samplerA);
	EXPECT_EQ(first.opaque, second.opaque);
	_device->EndFrame();
	_device->WaitIdle();

	ASSERT_TRUE(watch.Settle());
	EXPECT_EQ(watch.BarrierReports(), 2u);
}

TEST_F(TransientSetFixture, MisalignedBufferOffsetIsRefused) {
	ScopedGuardWatch watch;
	_device->BeginFrame();
	RHI::TransientAllocation block = _device->AllocateTransient(512);
	ASSERT_TRUE(block.IsValid());
	ASSERT_EQ(block.offset % RHI::BINDING_OFFSET_ALIGNMENT, 0u);
	auto commit = [&](u64 offset) {
		return _device->CreateTransientSet(_litPipeline, 1)
			.WriteUniformBuffer(0, block.buffer, offset, 256)
			.WriteTexture(1, _textureA)
			.WriteSampler(2, _samplerA)
			.Commit();
	};
	RHI::TransientSet head = commit(block.offset);
	RHI::TransientSet shifted = commit(block.offset + 16);
	RHI::TransientSet shiftedFurther = commit(block.offset + 32);
	RHI::TransientSet next = commit(block.offset + RHI::BINDING_OFFSET_ALIGNMENT);
	ASSERT_TRUE(head.IsValid() && shifted.IsValid() && shiftedFurther.IsValid() && next.IsValid());
	EXPECT_NE(head.opaque, next.opaque);
	EXPECT_NE(head.opaque, shifted.opaque);
	EXPECT_EQ(shifted.opaque, shiftedFurther.opaque);
	_device->EndFrame();
	_device->WaitIdle();

	ASSERT_TRUE(watch.Settle());
	EXPECT_EQ(watch.AlignmentReports(), 2u);
	EXPECT_EQ(watch.ValidationErrors(), 0u) << watch.FirstValidation();
}
