using System;
using System.Globalization;

namespace Shalltear;

/// <summary>2 ints (pxs, cells).</summary>
public struct Vec2I : IEquatable<Vec2I> {
	/// <summary>X component.</summary>
	public int x;
	/// <summary>Y component.</summary>
	public int y;

	/// <summary>Vector from components.</summary>
	public Vec2I(int x, int y) {
		this.x = x;
		this.y = y;
	}

	/// <summary>(0, 0).</summary>
	public static Vec2I Zero => default;

	/// <summary>(1, 1).</summary>
	public static Vec2I One => new(1, 1);

	/// <summary>Same components.</summary>
	public static bool operator ==(Vec2I left, Vec2I right) {
		return left.x == right.x && left.y == right.y;
	}

	/// <summary>Opposite of ==.</summary>
	public static bool operator !=(Vec2I left, Vec2I right) {
		return !(left == right);
	}

	/// <summary>Sum per component.</summary>
	public static Vec2I operator +(Vec2I left, Vec2I right) {
		return new Vec2I(left.x + right.x, left.y + right.y);
	}

	/// <summary>Difference per component.</summary>
	public static Vec2I operator -(Vec2I left, Vec2I right) {
		return new Vec2I(left.x - right.x, left.y - right.y);
	}

	/// <summary>Negated components.</summary>
	public static Vec2I operator -(Vec2I value) {
		return new Vec2I(-value.x, -value.y);
	}

	/// <summary>Same numbers as floats.</summary>
	public static implicit operator Vec2(Vec2I value) {
		return new Vec2(value.x, value.y);
	}

	/// <inheritdoc/>
	public readonly bool Equals(Vec2I other) {
		return this == other;
	}

	/// <inheritdoc/>
	public override readonly bool Equals(object? other) {
		return other is Vec2I value && this == value;
	}

	/// <inheritdoc/>
	public override readonly int GetHashCode() {
		return HashCode.Combine(x, y);
	}

	/// <inheritdoc/>
	public override readonly string ToString() {
		return string.Create(CultureInfo.InvariantCulture, $"({x}, {y})");
	}
}
