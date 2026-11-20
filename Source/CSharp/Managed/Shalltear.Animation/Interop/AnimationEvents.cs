using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace Shalltear.Interop;

public static unsafe partial class AnimationBindings {
	static partial void Attached() {
		AnimationEvents.Attach();
	}
}

// must match AnimationBindings.cpp
internal static unsafe class AnimationEvents {
	const int PRUNE_EVERY = 64;

	readonly struct Entry {
		internal readonly ulong _animator;
		internal readonly Action<AnimationEvent> _handler;

		internal Entry(ulong animator, Action<AnimationEvent> handler) {
			_animator = animator;
			_handler = handler;
		}
	}

	static readonly Dictionary<ulong, Entry> _handlers = new();
	static readonly List<ulong> _dead = new();
	static ulong _nextKey;
	static void* _table;

	internal static void Attach() {
		_table = Bridge.FindBindings("Animation", AnimationBindings.LAYOUT_HASH, sizeof(SltAnimationBindings));
		AnimationEventBindings.Attach((long)(delegate* unmanaged<ulong, AnimationEvent*, void>)&OnEvent);
		UnloadHooks.Add(Forget);
	}

	internal static uint Subscribe(Animator animator, Action<AnimationEvent> handler) {
		ArgumentNullException.ThrowIfNull(handler);
		if (++_nextKey % PRUNE_EVERY == 0) {
			Prune();
		}
		ulong key = _nextKey;
		uint token = animator.Listen(key, "Animator.Subscribe");
		_handlers[key] = new Entry(animator._id, handler);
		return token;
	}

	internal static bool Unsubscribe(Animator animator, uint token) {
		ulong key = animator.Unlisten(token, "Animator.Unsubscribe");
		return key != 0 && _handlers.Remove(key);
	}

	static void Prune() {
		foreach (KeyValuePair<ulong, Entry> held in _handlers) {
			if (!ObjectBindings.IsAlive(held.Value._animator)) {
				_dead.Add(held.Key);
			}
		}
		foreach (ulong key in _dead) {
			_handlers.Remove(key);
		}
		_dead.Clear();
	}

	static void Forget() {
		if (_table != null && Bridge.HoldsBindings("Animation", AnimationBindings.LAYOUT_HASH, sizeof(SltAnimationBindings), _table)) {
			AnimationEventBindings.ForgetAll();
		}
		_table = null;
		_handlers.Clear();
	}

	[UnmanagedCallersOnly]
	static void OnEvent(ulong key, AnimationEvent* animationEvent) {
		try {
			if (_handlers.TryGetValue(key, out Entry entry)) {
				entry._handler(*animationEvent);
			}
		} catch (Exception exception) {
			Bridge.Report(exception);
		}
	}
}
