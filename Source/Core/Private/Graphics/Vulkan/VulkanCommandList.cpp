#include "Graphics/Vulkan/VulkanInternal.h"

#include <algorithm>

namespace Shalltear::RHI {

	void CommandList::TransitionTexture(TextureHandle texture, ResourceState newState) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(!_impl->inRenderPass, "barriers are illegal inside a render pass");
		TextureResource* resource = d.textures.Resolve(texture);
		if (!resource) {
			SLT_LOG_WARN("Graphics", "TransitionTexture: stale handle");
			return;
		}
		if (resource->state == newState) {
			return;
		}
		CmdTransitionImage(d, _impl->commandBuffer, resource->image, resource->state, newState, IsDepthFormat(resource->format),
						   resource->awaitsAcquire);
		resource->state = newState;
		resource->awaitsAcquire = false;
	}

	void CommandList::TransitionBuffer(BufferHandle buffer, ResourceState newState) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(!_impl->inRenderPass, "barriers are illegal inside a render pass");
		BufferResource* resource = d.buffers.Resolve(buffer);
		if (!resource) {
			SLT_LOG_WARN("Graphics", "TransitionBuffer: stale handle");
			return;
		}
		if (resource->state == newState) {
			return;
		}
		if (d.lost) {
			resource->state = newState;
			return;
		}
		StateMasks src = ToStateMasks(resource->state, false);
		StateMasks dst = ToStateMasks(newState, false);

		VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
		barrier.srcStageMask = src.stages;
		barrier.srcAccessMask = src.access;
		barrier.dstStageMask = dst.stages;
		barrier.dstAccessMask = dst.access;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.buffer = resource->buffer;
		barrier.size = VK_WHOLE_SIZE;

		VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		dependency.bufferMemoryBarrierCount = 1;
		dependency.pBufferMemoryBarriers = &barrier;
		d.fd.vkCmdPipelineBarrier2(_impl->commandBuffer, &dependency);
		resource->state = newState;
	}

	void CommandList::UnorderedAccessBarrier(BufferHandle buffer) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(!_impl->inRenderPass, "barriers are illegal inside a render pass");
		BufferResource* resource = d.buffers.Resolve(buffer);
		if (!resource) {
			SLT_LOG_WARN("Graphics", "UnorderedAccessBarrier: stale handle");
			return;
		}
		SLT_CORE_ASSERT_MSG(resource->state == ResourceState::UnorderedAccess,
							"an unordered access barrier on a buffer in another state. Transition it");
		if (d.lost) {
			return;
		}
		StateMasks masks = ToStateMasks(ResourceState::UnorderedAccess, false);
		VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
		barrier.srcStageMask = masks.stages;
		barrier.srcAccessMask = masks.access;
		barrier.dstStageMask = masks.stages;
		barrier.dstAccessMask = masks.access;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.buffer = resource->buffer;
		barrier.size = VK_WHOLE_SIZE;
		VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		dependency.bufferMemoryBarrierCount = 1;
		dependency.pBufferMemoryBarriers = &barrier;
		d.fd.vkCmdPipelineBarrier2(_impl->commandBuffer, &dependency);
	}

	void CommandList::BeginRenderPass(const RenderPassDesc& desc) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(!_impl->inRenderPass, "BeginRenderPass while a pass is open");

		VkRenderingAttachmentInfo colorInfos[MAX_COLOR_ATTACHMENTS]{};
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
			SLT_CORE_ASSERT_MSG(attachment.layer < resource->arrayLayers, "color attachment layer is out of range");
			SLT_CORE_ASSERT_MSG(attachment.mip == 0 || static_cast<size_t>(attachment.mip) * resource->arrayLayers + attachment.layer < resource->layerViews.size(),
								"color attachment mip has no view (out of range, or the texture isn't a render target)");
			_impl->passColorFormats[_impl->passColorCount++] = resource->format;
			_impl->passSamples = resource->samples;
			VkRenderingAttachmentInfo& info = colorInfos[colorCount++];
			info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
			info.imageView = resource->AttachmentView(attachment.layer, attachment.mip);
			info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
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
				info.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
				info.resolveImageView = resolveRes->AttachmentView(0, 0);
				info.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			}
			info.loadOp = attachment.load == LoadOp::Load	 ? VK_ATTACHMENT_LOAD_OP_LOAD
						  : attachment.load == LoadOp::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
															 : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			info.storeOp = attachment.store == StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
			info.clearValue.color = {{attachment.clearColor.r, attachment.clearColor.g, attachment.clearColor.b, attachment.clearColor.a}};
			area = Vec2I{std::max(resource->size.x >> attachment.mip, 1), std::max(resource->size.y >> attachment.mip, 1)};
		}

		VkRenderingAttachmentInfo depthInfo{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		bool hasDepth = false;
		if (desc.depth) {
			TextureResource* resource = d.textures.Resolve(desc.depth->texture);
			if (!resource) {
				SLT_LOG_ERROR("Graphics", "BeginRenderPass: stale depth attachment");
				return;
			}
			SLT_CORE_ASSERT_MSG(resource->state == ResourceState::DepthWrite, "depth attachment must be in DepthWrite state");
			SLT_CORE_ASSERT_MSG(desc.depth->layer < resource->arrayLayers, "depth attachment layer is out of range");
			depthInfo.imageView = resource->AttachmentView(desc.depth->layer);
			depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
			depthInfo.loadOp = desc.depth->load == LoadOp::Load	   ? VK_ATTACHMENT_LOAD_OP_LOAD
							   : desc.depth->load == LoadOp::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
																   : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			depthInfo.storeOp = desc.depth->store == StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
			depthInfo.clearValue.depthStencil = {desc.depth->clearDepth, 0};
			_impl->passDepthFormat = resource->format;
			hasDepth = true;
			if (colorCount == 0) {
				_impl->passSamples = resource->samples;
				area = resource->size;
			}
		}

		VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
		rendering.renderArea = {{0, 0}, {static_cast<u32>(area.x), static_cast<u32>(area.y)}};
		rendering.layerCount = 1;
		rendering.colorAttachmentCount = colorCount;
		rendering.pColorAttachments = colorInfos;
		rendering.pDepthAttachment = hasDepth ? &depthInfo : nullptr;
		if (!d.lost) {
			d.fd.vkCmdBeginRendering(_impl->commandBuffer, &rendering);
		}

		_impl->inRenderPass = true;
		_impl->passArea = RectI{0, 0, area.x, area.y};

		SetViewportScissor(_impl->passArea);
	}

	void CommandList::EndRenderPass() {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(_impl->inRenderPass, "EndRenderPass without a pass open");
		if (!d.lost) {
			d.fd.vkCmdEndRendering(_impl->commandBuffer);
		}
		_impl->inRenderPass = false;
	}

	void CommandList::SetPipeline(PipelineHandle pipeline) {
		DeviceImpl& d = *_impl->device;
		PipelineResource* resource = d.pipelines.Resolve(pipeline);
		if (!resource) {
			SLT_LOG_WARN("Graphics", "SetPipeline: stale handle");
			return;
		}
		if (!resource->pipeline) {
			if (!d.lost && !d.deadPipelineWarned) {
				d.deadPipelineWarned = true;
				SLT_LOG_WARN("Graphics", "SetPipeline: pipeline is from before device rebuild. Recreate it");
			}
			_impl->boundPipelineIndex = 0;
			_impl->pipelineRefused = true;
			return;
		}
		const bool compute = resource->bindPoint == VK_PIPELINE_BIND_POINT_COMPUTE;
		SLT_CORE_ASSERT_MSG(!compute || !_impl->inRenderPass, "a compute pipeline bound inside a render pass");
#if defined(SLT_ENABLE_ASSERTS)
		if (_impl->inRenderPass && !compute) {
			SLT_CORE_ASSERT_MSG(resource->colorFormatCount == _impl->passColorCount, "pipeline color attachment count differs from the pass");
			for (u32 i = 0; i < resource->colorFormatCount; ++i) {
				SLT_CORE_ASSERT_MSG(resource->colorFormats[i] == _impl->passColorFormats[i], "pipeline color format differs from the pass attachment");
			}
			SLT_CORE_ASSERT_MSG(resource->depthFormat == _impl->passDepthFormat, "pipeline depth format differs from the pass attachment");
			SLT_CORE_ASSERT_MSG(resource->samples == _impl->passSamples, "pipeline sample count differs from the pass attachments");
		}
#endif
		if (!d.lost) {
			d.fd.vkCmdBindPipeline(_impl->commandBuffer, resource->bindPoint, resource->pipeline);
			if (resource->bindsTable) {
				d.fd.vkCmdBindDescriptorSets(_impl->commandBuffer, resource->bindPoint, resource->layout, TABLE_SET, 1, &d.table.set, 0, nullptr);
			}
		}
		_impl->boundPipelineIndex = pipeline.index;
		_impl->pipelineRefused = false;
		if (!compute) {
			SetFrontFace(FrontFace::Clockwise);
			if (resource->dynamicDepthBias && !d.lost) {
				d.fd.vkCmdSetDepthBias(_impl->commandBuffer, 0.0f, 0.0f, 0.0f);
			}
		}
	}

	void CommandList::SetFrontFace(FrontFace face) {
		DeviceImpl& d = *_impl->device;
		if (d.lost || (_impl->frontFaceSet && _impl->frontFace == face)) {
			return;
		}
		d.fd.vkCmdSetFrontFace(_impl->commandBuffer, face == FrontFace::CounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
		_impl->frontFace = face;
		_impl->frontFaceSet = true;
	}

	void CommandList::SetDepthBias(f32 constant, f32 slope) {
		DeviceImpl& d = *_impl->device;
		const bool dynamic = _impl->boundPipelineIndex != 0 && d.pipelines.slots[_impl->boundPipelineIndex].resource.dynamicDepthBias;
		SLT_CORE_ASSERT_MSG(dynamic, "SetDepthBias on pipeline without dynamicDepthBias");
		if (d.lost || !dynamic) {
			return;
		}
		d.fd.vkCmdSetDepthBias(_impl->commandBuffer, constant, 0.0f, slope);
	}

	void CommandList::SetViewportScissor(RectI rect) {
		DeviceImpl& d = *_impl->device;
		if (d.lost) {
			return;
		}

		VkViewport viewport{};
		viewport.x = static_cast<f32>(rect.position.x);
		viewport.y = static_cast<f32>(rect.position.y + rect.size.y);
		viewport.width = static_cast<f32>(rect.size.x);
		viewport.height = -static_cast<f32>(rect.size.y);
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		d.fd.vkCmdSetViewport(_impl->commandBuffer, 0, 1, &viewport);

		SetScissor(rect);
	}

	void CommandList::SetScissor(RectI rect) {
		DeviceImpl& d = *_impl->device;
		if (d.lost) {
			return;
		}
		VkRect2D scissor{};
		scissor.offset = {rect.position.x, rect.position.y};
		scissor.extent = {static_cast<u32>(std::max(0, rect.size.x)), static_cast<u32>(std::max(0, rect.size.y))};
		d.fd.vkCmdSetScissor(_impl->commandBuffer, 0, 1, &scissor);
	}

	void CommandList::BindTransientSet(u32 setIndex, TransientSet set) {
		DeviceImpl& d = *_impl->device;
		if (d.lost || _impl->pipelineRefused) {
			return;
		}
		if (!set.IsValid() || _impl->boundPipelineIndex == 0) {
			SLT_LOG_WARN("Graphics", "BindTransientSet: invalid set or no pipeline bound");
			return;
		}
		PipelineResource& pipeline = d.pipelines.slots[_impl->boundPipelineIndex].resource;
		VkDescriptorSet vkSet = reinterpret_cast<VkDescriptorSet>(set.opaque);
		d.fd.vkCmdBindDescriptorSets(_impl->commandBuffer, pipeline.bindPoint, pipeline.layout, setIndex, 1, &vkSet, 0, nullptr);
	}

	void CommandList::PushConstants(std::span<const u8> data) {
		DeviceImpl& d = *_impl->device;
		if (d.lost || _impl->pipelineRefused) {
			return;
		}
		if (_impl->boundPipelineIndex == 0) {
			SLT_LOG_WARN("Graphics", "PushConstants: no pipeline bound");
			return;
		}
		PipelineResource& pipeline = d.pipelines.slots[_impl->boundPipelineIndex].resource;
		SLT_CORE_ASSERT_MSG(data.size() <= pipeline.pushConstantSize, "push constant data exceeds the pipeline's declared size");
		d.fd.vkCmdPushConstants(_impl->commandBuffer, pipeline.layout, pipeline.pushStages,
								0, static_cast<u32>(data.size()), data.data());
	}

	void CommandList::BindIndexBuffer(BufferHandle buffer, bool sixteenBit) {
		DeviceImpl& d = *_impl->device;
		BufferResource* resource = d.buffers.Resolve(buffer);
		if (!resource) {
			SLT_LOG_WARN("Graphics", "BindIndexBuffer: stale handle");
			return;
		}
		if (d.lost) {
			return;
		}
		d.fd.vkCmdBindIndexBuffer(_impl->commandBuffer, resource->buffer, 0, sixteenBit ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
	}

	void CommandList::Draw(u32 vertexCount, u32 instanceCount) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(_impl->inRenderPass, "Draw outside a render pass");
		if (d.lost || _impl->pipelineRefused) {
			return;
		}
		d.fd.vkCmdDraw(_impl->commandBuffer, vertexCount, instanceCount, 0, 0);
	}

	void CommandList::DrawIndexed(u32 indexCount, u32 firstIndex, u32 instanceCount) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(_impl->inRenderPass, "DrawIndexed outside a render pass");
		if (d.lost || _impl->pipelineRefused) {
			return;
		}
		d.fd.vkCmdDrawIndexed(_impl->commandBuffer, indexCount, instanceCount, firstIndex, 0, 0);
	}

	void CommandList::DrawIndirect(BufferHandle arguments, u64 offset) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(_impl->inRenderPass, "DrawIndirect outside a render pass");
		if (d.lost || _impl->pipelineRefused) {
			return;
		}
		BufferResource* resource = d.buffers.Resolve(arguments);
		if (!resource) {
			SLT_LOG_WARN("Graphics", "DrawIndirect: stale handle");
			return;
		}
		SLT_CORE_ASSERT_MSG(HasFlag(resource->usage, BufferUsage::Indirect), "indirect arguments in a buffer created without Indirect usage");
		SLT_CORE_ASSERT_MSG(resource->state == ResourceState::IndirectArgument, "indirect arguments must be in the IndirectArgument state");
		SLT_CORE_ASSERT_MSG(offset % 4 == 0 && offset + 16 <= resource->size, "indirect arguments run past the buffer or are misaligned");
		d.fd.vkCmdDrawIndirect(_impl->commandBuffer, resource->buffer, offset, 1, 16);
	}

	namespace {

		bool BoundForCompute(const DeviceImpl& d, const CommandListImpl& impl) {
			return impl.boundPipelineIndex != 0 &&
				   d.pipelines.slots[impl.boundPipelineIndex].resource.bindPoint == VK_PIPELINE_BIND_POINT_COMPUTE;
		}

	} // anonymous namespace

	void CommandList::Dispatch(u32 groupsX, u32 groupsY, u32 groupsZ) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(!_impl->inRenderPass, "a dispatch inside a render pass");
		if (d.lost || _impl->pipelineRefused) {
			return;
		}
		if (!BoundForCompute(d, *_impl)) {
			SLT_LOG_WARN("Graphics", "Dispatch: no compute pipeline bound");
			return;
		}
		d.fd.vkCmdDispatch(_impl->commandBuffer, groupsX, groupsY, groupsZ);
	}

	void CommandList::DispatchIndirect(BufferHandle arguments, u64 offset) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(!_impl->inRenderPass, "a dispatch inside a render pass");
		BufferResource* resource = d.buffers.Resolve(arguments);
		if (!resource) {
			SLT_LOG_WARN("Graphics", "DispatchIndirect: stale handle");
			return;
		}
		if (d.lost || _impl->pipelineRefused) {
			return;
		}
		if (!BoundForCompute(d, *_impl)) {
			SLT_LOG_WARN("Graphics", "DispatchIndirect: no compute pipeline bound");
			return;
		}
		SLT_CORE_ASSERT_MSG(HasFlag(resource->usage, BufferUsage::Indirect), "indirect arguments in a buffer created without Indirect usage");
		SLT_CORE_ASSERT_MSG(resource->state == ResourceState::IndirectArgument, "indirect arguments must be in the IndirectArgument state");
		SLT_CORE_ASSERT_MSG(offset % 4 == 0 && offset + 12 <= resource->size, "indirect arguments run past the buffer or are misaligned");
		d.fd.vkCmdDispatchIndirect(_impl->commandBuffer, resource->buffer, offset);
	}

	void CommandList::CopyBufferRegion(BufferHandle src, BufferHandle dst, u64 srcOffset, u64 dstOffset, u64 size) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(!_impl->inRenderPass, "copies are illegal inside a render pass");
		BufferResource* srcResource = d.buffers.Resolve(src);
		BufferResource* dstResource = d.buffers.Resolve(dst);
		if (!srcResource || !dstResource) {
			SLT_LOG_WARN("Graphics", "CopyBufferRegion: stale handle");
			return;
		}
		if (d.lost) {
			return;
		}
		VkBufferCopy2 region{VK_STRUCTURE_TYPE_BUFFER_COPY_2};
		region.srcOffset = srcOffset;
		region.dstOffset = dstOffset;
		region.size = size;
		VkCopyBufferInfo2 copy{VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2};
		copy.srcBuffer = srcResource->buffer;
		copy.dstBuffer = dstResource->buffer;
		copy.regionCount = 1;
		copy.pRegions = &region;
		d.fd.vkCmdCopyBuffer2(_impl->commandBuffer, &copy);
		if (dstResource->access == MemoryAccess::Readback) {
			VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
			barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
			barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
			barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.buffer = dstResource->buffer;
			barrier.offset = dstOffset;
			barrier.size = size;
			VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
			dependency.bufferMemoryBarrierCount = 1;
			dependency.pBufferMemoryBarriers = &barrier;
			d.fd.vkCmdPipelineBarrier2(_impl->commandBuffer, &dependency);
		}
	}

	void CommandList::CopyBufferToTexture(BufferHandle src, TextureHandle dst, u32 mipLevel) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(!_impl->inRenderPass, "copies are illegal inside a render pass");
		BufferResource* srcResource = d.buffers.Resolve(src);
		TextureResource* dstResource = d.textures.Resolve(dst);
		if (!srcResource || !dstResource) {
			SLT_LOG_WARN("Graphics", "CopyBufferToTexture: stale handle");
			return;
		}
		SLT_CORE_ASSERT_MSG(dstResource->state == ResourceState::TransferDst, "destination must be in TransferDst state");
		if (d.lost) {
			return;
		}

		bool isDepth = IsDepthFormat(dstResource->format);
		VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
		region.imageSubresource.aspectMask = isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = mipLevel;
		region.imageSubresource.layerCount = 1;
		region.imageExtent = {static_cast<u32>(std::max(1, dstResource->size.x >> mipLevel)),
							  static_cast<u32>(std::max(1, dstResource->size.y >> mipLevel)), 1};
		VkCopyBufferToImageInfo2 copy{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
		copy.srcBuffer = srcResource->buffer;
		copy.dstImage = dstResource->image;
		copy.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		copy.regionCount = 1;
		copy.pRegions = &region;
		d.fd.vkCmdCopyBufferToImage2(_impl->commandBuffer, &copy);
	}

	void CommandList::CopyTextureToBuffer(TextureHandle src, BufferHandle dst, u64 dstOffset, u32 mipLevel, u32 layer) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(!_impl->inRenderPass, "copies are illegal inside a render pass");
		TextureResource* srcResource = d.textures.Resolve(src);
		BufferResource* dstResource = d.buffers.Resolve(dst);
		if (!srcResource || !dstResource) {
			SLT_LOG_WARN("Graphics", "CopyTextureToBuffer: stale handle");
			return;
		}
		SLT_CORE_ASSERT_MSG(srcResource->state == ResourceState::TransferSrc, "source must be in TransferSrc state");
		if (d.lost) {
			return;
		}
		if (mipLevel >= srcResource->mipLevels || layer >= srcResource->arrayLayers) {
			SLT_LOG_ERROR("Graphics", "CopyTextureToBuffer: mip {} layer {} out of range ({} mip(s), {} layer(s))", mipLevel, layer,
						  srcResource->mipLevels, srcResource->arrayLayers);
			return;
		}

		VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
		region.bufferOffset = dstOffset;
		region.imageSubresource.aspectMask = IsDepthFormat(srcResource->format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = mipLevel;
		region.imageSubresource.baseArrayLayer = layer;
		region.imageSubresource.layerCount = 1;
		region.imageExtent = {static_cast<u32>(std::max(srcResource->size.x >> mipLevel, 1)),
							  static_cast<u32>(std::max(srcResource->size.y >> mipLevel, 1)), 1};
		VkCopyImageToBufferInfo2 copy{VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2};
		copy.srcImage = srcResource->image;
		copy.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		copy.dstBuffer = dstResource->buffer;
		copy.regionCount = 1;
		copy.pRegions = &region;
		d.fd.vkCmdCopyImageToBuffer2(_impl->commandBuffer, &copy);
		if (dstResource->access == MemoryAccess::Readback) {
			VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
			barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
			barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
			barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.buffer = dstResource->buffer;
			barrier.offset = dstOffset;
			barrier.size = VK_WHOLE_SIZE;
			VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
			dependency.bufferMemoryBarrierCount = 1;
			dependency.pBufferMemoryBarriers = &barrier;
			d.fd.vkCmdPipelineBarrier2(_impl->commandBuffer, &dependency);
		}
	}

	void CommandList::GenerateMips(TextureHandle target) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(!_impl->inRenderPass, "a mip chain is illegal inside a render pass");
		TextureResource* resource = d.textures.Resolve(target);
		if (!resource) {
			SLT_LOG_WARN("Graphics", "GenerateMips: stale handle");
			return;
		}
		SLT_CORE_ASSERT_MSG(!IsDepthFormat(resource->format), "GenerateMips on a depth texture");
		if (resource->mipLevels <= 1) {
			if (resource->state != ResourceState::ShaderResource) {
				CmdTransitionImage(d, _impl->commandBuffer, resource->image, resource->state, ResourceState::ShaderResource, false);
				resource->state = ResourceState::ShaderResource;
			}
			return;
		}
		CmdGenerateMips(d, _impl->commandBuffer, *resource);
	}

	void CommandList::Breadcrumb(const char* name) {
		DeviceImpl& d = *_impl->device;
		if (d.lost || d.marks.empty() || !d.fd.vkCmdSetCheckpointNV || !name) {
			return;
		}
		char* slot = d.marks.data() + static_cast<size_t>(d.nextMark) * MARK_LENGTH;
		d.nextMark = (d.nextMark + 1) % MARK_COUNT;
		size_t length = 0;
		for (; length + 1 < MARK_LENGTH && name[length] != '\0'; ++length) {
			slot[length] = name[length];
		}
		slot[length] = '\0';
		d.fd.vkCmdSetCheckpointNV(_impl->commandBuffer, slot);
	}

	void CommandList::ResolveTexture(TextureHandle src, TextureHandle dst) {
		DeviceImpl& d = *_impl->device;
		SLT_CORE_ASSERT_MSG(!_impl->inRenderPass, "a resolve is illegal inside a render pass");
		TextureResource* srcResource = d.textures.Resolve(src);
		TextureResource* dstResource = d.textures.Resolve(dst);
		if (!srcResource || !dstResource) {
			SLT_LOG_WARN("Graphics", "ResolveTexture: stale handle");
			return;
		}
		if (srcResource->samples <= 1 || dstResource->samples != 1 ||
			srcResource->format != dstResource->format || srcResource->size != dstResource->size) {
			SLT_LOG_ERROR("Graphics", "ResolveTexture: {}x -> {}x, formats {}/{} (needs a multisample source and a matching single-sample destination)",
						  srcResource->samples, dstResource->samples,
						  static_cast<u32>(srcResource->format), static_cast<u32>(dstResource->format));
			return;
		}
		SLT_CORE_ASSERT_MSG(srcResource->state == ResourceState::TransferSrc, "resolve source must be in TransferSrc state");
		SLT_CORE_ASSERT_MSG(dstResource->state == ResourceState::TransferDst, "resolve destination must be in TransferDst state");
		if (d.lost) {
			return;
		}

		VkImageResolve2 region{VK_STRUCTURE_TYPE_IMAGE_RESOLVE_2};
		region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.srcSubresource.layerCount = 1;
		region.dstSubresource = region.srcSubresource;
		region.extent = {static_cast<u32>(srcResource->size.x), static_cast<u32>(srcResource->size.y), 1};

		VkResolveImageInfo2 resolve{VK_STRUCTURE_TYPE_RESOLVE_IMAGE_INFO_2};
		resolve.srcImage = srcResource->image;
		resolve.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		resolve.dstImage = dstResource->image;
		resolve.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		resolve.regionCount = 1;
		resolve.pRegions = &region;
		d.fd.vkCmdResolveImage2(_impl->commandBuffer, &resolve);
	}

} // namespace Shalltear::RHI
