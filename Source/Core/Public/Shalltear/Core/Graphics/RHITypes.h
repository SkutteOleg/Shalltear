#pragma once

#include "Shalltear/Core/CoreAPI.h"
#include "Shalltear/Core/EnumFlags.h"
#include "Shalltear/Core/Math/Geometry.h"
#include "Shalltear/Core/Math/Vector.h"
#include "Shalltear/Core/Math/Color.h"
#include "Shalltear/Core/NativeWindow.h"
#include "Shalltear/Core/Types.h"

#include <optional>
#include <span>
#include <string_view>

namespace Shalltear::RHI {

	struct BufferHandle {
		u32 index = 0;
		u32 generation = 0;
		bool IsValid() const { return index != 0; }
		bool operator==(const BufferHandle&) const = default;
	};
	struct TextureHandle {
		u32 index = 0;
		u32 generation = 0;
		bool IsValid() const { return index != 0; }
		bool operator==(const TextureHandle&) const = default;
	};
	struct SamplerHandle {
		u32 index = 0;
		u32 generation = 0;
		bool IsValid() const { return index != 0; }
		bool operator==(const SamplerHandle&) const = default;
	};
	struct PipelineHandle {
		u32 index = 0;
		u32 generation = 0;
		bool IsValid() const { return index != 0; }
		bool operator==(const PipelineHandle&) const = default;
	};

	// block formats are appended
	enum class Format : u8 {
		Unknown = 0,
		R8Unorm,
		RGBA8Unorm,
		RGBA8SRGB,
		BGRA8Unorm,
		BGRA8SRGB,
		RGBA16Float,
		RG16Float,
		R32Uint,
		D32Float,
		BC5Unorm,
		BC7Unorm,
		BC7SRGB,
		BC6HUFloat,
		RGBA32Float,
		Count
	};

	inline bool IsBlockCompressed(Format format) {
		return format == Format::BC5Unorm || format == Format::BC7Unorm || format == Format::BC7SRGB || format == Format::BC6HUFloat;
	}

	inline bool IsSRGB(Format format) {
		return format == Format::RGBA8SRGB || format == Format::BGRA8SRGB || format == Format::BC7SRGB;
	}

	inline Format WithoutSRGB(Format format) {
		switch (format) {
		case Format::RGBA8SRGB:
			return Format::RGBA8Unorm;
		case Format::BGRA8SRGB:
			return Format::BGRA8Unorm;
		case Format::BC7SRGB:
			return Format::BC7Unorm;
		default:
			return format;
		}
	}

	inline u64 FormatLevelBytes(Format format, Vec2I size) {
		const u64 width = static_cast<u64>(size.x > 0 ? size.x : 0);
		const u64 height = static_cast<u64>(size.y > 0 ? size.y : 0);
		switch (format) {
		case Format::BC5Unorm:
		case Format::BC7Unorm:
		case Format::BC7SRGB:
		case Format::BC6HUFloat:
			return ((width + 3) / 4) * ((height + 3) / 4) * 16;
		case Format::R8Unorm:
			return width * height;
		case Format::RGBA8Unorm:
		case Format::RGBA8SRGB:
		case Format::BGRA8Unorm:
		case Format::BGRA8SRGB:
		case Format::RG16Float:
		case Format::R32Uint:
		case Format::D32Float:
			return width * height * 4;
		case Format::RGBA16Float:
			return width * height * 8;
		case Format::RGBA32Float:
			return width * height * 16;
		case Format::Unknown:
		case Format::Count:
			break;
		}
		return 0;
	}

	enum class ResourceState : u8 {
		Undefined = 0,
		RenderTarget,
		DepthWrite,
		DepthRead,
		ShaderResource,
		TransferSrc,
		TransferDst,
		Present,
		UnorderedAccess,
		IndirectArgument,
		Count
	};

	enum class BufferUsage : u8 {
		None = 0,
		Vertex = 1 << 0,
		Index = 1 << 1,
		Uniform = 1 << 2,
		Storage = 1 << 3,
		TransferSrc = 1 << 4,
		TransferDst = 1 << 5,
		DeviceAddress = 1 << 6,
		Indirect = 1 << 7
	};
	SLT_ENUM_CLASS_FLAGS(BufferUsage)

	enum class TextureUsage : u8 {
		None = 0,
		Sampled = 1 << 0,
		RenderTarget = 1 << 1,
		DepthStencil = 1 << 2,
		TransferSrc = 1 << 3,
		TransferDst = 1 << 4
	};
	SLT_ENUM_CLASS_FLAGS(TextureUsage)

	enum class MemoryAccess : u8 {
		GPUOnly = 0,
		CPUToGPU,
		Readback
	};

	inline constexpr u32 FRAMES_IN_FLIGHT = 2;

	enum class LoadOp : u8 { Load = 0,
							 Clear,
							 DontCare };
	enum class StoreOp : u8 { Store = 0,
							  DontCare };

	enum class FilterMode : u8 { Nearest = 0,
								 Linear };
	enum class AddressMode : u8 { Repeat = 0,
								  ClampToEdge,
								  MirroredRepeat };

	enum class BlendMode : u8 { Opaque = 0,
								Alpha,
								Additive,
								Premultiplied,
								Multiply,
								Count };
	enum class DepthMode : u8 { TestWrite = 0,
								TestOnly,
								Off };
	enum class DepthCompare : u8 { NearerOrEqual = 0,
								   Nearer,
								   Farther,
								   FartherOrEqual,
								   Equal,
								   NotEqual,
								   Always,
								   Count };
	enum class CullMode : u8 { Back = 0,
							   Front,
							   Off,
							   Count };
	enum class FrontFace : u8 { Clockwise = 0,
								CounterClockwise };
	enum class PrimitiveTopology : u8 { TriangleList = 0,
										LineList,
										PointList };
	inline std::string_view BlendModeName(BlendMode mode) {
		switch (mode) {
		case BlendMode::Alpha:
			return "alpha";
		case BlendMode::Additive:
			return "additive";
		case BlendMode::Premultiplied:
			return "premultiplied";
		case BlendMode::Multiply:
			return "multiply";
		case BlendMode::Opaque:
		case BlendMode::Count:
			break;
		}
		return "opaque";
	}
	inline bool ParseBlendMode(std::string_view name, BlendMode& out) {
		for (u8 i = 0; i < static_cast<u8>(BlendMode::Count); ++i) {
			BlendMode candidate = static_cast<BlendMode>(i);
			if (BlendModeName(candidate) == name) {
				out = candidate;
				return true;
			}
		}
		return false;
	}

	inline std::string_view CullModeName(CullMode mode) {
		switch (mode) {
		case CullMode::Front:
			return "front";
		case CullMode::Off:
			return "off";
		case CullMode::Back:
		case CullMode::Count:
			break;
		}
		return "back";
	}
	inline bool ParseCullMode(std::string_view name, CullMode& out) {
		for (u8 i = 0; i < static_cast<u8>(CullMode::Count); ++i) {
			CullMode candidate = static_cast<CullMode>(i);
			if (CullModeName(candidate) == name) {
				out = candidate;
				return true;
			}
		}
		return false;
	}

	inline std::string_view DepthCompareName(DepthCompare compare) {
		switch (compare) {
		case DepthCompare::Nearer:
			return "nearer";
		case DepthCompare::Farther:
			return "farther";
		case DepthCompare::FartherOrEqual:
			return "farther_or_equal";
		case DepthCompare::Equal:
			return "equal";
		case DepthCompare::NotEqual:
			return "not_equal";
		case DepthCompare::Always:
			return "always";
		case DepthCompare::NearerOrEqual:
		case DepthCompare::Count:
			break;
		}
		return "nearer_or_equal";
	}
	inline bool ParseDepthCompare(std::string_view name, DepthCompare& out) {
		for (u8 i = 0; i < static_cast<u8>(DepthCompare::Count); ++i) {
			const DepthCompare candidate = static_cast<DepthCompare>(i);
			if (DepthCompareName(candidate) == name) {
				out = candidate;
				return true;
			}
		}
		return false;
	}

	inline std::string_view DepthModeName(DepthMode mode) {
		switch (mode) {
		case DepthMode::TestOnly:
			return "test_only";
		case DepthMode::Off:
			return "off";
		case DepthMode::TestWrite:
			break;
		}
		return "test_write";
	}
	inline bool ParseDepthMode(std::string_view name, DepthMode& out) {
		for (DepthMode candidate : {DepthMode::TestWrite, DepthMode::TestOnly, DepthMode::Off}) {
			if (DepthModeName(candidate) == name) {
				out = candidate;
				return true;
			}
		}
		return false;
	}

	enum class FillMode : u8 { Solid = 0,
							   Wireframe };

	struct ShaderRenderState {
		BlendMode blend = BlendMode::Opaque;
		DepthMode depth = DepthMode::TestWrite;
		CullMode cull = CullMode::Back;
		PrimitiveTopology topology = PrimitiveTopology::TriangleList;
		FillMode fill = FillMode::Solid;
		f32 depthBiasConstant = 0.0f;
		f32 depthBiasSlope = 0.0f;
		DepthCompare depthCompare = DepthCompare::NearerOrEqual;
		// bias pair applied per draw (not part of pipeline)
		bool dynamicDepthBias = false;
	};

	enum class DescriptorType : u8 {
		UniformBuffer = 0,
		StorageBuffer,
		SampledTexture,
		Sampler,
		Count
	};

	enum class ShaderStage : u8 {
		None = 0,
		Vertex = 1 << 0,
		Pixel = 1 << 1,
		Compute = 1 << 2
	};
	SLT_ENUM_CLASS_FLAGS(ShaderStage)

	// debugName is only read during the create call

	struct BufferDesc {
		u64 size = 0;
		BufferUsage usage = BufferUsage::None;
		MemoryAccess access = MemoryAccess::GPUOnly;
		const char* debugName = nullptr;
	};

	enum class TextureDimension : u8 { Texture2D,
									   Texture2DArray,
									   Texture3D,
									   Cube,
									   CubeArray,
									   Count };

	inline std::string_view TextureDimensionName(TextureDimension dimension) {
		switch (dimension) {
		case TextureDimension::Texture2DArray:
			return "2D array";
		case TextureDimension::Texture3D:
			return "3D";
		case TextureDimension::Cube:
			return "cube";
		case TextureDimension::CubeArray:
			return "cube array";
		case TextureDimension::Texture2D:
		case TextureDimension::Count:
			break;
		}
		return "2D";
	}

	struct TextureDesc {
		Vec2I size{};
		Format format = Format::Unknown;
		TextureUsage usage = TextureUsage::None;
		u32 mipLevels = 1;
		const char* debugName = nullptr;
		u32 samples = 1;
		u32 arrayLayers = 1;
		bool cube = false;
		bool cubeArray = false;
		u32 depth = 1;
		std::optional<TextureDimension> dimension;
	};

	inline TextureDimension ResolveTextureDimension(const TextureDesc& desc) {
		if (desc.dimension) {
			return *desc.dimension;
		}
		if (desc.cube) {
			return desc.cubeArray ? TextureDimension::CubeArray : TextureDimension::Cube;
		}
		if (desc.depth > 1) {
			return TextureDimension::Texture3D;
		}
		return desc.arrayLayers > 1 ? TextureDimension::Texture2DArray : TextureDimension::Texture2D;
	}

	struct SamplerDesc {
		FilterMode filter = FilterMode::Linear;
		AddressMode addressMode = AddressMode::Repeat;
		const char* debugName = nullptr;
		bool comparison = false;
		FilterMode mipFilter = FilterMode::Linear;
		u8 maxAnisotropy = 1;
	};

	struct DescriptorBindingDesc {
		u32 set = 0;
		u32 binding = 0;
		DescriptorType type = DescriptorType::UniformBuffer;
		ShaderStage stages = ShaderStage::None;
		bool readOnly = false;
	};

	inline constexpr u64 WHOLE_BUFFER = ~0ull;
	// buffer offset in a set must be a multiple (max device limit)
	inline constexpr u64 BINDING_OFFSET_ALIGNMENT = 256;

	inline constexpr u32 MAX_DESCRIPTOR_SETS = 4;
	inline constexpr u32 MAX_COLOR_ATTACHMENTS = 4;

	// device's persistent table (never transient)
	inline constexpr u32 TABLE_SET = 3;
	inline constexpr u32 TABLE_SAMPLERS_BINDING = 0;
	inline constexpr u32 TABLE_TEXTURES_2D_BINDING = 1;
	inline constexpr u32 TABLE_TEXTURES_CUBE_BINDING = 2;
	inline constexpr u32 TABLE_TEXTURES_3D_BINDING = 3;
	inline constexpr u32 TABLE_NONE = ~0u;

	// portable set 3 bindings (used by backends without push constants or bindless arrays)
	inline constexpr u32 TABLE_PUSH_BINDING = 0;
	inline constexpr u32 TABLE_TRANSIENT_RING_BINDING = 1;
	inline constexpr u32 TABLE_VERTEX_HEAP_BINDING = 2;
	inline constexpr u32 TABLE_ATLAS_META_BINDING = 3;
	inline constexpr u32 TABLE_ATLAS_2D_BINDING = 4;
	inline constexpr u32 TABLE_ATLAS_CUBE_BINDING = 5;
	inline constexpr u32 TABLE_ATLAS_SAMPLER_BINDING = 6;

	enum class ShaderBytecodeFormat : u8 {
		Spirv = 0,
		WGSL
	};

	struct DeviceCapabilities {
		bool bufferDeviceAddress = true;
		bool bindlessTextureTable = true;
		bool pushConstants = true;
		bool dynamicFrontFace = true;
		bool dynamicDepthBias = true;
		bool wireframeFill = true;
		bool persistentBufferMapping = true;
		bool syncReadback = true;
		bool textureCompressionBC = true;
		u32 maxSampleCount = 4;
		ShaderBytecodeFormat shaderFormat = ShaderBytecodeFormat::Spirv;
	};

	struct SpecializationConstant {
		u32 id = 0;
		u32 value = 0;
	};
	inline constexpr u32 MAX_SPECIALIZATION_CONSTANTS = 8;

	struct GraphicsPipelineDesc {
		// spans read only during creation
		std::span<const u8> vertexSpirv;
		std::span<const u8> pixelSpirv;
		std::span<const SpecializationConstant> pixelConstants;
		ShaderBytecodeFormat bytecodeFormat = ShaderBytecodeFormat::Spirv;

		std::span<const DescriptorBindingDesc> bindings;
		u32 pushConstantSize = 0;

		BlendMode blend = BlendMode::Opaque;
		DepthMode depth = DepthMode::TestWrite;
		CullMode cull = CullMode::Back;
		PrimitiveTopology topology = PrimitiveTopology::TriangleList;
		FillMode fill = FillMode::Solid;
		f32 depthBiasConstant = 0.0f;
		f32 depthBiasSlope = 0.0f;
		// pair above ignored, set via CommandList::SetDepthBias
		bool dynamicDepthBias = false;

		Format colorFormats[MAX_COLOR_ATTACHMENTS] = {};
		u32 colorFormatCount = 0;
		Format depthFormat = Format::Unknown;

		const char* debugName = nullptr;

		// must equal the pass attachments' sample count
		u32 samples = 1;
		DepthCompare depthCompare = DepthCompare::NearerOrEqual;
	};

	// bound outside a render pass only
	struct ComputePipelineDesc {
		std::span<const u8> spirv;
		ShaderBytecodeFormat bytecodeFormat = ShaderBytecodeFormat::Spirv;
		std::span<const DescriptorBindingDesc> bindings;
		u32 pushConstantSize = 0;
		const char* debugName = nullptr;
	};

	struct RenderPassDesc {
		struct Attachment {
			TextureHandle texture;
			LoadOp load = LoadOp::Clear;
			StoreOp store = StoreOp::Store;
			Color clearColor = Color::Black();
			u32 layer = 0;
			u32 mip = 0;
		};
		struct DepthAttachment {
			TextureHandle texture;
			LoadOp load = LoadOp::Clear;
			StoreOp store = StoreOp::Store;
			f32 clearDepth = 0.0f;
			u32 layer = 0;
		};

		std::span<const Attachment> colorAttachments;
		const DepthAttachment* depth = nullptr;
		// optional single-sample resolve targets aligned with colorAttachments
		std::span<const TextureHandle> resolveTargets;
	};

	// frame lifetime, never stored
	struct TransientSet {
		u64 opaque = 0;
		bool IsValid() const { return opaque != 0; }
	};

	// frame lifetime
	struct TransientAllocation {
		BufferHandle buffer;
		u64 offset = 0;
		void* cpu = nullptr;
		bool IsValid() const { return cpu != nullptr; }
	};

	struct NativeSwapChainDesc {
		NativeWindowHandle window;
		Vec2I sizePx{};
		bool vsync = true;
	};

	struct AdapterInfo {
		str name;
		u64 dedicatedVideoMemory = 0;
		bool discrete = false;
		u32 apiVersion = 0;
	};

	struct DeviceDesc {
		bool enableValidation = false;
		i32 forceAdapterIndex = -1;
		bool headless = false;
	};

	struct VideoMemoryInfo {
		u64 usedBytes = 0;
		u64 budgetBytes = 0;
	};

} // namespace Shalltear::RHI
