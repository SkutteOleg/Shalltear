using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Globalization;
using System.Linq;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace Shalltear.Generators;

[Generator]
public sealed class FunctionGenerator : IIncrementalGenerator {
	internal const string NATIVE_FUNCTION = "Shalltear.NativeFunctionAttribute";
	internal const string EXPORT_TO_NATIVE = "Shalltear.ExportToNativeAttribute";
	internal const string CALLABLE = "Shalltear.CallableAttribute";
	const string ENGINE_OBJECT = "Shalltear.EngineObject";
	const string INTEROP = "global::Shalltear.Interop.";

	static readonly SymbolDisplayFormat _plain =
		SymbolDisplayFormat.FullyQualifiedFormat.WithGlobalNamespaceStyle(SymbolDisplayGlobalNamespaceStyle.Omitted);

	static readonly SymbolDisplayFormat _qualified =
		SymbolDisplayFormat.FullyQualifiedFormat.AddMiscellaneousOptions(SymbolDisplayMiscellaneousOptions.IncludeNullableReferenceTypeModifier);

	static readonly DiagnosticDescriptor _unsupportedType = new(
		"SLT1005", "Type can't cross to C++", "'{0}' can't cross to C++ with type '{1}'. Use a number, bool, Vec2/Vec3/Vec4, Quat, Color, string, an enum or an engine object.",
		"Shalltear", DiagnosticSeverity.Error, true);

	static readonly DiagnosticDescriptor _wrongShape = new(
		"SLT1006", "Method can't be bound to C++", "'{0}' can't be bound to C++. {1}.", "Shalltear", DiagnosticSeverity.Error, true);

	sealed class Slot : IEquatable<Slot> {
		public string type = "";
		public string token = "";
		public string getter = "";
		public string cast = "";
		public string put = "";
		public int slots;

		public bool Equals(Slot? other) {
			return other != null && type == other.type && token == other.token && getter == other.getter && cast == other.cast && put == other.put && slots == other.slots;
		}

		public override bool Equals(object? other) {
			return Equals(other as Slot);
		}

		public override int GetHashCode() {
			return type.GetHashCode();
		}
	}

	sealed class Function : IEquatable<Function> {
		public bool export;
		public bool instanceMethod;
		public string receiverType = "";
		public string name = "";
		public string method = "";
		public string qualified = "";
		public string modifiers = "";
		public string ns = "";
		public string[] containers = Array.Empty<string>();
		public Slot? result;
		public Slot[] parameters = Array.Empty<Slot>();
		public string[] parameterNames = Array.Empty<string>();
		public string problem = "";
		public string problemType = "";
		public Location? location;

		public bool Equals(Function? other) {
			return other != null && export == other.export && instanceMethod == other.instanceMethod && receiverType == other.receiverType &&
				   name == other.name && method == other.method && qualified == other.qualified &&
				   modifiers == other.modifiers && ns == other.ns && containers.SequenceEqual(other.containers) && Equals(result, other.result) &&
				   parameters.SequenceEqual(other.parameters) && parameterNames.SequenceEqual(other.parameterNames) && problem == other.problem &&
				   problemType == other.problemType;
		}

		public override bool Equals(object? other) {
			return Equals(other as Function);
		}

		public override int GetHashCode() {
			return qualified.GetHashCode();
		}
	}

	public void Initialize(IncrementalGeneratorInitializationContext context) {
		IncrementalValuesProvider<Function> natives = context.SyntaxProvider.ForAttributeWithMetadataName(
			NATIVE_FUNCTION, static (node, _) => node is MethodDeclarationSyntax, static (found, _) => Describe(found, false, false));
		IncrementalValuesProvider<Function> exports = context.SyntaxProvider.ForAttributeWithMetadataName(
			EXPORT_TO_NATIVE, static (node, _) => node is MethodDeclarationSyntax, static (found, _) => Describe(found, true, false));
		IncrementalValuesProvider<Function> callables = context.SyntaxProvider.ForAttributeWithMetadataName(
			CALLABLE, static (node, _) => node is MethodDeclarationSyntax, static (found, _) => Describe(found, true, true));
		context.RegisterSourceOutput(natives.Collect().Combine(exports.Collect()).Combine(callables.Collect()), static (output, found) => {
			List<Function> valid = new();
			foreach (Function function in found.Left.Left.Concat(found.Left.Right).Concat(found.Right)) {
				if (function.problem.Length == 0) {
					valid.Add(function);
				} else if (function.problemType.Length > 0) {
					output.ReportDiagnostic(Diagnostic.Create(_unsupportedType, function.location, function.method, function.problemType));
				} else {
					output.ReportDiagnostic(Diagnostic.Create(_wrongShape, function.location, function.method, function.problem));
				}
			}
			if (valid.Count > 0) {
				valid.Sort(static (left, right) => string.CompareOrdinal(left.qualified, right.qualified));
				output.AddSource("FunctionTable.g.cs", SourceText.From(Render(valid), Encoding.UTF8));
			}
		});
	}

	static bool DerivesFrom(ITypeSymbol type, string ancestor) {
		for (INamedTypeSymbol? parent = type.BaseType; parent != null; parent = parent.BaseType) {
			if (parent.ToDisplayString() == ancestor) {
				return true;
			}
		}
		return false;
	}

	static Slot? Classify(ITypeSymbol type) {
		string display = type.ToDisplayString(_qualified);
		ITypeSymbol plain = type;
		string enumCast = "";
		if (type.TypeKind == TypeKind.Enum && type is INamedTypeSymbol named && named.EnumUnderlyingType != null) {
			plain = named.EnumUnderlyingType;
			enumCast = "(" + display + ")";
		}
		(string token, string getter, string cast, string put)? scalar = plain.SpecialType switch {
			SpecialType.System_Boolean => ("bool", "GetBoolean", "", ""),
			SpecialType.System_SByte => ("i8", "GetInt64", "(sbyte)", "(long)"),
			SpecialType.System_Byte => ("u8", "GetUInt64", "(byte)", "(ulong)"),
			SpecialType.System_Int16 => ("i16", "GetInt64", "(short)", "(long)"),
			SpecialType.System_UInt16 => ("u16", "GetUInt64", "(ushort)", "(ulong)"),
			SpecialType.System_Int32 => ("i32", "GetInt64", "(int)", "(long)"),
			SpecialType.System_UInt32 => ("u32", "GetUInt64", "(uint)", "(ulong)"),
			SpecialType.System_Int64 => ("i64", "GetInt64", "", "(long)"),
			SpecialType.System_UInt64 => ("u64", "GetUInt64", "", "(ulong)"),
			SpecialType.System_Single => ("f32", "GetSingle", "", ""),
			SpecialType.System_Double => ("f64", "GetDouble", "", ""),
			SpecialType.System_String => ("string", "GetString", "", ""),
			_ => null,
		};
		if (scalar != null) {
			if (enumCast.Length > 0 && scalar.Value.put.Length == 0) {
				return null;
			}
			return new Slot {
				type = display,
				token = scalar.Value.token,
				getter = scalar.Value.getter,
				cast = enumCast.Length > 0 ? enumCast : scalar.Value.cast,
				put = scalar.Value.put,
				slots = scalar.Value.token == "string" ? 2 : 1,
			};
		}
		switch (type.ToDisplayString(SymbolDisplayFormat.CSharpErrorMessageFormat)) {
		case "Shalltear.Vec2":
			return new Slot { type = display, token = "Vec2", getter = "GetVec2", slots = 1 };
		case "Shalltear.Vec3":
			return new Slot { type = display, token = "Vec3", getter = "GetVec3", slots = 2 };
		case "Shalltear.Vec4":
			return new Slot { type = display, token = "Vec4", getter = "GetVec4", slots = 2 };
		case "Shalltear.Quat":
			return new Slot { type = display, token = "Quat", getter = "GetQuat", slots = 2 };
		case "Shalltear.Color":
			return new Slot { type = display, token = "Color", getter = "GetColor", slots = 2 };
		}
		ITypeSymbol unannotated = type.WithNullableAnnotation(NullableAnnotation.NotAnnotated);
		if (type.TypeKind == TypeKind.Class && (unannotated.ToDisplayString(SymbolDisplayFormat.CSharpErrorMessageFormat) == ENGINE_OBJECT || DerivesFrom(type, ENGINE_OBJECT))) {
			string bare = unannotated.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);
			return new Slot { type = display, token = "object", getter = "GetObject<" + bare + ">", slots = 1 };
		}
		return null;
	}

	static Function Describe(GeneratorAttributeSyntaxContext found, bool export, bool callable) {
		IMethodSymbol method = (IMethodSymbol)found.TargetSymbol;
		MethodDeclarationSyntax syntax = (MethodDeclarationSyntax)found.TargetNode;
		string exportName = callable
			? method.ContainingType.ToDisplayString(_plain) + "::" + method.Name
			: (found.Attributes[0].ConstructorArguments.Length > 0 ? found.Attributes[0].ConstructorArguments[0].Value as string ?? "" : "");
		Function function = new() {
			export = export,
			instanceMethod = callable && !method.IsStatic,
			receiverType = callable && !method.IsStatic ? method.ContainingType.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat) : "",
			name = exportName,
			method = method.Name,
			qualified = method.ContainingType.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat) + "." + method.Name,
			location = syntax.Identifier.GetLocation(),
			ns = method.ContainingNamespace.IsGlobalNamespace ? "" : method.ContainingNamespace.ToDisplayString(),
		};
		if (function.name.Length == 0) {
			function.problem = "Give the attribute a name";
			return function;
		}
		if (callable) {
			if (method.IsGenericMethod || method.Parameters.Any(static parameter => parameter.RefKind != RefKind.None)) {
				function.problem = "Use a non-generic method without ref/in/out parameters";
				return function;
			}
			if (!method.IsStatic && !DerivesFrom(method.ContainingType, ENGINE_OBJECT)) {
				function.problem = "Instance [Callable] methods must be declared on a Behavior or DataAsset";
				return function;
			}
		} else if (!method.IsStatic || method.IsGenericMethod || method.Parameters.Any(static parameter => parameter.RefKind != RefKind.None)) {
			function.problem = "Use a static method without type parameters or ref/in/out parameters";
			return function;
		}
		List<string> containers = new();
		for (INamedTypeSymbol? type = method.ContainingType; type != null; type = type.ContainingType) {
			if (type.IsGenericType) {
				function.problem = "Move it out of generic type " + type.Name;
				return function;
			}
			if (export && type.DeclaredAccessibility is Accessibility.Private or Accessibility.Protected or Accessibility.ProtectedAndInternal) {
				function.problem = "Make type " + type.Name + " internal or public";
				return function;
			}
			string keyword = type.TypeKind == TypeKind.Struct ? "struct" : "class";
			if (type.IsRecord) {
				keyword = type.TypeKind == TypeKind.Struct ? "record struct" : "record";
			}
			containers.Insert(0, (type.IsStatic ? "static " : "") + "partial " + keyword + " " + type.Name);
		}
		function.containers = containers.ToArray();
		if (export) {
			bool reachable = method.DeclaredAccessibility is Accessibility.Public or Accessibility.Internal or Accessibility.ProtectedOrInternal;
			if (!reachable) {
				function.problem = "Make the method internal or public";
				return function;
			}
		} else {
			if (!method.IsPartialDefinition || method.PartialImplementationPart != null) {
				function.problem = "Declare it static partial without a body";
				return function;
			}
			function.modifiers = string.Join(" ", syntax.Modifiers.Select(static modifier => modifier.Text));
		}

		if (!method.ReturnsVoid) {
			function.result = Classify(method.ReturnType);
			if (function.result == null) {
				function.problem = "type";
				function.problemType = method.ReturnType.ToDisplayString();
				return function;
			}
		}
		List<Slot> parameters = new();
		List<string> names = new();
		foreach (IParameterSymbol parameter in method.Parameters) {
			Slot? slot = Classify(parameter.Type);
			if (slot == null) {
				function.problem = "type";
				function.problemType = parameter.Type.ToDisplayString();
				return function;
			}
			parameters.Add(slot);
			names.Add(parameter.Name);
		}
		function.parameters = parameters.ToArray();
		function.parameterNames = names.ToArray();
		return function;
	}

	static string Signature(Function function) {
		IEnumerable<string> paramTokens = function.parameters.Select(static parameter => parameter.token);
		if (function.instanceMethod) {
			paramTokens = new[] { "object" }.Concat(paramTokens);
		}
		return (function.result?.token ?? "void") + "(" + string.Join(",", paramTokens) + ")";
	}

	static string Literal(string text) {
		return SymbolDisplay.FormatLiteral(text, true);
	}

	static void RenderNative(StringBuilder text, Function function, int index) {
		string indent = new('\t', function.containers.Length);
		if (function.ns.Length > 0) {
			text.Append("namespace ").Append(function.ns).Append(" {\n");
		}
		for (int depth = 0; depth < function.containers.Length; ++depth) {
			text.Append('\t', depth).Append(function.containers[depth]).Append(" {\n");
		}
		string slot = "_sltNative" + index.ToString(CultureInfo.InvariantCulture);
		int slots = function.parameters.Sum(static parameter => parameter.slots) + 2;
		text.Append(indent).Append("static ").Append(INTEROP).Append("NativeFunction ").Append(slot).Append(";\n\n");
		text.Append(indent).Append(function.modifiers).Append(' ').Append(function.result?.type ?? "void").Append(' ').Append(function.method).Append('(');
		for (int at = 0; at < function.parameters.Length; ++at) {
			text.Append(at > 0 ? ", " : "").Append(function.parameters[at].type).Append(" @").Append(function.parameterNames[at]);
		}
		text.Append(") {\n");
		text.Append(indent).Append('\t').Append(INTEROP).Append("NativeCall sltCall = new(ref ").Append(slot).Append(", ").Append(Literal(function.name)).Append(", ");
		text.Append(Literal(Signature(function))).Append(", stackalloc ulong[").Append(slots.ToString(CultureInfo.InvariantCulture)).Append("]);\n");
		for (int at = 0; at < function.parameters.Length; ++at) {
			text.Append(indent).Append("\tsltCall.Put(").Append(function.parameters[at].put).Append('@').Append(function.parameterNames[at]).Append(");\n");
		}
		text.Append(indent).Append("\tsltCall.Invoke();\n");
		if (function.result != null) {
			text.Append(indent).Append("\treturn ").Append(function.result.cast).Append("sltCall.").Append(function.result.getter).Append("()");
			text.Append(function.result.token == "object" && !function.result.type.EndsWith("?", StringComparison.Ordinal) ? "!" : "").Append(";\n");
		}
		text.Append(indent).Append("}\n");
		for (int depth = function.containers.Length - 1; depth >= 0; --depth) {
			text.Append('\t', depth).Append("}\n");
		}
		if (function.ns.Length > 0) {
			text.Append("}\n");
		}
		text.Append('\n');
	}

	static void RenderExport(StringBuilder text, Function function) {
		text.Append("\t\t\t").Append(INTEROP).Append("NativeFunctions.Export(").Append(Literal(function.name)).Append(", ").Append(Literal(Signature(function)));
		text.Append(", static (ref ").Append(INTEROP).Append("NativeFrame frame) => {\n");
		if (function.instanceMethod) {
			text.Append("\t\t\t\t").Append(function.receiverType).Append("? self = frame.GetObject<").Append(function.receiverType).Append(">();\n");
			text.Append("\t\t\t\tif (self == null) {\n\t\t\t\t\treturn;\n\t\t\t\t}\n");
		}
		StringBuilder arguments = new();
		for (int at = 0; at < function.parameters.Length; ++at) {
			Slot parameter = function.parameters[at];
			string local = "a" + at.ToString(CultureInfo.InvariantCulture);
			text.Append("\t\t\t\t").Append(parameter.type).Append(' ').Append(local).Append(" = ").Append(parameter.cast).Append("frame.").Append(parameter.getter).Append("()");
			text.Append(parameter.token == "object" && !parameter.type.EndsWith("?", StringComparison.Ordinal) ? "!" : "").Append(";\n");
			arguments.Append(at > 0 ? ", " : "").Append(local);
		}
		string call = (function.instanceMethod ? "self." + function.method : function.qualified) + "(" + arguments + ")";
		if (function.result == null) {
			text.Append("\t\t\t\t").Append(call).Append(";\n");
		} else {
			text.Append("\t\t\t\tframe.Return(").Append(function.result.put).Append(call).Append(");\n");
		}
		text.Append("\t\t\t});\n");
	}

	static string Render(List<Function> functions) {
		StringBuilder text = new();
		text.Append("// <auto-generated/>\n");
		text.Append("#nullable enable\n\n");
		int index = 0;
		foreach (Function function in functions.Where(static function => !function.export)) {
			RenderNative(text, function, index++);
		}
		if (functions.Any(static function => function.export)) {
			text.Append("namespace Shalltear.Generated {\n");
			text.Append("\tstatic partial class TypeTable {\n");
			text.Append("\t\tstatic partial void RegisterFunctions() {\n");
			foreach (Function function in functions.Where(static function => function.export)) {
				RenderExport(text, function);
			}
			text.Append("\t\t}\n\t}\n}\n");
		}
		return text.ToString();
	}
}
