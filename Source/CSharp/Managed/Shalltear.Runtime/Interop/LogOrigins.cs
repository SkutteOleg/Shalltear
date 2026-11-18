using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Diagnostics.CodeAnalysis;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Threading;

namespace Shalltear.Interop;

internal static class LogOrigins {
	sealed class Origin {
		public string file = "";
		public int line;
	}

	const int FRAMES_TO_CALLER = 3;

	static readonly ConcurrentDictionary<(nint method, int offset), Origin> _sites = new();
	static readonly Origin _none = new();
	static int _resolved;

	internal static int ResolvedCount => _resolved;

	internal static int SiteCount => _sites.Count;

	[MethodImpl(MethodImplOptions.NoInlining)]
	internal static void Write(int level, string category, string message) {
		if (level < LogBindings.Level) {
			return;
		}
		Origin origin = RuntimeFeatures.LogOrigins ? Find() : _none;
		LogBindings.Write(level, category, message, origin.file, origin.line);
	}

	internal static void Clear() {
		_sites.Clear();
	}

	internal static bool FindThrower(Exception exception, out string file, out int line) {
		file = "";
		line = 0;
		return RuntimeFeatures.LogOrigins && FindUserFrame(exception, ref file, ref line);
	}

	[MethodImpl(MethodImplOptions.NoInlining)]
	[RequiresUnreferencedCode("Editor runtime only")]
	static Origin Find() {
		StackFrame frame = new(FRAMES_TO_CALLER, false);
		MethodBase? method = frame.GetMethod();
		if (method == null) {
			return _none;
		}
		ValueTuple<nint, int> site = new(method.MethodHandle.Value, frame.GetILOffset());
		if (_sites.TryGetValue(site, out Origin? known)) {
			return known;
		}
		StackFrame resolved = new(FRAMES_TO_CALLER, true);
		Origin origin = new() { file = resolved.GetFileName() ?? "", line = resolved.GetFileLineNumber() };
		Interlocked.Increment(ref _resolved);
		_sites[site] = origin;
		return origin;
	}

	[RequiresUnreferencedCode("Editor runtime only")]
	static bool FindUserFrame(Exception exception, ref string file, ref int line) {
		if (exception.InnerException != null && FindUserFrame(exception.InnerException, ref file, ref line)) {
			return true;
		}
		Assembly engine = typeof(LogOrigins).Assembly;
		StackTrace trace = new(exception, true);
		for (int index = 0; index < trace.FrameCount; ++index) {
			StackFrame? frame = trace.GetFrame(index);
			string? name = frame?.GetFileName();
			if (frame == null || string.IsNullOrEmpty(name) || frame.GetMethod()?.Module.Assembly == engine) {
				continue;
			}
			file = name;
			line = frame.GetFileLineNumber();
			return true;
		}
		return false;
	}
}
