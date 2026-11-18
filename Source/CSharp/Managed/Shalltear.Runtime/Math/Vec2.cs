using System;
using System.Globalization;
using System.Runtime.InteropServices;

#pragma warning disable CS1591

namespace Shalltear;

[StructLayout(LayoutKind.Sequential)]
public struct Vec2 : IEquatable<Vec2> {
	public float x;
	public float y;

	public Vec2(float x, float y) {
		this.x = x;
		this.y = y;
	}

	public Vec2(float value) {
		x = value;
		y = value;
	}

	public static Vec2 Zero => new(0.0f, 0.0f);
	public static Vec2 One => new(1.0f, 1.0f);
	public static Vec2 Right => new(1.0f, 0.0f);
	public static Vec2 Left => new(-1.0f, 0.0f);
	public static Vec2 Up => new(0.0f, 1.0f);
	public static Vec2 Down => new(0.0f, -1.0f);

	public float this[int index] {
		readonly get {
			return index switch {
				0 => x,
				1 => y,
				_ => throw new IndexOutOfRangeException(),
			};
		}
		set {
			switch (index) {
			case 0:
				x = value;
				break;
			case 1:
				y = value;
				break;
			default:
				throw new IndexOutOfRangeException();
			}
		}
	}

	public static bool operator ==(Vec2 a, Vec2 b) {
		return a.x == b.x && a.y == b.y;
	}

	public static bool operator !=(Vec2 a, Vec2 b) {
		return !(a == b);
	}

	public static Vec2 operator -(Vec2 value) {
		return new Vec2(-value.x, -value.y);
	}

	public static Vec2 operator +(Vec2 a, Vec2 b) {
		return new Vec2(a.x + b.x, a.y + b.y);
	}

	public static Vec2 operator -(Vec2 a, Vec2 b) {
		return new Vec2(a.x - b.x, a.y - b.y);
	}

	public static Vec2 operator *(Vec2 a, Vec2 b) {
		return new Vec2(a.x * b.x, a.y * b.y);
	}

	public static Vec2 operator /(Vec2 a, Vec2 b) {
		return new Vec2(a.x / b.x, a.y / b.y);
	}

	public static Vec2 operator *(Vec2 a, float b) {
		return new Vec2(a.x * b, a.y * b);
	}

	public static Vec2 operator *(float a, Vec2 b) {
		return new Vec2(a * b.x, a * b.y);
	}

	public static Vec2 operator /(Vec2 a, float b) {
		return new Vec2(a.x / b, a.y / b);
	}

	public readonly float LengthSquared => x * x + y * y;

	public readonly float Length => MathF.Sqrt(x * x + y * y);

	/// <summary>Unit length copy (Zero when length is ~0).</summary>
	public readonly Vec2 Normalized {
		get {
			float length = Length;
			return length > MathUtil.EPSILON ? this / length : Zero;
		}
	}

	public void Normalize() {
		this = Normalized;
	}

	/// <summary>Turned 90 degrees counter clockwise.</summary>
	public readonly Vec2 Perpendicular => new(-y, x);

	public static float Dot(Vec2 a, Vec2 b) {
		return a.x * b.x + a.y * b.y;
	}

	public static float Cross(Vec2 a, Vec2 b) {
		return a.x * b.y - a.y * b.x;
	}

	public static float DistanceSquared(Vec2 a, Vec2 b) {
		return (b - a).LengthSquared;
	}

	public static float Distance(Vec2 a, Vec2 b) {
		return (b - a).Length;
	}

	/// <summary>t clamped to 0-1.</summary>
	public static Vec2 Lerp(Vec2 a, Vec2 b, float t) {
		return LerpUnclamped(a, b, MathUtil.Clamp01(t));
	}

	public static Vec2 LerpUnclamped(Vec2 a, Vec2 b, float t) {
		return a + (b - a) * t;
	}

	public static Vec2 Min(Vec2 a, Vec2 b) {
		return new Vec2(MathUtil.Min(a.x, b.x), MathUtil.Min(a.y, b.y));
	}

	public static Vec2 Max(Vec2 a, Vec2 b) {
		return new Vec2(MathUtil.Max(a.x, b.x), MathUtil.Max(a.y, b.y));
	}

	public static Vec2 Abs(Vec2 value) {
		return new Vec2(MathUtil.Abs(value.x), MathUtil.Abs(value.y));
	}

	public static Vec2 Reflect(Vec2 direction, Vec2 normal) {
		return direction - normal * (2.0f * Dot(direction, normal));
	}

	public static bool Approximately(Vec2 a, Vec2 b, float epsilon = MathUtil.EPSILON) {
		return MathUtil.Approximately(a.x, b.x, epsilon) && MathUtil.Approximately(a.y, b.y, epsilon);
	}

	public readonly bool Equals(Vec2 other) {
		return x.Equals(other.x) && y.Equals(other.y);
	}

	public override readonly bool Equals(object? obj) {
		return obj is Vec2 other && Equals(other);
	}

	public override readonly int GetHashCode() {
		return HashCode.Combine(x, y);
	}

	public override readonly string ToString() {
		return string.Create(CultureInfo.InvariantCulture, $"({x}, {y})");
	}
}
