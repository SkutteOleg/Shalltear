#include "SkeletonBindings.h"

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

	} // anonymous namespace

	std::string_view GetBoneName(const SkeletonAsset& skeleton, i32 index) {
		const size_t bone = Bone(skeleton, index);
		return bone < skeleton.boneNames.size() ? std::string_view(skeleton.boneNames[bone]) : std::string_view{};
	}

	i32 GetBoneParent(const SkeletonAsset& skeleton, i32 index) {
		return skeleton.parents[Bone(skeleton, index)];
	}

} // namespace Shalltear::CSharpImpl::SkeletonBindings
