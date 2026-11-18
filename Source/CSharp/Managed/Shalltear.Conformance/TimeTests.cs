using System.Threading;

using Shalltear.Interop;

namespace Shalltear.Conformance;

static class TimeTests {
	[ConformanceTest]
	internal static void ValuesMatchEngine() {
		Check.Equal(0.25f, Time.DeltaTime);
		Check.Equal(0.5f, Time.UnscaledDeltaTime);
		Check.Equal(0.02f, Time.FixedDeltaTime);
		Check.Equal(12.5, Time.ScaledTime);
		Check.Equal(25.0, Time.UnscaledTime);
		Check.Equal(1234L, Time.FrameCount);
		Check.Equal(0.5f, Time.TimeScale);
	}

	[ConformanceTest]
	internal static void TimeScaleIsWritten() {
		float before = Time.TimeScale;
		Time.TimeScale = 2.0f;
		Check.Equal(2.0f, Time.TimeScale);
		Time.TimeScale = -1.0f;
		Check.Equal(2.0f, Time.TimeScale);
		Time.TimeScale = before;
		Check.Equal(before, Time.TimeScale);
	}
}

static class LogProbe {
	[ConformanceTest]
	internal static void WritesEachLevel() {
		Log.Trace("Conformance", "trace from C#");
		Log.Debug("Conformance", "debug from C#");
		Log.Info("Conformance", "info from C#");
		Log.Warn("Conformance", "warn from C#");
		Log.Error("Conformance", "error from C#");
	}

	[ConformanceTest]
	internal static void WritesOddText() {
		Log.Info("", "");
		Log.Info(null!, null!);
		Log.Info("Conformance", new string('x', 5000) + " caf\u00e9 end");
	}

	[ConformanceTest]
	internal static void WritesFromStartedThread() {
		Thread thread = new(static () => Log.Info("Conformance", "info from a started thread"));
		thread.Start();
		thread.Join();
	}
}
