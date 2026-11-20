using System;
using System.Collections.Generic;

using Shalltear.Interop;

namespace Shalltear.Conformance;

sealed class Rig : Behavior {
	public Animator? animator;
	public AnimationClip? clip;
	public AnimatorController? controller;
	public List<SpringBone> bones = new();
}

static class AnimationProbe {
	static bool Gone(EngineObject? value) {
		return value == null;
	}

	static void Throw(AnimationEvent step) {
		throw new InvalidOperationException("thrown by an animation handler");
	}

	static void Hear(AnimationEvent step) {
		BindingProbe.Record("heard " + step.time);
	}

	static void Remember(AnimationEvent step) {
		BindingProbe.Record("old handler");
	}

	static Animator Walking(string name) {
		Animator animator = SceneSystem.CreateObject(name).AddComponent<Animator>();
		animator.Play(AnimationTests.Stored<AnimationClip>(AnimationTests.WALK), 0f);
		return animator;
	}

	[ConformanceTest]
	internal static void HandlerThrows() {
		Animator animator = Walking("Thrower");
		animator.Subscribe(Throw);
		animator.Subscribe(Hear);
		animator.Evaluate(0.75f);
		animator.Owner.Destroy();
	}

	[ConformanceTest]
	internal static void KeepsListening() {
		SceneSystem.CreateObject("Keeper").AddComponent<Animator>().Subscribe(Remember);
	}

	[ConformanceTest]
	internal static void CountsKeepersSubscribers() {
		SceneObject? keeper = SceneSystem.FindObjectByName("Keeper");
		if (Gone(keeper)) {
			BindingProbe.Record("no keeper");
			return;
		}
		BindingProbe.Record("subscribers " + keeper!.GetComponent<Animator>()!.SubscriberCount);
	}
}

static class AnimationTests {
	internal const string WALK = "project://Assets/Walk.anim";
	const string HERO = "project://Assets/Hero.controller";

	static bool Gone(EngineObject? value) {
		return value == null;
	}

	internal static T Stored<T>(string path)
		where T : Asset {
		T? found = ObjectCache.Find<T>(BindingProbe.FindAsset(path));
		if (Gone(found)) {
			throw new ConformanceFailure("no " + typeof(T).Name + " at " + path);
		}
		return found!;
	}

	static string Refusal(Action call) {
		try {
			call();
		} catch (EngineException exception) {
			return exception.Message;
		}
		return "allowed";
	}

	static void Near(float expected, float actual, float tolerance, string what) {
		Check.True(MathF.Abs(expected - actual) <= tolerance, what + ": expected " + expected + ", got " + actual);
	}

	static void Retire(SceneObject used) {
		used.SetActive(false);
		used.Destroy();
	}

	[ConformanceTest]
	internal static void AnimatorPropertiesRoundTrip() {
		SceneObject actor = SceneSystem.CreateObject("Actor");
		Animator animator = actor.AddComponent<Animator>();
		Check.Same(animator, actor.GetComponent<Animator>());
		Check.True(Gone(animator.Clip) && Gone(animator.Skeleton), "no clip, no skeleton");
		Check.Equal(1f, animator.Speed);
		Check.Equal(AnimatorUpdateMode.Normal, animator.UpdateMode);
		Check.True(!animator.IsPlaying && !animator.IsTransitioning && !animator.IsRetargeting && !animator.IsExtractingRootMotion, "idle animator");
		Check.Equal(0u, animator.LayerCount);
		Check.Equal(0u, animator.SubscriberCount);
		bool outOfRange = false;
		try {
			animator.GetLayer(0);
		} catch (ArgumentOutOfRangeException) {
			outOfRange = true;
		}
		Check.True(outOfRange, "no layer before a Play");
		AnimationClip walk = Stored<AnimationClip>(WALK);
		animator.Clip = walk;
		animator.PlayAutomatically = false;
		animator.Speed = 2f;
		animator.UpdateMode = AnimatorUpdateMode.UnscaledTime;
		animator.ApplyRootMotion = true;
		animator.Priority = 0.25f;
		Check.Same(walk, animator.Clip);
		Check.True(!animator.PlayAutomatically && animator.ApplyRootMotion, "switches");
		Check.Equal(2f, animator.Speed);
		Check.Equal(AnimatorUpdateMode.UnscaledTime, animator.UpdateMode);
		Check.Equal(0.25f, animator.Priority);
		Check.Equal(Vec3.Zero, animator.RootMotionDelta);
		Check.Equal(Quat.Identity, animator.RootRotationDelta);
		animator.Clip = null;
		Check.True(Gone(animator.Clip), "clip cleared");

		ulong profileId = BindingProbe.MakeAsset("RetargetProfile");
		RetargetProfile profile = ObjectCache.Find<RetargetProfile>(profileId)!;
		Check.Equal(0, profile.ChainCount);
		Check.True(!profile.HasChain("Spine") && Gone(profile.Skeleton), "empty profile");
		animator.SetRetarget(profile, profile);
		animator.ClearRetarget();
		Check.True(!animator.IsRetargeting, "retarget cleared");
		BindingProbe.FreeAsset(profileId);
		Retire(actor);
	}

	[ConformanceTest]
	internal static void AnimatorPlaysClip() {
		SceneObject actor = SceneSystem.CreateObject("Walker");
		Animator animator = actor.AddComponent<Animator>();
		AnimationClip walk = Stored<AnimationClip>(WALK);
		PlayHandle played = animator.Play(walk, 0f);
		Check.True(played.IsValid && animator.IsPlaying, "playing");
		Check.Same(walk, played.Clip);
		Check.Equal(1u, animator.LayerCount);
		animator.Evaluate(0.25f);
		Near(0.5f, actor.LocalPosition.x, 0.001f, "moved by the clip");
		Near(0.25f, played.Time, 0.001f, "handle's time");
		Near(0.25f, animator.Time, 0.001f, "animator's time");
		Check.True(animator.BoundTrackCount >= 1u, "track bound: " + animator.BoundTrackCount);
		played.Time = 0.5f;
		played.Speed = 0.5f;
		Check.Equal(0.5f, played.Speed);
		animator.Evaluate(0.2f);
		Near(1.2f, actor.LocalPosition.x, 0.01f, "half speed");
		animator.Time = 0.75f;
		Near(0.75f, animator.Time, 0.001f, "animator's time set");

		PlayHandle again = animator.Play(walk);
		Check.True(again != played && again.IsValid && !played.IsValid, "a new Play ends the old handle");
		Check.Equal(again, again);
		again.Stop();
		Check.True(!again.IsValid && !animator.IsPlaying, "stopped");
		animator.Play(walk);
		animator.Stop();
		Check.True(!animator.IsPlaying, "animator stopped");
		Check.True(!default(PlayHandle).IsValid && Gone(default(PlayHandle).Clip), "an empty handle");
		Retire(actor);
	}

	[ConformanceTest]
	internal static void LayersAndBlends() {
		SceneObject actor = SceneSystem.CreateObject("Blender");
		Animator animator = actor.AddComponent<Animator>();
		AnimationClip walk = Stored<AnimationClip>(WALK);
		PlayHandle played = animator.PlayBlend([new AnimationMotion(walk, 0.25f), new AnimationMotion(walk, 0.75f, 2f)], 0f);
		Check.True(played.IsValid, "blend playing");
		AnimationLayer layer = animator.GetLayer(0);
		Check.Equal(0, layer.Index);
		Check.Same(animator, layer.Animator);
		Check.Equal(layer, animator.GetLayer(0));
		Check.Equal(2u, layer.MotionCount);
		AnimationMotion second = layer.GetMotion(1);
		Check.Same(walk, second.Clip);
		Check.Equal(0.75f, second.weight);
		Check.Equal(2f, second.speed);
		Check.True(layer.SetMotionWeight(0, 1f) && !layer.SetMotionWeight(5, 1f), "motion weights");
		Check.Equal(1f, layer.GetMotion(0).weight);
		Check.Equal("AnimationLayer has no motion 5 (2 motion(s))", Refusal(() => layer.GetMotion(5)));
		Check.True(layer.IsPlaying && layer.BoundTrackCount == 0u, "layer playing, tracks bound at the 1st evaluation");
		animator.Evaluate(0.1f);
		Check.True(layer.BoundTrackCount >= 1u, "tracks bound: " + layer.BoundTrackCount);
		Check.Same(walk, layer.Clip);

		layer.Weight = 0.5f;
		layer.Speed = 1.5f;
		layer.Blend = AnimationBlendMode.Additive;
		Check.Equal(0.5f, layer.Weight);
		Check.Equal(1.5f, layer.Speed);
		Check.Equal(AnimationBlendMode.Additive, layer.Blend);
		layer.Blend = AnimationBlendMode.Override;
		layer.NormalizedTime = 0.25f;
		Check.Equal(0.25f, layer.NormalizedTime);
		layer.Time = 0f;
		Check.Equal(0f, layer.NormalizedTime);

		Check.Equal(0, layer.MaskLength);
		Check.Equal(0.5f, layer.WeightForBone(0));
		layer.Weight = 1f;
		layer.SetMask([0.5f, 1f]);
		Check.Equal(2, layer.MaskLength);
		Span<float> mask = stackalloc float[4];
		Check.Equal(2, layer.GetMask(mask));
		Check.Equal(0.5f, mask[0]);
		Check.Equal(0.5f, layer.WeightForBone(0));
		layer.ClearMask();
		Check.Equal(0, layer.MaskLength);
		ulong rigId = BindingProbe.MakeSkeletonAsset();
		SkeletonAsset rig = ObjectCache.Find<SkeletonAsset>(rigId)!;
		Check.True(layer.SetMaskSubtree(rig, "Child", 0.25f) && !layer.SetMaskSubtree(rig, "Missing", 1f), "mask subtree");
		Check.Equal(2, layer.MaskLength);
		Check.Equal(0f, layer.WeightForBone(0));
		Check.Equal(0.25f, layer.WeightForBone(1));
		BindingProbe.FreeAsset(rigId);

		Check.Equal(1, animator.AddLayer());
		Check.Equal(2u, animator.LayerCount);
		AnimationLayer upper = animator.GetLayer(1);
		Check.True(upper != layer && !upper.IsPlaying, "a 2nd layer");
		Check.True(animator.RemoveLayer(1) && !animator.RemoveLayer(1), "layer removed once");
		Check.Equal("Animator has no layer 1 (1 layer(s))", Refusal(() => upper.Weight = 1f));
		layer.Stop();
		Check.True(!layer.IsPlaying, "layer stopped");
		Retire(actor);
	}

	[ConformanceTest]
	internal static void EventsReachSubscribers() {
		SceneObject actor = SceneSystem.CreateObject("Stepper");
		Animator animator = actor.AddComponent<Animator>();
		AnimationClip walk = Stored<AnimationClip>(WALK);
		List<AnimationEvent> heard = new();
		uint token = animator.Subscribe(heard.Add);
		Check.True(token != 0u, "a token");
		Check.Equal(1u, animator.SubscriberCount);
		animator.Play(walk, 0f);
		animator.Evaluate(0.25f);
		Check.Equal(0, heard.Count);
		animator.Evaluate(0.5f);
		Check.Equal(1, heard.Count);
		AnimationEvent step = heard[0];
		Check.True(step.Is("Footstep") && !step.Is("Jump"), "named");
		Check.Equal(Bridge.HashOf("Footstep"), step.NameHash);
		Check.Same(walk, step.Clip);
		Check.Equal(0, step.layer);
		Check.Equal(0.5f, step.time);
		Check.Equal(new Vec4(1f, 2f, 3f, 4f), step.value);
		Check.True(animator.Unsubscribe(token) && !animator.Unsubscribe(token), "unsubscribed once");
		Check.Equal(0u, animator.SubscriberCount);
		animator.Evaluate(1f);
		Check.Equal(1, heard.Count);
		Retire(actor);
	}

	[ConformanceTest]
	internal static void StateMachineFollowsParameters() {
		SceneObject actor = SceneSystem.CreateObject("Hero");
		Animator animator = actor.AddComponent<Animator>();
		AnimatorStateMachine machine = actor.AddComponent<AnimatorStateMachine>();
		AnimatorController hero = Stored<AnimatorController>(HERO);
		Check.True(Gone(machine.Controller), "no controller");
		Check.Same(animator, machine.Animator);
		machine.Controller = hero;
		Check.Same(hero, machine.Controller);
		Check.Equal(-1, machine.GetCurrentState(0));
		machine.Tick(0.1f);
		int idle = hero.FindState("Idle");
		int run = hero.FindState("Run");
		Check.Equal(idle, machine.GetCurrentState(0));
		Check.True(animator.IsPlaying && machine.GetNormalizedTime(0) >= 0f, "state playing");
		Check.Equal(0f, machine.GetFloat("Speed"));
		Check.True(machine.SetFloat("Speed", 1f), "Speed set");
		machine.Tick(0.1f);
		Check.Equal(run, machine.GetCurrentState(0));
		Check.True(machine.SetFloat("Speed", 0f), "Speed cleared");
		machine.Tick(0.1f);
		Check.Equal(idle, machine.GetCurrentState(0));

		Check.Equal(3, machine.GetInt("Lives"));
		Check.True(machine.SetInt("Lives", 2), "Lives set");
		Check.Equal(2, machine.GetInt("Lives"));
		Check.True(machine.GetBool("Grounded"), "Grounded default");
		Check.True(machine.SetBool("Grounded", false) && !machine.GetBool("Grounded"), "Grounded set");
		Check.True(machine.SetTrigger("Jump") && machine.GetBool("Jump"), "Jump triggered");
		Check.True(machine.ResetTrigger("Jump") && !machine.GetBool("Jump"), "Jump reset");
		Check.True(!machine.SetFloat("Missing", 1f) && machine.GetFloat("Missing") == 0f, "unknown parameter");

		Check.True(machine.Play("Run", 0f, 0) && !machine.Play("Missing", 0f, 0), "Play by name");
		Check.Equal(run, machine.GetCurrentState(0));
		Check.True(machine.Travel("Idle", 0) && !machine.Travel("Missing", 0), "Travel by name");
		Check.True(machine.GetTravelRemaining(0) <= 1u, "travel path: " + machine.GetTravelRemaining(0));
		Check.Equal(-1, machine.GetCurrentState(3));
		machine.Controller = null;
		Retire(actor);
	}

	[ConformanceTest]
	internal static void ControllerDescribesItself() {
		AnimatorController hero = Stored<AnimatorController>(HERO);
		Check.Equal(4, hero.ParameterCount);
		Check.Equal(2, hero.StateCount);
		Check.Equal(1, hero.LayerCount);
		Check.Equal(0, hero.FindParameter("Speed"));
		Check.Equal(-1, hero.FindParameter("Missing"));
		Check.Equal("Grounded", hero.GetParameterName(2));
		Check.Equal(AnimatorParameterKind.Trigger, hero.GetParameterKind(3));
		Check.Equal(AnimatorParameterKind.Int, hero.GetParameterKind(1));
		Check.Equal("Run", hero.GetStateName(hero.FindState("Run")));
		Check.Equal(-1, hero.FindState("Missing"));
		Check.Equal("AnimatorController has no parameter 9 (4 parameter(s))", Refusal(() => hero.GetParameterName(9)));
		Check.Equal("AnimatorController has no state -1 (2 state(s))", Refusal(() => hero.GetStateName(-1)));

		Span<float> weights = stackalloc float[4];
		Check.Equal(0, AnimatorBlend.Weights(hero, 0, Vec2.Zero, [0f, 3f, 1f, 0f], weights));
		Check.Equal("AnimatorBlend.Weights got 1 value(s) for 4 parameter(s). Pass 1 per parameter",
					Refusal(() => AnimatorBlend.Weights(hero, 0, Vec2.Zero, [0f], stackalloc float[4])));
	}

	[ConformanceTest]
	internal static void ClipFieldsRoundTrip() {
		AnimationClip walk = Stored<AnimationClip>(WALK);
		Check.Equal(1f, walk.Length);
		Check.Equal(30f, walk.FrameRate);
		Check.Equal(AnimationLoopMode.Loop, walk.Loop);
		Check.Equal(0f, walk.AdditiveReferenceTime);
		Check.True(walk.BakeRootMotionIntoPose && Gone(walk.Skeleton), "a property clip");
		Check.Equal(2, walk.TrackCount);
		walk.Loop = AnimationLoopMode.PingPong;
		walk.FrameRate = 24f;
		walk.AdditiveReferenceTime = 0.5f;
		walk.BakeRootMotionIntoPose = false;
		Check.Equal(AnimationLoopMode.PingPong, walk.Loop);
		Check.Equal(24f, walk.FrameRate);
		Check.Equal(0.5f, walk.AdditiveReferenceTime);
		Check.True(!walk.BakeRootMotionIntoPose, "root motion off");
		walk.Loop = AnimationLoopMode.Loop;
		walk.FrameRate = 30f;
		walk.AdditiveReferenceTime = 0f;
		walk.BakeRootMotionIntoPose = true;
		Check.Equal(1f, walk.Length);
	}

	[ConformanceTest]
	internal static void SpringBonesRoundTrip() {
		SceneObject tail = SceneSystem.CreateObject("Tail");
		SceneSystem.CreateObject("Tip").SetParent(tail);
		SpringBone bone = tail.AddComponent<SpringBone>();
		SpringBoneCollider ball = tail.AddComponent<SpringBoneCollider>();
		bone.RootBone = "Hips";
		bone.Stiffness = 0.25f;
		bone.Damping = 0.125f;
		bone.Gravity = new Vec3(0f, -2f, 0f);
		bone.Gust = new Vec3(1f, 0f, 0f);
		bone.GustFrequency = 0.5f;
		bone.MaxAngleDeg = 45f;
		bone.HingeAxis = SpringBoneHingeAxis.Z;
		bone.Radius = 0.05f;
		bone.Weight = 0.75f;
		bone.StepRate = 90f;
		Check.Equal("Hips", bone.RootBone);
		Check.Equal(0.25f, bone.Stiffness);
		Check.Equal(0.125f, bone.Damping);
		Check.Equal(new Vec3(0f, -2f, 0f), bone.Gravity);
		Check.Equal(new Vec3(1f, 0f, 0f), bone.Gust);
		Check.Equal(0.5f, bone.GustFrequency);
		Check.Equal(45f, bone.MaxAngleDeg);
		Check.Equal(SpringBoneHingeAxis.Z, bone.HingeAxis);
		Check.Equal(0.05f, bone.Radius);
		Check.Equal(0.75f, bone.Weight);
		Check.Equal(90f, bone.StepRate);
		bone.RootBone = "";

		ball.Shape = SpringBoneColliderShape.Capsule;
		ball.Radius = 0.2f;
		ball.Height = 1.5f;
		ball.Center = new Vec3(0f, 0.5f, 0f);
		Check.Equal(SpringBoneColliderShape.Capsule, ball.Shape);
		Check.Equal(0.2f, ball.Radius);
		Check.Equal(1.5f, ball.Height);
		Check.Equal(new Vec3(0f, 0.5f, 0f), ball.Center);

		Check.Equal(0u, bone.ColliderCount);
		bone.AddCollider(ball);
		Check.Equal(1u, bone.ColliderCount);
		Check.Same(ball, bone.GetCollider(0));
		Check.True(Gone(bone.GetCollider(1)), "no 2nd collider");
		bone.Simulate(0.1f);
		bone.Reset();
		bone.ClearColliders();
		Check.Equal(0u, bone.ColliderCount);
		Retire(tail);
	}

	[ConformanceTest]
	internal static void SkeletonsDescribeTheirBones() {
		ulong rigId = BindingProbe.MakeSkeletonAsset();
		SkeletonAsset rig = ObjectCache.Find<SkeletonAsset>(rigId)!;
		Check.Equal(2u, rig.BoneCount);
		Check.Equal(1, rig.FindBone("Child"));
		Check.Equal(-1, rig.FindBone("Missing"));
		Check.Equal("Root", rig.GetBoneName(0));
		Check.Equal(-1, rig.GetBoneParent(0));
		Check.Equal(0, rig.GetBoneParent(1));
		Check.Equal("SkeletonAsset has no bone 2 (2 bone(s))", Refusal(() => rig.GetBoneName(2)));

		SceneObject body = SceneSystem.CreateObject("Body");
		Skeleton skeleton = body.AddComponent<Skeleton>();
		Animator animator = body.AddComponent<Animator>();
		Check.True(Gone(skeleton.SkeletonAsset), "no rig yet");
		skeleton.SkeletonAsset = rig;
		Check.Same(rig, skeleton.SkeletonAsset);
		Check.Same(skeleton, animator.Skeleton);
		Check.Equal(0u, skeleton.ExposedBoneCount);
		Check.Equal(-1, skeleton.GetExposedBone(0));
		Check.True(Gone(skeleton.GetExposedObject(0)), "no exposed object");
		skeleton.UpdatePose();
		skeleton.SkeletonAsset = null;
		Retire(body);
		BindingProbe.FreeAsset(rigId);
	}

	[ConformanceTest]
	internal static void ScriptFieldsHoldAnimationTypes() {
		SceneObject source = SceneSystem.CreateObject("Rigged");
		Animator animator = source.AddComponent<Animator>();
		SpringBone bone = source.AddComponent<SpringBone>();
		Rig rig = source.AddComponent<Rig>();
		rig.animator = animator;
		rig.clip = Stored<AnimationClip>(WALK);
		rig.controller = Stored<AnimatorController>(HERO);
		rig.bones.Add(bone);

		SceneObject copy = SceneSystem.Instantiate(source);
		Rig copied = copy.GetComponent<Rig>()!;
		Check.Same(copy.GetComponent<Animator>(), copied.animator);
		Check.Same(rig.clip, copied.clip);
		Check.Same(rig.controller, copied.controller);
		Check.Equal(1, copied.bones.Count);
		Check.Same(copy.GetComponent<SpringBone>(), copied.bones[0]);
		Retire(source);
		Retire(copy);
	}

	[ConformanceTest]
	internal static void RepeatedAnimatorCallsAllocateNothing() {
		SceneObject actor = SceneSystem.CreateObject("Budget");
		Animator animator = actor.AddComponent<Animator>();
		AnimatorStateMachine machine = actor.AddComponent<AnimatorStateMachine>();
		machine.Controller = Stored<AnimatorController>(HERO);
		machine.Tick(0.016f);
		AnimationLayer layer = animator.GetLayer(0);
		PlayHandle played = animator.Play(Stored<AnimationClip>(WALK), 0f);
		animator.Evaluate(0.016f);

		long before = GC.GetAllocatedBytesForCurrentThread();
		int hits = 0;
		for (int round = 0; round < 1000; ++round) {
			hits += machine.SetFloat("Speed", 0.25f) && machine.GetFloat("Speed") == 0.25f ? 1 : 0;
			hits += machine.SetBool("Grounded", true) && machine.GetBool("Grounded") ? 1 : 0;
			hits += machine.SetTrigger("Jump") && machine.ResetTrigger("Jump") ? 1 : 0;
			hits += machine.SetInt("Lives", 3) && machine.GetInt("Lives") == 3 ? 1 : 0;
			hits += machine.GetCurrentState(0) >= 0 && machine.GetNormalizedTime(0) >= 0f ? 1 : 0;
			layer.Weight = 1f;
			hits += layer.Weight == 1f && animator.Speed == 1f && played.Speed >= 0f ? 1 : 0;
			animator.Evaluate(0.016f);
			hits += animator.Time >= 0f && ReferenceEquals(actor.GetComponent<Animator>(), animator) ? 1 : 0;
		}
		long allocated = GC.GetAllocatedBytesForCurrentThread() - before;
		Check.Equal(7000, hits);
		Check.Equal(0L, allocated);
		Retire(actor);
	}
}
