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
			return (ulong)_pointer;
		}
	}

	/// <summary>False once disposed.</summary>
	public bool IsAlive => _pointer != 0 && _createdEpoch == _epoch;

	internal static T? Wrap<T>(ulong pointer, bool owned, Func<nint, bool, T> make, object? owner)
		where T : NativeObject {
		if (pointer == 0) {
			return null;
		}
		T made = make((nint)pointer, owned);
		made._owner = owner;
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
		slots[0] = (ulong)pointer;
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
