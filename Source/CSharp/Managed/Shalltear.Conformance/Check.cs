using System.Collections.Generic;

using Shalltear.Interop;

namespace Shalltear.Conformance;

static class Check {
	internal static void Fail(string message) {
		throw new ConformanceFailure(message);
	}

	internal static void True(bool condition, string message) {
		if (!condition) {
			throw new ConformanceFailure(message);
		}
	}

	internal static void Equal<T>(T expected, T actual) {
		if (!EqualityComparer<T>.Default.Equals(expected, actual)) {
			throw new ConformanceFailure($"expected {expected}, got {actual}");
		}
	}
}
