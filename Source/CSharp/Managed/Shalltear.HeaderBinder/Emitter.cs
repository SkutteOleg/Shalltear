using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;

namespace Shalltear.HeaderBinder;

internal sealed class EmitOutput {
	public string CppThunks = "";
	public string CSharpBindings = "";
	public int ThunkCount;
	public bool HasBindings;
}

internal static class Emitter {
	static readonly HashSet<string> _csKeywords = new(StringComparer.Ordinal) {
		"abstract", "as", "base", "bool", "break", "byte", "case", "catch", "char", "checked", "class", "const",
		"continue", "decimal", "default", "delegate", "do", "double", "else", "enum", "event", "explicit", "extern",
		"false", "finally", "fixed", "float", "for", "foreach", "goto", "if", "implicit", "in", "int", "interface",
		"internal", "is", "lock", "long", "namespace", "new", "null", "object", "operator", "out", "override", "params",
		"private", "protected", "public", "readonly", "ref", "return", "sbyte", "sealed", "short", "sizeof",
		"stackalloc", "static", "string", "struct", "switch", "this", "throw", "true", "try", "typeof", "uint", "ulong",
		"unchecked", "unsafe", "ushort", "using", "virtual", "void", "volatile", "while",
	};

	sealed class Context {
		public HeaderModel Model;
		public StringBuilder CppFuncs = new();
		public List<string> ThunkNames = new();

		public Context(HeaderModel model) => Model = model;

		public int AllocateSlot(out string fnName) {
			int slot = ThunkNames.Count;
			fnName = "slt_uthunk_" + slot.ToString(CultureInfo.InvariantCulture);
			ThunkNames.Add(fnName);
			return slot;
		}
	}

	public static EmitOutput Emit(HeaderModel model) {
		Context ctx = new(model);

		// Group top-level items by C++ namespace (mapped :: -> .)
		Dictionary<string, List<CppEnum>> enumsByNs = new(StringComparer.Ordinal);
		Dictionary<string, List<CppRecord>> recordsByNs = new(StringComparer.Ordinal);
		Dictionary<string, List<CppFunction>> funcsByNs = new(StringComparer.Ordinal);
		Dictionary<string, List<CppGlobalVar>> globalsByNs = new(StringComparer.Ordinal);
		SortedSet<string> allNamespaces = new(StringComparer.Ordinal);

		HashSet<string> nestedEnumQuals = new(StringComparer.Ordinal);
		HashSet<string> nestedRecQuals = new(StringComparer.Ordinal);
		foreach (CppRecord rec in model.Records.Values) {
			foreach (CppRecord nr in rec.NestedRecords) {
				nestedRecQuals.Add(nr.QualifiedName);
			}
			foreach (CppEnum ne in rec.NestedEnums) {
				nestedEnumQuals.Add(ne.QualifiedName);
			}
		}

		foreach (CppEnum en in model.Enums.Values.OrderBy(static e => e.QualifiedName, StringComparer.Ordinal)) {
			if (nestedEnumQuals.Contains(en.QualifiedName)) {
				continue;
			}
			string ns = MapNamespace(en.Namespace, model.ModuleName);
			allNamespaces.Add(ns);
			GetOrAdd(enumsByNs, ns).Add(en);
		}
		foreach (CppRecord rec in model.Records.Values.OrderBy(static r => r.QualifiedName, StringComparer.Ordinal)) {
			if (nestedRecQuals.Contains(rec.QualifiedName)) {
				continue;
			}
			string ns = MapNamespace(rec.Namespace, model.ModuleName);
			allNamespaces.Add(ns);
			GetOrAdd(recordsByNs, ns).Add(rec);
		}
		foreach (CppFunction fn in model.FreeFunctions) {
			string ns = MapNamespace(fn.Namespace, model.ModuleName);
			allNamespaces.Add(ns);
			GetOrAdd(funcsByNs, ns).Add(fn);
		}
		foreach (CppGlobalVar gv in model.Globals) {
			string ns = MapNamespace(gv.Namespace, model.ModuleName);
			allNamespaces.Add(ns);
			GetOrAdd(globalsByNs, ns).Add(gv);
		}

		StringBuilder csBody = new();
		foreach (string ns in allNamespaces) {
			csBody.Append("namespace ").Append(ns).Append(" {\n\n");

			if (enumsByNs.TryGetValue(ns, out List<CppEnum>? enums)) {
				foreach (CppEnum en in enums) {
					EmitEnum(csBody, en, "\t");
				}
			}

			if (recordsByNs.TryGetValue(ns, out List<CppRecord>? records)) {
				foreach (CppRecord rec in records) {
					if (rec.IsValueStruct) {
						EmitValueStruct(ctx, csBody, rec, "\t");
					} else {
						EmitOpaqueClass(ctx, csBody, rec, "\t");
					}
				}
			}

			bool hasFuncs = funcsByNs.TryGetValue(ns, out List<CppFunction>? funcs) && funcs.Count > 0;
			bool hasGlobals = globalsByNs.TryGetValue(ns, out List<CppGlobalVar>? globals) && globals.Count > 0;
			if (hasFuncs || hasGlobals) {
				StringBuilder nativeClass = new();
				HashSet<string> emittedSigs = new(StringComparer.Ordinal);
				HashSet<string> emittedMembers = new(StringComparer.Ordinal);

				if (hasGlobals) {
					foreach (CppGlobalVar gv in globals!) {
						EmitGlobalGetter(ctx, nativeClass, gv, emittedMembers);
					}
				}
				if (hasFuncs) {
					foreach (CppFunction fn in funcs!) {
						EmitCallableOverloads(ctx, nativeClass, null, fn, "\t\t", emittedSigs);
					}
				}
				if (nativeClass.Length > 0) {
					csBody.Append("\tpublic static unsafe partial class Native {\n");
					csBody.Append(nativeClass);
					csBody.Append("\t}\n\n");
				}
			}

			csBody.Append("}\n\n");
		}

		bool hasBindings = csBody.Length > 0 || ctx.ThunkNames.Count > 0;
		if (!hasBindings) {
			return new EmitOutput {
				CppThunks = "// GENERATED by Shalltear.HeaderBinder. No non-reflected C++ bindings found.\n",
				CSharpBindings = "",
				ThunkCount = 0,
				HasBindings = false,
			};
		}

		// Assemble C# file
		StringBuilder cs = new();
		cs.Append("// <auto-generated/>\n");
		cs.Append("// GENERATED by Shalltear.HeaderBinder. Do not edit manually.\n");
		cs.Append("#nullable enable\n");
		cs.Append("#pragma warning disable CS0169, CS0649, CS1591\n\n");
		cs.Append("using System;\n");
		cs.Append("using System.Runtime.CompilerServices;\n");
		cs.Append("using System.Runtime.InteropServices;\n");
		cs.Append("using System.Text;\n");
		cs.Append("using System.Threading;\n");
		cs.Append("using Shalltear.Interop;\n\n");
		cs.Append(csBody);

		cs.Append("namespace Shalltear.Generated {\n");
		cs.Append("\tinternal static unsafe class __UserThunkCache_").Append(SanitizeIdent(model.ModuleName)).Append(" {\n");
		cs.Append("\t\tconst string MODULE_NAME = \"").Append(EscapeString(model.ModuleName)).Append("\";\n");
		cs.Append("\t\tstatic ulong _epoch;\n");
		cs.Append("\t\tstatic nint _table;\n");
		cs.Append("\t\tstatic uint _count;\n\n");
		cs.Append("\t\tinternal static ulong ModuleEpoch => ModuleThunks.GetModuleEpoch(MODULE_NAME);\n\n");
		cs.Append("\t\t[MethodImpl(MethodImplOptions.AggressiveInlining)]\n");
		cs.Append("\t\tinternal static nint Slot(int index) {\n");
		cs.Append("\t\t\treturn ModuleThunks.ResolveSlot(MODULE_NAME, index, ref _epoch, ref _table, ref _count);\n");
		cs.Append("\t\t}\n\n");
		cs.Append("\t\tinternal static void DeleteOrEnqueue(int slotIndex, nint ptr, ulong createdEpoch) {\n");
		cs.Append("\t\t\tModuleThunks.DeleteOrEnqueue(MODULE_NAME, slotIndex, ptr, createdEpoch, ref _epoch, ref _table, ref _count);\n");
		cs.Append("\t\t}\n\n");
		cs.Append("\t\tinternal static void EnqueueDelete(int slotIndex, nint ptr, ulong createdEpoch) {\n");
		cs.Append("\t\t\tModuleThunks.EnqueueDelete(MODULE_NAME, slotIndex, ptr, createdEpoch);\n");
		cs.Append("\t\t}\n");
		cs.Append("\t}\n");
		cs.Append("}\n");

		// Assemble C++ file
		StringBuilder cpp = new();
		cpp.Append("// GENERATED by Shalltear.HeaderBinder. Do not edit manually.\n");
		cpp.Append("#include \"Shalltear/Core/Module/ModuleThunkRegistry.h\"\n\n");
		cpp.Append("#include <cstdint>\n");
		cpp.Append("#include <cstring>\n");
		cpp.Append("#include <filesystem>\n");
		cpp.Append("#include <functional>\n");
		cpp.Append("#include <memory>\n");
		cpp.Append("#include <span>\n");
		cpp.Append("#include <string>\n");
		cpp.Append("#include <string_view>\n");
		cpp.Append("#include <vector>\n\n");
		cpp.Append("#if defined(__clang__)\n");
		cpp.Append("#pragma clang diagnostic push\n");
		cpp.Append("#pragma clang diagnostic ignored \"-Wdeprecated-declarations\"\n");
		cpp.Append("#pragma clang diagnostic ignored \"-Wunused-function\"\n");
		cpp.Append("#endif\n\n");
		foreach (string header in model.BoundHeaders) {
			cpp.Append("#include \"").Append(EscapeString(header)).Append("\"\n");
		}
		cpp.Append("\nnamespace {\n\n");
		cpp.Append("\tthread_local std::string _sltReturnStringScratch;\n\n");
		cpp.Append(ctx.CppFuncs);
		cpp.Append("\tstatic const void* const _sltUserThunkTable[] = {\n");
		if (ctx.ThunkNames.Count == 0) {
			cpp.Append("\t\tnullptr,\n");
		} else {
			foreach (string name in ctx.ThunkNames) {
				cpp.Append("\t\treinterpret_cast<const void*>(&").Append(name).Append("),\n");
			}
		}
		cpp.Append("\t};\n\n");
		cpp.Append("} // anonymous namespace\n\n");
		cpp.Append("SLT_REGISTER_MODULE_THUNKS(\"").Append(EscapeString(model.ModuleName)).Append("\", _sltUserThunkTable)\n\n");
		cpp.Append("#if defined(__clang__)\n");
		cpp.Append("#pragma clang diagnostic pop\n");
		cpp.Append("#endif\n");

		return new EmitOutput {
			CppThunks = cpp.ToString(),
			CSharpBindings = cs.ToString(),
			ThunkCount = ctx.ThunkNames.Count,
			HasBindings = true,
		};
	}

	static List<T> GetOrAdd<T>(Dictionary<string, List<T>> dict, string key) {
		if (!dict.TryGetValue(key, out List<T>? list)) {
			list = new List<T>();
			dict[key] = list;
		}
		return list;
	}

	static string MapNamespace(string cppNs, string moduleName) {
		if (string.IsNullOrEmpty(cppNs)) {
			return SanitizeIdent(moduleName) + ".NativeBindings";
		}
		return cppNs.Replace("::", ".", StringComparison.Ordinal);
	}

	static string EscapeCsIdent(string name) {
		return _csKeywords.Contains(name) ? "@" + name : name;
	}

	static string SanitizeIdent(string name) {
		StringBuilder sb = new(name.Length);
		foreach (char c in name) {
			sb.Append(char.IsLetterOrDigit(c) || c == '_' ? c : '_');
		}
		if (sb.Length == 0 || char.IsDigit(sb[0])) {
			sb.Insert(0, '_');
		}
		return sb.ToString();
	}

	static string EscapeString(string s) => s.Replace("\\", "\\\\", StringComparison.Ordinal).Replace("\"", "\\\"", StringComparison.Ordinal);

	static string CacheType(Context ctx) => "global::Shalltear.Generated.__UserThunkCache_" + SanitizeIdent(ctx.Model.ModuleName);

	static string CsQualifiedType(Context ctx, string cppQualified) {
		if (ctx.Model.Enums.TryGetValue(cppQualified, out CppEnum? en)) {
			string ns = MapNamespace(en.Namespace, ctx.Model.ModuleName);
			string rel = string.IsNullOrEmpty(en.Namespace)
				? cppQualified
				: cppQualified.Substring(en.Namespace.Length + 2);
			return "global::" + ns + "." + rel.Replace("::", ".", StringComparison.Ordinal);
		}
		if (ctx.Model.Records.TryGetValue(cppQualified, out CppRecord? rec)) {
			string ns = MapNamespace(rec.Namespace, ctx.Model.ModuleName);
			string rel = string.IsNullOrEmpty(rec.Namespace)
				? cppQualified
				: cppQualified.Substring(rec.Namespace.Length + 2);
			return "global::" + ns + "." + rel.Replace("::", ".", StringComparison.Ordinal);
		}
		return "nint";
	}

	static void EmitEnum(StringBuilder sb, CppEnum en, string indent) {
		if (en.IsAnonymous) {
			foreach ((string cName, long sVal, ulong uVal) in en.Constants) {
				string valStr = en.IsUnsigned
					? uVal.ToString(CultureInfo.InvariantCulture)
					: sVal.ToString(CultureInfo.InvariantCulture);
				sb.Append(indent).Append("public const ").Append(en.UnderlyingCSharp).Append(' ')
				  .Append(EscapeCsIdent(cName)).Append(" = ").Append(valStr).Append(";\n");
			}
			sb.Append('\n');
			return;
		}
		sb.Append(indent).Append("public enum ").Append(EscapeCsIdent(en.Name)).Append(" : ").Append(en.UnderlyingCSharp).Append(" {\n");
		foreach ((string cName, long sVal, ulong uVal) in en.Constants) {
			string valStr = en.IsUnsigned
				? uVal.ToString(CultureInfo.InvariantCulture)
				: sVal.ToString(CultureInfo.InvariantCulture);
			sb.Append(indent).Append('\t').Append(EscapeCsIdent(cName)).Append(" = ").Append(valStr).Append(",\n");
		}
		sb.Append(indent).Append("}\n\n");
	}

	static void EmitValueStruct(Context ctx, StringBuilder sb, CppRecord rec, string indent) {
		sb.Append(indent).Append("[StructLayout(LayoutKind.Explicit, Size = ").Append(rec.SizeBytes.ToString(CultureInfo.InvariantCulture)).Append(")]\n");
		sb.Append(indent).Append("public unsafe partial struct ").Append(EscapeCsIdent(rec.Name)).Append(" {\n");

		foreach (CppEnum ne in rec.NestedEnums) {
			EmitEnum(sb, ne, indent + "\t");
		}
		foreach (CppRecord nr in rec.NestedRecords) {
			if (nr.IsValueStruct) {
				EmitValueStruct(ctx, sb, nr, indent + "\t");
			}
		}

		// Emit anonymous nested structs first
		int anonIdx = 0;
		Dictionary<CppField, string> anonTypeNames = new();
		foreach (CppField field in rec.FlattenedFields) {
			if (field.Type.Kind == CppTypeKind.AnonymousStruct && field.Type.AnonymousRecord != null) {
				string nestedName = char.ToUpperInvariant(field.Name[0]) + field.Name.Substring(1) + "Data";
				if (nestedName == rec.Name) {
					nestedName += "_" + (++anonIdx).ToString(CultureInfo.InvariantCulture);
				}
				anonTypeNames[field] = nestedName;
				EmitAnonymousStruct(ctx, sb, field.Type.AnonymousRecord, nestedName, indent + "\t");
			}
		}

		HashSet<string> emittedFields = new(StringComparer.Ordinal);
		foreach (CppField field in rec.FlattenedFields) {
			if (field.OffsetBytes < 0 || !emittedFields.Add(field.Name)) {
				continue;
			}
			string fIdent = EscapeCsIdent(field.Name);
			if (field.Type.Kind == CppTypeKind.FixedArray && field.Type.Element != null) {
				string elemCs = MapScalarCs(field.Type.Element.Kind);
				sb.Append(indent).Append("\t[FieldOffset(").Append(field.OffsetBytes.ToString(CultureInfo.InvariantCulture)).Append(")]\n");
				sb.Append(indent).Append("\tpublic fixed ").Append(elemCs).Append(' ').Append(fIdent).Append('[')
				  .Append(field.Type.ArrayLength.ToString(CultureInfo.InvariantCulture)).Append("];\n");
			} else if (field.Type.Kind == CppTypeKind.AnonymousStruct && anonTypeNames.TryGetValue(field, out string? anonName)) {
				sb.Append(indent).Append("\t[FieldOffset(").Append(field.OffsetBytes.ToString(CultureInfo.InvariantCulture)).Append(")]\n");
				sb.Append(indent).Append("\tpublic ").Append(anonName).Append(' ').Append(fIdent).Append(";\n");
			} else if (field.Type.Kind == CppTypeKind.Pointer || field.Type.Kind == CppTypeKind.VoidPtr) {
				sb.Append(indent).Append("\t[FieldOffset(").Append(field.OffsetBytes.ToString(CultureInfo.InvariantCulture)).Append(")]\n");
				sb.Append(indent).Append("\tpublic nint ").Append(fIdent).Append(";\n");
			} else {
				string? csType = MapStructFieldCs(ctx, field.Type);
				if (csType != null) {
					sb.Append(indent).Append("\t[FieldOffset(").Append(field.OffsetBytes.ToString(CultureInfo.InvariantCulture)).Append(")]\n");
					sb.Append(indent).Append("\tpublic ").Append(csType).Append(' ').Append(fIdent).Append(";\n");
				}
			}
		}

		// Emit CreateDefault() if public default ctor exists
		if (rec.HasPublicDefaultCtor && !rec.IsAnonymous) {
			int slot = ctx.AllocateSlot(out string fnName);
			ctx.CppFuncs.Append("\textern \"C\" void ").Append(fnName).Append("(void* outResult) {\n");
			ctx.CppFuncs.Append("\t\t::").Append(rec.QualifiedName).Append(" value{};\n");
			ctx.CppFuncs.Append("\t\tstd::memcpy(outResult, &value, sizeof(value));\n");
			ctx.CppFuncs.Append("\t}\n\n");

			sb.Append('\n').Append(indent).Append("\tpublic static ").Append(EscapeCsIdent(rec.Name)).Append(" CreateDefault() {\n");
			sb.Append(indent).Append("\t\t").Append(EscapeCsIdent(rec.Name)).Append(" result = default;\n");
			sb.Append(indent).Append("\t\t((delegate* unmanaged[Cdecl]<void*, void>)").Append(CacheType(ctx)).Append(".Slot(")
			  .Append(slot.ToString(CultureInfo.InvariantCulture)).Append("))(&result);\n");
			sb.Append(indent).Append("\t\treturn result;\n");
			sb.Append(indent).Append("\t}\n");
		}

		HashSet<string> emittedSigs = new(StringComparer.Ordinal);
		foreach (CppFunction method in rec.Methods) {
			if (emittedFields.Contains(method.Name) || method.Name == rec.Name || method.Name == "CreateDefault") {
				continue;
			}
			EmitCallableOverloads(ctx, sb, rec, method, indent + "\t", emittedSigs);
		}

		sb.Append(indent).Append("}\n\n");
	}

	static void EmitAnonymousStruct(Context ctx, StringBuilder sb, CppRecord anon, string typeName, string indent) {
		sb.Append(indent).Append("[StructLayout(LayoutKind.Explicit, Size = ").Append(anon.SizeBytes.ToString(CultureInfo.InvariantCulture)).Append(")]\n");
		sb.Append(indent).Append("public unsafe partial struct ").Append(typeName).Append(" {\n");
		foreach (CppField f in anon.FlattenedFields) {
			if (f.OffsetBytes < 0) {
				continue;
			}
			string fIdent = EscapeCsIdent(f.Name);
			if (f.Type.Kind == CppTypeKind.FixedArray && f.Type.Element != null) {
				sb.Append(indent).Append("\t[FieldOffset(").Append(f.OffsetBytes.ToString(CultureInfo.InvariantCulture)).Append(")]\n");
				sb.Append(indent).Append("\tpublic fixed ").Append(MapScalarCs(f.Type.Element.Kind)).Append(' ').Append(fIdent)
				  .Append('[').Append(f.Type.ArrayLength.ToString(CultureInfo.InvariantCulture)).Append("];\n");
			} else if (f.Type.Kind is CppTypeKind.Pointer or CppTypeKind.VoidPtr) {
				sb.Append(indent).Append("\t[FieldOffset(").Append(f.OffsetBytes.ToString(CultureInfo.InvariantCulture)).Append(")]\n");
				sb.Append(indent).Append("\tpublic nint ").Append(fIdent).Append(";\n");
			} else {
				string? csType = MapStructFieldCs(ctx, f.Type);
				if (csType != null) {
					sb.Append(indent).Append("\t[FieldOffset(").Append(f.OffsetBytes.ToString(CultureInfo.InvariantCulture)).Append(")]\n");
					sb.Append(indent).Append("\tpublic ").Append(csType).Append(' ').Append(fIdent).Append(";\n");
				}
			}
		}
		sb.Append(indent).Append("}\n\n");
	}

	static void EmitOpaqueClass(Context ctx, StringBuilder sb, CppRecord rec, string indent) {
		string csName = EscapeCsIdent(rec.Name);
		sb.Append(indent).Append("public sealed unsafe partial class ").Append(csName).Append(" : IDisposable {\n");
		foreach (CppEnum ne in rec.NestedEnums) {
			EmitEnum(sb, ne, indent + "\t");
		}
		foreach (CppRecord nr in rec.NestedRecords) {
			if (nr.IsValueStruct) {
				EmitValueStruct(ctx, sb, nr, indent + "\t");
			}
		}

		sb.Append(indent).Append("\tinternal nint _ptr;\n");
		sb.Append(indent).Append("\treadonly bool _owns;\n");
		sb.Append(indent).Append("\treadonly ulong _epoch;\n\n");
		sb.Append(indent).Append("\tinternal ").Append(csName).Append("(nint ptr, bool owns) {\n");
		sb.Append(indent).Append("\t\t_ptr = ptr;\n");
		sb.Append(indent).Append("\t\t_owns = owns;\n");
		sb.Append(indent).Append("\t\t_epoch = owns && ptr != 0 ? ").Append(CacheType(ctx)).Append(".ModuleEpoch : 0UL;\n");
		sb.Append(indent).Append("\t}\n\n");
		sb.Append(indent).Append("\tpublic nint NativePointer => _ptr;\n");
		sb.Append(indent).Append("\tpublic bool IsValid => _ptr != 0;\n\n");
		sb.Append(indent).Append("\tpublic static ").Append(csName).Append("? FromPointer(nint ptr, bool owns = false) => ptr == 0 ? null : new ")
		  .Append(csName).Append("(ptr, owns);\n\n");

		// Destructor thunk (standard C++ delete for non-Shalltear classes)
		if (rec.HasPublicDestructor) {
			int dtorSlot = ctx.AllocateSlot(out string dtorFn);
			ctx.CppFuncs.Append("\textern \"C\" void ").Append(dtorFn).Append("(void* self) {\n");
			ctx.CppFuncs.Append("\t\tdelete static_cast<::").Append(rec.QualifiedName).Append("*>(self);\n");
			ctx.CppFuncs.Append("\t}\n\n");

			sb.Append(indent).Append("\tpublic void Dispose() {\n");
			sb.Append(indent).Append("\t\tnint p = Interlocked.Exchange(ref _ptr, 0);\n");
			sb.Append(indent).Append("\t\tif (_owns && p != 0) {\n");
			sb.Append(indent).Append("\t\t\t").Append(CacheType(ctx)).Append(".DeleteOrEnqueue(")
			  .Append(dtorSlot.ToString(CultureInfo.InvariantCulture)).Append(", p, _epoch);\n");
			sb.Append(indent).Append("\t\t}\n");
			sb.Append(indent).Append("\t\tGC.SuppressFinalize(this);\n");
			sb.Append(indent).Append("\t}\n\n");
			sb.Append(indent).Append("\t~").Append(csName).Append("() {\n");
			sb.Append(indent).Append("\t\tnint p = Interlocked.Exchange(ref _ptr, 0);\n");
			sb.Append(indent).Append("\t\tif (_owns && p != 0) {\n");
			sb.Append(indent).Append("\t\t\t").Append(CacheType(ctx)).Append(".EnqueueDelete(")
			  .Append(dtorSlot.ToString(CultureInfo.InvariantCulture)).Append(", p, _epoch);\n");
			sb.Append(indent).Append("\t\t}\n");
			sb.Append(indent).Append("\t}\n\n");
		} else {
			sb.Append(indent).Append("\tpublic void Dispose() {\n");
			sb.Append(indent).Append("\t\tInterlocked.Exchange(ref _ptr, 0);\n");
			sb.Append(indent).Append("\t}\n\n");
		}

		// Public constructors for concrete opaque classes
		if (!rec.IsAbstract) {
			HashSet<string> ctorSigs = new(StringComparer.Ordinal);
			if (rec.HasPublicDefaultCtor && rec.Constructors.Count == 0) {
				EmitOpaqueConstructor(ctx, sb, rec, Array.Empty<CppParam>(), indent + "\t", ctorSigs);
			} else {
				foreach (CppFunction ctor in rec.Constructors) {
					int minArity = ctor.Params.Count;
					for (int i = 0; i < ctor.Params.Count; ++i) {
						if (ctor.Params[i].HasDefault) {
							minArity = i;
							break;
						}
					}
					for (int arity = minArity; arity <= ctor.Params.Count; ++arity) {
						EmitOpaqueConstructor(ctx, sb, rec, ctor.Params.Take(arity).ToArray(), indent + "\t", ctorSigs);
					}
				}
			}
		}

		HashSet<string> usedMemberNames = new(StringComparer.Ordinal) {
			"NativePointer", "IsValid", "FromPointer", "Dispose", rec.Name,
		};

		// Public fields -> getter/setter properties
		foreach (CppField field in rec.FlattenedFields) {
			if (!field.IsPublic || field.Name.StartsWith("_", StringComparison.Ordinal)) {
				continue;
			}
			if (usedMemberNames.Contains(field.Name)) {
				continue;
			}
			EmitOpaqueFieldProperty(ctx, sb, rec, field, indent + "\t", usedMemberNames);
		}

		// Methods
		HashSet<string> emittedSigs = new(StringComparer.Ordinal);
		foreach (CppFunction method in rec.Methods) {
			if (usedMemberNames.Contains(method.Name)) {
				continue;
			}
			EmitCallableOverloads(ctx, sb, rec, method, indent + "\t", emittedSigs);
		}

		sb.Append(indent).Append("}\n\n");
	}

	static void EmitOpaqueConstructor(Context ctx, StringBuilder sb, CppRecord rec, CppParam[] parameters, string indent, HashSet<string> seenSigs) {
		List<ParamMarshal> marshaled = new();
		foreach (CppParam p in parameters) {
			ParamMarshal? m = BuildParamMarshal(ctx, p, marshaled.Count);
			if (m == null) {
				return;
			}
			marshaled.Add(m);
		}
		string sigKey = "ctor(" + string.Join(",", marshaled.Select(static m => m.CsSignatureType)) + ")";
		if (!seenSigs.Add(sigKey)) {
			return;
		}

		int slot = ctx.AllocateSlot(out string fnName);
		List<string> cppSigParams = new();
		StringBuilder cppPrologue = new();
		StringBuilder cppEpilogue = new();
		List<string> cppCallArgs = new();
		foreach (ParamMarshal m in marshaled) {
			cppSigParams.AddRange(m.CppParams);
			cppPrologue.Append(m.CppPrologue);
			cppEpilogue.Append(m.CppEpilogue);
			cppCallArgs.Add(m.CppCallExpr);
		}

		ctx.CppFuncs.Append("\textern \"C\" void* ").Append(fnName).Append('(').Append(string.Join(", ", cppSigParams)).Append(") {\n");
		ctx.CppFuncs.Append(cppPrologue);
		ctx.CppFuncs.Append("\t\tvoid* res = static_cast<void*>(new ::").Append(rec.QualifiedName).Append('(').Append(string.Join(", ", cppCallArgs)).Append("));\n");
		ctx.CppFuncs.Append(cppEpilogue);
		ctx.CppFuncs.Append("\t\treturn res;\n");
		ctx.CppFuncs.Append("\t}\n\n");

		sb.Append(indent).Append("public ").Append(EscapeCsIdent(rec.Name)).Append('(')
		  .Append(string.Join(", ", marshaled.Select(static m => m.CsParamDecl))).Append(") {\n");
		EmitCsCallBody(ctx, sb, indent + "\t", slot, marshaled, null, false, false, false, null, ctorAssign: true);
		sb.Append(indent).Append("}\n\n");
	}

	static void EmitOpaqueFieldProperty(Context ctx, StringBuilder sb, CppRecord rec, CppField field, string indent, HashSet<string> usedNames) {
		CppTypeRef t = field.Type;
		string propName = EscapeCsIdent(field.Name);

		// Pointer to opaque class field (e.g. Ball* ball;)
		if (t.Kind == CppTypeKind.Pointer && t.Element != null && t.Element.Kind == CppTypeKind.Record &&
			ctx.Model.Records.TryGetValue(t.Element.QualifiedName, out CppRecord? targetRec) && !targetRec.IsValueStruct) {
			string targetCs = CsQualifiedType(ctx, targetRec.QualifiedName);
			int getSlot = ctx.AllocateSlot(out string getFn);
			ctx.CppFuncs.Append("\textern \"C\" void* ").Append(getFn).Append("(void* self) {\n");
			ctx.CppFuncs.Append("\t\treturn static_cast<void*>(static_cast<::").Append(rec.QualifiedName).Append("*>(self)->").Append(field.Name).Append(");\n");
			ctx.CppFuncs.Append("\t}\n\n");

			usedNames.Add(field.Name);
			sb.Append(indent).Append("public ").Append(targetCs).Append("? ").Append(propName).Append(" {\n");
			sb.Append(indent).Append("\tget {\n");
			sb.Append(indent).Append("\t\tif (_ptr == 0) { throw new ObjectDisposedException(\"").Append(rec.Name).Append("\"); }\n");
			sb.Append(indent).Append("\t\tnint p = ((delegate* unmanaged[Cdecl]<void*, nint>)").Append(CacheType(ctx)).Append(".Slot(")
			  .Append(getSlot.ToString(CultureInfo.InvariantCulture)).Append("))((void*)_ptr);\n");
			sb.Append(indent).Append("\t\treturn ").Append(targetCs).Append(".FromPointer(p, false);\n");
			sb.Append(indent).Append("\t}\n");
			sb.Append(indent).Append("}\n\n");
			return;
		}

		// Scalar or enum field
		if (t.IsScalar || (t.Kind == CppTypeKind.Enum && ctx.Model.Enums.ContainsKey(t.QualifiedName))) {
			string csType = t.IsScalar ? MapScalarCs(t.Kind) : CsQualifiedType(ctx, t.QualifiedName);
			string abiCs = t.IsScalar ? MapScalarAbiCs(t.Kind) : ctx.Model.Enums[t.QualifiedName].UnderlyingCSharp;
			string abiCpp = t.IsScalar ? MapScalarAbiCpp(t.Kind) : ctx.Model.Enums[t.QualifiedName].UnderlyingCpp;

			int getSlot = ctx.AllocateSlot(out string getFn);
			ctx.CppFuncs.Append("\textern \"C\" ").Append(abiCpp).Append(' ').Append(getFn).Append("(void* self) {\n");
			ctx.CppFuncs.Append("\t\treturn static_cast<").Append(abiCpp).Append(">(static_cast<::").Append(rec.QualifiedName).Append("*>(self)->").Append(field.Name).Append(");\n");
			ctx.CppFuncs.Append("\t}\n\n");

			int setSlot = -1;
			if (!t.IsConst) {
				setSlot = ctx.AllocateSlot(out string setFn);
				ctx.CppFuncs.Append("\textern \"C\" void ").Append(setFn).Append("(void* self, ").Append(abiCpp).Append(" value) {\n");
				string castExpr = t.Kind == CppTypeKind.Bool
					? "value != 0"
					: (t.Kind == CppTypeKind.Enum ? $"static_cast<::{t.QualifiedName}>(value)" : "value");
				ctx.CppFuncs.Append("\t\tstatic_cast<::").Append(rec.QualifiedName).Append("*>(self)->").Append(field.Name).Append(" = ").Append(castExpr).Append(";\n");
				ctx.CppFuncs.Append("\t}\n\n");
			}

			usedNames.Add(field.Name);
			sb.Append(indent).Append("public ").Append(csType).Append(' ').Append(propName).Append(" {\n");
			sb.Append(indent).Append("\tget {\n");
			sb.Append(indent).Append("\t\tif (_ptr == 0) { throw new ObjectDisposedException(\"").Append(rec.Name).Append("\"); }\n");
			sb.Append(indent).Append("\t\t").Append(abiCs).Append(" raw = ((delegate* unmanaged[Cdecl]<void*, ").Append(abiCs).Append(">)")
			  .Append(CacheType(ctx)).Append(".Slot(").Append(getSlot.ToString(CultureInfo.InvariantCulture)).Append("))((void*)_ptr);\n");
			string fromRaw = t.Kind == CppTypeKind.Bool ? "raw != 0" : (t.Kind == CppTypeKind.Enum ? $"({csType})raw" : "raw");
			sb.Append(indent).Append("\t\treturn ").Append(fromRaw).Append(";\n");
			sb.Append(indent).Append("\t}\n");
			if (setSlot >= 0) {
				string toRaw = t.Kind == CppTypeKind.Bool ? "(byte)(value ? 1 : 0)" : (t.Kind == CppTypeKind.Enum ? $"({abiCs})value" : "value");
				sb.Append(indent).Append("\tset {\n");
				sb.Append(indent).Append("\t\tif (_ptr == 0) { throw new ObjectDisposedException(\"").Append(rec.Name).Append("\"); }\n");
				sb.Append(indent).Append("\t\t((delegate* unmanaged[Cdecl]<void*, ").Append(abiCs).Append(", void>)").Append(CacheType(ctx)).Append(".Slot(")
				  .Append(setSlot.ToString(CultureInfo.InvariantCulture)).Append("))((void*)_ptr, ").Append(toRaw).Append(");\n");
				sb.Append(indent).Append("\t}\n");
			}
			sb.Append(indent).Append("}\n\n");
			return;
		}

		// Value struct or math field (e.g. CarControls controls; CarConfig config; Vec minPos;)
		if (t.IsMath || (t.Kind == CppTypeKind.Record && ctx.Model.Records.TryGetValue(t.QualifiedName, out CppRecord? valRec) && valRec.IsValueStruct)) {
			string csType = t.IsMath ? MapMathCs(t.Kind) : CsQualifiedType(ctx, t.QualifiedName);
			string cppType = "::" + t.QualifiedName;

			int getSlot = ctx.AllocateSlot(out string getFn);
			ctx.CppFuncs.Append("\textern \"C\" void ").Append(getFn).Append("(void* self, void* outValue) {\n");
			ctx.CppFuncs.Append("\t\tconst auto& v = static_cast<::").Append(rec.QualifiedName).Append("*>(self)->").Append(field.Name).Append(";\n");
			ctx.CppFuncs.Append("\t\tstd::memcpy(outValue, &v, sizeof(v));\n");
			ctx.CppFuncs.Append("\t}\n\n");

			int setSlot = -1;
			if (!t.IsConst) {
				setSlot = ctx.AllocateSlot(out string setFn);
				ctx.CppFuncs.Append("\textern \"C\" void ").Append(setFn).Append("(void* self, const void* inValue) {\n");
				ctx.CppFuncs.Append("\t\t").Append(cppType).Append(" tmp;\n");
				ctx.CppFuncs.Append("\t\tstd::memcpy(&tmp, inValue, sizeof(tmp));\n");
				ctx.CppFuncs.Append("\t\tstatic_cast<::").Append(rec.QualifiedName).Append("*>(self)->").Append(field.Name).Append(" = tmp;\n");
				ctx.CppFuncs.Append("\t}\n\n");
			}

			usedNames.Add(field.Name);
			sb.Append(indent).Append("public ").Append(csType).Append(' ').Append(propName).Append(" {\n");
			sb.Append(indent).Append("\tget {\n");
			sb.Append(indent).Append("\t\tif (_ptr == 0) { throw new ObjectDisposedException(\"").Append(rec.Name).Append("\"); }\n");
			sb.Append(indent).Append("\t\t").Append(csType).Append(" res = default;\n");
			sb.Append(indent).Append("\t\t((delegate* unmanaged[Cdecl]<void*, void*, void>)").Append(CacheType(ctx)).Append(".Slot(")
			  .Append(getSlot.ToString(CultureInfo.InvariantCulture)).Append("))((void*)_ptr, &res);\n");
			sb.Append(indent).Append("\t\treturn res;\n");
			sb.Append(indent).Append("\t}\n");
			if (setSlot >= 0) {
				sb.Append(indent).Append("\tset {\n");
				sb.Append(indent).Append("\t\tif (_ptr == 0) { throw new ObjectDisposedException(\"").Append(rec.Name).Append("\"); }\n");
				sb.Append(indent).Append("\t\t").Append(csType).Append(" copy = value;\n");
				sb.Append(indent).Append("\t\t((delegate* unmanaged[Cdecl]<void*, void*, void>)").Append(CacheType(ctx)).Append(".Slot(")
				  .Append(setSlot.ToString(CultureInfo.InvariantCulture)).Append("))((void*)_ptr, &copy);\n");
				sb.Append(indent).Append("\t}\n");
			}
			sb.Append(indent).Append("}\n\n");
		}
	}

	static void EmitGlobalGetter(Context ctx, StringBuilder sb, CppGlobalVar gv, HashSet<string> emittedMembers) {
		if (!emittedMembers.Add(gv.Name)) {
			return;
		}
		CppTypeRef t = gv.Type;
		if (t.IsMath || (t.Kind == CppTypeKind.Record && ctx.Model.Records.TryGetValue(t.QualifiedName, out CppRecord? valRec) && valRec.IsValueStruct)) {
			string csType = t.IsMath ? MapMathCs(t.Kind) : CsQualifiedType(ctx, t.QualifiedName);
			int slot = ctx.AllocateSlot(out string fnName);
			ctx.CppFuncs.Append("\textern \"C\" void ").Append(fnName).Append("(void* outValue) {\n");
			ctx.CppFuncs.Append("\t\tstd::memcpy(outValue, &::").Append(gv.QualifiedName).Append(", sizeof(::").Append(gv.QualifiedName).Append("));\n");
			ctx.CppFuncs.Append("\t}\n\n");

			sb.Append("\t\tpublic static ").Append(csType).Append(' ').Append(EscapeCsIdent(gv.Name)).Append(" {\n");
			sb.Append("\t\t\tget {\n");
			sb.Append("\t\t\t\t").Append(csType).Append(" res = default;\n");
			sb.Append("\t\t\t\t((delegate* unmanaged[Cdecl]<void*, void>)").Append(CacheType(ctx)).Append(".Slot(")
			  .Append(slot.ToString(CultureInfo.InvariantCulture)).Append("))(&res);\n");
			sb.Append("\t\t\t\treturn res;\n");
			sb.Append("\t\t\t}\n");
			sb.Append("\t\t}\n\n");
		} else if (t.IsScalar || (t.Kind == CppTypeKind.Enum && ctx.Model.Enums.ContainsKey(t.QualifiedName))) {
			string csType = t.IsScalar ? MapScalarCs(t.Kind) : CsQualifiedType(ctx, t.QualifiedName);
			string abiCs = t.IsScalar ? MapScalarAbiCs(t.Kind) : ctx.Model.Enums[t.QualifiedName].UnderlyingCSharp;
			string abiCpp = t.IsScalar ? MapScalarAbiCpp(t.Kind) : ctx.Model.Enums[t.QualifiedName].UnderlyingCpp;
			int slot = ctx.AllocateSlot(out string fnName);
			ctx.CppFuncs.Append("\textern \"C\" ").Append(abiCpp).Append(' ').Append(fnName).Append("() {\n");
			ctx.CppFuncs.Append("\t\treturn static_cast<").Append(abiCpp).Append(">(::").Append(gv.QualifiedName).Append(");\n");
			ctx.CppFuncs.Append("\t}\n\n");
			string fromRaw = t.Kind == CppTypeKind.Bool ? "raw != 0" : (t.Kind == CppTypeKind.Enum ? $"({csType})raw" : "raw");
			sb.Append("\t\tpublic static ").Append(csType).Append(' ').Append(EscapeCsIdent(gv.Name)).Append(" {\n");
			sb.Append("\t\t\tget {\n");
			sb.Append("\t\t\t\t").Append(abiCs).Append(" raw = ((delegate* unmanaged[Cdecl]<").Append(abiCs).Append(">)")
			  .Append(CacheType(ctx)).Append(".Slot(").Append(slot.ToString(CultureInfo.InvariantCulture)).Append("))();\n");
			sb.Append("\t\t\t\treturn ").Append(fromRaw).Append(";\n");
			sb.Append("\t\t\t}\n");
			sb.Append("\t\t}\n\n");
		}
	}

	static void EmitCallableOverloads(Context ctx, StringBuilder sb, CppRecord? owner, CppFunction fn, string indent, HashSet<string> emittedSigs) {
		ReturnMarshal? ret = BuildReturnMarshal(ctx, owner, fn);
		if (ret == null) {
			return;
		}
		int minArity = fn.Params.Count;
		for (int i = 0; i < fn.Params.Count; ++i) {
			if (fn.Params[i].HasDefault) {
				minArity = i;
				break;
			}
		}
		for (int arity = minArity; arity <= fn.Params.Count; ++arity) {
			List<ParamMarshal> marshaled = new();
			bool ok = true;
			for (int i = 0; i < arity; ++i) {
				ParamMarshal? m = BuildParamMarshal(ctx, fn.Params[i], i);
				if (m == null) {
					ok = false;
					break;
				}
				marshaled.Add(m);
			}
			if (!ok) {
				continue;
			}

			bool isStatic = owner == null || fn.IsStatic;
			string sigKey = $"{(isStatic ? "S" : "I")}:{fn.Name}({string.Join(",", marshaled.Select(static m => m.CsSignatureType))})";
			if (!emittedSigs.Add(sigKey)) {
				continue;
			}

			int slot = ctx.AllocateSlot(out string thunkName);
			List<string> cppSigParams = new();
			if (!isStatic && owner != null) {
				cppSigParams.Add("void* self");
			}
			StringBuilder cppPrologue = new();
			StringBuilder cppEpilogue = new();
			List<string> cppCallArgs = new();
			foreach (ParamMarshal m in marshaled) {
				cppSigParams.AddRange(m.CppParams);
				cppPrologue.Append(m.CppPrologue);
				cppEpilogue.Append(m.CppEpilogue);
				cppCallArgs.Add(m.CppCallExpr);
			}
			cppSigParams.AddRange(ret.ExtraCppParams);

			string invokeTarget;
			if (owner == null) {
				invokeTarget = "::" + fn.QualifiedName;
			} else if (isStatic) {
				invokeTarget = "::" + owner.QualifiedName + "::" + fn.Name;
			} else if (owner.IsValueStruct) {
				cppPrologue.Insert(0, $"\t\t::{owner.QualifiedName} sltSelfCopy;\n\t\tstd::memcpy(&sltSelfCopy, self, sizeof(sltSelfCopy));\n");
				if (!fn.IsConst) {
					cppEpilogue.Append("\t\tstd::memcpy(self, &sltSelfCopy, sizeof(sltSelfCopy));\n");
				}
				invokeTarget = "sltSelfCopy." + fn.Name;
			} else {
				invokeTarget = $"static_cast<::{owner.QualifiedName}*>(self)->{fn.Name}";
			}
			string callExpr = invokeTarget + "(" + string.Join(", ", cppCallArgs) + ")";

			ctx.CppFuncs.Append("\textern \"C\" ").Append(ret.CppAbiReturn).Append(' ').Append(thunkName)
			   .Append('(').Append(string.Join(", ", cppSigParams)).Append(") {\n");
			ctx.CppFuncs.Append(cppPrologue);
			if (ret.Kind == ReturnKind.Void) {
				ctx.CppFuncs.Append("\t\t").Append(callExpr).Append(";\n");
				ctx.CppFuncs.Append(cppEpilogue);
			} else if (ret.Kind == ReturnKind.DirectScalarOrEnum) {
				ctx.CppFuncs.Append("\t\tauto sltRet = ").Append(callExpr).Append(";\n");
				ctx.CppFuncs.Append(cppEpilogue);
				ctx.CppFuncs.Append("\t\treturn static_cast<").Append(ret.CppAbiReturn).Append(">(sltRet);\n");
			} else if (ret.Kind == ReturnKind.OpaquePointer) {
				ctx.CppFuncs.Append("\t\tauto sltRet = ").Append(callExpr).Append(";\n");
				ctx.CppFuncs.Append(cppEpilogue);
				ctx.CppFuncs.Append("\t\treturn static_cast<void*>(const_cast<::").Append(ret.TargetQualified).Append("*>(sltRet));\n");
			} else if (ret.Kind == ReturnKind.OpaqueRef) {
				ctx.CppFuncs.Append("\t\tauto& sltRet = ").Append(callExpr).Append(";\n");
				ctx.CppFuncs.Append(cppEpilogue);
				ctx.CppFuncs.Append("\t\treturn static_cast<void*>(const_cast<::").Append(ret.TargetQualified).Append("*>(&sltRet));\n");
			} else if (ret.Kind == ReturnKind.ValueStructOut) {
				ctx.CppFuncs.Append("\t\tauto sltRet = ").Append(callExpr).Append(";\n");
				ctx.CppFuncs.Append(cppEpilogue);
				ctx.CppFuncs.Append("\t\tstd::memcpy(outResult, &sltRet, sizeof(sltRet));\n");
			} else if (ret.Kind == ReturnKind.StringOut) {
				ctx.CppFuncs.Append("\t\tauto sltRet = ").Append(callExpr).Append(";\n");
				ctx.CppFuncs.Append(cppEpilogue);
				ctx.CppFuncs.Append("\t\t_sltReturnStringScratch = std::string(sltRet);\n");
				ctx.CppFuncs.Append("\t\t*outLen = static_cast<int32_t>(_sltReturnStringScratch.size());\n");
				ctx.CppFuncs.Append("\t\treturn _sltReturnStringScratch.c_str();\n");
			} else if (ret.Kind == ReturnKind.CStringOut) {
				ctx.CppFuncs.Append("\t\tconst char* sltRet = ").Append(callExpr).Append(";\n");
				ctx.CppFuncs.Append(cppEpilogue);
				ctx.CppFuncs.Append("\t\t*outLen = sltRet ? static_cast<int32_t>(std::strlen(sltRet)) : 0;\n");
				ctx.CppFuncs.Append("\t\treturn sltRet;\n");
			} else if (ret.Kind == ReturnKind.VectorOfPtrOut) {
				ctx.CppFuncs.Append("\t\tconst auto& sltVec = ").Append(callExpr).Append(";\n");
				ctx.CppFuncs.Append(cppEpilogue);
				ctx.CppFuncs.Append("\t\tint32_t sltTotal = 0;\n");
				ctx.CppFuncs.Append("\t\tfor (auto* sltItem : sltVec) {\n");
				ctx.CppFuncs.Append("\t\t\tif (sltTotal < outCap) {\n");
				ctx.CppFuncs.Append("\t\t\t\toutBuf[sltTotal] = static_cast<void*>(const_cast<::").Append(ret.TargetQualified).Append("*>(sltItem));\n");
				ctx.CppFuncs.Append("\t\t\t}\n");
				ctx.CppFuncs.Append("\t\t\t++sltTotal;\n");
				ctx.CppFuncs.Append("\t\t}\n");
				ctx.CppFuncs.Append("\t\treturn sltTotal;\n");
			}
			ctx.CppFuncs.Append("\t}\n\n");

			sb.Append(indent).Append("public ").Append(isStatic ? "static " : "").Append(ret.CsReturnType).Append(' ')
			  .Append(EscapeCsIdent(fn.Name)).Append('(')
			  .Append(string.Join(", ", marshaled.Select(static m => m.CsParamDecl))).Append(") {\n");
			EmitCsCallBody(ctx, sb, indent + "\t", slot, marshaled, ret, !isStatic, owner != null && owner.IsValueStruct, owner != null && !owner.IsValueStruct, owner?.Name, ctorAssign: false);
			sb.Append(indent).Append("}\n\n");
		}
	}

	enum ReturnKind {
		Void,
		DirectScalarOrEnum,
		OpaquePointer,
		OpaqueRef,
		ValueStructOut,
		StringOut,
		CStringOut,
		VectorOfPtrOut,
	}

	sealed class ReturnMarshal {
		public ReturnKind Kind;
		public string CsReturnType = "void";
		public string CppAbiReturn = "void";
		public string CsAbiReturn = "void";
		public string TargetQualified = "";
		public bool ReturnBool;
		public bool ReturnEnum;
		public bool OwnsReturnedPointer;
		public List<string> ExtraCppParams = new();
		public List<string> ExtraCsAbiParams = new();
	}

	static ReturnMarshal? BuildReturnMarshal(Context ctx, CppRecord? owner, CppFunction fn) {
		CppTypeRef r = fn.ReturnType;
		if (r.Kind == CppTypeKind.Void) {
			return new ReturnMarshal { Kind = ReturnKind.Void };
		}
		if (r.IsScalar) {
			return new ReturnMarshal {
				Kind = ReturnKind.DirectScalarOrEnum,
				CsReturnType = MapScalarCs(r.Kind),
				CppAbiReturn = MapScalarAbiCpp(r.Kind),
				CsAbiReturn = MapScalarAbiCs(r.Kind),
				ReturnBool = r.Kind == CppTypeKind.Bool,
			};
		}
		if (r.Kind == CppTypeKind.Enum && ctx.Model.Enums.TryGetValue(r.QualifiedName, out CppEnum? en)) {
			return new ReturnMarshal {
				Kind = ReturnKind.DirectScalarOrEnum,
				CsReturnType = CsQualifiedType(ctx, r.QualifiedName),
				CppAbiReturn = en.UnderlyingCpp,
				CsAbiReturn = en.UnderlyingCSharp,
				ReturnEnum = true,
			};
		}
		if (r.Kind == CppTypeKind.VoidPtr) {
			return new ReturnMarshal {
				Kind = ReturnKind.DirectScalarOrEnum,
				CsReturnType = "nint",
				CppAbiReturn = "void*",
				CsAbiReturn = "nint",
			};
		}
		if (r.Kind == CppTypeKind.StdString) {
			return new ReturnMarshal {
				Kind = ReturnKind.StringOut,
				CsReturnType = "string",
				CppAbiReturn = "const char*",
				CsAbiReturn = "byte*",
				ExtraCppParams = ["int32_t* outLen"],
				ExtraCsAbiParams = ["int*"],
			};
		}
		if (r.Kind == CppTypeKind.CString) {
			return new ReturnMarshal {
				Kind = ReturnKind.CStringOut,
				CsReturnType = "string",
				CppAbiReturn = "const char*",
				CsAbiReturn = "byte*",
				ExtraCppParams = ["int32_t* outLen"],
				ExtraCsAbiParams = ["int*"],
			};
		}
		if (r.IsMath || (r.Kind == CppTypeKind.Record && ctx.Model.Records.TryGetValue(r.QualifiedName, out CppRecord? valRec) && valRec.IsValueStruct)) {
			return new ReturnMarshal {
				Kind = ReturnKind.ValueStructOut,
				CsReturnType = r.IsMath ? MapMathCs(r.Kind) : CsQualifiedType(ctx, r.QualifiedName),
				CppAbiReturn = "void",
				CsAbiReturn = "void",
				ExtraCppParams = ["void* outResult"],
				ExtraCsAbiParams = ["void*"],
			};
		}
		if (r.Kind == CppTypeKind.Reference && r.Element != null) {
			if (r.Element.IsMath || (r.Element.Kind == CppTypeKind.Record && ctx.Model.Records.TryGetValue(r.Element.QualifiedName, out CppRecord? refValRec) && refValRec.IsValueStruct)) {
				return new ReturnMarshal {
					Kind = ReturnKind.ValueStructOut,
					CsReturnType = r.Element.IsMath ? MapMathCs(r.Element.Kind) : CsQualifiedType(ctx, r.Element.QualifiedName),
					CppAbiReturn = "void",
					CsAbiReturn = "void",
					ExtraCppParams = ["void* outResult"],
					ExtraCsAbiParams = ["void*"],
				};
			}
			if (r.Element.Kind == CppTypeKind.Record && ctx.Model.Records.TryGetValue(r.Element.QualifiedName, out CppRecord? refOpaqueRec) && !refOpaqueRec.IsValueStruct) {
				return new ReturnMarshal {
					Kind = ReturnKind.OpaqueRef,
					CsReturnType = CsQualifiedType(ctx, refOpaqueRec.QualifiedName),
					CppAbiReturn = "void*",
					CsAbiReturn = "nint",
					TargetQualified = refOpaqueRec.QualifiedName,
					OwnsReturnedPointer = false,
				};
			}
		}
		if (r.Kind == CppTypeKind.Pointer && r.Element != null && r.Element.Kind == CppTypeKind.Record &&
			ctx.Model.Records.TryGetValue(r.Element.QualifiedName, out CppRecord? ptrRec) && !ptrRec.IsValueStruct) {
			bool owns = (owner != null && fn.IsStatic && ptrRec.QualifiedName == owner.QualifiedName &&
						 (fn.Name == "Create" || fn.Name == "DeserializeNew")) ||
						(owner != null && !fn.IsStatic && ptrRec.QualifiedName == owner.QualifiedName && fn.Name == "Clone");
			return new ReturnMarshal {
				Kind = ReturnKind.OpaquePointer,
				CsReturnType = CsQualifiedType(ctx, ptrRec.QualifiedName) + "?",
				CppAbiReturn = "void*",
				CsAbiReturn = "nint",
				TargetQualified = ptrRec.QualifiedName,
				OwnsReturnedPointer = owns,
			};
		}
		CppTypeRef vecSource = (r.Kind == CppTypeKind.Reference && r.Element != null) ? r.Element : r;
		if (vecSource.Kind == CppTypeKind.VectorOfPtr && vecSource.Element != null && vecSource.Element.Kind == CppTypeKind.Record &&
			ctx.Model.Records.TryGetValue(vecSource.Element.QualifiedName, out CppRecord? vecRec) && !vecRec.IsValueStruct) {
			return new ReturnMarshal {
				Kind = ReturnKind.VectorOfPtrOut,
				CsReturnType = CsQualifiedType(ctx, vecRec.QualifiedName) + "[]",
				CppAbiReturn = "int32_t",
				CsAbiReturn = "int",
				TargetQualified = vecRec.QualifiedName,
				ExtraCppParams = ["void** outBuf", "int32_t outCap"],
				ExtraCsAbiParams = ["nint*", "int"],
			};
		}
		return null;
	}

	sealed class ParamMarshal {
		public string CsParamDecl = "";
		public string CsSignatureType = "";
		public List<string> CppParams = new();
		public List<string> CsAbiParams = new();
		public string CppPrologue = "";
		public string CppEpilogue = "";
		public string CppCallExpr = "";
		public string CsFixedOpen = "";
		public string CsFixedClose = "";
		public string CsPrologue = "";
		public string CsEpilogue = "";
		public List<string> CsCallArgs = new();
	}

	static ParamMarshal? BuildParamMarshal(Context ctx, CppParam param, int idx) {
		string pIdent = EscapeCsIdent(param.Name);
		string cName = "p" + idx.ToString(CultureInfo.InvariantCulture);
		CppTypeRef t = param.Type;

		if (t.IsScalar) {
			string cs = MapScalarCs(t.Kind);
			string abiCs = MapScalarAbiCs(t.Kind);
			string abiCpp = MapScalarAbiCpp(t.Kind);
			return new ParamMarshal {
				CsParamDecl = $"{cs} {pIdent}",
				CsSignatureType = cs,
				CppParams = [$"{abiCpp} {cName}"],
				CsAbiParams = [abiCs],
				CppCallExpr = t.Kind == CppTypeKind.Bool ? $"({cName} != 0)" : cName,
				CsCallArgs = [t.Kind == CppTypeKind.Bool ? $"(byte)({pIdent} ? 1 : 0)" : pIdent],
			};
		}
		if (t.Kind == CppTypeKind.Enum && ctx.Model.Enums.TryGetValue(t.QualifiedName, out CppEnum? en)) {
			string cs = CsQualifiedType(ctx, t.QualifiedName);
			return new ParamMarshal {
				CsParamDecl = $"{cs} {pIdent}",
				CsSignatureType = cs,
				CppParams = [$"{en.UnderlyingCpp} {cName}"],
				CsAbiParams = [en.UnderlyingCSharp],
				CppCallExpr = $"static_cast<::{t.QualifiedName}>({cName})",
				CsCallArgs = [$"({en.UnderlyingCSharp}){pIdent}"],
			};
		}
		if (t.Kind == CppTypeKind.VoidPtr) {
			return new ParamMarshal {
				CsParamDecl = $"nint {pIdent}",
				CsSignatureType = "nint",
				CppParams = [$"void* {cName}"],
				CsAbiParams = ["nint"],
				CppCallExpr = cName,
				CsCallArgs = [pIdent],
			};
		}
		if (t.Kind is CppTypeKind.CString or CppTypeKind.StdString or CppTypeKind.FilesystemPath) {
			string cppConv;
			if (t.Kind == CppTypeKind.CString) {
				cppConv = $"{cName}Ptr";
			} else if (t.Kind == CppTypeKind.FilesystemPath) {
				cppConv = $"std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>({cName}Ptr ? {cName}Ptr : \"\"), static_cast<size_t>({cName}Len)))";
			} else if (t.CppSpelling.Contains("string_view", StringComparison.Ordinal)) {
				cppConv = $"std::string_view({cName}Ptr ? {cName}Ptr : \"\", static_cast<size_t>({cName}Len))";
			} else if (t.CppSpelling.Contains("Shalltear::str", StringComparison.Ordinal)) {
				cppConv = $"::Shalltear::str({cName}Ptr ? {cName}Ptr : \"\", static_cast<size_t>({cName}Len))";
			} else {
				cppConv = $"std::string({cName}Ptr ? {cName}Ptr : \"\", static_cast<size_t>({cName}Len))";
			}

			string bufVar = $"__strBuf{idx}";
			return new ParamMarshal {
				CsParamDecl = $"string {pIdent}",
				CsSignatureType = "string",
				CppParams = [$"const char* {cName}Ptr", $"int32_t {cName}Len"],
				CsAbiParams = ["byte*", "int"],
				CppCallExpr = cppConv,
				CsPrologue = $"using NativeUTF8Buffer {bufVar} = new({pIdent}, stackalloc byte[256]);\n",
				CsCallArgs = [$"{bufVar}.Pointer", $"{bufVar}.Length"],
			};
		}
		// Value struct / Math passed by value or const ref
		if (t.IsMath || (t.Kind == CppTypeKind.Record && ctx.Model.Records.TryGetValue(t.QualifiedName, out CppRecord? valRec) && valRec.IsValueStruct)) {
			string cs = t.IsMath ? MapMathCs(t.Kind) : CsQualifiedType(ctx, t.QualifiedName);
			string cppType = "::" + t.QualifiedName;
			return new ParamMarshal {
				CsParamDecl = $"{cs} {pIdent}",
				CsSignatureType = cs,
				CppParams = [$"const void* {cName}"],
				CsAbiParams = ["void*"],
				CppPrologue = $"\t\t{cppType} {cName}Val;\n\t\tstd::memcpy(&{cName}Val, {cName}, sizeof({cName}Val));\n",
				CppCallExpr = $"{cName}Val",
				CsPrologue = $"{cs} __copy{idx} = {pIdent};\n",
				CsCallArgs = [$"&__copy{idx}"],
			};
		}
		// Reference parameter (const T& or T&)
		if (t.Kind == CppTypeKind.Reference && t.Element != null) {
			CppTypeRef elem = t.Element;
			if (elem.IsMath || (elem.Kind == CppTypeKind.Record && ctx.Model.Records.TryGetValue(elem.QualifiedName, out CppRecord? rVal) && rVal.IsValueStruct)) {
				string cs = elem.IsMath ? MapMathCs(elem.Kind) : CsQualifiedType(ctx, elem.QualifiedName);
				string cppType = "::" + elem.QualifiedName;
				if (t.IsConst) {
					return new ParamMarshal {
						CsParamDecl = $"in {cs} {pIdent}",
						CsSignatureType = "in " + cs,
						CppParams = [$"const void* {cName}"],
						CsAbiParams = ["void*"],
						CppPrologue = $"\t\t{cppType} {cName}Val;\n\t\tstd::memcpy(&{cName}Val, {cName}, sizeof({cName}Val));\n",
						CppCallExpr = $"{cName}Val",
						CsFixedOpen = $"fixed ({cs}* __p{idx} = &{pIdent}) {{",
						CsFixedClose = "}",
						CsCallArgs = [$"__p{idx}"],
					};
				}
				return new ParamMarshal {
					CsParamDecl = $"ref {cs} {pIdent}",
					CsSignatureType = "ref " + cs,
					CppParams = [$"void* {cName}"],
					CsAbiParams = ["void*"],
					CppPrologue = $"\t\t{cppType} {cName}Val;\n\t\tstd::memcpy(&{cName}Val, {cName}, sizeof({cName}Val));\n",
					CppEpilogue = $"\t\tstd::memcpy({cName}, &{cName}Val, sizeof({cName}Val));\n",
					CppCallExpr = $"{cName}Val",
					CsFixedOpen = $"fixed ({cs}* __p{idx} = &{pIdent}) {{",
					CsFixedClose = "}",
					CsCallArgs = [$"__p{idx}"],
				};
			}
			if (elem.IsScalar && !t.IsConst) {
				string cs = MapScalarCs(elem.Kind);
				string abiCs = MapScalarAbiCs(elem.Kind);
				string abiCpp = MapScalarAbiCpp(elem.Kind);
				string localCpp = elem.Kind == CppTypeKind.Bool ? "bool" : abiCpp;
				return new ParamMarshal {
					CsParamDecl = $"ref {cs} {pIdent}",
					CsSignatureType = "ref " + cs,
					CppParams = [$"{abiCpp}* {cName}"],
					CsAbiParams = [$"{abiCs}*"],
					CppPrologue = elem.Kind == CppTypeKind.Bool ? $"\t\tbool {cName}Val = (*{cName} != 0);\n" : "",
					CppEpilogue = elem.Kind == CppTypeKind.Bool ? $"\t\t*{cName} = {cName}Val ? 1 : 0;\n" : "",
					CppCallExpr = elem.Kind == CppTypeKind.Bool ? $"{cName}Val" : $"*{cName}",
					CsPrologue = elem.Kind == CppTypeKind.Bool ? $"byte __b{idx} = (byte)({pIdent} ? 1 : 0);\n" : "",
					CsFixedOpen = elem.Kind == CppTypeKind.Bool ? "" : $"fixed ({abiCs}* __p{idx} = &{pIdent}) {{",
					CsFixedClose = elem.Kind == CppTypeKind.Bool ? "" : "}",
					CsEpilogue = elem.Kind == CppTypeKind.Bool ? $"{pIdent} = __b{idx} != 0;\n" : "",
					CsCallArgs = [elem.Kind == CppTypeKind.Bool ? $"&__b{idx}" : $"__p{idx}"],
				};
			}
			if (elem.Kind == CppTypeKind.Record && ctx.Model.Records.TryGetValue(elem.QualifiedName, out CppRecord? rOpaque) && !rOpaque.IsValueStruct) {
				string cs = CsQualifiedType(ctx, rOpaque.QualifiedName);
				return new ParamMarshal {
					CsParamDecl = $"{cs} {pIdent}",
					CsSignatureType = cs,
					CppParams = [$"void* {cName}"],
					CsAbiParams = ["nint"],
					CppCallExpr = $"*static_cast<::{rOpaque.QualifiedName}*>({cName})",
					CsCallArgs = [$"{pIdent}._ptr"],
				};
			}
		}
		// Pointer parameter (OpaqueClass* or ValueStruct*/Scalar*)
		if (t.Kind == CppTypeKind.Pointer && t.Element != null) {
			CppTypeRef elem = t.Element;
			if (elem.Kind == CppTypeKind.Record && ctx.Model.Records.TryGetValue(elem.QualifiedName, out CppRecord? pOpaque) && !pOpaque.IsValueStruct) {
				string cs = CsQualifiedType(ctx, pOpaque.QualifiedName);
				return new ParamMarshal {
					CsParamDecl = $"{cs}? {pIdent}",
					CsSignatureType = cs + "?",
					CppParams = [$"void* {cName}"],
					CsAbiParams = ["nint"],
					CppCallExpr = $"static_cast<::{pOpaque.QualifiedName}*>({cName})",
					CsCallArgs = [$"{pIdent} != null ? {pIdent}._ptr : 0"],
				};
			}
			if (elem.Kind == CppTypeKind.Enum && ctx.Model.Enums.TryGetValue(elem.QualifiedName, out CppEnum? pEn) && !t.IsConst) {
				string cs = CsQualifiedType(ctx, pEn.QualifiedName);
				return new ParamMarshal {
					CsParamDecl = $"ref {cs} {pIdent}",
					CsSignatureType = "ref " + cs,
					CppParams = [$"{pEn.UnderlyingCpp}* {cName}"],
					CsAbiParams = [$"{pEn.UnderlyingCSharp}*"],
					CppPrologue = $"\t\t::{pEn.QualifiedName} {cName}Val = static_cast<::{pEn.QualifiedName}>(*{cName});\n",
					CppEpilogue = $"\t\t*{cName} = static_cast<{pEn.UnderlyingCpp}>({cName}Val);\n",
					CppCallExpr = $"&{cName}Val",
					CsPrologue = $"{pEn.UnderlyingCSharp} __e{idx} = ({pEn.UnderlyingCSharp}){pIdent};\n",
					CsEpilogue = $"{pIdent} = ({cs})__e{idx};\n",
					CsCallArgs = [$"&__e{idx}"],
				};
			}
			if (elem.IsMath || (elem.Kind == CppTypeKind.Record && ctx.Model.Records.TryGetValue(elem.QualifiedName, out CppRecord? pVal) && pVal.IsValueStruct)) {
				string cs = elem.IsMath ? MapMathCs(elem.Kind) : CsQualifiedType(ctx, elem.QualifiedName);
				string cppType = "::" + elem.QualifiedName;
				if (t.IsConst) {
					return new ParamMarshal {
						CsParamDecl = $"in {cs} {pIdent}",
						CsSignatureType = "in " + cs,
						CppParams = [$"const void* {cName}"],
						CsAbiParams = ["void*"],
						CppPrologue = $"\t\t{cppType} {cName}Val;\n\t\tstd::memcpy(&{cName}Val, {cName}, sizeof({cName}Val));\n",
						CppCallExpr = $"&{cName}Val",
						CsFixedOpen = $"fixed ({cs}* __p{idx} = &{pIdent}) {{",
						CsFixedClose = "}",
						CsCallArgs = [$"__p{idx}"],
					};
				}
				return new ParamMarshal {
					CsParamDecl = $"ref {cs} {pIdent}",
					CsSignatureType = "ref " + cs,
					CppParams = [$"void* {cName}"],
					CsAbiParams = ["void*"],
					CppPrologue = $"\t\t{cppType} {cName}Val;\n\t\tstd::memcpy(&{cName}Val, {cName}, sizeof({cName}Val));\n",
					CppEpilogue = $"\t\tstd::memcpy({cName}, &{cName}Val, sizeof({cName}Val));\n",
					CppCallExpr = $"&{cName}Val",
					CsFixedOpen = $"fixed ({cs}* __p{idx} = &{pIdent}) {{",
					CsFixedClose = "}",
					CsCallArgs = [$"__p{idx}"],
				};
			}
		}
		// std::span<T>
		if (t.Kind == CppTypeKind.Span && t.Element != null) {
			CppTypeRef elem = t.Element;
			string? csElem = elem.IsScalar ? MapScalarCs(elem.Kind) : (elem.IsMath ? MapMathCs(elem.Kind) :
				(elem.Kind == CppTypeKind.Record && ctx.Model.Records.TryGetValue(elem.QualifiedName, out CppRecord? sVal) && sVal.IsValueStruct ? CsQualifiedType(ctx, sVal.QualifiedName) : null));
			string? cppElem = elem.IsScalar ? (elem.Kind == CppTypeKind.Bool ? "bool" : MapScalarAbiCpp(elem.Kind)) :
				(elem.IsMath || elem.Kind == CppTypeKind.Record ? "::" + elem.QualifiedName : null);
			if (csElem != null && cppElem != null) {
				string spanType = t.IsConst ? $"ReadOnlySpan<{csElem}>" : $"Span<{csElem}>";
				string constKw = t.IsConst ? "const " : "";
				return new ParamMarshal {
					CsParamDecl = $"{spanType} {pIdent}",
					CsSignatureType = spanType,
					CppParams = [$"{constKw}void* {cName}Ptr", $"int32_t {cName}Len"],
					CsAbiParams = ["void*", "int"],
					CppCallExpr = $"std::span<{constKw}{cppElem}>(reinterpret_cast<{constKw}{cppElem}*>({cName}Ptr), static_cast<size_t>({cName}Len))",
					CsFixedOpen = $"fixed ({csElem}* __p{idx} = {pIdent}) {{",
					CsFixedClose = "}",
					CsCallArgs = [$"__p{idx}", $"{pIdent}.Length"],
				};
			}
		}
		// std::function<void(...)>
		if (t.Kind == CppTypeKind.StdFunction) {
			return BuildStdFunctionParam(ctx, t, pIdent, cName, idx);
		}
		return null;
	}

	static ParamMarshal? BuildStdFunctionParam(Context ctx, CppTypeRef fnType, string pIdent, string cName, int idx) {
		List<string> csActionArgs = new();
		List<string> cppLambdaParams = new();
		StringBuilder cppPackArgs = new();
		List<string> csInvokeArgs = new();

		for (int i = 0; i < fnType.CallbackParams.Count; ++i) {
			CppTypeRef cp = fnType.CallbackParams[i];
			string arg = "a" + i.ToString(CultureInfo.InvariantCulture);
			string slot = $"__sltCbArgs[{i.ToString(CultureInfo.InvariantCulture)}]";
			string cppSlot = $"sltArgs[{i.ToString(CultureInfo.InvariantCulture)}]";
			if (cp.IsScalar) {
				string cs = MapScalarCs(cp.Kind);
				string abiCpp = MapScalarAbiCpp(cp.Kind);
				string cppParam = cp.Kind == CppTypeKind.Bool ? "bool" : abiCpp;
				csActionArgs.Add(cs);
				cppLambdaParams.Add($"{cppParam} {arg}");
				switch (cp.Kind) {
				case CppTypeKind.Bool:
					cppPackArgs.Append($"\t\t\t\t{cppSlot} = {arg} ? 1ULL : 0ULL;\n");
					csInvokeArgs.Add($"({slot} != 0UL)");
					break;
				case CppTypeKind.Float:
					cppPackArgs.Append($"\t\t\t\t{{ uint32_t sltBits = 0; std::memcpy(&sltBits, &{arg}, sizeof(sltBits)); {cppSlot} = sltBits; }}\n");
					csInvokeArgs.Add($"BitConverter.Int32BitsToSingle((int)(uint){slot})");
					break;
				case CppTypeKind.Double:
					cppPackArgs.Append($"\t\t\t\t{{ uint64_t sltBits = 0; std::memcpy(&sltBits, &{arg}, sizeof(sltBits)); {cppSlot} = sltBits; }}\n");
					csInvokeArgs.Add($"BitConverter.Int64BitsToDouble((long){slot})");
					break;
				case CppTypeKind.Int8 or CppTypeKind.Int16 or CppTypeKind.Int32 or CppTypeKind.Int64:
					cppPackArgs.Append($"\t\t\t\t{cppSlot} = static_cast<uint64_t>(static_cast<int64_t>({arg}));\n");
					csInvokeArgs.Add($"({cs})(long){slot}");
					break;
				default:
					cppPackArgs.Append($"\t\t\t\t{cppSlot} = static_cast<uint64_t>({arg});\n");
					csInvokeArgs.Add($"({cs}){slot}");
					break;
				}
			} else if (cp.Kind == CppTypeKind.Enum && ctx.Model.Enums.TryGetValue(cp.QualifiedName, out CppEnum? en)) {
				string cs = CsQualifiedType(ctx, cp.QualifiedName);
				csActionArgs.Add(cs);
				cppLambdaParams.Add($"::{cp.QualifiedName} {arg}");
				if (en.IsUnsigned) {
					cppPackArgs.Append($"\t\t\t\t{cppSlot} = static_cast<uint64_t>(static_cast<{en.UnderlyingCpp}>({arg}));\n");
					csInvokeArgs.Add($"({cs})({en.UnderlyingCSharp}){slot}");
				} else {
					cppPackArgs.Append($"\t\t\t\t{cppSlot} = static_cast<uint64_t>(static_cast<int64_t>(static_cast<{en.UnderlyingCpp}>({arg})));\n");
					csInvokeArgs.Add($"({cs})({en.UnderlyingCSharp})(long){slot}");
				}
			} else if (cp.Kind == CppTypeKind.VoidPtr) {
				csActionArgs.Add("nint");
				cppLambdaParams.Add($"void* {arg}");
				cppPackArgs.Append($"\t\t\t\t{cppSlot} = static_cast<uint64_t>(reinterpret_cast<uintptr_t>({arg}));\n");
				csInvokeArgs.Add($"(nint){slot}");
			} else if (cp.Kind == CppTypeKind.Pointer && cp.Element != null && cp.Element.Kind == CppTypeKind.Record &&
					   ctx.Model.Records.TryGetValue(cp.Element.QualifiedName, out CppRecord? rec) && !rec.IsValueStruct) {
				string cs = CsQualifiedType(ctx, rec.QualifiedName);
				csActionArgs.Add(cs + "?");
				cppLambdaParams.Add($"::{rec.QualifiedName}* {arg}");
				cppPackArgs.Append($"\t\t\t\t{cppSlot} = static_cast<uint64_t>(reinterpret_cast<uintptr_t>({arg}));\n");
				csInvokeArgs.Add($"{cs}.FromPointer((nint){slot}, false)");
			} else {
				return null;
			}
		}

		string actionType = csActionArgs.Count == 0 ? "Action" : $"Action<{string.Join(", ", csActionArgs)}>";

		string cppInvokeBody;
		if (fnType.CallbackParams.Count == 0) {
			cppInvokeBody = "\t\t\t\tfn(tok, nullptr);\n";
		} else {
			cppInvokeBody =
				$"\t\t\t\tuint64_t sltArgs[{fnType.CallbackParams.Count.ToString(CultureInfo.InvariantCulture)}] = {{}};\n" +
				cppPackArgs +
				"\t\t\t\tfn(tok, sltArgs);\n";
		}

		string cppPrologue =
			$"\t\tstd::function<void({string.Join(", ", cppLambdaParams)})> {cName}Func;\n" +
			$"\t\tif ({cName}Fn && {cName}Tok != 0) {{\n" +
			$"\t\t\tstd::shared_ptr<void> holder(static_cast<void*>(nullptr), [rel = {cName}Release, tok = {cName}Tok](void*) {{ if (rel && tok != 0) {{ rel(tok); }} }});\n" +
			$"\t\t\t{cName}Func = [fn = {cName}Fn, tok = {cName}Tok, holder]({string.Join(", ", cppLambdaParams)}) {{\n" +
			cppInvokeBody +
			"\t\t\t};\n" +
			"\t\t}\n";

		string cbLocal = $"__cbDelegate{idx}";
		string csPrologue =
			$"{actionType}? {cbLocal} = {pIdent};\n" +
			$"ulong __cbTok{idx} = {cbLocal} != null ? ModuleThunks.KeepCallback(__sltCbArgs => {cbLocal}({string.Join(", ", csInvokeArgs)})) : 0UL;\n" +
			$"nint __cbFn{idx} = __cbTok{idx} != 0UL ? ModuleThunks.CallbackInvokePtr : 0;\n" +
			$"nint __cbRel{idx} = __cbTok{idx} != 0UL ? ModuleThunks.CallbackReleasePtr : 0;\n";

		return new ParamMarshal {
			CsParamDecl = $"{actionType}? {pIdent}",
			CsSignatureType = actionType + "?",
			CppParams = [$"void (*{cName}Fn)(uint64_t, uint64_t*)", $"uint64_t {cName}Tok", $"void (*{cName}Release)(uint64_t)"],
			CsAbiParams = ["nint", "ulong", "nint"],
			CppPrologue = cppPrologue,
			CppCallExpr = $"{cName}Func",
			CsPrologue = csPrologue,
			CsCallArgs = [$"__cbFn{idx}", $"__cbTok{idx}", $"__cbRel{idx}"],
		};
	}

	static void EmitCsCallBody(Context ctx, StringBuilder sb, string indent, int slot, List<ParamMarshal> marshaled,
							   ReturnMarshal? ret, bool hasSelf, bool selfIsValueStruct, bool selfIsOpaque, string? ownerName, bool ctorAssign) {
		if (hasSelf && selfIsOpaque) {
			sb.Append(indent).Append("if (_ptr == 0) { throw new ObjectDisposedException(\"").Append(ownerName).Append("\"); }\n");
		}
		foreach (ParamMarshal m in marshaled) {
			if (!string.IsNullOrEmpty(m.CsPrologue)) {
				foreach (string line in m.CsPrologue.Split('\n', StringSplitOptions.RemoveEmptyEntries)) {
					sb.Append(indent).Append(line).Append('\n');
				}
			}
		}

		// Declare return locals outside any fixed (...) scopes so return statements can see them.
		if (!ctorAssign && ret != null) {
			if (ret.Kind == ReturnKind.DirectScalarOrEnum) {
				sb.Append(indent).Append(ret.CsAbiReturn).Append(" __raw = default;\n");
			} else if (ret.Kind is ReturnKind.OpaquePointer or ReturnKind.OpaqueRef) {
				sb.Append(indent).Append("nint __raw = 0;\n");
			} else if (ret.Kind == ReturnKind.ValueStructOut) {
				sb.Append(indent).Append(ret.CsReturnType).Append(" __res = default;\n");
			} else if (ret.Kind is ReturnKind.StringOut or ReturnKind.CStringOut) {
				sb.Append(indent).Append("string __res = string.Empty;\n");
			} else if (ret.Kind == ReturnKind.VectorOfPtrOut) {
				string elemCs = CsQualifiedType(ctx, ret.TargetQualified);
				sb.Append(indent).Append(elemCs).Append("[] __res = Array.Empty<").Append(elemCs).Append(">();\n");
			}
		}

		string innerIndent = indent;
		if (hasSelf && selfIsValueStruct) {
			sb.Append(innerIndent).Append("fixed (void* __self = &this) {\n");
			innerIndent += "\t";
		}
		foreach (ParamMarshal m in marshaled) {
			if (!string.IsNullOrEmpty(m.CsFixedOpen)) {
				sb.Append(innerIndent).Append(m.CsFixedOpen).Append('\n');
				innerIndent += "\t";
			}
		}

		List<string> abiParams = new();
		List<string> callArgs = new();
		if (hasSelf) {
			abiParams.Add("void*");
			callArgs.Add(selfIsValueStruct ? "__self" : "(void*)_ptr");
		}
		foreach (ParamMarshal m in marshaled) {
			abiParams.AddRange(m.CsAbiParams);
			callArgs.AddRange(m.CsCallArgs);
		}
		if (ret != null) {
			abiParams.AddRange(ret.ExtraCsAbiParams);
		}
		string abiRet = ctorAssign ? "nint" : (ret?.CsAbiReturn ?? "void");
		string fnPtrType = $"delegate* unmanaged[Cdecl]<{string.Join(", ", abiParams.Concat([abiRet]))}>";
		string slotExpr = $"(({fnPtrType}){CacheType(ctx)}.Slot({slot.ToString(CultureInfo.InvariantCulture)}))";

		if (ctorAssign) {
			sb.Append(innerIndent).Append("_ptr = ").Append(slotExpr).Append('(').Append(string.Join(", ", callArgs)).Append(");\n");
			sb.Append(innerIndent).Append("_owns = true;\n");
			sb.Append(innerIndent).Append("_epoch = _ptr != 0 ? ").Append(CacheType(ctx)).Append(".ModuleEpoch : 0UL;\n");
		} else if (ret == null || ret.Kind == ReturnKind.Void) {
			sb.Append(innerIndent).Append(slotExpr).Append('(').Append(string.Join(", ", callArgs)).Append(");\n");
		} else if (ret.Kind == ReturnKind.DirectScalarOrEnum) {
			sb.Append(innerIndent).Append("__raw = ").Append(slotExpr).Append('(').Append(string.Join(", ", callArgs)).Append(");\n");
		} else if (ret.Kind is ReturnKind.OpaquePointer or ReturnKind.OpaqueRef) {
			sb.Append(innerIndent).Append("__raw = ").Append(slotExpr).Append('(').Append(string.Join(", ", callArgs)).Append(");\n");
		} else if (ret.Kind == ReturnKind.ValueStructOut) {
			callArgs.Add("&__res");
			sb.Append(innerIndent).Append(slotExpr).Append('(').Append(string.Join(", ", callArgs)).Append(");\n");
		} else if (ret.Kind is ReturnKind.StringOut or ReturnKind.CStringOut) {
			sb.Append(innerIndent).Append("int __len = 0;\n");
			callArgs.Add("&__len");
			sb.Append(innerIndent).Append("byte* __str = ").Append(slotExpr).Append('(').Append(string.Join(", ", callArgs)).Append(");\n");
			sb.Append(innerIndent).Append("__res = (__str != null && __len > 0) ? Encoding.UTF8.GetString(__str, __len) : string.Empty;\n");
		} else if (ret.Kind == ReturnKind.VectorOfPtrOut) {
			string elemCs = CsQualifiedType(ctx, ret.TargetQualified);
			sb.Append(innerIndent).Append("nint* __stackBuf = stackalloc nint[64];\n");
			List<string> call1 = new(callArgs) { "__stackBuf", "64" };
			sb.Append(innerIndent).Append("int __total = ").Append(slotExpr).Append('(').Append(string.Join(", ", call1)).Append(");\n");
			sb.Append(innerIndent).Append("__res = new ").Append(elemCs).Append("[__total];\n");
			sb.Append(innerIndent).Append("if (__total <= 64) {\n");
			sb.Append(innerIndent).Append("\tfor (int __i = 0; __i < __total; ++__i) { __res[__i] = new ").Append(elemCs).Append("(__stackBuf[__i], false); }\n");
			sb.Append(innerIndent).Append("} else {\n");
			sb.Append(innerIndent).Append("\tnint[] __heap = new nint[__total];\n");
			sb.Append(innerIndent).Append("\tfixed (nint* __hp = __heap) {\n");
			List<string> call2 = new(callArgs) { "__hp", "__total" };
			sb.Append(innerIndent).Append("\t\t").Append(slotExpr).Append('(').Append(string.Join(", ", call2)).Append(");\n");
			sb.Append(innerIndent).Append("\t}\n");
			sb.Append(innerIndent).Append("\tfor (int __i = 0; __i < __total; ++__i) { __res[__i] = new ").Append(elemCs).Append("(__heap[__i], false); }\n");
			sb.Append(innerIndent).Append("}\n");
		}

		// Close fixed blocks
		for (int i = marshaled.Count - 1; i >= 0; --i) {
			if (!string.IsNullOrEmpty(marshaled[i].CsFixedClose)) {
				innerIndent = innerIndent.Substring(0, innerIndent.Length - 1);
				sb.Append(innerIndent).Append(marshaled[i].CsFixedClose).Append('\n');
			}
		}
		if (hasSelf && selfIsValueStruct) {
			innerIndent = innerIndent.Substring(0, innerIndent.Length - 1);
			sb.Append(innerIndent).Append("}\n");
		}
		foreach (ParamMarshal m in marshaled) {
			if (!string.IsNullOrEmpty(m.CsEpilogue)) {
				foreach (string line in m.CsEpilogue.Split('\n', StringSplitOptions.RemoveEmptyEntries)) {
					sb.Append(indent).Append(line).Append('\n');
				}
			}
		}

		// Return statement
		if (!ctorAssign && ret != null && ret.Kind != ReturnKind.Void) {
			if (ret.Kind == ReturnKind.DirectScalarOrEnum) {
				string expr = ret.ReturnBool ? "__raw != 0" : (ret.ReturnEnum ? $"({ret.CsReturnType})__raw" : "__raw");
				sb.Append(indent).Append("return ").Append(expr).Append(";\n");
			} else if (ret.Kind == ReturnKind.OpaquePointer) {
				string targetCs = CsQualifiedType(ctx, ret.TargetQualified);
				sb.Append(indent).Append("return ").Append(targetCs).Append(".FromPointer(__raw, ")
				  .Append(ret.OwnsReturnedPointer ? "true" : "false").Append(");\n");
			} else if (ret.Kind == ReturnKind.OpaqueRef) {
				string targetCs = CsQualifiedType(ctx, ret.TargetQualified);
				sb.Append(indent).Append("return new ").Append(targetCs).Append("(__raw, false);\n");
			} else {
				sb.Append(indent).Append("return __res;\n");
			}
		}
	}

	static string? MapStructFieldCs(Context ctx, CppTypeRef t) {
		if (t.IsScalar) {
			return MapScalarCs(t.Kind);
		}
		if (t.IsMath) {
			return MapMathCs(t.Kind);
		}
		if (t.Kind == CppTypeKind.Enum && ctx.Model.Enums.ContainsKey(t.QualifiedName)) {
			return CsQualifiedType(ctx, t.QualifiedName);
		}
		if (t.Kind == CppTypeKind.Record && ctx.Model.Records.TryGetValue(t.QualifiedName, out CppRecord? rec) && rec.IsValueStruct) {
			return CsQualifiedType(ctx, t.QualifiedName);
		}
		return null;
	}

	static string MapScalarCs(CppTypeKind k) => k switch {
		CppTypeKind.Bool => "bool",
		CppTypeKind.Int8 => "sbyte",
		CppTypeKind.UInt8 => "byte",
		CppTypeKind.Int16 => "short",
		CppTypeKind.UInt16 => "ushort",
		CppTypeKind.Int32 => "int",
		CppTypeKind.UInt32 => "uint",
		CppTypeKind.Int64 => "long",
		CppTypeKind.UInt64 => "ulong",
		CppTypeKind.Float => "float",
		CppTypeKind.Double => "double",
		_ => "int",
	};

	static string MapScalarAbiCs(CppTypeKind k) => k == CppTypeKind.Bool ? "byte" : MapScalarCs(k);

	static string MapScalarAbiCpp(CppTypeKind k) => k switch {
		CppTypeKind.Bool => "uint8_t",
		CppTypeKind.Int8 => "int8_t",
		CppTypeKind.UInt8 => "uint8_t",
		CppTypeKind.Int16 => "int16_t",
		CppTypeKind.UInt16 => "uint16_t",
		CppTypeKind.Int32 => "int32_t",
		CppTypeKind.UInt32 => "uint32_t",
		CppTypeKind.Int64 => "int64_t",
		CppTypeKind.UInt64 => "uint64_t",
		CppTypeKind.Float => "float",
		CppTypeKind.Double => "double",
		_ => "int32_t",
	};

	static string MapMathCs(CppTypeKind k) => k switch {
		CppTypeKind.MathVec2 => "global::Shalltear.Vec2",
		CppTypeKind.MathVec3 => "global::Shalltear.Vec3",
		CppTypeKind.MathVec4 => "global::Shalltear.Vec4",
		CppTypeKind.MathQuat => "global::Shalltear.Quat",
		CppTypeKind.MathColor => "global::Shalltear.Color",
		CppTypeKind.MathVec2I => "global::Shalltear.Vec2I",
		_ => "global::Shalltear.Vec3",
	};
}
