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
internal unsafe struct SltEngineAPI {
	public uint abiVersion;
	public ulong layoutHash;
	public delegate* unmanaged<SltString, SltString, SltString, void> reportException;
	public delegate* unmanaged<int, SltString, void> log;
	public delegate* unmanaged<SltString*, void> getLastError;
	public delegate* unmanaged<int> throwForTest;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct SltManagedAPI {
	public uint abiVersion;
	public ulong layoutHash;
	public delegate* unmanaged<SltString*, int, int> loadUserAssemblies;
	public delegate* unmanaged<int> unloadUserAssemblies;
	public delegate* unmanaged<int> getConformanceTestCount;
	public delegate* unmanaged<int, SltString*, int> getConformanceTestName;
	public delegate* unmanaged<int, SltString*, int> runConformanceTest;
	public delegate* unmanaged<void> shutdown;
}
