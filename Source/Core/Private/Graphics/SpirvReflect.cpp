#include "Shalltear/Core/Graphics/ShaderReflection.h"

#include "Shalltear/Core/Log.h"

#include <algorithm>
#include <format>
#include <unordered_map>

namespace Shalltear {

	namespace {

		constexpr u32 SPIRV_MAGIC = 0x07230203;

		enum SpvOp : u16 {
			OpName = 5,
			OpMemberName = 6,
			OpEntryPoint = 15,
			OpExecutionMode = 16,
			OpTypeVoid = 19,
			OpTypeBool = 20,
			OpTypeInt = 21,
			OpTypeFloat = 22,
			OpTypeVector = 23,
			OpTypeMatrix = 24,
			OpTypeImage = 25,
			OpTypeSampler = 26,
			OpTypeSampledImage = 27,
			OpTypeArray = 28,
			OpTypeRuntimeArray = 29,
			OpTypeStruct = 30,
			OpTypePointer = 32,
			OpConstant = 43,
			OpSpecConstantTrue = 48,
			OpSpecConstantFalse = 49,
			OpSpecConstant = 50,
			OpVariable = 59,
			OpDecorate = 71,
			OpMemberDecorate = 72,
		};

		enum SpvStorageClass : u32 {
			StorageUniformConstant = 0,
			StorageInput = 1,
			StorageUniform = 2,
			StorageOutput = 3,
			StoragePushConstant = 9,
			StorageStorageBuffer = 12,
		};

		enum SpvDecoration : u32 {
			DecorationSpecId = 1,
			DecorationBlock = 2,
			DecorationBufferBlock = 3,
			DecorationMatrixStride = 7,
			DecorationArrayStride = 6,
			DecorationBuiltIn = 11,
			DecorationNonWritable = 24,
			DecorationLocation = 30,
			DecorationBinding = 33,
			DecorationDescriptorSet = 34,
			DecorationOffset = 35,
		};

		enum SpvExecutionModel : u32 {
			ExecutionModelVertex = 0,
			ExecutionModelFragment = 4,
			ExecutionModelGLCompute = 5,
		};

		constexpr u32 EXECUTION_MODE_LOCAL_SIZE = 17;

		struct TypeEntry {
			u16 op = 0;
			u32 words[4] = {};
			Vector<u32> memberTypes{GetMemoryResource(MemTag::Asset)};
		};

		struct VariableEntry {
			u32 typeId = 0;
			u32 storageClass = 0;
		};

		struct Decorations {
			bool hasSet = false, hasBinding = false, hasLocation = false, hasBuiltIn = false, isBlock = false, isBufferBlock = false, nonWritable = false;
			u32 set = 0, binding = 0;
		};

		RHI::TextureDimension ImageDimension(u32 dim, bool arrayed) {
			switch (dim) {
			case 1:
				return arrayed ? RHI::TextureDimension::Texture2DArray : RHI::TextureDimension::Texture2D;
			case 2:
				return RHI::TextureDimension::Texture3D;
			case 3:
				return arrayed ? RHI::TextureDimension::CubeArray : RHI::TextureDimension::Cube;
			default:
				return RHI::TextureDimension::Texture2D;
			}
		}

	} // anonymous namespace

	SpirvReflection ReflectSpirv(std::span<const u8> blob) {
		SpirvReflection reflection;

		if (blob.size() < 20 || blob.size() % 4 != 0) {
			reflection.error = "not a SPIR-V module (bad size)";
			return reflection;
		}
		const u32* words = reinterpret_cast<const u32*>(blob.data());
		size_t wordCount = blob.size() / 4;
		if (words[0] != SPIRV_MAGIC) {
			reflection.error = "not a SPIR-V module (bad magic)";
			return reflection;
		}

		std::unordered_map<u32, TypeEntry> types;
		std::unordered_map<u32, VariableEntry> variables;
		std::unordered_map<u32, Decorations> decorations;
		std::unordered_map<u32, u32> constants;
		std::unordered_map<u32, str> names;
		std::unordered_map<u64, str> memberNames;
		std::unordered_map<u64, u32> memberOffsets;
		std::unordered_map<u64, u32> memberMatrixStrides;
		std::unordered_map<u32, u32> arrayStrides;
		std::unordered_map<u32, u32> specIds;
		std::vector<u32> specConstants;

		size_t cursor = 5;
		while (cursor < wordCount) {
			u32 first = words[cursor];
			u16 op = static_cast<u16>(first & 0xFFFF);
			u16 length = static_cast<u16>(first >> 16);
			if (length == 0 || cursor + length > wordCount) {
				reflection.error = "malformed SPIR-V (bad instruction length)";
				return reflection;
			}
			const u32* operands = words + cursor + 1;
			u32 operandCount = length - 1;

			switch (op) {
			case OpEntryPoint:
				if (operandCount >= 2) {
					u32 model = operands[0];
					if (model == ExecutionModelVertex) {
						reflection.stage = RHI::ShaderStage::Vertex;
					} else if (model == ExecutionModelFragment) {
						reflection.stage = RHI::ShaderStage::Pixel;
					} else if (model == ExecutionModelGLCompute) {
						reflection.stage = RHI::ShaderStage::Compute;
					} else {
						reflection.error = "unsupported execution model (vertex, px and compute only)";
						return reflection;
					}
				}
				break;
			case OpExecutionMode:
				if (operandCount >= 5 && operands[1] == EXECUTION_MODE_LOCAL_SIZE) {
					reflection.threadGroupSize = Vec3I{static_cast<i32>(operands[2]), static_cast<i32>(operands[3]),
													   static_cast<i32>(operands[4])};
				}
				break;
			case OpName:
				if (operandCount >= 2) {
					const char* text = reinterpret_cast<const char*>(operands + 1);
					size_t maxLength = static_cast<size_t>(operandCount - 1) * 4;
					names[operands[0]] = str(text, strnlen(text, maxLength));
				}
				break;
			case OpMemberName:
				if (operandCount >= 3) {
					const char* text = reinterpret_cast<const char*>(operands + 2);
					size_t maxLength = static_cast<size_t>(operandCount - 2) * 4;
					u64 key = (static_cast<u64>(operands[0]) << 32) | operands[1];
					memberNames[key] = str(text, strnlen(text, maxLength));
				}
				break;
			case OpDecorate:
				if (operandCount >= 2) {
					Decorations& decor = decorations[operands[0]];
					switch (operands[1]) {
					case DecorationDescriptorSet:
						decor.hasSet = true;
						decor.set = operands[2];
						break;
					case DecorationBinding:
						decor.hasBinding = true;
						decor.binding = operands[2];
						break;
					case DecorationLocation:
						decor.hasLocation = true;
						break;
					case DecorationBuiltIn:
						decor.hasBuiltIn = true;
						break;
					case DecorationBlock:
						decor.isBlock = true;
						break;
					case DecorationBufferBlock:
						decor.isBufferBlock = true;
						break;
					case DecorationNonWritable:
						decor.nonWritable = true;
						break;
					case DecorationArrayStride:
						arrayStrides[operands[0]] = operands[2];
						break;
					case DecorationSpecId:
						if (operandCount >= 3) {
							specIds[operands[0]] = operands[2];
						}
						break;
					default:
						break;
					}
				}
				break;
			case OpMemberDecorate:
				if (operandCount >= 3 && operands[2] == DecorationNonWritable) {
					decorations[operands[0]].nonWritable = true;
				}
				if (operandCount >= 4) {
					u64 key = (static_cast<u64>(operands[0]) << 32) | operands[1];
					if (operands[2] == DecorationOffset) {
						memberOffsets[key] = operands[3];
					} else if (operands[2] == DecorationMatrixStride) {
						memberMatrixStrides[key] = operands[3];
					}
				}
				break;
			case OpTypeVoid:
			case OpTypeBool:
			case OpTypeSampler: {
				TypeEntry& entry = types[operands[0]];
				entry.op = op;
				break;
			}
			case OpTypeInt:
			case OpTypeFloat:
			case OpTypeVector:
			case OpTypeMatrix:
			case OpTypeImage:
			case OpTypeSampledImage:
			case OpTypeArray:
			case OpTypeRuntimeArray:
			case OpTypePointer: {
				TypeEntry& entry = types[operands[0]];
				entry.op = op;
				for (u32 i = 0; i < std::min(operandCount - 1, 4u); ++i) {
					entry.words[i] = operands[1 + i];
				}
				break;
			}
			case OpTypeStruct: {
				TypeEntry& entry = types[operands[0]];
				entry.op = op;
				for (u32 i = 1; i < operandCount; ++i) {
					entry.memberTypes.push_back(operands[i]);
				}
				break;
			}
			case OpConstant:
				if (operandCount >= 3) {
					constants[operands[1]] = operands[2];
				}
				break;
			case OpSpecConstant:
			case OpSpecConstantTrue:
			case OpSpecConstantFalse:
				specConstants.push_back(operandCount >= 2 ? operands[1] : 0u);
				break;
			case OpVariable:
				if (operandCount >= 3) {
					VariableEntry& variable = variables[operands[1]];
					variable.typeId = operands[0];
					variable.storageClass = operands[2];
				}
				break;
			default:
				break;
			}
			cursor += length;
		}

		if (reflection.stage == RHI::ShaderStage::None) {
			reflection.error = "no entry point found";
			return reflection;
		}
		auto engineConstant = [](u32 constantId) {
			return constantId == SHADER_LOD_FADE_CONSTANT_ID || constantId == SHADER_SHADOW_MOMENTS_CONSTANT_ID ||
				   constantId == SHADER_GAMMA_COLOR_SPACE_CONSTANT_ID;
		};
		for (u32 id : specConstants) {
			const auto found = specIds.find(id);
			if (found == specIds.end() || !engineConstant(found->second)) {
				reflection.error = "specialization constants aren't supported in v1";
				return reflection;
			}
		}

		auto nameOf = [&](u32 id) -> str {
			auto it = names.find(id);
			return it != names.end() && !it->second.empty() ? it->second : std::format("<id {}>", id);
		};

		auto sizeOfType = [&](auto&& self, u32 typeId) -> u32 {
			auto it = types.find(typeId);
			if (it == types.end()) {
				return 0;
			}
			const TypeEntry& type = it->second;
			switch (type.op) {
			case OpTypeBool:
				return 4;
			case OpTypeInt:
			case OpTypeFloat:
				return type.words[0] / 8;
			case OpTypeVector:
				return self(self, type.words[0]) * type.words[1];
			case OpTypeMatrix: {
				return self(self, type.words[0]) * type.words[1];
			}
			case OpTypeArray: {
				u32 length = 0;
				auto constantIt = constants.find(type.words[1]);
				if (constantIt != constants.end()) {
					length = constantIt->second;
				}
				auto strideIt = arrayStrides.find(typeId);
				u32 stride = strideIt != arrayStrides.end() ? strideIt->second : self(self, type.words[0]);
				return stride * length;
			}
			case OpTypeStruct: {
				u32 size = 0;
				for (u32 member = 0; member < type.memberTypes.size(); ++member) {
					u64 key = (static_cast<u64>(typeId) << 32) | member;
					u32 offset = 0;
					auto offsetIt = memberOffsets.find(key);
					if (offsetIt != memberOffsets.end()) {
						offset = offsetIt->second;
					}
					u32 memberSize = self(self, type.memberTypes[member]);
					auto strideIt = memberMatrixStrides.find(key);
					if (strideIt != memberMatrixStrides.end()) {
						auto memberType = types.find(type.memberTypes[member]);
						if (memberType != types.end() && memberType->second.op == OpTypeMatrix) {
							memberSize = strideIt->second * memberType->second.words[1];
						}
					}
					size = std::max(size, offset + memberSize);
				}
				return size;
			}
			default:
				return 0;
			}
		};

		auto memberTypeOf = [&](u32 typeId) -> UniformMemberType {
			auto it = types.find(typeId);
			if (it == types.end()) {
				return UniformMemberType::Unsupported;
			}
			const TypeEntry& type = it->second;
			switch (type.op) {
			case OpTypeBool:
				return UniformMemberType::Bool;
			case OpTypeInt:
				return UniformMemberType::Int;
			case OpTypeFloat:
				return type.words[0] == 32 ? UniformMemberType::Float : UniformMemberType::Unsupported;
			case OpTypeVector: {
				auto componentIt = types.find(type.words[0]);
				bool isFloat32 = componentIt != types.end() && componentIt->second.op == OpTypeFloat &&
								 componentIt->second.words[0] == 32;
				if (!isFloat32) {
					return UniformMemberType::Unsupported;
				}
				switch (type.words[1]) {
				case 2:
					return UniformMemberType::Vec2;
				case 3:
					return UniformMemberType::Vec3;
				case 4:
					return UniformMemberType::Vec4;
				default:
					return UniformMemberType::Unsupported;
				}
			}
			default:
				return UniformMemberType::Unsupported;
			}
		};

		auto pointeeOf = [&](u32 pointerTypeId) -> u32 {
			auto it = types.find(pointerTypeId);
			if (it == types.end() || it->second.op != OpTypePointer) {
				return 0;
			}
			return it->second.words[1];
		};

		for (const auto& [id, variable] : variables) {
			switch (variable.storageClass) {
			case StorageInput: {
				auto decorIt = decorations.find(id);
				bool hasLocation = decorIt != decorations.end() && decorIt->second.hasLocation;
				if (reflection.stage == RHI::ShaderStage::Vertex && hasLocation) {
					reflection.error = std::format("vertex shader declares input attribute '{}'. Geometry must be pulled via buffer device address", nameOf(id));
					return reflection;
				}
				break;
			}
			case StoragePushConstant: {
				u32 pointee = pointeeOf(variable.typeId);
				reflection.pushConstantSize = std::max(reflection.pushConstantSize, sizeOfType(sizeOfType, pointee));
				break;
			}
			case StorageUniformConstant:
			case StorageUniform:
			case StorageStorageBuffer: {
				auto decorIt = decorations.find(id);
				if (decorIt == decorations.end() || !decorIt->second.hasSet || !decorIt->second.hasBinding) {
					reflection.error = std::format("resource '{}' lacks set/binding decorations", nameOf(id));
					return reflection;
				}

				RHI::DescriptorBindingDesc binding;
				binding.set = decorIt->second.set;
				binding.binding = decorIt->second.binding;
				binding.stages = reflection.stage;

				u32 pointee = pointeeOf(variable.typeId);
				auto typeIt = types.find(pointee);
				u16 typeOp = typeIt != types.end() ? typeIt->second.op : u16{0};
				auto pointeeDecor = decorations.find(pointee);
				const bool isStorageBuffer = variable.storageClass == StorageStorageBuffer ||
											 (variable.storageClass == StorageUniform &&
											  pointeeDecor != decorations.end() && pointeeDecor->second.isBufferBlock);

				bool portablePushBlock = false;
				if (binding.set == RHI::TABLE_SET) {
					if (typeOp == OpTypeRuntimeArray) {
						typeIt = types.find(typeIt->second.words[0]);
						typeOp = typeIt != types.end() ? typeIt->second.op : u16{0};
						const bool sampler = typeOp == OpTypeSampler && binding.binding == RHI::TABLE_SAMPLERS_BINDING;
						RHI::TextureDimension dimension = RHI::TextureDimension::Count;
						if (typeOp == OpTypeImage) {
							dimension = ImageDimension(typeIt->second.words[1], typeIt->second.words[3] != 0);
						}
						const bool image = (binding.binding == RHI::TABLE_TEXTURES_2D_BINDING && dimension == RHI::TextureDimension::Texture2D) ||
										   (binding.binding == RHI::TABLE_TEXTURES_CUBE_BINDING && dimension == RHI::TextureDimension::Cube) ||
										   (binding.binding == RHI::TABLE_TEXTURES_3D_BINDING && dimension == RHI::TextureDimension::Texture3D);
						if (!sampler && !image) {
							reflection.error = std::format("resource '{}' doesn't match texture table binding {}", nameOf(id), binding.binding);
							return reflection;
						}
					} else {
						const bool isPushUbo = binding.binding == RHI::TABLE_PUSH_BINDING &&
											   !isStorageBuffer &&
											   (variable.storageClass == StorageUniform || typeOp == OpTypeStruct);
						const bool isStorage = (binding.binding == RHI::TABLE_TRANSIENT_RING_BINDING ||
												binding.binding == RHI::TABLE_VERTEX_HEAP_BINDING ||
												binding.binding == RHI::TABLE_ATLAS_META_BINDING) &&
											   isStorageBuffer;
						const bool isAtlas2D = binding.binding == RHI::TABLE_ATLAS_2D_BINDING && typeOp == OpTypeImage &&
											   ImageDimension(typeIt->second.words[1], typeIt->second.words[3] != 0) == RHI::TextureDimension::Texture2D;
						const bool isAtlasCube = binding.binding == RHI::TABLE_ATLAS_CUBE_BINDING && typeOp == OpTypeImage &&
												 ImageDimension(typeIt->second.words[1], typeIt->second.words[3] != 0) == RHI::TextureDimension::CubeArray;
						const bool isAtlasSampler = binding.binding == RHI::TABLE_ATLAS_SAMPLER_BINDING && typeOp == OpTypeSampler;
						if (!isPushUbo && !isStorage && !isAtlas2D && !isAtlasCube && !isAtlasSampler) {
							reflection.error = std::format("resource '{}' in set {} isn't a texture table array", nameOf(id), RHI::TABLE_SET);
							return reflection;
						}
						portablePushBlock = isPushUbo;
					}
				} else if (typeOp == OpTypeArray || typeOp == OpTypeRuntimeArray) {
					reflection.error = std::format("resource '{}' is an array of descriptors (unsupported)", nameOf(id));
					return reflection;
				}
				if (typeOp == OpTypeSampledImage) {
					reflection.error = std::format("resource '{}' is a combined image sampler. Split texture + sampler bindings are required", nameOf(id));
					return reflection;
				}

				if (isStorageBuffer) {
					binding.type = RHI::DescriptorType::StorageBuffer;
					binding.readOnly = decorIt->second.nonWritable ||
									   (pointeeDecor != decorations.end() && pointeeDecor->second.nonWritable);
				} else if (variable.storageClass == StorageUniform) {
					binding.type = RHI::DescriptorType::UniformBuffer;
				} else if (typeOp == OpTypeImage) {
					binding.type = RHI::DescriptorType::SampledTexture;
				} else if (typeOp == OpTypeSampler) {
					binding.type = RHI::DescriptorType::Sampler;
				} else if (typeOp == OpTypeStruct) {
					binding.type = RHI::DescriptorType::UniformBuffer;
				} else {
					reflection.error = std::format("resource '{}' has an unsupported descriptor type", nameOf(id));
					return reflection;
				}

				reflection.bindings.push_back(binding);

				if (portablePushBlock && typeOp == OpTypeStruct) {
					reflection.pushConstantSize = std::max(reflection.pushConstantSize, sizeOfType(sizeOfType, pointee));
					break;
				}

				ShaderResourceInfo resource;
				resource.set = binding.set;
				resource.binding = binding.binding;
				resource.type = binding.type;
				if (typeOp == OpTypeImage) {
					resource.dimension = ImageDimension(typeIt->second.words[1], typeIt->second.words[3] != 0);
				}
				auto structNameIt = names.find(pointee);
				auto variableNameIt = names.find(id);
				bool preferStructName = (typeOp == OpTypeStruct);
				if (preferStructName && structNameIt != names.end() && !structNameIt->second.empty()) {
					resource.name = structNameIt->second;
				} else if (variableNameIt != names.end() && !variableNameIt->second.empty()) {
					resource.name = variableNameIt->second;
				} else if (structNameIt != names.end()) {
					resource.name = structNameIt->second;
				}
				reflection.resources.push_back(std::move(resource));

				if (binding.type == RHI::DescriptorType::UniformBuffer && typeOp == OpTypeStruct) {
					UniformBlockLayout block;
					block.set = binding.set;
					block.binding = binding.binding;
					block.sizeBytes = sizeOfType(sizeOfType, pointee);

					const TypeEntry& structType = typeIt->second;
					for (u32 member = 0; member < structType.memberTypes.size(); ++member) {
						u64 key = (static_cast<u64>(pointee) << 32) | member;

						UniformMember info;
						auto nameIt = memberNames.find(key);
						if (nameIt != memberNames.end()) {
							info.name = nameIt->second;
						}
						auto offsetIt = memberOffsets.find(key);
						info.offset = offsetIt != memberOffsets.end() ? offsetIt->second : 0;
						info.type = memberTypeOf(structType.memberTypes[member]);
						info.sizeBytes = sizeOfType(sizeOfType, structType.memberTypes[member]);
						block.members.push_back(std::move(info));
					}
					reflection.uniformBlocks.push_back(std::move(block));
				}
				break;
			}
			default:
				break;
			}
		}

		std::sort(reflection.bindings.begin(), reflection.bindings.end(),
				  [](const RHI::DescriptorBindingDesc& a, const RHI::DescriptorBindingDesc& b) {
					  return a.set != b.set ? a.set < b.set : a.binding < b.binding;
				  });

		reflection.valid = true;
		return reflection;
	}

	bool MergeReflections(const SpirvReflection& vertex, const SpirvReflection& pixel,
						  Vector<RHI::DescriptorBindingDesc>& outBindings, u32& outPushConstantSize, str& outError) {
		if (!vertex.valid || !pixel.valid) {
			outError = !vertex.valid ? vertex.error : pixel.error;
			return false;
		}

		outBindings.clear();
		outBindings.assign(vertex.bindings.begin(), vertex.bindings.end());
		for (const RHI::DescriptorBindingDesc& binding : pixel.bindings) {
			bool merged = false;
			for (RHI::DescriptorBindingDesc& existing : outBindings) {
				if (existing.set == binding.set && existing.binding == binding.binding) {
					if (existing.type != binding.type) {
						outError = std::format("set {} binding {}: stages disagree on its type", binding.set, binding.binding);
						return false;
					}
					existing.stages = existing.stages | binding.stages;
					existing.readOnly = existing.readOnly && binding.readOnly;
					merged = true;
					break;
				}
			}
			if (!merged) {
				outBindings.push_back(binding);
			}
		}

		for (const RHI::DescriptorBindingDesc& binding : outBindings) {
			if (binding.set >= RHI::MAX_DESCRIPTOR_SETS) {
				outError = std::format("set {} binding {} violates the binding convention (set 0 = per-view, set 1 = per-material, set 2 = the pass row's requests, set 3 = texture table)", binding.set, binding.binding);
				return false;
			}
		}

		std::sort(outBindings.begin(), outBindings.end(),
				  [](const RHI::DescriptorBindingDesc& a, const RHI::DescriptorBindingDesc& b) {
					  return a.set != b.set ? a.set < b.set : a.binding < b.binding;
				  });

		outPushConstantSize = std::max(vertex.pushConstantSize, pixel.pushConstantSize);
		return true;
	}

	std::string_view UniformMemberTypeName(UniformMemberType type) {
		switch (type) {
		case UniformMemberType::Float:
			return "float";
		case UniformMemberType::Vec2:
			return "float2";
		case UniformMemberType::Vec3:
			return "float3";
		case UniformMemberType::Vec4:
			return "float4";
		case UniformMemberType::Int:
			return "int";
		case UniformMemberType::Bool:
			return "bool";
		default:
			return "unsupported";
		}
	}

	void MergeMaterialInterfaces(const SpirvReflection& vertex, const SpirvReflection& pixel,
								 Vector<UniformBlockLayout>& outBlocks, Vector<ShaderResourceInfo>& outResources) {
		outBlocks.clear();
		outResources.clear();

		auto mergeBlocks = [&](const Vector<UniformBlockLayout>& blocks) {
			for (const UniformBlockLayout& block : blocks) {
				bool seen = false;
				for (const UniformBlockLayout& existing : outBlocks) {
					if (existing.set == block.set && existing.binding == block.binding) {
						seen = true;
						break;
					}
				}
				if (!seen) {
					outBlocks.push_back(block);
				}
			}
		};
		auto mergeResources = [&](const Vector<ShaderResourceInfo>& resources) {
			for (const ShaderResourceInfo& resource : resources) {
				bool seen = false;
				for (const ShaderResourceInfo& existing : outResources) {
					if (existing.set == resource.set && existing.binding == resource.binding) {
						seen = true;
						break;
					}
				}
				if (!seen) {
					outResources.push_back(resource);
				}
			}
		};

		mergeBlocks(vertex.uniformBlocks);
		mergeBlocks(pixel.uniformBlocks);
		mergeResources(vertex.resources);
		mergeResources(pixel.resources);

		auto bySetBinding = [](const auto& a, const auto& b) {
			return a.set != b.set ? a.set < b.set : a.binding < b.binding;
		};
		std::sort(outBlocks.begin(), outBlocks.end(), bySetBinding);
		std::sort(outResources.begin(), outResources.end(), bySetBinding);
	}

} // namespace Shalltear
