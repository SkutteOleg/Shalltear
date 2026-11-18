using System;
using System.Threading;

using Shalltear.Interop;

namespace Shalltear.Conformance;

static class BindingTests {
	static double MixExpected(bool flag, int count, float scale, Vec3 position, double weight, Vec4 plane, ulong mask, Quat rotation, Color tint, Vec2 offset) {
		return (flag ? 1.0 : 0.0) + 2.0 * count + 3.0 * scale + 5.0 * position.x + 7.0 * position.y + 11.0 * position.z + 13.0 * weight +
			   17.0 * plane.x + 19.0 * plane.y + 23.0 * plane.z + 29.0 * plane.w + 31.0 * mask + 37.0 * rotation.x + 41.0 * rotation.y +
			   43.0 * rotation.z + 47.0 * rotation.w + 53.0 * tint.r + 59.0 * tint.g + 61.0 * tint.b + 67.0 * tint.a + 71.0 * offset.x + 73.0 * offset.y;
	}

	[ConformanceTest]
	internal static void ScalarsRoundTrip() {
		Check.Equal(false, BindingProbe.EchoBool(true));
		Check.Equal(true, BindingProbe.EchoBool(false));
		Check.Equal(42, BindingProbe.EchoI32(41));
		Check.Equal(-7, BindingProbe.EchoI32(-8));
		Check.Equal(0xFFFF0000u, BindingProbe.EchoU32(0x0000FFFFu));
		Check.Equal(-5_000_000_000L, BindingProbe.EchoI64(5_000_000_000L));
		Check.Equal(0xFFFF0000FFFF0000ul ^ 0x123456789ABCDEF0ul, BindingProbe.EchoU64(0x123456789ABCDEF0ul));
		Check.Equal(1.25f, BindingProbe.EchoF32(2.5f));
		Check.Equal(0.3125, BindingProbe.EchoF64(1.25));
	}

	[ConformanceTest]
	internal static void ValuesRoundTrip() {
		Check.Equal(new Vec2(4.0f, 2.0f), BindingProbe.EchoVec2(new Vec2(1.0f, 2.0f)));
		Check.Equal(new Vec3(6.0f, 2.0f, 4.0f), BindingProbe.EchoVec3(new Vec3(1.0f, 2.0f, 3.0f)));
		Check.Equal(new Vec4(8.0f, 6.0f, 4.0f, 2.0f), BindingProbe.EchoVec4(new Vec4(1.0f, 2.0f, 3.0f, 4.0f)));
		Check.Equal(new Quat(4.0f, 3.0f, 2.0f, 1.0f), BindingProbe.EchoQuat(new Quat(1.0f, 2.0f, 3.0f, 4.0f)));
		Check.Equal(new Color(0.5f, 0.25f, 0.125f, 1.0f), BindingProbe.EchoColor(new Color(1.0f, 0.125f, 0.25f, 0.5f)));
	}

	[ConformanceTest]
	internal static void MixedArgumentsArriveInOrder() {
		Vec3 position = new(1.0f, 2.0f, 3.0f);
		Vec4 plane = new(4.0f, 5.0f, 6.0f, 7.0f);
		Quat rotation = new(8.0f, 9.0f, 10.0f, 11.0f);
		Color tint = new(12.0f, 13.0f, 14.0f, 15.0f);
		Vec2 offset = new(16.0f, 17.0f);
		Check.Equal(MixExpected(true, 3, 0.5f, position, 0.125, plane, 9ul, rotation, tint, offset),
					BindingProbe.Mix(true, 3, 0.5f, position, 0.125, plane, 9ul, rotation, tint, offset));
		Check.Equal(MixExpected(false, -4, -1.5f, plane.XYZ, 2.0, new Vec4(position, 0.0f), 1ul << 40, rotation, tint, offset),
					BindingProbe.Mix(false, -4, -1.5f, plane.XYZ, 2.0, new Vec4(position, 0.0f), 1ul << 40, rotation, tint, offset));
	}

	[ConformanceTest]
	internal static void GuardedCallsAnswerThroughPointer() {
		Check.Equal(new Vec3(5.0f, 7.0f, 16.0f), BindingProbe.SumGuarded(new Vec3(1.0f, 2.0f, 3.0f), new Vec4(4.0f, 5.0f, 6.0f, 7.0f)));
		Check.Equal(true, BindingProbe.IsPositive(0.5f));
		Check.Equal(false, BindingProbe.IsPositive(-0.5f));
	}

	[ConformanceTest]
	internal static void NativeThrowCarriesItsMessage() {
		try {
			BindingProbe.Throw();
		} catch (EngineException exception) {
			Check.Equal("thrown by a binding", exception.Message);
			return;
		}
		Check.Fail("no EngineException");
	}

	[ConformanceTest]
	internal static void StringsArriveAsUTF8() {
		Check.Equal(0, BindingProbe.SumBytes(""));
		Check.Equal(0, BindingProbe.SumBytes(null!));
		Check.Equal(3000 + 'a' + 'b' + 'c', BindingProbe.SumBytes("abc"));
		Check.Equal(2000 + 0xC3 + 0xA9, BindingProbe.SumBytes("\u00e9"));
		Check.Equal(1000 * 1000 + 1000 * 'z', BindingProbe.SumBytes(new string('z', 1000)));
		Check.Equal(600 * 1000 + 300 * (0xC3 + 0xA9), BindingProbe.SumBytes(new string('\u00e9', 300)));
	}

	[ConformanceTest]
	internal static void ValueLayoutsMatchEngine() {
		string? mismatch = Bindings.FindLayoutMismatch();
		Check.True(mismatch == null, mismatch ?? "");
	}

	[ConformanceTest]
	internal static void MainThreadBindingRefusesOtherThreads() {
		Exception? caught = null;
		float value = -1.0f;
		Thread thread = new(() => {
			try {
				value = Time.DeltaTime;
			} catch (Exception exception) {
				caught = exception;
			}
		});
		thread.Start();
		thread.Join();
		if (RuntimeFeatures.ThreadChecks) {
			Check.True(caught is InvalidOperationException, "expected InvalidOperationException, got " + (caught?.GetType().Name ?? "no exception"));
			Check.True(caught!.Message.StartsWith("Time.DeltaTime ", StringComparison.Ordinal), caught.Message);
		} else {
			Check.True(caught == null, caught?.Message ?? "");
			Check.Equal(Time.DeltaTime, value);
		}
	}

	[ConformanceTest]
	internal static void AnyThreadBindingRunsOffMainThread() {
		int value = 0;
		Thread thread = new(() => value = BindingProbe.EchoI32(41));
		thread.Start();
		thread.Join();
		Check.Equal(42, value);
	}
}

static class FeatureProbe {
	[ConformanceTest]
	internal static void ReportsThreadChecks() {
		Check.Fail(RuntimeFeatures.ThreadChecks ? "thread checks on" : "thread checks off");
	}
}
