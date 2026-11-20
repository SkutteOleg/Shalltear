#pragma once

#include "Shalltear/Core/Graphics/Skeleton.h"
#include "Shalltear/Core/Graphics/SkeletonAsset.h"
#include "Shalltear/Core/Types.h"

#include <string_view>

// refusals thrown as std::runtime_error
namespace Shalltear::CSharpImpl::SkeletonBindings {

	std::string_view GetBoneName(const SkeletonAsset& skeleton, i32 index);
	i32 GetBoneParent(const SkeletonAsset& skeleton, i32 index);

} // namespace Shalltear::CSharpImpl::SkeletonBindings
