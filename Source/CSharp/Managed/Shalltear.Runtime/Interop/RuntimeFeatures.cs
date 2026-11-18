using System;
using System.Diagnostics.CodeAnalysis;

namespace Shalltear.Interop;

internal static class RuntimeFeatures {
	internal const string THREAD_CHECKS = "Shalltear.ThreadChecks";

	[FeatureSwitchDefinition(THREAD_CHECKS)]
	internal static bool ThreadChecks { get; } = !AppContext.TryGetSwitch(THREAD_CHECKS, out bool enabled) || enabled;
}
