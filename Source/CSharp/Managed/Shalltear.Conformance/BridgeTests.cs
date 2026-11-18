using System;
using System.Runtime.CompilerServices;
using System.Threading;

using Shalltear.Interop;

namespace Shalltear.Conformance;

static class BridgeTests {
	[ModuleInitializer]
	internal static void Register() {
		ConformanceRegistry.Add("Bridge.NativeThrowBecomesEngineException", NativeThrowBecomesEngineException);
		ConformanceRegistry.Add("Bridge.FailureCarriesItsMessage", FailureCarriesItsMessage);
		ConformanceRegistry.Add("Probe.Fails", Fails);
		ConformanceRegistry.Add("Probe.ThrowsOnCallingThread", ThrowsOnCallingThread);
		ConformanceRegistry.Add("Probe.ThrowsOnStartedThread", ThrowsOnStartedThread);
	}

	static void NativeThrowBecomesEngineException() {
		try {
			Bridge.ThrowForTest();
		} catch (EngineException exception) {
			Check.Equal("thrown for a test", exception.Message);
			return;
		}
		Check.Fail("no EngineException");
	}

	static void FailureCarriesItsMessage() {
		try {
			Check.Equal(1, 2);
		} catch (ConformanceFailure failure) {
			Check.Equal("expected 1, got 2", failure.Message);
			return;
		}
		Check.Fail("no ConformanceFailure");
	}

	static void Fails() {
		Check.Fail("failed as asked");
	}

	static void ThrowsOnCallingThread() {
		throw new InvalidOperationException("thrown on the calling thread");
	}

	static void ThrowsOnStartedThread() {
		Thread thread = new(static () => throw new InvalidOperationException("thrown on a started thread"));
		thread.Start();
		thread.Join();
	}
}
