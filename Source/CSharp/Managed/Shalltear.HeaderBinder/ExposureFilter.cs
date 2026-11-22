using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;

namespace Shalltear.HeaderBinder;

internal sealed class ExposureFilter {
	public const string IGNORE_ANNOTATION = "slt_script_ignore";

	readonly HashSet<string> _excludedFunctions = new(StringComparer.Ordinal) {
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

	readonly HashSet<string> _excludedNamespaces = new(StringComparer.Ordinal) {
		"std",
		"__gnu_cxx",
		"Shalltear",
	};

	readonly HashSet<string> _excludedTypes = new(StringComparer.Ordinal);
	readonly List<string> _policyHeaderPatterns = new();

	public static ExposureFilter Load(string? policyPath) {
		ExposureFilter filter = new();
		if (string.IsNullOrWhiteSpace(policyPath) || !File.Exists(policyPath)) {
			return filter;
		}
		string text = File.ReadAllText(policyPath);
		JsonDocumentOptions opts = new() {
			CommentHandling = JsonCommentHandling.Skip,
			AllowTrailingCommas = true,
		};
		using JsonDocument doc = JsonDocument.Parse(text, opts);
		if (doc.RootElement.ValueKind != JsonValueKind.Object) {
			return filter;
		}
		ReadStringSet(doc.RootElement, "excludedNamespaces", filter._excludedNamespaces);
		ReadStringSet(doc.RootElement, "excludedFunctions", filter._excludedFunctions);
		ReadStringSet(doc.RootElement, "excludedFreeFunctionNames", filter._excludedFunctions);
		ReadStringSet(doc.RootElement, "excludedMethodNames", filter._excludedFunctions);
		ReadStringSet(doc.RootElement, "excludedStaticMethodNames", filter._excludedFunctions);
		ReadStringSet(doc.RootElement, "excludedTypes", filter._excludedTypes);
		ReadStringList(doc.RootElement, "excludedHeaderPatterns", filter._policyHeaderPatterns);
		return filter;
	}

	static void ReadStringSet(JsonElement root, string property, HashSet<string> target) {
		if (!root.TryGetProperty(property, out JsonElement elem) || elem.ValueKind != JsonValueKind.Array) {
			return;
		}
		foreach (JsonElement item in elem.EnumerateArray()) {
			if (item.ValueKind == JsonValueKind.String) {
				string? value = item.GetString();
				if (!string.IsNullOrWhiteSpace(value)) {
					target.Add(value.Trim());
				}
			}
		}
	}

	static void ReadStringList(JsonElement root, string property, List<string> target) {
		if (!root.TryGetProperty(property, out JsonElement elem) || elem.ValueKind != JsonValueKind.Array) {
			return;
		}
		foreach (JsonElement item in elem.EnumerateArray()) {
			if (item.ValueKind == JsonValueKind.String) {
				string? value = item.GetString();
				if (!string.IsNullOrWhiteSpace(value)) {
					string normalized = value.Trim().Replace('\\', '/');
					if (!target.Contains(normalized)) {
						target.Add(normalized);
					}
				}
			}
		}
	}

	public bool IsExcludedNamespace(string cppNamespace) {
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

	public bool IsExcludedType(string name, string qualifiedName) {
		if (!string.IsNullOrEmpty(name) && _excludedTypes.Contains(name)) {
			return true;
		}
		if (!string.IsNullOrEmpty(qualifiedName) && _excludedTypes.Contains(qualifiedName)) {
			return true;
		}
		return false;
	}

	public bool IsExcludedFunctionName(string name, bool fromBindNamespace) {
		if (string.IsNullOrEmpty(name)) {
			return true;
		}
		if (_excludedFunctions.Contains(name)) {
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

	public bool IsExcludedByPolicyHeaderPattern(string normalizedPath) {
		foreach (string pattern in _policyHeaderPatterns) {
			if (normalizedPath.Contains(pattern, StringComparison.OrdinalIgnoreCase)) {
				return true;
			}
		}
		return false;
	}

	public bool IsExcludedHeaderPath(string normalizedPath, bool explicitlyRequested) {
		if (IsExcludedByPolicyHeaderPattern(normalizedPath)) {
			return true;
		}
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
