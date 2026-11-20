using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace Shalltear.Interop;

// must match UIBindings.cpp
internal static unsafe class UIEvents {
	const int PRUNE_EVERY = 64;

	readonly struct Entry {
		internal readonly UIPanelRenderer _panel;
		internal readonly Action<UIEventArgs> _handler;

		internal Entry(UIPanelRenderer panel, Action<UIEventArgs> handler) {
			_panel = panel;
			_handler = handler;
		}
	}

	static readonly Dictionary<ulong, Entry> _handlers = new();
	static readonly List<ulong> _dead = new();
	static ulong _nextKey;
	static bool _attached;

	internal static int Count => _handlers.Count;

	internal static void Set(UIPanelRenderer panel, ulong element, UIEvent kind, Action<UIEventArgs>? handler) {
		ulong replaced;
		if (handler is null) {
			replaced = UIElementBindings.Unlisten(panel, element, kind, "UIElement.SetHandler");
		} else {
			if (!_attached) {
				UIEventBindings.Attach((long)(delegate* unmanaged<ulong, byte, double, SltString, ulong, void>)&OnEvent);
				UnloadHooks.Add(Forget);
				_attached = true;
			}
			if (++_nextKey % PRUNE_EVERY == 0) {
				Prune();
			}
			ulong key = _nextKey;
			replaced = UIElementBindings.Listen(panel, element, kind, key, "UIElement.SetHandler");
			_handlers[key] = new Entry(panel, handler);
		}
		if (replaced != 0) {
			_handlers.Remove(replaced);
		}
	}

	static void Prune() {
		foreach (KeyValuePair<ulong, Entry> held in _handlers) {
			if (!held.Value._panel) {
				_dead.Add(held.Key);
			}
		}
		foreach (ulong key in _dead) {
			_handlers.Remove(key);
		}
		_dead.Clear();
	}

	static void Forget() {
		if (_attached) {
			UIEventBindings.ForgetAll();
		}
		_handlers.Clear();
		_attached = false;
	}

	[UnmanagedCallersOnly]
	static void OnEvent(ulong key, byte kind, double value, SltString text, ulong element) {
		try {
			if (_handlers.TryGetValue(key, out Entry entry)) {
				entry._handler(new UIEventArgs((UIEvent)kind, new UIElement(entry._panel, element), value, Bridge.Text(text)));
			}
		} catch (Exception exception) {
			Bridge.Report(exception);
		}
	}
}
