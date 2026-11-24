#pragma once

#include "Shalltear/Core/Asset/Asset.h"
#include "Shalltear/Core/Asset/AssetImporter.h"
#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/CoreAPI.h"
#include "Shalltear/Core/Graphics/RHIDevice.h"
#include "Shalltear/Core/Graphics/RHITypes.h"
#include "Shalltear/Core/Graphics/ShaderReflection.h"

#include <span>
#include <string_view>

namespace Shalltear {

	inline constexpr u32 MATERIAL_DESCRIPTOR_SET = 1;

	inline constexpr u32 MAX_MATERIAL_SET_RESOURCES = RHI::TransientSetWriter::MAX_WRITES - 1;

	enum class ShaderTemplate : u8 {
		None = 0,
		Lit,
		Unlit,
		Sky
	};

	enum class ShaderProgramKind : u8 {
		Forward = 0,
		Depth,
		Compute
	};

	enum class ShaderDepthProgram : u8 {
		None = 0,
		Declared,
		Clipping
	};

	inline constexpr std::string_view ShaderAlphaClipKeyword = "SHALLTEAR_ALPHA_CLIP";

	struct ShaderPragmas {
		RHI::ShaderRenderState renderState;
		ShaderTemplate integrator = ShaderTemplate::None;
		Vector<ShaderMemberDefault> memberDefaults{GetMemoryResource(MemTag::Asset)};
		Vector<ShaderTextureDefault> textureDefaults{GetMemoryResource(MemTag::Asset)};
		Vector<str> keywords{GetMemoryResource(MemTag::Asset)};

		Vector<ShaderMemberRange> memberRanges{GetMemoryResource(MemTag::Asset)};
		Vector<ShaderMemberHint> memberHints{GetMemoryResource(MemTag::Asset)};
		Vector<ShaderMemberGroup> memberGroups{GetMemoryResource(MemTag::Asset)};
		Vector<ShaderKeywordBinding> keywordBindings{GetMemoryResource(MemTag::Asset)};
		Vector<ShaderMemberLabel> memberLabels{GetMemoryResource(MemTag::Asset)};
		bool skipOptimizations = false;
		bool kernel = false;
		ShaderDepthProgram depthProgram = ShaderDepthProgram::None;
		bool depthValue = false;
	};

	struct ShaderVariant {
		u64 keywordHash = 0;
		Vector<str> keywords{GetMemoryResource(MemTag::Asset)};
		Vector<u8> vertexSpirv{GetMemoryResource(MemTag::Asset)};
		Vector<u8> pixelSpirv{GetMemoryResource(MemTag::Asset)};
		Vector<u8> depthVertexSpirv{GetMemoryResource(MemTag::Asset)};
		Vector<u8> depthPixelSpirv{GetMemoryResource(MemTag::Asset)};
		Vector<u8> depthClipPixelSpirv{GetMemoryResource(MemTag::Asset)};
		Vector<u8> depthValuePixelSpirv{GetMemoryResource(MemTag::Asset)};
		Vector<u8> computeSpirv{GetMemoryResource(MemTag::Asset)};

		bool HasProgram(ShaderProgramKind kind) const {
			switch (kind) {
			case ShaderProgramKind::Depth:
				return !depthVertexSpirv.empty();
			case ShaderProgramKind::Compute:
				return !computeSpirv.empty();
			case ShaderProgramKind::Forward:
				break;
			}
			return !vertexSpirv.empty();
		}
	};

	inline constexpr u32 SHADER_BASE_VARIANT = 0;

	class SHALLTEAR_CORE_API ShaderAsset : public Asset {
	public:
		SLT_REFLECT_CLASS(ShaderAsset)
		SLT_REFLECT_PARENT(Asset)
		SLT_REFLECT_CREATABLE()
		SLT_END_REFLECT()

		ShaderAsset() = default;
		~ShaderAsset() override;

		Vector<ShaderVariant> variants{GetMemoryResource(MemTag::Asset)};
		Vector<str> keywords{GetMemoryResource(MemTag::Asset)};
		Vector<RHI::DescriptorBindingDesc> bindings{GetMemoryResource(MemTag::Asset)};
		u32 pushConstantSize = 0;
		Vec3I threadGroupSize{};
		RHI::ShaderBytecodeFormat bytecodeFormat = RHI::ShaderBytecodeFormat::Spirv;

		bool IsKernel() const { return !variants.empty() && variants[SHADER_BASE_VARIANT].HasProgram(ShaderProgramKind::Compute); }

		const ShaderVariant* GetVariant(u32 index) const;
		// pure lookup, must never compile
		u32 FindVariant(u64 keywordHash) const;
		u32 GetOrCompileVariant(std::span<const str> enabled);
		static u64 KeywordSetHash(std::span<const str> enabled);
		static RHI::ShaderBytecodeFormat GetDefaultImportBytecodeFormat();
		static void SetDefaultImportBytecodeFormat(RHI::ShaderBytecodeFormat format);

		Vector<UniformBlockLayout> uniformBlocks{GetMemoryResource(MemTag::Asset)};
		Vector<ShaderResourceInfo> resources{GetMemoryResource(MemTag::Asset)};

		const UniformBlockLayout* GetMaterialParamLayout() const;
		void GetMaterialTextureSlots(Vector<str>& out) const;
		RHI::TextureDimension GetTextureSlotDimension(std::string_view slotName) const;

		RHI::ShaderRenderState renderState;

		Vector<ShaderMemberDefault> memberDefaults{GetMemoryResource(MemTag::Asset)};

		Vector<ShaderTextureDefault> textureDefaults{GetMemoryResource(MemTag::Asset)};

		Vector<ShaderMemberRange> memberRanges{GetMemoryResource(MemTag::Asset)};
		Vector<ShaderMemberHint> memberHints{GetMemoryResource(MemTag::Asset)};
		Vector<ShaderMemberGroup> memberGroups{GetMemoryResource(MemTag::Asset)};
		Vector<ShaderKeywordBinding> keywordBindings{GetMemoryResource(MemTag::Asset)};
		Vector<ShaderMemberLabel> memberLabels{GetMemoryResource(MemTag::Asset)};

		const ShaderMemberRange* FindMemberRange(std::string_view memberName) const;
		ShaderHint FindMemberHint(std::string_view memberName) const;
		std::string_view FindKeywordMember(std::string_view keyword) const;
		std::string_view FindMemberLabel(std::string_view memberName) const;

		ShaderTemplate integrator = ShaderTemplate::None;
		bool skipOptimizations = false;
		ShaderDepthProgram depthProgram = ShaderDepthProgram::None;
		bool depthValue = false;

		ShaderTextureFallback GetTextureFallback(std::string_view slotName) const;

		struct Dependency {
			str physicalPath;
			u64 contentHash = 0;
		};
		Vector<Dependency> dependencies{GetMemoryResource(MemTag::Asset)};

		u32 version = 0;

		void AdoptImportedPayload(ShaderAsset& imported);

		bool AdoptVariant(ShaderAsset& compiled, u64 keywordHash);

		bool Import(const Path& virtualOrPhysical) override;
		StringHash GetAssetTypeId() const override { return ShaderAsset::GetReflectType()->name; }

		bool Cook(CookWriter& descriptor, CookWriter& payload) const override;
		bool LoadCooked(CookReader& descriptor, CookReader& payload) override;

		bool CookImportState(CookWriter& importState) const override;
		bool LoadImportState(CookReader& importState) override;

		static void ParsePragmas(std::string_view source, const Path& sourcePath, ShaderPragmas& out);
	private:
		bool ImportFromSource(const Path& virtualOrPhysical);
		u32 CompileVariant(std::span<const str> wanted, u64 keywordHash);
		void RecountMeshReader();
		friend struct ImportTierBuilder;

		bool _meshReader = false;
	};

	class SHALLTEAR_CORE_API ShaderAssetImporter : public IAssetImporter {
	public:
		StringHash GetAssetType() const override { return StringHash("ShaderAsset"); }
		Asset* Import(const Path& sourcePath) override;

		// bump when the cooked descriptor or import state changes
		u32 GetImporterVersion() const override { return 20; }

		bool CanCreateAsset() const override { return true; }
		std::string_view GetCreateMenuLabel() const override { return "Shader"; }
		std::string_view GetCreateDefaultFileName() const override { return "New Shader.hlsl"; }
		bool CreateAssetSource(Buffer& out) override;
	};

} // namespace Shalltear
