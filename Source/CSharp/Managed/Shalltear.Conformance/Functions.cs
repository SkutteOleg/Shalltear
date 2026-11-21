using System;

using Shalltear.Interop;

namespace Shalltear.Conformance;

enum Gear : byte {
	Low = 1,
	High = 2,
	Top = 3,
}

static partial class NativeCalls {
	[NativeFunction("ProbeScale")]
	internal static partial float Scale(float value, int factor);

	[NativeFunction("ProbeGreet")]
	internal static partial string Greet(string name, string? ending);

	[NativeFunction("ProbeMix")]
	internal static partial double Mix(Vec2 two, Vec3 three, Vec4 four, Quat turn, Color tint, bool flag, byte small, short negative, long big, double wide);

	[NativeFunction("ProbeSwap")]
	internal static partial Vec3 Swap(Vec3 value);

	[NativeFunction("ProbeTurn")]
	internal static partial Quat Turn(Quat value);

	[NativeFunction("ProbeTint")]
	internal static partial Color Tint(Vec4 value);

	[NativeFunction("ProbeWide")]
	internal static partial Vec4 Wide(Color value);

	[NativeFunction("ProbeFlip")]
	internal static partial Vec2 Flip(Vec2 value);

	[NativeFunction("ProbeNameOf")]
	internal static partial string NameOf(SceneObject? target);

	[NativeFunction("ProbeParentOf")]
	internal static partial SceneObject? ParentOf(SceneObject child);

	[NativeFunction("ProbeNextGear")]
	internal static partial Gear NextGear(Gear gear);

	[NativeFunction("ProbeNot")]
	internal static partial bool Not(bool value);

	[NativeFunction("ProbeBig")]
	internal static partial ulong Big(ulong value);

	[NativeFunction("ProbeNowhere")]
	internal static partial void Nowhere();

	[NativeFunction("ProbeScale")]
	internal static partial float ScaleWrong(float value, float factor);

	[ExportToNative("ConformanceTriple")]
	internal static int Triple(int value) {
		return value * 3;
	}

	[ExportToNative("ConformanceJoin")]
	internal static string Join(string left, Vec3 middle, SceneObject? right) {
		return left + " " + (int)middle.x + "," + (int)middle.y + "," + (int)middle.z + " " + (right ? right!.Name : "none");
	}

	[ExportToNative("ConformanceThrow")]
	internal static void Throw() {
		throw new InvalidOperationException("thrown by an export");
	}
}

static class FunctionProbe {
	[ConformanceTest]
	internal static void CallsThrowingExport() {
		BindingProbe.CallThrow();
		BindingProbe.Record("returned after the throw");
	}
}

static class FunctionTests {
	static bool IsGone(SceneObject? value) {
		return value == null;
	}

	[ConformanceTest]
	internal static void NativeFunctionsAreCalled() {
		Check.Equal(-6f, NativeCalls.Scale(1.5f, -4));
		Check.Equal("hello café!", NativeCalls.Greet("café", "!"));
		Check.Equal("hello ", NativeCalls.Greet("", null));
		Check.Equal(new string('x', 9000).Length + 6, NativeCalls.Greet(new string('x', 9000), "").Length);
		Check.Equal(153.0 + 1000.0 + 200.0 - 300.0 - 5_000_000_000.0 + 0.25,
					NativeCalls.Mix(new Vec2(1, 2), new Vec3(3, 4, 5), new Vec4(6, 7, 8, 9), new Quat(10, 11, 12, 13), new Color(14, 15, 16, 17), true, 200, -300,
									-5_000_000_000L, 0.25));
		Check.Equal(new Vec3(3, 2, 1), NativeCalls.Swap(new Vec3(1, 2, 3)));
		Quat turned = NativeCalls.Turn(new Quat(1, 2, 3, 4));
		Check.True(turned.x == 4f && turned.y == 3f && turned.z == 2f && turned.w == 1f, turned.ToString());
		Color tinted = NativeCalls.Tint(new Vec4(0.1f, 0.2f, 0.3f, 0.4f));
		Check.True(tinted.r == 0.1f && tinted.g == 0.2f && tinted.b == 0.3f && tinted.a == 0.4f, tinted.ToString());
		Vec4 wide = NativeCalls.Wide(new Color(1, 2, 3, 4));
		Check.True(wide.x == 4f && wide.y == 3f && wide.z == 2f && wide.w == 1f, wide.ToString());
		Check.Equal(new Vec2(2, 1), NativeCalls.Flip(new Vec2(1, 2)));
		Check.Equal(Gear.Top, NativeCalls.NextGear(Gear.High));
		Check.True(NativeCalls.Not(false) && !NativeCalls.Not(true), "bool crossed wrong");
		Check.Equal(~5UL, NativeCalls.Big(5UL));
	}

	[ConformanceTest]
	internal static void NativeFunctionsTakeObjects() {
		SceneObject parent = BindingProbe.CreateObject("FunctionParent");
		SceneObject child = BindingProbe.CreateObject("FunctionChild");
		child.SetParent(parent);
		Check.Equal("FunctionChild", NativeCalls.NameOf(child));
		Check.Equal("none", NativeCalls.NameOf(null));
		Check.Same(parent, NativeCalls.ParentOf(child));
		Check.True(IsGone(NativeCalls.ParentOf(parent)), "a root has a parent");
		parent.Destroy();
		BindingProbe.EndFrame();
		Check.Equal("none", NativeCalls.NameOf(child));
	}

	[ConformanceTest]
	internal static void UnknownOrMismatchedFunctionThrows() {
		try {
			NativeCalls.Nowhere();
			Check.Fail("a function nobody exports was called");
		} catch (EngineException exception) {
			Check.True(exception.Message.Contains("'ProbeNowhere' isn't exported", StringComparison.Ordinal), exception.Message);
		}
		try {
			NativeCalls.ScaleWrong(1f, 2f);
			Check.Fail("a function was called with another signature");
		} catch (EngineException exception) {
			Check.True(exception.Message.Contains("f32(f32,i32)", StringComparison.Ordinal) && exception.Message.Contains("f32(f32,f32)", StringComparison.Ordinal),
					   exception.Message);
		}
	}

	[ConformanceTest]
	internal static void ExportsAreCalledFromNative() {
		Check.True(BindingProbe.IsExported("ConformanceTriple") && !BindingProbe.IsExported("ConformanceNowhere"), "export table");
		Check.Equal(21, BindingProbe.CallTriple(7));
		SceneObject named = BindingProbe.CreateObject("Joined");
		Check.Equal("left 1,2,3 Joined", BindingProbe.CallJoin("left", new Vec3(1, 2, 3), named));
		Check.Equal("x 0,0,0 none", BindingProbe.CallJoin("x", default, null));
		named.Destroy();
		BindingProbe.EndFrame();
	}

	[ConformanceTest]
	internal static void RepeatedFunctionCallsAllocateNothing() {
		float sum = NativeCalls.Scale(1f, 1) + NativeCalls.Swap(default).x + NativeCalls.Turn(default).x + BindingProbe.CallTriple(1);
		long before = GC.GetAllocatedBytesForCurrentThread();
		for (int index = 0; index < 1000; ++index) {
			sum += NativeCalls.Scale(index, 2);
			sum += NativeCalls.Swap(new Vec3(index, 0, 1)).x;
			sum += NativeCalls.Turn(new Quat(0, 0, 0, index)).x;
			sum += BindingProbe.CallTriple(index);
		}
		long allocated = GC.GetAllocatedBytesForCurrentThread() - before;
		Check.True(sum > 0f, "calls returned nothing");
		Check.Equal(0L, allocated);
	}
}
