using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace Shalltear.Refactor;

internal static class Program {
	static readonly SymbolDisplayFormat _plain =
		SymbolDisplayFormat.FullyQualifiedFormat.WithGlobalNamespaceStyle(SymbolDisplayGlobalNamespaceStyle.Omitted);

	public static int Main(string[] args) {
		bool emitViews = false;
		bool emitWasmM2N = false;
		string? projectRoot = null;
		string? outDir = null;
		string? dllDir = null;
		string? existingHeader = null;
		string? outFile = null;
		string? manifestFile = null;
		string? targetFile = null;
		string? oldName = null;
		string? newName = null;
		string? runtimeDir = null;
		string? refsFile = null;

		for (int i = 0; i < args.Length; ++i) {
			string arg = args[i];
			string? Next() => i + 1 < args.Length ? args[++i] : null;
			switch (arg) {
			case "--emit-views":
				emitViews = true;
				break;
			case "--emit-wasm-m2n":
				emitWasmM2N = true;
				break;
			case "--dll-dir":
				dllDir = Next();
				break;
			case "--existing":
				existingHeader = Next();
				break;
			case "--out":
				outFile = Next();
				break;
			case "--manifest":
				manifestFile = Next();
				break;
			case "--project-root":
				projectRoot = Next();
				break;
			case "--out-dir":
				outDir = Next();
				break;
			case "--file":
				targetFile = Next();
				break;
			case "--old-name":
				oldName = Next();
				break;
			case "--new-name":
				newName = Next();
				break;
			case "--runtime-dir":
				runtimeDir = Next();
				break;
			case "--refs-file":
				refsFile = Next();
				break;
			}
		}

		if (emitWasmM2N) {
			if (string.IsNullOrWhiteSpace(dllDir) || string.IsNullOrWhiteSpace(outFile)) {
				Console.Error.WriteLine("usage: Shalltear.Refactor --emit-wasm-m2n --dll-dir <dir> --out <header> [--existing <header>] [--manifest <json>]");
				return 2;
			}
			return WasmM2NEmitter.Run(dllDir, existingHeader, outFile, manifestFile);
		}

		if (emitViews) {
			if (string.IsNullOrWhiteSpace(projectRoot) || string.IsNullOrWhiteSpace(outDir)) {
				Console.Error.WriteLine("usage: Shalltear.Refactor --emit-views --project-root <dir> --out-dir <dir> [--runtime-dir <dir>] [--refs-file <path>]");
				return 2;
			}
			return CppViewEmitter.Run(projectRoot, outDir, runtimeDir, refsFile);
		}

		if (string.IsNullOrWhiteSpace(targetFile) || string.IsNullOrWhiteSpace(oldName) || string.IsNullOrWhiteSpace(newName)) {
			Console.Error.WriteLine("usage: Shalltear.Refactor --file <path> --old-name <old> --new-name <new> [--project-root <dir>] [--runtime-dir <dir>]");
			return 2;
		}
		if (string.Equals(oldName, newName, StringComparison.Ordinal)) {
			return 0;
		}
		if (!SyntaxFacts.IsValidIdentifier(oldName) || !SyntaxFacts.IsValidIdentifier(newName)) {
			return 0;
		}

		string fullTarget = Path.GetFullPath(targetFile);
		if (!File.Exists(fullTarget)) {
			return 0;
		}

		List<string> sourceFiles = CollectSourceFiles(projectRoot, fullTarget);
		CSharpParseOptions parseOptions = CSharpParseOptions.Default.WithLanguageVersion(LanguageVersion.Preview);
		List<(string path, string text, SyntaxTree tree)> parsed = new(sourceFiles.Count);
		SyntaxTree? targetTree = null;

		foreach (string file in sourceFiles) {
			string text = File.ReadAllText(file);
			SyntaxTree tree = CSharpSyntaxTree.ParseText(text, parseOptions, path: file);
			parsed.Add((file, text, tree));
			if (PathsEqual(file, fullTarget)) {
				targetTree = tree;
			}
		}
		if (targetTree == null) {
			return 0;
		}

		List<MetadataReference> references = CollectReferences(runtimeDir, projectRoot, refsFile);
		CSharpCompilation compilation = CSharpCompilation.Create(
			"ShalltearRefactor",
			parsed.Select(static p => p.tree),
			references,
			new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));

		(INamedTypeSymbol? targetSymbol, MemberDeclarationSyntax? targetDecl) = FindDeclaredTarget(compilation, targetTree, oldName);
		if (targetSymbol == null || targetDecl == null) {
			return 0;
		}

		string oldFullName = targetSymbol.ToDisplayString(_plain);
		bool addFormerAttr = ShouldPreserveSerializedName(targetSymbol);

		foreach ((string file, string text, SyntaxTree tree) in parsed) {
			SemanticModel model = compilation.GetSemanticModel(tree);
			SyntaxNode root = tree.GetRoot();
			List<TextChange> changes = new();
			HashSet<int> seenStarts = new();

			void AddRename(SyntaxToken token) {
				if (token.ValueText == oldName && seenStarts.Add(token.Span.Start)) {
					changes.Add(new TextChange(token.Span, newName));
				}
			}

			foreach (SyntaxNode node in root.DescendantNodes()) {
				switch (node) {
				case BaseTypeDeclarationSyntax typeDecl when typeDecl.Identifier.ValueText == oldName:
					if (model.GetDeclaredSymbol(typeDecl) is INamedTypeSymbol sym && MatchesTarget(sym, targetSymbol)) {
						AddRename(typeDecl.Identifier);
					}
					break;
				case DelegateDeclarationSyntax delDecl when delDecl.Identifier.ValueText == oldName:
					if (model.GetDeclaredSymbol(delDecl) is INamedTypeSymbol delSym && MatchesTarget(delSym, targetSymbol)) {
						AddRename(delDecl.Identifier);
					}
					break;
				case ConstructorDeclarationSyntax ctor when ctor.Identifier.ValueText == oldName:
					if (model.GetDeclaredSymbol(ctor) is IMethodSymbol ctorSym && ctorSym.ContainingType != null && MatchesTarget(ctorSym.ContainingType, targetSymbol)) {
						AddRename(ctor.Identifier);
					}
					break;
				case DestructorDeclarationSyntax dtor when dtor.Identifier.ValueText == oldName:
					if (model.GetDeclaredSymbol(dtor) is IMethodSymbol dtorSym && dtorSym.ContainingType != null && MatchesTarget(dtorSym.ContainingType, targetSymbol)) {
						AddRename(dtor.Identifier);
					}
					break;
				case SimpleNameSyntax nameNode when nameNode.Identifier.ValueText == oldName:
					if (BindsToTarget(model, nameNode, targetSymbol)) {
						AddRename(nameNode.Identifier);
					}
					break;
				}
			}

			if (addFormerAttr && ReferenceEquals(tree, targetTree) && !HasFormerAttribute(targetDecl, oldFullName)) {
				TextChange? insertion = BuildFormerAttributeInsertion(text, root, targetDecl, oldFullName);
				if (insertion.HasValue) {
					changes.Add(insertion.Value);
				}
			}

			if (changes.Count == 0) {
				continue;
			}

			changes.Sort(static (a, b) => b.Span.Start.CompareTo(a.Span.Start));
			SourceText updated = SourceText.From(text).WithChanges(changes);
			File.WriteAllText(file, updated.ToString(), new UTF8Encoding(false));
		}

		return 0;
	}

	static List<string> CollectSourceFiles(string? projectRoot, string fullTarget) {
		List<string> files = new();
		HashSet<string> seen = new(OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);
		if (!string.IsNullOrWhiteSpace(projectRoot)) {
			string assetsDir = Path.Combine(Path.GetFullPath(projectRoot), "Assets");
			if (Directory.Exists(assetsDir)) {
				foreach (string file in Directory.EnumerateFiles(assetsDir, "*.cs", SearchOption.AllDirectories)) {
					string full = Path.GetFullPath(file);
					if (seen.Add(full)) {
						files.Add(full);
					}
				}
			}
		}
		if (seen.Add(fullTarget)) {
			files.Add(fullTarget);
		}
		return files;
	}

	internal static List<MetadataReference> CollectReferences(string? runtimeDir, string? projectRoot = null, string? refsFile = null) {
		List<MetadataReference> refs = new();
		HashSet<string> seen = new(OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);
		HashSet<string> seenNames = new(StringComparer.OrdinalIgnoreCase);

		void AddFile(string path) {
			if (string.IsNullOrEmpty(path) || !File.Exists(path)) {
				return;
			}
			string full = Path.GetFullPath(path);
			string fileName = Path.GetFileName(full);
			if (seen.Add(full) && seenNames.Add(fileName)) {
				try {
					refs.Add(MetadataReference.CreateFromFile(full));
				} catch {
				}
			}
		}

		if (!string.IsNullOrWhiteSpace(refsFile) && File.Exists(refsFile)) {
			foreach (string raw in File.ReadAllLines(refsFile)) {
				string line = raw.Trim();
				if (line.Length > 0) {
					AddFile(line);
				}
			}
		}

		string? coreDir = Path.GetDirectoryName(typeof(object).Assembly.Location);
		if (!string.IsNullOrEmpty(coreDir) && Directory.Exists(coreDir)) {
			foreach (string dll in Directory.EnumerateFiles(coreDir, "*.dll", SearchOption.TopDirectoryOnly)) {
				string name = Path.GetFileName(dll);
				if (name.Contains(".Native.", StringComparison.OrdinalIgnoreCase)) {
					continue;
				}
				if (name.StartsWith("System.", StringComparison.OrdinalIgnoreCase) ||
					name.Equals("netstandard.dll", StringComparison.OrdinalIgnoreCase) ||
					name.Equals("mscorlib.dll", StringComparison.OrdinalIgnoreCase) ||
					name.Equals("Microsoft.CSharp.dll", StringComparison.OrdinalIgnoreCase)) {
					AddFile(dll);
				}
			}
		}

		if (!string.IsNullOrWhiteSpace(runtimeDir) && Directory.Exists(runtimeDir)) {
			foreach (string dll in Directory.EnumerateFiles(runtimeDir, "Shalltear.*.dll", SearchOption.TopDirectoryOnly)) {
				AddFile(dll);
			}
		}

		if (!string.IsNullOrWhiteSpace(projectRoot)) {
			string fullProjectRoot = Path.GetFullPath(projectRoot);
			HashSet<string> sourceAssemblyNames = new(StringComparer.OrdinalIgnoreCase);
			if (Directory.Exists(fullProjectRoot)) {
				foreach (string csproj in Directory.EnumerateFiles(fullProjectRoot, "*.csproj", SearchOption.TopDirectoryOnly)) {
					string stem = Path.GetFileNameWithoutExtension(csproj);
					if (!stem.EndsWith(".Native.Scripts", StringComparison.OrdinalIgnoreCase) &&
						!stem.EndsWith(".Native", StringComparison.OrdinalIgnoreCase)) {
						sourceAssemblyNames.Add(stem + ".dll");
					}
				}
			}
			string binDir = Path.Combine(fullProjectRoot, "Library", "CSharp", "bin");
			if (Directory.Exists(binDir)) {
				foreach (string dll in Directory.EnumerateFiles(binDir, "*.dll", SearchOption.TopDirectoryOnly)) {
					string name = Path.GetFileName(dll);
					if (sourceAssemblyNames.Contains(name)) {
						continue;
					}
					AddFile(dll);
				}
			}
		}

		return refs;
	}

	static (INamedTypeSymbol?, MemberDeclarationSyntax?) FindDeclaredTarget(CSharpCompilation compilation, SyntaxTree tree, string oldName) {
		SemanticModel model = compilation.GetSemanticModel(tree);
		SyntaxNode root = tree.GetRoot();
		foreach (SyntaxNode node in root.DescendantNodes()) {
			if (node is BaseTypeDeclarationSyntax typeDecl && typeDecl.Identifier.ValueText == oldName &&
				model.GetDeclaredSymbol(typeDecl) is INamedTypeSymbol sym) {
				return (sym, typeDecl);
			}
			if (node is DelegateDeclarationSyntax delDecl && delDecl.Identifier.ValueText == oldName &&
				model.GetDeclaredSymbol(delDecl) is INamedTypeSymbol delSym) {
				return (delSym, delDecl);
			}
		}
		return (null, null);
	}

	static bool ShouldPreserveSerializedName(INamedTypeSymbol type) {
		if (type.IsGenericType || type.ContainingType != null || type.IsStatic) {
			return false;
		}
		if (type.TypeKind == TypeKind.Enum) {
			return true;
		}
		if (type.IsSerializable && type.TypeKind is TypeKind.Struct or TypeKind.Class) {
			return true;
		}
		for (INamedTypeSymbol? cur = type.BaseType; cur != null; cur = cur.BaseType) {
			string name = cur.ToDisplayString(_plain);
			if (name == "Shalltear.Behavior" || name == "Shalltear.DataAsset" || name == "Behavior" || name == "DataAsset") {
				return true;
			}
		}
		return false;
	}

	static bool HasFormerAttribute(MemberDeclarationSyntax decl, string oldFullName) {
		foreach (AttributeListSyntax list in decl.AttributeLists) {
			foreach (AttributeSyntax attr in list.Attributes) {
				string attrName = attr.Name.ToString();
				if (!attrName.EndsWith("FormerlySerializedAs", StringComparison.Ordinal) &&
					!attrName.EndsWith("FormerlySerializedAsAttribute", StringComparison.Ordinal)) {
					continue;
				}
				if (attr.ArgumentList?.Arguments.Count == 1 &&
					attr.ArgumentList.Arguments[0].Expression is LiteralExpressionSyntax literal &&
					literal.IsKind(SyntaxKind.StringLiteralExpression) &&
					string.Equals(literal.Token.ValueText, oldFullName, StringComparison.Ordinal)) {
					return true;
				}
			}
		}
		return false;
	}

	static TextChange? BuildFormerAttributeInsertion(string text, SyntaxNode root, MemberDeclarationSyntax decl, string oldFullName) {
		bool hasShalltearUsing = root.DescendantNodes().OfType<UsingDirectiveSyntax>()
			.Any(static u => u.Name != null && string.Equals(u.Name.ToString(), "Shalltear", StringComparison.Ordinal));
		string attrName = hasShalltearUsing ? "FormerlySerializedAs" : "global::Shalltear.FormerlySerializedAs";
		string newline = text.Contains("\r\n", StringComparison.Ordinal) ? "\r\n" : "\n";

		int insertPos = decl.SpanStart;
		int lineStart = text.LastIndexOf('\n', Math.Max(0, insertPos - 1)) + 1;
		int indentEnd = lineStart;
		while (indentEnd < insertPos && (text[indentEnd] == ' ' || text[indentEnd] == '\t')) {
			++indentEnd;
		}
		string indent = text.Substring(lineStart, indentEnd - lineStart);
		string literal = SymbolDisplay.FormatLiteral(oldFullName, true);
		string insertion = $"[{attrName}({literal})]{newline}{indent}";
		return new TextChange(new TextSpan(insertPos, 0), insertion);
	}

	static bool MatchesTarget(INamedTypeSymbol candidate, INamedTypeSymbol target) {
		return SymbolEqualityComparer.Default.Equals(candidate.OriginalDefinition, target.OriginalDefinition);
	}

	static bool IsInsideAttributeName(SimpleNameSyntax node) {
		for (SyntaxNode? cur = node.Parent; cur != null; cur = cur.Parent) {
			if (cur is AttributeSyntax attr) {
				return attr.Name.Span.Contains(node.Span);
			}
			if (cur is AttributeArgumentListSyntax or MemberDeclarationSyntax or StatementSyntax) {
				return false;
			}
		}
		return false;
	}

	static bool BindsToTarget(SemanticModel model, SimpleNameSyntax nameNode, INamedTypeSymbol target) {
		if (model.GetAliasInfo(nameNode) != null) {
			return false;
		}
		SymbolInfo symInfo = model.GetSymbolInfo(nameNode);
		if (SymbolMatchesTarget(symInfo.Symbol, target)) {
			return true;
		}
		foreach (ISymbol candidate in symInfo.CandidateSymbols) {
			if (SymbolMatchesTarget(candidate, target)) {
				return true;
			}
		}
		if (IsInsideAttributeName(nameNode) && nameNode.Ancestors().OfType<AttributeSyntax>().FirstOrDefault() is AttributeSyntax attrNode) {
			SymbolInfo attrInfo = model.GetSymbolInfo(attrNode);
			if (SymbolMatchesTarget(attrInfo.Symbol, target) || attrInfo.CandidateSymbols.Any(c => SymbolMatchesTarget(c, target))) {
				return true;
			}
		}
		TypeInfo typeInfo = model.GetTypeInfo(nameNode);
		if (typeInfo.Type is INamedTypeSymbol exprNamed &&
			nameNode.Parent is BaseObjectCreationExpressionSyntax or AttributeSyntax or QualifiedNameSyntax or TypeArgumentListSyntax or BaseTypeSyntax &&
			MatchesTarget(exprNamed, target)) {
			return true;
		}
		return false;
	}

	static bool SymbolMatchesTarget(ISymbol? symbol, INamedTypeSymbol target) {
		if (symbol == null) {
			return false;
		}
		if (symbol is INamedTypeSymbol named) {
			return MatchesTarget(named, target);
		}
		if (symbol is IMethodSymbol method &&
			method.MethodKind is MethodKind.Constructor or MethodKind.StaticConstructor or MethodKind.Destructor &&
			method.ContainingType != null) {
			return MatchesTarget(method.ContainingType, target);
		}
		return false;
	}

	static bool PathsEqual(string a, string b) {
		return string.Equals(Path.GetFullPath(a), Path.GetFullPath(b),
			OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal);
	}
}
