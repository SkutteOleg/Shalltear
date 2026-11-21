using System;

using Shalltear.Interop;

namespace Shalltear.Conformance;

static class SlotTests {
	[ConformanceTest]
	internal static void ScalarsAndValuesCross() {
		Check.Equal((sbyte)-7, SlotProbe.EchoI8(-7));
		Check.Equal((short)-300, SlotProbe.EchoI16(-300));
		Check.Equal((byte)250, SlotProbe.EchoU8(250));
		Check.Equal((ushort)65000, SlotProbe.EchoU16(65000));
		Check.Equal(-5_000_000_000L, SlotProbe.EchoI64(-5_000_000_000L));
		double blend = SlotProbe.Blend(true, -3, 0.5f, 2.25, new Vec2(1f, 2f), new Vec3(3f, 4f, 5f), new Vec4(6f, 7f, 8f, 9f), new Quat(1f, 0f, 0f, 0f),
									   new Color(0.25f, 0.25f, 0.25f, 0.25f), new Vec2I(-4, 10));
		Check.Equal(1.0 - 3 + 0.5 + 2.25 + 3 + 12 + 30 + 1 + 1 + 6, blend);
		Check.Equal(new Vec2I(9, -2), SlotProbe.Swap(new Vec2I(-2, 9)));
		Check.Equal(new Quat(4f, 3f, 2f, 1f), SlotProbe.Flip(new Quat(1f, 2f, 3f, 4f)));
		Check.Equal(ProbeTone.Mid, SlotProbe.NextTone(ProbeTone.Low));
		Check.Equal(ProbeTone.Low, SlotProbe.NextTone(ProbeTone.High));
		Check.Equal("hello каждый!", SlotProbe.Greet("каждый", "!"));
		Check.Equal("hello ", SlotProbe.Greet("", ""));
	}

	[ConformanceTest]
	internal static void ThrowBecomesEngineException() {
		try {
			SlotProbe.Throw();
		} catch (EngineException exception) {
			Check.True(exception.Message.Contains("thrown by a slot binding"), exception.Message);
			return;
		}
		Check.Fail("no exception");
	}

	[ConformanceTest]
	internal static void StructsCrossByValue() {
		ProbeShape shape = new() { width = 1.5f, count = 2, at = new Vec3(1f, 2f, 3f), flags = 3, tone = ProbeTone.Mid };
		ProbeShape grown = SlotProbe.Grow(shape, 0.25f);
		Check.Equal(1.75f, grown.width);
		Check.Equal(3, grown.count);
		Check.Equal(new Vec3(1.25f, 2f, 3f), grown.at);
		Check.Equal((byte)0x83, grown.flags);
		Check.Equal(ProbeTone.High, grown.tone);
		Check.Equal(1.5f, shape.width);

		ProbeWide half = SlotProbe.Halve(new ProbeWide { corner = new Vec4(2f, 4f, 6f, 8f), weight = 5.0, on = false });
		Check.Equal(new Vec4(1f, 2f, 3f, 4f), half.corner);
		Check.Equal(2.5, half.weight);
		Check.True(half.on, "on");
	}

	[ConformanceTest]
	internal static void OutParametersReturnEveryKind() {
		ProbeShape shape = new() { width = 2.5f, count = 4, at = new Vec3(7f, 8f, 9f), tone = ProbeTone.High };
		SlotProbe.Describe(shape, out int count, out Vec3 at, out string text, out ProbeTone tone, out bool wide, out double area);
		Check.Equal(4, count);
		Check.Equal(new Vec3(7f, 8f, 9f), at);
		Check.Equal("shape 2.5 x4", text);
		Check.Equal(ProbeTone.High, tone);
		Check.True(wide, "wide");
		Check.Equal(10.0, area);

		bool on = SlotProbe.Split(new ProbeWide { corner = new Vec4(8f, 8f, 8f, 8f), weight = 3.0, on = true }, out ProbeWide half, out string first, out string second);
		Check.True(on, "on");
		Check.Equal(new Vec4(4f, 4f, 4f, 4f), half.corner);
		Check.Equal("first", first);
		Check.Equal("second 3", second);

		SceneObject root = BindingProbe.CreateObject("SlotRoot");
		SceneObject middle = BindingProbe.CreateObject("SlotMiddle");
		SceneObject leaf = BindingProbe.CreateObject("SlotLeaf");
		middle.SetParent(root);
		leaf.SetParent(middle);
		SceneObject? parent = SlotProbe.FindParent(leaf, out SceneObject? found);
		Check.Same(middle, parent);
		Check.Same(root, found);
		Check.True(SlotProbe.FindParent(root, out found) == null, "root has a parent");
		Check.Same(root, found);
		root.Destroy();
		BindingProbe.EndFrame();
	}

	[ConformanceTest]
	internal static void ListsCrossBothWays() {
		Check.Equal(10, SlotProbe.Sum(new[] { 1, 2, 3, 4 }));
		Check.Equal(0, SlotProbe.Sum(ReadOnlySpan<int>.Empty));
		Span<float> filled = stackalloc float[3];
		SlotProbe.Fill(filled, 2.5f);
		Check.Equal(2.5f, filled[2]);
		int[] squares = SlotProbe.Count(4);
		Check.Equal(4, squares.Length);
		Check.Equal(9, squares[3]);
		Check.Equal(0, SlotProbe.Count(0).Length);

		ProbeShape[] shapes = SlotProbe.Shapes(3);
		Check.Equal(3, shapes.Length);
		Check.Equal(2.5f, shapes[2].width);
		Check.Equal(new Vec3(2f, 1f, 2f), shapes[2].at);
		Check.Equal(ProbeTone.High, shapes[1].tone);
		Check.Equal(4.5f, SlotProbe.Widths(shapes));

		Check.Equal("a, бб, c", SlotProbe.Join(new[] { "a", "бб", "c" }, ", "));
		string[] words = SlotProbe.Words("one два three");
		Check.Equal(3, words.Length);
		Check.Equal("два", words[1]);

		ProbeTone[] tones = SlotProbe.Tones();
		Check.Equal(3, tones.Length);
		Check.Equal(ProbeTone.Low, tones[0]);
		Check.Equal(ProbeTone.High, tones[2]);
		double[] halves = SlotProbe.Halves(new[] { 3.0, 5.0 });
		Check.Equal(2.5, halves[1]);

		SceneObject parent = BindingProbe.CreateObject("SlotParent");
		SceneObject first = BindingProbe.CreateObject("SlotChild");
		SceneObject second = BindingProbe.CreateObject("SlotChild");
		first.SetParent(parent);
		second.SetParent(parent);
		SceneObject[] children = SlotProbe.Children(parent);
		Check.Equal(2, children.Length);
		Check.Same(first, children[0]);
		Check.Same(second, children[1]);
		Check.Equal(2, SlotProbe.CountNamed(new[] { first, parent, second }, "SlotChild"));
		parent.Destroy();
		BindingProbe.EndFrame();
	}

	[ConformanceTest]
	internal static void NativeObjectsAreBorrowedOrOwned() {
		ProbeCounter counter = SlotProbe.Counter;
		int before = counter.Total;
		Check.Equal(before + 5, counter.Add(5));
		Check.Equal(before + 5, SlotProbe.Counter.Total);
		Check.True(counter.Equals(SlotProbe.Counter), "same counter");
		ProbeCounter[] counters = SlotProbe.Counters();
		Check.Equal(3, counters.Length);
		counters[1].Add(2);
		counters[2].Add(3);
		Check.Equal(counters[0].Total + counters[1].Total + counters[2].Total, SlotProbe.TotalOf(counters));

		int live = SlotProbe.CountLiveRecipes();
		ProbeRecipe made = SlotProbe.MakeRecipe("stew", 3);
		Check.Equal(live + 1, SlotProbe.CountLiveRecipes());
		Check.Equal("stew", made.Name);
		Check.Equal(1.5f, made.Heat);
		Check.Equal(3, made.Steps.Length);
		Check.Equal(3, made.Steps[2]);
		made.Name = "soup";
		made.Heat = 2f;
		made.Steps = new[] { 9, 8 };
		Check.Equal("soup in 2 step(s) at 2", SlotProbe.DescribeRecipe(made));

		ProbeRecipe blank = new();
		Check.Equal(live + 2, SlotProbe.CountLiveRecipes());
		Check.Equal("", blank.Name);
		blank.Dispose();
		Check.Equal(live + 1, SlotProbe.CountLiveRecipes());
		Check.True(!blank.IsAlive, "disposed");
		made.Dispose();
		Check.Equal(live, SlotProbe.CountLiveRecipes());
	}

	[ConformanceTest]
	internal static void RepeatedSlotCallsAllocateNothing() {
		ProbeShape shape = new() { width = 1f, count = 1 };
		float sum = SlotProbe.Grow(shape, 1f).width + (float)SlotProbe.EchoI64(1) + SlotProbe.Swap(default).x + SlotProbe.Counter.Total;
		long before = GC.GetAllocatedBytesForCurrentThread();
		for (int index = 0; index < 1000; ++index) {
			shape = SlotProbe.Grow(shape, 0.5f);
			sum += SlotProbe.EchoI16((short)index) + SlotProbe.Swap(new Vec2I(index, 1)).y + SlotProbe.Flip(new Quat(1f, 2f, 3f, 4f)).x;
			SlotProbe.Describe(shape, out int count, out Vec3 at, out _, out _, out _, out _);
			sum += count + at.x;
		}
		long bytes = GC.GetAllocatedBytesForCurrentThread() - before;
		Check.True(sum != 0f, "sum");
		Check.True(bytes <= 1000 * 64, $"{bytes} bytes in 1,000 rounds");
	}
}
