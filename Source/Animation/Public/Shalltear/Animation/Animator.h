#pragma once

#include "Shalltear/Animation/AnimationAPI.h"
#include "Shalltear/Animation/AnimationClip.h"
#include "Shalltear/Animation/AnimationEvent.h"
#include "Shalltear/Animation/AnimationLayer.h"
#include "Shalltear/Animation/RetargetProfile.h"

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Graphics/Skeleton.h"
#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Reflection/Reflect.h"
#include "Shalltear/Core/Scene/Component.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "Shalltear/Core/Types.h"

namespace Shalltear {

	enum class AnimatorUpdateMode : u8 {
		Normal = 0,
		AnimatePhysics = 1,
		UnscaledTime = 2,
	};

	inline constexpr f32 ANIMATION_DEFAULT_FADE = -1.0f;

	class PlayHandle;

} // namespace Shalltear

SLT_REFLECT_ENUM(::Shalltear::AnimatorUpdateMode)
SLT_ENUM_VALUE(Normal)
SLT_ENUM_VALUE(AnimatePhysics)
SLT_ENUM_VALUE(UnscaledTime)
SLT_END_ENUM()

namespace Shalltear {

	class SHALLTEAR_ANIMATION_API Animator : public Component {
	public:
		SLT_REFLECT_COMPONENT(Animator, Component)
		SLT_REFLECT_FIELD(_clip)
		SLT_REFLECT_FIELD(_playAutomatically)
		SLT_REFLECT_FIELD(_speed)
		SLT_REFLECT_FIELD(_updateMode)
		SLT_REFLECT_FIELD(_applyRootMotion)
		SLT_REFLECT_FIELD(_priority)
		SLT_END_REFLECT()

		Animator() = default;
		~Animator() override;

		Handle<AnimationClip> GetClip() const { return _clip; }
		void SetClip(Handle<AnimationClip> clip) { _clip = clip; }

		bool GetPlayAutomatically() const { return _playAutomatically; }
		void SetPlayAutomatically(bool playAutomatically) { _playAutomatically = playAutomatically; }

		f32 GetSpeed() const { return _speed; }
		void SetSpeed(f32 speed) { _speed = speed; }

		AnimatorUpdateMode GetUpdateMode() const { return _updateMode; }
		void SetUpdateMode(AnimatorUpdateMode updateMode) { _updateMode = updateMode; }

		bool GetApplyRootMotion() const { return _applyRootMotion; }
		void SetApplyRootMotion(bool applyRootMotion) { _applyRootMotion = applyRootMotion; }

		// fraction of screen height
		f32 GetPriority() const { return _priority; }
		void SetPriority(f32 priority) { _priority = priority; }

		PlayHandle Play(Handle<AnimationClip> what, f32 fade = ANIMATION_DEFAULT_FADE, i32 layer = 0);

		PlayHandle PlayBlend(const AnimationLayer::Motion* motions, u32 count,
							 f32 fade = ANIMATION_DEFAULT_FADE, i32 layer = 0);

		void Stop();

		bool IsPlaying() const;

		f32 GetTime() const;
		void SetTime(f32 seconds);

		void Evaluate(f32 deltaTime);

		Handle<Skeleton> GetSkeleton() const;

		u32 GetBoundTrackCount() const;

		i32 AddLayer();

		u32 GetLayerCount() const { return static_cast<u32>(_layers.size()); }

		// stable until RemoveLayer/destruction
		AnimationLayer* GetLayer(i32 index);
		const AnimationLayer* GetLayer(i32 index) const;

		bool RemoveLayer(i32 index);

		bool IsTransitioning() const { return _transition != nullptr; }

		u32 Subscribe(AnimationEventFn fn, void* user = nullptr);

		bool Unsubscribe(u32 token);

		u32 GetSubscriberCount() const { return static_cast<u32>(_subscribers.size()); }

		Vec3 GetRootMotionDelta() const { return _rootDelta.position; }
		Quat GetRootRotationDelta() const { return _rootDelta.rotation; }

		bool IsExtractingRootMotion() const { return _rootAnchored; }

		void SetRetarget(Handle<RetargetProfile> source, Handle<RetargetProfile> target);

		void ClearRetarget();

		bool IsRetargeting() const;
	protected:
		void OnEnable() override;
		void OnDisable() override;
	private:
		friend class PlayHandle;

		struct Transition;

		struct Retargeting;

		AnimationLayer& EnsureBaseLayer();
		void AdoptAuthoredClip();
		void RebuildFoldSet();
		void EndTransition();

		BoneTransform FoldBone(const BoneTransform& seed, i16 bone, bool outgoing, f32 back) const;

		const SkeletonAsset* PoseSkeleton(const SkeletonAsset* rig) const;

		BoneTransform PoseSeed(const SkeletonAsset& skeleton, i16 bone) const;

		bool EnsureRetargetMap(const SkeletonAsset* rig);

		void DropPoseSpace();

		void CaptureTransition(const SkeletonAsset* skeleton, f32 deltaTime);

		void WriteProperties(AnimationLayer& layer);

		struct PendingSignal {
			AnimationEvent event;
			AnimationTrackKind kind = AnimationTrackKind::Event;
			Handle<SceneObject> object;
			Handle<Component> component;
			Handle<Asset> cue;
		};

		void CollectSignals(AnimationLayer& layer, i32 layerIndex, bool closeEnd, Vector<PendingSignal>& out);

		i16 RootMotionBone(const SkeletonAsset* skeleton) const;

		void AccumulateRootMotion(i16 bone);

		Handle<AnimationClip> _clip;
		bool _playAutomatically = true;
		f32 _speed = 1.0f;
		AnimatorUpdateMode _updateMode = AnimatorUpdateMode::Normal;
		bool _applyRootMotion = true;
		f32 _priority = 0.0f;

		Vector<AnimationLayer*> _layers{GetMemoryResource(MemTag::Scene)};

		Vector<i16> _foldBones{GetMemoryResource(MemTag::Scene)};

		Transition* _transition = nullptr;

		Retargeting* _retarget = nullptr;

		f32 _pendingFade = 0.0f;

		struct EventSubscriber {
			u32 token = 0;
			AnimationEventFn fn = nullptr;
			void* user = nullptr;
		};

		Vector<EventSubscriber> _subscribers{GetMemoryResource(MemTag::Scene)};

		u32 _nextSubscriberToken = 1;

		BoneTransform _rootAnchor;
		bool _rootAnchored = false;

		i16 _rootBone = -1;

		BoneTransform _rootDelta;
	};

	class SHALLTEAR_ANIMATION_API PlayHandle {
	public:
		PlayHandle() = default;
		PlayHandle(Handle<Animator> animator, i32 layer, u32 id) : _animator(animator), _layer(layer), _id(id) {}

		bool IsValid() const;

		Handle<AnimationClip> GetClip() const;

		f32 GetTime() const;
		void SetTime(f32 seconds);

		f32 GetSpeed() const;
		void SetSpeed(f32 value);

		void Stop();

		Handle<Animator> GetAnimator() const { return _animator; }
		i32 GetLayer() const { return _layer; }
		u32 GetId() const { return _id; }
	private:
		const AnimationLayer* Resolve() const;
		AnimationLayer* ResolveMutable() const;

		Handle<Animator> _animator;
		i32 _layer = 0;
		u32 _id = 0;
	};

} // namespace Shalltear
