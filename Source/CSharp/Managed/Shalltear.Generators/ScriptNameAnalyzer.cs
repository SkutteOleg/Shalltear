using System.Collections.Immutable;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Diagnostics;
using Microsoft.CodeAnalysis.Operations;

namespace Shalltear.Generators;

[DiagnosticAnalyzer(LanguageNames.CSharp)]
public sealed class ScriptNameAnalyzer : DiagnosticAnalyzer {
	const string KEEP_NAME = "Shalltear.KeepNameAttribute";

	static readonly DiagnosticDescriptor _renamed = new(
		"SLT2003", "Name differs in Release players", "'{0}' differs in Release players ('{1}' renamed). Add [KeepName] to '{1}'.", "Shalltear",
		DiagnosticSeverity.Warning, true);

	static readonly string[] _keptNamespaces = ["System", "Microsoft", "Shalltear.Generated"];
	static readonly ImmutableHashSet<string> _enumNameMethods = ImmutableHashSet.Create("Parse", "TryParse", "GetName", "GetNames", "IsDefined", "Format");
	static readonly ImmutableHashSet<string> _typeNameProperties = ImmutableHashSet.Create("Name", "FullName", "Namespace", "AssemblyQualifiedName");
	static readonly ImmutableHashSet<string> _typeHolders = ImmutableHashSet.Create("System.Type", "System.Reflection.MemberInfo", "System.Reflection.TypeInfo");

	public override ImmutableArray<DiagnosticDescriptor> SupportedDiagnostics => ImmutableArray.Create(_renamed);

	public override void Initialize(AnalysisContext context) {
		context.ConfigureGeneratedCodeAnalysis(GeneratedCodeAnalysisFlags.None);
		context.EnableConcurrentExecution();
		context.RegisterOperationAction(CheckInvocation, OperationKind.Invocation);
		context.RegisterOperationAction(CheckProperty, OperationKind.PropertyReference);
		context.RegisterOperationAction(CheckInterpolation, OperationKind.Interpolation);
		context.RegisterOperationAction(CheckAppend, OperationKind.InterpolatedStringAppendFormatted);
		context.RegisterOperationAction(CheckConcatenation, OperationKind.Binary);
	}

	static void CheckInvocation(OperationAnalysisContext context) {
		IInvocationOperation invocation = (IInvocationOperation)context.Operation;
		IMethodSymbol method = invocation.TargetMethod;
		if (method.Name == "ToString" && invocation.Instance != null) {
			INamedTypeSymbol? printed = Renamed(Unwrap(invocation.Instance).Type, context.Compilation);
			if (printed != null && PrintsName(printed)) {
				Report(context, invocation, printed);
			}
			return;
		}
		if (method.ContainingType?.SpecialType != SpecialType.System_Enum || !method.IsStatic || !_enumNameMethods.Contains(method.Name)) {
			return;
		}
		ITypeSymbol? named = method.TypeArguments.Length == 1
								 ? method.TypeArguments[0]
								 : invocation.Arguments.Select(argument => Unwrap(argument.Value)).OfType<ITypeOfOperation>().FirstOrDefault()?.TypeOperand;
		INamedTypeSymbol? parsed = Renamed(named, context.Compilation);
		if (parsed != null) {
			Report(context, invocation, parsed);
		}
	}

	static void CheckProperty(OperationAnalysisContext context) {
		IPropertyReferenceOperation reference = (IPropertyReferenceOperation)context.Operation;
		if (!_typeNameProperties.Contains(reference.Property.Name) || reference.Instance == null ||
			!_typeHolders.Contains(reference.Property.ContainingType.ToDisplayString())) {
			return;
		}
		ITypeSymbol? named = Unwrap(reference.Instance) switch {
			ITypeOfOperation typeOf => typeOf.TypeOperand,
			IInvocationOperation { TargetMethod.Name: "GetType", Arguments.Length: 0, Instance: { } instance } => Unwrap(instance).Type,
			_ => null,
		};
		INamedTypeSymbol? read = Renamed(named, context.Compilation);
		if (read != null) {
			Report(context, reference, read);
		}
	}

	static void CheckInterpolation(OperationAnalysisContext context) {
		IInterpolationOperation interpolation = (IInterpolationOperation)context.Operation;
		INamedTypeSymbol? printed = Renamed(Unwrap(interpolation.Expression).Type, context.Compilation);
		if (printed != null && PrintsName(printed)) {
			Report(context, interpolation, printed);
		}
	}

	static void CheckAppend(OperationAnalysisContext context) {
		IInterpolatedStringAppendOperation append = (IInterpolatedStringAppendOperation)context.Operation;
		if (append.AppendCall is not IInvocationOperation { Arguments.Length: > 0 } call) {
			return;
		}
		INamedTypeSymbol? printed = Renamed(Unwrap(call.Arguments[0].Value).Type, context.Compilation);
		if (printed != null && PrintsName(printed)) {
			Report(context, append, printed);
		}
	}

	static void CheckConcatenation(OperationAnalysisContext context) {
		IBinaryOperation binary = (IBinaryOperation)context.Operation;
		if (binary.OperatorKind != BinaryOperatorKind.Add || binary.Type?.SpecialType != SpecialType.System_String) {
			return;
		}
		foreach (IOperation operand in new[] { binary.LeftOperand, binary.RightOperand }) {
			INamedTypeSymbol? printed = Renamed(Unwrap(operand).Type, context.Compilation);
			if (printed != null && PrintsName(printed)) {
				Report(context, operand, printed);
			}
		}
	}

	static IOperation Unwrap(IOperation operation) {
		while (operation is IConversionOperation { IsImplicit: true } conversion) {
			operation = conversion.Operand;
		}
		return operation;
	}

	static INamedTypeSymbol? Renamed(ITypeSymbol? type, Compilation compilation) {
		if (type is INamedTypeSymbol { OriginalDefinition.SpecialType: SpecialType.System_Nullable_T } nullable) {
			type = nullable.TypeArguments[0];
		}
		if (type is not INamedTypeSymbol named || !SymbolEqualityComparer.Default.Equals(named.ContainingAssembly, compilation.Assembly)) {
			return null;
		}
		INamedTypeSymbol outer = named;
		for (INamedTypeSymbol? current = named; current != null; current = current.ContainingType) {
			if (current.GetAttributes().Any(attribute => attribute.AttributeClass?.ToDisplayString() == KEEP_NAME)) {
				return null;
			}
			outer = current;
		}
		string space = outer.ContainingNamespace.ToDisplayString();
		return _keptNamespaces.Any(kept => space == kept || space.StartsWith(kept + ".", System.StringComparison.Ordinal)) ? null : named;
	}

	static bool PrintsName(INamedTypeSymbol type) {
		if (type.TypeKind == TypeKind.Enum) {
			return true;
		}
		for (INamedTypeSymbol? current = type; current != null && current.SpecialType is not (SpecialType.System_Object or SpecialType.System_ValueType);
			 current = current.BaseType) {
			foreach (ISymbol member in current.GetMembers("ToString")) {
				if (member is IMethodSymbol { Parameters.Length: 0, IsOverride: true } method && !(method.IsImplicitlyDeclared && current.IsRecord)) {
					return false;
				}
			}
		}
		return true;
	}

	static void Report(OperationAnalysisContext context, IOperation operation, INamedTypeSymbol type) {
		context.ReportDiagnostic(Diagnostic.Create(_renamed, operation.Syntax.GetLocation(), operation.Syntax.ToString(), type.Name));
	}
}
