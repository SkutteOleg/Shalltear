using System;
using System.Runtime.InteropServices;

namespace Shalltear.Interop;

internal static class ScriptEvents {
	// must match ScriptEvents.h
	internal const int SCENE_LOADED = 0;
	internal const int SCENE_UNLOADING = 1;
	const int COUNT = 2;

	static readonly Delegate?[] _handlers = new Delegate?[COUNT];

	internal static void Add(int id, Delegate? handler) {
		if (handler == null) {
			return;
		}
		bool first = _handlers[id] == null;
		if (first) {
			EventBindings.SetListening(id, true);
		}
		_handlers[id] = Delegate.Combine(_handlers[id], handler);
	}

	internal static void Remove(int id, Delegate? handler) {
		if (_handlers[id] == null) {
			return;
		}
		_handlers[id] = Delegate.Remove(_handlers[id], handler);
		if (_handlers[id] == null) {
			EventBindings.SetListening(id, false);
		}
	}

	internal static void Clear() {
		for (int id = 0; id < COUNT; ++id) {
			if (_handlers[id] != null) {
				_handlers[id] = null;
				EventBindings.SetListening(id, false);
			}
		}
	}

	internal static void Forget() {
		Array.Clear(_handlers);
	}

	[UnmanagedCallersOnly]
	internal static void Dispatch(int id, ulong subject) {
		try {
			if (id < 0 || id >= COUNT || _handlers[id] is not Action<Scene> handlers) {
				return;
			}
			Scene? scene = ObjectCache.Find<Scene>(subject);
			if (scene is null) {
				return;
			}
			foreach (Action<Scene> handler in Delegate.EnumerateInvocationList(handlers)) {
				try {
					handler(scene);
				} catch (Exception exception) {
					Bridge.Report(exception);
				}
			}
		} catch (Exception exception) {
			Bridge.Report(exception);
		}
	}
}
