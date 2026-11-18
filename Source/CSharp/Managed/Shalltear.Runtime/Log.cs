using System.Diagnostics;
using System.Runtime.CompilerServices;

using Shalltear.Interop;

namespace Shalltear;

/// <summary>Engine log (shown in Console).</summary>
public static class Log {
	const int TRACE = 0;
	const int DEBUG = 1;
	const int INFO = 2;
	const int WARN = 3;
	const int ERROR = 4;

	/// <summary>Trace row (calls compiled out without DEBUG).</summary>
	[Conditional("DEBUG")]
	[MethodImpl(MethodImplOptions.NoInlining)]
	public static void Trace(string category, string message) {
		LogOrigins.Write(TRACE, category, message);
	}

	/// <summary>Debug row (calls compiled out without DEBUG).</summary>
	[Conditional("DEBUG")]
	[MethodImpl(MethodImplOptions.NoInlining)]
	public static void Debug(string category, string message) {
		LogOrigins.Write(DEBUG, category, message);
	}

	/// <summary>Info row.</summary>
	[MethodImpl(MethodImplOptions.NoInlining)]
	public static void Info(string category, string message) {
		LogOrigins.Write(INFO, category, message);
	}

	/// <summary>Warning row.</summary>
	[MethodImpl(MethodImplOptions.NoInlining)]
	public static void Warn(string category, string message) {
		LogOrigins.Write(WARN, category, message);
	}

	/// <summary>Error row.</summary>
	[MethodImpl(MethodImplOptions.NoInlining)]
	public static void Error(string category, string message) {
		LogOrigins.Write(ERROR, category, message);
	}
}
