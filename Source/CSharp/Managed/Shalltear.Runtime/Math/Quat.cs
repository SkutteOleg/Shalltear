using System;
using System.Globalization;
using System.Runtime.InteropServices;

#pragma warning disable CS1591

namespace Shalltear;

public enum RotationOrder : byte {
	XYZ,
	XZY,
	YXZ,
	YZX,
	ZXY,
	ZYX,
	Default = ZXY,
}

/// <summary>Rotation. default(Quat) is all 0, start from Identity.</summary>
[StructLayout(LayoutKind.Sequential)]
public struct Quat : IEquatable<Quat> {
	public float x;
	public float y;
	public float z;
	public float w;

	public Quat(float x, float y, float z, float w) {
		this.x = x;
		this.y = y;
		this.z = z;
		this.w = w;
	}

	public static Quat Identity => new(0.0f, 0.0f, 0.0f, 1.0f);

	public static bool operator ==(Quat a, Quat b) {
		return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
	}

	public static bool operator !=(Quat a, Quat b) {
		return !(a == b);
	}

	/// <summary>b applied 1st, then a.</summary>
	public static Quat operator *(Quat a, Quat b) {
		return new Quat(
			a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
			a.w * b.y + a.y * b.w + a.z * b.x - a.x * b.z,
			a.w * b.z + a.z * b.w + a.x * b.y - a.y * b.x,
			a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
	}

	public static Vec3 operator *(Quat rotation, Vec3 value) {
		float xx = rotation.x * 2.0f;
		float yy = rotation.y * 2.0f;
		float zz = rotation.z * 2.0f;
		float xX = rotation.x * xx;
		float yY = rotation.y * yy;
		float zZ = rotation.z * zz;
		float xY = rotation.x * yy;
		float xZ = rotation.x * zz;
		float yZ = rotation.y * zz;
		float wX = rotation.w * xx;
		float wY = rotation.w * yy;
		float wZ = rotation.w * zz;
		return new Vec3(
			(1.0f - (yY + zZ)) * value.x + (xY - wZ) * value.y + (xZ + wY) * value.z,
			(xY + wZ) * value.x + (1.0f - (xX + zZ)) * value.y + (yZ - wX) * value.z,
			(xZ - wY) * value.x + (yZ + wX) * value.y + (1.0f - (xX + yY)) * value.z);
	}

	public static Quat operator -(Quat value) {
		return new Quat(-value.x, -value.y, -value.z, -value.w);
	}

	public static Quat operator *(Quat a, float b) {
		return new Quat(a.x * b, a.y * b, a.z * b, a.w * b);
	}

	public static Quat operator +(Quat a, Quat b) {
		return new Quat(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w);
	}

	public static float Dot(Quat a, Quat b) {
		return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
	}

	public readonly float LengthSquared => Dot(this, this);

	public readonly float Length => MathF.Sqrt(LengthSquared);

	/// <summary>Unit length copy (Identity when length is ~0).</summary>
	public readonly Quat Normalized {
		get {
			float length = Length;
			if (length <= MathUtil.EPSILON) {
				return Identity;
			}
			float inverse = 1.0f / length;
			return new Quat(x * inverse, y * inverse, z * inverse, w * inverse);
		}
	}

	public void Normalize() {
		this = Normalized;
	}

	public readonly Quat Conjugated => new(-x, -y, -z, w);

	public readonly Quat Inverse {
		get {
			float lengthSquared = LengthSquared;
			if (lengthSquared <= MathUtil.EPSILON) {
				return Identity;
			}
			float inverse = 1.0f / lengthSquared;
			return new Quat(-x * inverse, -y * inverse, -z * inverse, w * inverse);
		}
	}

	public static Quat AngleAxis(float degrees, Vec3 axis) {
		Vec3 unitAxis = axis.Normalized;
		if (unitAxis == Vec3.Zero) {
			return Identity;
		}
		float half = degrees * MathUtil.DEG_TO_RAD * 0.5f;
		float sine = MathF.Sin(half);
		return new Quat(unitAxis.x * sine, unitAxis.y * sine, unitAxis.z * sine, MathF.Cos(half));
	}

	/// <summary>Degrees. Default order is ZXY (Z applied 1st).</summary>
	public static Quat FromEuler(Vec3 eulerDegrees, RotationOrder order = RotationOrder.Default) {
		Vec3 half = eulerDegrees * (MathUtil.DEG_TO_RAD * 0.5f);
		Quat qX = new(MathF.Sin(half.x), 0.0f, 0.0f, MathF.Cos(half.x));
		Quat qY = new(0.0f, MathF.Sin(half.y), 0.0f, MathF.Cos(half.y));
		Quat qZ = new(0.0f, 0.0f, MathF.Sin(half.z), MathF.Cos(half.z));
		return order switch {
			RotationOrder.XYZ => qZ * qY * qX,
			RotationOrder.XZY => qY * qZ * qX,
			RotationOrder.YXZ => qZ * qX * qY,
			RotationOrder.YZX => qX * qZ * qY,
			RotationOrder.ZXY => qY * qX * qZ,
			RotationOrder.ZYX => qX * qY * qZ,
			_ => Identity,
		};
	}

	/// <summary>Degrees. Default order is ZXY (Z applied 1st).</summary>
	public static Quat FromEuler(float xDegrees, float yDegrees, float zDegrees, RotationOrder order = RotationOrder.Default) {
		return FromEuler(new Vec3(xDegrees, yDegrees, zDegrees), order);
	}

	/// <summary>Degrees in ZXY order.</summary>
	public readonly Vec3 ToEuler() {
		float xx = x * x;
		float yy = y * y;
		float zz = z * z;
		float ww = w * w;
		float m02 = 2.0f * (x * z + w * y);
		float m10 = 2.0f * (x * y + w * z);
		float m11 = (ww - xx) + (yy - zz);
		float m12 = 2.0f * (y * z - w * x);
		float m22 = (ww - xx) - (yy - zz);

		float cosineX = MathF.Sqrt(m10 * m10 + m11 * m11);
		float angleX = MathF.Atan2(-m12, cosineX);

		float angleY;
		float angleZ;
		if (cosineX > 0.01414f * (xx + yy + zz + ww)) {
			angleY = MathF.Atan2(m02, m22);
			angleZ = MathF.Atan2(m10, m11);
		} else {
			float m00 = (ww + xx) - (yy + zz);
			float m20 = 2.0f * (x * z - w * y);
			angleY = MathF.Atan2(-m20, m00);
			angleZ = 0.0f;
		}
		return new Vec3(angleX, angleY, angleZ) * MathUtil.RAD_TO_DEG;
	}

	public static Quat FromBasis(Vec3 right, Vec3 up, Vec3 forward) {
		float m00 = right.x, m01 = up.x, m02 = forward.x;
		float m10 = right.y, m11 = up.y, m12 = forward.y;
		float m20 = right.z, m21 = up.z, m22 = forward.z;

		float trace = m00 + m11 + m22;
		float s;
		if (trace > 0.0f) {
			s = MathF.Sqrt(trace + 1.0f) * 2.0f;
			return new Quat((m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s);
		}
		if (m00 > m11 && m00 > m22) {
			s = MathF.Sqrt(1.0f + m00 - m11 - m22) * 2.0f;
			return new Quat(0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s);
		}
		if (m11 > m22) {
			s = MathF.Sqrt(1.0f + m11 - m00 - m22) * 2.0f;
			return new Quat((m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s);
		}
		s = MathF.Sqrt(1.0f + m22 - m00 - m11) * 2.0f;
		return new Quat((m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s);
	}

	public static Quat LookRotation(Vec3 forward) {
		return LookRotation(forward, Vec3.Up);
	}

	/// <summary>Identity when forward has ~0 length.</summary>
	public static Quat LookRotation(Vec3 forward, Vec3 up) {
		Vec3 f = forward.Normalized;
		if (f == Vec3.Zero) {
			return Identity;
		}

		Vec3 r = Vec3.Cross(up, f);
		if (r.LengthSquared <= MathUtil.EPSILON) {
			r = Vec3.Cross(Vec3.Forward, f);
			if (r.LengthSquared <= MathUtil.EPSILON) {
				r = Vec3.Right;
			}
		}
		r.Normalize();
		Vec3 u = Vec3.Cross(f, r);
		return FromBasis(r, u, f);
	}

	/// <summary>Unsigned degrees.</summary>
	public static float Angle(Quat a, Quat b) {
		float dot = MathUtil.Min(MathUtil.Abs(Dot(a, b)), 1.0f);
		if (dot > 1.0f - 1e-6f) {
			return 0.0f;
		}
		return 2.0f * MathF.Acos(dot) * MathUtil.RAD_TO_DEG;
	}

	/// <summary>Normalized lerp, t clamped to 0-1.</summary>
	public static Quat Lerp(Quat a, Quat b, float t) {
		return LerpUnclamped(a, b, MathUtil.Clamp01(t));
	}

	public static Quat LerpUnclamped(Quat a, Quat b, float t) {
		Quat end = Dot(a, b) < 0.0f ? -b : b;
		return (a * (1.0f - t) + end * t).Normalized;
	}

	/// <summary>t clamped to 0-1.</summary>
	public static Quat Slerp(Quat a, Quat b, float t) {
		return SlerpUnclamped(a, b, MathUtil.Clamp01(t));
	}

	public static Quat SlerpUnclamped(Quat a, Quat b, float t) {
		float dot = Dot(a, b);
		Quat end = b;
		if (dot < 0.0f) {
			end = -b;
			dot = -dot;
		}

		if (dot > 0.9995f) {
			return (a * (1.0f - t) + end * t).Normalized;
		}

		float theta = MathF.Acos(MathUtil.Clamp(dot, -1.0f, 1.0f));
		float sinTheta = MathF.Sin(theta);
		float weightA = MathF.Sin((1.0f - t) * theta) / sinTheta;
		float weightB = MathF.Sin(t * theta) / sinTheta;
		return a * weightA + end * weightB;
	}

	/// <summary>Same rotation within epsilon (q and -q match).</summary>
	public static bool Approximately(Quat a, Quat b, float epsilon = MathUtil.EPSILON) {
		return MathUtil.Abs(Dot(a, b)) >= 1.0f - epsilon;
	}

	public readonly bool Equals(Quat other) {
		return x.Equals(other.x) && y.Equals(other.y) && z.Equals(other.z) && w.Equals(other.w);
	}

	public override readonly bool Equals(object? obj) {
		return obj is Quat other && Equals(other);
	}

	public override readonly int GetHashCode() {
		return HashCode.Combine(x, y, z, w);
	}

	public override readonly string ToString() {
		return string.Create(CultureInfo.InvariantCulture, $"({x}, {y}, {z}, {w})");
	}
}
