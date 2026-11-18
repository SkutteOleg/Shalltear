using System;

namespace Shalltear;

/// <summary>Serializes a non-public field (public ones are serialized already).</summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class ReflectAttribute : Attribute {}

/// <summary>Shows a numeric field as a slider in the Inspector.</summary>
[AttributeUsage(AttributeTargets.Field)]
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
[AttributeUsage(AttributeTargets.Field)]
public sealed class TooltipAttribute : Attribute {
	/// <summary>Hover text.</summary>
	public string Text { get; }

	/// <summary>Hover text for the field.</summary>
	public TooltipAttribute(string text) {
		Text = text;
	}
}

/// <summary>Heading shown above the field in the Inspector.</summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class HeaderAttribute : Attribute {
	/// <summary>Heading text.</summary>
	public string Text { get; }

	/// <summary>Heading above the field.</summary>
	public HeaderAttribute(string text) {
		Text = text;
	}
}

/// <summary>Hides a serialized field from the Inspector (still saved).</summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class HiddenAttribute : Attribute {}

/// <summary>Shows the field in the Inspector without letting it be edited.</summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class ReadOnlyAttribute : Attribute {}

/// <summary>Shows the field in the Inspector without saving it.</summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class TransientAttribute : Attribute {}
