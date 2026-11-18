using System.Collections.Immutable;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Diagnostics;

namespace Shalltear.Generators;

[DiagnosticAnalyzer(LanguageNames.CSharp)]
public sealed class EngineObjectAnalyzer : DiagnosticAnalyzer {
	const string ENGINE_OBJECT = "Shalltear.EngineObject";

	static readonly SymbolDisplayFormat _plain =
		SymbolDisplayFormat.FullyQualifiedFormat.WithGlobalNamespaceStyle(SymbolDisplayGlobalNamespaceStyle.Omitted);

	static readonly DiagnosticDescriptor _missesDestroyed = new(
		"SLT2001", "Operator misses destroyed objects", "'{0}' misses a destroyed {1}. Compare with == null.", "Shalltear", DiagnosticSeverity.Warning,
		true);

	public override ImmutableArray<DiagnosticDescriptor> SupportedDiagnostics => ImmutableArray.Create(_missesDestroyed);

	public override void Initialize(AnalysisContext context) {
		context.ConfigureGeneratedCodeAnalysis(GeneratedCodeAnalysisFlags.None);
		context.EnableConcurrentExecution();
		context.RegisterSyntaxNodeAction(Check, SyntaxKind.ConditionalAccessExpression, SyntaxKind.CoalesceExpression,
										 SyntaxKind.CoalesceAssignmentExpression, SyntaxKind.IsPatternExpression);
	}

	static void Check(SyntaxNodeAnalysisContext context) {
		ExpressionSyntax operand;
		string used;
		switch (context.Node) {
		case ConditionalAccessExpressionSyntax access:
			operand = access.Expression;
			used = "?.";
			break;
		case BinaryExpressionSyntax coalesce:
			operand = coalesce.Left;
			used = "??";
			break;
		case AssignmentExpressionSyntax assignment:
			operand = assignment.Left;
			used = "??=";
			break;
		case IsPatternExpressionSyntax match when IsNull(match.Pattern):
			operand = match.Expression;
			used = "is null";
			break;
		case IsPatternExpressionSyntax match when match.Pattern is UnaryPatternSyntax negated && IsNull(negated.Pattern):
			operand = match.Expression;
			used = "is not null";
			break;
		default:
			return;
		}
		ITypeSymbol? type = context.SemanticModel.GetTypeInfo(operand, context.CancellationToken).Type;
		if (type != null && IsEngineObject(type)) {
			context.ReportDiagnostic(Diagnostic.Create(_missesDestroyed, context.Node.GetLocation(), used, type.Name));
		}
	}

	static bool IsNull(PatternSyntax pattern) {
		return pattern is ConstantPatternSyntax constant && constant.Expression.IsKind(SyntaxKind.NullLiteralExpression);
	}

	static bool IsEngineObject(ITypeSymbol type) {
		if (type is ITypeParameterSymbol parameter) {
			return parameter.ConstraintTypes.Any(IsEngineObject);
		}
		for (ITypeSymbol? level = type; level != null; level = level.BaseType) {
			if (level.ToDisplayString(_plain) == ENGINE_OBJECT) {
				return true;
			}
		}
		return false;
	}
}
