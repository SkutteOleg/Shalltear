using System;

namespace Shalltear;

/// <summary>Binds a static partial method to a function exported by the project's C++ (SLT_SCRIPT_EXPORT).</summary>
[AttributeUsage(AttributeTargets.Method, Inherited = false)]
public sealed class NativeFunctionAttribute : Attribute {
	/// <summary>Exported name.</summary>
	public string Name { get; }

	/// <summary>Bind to the C++ function exported as name.</summary>
	public NativeFunctionAttribute(string name) {
		Name = name;
	}
}

/// <summary>Exports a static method to the project's C++ (ScriptFunctions::Find).</summary>
[AttributeUsage(AttributeTargets.Method, Inherited = false)]
public sealed class ExportToNativeAttribute : Attribute {
	/// <summary>Exported name.</summary>
	public string Name { get; }

	/// <summary>Export as name.</summary>
	public ExportToNativeAttribute(string name) {
		Name = name;
	}
}
