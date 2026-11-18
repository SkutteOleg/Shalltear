using System;
using System.Globalization;
using System.Text;

using Shalltear.Interop;

namespace Shalltear.Conformance;

static class MathTests {
	delegate void MathBody(ReadOnlySpan<float> input, Span<float> output);

	const int SETS = 48;

	static Vec2 V2(ReadOnlySpan<float> input, int at) {
		return new Vec2(input[at], input[at + 1]);
	}

	static Vec3 V3(ReadOnlySpan<float> input, int at) {
		return new Vec3(input[at], input[at + 1], input[at + 2]);
	}

	static Vec4 V4(ReadOnlySpan<float> input, int at) {
		return new Vec4(input[at], input[at + 1], input[at + 2], input[at + 3]);
	}

	static Quat Q(ReadOnlySpan<float> input, int at) {
		return new Quat(input[at], input[at + 1], input[at + 2], input[at + 3]);
	}

	static Color C(ReadOnlySpan<float> input, int at) {
		return new Color(input[at], input[at + 1], input[at + 2], input[at + 3]);
	}

	static void Put(Span<float> output, float value) {
		output[0] = value;
	}

	static void Put(Span<float> output, bool value) {
		output[0] = value ? 1.0f : 0.0f;
	}

	static void Put(Span<float> output, Vec2 value) {
		output[0] = value.x;
		output[1] = value.y;
	}

	static void Put(Span<float> output, Vec3 value) {
		output[0] = value.x;
		output[1] = value.y;
		output[2] = value.z;
	}

	static void Put(Span<float> output, Vec4 value) {
		output[0] = value.x;
		output[1] = value.y;
		output[2] = value.z;
		output[3] = value.w;
	}

	static void Put(Span<float> output, Quat value) {
		output[0] = value.x;
		output[1] = value.y;
		output[2] = value.z;
		output[3] = value.w;
	}

	static void Put(Span<float> output, Color value) {
		output[0] = value.r;
		output[1] = value.g;
		output[2] = value.b;
		output[3] = value.a;
	}

	static readonly (string Name, int Inputs, int Outputs, MathBody Body)[] _cases = {
		("Vec2.Add", 4, 2, static (i, o) => Put(o, V2(i, 0) + V2(i, 2))),
		("Vec2.Subtract", 4, 2, static (i, o) => Put(o, V2(i, 0) - V2(i, 2))),
		("Vec2.Multiply", 4, 2, static (i, o) => Put(o, V2(i, 0) * V2(i, 2))),
		("Vec2.Divide", 4, 2, static (i, o) => Put(o, V2(i, 0) / V2(i, 2))),
		("Vec2.Scale", 3, 2, static (i, o) => Put(o, V2(i, 0) * i[2])),
		("Vec2.ScaleLeft", 3, 2, static (i, o) => Put(o, i[2] * V2(i, 0))),
		("Vec2.DivideScalar", 3, 2, static (i, o) => Put(o, V2(i, 0) / i[2])),
		("Vec2.Negate", 2, 2, static (i, o) => Put(o, -V2(i, 0))),
		("Vec2.LengthSquared", 2, 1, static (i, o) => Put(o, V2(i, 0).LengthSquared)),
		("Vec2.Length", 2, 1, static (i, o) => Put(o, V2(i, 0).Length)),
		("Vec2.Normalized", 2, 2, static (i, o) => Put(o, V2(i, 0).Normalized)),
		("Vec2.Dot", 4, 1, static (i, o) => Put(o, Vec2.Dot(V2(i, 0), V2(i, 2)))),
		("Vec2.Cross", 4, 1, static (i, o) => Put(o, Vec2.Cross(V2(i, 0), V2(i, 2)))),
		("Vec2.DistanceSquared", 4, 1, static (i, o) => Put(o, Vec2.DistanceSquared(V2(i, 0), V2(i, 2)))),
		("Vec2.Distance", 4, 1, static (i, o) => Put(o, Vec2.Distance(V2(i, 0), V2(i, 2)))),
		("Vec2.Lerp", 5, 2, static (i, o) => Put(o, Vec2.Lerp(V2(i, 0), V2(i, 2), i[4]))),
		("Vec2.LerpUnclamped", 5, 2, static (i, o) => Put(o, Vec2.LerpUnclamped(V2(i, 0), V2(i, 2), i[4]))),
		("Vec2.Min", 4, 2, static (i, o) => Put(o, Vec2.Min(V2(i, 0), V2(i, 2)))),
		("Vec2.Max", 4, 2, static (i, o) => Put(o, Vec2.Max(V2(i, 0), V2(i, 2)))),
		("Vec2.Abs", 2, 2, static (i, o) => Put(o, Vec2.Abs(V2(i, 0)))),
		("Vec2.Perpendicular", 2, 2, static (i, o) => Put(o, V2(i, 0).Perpendicular)),
		("Vec2.Reflect", 4, 2, static (i, o) => Put(o, Vec2.Reflect(V2(i, 0), V2(i, 2)))),
		("Vec2.Approximately", 5, 1, static (i, o) => Put(o, Vec2.Approximately(V2(i, 0), V2(i, 2), MathUtil.Abs(i[4])))),
		("Vec2.Equals", 4, 1, static (i, o) => Put(o, V2(i, 0) == V2(i, 2))),

		("Vec3.Add", 6, 3, static (i, o) => Put(o, V3(i, 0) + V3(i, 3))),
		("Vec3.Subtract", 6, 3, static (i, o) => Put(o, V3(i, 0) - V3(i, 3))),
		("Vec3.Multiply", 6, 3, static (i, o) => Put(o, V3(i, 0) * V3(i, 3))),
		("Vec3.Divide", 6, 3, static (i, o) => Put(o, V3(i, 0) / V3(i, 3))),
		("Vec3.Scale", 4, 3, static (i, o) => Put(o, V3(i, 0) * i[3])),
		("Vec3.ScaleLeft", 4, 3, static (i, o) => Put(o, i[3] * V3(i, 0))),
		("Vec3.DivideScalar", 4, 3, static (i, o) => Put(o, V3(i, 0) / i[3])),
		("Vec3.Negate", 3, 3, static (i, o) => Put(o, -V3(i, 0))),
		("Vec3.LengthSquared", 3, 1, static (i, o) => Put(o, V3(i, 0).LengthSquared)),
		("Vec3.Length", 3, 1, static (i, o) => Put(o, V3(i, 0).Length)),
		("Vec3.Normalized", 3, 3, static (i, o) => Put(o, V3(i, 0).Normalized)),
		("Vec3.Dot", 6, 1, static (i, o) => Put(o, Vec3.Dot(V3(i, 0), V3(i, 3)))),
		("Vec3.Cross", 6, 3, static (i, o) => Put(o, Vec3.Cross(V3(i, 0), V3(i, 3)))),
		("Vec3.DistanceSquared", 6, 1, static (i, o) => Put(o, Vec3.DistanceSquared(V3(i, 0), V3(i, 3)))),
		("Vec3.Distance", 6, 1, static (i, o) => Put(o, Vec3.Distance(V3(i, 0), V3(i, 3)))),
		("Vec3.Lerp", 7, 3, static (i, o) => Put(o, Vec3.Lerp(V3(i, 0), V3(i, 3), i[6]))),
		("Vec3.LerpUnclamped", 7, 3, static (i, o) => Put(o, Vec3.LerpUnclamped(V3(i, 0), V3(i, 3), i[6]))),
		("Vec3.Min", 6, 3, static (i, o) => Put(o, Vec3.Min(V3(i, 0), V3(i, 3)))),
		("Vec3.Max", 6, 3, static (i, o) => Put(o, Vec3.Max(V3(i, 0), V3(i, 3)))),
		("Vec3.Abs", 3, 3, static (i, o) => Put(o, Vec3.Abs(V3(i, 0)))),
		("Vec3.Reflect", 6, 3, static (i, o) => Put(o, Vec3.Reflect(V3(i, 0), V3(i, 3)))),
		("Vec3.Project", 6, 3, static (i, o) => Put(o, Vec3.Project(V3(i, 0), V3(i, 3)))),
		("Vec3.ProjectOnPlane", 6, 3, static (i, o) => Put(o, Vec3.ProjectOnPlane(V3(i, 0), V3(i, 3)))),
		("Vec3.Angle", 6, 1, static (i, o) => Put(o, Vec3.Angle(V3(i, 0), V3(i, 3)))),
		("Vec3.Approximately", 7, 1, static (i, o) => Put(o, Vec3.Approximately(V3(i, 0), V3(i, 3), MathUtil.Abs(i[6])))),
		("Vec3.Equals", 6, 1, static (i, o) => Put(o, V3(i, 0) == V3(i, 3))),

		("Vec4.Add", 8, 4, static (i, o) => Put(o, V4(i, 0) + V4(i, 4))),
		("Vec4.Subtract", 8, 4, static (i, o) => Put(o, V4(i, 0) - V4(i, 4))),
		("Vec4.Multiply", 8, 4, static (i, o) => Put(o, V4(i, 0) * V4(i, 4))),
		("Vec4.Divide", 8, 4, static (i, o) => Put(o, V4(i, 0) / V4(i, 4))),
		("Vec4.Scale", 5, 4, static (i, o) => Put(o, V4(i, 0) * i[4])),
		("Vec4.ScaleLeft", 5, 4, static (i, o) => Put(o, i[4] * V4(i, 0))),
		("Vec4.DivideScalar", 5, 4, static (i, o) => Put(o, V4(i, 0) / i[4])),
		("Vec4.Negate", 4, 4, static (i, o) => Put(o, -V4(i, 0))),
		("Vec4.LengthSquared", 4, 1, static (i, o) => Put(o, V4(i, 0).LengthSquared)),
		("Vec4.Length", 4, 1, static (i, o) => Put(o, V4(i, 0).Length)),
		("Vec4.Normalized", 4, 4, static (i, o) => Put(o, V4(i, 0).Normalized)),
		("Vec4.Dot", 8, 1, static (i, o) => Put(o, Vec4.Dot(V4(i, 0), V4(i, 4)))),
		("Vec4.Lerp", 9, 4, static (i, o) => Put(o, Vec4.Lerp(V4(i, 0), V4(i, 4), i[8]))),
		("Vec4.LerpUnclamped", 9, 4, static (i, o) => Put(o, Vec4.LerpUnclamped(V4(i, 0), V4(i, 4), i[8]))),
		("Vec4.Min", 8, 4, static (i, o) => Put(o, Vec4.Min(V4(i, 0), V4(i, 4)))),
		("Vec4.Max", 8, 4, static (i, o) => Put(o, Vec4.Max(V4(i, 0), V4(i, 4)))),
		("Vec4.Approximately", 9, 1, static (i, o) => Put(o, Vec4.Approximately(V4(i, 0), V4(i, 4), MathUtil.Abs(i[8])))),
		("Vec4.Equals", 8, 1, static (i, o) => Put(o, V4(i, 0) == V4(i, 4))),
		("Vec4.XYZ", 4, 3, static (i, o) => Put(o, V4(i, 0).XYZ)),

		("Quat.Multiply", 8, 4, static (i, o) => Put(o, Q(i, 0) * Q(i, 4))),
		("Quat.Rotate", 7, 3, static (i, o) => Put(o, Q(i, 0) * V3(i, 4))),
		("Quat.Negate", 4, 4, static (i, o) => Put(o, -Q(i, 0))),
		("Quat.Scale", 5, 4, static (i, o) => Put(o, Q(i, 0) * i[4])),
		("Quat.Add", 8, 4, static (i, o) => Put(o, Q(i, 0) + Q(i, 4))),
		("Quat.Dot", 8, 1, static (i, o) => Put(o, Quat.Dot(Q(i, 0), Q(i, 4)))),
		("Quat.LengthSquared", 4, 1, static (i, o) => Put(o, Q(i, 0).LengthSquared)),
		("Quat.Length", 4, 1, static (i, o) => Put(o, Q(i, 0).Length)),
		("Quat.Normalized", 4, 4, static (i, o) => Put(o, Q(i, 0).Normalized)),
		("Quat.Conjugated", 4, 4, static (i, o) => Put(o, Q(i, 0).Conjugated)),
		("Quat.Inverse", 4, 4, static (i, o) => Put(o, Q(i, 0).Inverse)),
		("Quat.AngleAxis", 4, 4, static (i, o) => Put(o, Quat.AngleAxis(i[0] * 60.0f, V3(i, 1)))),
		("Quat.FromEuler", 3, 4, static (i, o) => Put(o, Quat.FromEuler(V3(i, 0) * 60.0f))),
		("Quat.FromEulerXYZ", 3, 4, static (i, o) => Put(o, Quat.FromEuler(V3(i, 0) * 60.0f, RotationOrder.XYZ))),
		("Quat.FromEulerXZY", 3, 4, static (i, o) => Put(o, Quat.FromEuler(V3(i, 0) * 60.0f, RotationOrder.XZY))),
		("Quat.FromEulerYXZ", 3, 4, static (i, o) => Put(o, Quat.FromEuler(V3(i, 0) * 60.0f, RotationOrder.YXZ))),
		("Quat.FromEulerYZX", 3, 4, static (i, o) => Put(o, Quat.FromEuler(V3(i, 0) * 60.0f, RotationOrder.YZX))),
		("Quat.FromEulerZXY", 3, 4, static (i, o) => Put(o, Quat.FromEuler(V3(i, 0) * 60.0f, RotationOrder.ZXY))),
		("Quat.FromEulerZYX", 3, 4, static (i, o) => Put(o, Quat.FromEuler(V3(i, 0) * 60.0f, RotationOrder.ZYX))),
		("Quat.ToEuler", 4, 3, static (i, o) => Put(o, Q(i, 0).ToEuler())),
		("Quat.FromBasis", 9, 4, static (i, o) => Put(o, Quat.FromBasis(V3(i, 0), V3(i, 3), V3(i, 6)))),
		("Quat.LookRotation", 6, 4, static (i, o) => Put(o, Quat.LookRotation(V3(i, 0), V3(i, 3)))),
		("Quat.LookRotationUp", 3, 4, static (i, o) => Put(o, Quat.LookRotation(V3(i, 0)))),
		("Quat.Angle", 8, 1, static (i, o) => Put(o, Quat.Angle(Q(i, 0), Q(i, 4)))),
		("Quat.Lerp", 9, 4, static (i, o) => Put(o, Quat.Lerp(Q(i, 0), Q(i, 4), i[8]))),
		("Quat.LerpUnclamped", 9, 4, static (i, o) => Put(o, Quat.LerpUnclamped(Q(i, 0), Q(i, 4), i[8]))),
		("Quat.Slerp", 9, 4, static (i, o) => Put(o, Quat.Slerp(Q(i, 0), Q(i, 4), i[8]))),
		("Quat.SlerpUnclamped", 9, 4, static (i, o) => Put(o, Quat.SlerpUnclamped(Q(i, 0), Q(i, 4), i[8]))),
		("Quat.Approximately", 9, 1, static (i, o) => Put(o, Quat.Approximately(Q(i, 0), Q(i, 4), MathUtil.Abs(i[8])))),
		("Quat.Equals", 8, 1, static (i, o) => Put(o, Q(i, 0) == Q(i, 4))),

		("Color.Add", 8, 4, static (i, o) => Put(o, C(i, 0) + C(i, 4))),
		("Color.Subtract", 8, 4, static (i, o) => Put(o, C(i, 0) - C(i, 4))),
		("Color.Multiply", 8, 4, static (i, o) => Put(o, C(i, 0) * C(i, 4))),
		("Color.Scale", 5, 4, static (i, o) => Put(o, C(i, 0) * i[4])),
		("Color.ScaleLeft", 5, 4, static (i, o) => Put(o, i[4] * C(i, 0))),
		("Color.Lerp", 9, 4, static (i, o) => Put(o, Color.Lerp(C(i, 0), C(i, 4), i[8]))),
		("Color.LerpUnclamped", 9, 4, static (i, o) => Put(o, Color.LerpUnclamped(C(i, 0), C(i, 4), i[8]))),
		("Color.Approximately", 9, 1, static (i, o) => Put(o, Color.Approximately(C(i, 0), C(i, 4), MathUtil.Abs(i[8])))),
		("Color.Saturated", 4, 4, static (i, o) => Put(o, C(i, 0).Saturated)),
		("Color.Equals", 8, 1, static (i, o) => Put(o, C(i, 0) == C(i, 4))),
	};

	static float Next(ref uint seed) {
		seed = seed * 1664525u + 1013904223u;
		return ((seed >> 8) & 0xFFFFFF) / 16777216.0f * 6.0f - 3.0f;
	}

	static void Fill(Span<float> values, int set, ref uint seed, bool rotations) {
		for (int index = 0; index < values.Length; ++index) {
			values[index] = set switch {
				0 => 0.0f,
				1 => 1.0f,
				2 => index % 3 == 0 ? 1.0f : 0.0f,
				3 => Next(ref seed) * 1e-6f,
				4 => Next(ref seed) * 1000.0f,
				7 => Next(ref seed) * 3e-5f,
				8 => Next(ref seed) * 2e-3f,
				_ => Next(ref seed),
			};
		}
		if (rotations && (set == 6 || (set >= 9 && set < 24))) {
			for (int start = 0; start + 4 <= values.Length && start < 8; start += 4) {
				Quat unit = new Quat(values[start], values[start + 1], values[start + 2], values[start + 3]).Normalized;
				values[start] = unit.x;
				values[start + 1] = unit.y;
				values[start + 2] = unit.z;
				values[start + 3] = unit.w;
			}
		}
		if (set == 5 || set == 6) {
			int half = values.Length / 2;
			for (int index = 0; index < half; ++index) {
				values[half + index] = values[index];
			}
		}
	}

	static bool Close(float expected, float actual) {
		if (float.IsNaN(expected) || float.IsNaN(actual)) {
			return float.IsNaN(expected) && float.IsNaN(actual);
		}
		if (float.IsInfinity(expected) || float.IsInfinity(actual)) {
			return expected == actual;
		}
		float scale = MathF.Max(1.0f, MathF.Max(MathF.Abs(expected), MathF.Abs(actual)));
		return MathF.Abs(expected - actual) <= 1e-6f * scale;
	}

	static string Describe(ReadOnlySpan<float> values) {
		StringBuilder text = new();
		foreach (float value in values) {
			text.Append(text.Length == 0 ? "" : " ").Append(value.ToString("R", CultureInfo.InvariantCulture));
		}
		return text.ToString();
	}

	[ConformanceTest]
	internal static void MatchesNative() {
		Check.Equal(_cases.Length, BindingProbe.GetMathCaseCount());
		Span<float> input = stackalloc float[9];
		Span<float> expected = stackalloc float[4];
		Span<float> actual = stackalloc float[4];
		uint seed = 0x9E3779B9u;
		foreach ((string name, int inputs, int outputs, MathBody body) in _cases) {
			bool rotations = name.StartsWith("Quat.", StringComparison.Ordinal);
			for (int set = 0; set < SETS; ++set) {
				Span<float> values = input.Slice(0, inputs);
				Fill(values, set, ref seed, rotations);
				expected.Clear();
				actual.Clear();
				int written = BindingProbe.EvaluateMath(name, values, expected);
				Check.True(written == outputs, $"{name}: native wrote {written} value(s), C# has {outputs}");
				body(values, actual);
				for (int index = 0; index < outputs; ++index) {
					if (!Close(expected[index], actual[index])) {
						Check.Fail($"{name} output {index}: native {expected[index]:R}, C# {actual[index]:R} for {Describe(values)}");
					}
				}
			}
		}
	}

	[ConformanceTest]
	internal static void UnknownMathCaseIsRefused() {
		Span<float> one = stackalloc float[1];
		Check.Equal(-1, BindingProbe.EvaluateMath("Vec9.Add", one, one));
		Check.Equal(-1, BindingProbe.EvaluateMath("Vec2.Add", one, one));
	}

	[ConformanceTest]
	internal static void HexMatchesKnownValues() {
		Check.Equal(Color.Red, Color.FromHex("#ff0000"));
		Check.Equal(new Color(0.0f, 1.0f, 0.0f, 8.0f * (1.0f / 15.0f)), Color.FromHex("0F08"));
		Check.Equal(Color.Magenta, Color.FromHex("zzz"));
		Check.Equal(false, Color.TryParseHex("#12", out _));
		Check.Equal(true, Color.TryParseHex("#80808080", out Color gray));
		Check.Equal(128.0f * (1.0f / 255.0f), gray.a);
		Check.Equal("#ff8000ff", new Color(1.0f, 0.5f, 0.0f).ToHex());
		Check.Equal("#ff0080ff", new Color(2.0f, -1.0f, 0.5f).ToHex());
	}

	[ConformanceTest]
	internal static void TextUsesInvariantCulture() {
		CultureInfo before = CultureInfo.CurrentCulture;
		try {
			try {
				CultureInfo.CurrentCulture = new CultureInfo("de-DE");
			} catch (CultureNotFoundException) {
			}
			Check.Equal("(1, 2.5)", new Vec2(1.0f, 2.5f).ToString());
			Check.Equal("(1, 2.5, -3)", new Vec3(1.0f, 2.5f, -3.0f).ToString());
			Check.Equal("(1, 2.5, -3, 0.25)", new Vec4(1.0f, 2.5f, -3.0f, 0.25f).ToString());
			Check.Equal("(0, 0, 0, 1)", Quat.Identity.ToString());
			Check.Equal("(1, 0.5, 0, 1)", new Color(1.0f, 0.5f, 0.0f).ToString());
		} finally {
			CultureInfo.CurrentCulture = before;
		}
	}

	[ConformanceTest]
	internal static void IndexersReachEachComponent() {
		Vec4 value = new(1.0f, 2.0f, 3.0f, 4.0f);
		value[3] = 9.0f;
		Check.Equal(new Vec4(1.0f, 2.0f, 3.0f, 9.0f), value);
		Check.Equal(3.0f, value[2]);
		Vec3 three = value.XYZ;
		three[0] = 7.0f;
		Check.Equal(new Vec3(7.0f, 2.0f, 3.0f), three);
		Vec2 two = new(5.0f, 6.0f);
		two[1] = 8.0f;
		Check.Equal(8.0f, two[1]);
		try {
			_ = three[3];
		} catch (IndexOutOfRangeException) {
			return;
		}
		Check.Fail("no IndexOutOfRangeException");
	}

	[ConformanceTest]
	internal static void EqualsAndOperatorDisagreeOnNaN() {
		Vec3 value = new(float.NaN, 1.0f, 2.0f);
		Check.Equal(true, value.Equals(value));
		Vec3 same = value;
		Check.Equal(false, value == same);
		Check.Equal(value.GetHashCode(), new Vec3(float.NaN, 1.0f, 2.0f).GetHashCode());
		Check.Equal(default(Quat), new Quat(0.0f, 0.0f, 0.0f, 0.0f));
	}
}
