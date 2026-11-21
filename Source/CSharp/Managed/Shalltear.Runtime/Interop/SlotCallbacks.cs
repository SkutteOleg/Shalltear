using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Shalltear.Interop;

internal unsafe delegate void SlotCallback(ulong* slots, ulong* results);

internal static unsafe class SlotCallbacks {
	const int MISSING = 2;
	const int THROWN = 1;
	const int TEXT_BYTES = 256;

	static readonly object _gate = new();
	static readonly Dictionary<ulong, SlotCallback> _kept = new();
	static ulong _last;

	[ThreadStatic]
	static byte[]? _text;

	internal static ulong Keep(SlotCallback? callback) {
		if (callback == null) {
			return 0;
		}
		lock (_gate) {
			_kept.Add(++_last, callback);
			return _last;
		}
	}

	internal static int Count {
		get {
			lock (_gate) {
				return _kept.Count;
			}
		}
	}

	internal static void Drop(ulong id) {
		lock (_gate) {
			_kept.Remove(id);
		}
	}

	internal static void Forget() {
		lock (_gate) {
			_kept.Clear();
		}
	}

	internal static void KeepText(ulong* results, string? value) {
		if (string.IsNullOrEmpty(value)) {
			results[0] = 0;
			results[1] = 0;
			return;
		}
		int size = Encoding.UTF8.GetByteCount(value);
		if (_text == null || _text.Length < size) {
			_text = GC.AllocateUninitializedArray<byte>(size > TEXT_BYTES ? size : TEXT_BYTES, true);
		}
		results[0] = (ulong)(nint)Unsafe.AsPointer(ref MemoryMarshal.GetArrayDataReference(_text));
		results[1] = (ulong)Encoding.UTF8.GetBytes(value, _text);
	}

	[UnmanagedCallersOnly]
	internal static int Invoke(ulong id, ulong* arguments, ulong* results) {
		try {
			SlotCallback? callback;
			lock (_gate) {
				_kept.TryGetValue(id, out callback);
			}
			if (callback == null) {
				return MISSING;
			}
			callback(arguments, results);
			return Bridge.OK;
		} catch (Exception exception) {
			Bridge.Report(exception);
			return THROWN;
		}
	}

	[UnmanagedCallersOnly]
	internal static void Release(ulong id) {
		lock (_gate) {
			_kept.Remove(id);
		}
	}
}
