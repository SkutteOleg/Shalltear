#include "Shalltear/Core/Graphics/ShaderAsset.h"

#include "Asset/ImportTierInternal.h"
#include "Graphics/SceneRequests.h"

#include "Shalltear/Core/Graphics/ShaderReflection.h"

#include "Shalltear/Core/Asset/PakFormat.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/StringHash.h"
#include "Shalltear/Core/StringUtil.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <string_view>

namespace Shalltear {

	namespace {

		std::atomic<u32> liveMeshReaders{0};
#ifdef SHALLTEAR_RHI_WEBGPU
		std::atomic<u8> _defaultImportFormat{static_cast<u8>(RHI::ShaderBytecodeFormat::WGSL)};
#else
		std::atomic<u8> _defaultImportFormat{static_cast<u8>(RHI::ShaderBytecodeFormat::Spirv)};
#endif

	} // anonymous namespace

	RHI::ShaderBytecodeFormat ShaderAsset::GetDefaultImportBytecodeFormat() {
		return static_cast<RHI::ShaderBytecodeFormat>(_defaultImportFormat.load(std::memory_order_relaxed));
	}

	void ShaderAsset::SetDefaultImportBytecodeFormat(RHI::ShaderBytecodeFormat format) {
		_defaultImportFormat.store(static_cast<u8>(format), std::memory_order_relaxed);
	}

	u32 SceneRequests::CountLiveMeshReaders() { return liveMeshReaders.load(std::memory_order_relaxed); }

	ShaderAsset::~ShaderAsset() {
		if (_meshReader) {
			liveMeshReaders.fetch_sub(1, std::memory_order_relaxed);
		}
	}

	void ShaderAsset::RecountMeshReader() {
		const bool reads = SceneRequests::MayReadSceneOnMesh(*this);
		if (reads != _meshReader) {
			_meshReader = reads;
			if (reads) {
				liveMeshReaders.fetch_add(1, std::memory_order_relaxed);
			} else {
				liveMeshReaders.fetch_sub(1, std::memory_order_relaxed);
			}
		}
	}

	bool ShaderAsset::Import(const Path& virtualOrPhysical) {
		if (const ImportTierTable* tier = ImportTierInternal::Get()) {
			return tier->importShader(*this, virtualOrPhysical);
		}
		SLT_LOG_ERROR("Graphics", "ShaderAsset: can't import '{}' (no ImportTier::Install in this process)",
					  virtualOrPhysical.ToString());
		return false;
	}

	u64 ShaderAsset::KeywordSetHash(std::span<const str> enabled) {
		Vector<std::string_view> sorted{GetMemoryResource(MemTag::Asset)};
		for (const str& keyword : enabled) {
			if (!keyword.empty()) {
				sorted.push_back(std::string_view{keyword});
			}
		}
		std::sort(sorted.begin(), sorted.end());
		sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

		if (sorted.empty()) {
			return 0;
		}

		u64 hash = 1469598103934665603ull;
		for (std::string_view keyword : sorted) {
			for (char c : keyword) {
				hash = (hash ^ static_cast<u8>(c)) * 1099511628211ull;
			}
			hash = (hash ^ 0xFFull) * 1099511628211ull;
		}
		return hash;
	}

	const ShaderVariant* ShaderAsset::GetVariant(u32 index) const {
		return index < variants.size() ? &variants[index] : nullptr;
	}

	u32 ShaderAsset::FindVariant(u64 keywordHash) const {
		for (size_t i = 0; i < variants.size(); ++i) {
			if (variants[i].keywordHash == keywordHash) {
				return static_cast<u32>(i);
			}
		}
		return SHADER_BASE_VARIANT;
	}

	u32 ShaderAsset::GetOrCompileVariant(std::span<const str> enabled) {
		Vector<str> wanted{GetMemoryResource(MemTag::Asset)};
		for (const str& keyword : enabled) {
			bool declared = false;
			for (const str& candidate : keywords) {
				if (candidate == keyword) {
					declared = true;
					break;
				}
			}
			if (!declared) {
				SLT_LOG_WARN("Graphics", "shader '{}': keyword '{}' is enabled but not declared. Ignoring it", virtualPath, keyword);
				continue;
			}
			wanted.push_back(keyword);
		}
		std::sort(wanted.begin(), wanted.end());
		wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
		if (wanted.empty()) {
			return SHADER_BASE_VARIANT;
		}

		u64 hash = KeywordSetHash({wanted.data(), wanted.size()});
		for (size_t i = 0; i < variants.size(); ++i) {
			if (variants[i].keywordHash == hash) {
				return static_cast<u32>(i);
			}
		}

		if (dependencies.empty()) {
			SLT_LOG_WARN("Graphics", "shader '{}': no variant was cooked for this keyword set (drawing the base variant)", virtualPath);
			return SHADER_BASE_VARIANT;
		}

		if (const ImportTierTable* tier = ImportTierInternal::Get()) {
			return tier->compileVariant(*this, {wanted.data(), wanted.size()}, hash);
		}
		SLT_LOG_WARN("Graphics", "shader '{}': no import tier installed to compile the variant (drawing the base variant)", virtualPath);
		return SHADER_BASE_VARIANT;
	}

	const UniformBlockLayout* ShaderAsset::GetMaterialParamLayout() const {
		for (const UniformBlockLayout& block : uniformBlocks) {
			if (block.set == MATERIAL_DESCRIPTOR_SET) {
				return &block;
			}
		}
		return nullptr;
	}

	void ShaderAsset::GetMaterialTextureSlots(Vector<str>& out) const {
		out.clear();
		for (const ShaderResourceInfo& resource : resources) {
			if (resource.set == MATERIAL_DESCRIPTOR_SET && resource.type == RHI::DescriptorType::SampledTexture) {
				out.push_back(resource.name);
			}
		}
	}

	RHI::TextureDimension ShaderAsset::GetTextureSlotDimension(std::string_view slotName) const {
		for (const ShaderResourceInfo& resource : resources) {
			if (resource.set == MATERIAL_DESCRIPTOR_SET && resource.type == RHI::DescriptorType::SampledTexture && resource.name == slotName) {
				return resource.dimension;
			}
		}
		return RHI::TextureDimension::Texture2D;
	}

	ShaderTextureFallback ShaderAsset::GetTextureFallback(std::string_view slotName) const {
		for (const ShaderTextureDefault& declared : textureDefaults) {
			if (declared.name == slotName) {
				return declared.fallback;
			}
		}
		for (const ShaderResourceInfo& resource : resources) {
			if (resource.type == RHI::DescriptorType::SampledTexture && resource.name == slotName) {
				return resource.dimension == RHI::TextureDimension::Texture2D ? ShaderTextureFallback::White : ShaderTextureFallback::Gray;
			}
		}
		return ShaderTextureFallback::White;
	}

	void ShaderAsset::AdoptImportedPayload(ShaderAsset& imported) {
		variants = std::move(imported.variants);
		keywords = std::move(imported.keywords);
		bindings = std::move(imported.bindings);
		pushConstantSize = imported.pushConstantSize;
		threadGroupSize = imported.threadGroupSize;
		bytecodeFormat = imported.bytecodeFormat;
		uniformBlocks = std::move(imported.uniformBlocks);
		resources = std::move(imported.resources);
		renderState = imported.renderState;
		integrator = imported.integrator;
		skipOptimizations = imported.skipOptimizations;
		depthProgram = imported.depthProgram;
		depthValue = imported.depthValue;
		memberDefaults = std::move(imported.memberDefaults);
		textureDefaults = std::move(imported.textureDefaults);
		memberRanges = std::move(imported.memberRanges);
		memberHints = std::move(imported.memberHints);
		memberGroups = std::move(imported.memberGroups);
		keywordBindings = std::move(imported.keywordBindings);
		memberLabels = std::move(imported.memberLabels);
		dependencies = std::move(imported.dependencies);
		RecountMeshReader();
		imported.RecountMeshReader();
	}

	const ShaderMemberRange* ShaderAsset::FindMemberRange(std::string_view memberName) const {
		for (const ShaderMemberRange& declared : memberRanges) {
			if (declared.name == memberName) {
				return &declared;
			}
		}
		return nullptr;
	}

	ShaderHint ShaderAsset::FindMemberHint(std::string_view memberName) const {
		for (const ShaderMemberHint& declared : memberHints) {
			if (declared.name == memberName) {
				return declared.hint;
			}
		}
		return ShaderHint::None;
	}

	std::string_view ShaderAsset::FindKeywordMember(std::string_view keyword) const {
		for (const ShaderKeywordBinding& declared : keywordBindings) {
			if (declared.keyword == keyword) {
				return std::string_view{declared.member};
			}
		}
		return {};
	}

	std::string_view ShaderAsset::FindMemberLabel(std::string_view memberName) const {
		for (const ShaderMemberLabel& declared : memberLabels) {
			if (declared.name == memberName) {
				return std::string_view{declared.label};
			}
		}
		return {};
	}

	bool ShaderAsset::Cook(CookWriter& descriptor, CookWriter& payload) const {
		descriptor.Write(static_cast<u32>(variants.size()));
		for (const ShaderVariant& variant : variants) {
			descriptor.Write(variant.keywordHash);
			descriptor.Write(static_cast<u32>(variant.keywords.size()));
			for (const str& keyword : variant.keywords) {
				descriptor.WriteString(keyword);
			}
			descriptor.Write(static_cast<u32>(variant.vertexSpirv.size()));
			descriptor.Write(static_cast<u32>(variant.pixelSpirv.size()));
			descriptor.Write(static_cast<u32>(variant.depthVertexSpirv.size()));
			descriptor.Write(static_cast<u32>(variant.depthPixelSpirv.size()));
			descriptor.Write(static_cast<u32>(variant.depthClipPixelSpirv.size()));
			descriptor.Write(static_cast<u32>(variant.depthValuePixelSpirv.size()));
			descriptor.Write(static_cast<u32>(variant.computeSpirv.size()));
		}
		descriptor.Write(static_cast<u32>(keywords.size()));
		for (const str& keyword : keywords) {
			descriptor.WriteString(keyword);
		}
		descriptor.Write(pushConstantSize);

		descriptor.Write(static_cast<u8>(renderState.blend));
		descriptor.Write(static_cast<u8>(renderState.depth));
		descriptor.Write(static_cast<u8>(renderState.cull));
		descriptor.Write(static_cast<u8>(renderState.topology));
		descriptor.Write(static_cast<u8>(renderState.fill));
		descriptor.Write(renderState.depthBiasConstant);
		descriptor.Write(renderState.depthBiasSlope);
		descriptor.Write(static_cast<u8>(renderState.depthCompare));

		descriptor.Write(static_cast<u32>(bindings.size()));
		for (const RHI::DescriptorBindingDesc& binding : bindings) {
			descriptor.Write(binding.set);
			descriptor.Write(binding.binding);
			descriptor.Write(static_cast<u8>(binding.type));
			u8 stageFlags = static_cast<u8>(binding.stages);
			if (binding.readOnly) {
				stageFlags |= 0x80u;
			}
			descriptor.Write(stageFlags);
		}

		descriptor.Write(static_cast<u32>(uniformBlocks.size()));
		for (const UniformBlockLayout& block : uniformBlocks) {
			descriptor.Write(block.set);
			descriptor.Write(block.binding);
			descriptor.Write(block.sizeBytes);
			descriptor.Write(static_cast<u32>(block.members.size()));
			for (const UniformMember& member : block.members) {
				descriptor.WriteString(member.name);
				descriptor.Write(static_cast<u8>(member.type));
				descriptor.Write(member.offset);
				descriptor.Write(member.sizeBytes);
			}
		}

		descriptor.Write(static_cast<u32>(resources.size()));
		for (const ShaderResourceInfo& resource : resources) {
			descriptor.Write(resource.set);
			descriptor.Write(resource.binding);
			descriptor.Write(static_cast<u8>(resource.type));
			descriptor.WriteString(resource.name);
			descriptor.Write(static_cast<u8>(resource.dimension));
		}

		descriptor.Write(static_cast<u32>(textureDefaults.size()));
		for (const ShaderTextureDefault& declared : textureDefaults) {
			descriptor.WriteString(declared.name);
			descriptor.Write(static_cast<u8>(declared.fallback));
		}

		descriptor.Write(threadGroupSize.x);
		descriptor.Write(threadGroupSize.y);
		descriptor.Write(threadGroupSize.z);

		descriptor.Write(static_cast<u8>(integrator));

		descriptor.Write(static_cast<u32>(memberDefaults.size()));
		for (const ShaderMemberDefault& declared : memberDefaults) {
			const u32 count = std::min<u32>(declared.count, 4);
			descriptor.WriteString(declared.name);
			descriptor.Write(count);
			for (u32 i = 0; i < count; ++i) {
				descriptor.Write(declared.numbers[i]);
			}
		}
		descriptor.Write(static_cast<u8>(bytecodeFormat));

		for (const ShaderVariant& variant : variants) {
			payload.WriteBytes(variant.vertexSpirv.data(), variant.vertexSpirv.size());
			payload.WriteBytes(variant.pixelSpirv.data(), variant.pixelSpirv.size());
			payload.WriteBytes(variant.depthVertexSpirv.data(), variant.depthVertexSpirv.size());
			payload.WriteBytes(variant.depthPixelSpirv.data(), variant.depthPixelSpirv.size());
			payload.WriteBytes(variant.depthClipPixelSpirv.data(), variant.depthClipPixelSpirv.size());
			payload.WriteBytes(variant.depthValuePixelSpirv.data(), variant.depthValuePixelSpirv.size());
			payload.WriteBytes(variant.computeSpirv.data(), variant.computeSpirv.size());
		}
		return true;
	}

	bool ShaderAsset::LoadCooked(CookReader& descriptor, CookReader& payload) {
		struct VariantRecord {
			u64 keywordHash = 0;
			Vector<str> keywords{GetMemoryResource(MemTag::Asset)};
			u32 vertexSize = 0;
			u32 pixelSize = 0;
			u32 depthVertexSize = 0;
			u32 depthPixelSize = 0;
			u32 depthClipPixelSize = 0;
			u32 depthValuePixelSize = 0;
			u32 computeSize = 0;
		};
		Vector<VariantRecord> variantRecords{GetMemoryResource(MemTag::Asset)};
		u32 variantCount = 0;
		descriptor.Read(variantCount);
		for (u32 i = 0; i < variantCount && !descriptor.Failed(); ++i) {
			VariantRecord record;
			u32 keywordCount = 0;
			descriptor.Read(record.keywordHash);
			descriptor.Read(keywordCount);
			for (u32 k = 0; k < keywordCount && !descriptor.Failed(); ++k) {
				record.keywords.push_back(str(descriptor.ReadString()));
			}
			descriptor.Read(record.vertexSize);
			descriptor.Read(record.pixelSize);
			descriptor.Read(record.depthVertexSize);
			descriptor.Read(record.depthPixelSize);
			descriptor.Read(record.depthClipPixelSize);
			descriptor.Read(record.depthValuePixelSize);
			descriptor.Read(record.computeSize);
			variantRecords.push_back(std::move(record));
		}

		Vector<str> readKeywords{GetMemoryResource(MemTag::Asset)};
		u32 declaredCount = 0;
		descriptor.Read(declaredCount);
		for (u32 i = 0; i < declaredCount && !descriptor.Failed(); ++i) {
			readKeywords.push_back(str(descriptor.ReadString()));
		}

		u32 readPushConstantSize = 0;
		descriptor.Read(readPushConstantSize);

		RHI::ShaderRenderState state;
		u8 blend = 0;
		u8 depth = 0;
		u8 cull = 0;
		u8 topology = 0;
		u8 fill = 0;
		descriptor.Read(blend);
		descriptor.Read(depth);
		descriptor.Read(cull);
		descriptor.Read(topology);
		descriptor.Read(fill);
		descriptor.Read(state.depthBiasConstant);
		descriptor.Read(state.depthBiasSlope);
		u8 depthCompare = 0;
		descriptor.Read(depthCompare);
		state.blend = static_cast<RHI::BlendMode>(blend);
		state.depth = static_cast<RHI::DepthMode>(depth);
		state.cull = static_cast<RHI::CullMode>(cull);
		state.topology = static_cast<RHI::PrimitiveTopology>(topology);
		state.fill = static_cast<RHI::FillMode>(fill);
		state.depthCompare = depthCompare < static_cast<u8>(RHI::DepthCompare::Count) ? static_cast<RHI::DepthCompare>(depthCompare) : RHI::DepthCompare::NearerOrEqual;

		Vector<RHI::DescriptorBindingDesc> readBindings{GetMemoryResource(MemTag::Asset)};
		u32 bindingCount = 0;
		descriptor.Read(bindingCount);
		for (u32 i = 0; i < bindingCount && !descriptor.Failed(); ++i) {
			RHI::DescriptorBindingDesc binding;
			u8 type = 0;
			u8 stages = 0;
			descriptor.Read(binding.set);
			descriptor.Read(binding.binding);
			descriptor.Read(type);
			descriptor.Read(stages);
			binding.type = static_cast<RHI::DescriptorType>(type);
			binding.readOnly = (stages & 0x80u) != 0u;
			binding.stages = static_cast<RHI::ShaderStage>(stages & 0x7Fu);
			readBindings.push_back(binding);
		}

		Vector<UniformBlockLayout> readBlocks{GetMemoryResource(MemTag::Asset)};
		u32 blockCount = 0;
		descriptor.Read(blockCount);
		for (u32 i = 0; i < blockCount && !descriptor.Failed(); ++i) {
			UniformBlockLayout block;
			u32 memberCount = 0;
			descriptor.Read(block.set);
			descriptor.Read(block.binding);
			descriptor.Read(block.sizeBytes);
			descriptor.Read(memberCount);
			for (u32 m = 0; m < memberCount && !descriptor.Failed(); ++m) {
				UniformMember member;
				u8 type = 0;
				member.name = str(descriptor.ReadString());
				descriptor.Read(type);
				descriptor.Read(member.offset);
				descriptor.Read(member.sizeBytes);
				member.type = static_cast<UniformMemberType>(type);
				block.members.push_back(std::move(member));
			}
			readBlocks.push_back(std::move(block));
		}

		Vector<ShaderResourceInfo> readResources{GetMemoryResource(MemTag::Asset)};
		u32 resourceCount = 0;
		descriptor.Read(resourceCount);
		for (u32 i = 0; i < resourceCount && !descriptor.Failed(); ++i) {
			ShaderResourceInfo resource;
			u8 type = 0;
			descriptor.Read(resource.set);
			descriptor.Read(resource.binding);
			descriptor.Read(type);
			resource.name = str(descriptor.ReadString());
			resource.type = static_cast<RHI::DescriptorType>(type);
			u8 dimension = 0;
			descriptor.Read(dimension);
			resource.dimension = dimension < static_cast<u8>(RHI::TextureDimension::Count) ? static_cast<RHI::TextureDimension>(dimension)
																						   : RHI::TextureDimension::Texture2D;
			readResources.push_back(std::move(resource));
		}

		Vector<ShaderTextureDefault> readTextureDefaults{GetMemoryResource(MemTag::Asset)};
		u32 textureDefaultCount = 0;
		descriptor.Read(textureDefaultCount);
		for (u32 i = 0; i < textureDefaultCount && !descriptor.Failed(); ++i) {
			ShaderTextureDefault declared;
			declared.name = str(descriptor.ReadString());
			u8 fallback = 0;
			descriptor.Read(fallback);
			declared.fallback = fallback <= static_cast<u8>(ShaderTextureFallback::Gray)
									? static_cast<ShaderTextureFallback>(fallback)
									: ShaderTextureFallback::White;
			readTextureDefaults.push_back(std::move(declared));
		}

		Vec3I readThreadGroupSize{};
		descriptor.Read(readThreadGroupSize.x);
		descriptor.Read(readThreadGroupSize.y);
		descriptor.Read(readThreadGroupSize.z);

		u8 readIntegrator = 0;
		descriptor.Read(readIntegrator);

		Vector<ShaderMemberDefault> readDefaults{GetMemoryResource(MemTag::Asset)};
		u32 defaultCount = 0;
		descriptor.Read(defaultCount);
		for (u32 i = 0; i < defaultCount && !descriptor.Failed(); ++i) {
			ShaderMemberDefault declared;
			declared.name = str(descriptor.ReadString());
			descriptor.Read(declared.count);
			for (u32 v = 0; v < declared.count && !descriptor.Failed(); ++v) {
				f32 value = 0.0f;
				descriptor.Read(value);
				if (v < 4) {
					declared.numbers[v] = value;
				}
			}
			declared.count = std::min<u32>(declared.count, 4);
			readDefaults.push_back(std::move(declared));
		}

		u8 readBytecodeFormat = 0;
		descriptor.Read(readBytecodeFormat);

		Vector<ShaderVariant> readVariants{GetMemoryResource(MemTag::Asset)};
		for (VariantRecord& record : variantRecords) {
			if (payload.Failed()) {
				break;
			}
			ShaderVariant variant;
			variant.keywordHash = record.keywordHash;
			variant.keywords = std::move(record.keywords);
			payload.ReadInto(variant.vertexSpirv, record.vertexSize);
			payload.ReadInto(variant.pixelSpirv, record.pixelSize);
			payload.ReadInto(variant.depthVertexSpirv, record.depthVertexSize);
			payload.ReadInto(variant.depthPixelSpirv, record.depthPixelSize);
			payload.ReadInto(variant.depthClipPixelSpirv, record.depthClipPixelSize);
			payload.ReadInto(variant.depthValuePixelSpirv, record.depthValuePixelSize);
			payload.ReadInto(variant.computeSpirv, record.computeSize);
			readVariants.push_back(std::move(variant));
		}
		if (descriptor.Failed() || payload.Failed()) {
			return false;
		}

		pushConstantSize = readPushConstantSize;
		threadGroupSize = readThreadGroupSize;
		bytecodeFormat = readBytecodeFormat == static_cast<u8>(RHI::ShaderBytecodeFormat::WGSL)
							 ? RHI::ShaderBytecodeFormat::WGSL
							 : RHI::ShaderBytecodeFormat::Spirv;
		integrator = readIntegrator <= static_cast<u8>(ShaderTemplate::Sky) ? static_cast<ShaderTemplate>(readIntegrator)
																			: ShaderTemplate::None;
		memberDefaults = std::move(readDefaults);
		renderState = state;
		bindings = std::move(readBindings);
		uniformBlocks = std::move(readBlocks);
		resources = std::move(readResources);
		textureDefaults = std::move(readTextureDefaults);
		variants = std::move(readVariants);
		keywords = std::move(readKeywords);
		RecountMeshReader();
		return true;
	}

	bool ShaderAsset::CookImportState(CookWriter& importState) const {
		importState.Write(static_cast<u32>(dependencies.size()));
		for (const Dependency& dependency : dependencies) {
			importState.WriteString(dependency.physicalPath);
			importState.Write(dependency.contentHash);
		}
		importState.Write(static_cast<u32>(memberRanges.size()));
		for (const ShaderMemberRange& declared : memberRanges) {
			importState.WriteString(declared.name);
			importState.Write(declared.minValue);
			importState.Write(declared.maxValue);
		}
		importState.Write(static_cast<u32>(memberHints.size()));
		for (const ShaderMemberHint& declared : memberHints) {
			importState.WriteString(declared.name);
			importState.Write(static_cast<u8>(declared.hint));
		}
		importState.Write(static_cast<u32>(memberGroups.size()));
		for (const ShaderMemberGroup& declared : memberGroups) {
			importState.WriteString(declared.name);
			importState.Write(static_cast<u32>(declared.members.size()));
			for (const str& member : declared.members) {
				importState.WriteString(member);
			}
		}
		importState.Write(static_cast<u32>(keywordBindings.size()));
		for (const ShaderKeywordBinding& declared : keywordBindings) {
			importState.WriteString(declared.keyword);
			importState.WriteString(declared.member);
			importState.Write(declared.value);
			importState.WriteString(declared.entry);
		}
		importState.Write(static_cast<u32>(memberLabels.size()));
		for (const ShaderMemberLabel& declared : memberLabels) {
			importState.WriteString(declared.name);
			importState.WriteString(declared.label);
		}
		importState.Write(static_cast<u8>(skipOptimizations ? 1 : 0));
		importState.Write(static_cast<u8>(depthProgram));
		importState.Write(static_cast<u8>(depthValue ? 1 : 0));
		return true;
	}

	bool ShaderAsset::LoadImportState(CookReader& importState) {
		u32 count = 0;
		importState.Read(count);

		Vector<Dependency> restored{GetMemoryResource(MemTag::Asset)};
		for (u32 i = 0; i < count && !importState.Failed(); ++i) {
			Dependency dependency;
			dependency.physicalPath = str(importState.ReadString());
			importState.Read(dependency.contentHash);
			restored.push_back(std::move(dependency));
		}

		Vector<ShaderMemberRange> restoredRanges{GetMemoryResource(MemTag::Asset)};
		u32 rangeCount = 0;
		importState.Read(rangeCount);
		for (u32 i = 0; i < rangeCount && !importState.Failed(); ++i) {
			ShaderMemberRange declared;
			declared.name = str(importState.ReadString());
			importState.Read(declared.minValue);
			importState.Read(declared.maxValue);
			restoredRanges.push_back(std::move(declared));
		}

		Vector<ShaderMemberHint> restoredHints{GetMemoryResource(MemTag::Asset)};
		u32 hintCount = 0;
		importState.Read(hintCount);
		for (u32 i = 0; i < hintCount && !importState.Failed(); ++i) {
			ShaderMemberHint declared;
			declared.name = str(importState.ReadString());
			u8 hint = 0;
			importState.Read(hint);
			declared.hint = hint <= static_cast<u8>(ShaderHint::Hidden) ? static_cast<ShaderHint>(hint) : ShaderHint::None;
			restoredHints.push_back(std::move(declared));
		}

		Vector<ShaderMemberGroup> restoredGroups{GetMemoryResource(MemTag::Asset)};
		u32 groupCount = 0;
		importState.Read(groupCount);
		for (u32 i = 0; i < groupCount && !importState.Failed(); ++i) {
			ShaderMemberGroup declared;
			declared.name = str(importState.ReadString());
			u32 memberCount = 0;
			importState.Read(memberCount);
			for (u32 m = 0; m < memberCount && !importState.Failed(); ++m) {
				declared.members.push_back(str(importState.ReadString()));
			}
			restoredGroups.push_back(std::move(declared));
		}

		Vector<ShaderKeywordBinding> restoredBindings{GetMemoryResource(MemTag::Asset)};
		u32 bindingCount = 0;
		importState.Read(bindingCount);
		for (u32 i = 0; i < bindingCount && !importState.Failed(); ++i) {
			ShaderKeywordBinding declared;
			declared.keyword = str(importState.ReadString());
			declared.member = str(importState.ReadString());
			importState.Read(declared.value);
			declared.entry = str(importState.ReadString());
			restoredBindings.push_back(std::move(declared));
		}

		Vector<ShaderMemberLabel> restoredLabels{GetMemoryResource(MemTag::Asset)};
		u32 labelCount = 0;
		importState.Read(labelCount);
		for (u32 i = 0; i < labelCount && !importState.Failed(); ++i) {
			ShaderMemberLabel declared;
			declared.name = str(importState.ReadString());
			declared.label = str(importState.ReadString());
			restoredLabels.push_back(std::move(declared));
		}
		u8 restoredSkip = 0;
		importState.Read(restoredSkip);
		u8 restoredDepth = 0;
		importState.Read(restoredDepth);
		u8 restoredValue = 0;
		importState.Read(restoredValue);

		if (importState.Failed()) {
			return false;
		}
		dependencies = std::move(restored);
		memberRanges = std::move(restoredRanges);
		memberHints = std::move(restoredHints);
		memberGroups = std::move(restoredGroups);
		keywordBindings = std::move(restoredBindings);
		memberLabels = std::move(restoredLabels);
		skipOptimizations = restoredSkip != 0;
		depthProgram = restoredDepth <= static_cast<u8>(ShaderDepthProgram::Clipping) ? static_cast<ShaderDepthProgram>(restoredDepth)
																					  : ShaderDepthProgram::None;
		depthValue = depthProgram != ShaderDepthProgram::None && restoredValue != 0;
		return true;
	}

	Asset* ShaderAssetImporter::Import(const Path& sourcePath) {
		ShaderAsset* asset = New<ShaderAsset>(MemTag::Asset);
		if (!asset->Import(sourcePath)) {
			Delete(asset);
			return nullptr;
		}
		return asset;
	}

	namespace {

		constexpr std::string_view SurfaceShaderTemplate = R"(// new Shalltear surface shader

#pragma shalltear_template lit
#pragma shalltear_blend opaque
#pragma shalltear_depth test_write
#pragma shalltear_cull back

#pragma shalltear_default baseColor 1 1 1 1
#pragma shalltear_default metallic 0
#pragma shalltear_default roughness 0.5

[[vk::binding(0, 1)]] cbuffer ShalltearMaterial {
	float4 baseColor;
	float metallic;
	float roughness;
};
[[vk::binding(1, 1)]] Texture2D baseColorTexture;
[[vk::binding(2, 1)]] SamplerState baseColorSampler;

void ShalltearSurfaceFunction(ShalltearSurfaceInput input, inout ShalltearSurface surface) {
	float4 base = baseColorTexture.Sample(baseColorSampler, input.uv) * baseColor * input.color;
	surface.baseColor = base.rgb;
	surface.alpha = base.a;
	surface.metallic = metallic;
	surface.roughness = roughness;

	// rest is yours. `surface` also has normal/occlusion/
	// emissive/reflectance, `input` has the world position/geometric
	// normal/tangent basis/UV/view direction
}
)";

	} // anonymous namespace

	bool ShaderAssetImporter::CreateAssetSource(Buffer& out) {
		out.Allocate(SurfaceShaderTemplate.size(), MemTag::Asset);
		if (!out) {
			return false;
		}
		std::memcpy(out.data, SurfaceShaderTemplate.data(), SurfaceShaderTemplate.size());
		return true;
	}

} // namespace Shalltear

SLT_REGISTER_TYPE(Shalltear::ShaderAsset)
