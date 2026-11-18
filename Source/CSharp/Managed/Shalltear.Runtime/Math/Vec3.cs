using System;
using System.Globalization;
using System.Runtime.InteropServices;

#pragma warning disable CS1591

namespace Shalltear;

[StructLayout(LayoutKind.Sequential)]
public struct Vec3 : IEquatable<Vec3> {
	public float x;
	public float y;
	public float z;

	public Vec3(float x, float y, float z) {
		this.x = x;
		this.y = y;
		this.z = z;
	}

	public Vec3(float value) {
		x = value;
		y = value;
		z = value;
	}

	public Vec3(Vec2 xy, float z) {
		x = xy.x;
		y = xy.y;
		this.z = z;
	}

	public static Vec3 Zero => new(0.0f, 0.0f, 0.0f);
	public static Vec3 One => new(1.0f, 1.0f, 1.0f);
	public static Vec3 Right => new(1.0f, 0.0f, 0.0f);
	public static Vec3 Left => new(-1.0f, 0.0f, 0.0f);
	public static Vec3 Up => new(0.0f, 1.0f, 0.0f);
	public static Vec3 Down => new(0.0f, -1.0f, 0.0f);
	public static Vec3 Forward => new(0.0f, 0.0f, 1.0f);
	public static Vec3 Back => new(0.0f, 0.0f, -1.0f);

	public float this[int index] {
		readonly get {
			return index switch {
				0 => x,
				1 => y,
				2 => z,
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
			default:
				throw new IndexOutOfRangeException();
			}
		}
	}

	public static bool operator ==(Vec3 a, Vec3 b) {
		return a.x == b.x && a.y == b.y && a.z == b.z;
	}

	public static bool operator !=(Vec3 a, Vec3 b) {
		return !(a == b);
	}

	public static Vec3 operator -(Vec3 value) {
		return new Vec3(-value.x, -value.y, -value.z);
	}

	public static Vec3 operator +(Vec3 a, Vec3 b) {
		return new Vec3(a.x + b.x, a.y + b.y, a.z + b.z);
	}

	public static Vec3 operator -(Vec3 a, Vec3 b) {
		return new Vec3(a.x - b.x, a.y - b.y, a.z - b.z);
	}

	public static Vec3 operator *(Vec3 a, Vec3 b) {
		return new Vec3(a.x * b.x, a.y * b.y, a.z * b.z);
	}

	public static Vec3 operator /(Vec3 a, Vec3 b) {
		return new Vec3(a.x / b.x, a.y / b.y, a.z / b.z);
	}

	public static Vec3 operator *(Vec3 a, float b) {
		return new Vec3(a.x * b, a.y * b, a.z * b);
	}

	public static Vec3 operator *(float a, Vec3 b) {
		return new Vec3(a * b.x, a * b.y, a * b.z);
	}

	public static Vec3 operator /(Vec3 a, float b) {
		return new Vec3(a.x / b, a.y / b, a.z / b);
	}

	public readonly float LengthSquared => x * x + y * y + z * z;

	public readonly float Length => MathF.Sqrt(x * x + y * y + z * z);

	/// <summary>Unit length copy (Zero when length is ~0).</summary>
	public readonly Vec3 Normalized {
		get {
			float length = Length;
			return length > MathUtil.EPSILON ? this / length : Zero;
		}
	}

	public void Normalize() {
		this = Normalized;
	}

	public static float Dot(Vec3 a, Vec3 b) {
		return a.x * b.x + a.y * b.y + a.z * b.z;
	}

	public static Vec3 Cross(Vec3 a, Vec3 b) {
		return new Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
	}

	public static float DistanceSquared(Vec3 a, Vec3 b) {
		return (b - a).LengthSquared;
	}

	public static float Distance(Vec3 a, Vec3 b) {
		return (b - a).Length;
	}

	/// <summary>t clamped to 0-1.</summary>
	public static Vec3 Lerp(Vec3 a, Vec3 b, float t) {
		return LerpUnclamped(a, b, MathUtil.Clamp01(t));
	}

	public static Vec3 LerpUnclamped(Vec3 a, Vec3 b, float t) {
		return a + (b - a) * t;
	}

	public static Vec3 Min(Vec3 a, Vec3 b) {
		return new Vec3(MathUtil.Min(a.x, b.x), MathUtil.Min(a.y, b.y), MathUtil.Min(a.z, b.z));
	}

	public static Vec3 Max(Vec3 a, Vec3 b) {
		return new Vec3(MathUtil.Max(a.x, b.x), MathUtil.Max(a.y, b.y), MathUtil.Max(a.z, b.z));
	}

	public static Vec3 Abs(Vec3 value) {
		return new Vec3(MathUtil.Abs(value.x), MathUtil.Abs(value.y), MathUtil.Abs(value.z));
	}

	public static Vec3 Reflect(Vec3 direction, Vec3 normal) {
		return direction - normal * (2.0f * Dot(direction, normal));
	}

	/// <summary>Zero when onto has ~0 length.</summary>
	public static Vec3 Project(Vec3 value, Vec3 onto) {
		float denominator = Dot(onto, onto);
		if (denominator <= MathUtil.EPSILON) {
			return Zero;
		}
		return onto * (Dot(value, onto) / denominator);
	}

	public static Vec3 ProjectOnPlane(Vec3 value, Vec3 planeNormal) {
		return value - Project(value, planeNormal);
	}

	/// <summary>Unsigned degrees.</summary>
	public static float Angle(Vec3 a, Vec3 b) {
		float denominator = MathF.Sqrt(a.LengthSquared * b.LengthSquared);
		if (denominator <= MathUtil.EPSILON) {
			return 0.0f;
		}
		float cosine = MathUtil.Clamp(Dot(a, b) / denominator, -1.0f, 1.0f);
		return MathF.Acos(cosine) * MathUtil.RAD_TO_DEG;
	}

	public static bool Approximately(Vec3 a, Vec3 b, float epsilon = MathUtil.EPSILON) {
		return MathUtil.Approximately(a.x, b.x, epsilon) && MathUtil.Approximately(a.y, b.y, epsilon) && MathUtil.Approximately(a.z, b.z, epsilon);
	}

	public readonly bool Equals(Vec3 other) {
		return x.Equals(other.x) && y.Equals(other.y) && z.Equals(other.z);
	}

	public override readonly bool Equals(object? obj) {
		return obj is Vec3 other && Equals(other);
	}

	public override readonly int GetHashCode() {
		return HashCode.Combine(x, y, z);
	}

	public override readonly string ToString() {
		return string.Create(CultureInfo.InvariantCulture, $"({x}, {y}, {z})");
	}
}
