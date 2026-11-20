using System;

namespace Shalltear;

/// <summary>Asset made from a C# class (serialized fields saved in a file of AssetMenu's extension).</summary>
public abstract class DataAsset : Asset {
	/// <summary>Called by generated code.</summary>
	protected DataAsset() {}
}

/// <summary>Create menu entry + file extension of a DataAsset class (1 class per extension).</summary>
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class AssetMenuAttribute : Attribute {
	/// <summary>Entry label (/ for a submenu) + extension with its dot, like ".level".</summary>
	public AssetMenuAttribute(string label, string extension) {
		Label = label;
		Extension = extension;
	}

	/// <summary>Entry in the Create menu.</summary>
	public string Label { get; }

	/// <summary>Extension of the asset's files.</summary>
	public string Extension { get; }
}
