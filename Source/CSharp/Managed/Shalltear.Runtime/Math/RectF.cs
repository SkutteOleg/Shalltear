using System;
using System.Runtime.InteropServices;

#pragma warning disable CS1591

namespace Shalltear;

[StructLayout(LayoutKind.Sequential)]
public struct RectF : IEquatable<RectF> {
	public Vec2 position;
	public Vec2 size;

	public RectF(Vec2 position, Vec2 size) {
		this.position = position;
		this.size = size;
	}

	public RectF(float x, float y, float width, float height) {
		position = new Vec2(x, y);
		size = new Vec2(width, height);
	}

	public static bool operator ==(RectF a, RectF b) {
		return a.position == b.position && a.size == b.size;
	}

	public static bool operator !=(RectF a, RectF b) {
		return !(a == b);
	}

	public readonly bool Equals(RectF other) {
		return this == other;
	}

	public override readonly bool Equals(object? other) {
		return other is RectF value && this == value;
	}

	public override readonly int GetHashCode() {
		return HashCode.Combine(position, size);
	}

	public override readonly string ToString() {
		return "(" + position + ", " + size + ")";
	}
}
