using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

namespace Shalltear.HeaderBinder;

internal enum CXErrorCode : int {
	Success = 0,
	Failure = 1,
	Crashed = 2,
	InvalidArguments = 3,
	ASTReadError = 4,
}

internal enum CXDiagnosticSeverity : uint {
	Ignored = 0,
	Note = 1,
	Warning = 2,
	Error = 3,
	Fatal = 4,
}

internal enum CXCursorKind : uint {
	UnexposedDecl = 1,
	StructDecl = 2,
	UnionDecl = 3,
	ClassDecl = 4,
	EnumDecl = 5,
	FieldDecl = 6,
	EnumConstantDecl = 7,
	FunctionDecl = 8,
	VarDecl = 9,
	ParmDecl = 10,
	TypedefDecl = 20,
	CXXMethod = 21,
	Namespace = 22,
	LinkageSpec = 23,
	Constructor = 24,
	Destructor = 25,
	ConversionFunction = 26,
	FunctionTemplate = 30,
	ClassTemplate = 31,
	ClassTemplatePartialSpecialization = 32,
	TypeAliasDecl = 36,
	CXXAccessSpecDecl = 39,
	CXXBaseSpecifier = 44,
	TranslationUnit = 350,
	AnnotateAttr = 406,
}

internal enum CXTypeKind : uint {
	Invalid = 0,
	Unexposed = 1,
	Void = 2,
	Bool = 3,
	Char_U = 4,
	UChar = 5,
	Char16 = 6,
	Char32 = 7,
	UShort = 8,
	UInt = 9,
	ULong = 10,
	ULongLong = 11,
	UInt128 = 12,
	Char_S = 13,
	SChar = 14,
	WChar = 15,
	Short = 16,
	Int = 17,
	Long = 18,
	LongLong = 19,
	Int128 = 20,
	Float = 21,
	Double = 22,
	LongDouble = 23,
	NullPtr = 24,
	Pointer = 101,
	LValueReference = 103,
	RValueReference = 104,
	Record = 105,
	Enum = 106,
	Typedef = 107,
	FunctionNoProto = 110,
	FunctionProto = 111,
	ConstantArray = 112,
	IncompleteArray = 114,
	Auto = 118,
	Elaborated = 119,
}

internal enum CX_CXXAccessSpecifier : uint {
	Invalid = 0,
	Public = 1,
	Protected = 2,
	Private = 3,
}

internal enum CXStorageClass : int {
	Invalid = 0,
	None = 1,
	Extern = 2,
	Static = 3,
	PrivateExtern = 4,
}

internal enum CXChildVisitResult : int {
	Break = 0,
	Continue = 1,
	Recurse = 2,
}

internal enum CXTokenKind : uint {
	Punctuation = 0,
	Keyword = 1,
	Identifier = 2,
	Literal = 3,
	Comment = 4,
}

[StructLayout(LayoutKind.Sequential)]
internal struct CXString {
	public nint Data;
	public uint PrivateFlags;
}

[StructLayout(LayoutKind.Sequential)]
internal struct CXCursor {
	public CXCursorKind Kind;
	public int XData;
	public nint Data0;
	public nint Data1;
	public nint Data2;
}

[StructLayout(LayoutKind.Sequential)]
internal struct CXType {
	public CXTypeKind Kind;
	public nint Data0;
	public nint Data1;
}

[StructLayout(LayoutKind.Sequential)]
internal struct CXSourceLocation {
	public nint PtrData0;
	public nint PtrData1;
	public uint IntData;
}

[StructLayout(LayoutKind.Sequential)]
internal struct CXSourceRange {
	public nint PtrData0;
	public nint PtrData1;
	public uint BeginIntData;
	public uint EndIntData;
}

[StructLayout(LayoutKind.Sequential)]
internal struct CXUnsavedFile {
	public nint Filename;
	public nint Contents;
	public uint Length;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct CXToken {
	public fixed uint IntData[4];
	public nint PtrData;
}

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal delegate CXChildVisitResult CXCursorVisitor(CXCursor cursor, CXCursor parent, nint clientData);

internal static unsafe class LibClang {
	const string LIB_NAME = "libclang";

	static string? _searchDir;
	static bool _resolverInstalled;

	public static void ConfigureSearchDirectory(string? libclangDir) {
		_searchDir = libclangDir;
		if (_resolverInstalled) {
			return;
		}
		_resolverInstalled = true;
		NativeLibrary.SetDllImportResolver(typeof(LibClang).Assembly, ResolveLibrary);
	}

	static nint ResolveLibrary(string libraryName, Assembly assembly, DllImportSearchPath? searchPath) {
		if (libraryName != LIB_NAME) {
			return 0;
		}
		string[] fileNames = OperatingSystem.IsWindows()
			? ["libclang.dll"]
			: ["libclang.so", "libclang.so.23", "libclang.so.22", "libclang.so.21", "libclang.so.20", "libclang.so.19", "libclang.so.18"];

		if (!string.IsNullOrWhiteSpace(_searchDir)) {
			string[] probeDirs = [_searchDir!, Path.Combine(_searchDir!, "bin"), Path.Combine(_searchDir!, "lib")];
			foreach (string dir in probeDirs) {
				foreach (string file in fileNames) {
					string candidate = Path.Combine(dir, file);
					if (File.Exists(candidate) && NativeLibrary.TryLoad(candidate, out nint handle)) {
						return handle;
					}
				}
			}
		}
		foreach (string file in fileNames) {
			if (NativeLibrary.TryLoad(file, assembly, searchPath, out nint handle)) {
				return handle;
			}
		}
		return 0;
	}

	public static string ToStringAndDispose(CXString str) {
		try {
			nint ptr = clang_getCString(str);
			return ptr == 0 ? string.Empty : Marshal.PtrToStringUTF8(ptr) ?? string.Empty;
		} finally {
			clang_disposeString(str);
		}
	}

	public static string GetSpelling(CXCursor cursor) => ToStringAndDispose(clang_getCursorSpelling(cursor));
	public static string GetTypeSpelling(CXType type) => ToStringAndDispose(clang_getTypeSpelling(type));

	public static string GetCursorFilePath(CXCursor cursor) {
		CXSourceLocation loc = clang_getCursorLocation(cursor);
		clang_getSpellingLocation(loc, out nint file, out _, out _, out _);
		if (file == 0) {
			return string.Empty;
		}
		return ToStringAndDispose(clang_getFileName(file));
	}

	public static long GetOffsetOfBytes(CXType recordType, string fieldName) {
		byte[] utf8 = Encoding.UTF8.GetBytes(fieldName + "\0");
		fixed (byte* ptr = utf8) {
			long bits = clang_Type_getOffsetOf(recordType, ptr);
			return bits < 0 ? -1 : bits / 8;
		}
	}

	public const uint CXTranslationUnit_SkipFunctionBodies = 0x40;
	public const uint CXTranslationUnit_DetailedPreprocessingRecord = 0x01;

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern nint clang_createIndex(int excludeDeclarationsFromPCH, int displayDiagnostics);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern void clang_disposeIndex(nint index);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXErrorCode clang_parseTranslationUnit2(
		nint cIdx,
		byte* sourceFilename,
		nint* commandLineArgs,
		int numCommandLineArgs,
		CXUnsavedFile* unsavedFiles,
		uint numUnsavedFiles,
		uint options,
		out nint outTU);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern void clang_disposeTranslationUnit(nint tu);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXCursor clang_getTranslationUnitCursor(nint tu);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern uint clang_visitChildren(CXCursor parent, CXCursorVisitor visitor, nint clientData);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern nint clang_getCString(CXString str);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern void clang_disposeString(CXString str);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXString clang_getCursorSpelling(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXCursorKind clang_getCursorKind(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXType clang_getCursorType(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern uint clang_isFunctionTypeVariadic(CXType t);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXType clang_getCursorResultType(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern int clang_Cursor_getNumArguments(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXCursor clang_Cursor_getArgument(CXCursor cursor, uint i);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern uint clang_isCursorDefinition(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern uint clang_Cursor_isAnonymous(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern uint clang_Cursor_isAnonymousRecordDecl(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern uint clang_Cursor_isBitField(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXStorageClass clang_Cursor_getStorageClass(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CX_CXXAccessSpecifier clang_getCXXAccessSpecifier(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern uint clang_CXXMethod_isStatic(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern uint clang_CXXMethod_isConst(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern uint clang_CXXMethod_isDeleted(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern uint clang_CXXMethod_isPureVirtual(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern uint clang_CXXMethod_isVirtual(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern uint clang_CXXRecord_isAbstract(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXCursor clang_getCursorSemanticParent(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXSourceLocation clang_getCursorLocation(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXSourceRange clang_getCursorExtent(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern void clang_getSpellingLocation(CXSourceLocation location, out nint file, out uint line, out uint column, out uint offset);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXString clang_getFileName(nint file);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXType clang_getEnumDeclIntegerType(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern long clang_getEnumConstantDeclValue(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern ulong clang_getEnumConstantDeclUnsignedValue(CXCursor cursor);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXString clang_getTypeSpelling(CXType type);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXType clang_getCanonicalType(CXType type);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXCursor clang_getTypeDeclaration(CXType type);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXType clang_getPointeeType(CXType type);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXType clang_getResultType(CXType type);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern int clang_getNumArgTypes(CXType type);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXType clang_getArgType(CXType type, uint i);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXType clang_getArrayElementType(CXType type);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern long clang_getArraySize(CXType type);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern int clang_Type_getNumTemplateArguments(CXType type);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXType clang_Type_getTemplateArgumentAsType(CXType type, uint i);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern uint clang_isConstQualifiedType(CXType type);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern long clang_Type_getSizeOf(CXType type);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern long clang_Type_getAlignOf(CXType type);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern long clang_Type_getOffsetOf(CXType type, byte* fieldName);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern uint clang_getNumDiagnostics(nint tu);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern nint clang_getDiagnostic(nint tu, uint index);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXDiagnosticSeverity clang_getDiagnosticSeverity(nint diag);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXString clang_formatDiagnostic(nint diag, uint options);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern void clang_disposeDiagnostic(nint diag);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern void clang_tokenize(nint tu, CXSourceRange range, out CXToken* tokens, out uint numTokens);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXTokenKind clang_getTokenKind(CXToken token);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern CXString clang_getTokenSpelling(nint tu, CXToken token);

	[DllImport(LIB_NAME, CallingConvention = CallingConvention.Cdecl)]
	public static extern void clang_disposeTokens(nint tu, CXToken* tokens, uint numTokens);
}
