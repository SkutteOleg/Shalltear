#include "SkeletonBindings.h"

#include "BindingSupport.h"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace Shalltear::CSharpImpl::SkeletonBindings {

	namespace {

		size_t Bone(const SkeletonAsset& skeleton, i32 index) {
			if (index < 0 || static_cast<u32>(index) >= skeleton.GetBoneCount()) {
				throw std::runtime_error(std::format("SkeletonAsset has no bone {} ({} bone(s))", index, skeleton.GetBoneCount()));
			}
			return static_cast<size_t>(index);
		}

		i32 Copy(std::span<const BoneTransform> from, std::span<SltBoneTransform> to) noexcept {
			const size_t count = std::min(from.size(), to.size());
			for (size_t index = 0; index < count; ++index) {
				to[index] = SltBoneTransform{from[index].position, FromEngine(from[index].rotation), from[index].scale};
			}
			return static_cast<i32>(from.size());
		}

	} // anonymous namespace

	std::string_view GetBoneName(const SkeletonAsset& skeleton, i32 index) {
		const size_t bone = Bone(skeleton, index);
		return bone < skeleton.boneNames.size() ? std::string_view(skeleton.boneNames[bone]) : std::string_view{};
	}

	i32 GetBoneParent(const SkeletonAsset& skeleton, i32 index) {
		return skeleton.parents[Bone(skeleton, index)];
	}

	i32 ReadLocalPose(Skeleton& skeleton, std::span<SltBoneTransform> pose) {
		skeleton.UpdatePose();
		return Copy(skeleton.GetLocalPose(), pose);
	}

	void WriteBoneLocalPose(Skeleton& skeleton, i32 bone, const Vec3& position, const Quat& rotation, const Vec3& scale) {
		const SkeletonAsset* asset = skeleton.GetSkeleton().Get();
		const u32 count = asset ? asset->GetBoneCount() : 0;
		if (bone < 0 || static_cast<u32>(bone) >= count) {
			throw std::runtime_error(std::format("Skeleton has no bone {} ({} bone(s))", bone, count));
		}
		skeleton.SetBoneLocalPose(static_cast<u32>(bone), BoneTransform{position, rotation, scale});
	}

	i32 ReadRestPose(const SkeletonAsset& skeleton, std::span<SltBoneTransform> pose) noexcept {
		return Copy(skeleton.restPose, pose);
	}

} // namespace Shalltear::CSharpImpl::SkeletonBindings
