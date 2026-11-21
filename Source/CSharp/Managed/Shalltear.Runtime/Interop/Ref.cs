namespace Shalltear.Interop;

/// <summary>C++ value a callback may change (valid until the callback returns).</summary>
public readonly unsafe struct Ref<T>
	where T : unmanaged {
	readonly T* _pointer;

	/// <summary>Reference to the value at a C++ address.</summary>
	public Ref(ulong address) {
		_pointer = (T*)address;
	}

	/// <summary>Value (read or assign).</summary>
	public ref T Value => ref *_pointer;
}
