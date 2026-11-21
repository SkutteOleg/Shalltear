using System;
using System.ComponentModel;
using System.Threading;

namespace Shalltear.Interop;

/// <summary>Epoch-cached native thunk table lookup for user C++ modules.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public static unsafe class ModuleThunks {
	static ulong* _epochPtr;

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

	internal static void Reset() {
		_epochPtr = null;
	}
}
