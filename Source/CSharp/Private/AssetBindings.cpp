#include "AssetBindings.h"

#include "BindingSupport.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Asset/AssetImporter.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"

#include <format>
#include <stdexcept>

namespace Shalltear::CSharpImpl::AssetBindings {

	namespace {

		str _text;

		Asset& Resolve(u64 id) {
			Object* object = ObjectRegistry::Resolve(id);
			if (!object || !TypeRegistry::IsDerivedFrom(object->GetType(), Asset::GetReflectType())) {
				throw std::runtime_error("Asset is gone (unloaded)");
			}
			return *static_cast<Asset*>(object);
		}

		bool IsFile(std::string_view path) {
			return path.find("://") != std::string_view::npos && FileSystem::Exists(FileSystem::Resolve(Path(str(path))));
		}

	} // anonymous namespace

	u64 Load(std::string_view path) {
		return IsFile(path) ? IdOf(AssetDatabase::LoadAsset(Path(str(path)))) : 0;
	}

	void Unload(u64 id) {
		AssetDatabase::UnloadAsset(&Resolve(id));
	}

	void Save(u64 id) {
		Asset& asset = Resolve(id);
		if (asset.virtualPath.empty()) {
			throw std::runtime_error("AssetDatabase.Save got an asset with no file. Make it with AssetDatabase.Create");
		}
		AssetDatabase::SaveAsset(&asset);
	}

	u64 Create(std::string_view path, u64 typeHash) {
		const Path file{str(path)};
		IAssetImporter* importer = AssetImporterRegistry::Find(StringHash(typeHash));
		if (!importer || !importer->CanCreateAsset()) {
			throw std::runtime_error("AssetDatabase.Create got a type with no Create menu entry. Give it one (or load an existing file)");
		}
		if (AssetImporterRegistry::FindByExtension(file.GetExtension()) != importer) {
			throw std::runtime_error(std::format("AssetDatabase.Create got '{}', a path with another type's extension. Use the type's extension", path));
		}
		if (IsFile(path)) {
			throw std::runtime_error(std::format("AssetDatabase.Create got '{}', an existing file. Load it (or pick another path)", path));
		}
		const Guid created = AssetDatabase::CreateAsset(file, StringHash(typeHash));
		return created.IsValid() ? IdOf(AssetDatabase::LoadAssetByGuid(created)) : 0;
	}

	std::string_view LookupPath(std::string_view assetId) {
		_text = AssetDatabase::LookupPath(Guid::FromString(assetId)).ToString();
		return _text;
	}

	std::string_view LookupAssetId(std::string_view path) {
		const Guid found = AssetDatabase::LookupGuid(Path(str(path)));
		_text = found.IsValid() ? found.ToString() : str();
		return _text;
	}

	std::string_view GetPath(u64 id) {
		return Resolve(id).virtualPath;
	}

	std::string_view GetAssetId(u64 id) {
		const Asset& asset = Resolve(id);
		_text = asset.assetId.IsValid() ? asset.assetId.ToString() : str();
		return _text;
	}

	AssetLoadState GetLoadState(u64 id) {
		return Resolve(id).GetLoadState();
	}

	i32 GetRefCount(u64 id) {
		return Resolve(id).GetRefCount();
	}

} // namespace Shalltear::CSharpImpl::AssetBindings
