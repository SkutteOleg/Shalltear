namespace Shalltear;

/// <summary>Script component. Hooks run during Play only.</summary>
public abstract class Behavior : Component {
	// must match SLT_HOOK_* in ScriptBridge.h
	const int HOOK_ATTACH = 0;
	const int HOOK_DETACH = 1;
	const int HOOK_ENABLE = 2;
	const int HOOK_DISABLE = 3;
	const int HOOK_DESTROY = 4;
	const int HOOK_UPDATE = 5;
	const int HOOK_FIXED_UPDATE = 6;
	const int HOOK_LATE_UPDATE = 7;

	/// <summary>Runs in edit mode too (keep it free of game logic).</summary>
	protected Behavior() {}

	/// <summary>Once per frame.</summary>
	protected virtual void OnUpdate() {}

	/// <summary>Once per fixed step.</summary>
	protected virtual void OnFixedUpdate() {}

	/// <summary>Once per frame (after all OnUpdate calls).</summary>
	protected virtual void OnLateUpdate() {}

	internal void RunHook(int hook) {
		switch (hook) {
		case HOOK_ATTACH:
			OnAttach();
			break;
		case HOOK_DETACH:
			OnDetach();
			break;
		case HOOK_ENABLE:
			OnEnable();
			break;
		case HOOK_DISABLE:
			OnDisable();
			break;
		case HOOK_DESTROY:
			OnDestroy();
			break;
		case HOOK_UPDATE:
			OnUpdate();
			break;
		case HOOK_FIXED_UPDATE:
			OnFixedUpdate();
			break;
		case HOOK_LATE_UPDATE:
			OnLateUpdate();
			break;
		}
	}
}
