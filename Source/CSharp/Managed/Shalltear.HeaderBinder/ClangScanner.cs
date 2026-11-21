using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;

namespace Shalltear.HeaderBinder;

internal sealed class ScanOptions {
	public string ModuleName = "";
	public List<string> Headers = new();
	public List<string> BindNamespaces = new();
	public List<string> IncludeDirs = new();
	public List<string> Defines = new();
	public List<string> ExtraArgs = new();
	public string? LibClangDir;
	public string? Sysroot;
}

internal static unsafe class ClangScanner {
	sealed class ScanContext {
		public nint Tu;
		public HeaderModel Model = new();
		public HashSet<string> ExplicitHeaders = new(OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);
		public List<string> BindNamespaces = new();
		public Dictionary<string, CXType> RecordTypes = new(StringComparer.Ordinal);
		public Dictionary<string, CXCursor> RecordCursors = new(StringComparer.Ordinal);
		public Dictionary<string, List<CppField>> AllRawFields = new(StringComparer.Ordinal);
		public HashSet<string> SeenFunctions = new(StringComparer.Ordinal);
		public HashSet<string> SeenGlobals = new(StringComparer.Ordinal);
	}

	public static HeaderModel Scan(ScanOptions options) {
		LibClang.ConfigureSearchDirectory(options.LibClangDir);

		string virtualFile = "__shalltear_bind_umbrella__.cpp";
		StringBuilder umbrella = new();
		foreach (string header in options.Headers) {
			string normalized = Path.GetFullPath(header).Replace('\\', '/');
			umbrella.Append("#include \"").Append(normalized).Append("\"\n");
		}

		List<string> args = new() {
			"-x", "c++",
			"-std=c++20",
			"-DSHALLTEAR_HEADER_BINDER=1",
			"-Wno-everything",
		};
		if (!string.IsNullOrWhiteSpace(options.Sysroot)) {
			args.Add("--sysroot=" + options.Sysroot);
		}
		foreach (string inc in options.IncludeDirs) {
			if (!string.IsNullOrWhiteSpace(inc)) {
				args.Add("-I" + inc);
			}
		}
		foreach (string def in options.Defines) {
			if (!string.IsNullOrWhiteSpace(def)) {
				args.Add("-D" + def);
			}
		}
		args.AddRange(options.ExtraArgs);

		nint index = LibClang.clang_createIndex(0, 0);
		if (index == 0) {
			throw new InvalidOperationException("libclang: clang_createIndex returned null");
		}

		nint tu = 0;
		List<nint> allocatedArgs = new();
		nint filePtr = 0;
		nint contentPtr = 0;
		try {
			filePtr = Marshal.StringToCoTaskMemUTF8(virtualFile);
			byte[] contentBytes = Encoding.UTF8.GetBytes(umbrella.ToString());
			contentPtr = Marshal.AllocCoTaskMem(contentBytes.Length + 1);
			Marshal.Copy(contentBytes, 0, contentPtr, contentBytes.Length);
			Marshal.WriteByte(contentPtr, contentBytes.Length, 0);

			CXUnsavedFile unsaved = new() {
				Filename = filePtr,
				Contents = contentPtr,
				Length = (uint)contentBytes.Length,
			};

			foreach (string arg in args) {
				allocatedArgs.Add(Marshal.StringToCoTaskMemUTF8(arg));
			}
			nint[] argArray = allocatedArgs.ToArray();

			CXErrorCode err;
			fixed (nint* argPtr = argArray) {
				err = LibClang.clang_parseTranslationUnit2(
					index,
					(byte*)filePtr,
					argPtr,
					argArray.Length,
					&unsaved,
					1,
					LibClang.CXTranslationUnit_SkipFunctionBodies,
					out tu);
			}
			if (err != CXErrorCode.Success || tu == 0) {
				throw new InvalidOperationException($"libclang failed to parse headers (CXErrorCode={err})");
			}

			uint diagCount = LibClang.clang_getNumDiagnostics(tu);
			List<string> fatalErrors = new();
			for (uint i = 0; i < diagCount; ++i) {
				nint diag = LibClang.clang_getDiagnostic(tu, i);
				try {
					CXDiagnosticSeverity sev = LibClang.clang_getDiagnosticSeverity(diag);
					if (sev is CXDiagnosticSeverity.Error or CXDiagnosticSeverity.Fatal) {
						string text = LibClang.ToStringAndDispose(LibClang.clang_formatDiagnostic(diag, 0));
						if (fatalErrors.Count < 10) {
							fatalErrors.Add(text);
						}
					}
				} finally {
					LibClang.clang_disposeDiagnostic(diag);
				}
			}
			if (fatalErrors.Count > 0) {
				Console.Error.WriteLine("[HeaderBinder] Clang parse diagnostics (continuing with partial AST):");
				foreach (string e in fatalErrors) {
					Console.Error.WriteLine("  " + e);
				}
			}

			ScanContext ctx = new() {
				Tu = tu,
				BindNamespaces = options.BindNamespaces.Where(static s => !string.IsNullOrWhiteSpace(s)).ToList(),
			};
			ctx.Model.ModuleName = options.ModuleName;
			foreach (string header in options.Headers) {
				string norm = NormalizePath(header);
				ctx.ExplicitHeaders.Add(norm);
				ctx.Model.BoundHeaders.Add(norm);
			}

			CXCursor root = LibClang.clang_getTranslationUnitCursor(tu);
			VisitScope(ctx, root, "", false);

			ClassifyValueStructs(ctx);
			FlattenRecordFields(ctx);
			return ctx.Model;
		} finally {
			if (tu != 0) {
				LibClang.clang_disposeTranslationUnit(tu);
			}
			LibClang.clang_disposeIndex(index);
			foreach (nint ptr in allocatedArgs) {
				Marshal.FreeCoTaskMem(ptr);
			}
			if (filePtr != 0) {
				Marshal.FreeCoTaskMem(filePtr);
			}
			if (contentPtr != 0) {
				Marshal.FreeCoTaskMem(contentPtr);
			}
		}
	}

	static string NormalizePath(string path) {
		if (string.IsNullOrWhiteSpace(path)) {
			return "";
		}
		try {
			return Path.GetFullPath(path).Replace('\\', '/');
		} catch {
			return path.Replace('\\', '/');
		}
	}

	static bool MatchesBindNamespace(ScanContext ctx, string ns) {
		if (ctx.BindNamespaces.Count == 0 || string.IsNullOrEmpty(ns)) {
			return false;
		}
		foreach (string wanted in ctx.BindNamespaces) {
			if (ns == wanted || ns.StartsWith(wanted + "::", StringComparison.Ordinal)) {
				return true;
			}
		}
		return false;
	}

	static bool ShouldIncludeCursor(ScanContext ctx, CXCursor cursor, string currentNamespace, out bool fromBindNamespace) {
		fromBindNamespace = false;
		if (ExposureFilter.IsExcludedNamespace(currentNamespace)) {
			return false;
		}
		if (HasIgnoreAnnotation(cursor)) {
			return false;
		}
		string filePath = NormalizePath(LibClang.GetCursorFilePath(cursor));
		if (string.IsNullOrEmpty(filePath)) {
			return false;
		}
		bool explicitFile = ctx.ExplicitHeaders.Contains(filePath);
		if (MatchesBindNamespace(ctx, currentNamespace)) {
			fromBindNamespace = true;
			return true;
		}
		if (ctx.BindNamespaces.Count > 0) {
			return explicitFile && !ExposureFilter.IsExcludedHeaderPath(filePath, true);
		}
		return explicitFile && !ExposureFilter.IsExcludedHeaderPath(filePath, true);
	}

	static bool HasIgnoreAnnotation(CXCursor cursor) {
		bool ignored = false;
		LibClang.clang_visitChildren(cursor, (child, _, _) => {
			if (child.Kind == CXCursorKind.AnnotateAttr) {
				string text = LibClang.GetSpelling(child);
				if (text.Contains(ExposureFilter.IGNORE_ANNOTATION, StringComparison.Ordinal)) {
					ignored = true;
					return CXChildVisitResult.Break;
				}
			}
			return CXChildVisitResult.Continue;
		}, 0);
		return ignored;
	}

	static void VisitScope(ScanContext ctx, CXCursor scope, string currentNamespace, bool inRecord) {
		LibClang.clang_visitChildren(scope, (cursor, _, _) => {
			switch (cursor.Kind) {
			case CXCursorKind.Namespace: {
				string nsName = LibClang.GetSpelling(cursor);
				if (string.IsNullOrEmpty(nsName)) {
					return CXChildVisitResult.Continue;
				}
				string nextNs = string.IsNullOrEmpty(currentNamespace) ? nsName : currentNamespace + "::" + nsName;
				if (!ExposureFilter.IsExcludedNamespace(nextNs)) {
					VisitScope(ctx, cursor, nextNs, false);
				}
				return CXChildVisitResult.Continue;
			}
			case CXCursorKind.LinkageSpec:
				VisitScope(ctx, cursor, currentNamespace, inRecord);
				return CXChildVisitResult.Continue;
			case CXCursorKind.EnumDecl:
				if (LibClang.clang_isCursorDefinition(cursor) != 0 && ShouldIncludeCursor(ctx, cursor, currentNamespace, out _)) {
					ScanEnum(ctx, cursor, currentNamespace, null);
				}
				return CXChildVisitResult.Continue;
			case CXCursorKind.StructDecl:
			case CXCursorKind.ClassDecl:
				if (LibClang.clang_isCursorDefinition(cursor) != 0 &&
					LibClang.clang_Cursor_isAnonymous(cursor) == 0 &&
					ShouldIncludeCursor(ctx, cursor, currentNamespace, out bool fromBindNs)) {
					ScanRecord(ctx, cursor, currentNamespace, null, fromBindNs);
				}
				return CXChildVisitResult.Continue;
			case CXCursorKind.FunctionDecl:
				if (ShouldIncludeCursor(ctx, cursor, currentNamespace, out bool fnFromBindNs)) {
					ScanFreeFunction(ctx, cursor, currentNamespace, fnFromBindNs);
				}
				return CXChildVisitResult.Continue;
			case CXCursorKind.VarDecl:
				if (ShouldIncludeCursor(ctx, cursor, currentNamespace, out bool varFromBindNs)) {
					ScanGlobalVar(ctx, cursor, currentNamespace, varFromBindNs);
				}
				return CXChildVisitResult.Continue;
			}
			return CXChildVisitResult.Continue;
		}, 0);
	}

	static void ScanEnum(ScanContext ctx, CXCursor cursor, string ns, CppRecord? parentRecord) {
		string name = LibClang.GetSpelling(cursor);
		bool isAnon = LibClang.clang_Cursor_isAnonymous(cursor) != 0 || string.IsNullOrEmpty(name) || name.Contains('(') || name.Contains(' ');
		if (isAnon && parentRecord == null) {
			return;
		}
		string qualified = isAnon
			? parentRecord!.QualifiedName + "::__anon_enum_" + parentRecord.NestedEnums.Count
			: (parentRecord != null
				? parentRecord.QualifiedName + "::" + name
				: (string.IsNullOrEmpty(ns) ? name : ns + "::" + name));
		if (ctx.Model.Enums.ContainsKey(qualified)) {
			return;
		}

		CXType underlying = LibClang.clang_getCanonicalType(LibClang.clang_getEnumDeclIntegerType(cursor));
		(string csType, string cppType, bool unsigned) = underlying.Kind switch {
			CXTypeKind.SChar or CXTypeKind.Char_S => ("sbyte", "int8_t", false),
			CXTypeKind.UChar or CXTypeKind.Char_U => ("byte", "uint8_t", true),
			CXTypeKind.Short => ("short", "int16_t", false),
			CXTypeKind.UShort => ("ushort", "uint16_t", true),
			CXTypeKind.UInt => ("uint", "uint32_t", true),
			CXTypeKind.Long when LibClang.clang_Type_getSizeOf(underlying) == 8 => ("long", "int64_t", false),
			CXTypeKind.ULong when LibClang.clang_Type_getSizeOf(underlying) == 8 => ("ulong", "uint64_t", true),
			CXTypeKind.ULong => ("uint", "uint32_t", true),
			CXTypeKind.LongLong => ("long", "int64_t", false),
			CXTypeKind.ULongLong => ("ulong", "uint64_t", true),
			_ => ("int", "int32_t", false),
		};

		CppEnum en = new() {
			Namespace = ns,
			Name = isAnon ? "" : name,
			QualifiedName = qualified,
			UnderlyingCSharp = csType,
			UnderlyingCpp = cppType,
			IsUnsigned = unsigned,
			IsAnonymous = isAnon,
		};

		LibClang.clang_visitChildren(cursor, (child, _, _) => {
			if (child.Kind == CXCursorKind.EnumConstantDecl) {
				string cName = LibClang.GetSpelling(child);
				long sVal = LibClang.clang_getEnumConstantDeclValue(child);
				ulong uVal = LibClang.clang_getEnumConstantDeclUnsignedValue(child);
				en.Constants.Add((cName, sVal, uVal));
			}
			return CXChildVisitResult.Continue;
		}, 0);

		ctx.Model.Enums[qualified] = en;
		if (parentRecord != null) {
			parentRecord.NestedEnums.Add(en);
		}
	}

	static CppRecord? ScanRecord(ScanContext ctx, CXCursor cursor, string ns, CppRecord? parentRecord, bool fromBindNamespace) {
		string name = LibClang.GetSpelling(cursor);
		bool isAnon = LibClang.clang_Cursor_isAnonymous(cursor) != 0 || string.IsNullOrEmpty(name);
		string qualified = isAnon
			? ""
			: (parentRecord != null
				? parentRecord.QualifiedName + "::" + name
				: (string.IsNullOrEmpty(ns) ? name : ns + "::" + name));

		if (!isAnon && ctx.Model.Records.ContainsKey(qualified)) {
			return ctx.Model.Records[qualified];
		}

		CXType recordType = LibClang.clang_getCursorType(cursor);
		long sizeBytes = LibClang.clang_Type_getSizeOf(recordType);
		long alignBytes = LibClang.clang_Type_getAlignOf(recordType);

		CppRecord rec = new() {
			Namespace = ns,
			Name = name,
			QualifiedName = qualified,
			SizeBytes = sizeBytes,
			AlignBytes = alignBytes,
			IsStructKeyword = cursor.Kind == CXCursorKind.StructDecl,
			IsAbstract = LibClang.clang_CXXRecord_isAbstract(cursor) != 0,
			IsAnonymous = isAnon,
		};

		bool hasAnyCtor = false;
		bool hasPublicDefaultCtor = false;
		bool hasReflectMacro = false;
		List<CppField> rawFields = new();
		HashSet<string> seenMethodSigs = new(StringComparer.Ordinal);

		LibClang.clang_visitChildren(cursor, (child, _, _) => {
			CX_CXXAccessSpecifier access = LibClang.clang_getCXXAccessSpecifier(child);
			bool isPublic = access == CX_CXXAccessSpecifier.Public || access == CX_CXXAccessSpecifier.Invalid;

			switch (child.Kind) {
			case CXCursorKind.CXXBaseSpecifier: {
				CXType baseType = LibClang.clang_getCanonicalType(LibClang.clang_getCursorType(child));
				CXCursor baseDecl = LibClang.clang_getTypeDeclaration(baseType);
				string baseQual = GetQualifiedCursorName(baseDecl);
				if (!string.IsNullOrEmpty(baseQual)) {
					rec.BaseQualifiedNames.Add(baseQual);
				}
				return CXChildVisitResult.Continue;
			}
			case CXCursorKind.StructDecl:
			case CXCursorKind.ClassDecl:
			case CXCursorKind.UnionDecl:
				if (LibClang.clang_isCursorDefinition(child) != 0 && isPublic) {
					if (LibClang.clang_Cursor_isAnonymous(child) != 0) {
						CollectAnonymousFields(ctx, child, recordType, ns, rec, fromBindNamespace, rawFields);
					} else if (child.Kind != CXCursorKind.UnionDecl) {
						CppRecord? nested = ScanRecord(ctx, child, ns, rec, fromBindNamespace);
						if (nested != null) {
							rec.NestedRecords.Add(nested);
						}
					}
				}
				return CXChildVisitResult.Continue;
			case CXCursorKind.EnumDecl:
				if (LibClang.clang_isCursorDefinition(child) != 0 && isPublic) {
					ScanEnum(ctx, child, ns, rec);
				}
				return CXChildVisitResult.Continue;
			case CXCursorKind.FieldDecl: {
				if (HasIgnoreAnnotation(child)) {
					return CXChildVisitResult.Continue;
				}
				string fName = LibClang.GetSpelling(child);
				if (string.IsNullOrEmpty(fName)) {
					return CXChildVisitResult.Continue;
				}
				CXType fType = LibClang.clang_getCursorType(child);
				CppTypeRef mapped = MapType(ctx, fType, ns, rec, fromBindNamespace);
				if (LibClang.clang_Cursor_isBitField(child) != 0) {
					mapped = new CppTypeRef { Kind = CppTypeKind.Unsupported, CppSpelling = LibClang.GetTypeSpelling(fType) };
				}
				long offset = LibClang.GetOffsetOfBytes(recordType, fName);
				CppField field = new() {
					Name = fName,
					Type = mapped,
					OffsetBytes = offset,
					IsPublic = isPublic,
				};
				rawFields.Add(field);
				if (isPublic) {
					rec.DirectFields.Add(field);
				}
				return CXChildVisitResult.Continue;
			}
			case CXCursorKind.Constructor: {
				hasAnyCtor = true;
				bool deleted = LibClang.clang_CXXMethod_isDeleted(child) != 0;
				int numArgs = LibClang.clang_Cursor_getNumArguments(child);
				if (deleted && numArgs == 1) {
					rec.HasDeletedCopyOrMove = true;
					return CXChildVisitResult.Continue;
				}
				if (!deleted && isPublic && !HasIgnoreAnnotation(child)) {
					CppFunction ctor = BuildFunction(ctx, child, ns, rec.Name, rec.QualifiedName, fromBindNamespace);
					bool allOptional = ctor.Params.All(static p => p.HasDefault);
					if (allOptional) {
						hasPublicDefaultCtor = true;
					}
					if (!isAnon) {
						rec.Constructors.Add(ctor);
					}
				}
				return CXChildVisitResult.Continue;
			}
			case CXCursorKind.Destructor:
				if (LibClang.clang_CXXMethod_isVirtual(child) != 0) {
					rec.HasVirtuals = true;
				}
				if (LibClang.clang_CXXMethod_isDeleted(child) != 0 || !isPublic) {
					rec.HasPublicDestructor = false;
				}
				return CXChildVisitResult.Continue;
			case CXCursorKind.CXXMethod: {
				if (LibClang.clang_CXXMethod_isVirtual(child) != 0 || LibClang.clang_CXXMethod_isPureVirtual(child) != 0) {
					rec.HasVirtuals = true;
				}
				string mName = LibClang.GetSpelling(child);
				if (mName == "GetReflectType" || mName == "_slt_reflect_dummy") {
					hasReflectMacro = true;
				}
				if (!isPublic || LibClang.clang_CXXMethod_isDeleted(child) != 0 || HasIgnoreAnnotation(child)) {
					return CXChildVisitResult.Continue;
				}
				if (ExposureFilter.IsExcludedFunctionName(mName, fromBindNamespace)) {
					return CXChildVisitResult.Continue;
				}
				if (isAnon) {
					return CXChildVisitResult.Continue;
				}
				CppFunction method = BuildFunction(ctx, child, ns, mName, rec.QualifiedName + "::" + mName, fromBindNamespace);
				method.IsStatic = LibClang.clang_CXXMethod_isStatic(child) != 0;
				method.IsConst = LibClang.clang_CXXMethod_isConst(child) != 0;
				string sigKey = $"{(method.IsStatic ? "S" : "I")}:{method.Name}({string.Join(",", method.Params.Select(static p => p.Type.CppSpelling))})";
				if (seenMethodSigs.Add(sigKey)) {
					rec.Methods.Add(method);
				}
				return CXChildVisitResult.Continue;
			}
			}
			return CXChildVisitResult.Continue;
		}, 0);

		rec.HasPublicDefaultCtor = !hasAnyCtor || hasPublicDefaultCtor;
		if (hasReflectMacro) {
			// Reflected Shalltear components are already bound by CSharpNativeTypes.
			return null;
		}
		if (!isAnon) {
			ctx.Model.Records[qualified] = rec;
			ctx.RecordTypes[qualified] = recordType;
			ctx.RecordCursors[qualified] = cursor;
			ctx.AllRawFields[qualified] = rawFields;
		} else {
			rec.FlattenedFields.AddRange(rawFields);
		}
		return rec;
	}

	static void CollectAnonymousFields(
		ScanContext ctx,
		CXCursor anonCursor,
		CXType enclosingRecordType,
		string ns,
		CppRecord enclosingRec,
		bool fromBindNamespace,
		List<CppField> rawFields) {
		LibClang.clang_visitChildren(anonCursor, (child, _, _) => {
			CX_CXXAccessSpecifier access = LibClang.clang_getCXXAccessSpecifier(child);
			bool isPublic = access == CX_CXXAccessSpecifier.Public || access == CX_CXXAccessSpecifier.Invalid;
			if (!isPublic || HasIgnoreAnnotation(child)) {
				return CXChildVisitResult.Continue;
			}
			if (child.Kind is CXCursorKind.StructDecl or CXCursorKind.UnionDecl &&
				LibClang.clang_isCursorDefinition(child) != 0 &&
				LibClang.clang_Cursor_isAnonymous(child) != 0) {
				CollectAnonymousFields(ctx, child, enclosingRecordType, ns, enclosingRec, fromBindNamespace, rawFields);
				return CXChildVisitResult.Continue;
			}
			if (child.Kind == CXCursorKind.FieldDecl) {
				string fName = LibClang.GetSpelling(child);
				if (string.IsNullOrEmpty(fName)) {
					return CXChildVisitResult.Continue;
				}
				long offset = LibClang.GetOffsetOfBytes(enclosingRecordType, fName);
				if (offset < 0) {
					return CXChildVisitResult.Continue;
				}
				CXType fType = LibClang.clang_getCursorType(child);
				CppTypeRef mapped = MapType(ctx, fType, ns, enclosingRec, fromBindNamespace);
				if (LibClang.clang_Cursor_isBitField(child) != 0) {
					mapped = new CppTypeRef { Kind = CppTypeKind.Unsupported, CppSpelling = LibClang.GetTypeSpelling(fType) };
				}
				CppField field = new() {
					Name = fName,
					Type = mapped,
					OffsetBytes = offset,
					IsPublic = true,
				};
				rawFields.Add(field);
				enclosingRec.DirectFields.Add(field);
			}
			return CXChildVisitResult.Continue;
		}, 0);
	}

	static void ScanFreeFunction(ScanContext ctx, CXCursor cursor, string ns, bool fromBindNamespace) {
		string name = LibClang.GetSpelling(cursor);
		if (ExposureFilter.IsExcludedFunctionName(name, fromBindNamespace)) {
			return;
		}
		if (LibClang.clang_Cursor_getStorageClass(cursor) == CXStorageClass.Static && LibClang.clang_isCursorDefinition(cursor) == 0) {
			return;
		}
		string qualified = string.IsNullOrEmpty(ns) ? name : ns + "::" + name;
		CppFunction fn = BuildFunction(ctx, cursor, ns, name, qualified, fromBindNamespace);
		fn.IsStatic = true;
		string key = $"{qualified}({string.Join(",", fn.Params.Select(static p => p.Type.CppSpelling))})";
		if (ctx.SeenFunctions.Add(key)) {
			ctx.Model.FreeFunctions.Add(fn);
		}
	}

	static void ScanGlobalVar(ScanContext ctx, CXCursor cursor, string ns, bool fromBindNamespace) {
		CXStorageClass storage = LibClang.clang_Cursor_getStorageClass(cursor);
		if (storage != CXStorageClass.Extern) {
			return;
		}
		string name = LibClang.GetSpelling(cursor);
		if (string.IsNullOrEmpty(name) || (fromBindNamespace && name.StartsWith("_", StringComparison.Ordinal))) {
			return;
		}
		string qualified = string.IsNullOrEmpty(ns) ? name : ns + "::" + name;
		if (!ctx.SeenGlobals.Add(qualified)) {
			return;
		}
		CXType varType = LibClang.clang_getCursorType(cursor);
		CppTypeRef mapped = MapType(ctx, varType, ns, null, fromBindNamespace);
		ctx.Model.Globals.Add(new CppGlobalVar {
			Namespace = ns,
			Name = name,
			QualifiedName = qualified,
			Type = mapped,
			IsConst = LibClang.clang_isConstQualifiedType(varType) != 0,
		});
	}

	static CppFunction BuildFunction(ScanContext ctx, CXCursor cursor, string ns, string name, string qualified, bool fromBindNamespace) {
		CppFunction fn = new() {
			Namespace = ns,
			Name = name,
			QualifiedName = qualified,
			ReturnType = cursor.Kind == CXCursorKind.Constructor
				? new CppTypeRef { Kind = CppTypeKind.Void }
				: MapType(ctx, LibClang.clang_getCursorResultType(cursor), ns, null, fromBindNamespace),
			IsVariadic = LibClang.clang_isFunctionTypeVariadic(LibClang.clang_getCursorType(cursor)) != 0,
		};
		int numArgs = LibClang.clang_Cursor_getNumArguments(cursor);
		for (uint i = 0; i < (uint)Math.Max(numArgs, 0); ++i) {
			CXCursor parm = LibClang.clang_Cursor_getArgument(cursor, i);
			string pName = LibClang.GetSpelling(parm);
			if (string.IsNullOrEmpty(pName)) {
				pName = "arg" + i;
			}
			CXType pType = LibClang.clang_getCursorType(parm);
			fn.Params.Add(new CppParam {
				Name = pName,
				Type = MapType(ctx, pType, ns, null, fromBindNamespace),
				HasDefault = ParameterHasDefaultValue(ctx.Tu, parm),
			});
		}
		return fn;
	}

	static bool ParameterHasDefaultValue(nint tu, CXCursor parm) {
		CXSourceRange range = LibClang.clang_getCursorExtent(parm);
		LibClang.clang_tokenize(tu, range, out CXToken* tokens, out uint numTokens);
		if (tokens == null || numTokens == 0) {
			return false;
		}
		try {
			int depth = 0;
			for (uint i = 0; i < numTokens; ++i) {
				string spelling = LibClang.ToStringAndDispose(LibClang.clang_getTokenSpelling(tu, tokens[i]));
				if (spelling is "(" or "<" or "[" or "{") {
					++depth;
				} else if (spelling is ")" or ">" or "]" or "}") {
					if (depth > 0) {
						--depth;
					}
				} else if (depth == 0 && spelling == "=") {
					return true;
				}
			}
			return false;
		} finally {
			LibClang.clang_disposeTokens(tu, tokens, numTokens);
		}
	}

	static string GetQualifiedCursorName(CXCursor cursor) {
		List<string> parts = new();
		CXCursor cur = cursor;
		while (cur.Kind != CXCursorKind.TranslationUnit && cur.Kind != 0) {
			if (cur.Kind is CXCursorKind.Namespace or CXCursorKind.StructDecl or CXCursorKind.ClassDecl or CXCursorKind.EnumDecl) {
				string s = LibClang.GetSpelling(cur);
				if (!string.IsNullOrEmpty(s)) {
					parts.Add(s);
				}
			}
			CXCursor parent = LibClang.clang_getCursorSemanticParent(cur);
			if (parent.Kind == cur.Kind && parent.Data0 == cur.Data0 && parent.Data1 == cur.Data1) {
				break;
			}
			cur = parent;
		}
		parts.Reverse();
		return string.Join("::", parts);
	}

	static CppTypeRef MapType(ScanContext ctx, CXType rawType, string ns, CppRecord? enclosingRecord, bool fromBindNamespace) {
		bool isConst = LibClang.clang_isConstQualifiedType(rawType) != 0;
		string rawSpelling = LibClang.GetTypeSpelling(rawType);
		CXType canon = LibClang.clang_getCanonicalType(rawType);
		string canonSpelling = LibClang.GetTypeSpelling(canon);
		isConst |= LibClang.clang_isConstQualifiedType(canon) != 0;

		switch (canon.Kind) {
		case CXTypeKind.Void:
			return new CppTypeRef { Kind = CppTypeKind.Void, CppSpelling = rawSpelling };
		case CXTypeKind.Bool:
			return new CppTypeRef { Kind = CppTypeKind.Bool, CppSpelling = rawSpelling, IsConst = isConst };
		case CXTypeKind.SChar:
		case CXTypeKind.Char_S:
			return new CppTypeRef { Kind = CppTypeKind.Int8, CppSpelling = rawSpelling, IsConst = isConst };
		case CXTypeKind.UChar:
		case CXTypeKind.Char_U:
			return new CppTypeRef { Kind = CppTypeKind.UInt8, CppSpelling = rawSpelling, IsConst = isConst };
		case CXTypeKind.Short:
			return new CppTypeRef { Kind = CppTypeKind.Int16, CppSpelling = rawSpelling, IsConst = isConst };
		case CXTypeKind.UShort:
			return new CppTypeRef { Kind = CppTypeKind.UInt16, CppSpelling = rawSpelling, IsConst = isConst };
		case CXTypeKind.Int:
			return new CppTypeRef { Kind = CppTypeKind.Int32, CppSpelling = rawSpelling, IsConst = isConst };
		case CXTypeKind.UInt:
			return new CppTypeRef { Kind = CppTypeKind.UInt32, CppSpelling = rawSpelling, IsConst = isConst };
		case CXTypeKind.Long:
			return new CppTypeRef { Kind = LibClang.clang_Type_getSizeOf(canon) == 8 ? CppTypeKind.Int64 : CppTypeKind.Int32, CppSpelling = rawSpelling, IsConst = isConst };
		case CXTypeKind.ULong:
			return new CppTypeRef { Kind = LibClang.clang_Type_getSizeOf(canon) == 8 ? CppTypeKind.UInt64 : CppTypeKind.UInt32, CppSpelling = rawSpelling, IsConst = isConst };
		case CXTypeKind.LongLong:
			return new CppTypeRef { Kind = CppTypeKind.Int64, CppSpelling = rawSpelling, IsConst = isConst };
		case CXTypeKind.ULongLong:
			return new CppTypeRef { Kind = CppTypeKind.UInt64, CppSpelling = rawSpelling, IsConst = isConst };
		case CXTypeKind.Float:
			return new CppTypeRef { Kind = CppTypeKind.Float, CppSpelling = rawSpelling, IsConst = isConst };
		case CXTypeKind.Double:
			return new CppTypeRef { Kind = CppTypeKind.Double, CppSpelling = rawSpelling, IsConst = isConst };
		case CXTypeKind.Pointer: {
			CXType pointee = LibClang.clang_getPointeeType(canon);
			CXType pointeeCanon = LibClang.clang_getCanonicalType(pointee);
			if (pointeeCanon.Kind == CXTypeKind.Void) {
				return new CppTypeRef { Kind = CppTypeKind.VoidPtr, CppSpelling = rawSpelling, IsConst = LibClang.clang_isConstQualifiedType(pointee) != 0 };
			}
			if ((pointeeCanon.Kind is CXTypeKind.Char_S or CXTypeKind.Char_U) && LibClang.clang_isConstQualifiedType(pointee) != 0) {
				return new CppTypeRef { Kind = CppTypeKind.CString, CppSpelling = rawSpelling, IsConst = true };
			}
			return new CppTypeRef {
				Kind = CppTypeKind.Pointer,
				CppSpelling = rawSpelling,
				IsConst = LibClang.clang_isConstQualifiedType(pointee) != 0,
				Element = MapType(ctx, pointee, ns, enclosingRecord, fromBindNamespace),
			};
		}
		case CXTypeKind.LValueReference: {
			CXType pointee = LibClang.clang_getPointeeType(canon);
			bool refConst = LibClang.clang_isConstQualifiedType(pointee) != 0;
			CppTypeRef inner = MapType(ctx, pointee, ns, enclosingRecord, fromBindNamespace);
			if (inner.Kind is CppTypeKind.StdString or CppTypeKind.FilesystemPath or CppTypeKind.Span or CppTypeKind.VectorOfPtr or CppTypeKind.StdFunction) {
				inner.IsConst = refConst;
				return inner;
			}
			return new CppTypeRef {
				Kind = CppTypeKind.Reference,
				CppSpelling = rawSpelling,
				IsConst = refConst,
				Element = inner,
			};
		}
		case CXTypeKind.ConstantArray: {
			long len = LibClang.clang_getArraySize(canon);
			CXType elem = LibClang.clang_getArrayElementType(canon);
			return new CppTypeRef {
				Kind = CppTypeKind.FixedArray,
				CppSpelling = rawSpelling,
				ArrayLength = (int)Math.Clamp(len, 0, int.MaxValue),
				Element = MapType(ctx, elem, ns, enclosingRecord, fromBindNamespace),
			};
		}
		case CXTypeKind.Enum: {
			CXCursor decl = LibClang.clang_getTypeDeclaration(canon);
			string qual = GetQualifiedCursorName(decl);
			return new CppTypeRef { Kind = CppTypeKind.Enum, CppSpelling = rawSpelling, QualifiedName = qual, IsConst = isConst };
		}
		case CXTypeKind.Record: {
			CXCursor decl = LibClang.clang_getTypeDeclaration(canon);
			if (LibClang.clang_Cursor_isAnonymous(decl) != 0 || LibClang.clang_Cursor_isAnonymousRecordDecl(decl) != 0) {
				CppRecord? anon = ScanRecord(ctx, decl, ns, enclosingRecord, fromBindNamespace);
				return new CppTypeRef {
					Kind = anon != null ? CppTypeKind.AnonymousStruct : CppTypeKind.Unsupported,
					CppSpelling = rawSpelling,
					AnonymousRecord = anon,
				};
			}

			string normalized = StripConstAndInlineNs(canonSpelling);
			if (normalized is "Shalltear::Vec2" or "Shalltear::Math::Vec2") {
				return new CppTypeRef { Kind = CppTypeKind.MathVec2, CppSpelling = rawSpelling, QualifiedName = "Shalltear::Vec2", IsConst = isConst };
			}
			if (normalized is "Shalltear::Vec3" or "Shalltear::Math::Vec3") {
				return new CppTypeRef { Kind = CppTypeKind.MathVec3, CppSpelling = rawSpelling, QualifiedName = "Shalltear::Vec3", IsConst = isConst };
			}
			if (normalized is "Shalltear::Vec4" or "Shalltear::Math::Vec4") {
				return new CppTypeRef { Kind = CppTypeKind.MathVec4, CppSpelling = rawSpelling, QualifiedName = "Shalltear::Vec4", IsConst = isConst };
			}
			if (normalized is "Shalltear::Quat" or "Shalltear::Math::Quat") {
				return new CppTypeRef { Kind = CppTypeKind.MathQuat, CppSpelling = rawSpelling, QualifiedName = "Shalltear::Quat", IsConst = isConst };
			}
			if (normalized is "Shalltear::Color" or "Shalltear::Math::Color") {
				return new CppTypeRef { Kind = CppTypeKind.MathColor, CppSpelling = rawSpelling, QualifiedName = "Shalltear::Color", IsConst = isConst };
			}
			if (normalized is "Shalltear::Vec2I" or "Shalltear::Math::Vec2I") {
				return new CppTypeRef { Kind = CppTypeKind.MathVec2I, CppSpelling = rawSpelling, QualifiedName = "Shalltear::Vec2I", IsConst = isConst };
			}
			if (normalized == "Shalltear::str" ||
				normalized.StartsWith("std::basic_string<char", StringComparison.Ordinal) ||
				normalized.StartsWith("std::basic_string_view<char", StringComparison.Ordinal)) {
				return new CppTypeRef { Kind = CppTypeKind.StdString, CppSpelling = rawSpelling, IsConst = isConst };
			}
			if (normalized is "std::filesystem::path" or "std::filesystem::__cxx11::path") {
				return new CppTypeRef { Kind = CppTypeKind.FilesystemPath, CppSpelling = rawSpelling, IsConst = isConst };
			}
			if (normalized.StartsWith("std::span<", StringComparison.Ordinal)) {
				if (LibClang.clang_Type_getNumTemplateArguments(canon) >= 1) {
					CXType elem = LibClang.clang_Type_getTemplateArgumentAsType(canon, 0);
					return new CppTypeRef {
						Kind = CppTypeKind.Span,
						CppSpelling = rawSpelling,
						IsConst = LibClang.clang_isConstQualifiedType(elem) != 0,
						Element = MapType(ctx, elem, ns, enclosingRecord, fromBindNamespace),
					};
				}
			}
			if (normalized.StartsWith("std::vector<", StringComparison.Ordinal) ||
				normalized.StartsWith("std::unordered_set<", StringComparison.Ordinal) ||
				normalized.StartsWith("std::set<", StringComparison.Ordinal)) {
				if (LibClang.clang_Type_getNumTemplateArguments(canon) >= 1) {
					CXType elem = LibClang.clang_getCanonicalType(LibClang.clang_Type_getTemplateArgumentAsType(canon, 0));
					if (elem.Kind == CXTypeKind.Pointer) {
						CXType pointee = LibClang.clang_getPointeeType(elem);
						return new CppTypeRef {
							Kind = CppTypeKind.VectorOfPtr,
							CppSpelling = rawSpelling,
							Element = MapType(ctx, pointee, ns, enclosingRecord, fromBindNamespace),
						};
					}
				}
			}
			if (normalized.StartsWith("std::function<", StringComparison.Ordinal)) {
				if (LibClang.clang_Type_getNumTemplateArguments(canon) >= 1) {
					CXType sig = LibClang.clang_getCanonicalType(LibClang.clang_Type_getTemplateArgumentAsType(canon, 0));
					if (sig.Kind == CXTypeKind.FunctionProto && LibClang.clang_getResultType(sig).Kind == CXTypeKind.Void) {
						CppTypeRef fnRef = new() { Kind = CppTypeKind.StdFunction, CppSpelling = rawSpelling };
						int argCount = LibClang.clang_getNumArgTypes(sig);
						for (uint i = 0; i < (uint)Math.Max(argCount, 0); ++i) {
							fnRef.CallbackParams.Add(MapType(ctx, LibClang.clang_getArgType(sig, i), ns, enclosingRecord, fromBindNamespace));
						}
						return fnRef;
					}
				}
			}

			string qual = GetQualifiedCursorName(decl);
			if (!string.IsNullOrEmpty(qual)) {
				return new CppTypeRef { Kind = CppTypeKind.Record, CppSpelling = rawSpelling, QualifiedName = qual, IsConst = isConst };
			}
			return new CppTypeRef { Kind = CppTypeKind.Unsupported, CppSpelling = rawSpelling };
		}
		default:
			return new CppTypeRef { Kind = CppTypeKind.Unsupported, CppSpelling = rawSpelling };
		}
	}

	static string StripConstAndInlineNs(string spelling) {
		string s = spelling.Trim();
		if (s.StartsWith("const ", StringComparison.Ordinal)) {
			s = s.Substring(6).Trim();
		}
		if (s.StartsWith("struct ", StringComparison.Ordinal)) {
			s = s.Substring(7).Trim();
		}
		if (s.StartsWith("class ", StringComparison.Ordinal)) {
			s = s.Substring(6).Trim();
		}
		s = s.Replace("std::__1::", "std::", StringComparison.Ordinal)
			 .Replace("std::__cxx11::", "std::", StringComparison.Ordinal);
		return s;
	}

	static void ClassifyValueStructs(ScanContext ctx) {
		// Optimistically mark eligible records as ValueStruct, then iterate to fixed point.
		foreach (CppRecord rec in ctx.Model.Records.Values) {
			rec.IsValueStruct = rec.SizeBytes > 0 && !rec.IsAbstract && !rec.HasVirtuals && !rec.HasDeletedCopyOrMove && rec.HasPublicDefaultCtor;
		}
		bool changed = true;
		while (changed) {
			changed = false;
			foreach (CppRecord rec in ctx.Model.Records.Values) {
				if (!rec.IsValueStruct) {
					continue;
				}
				if (!CheckRecordValueSafe(ctx, rec)) {
					rec.IsValueStruct = false;
					changed = true;
				}
			}
		}
	}

	static bool CheckRecordValueSafe(ScanContext ctx, CppRecord rec) {
		foreach (string baseQual in rec.BaseQualifiedNames) {
			if (!ctx.Model.Records.TryGetValue(baseQual, out CppRecord? baseRec) || !baseRec.IsValueStruct) {
				return false;
			}
		}
		List<CppField> fields = rec.IsAnonymous ? rec.FlattenedFields : ctx.AllRawFields[rec.QualifiedName];
		foreach (CppField f in fields) {
			if (f.OffsetBytes < 0 || !IsFieldBlittable(ctx, f.Type)) {
				return false;
			}
		}
		return true;
	}

	static bool IsFieldBlittable(ScanContext ctx, CppTypeRef t) {
		if (t.IsScalar || t.IsMath || t.Kind == CppTypeKind.VoidPtr) {
			return true;
		}
		if (t.Kind == CppTypeKind.Enum) {
			return ctx.Model.Enums.ContainsKey(t.QualifiedName);
		}
		if (t.Kind == CppTypeKind.Pointer) {
			return true; // stored as nint inside explicit-layout struct
		}
		if (t.Kind == CppTypeKind.Record) {
			return ctx.Model.Records.TryGetValue(t.QualifiedName, out CppRecord? nested) && nested.IsValueStruct;
		}
		if (t.Kind == CppTypeKind.FixedArray) {
			return t.ArrayLength > 0 && t.Element != null && t.Element.IsScalar;
		}
		if (t.Kind == CppTypeKind.AnonymousStruct && t.AnonymousRecord != null) {
			return CheckRecordValueSafe(ctx, t.AnonymousRecord);
		}
		return false;
	}

	static void FlattenRecordFields(ScanContext ctx) {
		foreach (CppRecord rec in ctx.Model.Records.Values) {
			if (!rec.IsValueStruct) {
				rec.FlattenedFields.AddRange(rec.DirectFields);
				continue;
			}
			CXType recType = ctx.RecordTypes[rec.QualifiedName];
			HashSet<string> seen = new(StringComparer.Ordinal);
			CollectFlattened(ctx, rec, recType, seen, rec.FlattenedFields);
		}
	}

	static void CollectFlattened(ScanContext ctx, CppRecord cur, CXType derivedType, HashSet<string> seen, List<CppField> output) {
		foreach (string baseQual in cur.BaseQualifiedNames) {
			if (ctx.Model.Records.TryGetValue(baseQual, out CppRecord? baseRec)) {
				CollectFlattened(ctx, baseRec, derivedType, seen, output);
			}
		}
		foreach (CppField f in cur.DirectFields) {
			if (!seen.Add(f.Name)) {
				continue;
			}
			long offset = LibClang.GetOffsetOfBytes(derivedType, f.Name);
			output.Add(new CppField {
				Name = f.Name,
				Type = f.Type,
				OffsetBytes = offset >= 0 ? offset : f.OffsetBytes,
				IsPublic = f.IsPublic,
			});
		}
	}
}
