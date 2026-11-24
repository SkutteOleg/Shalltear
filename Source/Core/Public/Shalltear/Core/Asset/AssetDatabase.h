#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/CoreAPI.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/StringHash.h"
#include "Shalltear/Core/Types.h"

#include <span>

namespace Shalltear {

	class SHALLTEAR_CORE_API Asset;

	class SHALLTEAR_CORE_API JSONValue;

	enum class PakDocumentKind : u32;

	struct AssetProduct {
		str key;
		StringHash type;
		Guid guid;
		Path extractedPath;
		str name;
	};

	class SHALLTEAR_CORE_API AssetDatabase {
	public:
		static void Initialize();

		static void Terminate();

		static bool MountPackage(const Path& physicalPath, str& outError);

		static bool ReadPackageDocument(PakDocumentKind kind, str& out);

		static bool ReadManagedBundle(void (*onAssembly)(const char* name, const u8* data, u32 size, void* user),
									  void* user);

		static Asset* LoadAsset(const Path& virtualPath);

		static Asset* LoadAssetByGuid(const Guid& assetId);

		// weak lookup, no import, no AddRef
		static Asset* FindLoadedByGuid(const Guid& assetId);

		static Guid EnsureAssetIdentity(const Path& virtualPath);

		// virtualPath must be set beforehand
		static void SaveAsset(Asset* asset);

		static bool WriteImporterSettings(const Path& virtualPath, const JSONValue& settings);

		static bool ReimportAsset(const Path& virtualPath);

		static Guid CreateAsset(const Path& virtualPath, StringHash typeId, u32 templateIndex = 0);

		static void UnloadAsset(Asset* asset);

		static u32 CollectGarbage();

		static Guid GenerateGuid();

		static Path GetMetaPath(const Path& assetPath);

		static void ScanProjectDirectory();

		static void RebuildDatabase();

		static bool IsAssetPath(const Path& path);

		static Guid LookupGuid(const Path& path);
		static Path LookupPath(const Guid& assetId);

		// sorted, from project index/mounted package
		static void FindAssets(StringHash typeId, Vector<Guid>& out);

		static void GetProducts(const Guid& sourceGuid, Vector<AssetProduct>& out);

		static Guid GetProductSource(const Guid& productGuid);

		static bool ExtractProduct(const Guid& productGuid, const Path& virtualPath);

		static std::span<const Guid> GetEngineBuiltinRoots();

		static bool ValidatePackage(const Path& physicalPath, std::span<const Guid> expected, str& outError);
	};

	class SHALLTEAR_CORE_API AssetLoadRecorder {
	public:
		// sink must outlive the recorder
		explicit AssetLoadRecorder(Vector<Guid>& sink) noexcept;
		~AssetLoadRecorder();

		AssetLoadRecorder(const AssetLoadRecorder&) = delete;
		AssetLoadRecorder& operator=(const AssetLoadRecorder&) = delete;
	private:
		bool _installed = false;
	};

} // namespace Shalltear
