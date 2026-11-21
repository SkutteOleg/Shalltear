using System;
using System.Collections.Generic;

namespace Shalltear.HeaderBinder;

internal static class ExposureFilter {
	public const string IGNORE_ANNOTATION = "slt_script_ignore";

	static readonly HashSet<string> _excludedLifecycleFunctions = new(StringComparer.Ordinal) {
		"Initialize",
		"Shutdown",
		"Terminate",
		"Mount",
		"Unmount",
		"MountPackage",
		"UnmountPackage",
		"ResetForTests",
		"RegisterReflectType",
		"GetReflectType",
		"_slt_reflect_dummy",
	};

	static readonly HashSet<string> _excludedNamespaces = new(StringComparer.Ordinal) {
		"std",
		"__gnu_cxx",
		"Shalltear",
	};

	public static bool IsExcludedNamespace(string cppNamespace) {
		if (string.IsNullOrEmpty(cppNamespace)) {
			return false;
		}
		foreach (string excluded in _excludedNamespaces) {
			if (cppNamespace == excluded || cppNamespace.StartsWith(excluded + "::", StringComparison.Ordinal)) {
				return true;
			}
		}
		return false;
	}

	public static bool IsExcludedFunctionName(string name, bool fromBindNamespace) {
		if (string.IsNullOrEmpty(name)) {
			return true;
		}
		if (_excludedLifecycleFunctions.Contains(name)) {
			return true;
		}
		if (name.StartsWith("operator", StringComparison.Ordinal)) {
			return true;
		}
		if (fromBindNamespace && name.StartsWith("_", StringComparison.Ordinal)) {
			return true;
		}
		return false;
	}

	public static bool IsExcludedHeaderPath(string normalizedPath, bool explicitlyRequested) {
		if (explicitlyRequested) {
			return false;
		}
		if (normalizedPath.Contains("/Private/", StringComparison.OrdinalIgnoreCase) ||
			normalizedPath.Contains("/ThirdParty/", StringComparison.OrdinalIgnoreCase)) {
			return true;
		}
		return false;
	}
}
