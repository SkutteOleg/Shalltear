using System;
using System.Collections.Concurrent;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace Shalltear.Interop;

internal unsafe delegate void SlotCallback(ulong* slots, ulong* results);

internal static unsafe class SlotCallbacks {
	const int MISSING = 2;
	const int THROWN = 1;
	const int TEXT_BYTES = 256;
	const int LIST_BYTES = 256;

	static readonly ConcurrentDictionary<ulong, SlotCallback> _kept = new();
	static ulong _last;

	[ThreadStatic]
	static byte[]? _text;

	[ThreadStatic]
	static byte[]? _list;

	internal static ulong Keep(SlotCallback? callback) {
		if (callback == null) {
			return 0;
		}
		ulong id = Interlocked.Increment(ref _last);
		_kept[id] = callback;
		return id;
	}

	internal static int Count => _kept.Count;

	internal static void Drop(ulong id) {
		_kept.TryRemove(id, out _);
	}

	internal static void Forget() {
		_kept.Clear();
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
		results[0] = (ulong)Unsafe.AsPointer(ref MemoryMarshal.GetArrayDataReference(_text));
		results[1] = (ulong)Encoding.UTF8.GetBytes(value, _text);
	}

	static byte* KeepBytes(int size) {
		if (_list == null || _list.Length < size) {
			_list = GC.AllocateUninitializedArray<byte>(size > LIST_BYTES ? size : LIST_BYTES, true);
		}
		return (byte*)Unsafe.AsPointer(ref MemoryMarshal.GetArrayDataReference(_list));
	}

	internal static void KeepArray<T>(ulong* results, T[]? values)
		where T : unmanaged {
		int count = values?.Length ?? 0;
		byte* kept = KeepBytes(count * sizeof(T));
		if (count != 0) {
			values.AsSpan().CopyTo(new Span<T>(kept, count));
		}
		results[0] = (ulong)kept;
		results[1] = (ulong)count;
	}

	internal static void KeepNatives<T>(ulong* results, T?[]? values)
		where T : NativeObject {
		int count = values?.Length ?? 0;
		nint* kept = (nint*)KeepBytes(count * sizeof(nint));
		for (int index = 0; index < count; ++index) {
			T? item = values![index];
			kept[index] = item is null ? 0 : (nint)(nuint)item.Pointer;
		}
		results[0] = (ulong)kept;
		results[1] = (ulong)count;
	}

	[UnmanagedCallersOnly]
	internal static int Invoke(ulong id, ulong* arguments, ulong* results) {
		try {
			if (!_kept.TryGetValue(id, out SlotCallback? callback)) {
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
		_kept.TryRemove(id, out _);
	}
}
