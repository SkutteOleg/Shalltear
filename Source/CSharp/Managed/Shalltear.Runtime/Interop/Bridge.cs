using System;
using System.Diagnostics.CodeAnalysis;
using System.Runtime.CompilerServices;
using System.Runtime.ExceptionServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Shalltear.Interop;

internal static unsafe class Bridge {
	internal const uint ABI_VERSION = 12;
	internal const ulong LAYOUT_HASH = Bindings.LAYOUT_HASH;

	internal const int OK = 0;
	internal const int FAILED = -1;
	const int ABI_MISMATCH = 1;
	const int LAYOUT_MISMATCH = 2;
	const int DESTROYED = 3;

	static SltEngineAPI _engine;
	static volatile bool _attached;
	static int _mainThreadId;
	static bool _handlerInstalled;

	[UnmanagedCallersOnly]
	[UnconditionalSuppressMessage("Trimming", "IL2026", Justification = "Editor runtime only")]
	[UnconditionalSuppressMessage("AOT", "IL3050", Justification = "Editor runtime only")]
	static int Initialize(SltEngineAPI* engine, SltManagedAPI* managed) {
		int result = Attach(engine, managed);
		if (result == OK) {
			UserAssemblies.Forget();
			managed->loadUserAssemblies = &UserAssemblies.Load;
			managed->loadKeptUserAssemblies = &UserAssemblies.LoadKept;
			managed->unloadUserAssemblies = &UserAssemblies.Unload;
		}
		return result;
	}

	internal static int Attach(SltEngineAPI* engine, SltManagedAPI* managed) {
		managed->abiVersion = ABI_VERSION;
		if (engine->abiVersion != ABI_VERSION) {
			return ABI_MISMATCH;
		}
		managed->layoutHash = LAYOUT_HASH;
		if (engine->layoutHash != LAYOUT_HASH) {
			return LAYOUT_MISMATCH;
		}

		_engine = *engine;
		Bindings.table = engine->bindings;
		_mainThreadId = Environment.CurrentManagedThreadId;
		_attached = true;
		managed->loadUserAssemblies = null;
		managed->loadKeptUserAssemblies = null;
		managed->unloadUserAssemblies = null;
		managed->createInstance = &ScriptTypes.CreateInstance;
		managed->destroyInstance = &ScriptTypes.DestroyInstance;
		managed->invokeHook = &ScriptTypes.InvokeHook;
		managed->pushFields = &ScriptTypes.PushFields;
		managed->pullFields = &ScriptTypes.PullFields;
		managed->getConformanceTestCount = &GetConformanceTestCount;
		managed->getConformanceTestName = &GetConformanceTestName;
		managed->runConformanceTest = &RunConformanceTest;
		managed->tick = &Tick;
		managed->shutdown = &Shutdown;
		managed->dispatchEvent = &ScriptEvents.Dispatch;

		if (!_handlerInstalled) {
			_handlerInstalled = true;
			ExceptionHandling.SetUnhandledExceptionHandler(static exception => {
				Report(exception);
				return true;
			});
		}
		return OK;
	}

	internal static bool IsAttached => _attached;

	internal static bool RegisterManagedType(SltTypeDesc* type) {
		return _engine.registerManagedType(type) == OK;
	}

	internal static bool RegisterManagedEnum(SltEnumDesc* type) {
		return _engine.registerManagedEnum(type) == OK;
	}

	internal static bool RegisterManagedValue(SltValueDesc* type) {
		return _engine.registerManagedValue(type) == OK;
	}

	internal static int TextSize => _engine.textSize;

	internal static void StringAssign(nint field, SltString text) {
		_engine.stringAssign((void*)field, text);
	}

	internal static SltString StringRead(nint field) {
		SltString text;
		_engine.stringRead((void*)field, &text);
		return text;
	}

	internal static nint ListResize(nint field, int count) {
		return (nint)_engine.listResize((void*)field, count);
	}

	internal static nint ListData(nint field, out int count) {
		int found;
		nint data = (nint)_engine.listData((void*)field, &found);
		count = found;
		return data;
	}

	internal static nint ScriptOf(ulong id) {
		return _engine.scriptOf(id);
	}

	internal static void* FindBindings(string name, ulong layoutHash, int size) {
		byte[] bytes = Encoding.UTF8.GetBytes(name);
		void* table;
		fixed (byte* ptr = bytes) {
			if (_engine.findBindings(new SltString { ptr = ptr, length = bytes.Length }, layoutHash, size, &table) != OK) {
				throw LastError();
			}
		}
		return table;
	}

	internal static ulong HashOf(string name) {
		ulong hash = 0xcbf29ce484222325UL;
		foreach (byte value in Encoding.UTF8.GetBytes(name)) {
			hash = (hash ^ value) * 0x00000100000001b3UL;
		}
		return hash;
	}

	internal static bool OffMainThread {
		[MethodImpl(MethodImplOptions.AggressiveInlining)]
		get => RuntimeFeatures.ThreadChecks && Environment.CurrentManagedThreadId != _mainThreadId;
	}

	[DoesNotReturn]
	internal static void ThrowOffMainThread(string member) {
		throw new InvalidOperationException(member + " called off main thread. Call from main thread");
	}

	internal static void Report(Exception exception) {
		if (!_attached) {
			return;
		}
		string said = exception.Message;
		string frames = exception.StackTrace ?? "";
		for (Exception? inner = exception.InnerException; inner != null; inner = inner.InnerException) {
			said += " ---> " + (inner.GetType().FullName ?? inner.GetType().Name) + ": " + inner.Message;
			frames = (inner.StackTrace ?? "") + "\n   --- End of inner exception stack trace ---\n" + frames;
		}
		LogOrigins.FindThrower(exception, out string thrower, out int line);
		byte[] type = Encoding.UTF8.GetBytes(exception.GetType().FullName ?? exception.GetType().Name);
		byte[] message = Encoding.UTF8.GetBytes(said);
		byte[] trace = Encoding.UTF8.GetBytes(frames);
		byte[] file = Encoding.UTF8.GetBytes(thrower);
		fixed (byte* typePtr = type, messagePtr = message, tracePtr = trace, filePtr = file) {
			_engine.reportException(
				new SltString { ptr = typePtr, length = type.Length },
				new SltString { ptr = messagePtr, length = message.Length },
				new SltString { ptr = tracePtr, length = trace.Length },
				new SltString { ptr = filePtr, length = file.Length },
				line);
		}
	}

	internal static void Log(int level, string text) {
		if (!_attached) {
			return;
		}
		byte[] bytes = Encoding.UTF8.GetBytes(text);
		fixed (byte* ptr = bytes) {
			_engine.log(level, new SltString { ptr = ptr, length = bytes.Length });
		}
	}

	internal static EngineException LastError() {
		SltString text;
		_engine.getLastError(&text);
		return new EngineException(text.length > 0 ? Encoding.UTF8.GetString(text.ptr, text.length) : "engine call failed");
	}

	internal static Exception Failure(int status, string member) {
		return status == DESTROYED ? new DestroyedObjectException(member) : LastError();
	}

	internal static string Text(SltString text) {
		return text.length > 0 ? Encoding.UTF8.GetString(text.ptr, text.length) : "";
	}

	internal static void ThrowForTest() {
		if (_engine.throwForTest == null) {
			throw new InvalidOperationException("engine built without test hooks");
		}
		if (_engine.throwForTest() != OK) {
			throw LastError();
		}
	}

	[UnmanagedCallersOnly]
	static int GetConformanceTestCount() {
		return ConformanceRegistry.Count;
	}

	[UnmanagedCallersOnly]
	static int GetConformanceTestName(int index, SltString* outName) {
		try {
			ConformanceRegistry.GetName(index, outName);
			return OK;
		} catch (Exception exception) {
			Report(exception);
			return FAILED;
		}
	}

	[UnmanagedCallersOnly]
	static int RunConformanceTest(int index, SltString* outMessage) {
		try {
			return ConformanceRegistry.Run(index, outMessage);
		} catch (Exception exception) {
			Report(exception);
			return FAILED;
		}
	}

	[UnmanagedCallersOnly]
	static void Tick() {
		try {
			ObjectCache.Sweep();
			InputActionMap.FreeRetired();
		} catch (Exception exception) {
			Report(exception);
		}
	}

	[UnmanagedCallersOnly]
	static void Shutdown() {
		ScriptEvents.Forget();
		InputActionMap.Forget();
		ScriptTypes.Clear();
		ScriptTypes.ForgetListeners();
		ObjectCache.Clear();
		_attached = false;
	}
}
