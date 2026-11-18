namespace Shalltear;

/// <summary>Part of a scene object. Scripts derive from Behavior.</summary>
public abstract partial class Component : EngineObject {
	private protected Component() {}

	/// <summary>Added to its object.</summary>
	protected virtual void OnAttach() {}

	/// <summary>Removed from its object.</summary>
	protected virtual void OnDetach() {}

	/// <summary>Became enabled (also after OnAttach when enabled).</summary>
	protected virtual void OnEnable() {}

	/// <summary>Became disabled.</summary>
	protected virtual void OnDisable() {}

	/// <summary>About to be destroyed.</summary>
	protected virtual void OnDestroy() {}
}
