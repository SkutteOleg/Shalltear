#include "TestsPCH.h"

#include "Graphics/EnvironmentIBL.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Graphics/RHICommandList.h"
#include "Shalltear/Core/Graphics/RHIDevice.h"
#include "Shalltear/Core/Graphics/ShaderAsset.h"
#include "Shalltear/Core/Math/HalfFloat.h"

#include <cmath>
#include <filesystem>

using namespace Shalltear;
using Shalltear::GraphicsInternal::RadianceCube;

namespace {

	std::filesystem::path FindRepoDir(const char* relative) {
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

	Vec3 SmoothSky(const Vec3& d) {
		return Vec3(0.3f + 0.25f * d.x, 0.35f + 0.3f * d.y, 0.4f - 0.2f * d.z);
	}

	Vec3 LobedSky(const Vec3& d) {
		const Vec3 lobe = Vec3(0.48f, 0.6f, -0.64f).Normalized();
		const f32 spot = std::pow(Math::Max(Vec3::Dot(d, lobe), 0.0f), 48.0f) * 6.0f;
		return SmoothSky(d) + Vec3(spot, spot * 0.8f, spot * 0.5f);
	}

	constexpr u32 FACE = 64;

	struct PrefilterPush {
		u32 face = 0;
		f32 roughness = 0.0f;
		f32 sourceFaceSize = 0.0f;
		f32 sourceMipCount = 0.0f;
		f32 targetSizeX = 0.0f;
		f32 targetSizeY = 0.0f;
	};

	struct LevelError {
		f64 mean = 0.0;
		f32 worst = 0.0f;
	};

	class SkyPrefilterFixture : public ::testing::Test {
	protected:
		RHI::Device* _device = nullptr;
		ShaderAsset _shader;
		bool _compiled = false;

		void SetUp() override {
			RHI::DeviceDesc desc;
			desc.headless = true;
			_device = RHI::Device::Create(desc);
			if (!_device) {
				GTEST_SKIP() << "no Vulkan ICD available";
			}
			const std::filesystem::path source = FindRepoDir("EngineAssets/Shaders/SkyPrefilter.hlsl");
			ASSERT_FALSE(source.empty());
			_compiled = _shader.Import(Path(source.generic_string()));
		}

		void TearDown() override {
			if (_device) {
				_device->WaitIdle();
				RHI::Device::Destroy(_device);
			}
		}

		bool Measure(Vec3 (*sky)(const Vec3&), Vector<LevelError>& out) {
			RadianceCube expected;
			expected.faceSize = FACE;
			expected.mipCount = GraphicsInternal::RadianceCubeMipCount(FACE);
			expected.pixels.resize(static_cast<size_t>(GraphicsInternal::RadianceCubeBytes(FACE, expected.mipCount)));
			for (u32 face = 0; face < GraphicsInternal::CUBE_FACE_COUNT; ++face) {
				u16* texels = reinterpret_cast<u16*>(expected.pixels.data() + GraphicsInternal::RadianceCubeFaceOffset(FACE, expected.mipCount, 0, face));
				for (u32 y = 0; y < FACE; ++y) {
					for (u32 x = 0; x < FACE; ++x) {
						const Vec3 color = sky(GraphicsInternal::CubeTexelDirection(face, (static_cast<f32>(x) + 0.5f) / FACE,
																					(static_cast<f32>(y) + 0.5f) / FACE));
						u16* texel = texels + (static_cast<size_t>(y) * FACE + x) * 4;
						texel[0] = Math::FloatToHalf(color.x);
						texel[1] = Math::FloatToHalf(color.y);
						texel[2] = Math::FloatToHalf(color.z);
						texel[3] = Math::FloatToHalf(1.0f);
					}
				}
			}
			const RadianceCube uploaded = expected;
			GraphicsInternal::IrradianceSH sh;
			GraphicsInternal::PrefilterRadianceCube(expected, sh);

			RHI::TextureDesc sourceDesc;
			sourceDesc.size = {static_cast<i32>(FACE), static_cast<i32>(FACE)};
			sourceDesc.format = RHI::Format::RGBA16Float;
			sourceDesc.arrayLayers = GraphicsInternal::CUBE_FACE_COUNT;
			sourceDesc.cube = true;
			sourceDesc.mipLevels = GraphicsInternal::BoxChainMipCount(FACE);
			sourceDesc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferSrc | RHI::TextureUsage::TransferDst;
			sourceDesc.debugName = "OracleSource";
			RHI::TextureHandle sourceCube = _device->CreateTextureResource(sourceDesc);
			RHI::TextureDesc chainDesc = sourceDesc;
			chainDesc.mipLevels = expected.mipCount;
			chainDesc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferSrc;
			chainDesc.debugName = "OracleChain";
			RHI::TextureHandle chain = _device->CreateTextureResource(chainDesc);
			RHI::SamplerDesc samplerDesc;
			samplerDesc.addressMode = RHI::AddressMode::ClampToEdge;
			RHI::SamplerHandle sampler = _device->CreateSamplerResource(samplerDesc);
			if (!sourceCube.IsValid() || !chain.IsValid() || !sampler.IsValid()) {
				return false;
			}
			const u64 faceBytes = static_cast<u64>(FACE) * FACE * 8;
			for (u32 face = 0; face < GraphicsInternal::CUBE_FACE_COUNT; ++face) {
				_device->UploadTexture(sourceCube,
									   {uploaded.pixels.data() + GraphicsInternal::RadianceCubeFaceOffset(FACE, uploaded.mipCount, 0, face),
										static_cast<size_t>(faceBytes)},
									   0, face);
			}

			const ShaderVariant& code = _shader.variants[SHADER_BASE_VARIANT];
			RHI::GraphicsPipelineDesc pipelineDesc;
			pipelineDesc.vertexSpirv = {code.vertexSpirv.data(), code.vertexSpirv.size()};
			pipelineDesc.pixelSpirv = {code.pixelSpirv.data(), code.pixelSpirv.size()};
			pipelineDesc.bytecodeFormat = _shader.bytecodeFormat;
			pipelineDesc.bindings = {_shader.bindings.data(), _shader.bindings.size()};
			pipelineDesc.pushConstantSize = _shader.pushConstantSize;
			pipelineDesc.blend = _shader.renderState.blend;
			pipelineDesc.depth = _shader.renderState.depth;
			pipelineDesc.cull = _shader.renderState.cull;
			pipelineDesc.colorFormats[0] = RHI::Format::RGBA16Float;
			pipelineDesc.colorFormatCount = 1;
			pipelineDesc.debugName = "OraclePrefilter";
			RHI::PipelineHandle pipeline = _device->CreateGraphicsPipeline(pipelineDesc);
			if (!pipeline.IsValid() || _shader.pushConstantSize != sizeof(PrefilterPush)) {
				return false;
			}

			RHI::CommandList* cmd = _device->BeginFrame();
			cmd->GenerateMips(sourceCube);
			cmd->TransitionTexture(chain, RHI::ResourceState::RenderTarget);
			RHI::TransientSetWriter writer = _device->CreateTransientSet(pipeline, 0);
			writer.WriteTexture(0, sourceCube);
			writer.WriteSampler(1, sampler);
			const RHI::TransientSet set = writer.Commit();
			for (u32 level = 0; level < expected.mipCount; ++level) {
				const u32 side = FACE >> level;
				for (u32 face = 0; face < GraphicsInternal::CUBE_FACE_COUNT; ++face) {
					RHI::RenderPassDesc::Attachment attachment;
					attachment.texture = chain;
					attachment.layer = face;
					attachment.mip = level;
					attachment.load = RHI::LoadOp::DontCare;
					RHI::RenderPassDesc pass;
					pass.colorAttachments = {&attachment, 1};
					cmd->BeginRenderPass(pass);
					cmd->SetPipeline(pipeline);
					cmd->BindTransientSet(0, set);
					PrefilterPush push;
					push.face = face;
					push.roughness = static_cast<f32>(level) / static_cast<f32>(expected.mipCount - 1);
					push.sourceFaceSize = static_cast<f32>(FACE);
					push.sourceMipCount = static_cast<f32>(sourceDesc.mipLevels);
					push.targetSizeX = static_cast<f32>(side);
					push.targetSizeY = static_cast<f32>(side);
					cmd->PushConstants({reinterpret_cast<const u8*>(&push), sizeof(push)});
					cmd->Draw(3);
					cmd->EndRenderPass();
				}
			}
			_device->EndFrame();
			_device->WaitIdle();

			out.clear();
			for (u32 level = 0; level < expected.mipCount; ++level) {
				const u32 side = FACE >> level;
				f64 sumError = 0.0;
				f64 sumValue = 0.0;
				LevelError error;
				for (u32 face = 0; face < GraphicsInternal::CUBE_FACE_COUNT; ++face) {
					Vector<u8> pixels{GetMemoryResource(MemTag::Graphics)};
					pixels.assign(static_cast<size_t>(side) * side * 8, 0);
					_device->ReadbackTexture(chain, {pixels.data(), pixels.size()}, level, face);
					const u16* gpu = reinterpret_cast<const u16*>(pixels.data());
					const u16* cpu = reinterpret_cast<const u16*>(expected.pixels.data() +
																  GraphicsInternal::RadianceCubeFaceOffset(FACE, expected.mipCount, level, face));
					for (u32 texel = 0; texel < side * side; ++texel) {
						for (u32 channel = 0; channel < 3; ++channel) {
							const f32 a = Math::HalfToFloat(gpu[texel * 4 + channel]);
							const f32 b = Math::HalfToFloat(cpu[texel * 4 + channel]);
							sumError += std::fabs(a - b);
							sumValue += b;
							error.worst = Math::Max(error.worst, std::fabs(a - b) / Math::Max(b, 0.05f));
						}
					}
				}
				error.mean = sumError / Math::Max(sumValue, 1e-6);
				out.push_back(error);
			}

			_device->DestroyResource(pipeline);
			_device->DestroyResource(sampler);
			_device->DestroyResource(chain);
			_device->DestroyResource(sourceCube);
			for (int i = 0; i < 2; ++i) {
				_device->BeginFrame();
				_device->EndFrame();
			}
			return true;
		}
	};

} // anonymous namespace

TEST_F(SkyPrefilterFixture, SmoothSkyMatchesOracle) {
	if (!_compiled) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	Vector<LevelError> levels{GetMemoryResource(MemTag::Graphics)};
	ASSERT_TRUE(Measure(&SmoothSky, levels));
	ASSERT_EQ(levels.size(), static_cast<size_t>(GraphicsInternal::RadianceCubeMipCount(FACE)));
	EXPECT_EQ(levels[0].worst, 0.0f);
	for (size_t level = 0; level < levels.size(); ++level) {
		EXPECT_LT(levels[level].mean, 0.005) << "level " << level << ": mean difference " << levels[level].mean;
		EXPECT_LT(levels[level].worst, 0.03f) << "level " << level << ": worst texel " << levels[level].worst;
	}
}

TEST_F(SkyPrefilterFixture, LobedSkyNearOracle) {
	if (!_compiled) {
		GTEST_SKIP() << "dxcompiler.dll unavailable";
	}
	Vector<LevelError> levels{GetMemoryResource(MemTag::Graphics)};
	ASSERT_TRUE(Measure(&LobedSky, levels));
	ASSERT_EQ(levels.size(), static_cast<size_t>(GraphicsInternal::RadianceCubeMipCount(FACE)));
	for (size_t level = 0; level < levels.size(); ++level) {
		EXPECT_LT(levels[level].mean, 0.02) << "level " << level << ": mean difference " << levels[level].mean;
		EXPECT_LT(levels[level].worst, 0.15f) << "level " << level << ": worst texel " << levels[level].worst;
	}
}
