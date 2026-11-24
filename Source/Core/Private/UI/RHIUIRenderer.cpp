#include "UI/RHIUIRenderer.h"

#include "Asset/EngineBuiltins.h"
#include "UI/LayerPassStack.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Graphics/RHICommandList.h"
#include "Shalltear/Core/Graphics/RHIDevice.h"
#include "Shalltear/Core/Graphics/ShaderAsset.h"
#include "Shalltear/Core/Profiler.h"

#include <cmath>
#include <cstring>
#include <functional>

namespace Shalltear::UI {

	namespace {

		struct UIPushConstants {
			f32 viewportSize[2];
			f32 originPx[2];
			u64 quadAddress;
			u32 textureFlags;
			u32 passMode;
		};
		static_assert(sizeof(UIPushConstants) == 32);

		constexpr u32 UI_TEXTURE_DECODES = 1u;

		// must match UI_PASS_* in UI.hlsl
		constexpr u32 UI_PASS_COLOR = 0u;
		constexpr u32 UI_PASS_COVERAGE = 1u;
		constexpr u32 UI_PASS_BLUR_X = 2u;
		constexpr u32 UI_PASS_BLUR_Y = 3u;

		u32 TextureFlagsFor(RHI::Format format) {
			return RHI::IsSRGB(format) ? UI_TEXTURE_DECODES : 0u;
		}

		RectI ToPixelRect(const Rect& rect) {
			i32 x = static_cast<i32>(std::floor(rect.x));
			i32 y = static_cast<i32>(std::floor(rect.y));
			i32 x1 = static_cast<i32>(std::ceil(rect.x + rect.w));
			i32 y1 = static_cast<i32>(std::ceil(rect.y + rect.h));
			return RectI{x, y, std::max(0, x1 - x), std::max(0, y1 - y)};
		}

	} // anonymous namespace

	RHIUIRenderer::RHIUIRenderer(RHI::Device* device)
		: _device(device) {
	}

	RHIUIRenderer::~RHIUIRenderer() {
		if (!_device) {
			return;
		}
		for (PipelineEntry& entry : _pipelines) {
			if (entry.pipeline.IsValid()) {
				_device->DestroyResource(entry.pipeline);
			}
		}
		for (size_t i = 0; i < static_cast<size_t>(SamplerKind::Count); ++i) {
			if (_samplers[i].IsValid()) {
				_device->DestroyResource(_samplers[i]);
			}
		}
		if (_whiteTexture.IsValid()) {
			_device->DestroyResource(_whiteTexture);
		}
		for (PooledTarget& target : _targetPool) {
			if (target.texture.IsValid()) {
				_device->DestroyResource(target.texture);
			}
		}
		for (RHI::TextureHandle atlas : _atlases) {
			if (atlas.IsValid()) {
				_device->DestroyResource(atlas);
			}
		}
		if (Asset* shader = _shader.Get()) {
			AssetDatabase::UnloadAsset(shader);
		}
	}

	// UNORM, the painter blends in encoded space
	RHI::TextureHandle RHIUIRenderer::CreateAtlas(Vec2I size, AtlasFormat format) {
		if (!_device) {
			return {};
		}
		RHI::TextureDesc desc;
		desc.size = size;
		desc.format = format == AtlasFormat::RGBA8 ? RHI::Format::RGBA8Unorm : RHI::Format::R8Unorm;
		desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst;
		desc.debugName = format == AtlasFormat::RGBA8 ? "UIIconAtlas" : "UIGlyphAtlas";
		RHI::TextureHandle atlas = _device->CreateTextureResource(desc);
		if (atlas.IsValid()) {
			ClearAtlas(atlas);
			_atlases.push_back(atlas);
		}
		return atlas;
	}

	void RHIUIRenderer::ClearAtlas(RHI::TextureHandle atlas) {
		const Vec2I size = _device->GetTextureSize(atlas);
		Vector<u8> zero{GetMemoryResource(MemTag::UI)};
		zero.resize(static_cast<size_t>(RHI::FormatLevelBytes(_device->GetTextureFormat(atlas), size)), 0);
		_device->UploadTexture(atlas, {zero.data(), zero.size()});
	}

	void RHIUIRenderer::FillWhiteTexture() {
		const u8 whitePixel[4] = {255, 255, 255, 255};
		_device->UploadTexture(_whiteTexture, whitePixel);
	}

	void RHIUIRenderer::RestoreDeviceContent() {
		if (!_device) {
			return;
		}
		for (PipelineEntry& entry : _pipelines) {
			if (entry.pipeline.IsValid()) {
				_device->DestroyResource(entry.pipeline);
			}
		}
		_pipelines.clear();
		if (_whiteTexture.IsValid()) {
			FillWhiteTexture();
		}
		for (RHI::TextureHandle atlas : _atlases) {
			ClearAtlas(atlas);
		}
	}

	void RHIUIRenderer::UpdateAtlas(RHI::TextureHandle atlas, RectI region, std::span<const u8> pixels) {
		if (_device && atlas.IsValid()) {
			_device->UploadTextureRegion(atlas, pixels, region);
		}
	}

	RHI::PipelineHandle RHIUIRenderer::GetOrCreatePipeline(RHI::Format targetFormat) {
		if (!_shaderLoadAttempted) {
			_shaderLoadAttempted = true;
			Asset* asset = AssetDatabase::LoadAssetByGuid(EngineBuiltins::UIShader);
			if (asset && asset->GetAssetTypeId() == StringHash("ShaderAsset")) {
				_shader = Handle<ShaderAsset>(static_cast<ShaderAsset*>(asset));
				_shaderVersion = static_cast<ShaderAsset*>(asset)->version;
			} else {
				if (asset) {
					AssetDatabase::UnloadAsset(asset);
				}
				SLT_LOG_WARN("UI", "the UI shader is unavailable. UI rendering disabled");
			}
		}
		ShaderAsset* shader = _shader.Get();
		if (!shader) {
			return {};
		}

		if (shader->version != _shaderVersion) {
			for (PipelineEntry& entry : _pipelines) {
				if (entry.pipeline.IsValid()) {
					_device->DestroyResource(entry.pipeline);
				}
			}
			_pipelines.clear();
			_shaderVersion = shader->version;
		}

		for (const PipelineEntry& entry : _pipelines) {
			if (entry.format == targetFormat) {
				return entry.pipeline;
			}
		}

		const ShaderVariant* code = shader->GetVariant(SHADER_BASE_VARIANT);
		if (!code) {
			return {};
		}

		RHI::GraphicsPipelineDesc desc;
		desc.vertexSpirv = {code->vertexSpirv.data(), code->vertexSpirv.size()};
		desc.pixelSpirv = {code->pixelSpirv.data(), code->pixelSpirv.size()};
		desc.bytecodeFormat = shader->bytecodeFormat;
		desc.bindings = {shader->bindings.data(), shader->bindings.size()};
		desc.pushConstantSize = shader->pushConstantSize;
		desc.blend = shader->renderState.blend;
		desc.depth = shader->renderState.depth;
		desc.cull = shader->renderState.cull;
		desc.colorFormats[0] = targetFormat;
		desc.colorFormatCount = 1;
		desc.depthFormat = RHI::Format::Unknown;
		desc.debugName = "UIPipeline";
		RHI::PipelineHandle pipeline = _device->CreateGraphicsPipeline(desc);
		if (pipeline.IsValid()) {
			_pipelines.push_back({targetFormat, pipeline});
		}
		return pipeline;
	}

	RHI::TextureHandle RHIUIRenderer::AcquireTarget(Vec2I size, RHI::Format format, const char* debugName) {
		for (PooledTarget& target : _targetPool) {
			if (!target.usedThisSubmit && target.size == size && target.format == format) {
				target.usedThisSubmit = true;
				target.lastUsedSubmit = _submitIndex;
				return target.texture;
			}
		}
		RHI::TextureDesc desc;
		desc.size = size;
		desc.format = format;
		desc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::Sampled;
		desc.debugName = debugName;
		RHI::TextureHandle texture = _device->CreateTextureResource(desc);
		if (texture.IsValid()) {
			_targetPool.push_back({texture, size, format, true, _submitIndex});
		}
		return texture;
	}

	RHI::TextureHandle RHIUIRenderer::DrawShadowMasks(RHI::CommandList& cmd, const RenderList& list, u64 quadsAddress,
													  u64 blurQuadsAddress, u32 blurQuadCount) {
		RHI::Device& device = *_device;
		const Vec2I size = list.shadowAtlasSizePx;
		RHI::PipelineHandle pipeline = GetOrCreatePipeline(RHI::Format::R8Unorm);
		if (!pipeline.IsValid() || blurQuadCount == 0) {
			return {};
		}
		RHI::TextureHandle mask = AcquireTarget(size, RHI::Format::R8Unorm, "UIShadowMask");
		RHI::TextureHandle blurred = AcquireTarget(size, RHI::Format::R8Unorm, "UIShadowBlur");
		if (!mask.IsValid() || !blurred.IsValid()) {
			return {};
		}

		struct CoverageTarget {
			RHI::TextureHandle texture;
			Vec2I size{};
			RHI::PipelineHandle pipeline;
		};
		const CoverageTarget maskTarget{mask, size, pipeline};
		const CoverageTarget blurTarget{blurred, size, pipeline};

		RHI::TextureHandle boundTexture{0xFFFFFFFEu, 0};
		SamplerKind boundSampler = SamplerKind::Count;
		u32 boundFlags = 0;
		auto open = [&](const CoverageTarget& target, bool clear) {
			cmd.TransitionTexture(target.texture, RHI::ResourceState::RenderTarget);
			RHI::RenderPassDesc::Attachment attachment;
			attachment.texture = target.texture;
			attachment.load = clear ? RHI::LoadOp::Clear : RHI::LoadOp::Load;
			attachment.clearColor = Color::Clear();
			RHI::RenderPassDesc pass;
			pass.colorAttachments = {&attachment, 1};
			cmd.BeginRenderPass(pass);
			cmd.SetPipeline(target.pipeline);
			cmd.SetViewportScissor(RectI{0, 0, target.size.x, target.size.y});
			boundTexture = RHI::TextureHandle{0xFFFFFFFEu, 0};
			boundSampler = SamplerKind::Count;
		};
		auto bind = [&](const CoverageTarget& target, RHI::TextureHandle texture, SamplerKind sampler) {
			if (texture == boundTexture && sampler == boundSampler) {
				return;
			}
			RHI::TransientSet set = device.CreateTransientSet(target.pipeline, 1)
										.WriteTexture(1, texture)
										.WriteSampler(2, _samplers[static_cast<size_t>(sampler)])
										.Commit();
			cmd.BindTransientSet(1, set);
			boundTexture = texture;
			boundSampler = sampler;
			boundFlags = TextureFlagsFor(device.GetTextureFormat(texture));
		};
		auto draw = [&](const CoverageTarget& target, u64 address, Vec2 origin, u32 passMode, u32 quadCount) {
			UIPushConstants push;
			push.viewportSize[0] = static_cast<f32>(target.size.x);
			push.viewportSize[1] = static_cast<f32>(target.size.y);
			push.originPx[0] = origin.x;
			push.originPx[1] = origin.y;
			push.quadAddress = address;
			push.textureFlags = boundFlags;
			push.passMode = passMode;
			cmd.PushConstants({reinterpret_cast<const u8*>(&push), sizeof(push)});
			cmd.Draw(quadCount * 6);
			++_lastDrawCalls;
		};
		auto quadAddress = [&](u32 firstQuad) { return quadsAddress + static_cast<u64>(firstQuad) * sizeof(UIQuad); };

		const RHI::PipelineHandle layerPipeline = GetOrCreatePipeline(RHI::Format::RGBA16Float);
		std::function<void(size_t, size_t, const CoverageTarget&, const RectI&, Vec2)> cover;
		cover = [&](size_t begin, size_t end, const CoverageTarget& target, const RectI& scissor, Vec2 origin) {
			for (size_t j = begin; j < end; ++j) {
				const UICommand& command = list.commands[j];
				if (command.kind == UICommandKind::PushLayer) {
					size_t pop = j + 1;
					for (u32 depth = 1; pop < end; ++pop) {
						depth += list.commands[pop].kind == UICommandKind::PushLayer ? 1u : 0u;
						depth -= list.commands[pop].kind == UICommandKind::PopLayer ? 1u : 0u;
						if (depth == 0) {
							break;
						}
					}
					const size_t composite = pop + 1;
					const bool turned = composite < end && list.commands[composite].kind == UICommandKind::Quads &&
										list.commands[composite].texture == LayerCompositeTexture && list.commands[composite].quadCount > 0 &&
										(list.quads[list.commands[composite].firstQuad].flags & QUAD_FLAG_ROTATED) != 0;
					if (!turned) {
						continue;
					}
					const RectI bounds = ToPixelRect(command.layerBounds);
					const Vec2I layerSize{std::max(1, bounds.size.x), std::max(1, bounds.size.y)};
					const CoverageTarget scratch{AcquireTarget(layerSize, RHI::Format::RGBA16Float, "UIShadowLayer"), layerSize, layerPipeline};
					if (scratch.texture.IsValid() && scratch.pipeline.IsValid()) {
						cmd.EndRenderPass();
						open(scratch, true);
						cover(j + 1, pop, scratch, RectI{0, 0, layerSize.x, layerSize.y},
							  {static_cast<f32>(bounds.position.x), static_cast<f32>(bounds.position.y)});
						cmd.EndRenderPass();
						cmd.TransitionTexture(scratch.texture, RHI::ResourceState::ShaderResource);
						open(target, false);
						cmd.SetScissor(scissor);
						bind(target, scratch.texture, list.commands[composite].sampler);
						draw(target, quadAddress(list.commands[composite].firstQuad), origin, UI_PASS_COVERAGE, 1);
					}
					j = composite;
					continue;
				}
				if (command.kind != UICommandKind::Quads || command.quadCount == 0 ||
					command.texture == ShadowMaskTexture || command.texture == LayerCompositeTexture) {
					continue;
				}
				RHI::TextureHandle texture = command.texture;
				if (!texture.IsValid() || !device.IsAlive(texture)) {
					texture = _whiteTexture;
				}
				bind(target, texture, command.sampler);
				draw(target, quadAddress(command.firstQuad), origin, UI_PASS_COVERAGE, command.quadCount);
			}
		};

		open(maskTarget, true);
		for (size_t i = 0; i < list.commands.size(); ++i) {
			if (list.commands[i].kind != UICommandKind::PushShadow) {
				continue;
			}
			const UIShadowCaster& caster = list.shadowCasters[list.commands[i].shadowCaster];
			if (caster.slot.size.x <= 0 || caster.slot.size.y <= 0) {
				continue;
			}
			size_t scopeEnd = i + 1;
			for (u32 depth = 1; scopeEnd < list.commands.size(); ++scopeEnd) {
				depth += list.commands[scopeEnd].kind == UICommandKind::PushShadow ? 1u : 0u;
				depth -= list.commands[scopeEnd].kind == UICommandKind::PopShadow ? 1u : 0u;
				if (depth == 0) {
					break;
				}
			}
			cmd.SetScissor(caster.slot);
			const Vec2 origin{caster.source.x - static_cast<f32>(caster.slot.position.x + caster.marginPx),
							  caster.source.y - static_cast<f32>(caster.slot.position.y + caster.marginPx)};
			cover(i + 1, scopeEnd, maskTarget, caster.slot, origin);
		}
		cmd.EndRenderPass();

		cmd.TransitionTexture(mask, RHI::ResourceState::ShaderResource);
		open(blurTarget, true);
		bind(blurTarget, mask, SamplerKind::NearestClamp);
		draw(blurTarget, blurQuadsAddress, {0.0f, 0.0f}, UI_PASS_BLUR_X, blurQuadCount);
		cmd.EndRenderPass();
		cmd.TransitionTexture(blurred, RHI::ResourceState::ShaderResource);
		open(maskTarget, true);
		bind(maskTarget, blurred, SamplerKind::NearestClamp);
		draw(maskTarget, blurQuadsAddress, {0.0f, 0.0f}, UI_PASS_BLUR_Y, blurQuadCount);
		cmd.EndRenderPass();
		cmd.TransitionTexture(mask, RHI::ResourceState::ShaderResource);
		return mask;
	}

	void RHIUIRenderer::Submit(const RenderList& list, const SubmitInfo& info) {
		_lastDrawCalls = 0;
		_lastQuads = 0;
		if (!_device || !info.commandList || !info.target.IsValid() || list.commands.empty() || list.quads.empty()) {
			return;
		}
		RHI::Device& device = *_device;
		RHI::CommandList& cmd = *info.commandList;

		RHI::Format targetFormat = device.GetTextureFormat(info.target);
		RHI::PipelineHandle targetPipeline = GetOrCreatePipeline(targetFormat);
		if (!targetPipeline.IsValid()) {
			return;
		}

		if (!_samplersCreated) {
			RHI::SamplerDesc samplerDesc;
			samplerDesc.filter = RHI::FilterMode::Linear;
			samplerDesc.addressMode = RHI::AddressMode::ClampToEdge;
			samplerDesc.debugName = "UILinearClamp";
			_samplers[static_cast<size_t>(SamplerKind::LinearClamp)] = device.CreateSamplerResource(samplerDesc);
			samplerDesc.addressMode = RHI::AddressMode::Repeat;
			samplerDesc.debugName = "UILinearWrap";
			_samplers[static_cast<size_t>(SamplerKind::LinearWrap)] = device.CreateSamplerResource(samplerDesc);
			samplerDesc.mipFilter = RHI::FilterMode::Nearest;
			samplerDesc.addressMode = RHI::AddressMode::ClampToEdge;
			samplerDesc.debugName = "UIBilinearClamp";
			_samplers[static_cast<size_t>(SamplerKind::BilinearClamp)] = device.CreateSamplerResource(samplerDesc);
			samplerDesc.filter = RHI::FilterMode::Nearest;
			samplerDesc.debugName = "UINearestClamp";
			_samplers[static_cast<size_t>(SamplerKind::NearestClamp)] = device.CreateSamplerResource(samplerDesc);
			samplerDesc.addressMode = RHI::AddressMode::Repeat;
			samplerDesc.debugName = "UINearestWrap";
			_samplers[static_cast<size_t>(SamplerKind::NearestWrap)] = device.CreateSamplerResource(samplerDesc);
			_samplersCreated = true;
		}
		if (!_whiteTexture.IsValid()) {
			RHI::TextureDesc whiteDesc;
			whiteDesc.size = {1, 1};
			whiteDesc.format = RHI::Format::RGBA8Unorm;
			whiteDesc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst;
			whiteDesc.debugName = "UIWhite";
			_whiteTexture = device.CreateTextureResource(whiteDesc);
			FillWhiteTexture();
		}

		u32 blurQuadCount = 0;
		if (list.shadowAtlasSizePx.x > 0 && list.shadowAtlasSizePx.y > 0) {
			for (const UIShadowCaster& caster : list.shadowCasters) {
				blurQuadCount += caster.slot.size.x > 0 && caster.slot.size.y > 0 ? 1u : 0u;
			}
		}
		const u64 quadBytes = list.quads.size() * sizeof(UIQuad);
		RHI::TransientAllocation allocation = device.AllocateTransient(quadBytes + blurQuadCount * sizeof(UIQuad), 16);
		if (!allocation.IsValid()) {
			return;
		}
		std::memcpy(allocation.cpu, list.quads.data(), quadBytes);
		if (blurQuadCount > 0) {
			UIQuad* blurQuads = reinterpret_cast<UIQuad*>(static_cast<u8*>(allocation.cpu) + quadBytes);
			for (const UIShadowCaster& caster : list.shadowCasters) {
				if (caster.slot.size.x <= 0 || caster.slot.size.y <= 0) {
					continue;
				}
				UIQuad quad;
				quad.rect = {static_cast<f32>(caster.slot.position.x), static_cast<f32>(caster.slot.position.y),
							 static_cast<f32>(caster.slot.size.x), static_cast<f32>(caster.slot.size.y)};
				quad.color0 = Color::White();
				quad.color1 = Color::White();
				quad.clipRect = InfiniteClip;
				quad.softness = caster.blurPx;
				*blurQuads++ = quad;
			}
		}
		const u64 baseAddress = device.GetBufferDeviceAddress(allocation.buffer) + allocation.offset;
		if (baseAddress == allocation.offset) {
			return;
		}

		++_submitIndex;
		for (size_t i = _targetPool.size(); i-- > 0;) {
			PooledTarget& target = _targetPool[i];
			target.usedThisSubmit = false;
			if (target.lastUsedSubmit + RETENTION_SUBMITS >= _submitIndex) {
				continue;
			}
			if (target.texture.IsValid()) {
				device.DestroyResource(target.texture);
			}
			_targetPool.erase(_targetPool.begin() + static_cast<i64>(i));
		}
		while (_targetPool.size() > MAX_POOLED_TARGETS) {
			size_t oldest = 0;
			for (size_t i = 1; i < _targetPool.size(); ++i) {
				if (_targetPool[i].lastUsedSubmit < _targetPool[oldest].lastUsedSubmit) {
					oldest = i;
				}
			}
			if (_targetPool[oldest].texture.IsValid()) {
				device.DestroyResource(_targetPool[oldest].texture);
			}
			_targetPool.erase(_targetPool.begin() + static_cast<i64>(oldest));
		}

		RHI::TextureHandle shadowMask{};
		if (blurQuadCount > 0) {
			SLT_PROFILE_ZONE("UI.ShadowMasks");
			shadowMask = DrawShadowMasks(cmd, list, baseAddress, baseAddress + quadBytes, blurQuadCount);
		}

		LayerPassStack passStack;
		passStack.Reset(info.target, info.targetSizePx);

		auto pipelineFor = [&](const LayerPass& entry) -> RHI::PipelineHandle {
			RHI::Format format = device.GetTextureFormat(entry.target);
			return GetOrCreatePipeline(format);
		};

		auto openPass = [&](const LayerPass& entry, bool clear) -> bool {
			RHI::PipelineHandle pipeline = pipelineFor(entry);
			if (!pipeline.IsValid()) {
				return false;
			}
			RHI::RenderPassDesc::Attachment attachment;
			attachment.texture = entry.target;
			attachment.load = clear ? RHI::LoadOp::Clear : RHI::LoadOp::Load;
			attachment.clearColor = Color::Clear();
			RHI::RenderPassDesc pass;
			pass.colorAttachments = {&attachment, 1};
			cmd.BeginRenderPass(pass);
			cmd.SetPipeline(pipeline);
			cmd.SetViewportScissor(RectI{0, 0, entry.sizePx.x, entry.sizePx.y});
			return true;
		};

		RHI::TextureHandle boundTexture{0xFFFFFFFEu, 0};
		SamplerKind boundSampler = SamplerKind::Count;
		u32 boundTextureFlags = 0;
		u64 pushedAddress = ~0ull;
		u32 pushedTextureFlags = ~0u;

		auto resetBindings = [&] {
			boundTexture = RHI::TextureHandle{0xFFFFFFFEu, 0};
			boundSampler = SamplerKind::Count;
			pushedAddress = ~0ull;
			pushedTextureFlags = ~0u;
		};

		if (!openPass(passStack.Top(), false)) {
			return;
		}

		for (const UICommand& command : list.commands) {
			switch (command.kind) {
			case UICommandKind::PushLayer: {
				cmd.EndRenderPass();

				RectI bounds = ToPixelRect(command.layerBounds);
				Vec2I layerSize = {std::max(1, bounds.size.x), std::max(1, bounds.size.y)};
				Vec2 origin{static_cast<f32>(bounds.position.x), static_cast<f32>(bounds.position.y)};

				RHI::TextureHandle layer = AcquireTarget(layerSize, RHI::Format::RGBA16Float, "UILayer");

				if (!layer.IsValid()) {
					openPass(passStack.Push(RHI::TextureHandle{}, layerSize, origin), false);
					resetBindings();
					break;
				}

				cmd.TransitionTexture(layer, RHI::ResourceState::RenderTarget);
				if (!openPass(passStack.Push(layer, layerSize, origin), true)) {
					openPass(passStack.DegradeTop(), false);
				}
				resetBindings();
				break;
			}
			case UICommandKind::PopLayer: {
				LayerPassStack::PopResult pop = passStack.Pop();
				if (!pop.popped) {
					break;
				}
				cmd.EndRenderPass();
				if (pop.closed.layered) {
					cmd.TransitionTexture(pop.closed.target, RHI::ResourceState::ShaderResource);
				}
				openPass(pop.parent, false);
				resetBindings();
				break;
			}
			case UICommandKind::PushShadow:
			case UICommandKind::PopShadow:
				break;
			case UICommandKind::Quads: {
				if (command.quadCount == 0) {
					break;
				}
				RHI::TextureHandle texture = command.texture;
				if (texture == ShadowMaskTexture) {
					if (!shadowMask.IsValid()) {
						break;
					}
					texture = shadowMask;
				}
				if (texture == LayerCompositeTexture) {
					texture = passStack.CompositeSource();
					if (!texture.IsValid() || !device.IsAlive(texture)) {
						break;
					}
				}
				if (!texture.IsValid() || !device.IsAlive(texture)) {
					texture = _whiteTexture;
				}
				SamplerKind sampler = command.sampler;

				if (!(texture == boundTexture) || sampler != boundSampler) {
					SLT_PROFILE_ZONE("UI.Set");
					RHI::PipelineHandle pipeline = pipelineFor(passStack.Top());
					RHI::TransientSet set = device.CreateTransientSet(pipeline, 1)
												.WriteTexture(1, texture)
												.WriteSampler(2, _samplers[static_cast<size_t>(sampler)])
												.Commit();
					cmd.BindTransientSet(1, set);
					boundTexture = texture;
					boundSampler = sampler;
					boundTextureFlags = TextureFlagsFor(device.GetTextureFormat(texture));
				}

				u64 address = baseAddress + static_cast<u64>(command.firstQuad) * sizeof(UIQuad);
				if (address != pushedAddress || boundTextureFlags != pushedTextureFlags) {
					const LayerPass& pass = passStack.Top();
					UIPushConstants push;
					push.viewportSize[0] = static_cast<f32>(pass.sizePx.x);
					push.viewportSize[1] = static_cast<f32>(pass.sizePx.y);
					push.originPx[0] = pass.originPx.x;
					push.originPx[1] = pass.originPx.y;
					push.quadAddress = address;
					push.textureFlags = boundTextureFlags;
					push.passMode = UI_PASS_COLOR;
					cmd.PushConstants({reinterpret_cast<const u8*>(&push), sizeof(push)});
					pushedAddress = address;
					pushedTextureFlags = boundTextureFlags;
				}

				cmd.Draw(command.quadCount * 6);
				++_lastDrawCalls;
				_lastQuads += command.quadCount;
				break;
			}
			}
		}

		while (passStack.Depth() > 1) {
			cmd.EndRenderPass();
			openPass(passStack.Pop().parent, false);
		}
		cmd.EndRenderPass();
	}

} // namespace Shalltear::UI
