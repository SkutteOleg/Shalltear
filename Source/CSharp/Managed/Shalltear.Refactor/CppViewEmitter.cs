using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace Shalltear.Refactor;

internal static class CppViewEmitter {
	static readonly SymbolDisplayFormat _plain =
		SymbolDisplayFormat.FullyQualifiedFormat.WithGlobalNamespaceStyle(SymbolDisplayGlobalNamespaceStyle.Omitted);

	static readonly HashSet<string> _cppKeywords = new(StringComparer.Ordinal) {
		"alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch",
		"char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const", "consteval", "constexpr",
		"constinit", "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete",
		"do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float", "for",
		"friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
		"nullptr", "operator", "or", "or_eq", "private", "protected", "public", "register", "reinterpret_cast",
		"requires", "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
		"switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename",
		"union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
	};

	public static int Run(string projectRoot, string outDir, string? runtimeDir) {
		string fullRoot = Path.GetFullPath(projectRoot);
		string assetsDir = Path.Combine(fullRoot, "Assets");
		List<string> sourceFiles = new();
		if (Directory.Exists(assetsDir)) {
			sourceFiles.AddRange(Directory.EnumerateFiles(assetsDir, "*.cs", SearchOption.AllDirectories));
		}
		string fullOut = Path.GetFullPath(outDir);
		Directory.CreateDirectory(fullOut);

		HashSet<string> expectedFiles = new(OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);
		if (sourceFiles.Count > 0) {
			CSharpParseOptions parseOptions = CSharpParseOptions.Default.WithLanguageVersion(LanguageVersion.Preview);
			List<SyntaxTree> trees = new(sourceFiles.Count);
			foreach (string file in sourceFiles) {
				trees.Add(CSharpSyntaxTree.ParseText(File.ReadAllText(file), parseOptions, path: file));
			}

			List<MetadataReference> references = Program.CollectReferences(runtimeDir);
			CSharpCompilation compilation = CSharpCompilation.Create(
				"ShalltearViewEmitter",
				trees,
				references,
				new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));

			List<INamedTypeSymbol> classes = new();
			HashSet<string> seenNames = new(StringComparer.Ordinal);
			foreach (SyntaxTree tree in trees) {
				SemanticModel model = compilation.GetSemanticModel(tree);
				foreach (ClassDeclarationSyntax decl in tree.GetRoot().DescendantNodes().OfType<ClassDeclarationSyntax>()) {
					if (model.GetDeclaredSymbol(decl) is not INamedTypeSymbol symbol) {
						continue;
					}
					if (symbol.IsAbstract || symbol.IsStatic || symbol.IsGenericType || symbol.ContainingType != null) {
						continue;
					}
					if (!DerivesFrom(symbol, "Shalltear.Behavior") && !DerivesFrom(symbol, "Shalltear.DataAsset")) {
						continue;
					}
					string fullName = symbol.ToDisplayString(_plain);
					if (seenNames.Add(fullName)) {
						classes.Add(symbol);
					}
				}
			}

			foreach (INamedTypeSymbol cls in classes.OrderBy(static c => c.ToDisplayString(_plain), StringComparer.Ordinal)) {
				string headerName = cls.Name + ".generated.h";
				string headerPath = Path.Combine(fullOut, headerName);
				expectedFiles.Add(headerPath);
				string content = EmitViewHeader(cls);
				if (!File.Exists(headerPath) || File.ReadAllText(headerPath) != content) {
					File.WriteAllText(headerPath, content, new UTF8Encoding(false));
				}
			}
		}

		foreach (string existing in Directory.EnumerateFiles(fullOut, "*.generated.h", SearchOption.TopDirectoryOnly)) {
			if (!expectedFiles.Contains(Path.GetFullPath(existing))) {
				try {
					File.Delete(existing);
				} catch {
				}
			}
		}
		return 0;
	}

	static bool DerivesFrom(INamedTypeSymbol type, string ancestor) {
		for (INamedTypeSymbol? cur = type.BaseType; cur != null; cur = cur.BaseType) {
			string name = cur.ToDisplayString(_plain);
			if (name == ancestor || (ancestor == "Shalltear.Behavior" && name == "Behavior") ||
				(ancestor == "Shalltear.DataAsset" && name == "DataAsset")) {
				return true;
			}
		}
		return false;
	}

	static string? MapCppType(ITypeSymbol type, out bool isHandle, out bool isString) {
		isHandle = false;
		isString = false;
		switch (type.SpecialType) {
		case SpecialType.System_Boolean:
			return "bool";
		case SpecialType.System_SByte:
			return "::Shalltear::i8";
		case SpecialType.System_Byte:
			return "::Shalltear::u8";
		case SpecialType.System_Int16:
			return "::Shalltear::i16";
		case SpecialType.System_UInt16:
			return "::Shalltear::u16";
		case SpecialType.System_Int32:
			return "::Shalltear::i32";
		case SpecialType.System_UInt32:
			return "::Shalltear::u32";
		case SpecialType.System_Int64:
			return "::Shalltear::i64";
		case SpecialType.System_UInt64:
			return "::Shalltear::u64";
		case SpecialType.System_Single:
			return "::Shalltear::f32";
		case SpecialType.System_Double:
			return "::Shalltear::f64";
		case SpecialType.System_String:
			isString = true;
			return "::Shalltear::str";
		}
		string name = type.WithNullableAnnotation(NullableAnnotation.NotAnnotated).ToDisplayString(_plain);
		switch (name) {
		case "Shalltear.Vec2":
			return "::Shalltear::Vec2";
		case "Shalltear.Vec3":
			return "::Shalltear::Vec3";
		case "Shalltear.Vec4":
			return "::Shalltear::Vec4";
		case "Shalltear.Quat":
			return "::Shalltear::Quat";
		case "Shalltear.Color":
			return "::Shalltear::Color";
		}
		if (type.TypeKind == TypeKind.Class && type is INamedTypeSymbol named) {
			for (INamedTypeSymbol? cur = named; cur != null; cur = cur.BaseType) {
				string curName = cur.ToDisplayString(_plain);
				if (curName == "Shalltear.EngineObject" || curName == "EngineObject") {
					isHandle = true;
					return "::Shalltear::Handle<::Shalltear::Object>";
				}
			}
		}
		return null;
	}

	static string EscapeIdent(string name) {
		return _cppKeywords.Contains(name) ? name + "_" : name;
	}

	static string EmitViewHeader(INamedTypeSymbol cls) {
		string fullName = cls.ToDisplayString(_plain);
		string viewClass = cls.Name + "View";
		bool isBehavior = DerivesFrom(cls, "Shalltear.Behavior");

		List<(string name, string cppType, bool isHandle)> fields = new();
		HashSet<string> seenFields = new(StringComparer.Ordinal);
		List<IMethodSymbol> callables = new();
		HashSet<string> seenMethods = new(StringComparer.Ordinal);

		for (INamedTypeSymbol? level = cls; level != null; level = level.BaseType) {
			string levelName = level.ToDisplayString(_plain);
			if (levelName == "Shalltear.Behavior" || levelName == "Shalltear.DataAsset" || levelName == "Behavior" || levelName == "DataAsset") {
				break;
			}
			foreach (ISymbol member in level.GetMembers()) {
				if (member is IFieldSymbol field && !field.IsStatic && !field.IsConst && !field.IsReadOnly && !field.IsImplicitlyDeclared) {
					bool marked = field.GetAttributes().Any(static a => a.AttributeClass?.ToDisplayString(_plain) == "Shalltear.ReflectAttribute");
					bool nonSerialized = field.GetAttributes().Any(static a => a.AttributeClass?.ToDisplayString(_plain) == "System.NonSerializedAttribute");
					if ((field.DeclaredAccessibility == Accessibility.Public || marked) && !nonSerialized && seenFields.Add(field.Name)) {
						string? cppType = MapCppType(field.Type, out bool isHandle, out _);
						if (cppType != null) {
							fields.Add((field.Name, cppType, isHandle));
						}
					}
				} else if (member is IPropertySymbol prop && !prop.IsStatic && !prop.IsIndexer && prop.GetMethod != null && prop.SetMethod != null) {
					bool marked = prop.GetAttributes().Any(static a => a.AttributeClass?.ToDisplayString(_plain) == "Shalltear.ReflectAttribute");
					if (marked && seenFields.Add(prop.Name)) {
						string? cppType = MapCppType(prop.Type, out bool isHandle, out _);
						if (cppType != null) {
							fields.Add((prop.Name, cppType, isHandle));
						}
					}
				} else if (member is IMethodSymbol method && !method.IsGenericMethod &&
						   method.GetAttributes().Any(static a => a.AttributeClass?.ToDisplayString(_plain) == "Shalltear.CallableAttribute")) {
					if (seenMethods.Add(method.Name)) {
						callables.Add(method);
					}
				}
			}
		}

		StringBuilder sb = new();
		sb.Append("// <auto-generated/>\n");
		sb.Append("// GENERATED by Shalltear C# view emitter. Do not edit manually.\n");
		sb.Append("#pragma once\n\n");
		sb.Append("#include \"Shalltear/Core/Handle.h\"\n");
		sb.Append("#include \"Shalltear/Core/Math/Color.h\"\n");
		sb.Append("#include \"Shalltear/Core/Math/Quat.h\"\n");
		sb.Append("#include \"Shalltear/Core/Math/Vector.h\"\n");
		sb.Append("#include \"Shalltear/Core/Module/ScriptFunctions.h\"\n");
		sb.Append("#include \"Shalltear/Core/Object.h\"\n");
		sb.Append("#include \"Shalltear/Core/Reflection/TypeRegistry.h\"\n");
		sb.Append("#include \"Shalltear/Core/Reflection/Variant.h\"\n");
		sb.Append("#include \"Shalltear/Core/Reflection/Walk.h\"\n");
		if (isBehavior) {
			sb.Append("#include \"Shalltear/Core/Scene/Component.h\"\n");
			sb.Append("#include \"Shalltear/Core/Scene/SceneObject.h\"\n");
		}
		sb.Append("\n#include <bit>\n#include <string_view>\n\n");
		sb.Append("namespace Shalltear::CSharpViews {\n\n");
		sb.Append("\tclass ").Append(viewClass).Append(" {\n");
		sb.Append("\tpublic:\n");
		sb.Append("\t\tstatic constexpr ::Shalltear::StringHash TYPE_HASH{\"").Append(fullName).Append("\"};\n\n");
		sb.Append("\t\t").Append(viewClass).Append("() = default;\n");
		sb.Append("\t\texplicit ").Append(viewClass).Append("(::Shalltear::Handle<::Shalltear::Object> handle) noexcept : _handle(handle) {}\n");
		sb.Append("\t\texplicit ").Append(viewClass).Append("(::Shalltear::Object* object) noexcept : _handle(ValidateInstance(object)) {}\n\n");
		sb.Append("\t\tstatic const ::Shalltear::TypeInfo* GetStaticType() noexcept {\n");
		sb.Append("\t\t\treturn ::Shalltear::TypeRegistry::FindType(TYPE_HASH);\n");
		sb.Append("\t\t}\n\n");
		if (isBehavior) {
			sb.Append("\t\tstatic ").Append(viewClass).Append(" From(::Shalltear::SceneObject& owner) noexcept {\n");
			sb.Append("\t\t\tconst ::Shalltear::TypeInfo* targetType = GetStaticType();\n");
			sb.Append("\t\t\tif (!targetType) {\n\t\t\t\treturn {};\n\t\t\t}\n");
			sb.Append("\t\t\tfor (::Shalltear::u32 i = 0; i < owner.GetComponentCount(); ++i) {\n");
			sb.Append("\t\t\t\t::Shalltear::Component* comp = owner.GetComponentAt(i).Get();\n");
			sb.Append("\t\t\t\tif (comp && !comp->IsPendingDestroy() && ::Shalltear::TypeRegistry::IsDerivedFrom(comp->GetType(), targetType)) {\n");
			sb.Append("\t\t\t\t\treturn ").Append(viewClass).Append("(comp);\n");
			sb.Append("\t\t\t\t}\n\t\t\t}\n\t\t\treturn {};\n\t\t}\n\n");
		}
		sb.Append("\t\tbool IsValid() const noexcept { return ValidateInstance(_handle.Get()) != nullptr; }\n");
		sb.Append("\t\texplicit operator bool() const noexcept { return IsValid(); }\n");
		sb.Append("\t\t::Shalltear::Handle<::Shalltear::Object> GetHandle() const noexcept { return _handle; }\n\n");

		foreach ((string fname, string cppType, bool isHandle) in fields) {
			string pascal = char.ToUpperInvariant(fname[0]) + fname.Substring(1);
			sb.Append("\t\t").Append(cppType).Append(" Get").Append(pascal).Append("() const {\n");
			sb.Append("\t\t\t::Shalltear::Object* inst = ValidateInstance(_handle.Get());\n");
			sb.Append("\t\t\tif (!inst) {\n\t\t\t\treturn {};\n\t\t\t}\n");
			sb.Append("\t\t\t::Shalltear::Variant value;\n");
			sb.Append("\t\t\tif (!::Shalltear::GetField(inst, inst->GetType(), ::Shalltear::StringHash(\"").Append(fname).Append("\"), &value)) {\n");
			sb.Append("\t\t\t\treturn {};\n\t\t\t}\n");
			if (isHandle) {
				sb.Append("\t\t\tconst ::Shalltear::u64* id = value.TryGet<::Shalltear::u64>();\n");
				sb.Append("\t\t\treturn id ? ::Shalltear::Handle<::Shalltear::Object>(::Shalltear::ObjectRegistry::Resolve(*id)) : ::Shalltear::Handle<::Shalltear::Object>{};\n");
			} else {
				sb.Append("\t\t\tconst ").Append(cppType).Append("* ptr = value.TryGet<").Append(cppType).Append(">();\n");
				sb.Append("\t\t\treturn ptr ? *ptr : ").Append(cppType).Append("{};\n");
			}
			sb.Append("\t\t}\n\n");

			sb.Append("\t\tvoid Set").Append(pascal).Append("(const ").Append(cppType).Append("& next) const {\n");
			sb.Append("\t\t\t::Shalltear::Object* inst = ValidateInstance(_handle.Get());\n");
			sb.Append("\t\t\tif (!inst) {\n\t\t\t\treturn;\n\t\t\t}\n");
			if (isHandle) {
				sb.Append("\t\t\tconst ::Shalltear::Variant wrapped(next.GetInstanceId());\n");
			} else {
				sb.Append("\t\t\tconst ::Shalltear::Variant wrapped(next);\n");
			}
			sb.Append("\t\t\t::Shalltear::SetField(inst, inst->GetType(), ::Shalltear::StringHash(\"").Append(fname).Append("\"), &wrapped);\n");
			sb.Append("\t\t}\n\n");
		}

		foreach (IMethodSymbol method in callables) {
			string retCpp = "void";
			if (!method.ReturnsVoid) {
				string? mapped = MapCppType(method.ReturnType, out _, out _);
				if (mapped == null) {
					continue;
				}
				retCpp = mapped;
			}
			List<(string cppType, string paramName)> mparams = new();
			bool ok = true;
			foreach (IParameterSymbol param in method.Parameters) {
				string? mapped = MapCppType(param.Type, out _, out bool isStr);
				if (mapped == null) {
					ok = false;
					break;
				}
				mparams.Add((isStr ? "std::string_view" : mapped, EscapeIdent(param.Name)));
			}
			if (!ok) {
				continue;
			}
			string exportName = method.ContainingType.ToDisplayString(_plain) + "::" + method.Name;
			List<string> sigParams = new();
			if (!method.IsStatic) {
				sigParams.Add("::Shalltear::Handle<::Shalltear::Object>");
			}
			sigParams.AddRange(mparams.Select(static p => p.cppType));
			string sig = retCpp + "(" + string.Join(", ", sigParams) + ")";

			sb.Append("\t\t").Append(method.IsStatic ? "static " : "").Append(retCpp).Append(' ').Append(EscapeIdent(method.Name)).Append('(');
			sb.Append(string.Join(", ", mparams.Select(static p => p.cppType + " " + p.paramName)));
			sb.Append(')').Append(method.IsStatic ? "" : " const").Append(" {\n");
			if (!method.IsStatic) {
				sb.Append("\t\t\tif (!IsValid()) {\n\t\t\t\treturn").Append(retCpp == "void" ? "" : " {}").Append(";\n\t\t\t}\n");
			}
			sb.Append("\t\t\tstatic auto fn = ::Shalltear::ScriptFunctions::Find<").Append(sig).Append(">(\"").Append(exportName).Append("\");\n");
			List<string> callArgs = new();
			if (!method.IsStatic) {
				callArgs.Add("_handle");
			}
			callArgs.AddRange(mparams.Select(static p => p.paramName));
			sb.Append("\t\t\t").Append(retCpp == "void" ? "" : "return ").Append("fn(").Append(string.Join(", ", callArgs)).Append(");\n");
			sb.Append("\t\t}\n\n");
		}

		sb.Append("\tprivate:\n");
		sb.Append("\t\tstatic ::Shalltear::Object* ValidateInstance(::Shalltear::Object* inst) noexcept {\n");
		sb.Append("\t\t\tif (!inst) {\n\t\t\t\treturn nullptr;\n\t\t\t}\n");
		sb.Append("\t\t\tconst ::Shalltear::TypeInfo* want = GetStaticType();\n");
		sb.Append("\t\t\tconst ::Shalltear::TypeInfo* got = inst->GetType();\n");
		sb.Append("\t\t\treturn (want && got && ::Shalltear::TypeRegistry::IsDerivedFrom(got, want)) ? inst : nullptr;\n");
		sb.Append("\t\t}\n\n");
		sb.Append("\t\t::Shalltear::Handle<::Shalltear::Object> _handle{};\n");
		sb.Append("\t};\n\n");
		if (!_cppKeywords.Contains(cls.Name)) {
			sb.Append("\tusing ").Append(cls.Name).Append(" = ").Append(viewClass).Append(";\n\n");
		}
		sb.Append("} // namespace Shalltear::CSharpViews\n");
		return sb.ToString();
	}
}
