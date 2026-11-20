#pragma once

#include "Generated/AnimationBindings.generated.h"

#include "Shalltear/Core/Math/Vector.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/Animation.h"

#include <span>

// refusals thrown as std::runtime_error
namespace Shalltear::CSharpImpl::AnimationBindings {

	AnimationLayer& GetLayer(Animator& animator, i32 layer);
	void ReadMotion(Animator& animator, i32 layer, i32 index, std::span<SltAnimationMotion> motion);
	// returned count may exceed weights.size()
	i32 ReadMask(Animator& animator, i32 layer, std::span<f32> weights);
	void WriteMask(Animator& animator, i32 layer, std::span<const f32> weights);

	void PlayClip(Animator& animator, AnimationClip* clip, f32 fade, i32 layer, std::span<SltPlayHandle> played);
	void PlayMotions(Animator& animator, std::span<const SltAnimationMotion> motions, f32 fade, i32 layer, std::span<SltPlayHandle> played);
	PlayHandle MakePlay(u64 animatorId, i32 layer, u32 id);

	const AnimatorController::Parameter& Parameter(const AnimatorController& controller, i32 index);
	const AnimatorController::State& State(const AnimatorController& controller, i32 index);
	i32 BlendWeights(const AnimatorController& controller, i32 node, const Vec2& sample, std::span<const f32> values, std::span<f32> weights);

	u32 Listen(Animator& animator, u64 key);
	u64 Unlisten(Animator& animator, u32 token);
	// managed function pointer, must match AnimationEvents.cs
	void AttachEvents(i64 entry);
	void UnlistenAll();

	void Release();

} // namespace Shalltear::CSharpImpl::AnimationBindings
