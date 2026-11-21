using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Shalltear.HeaderBinder;

internal static class Program {
	public static int Main(string[] args) {
		List<string> allArgs = new(args.Length);
		for (int i = 0; i < args.Length; ++i) {
			if (args[i] == "--args-file" && i + 1 < args.Length) {
				string rsp = args[++i];
				if (!string.IsNullOrWhiteSpace(rsp) && File.Exists(rsp)) {
					foreach (string rawLine in File.ReadAllLines(rsp)) {
						string line = rawLine.Trim();
						if (line.Length == 0 || line.StartsWith("#", StringComparison.Ordinal)) {
							continue;
						}
						allArgs.Add(line);
					}
				}
			} else {
				allArgs.Add(args[i]);
			}
		}

		ScanOptions options = new();
		string? outCpp = null;
		string? outCs = null;

		for (int i = 0; i < allArgs.Count; ++i) {
			string rawArg = allArgs[i];
			string arg = rawArg;
			string? inlineVal = null;
			int eq = rawArg.IndexOf('=');
			if (rawArg.StartsWith("--", StringComparison.Ordinal) && eq > 2) {
				arg = rawArg.Substring(0, eq);
				inlineVal = rawArg.Substring(eq + 1);
			}
			string? Next() => inlineVal ?? (i + 1 < allArgs.Count ? allArgs[++i] : null);
			switch (arg) {
			case "--module":
				options.ModuleName = Next() ?? "";
				break;
			case "--header":
				AddSplit(options.Headers, Next());
				break;
			case "--namespace":
				AddSplit(options.BindNamespaces, Next());
				break;
			case "--include":
				AddSplit(options.IncludeDirs, Next());
				break;
			case "--define":
				AddSplit(options.Defines, Next());
				break;
			case "--clang-arg":
				string? extra = Next();
				if (!string.IsNullOrWhiteSpace(extra)) {
					options.ExtraArgs.Add(extra);
				}
				break;
			case "--libclang-dir":
				options.LibClangDir = Next();
				break;
			case "--sysroot":
				options.Sysroot = Next();
				break;
			case "--out-cpp":
				outCpp = Next();
				break;
			case "--out-cs":
				outCs = Next();
				break;
			}
		}

		if (string.IsNullOrWhiteSpace(options.ModuleName) || options.Headers.Count == 0 ||
			string.IsNullOrWhiteSpace(outCpp) || string.IsNullOrWhiteSpace(outCs)) {
			Console.Error.WriteLine("usage: Shalltear.HeaderBinder --module <name> --header <path> [--namespace <ns>] [--include <dir>] [--define <def>] [--libclang-dir <dir>] --out-cpp <path> --out-cs <path>");
			return 2;
		}

		try {
			HeaderModel model = ClangScanner.Scan(options);
			EmitOutput output = Emitter.Emit(model);
			WriteIfChanged(outCpp, output.CppThunks);
			if (output.HasBindings) {
				WriteIfChanged(outCs, output.CSharpBindings);
			} else if (File.Exists(outCs)) {
				File.Delete(outCs);
			}
			return 0;
		} catch (Exception ex) {
			Console.Error.WriteLine("[Shalltear.HeaderBinder] error: " + ex.Message);
			return 1;
		}
	}

	static void AddSplit(List<string> target, string? value) {
		if (string.IsNullOrWhiteSpace(value)) {
			return;
		}
		foreach (string part in value.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)) {
			if (part.Length > 0 && !target.Contains(part)) {
				target.Add(part);
			}
		}
	}

	static void WriteIfChanged(string path, string content) {
		string full = Path.GetFullPath(path);
		string? dir = Path.GetDirectoryName(full);
		if (!string.IsNullOrEmpty(dir)) {
			Directory.CreateDirectory(dir);
		}
		if (File.Exists(full) && File.ReadAllText(full) == content) {
			return;
		}
		File.WriteAllText(full, content, new UTF8Encoding(false));
	}
}
