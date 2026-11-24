#pragma once

#include "Shalltear/Core/Asset/PakFormat.h"
#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Graphics/RHITypes.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/Editor/EditorAPI.h"

namespace Shalltear {

	class Asset;

	namespace CookOps {

		struct PakAssetSource {
			Guid guid;
			u64 typeNameHash = 0;
			u16 recordVersion = 1;
			u64 layoutHash = 0;
			u32 typeDescStride = 0;
			str name;
			Vector<u8> descriptor{GetMemoryResource(MemTag::Editor)};
			Vector<u8> payload{GetMemoryResource(MemTag::Editor)};
			u32 payloadAlignment = 16;
			PakResidency residency = PakResidency::Hot;
			u16 flags = 0;
			Vector<Guid> dependencies{GetMemoryResource(MemTag::Editor)};
		};

		enum class CookCompression : u8 {
			None = 0,
			LZ4 = 1,
			LZ4HC = 2
		};

		struct PakDocumentSource {
			PakDocumentKind kind = PakDocumentKind::BootManifest;
			str text;
		};

		struct PakWriteDesc {
			u32 targetId = 0;
			u32 localeId = 0;
			bool shipDebugNames = true;
			u32 pageSize = PAK_DEFAULT_PAGE_SIZE;
			CookCompression compression = CookCompression::None;
			RHI::ShaderBytecodeFormat shaderFormat = RHI::ShaderBytecodeFormat::Spirv;
			Vector<str> strings{GetMemoryResource(MemTag::Editor)};

			Vector<PakDocumentSource> documents{GetMemoryResource(MemTag::Editor)};
		};

		struct CookReportAsset {
			Guid guid;
			str name;
			str typeName;

			u32 descriptorBytes = 0;
			u64 payloadRaw = 0;
			u64 payloadStored = 0;
			u32 pageCount = 0;
			u8 codec = 0;

			bool isRoot = false;

			Vector<u32> dependsOn{GetMemoryResource(MemTag::Editor)};
			Vector<u32> pulledInBy{GetMemoryResource(MemTag::Editor)};

			Vector<u32> rootPath{GetMemoryResource(MemTag::Editor)};
		};

		struct CookReport {
			Vector<CookReportAsset> assets{GetMemoryResource(MemTag::Editor)};

			u64 fileBytes = 0;
			u64 tocBytes = 0;
			u64 payloadBytes = 0;
			u32 regionCount = 0;
			u32 pageCount = 0;

			u32 candidateCount = 0;

			u8 compression = 0;
			bool debugNamesShipped = true;
			str profileName;
			str packagePath;
		};

		SHALLTEAR_EDITOR_API str WriteReportJSON(const CookReport& report);
		SHALLTEAR_EDITOR_API bool ReadReportJSON(std::string_view json, CookReport& out, str& outError);

		SHALLTEAR_EDITOR_API bool WritePackage(const Path& outputPath, const Vector<PakAssetSource>& assets,
											   const PakWriteDesc& desc, str& outError);

		SHALLTEAR_EDITOR_API bool BuildPackage(const Vector<PakAssetSource>& assets, const PakWriteDesc& desc,
											   Vector<u8>& out, str& outError, CookReport* outReport = nullptr);

		SHALLTEAR_EDITOR_API bool CookAsset(const Asset& asset, PakStringPool& strings, PakAssetSource& out,
											str& outError);

		struct CookClosure {
			Vector<Guid> assets{GetMemoryResource(MemTag::Editor)};
			Vector<Vector<Guid>> dependencies{GetMemoryResource(MemTag::Editor)};

			Vector<Guid> roots{GetMemoryResource(MemTag::Editor)};

			struct ShaderVariantRequest {
				Guid shader;
				Vector<str> keywords{GetMemoryResource(MemTag::Editor)};
			};
			Vector<ShaderVariantRequest> variantRequests{GetMemoryResource(MemTag::Editor)};

			static SHALLTEAR_EDITOR_API CookClosure Of(const Vector<Guid>& assets);
		};

		SHALLTEAR_EDITOR_API bool ComputeClosure(const Vector<Guid>& sceneRoots, CookClosure& out, str& outError);

		// assets of IsAlwaysCooked importers, appended unless listed or failing import
		SHALLTEAR_EDITOR_API u32 CollectAlwaysCookedRoots(Vector<Guid>& out, Vector<str>* failedImports = nullptr);

		SHALLTEAR_EDITOR_API bool CookPackage(const CookClosure& closure, const PakWriteDesc& desc, Vector<u8>& out,
											  str& outError, CookReport* outReport = nullptr);

	} // namespace CookOps

} // namespace Shalltear
