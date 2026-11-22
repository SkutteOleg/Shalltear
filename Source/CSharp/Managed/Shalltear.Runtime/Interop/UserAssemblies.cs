using System;
using System.Diagnostics.CodeAnalysis;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;

namespace Shalltear.Interop;

[RequiresUnreferencedCode("Editor runtime only")]
[RequiresDynamicCode("Editor runtime only")]
internal static unsafe class UserAssemblies {
	[RequiresUnreferencedCode("Editor runtime only")]
	[RequiresDynamicCode("Editor runtime only")]
	sealed class Context : AssemblyLoadContext {
		readonly string _directory;

		public Context(string directory) : base("Shalltear.Scripts", isCollectible: true) {
			_directory = directory;
		}

		protected override Assembly? Load(AssemblyName name) {
			Assembly runtime = typeof(Bridge).Assembly;
			if (name.Name == runtime.GetName().Name) {
				return runtime;
			}
			if (name.Name != null && name.Name.StartsWith("Shalltear.", StringComparison.Ordinal)) {
				try {
					return GetLoadContext(runtime)!.LoadFromAssemblyName(name);
				} catch (FileNotFoundException) {
				}
			}
			string candidate = Path.Combine(_directory, name.Name + ".dll");
			return File.Exists(candidate) ? LoadUnlocked(candidate) : null;
		}

		public Assembly LoadUnlocked(string path) {
			return LoadWithSymbols(File.ReadAllBytes(path), ReadSymbols(path));
		}

		public Assembly LoadWithSymbols(byte[] image, byte[]? symbols) {
			return LoadFromStream(new MemoryStream(image), symbols == null ? null : new MemoryStream(symbols));
		}
	}

	sealed class Images {
		public string directory = "";
		public byte[][] bytes = [];
		public byte[]?[] symbols = [];
	}

	static byte[]? ReadSymbols(string assembly) {
		string symbols = Path.ChangeExtension(assembly, ".pdb");
		return File.Exists(symbols) ? File.ReadAllBytes(symbols) : null;
	}

	static Context? _context;
	static Images? _loaded;
	static Images? _kept;

	[UnmanagedCallersOnly]
	internal static int Load(SltString* paths, int count) {
		try {
			if (_context != null) {
				throw new InvalidOperationException("user assemblies are already loaded");
			}
			if (count <= 0) {
				return Bridge.OK;
			}
			string first = Encoding.UTF8.GetString(paths[0].ptr, paths[0].length);
			Images images = new() { directory = Path.GetDirectoryName(Path.GetFullPath(first)) ?? "", bytes = new byte[count][], symbols = new byte[]?[count] };
			for (int index = 0; index < count; ++index) {
				string path = Path.GetFullPath(Encoding.UTF8.GetString(paths[index].ptr, paths[index].length));
				images.bytes[index] = File.ReadAllBytes(path);
				images.symbols[index] = ReadSymbols(path);
			}
			LoadImages(images);
			return Bridge.OK;
		} catch (Exception exception) {
			Bridge.Report(exception);
			return Bridge.FAILED;
		}
	}

	internal static void Forget() {
		_loaded = null;
		_kept = null;
	}

	[UnmanagedCallersOnly]
	internal static int LoadKept() {
		try {
			if (_context != null) {
				throw new InvalidOperationException("user assemblies are already loaded");
			}
			if (_kept == null) {
				return Bridge.FAILED;
			}
			LoadImages(_kept);
			return Bridge.OK;
		} catch (Exception exception) {
			Bridge.Report(exception);
			return Bridge.FAILED;
		}
	}

	static void LoadImages(Images images) {
		Context context = new(images.directory);
		_context = context;
		try {
			for (int index = 0; index < images.bytes.Length; ++index) {
				Assembly assembly = context.LoadWithSymbols(images.bytes[index], images.symbols[index]);
				RuntimeHelpers.RunModuleConstructor(assembly.ManifestModule.ModuleHandle);
			}
		} catch {
			Release();
			throw;
		}
		_loaded = images;
	}

	[UnmanagedCallersOnly]
	internal static int Unload() {
		try {
			if (_loaded != null) {
				_kept = _loaded;
				_loaded = null;
			}
			return Release();
		} catch (Exception exception) {
			Bridge.Report(exception);
			return Bridge.FAILED;
		}
	}

	// context must leave this frame's stack before collection
	[MethodImpl(MethodImplOptions.NoInlining)]
	static WeakReference Detach() {
		Context? context = _context;
		_context = null;
		ConformanceRegistry.Clear();
		ScriptTypes.Clear();
		LogOrigins.Clear();
		ScriptEvents.Clear();
		FrameAwaits.Forget();
		NativeFunctions.Forget();
		SlotCallbacks.Forget();
		ModuleThunks.ForgetCallbacks();
		ModuleThunks.DrainDeletes();
		ObjectCache.ForgetUser();
		UnloadHooks.Run();
		if (context == null) {
			return new WeakReference(null);
		}
		context.Unload();
		return new WeakReference(context);
	}

	static int Release() {
		WeakReference alive = Detach();
		for (int attempt = 0; attempt < 10 && alive.IsAlive; ++attempt) {
			GC.Collect();
			GC.WaitForPendingFinalizers();
			ModuleThunks.DrainDeletes();
		}
		return alive.IsAlive ? 1 : Bridge.OK;
	}
}
