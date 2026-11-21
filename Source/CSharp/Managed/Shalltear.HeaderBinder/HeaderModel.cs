using System;
using System.Collections.Generic;

namespace Shalltear.HeaderBinder;

internal enum CppTypeKind {
	Unsupported = 0,
	Void,
	Bool,
	Int8,
	UInt8,
	Int16,
	UInt16,
	Int32,
	UInt32,
	Int64,
	UInt64,
	Float,
	Double,
	MathVec2,
	MathVec3,
	MathVec4,
	MathQuat,
	MathColor,
	MathVec2I,
	Enum,
	Record,
	CString,
	StdString,
	FilesystemPath,
	VoidPtr,
	Pointer,
	Reference,
	Span,
	VectorOfPtr,
	StdFunction,
	FixedArray,
	AnonymousStruct,
}

internal sealed class CppTypeRef {
	public CppTypeKind Kind;
	public string CppSpelling = "";
	public string QualifiedName = "";
	public bool IsConst;
	public CppTypeRef? Element;
	public int ArrayLength;
	public List<CppTypeRef> CallbackParams = new();
	public CppRecord? AnonymousRecord;

	public bool IsScalar => Kind is CppTypeKind.Bool or CppTypeKind.Int8 or CppTypeKind.UInt8 or
		CppTypeKind.Int16 or CppTypeKind.UInt16 or CppTypeKind.Int32 or CppTypeKind.UInt32 or
		CppTypeKind.Int64 or CppTypeKind.UInt64 or CppTypeKind.Float or CppTypeKind.Double;

	public bool IsMath => Kind is CppTypeKind.MathVec2 or CppTypeKind.MathVec3 or CppTypeKind.MathVec4 or
		CppTypeKind.MathQuat or CppTypeKind.MathColor or CppTypeKind.MathVec2I;
}

internal sealed class CppEnum {
	public string Namespace = "";
	public string Name = "";
	public string QualifiedName = "";
	public string UnderlyingCSharp = "int";
	public string UnderlyingCpp = "int32_t";
	public bool IsUnsigned;
	public bool IsAnonymous;
	public List<(string Name, long Value, ulong UnsignedValue)> Constants = new();
}

internal sealed class CppField {
	public string Name = "";
	public CppTypeRef Type = new();
	public long OffsetBytes = -1;
	public bool IsPublic = true;
}

internal sealed class CppParam {
	public string Name = "";
	public CppTypeRef Type = new();
	public bool HasDefault;
}

internal sealed class CppFunction {
	public string Namespace = "";
	public string Name = "";
	public string QualifiedName = "";
	public CppTypeRef ReturnType = new();
	public List<CppParam> Params = new();
	public bool IsStatic;
	public bool IsConst;
	public bool IsVariadic;
}

internal sealed class CppGlobalVar {
	public string Namespace = "";
	public string Name = "";
	public string QualifiedName = "";
	public CppTypeRef Type = new();
	public bool IsConst;
}

internal sealed class CppRecord {
	public string Namespace = "";
	public string Name = "";
	public string QualifiedName = "";
	public long SizeBytes;
	public long AlignBytes;
	public bool IsStructKeyword;
	public bool IsValueStruct;
	public bool IsAbstract;
	public bool HasVirtuals;
	public bool HasPublicDefaultCtor;
	public bool HasDeletedCopyOrMove;
	public bool HasPublicDestructor = true;
	public bool IsAnonymous;
	public List<string> BaseQualifiedNames = new();
	public List<CppField> DirectFields = new();
	public List<CppField> FlattenedFields = new();
	public List<CppFunction> Methods = new();
	public List<CppFunction> Constructors = new();
	public List<CppRecord> NestedRecords = new();
	public List<CppEnum> NestedEnums = new();
}

internal sealed class HeaderModel {
	public string ModuleName = "";
	public List<string> BoundHeaders = new();
	public Dictionary<string, CppEnum> Enums = new(StringComparer.Ordinal);
	public Dictionary<string, CppRecord> Records = new(StringComparer.Ordinal);
	public List<CppFunction> FreeFunctions = new();
	public List<CppGlobalVar> Globals = new();
}
