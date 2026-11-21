namespace Shalltear.Interop;

/// <summary>C++ addresses of engine objects (for callbacks taking raw pointers).</summary>
public static class Addresses {
	/// <summary>Engine object at a C++ address (null at 0).</summary>
	public static T? ObjectAt<T>(ulong address)
		where T : EngineObject {
		return address == 0 ? null : ObjectCache.Find<T>(AddressBindings.IdAt(address));
	}

	/// <summary>C++ address of an engine object (0 once destroyed).</summary>
	public static ulong Of(EngineObject target) {
		return AddressBindings.OfId(target._id);
	}
}
