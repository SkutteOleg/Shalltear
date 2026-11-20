using System;
using System.Collections.Generic;

namespace Shalltear.Interop;

internal static class UnloadHooks {
	static readonly List<Action> _hooks = new();

	internal static void Add(Action hook) {
		if (!_hooks.Contains(hook)) {
			_hooks.Add(hook);
		}
	}

	internal static void Run() {
		foreach (Action hook in _hooks) {
			try {
				hook();
			} catch (Exception exception) {
				Bridge.Report(exception);
			}
		}
	}
}
