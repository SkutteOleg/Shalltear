using System;
using System.Globalization;
using System.Runtime.InteropServices;

#pragma warning disable CS1591

namespace Shalltear;

[StructLayout(LayoutKind.Sequential)]
public struct Color : IEquatable<Color> {
	public float r;
	public float g;
	public float b;
	public float a;

	public Color(float r, float g, float b, float a = 1.0f) {
		this.r = r;
		this.g = g;
		this.b = b;
		this.a = a;
	}

	public static Color White => new(1.0f, 1.0f, 1.0f);
	public static Color Black => new(0.0f, 0.0f, 0.0f);
	public static Color Red => new(1.0f, 0.0f, 0.0f);
	public static Color Green => new(0.0f, 1.0f, 0.0f);
	public static Color Blue => new(0.0f, 0.0f, 1.0f);
	public static Color Yellow => new(1.0f, 1.0f, 0.0f);
	public static Color Cyan => new(0.0f, 1.0f, 1.0f);
	public static Color Magenta => new(1.0f, 0.0f, 1.0f);
	public static Color Gray => new(0.5f, 0.5f, 0.5f);
	public static Color Clear => new(0.0f, 0.0f, 0.0f, 0.0f);

	public static bool operator ==(Color x, Color y) {
		return x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a;
	}

	public static bool operator !=(Color x, Color y) {
		return !(x == y);
	}

	public static Color operator +(Color x, Color y) {
		return new Color(x.r + y.r, x.g + y.g, x.b + y.b, x.a + y.a);
	}

	public static Color operator -(Color x, Color y) {
		return new Color(x.r - y.r, x.g - y.g, x.b - y.b, x.a - y.a);
	}

	public static Color operator *(Color x, Color y) {
		return new Color(x.r * y.r, x.g * y.g, x.b * y.b, x.a * y.a);
	}

	public static Color operator *(Color x, float y) {
		return new Color(x.r * y, x.g * y, x.b * y, x.a * y);
	}

	public static Color operator *(float x, Color y) {
		return y * x;
	}

	/// <summary>t clamped to 0-1.</summary>
	public static Color Lerp(Color x, Color y, float t) {
		return LerpUnclamped(x, y, MathUtil.Clamp01(t));
	}

	public static Color LerpUnclamped(Color x, Color y, float t) {
		return x + (y - x) * t;
	}

	public static bool Approximately(Color x, Color y, float epsilon = MathUtil.EPSILON) {
		return MathUtil.Approximately(x.r, y.r, epsilon) && MathUtil.Approximately(x.g, y.g, epsilon) && MathUtil.Approximately(x.b, y.b, epsilon) &&
			   MathUtil.Approximately(x.a, y.a, epsilon);
	}

	/// <summary>Channels clamped to 0-1.</summary>
	public readonly Color Saturated => new(MathUtil.Clamp01(r), MathUtil.Clamp01(g), MathUtil.Clamp01(b), MathUtil.Clamp01(a));

	/// <summary>RGB, RGBA, RRGGBB or RRGGBBAA hex digits (leading # allowed).</summary>
	public static bool TryParseHex(ReadOnlySpan<char> text, out Color color) {
		color = default;
		if (!text.IsEmpty && text[0] == '#') {
			text = text.Slice(1);
		}
		int length = text.Length;
		if (length != 3 && length != 4 && length != 6 && length != 8) {
			return false;
		}

		uint value = 0;
		foreach (char character in text) {
			int digit;
			if (character >= '0' && character <= '9') {
				digit = character - '0';
			} else if (character >= 'a' && character <= 'f') {
				digit = character - 'a' + 10;
			} else if (character >= 'A' && character <= 'F') {
				digit = character - 'A' + 10;
			} else {
				return false;
			}
			value = (value << 4) | (uint)digit;
		}

		const float INV15 = 1.0f / 15.0f;
		const float INV255 = 1.0f / 255.0f;
		switch (length) {
		case 3:
			color = new Color(((value >> 8) & 0xF) * INV15, ((value >> 4) & 0xF) * INV15, (value & 0xF) * INV15);
			return true;
		case 4:
			color = new Color(((value >> 12) & 0xF) * INV15, ((value >> 8) & 0xF) * INV15, ((value >> 4) & 0xF) * INV15, (value & 0xF) * INV15);
			return true;
		case 6:
			color = new Color(((value >> 16) & 0xFF) * INV255, ((value >> 8) & 0xFF) * INV255, (value & 0xFF) * INV255);
			return true;
		default:
			color = new Color(((value >> 24) & 0xFF) * INV255, ((value >> 16) & 0xFF) * INV255, ((value >> 8) & 0xFF) * INV255, (value & 0xFF) * INV255);
			return true;
		}
	}

	/// <summary>Magenta when text doesn't parse.</summary>
	public static Color FromHex(ReadOnlySpan<char> text) {
		return TryParseHex(text, out Color color) ? color : Magenta;
	}

	/// <summary>#rrggbbaa, channels clamped to 0-1.</summary>
	public readonly string ToHex() {
		return string.Create(CultureInfo.InvariantCulture, $"#{ToByte(r):x2}{ToByte(g):x2}{ToByte(b):x2}{ToByte(a):x2}");
	}

	/// <summary>8 bits per channel, channels clamped to 0-1.</summary>
	public readonly Color32 ToColor32() {
		return new Color32 { r = ToByte(r), g = ToByte(g), b = ToByte(b), a = ToByte(a) };
	}

	static byte ToByte(float channel) {
		return (byte)MathF.Round(MathUtil.Clamp01(channel) * 255.0f, MidpointRounding.AwayFromZero);
	}

	public readonly bool Equals(Color other) {
		return r.Equals(other.r) && g.Equals(other.g) && b.Equals(other.b) && a.Equals(other.a);
	}

	public override readonly bool Equals(object? obj) {
		return obj is Color other && Equals(other);
	}

	public override readonly int GetHashCode() {
		return HashCode.Combine(r, g, b, a);
	}

	public override readonly string ToString() {
		return string.Create(CultureInfo.InvariantCulture, $"({r}, {g}, {b}, {a})");
	}
}
