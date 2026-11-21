using System;

using Shalltear.Interop;

namespace Shalltear;

/// <summary>Scene load/unload notices (handlers dropped when scripts reload).</summary>
public static partial class SceneEvents {
	/// <summary>Scene joined loaded scenes.</summary>
	public static event Action<Scene> Loaded {
		add => ScriptEvents.Add(ScriptEvents.SCENE_LOADED, value);
		remove => ScriptEvents.Remove(ScriptEvents.SCENE_LOADED, value);
	}

	/// <summary>Scene about to lose its objects.</summary>
	public static event Action<Scene> Unloading {
		add => ScriptEvents.Add(ScriptEvents.SCENE_UNLOADING, value);
		remove => ScriptEvents.Remove(ScriptEvents.SCENE_UNLOADING, value);
	}
}
