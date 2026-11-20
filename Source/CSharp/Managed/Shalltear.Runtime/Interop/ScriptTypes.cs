using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Shalltear.Interop;

/// <summary>Called by generated code.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public delegate void ScriptFieldSync(EngineObject owner, nint shadow);

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
	internal readonly ulong _typeHash;

	/// <summary>1 serialized field (typeHash = its enum, value or referenced type).</summary>
	public ScriptField(string name, ulong nameHash, int kind, uint flags, float rangeMin, float rangeMax, string? tooltip, string? header, ulong typeHash) {
		_name = name;
		_nameHash = nameHash;
		_kind = kind;
		_flags = flags;
		_rangeMin = rangeMin;
		_rangeMax = rangeMax;
		_tooltip = tooltip;
		_header = header;
		_typeHash = typeHash;
	}
}

/// <summary>Called by generated code.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public readonly struct ScriptEnumConstant {
	internal readonly string _name;
	internal readonly ulong _nameHash;
	internal readonly long _value;

	/// <summary>1 constant of a serialized enum.</summary>
	public ScriptEnumConstant(string name, ulong nameHash, long value) {
		_name = name;
		_nameHash = nameHash;
		_value = value;
	}
}

internal sealed class ScriptType {
	internal Func<EngineObject>? _factory;
	internal ScriptFieldSync[] _push = [];
	internal ScriptFieldSync[] _pull = [];
	internal int _texts;
}

/// <summary>Called by generated code.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public static unsafe class ScriptTypes {
	const uint TYPE_ABSTRACT = 1;
	const uint TYPE_NORMALIZE = 2;
	const uint TYPE_DATA_ASSET = 4;

	static readonly List<ScriptType> _types = new();
	static readonly Dictionary<ulong, ScriptType> _byHash = new();
	static readonly List<(int slot, Type listener)> _listeners = new();

	/// <summary>False outside the engine.</summary>
	public static bool IsAttached => Bridge.IsAttached;

	/// <summary>Registers 1 serialized enum (before types using it).</summary>
	public static void RegisterEnum(string name, ulong nameHash, int size, ReadOnlySpan<ScriptEnumConstant> constants) {
		if (!Bridge.IsAttached) {
			return;
		}
		int textBytes = Encoding.UTF8.GetByteCount(name);
		foreach (ScriptEnumConstant constant in constants) {
			textBytes += Encoding.UTF8.GetByteCount(constant._name);
		}
		byte[] text = new byte[Math.Max(textBytes, 1)];
		SltEnumConstant[] descs = new SltEnumConstant[Math.Max(constants.Length, 1)];
		fixed (byte* textStart = text)
		fixed (SltEnumConstant* descStart = descs) {
			byte* textEnd = textStart + text.Length;
			byte* at = textStart;
			for (int index = 0; index < constants.Length; ++index) {
				descStart[index].name = Put(constants[index]._name, ref at, textEnd);
				descStart[index].nameHash = constants[index]._nameHash;
				descStart[index].value = constants[index]._value;
			}
			SltEnumDesc desc;
			desc.name = Put(name, ref at, textEnd);
			desc.nameHash = nameHash;
			desc.size = size;
			desc.constants = descStart;
			desc.constantCount = constants.Length;
			Bridge.RegisterManagedEnum(&desc);
		}
	}

	/// <summary>Registers 1 serialized value type + writes field offsets. Returns its size.</summary>
	public static int RegisterValue(string name, ulong nameHash, ReadOnlySpan<ScriptField> fields, Span<int> offsets) {
		if (!Bridge.IsAttached) {
			return 0;
		}
		byte[] text = new byte[TextBytes(name, fields)];
		SltFieldDesc[] descs = new SltFieldDesc[Math.Max(fields.Length, 1)];
		fixed (byte* textStart = text)
		fixed (SltFieldDesc* descStart = descs) {
			byte* textEnd = textStart + text.Length;
			byte* at = textStart;
			Describe(fields, descStart, ref at, textEnd);
			SltValueDesc desc;
			desc.name = Put(name, ref at, textEnd);
			desc.nameHash = nameHash;
			desc.fields = descStart;
			desc.fieldCount = fields.Length;
			desc.size = 0;
			if (!Bridge.RegisterManagedValue(&desc)) {
				return 0;
			}
			for (int index = 0; index < fields.Length; ++index) {
				offsets[index] = descs[index].offset;
			}
			return desc.size;
		}
	}

	/// <summary>Registers 1 component class (parents before children) + writes field offsets.</summary>
	public static void Register<T>(string name, ulong nameHash, ulong parentHash, bool isAbstract, uint hookMask, uint tickMask, Func<EngineObject>? factory,
								   string? script, ReadOnlySpan<ScriptField> fields, Span<int> offsets, ScriptFieldSync? push, ScriptFieldSync? pull,
								   bool normalizes, int texts, out int textBase, string? assetMenu = null, string? assetExtension = null)
		where T : EngineObject {
		textBase = 0;
		if (!Bridge.IsAttached) {
			return;
		}
		ScriptType type = new() { _factory = factory };
		if (_byHash.TryGetValue(parentHash, out ScriptType? parent)) {
			type._push = parent._push;
			type._pull = parent._pull;
			textBase = parent._texts;
		}
		type._texts = textBase + texts;
		if (push != null && pull != null) {
			type._push = [..type._push, push];
			type._pull = [..type._pull, pull];
		}

		byte[] text = new byte[TextBytes(name, fields) + Encoding.UTF8.GetByteCount(script ?? "") + Encoding.UTF8.GetByteCount(assetMenu ?? "") +
							   Encoding.UTF8.GetByteCount(assetExtension ?? "")];
		SltFieldDesc[] descs = new SltFieldDesc[Math.Max(fields.Length, 1)];
		bool registered;
		fixed (byte* textStart = text)
		fixed (SltFieldDesc* descStart = descs) {
			byte* textEnd = textStart + text.Length;
			byte* at = textStart;
			Describe(fields, descStart, ref at, textEnd);
			SltTypeDesc desc;
			desc.name = Put(name, ref at, textEnd);
			desc.nameHash = nameHash;
			desc.parentHash = parentHash;
			desc.managedTypeId = _types.Count;
			desc.flags = (isAbstract ? TYPE_ABSTRACT : 0) | (normalizes ? TYPE_NORMALIZE : 0) | (typeof(DataAsset).IsAssignableFrom(typeof(T)) ? TYPE_DATA_ASSET : 0);
			desc.hookMask = hookMask;
			desc.tickMask = tickMask;
			desc.listenerMask = 0;
			foreach ((int slot, Type listener) in _listeners) {
				desc.listenerMask |= listener.IsAssignableFrom(typeof(T)) ? 1u << slot : 0;
			}
			desc.fields = descStart;
			desc.fieldCount = fields.Length;
			desc.script = Put(script, ref at, textEnd);
			desc.assetMenu = Put(assetMenu, ref at, textEnd);
			desc.assetExtension = Put(assetExtension, ref at, textEnd);
			registered = Bridge.RegisterManagedType(&desc);
		}
		_types.Add(type);
		if (!registered) {
			return;
		}
		_byHash[nameHash] = type;
		ScriptIndex<T>.value = _types.Count;
		ScriptTypeHash<T>.value = nameHash;
		for (int index = 0; index < fields.Length; ++index) {
			offsets[index] = descs[index].offset;
		}
	}

	static int TextBytes(string name, ReadOnlySpan<ScriptField> fields) {
		int bytes = Encoding.UTF8.GetByteCount(name);
		foreach (ScriptField field in fields) {
			bytes += Encoding.UTF8.GetByteCount(field._name) + Encoding.UTF8.GetByteCount(field._tooltip ?? "") + Encoding.UTF8.GetByteCount(field._header ?? "");
		}
		return Math.Max(bytes, 1);
	}

	static void Describe(ReadOnlySpan<ScriptField> fields, SltFieldDesc* descs, ref byte* at, byte* end) {
		for (int index = 0; index < fields.Length; ++index) {
			ScriptField field = fields[index];
			descs[index].name = Put(field._name, ref at, end);
			descs[index].nameHash = field._nameHash;
			descs[index].kind = field._kind;
			descs[index].flags = field._flags;
			descs[index].rangeMin = field._rangeMin;
			descs[index].rangeMax = field._rangeMax;
			descs[index].tooltip = Put(field._tooltip, ref at, end);
			descs[index].header = Put(field._header, ref at, end);
			descs[index].typeHash = field._typeHash;
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

	internal static void AddListener(int slot, Type listener) {
		_listeners.RemoveAll(held => held.slot == slot);
		_listeners.Add((slot, listener));
	}

	internal static void ForgetListeners() {
		_listeners.Clear();
	}

	[UnmanagedCallersOnly]
	internal static nint CreateInstance(int managedTypeId, ulong nativeId) {
		try {
			ScriptType type = _types[managedTypeId];
			if (type._factory == null) {
				return 0;
			}
			EngineObject script = type._factory();
			script._id = nativeId;
			script._scriptType = type;
			return GCHandle.ToIntPtr(GCHandle.Alloc(script));
		} catch (Exception exception) {
			Bridge.Report(exception);
			return 0;
		}
	}

	[UnmanagedCallersOnly]
	internal static void DestroyInstance(nint instance) {
		try {
			GCHandle.FromIntPtr(instance).Free();
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
			EngineObject script = Unsafe.As<EngineObject>(GCHandle.FromIntPtr(instance).Target!);
			foreach (ScriptFieldSync push in script._scriptType!._push) {
				push(script, (nint)shadow);
			}
		} catch (Exception exception) {
			Bridge.Report(exception);
		}
	}

	[UnmanagedCallersOnly]
	internal static void PullFields(nint instance, byte* shadow, ulong nativeId) {
		try {
			EngineObject script = Unsafe.As<EngineObject>(GCHandle.FromIntPtr(instance).Target!);
			script._id = nativeId;
			foreach (ScriptFieldSync pull in script._scriptType!._pull) {
				pull(script, (nint)shadow);
			}
		} catch (Exception exception) {
			Bridge.Report(exception);
		}
	}
}
