using System;

using Shalltear.Interop;

namespace Shalltear;

/// <summary>1 layer of an Animator (calls throw after RemoveLayer).</summary>
public readonly struct AnimationLayer : IEquatable<AnimationLayer> {
	readonly Animator _animator;
	readonly int _index;

	internal AnimationLayer(Animator animator, int index) {
		_animator = animator;
		_index = index;
	}

	/// <summary>Layer's Animator.</summary>
	public Animator Animator => _animator;

	/// <summary>Index of the layer in its Animator.</summary>
	public int Index => _index;

	/// <summary>Number of blended motions.</summary>
	public uint MotionCount => AnimationLayerBindings.GetMotionCount(_animator, _index, "AnimationLayer.MotionCount");

	/// <summary>Motion at index.</summary>
	public AnimationMotion GetMotion(int index) {
		AnimationMotion motion = default;
		AnimationLayerBindings.ReadMotion(_animator, _index, index, new Span<AnimationMotion>(ref motion), "AnimationLayer.GetMotion");
		return motion;
	}

	/// <summary>Set the weight of the motion at index. False when there is none.</summary>
	public bool SetMotionWeight(int index, float value) {
		return AnimationLayerBindings.SetMotionWeight(_animator, _index, index, value, "AnimationLayer.SetMotionWeight");
	}

	/// <summary>True while the layer plays.</summary>
	public bool IsPlaying => AnimationLayerBindings.IsPlaying(_animator, _index, "AnimationLayer.IsPlaying");

	/// <summary>Clip with the largest weight (null when none).</summary>
	public AnimationClip? Clip => AnimationLayerBindings.GetClip(_animator, _index, "AnimationLayer.Clip");

	/// <summary>Share of the layer in the pose.</summary>
	public float Weight {
		get => AnimationLayerBindings.GetWeight(_animator, _index, "AnimationLayer.Weight");
		set => AnimationLayerBindings.SetWeight(_animator, _index, value, "AnimationLayer.Weight");
	}

	/// <summary>Playback speed of the layer.</summary>
	public float Speed {
		get => AnimationLayerBindings.GetSpeed(_animator, _index, "AnimationLayer.Speed");
		set => AnimationLayerBindings.SetSpeed(_animator, _index, value, "AnimationLayer.Speed");
	}

	/// <summary>How the layer joins the layers below.</summary>
	public AnimationBlendMode Blend {
		get => AnimationLayerBindings.GetBlend(_animator, _index, "AnimationLayer.Blend");
		set => AnimationLayerBindings.SetBlend(_animator, _index, value, "AnimationLayer.Blend");
	}

	/// <summary>Set the mask weight of rootBone + all bones below. False when the rig has no such bone.</summary>
	public bool SetMaskSubtree(SkeletonAsset skeleton, string rootBone, float value) {
		return AnimationLayerBindings.SetMaskSubtree(_animator, _index, skeleton, rootBone, value, "AnimationLayer.SetMaskSubtree");
	}

	/// <summary>Weight all bones fully.</summary>
	public void ClearMask() {
		AnimationLayerBindings.ClearMask(_animator, _index, "AnimationLayer.ClearMask");
	}

	/// <summary>Mask weight of a bone of the clip's rig.</summary>
	public float WeightForBone(int bone) {
		return AnimationLayerBindings.WeightForBone(_animator, _index, bone, "AnimationLayer.WeightForBone");
	}

	/// <summary>Number of bones in the mask.</summary>
	public int MaskLength => AnimationLayerBindings.GetMaskLength(_animator, _index, "AnimationLayer.MaskLength");

	/// <summary>Copy the mask into weights. Returns MaskLength.</summary>
	public int GetMask(Span<float> weights) {
		return AnimationLayerBindings.ReadMask(_animator, _index, weights, "AnimationLayer.GetMask");
	}

	/// <summary>Weight bone N by weights[N].</summary>
	public void SetMask(ReadOnlySpan<float> weights) {
		AnimationLayerBindings.WriteMask(_animator, _index, weights, "AnimationLayer.SetMask");
	}

	/// <summary>Playback position in seconds.</summary>
	public float Time {
		get => AnimationLayerBindings.GetTime(_animator, _index, "AnimationLayer.Time");
		set => AnimationLayerBindings.SetTime(_animator, _index, value, "AnimationLayer.Time");
	}

	/// <summary>Playback position in cycles (+1 per cycle).</summary>
	public float NormalizedTime {
		get => AnimationLayerBindings.GetNormalizedTime(_animator, _index, "AnimationLayer.NormalizedTime");
		set => AnimationLayerBindings.SetNormalizedTime(_animator, _index, value, "AnimationLayer.NormalizedTime");
	}

	/// <summary>Stop the layer.</summary>
	public void Stop() {
		AnimationLayerBindings.Stop(_animator, _index, "AnimationLayer.Stop");
	}

	/// <summary>Tracks of the layer's clips bound to a target.</summary>
	public uint BoundTrackCount => AnimationLayerBindings.GetBoundTrackCount(_animator, _index, "AnimationLayer.BoundTrackCount");

	/// <inheritdoc/>
	public bool Equals(AnimationLayer other) {
		return ReferenceEquals(_animator, other._animator) && _index == other._index;
	}

	/// <inheritdoc/>
	public override bool Equals(object? other) {
		return other is AnimationLayer value && Equals(value);
	}

	/// <inheritdoc/>
	public override int GetHashCode() {
		return HashCode.Combine(_animator, _index);
	}

	/// <summary>Same layer of the same Animator.</summary>
	public static bool operator ==(AnimationLayer left, AnimationLayer right) {
		return left.Equals(right);
	}

	/// <summary>Opposite of ==.</summary>
	public static bool operator !=(AnimationLayer left, AnimationLayer right) {
		return !left.Equals(right);
	}
}
