using System;

using Shalltear.Interop;

namespace Shalltear.Conformance;

sealed class Hud : Behavior {
	public UIPanelRenderer? panel;
	public UIDocumentAsset? document;
}

static class UIProbe {
	static void Count(UIEventArgs click) {
		BindingProbe.Record("kept handler ran");
	}

	static void Throw(UIEventArgs click) {
		throw new InvalidOperationException("thrown by a UI handler");
	}

	static void Hear(UIEventArgs click) {
		BindingProbe.Record("heard " + click.Kind);
	}

	static void Swap(UIEventArgs click) {
		BindingProbe.Record("swapped");
		click.Element.SetHandler(UIEvent.Click, Hear);
	}

	[ConformanceTest]
	internal static void KeepsAHandler() {
		UITests.Panel("Panel Keeper").FindElement("play").SetHandler(UIEvent.Click, Count);
	}

	[ConformanceTest]
	internal static void ClicksKeepersButton() {
		SceneObject? keeper = SceneSystem.FindObjectByName("Panel Keeper");
		UIPanelRenderer? panel = keeper ? keeper!.GetComponent<UIPanelRenderer>() : null;
		if (!panel) {
			Check.Fail("no Keeper");
		}
		BindingProbe.ClickElement(panel!, panel!.FindElement("play")._key);
	}

	[ConformanceTest]
	internal static void ReportsKeepersHandler() {
		UIPanelRenderer panel = SceneSystem.FindObjectByName("Panel Keeper")!.GetComponent<UIPanelRenderer>()!;
		BindingProbe.Record("handler " + (BindingProbe.HasClickHandler(panel, panel.FindElement("play")._key) ? 1 : 0));
	}

	[ConformanceTest]
	internal static void HandlerThrows() {
		UIPanelRenderer panel = UITests.Panel("Thrower");
		UIElement play = panel.FindElement("play");
		play.SetHandler(UIEvent.Click, Throw);
		play.SetHandler(UIEvent.MouseUp, Hear);
		BindingProbe.ClickElement(panel, play._key);
		UITests.Retire(panel);
	}

	[ConformanceTest]
	internal static void SwapsInsideAHandler() {
		UIPanelRenderer panel = UITests.Panel("Swapper");
		panel.FindElement("play").SetHandler(UIEvent.Click, Swap);
		BindingProbe.ClickElement(panel, panel.FindElement("play")._key);
		BindingProbe.ClickElement(panel, panel.FindElement("play")._key);
	}

	[ConformanceTest]
	internal static void ClicksSwappersButton() {
		SceneObject? swapper = SceneSystem.FindObjectByName("Swapper");
		UIPanelRenderer panel = swapper!.GetComponent<UIPanelRenderer>()!;
		BindingProbe.ClickElement(panel, panel.FindElement("play")._key);
		UITests.Retire(panel);
	}
}

static class UITests {
	internal const string MENU = "project://Assets/Menu.slxml";

	static bool Gone(EngineObject? value) {
		return value == null;
	}

	static string Refusal(Action call) {
		try {
			call();
		} catch (EngineException exception) {
			return exception.Message;
		} catch (InvalidOperationException exception) {
			return exception.Message;
		}
		return "allowed";
	}

	internal static UIPanelRenderer Panel(string name) {
		UIPanelRenderer panel = SceneSystem.CreateObject(name).AddComponent<UIPanelRenderer>();
		panel.Document = AnimationTests.Stored<UIDocumentAsset>(MENU);
		return panel;
	}

	internal static void Retire(UIPanelRenderer panel) {
		panel.Owner.SetActive(false);
		panel.Owner.Destroy();
	}

	[ConformanceTest]
	internal static void PanelFindsDocumentElements() {
		UIPanelRenderer panel = Panel("Menu");
		Check.True(panel.IsBuilt, "panel built");
		Check.Same(AnimationTests.Stored<UIDocumentAsset>(MENU), panel.Document);
		Check.True(panel.Root.IsValid, "root");
		UIElement title = panel.FindElement("title");
		Check.True(title.IsValid && title.HasId("title") && !title.HasId("play"), "title found");
		Check.Equal("Coins: 0", title.Text);
		title.Text = "Coins: 3";
		Check.Equal("Coins: 3", title.Text);
		Check.True(!panel.FindElement("missing").IsValid && !default(UIElement).IsValid, "no such element");
		UIElement screen = panel.FindElement("screen");
		Check.Equal(screen, title.Parent);
		Check.Equal(title, screen.FirstChild);
		Check.True(title != screen && !title.Equals(screen), "different elements");
		Check.Equal(panel.FindElement("play"), title.NextSibling);
		Check.Same(panel, title.Owner);
		panel.SortOrder = 2f;
		Check.Equal(2f, panel.SortOrder);
		panel.Document = null;
		Check.True(!title.IsValid && !panel.FindElement("title").IsValid, "document removed");
		Retire(panel);
	}

	[ConformanceTest]
	internal static void ElementStateRoundTrips() {
		UIPanelRenderer panel = Panel("States");
		UIElement title = panel.FindElement("title");
		Check.True(title.Visible && title.Hoverable, "visible, hoverable");
		title.Visible = false;
		title.Hoverable = false;
		title.Focusable = true;
		title.Tint = new Color(1f, 0.5f, 0.25f, 1f);
		Check.True(!title.Visible && !title.Hoverable && title.Focusable, "flags");
		Check.Equal(new Color(1f, 0.5f, 0.25f, 1f), title.Tint);
		title.Visible = true;
		Check.True(title.HasClass("big") && !title.HasClass("small"), "document class");
		title.AddClass("small");
		title.AddClass("small");
		Check.True(title.HasClass("small"), "class added");
		title.RemoveClass("small");
		Check.True(!title.HasClass("small"), "class removed");
		title.AddClass("b");
		title.AddClass("c");
		title.AddClass("d");
		Check.Equal("UI element has 4 classes (the most allowed). Remove one", Refusal(() => title.AddClass("e")));
		Check.Equal("UIElement.AddClass got an empty class. Pass a class name", Refusal(() => title.AddClass("")));
		title.SetTooltip("Coins so far");
		title.SetTextKey("hud.coins", "Coins");
		Check.Equal("hud.coins", title.TextKey);
		title.SetRichText(false);
		title.ClearText();
		Check.Equal("", title.Text);
		title.TextInput = true;
		Check.True(title.TextInput, "text input");
		title.Focus();
		Check.True(title.IsFocused && panel.Focused == title, "focused");
		panel.ClearFocus();
		Check.True(!title.IsFocused && !panel.Focused.IsValid, "focus cleared");
		Check.True(!title.IsHovered, "not hovered");
		title.ScrollOffset = Vec2.Zero;
		Check.Equal(Vec2.Zero, title.ScrollOffset);
		title.ScrollIntoView();
		Retire(panel);
	}

	[ConformanceTest]
	internal static void StylesApplyByProperty() {
		UIPanelRenderer panel = Panel("Styled");
		UIElement title = panel.FindElement("title");
		Check.True(title.SetStyle("display", "none"), "display set");
		Check.Equal("none", title.GetStyle("display"));
		Check.True(title.SetStyle("display", "flex"), "display back");
		Check.Equal("flex", title.GetStyle("display"));
		Check.True(!title.SetStyle("colour", "red"), "unknown property");
		Check.Equal("", title.GetStyle("colour"));
		Check.True(title.SetStyle("opacity", "0.5"), "opacity");
		Check.Equal("0.5", title.GetStyle("opacity"));
		UIElement screen = panel.FindElement("screen");
		Check.True(screen.SetStyle("color", "#ff0000"), "parent color");
		BindingProbe.UpdateUI(panel);
		Check.Equal("0.5", title.GetComputedStyle("opacity"));
		Check.Equal(screen.GetStyle("color"), title.GetComputedStyle("color"));
		Check.True(title.GetStyle("color") != screen.GetStyle("color"), "color not set on the title");
		Retire(panel);
	}

	[ConformanceTest]
	internal static void ElementsAreMadeAndDestroyed() {
		UIPanelRenderer panel = Panel("Builder");
		UIElement screen = panel.FindElement("screen");
		UIElement made = screen.CreateChild();
		Check.True(made.IsValid, "made");
		Check.Equal(screen, made.Parent);
		Check.True(!made.NextSibling.IsValid && !made.FirstChild.IsValid, "last, empty");
		made.Text = "Made";
		Check.Equal("Made", made.Text);
		UIElement copy = made.Instantiate(AnimationTests.Stored<UIDocumentAsset>(MENU));
		Check.True(copy.IsValid, "instantiated");
		Check.True(made.FirstChild.IsValid, "copy under made");
		made.ClearChildren();
		Check.True(copy.IsValid, "cleared at the next update");
		BindingProbe.UpdateUI(panel);
		Check.True(!copy.IsValid && made.IsValid && !made.FirstChild.IsValid, "children cleared");
		made.Destroy();
		Check.True(made.IsValid, "destroyed at the next update");
		BindingProbe.UpdateUI(panel);
		Check.True(!made.IsValid, "destroyed");
		Check.Equal("UI element is gone (destroyed, or its panel rebuilt)", Refusal(() => made.Text = "x"));
		Check.Equal("UIElement.Destroy got the panel's root. Destroy its children", Refusal(() => panel.Root.Destroy()));
		Check.Equal("UIElement.Text called on an empty UIElement. Check IsValid", Refusal(() => default(UIElement).Text = "x"));
		Check.Equal("UIElement.Text called on an empty UIElement. Check IsValid", Refusal(() => panel.FindElement("missing").Text = "x"));
		panel.SetEnabled(false);
		Check.True(!panel.IsBuilt, "disabled panel");
		Check.Equal("UIPanelRenderer has no UI. Enable it while the UI system runs", Refusal(() => screen.CreateChild()));
		Retire(panel);
	}

	[ConformanceTest]
	internal static void EventsReachHandlers() {
		UIPanelRenderer panel = Panel("Clicks");
		UIElement play = panel.FindElement("play");
		int clicks = 0;
		UIEventArgs heard = default;
		play.SetHandler(UIEvent.Click, click => {
			++clicks;
			heard = click;
		});
		int held = UIEvents.Count;
		BindingProbe.ClickElement(panel, play._key);
		Check.Equal(1, clicks);
		Check.Equal(UIEvent.Click, heard.Kind);
		Check.Equal(play, heard.Element);
		int replaced = 0;
		play.SetHandler(UIEvent.Click, click => ++replaced);
		Check.Equal(held, UIEvents.Count);
		BindingProbe.ClickElement(panel, play._key);
		Check.Equal(1, clicks);
		Check.Equal(1, replaced);
		play.SetHandler(UIEvent.Click, null);
		Check.Equal(held - 1, UIEvents.Count);
		BindingProbe.ClickElement(panel, play._key);
		Check.Equal(1, replaced);

		UIElement sound = panel.FindElement("sound");
		double value = -1;
		sound.SetHandler(UIEvent.ValueChange, change => value = change.Value);
		BindingProbe.ClickElement(panel, sound._key);
		Check.Equal(1.0, value);
		BindingProbe.ClickElement(panel, sound._key);
		Check.Equal(0.0, value);
		Check.Equal("UIEvent 200 isn't an event. Pass a UIEvent value", Refusal(() => play.SetHandler((UIEvent)200, click => {})));
		sound.SetHandler(UIEvent.ValueChange, null);
		Retire(panel);
	}

	[ConformanceTest]
	internal static void ImagesAndLayoutRoundTrip() {
		UIPanelRenderer panel = Panel("Pictured");
		UIElement title = panel.FindElement("title");
		ulong iconId = BindingProbe.MakeAsset("TextureAsset");
		Texture icon = ObjectCache.Find<Texture>(iconId)!;
		Check.True(Gone(title.Image), "no image");
		title.SetImage(icon, 16f);
		Check.Same(icon, title.Image);
		title.ClearImage();
		Check.True(Gone(title.Image), "image cleared");
		title.SetImage(icon, 16f);
		title.SetImage(null, 16f);
		Check.True(Gone(title.Image), "null clears");
		title.SetStyle("padding", "4px");
		BindingProbe.UpdateUI(panel);
		UIElement play = panel.FindElement("play");
		Check.Equal(200f, play.Rect.size.x);
		Check.Equal(title.Rect.size.x - 8f, title.ContentRect.size.x);
		Retire(panel);
		BindingProbe.FreeAsset(iconId);
	}

	[ConformanceTest]
	internal static void PanelSettingsRoundTrip() {
		ulong settingsId = BindingProbe.MakeAsset("UIPanelSettings");
		UIPanelSettings settings = ObjectCache.Find<UIPanelSettings>(settingsId)!;
		Check.Equal(UIPanelScaleMode.ConstantPhysicalSize, settings.ScaleMode);
		settings.ScaleMode = UIPanelScaleMode.ConstantPixelSize;
		settings.Scale = 2f;
		settings.ReferenceDPI = 144f;
		settings.ReferenceResolution = new Vec2I(1920, 1080);
		settings.ScreenMatchMode = UIPanelScreenMatchMode.Expand;
		settings.Match = 0.5f;
		settings.LogarithmicMatch = true;
		settings.SortOrder = 3f;
		settings.FontSizePx = 20f;
		settings.TextColor = Color.Yellow;
		Check.Equal(UIPanelScaleMode.ConstantPixelSize, settings.ScaleMode);
		Check.Equal(2f, settings.Scale);
		Check.Equal(144f, settings.ReferenceDPI);
		Check.Equal(new Vec2I(1920, 1080), settings.ReferenceResolution);
		Check.Equal(UIPanelScreenMatchMode.Expand, settings.ScreenMatchMode);
		Check.Equal(0.5f, settings.Match);
		Check.True(settings.LogarithmicMatch, "logarithmic");
		Check.Equal(3f, settings.SortOrder);
		Check.Equal(20f, settings.FontSizePx);
		Check.Equal(Color.Yellow, settings.TextColor);
		Check.True(Gone(settings.Font), "engine font");
		Check.Equal(2f, settings.ResolveContentScale(new Vec2I(1200, 800), 1f));
		UIPanelRenderer panel = Panel("Scaled");
		panel.PanelSettings = settings;
		Check.Same(settings, panel.PanelSettings);
		Check.True(panel.FindElement("title").IsValid, "rebuilt with settings");
		panel.PanelSettings = null;
		Retire(panel);
		BindingProbe.FreeAsset(settingsId);
	}

	[ConformanceTest]
	internal static void ScriptFieldsHoldUITypes() {
		UIPanelRenderer panel = Panel("Held");
		Hud hud = panel.Owner.AddComponent<Hud>();
		hud.panel = panel;
		hud.document = AnimationTests.Stored<UIDocumentAsset>(MENU);
		SceneObject copy = SceneSystem.Instantiate(panel.Owner);
		Hud copied = copy.GetComponent<Hud>()!;
		Check.Same(copy.GetComponent<UIPanelRenderer>(), copied.panel);
		Check.Same(hud.document, copied.document);
		Retire(panel);
		copy.SetActive(false);
		copy.Destroy();
	}

	[ConformanceTest]
	internal static void RepeatedUICallsAllocateNothing() {
		UIPanelRenderer panel = Panel("Budget");
		UIElement title = panel.FindElement("title");
		title.Text = "Coins: 1";
		title.SetStyle("display", "flex");

		long before = GC.GetAllocatedBytesForCurrentThread();
		int hits = 0;
		for (int round = 0; round < 1000; ++round) {
			UIElement found = panel.FindElement("title");
			found.Text = "Coins: 3";
			hits += found.SetStyle("display", "flex") && found.HasClass("big") && found.IsValid ? 1 : 0;
			found.Visible = true;
			hits += found.Visible && found == title && panel.IsBuilt ? 1 : 0;
		}
		long allocated = GC.GetAllocatedBytesForCurrentThread() - before;
		Check.Equal(2000, hits);
		Check.Equal(0L, allocated);
		Retire(panel);
	}
}
