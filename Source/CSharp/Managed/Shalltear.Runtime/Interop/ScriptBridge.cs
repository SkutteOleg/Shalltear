using System.Runtime.InteropServices;

// must match ScriptBridge.h
#pragma warning disable CS0649

namespace Shalltear.Interop;

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct SltString {
	public byte* ptr;
	public int length;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct SltFieldDesc {
	public SltString name;
	public ulong nameHash;
	public int kind;
	public uint flags;
	public float rangeMin;
	public float rangeMax;
	public SltString tooltip;
	public SltString header;
	public ulong typeHash;
	public int offset;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct SltEnumConstant {
	public SltString name;
	public long value;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct SltEnumDesc {
	public SltString name;
	public ulong nameHash;
	public int size;
	public SltEnumConstant* constants;
	public int constantCount;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct SltValueDesc {
	public SltString name;
	public ulong nameHash;
	public SltFieldDesc* fields;
	public int fieldCount;
	public int size;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct SltTypeDesc {
	public SltString name;
	public ulong nameHash;
	public ulong parentHash;
	public int managedTypeId;
	public uint flags;
	public uint hookMask;
	public uint tickMask;
	public SltFieldDesc* fields;
	public int fieldCount;
	public SltString script;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct SltEngineAPI {
	public uint abiVersion;
	public ulong layoutHash;
	public int textSize;
	public delegate* unmanaged<SltString, SltString, SltString, void> reportException;
	public delegate* unmanaged<int, SltString, void> log;
	public delegate* unmanaged<SltString*, void> getLastError;
	public delegate* unmanaged<SltTypeDesc*, int> registerManagedType;
	public delegate* unmanaged<SltEnumDesc*, int> registerManagedEnum;
	public delegate* unmanaged<SltValueDesc*, int> registerManagedValue;
	public delegate* unmanaged<void*, SltString, void> stringAssign;
	public delegate* unmanaged<void*, SltString*, void> stringRead;
	public delegate* unmanaged<void*, int, void*> listResize;
	public delegate* unmanaged<void*, int*, void*> listData;
	public delegate* unmanaged<ulong, nint> scriptOf;
	public delegate* unmanaged<int> throwForTest;
	public SltBindings bindings;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct SltManagedAPI {
	public uint abiVersion;
	public ulong layoutHash;
	public delegate* unmanaged<SltString*, int, int> loadUserAssemblies;
	public delegate* unmanaged<int> loadKeptUserAssemblies;
	public delegate* unmanaged<int> unloadUserAssemblies;
	public delegate* unmanaged<int, ulong, nint> createInstance;
	public delegate* unmanaged<nint, void> destroyInstance;
	public delegate* unmanaged<nint, int, void> invokeHook;
	public delegate* unmanaged<nint, byte*, void> pushFields;
	public delegate* unmanaged<nint, byte*, ulong, void> pullFields;
	public delegate* unmanaged<int> getConformanceTestCount;
	public delegate* unmanaged<int, SltString*, int> getConformanceTestName;
	public delegate* unmanaged<int, SltString*, int> runConformanceTest;
	public delegate* unmanaged<void> tick;
	public delegate* unmanaged<void> shutdown;
}
