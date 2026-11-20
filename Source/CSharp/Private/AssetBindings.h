#pragma once

#include "Shalltear/Core/Asset/Asset.h"
#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Types.h"

#include <string_view>

// refusals thrown as std::runtime_error
namespace Shalltear::CSharpImpl::AssetBindings {

	u64 Load(std::string_view path);
	void Unload(u64 id);
	void Save(u64 id);
	u64 Create(std::string_view path, u64 typeHash);
	std::string_view LookupPath(std::string_view assetId);
	std::string_view LookupAssetId(std::string_view path);

	std::string_view GetPath(u64 id);
	std::string_view GetAssetId(u64 id);
	AssetLoadState GetLoadState(u64 id);
	i32 GetRefCount(u64 id);

} // namespace Shalltear::CSharpImpl::AssetBindings
