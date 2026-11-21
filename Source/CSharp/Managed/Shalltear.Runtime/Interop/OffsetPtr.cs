namespace Shalltear.Interop;

/// <summary>Self-relative pointer (in engine memory, reached through a Ref).</summary>
public unsafe struct OffsetPtr<T>
	where T : unmanaged {
	long _offset;

	/// <summary>Address of the target (0 when unset).</summary>
	public ulong Get() {
		fixed (long* self = &_offset) {
			return _offset == 0 ? 0 : (ulong)((byte*)self + _offset);
		}
	}

	/// <summary>Points at an address (0 to unset).</summary>
	public void Set(ulong address) {
		fixed (long* self = &_offset) {
			_offset = address == 0 ? 0 : (long)((byte*)address - (byte*)self);
		}
	}

	/// <summary>Target as a value to read or assign.</summary>
	public Ref<T> Target => new(Get());
}
