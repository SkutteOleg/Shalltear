#include "TestsPCH.h"

#include "UITestUtil.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Graphics.h"
#include "Shalltear/Core/Window.h"
#include "Shalltear/Core/Jobs/JobSystem.h"

#include "Shalltear/Editor/EditorTheme.h"

#include "../../Editor/Private/Panels/PanelHeader.h"

#include <bit>
#include <mutex>
#include <chrono>
#include <thread>
#include <cmath>
#include <filesystem>

using namespace Shalltear;
using namespace UITest;

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

	class UIRenderFixture : public ::testing::Test {
	protected:
		bool _mounted = false;

		void SetUp() override {
			std::filesystem::path engineAssets = RepoDir("EngineAssets");
			if (!engineAssets.empty()) {
				FileSystem::Mount("engine", Path(engineAssets.generic_string()));
				_mounted = true;
			}
			AssetDatabase::Initialize();
			WindowSystem::Initialize();
			RenderSystem::Initialize();
			UISystem::Initialize();
		}

		void TearDown() override {
			UISystem::Terminate();
			AssetDatabase::Terminate();
			RenderSystem::Terminate();
			WindowSystem::Terminate();
			if (_mounted) {
				FileSystem::Unmount("engine");
			}
		}

		Handle<Window> MakeHiddenWindow() {
			WindowDesc desc;
			desc.title = "UIRenderTest";
			desc.clientSize = {320, 240};
			desc.flags = WindowFlags::Resizable | WindowFlags::Hidden;
			return WindowSystem::Create(desc);
		}
	};

	class StaleTextureCapturingSink : public ILogSink {
	public:
		static constexpr const char* SENTINEL = "ui-render-sink-sentinel";

		void ReceiveLog(const LogEvent& event) override {
			std::lock_guard<std::mutex> lock(_mutex);
			if (event.message.find(SENTINEL) != str::npos) {
				_armed = true;
				return;
			}
			if (!_armed) {
				return;
			}
			_messages.push_back(str(event.message));
		}
		void Flush() override {}

		bool Armed() const {
			std::lock_guard<std::mutex> lock(_mutex);
			return _armed;
		}

		u32 CountMatching(std::string_view needle) const {
			std::lock_guard<std::mutex> lock(_mutex);
			u32 count = 0;
			for (const str& message : _messages) {
				if (message.find(needle) != str::npos) {
					++count;
				}
			}
			return count;
		}
	private:
		mutable std::mutex _mutex;
		std::vector<str> _messages;
		bool _armed = false;
	};

	class UIRenderLogFixture : public UIRenderFixture {
	protected:
		void SetUp() override {
			JobSystem::Initialize();
			LogManager::Initialize({false, false});
			UIRenderFixture::SetUp();
		}
		void TearDown() override {
			UIRenderFixture::TearDown();
			LogManager::Terminate();
			JobSystem::Terminate();
		}
	};

	bool ArmUIRenderSink(const std::shared_ptr<StaleTextureCapturingSink>& sink) {
		SLT_LOG_WARN("Graphics", "{}", StaleTextureCapturingSink::SENTINEL);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline) {
			if (sink->Armed()) {
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return false;
	}

} // anonymous namespace

TEST_F(UIRenderFixture, R8RegionUploadRoundTrips) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	RHI::Device* device = RenderSystem::GetDevice();

	RHI::TextureDesc desc;
	desc.size = {16, 16};
	desc.format = RHI::Format::R8Unorm;
	desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst | RHI::TextureUsage::TransferSrc;
	desc.debugName = "R8RoundTrip";
	RHI::TextureHandle texture = device->CreateTextureResource(desc);
	ASSERT_TRUE(texture.IsValid());

	Vector<u8> zero{GetMemoryResource(MemTag::Debug)};
	zero.resize(16 * 16, 0);
	device->UploadTexture(texture, {zero.data(), zero.size()});

	Vector<u8> patch{GetMemoryResource(MemTag::Debug)};
	patch.resize(4 * 4, 0xAB);
	device->UploadTextureRegion(texture, {patch.data(), patch.size()}, RectI{5, 7, 4, 4});

	Vector<u8> readback{GetMemoryResource(MemTag::Debug)};
	readback.resize(16 * 16);
	device->ReadbackTexture(texture, {readback.data(), readback.size()});

	EXPECT_EQ(readback[7 * 16 + 5], 0xAB);
	EXPECT_EQ(readback[10 * 16 + 8], 0xAB);
	EXPECT_EQ(readback[0], 0x00);
	EXPECT_EQ(readback[7 * 16 + 4], 0x00);

	device->DestroyResource(texture);
}

TEST_F(UIRenderFixture, UIOnlyWindowRendersThroughOverlaySeam) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}

	Handle<Window> window = MakeHiddenWindow();
	ASSERT_TRUE(window.IsValid());
	UI::Context* ctx = UISystem::CreateContext(window);
	ASSERT_NE(ctx, nullptr);

	UI::ElementHandle panel = ctx->CreateElement();
	ctx->AppendChild(ctx->GetRoot(), panel);
	UI::Style panelStyle;
	panelStyle.flexDirection = UI::FlexDirection::Column;
	panelStyle.width = UI::Length::Percent(1.0f);
	panelStyle.height = UI::Length::Percent(1.0f);
	panelStyle.padding = UI::Edges::All(8.0f);
	panelStyle.background = UI::Brush::SolidColor(Color(0.15f, 0.15f, 0.18f));
	ctx->SetStyle(panel, panelStyle);

	UI::ElementHandle gradient = ctx->CreateElement();
	ctx->AppendChild(panel, gradient);
	UI::Style gradientStyle;
	gradientStyle.width = UI::Length::Percent(1.0f);
	gradientStyle.height = UI::Length::Pixel(40.0f);
	gradientStyle.borderRadius = UI::Radii::All(6.0f);
	gradientStyle.background = UI::Brush::Gradient(Color::Red(), Color::Blue());
	ctx->SetStyle(gradient, gradientStyle);

	UI::ElementHandle label = ctx->CreateElement();
	ctx->AppendChild(panel, label);
	ctx->SetText(label, UISystem::GetDefaultFont(), 16.0f, Color::White(), "Shalltear UI on device");

	for (int frame = 0; frame < 2; ++frame) {
		WindowSystem::PumpEvents();
		UISystem::Update();
		RenderSystem::RenderAll();
	}

	UISystem::Stats stats = UISystem::GetStats();
	EXPECT_GT(stats.drawCalls, 0u);
	EXPECT_GT(stats.quads, 0u);
	EXPECT_EQ(stats.contexts, 1u);

	if (UISystem::GetDefaultFont().IsValid()) {
		EXPECT_GT(stats.quads, 2u);
	}

	WindowSystem::RequestDestroy(window);
	WindowSystem::PumpEvents();
	EXPECT_EQ(UISystem::GetStats().contexts, 0u);
}

TEST_F(UIRenderFixture, DockedUIRendersLayersAndTabs) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}

	Handle<Window> window = MakeHiddenWindow();
	ASSERT_TRUE(window.IsValid());
	UISystem::RegisterPanel("test.left"_hash, "Left", [](UI::Context& ctx, UI::ElementHandle parent) {
		UI::ElementHandle content = ctx.CreateElement();
		ctx.AppendChild(parent, content);
		UI::Style style;
		style.flexGrow = 1.0f;
		style.background = UI::Brush::SolidColor(Color(0.2f, 0.5f, 0.3f));
		style.opacity = 0.6f;
		ctx.SetStyle(content, style);
		UI::ElementHandle inner = ctx.CreateElement();
		ctx.AppendChild(content, inner);
		UI::Style innerStyle;
		innerStyle.width = UI::Length::Pixel(40.0f);
		innerStyle.height = UI::Length::Pixel(40.0f);
		innerStyle.background = UI::Brush::SolidColor(Color::Yellow());
		ctx.SetStyle(inner, innerStyle);
	});
	UISystem::RegisterPanel("test.right"_hash, "Right", [](UI::Context&, UI::ElementHandle) {});

	UI::DockManager* dock = UISystem::CreateDockManager(window);
	ASSERT_NE(dock, nullptr);
	dock->DockPanel("test.left"_hash, dock->GetRootNode(), UI::DockZone::Center);
	dock->DockPanel("test.right"_hash, dock->GetRootNode(), UI::DockZone::Right);

	for (int frame = 0; frame < 2; ++frame) {
		WindowSystem::PumpEvents();
		UISystem::Update();
		RenderSystem::RenderAll();
	}

	UISystem::Stats stats = UISystem::GetStats();
	EXPECT_GT(stats.drawCalls, 0u);
	EXPECT_GT(stats.quads, 4u);

	WindowSystem::RequestDestroy(window);
	WindowSystem::PumpEvents();
}

namespace {

	f32 HalfToFloat(u16 half) {
		u32 sign = (half & 0x8000u) << 16;
		u32 exponent = (half >> 10) & 0x1F;
		u32 mantissa = half & 0x3FF;
		if (exponent == 0) {
			return std::bit_cast<f32>(sign);
		}
		if (exponent == 31) {
			return std::bit_cast<f32>(sign | 0x7F800000u);
		}
		return std::bit_cast<f32>(sign | ((exponent + 112) << 23) | (mantissa << 13));
	}

	f32 RoundedBoxSDF(f32 px, f32 py, const UI::Rect& box, f32 radius) {
		f32 qx = std::abs(px - (box.x + box.w * 0.5f)) - box.w * 0.5f + radius;
		f32 qy = std::abs(py - (box.y + box.h * 0.5f)) - box.h * 0.5f + radius;
		f32 inside = std::min(std::max(qx, qy), 0.0f);
		f32 cx = std::max(qx, 0.0f);
		f32 cy = std::max(qy, 0.0f);
		return inside + std::sqrt(cx * cx + cy * cy) - radius;
	}

	f32 ExactCornerCoverage(i32 x, i32 y, const UI::Rect& box, f32 radius) {
		constexpr i32 SAMPLES = 64;
		i32 inside = 0;
		for (i32 sy = 0; sy < SAMPLES; ++sy) {
			for (i32 sx = 0; sx < SAMPLES; ++sx) {
				f32 px = static_cast<f32>(x) + (static_cast<f32>(sx) + 0.5f) / SAMPLES;
				f32 py = static_cast<f32>(y) + (static_cast<f32>(sy) + 0.5f) / SAMPLES;
				if (RoundedBoxSDF(px, py, box, radius) <= 0.0f) {
					++inside;
				}
			}
		}
		return static_cast<f32>(inside) / static_cast<f32>(SAMPLES * SAMPLES);
	}

	struct UIPushConstants {
		f32 viewportSize[2];
		f32 originPx[2];
		u64 quadAddress;
		u32 textureFlags;
		u32 passMode;
	};
	static_assert(sizeof(UIPushConstants) == 32);

	u32 MirroredTextureFlags(RHI::Format format) {
		return RHI::IsSRGB(format) ? 1u : 0u;
	}

	bool RenderListOffscreen(RHI::Device& device, const UI::RenderList& list,
							 Vec2I size, Vector<u8>& outTexels) {
		Asset* asset = AssetDatabase::LoadAsset(Path("engine://Shaders/UI.hlsl"));
		if (!asset || asset->GetAssetTypeId() != StringHash("ShaderAsset")) {
			if (asset) {
				AssetDatabase::UnloadAsset(asset);
			}
			return false;
		}
		ShaderAsset* shader = static_cast<ShaderAsset*>(asset);

		RHI::GraphicsPipelineDesc pipelineDesc;
		pipelineDesc.vertexSpirv = {shader->variants[SHADER_BASE_VARIANT].vertexSpirv.data(), shader->variants[SHADER_BASE_VARIANT].vertexSpirv.size()};
		pipelineDesc.pixelSpirv = {shader->variants[SHADER_BASE_VARIANT].pixelSpirv.data(), shader->variants[SHADER_BASE_VARIANT].pixelSpirv.size()};
		pipelineDesc.bytecodeFormat = shader->bytecodeFormat;
		pipelineDesc.bindings = {shader->bindings.data(), shader->bindings.size()};
		pipelineDesc.pushConstantSize = shader->pushConstantSize;
		pipelineDesc.blend = shader->renderState.blend;
		pipelineDesc.depth = shader->renderState.depth;
		pipelineDesc.cull = shader->renderState.cull;
		pipelineDesc.colorFormats[0] = RHI::Format::RGBA16Float;
		pipelineDesc.colorFormatCount = 1;
		pipelineDesc.depthFormat = RHI::Format::Unknown;
		pipelineDesc.debugName = "UIPixelTestPipeline";
		RHI::PipelineHandle pipeline = device.CreateGraphicsPipeline(pipelineDesc);

		RHI::TextureDesc targetDesc;
		targetDesc.size = size;
		targetDesc.format = RHI::Format::RGBA16Float;
		targetDesc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::TransferSrc;
		targetDesc.debugName = "UIPixelTestTarget";
		RHI::TextureHandle target = device.CreateTextureResource(targetDesc);

		RHI::TextureDesc whiteDesc;
		whiteDesc.size = {1, 1};
		whiteDesc.format = RHI::Format::RGBA8Unorm;
		whiteDesc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst;
		whiteDesc.debugName = "UIPixelTestWhite";
		RHI::TextureHandle white = device.CreateTextureResource(whiteDesc);
		const u8 whitePixel[4] = {255, 255, 255, 255};
		device.UploadTexture(white, whitePixel);

		RHI::SamplerDesc samplerDesc;
		samplerDesc.filter = RHI::FilterMode::Linear;
		samplerDesc.addressMode = RHI::AddressMode::ClampToEdge;
		samplerDesc.debugName = "UIPixelTestSampler";
		RHI::SamplerHandle sampler = device.CreateSamplerResource(samplerDesc);

		Vector<RHI::TextureHandle> layers{GetMemoryResource(MemTag::Debug)};
		bool ok = pipeline.IsValid() && target.IsValid() && white.IsValid() && sampler.IsValid();
		if (ok) {
			RHI::CommandList* cmd = device.BeginFrame();
			const u64 quadBytes = list.quads.size() * sizeof(UI::UIQuad);
			RHI::TransientAllocation allocation = device.AllocateTransient(quadBytes, 16);
			ok = allocation.IsValid();
			if (ok) {
				std::memcpy(allocation.cpu, list.quads.data(), quadBytes);
				const u64 baseAddress = device.GetBufferDeviceAddress(allocation.buffer) + allocation.offset;

				struct Pass {
					RHI::TextureHandle target;
					Vec2I sizePx{};
					Vec2 originPx{};
				};
				Vector<Pass> passStack{GetMemoryResource(MemTag::Debug)};
				passStack.push_back({target, size, {0.0f, 0.0f}});

				auto openPass = [&](const Pass& pass, bool clear) {
					cmd->TransitionTexture(pass.target, RHI::ResourceState::RenderTarget);
					RHI::RenderPassDesc::Attachment attachment;
					attachment.texture = pass.target;
					attachment.load = clear ? RHI::LoadOp::Clear : RHI::LoadOp::Load;
					attachment.clearColor = Color::Clear();
					RHI::RenderPassDesc passDesc;
					passDesc.colorAttachments = {&attachment, 1};
					cmd->BeginRenderPass(passDesc);
					cmd->SetPipeline(pipeline);
					cmd->SetViewportScissor(RectI{0, 0, pass.sizePx.x, pass.sizePx.y});
				};

				RHI::TextureHandle lastPopped{};
				openPass(passStack.back(), true);
				for (const UI::UICommand& command : list.commands) {
					switch (command.kind) {
					case UI::UICommandKind::PushLayer: {
						cmd->EndRenderPass();
						i32 x0 = static_cast<i32>(std::floor(command.layerBounds.x));
						i32 y0 = static_cast<i32>(std::floor(command.layerBounds.y));
						i32 x1 = static_cast<i32>(std::ceil(command.layerBounds.x + command.layerBounds.w));
						i32 y1 = static_cast<i32>(std::ceil(command.layerBounds.y + command.layerBounds.h));
						Vec2I layerSize{std::max(1, x1 - x0), std::max(1, y1 - y0)};
						RHI::TextureDesc layerDesc;
						layerDesc.size = layerSize;
						layerDesc.format = RHI::Format::RGBA16Float;
						layerDesc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::Sampled;
						layerDesc.debugName = "UIPixelTestLayer";
						RHI::TextureHandle layer = device.CreateTextureResource(layerDesc);
						layers.push_back(layer);
						passStack.push_back({layer, layerSize, {static_cast<f32>(x0), static_cast<f32>(y0)}});
						openPass(passStack.back(), true);
						break;
					}
					case UI::UICommandKind::PopLayer: {
						cmd->EndRenderPass();
						lastPopped = passStack.back().target;
						passStack.pop_back();
						cmd->TransitionTexture(lastPopped, RHI::ResourceState::ShaderResource);
						openPass(passStack.back(), false);
						break;
					}
					case UI::UICommandKind::PushShadow:
					case UI::UICommandKind::PopShadow:
						break;
					case UI::UICommandKind::Quads: {
						RHI::TextureHandle texture = command.texture;
						if (texture == UI::LayerCompositeTexture) {
							texture = lastPopped;
						}
						if (!texture.IsValid()) {
							texture = white;
						}
						RHI::TransientSet set = device.CreateTransientSet(pipeline, 1)
													.WriteTexture(1, texture)
													.WriteSampler(2, sampler)
													.Commit();
						cmd->BindTransientSet(1, set);
						const Pass& pass = passStack.back();
						UIPushConstants push;
						push.viewportSize[0] = static_cast<f32>(pass.sizePx.x);
						push.viewportSize[1] = static_cast<f32>(pass.sizePx.y);
						push.originPx[0] = pass.originPx.x;
						push.originPx[1] = pass.originPx.y;
						push.quadAddress = baseAddress + static_cast<u64>(command.firstQuad) * sizeof(UI::UIQuad);
						push.textureFlags = MirroredTextureFlags(device.GetTextureFormat(texture));
						push.passMode = 0;
						cmd->PushConstants({reinterpret_cast<const u8*>(&push), sizeof(push)});
						cmd->Draw(command.quadCount * 6u);
						break;
					}
					}
				}
				cmd->EndRenderPass();
			}
			device.EndFrame();
			device.WaitIdle();

			if (ok) {
				outTexels.resize(static_cast<size_t>(size.x) * size.y * 8);
				device.ReadbackTexture(target, {outTexels.data(), outTexels.size()});
			}
		}

		device.DestroyResource(pipeline);
		device.DestroyResource(target);
		device.DestroyResource(white);
		device.DestroyResource(sampler);
		for (RHI::TextureHandle layer : layers) {
			device.DestroyResource(layer);
		}
		AssetDatabase::UnloadAsset(shader);
		device.BeginFrame();
		device.EndFrame();
		device.BeginFrame();
		device.EndFrame();
		device.WaitIdle();
		return ok;
	}

	bool RenderQuadsOffscreen(RHI::Device& device, std::span<const UI::UIQuad> quads,
							  Vec2I size, Vector<u8>& outTexels) {
		UI::RenderList list;
		list.quads.assign(quads.begin(), quads.end());
		UI::UICommand command;
		command.kind = UI::UICommandKind::Quads;
		command.firstQuad = 0;
		command.quadCount = static_cast<u32>(quads.size());
		list.commands.push_back(command);
		return RenderListOffscreen(device, list, size, outTexels);
	}

	constexpr const char* DOCK_SHAPE_SKIN = R"css(
.aa-leaf { display: flex; flex-direction: column; width: 100%; height: 100%; flex-grow: 1; background-color: #2b2c2f; border-radius: 6; overflow: hidden; box-shadow: inset 0 0 0 1 #ffffff08; }
.aa-bar { width: 100%; display: flex; overflow: scroll; flex-shrink: 0; background-color: #141516; border-radius: 6 6 0 0; box-shadow: inset 0 0 0 1 #ffffff08, inset 0 -1 0 #00000050; }
.aa-tab { padding: 6 8 6 8; flex-direction: row; align-items: center; flex-shrink: 0; background-color: #2b2c2f; border-radius: 5 5 0 0; box-shadow: inset 1px 0 0 0 #ffffff08, inset 0 1px 0 0 #ffffff08, inset -1px 0 0 0 #ffffff08; }
)css";

	struct DockShapeParts {
		UI::ElementHandle leaf;
		UI::ElementHandle bar;
		UI::ElementHandle tab;
	};

	DockShapeParts BuildDockShape(UI::Context& ctx, UI::ElementHandle parent, Vec2 labelSize) {
		DockShapeParts parts;
		parts.leaf = ctx.CreateElement();
		ctx.AddClass(parts.leaf, StringHash("aa-leaf"));
		ctx.AppendChild(parent, parts.leaf);
		parts.bar = ctx.CreateElement();
		ctx.AddClass(parts.bar, StringHash("aa-bar"));
		ctx.AppendChild(parts.leaf, parts.bar);
		parts.tab = ctx.CreateElement();
		ctx.AddClass(parts.tab, StringHash("aa-tab"));
		ctx.AppendChild(parts.bar, parts.tab);
		UI::ElementHandle label = ctx.CreateElement();
		ctx.SetStyle(label, FixedBox(labelSize.x, labelSize.y));
		ctx.AppendChild(parts.tab, label);
		return parts;
	}

} // anonymous namespace

TEST_F(UIRenderFixture, RoundedQuadPixelsAreMirrorSymmetric) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}

	UI::UIQuad quad;
	quad.rect = {9.0f, 9.0f, 45.0f, 45.0f};
	quad.color0 = Color::White();
	quad.color1 = quad.color0;
	quad.radii = UI::Radii::All(12.0f);
	quad.clipRect = UI::InfiniteClip;

	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	if (!RenderQuadsOffscreen(*RenderSystem::GetDevice(), {&quad, 1}, {64, 64}, texels)) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	const u16* halves = reinterpret_cast<const u16*>(texels.data());
	auto red = [&](i32 x, i32 y) { return halves[(static_cast<size_t>(y) * 64 + x) * 4]; };

	for (i32 y = 0; y < 64; ++y) {
		for (i32 x = 0; x <= 62; ++x) {
			ASSERT_EQ(red(x, y), red(62 - x, y)) << x << ", " << y << ")";
		}
	}
	for (i32 y = 0; y <= 62; ++y) {
		for (i32 x = 0; x < 64; ++x) {
			ASSERT_EQ(red(x, y), red(x, 62 - y)) << x << ", " << y << ")";
		}
	}
	for (i32 y = 0; y < 64; ++y) {
		for (i32 x = 0; x < y; ++x) {
			ASSERT_EQ(red(x, y), red(y, x)) << x << ", " << y << ")";
		}
	}

	EXPECT_EQ(HalfToFloat(red(31, 31)), 1.0f);
	EXPECT_EQ(HalfToFloat(red(9, 31)), 1.0f);
	EXPECT_EQ(HalfToFloat(red(8, 31)), 0.0f);
}

TEST_F(UIRenderFixture, OutlineRingCornerBandReachesFullCoverage) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}

	const UI::Rect outerBox = {8.0f, 8.0f, 48.0f, 48.0f};
	const UI::Rect innerBox = {9.0f, 9.0f, 46.0f, 46.0f};
	UI::UIQuad quad;
	quad.rect = outerBox;
	quad.uv = innerBox;
	quad.color0 = Color::White();
	quad.radii = UI::Radii::All(12.0f);
	quad.color1 = Color(11.0f, 11.0f, 11.0f, 11.0f);
	quad.clipRect = UI::InfiniteClip;
	quad.flags = UI::QUAD_FLAG_RING;

	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	if (!RenderQuadsOffscreen(*RenderSystem::GetDevice(), {&quad, 1}, {64, 64}, texels)) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	const u16* halves = reinterpret_cast<const u16*>(texels.data());
	auto red = [&](i32 x, i32 y) { return halves[(static_cast<size_t>(y) * 64 + x) * 4]; };

	i32 checked = 0;
	for (i32 y = 0; y < 64; ++y) {
		for (i32 x = 0; x < 64; ++x) {
			f32 px = static_cast<f32>(x) + 0.5f;
			f32 py = static_cast<f32>(y) + 0.5f;
			f32 distOuter = RoundedBoxSDF(px, py, outerBox, 12.0f);
			if (distOuter <= -0.4f && distOuter >= -0.6f) {
				++checked;
				EXPECT_GE(HalfToFloat(red(x, y)), 0.895f) << "band pixel (" << x << ", " << y << ")";
			}
		}
	}
	ASSERT_GE(checked, 8);

	EXPECT_EQ(HalfToFloat(red(32, 8)), 1.0f);
	EXPECT_EQ(HalfToFloat(red(32, 9)), 0.0f);

	for (i32 y = 0; y < 64; ++y) {
		for (i32 x = 0; x < 32; ++x) {
			ASSERT_EQ(red(x, y), red(63 - x, y)) << x << ", " << y << ")";
			ASSERT_EQ(red(y, x), red(y, 63 - x)) << y << ", " << x << ")";
		}
	}
}

TEST_F(UIRenderFixture, TabLikeCompositeIsPixelSymmetric) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}

	for (f32 scale : {1.0f, 1.25f, 1.5f}) {
		const i32 extent = static_cast<i32>(96.0f * scale);
		SCOPED_TRACE(::testing::Message() << "contentScale " << scale);

		UI::ContextDesc desc;
		desc.sizePx = {extent, extent};
		desc.contentScale = scale;
		UI::Context ctx(desc, nullptr);

		UI::Style rootStyle;
		rootStyle.background = UI::Brush::SolidColor(Color(0.078f, 0.078f, 0.086f, 1.0f));
		rootStyle.padding = UI::Edges::All(16.0f);
		ctx.SetStyle(ctx.GetRoot(), rootStyle);

		UI::Style tabStyle;
		tabStyle.width = UI::Length::Pixel(64.0f);
		tabStyle.height = UI::Length::Pixel(64.0f);
		tabStyle.background = UI::Brush::SolidColor(Color(0.169f, 0.169f, 0.184f, 1.0f));
		tabStyle.borderRadius = UI::Radii::All(5.0f);
		tabStyle.borderWidth = UI::Edges::All(1.0f);
		tabStyle.borderColor = Color(0.35f, 0.40f, 0.50f, 1.0f);
		UI::BoxShadow outline;
		outline.spread = 1.0f;
		outline.inset = true;
		outline.color = Color(0.05f, 0.10f, 0.20f, 0.75f);
		tabStyle.AddBoxShadow(outline);
		UI::BoxShadow drop;
		drop.spread = 2.0f;
		drop.color = Color(0.0f, 0.0f, 0.0f, 0.5f);
		tabStyle.AddBoxShadow(drop);
		tabStyle.overflow = UI::Overflow::Hidden;
		tabStyle.padding = UI::Edges::All(8.0f);
		UI::ElementHandle tab = ctx.CreateElement();
		ctx.SetStyle(tab, tabStyle);
		ctx.AppendChild(ctx.GetRoot(), tab);

		UI::Style childStyle;
		childStyle.width = UI::Length::Percent(1.0f);
		childStyle.height = UI::Length::Percent(1.0f);
		childStyle.background = UI::Brush::SolidColor(Color(0.0f, 0.47f, 0.84f, 1.0f));
		childStyle.borderRadius = UI::Radii::All(4.0f);
		UI::ElementHandle child = ctx.CreateElement();
		ctx.SetStyle(child, childStyle);
		ctx.AppendChild(tab, child);

		ctx.Update();
		UI::RenderList list;
		ctx.BuildRenderList(list);
		ASSERT_EQ(list.quads.size(), 7u);
		for (const UI::UICommand& command : list.commands) {
			if (command.kind == UI::UICommandKind::Quads && command.texture.IsValid()) {
				ASSERT_TRUE(command.texture == UI::LayerCompositeTexture);
			}
		}
		bool mirrorPlaced = true;
		for (const UI::UIQuad& quad : list.quads) {
			mirrorPlaced = mirrorPlaced &&
						   quad.rect.x == static_cast<f32>(extent) - (quad.rect.x + quad.rect.w) &&
						   quad.rect.y == static_cast<f32>(extent) - (quad.rect.y + quad.rect.h);
		}

		Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
		if (!RenderListOffscreen(*RenderSystem::GetDevice(), list, {extent, extent}, texels)) {
			GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
		}
		const u16* halves = reinterpret_cast<const u16*>(texels.data());
		auto texel = [&](i32 x, i32 y, i32 channel) {
			return halves[(static_cast<size_t>(y) * extent + x) * 4 + channel];
		};

		const i32 last = extent - 1;
		for (i32 y = 0; y < extent; ++y) {
			for (i32 x = 0; x < extent; ++x) {
				for (i32 c = 0; c < 4; ++c) {
					ASSERT_EQ(texel(x, y, c), texel(y, x, c))
						<< "transpose at (" << x << ", " << y << ") channel " << c;
					if (mirrorPlaced) {
						ASSERT_EQ(texel(x, y, c), texel(last - x, y, c)) << x << ", " << y << ") channel " << c;
						ASSERT_EQ(texel(x, y, c), texel(x, last - y, c)) << x << ", " << y << ") channel " << c;
					}
				}
			}
		}
		if (scale == 1.0f) {
			ASSERT_TRUE(mirrorPlaced);
		}
	}
}

TEST_F(UIRenderFixture, DockActiveTabOutlinePixelParity) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}

	const Vec2I size{128, 64};
	UI::ContextDesc desc;
	desc.sizePx = size;
	desc.contentScale = 1.0f;
	UI::Context ctx(desc, nullptr);

	ctx.LoadStyleSheet(DOCK_SHAPE_SKIN);
	UI::ElementHandle tab = BuildDockShape(ctx, ctx.GetRoot(), {40.0f, 14.0f}).tab;

	ctx.Update();
	UI::RenderList list;
	ctx.BuildRenderList(list);
	for (const UI::UICommand& command : list.commands) {
		if (command.kind == UI::UICommandKind::Quads && command.texture.IsValid()) {
			ASSERT_TRUE(command.texture == UI::LayerCompositeTexture);
		}
	}
	const UI::Rect tabBox = ctx.GetLayout(tab).borderBox;
	ASSERT_EQ(tabBox.x, 0.0f);
	ASSERT_EQ(tabBox.y, 0.0f);
	ASSERT_GE(tabBox.w, 40.0f);
	ASSERT_GE(tabBox.h, 20.0f);

	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	if (!RenderListOffscreen(*RenderSystem::GetDevice(), list, size, texels)) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	const u16* halves = reinterpret_cast<const u16*>(texels.data());
	auto texel = [&](i32 x, i32 y, i32 c) {
		return halves[(static_cast<size_t>(y) * size.x + x) * 4 + c];
	};
	auto sRGB8 = [&](i32 x, i32 y, i32 c) {
		return static_cast<i32>(HalfToFloat(texel(x, y, c)) * 255.0f + 0.5f);
	};

	const i32 tabRight = static_cast<i32>(tabBox.x + tabBox.w);
	const i32 tabBottom = static_cast<i32>(tabBox.y + tabBox.h);
	for (i32 c = 0; c < 4; ++c) {
		for (i32 x = 8; x < tabRight - 8; ++x) {
			ASSERT_EQ(texel(x, 0, c), texel(8, 0, c)) << x << " channel " << c;
		}
		for (i32 y = 8; y < tabBottom - 1; ++y) {
			ASSERT_EQ(texel(0, y, c), texel(0, 8, c)) << y << " channel " << c;
		}
	}

	for (i32 c = 0; c < 3; ++c) {
		ASSERT_EQ(sRGB8(8, 0, c), sRGB8(0, 8, c));
	}

	f32 worst = 0.0f;
	for (i32 y = 0; y < 14; ++y) {
		for (i32 x = 0; x < y; ++x) {
			for (i32 c = 0; c < 3; ++c) {
				f32 a = HalfToFloat(texel(x, y, c)) * 255.0f;
				f32 b = HalfToFloat(texel(y, x, c)) * 255.0f;
				worst = std::max(worst, std::abs(a - b));
				ASSERT_LE(std::abs(a - b), 0.4f) << x << ", " << y << ") channel " << c << ": " << a << " against " << b;
			}
		}
	}
	EXPECT_GT(worst, 0.0f);

	const f32 outside = HalfToFloat(texel(0, 0, 0));
	const f32 outlineBand = HalfToFloat(texel(0, 8, 0));
	const f32 tabFill = HalfToFloat(texel(8, 8, 0));
	const f32 well = HalfToFloat(texel(80, 8, 0));
	EXPECT_LT(outside, 0.001f);
	EXPECT_GT(well, outside);
	EXPECT_GT(tabFill, well * 2.0f);
	EXPECT_GT(outlineBand, tabFill);
}

TEST_F(UIRenderFixture, TabMeetsBarAtFractionalOrigin) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}

	const Vec2I size{160, 64};
	UI::ContextDesc desc;
	desc.sizePx = size;
	desc.contentScale = 1.25f;
	UI::Context ctx(desc, nullptr);

	ctx.LoadStyleSheet(DOCK_SHAPE_SKIN);

	UI::Style holderStyle;
	holderStyle.position = UI::Position::Absolute;
	holderStyle.left = UI::Length::Pixel(2.6f);
	holderStyle.top = UI::Length::Pixel(2.6f);
	holderStyle.width = UI::Length::Pixel(120.0f);
	holderStyle.height = UI::Length::Pixel(40.0f);
	UI::ElementHandle holder = MakeChild(ctx, ctx.GetRoot(), holderStyle);

	DockShapeParts parts = BuildDockShape(ctx, holder, {40.0f, 14.0f});
	UI::ElementHandle bar = parts.bar;
	UI::ElementHandle tab = parts.tab;

	ctx.Update();
	UI::RenderList list;
	ctx.BuildRenderList(list);

	const UI::Rect rawBar = ctx.GetLayout(bar).borderBox;
	ASSERT_NE(rawBar.x, UI::SnapToPixel(rawBar.x));
	const UI::Rect barBox = UI::SnapRectToPixels(rawBar);
	const UI::Rect tabBox = UI::SnapRectToPixels(ctx.GetLayout(tab).borderBox);

	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	if (!RenderListOffscreen(*RenderSystem::GetDevice(), list, size, texels)) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	const u16* halves = reinterpret_cast<const u16*>(texels.data());
	auto luminance = [&](i32 x, i32 y) {
		return HalfToFloat(halves[(static_cast<size_t>(y) * size.x + x) * 4]);
	};

	const i32 row = static_cast<i32>(tabBox.y) + 16;
	const i32 bandX = static_cast<i32>(barBox.x);
	const i32 tabX = static_cast<i32>(tabBox.x);
	EXPECT_EQ(tabX, bandX);

	const f32 outside = luminance(bandX - 1, row);
	const f32 band = luminance(bandX, row);
	const f32 firstInside = luminance(bandX + 1, row);
	const f32 tabInterior = luminance(tabX + 8, row);
	const f32 well = luminance(static_cast<i32>(tabBox.x + tabBox.w) + 4, row);

	EXPECT_LT(outside, 0.001f);
	EXPECT_GT(well, 0.0f);
	EXPECT_GT(tabInterior, well * 2.0f);
	EXPECT_GT(band, tabInterior);

	EXPECT_NEAR(firstInside, tabInterior, 0.001f);
	EXPECT_GT(firstInside, well * 2.0f);
}

TEST_F(UIRenderFixture, ViewportTextureIsOneToOne) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	RHI::Device* device = RenderSystem::GetDevice();
	ASSERT_NE(device, nullptr);

	constexpr i32 EXTENT = 16;
	constexpr i32 SURFACE = 32;

	Vector<u8> pattern{GetMemoryResource(MemTag::Debug)};
	pattern.resize(static_cast<size_t>(EXTENT) * EXTENT * 4);
	for (i32 y = 0; y < EXTENT; ++y) {
		for (i32 x = 0; x < EXTENT; ++x) {
			u8 value = ((x + y) % 2 == 0) ? 255 : 0;
			u8* texel = &pattern[(static_cast<size_t>(y) * EXTENT + x) * 4];
			texel[0] = value;
			texel[1] = value;
			texel[2] = value;
			texel[3] = 255;
		}
	}

	RHI::TextureDesc checkerDesc;
	checkerDesc.size = {EXTENT, EXTENT};
	checkerDesc.format = RHI::Format::RGBA8Unorm;
	checkerDesc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst;
	checkerDesc.debugName = "ViewportCheckerboard";
	RHI::TextureHandle checker = device->CreateTextureResource(checkerDesc);
	ASSERT_TRUE(checker.IsValid());
	device->UploadTexture(checker, pattern);

	auto blitInto = [&](const UI::Rect& rect, Vector<u8>& outTexels) {
		UI::UIQuad quad;
		quad.rect = rect;
		quad.uv = {0.0f, 0.0f, 1.0f, 1.0f};
		quad.color0 = Color::White();
		quad.color1 = quad.color0;
		quad.clipRect = UI::InfiniteClip;

		UI::RenderList list;
		list.quads.push_back(quad);
		UI::UICommand command;
		command.kind = UI::UICommandKind::Quads;
		command.texture = checker;
		command.firstQuad = 0;
		command.quadCount = 1;
		list.commands.push_back(command);
		return RenderListOffscreen(*device, list, {SURFACE, SURFACE}, outTexels);
	};

	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	if (!blitInto({8.0f, 8.0f, 16.0f, 16.0f}, texels)) {
		device->DestroyResource(checker);
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	const u16* halves = reinterpret_cast<const u16*>(texels.data());
	auto red = [&](i32 x, i32 y) {
		return HalfToFloat(halves[(static_cast<size_t>(y) * SURFACE + x) * 4]);
	};

	for (i32 j = 0; j < EXTENT; ++j) {
		for (i32 i = 0; i < EXTENT; ++i) {
			f32 expected = ((i + j) % 2 == 0) ? 1.0f : 0.0f;
			ASSERT_EQ(red(8 + i, 8 + j), expected) << "texel (" << i << ", " << j;
		}
	}

	Vector<u8> stretched{GetMemoryResource(MemTag::Debug)};
	ASSERT_TRUE(blitInto({8.0f, 8.0f, 17.0f, 16.0f}, stretched));
	const u16* stretchedHalves = reinterpret_cast<const u16*>(stretched.data());
	u32 blended = 0;
	for (i32 j = 0; j < EXTENT; ++j) {
		for (i32 i = 0; i < 17; ++i) {
			f32 value = HalfToFloat(stretchedHalves[((static_cast<size_t>(8 + j) * SURFACE) + 8 + i) * 4]);
			if (value > 0.02f && value < 0.98f) {
				++blended;
			}
		}
	}
	EXPECT_GT(blended, 0u);

	device->DestroyResource(checker);
}

TEST_F(UIRenderFixture, CoincidentRoundedClipDoesNotSquareCoverage) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}

	const UI::Rect box = {8.0f, 8.0f, 48.0f, 48.0f};
	const f32 radius = 12.0f;

	UI::UIQuad plain;
	plain.rect = box;
	plain.color0 = Color::White();
	plain.color1 = plain.color0;
	plain.radii = UI::Radii::All(radius);
	plain.clipRect = UI::InfiniteClip;

	UI::UIQuad clipped = plain;
	clipped.clipRect = box;
	clipped.clipRadii = UI::Radii::All(radius);

	Vector<u8> plainTexels{GetMemoryResource(MemTag::Debug)};
	Vector<u8> clippedTexels{GetMemoryResource(MemTag::Debug)};
	if (!RenderQuadsOffscreen(*RenderSystem::GetDevice(), {&plain, 1}, {64, 64}, plainTexels)) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	ASSERT_TRUE(RenderQuadsOffscreen(*RenderSystem::GetDevice(), {&clipped, 1}, {64, 64}, clippedTexels));
	const u16* plainHalves = reinterpret_cast<const u16*>(plainTexels.data());
	const u16* clippedHalves = reinterpret_cast<const u16*>(clippedTexels.data());

	i32 partials = 0;
	for (i32 y = 0; y < 64; ++y) {
		for (i32 x = 0; x < 64; ++x) {
			size_t index = (static_cast<size_t>(y) * 64 + x) * 4;
			ASSERT_EQ(clippedHalves[index], plainHalves[index]) << "texel (" << x << ", " << y << ")";
			f32 value = HalfToFloat(plainHalves[index]);
			if (value > 0.02f && value < 0.98f) {
				++partials;
			}
		}
	}
	ASSERT_GE(partials, 24);

	for (i32 y = 8; y < 20; ++y) {
		for (i32 x = 8; x < 20; ++x) {
			f32 dist = RoundedBoxSDF(static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f, box, radius);
			if (dist < -1.0f || dist > 1.0f) {
				continue;
			}
			f32 measured = HalfToFloat(clippedHalves[(static_cast<size_t>(y) * 64 + x) * 4]);
			EXPECT_NEAR(measured, ExactCornerCoverage(x, y, box, radius), 0.06f)
				<< "arc pixel (" << x << ", " << y << ")";
		}
	}
}

TEST_F(UIRenderFixture, InnerClipSkipsFarArcs) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}

	UI::ContextDesc desc;
	desc.sizePx = {64, 64};
	desc.contentScale = 1.0f;
	UI::Context ctx(desc, nullptr);

	UI::Style leafStyle = FixedBox(64.0f, 64.0f);
	leafStyle.flexDirection = UI::FlexDirection::Column;
	leafStyle.background = UI::Brush::SolidColor(Color::Black());
	leafStyle.overflow = UI::Overflow::Hidden;
	leafStyle.borderRadius = UI::Radii::All(12.0f);
	UI::ElementHandle leaf = MakeChild(ctx, ctx.GetRoot(), leafStyle);

	MakeChild(ctx, leaf, FixedBox(64.0f, 24.0f));
	UI::Style fieldStyle = FixedBox(40.0f, 20.0f);
	fieldStyle.background = UI::Brush::SolidColor(Color::White());
	fieldStyle.overflow = UI::Overflow::Hidden;
	MakeChild(ctx, leaf, fieldStyle);

	ctx.Update();
	UI::RenderList list;
	ctx.BuildRenderList(list);

	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	if (!RenderListOffscreen(*RenderSystem::GetDevice(), list, {64, 64}, texels)) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	const u16* halves = reinterpret_cast<const u16*>(texels.data());
	auto channel = [&](i32 x, i32 y, i32 c) { return HalfToFloat(halves[(static_cast<size_t>(y) * 64 + x) * 4 + c]); };

	EXPECT_GT(channel(0, 24, 0), 0.98f);
	EXPECT_GT(channel(0, 43, 0), 0.98f);

	EXPECT_LT(channel(0, 20, 0), 0.02f);
	EXPECT_LT(channel(0, 0, 3), 0.02f);
	EXPECT_GT(channel(32, 0, 3), 0.98f);
}

TEST_F(UIRenderFixture, DockCornerArcKeepsAntialiasingOnBlack) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}

	const Vec2I size{64, 64};
	UI::ContextDesc desc;
	desc.sizePx = size;
	desc.contentScale = 1.25f;
	UI::Context ctx(desc, nullptr);
	ctx.LoadStyleSheet(DOCK_SHAPE_SKIN);

	UI::Style holderStyle;
	holderStyle.position = UI::Position::Absolute;
	holderStyle.left = UI::Length::Pixel(4.0f);
	holderStyle.top = UI::Length::Pixel(4.0f);
	holderStyle.width = UI::Length::Pixel(40.0f);
	holderStyle.height = UI::Length::Pixel(36.0f);
	UI::ElementHandle holder = MakeChild(ctx, ctx.GetRoot(), holderStyle);

	UI::ElementHandle leaf = BuildDockShape(ctx, holder, {10.0f, 10.0f}).leaf;

	ctx.Update();
	UI::RenderList list;
	ctx.BuildRenderList(list);

	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	if (!RenderListOffscreen(*RenderSystem::GetDevice(), list, size, texels)) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	const u16* halves = reinterpret_cast<const u16*>(texels.data());
	auto sRGB8 = [&](i32 x, i32 y) {
		f32 encoded = HalfToFloat(halves[(static_cast<size_t>(y) * size.x + x) * 4]);
		return static_cast<i32>(encoded * 255.0f + 0.5f);
	};

	const UI::Rect leafBox = UI::SnapRectToPixels(ctx.GetLayout(leaf).borderBox);
	ASSERT_EQ(leafBox.x, 5.0f);
	ASSERT_EQ(leafBox.y, 5.0f);

	const i32 background = sRGB8(4, 4);
	const i32 surface = sRGB8(20, 20);
	EXPECT_EQ(background, 0);
	EXPECT_GE(surface, 40);

	constexpr i32 BAND = 50;

	for (i32 y = 12; y < 16; ++y) {
		EXPECT_EQ(sRGB8(4, y), 0) << y;
		EXPECT_EQ(sRGB8(5, y), BAND) << y;
	}

	EXPECT_NEAR(sRGB8(5, 8), 2, 2);
	EXPECT_NEAR(sRGB8(5, 9), 17, 3);
	EXPECT_NEAR(sRGB8(5, 10), 36, 3);
	EXPECT_NEAR(sRGB8(5, 11), 48, 3);

	EXPECT_GT(sRGB8(5, 8), 0);
	EXPECT_LT(sRGB8(5, 8), sRGB8(5, 9));
	EXPECT_LT(sRGB8(5, 9), sRGB8(5, 10));
	EXPECT_LT(sRGB8(5, 10), sRGB8(5, 11));
	EXPECT_LE(sRGB8(5, 11), BAND);

	EXPECT_EQ(sRGB8(7, 6), sRGB8(6, 7));
	EXPECT_EQ(sRGB8(5, 9), sRGB8(9, 5));
	EXPECT_EQ(sRGB8(5, 10), sRGB8(10, 5));
}

TEST_F(UIRenderFixture, HairlineRingWeighsOnePixelThroughCorners) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}

	constexpr f32 OUTER_RADIUS = 12.0f;
	constexpr f32 INNER_RADIUS = 11.0f;
	const UI::Rect outerBox = {8.0f, 8.0f, 48.0f, 48.0f};
	const UI::Rect innerBox = {9.0f, 9.0f, 46.0f, 46.0f};

	UI::UIQuad quad;
	quad.rect = outerBox;
	quad.uv = innerBox;
	quad.color0 = Color::White();
	quad.color1 = Color(INNER_RADIUS, INNER_RADIUS, INNER_RADIUS, INNER_RADIUS);
	quad.radii = UI::Radii::All(OUTER_RADIUS);
	quad.clipRect = UI::InfiniteClip;
	quad.flags = UI::QUAD_FLAG_RING;

	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	if (!RenderQuadsOffscreen(*RenderSystem::GetDevice(), {&quad, 1}, {64, 64}, texels)) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	const u16* halves = reinterpret_cast<const u16*>(texels.data());

	f64 ink = 0.0;
	for (i32 y = 0; y < 64; ++y) {
		for (i32 x = 0; x < 64; ++x) {
			ink += HalfToFloat(halves[(static_cast<size_t>(y) * 64 + x) * 4]);
		}
	}

	auto roundedArea = [](const UI::Rect& box, f32 radius) {
		return static_cast<f64>(box.w) * box.h -
			   (4.0 - 3.14159265358979) * static_cast<f64>(radius) * radius;
	};
	const f64 area = roundedArea(outerBox, OUTER_RADIUS) - roundedArea(innerBox, INNER_RADIUS);

	EXPECT_NEAR(ink, area, area * 0.02);

	i32 partials = 0;
	for (i32 y = 0; y < 64; ++y) {
		for (i32 x = 0; x < 64; ++x) {
			f32 value = HalfToFloat(halves[(static_cast<size_t>(y) * 64 + x) * 4]);
			if (value > 0.02f && value < 0.98f) {
				++partials;
			}
		}
	}
	ASSERT_GE(partials, 24);
}

TEST_F(UIRenderFixture, AuthoredTranslucentTokensCompositeWayCssDoes) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}

	const Vec2I size{32, 32};
	UI::ContextDesc desc;
	desc.sizePx = size;
	desc.contentScale = 1.0f;
	UI::Context ctx(desc, nullptr);

	auto ch = [](i32 v) { return static_cast<f32>(v) / 255.0f; };
	UI::Style style;
	style.position = UI::Position::Absolute;
	style.left = UI::Length::Pixel(4.0f);
	style.top = UI::Length::Pixel(4.0f);
	style.width = UI::Length::Pixel(24.0f);
	style.height = UI::Length::Pixel(24.0f);
	style.background = UI::Brush::SolidColor(Color(ch(0x2b), ch(0x2c), ch(0x2f), 1.0f));
	UI::BoxShadow inset;
	inset.inset = true;
	inset.spread = 1.0f;
	inset.color = Color(1.0f, 1.0f, 1.0f, ch(0x08));
	style.AddBoxShadow(inset);
	MakeChild(ctx, ctx.GetRoot(), style);

	ctx.Update();
	UI::RenderList list;
	ctx.BuildRenderList(list);

	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	if (!RenderListOffscreen(*RenderSystem::GetDevice(), list, size, texels)) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	const u16* halves = reinterpret_cast<const u16*>(texels.data());
	auto sRGB8 = [&](i32 x, i32 y, i32 c) {
		f32 encoded = HalfToFloat(halves[(static_cast<size_t>(y) * size.x + x) * 4 + c]);
		return static_cast<i32>(encoded * 255.0f + 0.5f);
	};

	EXPECT_EQ(sRGB8(16, 16, 0), 0x2b);
	EXPECT_EQ(sRGB8(16, 16, 1), 0x2c);
	EXPECT_EQ(sRGB8(16, 16, 2), 0x2f);

	struct Probe {
		i32 x, y;
		const char* edge;
	};
	const Probe probes[] = {{16, 4, "top"}, {4, 16, "left"}, {27, 16, "right"}, {16, 27, "bottom"}};
	for (const Probe& probe : probes) {
		EXPECT_EQ(sRGB8(probe.x, probe.y, 0), 0x32) << probe.edge << " stroke red";
		EXPECT_EQ(sRGB8(probe.x, probe.y, 1), 0x33) << probe.edge << " stroke green";
		EXPECT_EQ(sRGB8(probe.x, probe.y, 2), 0x36) << probe.edge << " stroke blue";
	}

	EXPECT_GT(sRGB8(16, 4, 0), sRGB8(16, 16, 0));
}

TEST_F(UIRenderFixture, InsetBandsFollowArcsWayCssDoes) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}

	const Vec2I size{40, 28};
	const UI::Rect box{4.0f, 4.0f, 32.0f, 20.0f};
	constexpr f32 RADIUS = 6.0f;

	auto truth = [&](i32 x, i32 y, f32 offsetX, f32 offsetY) {
		constexpr i32 SAMPLES = 32;
		auto covered = [&](f32 px, f32 py) { return RoundedBoxSDF(px, py, box, RADIUS) <= 0.0f; };
		i32 inside = 0;
		for (i32 sy = 0; sy < SAMPLES; ++sy) {
			for (i32 sx = 0; sx < SAMPLES; ++sx) {
				f32 px = static_cast<f32>(x) + (static_cast<f32>(sx) + 0.5f) / SAMPLES;
				f32 py = static_cast<f32>(y) + (static_cast<f32>(sy) + 0.5f) / SAMPLES;
				if (covered(px, py) && !covered(px - offsetX, py - offsetY)) {
					++inside;
				}
			}
		}
		return static_cast<f32>(inside) / static_cast<f32>(SAMPLES * SAMPLES);
	};

	struct Band {
		f32 offsetX, offsetY;
		const char* css;
	};
	const Band bands[] = {
		{1.0f, 0.0f, "inset 1px 0 0 0"},
		{0.0f, 1.0f, "inset 0 1px 0 0"},
		{-1.0f, 0.0f, "inset -1px 0 0 0"},
		{0.0f, 6.0f, "inset 0 6px 0 0"}};

	for (const Band& band : bands) {
		UI::ContextDesc desc;
		desc.sizePx = size;
		desc.contentScale = 1.0f;
		UI::Context ctx(desc, nullptr);

		UI::Style style;
		style.position = UI::Position::Absolute;
		style.left = UI::Length::Pixel(box.x);
		style.top = UI::Length::Pixel(box.y);
		style.width = UI::Length::Pixel(box.w);
		style.height = UI::Length::Pixel(box.h);
		style.borderRadius = UI::Radii::All(RADIUS);
		UI::BoxShadow shadow;
		shadow.inset = true;
		shadow.x = band.offsetX;
		shadow.y = band.offsetY;
		shadow.color = Color::White();
		style.AddBoxShadow(shadow);
		MakeChild(ctx, ctx.GetRoot(), style);
		ctx.Update();

		UI::RenderList list;
		ctx.BuildRenderList(list);
		ASSERT_EQ(list.quads.size(), 1u) << band.css;

		Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
		if (!RenderListOffscreen(*RenderSystem::GetDevice(), list, size, texels)) {
			GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
		}
		const u16* halves = reinterpret_cast<const u16*>(texels.data());

		f32 worst = 0.0f;
		i32 worstX = 0, worstY = 0;
		i32 partials = 0;
		for (i32 y = 0; y < size.y; ++y) {
			for (i32 x = 0; x < size.x; ++x) {
				f32 measured = HalfToFloat(halves[(static_cast<size_t>(y) * size.x + x) * 4 + 3]);
				f32 expected = truth(x, y, band.offsetX, band.offsetY);
				if (std::abs(measured - expected) > worst) {
					worst = std::abs(measured - expected);
					worstX = x;
					worstY = y;
				}
				if (expected > 0.05f && expected < 0.95f) {
					++partials;
				}
			}
		}
		EXPECT_LE(worst, 0.05f) << band.css << ": worst pixel (" << worstX << ", " << worstY
								<< ") measured " << HalfToFloat(halves[(static_cast<size_t>(worstY) * size.x + worstX) * 4 + 3])
								<< " against " << truth(worstX, worstY, band.offsetX, band.offsetY);

		ASSERT_GE(partials, 16) << band.css;
	}
}

TEST_F(UIRenderFixture, ClippedChildCoversParent) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}

	const Vec2I size{40, 40};
	const UI::Rect leafBox{4.0f, 4.0f, 32.0f, 32.0f};
	constexpr f32 RADIUS = 6.0f;
	constexpr f32 TRACK_LEFT = 22.0f;
	auto ch = [](i32 v) { return static_cast<f32>(v) / 255.0f; };

	UI::ContextDesc desc;
	desc.sizePx = size;
	desc.contentScale = 1.0f;
	UI::Context ctx(desc, nullptr);

	UI::Style leaf;
	leaf.position = UI::Position::Absolute;
	leaf.left = UI::Length::Pixel(leafBox.x);
	leaf.top = UI::Length::Pixel(leafBox.y);
	leaf.width = UI::Length::Pixel(leafBox.w);
	leaf.height = UI::Length::Pixel(leafBox.h);
	leaf.borderRadius = UI::Radii::All(RADIUS);
	leaf.background = UI::Brush::SolidColor(Color(ch(0x2b), ch(0x2c), ch(0x2f), 1.0f));
	leaf.overflow = UI::Overflow::Hidden;
	UI::BoxShadow ring;
	ring.inset = true;
	ring.spread = 1.0f;
	ring.color = Color(1.0f, 1.0f, 1.0f, ch(0x08));
	leaf.AddBoxShadow(ring);
	UI::ElementHandle leafHandle = MakeChild(ctx, ctx.GetRoot(), leaf);

	UI::Style track;
	track.position = UI::Position::Absolute;
	track.left = UI::Length::Pixel(TRACK_LEFT);
	track.top = UI::Length::Pixel(0.0f);
	track.width = UI::Length::Pixel(leafBox.w - TRACK_LEFT);
	track.height = UI::Length::Pixel(leafBox.h);
	track.background = UI::Brush::SolidColor(Color(ch(0x14), ch(0x15), ch(0x16), 1.0f));
	MakeChild(ctx, leafHandle, track);

	ctx.Update();
	UI::RenderList list;
	ctx.BuildRenderList(list);

	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	if (!RenderListOffscreen(*RenderSystem::GetDevice(), list, size, texels)) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	const u16* halves = reinterpret_cast<const u16*>(texels.data());
	auto measured = [&](i32 x, i32 y) {
		return HalfToFloat(halves[(static_cast<size_t>(y) * size.x + x) * 4 + 1]) * 255.0f;
	};

	const f32 well = static_cast<f32>(0x15);
	auto truth = [&](i32 x, i32 y) {
		constexpr i32 SAMPLES = 32;
		f32 total = 0.0f;
		for (i32 sy = 0; sy < SAMPLES; ++sy) {
			for (i32 sx = 0; sx < SAMPLES; ++sx) {
				f32 px = static_cast<f32>(x) + (static_cast<f32>(sx) + 0.5f) / SAMPLES;
				f32 py = static_cast<f32>(y) + (static_cast<f32>(sy) + 0.5f) / SAMPLES;
				if (RoundedBoxSDF(px, py, leafBox, RADIUS) > 0.0f) {
					continue;
				}
				if (px >= leafBox.x + TRACK_LEFT) {
					total += well;
				} else {
					bool inRing = RoundedBoxSDF(px, py, leafBox, RADIUS) > -1.0f;
					total += inRing ? 51.0f : 44.0f;
				}
			}
		}
		return total / static_cast<f32>(SAMPLES * SAMPLES);
	};

	f32 worst = 0.0f;
	i32 worstX = 0, worstY = 0;
	for (i32 y = 24; y < 38; ++y) {
		for (i32 x = static_cast<i32>(leafBox.x + TRACK_LEFT); x < size.x; ++x) {
			f32 difference = std::abs(measured(x, y) - truth(x, y));
			if (difference > worst) {
				worst = difference;
				worstX = x;
				worstY = y;
			}
		}
	}
	EXPECT_LE(worst, 2.0f) << "worst pixel (" << worstX << ", " << worstY << ") measured "
						   << measured(worstX, worstY) << " against " << truth(worstX, worstY);

	for (i32 y = 24; y < 38; ++y) {
		for (i32 x = static_cast<i32>(leafBox.x + TRACK_LEFT); x < size.x; ++x) {
			EXPECT_LE(measured(x, y), well + 0.5f) << x << ", " << y << ")";
		}
	}

	i32 partials = 0;
	for (i32 y = 24; y < 38; ++y) {
		for (i32 x = static_cast<i32>(leafBox.x + TRACK_LEFT); x < size.x; ++x) {
			f32 value = measured(x, y);
			if (value > 1.0f && value < well - 1.0f) {
				++partials;
			}
		}
	}
	ASSERT_GE(partials, 4);
}

TEST_F(UIRenderFixture, GradientRampsBetweenColors) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}

	UI::UIQuad quad;
	quad.rect = {0.0f, 0.0f, 64.0f, 64.0f};
	quad.color0 = Color(1.0f, 0.0f, 0.0f, 1.0f);
	quad.color1 = Color(1.0f, 0.0f, 0.0f, 0.0f);
	quad.clipRect = UI::InfiniteClip;

	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	if (!RenderQuadsOffscreen(*RenderSystem::GetDevice(), {&quad, 1}, {64, 64}, texels)) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	const u16* halves = reinterpret_cast<const u16*>(texels.data());
	auto alphaAt = [halves](i32 x, i32 y) {
		return HalfToFloat(halves[(static_cast<size_t>(y) * 64 + x) * 4 + 3]);
	};
	auto redAt = [halves](i32 x, i32 y) {
		return HalfToFloat(halves[(static_cast<size_t>(y) * 64 + x) * 4]);
	};

	EXPECT_NEAR(alphaAt(0, 32), 1.0f, 0.03f);
	EXPECT_NEAR(alphaAt(63, 32), 0.0f, 0.03f);
	EXPECT_NEAR(alphaAt(32, 32), 0.5f, 0.06f);
	for (i32 x = 1; x < 64; ++x) {
		ASSERT_LE(alphaAt(x, 32), alphaAt(x - 1, 32) + 0.02f) << x;
	}
	EXPECT_NEAR(redAt(16, 32), alphaAt(16, 32), 0.03f);

	EXPECT_NEAR(alphaAt(16, 4), alphaAt(16, 60), 0.02f);
}

TEST_F(UIRenderFixture, RadialFillMasksWedge) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}

	constexpr f32 PI = 3.14159265f;
	UI::UIQuad quad;
	quad.rect = {0.0f, 0.0f, 64.0f, 64.0f};
	quad.color0 = Color(1.0f, 1.0f, 1.0f, 1.0f);
	quad.color1 = Color(32.0f, 32.0f, PI * 0.5f, PI);
	quad.clipRect = UI::InfiniteClip;
	quad.flags = UI::QUAD_FLAG_RADIAL_FILL;

	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	if (!RenderQuadsOffscreen(*RenderSystem::GetDevice(), {&quad, 1}, {64, 64}, texels)) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	const u16* halves = reinterpret_cast<const u16*>(texels.data());
	auto alphaAt = [halves](i32 x, i32 y) {
		return HalfToFloat(halves[(static_cast<size_t>(y) * 64 + x) * 4 + 3]);
	};

	EXPECT_NEAR(alphaAt(8, 32), 1.0f, 0.02f);
	EXPECT_NEAR(alphaAt(20, 50), 1.0f, 0.02f);
	EXPECT_NEAR(alphaAt(20, 12), 1.0f, 0.02f);
	EXPECT_NEAR(alphaAt(56, 32), 0.0f, 0.02f);
	EXPECT_NEAR(alphaAt(44, 50), 0.0f, 0.02f);
	EXPECT_NEAR(alphaAt(31, 56), 1.0f, 0.02f);
	EXPECT_NEAR(alphaAt(32, 56), 0.0f, 0.02f);
	EXPECT_NEAR(alphaAt(31, 8), 1.0f, 0.02f);
	EXPECT_NEAR(alphaAt(32, 8), 0.0f, 0.02f);

	quad.color1 = Color(32.0f, 32.0f, PI * 0.5f, -PI);
	ASSERT_TRUE(RenderQuadsOffscreen(*RenderSystem::GetDevice(), {&quad, 1}, {64, 64}, texels));
	halves = reinterpret_cast<const u16*>(texels.data());
	EXPECT_NEAR(alphaAt(8, 32), 0.0f, 0.02f);
	EXPECT_NEAR(alphaAt(56, 32), 1.0f, 0.02f);
	EXPECT_NEAR(alphaAt(32, 56), 1.0f, 0.02f);
	EXPECT_NEAR(alphaAt(31, 56), 0.0f, 0.02f);

	u32 edge = 0;
	for (i32 y = 0; y < 64; ++y) {
		for (i32 x = 0; x < 64; ++x) {
			const f32 alpha = alphaAt(x, y);
			if (alpha > 0.05f && alpha < 0.95f) {
				++edge;
			}
		}
	}
	EXPECT_LE(edge, 8u);
}

TEST_F(UIRenderFixture, HeaderKeepsLeafOutline) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	for (f32 scale : {1.0f, 1.25f}) {
		SCOPED_TRACE(scale);
		const Vec2I size{static_cast<i32>(128 * scale), static_cast<i32>(96 * scale)};
		UI::ContextDesc desc;
		desc.sizePx = size;
		desc.contentScale = scale;
		UI::Context ctx(desc, nullptr);
		EditorTheme::ApplyToContext(ctx);

		UI::Style host;
		host.position = UI::Position::Absolute;
		host.left = UI::Length::Pixel(4.0f);
		host.top = UI::Length::Pixel(4.0f);
		host.width = UI::Length::Pixel(120.0f);
		host.height = UI::Length::Pixel(88.0f);
		host.flexDirection = UI::FlexDirection::Column;
		UI::ElementHandle hostHandle = MakeChild(ctx, ctx.GetRoot(), host);
		UI::ElementHandle leaf = ctx.CreateElement();
		ctx.AddClass(leaf, StringHash("dock-leaf"));
		ctx.AppendChild(hostHandle, leaf);
		UI::Style tabBar = FixedBox(120.0f, 20.0f);
		tabBar.flexShrink = 0.0f;
		MakeChild(ctx, leaf, tabBar);
		UI::ElementHandle strip = PanelHeader::BuildStrip(ctx, leaf);
		UI::Style body;
		body.flexGrow = 1.0f;
		body.width = UI::Length::Percent(1.0f);
		MakeChild(ctx, leaf, body);
		ctx.Update();

		UI::RenderList list;
		ctx.BuildRenderList(list);
		Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
		if (!RenderListOffscreen(*RenderSystem::GetDevice(), list, size, texels)) {
			GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
		}
		const u16* halves = reinterpret_cast<const u16*>(texels.data());
		auto measured = [&](i32 x, i32 y) {
			return HalfToFloat(halves[(static_cast<size_t>(y) * size.x + x) * 4 + 1]) * 255.0f;
		};

		const UI::Rect leafBox = ctx.GetLayout(leaf).borderBox;
		const UI::Rect stripBox = ctx.GetLayout(strip).borderBox;
		const i32 left = static_cast<i32>(std::lround(leafBox.x));
		const i32 right = static_cast<i32>(std::lround(leafBox.x + leafBox.w)) - 1;
		const i32 stripTop = static_cast<i32>(std::ceil(stripBox.y));
		const i32 stripBottom = static_cast<i32>(std::floor(stripBox.y + stripBox.h));
		const i32 farRow = static_cast<i32>(leafBox.y + leafBox.h) - 12;
		const i32 middle = (left + right) / 2;

		ASSERT_GT(measured(left, farRow), measured(left + 2, farRow) + 1.0f);
		for (i32 y = stripTop; y < stripBottom - 1; ++y) {
			EXPECT_NEAR(measured(left, y), measured(left, farRow), 0.5f) << y;
			EXPECT_NEAR(measured(right, y), measured(right, farRow), 0.5f) << y;
		}

		ASSERT_LT(measured(middle, stripBottom), measured(middle, farRow) - 1.0f);
		for (i32 y = stripBottom; y < stripBottom + static_cast<i32>(std::ceil(4.0f * scale)); ++y) {
			EXPECT_NEAR(measured(left + 1, y), measured(middle, y), 0.5f) << y;
			EXPECT_NEAR(measured(right - 1, y), measured(middle, y), 0.5f) << y;
		}
		EXPECT_NEAR(measured(middle, stripTop - 1), measured(middle, farRow), 0.5f);
	}
}

TEST_F(UIRenderFixture, EllipsizedRunEndsWithRasterizedMark) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	Handle<FontAsset> font = UISystem::GetDefaultFont();
	if (!font.IsValid()) {
		GTEST_SKIP() << "engine font not mounted";
	}
	UI::IFontSystem* fonts = UISystem::GetFontSystem();
	ASSERT_NE(fonts, nullptr);

	auto query = [&](std::string_view text, UI::TextOverflow overflow, f32 width) {
		UI::TextQuery out;
		out.font = font;
		out.sizePx = 16.0f;
		out.utf8 = text;
		out.overflow = overflow;
		out.wrapWidth = width;
		return out;
	};

	UI::ShapedRun mark;
	ASSERT_TRUE(fonts->ShapeText(query("\xE2\x80\xA6", UI::TextOverflow::Wrap, 0.0f), mark));
	ASSERT_FALSE(mark.glyphs.empty());

	constexpr std::string_view Text = "New Object (Copy) (Copy)";
	const f32 budget = fonts->MeasureText(query(Text, UI::TextOverflow::Wrap, 0.0f)).x * 0.5f;
	UI::ShapedRun run;
	ASSERT_TRUE(fonts->ShapeText(query(Text, UI::TextOverflow::Ellipsis, budget), run));
	ASSERT_FALSE(run.glyphs.empty());

	const UI::Rect& tail = run.glyphs.back().uv;
	const UI::Rect& wanted = mark.glyphs.front().uv;
	EXPECT_FLOAT_EQ(tail.x, wanted.x);
	EXPECT_FLOAT_EQ(tail.y, wanted.y);
	EXPECT_FLOAT_EQ(tail.w, wanted.w);
	EXPECT_FLOAT_EQ(tail.h, wanted.h);

	EXPECT_LE(run.glyphs.back().rect.x + run.glyphs.back().rect.w, budget);
}

TEST_F(UIRenderFixture, AlignedGlyphsMoveWithTheirLine) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	Handle<FontAsset> font = UISystem::GetDefaultFont();
	if (!font.IsValid()) {
		GTEST_SKIP() << "engine font not mounted";
	}
	UI::IFontSystem* fonts = UISystem::GetFontSystem();
	ASSERT_NE(fonts, nullptr);

	UI::TextQuery query;
	query.font = font;
	query.sizePx = 16.0f;
	query.utf8 = "short\nlonger line";
	query.outlineWidth = 1.0f;
	UI::ShapedRun start;
	ASSERT_TRUE(fonts->ShapeText(query, start));
	query.align = UI::Align::End;
	UI::ShapedRun end;
	ASSERT_TRUE(fonts->ShapeText(query, end));

	const f32 shift = end.StopForByte(0).x;
	EXPECT_FLOAT_EQ(shift, UI::SnapToPixel(start.extent.x - start.CaretXForByte(5)));
	ASSERT_GT(shift, 10.0f);
	ASSERT_EQ(end.glyphs.size(), start.glyphs.size());
	ASSERT_EQ(end.outlineGlyphs.size(), start.outlineGlyphs.size());
	ASSERT_GT(start.glyphs.size(), 5u);
	ASSERT_GT(start.outlineGlyphs.size(), 5u);
	for (size_t k = 0; k < start.glyphs.size(); ++k) {
		EXPECT_FLOAT_EQ(end.glyphs[k].rect.x, start.glyphs[k].rect.x + (k < 5 ? shift : 0.0f)) << k;
		EXPECT_FLOAT_EQ(end.glyphs[k].rect.y, start.glyphs[k].rect.y) << k;
	}
	for (size_t k = 0; k < start.outlineGlyphs.size(); ++k) {
		EXPECT_FLOAT_EQ(end.outlineGlyphs[k].rect.x, start.outlineGlyphs[k].rect.x + (k < 5 ? shift : 0.0f)) << k;
	}
}

TEST_F(UIRenderFixture, CenteredLinesInkAboutBoxCenter) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	Handle<FontAsset> font = UISystem::GetDefaultFont();
	if (!font.IsValid()) {
		GTEST_SKIP() << "engine font not mounted";
	}

	const Vec2I size{160, 48};
	UI::ContextDesc desc;
	desc.sizePx = size;
	desc.contentScale = 1.0f;
	UI::Context ctx(desc, UISystem::GetFontSystem());
	UI::Style style = FixedBox(160.0f, 48.0f);
	style.textAlign = UI::Align::Center;
	UI::ElementHandle label = MakeChild(ctx, ctx.GetRoot(), style);
	ctx.SetText(label, font, 16.0f, Color::White(), "ab\nabcdefghij");
	ctx.Update();
	UI::RenderList list;
	ctx.BuildRenderList(list);

	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	if (!RenderListOffscreen(*RenderSystem::GetDevice(), list, size, texels)) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	const u16* halves = reinterpret_cast<const u16*>(texels.data());
	auto alpha = [&](i32 x, i32 y) { return HalfToFloat(halves[(static_cast<size_t>(y) * size.x + x) * 4 + 3]); };

	const UI::Context::ShapedTextView view = ctx.GetShapedText(label);
	ASSERT_NE(view.run, nullptr);
	ASSERT_EQ(view.run->LineCount(), 2u);
	for (u32 line = 0; line < 2; ++line) {
		const i32 top = static_cast<i32>(view.originPx.y + static_cast<f32>(line) * view.run->lineHeight);
		const i32 bottom = std::min(size.y, static_cast<i32>(top + view.run->lineHeight));
		i32 left = size.x;
		i32 right = -1;
		for (i32 y = std::max(0, top); y < bottom; ++y) {
			for (i32 x = 0; x < size.x; ++x) {
				if (alpha(x, y) > 0.25f) {
					left = std::min(left, x);
					right = std::max(right, x);
				}
			}
		}
		ASSERT_GE(right, left) << line;
		EXPECT_NEAR(static_cast<f32>(left + right + 1) * 0.5f, static_cast<f32>(size.x) * 0.5f, 2.0f) << line;
	}
}

TEST_F(UIRenderFixture, BoldAndItalicTextReadBack) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	Handle<FontAsset> font = UISystem::GetDefaultFont();
	if (!font.IsValid()) {
		GTEST_SKIP() << "engine font not mounted";
	}

	const Vec2I size{160, 48};
	UI::ContextDesc desc;
	desc.sizePx = size;
	desc.contentScale = 1.0f;
	UI::Context ctx(desc, UISystem::GetFontSystem());
	UI::ElementHandle label = MakeChild(ctx, ctx.GetRoot(), FixedBox(160.0f, 48.0f));
	ctx.SetText(label, font, 32.0f, Color::White(), "llll");

	struct Ink {
		f32 total = 0.0f;
		f32 lean = 0.0f;
	};
	auto draw = [&](u16 weight, UI::FontStyle fontStyle, Ink& out) {
		UI::Style style = FixedBox(160.0f, 48.0f);
		style.fontWeight = weight;
		style.fontStyle = fontStyle;
		ctx.SetStyle(label, style);
		ctx.Update();
		UI::RenderList list;
		ctx.BuildRenderList(list);
		Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
		if (!RenderListOffscreen(*RenderSystem::GetDevice(), list, size, texels)) {
			return false;
		}
		const u16* halves = reinterpret_cast<const u16*>(texels.data());
		auto alpha = [&](i32 x, i32 y) { return HalfToFloat(halves[(static_cast<size_t>(y) * size.x + x) * 4 + 3]); };
		i32 top = size.y;
		i32 bottom = -1;
		for (i32 y = 0; y < size.y; ++y) {
			for (i32 x = 0; x < size.x; ++x) {
				const f32 a = alpha(x, y);
				out.total += a;
				if (a > 0.25f) {
					top = std::min(top, y);
					bottom = std::max(bottom, y);
				}
			}
		}
		const i32 band = std::max(1, (bottom - top + 1) / 4);
		auto centroid = [&](i32 from, i32 to) {
			f32 sum = 0.0f;
			f32 weighted = 0.0f;
			for (i32 y = from; y < to; ++y) {
				for (i32 x = 0; x < size.x; ++x) {
					sum += alpha(x, y);
					weighted += alpha(x, y) * static_cast<f32>(x);
				}
			}
			return sum > 0.0f ? weighted / sum : 0.0f;
		};
		out.lean = centroid(top, top + band) - centroid(bottom + 1 - band, bottom + 1);
		return bottom >= top;
	};

	Ink regular, bold, italic;
	if (!draw(400, UI::FontStyle::Normal, regular)) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	ASSERT_TRUE(draw(700, UI::FontStyle::Normal, bold));
	ASSERT_TRUE(draw(400, UI::FontStyle::Italic, italic));

	EXPECT_GT(bold.total, regular.total * 1.3f);
	EXPECT_NEAR(regular.lean, 0.0f, 1.0f);
	EXPECT_GT(italic.lean, 3.0f);
	EXPECT_NEAR(italic.total, regular.total, regular.total * 0.1f);
}

TEST_F(UIRenderFixture, LineHeightTextReadBack) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	Handle<FontAsset> font = UISystem::GetDefaultFont();
	if (!font.IsValid()) {
		GTEST_SKIP() << "engine font not mounted";
	}

	const Vec2I size{64, 128};
	UI::ContextDesc desc;
	desc.sizePx = size;
	desc.contentScale = 1.0f;
	UI::Context ctx(desc, UISystem::GetFontSystem());
	UI::ElementHandle label = MakeChild(ctx, ctx.GetRoot(), FixedBox(64.0f, 128.0f));
	ctx.SetText(label, font, 20.0f, Color::White(), "H\nH");

	auto inkStarts = [&](UI::Length lineHeight, Vector<i32>& starts) {
		UI::Style style = FixedBox(64.0f, 128.0f);
		style.lineHeight = lineHeight;
		ctx.SetStyle(label, style);
		ctx.Update();
		UI::RenderList list;
		ctx.BuildRenderList(list);
		Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
		if (!RenderListOffscreen(*RenderSystem::GetDevice(), list, size, texels)) {
			return false;
		}
		const u16* halves = reinterpret_cast<const u16*>(texels.data());
		bool previous = false;
		for (i32 y = 0; y < size.y; ++y) {
			bool inked = false;
			for (i32 x = 0; x < size.x; ++x) {
				inked = inked || HalfToFloat(halves[(static_cast<size_t>(y) * size.x + x) * 4 + 3]) > 0.25f;
			}
			if (inked && !previous) {
				starts.push_back(y);
			}
			previous = inked;
		}
		return true;
	};

	Vector<i32> normal{GetMemoryResource(MemTag::Debug)};
	Vector<i32> loose{GetMemoryResource(MemTag::Debug)};
	if (!inkStarts(UI::Length::Auto(), normal)) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	ASSERT_TRUE(inkStarts(UI::Length::Pixel(50.0f), loose));
	ASSERT_EQ(normal.size(), 2u);
	ASSERT_EQ(loose.size(), 2u);

	const i32 fontLine = static_cast<i32>(UISystem::GetFontSystem()->GetLineHeight(font, 20.0f));
	ASSERT_LT(fontLine, 40);
	EXPECT_EQ(normal[1] - normal[0], fontLine);
	EXPECT_EQ(loose[1] - loose[0], 50);
	EXPECT_EQ(loose[0] - normal[0], (50 - fontLine) / 2);
}

TEST_F(UIRenderFixture, RichTextReadBack) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	Handle<FontAsset> font = UISystem::GetDefaultFont();
	if (!font.IsValid()) {
		GTEST_SKIP() << "engine font not mounted";
	}

	const Vec2I size{200, 48};
	UI::ContextDesc desc;
	desc.sizePx = size;
	desc.contentScale = 1.0f;
	UI::Context ctx(desc, UISystem::GetFontSystem());
	UI::ElementHandle label = MakeChild(ctx, ctx.GetRoot(), FixedBox(200.0f, 48.0f));
	ctx.SetText(label, font, 32.0f, Color::White(), "llll");
	ctx.SetRichText(label, true);

	struct Texel {
		f32 r, g, b, a;
	};
	std::vector<Texel> texels;
	auto draw = [&](std::string_view text) {
		ctx.SetTextString(label, text);
		ctx.Update();
		UI::RenderList list;
		ctx.BuildRenderList(list);
		Vector<u8> bytes{GetMemoryResource(MemTag::Debug)};
		if (!RenderListOffscreen(*RenderSystem::GetDevice(), list, size, bytes)) {
			return false;
		}
		const u16* halves = reinterpret_cast<const u16*>(bytes.data());
		texels.clear();
		for (size_t i = 0; i < static_cast<size_t>(size.x) * size.y; ++i) {
			texels.push_back({HalfToFloat(halves[i * 4]), HalfToFloat(halves[i * 4 + 1]), HalfToFloat(halves[i * 4 + 2]), HalfToFloat(halves[i * 4 + 3])});
		}
		return true;
	};
	auto at = [&](i32 x, i32 y) -> const Texel& { return texels[static_cast<size_t>(y) * size.x + x]; };

	if (!draw("llll")) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	i32 plainBottom = -1;
	i32 left = size.x;
	i32 right = -1;
	for (i32 y = 0; y < size.y; ++y) {
		for (i32 x = 0; x < size.x; ++x) {
			if (at(x, y).a > 0.5f) {
				plainBottom = std::max(plainBottom, y);
				left = std::min(left, x);
				right = std::max(right, x);
			}
		}
	}
	ASSERT_GT(right, left);

	ASSERT_TRUE(draw("<color=#f00>llll</color><color=#0f0>llll</color>"));
	i32 lastRed = -1;
	i32 firstGreen = size.x;
	u32 red = 0;
	u32 green = 0;
	for (i32 y = 0; y < size.y; ++y) {
		for (i32 x = 0; x < size.x; ++x) {
			const Texel& texel = at(x, y);
			if (texel.a > 0.9f && texel.r > 0.8f && texel.g < 0.2f) {
				++red;
				lastRed = std::max(lastRed, x);
			} else if (texel.a > 0.9f && texel.g > 0.8f && texel.r < 0.2f) {
				++green;
				firstGreen = std::min(firstGreen, x);
			}
		}
	}
	EXPECT_GT(red, 20u);
	EXPECT_GT(green, 20u);
	EXPECT_LT(lastRed, firstGreen);

	ASSERT_TRUE(draw("<u>llll</u>"));
	i32 widest = 0;
	for (i32 y = plainBottom + 1; y < size.y; ++y) {
		i32 inked = 0;
		for (i32 x = 0; x < size.x; ++x) {
			inked += at(x, y).a > 0.5f ? 1 : 0;
		}
		widest = std::max(widest, inked);
	}
	EXPECT_GE(widest, (right - left + 1) * 4 / 5);

	ASSERT_TRUE(draw("<mark=#00f>llll</mark>"));
	u32 white = 0;
	u32 blue = 0;
	for (const Texel& texel : texels) {
		if (texel.r > 0.8f && texel.g > 0.8f && texel.b > 0.8f) {
			++white;
		} else if (texel.b > 0.8f && texel.r < 0.2f && texel.g < 0.2f) {
			++blue;
		}
	}
	EXPECT_GT(white, 20u);
	EXPECT_GT(blue, 100u);
}

TEST_F(UIRenderFixture, CenteredInputInksAboutBoxCenter) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	Handle<FontAsset> font = UISystem::GetDefaultFont();
	if (!font.IsValid()) {
		GTEST_SKIP() << "engine font not mounted";
	}

	const Vec2I size{160, 32};
	UI::ContextDesc desc;
	desc.sizePx = size;
	desc.contentScale = 1.0f;
	UI::Context ctx(desc, UISystem::GetFontSystem());
	UI::Style style = FixedBox(160.0f, 32.0f);
	style.textAlign = UI::Align::Center;
	UI::ElementHandle input = MakeChild(ctx, ctx.GetRoot(), style);
	ctx.SetText(input, font, 16.0f, Color::White(), "abc");
	ctx.SetTextInput(input, true);

	auto inkColumns = [&](i32& left, i32& right) {
		ctx.Update();
		UI::RenderList list;
		ctx.BuildRenderList(list);
		Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
		if (!RenderListOffscreen(*RenderSystem::GetDevice(), list, size, texels)) {
			return false;
		}
		const u16* halves = reinterpret_cast<const u16*>(texels.data());
		left = size.x;
		right = -1;
		for (i32 y = 0; y < size.y; ++y) {
			for (i32 x = 0; x < size.x; ++x) {
				if (HalfToFloat(halves[(static_cast<size_t>(y) * size.x + x) * 4 + 3]) > 0.25f) {
					left = std::min(left, x);
					right = std::max(right, x);
				}
			}
		}
		return true;
	};

	i32 left = 0;
	i32 right = 0;
	if (!inkColumns(left, right)) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	ASSERT_GE(right, left);
	EXPECT_GT(left, 40);
	EXPECT_NEAR(static_cast<f32>(left + right + 1) * 0.5f, static_cast<f32>(size.x) * 0.5f, 2.0f);

	ctx.SetTextString(input, "a value far wider than the box it sits in");
	ASSERT_TRUE(inkColumns(left, right));
	EXPECT_LE(left, 2);
	EXPECT_GE(right, size.x - 3);
}

TEST_F(UIRenderFixture, FocusedEmptyInputInksCaret) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	Handle<FontAsset> font = UISystem::GetDefaultFont();
	if (!font.IsValid()) {
		GTEST_SKIP() << "engine font not mounted";
	}

	const Vec2I size{64, 32};
	UI::ContextDesc desc;
	desc.sizePx = size;
	desc.contentScale = 1.0f;
	UI::Context ctx(desc, UISystem::GetFontSystem());
	UI::Style style = FixedBox(64.0f, 32.0f);
	style.textAlignVertical = UI::Align::Center;
	UI::ElementHandle input = MakeChild(ctx, ctx.GetRoot(), style);
	ctx.SetText(input, font, 16.0f, Color::White(), "");
	ctx.SetTextInput(input, true);
	ctx.Update();
	ctx.SetFocus(input);
	ctx.Update();
	ASSERT_EQ(ctx.GetFocused(), input);

	struct Ink {
		i32 left;
		i32 right;
		i32 top;
		i32 bottom;
	};
	auto inkBounds = [&](Ink& ink) {
		UI::RenderList list;
		ctx.BuildRenderList(list);
		ink = {size.x, -1, size.y, -1};
		if (list.quads.empty()) {
			return true;
		}
		Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
		if (!RenderListOffscreen(*RenderSystem::GetDevice(), list, size, texels)) {
			return false;
		}
		const u16* halves = reinterpret_cast<const u16*>(texels.data());
		for (i32 y = 0; y < size.y; ++y) {
			for (i32 x = 0; x < size.x; ++x) {
				if (HalfToFloat(halves[(static_cast<size_t>(y) * size.x + x) * 4 + 3]) > 0.25f) {
					ink = {std::min(ink.left, x), std::max(ink.right, x), std::min(ink.top, y), std::max(ink.bottom, y)};
				}
			}
		}
		return true;
	};

	Ink empty{};
	if (!inkBounds(empty)) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	const i32 fontLine = static_cast<i32>(UISystem::GetFontSystem()->GetLineHeight(font, 16.0f));
	ASSERT_GT(fontLine, 0);
	EXPECT_EQ(empty.left, 0);
	EXPECT_EQ(empty.right, 0);
	EXPECT_EQ(empty.bottom - empty.top + 1, fontLine);
	EXPECT_NEAR(static_cast<f32>(empty.top + empty.bottom + 1) * 0.5f, static_cast<f32>(size.y) * 0.5f, 1.0f);

	InputEvent space{};
	space.type = InputEventType::Text;
	space.codepoint = U' ';
	ctx.InjectInput(space);
	ctx.Update();
	ASSERT_EQ(ctx.GetTextString(input), " ");
	Ink spaced{};
	ASSERT_TRUE(inkBounds(spaced));
	EXPECT_GT(spaced.left, empty.left);
	EXPECT_EQ(spaced.right, spaced.left);
	EXPECT_EQ(spaced.top, empty.top);
	EXPECT_EQ(spaced.bottom, empty.bottom);
}

namespace {

	f32 TurnedRoundedBoxSDF(f32 px, f32 py, const UI::Rect& box, f32 radius, f32 rotation) {
		const f32 centreX = box.x + box.w * 0.5f;
		const f32 centreY = box.y + box.h * 0.5f;
		const f32 c = std::cos(rotation);
		const f32 s = std::sin(rotation);
		const f32 dx = px - centreX;
		const f32 dy = py - centreY;
		return RoundedBoxSDF(centreX + dx * c + dy * s, centreY - dx * s + dy * c, box, radius);
	}

	f32 ExactTurnedCoverage(i32 x, i32 y, const UI::Rect& box, f32 radius, f32 rotation) {
		constexpr i32 SAMPLES = 64;
		i32 inside = 0;
		for (i32 sy = 0; sy < SAMPLES; ++sy) {
			for (i32 sx = 0; sx < SAMPLES; ++sx) {
				const f32 px = static_cast<f32>(x) + (static_cast<f32>(sx) + 0.5f) / SAMPLES;
				const f32 py = static_cast<f32>(y) + (static_cast<f32>(sy) + 0.5f) / SAMPLES;
				if (TurnedRoundedBoxSDF(px, py, box, radius, rotation) <= 0.0f) {
					++inside;
				}
			}
		}
		return static_cast<f32>(inside) / static_cast<f32>(SAMPLES * SAMPLES);
	}

	using StrokeDraw = void (*)(UI::PaintScope&);

	bool RenderStrokeOffscreen(StrokeDraw draw, Vector<u8>& outTexels, const UI::Rect* clip = nullptr) {
		UI::ContextDesc desc;
		desc.sizePx = {64, 64};
		desc.contentScale = 1.0f;
		UI::Context ctx(desc, nullptr);

		UI::ElementHandle host = ctx.GetRoot();
		if (clip) {
			UI::Style clipStyle = FixedBox(clip->w, clip->h);
			clipStyle.position = UI::Position::Absolute;
			clipStyle.left = UI::Length::Pixel(clip->x);
			clipStyle.top = UI::Length::Pixel(clip->y);
			clipStyle.overflow = UI::Overflow::Hidden;
			host = MakeChild(ctx, ctx.GetRoot(), clipStyle);
		}
		UI::ElementHandle painter = MakeChild(ctx, host, FixedBox(1.0f, 1.0f));
		ctx.Events(painter).customDraw = [draw](UI::Context&, UI::ElementHandle, UI::PaintScope& scope) { draw(scope); };
		ctx.Update();

		UI::RenderList list;
		ctx.BuildRenderList(list);
		return RenderListOffscreen(*RenderSystem::GetDevice(), list, {64, 64}, outTexels);
	}

} // namespace

TEST_F(UIRenderFixture, TurnedRecordCoversGeometry) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}

	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	const bool rendered = RenderStrokeOffscreen(
		[](UI::PaintScope& scope) { scope.EmitLine({14.0f, 14.0f}, {50.0f, 50.0f}, 8.0f, Color::White()); }, texels);
	if (!rendered) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	const u16* halves = reinterpret_cast<const u16*>(texels.data());
	auto coverage = [&](i32 x, i32 y) { return HalfToFloat(halves[(static_cast<size_t>(y) * 64 + x) * 4]); };

	EXPECT_NEAR(coverage(20, 20), 1.0f, 0.01f);
	EXPECT_NEAR(coverage(44, 44), 1.0f, 0.01f);
	EXPECT_NEAR(coverage(44, 20), 0.0f, 0.01f);
	EXPECT_NEAR(coverage(20, 44), 0.0f, 0.01f);
	EXPECT_NEAR(coverage(52, 32), 0.0f, 0.01f);

	f32 ink = 0.0f;
	for (i32 y = 0; y < 64; ++y) {
		for (i32 x = 0; x < 64; ++x) {
			ink += coverage(x, y);
		}
	}
	EXPECT_NEAR(ink, 36.0f * 1.4142136f * 8.0f, 2.0f);
}

TEST_F(UIRenderFixture, TurnedStrokesCoverageTracksSupersampledTruth) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}

	struct Sweep {
		Vec2 from;
		Vec2 to;
		f32 width;
		void (*draw)(UI::PaintScope&);
	};
	const Sweep sweeps[] = {
		{{12.0f, 12.0f},
		 {52.0f, 52.0f},
		 5.0f,
		 [](UI::PaintScope& scope) {
			 scope.EmitLine({12.0f, 12.0f}, {52.0f, 52.0f}, 5.0f, Color::White(), UI::LineCap::Round);
		 }},
		{{12.0f, 12.0f},
		 {52.0f, 35.0f},
		 5.0f,
		 [](UI::PaintScope& scope) {
			 scope.EmitLine({12.0f, 12.0f}, {52.0f, 35.0f}, 5.0f, Color::White(), UI::LineCap::Round);
		 }},
		{{12.0f, 12.0f},
		 {40.0f, 52.0f},
		 5.0f,
		 [](UI::PaintScope& scope) {
			 scope.EmitLine({12.0f, 12.0f}, {40.0f, 52.0f}, 5.0f, Color::White(), UI::LineCap::Round);
		 }},
		{{18.0f, 24.0f},
		 {46.0f, 32.0f},
		 18.0f,
		 [](UI::PaintScope& scope) {
			 scope.EmitLine({18.0f, 24.0f}, {46.0f, 32.0f}, 18.0f, Color::White(), UI::LineCap::Round);
		 }},
	};

	for (const Sweep& sweep : sweeps) {
		Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
		if (!RenderStrokeOffscreen(sweep.draw, texels)) {
			GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
		}
		const u16* halves = reinterpret_cast<const u16*>(texels.data());

		const f32 dx = sweep.to.x - sweep.from.x;
		const f32 dy = sweep.to.y - sweep.from.y;
		const f32 rotation = std::atan2(dy, dx);
		const f32 radius = sweep.width * 0.5f;
		const f32 boxLength = std::sqrt(dx * dx + dy * dy) + sweep.width;
		const UI::Rect box = {(sweep.from.x + sweep.to.x) * 0.5f - boxLength * 0.5f,
							  (sweep.from.y + sweep.to.y) * 0.5f - radius, boxLength, sweep.width};

		i32 partials = 0;
		f32 worst = 0.0f;
		i32 worstX = 0;
		i32 worstY = 0;
		for (i32 y = 0; y < 64; ++y) {
			for (i32 x = 0; x < 64; ++x) {
				const f32 dist = TurnedRoundedBoxSDF(static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f,
													 box, radius, rotation);
				if (dist < -1.5f || dist > 1.5f) {
					continue;
				}
				const f32 measured = HalfToFloat(halves[(static_cast<size_t>(y) * 64 + x) * 4]);
				const f32 error = std::abs(measured - ExactTurnedCoverage(x, y, box, radius, rotation));
				if (error > worst) {
					worst = error;
					worstX = x;
					worstY = y;
				}
				if (measured > 0.02f && measured < 0.98f) {
					++partials;
				}
			}
		}

		ASSERT_GE(partials, 70);
		EXPECT_LE(worst, 0.03f) << "angle " << rotation << ' ' << worstX << ", " << worstY << "), error " << worst;
	}
}

TEST_F(UIRenderFixture, TurnedStrokeClipped) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}

	const UI::Rect clip = {0.0f, 0.0f, 64.0f, 32.0f};
	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	const bool rendered = RenderStrokeOffscreen(
		[](UI::PaintScope& scope) { scope.EmitLine({0.0f, 0.0f}, {64.0f, 64.0f}, 3.0f, Color::White()); }, texels,
		&clip);
	if (!rendered) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	const u16* halves = reinterpret_cast<const u16*>(texels.data());
	auto coverage = [&](i32 x, i32 y) { return HalfToFloat(halves[(static_cast<size_t>(y) * 64 + x) * 4]); };

	i32 above = 0;
	for (i32 y = 0; y < 64; ++y) {
		for (i32 x = 0; x < 64; ++x) {
			if (y >= 33) {
				ASSERT_LE(coverage(x, y), 0.01f) << x << ", " << y << ")";
			} else if (coverage(x, y) > 0.5f) {
				++above;
			}
		}
	}
	ASSERT_GE(above, 20);
	EXPECT_GT(coverage(30, 30), 0.5f);
}

namespace {

	UI::Style TurnedBox(f32 left, f32 top, f32 width, f32 height, f32 rotate, Color color) {
		UI::Style style;
		style.position = UI::Position::Absolute;
		style.left = UI::Length::Pixel(left);
		style.top = UI::Length::Pixel(top);
		style.width = UI::Length::Pixel(width);
		style.height = UI::Length::Pixel(height);
		style.rotate = rotate;
		if (color.a > 0.0f) {
			style.background = UI::Brush::SolidColor(color);
		}
		return style;
	}

	bool RenderContextOffscreen(UI::Context& ctx, Vec2I size, Vector<u8>& texels) {
		ctx.Update();
		UI::RenderList list;
		ctx.BuildRenderList(list);
		return RenderListOffscreen(*RenderSystem::GetDevice(), list, size, texels);
	}

	UI::ContextDesc TurnedDesc() {
		UI::ContextDesc desc;
		desc.sizePx = {64, 64};
		desc.contentScale = 1.0f;
		return desc;
	}

} // anonymous namespace

TEST_F(UIRenderFixture, TurnedRightAngleLandsOnPixels) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	UI::Context ctx(TurnedDesc(), nullptr);
	UI::ElementHandle bar = MakeChild(ctx, ctx.GetRoot(), TurnedBox(12.0f, 28.0f, 40.0f, 8.0f, 90.0f, Color::Clear()));
	MakeChild(ctx, bar, TurnedBox(0.0f, 0.0f, 20.0f, 8.0f, 0.0f, Color(1.0f, 0.0f, 0.0f, 1.0f)));
	MakeChild(ctx, bar, TurnedBox(20.0f, 0.0f, 20.0f, 8.0f, 0.0f, Color(0.0f, 0.0f, 1.0f, 1.0f)));

	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	if (!RenderContextOffscreen(ctx, {64, 64}, texels)) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	const u16* halves = reinterpret_cast<const u16*>(texels.data());
	auto channel = [&](i32 x, i32 y, i32 c) { return HalfToFloat(halves[(static_cast<size_t>(y) * 64 + x) * 4 + c]); };

	i32 red = 0;
	i32 blue = 0;
	for (i32 y = 0; y < 64; ++y) {
		for (i32 x = 0; x < 64; ++x) {
			const bool column = x >= 28 && x <= 35;
			const f32 wantRed = column && y >= 12 && y <= 31 ? 1.0f : 0.0f;
			const f32 wantBlue = column && y >= 32 && y <= 51 ? 1.0f : 0.0f;
			ASSERT_NEAR(channel(x, y, 0), wantRed, 0.5f / 255.0f) << x << ", " << y;
			ASSERT_NEAR(channel(x, y, 2), wantBlue, 0.5f / 255.0f) << x << ", " << y;
			red += wantRed > 0.0f ? 1 : 0;
			blue += wantBlue > 0.0f ? 1 : 0;
		}
	}
	EXPECT_EQ(red, 160);
	EXPECT_EQ(blue, 160);
}

TEST_F(UIRenderFixture, TurnedEighthCoversItsArea) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	UI::Context ctx(TurnedDesc(), nullptr);
	MakeChild(ctx, ctx.GetRoot(), TurnedBox(17.0f, 27.0f, 30.0f, 10.0f, 45.0f, Color::White()));

	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	if (!RenderContextOffscreen(ctx, {64, 64}, texels)) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	const u16* halves = reinterpret_cast<const u16*>(texels.data());
	auto coverage = [&](i32 x, i32 y) { return HalfToFloat(halves[(static_cast<size_t>(y) * 64 + x) * 4]); };

	f32 area = 0.0f;
	i32 partials = 0;
	for (i32 y = 0; y < 64; ++y) {
		for (i32 x = 0; x < 64; ++x) {
			const f32 c = coverage(x, y);
			area += c;
			partials += c > 0.05f && c < 0.95f ? 1 : 0;
			if (std::abs(x + 0.5f - 32.0f) > 15.5f || std::abs(y + 0.5f - 32.0f) > 15.5f) {
				ASSERT_LE(c, 0.001f) << x << ", " << y;
			}
		}
	}
	EXPECT_NEAR(area, 300.0f, 6.0f);
	EXPECT_GE(partials, 40);
	EXPECT_GT(coverage(32, 32), 0.999f);
	EXPECT_LE(coverage(40, 25), 0.001f);
	EXPECT_GT(coverage(38, 37), 0.999f);
}

TEST_F(UIRenderFixture, TurnedChildCutByParentClip) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	UI::Context ctx(TurnedDesc(), nullptr);
	UI::Style frame = TurnedBox(0.0f, 0.0f, 64.0f, 32.0f, 0.0f, Color::Clear());
	frame.overflow = UI::Overflow::Hidden;
	UI::ElementHandle clipper = MakeChild(ctx, ctx.GetRoot(), frame);
	MakeChild(ctx, clipper, TurnedBox(12.0f, 12.0f, 40.0f, 8.0f, 90.0f, Color::White()));

	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	if (!RenderContextOffscreen(ctx, {64, 64}, texels)) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	const u16* halves = reinterpret_cast<const u16*>(texels.data());
	auto coverage = [&](i32 x, i32 y) { return HalfToFloat(halves[(static_cast<size_t>(y) * 64 + x) * 4]); };

	i32 inked = 0;
	for (i32 y = 0; y < 64; ++y) {
		for (i32 x = 0; x < 64; ++x) {
			if (y >= 32) {
				ASSERT_LE(coverage(x, y), 0.001f) << x << ", " << y;
			} else if (coverage(x, y) > 0.999f) {
				++inked;
			}
		}
	}
	EXPECT_EQ(inked, 8 * 32);
	EXPECT_GT(coverage(32, 0), 0.999f);
}

namespace {

	f64 Overlap(i32 pixel, f64 lo, f64 hi) {
		return std::clamp(std::min(pixel + 1.0, hi) - std::max(static_cast<f64>(pixel), lo), 0.0, 1.0);
	}

	f64 RectCoverage(i32 x, i32 y, const UI::Rect& rect) {
		return Overlap(x, rect.x, rect.x + rect.w) * Overlap(y, rect.y, rect.y + rect.h);
	}

	void ExpectCoverage(const Vector<u8>& texels, i32 channel, const std::function<f64(i32, i32)>& expected) {
		const u16* halves = reinterpret_cast<const u16*>(texels.data());
		for (i32 y = 0; y < 64; ++y) {
			for (i32 x = 0; x < 64; ++x) {
				ASSERT_NEAR(HalfToFloat(halves[(static_cast<size_t>(y) * 64 + x) * 4 + channel]), expected(x, y), 0.005) << x << ", " << y;
			}
		}
	}

	f64 Total(const Vector<u8>& texels, i32 channel) {
		const u16* halves = reinterpret_cast<const u16*>(texels.data());
		f64 total = 0.0;
		for (size_t texel = 0; texel < 64 * 64; ++texel) {
			total += HalfToFloat(halves[texel * 4 + channel]);
		}
		return total;
	}

	bool RenderCustomDraw(Vector<u8>& texels, const std::function<void(UI::PaintScope&)>& draw) {
		UI::Context ctx(TurnedDesc(), nullptr);
		UI::ElementHandle host = MakeChild(ctx, ctx.GetRoot(), FixedBox(64.0f, 64.0f));
		ctx.Events(host).customDraw = [&draw](UI::Context&, UI::ElementHandle, UI::PaintScope& scope) { draw(scope); };
		return RenderContextOffscreen(ctx, {64, 64}, texels);
	}

} // anonymous namespace

TEST_F(UIRenderFixture, SubPixelQuadMovesInEighths) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	for (i32 step = 0; step < 8; ++step) {
		const f32 shift = static_cast<f32>(step) * 0.125f;
		const UI::Rect rect{20.0f + shift, 30.0f + shift * 0.5f, 9.5f, 6.25f};
		for (const bool snap : {false, true}) {
			Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
			const bool drawn = RenderCustomDraw(texels, [&](UI::PaintScope& scope) {
				scope.SetSnapToPixels(snap);
				scope.EmitQuad(rect, Color::White());
			});
			if (!drawn) {
				GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
			}
			SCOPED_TRACE(testing::Message() << step << snap);
			const UI::Rect placed = snap ? UI::SnapRectToPixels(rect) : rect;
			ExpectCoverage(texels, 0, [&](i32 x, i32 y) { return RectCoverage(x, y, placed); });
		}
	}
}

TEST_F(UIRenderFixture, SubPixelQuadCutByFractionalClip) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	UI::UIQuad quad;
	quad.rect = {12.375f, 14.625f, 10.5f, 7.25f};
	quad.uv = {0.0f, 0.0f, 1.0f, 1.0f};
	quad.color0 = Color::White();
	quad.color1 = quad.color0;
	quad.clipRect = {16.25f, 15.5f, 30.0f, 30.0f};
	quad.flags = UI::QUAD_FLAG_SUBPIXEL;

	UI::RenderList list;
	list.quads.push_back(quad);
	UI::UICommand command;
	command.kind = UI::UICommandKind::Quads;
	command.firstQuad = 0;
	command.quadCount = 1;
	list.commands.push_back(command);

	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	if (!RenderListOffscreen(*RenderSystem::GetDevice(), list, {64, 64}, texels)) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	const UI::Rect shown = UI::RectIntersect(quad.rect, quad.clipRect);
	ExpectCoverage(texels, 0, [&](i32 x, i32 y) { return RectCoverage(x, y, shown); });
}

TEST_F(UIRenderFixture, SubPixelSpriteStaysInsideUVRect) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	RHI::Device* device = RenderSystem::GetDevice();
	ASSERT_NE(device, nullptr);

	const u8 strip[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 255, 0, 0, 255};
	RHI::TextureDesc sheetDesc;
	sheetDesc.size = {4, 1};
	sheetDesc.format = RHI::Format::RGBA8Unorm;
	sheetDesc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst;
	sheetDesc.debugName = "SubPixelSpriteSheet";
	RHI::TextureHandle sheet = device->CreateTextureResource(sheetDesc);
	ASSERT_TRUE(sheet.IsValid());
	device->UploadTexture(sheet, strip);

	const UI::Rect sprite{0.25f, 0.0f, 0.5f, 1.0f};
	const UI::Rect oneToOne{20.375f, 10.5f, 2.0f, 3.0f};
	const UI::Rect stretched{30.625f, 20.25f, 7.0f, 3.0f};
	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	const bool drawn = RenderCustomDraw(texels, [&](UI::PaintScope& scope) {
		scope.SetSnapToPixels(false);
		scope.EmitTexturedQuad(oneToOne, sheet, sprite, Color::White());
		scope.EmitTexturedQuad(stretched, sheet, sprite, Color::White());
	});
	device->DestroyResource(sheet);
	if (!drawn) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	EXPECT_LT(Total(texels, 0), 0.001);
	ExpectCoverage(texels, 1, [&](i32 x, i32 y) { return RectCoverage(x, y, oneToOne) + RectCoverage(x, y, stretched); });
}

TEST_F(UIRenderFixture, SubPixelCoverageQuadCoversArea) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	const UI::Rect rect{20.375f, 20.625f, 10.5f, 7.25f};
	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	const bool drawn = RenderCustomDraw(texels, [&](UI::PaintScope& scope) {
		scope.SetSnapToPixels(false);
		scope.EmitTexturedQuad(rect, {}, {0.0f, 0.0f, 1.0f, 1.0f}, Color::White(), UI::SamplerKind::LinearClamp, {},
							   UI::TexelEncoding::Coverage);
	});
	if (!drawn) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	ExpectCoverage(texels, 0, [&](i32 x, i32 y) { return RectCoverage(x, y, rect); });
}

TEST_F(UIRenderFixture, SubPixelRoundedCoverageQuadCoversArea) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	const UI::Rect rect{20.375f, 20.625f, 12.0f, 10.0f};
	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	const bool drawn = RenderCustomDraw(texels, [&](UI::PaintScope& scope) {
		scope.SetSnapToPixels(false);
		scope.EmitTexturedQuad(rect, {}, {0.0f, 0.0f, 1.0f, 1.0f}, Color::White(), UI::SamplerKind::LinearClamp,
							   UI::Radii::All(3.0f), UI::TexelEncoding::Coverage);
	});
	if (!drawn) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	EXPECT_NEAR(Total(texels, 0), rect.w * rect.h - (4.0 - 3.14159265) * 9.0, 0.5);
}

TEST_F(UIRenderFixture, SubPixelRingCoversArea) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	const UI::Rect rect{20.375f, 20.625f, 12.0f, 10.0f};
	Vector<u8> texels{GetMemoryResource(MemTag::Debug)};
	const bool drawn = RenderCustomDraw(texels, [&](UI::PaintScope& scope) {
		scope.SetSnapToPixels(false);
		scope.EmitRing(rect, UI::Edges::All(2.0f), Color::White());
	});
	if (!drawn) {
		GTEST_SKIP() << "engine://Shaders/UI.hlsl unavailable (dxcompiler not staged)";
	}
	const UI::Rect hole{rect.x + 2.0f, rect.y + 2.0f, rect.w - 4.0f, rect.h - 4.0f};
	ExpectCoverage(texels, 0, [&](i32 x, i32 y) { return RectCoverage(x, y, rect) - RectCoverage(x, y, hole); });
}

TEST_F(UIRenderLogFixture, DestroyedTextureFallsBack) {
	if (!RenderSystem::IsRenderingAvailable()) {
		GTEST_SKIP() << "no Vulkan device available";
	}
	RHI::Device* device = RenderSystem::GetDevice();

	Handle<Window> window = MakeHiddenWindow();
	ASSERT_TRUE(window.IsValid());
	UI::Context* ctx = UISystem::CreateContext(window);
	ASSERT_NE(ctx, nullptr);

	RHI::TextureDesc desc;
	desc.size = {8, 8};
	desc.format = RHI::Format::RGBA8Unorm;
	desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst;
	desc.debugName = "PreviewUnderARow";
	RHI::TextureHandle preview = device->CreateTextureResource(desc);
	ASSERT_TRUE(preview.IsValid());
	Vector<u8> pixels{GetMemoryResource(MemTag::Debug)};
	pixels.resize(8 * 8 * 4, 0x80);
	device->UploadTexture(preview, {pixels.data(), pixels.size()});

	UI::ElementHandle row = ctx->CreateElement();
	ctx->AppendChild(ctx->GetRoot(), row);
	UI::Style rowStyle;
	rowStyle.width = UI::Length::Pixel(64.0f);
	rowStyle.height = UI::Length::Pixel(64.0f);
	rowStyle.background = UI::Brush::ImageBrush(preview);
	ctx->SetStyle(row, rowStyle);

	WindowSystem::PumpEvents();
	UISystem::Update();
	RenderSystem::RenderAll();
	ASSERT_GT(UISystem::GetStats().quads, 0u);

	LogManager::SetGlobalLevel(LogLevel::Trace);
	auto sink = std::make_shared<StaleTextureCapturingSink>();
	LogManager::AddSink(sink);
	ASSERT_TRUE(ArmUIRenderSink(sink));

	device->DestroyResource(preview);
	ASSERT_FALSE(device->IsAlive(preview));
	ASSERT_TRUE(preview.IsValid());

	for (int frame = 0; frame < 2; ++frame) {
		WindowSystem::PumpEvents();
		UISystem::Update();
		RenderSystem::RenderAll();
	}
	ASSERT_TRUE(ArmUIRenderSink(sink));

	EXPECT_EQ(sink->CountMatching("stale texture"), 0u);
	LogManager::RemoveSink(sink);

	WindowSystem::RequestDestroy(window);
	WindowSystem::PumpEvents();
}
