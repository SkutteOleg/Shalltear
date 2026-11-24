#include "Graphics/WebGPU/WebGPUInternal.h"

#include <algorithm>
#include <cstring>

namespace Shalltear::RHI {

	namespace {

		void BindSet3ForPass(DeviceImpl& d, CommandListImpl& cmd, const PipelineResource& pipeline) {
			if (!pipeline.bindsTable) {
				return;
			}
			FrameContext& frame = d.frames[d.frameIndex];
			if (frame.set3HeapVersion != d.vertexHeap.version || !frame.set3GraphicsGroup || !frame.set3ComputeReadWriteGroup) {
				d.EnsureFrameSet3BindGroups(frame);
			}
			const bool rw = pipeline.isCompute && pipeline.tableWritable;
			WGPUBindGroup group = rw ? frame.set3ComputeReadWriteGroup : frame.set3GraphicsGroup;
			const u32 dynOffset = cmd.currentPushOffset;
			if (cmd.inRenderPass && cmd.renderPass) {
				if (cmd.boundRenderSets[TABLE_SET] != group || cmd.boundRenderSet3Offset != dynOffset) {
					d.wgpu.wgpuRenderPassEncoderSetBindGroup(cmd.renderPass, TABLE_SET, group, 1, &dynOffset);
					cmd.boundRenderSets[TABLE_SET] = group;
					cmd.boundRenderSet3Offset = dynOffset;
				}
				cmd.renderSet3Dirty = false;
			} else if (cmd.computePass) {
				if (cmd.boundComputeActiveSets[TABLE_SET] != group || cmd.boundComputeSet3Offset != dynOffset) {
					d.wgpu.wgpuComputePassEncoderSetBindGroup(cmd.computePass, TABLE_SET, group, 1, &dynOffset);
					cmd.boundComputeActiveSets[TABLE_SET] = group;
					cmd.boundComputeSet3Offset = dynOffset;
				}
			}
			cmd.set3Bound = true;
			cmd.set3BoundReadWrite = rw;
		}

		void EnsureGraphicsVariantBound(DeviceImpl& d, CommandListImpl& cmd) {
			if (d.lost || !cmd.inRenderPass || !cmd.renderPass || cmd.boundPipelineIndex == 0 || cmd.pipelineRefused) {
				return;
			}
			PipelineResource& pipeline = d.pipelines.slots[cmd.boundPipelineIndex].resource;
			if (pipeline.isCompute) {
				return;
			}
			if (cmd.graphicsVariantDirty) {
				const i32 bias = pipeline.dynamicDepthBias ? cmd.depthBias : static_cast<i32>(pipeline.depthBiasConstant);
				const f32 slope = pipeline.dynamicDepthBias ? cmd.depthBiasSlope : pipeline.depthBiasSlope;
				WGPURenderPipeline variant = d.GetOrCreateGraphicsVariant(pipeline, cmd.frontFace, bias, slope);
				if (variant && variant != cmd.boundRenderPipeline) {
					d.wgpu.wgpuRenderPassEncoderSetPipeline(cmd.renderPass, variant);
					cmd.boundRenderPipeline = variant;
				}
				cmd.graphicsVariantDirty = false;
			}
			if (cmd.renderSet3Dirty) {
				BindSet3ForPass(d, cmd, pipeline);
			}
		}

		u32 FormatRowBytesTight(Format format, u32 width) {
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

	} // anonymous namespace

	void DeviceImpl::CloseActiveComputePass(CommandListImpl& cmd) {
		if (cmd.computePass) {
			wgpu.wgpuComputePassEncoderEnd(cmd.computePass);
			wgpu.wgpuComputePassEncoderRelease(cmd.computePass);
			cmd.computePass = nullptr;
			cmd.ResetComputePassCache();
		}
	}

	void DeviceImpl::EnsureActiveComputePass(CommandListImpl& cmd) {
		if (!cmd.computePass && cmd.encoder && !lost) {
			WGPUComputePassDescriptor desc{};
			cmd.computePass = wgpu.wgpuCommandEncoderBeginComputePass(cmd.encoder, &desc);
			cmd.ResetComputePassCache();
		}
	}

	void CommandList::TransitionTexture(TextureHandle texture, ResourceState newState) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(!_impl->inRenderPass, "barriers are illegal inside a render pass");
		TextureResource* resource = d.textures.Resolve(texture);
		if (!resource) {
			return;
		}
		resource->state = newState;
	}

	void CommandList::TransitionBuffer(BufferHandle buffer, ResourceState newState) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(!_impl->inRenderPass, "barriers are illegal inside a render pass");
		BufferResource* resource = d.buffers.Resolve(buffer);
		if (!resource) {
			return;
		}
		if (resource->state == ResourceState::UnorderedAccess && newState != ResourceState::UnorderedAccess) {
			d.CloseActiveComputePass(*_impl);
		}
		resource->state = newState;
	}

	void CommandList::UnorderedAccessBarrier(BufferHandle buffer) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(!_impl->inRenderPass, "barriers are illegal inside a render pass");
		BufferResource* resource = d.buffers.Resolve(buffer);
		if (!resource) {
			return;
		}
		SLT_CORE_ASSERT_MSG(resource->state == ResourceState::UnorderedAccess,
							"an unordered access barrier on a buffer in another state. Transition it");
		// Ending the compute pass enforces a full storage-buffer visibility barrier across all WebGPU implementations
		d.CloseActiveComputePass(*_impl);
	}

	void CommandList::BeginRenderPass(const RenderPassDesc& desc) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(!_impl->inRenderPass, "BeginRenderPass while a pass is open");
		d.CloseActiveComputePass(*_impl);

		WGPURenderPassColorAttachment colorInfos[MAX_COLOR_ATTACHMENTS]{};
		u32 colorCount = 0;
		Vec2I area{};

		_impl->passColorCount = 0;
		_impl->passDepthFormat = Format::Unknown;
		_impl->passSamples = 1;

		for (u32 i = 0; i < desc.colorAttachments.size(); ++i) {
			const RenderPassDesc::Attachment& attachment = desc.colorAttachments[i];
			TextureResource* resource = d.textures.Resolve(attachment.texture);
			if (!resource) {
				SLT_LOG_ERROR("Graphics", "BeginRenderPass: stale color attachment");
				return;
			}
			SLT_CORE_ASSERT_MSG(resource->state == ResourceState::RenderTarget, "color attachment must be in RenderTarget state");
			_impl->passColorFormats[_impl->passColorCount++] = resource->format;
			_impl->passSamples = resource->samples;
			WGPURenderPassColorAttachment& info = colorInfos[colorCount++];
			info.view = resource->AttachmentView(attachment.layer, attachment.mip);
			if (i < desc.resolveTargets.size() && desc.resolveTargets[i].IsValid()) {
				TextureResource* resolveRes = d.textures.Resolve(desc.resolveTargets[i]);
				if (!resolveRes) {
					SLT_LOG_ERROR("Graphics", "BeginRenderPass: stale resolve target");
					return;
				}
				SLT_CORE_ASSERT_MSG(resolveRes->state == ResourceState::RenderTarget, "resolve target must be in RenderTarget state");
				SLT_CORE_ASSERT_MSG(resource->samples > 1 && resolveRes->samples == 1 &&
										resolveRes->format == resource->format && resolveRes->size == resource->size,
									"resolve target must be a matching single-sample texture");
				info.resolveTarget = resolveRes->AttachmentView(0, 0);
			}
			info.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
			info.loadOp = attachment.load == LoadOp::Load ? WGPULoadOp_Load : WGPULoadOp_Clear;
			info.storeOp = attachment.store == StoreOp::Store ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
			info.clearValue = {attachment.clearColor.r, attachment.clearColor.g, attachment.clearColor.b, attachment.clearColor.a};
			area = Vec2I{std::max(resource->size.x >> attachment.mip, 1), std::max(resource->size.y >> attachment.mip, 1)};
		}

		WGPURenderPassDepthStencilAttachment depthInfo{};
		bool hasDepth = false;
		if (desc.depth) {
			TextureResource* resource = d.textures.Resolve(desc.depth->texture);
			if (!resource) {
				SLT_LOG_ERROR("Graphics", "BeginRenderPass: stale depth attachment");
				return;
			}
			SLT_CORE_ASSERT_MSG(resource->state == ResourceState::DepthWrite, "depth attachment must be in DepthWrite state");
			depthInfo.view = resource->AttachmentView(desc.depth->layer);
			depthInfo.depthLoadOp = desc.depth->load == LoadOp::Load ? WGPULoadOp_Load : WGPULoadOp_Clear;
			depthInfo.depthStoreOp = desc.depth->store == StoreOp::Store ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
			depthInfo.depthClearValue = desc.depth->clearDepth;
			depthInfo.depthReadOnly = 0;
			depthInfo.stencilLoadOp = WGPULoadOp_Undefined;
			depthInfo.stencilStoreOp = WGPUStoreOp_Undefined;
			depthInfo.stencilReadOnly = 1;
			_impl->passDepthFormat = resource->format;
			hasDepth = true;
			if (colorCount == 0) {
				_impl->passSamples = resource->samples;
				area = resource->size;
			}
		}

		WGPURenderPassDescriptor passDesc{};
		passDesc.colorAttachmentCount = colorCount;
		passDesc.colorAttachments = colorInfos;
		passDesc.depthStencilAttachment = hasDepth ? &depthInfo : nullptr;

		if (!d.lost && _impl->encoder) {
			_impl->renderPass = d.wgpu.wgpuCommandEncoderBeginRenderPass(_impl->encoder, &passDesc);
		}
		_impl->inRenderPass = true;
		_impl->boundPipelineIndex = 0;
		_impl->ResetRenderPassCache();
		_impl->passArea = RectI{0, 0, area.x, area.y};

		SetViewportScissor(_impl->passArea);
	}

	void CommandList::EndRenderPass() {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(_impl->inRenderPass, "EndRenderPass without a pass open");
		if (_impl->renderPass) {
			d.wgpu.wgpuRenderPassEncoderEnd(_impl->renderPass);
			d.wgpu.wgpuRenderPassEncoderRelease(_impl->renderPass);
			_impl->renderPass = nullptr;
		}
		_impl->inRenderPass = false;
		_impl->ResetRenderPassCache();
	}

	void CommandList::SetPipeline(PipelineHandle pipeline) {
		DeviceImpl& d = *_impl->device;
		PipelineResource* resource = d.pipelines.Resolve(pipeline);
		if (!resource) {
			SLT_LOG_WARN("Graphics", "SetPipeline: stale handle");
			return;
		}
		if (!resource->layout) {
			if (!d.lost && !d.deadPipelineWarned) {
				d.deadPipelineWarned = true;
				SLT_LOG_WARN("Graphics", "SetPipeline: pipeline is from before device rebuild. Recreate it");
			}
			_impl->boundPipelineIndex = 0;
			_impl->boundRenderPipeline = nullptr;
			_impl->graphicsVariantDirty = false;
			_impl->renderSet3Dirty = false;
			_impl->pipelineRefused = true;
			return;
		}
		SLT_CORE_ASSERT_MSG(!resource->isCompute || !_impl->inRenderPass, "a compute pipeline bound inside a render pass");

		_impl->boundPipelineIndex = pipeline.index;
		_impl->pipelineRefused = false;
		_impl->frontFace = FrontFace::Clockwise;
		_impl->depthBias = 0;
		_impl->depthBiasSlope = 0.0f;

		if (d.lost) {
			return;
		}

		if (resource->isCompute) {
			for (WGPUBindGroup& bg : _impl->boundComputeSets) {
				bg = nullptr;
			}
			d.EnsureActiveComputePass(*_impl);
			if (_impl->computePass) {
				if (_impl->boundComputePipeline != resource->computePipeline) {
					d.wgpu.wgpuComputePassEncoderSetPipeline(_impl->computePass, resource->computePipeline);
					_impl->boundComputePipeline = resource->computePipeline;
				}
				for (u32 s = 0; s < resource->setCount && s < TABLE_SET; ++s) {
					if (!resource->setHasBindings[s] && d.table.emptyBindGroup &&
						_impl->boundComputeActiveSets[s] != d.table.emptyBindGroup) {
						d.wgpu.wgpuComputePassEncoderSetBindGroup(_impl->computePass, s, d.table.emptyBindGroup, 0, nullptr);
						_impl->boundComputeActiveSets[s] = d.table.emptyBindGroup;
					}
				}
				BindSet3ForPass(d, *_impl, *resource);
			}
		} else if (_impl->inRenderPass && _impl->renderPass) {
			_impl->graphicsVariantDirty = true;
			_impl->renderSet3Dirty = resource->bindsTable;
			for (u32 s = 0; s < resource->setCount && s < TABLE_SET; ++s) {
				if (!resource->setHasBindings[s] && d.table.emptyBindGroup &&
					_impl->boundRenderSets[s] != d.table.emptyBindGroup) {
					d.wgpu.wgpuRenderPassEncoderSetBindGroup(_impl->renderPass, s, d.table.emptyBindGroup, 0, nullptr);
					_impl->boundRenderSets[s] = d.table.emptyBindGroup;
				}
			}
		}
	}

	void CommandList::SetFrontFace(FrontFace face) {
		if (_impl->frontFace == face) {
			return;
		}
		_impl->frontFace = face;
		_impl->graphicsVariantDirty = true;
	}

	void CommandList::SetDepthBias(f32 constant, f32 slope) {
		DeviceImpl& d = *_impl->device;
		const bool dynamic = _impl->boundPipelineIndex != 0 && d.pipelines.slots[_impl->boundPipelineIndex].resource.dynamicDepthBias;
		SLT_CORE_ASSERT_MSG(dynamic, "SetDepthBias on pipeline without dynamicDepthBias");
		if (!dynamic) {
			return;
		}
		const i32 bias = static_cast<i32>(constant);
		if (_impl->depthBias == bias && _impl->depthBiasSlope == slope) {
			return;
		}
		_impl->depthBias = bias;
		_impl->depthBiasSlope = slope;
		_impl->graphicsVariantDirty = true;
	}

	void CommandList::SetViewportScissor(RectI rect) {
		DeviceImpl& d = *_impl->device;
		if (d.lost || !_impl->renderPass) {
			return;
		}
		const i32 passW = std::max(0, _impl->passArea.size.x);
		const i32 passH = std::max(0, _impl->passArea.size.y);
		const i32 x0 = std::clamp(rect.position.x, 0, passW);
		const i32 y0 = std::clamp(rect.position.y, 0, passH);
		const i32 x1 = std::clamp(rect.position.x + rect.size.x, x0, passW);
		const i32 y1 = std::clamp(rect.position.y + rect.size.y, y0, passH);
		const i32 w = std::max(0, x1 - x0);
		const i32 h = std::max(0, y1 - y0);
		if (w > 0 && h > 0 &&
			(!_impl->viewportBound || _impl->boundViewportX != x0 || _impl->boundViewportY != y0 ||
			 _impl->boundViewportW != w || _impl->boundViewportH != h)) {
			d.wgpu.wgpuRenderPassEncoderSetViewport(_impl->renderPass, static_cast<f32>(x0), static_cast<f32>(y0),
													static_cast<f32>(w), static_cast<f32>(h), 0.0f, 1.0f);
			_impl->viewportBound = true;
			_impl->boundViewportX = x0;
			_impl->boundViewportY = y0;
			_impl->boundViewportW = w;
			_impl->boundViewportH = h;
		}
		SetScissor(rect);
	}

	void CommandList::SetScissor(RectI rect) {
		DeviceImpl& d = *_impl->device;
		if (d.lost || !_impl->renderPass) {
			return;
		}
		const i32 passW = std::max(0, _impl->passArea.size.x);
		const i32 passH = std::max(0, _impl->passArea.size.y);
		const i32 x0 = std::clamp(rect.position.x, 0, passW);
		const i32 y0 = std::clamp(rect.position.y, 0, passH);
		const i32 x1 = std::clamp(rect.position.x + rect.size.x, x0, passW);
		const i32 y1 = std::clamp(rect.position.y + rect.size.y, y0, passH);
		const u32 sx = static_cast<u32>(x0);
		const u32 sy = static_cast<u32>(y0);
		const u32 sw = static_cast<u32>(x1 - x0);
		const u32 sh = static_cast<u32>(y1 - y0);
		if (!_impl->scissorBound || _impl->boundScissorX != sx || _impl->boundScissorY != sy ||
			_impl->boundScissorW != sw || _impl->boundScissorH != sh) {
			d.wgpu.wgpuRenderPassEncoderSetScissorRect(_impl->renderPass, sx, sy, sw, sh);
			_impl->scissorBound = true;
			_impl->boundScissorX = sx;
			_impl->boundScissorY = sy;
			_impl->boundScissorW = sw;
			_impl->boundScissorH = sh;
		}
	}

	void CommandList::BindTransientSet(u32 setIndex, TransientSet set) {
		DeviceImpl& d = *_impl->device;
		if (d.lost || _impl->pipelineRefused || !set.IsValid() || (set.opaque & (1ull << 63)) != 0 || _impl->boundPipelineIndex == 0) {
			return;
		}
		PipelineResource& pipeline = d.pipelines.slots[_impl->boundPipelineIndex].resource;
		WGPUBindGroup group = reinterpret_cast<WGPUBindGroup>(static_cast<uintptr_t>(set.opaque));
		if (pipeline.isCompute) {
			if (setIndex < MAX_DESCRIPTOR_SETS) {
				_impl->boundComputeSets[setIndex] = group;
			}
			d.EnsureActiveComputePass(*_impl);
			if (_impl->computePass) {
				if (setIndex >= MAX_DESCRIPTOR_SETS || _impl->boundComputeActiveSets[setIndex] != group) {
					d.wgpu.wgpuComputePassEncoderSetBindGroup(_impl->computePass, setIndex, group, 0, nullptr);
					if (setIndex < MAX_DESCRIPTOR_SETS) {
						_impl->boundComputeActiveSets[setIndex] = group;
					}
				}
			}
		} else if (_impl->renderPass) {
			if (setIndex < MAX_DESCRIPTOR_SETS && _impl->boundRenderSets[setIndex] == group) {
				return;
			}
			if (setIndex < MAX_DESCRIPTOR_SETS) {
				_impl->boundRenderSets[setIndex] = group;
			}
			d.wgpu.wgpuRenderPassEncoderSetBindGroup(_impl->renderPass, setIndex, group, 0, nullptr);
		}
	}

	void CommandList::PushConstants(std::span<const u8> data) {
		DeviceImpl& d = *_impl->device;
		if (d.lost || _impl->pipelineRefused || _impl->boundPipelineIndex == 0 || data.empty()) {
			return;
		}
		PipelineResource& pipeline = d.pipelines.slots[_impl->boundPipelineIndex].resource;
		SLT_CORE_ASSERT_MSG(data.size() <= WEBGPU_PUSH_STRIDE, "push constant data exceeds 256 B slot");

		FrameContext& frame = d.frames[d.frameIndex];
		if (_impl->lastPushSize == static_cast<u32>(data.size()) &&
			frame.pushUsed >= _impl->currentPushOffset + data.size() &&
			std::memcmp(frame.pushCPU.data() + _impl->currentPushOffset, data.data(), data.size()) == 0) {
			if (pipeline.bindsTable && !_impl->inRenderPass) {
				BindSet3ForPass(d, *_impl, pipeline);
			}
			return;
		}
		if (frame.pushUsed + WEBGPU_PUSH_STRIDE > frame.pushCapacity) {
			SLT_LOG_ERROR("Graphics", "WebGPU: push constant ring exhausted ({} B)", frame.pushCapacity);
			return;
		}
		const u32 offset = static_cast<u32>(frame.pushUsed);
		frame.pushUsed += WEBGPU_PUSH_STRIDE;
		std::memcpy(frame.pushCPU.data() + offset, data.data(), data.size());
		_impl->currentPushOffset = offset;
		_impl->lastPushSize = static_cast<u32>(data.size());

		if (pipeline.bindsTable) {
			if (_impl->inRenderPass) {
				_impl->renderSet3Dirty = true;
			} else {
				BindSet3ForPass(d, *_impl, pipeline);
			}
		}
	}

	void CommandList::BindIndexBuffer(BufferHandle buffer, bool sixteenBit) {
		DeviceImpl& d = *_impl->device;
		BufferResource* resource = d.buffers.Resolve(buffer);
		if (!resource || d.lost || !_impl->renderPass) {
			return;
		}
		WGPUBuffer gpuBuf = resource->inVertexHeap ? d.vertexHeap.buffer : resource->buffer;
		const WGPUIndexFormat format = sixteenBit ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32;
		const u64 offset = resource->inVertexHeap ? resource->heapOffset : 0;
		const u64 size = resource->gpuSize;
		if (_impl->boundIndexBuffer == gpuBuf && _impl->boundIndexFormat == format &&
			_impl->boundIndexOffset == offset && _impl->boundIndexSize == size) {
			return;
		}
		d.wgpu.wgpuRenderPassEncoderSetIndexBuffer(_impl->renderPass, gpuBuf, format, offset, size);
		_impl->boundIndexBuffer = gpuBuf;
		_impl->boundIndexFormat = format;
		_impl->boundIndexOffset = offset;
		_impl->boundIndexSize = size;
	}

	void CommandList::Draw(u32 vertexCount, u32 instanceCount) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(_impl->inRenderPass, "Draw outside a render pass");
		if (d.lost || _impl->pipelineRefused || !_impl->renderPass) {
			return;
		}
		EnsureGraphicsVariantBound(d, *_impl);
		d.wgpu.wgpuRenderPassEncoderDraw(_impl->renderPass, vertexCount, instanceCount, 0, 0);
	}

	void CommandList::DrawIndexed(u32 indexCount, u32 firstIndex, u32 instanceCount) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(_impl->inRenderPass, "DrawIndexed outside a render pass");
		if (d.lost || _impl->pipelineRefused || !_impl->renderPass) {
			return;
		}
		EnsureGraphicsVariantBound(d, *_impl);
		d.wgpu.wgpuRenderPassEncoderDrawIndexed(_impl->renderPass, indexCount, instanceCount, firstIndex, 0, 0);
	}

	void CommandList::DrawIndirect(BufferHandle arguments, u64 offset) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(_impl->inRenderPass, "DrawIndirect outside a render pass");
		if (d.lost || _impl->pipelineRefused || !_impl->renderPass) {
			return;
		}
		BufferResource* resource = d.buffers.Resolve(arguments);
		if (!resource) {
			return;
		}
		EnsureGraphicsVariantBound(d, *_impl);
		WGPUBuffer gpuBuf = resource->inVertexHeap ? d.vertexHeap.buffer : resource->buffer;
		const u64 base = resource->inVertexHeap ? resource->heapOffset : 0;
		d.wgpu.wgpuRenderPassEncoderDrawIndirect(_impl->renderPass, gpuBuf, base + offset);
	}

	void CommandList::Dispatch(u32 groupsX, u32 groupsY, u32 groupsZ) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(!_impl->inRenderPass, "a dispatch inside a render pass");
		if (d.lost || _impl->pipelineRefused || _impl->boundPipelineIndex == 0) {
			return;
		}
		PipelineResource& pipeline = d.pipelines.slots[_impl->boundPipelineIndex].resource;
		if (!pipeline.isCompute) {
			return;
		}
		if (!_impl->computePass) {
			d.EnsureActiveComputePass(*_impl);
			d.wgpu.wgpuComputePassEncoderSetPipeline(_impl->computePass, pipeline.computePipeline);
			_impl->boundComputePipeline = pipeline.computePipeline;
			for (u32 s = 0; s < pipeline.setCount && s < TABLE_SET; ++s) {
				WGPUBindGroup bg = pipeline.setHasBindings[s] ? _impl->boundComputeSets[s] : d.table.emptyBindGroup;
				if (bg) {
					d.wgpu.wgpuComputePassEncoderSetBindGroup(_impl->computePass, s, bg, 0, nullptr);
					_impl->boundComputeActiveSets[s] = bg;
				}
			}
			BindSet3ForPass(d, *_impl, pipeline);
		}
		d.wgpu.wgpuComputePassEncoderDispatchWorkgroups(_impl->computePass, groupsX, groupsY, groupsZ);
	}

	void CommandList::DispatchIndirect(BufferHandle arguments, u64 offset) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(!_impl->inRenderPass, "a dispatch inside a render pass");
		if (d.lost || _impl->pipelineRefused || _impl->boundPipelineIndex == 0) {
			return;
		}
		BufferResource* resource = d.buffers.Resolve(arguments);
		PipelineResource& pipeline = d.pipelines.slots[_impl->boundPipelineIndex].resource;
		if (!resource || !pipeline.isCompute) {
			return;
		}
		if (!_impl->computePass) {
			d.EnsureActiveComputePass(*_impl);
			d.wgpu.wgpuComputePassEncoderSetPipeline(_impl->computePass, pipeline.computePipeline);
			_impl->boundComputePipeline = pipeline.computePipeline;
			for (u32 s = 0; s < pipeline.setCount && s < TABLE_SET; ++s) {
				WGPUBindGroup bg = pipeline.setHasBindings[s] ? _impl->boundComputeSets[s] : d.table.emptyBindGroup;
				if (bg) {
					d.wgpu.wgpuComputePassEncoderSetBindGroup(_impl->computePass, s, bg, 0, nullptr);
					_impl->boundComputeActiveSets[s] = bg;
				}
			}
			BindSet3ForPass(d, *_impl, pipeline);
		}
		WGPUBuffer gpuBuf = resource->inVertexHeap ? d.vertexHeap.buffer : resource->buffer;
		const u64 base = resource->inVertexHeap ? resource->heapOffset : 0;
		d.wgpu.wgpuComputePassEncoderDispatchWorkgroupsIndirect(_impl->computePass, gpuBuf, base + offset);
	}

	void CommandList::CopyBufferRegion(BufferHandle src, BufferHandle dst, u64 srcOffset, u64 dstOffset, u64 size) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(!_impl->inRenderPass, "copies are illegal inside a render pass");
		d.CloseActiveComputePass(*_impl);
		BufferResource* srcRes = d.buffers.Resolve(src);
		BufferResource* dstRes = d.buffers.Resolve(dst);
		if (!srcRes || !dstRes || d.lost || !_impl->encoder || size == 0) {
			return;
		}

		WGPUBuffer srcBuf = srcRes->inVertexHeap ? d.vertexHeap.buffer : srcRes->buffer;
		const u64 srcBase = srcRes->inVertexHeap ? srcRes->heapOffset : 0;
		const u64 alignedSize = AlignUp(size, 4);

		if (dstRes->access == MemoryAccess::Readback && dstRes->cpuShadow) {
			WGPUBufferDescriptor stDesc{};
			stDesc.size = alignedSize;
			stDesc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
			WGPUBuffer staging = d.wgpu.wgpuDeviceCreateBuffer(d.device, &stDesc);
			if (!staging) {
				return;
			}
			d.wgpu.wgpuCommandEncoderCopyBufferToBuffer(_impl->encoder, srcBuf, srcBase + srcOffset, staging, 0, alignedSize);
			auto pending = std::make_shared<PendingReadback>();
			pending->staging = staging;
			pending->dstShadow = dstRes->cpuShadow;
			pending->dstOffset = dstOffset;
			pending->tightRowBytes = static_cast<u32>(size);
			pending->paddedRowBytes = static_cast<u32>(alignedSize);
			pending->rowCount = 1;
			d.frames[d.frameIndex].pendingReadbacks.push_back(std::move(pending));
			return;
		}

		WGPUBuffer dstBuf = dstRes->inVertexHeap ? d.vertexHeap.buffer : dstRes->buffer;
		const u64 dstBase = dstRes->inVertexHeap ? dstRes->heapOffset : 0;
		d.wgpu.wgpuCommandEncoderCopyBufferToBuffer(_impl->encoder, srcBuf, srcBase + srcOffset, dstBuf, dstBase + dstOffset, alignedSize);
	}

	void CommandList::CopyBufferToTexture(BufferHandle src, TextureHandle dst, u32 mipLevel) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(!_impl->inRenderPass, "copies are illegal inside a render pass");
		d.CloseActiveComputePass(*_impl);
		BufferResource* srcRes = d.buffers.Resolve(src);
		TextureResource* dstRes = d.textures.Resolve(dst);
		if (!srcRes || !dstRes || d.lost || !_impl->encoder || !dstRes->texture) {
			return;
		}
		const u32 mipW = static_cast<u32>(std::max(1, dstRes->size.x >> mipLevel));
		const u32 mipH = static_cast<u32>(std::max(1, dstRes->size.y >> mipLevel));
		const u32 rowBytes = FormatRowBytesTight(dstRes->gpuFormat, mipW);

		WGPUBuffer srcBuf = srcRes->inVertexHeap ? d.vertexHeap.buffer : srcRes->buffer;
		const u64 srcBase = srcRes->inVertexHeap ? srcRes->heapOffset : 0;

		WGPUTexelCopyBufferInfo srcInfo{};
		srcInfo.buffer = srcBuf;
		srcInfo.layout.offset = srcBase;
		srcInfo.layout.bytesPerRow = static_cast<u32>(AlignUp(rowBytes, 256u));
		srcInfo.layout.rowsPerImage = mipH;

		WGPUTexelCopyTextureInfo dstInfo{};
		dstInfo.texture = dstRes->texture;
		dstInfo.mipLevel = mipLevel;
		dstInfo.aspect = IsDepthFormat(dstRes->gpuFormat) ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All;

		const u32 copyW = IsBlockCompressed(dstRes->gpuFormat) ? static_cast<u32>(AlignUp(mipW, 4)) : mipW;
		const u32 copyH = IsBlockCompressed(dstRes->gpuFormat) ? static_cast<u32>(AlignUp(mipH, 4)) : mipH;
		WGPUExtent3D extent{copyW, copyH, 1};
		d.wgpu.wgpuCommandEncoderCopyBufferToTexture(_impl->encoder, &srcInfo, &dstInfo, &extent);
	}

	void CommandList::CopyTextureToBuffer(TextureHandle src, BufferHandle dst, u64 dstOffset, u32 mipLevel, u32 layer) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(!_impl->inRenderPass, "copies are illegal inside a render pass");
		d.CloseActiveComputePass(*_impl);
		TextureResource* srcRes = d.textures.Resolve(src);
		BufferResource* dstRes = d.buffers.Resolve(dst);
		if (!srcRes || !dstRes || d.lost || !_impl->encoder || !srcRes->texture) {
			return;
		}
		if (mipLevel >= srcRes->mipLevels || layer >= srcRes->arrayLayers) {
			return;
		}
		const u32 mipW = static_cast<u32>(std::max(1, srcRes->size.x >> mipLevel));
		const u32 mipH = static_cast<u32>(std::max(1, srcRes->size.y >> mipLevel));
		const u32 tightRowBytes = FormatRowBytesTight(srcRes->gpuFormat, mipW);
		const u32 paddedRowBytes = static_cast<u32>(AlignUp(tightRowBytes, 256u));
		const u64 paddedSize = static_cast<u64>(paddedRowBytes) * mipH;

		WGPUTexelCopyTextureInfo srcInfo{};
		srcInfo.texture = srcRes->texture;
		srcInfo.mipLevel = mipLevel;
		srcInfo.origin = {0, 0, srcRes->dimension == TextureDimension::Texture3D ? 0u : layer};
		srcInfo.aspect = IsDepthFormat(srcRes->gpuFormat) ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All;
		const u32 copyW = IsBlockCompressed(srcRes->gpuFormat) ? static_cast<u32>(AlignUp(mipW, 4)) : mipW;
		const u32 copyH = IsBlockCompressed(srcRes->gpuFormat) ? static_cast<u32>(AlignUp(mipH, 4)) : mipH;
		WGPUExtent3D extent{copyW, copyH, 1};

		if (dstRes->access == MemoryAccess::Readback && dstRes->cpuShadow) {
			WGPUBufferDescriptor stDesc{};
			stDesc.size = paddedSize;
			stDesc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
			WGPUBuffer staging = d.wgpu.wgpuDeviceCreateBuffer(d.device, &stDesc);
			if (!staging) {
				return;
			}
			WGPUTexelCopyBufferInfo dstInfo{};
			dstInfo.buffer = staging;
			dstInfo.layout.offset = 0;
			dstInfo.layout.bytesPerRow = paddedRowBytes;
			dstInfo.layout.rowsPerImage = mipH;
			d.wgpu.wgpuCommandEncoderCopyTextureToBuffer(_impl->encoder, &srcInfo, &dstInfo, &extent);

			auto pending = std::make_shared<PendingReadback>();
			pending->staging = staging;
			pending->dstShadow = dstRes->cpuShadow;
			pending->dstOffset = dstOffset;
			pending->tightRowBytes = tightRowBytes;
			pending->paddedRowBytes = paddedRowBytes;
			pending->rowCount = mipH;
			d.frames[d.frameIndex].pendingReadbacks.push_back(std::move(pending));
			return;
		}

		WGPUBuffer dstBuf = dstRes->inVertexHeap ? d.vertexHeap.buffer : dstRes->buffer;
		const u64 dstBase = dstRes->inVertexHeap ? dstRes->heapOffset : 0;
		WGPUTexelCopyBufferInfo dstInfo{};
		dstInfo.buffer = dstBuf;
		dstInfo.layout.offset = dstBase + dstOffset;
		dstInfo.layout.bytesPerRow = paddedRowBytes;
		dstInfo.layout.rowsPerImage = mipH;
		d.wgpu.wgpuCommandEncoderCopyTextureToBuffer(_impl->encoder, &srcInfo, &dstInfo, &extent);
	}

	void CommandList::GenerateMips(TextureHandle target) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(!_impl->inRenderPass, "a mip chain is illegal inside a render pass");
		d.CloseActiveComputePass(*_impl);
		TextureResource* resource = d.textures.Resolve(target);
		if (!resource || !_impl->encoder) {
			return;
		}
		d.CmdGenerateMips(_impl->encoder, *resource);
	}

	void CommandList::ResolveTexture(TextureHandle src, TextureHandle dst) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(!_impl->inRenderPass, "a resolve is illegal inside a render pass");
		d.CloseActiveComputePass(*_impl);
		TextureResource* srcRes = d.textures.Resolve(src);
		TextureResource* dstRes = d.textures.Resolve(dst);
		if (!srcRes || !dstRes || d.lost || !_impl->encoder) {
			return;
		}
		if (srcRes->samples <= 1 || dstRes->samples != 1 || srcRes->format != dstRes->format || srcRes->size != dstRes->size) {
			return;
		}

		WGPURenderPassColorAttachment color{};
		color.view = srcRes->view;
		color.resolveTarget = dstRes->view;
		color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
		color.loadOp = WGPULoadOp_Load;
		color.storeOp = WGPUStoreOp_Store;

		WGPURenderPassDescriptor passDesc{};
		passDesc.colorAttachmentCount = 1;
		passDesc.colorAttachments = &color;
		WGPURenderPassEncoder pass = d.wgpu.wgpuCommandEncoderBeginRenderPass(_impl->encoder, &passDesc);
		d.wgpu.wgpuRenderPassEncoderEnd(pass);
		d.wgpu.wgpuRenderPassEncoderRelease(pass);
	}

	void CommandList::Breadcrumb(const char* name) {
		DeviceImpl& d = *_impl->device;
		if (d.lost || !_impl->encoder || !name || _impl->inRenderPass || _impl->computePass) {
			return;
		}
		d.wgpu.wgpuCommandEncoderInsertDebugMarker(_impl->encoder, ToWGPUString(name));
	}

} // namespace Shalltear::RHI
