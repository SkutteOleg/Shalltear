#include "ProceduralBindings.h"

#include "Shalltear/Core/Asset/Asset.h"
#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Object.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"

#include <format>
#include <stdexcept>

namespace Shalltear::CSharpImpl::ProceduralBindings {

	u64 CreateTransient(u64 typeHash, std::string_view name) {
		const TypeInfo* typeInfo = TypeRegistry::FindType(StringHash(typeHash));
		if (!typeInfo) {
			throw std::runtime_error("Asset.CreateTransient got an unknown asset type");
		}
		if (typeInfo->flags & TypeFlagAbstract) {
			throw std::runtime_error(std::format("Asset.CreateTransient can't create {} (abstract)", typeInfo->displayName));
		}
		if (!TypeRegistry::IsDerivedFrom(typeInfo, Asset::GetReflectType())) {
			throw std::runtime_error(std::format("Asset.CreateTransient got {} (not an Asset)", typeInfo->displayName));
		}

		Object* object = TypeRegistry::CreateObjectInstance(typeInfo, MemTag::Asset);
		if (!object) {
			throw std::runtime_error(std::format("Asset.CreateTransient couldn't create {}", typeInfo->displayName));
		}

		auto* asset = static_cast<Asset*>(object);
		if (!name.empty()) {
			asset->virtualPath = str(name);
		}
		asset->AddRef();
		asset->SetLoadState(AssetLoadState::Loaded);
		return asset->GetInstanceId();
	}

	void DestroyTransient(u64 id) {
		if (id == 0) {
			return;
		}
		Object* object = ObjectRegistry::Resolve(id);
		if (!object) {
			return;
		}
		if (!TypeRegistry::IsDerivedFrom(object->GetType(), Asset::GetReflectType())) {
			throw std::runtime_error("Asset.DestroyTransient got an object that isn't an Asset");
		}

		auto* asset = static_cast<Asset*>(object);
		if (asset->assetId.IsValid() && AssetDatabase::FindLoadedByGuid(asset->assetId) == asset) {
			throw std::runtime_error("Asset.DestroyTransient got an asset owned by AssetDatabase. Use AssetDatabase.Unload");
		}
		asset->NotifyUnloaded();
		TypeRegistry::DestroyObjectInstance(asset);
	}

} // namespace Shalltear::CSharpImpl::ProceduralBindings
