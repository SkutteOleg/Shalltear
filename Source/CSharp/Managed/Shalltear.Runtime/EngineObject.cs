using System;

using Shalltear.Interop;

namespace Shalltear;

/// <summary>Object owned by engine (== null once destroyed).</summary>
public abstract class EngineObject {
	internal ulong _id;
	internal ScriptType? _scriptType;
	internal string?[]? _texts;

	private protected EngineObject() {}

	/// <summary>True while object exists (false for null).</summary>
	public static implicit operator bool(EngineObject? value) {
		return value is not null && value.IsAlive;
	}

	/// <summary>Destroyed object == null (+ another destroyed one).</summary>
	public static bool operator ==(EngineObject? left, EngineObject? right) {
		bool leftGone = left is null || !left.IsAlive;
		bool rightGone = right is null || !right.IsAlive;
		return leftGone || rightGone ? leftGone == rightGone : ReferenceEquals(left, right);
	}

	/// <summary>Opposite of ==.</summary>
	public static bool operator !=(EngineObject? left, EngineObject? right) {
		return !(left == right);
	}

	/// <inheritdoc/>
	public override bool Equals(object? other) {
		return other is EngineObject value ? this == value : other is null && !IsAlive;
	}

	/// <inheritdoc/>
	public override int GetHashCode() {
		return _id.GetHashCode();
	}

	bool IsAlive => _id != 0 && ObjectBindings.IsAlive(_id);
}

/// <summary>Thrown when a member of a destroyed object is used.</summary>
public sealed class DestroyedObjectException : Exception {
	internal DestroyedObjectException(string member) : base(member + " used on destroyed object. Check for null before use") {}
}
