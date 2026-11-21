using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace Shalltear.Interop;

/// <summary>Called by generated code.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public struct NativeFunction {
	internal nint _thunk;
	internal ulong _context;
	internal int _epoch;
}

/// <summary>Called by generated code.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public delegate void NativeExport(ref NativeFrame frame);

// must match ScriptFunctionDetail::Slot in ScriptFunctions.h
internal static unsafe class NativeSlots {
	const int TEXT_BYTES = 4096;

	[ThreadStatic]
	static byte[]? _text;

	[ThreadStatic]
	static int _textUsed;

	[ThreadStatic]
	static List<byte[]>? _spilled;

	[ThreadStatic]
	static byte[]? _returned;

	internal static ulong Pack(float low, float high) {
		return BitConverter.SingleToUInt32Bits(low) | ((ulong)BitConverter.SingleToUInt32Bits(high) << 32);
	}

	internal static float Low(ulong slot) {
		return BitConverter.UInt32BitsToSingle((uint)slot);
	}

	internal static float High(ulong slot) {
		return BitConverter.UInt32BitsToSingle((uint)(slot >> 32));
	}

	internal static int Mark => _textUsed;

	internal static void Release(int mark) {
		_textUsed = mark;
		if (mark == 0) {
			_spilled?.Clear();
		}
	}

	internal static void Keep(string? value, ulong* slots) {
		if (string.IsNullOrEmpty(value)) {
			slots[0] = 0;
			slots[1] = 0;
			return;
		}
		int bytes = Encoding.UTF8.GetByteCount(value);
		_text ??= GC.AllocateUninitializedArray<byte>(TEXT_BYTES, pinned: true);
		byte[] target = _text;
		int at = _textUsed;
		if (at + bytes > target.Length) {
			target = GC.AllocateUninitializedArray<byte>(bytes, pinned: true);
			(_spilled ??= new()).Add(target);
			at = 0;
		} else {
			_textUsed = at + bytes;
		}
		Encoding.UTF8.GetBytes(value, target.AsSpan(at, bytes));
		slots[0] = (ulong)(nint)Marshal.UnsafeAddrOfPinnedArrayElement(target, at);
		slots[1] = (ulong)bytes;
	}

	internal static void Return(string? value, ulong* slots) {
		if (string.IsNullOrEmpty(value)) {
			slots[0] = 0;
			slots[1] = 0;
			return;
		}
		int bytes = Encoding.UTF8.GetByteCount(value);
		if (_returned == null || _returned.Length < bytes) {
			_returned = GC.AllocateUninitializedArray<byte>(Math.Max(bytes, TEXT_BYTES), pinned: true);
		}
		Encoding.UTF8.GetBytes(value, _returned);
		slots[0] = (ulong)(nint)Marshal.UnsafeAddrOfPinnedArrayElement(_returned, 0);
		slots[1] = (ulong)bytes;
	}

	internal static string Text(ulong* slots) {
		return slots[1] == 0 ? "" : Encoding.UTF8.GetString((byte*)(nint)slots[0], (int)slots[1]);
	}
}

/// <summary>Called by generated code.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public static unsafe class NativeFunctions {
	internal static int _epoch = 1;

	static readonly List<NativeExport> _exports = new();

	/// <summary>Registers 1 method for the project's C++.</summary>
	public static void Export(string name, string signature, NativeExport handler) {
		if (!Bridge.IsAttached) {
			return;
		}
		_exports.Add(handler);
		FunctionBindings.Export(name, signature, (ulong)_exports.Count);
	}

	internal static void Bind(ref NativeFunction function, string name, string signature) {
		long* found = stackalloc long[2];
		FunctionBindings.Find(name, signature, new Span<long>(found, 2));
		function._thunk = (nint)found[0];
		function._context = (ulong)found[1];
		function._epoch = _epoch;
	}

	internal static void Forget() {
		_exports.Clear();
		++_epoch;
	}

	[UnmanagedCallersOnly]
	internal static void CallExport(ulong id, ulong* arguments, ulong* result) {
		try {
			if (id == 0 || id > (ulong)_exports.Count) {
				return;
			}
			NativeFrame frame = new(arguments, result);
			_exports[(int)(id - 1)](ref frame);
		} catch (Exception exception) {
			Bridge.Report(exception);
		}
	}

	[UnmanagedCallersOnly]
	internal static void FunctionsChanged() {
		++_epoch;
	}
}

/// <summary>Called by generated code.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public unsafe ref struct NativeCall {
	readonly Span<ulong> _slots;
	readonly nint _thunk;
	readonly ulong _context;
	readonly int _mark;
	int _at;
	int _read;

	/// <summary>Starts 1 call (slots = argument slots + 2).</summary>
	public NativeCall(ref NativeFunction function, string name, string signature, Span<ulong> slots) {
		if (function._epoch != NativeFunctions._epoch) {
			NativeFunctions.Bind(ref function, name, signature);
		}
		_thunk = function._thunk;
		_context = function._context;
		_slots = slots;
		_mark = NativeSlots.Mark;
		_at = 0;
		_read = slots.Length - 2;
		slots.Slice(_read).Clear();
	}

	/// <summary>Next argument.</summary>
	public void Put(bool value) {
		_slots[_at++] = value ? 1UL : 0UL;
	}

	/// <summary>Next argument.</summary>
	public void Put(long value) {
		_slots[_at++] = (ulong)value;
	}

	/// <summary>Next argument.</summary>
	public void Put(ulong value) {
		_slots[_at++] = value;
	}

	/// <summary>Next argument.</summary>
	public void Put(float value) {
		_slots[_at++] = BitConverter.SingleToUInt32Bits(value);
	}

	/// <summary>Next argument.</summary>
	public void Put(double value) {
		_slots[_at++] = BitConverter.DoubleToUInt64Bits(value);
	}

	/// <summary>Next argument.</summary>
	public void Put(Vec2 value) {
		_slots[_at++] = NativeSlots.Pack(value.x, value.y);
	}

	/// <summary>Next argument.</summary>
	public void Put(Vec3 value) {
		_slots[_at++] = NativeSlots.Pack(value.x, value.y);
		_slots[_at++] = NativeSlots.Pack(value.z, 0f);
	}

	/// <summary>Next argument.</summary>
	public void Put(Vec4 value) {
		_slots[_at++] = NativeSlots.Pack(value.x, value.y);
		_slots[_at++] = NativeSlots.Pack(value.z, value.w);
	}

	/// <summary>Next argument.</summary>
	public void Put(Quat value) {
		_slots[_at++] = NativeSlots.Pack(value.x, value.y);
		_slots[_at++] = NativeSlots.Pack(value.z, value.w);
	}

	/// <summary>Next argument.</summary>
	public void Put(Color value) {
		_slots[_at++] = NativeSlots.Pack(value.r, value.g);
		_slots[_at++] = NativeSlots.Pack(value.b, value.a);
	}

	/// <summary>Next argument.</summary>
	public void Put(EngineObject? value) {
		_slots[_at++] = value is null ? 0UL : value._id;
	}

	/// <summary>Next argument.</summary>
	public void Put(string? value) {
		fixed (ulong* slots = _slots) {
			NativeSlots.Keep(value, slots + _at);
		}
		_at += 2;
	}

	/// <summary>Runs the call.</summary>
	public void Invoke() {
		fixed (ulong* slots = _slots) {
			((delegate* unmanaged<ulong, ulong*, ulong*, void>)_thunk)(_context, slots, slots + _read);
		}
		NativeSlots.Release(_mark);
	}

	/// <summary>Result.</summary>
	public bool GetBoolean() {
		return _slots[_read] != 0;
	}

	/// <summary>Result.</summary>
	public long GetInt64() {
		return (long)_slots[_read];
	}

	/// <summary>Result.</summary>
	public ulong GetUInt64() {
		return _slots[_read];
	}

	/// <summary>Result.</summary>
	public float GetSingle() {
		return NativeSlots.Low(_slots[_read]);
	}

	/// <summary>Result.</summary>
	public double GetDouble() {
		return BitConverter.UInt64BitsToDouble(_slots[_read]);
	}

	/// <summary>Result.</summary>
	public Vec2 GetVec2() {
		return new Vec2(NativeSlots.Low(_slots[_read]), NativeSlots.High(_slots[_read]));
	}

	/// <summary>Result.</summary>
	public Vec3 GetVec3() {
		return new Vec3(NativeSlots.Low(_slots[_read]), NativeSlots.High(_slots[_read]), NativeSlots.Low(_slots[_read + 1]));
	}

	/// <summary>Result.</summary>
	public Vec4 GetVec4() {
		return new Vec4(NativeSlots.Low(_slots[_read]), NativeSlots.High(_slots[_read]), NativeSlots.Low(_slots[_read + 1]), NativeSlots.High(_slots[_read + 1]));
	}

	/// <summary>Result.</summary>
	public Quat GetQuat() {
		return new Quat(NativeSlots.Low(_slots[_read]), NativeSlots.High(_slots[_read]), NativeSlots.Low(_slots[_read + 1]), NativeSlots.High(_slots[_read + 1]));
	}

	/// <summary>Result.</summary>
	public Color GetColor() {
		return new Color(NativeSlots.Low(_slots[_read]), NativeSlots.High(_slots[_read]), NativeSlots.Low(_slots[_read + 1]), NativeSlots.High(_slots[_read + 1]));
	}

	/// <summary>Result.</summary>
	public T? GetObject<T>()
		where T : EngineObject {
		return ObjectCache.Find<T>(_slots[_read]);
	}

	/// <summary>Result.</summary>
	public string GetString() {
		fixed (ulong* slots = _slots) {
			return NativeSlots.Text(slots + _read);
		}
	}
}

/// <summary>Called by generated code.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public unsafe ref struct NativeFrame {
	readonly ulong* _arguments;
	readonly ulong* _result;
	int _at;

	internal NativeFrame(ulong* arguments, ulong* result) {
		_arguments = arguments;
		_result = result;
		_at = 0;
	}

	/// <summary>Next argument.</summary>
	public bool GetBoolean() {
		return _arguments[_at++] != 0;
	}

	/// <summary>Next argument.</summary>
	public long GetInt64() {
		return (long)_arguments[_at++];
	}

	/// <summary>Next argument.</summary>
	public ulong GetUInt64() {
		return _arguments[_at++];
	}

	/// <summary>Next argument.</summary>
	public float GetSingle() {
		return NativeSlots.Low(_arguments[_at++]);
	}

	/// <summary>Next argument.</summary>
	public double GetDouble() {
		return BitConverter.UInt64BitsToDouble(_arguments[_at++]);
	}

	/// <summary>Next argument.</summary>
	public Vec2 GetVec2() {
		ulong slot = _arguments[_at++];
		return new Vec2(NativeSlots.Low(slot), NativeSlots.High(slot));
	}

	/// <summary>Next argument.</summary>
	public Vec3 GetVec3() {
		_at += 2;
		return new Vec3(NativeSlots.Low(_arguments[_at - 2]), NativeSlots.High(_arguments[_at - 2]), NativeSlots.Low(_arguments[_at - 1]));
	}

	/// <summary>Next argument.</summary>
	public Vec4 GetVec4() {
		_at += 2;
		return new Vec4(NativeSlots.Low(_arguments[_at - 2]), NativeSlots.High(_arguments[_at - 2]), NativeSlots.Low(_arguments[_at - 1]), NativeSlots.High(_arguments[_at - 1]));
	}

	/// <summary>Next argument.</summary>
	public Quat GetQuat() {
		_at += 2;
		return new Quat(NativeSlots.Low(_arguments[_at - 2]), NativeSlots.High(_arguments[_at - 2]), NativeSlots.Low(_arguments[_at - 1]), NativeSlots.High(_arguments[_at - 1]));
	}

	/// <summary>Next argument.</summary>
	public Color GetColor() {
		_at += 2;
		return new Color(NativeSlots.Low(_arguments[_at - 2]), NativeSlots.High(_arguments[_at - 2]), NativeSlots.Low(_arguments[_at - 1]), NativeSlots.High(_arguments[_at - 1]));
	}

	/// <summary>Next argument.</summary>
	public T? GetObject<T>()
		where T : EngineObject {
		return ObjectCache.Find<T>(_arguments[_at++]);
	}

	/// <summary>Next argument.</summary>
	public string GetString() {
		_at += 2;
		return NativeSlots.Text(_arguments + _at - 2);
	}

	/// <summary>Result.</summary>
	public void Return(bool value) {
		_result[0] = value ? 1UL : 0UL;
	}

	/// <summary>Result.</summary>
	public void Return(long value) {
		_result[0] = (ulong)value;
	}

	/// <summary>Result.</summary>
	public void Return(ulong value) {
		_result[0] = value;
	}

	/// <summary>Result.</summary>
	public void Return(float value) {
		_result[0] = BitConverter.SingleToUInt32Bits(value);
	}

	/// <summary>Result.</summary>
	public void Return(double value) {
		_result[0] = BitConverter.DoubleToUInt64Bits(value);
	}

	/// <summary>Result.</summary>
	public void Return(Vec2 value) {
		_result[0] = NativeSlots.Pack(value.x, value.y);
	}

	/// <summary>Result.</summary>
	public void Return(Vec3 value) {
		_result[0] = NativeSlots.Pack(value.x, value.y);
		_result[1] = NativeSlots.Pack(value.z, 0f);
	}

	/// <summary>Result.</summary>
	public void Return(Vec4 value) {
		_result[0] = NativeSlots.Pack(value.x, value.y);
		_result[1] = NativeSlots.Pack(value.z, value.w);
	}

	/// <summary>Result.</summary>
	public void Return(Quat value) {
		_result[0] = NativeSlots.Pack(value.x, value.y);
		_result[1] = NativeSlots.Pack(value.z, value.w);
	}

	/// <summary>Result.</summary>
	public void Return(Color value) {
		_result[0] = NativeSlots.Pack(value.r, value.g);
		_result[1] = NativeSlots.Pack(value.b, value.a);
	}

	/// <summary>Result.</summary>
	public void Return(EngineObject? value) {
		_result[0] = value is null ? 0UL : value._id;
	}

	/// <summary>Result.</summary>
	public void Return(string? value) {
		NativeSlots.Return(value, _result);
	}
}
