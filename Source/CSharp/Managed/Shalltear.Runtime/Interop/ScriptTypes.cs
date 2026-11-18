using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Shalltear.Interop;

/// <summary>Called by generated code.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public static unsafe class ScriptTypes {
	const uint TYPE_ABSTRACT = 1;

	static readonly List<Func<Behavior>?> _factories = new();

	/// <summary>False outside the engine.</summary>
	public static bool IsAttached => Bridge.IsAttached;

	/// <summary>Registers 1 component class (parents before children).</summary>
	public static void Register(string name, ulong nameHash, ulong parentHash, bool isAbstract, uint hookMask, uint tickMask, Func<Behavior>? factory) {
		if (!Bridge.IsAttached) {
			return;
		}
		using UTF8Scratch nameText = new(name, stackalloc byte[UTF8Scratch.STACK_BYTES]);
		SltTypeDesc type;
		type.name = nameText.Text;
		type.nameHash = nameHash;
		type.parentHash = parentHash;
		type.managedTypeId = _factories.Count;
		type.flags = isAbstract ? TYPE_ABSTRACT : 0;
		type.hookMask = hookMask;
		type.tickMask = tickMask;
		Bridge.RegisterManagedType(&type);
		_factories.Add(factory);
	}

	internal static void Clear() {
		_factories.Clear();
	}

	[UnmanagedCallersOnly]
	internal static nint CreateInstance(int managedTypeId, ulong nativeId) {
		try {
			Func<Behavior>? factory = _factories[managedTypeId];
			if (factory == null) {
				return 0;
			}
			Behavior behavior = factory();
			behavior._nativeId = nativeId;
			return GCHandle.ToIntPtr(GCHandle.Alloc(behavior));
		} catch (Exception exception) {
			Bridge.Report(exception);
			return 0;
		}
	}

	[UnmanagedCallersOnly]
	internal static void DestroyInstance(nint instance) {
		try {
			GCHandle handle = GCHandle.FromIntPtr(instance);
			if (handle.Target is Behavior behavior) {
				behavior._nativeId = 0;
			}
			handle.Free();
		} catch (Exception exception) {
			Bridge.Report(exception);
		}
	}

	[UnmanagedCallersOnly]
	internal static void InvokeHook(nint instance, int hook) {
		try {
			Unsafe.As<Behavior>(GCHandle.FromIntPtr(instance).Target!).RunHook(hook);
		} catch (Exception exception) {
			Bridge.Report(exception);
		}
	}
}
