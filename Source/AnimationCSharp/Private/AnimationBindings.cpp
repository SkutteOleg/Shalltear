#include "AnimationBindings.h"

#include "BindingSupport.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <vector>

namespace Shalltear::CSharpImpl::AnimationBindings {

	namespace {

		struct Listener {
			u64 animator = 0;
			u32 token = 0;
			u64 key = 0;
		};

		void (*_entry)(uint64_t key, const SltAnimationEvent* event) = nullptr;
		std::vector<Listener> _listeners;
		Vector<AnimationLayer::Motion> _motions;

		void OnEvent(const AnimationEvent& event, void* user) {
			if (!_entry) {
				return;
			}
			const SltAnimationEvent sent{event.name.GetValue(), IdOf(event.clip), event.layer, event.time, FromEngine(event.value)};
			ManagedTypes::FlushDirty();
			_entry(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(user)), &sent);
		}

		void Report(const PlayHandle& played, std::span<SltPlayHandle> out) {
			if (!out.empty()) {
				out[0] = SltPlayHandle{IdOf(played.GetAnimator()), played.GetLayer(), played.GetId()};
			}
		}

	} // anonymous namespace

	AnimationLayer& GetLayer(Animator& animator, i32 layer) {
		AnimationLayer* found = animator.GetLayer(layer);
		if (!found) {
			throw std::runtime_error(std::format("Animator has no layer {} ({} layer(s))", layer, animator.GetLayerCount()));
		}
		return *found;
	}

	void ReadMotion(Animator& animator, i32 layer, i32 index, std::span<SltAnimationMotion> motion) {
		const AnimationLayer& read = GetLayer(animator, layer);
		if (index < 0 || static_cast<u32>(index) >= read.GetMotionCount()) {
			throw std::runtime_error(std::format("AnimationLayer has no motion {} ({} motion(s))", index, read.GetMotionCount()));
		}
		const AnimationLayer::Motion found = read.GetMotion(static_cast<u32>(index));
		if (!motion.empty()) {
			motion[0] = SltAnimationMotion{IdOf(found.clip), found.weight, found.speed};
		}
	}

	i32 ReadMask(Animator& animator, i32 layer, std::span<f32> weights) {
		const AnimationLayer& read = GetLayer(animator, layer);
		std::copy_n(read.mask.begin(), std::min(read.mask.size(), weights.size()), weights.begin());
		return static_cast<i32>(read.mask.size());
	}

	void WriteMask(Animator& animator, i32 layer, std::span<const f32> weights) {
		AnimationLayer& written = GetLayer(animator, layer);
		written.mask.assign(weights.begin(), weights.end());
	}

	void PlayClip(Animator& animator, AnimationClip* clip, f32 fade, i32 layer, std::span<SltPlayHandle> played) {
		Report(animator.Play(Handle<AnimationClip>(clip), fade, layer), played);
	}

	void PlayMotions(Animator& animator, std::span<const SltAnimationMotion> motions, f32 fade, i32 layer, std::span<SltPlayHandle> played) {
		_motions.clear();
		for (const SltAnimationMotion& motion : motions) {
			AnimationLayer::Motion engine;
			engine.clip = Handle<AnimationClip>(ResolveObject<AnimationClip>(motion.clipId));
			engine.weight = motion.weight;
			engine.speed = motion.speed;
			_motions.push_back(engine);
		}
		Report(animator.PlayBlend(_motions.data(), static_cast<u32>(_motions.size()), fade, layer), played);
	}

	PlayHandle MakePlay(u64 animatorId, i32 layer, u32 id) {
		return PlayHandle(Handle<Animator>(ResolveObject<Animator>(animatorId)), layer, id);
	}

	const AnimatorController::Parameter& Parameter(const AnimatorController& controller, i32 index) {
		if (index < 0 || static_cast<size_t>(index) >= controller.parameters.size()) {
			throw std::runtime_error(std::format("AnimatorController has no parameter {} ({} parameter(s))", index, controller.parameters.size()));
		}
		return controller.parameters[static_cast<size_t>(index)];
	}

	const AnimatorController::State& State(const AnimatorController& controller, i32 index) {
		if (index < 0 || static_cast<size_t>(index) >= controller.states.size()) {
			throw std::runtime_error(std::format("AnimatorController has no state {} ({} state(s))", index, controller.states.size()));
		}
		return controller.states[static_cast<size_t>(index)];
	}

	i32 BlendWeights(const AnimatorController& controller, i32 node, const Vec2& sample, std::span<const f32> values, std::span<f32> weights) {
		if (values.size() < controller.parameters.size()) {
			throw std::runtime_error(std::format("AnimatorBlend.Weights got {} value(s) for {} parameter(s). Pass 1 per parameter", values.size(),
												 controller.parameters.size()));
		}
		return static_cast<i32>(AnimatorBlend::Weights(controller, node, sample, values.data(), weights.data(), static_cast<u32>(weights.size())));
	}

	u32 Listen(Animator& animator, u64 key) {
		std::erase_if(_listeners, [](const Listener& held) { return !ObjectRegistry::Resolve(held.animator); });
		const u32 token = animator.Subscribe(&OnEvent, reinterpret_cast<void*>(static_cast<uintptr_t>(key)));
		_listeners.push_back(Listener{animator.GetInstanceId(), token, key});
		return token;
	}

	u64 Unlisten(Animator& animator, u32 token) {
		const u64 id = animator.GetInstanceId();
		const auto found = std::find_if(_listeners.begin(), _listeners.end(), [&](const Listener& held) { return held.animator == id && held.token == token; });
		if (found == _listeners.end() || !animator.Unsubscribe(token)) {
			return 0;
		}
		const u64 key = found->key;
		_listeners.erase(found);
		return key;
	}

	void AttachEvents(i64 entry) {
		_entry = reinterpret_cast<decltype(_entry)>(static_cast<intptr_t>(entry));
	}

	void UnlistenAll() {
		for (const Listener& held : _listeners) {
			if (Animator* animator = ResolveObject<Animator>(held.animator)) {
				animator->Unsubscribe(held.token);
			}
		}
		_listeners.clear();
	}

	void Release() {
		UnlistenAll();
		_entry = nullptr;
		_listeners = {};
		_motions = {};
	}

} // namespace Shalltear::CSharpImpl::AnimationBindings
