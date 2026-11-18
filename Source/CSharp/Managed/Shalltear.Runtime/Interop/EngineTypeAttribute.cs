using System;
using System.ComponentModel;

namespace Shalltear.Interop;

/// <summary>Engine class a wrapper stands for (read by generated code).</summary>
[AttributeUsage(AttributeTargets.Class)]
[EditorBrowsable(EditorBrowsableState.Never)]
public sealed class EngineTypeAttribute : Attribute {
	/// <summary>Engine class name.</summary>
	public string Name { get; }

	/// <summary>Wrapper of engine class name.</summary>
	public EngineTypeAttribute(string name) {
		Name = name;
	}
}
