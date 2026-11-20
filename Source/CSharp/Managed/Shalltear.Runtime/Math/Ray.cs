using System;
using System.Runtime.InteropServices;

#pragma warning disable CS1591

namespace Shalltear;

[StructLayout(LayoutKind.Sequential)]
public struct Ray : IEquatable<Ray> {
	public Vec3 origin;
	public Vec3 direction;

	public Ray(Vec3 origin, Vec3 direction) {
		this.origin = origin;
		this.direction = direction.Normalized;
	}

	public readonly Vec3 GetPoint(float distance) {
		return origin + direction * distance;
	}

	public static bool operator ==(Ray a, Ray b) {
		return a.origin == b.origin && a.direction == b.direction;
	}

	public static bool operator !=(Ray a, Ray b) {
		return !(a == b);
	}

	public readonly bool Equals(Ray other) {
		return this == other;
	}

	public override readonly bool Equals(object? other) {
		return other is Ray value && this == value;
	}

	public override readonly int GetHashCode() {
		return HashCode.Combine(origin, direction);
	}
}
