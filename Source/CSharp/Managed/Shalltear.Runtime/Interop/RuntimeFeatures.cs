using System;
using System.Diagnostics.CodeAnalysis;

namespace Shalltear.Interop;

internal static class RuntimeFeatures {
	internal const string THREAD_CHECKS = "Shalltear.ThreadChecks";

	internal const string LOG_ORIGINS = "Shalltear.LogOrigins";

	[FeatureSwitchDefinition(THREAD_CHECKS)]
	internal static bool ThreadChecks { get; } = !AppContext.TryGetSwitch(THREAD_CHECKS, out bool enabled) || enabled;

	[FeatureSwitchDefinition(LOG_ORIGINS)]
	[FeatureGuard(typeof(RequiresUnreferencedCodeAttribute))]
	internal static bool LogOrigins { get; } = !AppContext.TryGetSwitch(LOG_ORIGINS, out bool enabled) || enabled;
}
