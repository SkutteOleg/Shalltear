using System;
using System.Buffers;
using System.Collections.Concurrent;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Unicode;
using System.Threading;

namespace Shalltear.Interop;

/// <summary>Managed callback invoker unpacked from a native slot buffer.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public unsafe delegate void ModuleCallbackInvoker(ulong* args);

/// <summary>Stack-backed null-terminated UTF-8 scratch buffer for generated user-module interop.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public unsafe ref struct NativeUTF8Buffer {
	byte* _ptr;
	byte* _heap;
	int _length;

	/// <summary>Encode text as null-terminated UTF-8 into stack (falling back to NativeMemory when larger).</summary>
	public NativeUTF8Buffer(string? text, Span<byte> stack) {
		_heap = null;
		if (string.IsNullOrEmpty(text)) {
			if (stack.Length > 0) {
				stack[0] = 0;
				_ptr = (byte*)Unsafe.AsPointer(ref MemoryMarshal.GetReference(stack));
			} else {
				_heap = (byte*)NativeMemory.Alloc(1);
				_heap[0] = 0;
				_ptr = _heap;
			}
			_length = 0;
			return;
		}
		if (stack.Length > 1 && Utf8.FromUtf16(text, stack.Slice(0, stack.Length - 1), out _, out int written) == OperationStatus.Done) {
			stack[written] = 0;
			_ptr = (byte*)Unsafe.AsPointer(ref MemoryMarshal.GetReference(stack));
			_length = written;
			return;
		}
		int exact = Encoding.UTF8.GetByteCount(text);
		_heap = (byte*)NativeMemory.Alloc((nuint)(exact + 1));
		if (exact > 0) {
			Encoding.UTF8.GetBytes(text, new Span<byte>(_heap, exact));
		}
		_heap[exact] = 0;
		_ptr = _heap;
		_length = exact;
	}

	/// <summary>Null-terminated UTF-8 pointer valid until Dispose.</summary>
	public readonly byte* Pointer => _ptr;

	/// <summary>Byte count excluding the trailing null terminator.</summary>
	public readonly int Length => _length;

	/// <summary>Release any heap fallback allocation.</summary>
	public void Dispose() {
		if (_heap != null) {
			NativeMemory.Free(_heap);
			_heap = null;
		}
		_ptr = null;
		_length = 0;
	}
}

/// <summary>Epoch-cached native thunk table lookup, main-thread delete queue, and default-ALC callback registry for user C++ modules.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public static unsafe class ModuleThunks {
	readonly struct PendingDelete {
		public readonly string moduleName;
		public readonly int slotIndex;
		public readonly nint ptr;
		public readonly ulong createdEpoch;

		public PendingDelete(string moduleName, int slotIndex, nint ptr, ulong createdEpoch) {
			this.moduleName = moduleName;
			this.slotIndex = slotIndex;
			this.ptr = ptr;
			this.createdEpoch = createdEpoch;
		}
	}

	static readonly ConcurrentQueue<PendingDelete> _pendingDeletes = new();
	static readonly ConcurrentDictionary<ulong, ModuleCallbackInvoker> _callbacks = new();
	static ulong* _epochPtr;
	static ulong _lastCallbackToken;

	/// <summary>Current monotonic registry epoch.</summary>
	public static ulong CurrentEpoch {
		get {
			ulong* ptr = _epochPtr;
			if (ptr == null) {
				if (!Bridge.IsAttached) {
					return 0;
				}
				ptr = (ulong*)(nint)ModuleThunkBindings.GetEpochAddress();
				_epochPtr = ptr;
			}
			return ptr != null ? Volatile.Read(ref *ptr) : ModuleThunkBindings.GetEpoch();
		}
	}

	/// <summary>Current per-module registration epoch for moduleName (0 when unregistered).</summary>
	public static ulong GetModuleEpoch(string moduleName) {
		if (!Bridge.IsAttached || string.IsNullOrEmpty(moduleName)) {
			return 0;
		}
		return ModuleThunkBindings.GetModuleEpoch(moduleName);
	}

	/// <summary>Refresh table pointer and slot count for moduleName when the registry epoch changes.</summary>
	public static nint ResolveSlot(string moduleName, int slotIndex, ref ulong cachedEpoch, ref nint cachedTable, ref uint cachedCount) {
		ulong epoch = CurrentEpoch;
		if (epoch == 0 || epoch != cachedEpoch || cachedTable == 0) {
			cachedTable = (nint)ModuleThunkBindings.GetTable(moduleName);
			cachedCount = ModuleThunkBindings.GetCount(moduleName);
			cachedEpoch = epoch;
		}
		if (cachedTable == 0) {
			throw new EngineException("Native module '" + moduleName + "' thunk table is not registered. Build and load the C++ module first.");
		}
		if ((uint)slotIndex >= cachedCount) {
			throw new EngineException("Native module '" + moduleName + "' thunk slot " + slotIndex + " out of range (" + cachedCount + " slots). Rebuild the C++ module.");
		}
		nint fn = ((nint*)cachedTable)[slotIndex];
		if (fn == 0) {
			throw new EngineException("Native module '" + moduleName + "' thunk slot " + slotIndex + " is null.");
		}
		return fn;
	}

	/// <summary>Delete an owned native C++ object immediately on the main thread or enqueue for main-thread drain.</summary>
	public static void DeleteOrEnqueue(string moduleName, int slotIndex, nint ptr, ulong createdModuleEpoch,
									   ref ulong cachedEpoch, ref nint cachedTable, ref uint cachedCount) {
		if (ptr == 0 || createdModuleEpoch == 0 || !Bridge.IsAttached) {
			return;
		}
		ulong liveEpoch = ModuleThunkBindings.GetModuleEpoch(moduleName);
		if (liveEpoch == 0 || liveEpoch != createdModuleEpoch) {
			return;
		}
		if (Bridge.IsMainThread) {
			nint fn = ResolveSlot(moduleName, slotIndex, ref cachedEpoch, ref cachedTable, ref cachedCount);
			((delegate* unmanaged[Cdecl]<void*, void>)fn)((void*)ptr);
			return;
		}
		_pendingDeletes.Enqueue(new PendingDelete(moduleName, slotIndex, ptr, createdModuleEpoch));
	}

	/// <summary>Queue an owned native C++ object for main-thread deletion (safe from GC finalizer thread).</summary>
	public static void EnqueueDelete(string moduleName, int slotIndex, nint ptr, ulong createdModuleEpoch) {
		if (ptr == 0 || createdModuleEpoch == 0 || !Bridge.IsAttached) {
			return;
		}
		_pendingDeletes.Enqueue(new PendingDelete(moduleName, slotIndex, ptr, createdModuleEpoch));
	}

	/// <summary>Register a managed callback invoker in the default ALC and return its generation token.</summary>
	public static ulong KeepCallback(ModuleCallbackInvoker? invoker) {
		if (invoker == null) {
			return 0;
		}
		ulong token = Interlocked.Increment(ref _lastCallbackToken);
		_callbacks[token] = invoker;
		return token;
	}

	/// <summary>Function pointer to the default-ALC callback invocation trampoline.</summary>
	public static nint CallbackInvokePtr => (nint)(delegate* unmanaged[Cdecl]<ulong, ulong*, void>)&InvokeCallback;

	/// <summary>Function pointer to the default-ALC callback release trampoline.</summary>
	public static nint CallbackReleasePtr => (nint)(delegate* unmanaged[Cdecl]<ulong, void>)&ReleaseCallback;

	[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
	static void InvokeCallback(ulong token, ulong* args) {
		if (token == 0 || !Bridge.IsAttached) {
			return;
		}
		try {
			if (_callbacks.TryGetValue(token, out ModuleCallbackInvoker? invoker)) {
				invoker(args);
			}
		} catch (Exception exception) {
			Bridge.Report(exception);
		}
	}

	[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
	static void ReleaseCallback(ulong token) {
		if (token != 0) {
			_callbacks.TryRemove(token, out _);
		}
	}

	internal static void DrainDeletes() {
		while (_pendingDeletes.TryDequeue(out PendingDelete entry)) {
			if (!Bridge.IsAttached) {
				continue;
			}
			ulong liveEpoch = ModuleThunkBindings.GetModuleEpoch(entry.moduleName);
			if (liveEpoch == 0 || liveEpoch != entry.createdEpoch) {
				continue;
			}
			try {
				nint table = (nint)ModuleThunkBindings.GetTable(entry.moduleName);
				uint count = ModuleThunkBindings.GetCount(entry.moduleName);
				if (table == 0 || (uint)entry.slotIndex >= count) {
					continue;
				}
				nint fn = ((nint*)table)[entry.slotIndex];
				if (fn != 0) {
					((delegate* unmanaged[Cdecl]<void*, void>)fn)((void*)entry.ptr);
				}
			} catch (Exception exception) {
				Bridge.Report(exception);
			}
		}
	}

	internal static void ForgetCallbacks() {
		_callbacks.Clear();
	}

	internal static void Reset() {
		DrainDeletes();
		_callbacks.Clear();
		while (_pendingDeletes.TryDequeue(out _)) {
		}
		_epochPtr = null;
	}
}
