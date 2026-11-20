using System;

using Shalltear.Interop;

namespace Shalltear;

public sealed partial class UIPanelRenderer {
	/// <summary>Element with id in the document (IsValid false when none).</summary>
	public UIElement FindElement(string id) {
		return new UIElement(this, FindElementKey(id));
	}

	/// <summary>Element the document is built under.</summary>
	public UIElement Root => new(this, RootKey);

	/// <summary>Element with keyboard focus (IsValid false when none).</summary>
	public UIElement Focused => new(this, FocusedKey);

	/// <summary>Element under the pointer (IsValid false when none).</summary>
	public UIElement Hovered => new(this, HoveredKey);
}

/// <summary>Arguments of a UI element handler.</summary>
public readonly struct UIEventArgs {
	internal UIEventArgs(UIEvent kind, UIElement element, double value, string text) {
		Kind = kind;
		Element = element;
		Value = value;
		Text = text;
	}

	/// <summary>Event triggering the handler.</summary>
	public UIEvent Kind { get; }

	/// <summary>Element the event reached.</summary>
	public UIElement Element { get; }

	/// <summary>New value of a ValueChange (checkbox 0 or 1, slider value, dropdown index).</summary>
	public double Value { get; }

	/// <summary>Text of a TextChange or TextCommit ("" otherwise).</summary>
	public string Text { get; }
}

/// <summary>Element of a panel's UI, resolved by each call (calls throw once gone).</summary>
public readonly struct UIElement : IEquatable<UIElement> {
	readonly UIPanelRenderer? _panel;
	internal readonly ulong _key;

	internal UIElement(UIPanelRenderer panel, ulong key) {
		_panel = panel;
		_key = key;
	}

	UIPanelRenderer Panel(string member) {
		if (_panel is null || _key == 0) {
			throw new InvalidOperationException(member + " called on an empty UIElement. Check IsValid");
		}
		return _panel;
	}

	/// <summary>Element's panel (null for an empty one).</summary>
	public UIPanelRenderer? Owner => _panel;

	/// <summary>True while the element exists.</summary>
	public bool IsValid => _panel && _key != 0 && UIElementBindings.IsAlive(_panel!, _key, "UIElement.IsValid");

	/// <summary>Parent element (IsValid false for the root).</summary>
	public UIElement Parent => new(_panel!, UIElementBindings.GetParent(Panel("UIElement.Parent"), _key, "UIElement.Parent"));

	/// <summary>1st child (IsValid false when none).</summary>
	public UIElement FirstChild => new(_panel!, UIElementBindings.GetFirstChild(Panel("UIElement.FirstChild"), _key, "UIElement.FirstChild"));

	/// <summary>Next element under the same parent (IsValid false when last).</summary>
	public UIElement NextSibling => new(_panel!, UIElementBindings.GetNextSibling(Panel("UIElement.NextSibling"), _key, "UIElement.NextSibling"));

	/// <summary>Hash of the element's id (0 when none).</summary>
	public ulong IdHash => UIElementBindings.GetIdHash(Panel("UIElement.IdHash"), _key, "UIElement.IdHash");

	/// <summary>True when the element's id is id.</summary>
	public bool HasId(string id) {
		return IdHash == Bridge.HashOf(id);
	}

	/// <summary>Element is drawn + takes input.</summary>
	public bool Visible {
		get => UIElementBindings.GetVisible(Panel("UIElement.Visible"), _key, "UIElement.Visible");
		set => UIElementBindings.SetVisible(Panel("UIElement.Visible"), _key, value, "UIElement.Visible");
	}

	/// <summary>Element reacts to the pointer.</summary>
	public bool Hoverable {
		get => UIElementBindings.GetHoverable(Panel("UIElement.Hoverable"), _key, "UIElement.Hoverable");
		set => UIElementBindings.SetHoverable(Panel("UIElement.Hoverable"), _key, value, "UIElement.Hoverable");
	}

	/// <summary>Element can take keyboard focus.</summary>
	public bool Focusable {
		get => UIElementBindings.GetFocusable(Panel("UIElement.Focusable"), _key, "UIElement.Focusable");
		set => UIElementBindings.SetFocusable(Panel("UIElement.Focusable"), _key, value, "UIElement.Focusable");
	}

	/// <summary>Color multiplying the element's image + icon (sRGB).</summary>
	public Color Tint {
		get => UIElementBindings.GetTint(Panel("UIElement.Tint"), _key, "UIElement.Tint");
		set => UIElementBindings.SetTint(Panel("UIElement.Tint"), _key, value, "UIElement.Tint");
	}

	/// <summary>Add class name (matched by the document's style sheets).</summary>
	public void AddClass(string name) {
		UIElementBindings.AddClass(Panel("UIElement.AddClass"), _key, name, "UIElement.AddClass");
	}

	/// <summary>Remove class name.</summary>
	public void RemoveClass(string name) {
		UIElementBindings.RemoveClass(Panel("UIElement.RemoveClass"), _key, name, "UIElement.RemoveClass");
	}

	/// <summary>True when the element has class name.</summary>
	public bool HasClass(string name) {
		return UIElementBindings.HasClass(Panel("UIElement.HasClass"), _key, name, "UIElement.HasClass");
	}

	/// <summary>Set style property to value, like a document's style attribute ("display", "none"). False when either is invalid.</summary>
	public bool SetStyle(string property, string value) {
		return UIElementBindings.SetStyle(Panel("UIElement.SetStyle"), _key, property, value, "UIElement.SetStyle");
	}

	/// <summary>Style property as set on the element ("" when unknown).</summary>
	public string GetStyle(string property) {
		return UIElementBindings.GetStyle(Panel("UIElement.GetStyle"), _key, property, false, "UIElement.GetStyle");
	}

	/// <summary>Style property after the sheets + inheritance ("" when unknown).</summary>
	public string GetComputedStyle(string property) {
		return UIElementBindings.GetStyle(Panel("UIElement.GetComputedStyle"), _key, property, true, "UIElement.GetComputedStyle");
	}

	/// <summary>Text the element shows.</summary>
	public string Text {
		get => UIElementBindings.GetText(Panel("UIElement.Text"), _key, "UIElement.Text");
		set => UIElementBindings.SetText(Panel("UIElement.Text"), _key, value, "UIElement.Text");
	}

	/// <summary>Show the localized text of key (fallback when missing from tables).</summary>
	public void SetTextKey(string key, string fallback = "") {
		UIElementBindings.SetTextKey(Panel("UIElement.SetTextKey"), _key, key, fallback, "UIElement.SetTextKey");
	}

	/// <summary>Localization key of the text ("" when none).</summary>
	public string TextKey => UIElementBindings.GetTextKey(Panel("UIElement.TextKey"), _key, "UIElement.TextKey");

	/// <summary>Read tags like &lt;b&gt; in the text.</summary>
	public void SetRichText(bool enabled) {
		UIElementBindings.SetRichText(Panel("UIElement.SetRichText"), _key, enabled, "UIElement.SetRichText");
	}

	/// <summary>Remove the element's text.</summary>
	public void ClearText() {
		UIElementBindings.ClearText(Panel("UIElement.ClearText"), _key, "UIElement.ClearText");
	}

	/// <summary>Element takes typed text.</summary>
	public bool TextInput {
		get => UIElementBindings.GetTextInput(Panel("UIElement.TextInput"), _key, "UIElement.TextInput");
		set => UIElementBindings.SetTextInput(Panel("UIElement.TextInput"), _key, value, "UIElement.TextInput");
	}

	/// <summary>Show text while the pointer is over the element.</summary>
	public void SetTooltip(string text) {
		UIElementBindings.SetTooltip(Panel("UIElement.SetTooltip"), _key, text, "UIElement.SetTooltip");
	}

	/// <summary>Show texture at sizePx (null to remove the image).</summary>
	public void SetImage(Texture? texture, float sizePx) {
		UIElementBindings.SetImage(Panel("UIElement.SetImage"), _key, texture, sizePx, "UIElement.SetImage");
	}

	/// <summary>Remove the element's image.</summary>
	public void ClearImage() {
		UIElementBindings.ClearImage(Panel("UIElement.ClearImage"), _key, "UIElement.ClearImage");
	}

	/// <summary>Texture the element shows (null when none).</summary>
	public Texture? Image => UIElementBindings.GetImage(Panel("UIElement.Image"), _key, "UIElement.Image");

	/// <summary>Give the element keyboard focus.</summary>
	public void Focus() {
		UIElementBindings.Focus(Panel("UIElement.Focus"), _key, "UIElement.Focus");
	}

	/// <summary>True while the element has keyboard focus.</summary>
	public bool IsFocused => Panel("UIElement.IsFocused").FocusedKey == _key;

	/// <summary>True while the pointer is over the element.</summary>
	public bool IsHovered => Panel("UIElement.IsHovered").HoveredKey == _key;

	/// <summary>Border box in the panel's px, from the last layout.</summary>
	public RectF Rect {
		get {
			Vec4 rect = UIElementBindings.GetRect(Panel("UIElement.Rect"), _key, false, "UIElement.Rect");
			return new RectF(rect.x, rect.y, rect.z, rect.w);
		}
	}

	/// <summary>Content box in the panel's px, from the last layout.</summary>
	public RectF ContentRect {
		get {
			Vec4 rect = UIElementBindings.GetRect(Panel("UIElement.ContentRect"), _key, true, "UIElement.ContentRect");
			return new RectF(rect.x, rect.y, rect.z, rect.w);
		}
	}

	/// <summary>Scroll position of a scrolling element in px.</summary>
	public Vec2 ScrollOffset {
		get => UIElementBindings.GetScrollOffset(Panel("UIElement.ScrollOffset"), _key, "UIElement.ScrollOffset");
		set => UIElementBindings.SetScrollOffset(Panel("UIElement.ScrollOffset"), _key, value, "UIElement.ScrollOffset");
	}

	/// <summary>Scroll the element's scrolling parents to show it.</summary>
	public void ScrollIntoView() {
		UIElementBindings.ScrollIntoView(Panel("UIElement.ScrollIntoView"), _key, "UIElement.ScrollIntoView");
	}

	/// <summary>New empty element, last under this one.</summary>
	public UIElement CreateChild() {
		return new UIElement(_panel!, UIElementBindings.CreateChild(Panel("UIElement.CreateChild"), _key, "UIElement.CreateChild"));
	}

	/// <summary>Destroy the element + its children at the panel's next update.</summary>
	public void Destroy() {
		UIElementBindings.Destroy(Panel("UIElement.Destroy"), _key, "UIElement.Destroy");
	}

	/// <summary>Destroy all children at the panel's next update.</summary>
	public void ClearChildren() {
		UIElementBindings.ClearChildren(Panel("UIElement.ClearChildren"), _key, "UIElement.ClearChildren");
	}

	/// <summary>Build document under this element. Returns its root.</summary>
	public UIElement Instantiate(UIDocumentAsset document) {
		ArgumentNullException.ThrowIfNull(document);
		return new UIElement(_panel!, UIElementBindings.Instantiate(Panel("UIElement.Instantiate"), _key, document, "UIElement.Instantiate"));
	}

	/// <summary>Run handler on kind, replacing the handler set before (null to remove it). Changes made inside a handler applied at frame end.</summary>
	public void SetHandler(UIEvent kind, Action<UIEventArgs>? handler) {
		UIEvents.Set(Panel("UIElement.SetHandler"), _key, kind, handler);
	}

	/// <summary>True for the same element of the same panel.</summary>
	public bool Equals(UIElement other) {
		return ReferenceEquals(_panel, other._panel) && _key == other._key;
	}

	/// <summary>True for the same element of the same panel.</summary>
	public override bool Equals(object? other) {
		return other is UIElement element && Equals(element);
	}

	/// <summary>Hash of the panel + the element.</summary>
	public override int GetHashCode() {
		return HashCode.Combine(_panel is null ? 0UL : _panel._id, _key);
	}

	/// <summary>True for the same element of the same panel.</summary>
	public static bool operator ==(UIElement left, UIElement right) {
		return left.Equals(right);
	}

	/// <summary>False for the same element of the same panel.</summary>
	public static bool operator !=(UIElement left, UIElement right) {
		return !left.Equals(right);
	}
}
