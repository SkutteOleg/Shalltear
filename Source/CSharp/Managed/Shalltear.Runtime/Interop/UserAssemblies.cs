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
			string candidate = Path.Combine(_directory, name.Name + ".dll");
			return File.Exists(candidate) ? LoadUnlocked(candidate) : null;
		}

		public Assembly LoadUnlocked(string path) {
			return LoadFromStream(new MemoryStream(File.ReadAllBytes(path)));
		}
	}

	sealed class Images {
		public string directory = "";
		public byte[][] bytes = [];
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
			Images images = new() { directory = Path.GetDirectoryName(Path.GetFullPath(first)) ?? "", bytes = new byte[count][] };
			for (int index = 0; index < count; ++index) {
				string path = Encoding.UTF8.GetString(paths[index].ptr, paths[index].length);
				images.bytes[index] = File.ReadAllBytes(Path.GetFullPath(path));
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
			foreach (byte[] image in images.bytes) {
				Assembly assembly = context.LoadFromStream(new MemoryStream(image));
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
		}
		return alive.IsAlive ? 1 : Bridge.OK;
	}
}
