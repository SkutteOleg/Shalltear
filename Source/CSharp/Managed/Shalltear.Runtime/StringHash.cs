using System;
using System.Globalization;

using Shalltear.Interop;

namespace Shalltear;

/// <summary>Hash of a name (names compared by hash in engine).</summary>
public readonly struct StringHash : IEquatable<StringHash> {
	/// <summary>Hash value.</summary>
	public readonly ulong value;

	/// <summary>Hash from value.</summary>
	public StringHash(ulong value) {
		this.value = value;
	}

	/// <summary>Hash of a name.</summary>
	public StringHash(string name) {
		value = Bridge.HashOf(name);
	}

	/// <summary>Hash of a name.</summary>
	public static implicit operator StringHash(string name) {
		return new StringHash(name);
	}

	/// <summary>Same hash.</summary>
	public static bool operator ==(StringHash left, StringHash right) {
		return left.value == right.value;
	}

	/// <summary>Opposite of ==.</summary>
	public static bool operator !=(StringHash left, StringHash right) {
		return left.value != right.value;
	}

	/// <inheritdoc/>
	public bool Equals(StringHash other) {
		return value == other.value;
	}

	/// <inheritdoc/>
	public override bool Equals(object? other) {
		return other is StringHash hash && value == hash.value;
	}

	/// <inheritdoc/>
	public override int GetHashCode() {
		return value.GetHashCode();
	}

	/// <inheritdoc/>
	public override string ToString() {
		return value.ToString("x16", CultureInfo.InvariantCulture);
	}
}
