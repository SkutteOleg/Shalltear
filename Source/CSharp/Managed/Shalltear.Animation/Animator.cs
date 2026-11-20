using System;

using Shalltear.Interop;

namespace Shalltear;

public sealed partial class Animator {
	/// <summary>Play's default fade (the layer's fade is used).</summary>
	public const float DEFAULT_FADE = -1f;

	/// <summary>Play clip on layer, fading over fade seconds.</summary>
	public PlayHandle Play(AnimationClip clip, float fade = DEFAULT_FADE, int layer = 0) {
		ArgumentNullException.ThrowIfNull(clip);
		PlayHandle played = default;
		PlayClip(clip, fade, layer, new Span<PlayHandle>(ref played), "Animator.Play");
		return played;
	}

	/// <summary>Play a blend of motions on layer, fading over fade seconds.</summary>
	public PlayHandle PlayBlend(ReadOnlySpan<AnimationMotion> motions, float fade = DEFAULT_FADE, int layer = 0) {
		PlayHandle played = default;
		PlayMotions(motions, fade, layer, new Span<PlayHandle>(ref played), "Animator.PlayBlend");
		return played;
	}

	/// <summary>Layer at index (layer 0 made by the 1st Play).</summary>
	public AnimationLayer GetLayer(int index) {
		if ((uint)index >= LayerCount) {
			throw new ArgumentOutOfRangeException(nameof(index), index, "Animator has " + LayerCount + " layer(s)");
		}
		return new AnimationLayer(this, index);
	}

	/// <summary>Call handler for each event key passed by a playing clip. Returns its token.</summary>
	public uint Subscribe(Action<AnimationEvent> handler) {
		return AnimationEvents.Subscribe(this, handler);
	}

	/// <summary>Stop calling the handler of token. False when there is no such subscription.</summary>
	public bool Unsubscribe(uint token) {
		return AnimationEvents.Unsubscribe(this, token);
	}
}

public partial struct PlayHandle : IEquatable<PlayHandle> {
	/// <summary>True while playing on its layer.</summary>
	public readonly bool IsValid => PlayHandleBindings.IsValid(_animatorId, _layer, _id);

	/// <summary>Leading clip of this Play (null once gone).</summary>
	public readonly AnimationClip? Clip => PlayHandleBindings.GetClip(_animatorId, _layer, _id);

	/// <summary>Playback position in seconds.</summary>
	public readonly float Time {
		get => PlayHandleBindings.GetTime(_animatorId, _layer, _id);
		set => PlayHandleBindings.SetTime(_animatorId, _layer, _id, value);
	}

	/// <summary>Playback speed of its layer.</summary>
	public readonly float Speed {
		get => PlayHandleBindings.GetSpeed(_animatorId, _layer, _id);
		set => PlayHandleBindings.SetSpeed(_animatorId, _layer, _id, value);
	}

	/// <summary>Stop this Play (later Plays on its layer untouched).</summary>
	public readonly void Stop() {
		PlayHandleBindings.Stop(_animatorId, _layer, _id);
	}

	/// <inheritdoc/>
	public readonly bool Equals(PlayHandle other) {
		return _animatorId == other._animatorId && _layer == other._layer && _id == other._id;
	}

	/// <inheritdoc/>
	public override readonly bool Equals(object? other) {
		return other is PlayHandle value && Equals(value);
	}

	/// <inheritdoc/>
	public override readonly int GetHashCode() {
		return HashCode.Combine(_animatorId, _layer, _id);
	}

	/// <summary>Same Play.</summary>
	public static bool operator ==(PlayHandle left, PlayHandle right) {
		return left.Equals(right);
	}

	/// <summary>Opposite of ==.</summary>
	public static bool operator !=(PlayHandle left, PlayHandle right) {
		return !left.Equals(right);
	}
}

public partial struct AnimationMotion {
	/// <summary>Motion of clip at weight + speed.</summary>
	public AnimationMotion(AnimationClip clip, float weight = 1f, float speed = 1f) {
		ArgumentNullException.ThrowIfNull(clip);
		_clipId = clip._id;
		this.weight = weight;
		this.speed = speed;
	}

	/// <summary>Clip of the motion.</summary>
	public readonly AnimationClip? Clip => ObjectCache.Find<AnimationClip>(_clipId);
}

public partial struct AnimationEvent {
	/// <summary>Hash of the event's name.</summary>
	public readonly ulong NameHash => _nameHash;

	/// <summary>Clip of the event's key.</summary>
	public readonly AnimationClip? Clip => ObjectCache.Find<AnimationClip>(_clipId);

	/// <summary>True when the event's name is name.</summary>
	public readonly bool Is(string name) {
		return Bridge.HashOf(name) == _nameHash;
	}
}
