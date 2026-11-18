using System.Runtime.InteropServices;

using Shalltear.Interop;

namespace Shalltear.Generated;

static unsafe class Exports {
	[UnmanagedCallersOnly(EntryPoint = "ShalltearScriptsInitialize")]
	static int Initialize(SltEngineAPI* engine, SltManagedAPI* managed) {
		ConformanceTable.Register();
		return Bridge.Attach(engine, managed);
	}
}
