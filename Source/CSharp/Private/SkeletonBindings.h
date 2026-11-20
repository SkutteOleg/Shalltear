#pragma once

#include "ScriptBridge.h"

#include "Shalltear/Core/Graphics/Skeleton.h"
#include "Shalltear/Core/Graphics/SkeletonAsset.h"
#include "Shalltear/Core/Math/Quat.h"
#include "Shalltear/Core/Math/Vector.h"
#include "Shalltear/Core/Types.h"

#include <span>
#include <string_view>

// refusals thrown as std::runtime_error
namespace Shalltear::CSharpImpl::SkeletonBindings {

	std::string_view GetBoneName(const SkeletonAsset& skeleton, i32 index);
	i32 GetBoneParent(const SkeletonAsset& skeleton, i32 index);

	// returned counts may exceed pose.size()
	i32 ReadLocalPose(Skeleton& skeleton, std::span<SltBoneTransform> pose);
	void WriteBoneLocalPose(Skeleton& skeleton, i32 bone, const Vec3& position, const Quat& rotation, const Vec3& scale);
	i32 ReadRestPose(const SkeletonAsset& skeleton, std::span<SltBoneTransform> pose) noexcept;

} // namespace Shalltear::CSharpImpl::SkeletonBindings
