using System;
using System.Threading;

using Shalltear.Interop;

namespace Shalltear.Conformance;

static class BridgeTests {
	[ConformanceTest]
	internal static void NativeThrowBecomesEngineException() {
		try {
			Bridge.ThrowForTest();
		} catch (EngineException exception) {
			Check.Equal("thrown for a test", exception.Message);
			return;
		}
		Check.Fail("no EngineException");
	}

	[ConformanceTest]
	internal static void FailureCarriesItsMessage() {
		try {
			Check.Equal(1, 2);
		} catch (ConformanceFailure failure) {
			Check.Equal("expected 1, got 2", failure.Message);
			return;
		}
		Check.Fail("no ConformanceFailure");
	}
}

static class Probe {
	[ConformanceTest]
	internal static void Fails() {
		Check.Fail("failed as asked");
	}

	[ConformanceTest]
	internal static void ThrowsOnCallingThread() {
		throw new InvalidOperationException("thrown on the calling thread");
	}

	[ConformanceTest]
	internal static void ThrowsOnStartedThread() {
		Thread thread = new(static () => throw new InvalidOperationException("thrown on a started thread"));
		thread.Start();
		thread.Join();
	}
}
