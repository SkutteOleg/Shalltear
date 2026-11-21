using System.Collections.Generic;

using Shalltear.Interop;

namespace Shalltear;

/// <summary>Part of a scene object. Scripts derive from Behavior.</summary>
public abstract partial class Component : EngineObject {
	private protected Component() {}

	/// <summary>Destroy this component at end of frame.</summary>
	public void Destroy() {
		ObjectBindings.DestroyComponent(_id, "Component.Destroy");
	}

	/// <summary>1st component of type T on Owner (null when none).</summary>
	public T? GetComponent<T>()
		where T : class {
		return Owner.GetComponent<T>();
	}

	/// <summary>Try to fetch the 1st component of type T on Owner.</summary>
	public bool TryGetComponent<T>(out T? component)
		where T : class {
		return Owner.TryGetComponent(out component);
	}

	/// <summary>True when Owner carries at least one component of type T.</summary>
	public bool HasComponent<T>()
		where T : class {
		return Owner.HasComponent<T>();
	}

	/// <summary>Fill results with each component of type T on Owner.</summary>
	public void GetComponents<T>(List<T> results)
		where T : class {
		Owner.GetComponents(results);
	}

	/// <summary>1st component of type T on Owner or a descendant.</summary>
	public T? GetComponentInChildren<T>(bool includeInactive = false)
		where T : class {
		return Owner.GetComponentInChildren<T>(includeInactive);
	}

	/// <summary>1st component of type T on Owner or an ancestor.</summary>
	public T? GetComponentInParent<T>()
		where T : class {
		return Owner.GetComponentInParent<T>();
	}

	/// <summary>Add a component of type T to Owner.</summary>
	public T AddComponent<T>()
		where T : Component {
		return Owner.AddComponent<T>();
	}

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
