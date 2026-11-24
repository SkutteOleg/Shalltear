using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.IO;
using System.Linq;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;
using System.Text;
using System.Text.RegularExpressions;

namespace Shalltear.Refactor;

internal static class WasmM2NEmitter {
	sealed class CookieTypeProvider : ISignatureTypeProvider<char, object?> {
		public static readonly CookieTypeProvider Instance = new();

		public char GetPrimitiveType(PrimitiveTypeCode typeCode) => typeCode switch {
			PrimitiveTypeCode.Void => 'V',
			PrimitiveTypeCode.Int64 or PrimitiveTypeCode.UInt64 => 'L',
			PrimitiveTypeCode.Single => 'F',
			PrimitiveTypeCode.Double => 'D',
			_ => 'I',
		};

		public char GetTypeFromDefinition(MetadataReader reader, TypeDefinitionHandle handle, byte rawTypeKind) => 'I';
		public char GetTypeFromReference(MetadataReader reader, TypeReferenceHandle handle, byte rawTypeKind) => 'I';
		public char GetTypeFromSpecification(MetadataReader reader, object? genericContext, TypeSpecificationHandle handle, byte rawTypeKind) => 'I';
		public char GetSZArrayType(char elementType) => 'I';
		public char GetArrayType(char elementType, ArrayShape shape) => 'I';
		public char GetByReferenceType(char elementType) => 'I';
		public char GetPointerType(char elementType) => 'I';
		public char GetPinnedType(char elementType) => elementType;
		public char GetModifiedType(char modifier, char unmodifiedType, bool isRequired) => unmodifiedType;
		public char GetGenericInstantiation(char genericType, ImmutableArray<char> typeArguments) => 'I';
		public char GetGenericMethodParameter(object? genericContext, int index) => 'I';
		public char GetGenericTypeParameter(object? genericContext, int index) => 'I';

		public char GetFunctionPointerType(MethodSignature<char> signature) {
			RecordSignature(signature);
			return 'I';
		}

		[ThreadStatic]
		internal static HashSet<string>? CurrentSignatures;

		internal static void RecordSignature(MethodSignature<char> signature) {
			HashSet<string>? sink = CurrentSignatures;
			if (sink == null) {
				return;
			}
			// Mono wasm M2N signatures only apply to unmanaged/static function pointers
			if (signature.Header.IsInstance) {
				return;
			}
			StringBuilder cookie = new(1 + signature.ParameterTypes.Length);
			cookie.Append(signature.ReturnType);
			foreach (char p in signature.ParameterTypes) {
				cookie.Append(p == 'V' ? 'I' : p);
			}
			sink.Add(cookie.ToString());
		}
	}

	public static int Run(string dllDir, string? existingHeader, string outHeader, string? manifestPath) {
		if (!Directory.Exists(dllDir)) {
			Console.Error.WriteLine($"wasm-m2n: dll directory not found: '{dllDir}'");
			return 2;
		}

		HashSet<string> existing = new(StringComparer.Ordinal);
		if (!string.IsNullOrEmpty(existingHeader) && File.Exists(existingHeader)) {
			string text = File.ReadAllText(existingHeader);
			foreach (Match m in Regex.Matches(text, @"\{\s*""([VILFD]+)""\s*,")) {
				existing.Add(m.Groups[1].Value);
			}
		}

		HashSet<string> discovered = new(StringComparer.Ordinal);
		CookieTypeProvider.CurrentSignatures = discovered;
		List<string> dllNames = new();

		try {
			foreach (string dllPath in Directory.EnumerateFiles(dllDir, "*.dll", SearchOption.TopDirectoryOnly).OrderBy(static p => Path.GetFileName(p), StringComparer.OrdinalIgnoreCase)) {
				dllNames.Add(Path.GetFileName(dllPath));
				ScanAssembly(dllPath);
			}
		} finally {
			CookieTypeProvider.CurrentSignatures = null;
		}

		List<string> extras = discovered.Where(c => !existing.Contains(c)).OrderBy(static c => c, StringComparer.Ordinal).ToList();

		string? outDir = Path.GetDirectoryName(Path.GetFullPath(outHeader));
		if (!string.IsNullOrEmpty(outDir)) {
			Directory.CreateDirectory(outDir);
		}
		File.WriteAllText(outHeader, RenderHeader(extras), new UTF8Encoding(false));

		if (!string.IsNullOrEmpty(manifestPath)) {
			string? manifestDir = Path.GetDirectoryName(Path.GetFullPath(manifestPath));
			if (!string.IsNullOrEmpty(manifestDir)) {
				Directory.CreateDirectory(manifestDir);
			}
			StringBuilder json = new();
			json.Append("[\n");
			for (int i = 0; i < dllNames.Count; ++i) {
				json.Append('\t').Append('"').Append(dllNames[i].Replace("\\", "\\\\").Replace("\"", "\\\"")).Append('"');
				if (i + 1 < dllNames.Count) {
					json.Append(',');
				}
				json.Append('\n');
			}
			json.Append("]\n");
			File.WriteAllText(manifestPath, json.ToString(), new UTF8Encoding(false));
		}

		return 0;
	}

	static void ScanAssembly(string dllPath) {
		using FileStream stream = File.OpenRead(dllPath);
		using PEReader pe = new(stream);
		if (!pe.HasMetadata) {
			return;
		}
		MetadataReader reader = pe.GetMetadataReader();
		CookieTypeProvider provider = CookieTypeProvider.Instance;

		int standaloneCount = reader.GetTableRowCount(TableIndex.StandAloneSig);
		for (int row = 1; row <= standaloneCount; ++row) {
			StandaloneSignatureHandle handle = MetadataTokens.StandaloneSignatureHandle(row);
			StandaloneSignature sig = reader.GetStandaloneSignature(handle);
			if (sig.GetKind() != StandaloneSignatureKind.Method) {
				continue;
			}
			try {
				MethodSignature<char> methodSig = sig.DecodeMethodSignature(provider, null);
				if (methodSig.Header.CallingConvention != SignatureCallingConvention.Default) {
					CookieTypeProvider.RecordSignature(methodSig);
				}
			} catch {
			}
		}

		foreach (FieldDefinitionHandle handle in reader.FieldDefinitions) {
			try {
				reader.GetFieldDefinition(handle).DecodeSignature(provider, null);
			} catch {
			}
		}

		foreach (MethodDefinitionHandle handle in reader.MethodDefinitions) {
			try {
				reader.GetMethodDefinition(handle).DecodeSignature(provider, null);
			} catch {
			}
		}

		foreach (MemberReferenceHandle handle in reader.MemberReferences) {
			try {
				MemberReference mr = reader.GetMemberReference(handle);
				if (mr.GetKind() == MemberReferenceKind.Field) {
					mr.DecodeFieldSignature(provider, null);
				} else {
					mr.DecodeMethodSignature(provider, null);
				}
			} catch {
			}
		}
	}

	static string CTypeOf(char kind) => kind switch {
		'V' => "void",
		'L' => "int64_t",
		'F' => "float",
		'D' => "double",
		_ => "int",
	};

	static string RenderHeader(List<string> cookies) {
		StringBuilder sb = new();
		sb.Append("// GENERATED by Shalltear.Refactor --emit-wasm-m2n. Do not edit manually.\n");
		sb.Append("#pragma once\n\n");
		sb.Append("#include \"pinvoke.h\"\n");
		sb.Append("#include <stdint.h>\n");
		sb.Append("#include <stdlib.h>\n");
		sb.Append("#include <string.h>\n\n");

		foreach (string cookie in cookies) {
			char ret = cookie[0];
			string lower = cookie.ToLowerInvariant();
			sb.Append($"static void shalltear_wasm_invoke_{lower}(void *target_func, MonoInterpMethodArguments *margs) {{\n");
			sb.Append($"\ttypedef {CTypeOf(ret)} (*T)(");
			if (cookie.Length == 1) {
				sb.Append("void");
			} else {
				for (int i = 1; i < cookie.Length; ++i) {
					if (i > 1) {
						sb.Append(", ");
					}
					sb.Append($"{CTypeOf(cookie[i])} a{i - 1}");
				}
			}
			sb.Append(");\n");
			sb.Append("\tT func = (T)target_func;\n");
			sb.Append(ret == 'V' ? "\tfunc(" : $"\t{CTypeOf(ret)} res = func(");
			int ipos = 0;
			int fpos = 0;
			for (int i = 1; i < cookie.Length; ++i) {
				if (i > 1) {
					sb.Append(", ");
				}
				switch (cookie[i]) {
				case 'L':
					sb.Append($"mono_wasm_interp_method_args_get_larg(margs, {ipos})");
					ipos += 2;
					break;
				case 'F':
					sb.Append($"mono_wasm_interp_method_args_get_farg(margs, {fpos})");
					fpos += 1;
					break;
				case 'D':
					sb.Append($"mono_wasm_interp_method_args_get_darg(margs, {fpos})");
					fpos += 1;
					break;
				default:
					sb.Append($"mono_wasm_interp_method_args_get_iarg(margs, {ipos})");
					ipos += 1;
					break;
				}
			}
			sb.Append(");\n");
			if (ret != 'V') {
				sb.Append("\tvoid *retval = mono_wasm_interp_method_args_get_retval(margs);\n");
				sb.Append($"\t*({CTypeOf(ret)} *)retval = res;\n");
			}
			sb.Append("}\n\n");
		}

		sb.Append("static InterpToNative shalltear_extra_m2n_invokes[] = {\n");
		for (int i = 0; i < cookies.Count; ++i) {
			string cookie = cookies[i];
			sb.Append($"\t{{\"{cookie}\", shalltear_wasm_invoke_{cookie.ToLowerInvariant()}}}");
			if (i + 1 < cookies.Count) {
				sb.Append(',');
			}
			sb.Append('\n');
		}
		if (cookies.Count == 0) {
			sb.Append("\t{NULL, NULL}\n");
		}
		sb.Append("};\n\n");

		sb.Append("static int shalltear_extra_m2n_compare(const void *key, const void *elem) {\n");
		sb.Append("\treturn strcmp((const char *)key, ((const InterpToNative *)elem)->signature);\n");
		sb.Append("}\n\n");

		sb.Append("static void *shalltear_wasm_extra_m2n_callback(const char *cookie) {\n");
		if (cookies.Count == 0) {
			sb.Append("\t(void)cookie;\n\treturn NULL;\n");
		} else {
			sb.Append("\tInterpToNative *found = (InterpToNative *)bsearch(cookie, shalltear_extra_m2n_invokes, ");
			sb.Append($"{cookies.Count}, sizeof(InterpToNative), shalltear_extra_m2n_compare);\n");
			sb.Append("\treturn found ? found->func : NULL;\n");
		}
		sb.Append("}\n");
		return sb.ToString();
	}
}
