using System;
using System.Globalization;
using System.Runtime.InteropServices;

#pragma warning disable CS1591

namespace Shalltear;

[StructLayout(LayoutKind.Sequential)]
public struct Vec4 : IEquatable<Vec4> {
	public float x;
	public float y;
	public float z;
	public float w;

	public Vec4(float x, float y, float z, float w) {
		this.x = x;
		this.y = y;
		this.z = z;
		this.w = w;
	}

	public Vec4(float value) {
		x = value;
		y = value;
		z = value;
		w = value;
	}

	public Vec4(Vec3 xyz, float w) {
		x = xyz.x;
		y = xyz.y;
		z = xyz.z;
		this.w = w;
	}

	public static Vec4 Zero => new(0.0f, 0.0f, 0.0f, 0.0f);
	public static Vec4 One => new(1.0f, 1.0f, 1.0f, 1.0f);

	public float this[int index] {
		readonly get {
			return index switch {
				0 => x,
				1 => y,
				2 => z,
				3 => w,
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
			case 2:
				z = value;
				break;
			case 3:
				w = value;
				break;
			default:
				throw new IndexOutOfRangeException();
			}
		}
	}

	public readonly Vec3 XYZ => new(x, y, z);

	public static bool operator ==(Vec4 a, Vec4 b) {
		return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
	}

	public static bool operator !=(Vec4 a, Vec4 b) {
		return !(a == b);
	}

	public static Vec4 operator -(Vec4 value) {
		return new Vec4(-value.x, -value.y, -value.z, -value.w);
	}

	public static Vec4 operator +(Vec4 a, Vec4 b) {
		return new Vec4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w);
	}

	public static Vec4 operator -(Vec4 a, Vec4 b) {
		return new Vec4(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w);
	}

	public static Vec4 operator *(Vec4 a, Vec4 b) {
		return new Vec4(a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w);
	}

	public static Vec4 operator /(Vec4 a, Vec4 b) {
		return new Vec4(a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w);
	}

	public static Vec4 operator *(Vec4 a, float b) {
		return new Vec4(a.x * b, a.y * b, a.z * b, a.w * b);
	}

	public static Vec4 operator *(float a, Vec4 b) {
		return new Vec4(a * b.x, a * b.y, a * b.z, a * b.w);
	}

	public static Vec4 operator /(Vec4 a, float b) {
		return new Vec4(a.x / b, a.y / b, a.z / b, a.w / b);
	}

	public readonly float LengthSquared => x * x + y * y + z * z + w * w;

	public readonly float Length => MathF.Sqrt(x * x + y * y + z * z + w * w);

	/// <summary>Unit length copy (Zero when length is ~0).</summary>
	public readonly Vec4 Normalized {
		get {
			float length = Length;
			return length > MathUtil.EPSILON ? this / length : Zero;
		}
	}

	public void Normalize() {
		this = Normalized;
	}

	public static float Dot(Vec4 a, Vec4 b) {
		return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
	}

	/// <summary>t clamped to 0-1.</summary>
	public static Vec4 Lerp(Vec4 a, Vec4 b, float t) {
		return LerpUnclamped(a, b, MathUtil.Clamp01(t));
	}

	public static Vec4 LerpUnclamped(Vec4 a, Vec4 b, float t) {
		return a + (b - a) * t;
	}

	public static Vec4 Min(Vec4 a, Vec4 b) {
		return new Vec4(MathUtil.Min(a.x, b.x), MathUtil.Min(a.y, b.y), MathUtil.Min(a.z, b.z), MathUtil.Min(a.w, b.w));
	}

	public static Vec4 Max(Vec4 a, Vec4 b) {
		return new Vec4(MathUtil.Max(a.x, b.x), MathUtil.Max(a.y, b.y), MathUtil.Max(a.z, b.z), MathUtil.Max(a.w, b.w));
	}

	public static bool Approximately(Vec4 a, Vec4 b, float epsilon = MathUtil.EPSILON) {
		return MathUtil.Approximately(a.x, b.x, epsilon) && MathUtil.Approximately(a.y, b.y, epsilon) && MathUtil.Approximately(a.z, b.z, epsilon) &&
			   MathUtil.Approximately(a.w, b.w, epsilon);
	}

	public readonly bool Equals(Vec4 other) {
		return x.Equals(other.x) && y.Equals(other.y) && z.Equals(other.z) && w.Equals(other.w);
	}

	public override readonly bool Equals(object? obj) {
		return obj is Vec4 other && Equals(other);
	}

	public override readonly int GetHashCode() {
		return HashCode.Combine(x, y, z, w);
	}

	public override readonly string ToString() {
		return string.Create(CultureInfo.InvariantCulture, $"({x}, {y}, {z}, {w})");
	}
}
