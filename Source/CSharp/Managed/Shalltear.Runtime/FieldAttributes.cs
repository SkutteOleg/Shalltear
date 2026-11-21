using System;

namespace Shalltear;

/// <summary>Serializes a non-public field or auto-property (public fields are serialized already).</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public sealed class ReflectAttribute : Attribute {}

/// <summary>Shows a numeric field as a slider in the Inspector.</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public sealed class RangeAttribute : Attribute {
	/// <summary>Slider's lowest value.</summary>
	public float Min { get; }

	/// <summary>Slider's highest value.</summary>
	public float Max { get; }

	/// <summary>Slider from min to max.</summary>
	public RangeAttribute(float min, float max) {
		Min = min;
		Max = max;
	}
}

/// <summary>Text shown when hovering the field in the Inspector.</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public sealed class TooltipAttribute : Attribute {
	/// <summary>Hover text.</summary>
	public string Text { get; }

	/// <summary>Hover text for the field.</summary>
	public TooltipAttribute(string text) {
		Text = text;
	}
}

/// <summary>Heading shown above the field in the Inspector.</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public sealed class HeaderAttribute : Attribute {
	/// <summary>Heading text.</summary>
	public string Text { get; }

	/// <summary>Heading above the field.</summary>
	public HeaderAttribute(string text) {
		Text = text;
	}
}

/// <summary>Hides a serialized field from the Inspector (still saved).</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public sealed class HiddenAttribute : Attribute {}

/// <summary>Shows the field in the Inspector without letting it be edited.</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public sealed class ReadOnlyAttribute : Attribute {}

/// <summary>Shows the field in the Inspector without saving it.</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public sealed class TransientAttribute : Attribute {}

/// <summary>Matches serialized data previously stored under oldName.</summary>
[AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct | AttributeTargets.Enum | AttributeTargets.Field | AttributeTargets.Property,
				AllowMultiple = true, Inherited = false)]
public sealed class FormerlySerializedAsAttribute : Attribute {
	/// <summary>Previous serialized name.</summary>
	public string OldName { get; }

	/// <summary>Map oldName to this type or field during deserialization.</summary>
	public FormerlySerializedAsAttribute(string oldName) {
		OldName = oldName;
	}
}

/// <summary>Vertical spacing in pixels before the field in the Inspector.</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public sealed class SpaceAttribute : Attribute {
	/// <summary>Height in pixels.</summary>
	public float Height { get; }

	/// <summary>Add vertical space above the field.</summary>
	public SpaceAttribute(float height = 8f) {
		Height = height;
	}
}

/// <summary>Shows a string field as a multi-line text box in the Inspector.</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public sealed class MultilineAttribute : Attribute {
	/// <summary>Visible line count.</summary>
	public int Lines { get; }

	/// <summary>Multi-line text box with the given line count.</summary>
	public MultilineAttribute(int lines = 3) {
		Lines = lines;
	}
}

/// <summary>Shows a string field as a resizable multi-line text area in the Inspector.</summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public sealed class TextAreaAttribute : Attribute {
	/// <summary>Minimum visible line count.</summary>
	public int MinLines { get; }

	/// <summary>Maximum visible line count.</summary>
	public int MaxLines { get; }

	/// <summary>Multi-line text area with min and max line counts.</summary>
	public TextAreaAttribute(int minLines = 3, int maxLines = 6) {
		MinLines = minLines;
		MaxLines = maxLines;
	}
}

/// <summary>Automatically adds requiredComponent when this component is added.</summary>
[AttributeUsage(AttributeTargets.Class, AllowMultiple = true, Inherited = true)]
public sealed class RequireComponentAttribute : Attribute {
	/// <summary>Required component type.</summary>
	public Type RequiredComponent { get; }

	/// <summary>Require the given component type on the same SceneObject.</summary>
	public RequireComponentAttribute(Type requiredComponent) {
		RequiredComponent = requiredComponent;
	}
}

/// <summary>Prevents adding more than one instance of this component to the same SceneObject.</summary>
[AttributeUsage(AttributeTargets.Class, Inherited = true)]
public sealed class DisallowMultipleComponentAttribute : Attribute {}

/// <summary>Category and display path in the Add Component menu.</summary>
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class AddComponentMenuAttribute : Attribute {
	/// <summary>Slash-separated menu path (e.g. "Gameplay/Player").</summary>
	public string MenuPath { get; }

	/// <summary>Sort order hint within the menu category.</summary>
	public int Order { get; }

	/// <summary>Place this component at menuPath in the Add Component menu.</summary>
	public AddComponentMenuAttribute(string menuPath, int order = 0) {
		MenuPath = menuPath;
		Order = order;
	}
}
