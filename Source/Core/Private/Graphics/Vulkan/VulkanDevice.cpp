#include "Graphics/Vulkan/VulkanInternal.h"

#include "Graphics/Vulkan/VulkanPlatform.h"

#include "Shalltear/Core/Memory.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <thread>

namespace Shalltear::RHI {

	namespace {
		constexpr VkPipelineStageFlags2 ACQUIRE_WAIT_STAGES = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
	} // anonymous namespace

	VkFormat ToVkFormat(Format format) {
		switch (format) {
		case Format::R8Unorm:
			return VK_FORMAT_R8_UNORM;
		case Format::RGBA8Unorm:
			return VK_FORMAT_R8G8B8A8_UNORM;
		case Format::RGBA8SRGB:
			return VK_FORMAT_R8G8B8A8_SRGB;
		case Format::BGRA8Unorm:
			return VK_FORMAT_B8G8R8A8_UNORM;
		case Format::BGRA8SRGB:
			return VK_FORMAT_B8G8R8A8_SRGB;
		case Format::RGBA16Float:
			return VK_FORMAT_R16G16B16A16_SFLOAT;
		case Format::RG16Float:
			return VK_FORMAT_R16G16_SFLOAT;
		case Format::R32Uint:
			return VK_FORMAT_R32_UINT;
		case Format::D32Float:
			return VK_FORMAT_D32_SFLOAT;
		case Format::BC5Unorm:
			return VK_FORMAT_BC5_UNORM_BLOCK;
		case Format::BC7Unorm:
			return VK_FORMAT_BC7_UNORM_BLOCK;
		case Format::BC7SRGB:
			return VK_FORMAT_BC7_SRGB_BLOCK;
		case Format::BC6HUFloat:
			return VK_FORMAT_BC6H_UFLOAT_BLOCK;
		case Format::RGBA32Float:
			return VK_FORMAT_R32G32B32A32_SFLOAT;
		default:
			return VK_FORMAT_UNDEFINED;
		}
	}

	bool IsDepthFormat(Format format) {
		return format == Format::D32Float;
	}

	StateMasks ToStateMasks(ResourceState state, bool isDepth) {
		switch (state) {
		case ResourceState::Undefined:
			return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED};
		case ResourceState::RenderTarget:
			return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
					VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
					VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		case ResourceState::DepthWrite:
			return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
					VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
					VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL};
		case ResourceState::DepthRead:
			return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
					VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
					VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
		case ResourceState::ShaderResource:
			return {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
					VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
					isDepth ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		case ResourceState::TransferSrc:
			return {VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
		case ResourceState::TransferDst:
			return {VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
		case ResourceState::Present:
			return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
		case ResourceState::UnorderedAccess:
			return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
					VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
					VK_IMAGE_LAYOUT_GENERAL};
		case ResourceState::IndirectArgument:
			return {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, VK_IMAGE_LAYOUT_GENERAL};
		default:
			return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL};
		}
	}

	void CmdTransitionImage(DeviceImpl& d, VkCommandBuffer cmd, VkImage image, ResourceState from, ResourceState to, bool isDepth,
							bool awaitsAcquire) {
		if (d.lost) {
			return;
		}
		StateMasks src = ToStateMasks(from, isDepth);
		StateMasks dst = ToStateMasks(to, isDepth);

		VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
		barrier.srcStageMask = awaitsAcquire ? src.stages | ACQUIRE_WAIT_STAGES : src.stages;
		barrier.srcAccessMask = src.access;
		barrier.dstStageMask = dst.stages;
		barrier.dstAccessMask = dst.access;
		barrier.oldLayout = src.layout;
		barrier.newLayout = dst.layout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
		barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

		VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		dependency.imageMemoryBarrierCount = 1;
		dependency.pImageMemoryBarriers = &barrier;
		d.fd.vkCmdPipelineBarrier2(cmd, &dependency);
	}

	const char* VkResultName(VkResult result) {
		switch (result) {
		case VK_SUCCESS:
			return "VK_SUCCESS";
		case VK_NOT_READY:
			return "VK_NOT_READY";
		case VK_TIMEOUT:
			return "VK_TIMEOUT";
		case VK_SUBOPTIMAL_KHR:
			return "VK_SUBOPTIMAL_KHR";
		case VK_ERROR_OUT_OF_HOST_MEMORY:
			return "VK_ERROR_OUT_OF_HOST_MEMORY";
		case VK_ERROR_OUT_OF_DEVICE_MEMORY:
			return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
		case VK_ERROR_INITIALIZATION_FAILED:
			return "VK_ERROR_INITIALIZATION_FAILED";
		case VK_ERROR_DEVICE_LOST:
			return "VK_ERROR_DEVICE_LOST";
		case VK_ERROR_LAYER_NOT_PRESENT:
			return "VK_ERROR_LAYER_NOT_PRESENT";
		case VK_ERROR_EXTENSION_NOT_PRESENT:
			return "VK_ERROR_EXTENSION_NOT_PRESENT";
		case VK_ERROR_FEATURE_NOT_PRESENT:
			return "VK_ERROR_FEATURE_NOT_PRESENT";
		case VK_ERROR_INCOMPATIBLE_DRIVER:
			return "VK_ERROR_INCOMPATIBLE_DRIVER";
		case VK_ERROR_SURFACE_LOST_KHR:
			return "VK_ERROR_SURFACE_LOST_KHR";
		case VK_ERROR_OUT_OF_DATE_KHR:
			return "VK_ERROR_OUT_OF_DATE_KHR";
		default:
			return "VK_ERROR_<unmapped>";
		}
	}

	namespace {

		struct HostAllocPrefix {
			void* original;
			size_t size;
		};

		void* HostAllocate(size_t size, size_t alignment) {
			if (size == 0) {
				return nullptr;
			}
			alignment = std::max<size_t>(alignment, alignof(HostAllocPrefix));
			size_t total = size + sizeof(HostAllocPrefix) + alignment;
			void* raw = Allocate(total, MemTag::Graphics, 16);
			uintptr_t payload = reinterpret_cast<uintptr_t>(raw) + sizeof(HostAllocPrefix);
			payload = (payload + alignment - 1) & ~(alignment - 1);
			HostAllocPrefix* prefix = reinterpret_cast<HostAllocPrefix*>(payload) - 1;
			prefix->original = raw;
			prefix->size = size;
			return reinterpret_cast<void*>(payload);
		}

		void HostFree(void* p) {
			if (!p) {
				return;
			}
			HostAllocPrefix* prefix = static_cast<HostAllocPrefix*>(p) - 1;
			Free(prefix->original);
		}

		VKAPI_ATTR void* VKAPI_CALL VmaHostAlloc(void*, size_t size, size_t alignment, VkSystemAllocationScope) {
			return HostAllocate(size, alignment);
		}

		VKAPI_ATTR void* VKAPI_CALL VmaHostRealloc(void*, void* original, size_t size, size_t alignment, VkSystemAllocationScope) {
			if (!original) {
				return HostAllocate(size, alignment);
			}
			if (size == 0) {
				HostFree(original);
				return nullptr;
			}
			HostAllocPrefix* prefix = static_cast<HostAllocPrefix*>(original) - 1;
			void* replacement = HostAllocate(size, alignment);
			if (replacement) {
				std::memcpy(replacement, original, std::min(prefix->size, size));
			}
			HostFree(original);
			return replacement;
		}

		VKAPI_ATTR void VKAPI_CALL VmaHostFree(void*, void* p) {
			HostFree(p);
		}

		const VkAllocationCallbacks* GetVmaHostCallbacks() {
			static const VkAllocationCallbacks callbacks{
				nullptr, &VmaHostAlloc, &VmaHostRealloc, &VmaHostFree, nullptr, nullptr};
			return &callbacks;
		}

		VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessengerCallback(
			VkDebugUtilsMessageSeverityFlagBitsEXT severity,
			VkDebugUtilsMessageTypeFlagsEXT,
			const VkDebugUtilsMessengerCallbackDataEXT* data,
			void*) {
			const char* message = (data && data->pMessage) ? data->pMessage : "<no message>";
			if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
				SLT_LOG_ERROR("Vulkan", "{}", message);
			} else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
				SLT_LOG_WARN("Vulkan", "{}", message);
			} else {
				SLT_LOG_DEBUG("Vulkan", "{}", message);
			}
			return VK_FALSE;
		}

		struct CandidateInfo {
			VkPhysicalDevice physical = VK_NULL_HANDLE;
			AdapterInfo info;
			u32 graphicsFamily = 0;
			u32 vendor = 0;
			u32 device = 0;
			bool usable = false;
			str rejection;
		};

		bool HasExtension(std::span<const VkExtensionProperties> available, const char* name) {
			for (const VkExtensionProperties& ext : available) {
				if (std::strcmp(ext.extensionName, name) == 0) {
					return true;
				}
			}
			return false;
		}

		CandidateInfo EvaluateAdapter(const VulkanInstanceFunctions& fi, VkInstance instance, VkPhysicalDevice physical, bool headless) {
			CandidateInfo candidate;
			candidate.physical = physical;

			VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
			fi.vkGetPhysicalDeviceProperties2(physical, &props);
			candidate.info.name = props.properties.deviceName;
			candidate.info.discrete = props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
			candidate.info.apiVersion = props.properties.apiVersion;
			candidate.vendor = props.properties.vendorID;
			candidate.device = props.properties.deviceID;

			VkPhysicalDeviceMemoryProperties2 memory{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
			fi.vkGetPhysicalDeviceMemoryProperties2(physical, &memory);
			for (u32 i = 0; i < memory.memoryProperties.memoryHeapCount; ++i) {
				if (memory.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
					candidate.info.dedicatedVideoMemory += memory.memoryProperties.memoryHeaps[i].size;
				}
			}

			if (props.properties.apiVersion < VK_API_VERSION_1_3) {
				candidate.rejection = "Vulkan 1.3 not supported";
				return candidate;
			}

			VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
			VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
			features12.pNext = &features13;
			VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
			features.pNext = &features12;
			fi.vkGetPhysicalDeviceFeatures2(physical, &features);

			if (!features13.dynamicRendering || !features13.synchronization2) {
				candidate.rejection = "dynamicRendering/synchronization2 missing";
				return candidate;
			}
			if (!features13.shaderDemoteToHelperInvocation) {
				candidate.rejection = "shaderDemoteToHelperInvocation missing";
				return candidate;
			}
			if (!features12.bufferDeviceAddress || !features12.timelineSemaphore || !features12.descriptorIndexing) {
				candidate.rejection = "bufferDeviceAddress/timelineSemaphore/descriptorIndexing missing";
				return candidate;
			}
			if (!features.features.shaderInt64) {
				candidate.rejection = "shaderInt64 missing";
				return candidate;
			}

			u32 extensionCount = 0;
			fi.vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, nullptr);
			Vector<VkExtensionProperties> extensions{GetMemoryResource(MemTag::Graphics)};
			extensions.resize(extensionCount);
			fi.vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, extensions.data());
			if (!headless && !HasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
				candidate.rejection = "VK_KHR_swapchain missing";
				return candidate;
			}

			u32 familyCount = 0;
			fi.vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
			Vector<VkQueueFamilyProperties> families{GetMemoryResource(MemTag::Graphics)};
			families.resize(familyCount);
			fi.vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());

			bool found = false;
			constexpr VkQueueFlags FRAME_QUEUE = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
			for (u32 i = 0; i < familyCount; ++i) {
				if ((families[i].queueFlags & FRAME_QUEUE) != FRAME_QUEUE) {
					continue;
				}
				if (!headless && !VulkanPlatform::QueueFamilyPresents(instance, physical, i)) {
					continue;
				}
				candidate.graphicsFamily = i;
				found = true;
				break;
			}
			if (!found) {
				candidate.rejection = headless ? "no graphics and compute queue family" : "no graphics and compute family with Win32 present support";
				return candidate;
			}

			candidate.usable = true;
			return candidate;
		}

		u64 ScoreAdapter(const CandidateInfo& candidate) {
			u64 score = candidate.info.dedicatedVideoMemory >> 20;
			if (candidate.info.discrete) {
				score += 1ull << 40;
			}
			return score;
		}

	} // anonymous namespace

	void DeviceImpl::SetDebugName(u64 objectHandle, u32 objectType, const char* name) {
		if (!fd.vkSetDebugUtilsObjectNameEXT || !name || lost) {
			return;
		}
		VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
		info.objectType = static_cast<VkObjectType>(objectType);
		info.objectHandle = objectHandle;
		info.pObjectName = name;
		fd.vkSetDebugUtilsObjectNameEXT(device, &info);
	}

	DeletionQueue& DeviceImpl::DeletionQueueForDestroy() {
		if (inFrame) {
			return frames[frameIndex].deletionQueue;
		}
		return frames[(frameIndex + FRAMES_IN_FLIGHT - 1) % FRAMES_IN_FLIGHT].deletionQueue;
	}

	void DeviceImpl::FlushDeletionQueue(DeletionQueue& queue) {
		for (const DeletionQueue::TableRelease& release : queue.tableReleases) {
			table.freeIndices[release.binding].push_back(release.index);
		}
		for (const DeletionQueue::DeferredBuffer& entry : queue.buffers) {
			vmaDestroyBuffer(allocator, entry.buffer, entry.allocation);
		}
		for (const DeletionQueue::DeferredTexture& entry : queue.textures) {
			if (entry.view) {
				fd.vkDestroyImageView(device, entry.view, nullptr);
			}
			if (entry.allocation) {
				vmaDestroyImage(allocator, entry.image, entry.allocation);
			}
		}
		for (VkSampler sampler : queue.samplers) {
			fd.vkDestroySampler(device, sampler, nullptr);
		}
		for (VkPipeline pipeline : queue.pipelines) {
			fd.vkDestroyPipeline(device, pipeline, nullptr);
		}
		for (VkPipelineLayout layout : queue.pipelineLayouts) {
			fd.vkDestroyPipelineLayout(device, layout, nullptr);
		}
		queue.buffers.clear();
		queue.textures.clear();
		queue.samplers.clear();
		queue.pipelines.clear();
		queue.pipelineLayouts.clear();
		queue.tableReleases.clear();
	}

	UploadRingBlock& DeviceImpl::EnsureUploadSpace(u64 size, u64 alignment) {
		FrameContext& frame = frames[frameIndex];

		for (u32 i = frame.uploadRingActiveBlock; i < frame.uploadRing.size(); ++i) {
			UploadRingBlock& block = frame.uploadRing[i];
			u64 aligned = (block.used + alignment - 1) & ~(alignment - 1);
			if (aligned + size <= block.capacity) {
				frame.uploadRingActiveBlock = i;
				return block;
			}
		}

		constexpr u64 DEFAULT_BLOCK = 4ull * 1024 * 1024;
		u64 capacity = std::max(DEFAULT_BLOCK, size + alignment);

		UploadRingBlock block;
		block.capacity = capacity;
		block.used = 0;
		VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bufferInfo.size = capacity;
		bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
		allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		VmaAllocationInfo resultInfo{};
		if (!lost) {
			SLT_VK_CHECK(*this, vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &block.buffer, &block.allocation, &resultInfo));
		}
		if (lost) {
			block.buffer = VK_NULL_HANDLE;
			block.mapped = static_cast<u8*>(Allocate(capacity, MemTag::Graphics, 16));
			frame.uploadRing.push_back(block);
			frame.uploadRingActiveBlock = static_cast<u32>(frame.uploadRing.size() - 1);
			return frame.uploadRing.back();
		}
		block.mapped = static_cast<u8*>(resultInfo.pMappedData);

		auto [index, generation] = buffers.Acquire();
		BufferResource& resource = buffers.slots[index].resource;
		resource.buffer = block.buffer;
		resource.allocation = VK_NULL_HANDLE;
		resource.size = capacity;
		resource.usage = BufferUsage::Uniform | BufferUsage::Storage | BufferUsage::TransferSrc | BufferUsage::DeviceAddress;
		resource.access = MemoryAccess::CPUToGPU;
		resource.mapped = block.mapped;
		{
			VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
			addressInfo.buffer = block.buffer;
			resource.deviceAddress = fd.vkGetBufferDeviceAddress(device, &addressInfo);
		}
		block.handle = BufferHandle{index, generation};

		SetDebugName(reinterpret_cast<u64>(block.buffer), VK_OBJECT_TYPE_BUFFER, "UploadRingBlock");

		frame.uploadRing.push_back(block);
		frame.uploadRingActiveBlock = static_cast<u32>(frame.uploadRing.size() - 1);
		return frame.uploadRing.back();
	}

	void DeviceImpl::DestroyCore() {
		if (device && fd.vkDestroyDevice) {
			fd.vkDestroyDevice(device, nullptr);
		}
		if (debugMessenger && fi.vkDestroyDebugUtilsMessengerEXT) {
			reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(fi.vkDestroyDebugUtilsMessengerEXT)(instance, debugMessenger, nullptr);
		}
		if (instance && fi.vkDestroyInstance) {
			fi.vkDestroyInstance(instance, nullptr);
		}
		device = VK_NULL_HANDLE;
		queue = VK_NULL_HANDLE;
		physicalDevice = VK_NULL_HANDLE;
		debugMessenger = VK_NULL_HANDLE;
		instance = VK_NULL_HANDLE;
		fd = {};
		fi = {};
	}

	bool DeviceImpl::CreateCore(bool rebuild) {
		DeviceImpl* impl = this;
		impl->validation = desc.enableValidation;
		impl->headless = desc.headless;

		auto fail = [&](const char* reason) {
			if (rebuild) {
				rebuildFailure = reason;
			} else {
				SLT_LOG_ERROR("Graphics", "Device::Create failed: {}", reason);
			}
			DestroyCore();
			return false;
		};
		auto failCall = [&](const char* call, VkResult result) {
			return fail((str(call) + " returned " + VkResultName(result)).c_str());
		};

		u32 layerCount = 0;
		VulkanLoader::Get_vkEnumerateInstanceLayerProperties()(&layerCount, nullptr);
		Vector<VkLayerProperties> layers{GetMemoryResource(MemTag::Graphics)};
		layers.resize(layerCount);
		VulkanLoader::Get_vkEnumerateInstanceLayerProperties()(&layerCount, layers.data());

		bool validationAvailable = false;
		for (const VkLayerProperties& layer : layers) {
			if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
				validationAvailable = true;
				break;
			}
		}
		if (impl->validation && !validationAvailable) {
			SLT_LOG_DEBUG("Graphics", "validation requested but VK_LAYER_KHRONOS_validation isn't installed");
			impl->validation = false;
		}

		u32 instanceExtensionCount = 0;
		VulkanLoader::Get_vkEnumerateInstanceExtensionProperties()(nullptr, &instanceExtensionCount, nullptr);
		Vector<VkExtensionProperties> instanceExtensions{GetMemoryResource(MemTag::Graphics)};
		instanceExtensions.resize(instanceExtensionCount);
		VulkanLoader::Get_vkEnumerateInstanceExtensionProperties()(nullptr, &instanceExtensionCount, instanceExtensions.data());

		Vector<const char*> enabledExtensions{GetMemoryResource(MemTag::Graphics)};
		Vector<const char*> windowExtensions{GetMemoryResource(MemTag::Graphics)};
		for (const char* extension : VulkanPlatform::GetSurfaceExtensions()) {
			if (HasExtension(instanceExtensions, extension)) {
				windowExtensions.push_back(extension);
			}
		}
		bool haveSurfaceExtensions = HasExtension(instanceExtensions, VK_KHR_SURFACE_EXTENSION_NAME) &&
									 !windowExtensions.empty();
		if (!desc.headless && !haveSurfaceExtensions) {
			static const str unavailable = [] {
				str text = VK_KHR_SURFACE_EXTENSION_NAME;
				for (const char* extension : VulkanPlatform::GetSurfaceExtensions()) {
					text += " / ";
					text += extension;
				}
				return text + " unavailable";
			}();
			return fail(unavailable.c_str());
		}
		impl->surfaceCapabilities2 = haveSurfaceExtensions && HasExtension(instanceExtensions, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
		if (haveSurfaceExtensions) {
			enabledExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
			enabledExtensions.insert(enabledExtensions.end(), windowExtensions.begin(), windowExtensions.end());
		}
		if (impl->surfaceCapabilities2) {
			enabledExtensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
		}
		bool haveDebugUtils = HasExtension(instanceExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		if (impl->validation && haveDebugUtils) {
			enabledExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		}

		VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		appInfo.pApplicationName = "Shalltear";
		appInfo.pEngineName = "Shalltear";
		appInfo.apiVersion = VK_API_VERSION_1_3;

		const char* validationLayer = "VK_LAYER_KHRONOS_validation";
		VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		instanceInfo.pApplicationInfo = &appInfo;
		instanceInfo.enabledExtensionCount = static_cast<u32>(enabledExtensions.size());
		instanceInfo.ppEnabledExtensionNames = enabledExtensions.data();
		if (impl->validation) {
			instanceInfo.enabledLayerCount = 1;
			instanceInfo.ppEnabledLayerNames = &validationLayer;
		}

		VkResult result = VulkanLoader::Get_vkCreateInstance()(&instanceInfo, nullptr, &impl->instance);
		if (result != VK_SUCCESS) {
			return failCall("vkCreateInstance", result);
		}
		if (!VulkanLoader::LoadInstanceTable(impl->instance, impl->fi)) {
			return fail("instance function table incomplete");
		}

		if (impl->validation && haveDebugUtils && impl->fi.vkCreateDebugUtilsMessengerEXT) {
			VkDebugUtilsMessengerCreateInfoEXT messengerInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
			messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
			messengerInfo.pfnUserCallback = &DebugMessengerCallback;
			reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(impl->fi.vkCreateDebugUtilsMessengerEXT)(
				impl->instance, &messengerInfo, nullptr, &impl->debugMessenger);
		}

		u32 physicalCount = 0;
		impl->fi.vkEnumeratePhysicalDevices(impl->instance, &physicalCount, nullptr);
		if (physicalCount == 0) {
			return fail("no Vulkan physical devices");
		}
		Vector<VkPhysicalDevice> physicals{GetMemoryResource(MemTag::Graphics)};
		physicals.resize(physicalCount);
		impl->fi.vkEnumeratePhysicalDevices(impl->instance, &physicalCount, physicals.data());

		Vector<CandidateInfo> candidates{GetMemoryResource(MemTag::Graphics)};
		impl->adapters.clear();
		for (VkPhysicalDevice physical : physicals) {
			candidates.push_back(EvaluateAdapter(impl->fi, impl->instance, physical, impl->headless));
			impl->adapters.push_back(candidates.back().info);
		}

		i32 selected = -1;
		if (rebuild) {
			for (size_t i = 0; i < candidates.size() && selected < 0; ++i) {
				if (candidates[i].usable && candidates[i].vendor == impl->adapterVendor && candidates[i].device == impl->adapterDevice) {
					selected = static_cast<i32>(i);
				}
			}
			if (selected < 0) {
				return fail("adapter unavailable");
			}
		} else if (desc.forceAdapterIndex >= 0) {
			if (desc.forceAdapterIndex < static_cast<i32>(candidates.size()) && candidates[desc.forceAdapterIndex].usable) {
				selected = desc.forceAdapterIndex;
			} else {
				SLT_LOG_WARN("Graphics", "forced adapter idx {} unusable (falling back to scored selection)", desc.forceAdapterIndex);
			}
		}
		if (selected < 0) {
			u64 bestScore = 0;
			for (size_t i = 0; i < candidates.size(); ++i) {
				if (!candidates[i].usable) {
					continue;
				}
				u64 score = ScoreAdapter(candidates[i]);
				if (selected < 0 || score > bestScore) {
					selected = static_cast<i32>(i);
					bestScore = score;
				}
			}
		}
		if (selected < 0) {
			for (size_t i = 0; i < candidates.size(); ++i) {
				SLT_LOG_ERROR("Graphics", "  adapter {}: '{}' rejected: {}", i, candidates[i].info.name, candidates[i].rejection);
			}
			return fail("no adapter satisfies the Vulkan 1.3 feature contract");
		}

		impl->selectedAdapter = selected;
		impl->physicalDevice = candidates[selected].physical;
		impl->queueFamily = candidates[selected].graphicsFamily;
		impl->adapterVendor = candidates[selected].vendor;
		impl->adapterDevice = candidates[selected].device;
		if (!rebuild) {
			SLT_LOG_INFO("Graphics", "Selected adapter: '{}' ({} MiB VRAM, api {}.{}.{})",
						 impl->adapters[selected].name,
						 impl->adapters[selected].dedicatedVideoMemory >> 20,
						 VK_API_VERSION_MAJOR(impl->adapters[selected].apiVersion),
						 VK_API_VERSION_MINOR(impl->adapters[selected].apiVersion),
						 VK_API_VERSION_PATCH(impl->adapters[selected].apiVersion));
			if (impl->validation) {
				SLT_LOG_INFO("Graphics", "Vulkan validation layer on (slower frames). Set [Graphics] validation = false to disable");
			}
		}

		{
			VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
			impl->fi.vkGetPhysicalDeviceProperties2(impl->physicalDevice, &props);
			impl->maxAnisotropy = props.properties.limits.maxSamplerAnisotropy;
			impl->timestampSeconds = props.properties.limits.timestampComputeAndGraphics
										 ? static_cast<f64>(props.properties.limits.timestampPeriod) * 1.0e-9
										 : 0.0;
			impl->framebufferColorSamples = static_cast<u32>(props.properties.limits.framebufferColorSampleCounts);
			impl->framebufferDepthSamples = static_cast<u32>(props.properties.limits.framebufferDepthSampleCounts);
		}

		f32 queuePriority = 1.0f;
		VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		queueInfo.queueFamilyIndex = impl->queueFamily;
		queueInfo.queueCount = 1;
		queueInfo.pQueuePriorities = &queuePriority;

		VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
		features13.dynamicRendering = VK_TRUE;
		features13.synchronization2 = VK_TRUE;
		features13.shaderDemoteToHelperInvocation = VK_TRUE;
		VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
		features12.pNext = &features13;
		features12.bufferDeviceAddress = VK_TRUE;
		features12.timelineSemaphore = VK_TRUE;
		features12.descriptorIndexing = VK_TRUE;
		VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
		features.pNext = &features12;
		features.features.samplerAnisotropy = VK_TRUE;
		features.features.shaderInt64 = VK_TRUE;
		Vector<VkExtensionProperties> available{GetMemoryResource(MemTag::Graphics)};
		{
			u32 count = 0;
			impl->fi.vkEnumerateDeviceExtensionProperties(impl->physicalDevice, nullptr, &count, nullptr);
			available.resize(count);
			impl->fi.vkEnumerateDeviceExtensionProperties(impl->physicalDevice, nullptr, &count, available.data());
		}
		const bool haveFaultExtension = HasExtension(available, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);

		bool tableFeatures = false;
		VkPhysicalDeviceFaultFeaturesEXT faultFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
		{
			VkPhysicalDeviceVulkan12Features supported12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
			VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
			supported.pNext = &supported12;
			if (haveFaultExtension) {
				supported12.pNext = &faultFeatures;
			}
			impl->fi.vkGetPhysicalDeviceFeatures2(impl->physicalDevice, &supported);
			tableFeatures = supported12.runtimeDescriptorArray && supported12.shaderSampledImageArrayNonUniformIndexing &&
							supported12.descriptorBindingPartiallyBound && supported12.descriptorBindingSampledImageUpdateAfterBind &&
							supported12.descriptorBindingUpdateUnusedWhilePending;
			const bool fillModeNonSolid = supported.features.fillModeNonSolid == VK_TRUE;
			const bool imageCubeArray = supported.features.imageCubeArray == VK_TRUE;
			const bool textureCompressionBC = supported.features.textureCompressionBC == VK_TRUE;
			if (rebuild && ((impl->tableSupported && !tableFeatures) || (impl->fillModeNonSolid && !fillModeNonSolid) ||
							(impl->imageCubeArray && !imageCubeArray) || (impl->textureCompressionBC && !textureCompressionBC))) {
				return fail("adapter lost feature in use");
			}
			impl->fillModeNonSolid = fillModeNonSolid;
			impl->imageCubeArray = imageCubeArray;
			impl->textureCompressionBC = textureCompressionBC;
		}
		impl->tableFeatures = tableFeatures;
		impl->faultInfo = haveFaultExtension && faultFeatures.deviceFault == VK_TRUE;
		impl->checkpoints = HasExtension(available, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
		if (impl->checkpoints) {
			impl->marks.resize(static_cast<size_t>(MARK_COUNT) * MARK_LENGTH, '\0');
		}
		features.features.fillModeNonSolid = impl->fillModeNonSolid ? VK_TRUE : VK_FALSE;
		features.features.imageCubeArray = impl->imageCubeArray ? VK_TRUE : VK_FALSE;
		features.features.textureCompressionBC = impl->textureCompressionBC ? VK_TRUE : VK_FALSE;
		features12.runtimeDescriptorArray = tableFeatures ? VK_TRUE : VK_FALSE;
		features12.shaderSampledImageArrayNonUniformIndexing = tableFeatures ? VK_TRUE : VK_FALSE;
		features12.descriptorBindingPartiallyBound = tableFeatures ? VK_TRUE : VK_FALSE;
		features12.descriptorBindingSampledImageUpdateAfterBind = tableFeatures ? VK_TRUE : VK_FALSE;
		features12.descriptorBindingUpdateUnusedWhilePending = tableFeatures ? VK_TRUE : VK_FALSE;

		Vector<const char*> deviceExtensions{GetMemoryResource(MemTag::Graphics)};
		const bool swapchain = (!impl->headless || haveSurfaceExtensions) && HasExtension(available, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
		if (swapchain) {
			deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
		}
		if (impl->faultInfo) {
			deviceExtensions.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
			faultFeatures.pNext = nullptr;
			faultFeatures.deviceFaultVendorBinary = VK_FALSE;
			features13.pNext = &faultFeatures;
		}
		if (impl->checkpoints) {
			deviceExtensions.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
		}

		VkPhysicalDevicePresentIdFeaturesKHR presentId{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
		VkPhysicalDevicePresentWaitFeaturesKHR presentWait{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
		VkPhysicalDevicePresentId2FeaturesKHR presentId2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR};
		VkPhysicalDevicePresentWait2FeaturesKHR presentWait2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR};
		impl->presentWait = PresentWait::None;
		if (swapchain) {
			VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
			if (impl->surfaceCapabilities2 && HasExtension(available, VK_KHR_PRESENT_ID_2_EXTENSION_NAME) &&
				HasExtension(available, VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME)) {
				supported.pNext = &presentId2;
				presentId2.pNext = &presentWait2;
				impl->fi.vkGetPhysicalDeviceFeatures2(impl->physicalDevice, &supported);
				if (presentId2.presentId2 && presentWait2.presentWait2) {
					impl->presentWait = PresentWait::V2;
					deviceExtensions.push_back(VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
					deviceExtensions.push_back(VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME);
				}
			}
			if (impl->presentWait == PresentWait::None && HasExtension(available, VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
				HasExtension(available, VK_KHR_PRESENT_WAIT_EXTENSION_NAME)) {
				supported.pNext = &presentId;
				presentId.pNext = &presentWait;
				impl->fi.vkGetPhysicalDeviceFeatures2(impl->physicalDevice, &supported);
				if (presentId.presentId && presentWait.presentWait) {
					impl->presentWait = PresentWait::V1;
					deviceExtensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
					deviceExtensions.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
				}
			}
			if (impl->presentWait != PresentWait::None) {
				VkBaseOutStructure* tail = reinterpret_cast<VkBaseOutStructure*>(&features13);
				while (tail->pNext) {
					tail = tail->pNext;
				}
				tail->pNext = reinterpret_cast<VkBaseOutStructure*>(supported.pNext);
			}
		}

		VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		deviceInfo.pNext = &features;
		deviceInfo.queueCreateInfoCount = 1;
		deviceInfo.pQueueCreateInfos = &queueInfo;
		deviceInfo.enabledExtensionCount = static_cast<u32>(deviceExtensions.size());
		deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();

		result = impl->fi.vkCreateDevice(impl->physicalDevice, &deviceInfo, nullptr, &impl->device);
		if (result != VK_SUCCESS) {
			return failCall("vkCreateDevice", result);
		}
		if (!VulkanLoader::LoadDeviceTable(impl->device, impl->fi.vkGetDeviceProcAddr, impl->fd)) {
			return fail("device function table incomplete");
		}
		if (!(impl->presentWait == PresentWait::V2 ? impl->fd.vkWaitForPresent2KHR != nullptr : impl->fd.vkWaitForPresentKHR != nullptr)) {
			impl->presentWait = PresentWait::None;
		}
		impl->fd.vkGetDeviceQueue(impl->device, impl->queueFamily, 0, &impl->queue);
		if (!tableFeatures && !rebuild) {
			SLT_LOG_WARN("Graphics", "Adapter lacks descriptor indexing features for texture table");
		}
		return true;
	}

	bool DeviceImpl::CreateObjects() {
		DeviceImpl* impl = this;
		if (tableFeatures) {
			impl->CreateTable();
		}
		tableSupported = table.set != VK_NULL_HANDLE;

		VmaVulkanFunctions vmaFunctions{};
		vmaFunctions.vkGetInstanceProcAddr = VulkanLoader::GetInstanceProcAddrFn();
		vmaFunctions.vkGetDeviceProcAddr = impl->fi.vkGetDeviceProcAddr;

		VmaAllocatorCreateInfo allocatorInfo{};
		allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
		allocatorInfo.physicalDevice = impl->physicalDevice;
		allocatorInfo.device = impl->device;
		allocatorInfo.instance = impl->instance;
		allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
		allocatorInfo.pVulkanFunctions = &vmaFunctions;
		allocatorInfo.pAllocationCallbacks = GetVmaHostCallbacks();
		if (vmaCreateAllocator(&allocatorInfo, &impl->allocator) != VK_SUCCESS) {
			impl->allocator = VK_NULL_HANDLE;
			return false;
		}

		VkSemaphoreTypeCreateInfo timelineType{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
		timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		semaphoreInfo.pNext = &timelineType;
		SLT_VK_CHECK(*impl, impl->fd.vkCreateSemaphore(impl->device, &semaphoreInfo, nullptr, &impl->timeline));

		for (FrameContext& frame : impl->frames) {
			VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
			poolInfo.queueFamilyIndex = impl->queueFamily;
			SLT_VK_CHECK(*impl, impl->fd.vkCreateCommandPool(impl->device, &poolInfo, nullptr, &frame.commandPool));

			VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
			allocInfo.commandPool = frame.commandPool;
			allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			allocInfo.commandBufferCount = 1;
			SLT_VK_CHECK(*impl, impl->fd.vkAllocateCommandBuffers(impl->device, &allocInfo, &frame.commandBuffer));
			SLT_VK_CHECK(*impl, impl->fd.vkAllocateCommandBuffers(impl->device, &allocInfo, &frame.uploadCommandBuffer));

			frame.commandListImpl.device = impl;
			frame.commandListImpl.commandBuffer = frame.commandBuffer;
			frame.commandList._impl = &frame.commandListImpl;
		}

		VkCommandPoolCreateInfo immediatePoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		immediatePoolInfo.queueFamilyIndex = impl->queueFamily;
		SLT_VK_CHECK(*impl, impl->fd.vkCreateCommandPool(impl->device, &immediatePoolInfo, nullptr, &impl->immediatePool));
		VkCommandBufferAllocateInfo immediateAlloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		immediateAlloc.commandPool = impl->immediatePool;
		immediateAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		immediateAlloc.commandBufferCount = 1;
		SLT_VK_CHECK(*impl, impl->fd.vkAllocateCommandBuffers(impl->device, &immediateAlloc, &impl->immediateBuffer));
		VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		SLT_VK_CHECK(*impl, impl->fd.vkCreateFence(impl->device, &fenceInfo, nullptr, &impl->immediateFence));

		VkPipelineCacheCreateInfo cacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
		SLT_VK_CHECK(*impl, impl->fd.vkCreatePipelineCache(impl->device, &cacheInfo, nullptr, &impl->pipelineCache));

		impl->gpuFrameSeconds = PRESENT_WORK_UNKNOWN;
		if (impl->timestampSeconds > 0.0) {
			VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
			queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
			queryInfo.queryCount = FRAMES_IN_FLIGHT * 2;
			if (impl->fd.vkCreateQueryPool(impl->device, &queryInfo, nullptr, &impl->frameTimestamps) != VK_SUCCESS) {
				impl->frameTimestamps = VK_NULL_HANDLE;
			}
		}
		return impl->timeline && impl->immediatePool && impl->immediateFence && impl->pipelineCache;
	}

	void DeviceImpl::ReleaseObjects() {
		if (device) {
			fd.vkDeviceWaitIdle(device);
		}
		for (FrameContext& frame : frames) {
			for (UploadRingBlock& block : frame.uploadRing) {
				if (block.buffer) {
					buffers.Release(block.handle.index);
					vmaDestroyBuffer(allocator, block.buffer, block.allocation);
				} else {
					Free(block.mapped);
				}
			}
			frame.uploadRing.clear();
			frame.uploadRingActiveBlock = 0;
		}
		if (!device) {
			return;
		}

		DiscardPendingUploads();
		for (SwapChain* swapChain : swapChains) {
			ReleaseSwapChainObjects(*swapChain->_impl);
		}

		for (FrameContext& frame : frames) {
			FlushDeletionQueue(frame.deletionQueue);
			DestroyFrameDescriptors(frame);
			if (frame.commandPool) {
				fd.vkDestroyCommandPool(device, frame.commandPool, nullptr);
			}
			frame.commandPool = VK_NULL_HANDLE;
			frame.commandBuffer = VK_NULL_HANDLE;
			frame.uploadCommandBuffer = VK_NULL_HANDLE;
			frame.commandListImpl.commandBuffer = VK_NULL_HANDLE;
			frame.waitSemaphores.clear();
			frame.signalSemaphores.clear();
			frame.timelineValue = 0;
			frame.timed = false;
		}
		FlushDeletionQueue(immediateDeletions);
		if (frameTimestamps) {
			fd.vkDestroyQueryPool(device, frameTimestamps, nullptr);
			frameTimestamps = VK_NULL_HANDLE;
		}

		for (auto& slot : buffers.slots) {
			BufferResource& resource = slot.resource;
			if (slot.alive && resource.buffer) {
				vmaDestroyBuffer(allocator, resource.buffer, resource.allocation);
			}
			resource.buffer = VK_NULL_HANDLE;
			resource.allocation = VK_NULL_HANDLE;
			resource.mapped = nullptr;
			resource.deviceAddress = 0;
		}
		for (auto& slot : textures.slots) {
			TextureResource& resource = slot.resource;
			if (slot.alive) {
				for (VkImageView layerView : resource.layerViews) {
					fd.vkDestroyImageView(device, layerView, nullptr);
				}
				if (resource.view) {
					fd.vkDestroyImageView(device, resource.view, nullptr);
				}
				if (resource.allocation) {
					vmaDestroyImage(allocator, resource.image, resource.allocation);
				}
			}
			resource.layerViews.clear();
			resource.view = VK_NULL_HANDLE;
			resource.image = VK_NULL_HANDLE;
			resource.allocation = VK_NULL_HANDLE;
		}
		for (auto& slot : samplers.slots) {
			if (slot.alive && slot.resource.sampler) {
				fd.vkDestroySampler(device, slot.resource.sampler, nullptr);
			}
			slot.resource.sampler = VK_NULL_HANDLE;
		}
		for (auto& slot : pipelines.slots) {
			PipelineResource& resource = slot.resource;
			if (slot.alive && resource.pipeline) {
				fd.vkDestroyPipeline(device, resource.pipeline, nullptr);
			}
			if (slot.alive && resource.layout) {
				fd.vkDestroyPipelineLayout(device, resource.layout, nullptr);
			}
			resource.pipeline = VK_NULL_HANDLE;
			resource.layout = VK_NULL_HANDLE;
			for (VkDescriptorSetLayout& setLayout : resource.setLayouts) {
				setLayout = VK_NULL_HANDLE;
			}
		}

		for (auto& [hash, layout] : setLayoutCache) {
			fd.vkDestroyDescriptorSetLayout(device, layout, nullptr);
		}
		setLayoutCache.clear();
		DestroyTable();

		if (pipelineCache) {
			fd.vkDestroyPipelineCache(device, pipelineCache, nullptr);
		}
		if (immediateFence) {
			fd.vkDestroyFence(device, immediateFence, nullptr);
		}
		if (immediatePool) {
			fd.vkDestroyCommandPool(device, immediatePool, nullptr);
		}
		if (timeline) {
			fd.vkDestroySemaphore(device, timeline, nullptr);
		}
		pipelineCache = VK_NULL_HANDLE;
		immediateFence = VK_NULL_HANDLE;
		immediatePool = VK_NULL_HANDLE;
		immediateBuffer = VK_NULL_HANDLE;
		timeline = VK_NULL_HANDLE;
		timelineCounter = 0;
		frameIndex = 0;

		if (allocator) {
			vmaDestroyAllocator(allocator);
		}
		allocator = VK_NULL_HANDLE;
	}

	bool DeviceImpl::RestoreObjects() {
		const bool hadTable = tableSupported;
		if (!CreateObjects() || (hadTable && !tableSupported)) {
			rebuildFailure = "object creation failed";
			return false;
		}
		for (u32 binding = 0; binding < TABLE_BINDING_COUNT; ++binding) {
			if (table.used[binding] > table.capacity[binding]) {
				rebuildFailure = "texture table shrank";
				return false;
			}
		}
		for (auto& slot : buffers.slots) {
			if (slot.alive && !CreateBufferObject(slot.resource, nullptr)) {
				rebuildFailure = "buffer creation failed";
				return false;
			}
		}
		for (auto& slot : textures.slots) {
			if (slot.alive && !CreateTextureObjects(slot.resource, nullptr)) {
				rebuildFailure = "texture creation failed";
				return false;
			}
			if (slot.alive && slot.resource.tableIndex != TABLE_NONE) {
				WriteTableEntry(TableBindingOf(slot.resource.dimension), slot.resource.tableIndex, slot.resource.view, VK_NULL_HANDLE);
			}
		}
		for (auto& slot : samplers.slots) {
			if (slot.alive && !CreateSamplerObject(slot.resource)) {
				rebuildFailure = "sampler creation failed";
				return false;
			}
			if (slot.alive && slot.resource.tableIndex != TABLE_NONE) {
				WriteTableEntry(TABLE_SAMPLERS_BINDING, slot.resource.tableIndex, VK_NULL_HANDLE, slot.resource.sampler);
			}
		}

		ImmediateSubmit([&](VkCommandBuffer cmd) {
			for (auto& slot : textures.slots) {
				const TextureResource& resource = slot.resource;
				if (slot.alive && resource.state != ResourceState::Undefined) {
					CmdTransitionImage(*this, cmd, resource.image, ResourceState::Undefined, resource.state, IsDepthFormat(resource.format));
				}
			}
		});
		if (lost) {
			rebuildFailure = "lost again";
			return false;
		}

		for (SwapChain* swapChain : swapChains) {
			RestoreSwapChainObjects(*swapChain->_impl);
		}
		return true;
	}

	Device* Device::Create(const DeviceDesc& desc) {
		if (!VulkanLoader::EnsureLoaded()) {
			SLT_LOG_INFO("Graphics", "Device::Create failed: Vulkan loader unavailable");
			return nullptr;
		}

		DeviceImpl* impl = New<DeviceImpl>(MemTag::Graphics);
		impl->desc = desc;
		impl->mainThread = std::this_thread::get_id();
		if (!impl->CreateCore(false)) {
			Delete(impl);
			return nullptr;
		}
		if (!impl->CreateObjects()) {
			SLT_LOG_ERROR("Graphics", "Device::Create failed: object creation failed");
			impl->ReleaseObjects();
			impl->DestroyCore();
			Delete(impl);
			return nullptr;
		}

		Device* device = New<Device>(MemTag::Graphics);
		device->_impl = impl;
		return device;
	}

	void Device::Destroy(Device* device) {
		if (!device) {
			return;
		}
		DeviceImpl* impl = device->_impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		SLT_CORE_ASSERT_MSG(!impl->inFrame, "Device::Destroy called inside a frame");

		if (!impl->swapChains.empty()) {
			SLT_LOG_ERROR("Graphics", "{} swapchain(s) still alive at device teardown. Destroying", impl->swapChains.size());
			while (!impl->swapChains.empty()) {
				device->DestroySwapChain(impl->swapChains.back());
			}
		}

		impl->ReleaseObjects();

		auto reportLeaks = [](const char* kind, u32 count) {
			if (count > 0) {
				SLT_LOG_ERROR("Graphics", "{} {} object(s) leaked at device teardown", count, kind);
			}
		};
		reportLeaks("buffer", impl->buffers.liveCount);
		reportLeaks("texture", impl->textures.liveCount);
		reportLeaks("sampler", impl->samplers.liveCount);
		reportLeaks("pipeline", impl->pipelines.liveCount);
		SLT_CORE_ASSERT_MSG(
			impl->buffers.liveCount == 0 && impl->textures.liveCount == 0 &&
				impl->samplers.liveCount == 0 && impl->pipelines.liveCount == 0,
			"GPU resources leaked at device teardown (see log)");

		impl->DestroyCore();

		Delete(impl);
		Delete(device);
	}

	bool Device::IsLost() const {
		return _impl->lost;
	}

	u32 Device::GetFailedRebuildCount() const {
		return _impl->failedRebuilds;
	}

	void Device::SimulateLoss(u32 failedRebuilds) {
		SLT_RHI_ASSERT_MAIN_THREAD(_impl);
		_impl->simulatedFailures = failedRebuilds;
		_impl->MarkLost(nullptr);
	}

	bool Device::Recover() {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		SLT_CORE_ASSERT_MSG(!impl->inFrame, "Recover called inside a frame");
		if (!impl->lost) {
			return true;
		}
		const auto started = std::chrono::steady_clock::now();
		if (started < impl->nextRebuild) {
			return false;
		}

		impl->ReleaseObjects();
		impl->DestroyCore();
		impl->lost = false;
		const auto lostAt = impl->lostAt;
		bool rebuilt = false;
		if (impl->simulatedFailures > 0) {
			--impl->simulatedFailures;
			impl->rebuildFailure = "simulated";
		} else {
			rebuilt = impl->CreateCore(true) && impl->RestoreObjects();
		}
		if (!rebuilt) {
			impl->ReleaseObjects();
			impl->DestroyCore();
			const bool lostAgain = impl->lost;
			impl->lost = true;
			impl->lostAt = lostAt;
			if (!lostAgain) {
				impl->nextRebuild = started + std::chrono::milliseconds(REBUILD_RETRY_MS);
			}
			++impl->failedRebuilds;
			if (LogsFailedRebuild(impl->failedRebuilds)) {
				SLT_LOG_WARN("Graphics", "couldn't rebuild device ({}, {} attempt(s) in {} s). Retrying", impl->rebuildFailure, impl->failedRebuilds,
							 std::chrono::duration_cast<std::chrono::seconds>(started - lostAt).count());
			}
			return false;
		}

		impl->failedRebuilds = 0;
		impl->deadPipelineWarned = false;
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
		SLT_LOG_INFO("Graphics", "device rebuilt in {} ms ({} buffer(s), {} texture(s), {} sampler(s), {} swapchain(s))", elapsed.count(),
					 impl->buffers.liveCount, impl->textures.liveCount, impl->samplers.liveCount, impl->swapChains.size());
		return true;
	}

	bool DeviceImpl::NoteLoss(VkResult result, const char* call) {
		if (result == VK_ERROR_DEVICE_LOST) {
			MarkLost(call);
		}
		return lost;
	}

	bool DeviceImpl::WaitForSubmission(u64 timelineValue, u64 timeoutNanoseconds) {
		if (lost) {
			return false;
		}
		if (timelineValue == 0) {
			return true;
		}
		VkSemaphoreWaitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
		waitInfo.semaphoreCount = 1;
		waitInfo.pSemaphores = &timeline;
		waitInfo.pValues = &timelineValue;
		const VkResult result = fd.vkWaitSemaphores(device, &waitInfo, timeoutNanoseconds);
		if (result == VK_TIMEOUT) {
			return false;
		}
		SLT_VK_CHECK(*this, result);
		return result == VK_SUCCESS;
	}

	void DeviceImpl::MarkLost(const char* call) {
		if (lost) {
			return;
		}
		lost = true;
		lossSimulated = call == nullptr;
		const auto now = std::chrono::steady_clock::now();
		lostAt = now;
		if (lossSimulated) {
			nextRebuild = now;
			SLT_LOG_WARN("Graphics", "device loss simulated");
			return;
		}

		const u32 inWindow = losses.Note(static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(now - created).count()));
		const u64 delayMs = RebuildDelayMs(inWindow);
		nextRebuild = now + std::chrono::milliseconds(delayMs);
		SLT_LOG_ERROR("Graphics", "device lost ({} returned VK_ERROR_DEVICE_LOST, {} loss(es) in last {} s). Rebuilding in {} s", call, inWindow,
					  LOSS_WINDOW_MS / 1000, delayMs / 1000);
		LogLossCause();
	}

	namespace {

		const char* FaultAddressName(VkDeviceFaultAddressTypeEXT type) {
			switch (type) {
			case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT:
				return "invalid read";
			case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT:
				return "invalid write";
			case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT:
				return "invalid execute";
			case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT:
			case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT:
			case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT:
				return "instruction pointer";
			default:
				return "address";
			}
		}

	} // anonymous namespace

	void DeviceImpl::LogLossCause() {
		if (checkpoints && fd.vkGetQueueCheckpointDataNV) {
			u32 count = 0;
			fd.vkGetQueueCheckpointDataNV(queue, &count, nullptr);
			Vector<VkCheckpointDataNV> reached{GetMemoryResource(MemTag::Graphics)};
			reached.resize(count, VkCheckpointDataNV{VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
			fd.vkGetQueueCheckpointDataNV(queue, &count, reached.data());
			for (u32 i = 0; i < count; ++i) {
				const char* name = static_cast<const char*>(reached[i].pCheckpointMarker);
				const bool ours = name >= marks.data() && name < marks.data() + marks.size();
				SLT_LOG_ERROR("Graphics", "  GPU {} '{}'", reached[i].stage == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT ? "finished" : "started",
							  ours ? name : "<frame start>");
			}
		}
		if (!faultInfo || !fd.vkGetDeviceFaultInfoEXT) {
			return;
		}
		VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
		if (fd.vkGetDeviceFaultInfoEXT(device, &counts, nullptr) != VK_SUCCESS) {
			return;
		}
		Vector<VkDeviceFaultAddressInfoEXT> addresses{GetMemoryResource(MemTag::Graphics)};
		Vector<VkDeviceFaultVendorInfoEXT> vendors{GetMemoryResource(MemTag::Graphics)};
		addresses.resize(counts.addressInfoCount);
		vendors.resize(counts.vendorInfoCount);
		counts.vendorBinarySize = 0;
		VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
		info.pAddressInfos = addresses.data();
		info.pVendorInfos = vendors.data();
		const VkResult result = fd.vkGetDeviceFaultInfoEXT(device, &counts, &info);
		if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
			return;
		}
		info.description[VK_MAX_DESCRIPTION_SIZE - 1] = '\0';
		if (info.description[0] != '\0') {
			SLT_LOG_ERROR("Graphics", "  driver: {}", info.description);
		}
		for (u32 i = 0; i < counts.addressInfoCount && i < addresses.size(); ++i) {
			const VkDeviceFaultAddressInfoEXT& fault = addresses[i];
			const u64 precision = fault.addressPrecision > 0 ? fault.addressPrecision : 1;
			const u64 low = fault.reportedAddress & ~(precision - 1);
			const u64 high = fault.reportedAddress | (precision - 1);
			u32 owner = 0;
			for (u32 index = 1; index < buffers.slots.size() && owner == 0; ++index) {
				const BufferResource& buffer = buffers.slots[index].resource;
				if (buffers.slots[index].alive && buffer.deviceAddress != 0 && high >= buffer.deviceAddress && low < buffer.deviceAddress + buffer.size) {
					owner = index;
				}
			}
			if (owner != 0) {
				SLT_LOG_ERROR("Graphics", "  {} at 0x{:x} (in live {}-byte buffer)", FaultAddressName(fault.addressType), fault.reportedAddress,
							  buffers.slots[owner].resource.size);
			} else {
				SLT_LOG_ERROR("Graphics", "  {} at 0x{:x} (in no live buffer)", FaultAddressName(fault.addressType), fault.reportedAddress);
			}
		}
		for (u32 i = 0; i < counts.vendorInfoCount && i < vendors.size(); ++i) {
			vendors[i].description[VK_MAX_DESCRIPTION_SIZE - 1] = '\0';
			SLT_LOG_ERROR("Graphics", "  driver fault 0x{:x} (data 0x{:x}): {}", vendors[i].vendorFaultCode, vendors[i].vendorFaultData, vendors[i].description);
		}
	}

	std::span<const AdapterInfo> Device::GetAdapters() const {
		return {_impl->adapters.data(), _impl->adapters.size()};
	}

	const AdapterInfo& Device::GetSelectedAdapter() const {
		return _impl->adapters[_impl->selectedAdapter];
	}

	DeviceCapabilities Device::GetCapabilities() const {
		DeviceCapabilities caps;
		caps.bufferDeviceAddress = true;
		caps.bindlessTextureTable = true;
		caps.pushConstants = true;
		caps.dynamicFrontFace = true;
		caps.dynamicDepthBias = true;
		caps.wireframeFill = _impl->fillModeNonSolid;
		caps.persistentBufferMapping = true;
		caps.syncReadback = true;
		caps.textureCompressionBC = _impl->textureCompressionBC;
		caps.maxSampleCount = 4;
		caps.shaderFormat = ShaderBytecodeFormat::Spirv;
		return caps;
	}

	u32 Device::GetSupportedSampleCounts(Format format, TextureUsage usage) const {
		DeviceImpl* impl = _impl;
		if (format == Format::Unknown || !impl->physicalDevice) {
			return 1;
		}

		VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
		info.format = ToVkFormat(format);
		info.type = VK_IMAGE_TYPE_2D;
		info.tiling = VK_IMAGE_TILING_OPTIMAL;
		info.usage = ToVkImageUsage(usage);
		VkImageFormatProperties2 props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
		if (impl->fi.vkGetPhysicalDeviceImageFormatProperties2(impl->physicalDevice, &info, &props) != VK_SUCCESS) {
			return 1;
		}

		u32 counts = static_cast<u32>(props.imageFormatProperties.sampleCounts);
		if (HasFlag(usage, TextureUsage::RenderTarget)) {
			counts &= impl->framebufferColorSamples;
		}
		if (HasFlag(usage, TextureUsage::DepthStencil)) {
			counts &= impl->framebufferDepthSamples;
		}
		return counts | 1u;
	}

	VideoMemoryInfo Device::GetVideoMemoryInfo() const {
		if (!_impl->allocator) {
			return {};
		}
		VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
		vmaGetHeapBudgets(_impl->allocator, budgets);

		VkPhysicalDeviceMemoryProperties2 memory{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
		_impl->fi.vkGetPhysicalDeviceMemoryProperties2(_impl->physicalDevice, &memory);

		VideoMemoryInfo info;
		for (u32 i = 0; i < memory.memoryProperties.memoryHeapCount; ++i) {
			if (memory.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
				info.usedBytes += budgets[i].usage;
				info.budgetBytes += budgets[i].budget;
			}
		}
		return info;
	}

	u32 Device::GetLiveBufferCount() const { return _impl->buffers.liveCount; }
	u32 Device::GetLiveTextureCount() const { return _impl->textures.liveCount; }
	u32 Device::GetLiveSamplerCount() const { return _impl->samplers.liveCount; }
	u32 Device::GetLivePipelineCount() const { return _impl->pipelines.liveCount; }

	void Device::WaitIdle() {
		SLT_RHI_ASSERT_MAIN_THREAD(_impl);
		_impl->FlushPendingUploads();
		if (!_impl->lost) {
			_impl->NoteLoss(_impl->fd.vkDeviceWaitIdle(_impl->device), "vkDeviceWaitIdle");
		}
	}

	void Device::LoadPipelineCache(std::span<const u8> blob) {
		SLT_RHI_ASSERT_MAIN_THREAD(_impl);
		if (blob.empty() || _impl->lost) {
			return;
		}
		VkPipelineCache replacement = VK_NULL_HANDLE;
		VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
		info.initialDataSize = blob.size();
		info.pInitialData = blob.data();
		if (_impl->fd.vkCreatePipelineCache(_impl->device, &info, nullptr, &replacement) == VK_SUCCESS) {
			_impl->fd.vkDestroyPipelineCache(_impl->device, _impl->pipelineCache, nullptr);
			_impl->pipelineCache = replacement;
		}
	}

	size_t Device::SavePipelineCache(std::span<u8> out) const {
		SLT_RHI_ASSERT_MAIN_THREAD(_impl);
		if (_impl->lost) {
			return 0;
		}
		size_t size = 0;
		_impl->fd.vkGetPipelineCacheData(_impl->device, _impl->pipelineCache, &size, nullptr);
		if (out.empty() || size == 0) {
			return size;
		}
		if (out.size() < size) {
			return 0;
		}
		if (_impl->fd.vkGetPipelineCacheData(_impl->device, _impl->pipelineCache, &size, out.data()) != VK_SUCCESS) {
			return 0;
		}
		return size;
	}

	CommandList* Device::BeginFrame() {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		SLT_CORE_ASSERT_MSG(!impl->inFrame, "BeginFrame called twice without EndFrame");

		FrameContext& frame = impl->frames[impl->frameIndex];

		impl->WaitForSubmission(frame.timelineValue, UINT64_MAX);

		const u32 firstTimestamp = impl->frameIndex * 2;
		if (!impl->lost && frame.timed) {
			u64 ticks[2] = {};
			if (impl->fd.vkGetQueryPoolResults(impl->device, impl->frameTimestamps, firstTimestamp, 2, sizeof(ticks), ticks, sizeof(u64),
											   VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
				ticks[1] >= ticks[0]) {
				impl->gpuFrameSeconds = static_cast<f64>(ticks[1] - ticks[0]) * impl->timestampSeconds;
			}
		}
		frame.timed = false;

		if (!impl->lost) {
			impl->FlushDeletionQueue(frame.deletionQueue);
			SLT_VK_CHECK(*impl, impl->fd.vkResetCommandPool(impl->device, frame.commandPool, 0));
			impl->ResetFrameDescriptors(frame);
		}
		for (UploadRingBlock& block : frame.uploadRing) {
			block.used = 0;
		}
		frame.uploadRingActiveBlock = 0;
		frame.waitSemaphores.clear();
		frame.signalSemaphores.clear();

		if (!impl->lost) {
			VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			SLT_VK_CHECK(*impl, impl->fd.vkBeginCommandBuffer(frame.commandBuffer, &begin));
			if (impl->frameTimestamps) {
				impl->fd.vkCmdResetQueryPool(frame.commandBuffer, impl->frameTimestamps, firstTimestamp, 2);
				impl->fd.vkCmdWriteTimestamp2(frame.commandBuffer, VK_PIPELINE_STAGE_2_NONE, impl->frameTimestamps, firstTimestamp);
			}
		}

		frame.commandListImpl.boundPipelineIndex = 0;
		frame.commandListImpl.inRenderPass = false;
		frame.commandListImpl.pipelineRefused = false;
		frame.commandListImpl.frontFaceSet = false;

		impl->inFrame = true;
		return &frame.commandList;
	}

	void Device::EndFrame() {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		SLT_CORE_ASSERT_MSG(impl->inFrame, "EndFrame without BeginFrame");

		FrameContext& frame = impl->frames[impl->frameIndex];
		SLT_CORE_ASSERT_MSG(!frame.commandListImpl.inRenderPass, "EndFrame inside an open render pass");

		if (impl->lost) {
			impl->inFrame = false;
			return;
		}

		if (impl->frameTimestamps) {
			impl->fd.vkCmdWriteTimestamp2(frame.commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, impl->frameTimestamps, impl->frameIndex * 2 + 1);
			frame.timed = true;
		}
		SLT_VK_CHECK(*impl, impl->fd.vkEndCommandBuffer(frame.commandBuffer));

		const bool prologue = impl->HasPendingUploads();
		if (prologue) {
			VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			SLT_VK_CHECK(*impl, impl->fd.vkBeginCommandBuffer(frame.uploadCommandBuffer, &begin));
			impl->RecordPendingUploads(frame.uploadCommandBuffer, frame.deletionQueue);
			SLT_VK_CHECK(*impl, impl->fd.vkEndCommandBuffer(frame.uploadCommandBuffer));
		}

		Vector<VkSemaphoreSubmitInfo> waits{GetMemoryResource(MemTag::Graphics)};
		for (VkSemaphore semaphore : frame.waitSemaphores) {
			VkSemaphoreSubmitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
			info.semaphore = semaphore;
			info.stageMask = ACQUIRE_WAIT_STAGES;
			waits.push_back(info);
		}

		Vector<VkSemaphoreSubmitInfo> signals{GetMemoryResource(MemTag::Graphics)};
		for (VkSemaphore semaphore : frame.signalSemaphores) {
			VkSemaphoreSubmitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
			info.semaphore = semaphore;
			info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			signals.push_back(info);
		}
		{
			VkSemaphoreSubmitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
			info.semaphore = impl->timeline;
			info.value = ++impl->timelineCounter;
			info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			signals.push_back(info);
		}

		VkCommandBufferSubmitInfo cmdInfos[2]{{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO},
											  {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}};
		u32 cmdCount = 0;
		if (prologue) {
			cmdInfos[cmdCount++].commandBuffer = frame.uploadCommandBuffer;
		}
		cmdInfos[cmdCount++].commandBuffer = frame.commandBuffer;

		VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
		submit.waitSemaphoreInfoCount = static_cast<u32>(waits.size());
		submit.pWaitSemaphoreInfos = waits.data();
		submit.commandBufferInfoCount = cmdCount;
		submit.pCommandBufferInfos = cmdInfos;
		submit.signalSemaphoreInfoCount = static_cast<u32>(signals.size());
		submit.pSignalSemaphoreInfos = signals.data();
		SLT_VK_CHECK(*impl, impl->fd.vkQueueSubmit2(impl->queue, 1, &submit, VK_NULL_HANDLE));

		impl->inFrame = false;
		if (impl->lost) {
			return;
		}
		frame.timelineValue = impl->timelineCounter;
		impl->frameIndex = (impl->frameIndex + 1) % FRAMES_IN_FLIGHT;
	}

	bool Device::IsInFrame() const {
		return _impl->inFrame;
	}

	TransientAllocation Device::AllocateTransient(u64 size, u64 alignment) {
		DeviceImpl* impl = _impl;
		SLT_RHI_ASSERT_MAIN_THREAD(impl);
		SLT_CORE_ASSERT_MSG(impl->inFrame, "AllocateTransient outside a frame");
		SLT_CORE_ASSERT(size > 0 && IsPowerOfTwo(alignment));

		UploadRingBlock& block = impl->EnsureUploadSpace(size, alignment);
		u64 offset = (block.used + alignment - 1) & ~(alignment - 1);
		block.used = offset + size;

		TransientAllocation allocation;
		allocation.buffer = block.handle;
		allocation.offset = offset;
		allocation.cpu = block.mapped + offset;
		return allocation;
	}

} // namespace Shalltear::RHI
