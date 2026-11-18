using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Shalltear.Interop;

/// <summary>Called by generated code.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public delegate void ScriptFieldSync(Behavior behavior, nint shadow);

/// <summary>Called by generated code.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public readonly struct ScriptField {
	internal readonly string _name;
	internal readonly ulong _nameHash;
	internal readonly int _kind;
	internal readonly uint _flags;
	internal readonly float _rangeMin;
	internal readonly float _rangeMax;
	internal readonly string? _tooltip;
	internal readonly string? _header;

	/// <summary>1 serialized field of a component class.</summary>
	public ScriptField(string name, ulong nameHash, int kind, uint flags, float rangeMin, float rangeMax, string? tooltip, string? header) {
		_name = name;
		_nameHash = nameHash;
		_kind = kind;
		_flags = flags;
		_rangeMin = rangeMin;
		_rangeMax = rangeMax;
		_tooltip = tooltip;
		_header = header;
	}
}

internal sealed class ScriptType {
	internal Func<Behavior>? _factory;
	internal ScriptFieldSync[] _push = [];
	internal ScriptFieldSync[] _pull = [];
}

/// <summary>Called by generated code.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public static unsafe class ScriptTypes {
	const uint TYPE_ABSTRACT = 1;

	static readonly List<ScriptType> _types = new();
	static readonly Dictionary<ulong, ScriptType> _byHash = new();

	/// <summary>False outside the engine.</summary>
	public static bool IsAttached => Bridge.IsAttached;

	/// <summary>Registers 1 component class (parents before children) + writes field offsets.</summary>
	public static void Register(string name, ulong nameHash, ulong parentHash, bool isAbstract, uint hookMask, uint tickMask, Func<Behavior>? factory,
								ReadOnlySpan<ScriptField> fields, Span<int> offsets, ScriptFieldSync? push, ScriptFieldSync? pull) {
		if (!Bridge.IsAttached) {
			return;
		}
		ScriptType type = new() { _factory = factory };
		if (_byHash.TryGetValue(parentHash, out ScriptType? parent)) {
			type._push = parent._push;
			type._pull = parent._pull;
		}
		if (push != null && pull != null) {
			type._push = [..type._push, push];
			type._pull = [..type._pull, pull];
		}

		int textBytes = Encoding.UTF8.GetByteCount(name);
		foreach (ScriptField field in fields) {
			textBytes += Encoding.UTF8.GetByteCount(field._name) + Encoding.UTF8.GetByteCount(field._tooltip ?? "") +
						 Encoding.UTF8.GetByteCount(field._header ?? "");
		}
		byte[] text = new byte[Math.Max(textBytes, 1)];
		SltFieldDesc[] descs = new SltFieldDesc[Math.Max(fields.Length, 1)];
		bool registered;
		fixed (byte* textStart = text)
		fixed (SltFieldDesc* descStart = descs) {
			byte* textEnd = textStart + text.Length;
			byte* at = textStart;
			for (int index = 0; index < fields.Length; ++index) {
				ScriptField field = fields[index];
				descStart[index].name = Put(field._name, ref at, textEnd);
				descStart[index].nameHash = field._nameHash;
				descStart[index].kind = field._kind;
				descStart[index].flags = field._flags;
				descStart[index].rangeMin = field._rangeMin;
				descStart[index].rangeMax = field._rangeMax;
				descStart[index].tooltip = Put(field._tooltip, ref at, textEnd);
				descStart[index].header = Put(field._header, ref at, textEnd);
			}
			SltTypeDesc desc;
			desc.name = Put(name, ref at, textEnd);
			desc.nameHash = nameHash;
			desc.parentHash = parentHash;
			desc.managedTypeId = _types.Count;
			desc.flags = isAbstract ? TYPE_ABSTRACT : 0;
			desc.hookMask = hookMask;
			desc.tickMask = tickMask;
			desc.fields = descStart;
			desc.fieldCount = fields.Length;
			registered = Bridge.RegisterManagedType(&desc);
		}
		_types.Add(type);
		if (!registered) {
			return;
		}
		_byHash[nameHash] = type;
		for (int index = 0; index < fields.Length; ++index) {
			offsets[index] = descs[index].offset;
		}
	}

	static SltString Put(string? value, ref byte* at, byte* end) {
		int written = Encoding.UTF8.GetBytes(value ?? "", new Span<byte>(at, (int)(end - at)));
		SltString put = new() { ptr = at, length = written };
		at += written;
		return put;
	}

	internal static void Clear() {
		_types.Clear();
		_byHash.Clear();
	}

	[UnmanagedCallersOnly]
	internal static nint CreateInstance(int managedTypeId, ulong nativeId) {
		try {
			ScriptType type = _types[managedTypeId];
			if (type._factory == null) {
				return 0;
			}
			Behavior behavior = type._factory();
			behavior._nativeId = nativeId;
			behavior._scriptType = type;
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

	[UnmanagedCallersOnly]
	internal static void PushFields(nint instance, byte* shadow) {
		try {
			Behavior behavior = Unsafe.As<Behavior>(GCHandle.FromIntPtr(instance).Target!);
			foreach (ScriptFieldSync push in behavior._scriptType!._push) {
				push(behavior, (nint)shadow);
			}
		} catch (Exception exception) {
			Bridge.Report(exception);
		}
	}

	[UnmanagedCallersOnly]
	internal static void PullFields(nint instance, byte* shadow, ulong nativeId) {
		try {
			Behavior behavior = Unsafe.As<Behavior>(GCHandle.FromIntPtr(instance).Target!);
			behavior._nativeId = nativeId;
			foreach (ScriptFieldSync pull in behavior._scriptType!._pull) {
				pull(behavior, (nint)shadow);
			}
		} catch (Exception exception) {
			Bridge.Report(exception);
		}
	}
}
