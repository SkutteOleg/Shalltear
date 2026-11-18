using System;
using System.Threading;

using Shalltear.Interop;

namespace Shalltear.Conformance;

sealed class HookRecorder : Behavior {
	public HookRecorder() {
		BindingProbe.Record("constructed");
	}

	protected override void OnAttach() {
		BindingProbe.Record("attach");
	}

	protected override void OnDetach() {
		BindingProbe.Record("detach");
	}

	protected override void OnEnable() {
		BindingProbe.Record("enable");
	}

	protected override void OnDisable() {
		BindingProbe.Record("disable");
	}

	protected override void OnDestroy() {
		BindingProbe.Record("destroy");
	}

	protected override void OnUpdate() {
		BindingProbe.Record("update");
	}

	protected override void OnFixedUpdate() {
		BindingProbe.Record("fixed update");
	}

	protected override void OnLateUpdate() {
		BindingProbe.Record("late update");
	}
}

sealed class UpdateOnly : Behavior {
	static int _created;

	readonly int _number = Interlocked.Increment(ref _created);
	int _updates;

	protected override void OnUpdate() {
		++_updates;
		BindingProbe.Record($"update only {_number} ran {_updates}");
	}
}

sealed class Silent : Behavior {}

abstract class EnableCounter : Behavior {
	protected int _enables;

	protected override void OnEnable() {
		++_enables;
	}
}

sealed class LateReporter : EnableCounter {
	protected override void OnLateUpdate() {
		BindingProbe.Record($"late reporter enabled {_enables}");
	}
}

class Holder<T> : Behavior {
	public T held = default!;
	[Reflect]
	T spare = default!;

	protected override void OnFixedUpdate() {
		BindingProbe.Record($"generic fixed update {held} {spare}");
	}
}

sealed class HolderOfInt : Holder<int> {}

sealed class ThrowsInConstructor : Behavior {
	public ThrowsInConstructor() {
		throw new InvalidOperationException("thrown by a constructor");
	}

	protected override void OnUpdate() {
		BindingProbe.Record("update after a failed constructor");
	}
}

sealed class ThrowsInUpdate : Behavior {
	protected override void OnUpdate() {
		BindingProbe.Record("update before the throw");
		throw new InvalidOperationException("thrown by an update");
	}

	protected override void OnLateUpdate() {
		BindingProbe.Record("late update after the throw");
	}
}

static class ComponentProbe {
	static ulong Hash(string text) {
		ulong hash = 0xcbf29ce484222325UL;
		foreach (char character in text) {
			hash = (hash ^ character) * 0x00000100000001b3UL;
		}
		return hash;
	}

	[ConformanceTest]
	internal static void RegistersATakenName() {
		ScriptTypes.Register<Silent>("SceneObject", Hash("SceneObject"), 0UL, false, 0u, 0u, static () => new Silent(), default, default, null, null);
	}

	[ConformanceTest]
	internal static void RegistersAWrongHash() {
		ScriptTypes.Register<Silent>("Shalltear.Conformance.Misnamed", Hash("Shalltear.Conformance.Other"), 0UL, false, 0u, 0u, static () => new Silent(), default, default, null, null);
	}

	[ConformanceTest]
	internal static void RegistersAnUnknownParent() {
		ScriptTypes.Register<Silent>("Shalltear.Conformance.Orphan", Hash("Shalltear.Conformance.Orphan"), Hash("SceneObject"), false, 0u, 0u,
									 static () => new Silent(), default, default, null, null);
	}
}
