using System;
using System.Collections.Concurrent;

namespace Shalltear.Interop;

/// <summary>Engine object without an object id (base of generated wrappers over a C++ class or struct).</summary>
public abstract unsafe class NativeObject : IDisposable, IEquatable<NativeObject> {
	static readonly ConcurrentQueue<(nint pointer, nint release)> _pending = new();
	static volatile int _epoch;

	readonly int _createdEpoch = _epoch;
	nint _pointer;
	delegate* unmanaged<ulong*, ulong*, int> _release;
	bool _shared;
	internal object? _owner;

	private protected NativeObject(nint pointer, delegate* unmanaged<ulong*, ulong*, int> release) {
		_pointer = pointer;
		_release = release;
	}

	/// <summary>Called by the collector (owned C++ value freed at next frame).</summary>
	~NativeObject() {
		if (_pointer != 0 && _release != null && _createdEpoch == _epoch) {
			_pending.Enqueue((_pointer, (nint)_release));
		}
	}

	internal ulong Pointer {
		get {
			if (_pointer == 0 || _createdEpoch != _epoch) {
				throw new ObjectDisposedException(GetType().Name);
			}
			return (ulong)(nuint)_pointer;
		}
	}

	internal ulong Give(string member) {
		ulong pointer = Pointer;
		NativeObject holder = this;
		while (holder._release == null && holder._owner is NativeObject owner) {
			holder = owner;
		}
		if (holder._release == null || holder._shared) {
			throw new InvalidOperationException(member + " takes ownership of an object C# doesn't own");
		}
		holder._release = null;
		return pointer;
	}

	internal ulong Share(bool take) {
		ulong pointer = Pointer;
		NativeObject holder = this;
		while (holder._release == null && holder._owner is NativeObject owner) {
			holder = owner;
		}
		if (!take || holder._release == null || holder._shared) {
			return pointer;
		}
		holder._release = null;
		return pointer | 1UL;
	}

	/// <summary>False once disposed.</summary>
	public bool IsAlive => _pointer != 0 && _createdEpoch == _epoch;

	/// <summary>Address of the C++ object (0 once disposed).</summary>
	public ulong Address => _createdEpoch == _epoch ? (ulong)(nuint)_pointer : 0;

	internal static T? Wrap<T>(ulong pointer, bool owned, Func<nint, bool, T> make, object? owner)
		where T : NativeObject {
		if (pointer == 0) {
			return null;
		}
		T made = make((nint)pointer, owned);
		made._owner = owner;
		return made;
	}

	internal static T? WrapShared<T>(ulong pointer, Func<nint, bool, T> make)
		where T : NativeObject {
		if (pointer == 0) {
			return null;
		}
		T made = make((nint)pointer, false);
		made._release = Bindings.table.addressesReleaseShared;
		made._shared = true;
		return made;
	}

	/// <summary>Frees an owned C++ value now (else freed by the collector).</summary>
	public void Dispose() {
		nint pointer = _pointer;
		_pointer = 0;
		if (pointer != 0 && _release != null && _createdEpoch == _epoch) {
			if (Bridge.OffMainThread) {
				_pending.Enqueue((pointer, (nint)_release));
			} else {
				Free(pointer, _release);
			}
		}
		GC.SuppressFinalize(this);
	}

	static void Free(nint pointer, delegate* unmanaged<ulong*, ulong*, int> release) {
		ulong* slots = stackalloc ulong[1];
		slots[0] = (ulong)(nuint)pointer;
		release(slots, slots);
	}

	internal static void Drain() {
		(nint pointer, nint release) entry;
		while (_pending.TryDequeue(out entry)) {
			if (Bridge.IsAttached) {
				Free(entry.pointer, (delegate* unmanaged<ulong*, ulong*, int>)entry.release);
			}
		}
	}

	// older natives point into an unloaded image, don't fucking free them
	internal static void Forget() {
		Drain();
		++_epoch;
		_pending.Clear();
	}

	/// <inheritdoc/>
	public bool Equals(NativeObject? other) {
		return other is not null && other._pointer == _pointer;
	}

	/// <inheritdoc/>
	public override bool Equals(object? other) {
		return other is NativeObject native && native._pointer == _pointer;
	}

	/// <inheritdoc/>
	public override int GetHashCode() {
		return _pointer.GetHashCode();
	}
}
