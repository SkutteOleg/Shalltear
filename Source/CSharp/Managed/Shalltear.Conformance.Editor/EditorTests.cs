using Shalltear.Editor;
using Shalltear.Interop;

namespace Shalltear.Conformance;

static class EditorProbe {
	[EditorLoad]
	internal static void Loaded() {
		BindingProbe.Record("editor load");
	}

	[ConformanceTest]
	internal static void AddsAMenuItem() {
		EditorMenusMenuItemDesc item = new() { Label = "Probe Item", Shortcut = "Ctrl+9" };
		item.SetOnSelect(static context => BindingProbe.Record("probe item selected"));
		item.SetIsEnabled(static () => true);
		Check.True(EditorMenus.Register("Probe", item), "registered");
	}

	[ConformanceTest]
	internal static void RegistersAPanel() {
		TypeInfo? type = EditorPanelProxy.Register(
			"Conformance.ProbePanel",
			static panel => {
				BindingProbe.Record("panel wired");
				panel.SetGetTitleBase(static () => "Probe");
				panel.SetOnBuildContent(static (context, root) => BindingProbe.Record("panel built"));
			},
			static builder => builder.AddAttribute("EditorPanelName", Variant.From("Probe Panel")));
		Check.True(type is not null, "registered");
	}
}
