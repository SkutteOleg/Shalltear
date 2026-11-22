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

	static readonly Dictionary<string, string> _coreEngineHeaders = new(StringComparer.Ordinal) {
		["SceneObject"] = "Shalltear/Core/Scene/SceneObject.h",
		["Component"] = "Shalltear/Core/Scene/Component.h",
		["Behavior"] = "Shalltear/Core/Scene/Behavior.h",
		["Scene"] = "Shalltear/Core/Scene/Scene.h",
		["SceneAsset"] = "Shalltear/Core/Scene/SceneAsset.h",
		["PrefabAsset"] = "Shalltear/Core/Scene/Prefab.h",
		["Asset"] = "Shalltear/Core/Asset/Asset.h",
		["DataAsset"] = "Shalltear/Core/Asset/DataAsset.h",
		["TextAsset"] = "Shalltear/Core/Asset/TextAsset.h",
		["MeshAsset"] = "Shalltear/Core/Graphics/MeshAsset.h",
		["MaterialAsset"] = "Shalltear/Core/Graphics/MaterialAsset.h",
		["TextureAsset"] = "Shalltear/Core/Graphics/TextureAsset.h",
		["SpriteAsset"] = "Shalltear/Core/Graphics/SpriteAsset.h",
		["ShaderAsset"] = "Shalltear/Core/Graphics/ShaderAsset.h",
		["SkeletonAsset"] = "Shalltear/Core/Graphics/SkeletonAsset.h",
		["SkinAsset"] = "Shalltear/Core/Graphics/SkinAsset.h",
		["MeshRenderer"] = "Shalltear/Core/Graphics/MeshRenderer.h",
		["SkinnedMeshRenderer"] = "Shalltear/Core/Graphics/SkinnedMeshRenderer.h",
		["Camera"] = "Shalltear/Core/Graphics/Camera.h",
		["Light"] = "Shalltear/Core/Graphics/Light.h",
		["FontAsset"] = "Shalltear/Core/UI/FontAsset.h",
		["StyleSheetAsset"] = "Shalltear/Core/UI/StyleSheetAsset.h",
		["VectorAsset"] = "Shalltear/Core/UI/VectorAsset.h",
	};

	sealed class MappedFieldType {
		public string CppType = "";
		public bool IsHandle;
		public bool IsString;
		public bool IsEnum;
		public string? EngineTypeName;
		public string? CoreHeader;
	}

	public static int Run(string projectRoot, string outDir, string? runtimeDir, string? refsFile = null) {
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

			List<MetadataReference> references = Program.CollectReferences(runtimeDir, projectRoot, refsFile);
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

			Dictionary<string, int> shortNameCounts = new(StringComparer.OrdinalIgnoreCase);
			foreach (INamedTypeSymbol cls in classes) {
				shortNameCounts[cls.Name] = shortNameCounts.TryGetValue(cls.Name, out int count) ? count + 1 : 1;
			}

			foreach (INamedTypeSymbol cls in classes.OrderBy(static c => c.ToDisplayString(_plain), StringComparer.Ordinal)) {
				bool collided = shortNameCounts[cls.Name] > 1;
				string fullName = cls.ToDisplayString(_plain);
				string headerName = (collided ? fullName : cls.Name) + ".generated.h";
				string headerPath = Path.Combine(fullOut, headerName);
				expectedFiles.Add(headerPath);
				string content = EmitViewHeader(cls, collided);
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

	static bool TryResolveEngineType(INamedTypeSymbol named, out string cppClass, out string? coreHeader) {
		cppClass = "";
		coreHeader = null;
		foreach (AttributeData attr in named.GetAttributes()) {
			string? attrName = attr.AttributeClass?.ToDisplayString(_plain);
			if ((attrName == "Shalltear.Interop.EngineTypeAttribute" || attrName == "EngineTypeAttribute") &&
				attr.ConstructorArguments.Length == 1 &&
				attr.ConstructorArguments[0].Value is string rawName &&
				!string.IsNullOrWhiteSpace(rawName) &&
				IsSimpleCppIdentifier(rawName)) {
				cppClass = rawName;
				_coreEngineHeaders.TryGetValue(rawName, out coreHeader);
				return true;
			}
		}
		string full = named.ToDisplayString(_plain);
		string? mapped = full switch {
			"Shalltear.SceneObject" or "SceneObject" => "SceneObject",
			"Shalltear.Component" or "Component" => "Component",
			"Shalltear.Behavior" or "Behavior" => "Behavior",
			"Shalltear.Asset" or "Asset" => "Asset",
			"Shalltear.DataAsset" or "DataAsset" => "DataAsset",
			_ => null,
		};
		if (mapped != null) {
			cppClass = mapped;
			_coreEngineHeaders.TryGetValue(mapped, out coreHeader);
			return true;
		}
		return false;
	}

	static bool IsSimpleCppIdentifier(string text) {
		if (string.IsNullOrEmpty(text) || (!char.IsLetter(text[0]) && text[0] != '_')) {
			return false;
		}
		foreach (char c in text) {
			if (!char.IsLetterOrDigit(c) && c != '_') {
				return false;
			}
		}
		return !_cppKeywords.Contains(text);
	}

	static MappedFieldType? MapCppType(ITypeSymbol type, bool allowForwardHandle) {
		switch (type.SpecialType) {
		case SpecialType.System_Boolean:
			return new MappedFieldType { CppType = "bool" };
		case SpecialType.System_SByte:
			return new MappedFieldType { CppType = "::Shalltear::i8" };
		case SpecialType.System_Byte:
			return new MappedFieldType { CppType = "::Shalltear::u8" };
		case SpecialType.System_Int16:
			return new MappedFieldType { CppType = "::Shalltear::i16" };
		case SpecialType.System_UInt16:
			return new MappedFieldType { CppType = "::Shalltear::u16" };
		case SpecialType.System_Int32:
			return new MappedFieldType { CppType = "::Shalltear::i32" };
		case SpecialType.System_UInt32:
			return new MappedFieldType { CppType = "::Shalltear::u32" };
		case SpecialType.System_Int64:
			return new MappedFieldType { CppType = "::Shalltear::i64" };
		case SpecialType.System_UInt64:
			return new MappedFieldType { CppType = "::Shalltear::u64" };
		case SpecialType.System_Single:
			return new MappedFieldType { CppType = "::Shalltear::f32" };
		case SpecialType.System_Double:
			return new MappedFieldType { CppType = "::Shalltear::f64" };
		case SpecialType.System_String:
			return new MappedFieldType { CppType = "::Shalltear::str", IsString = true };
		}
		if (type.TypeKind == TypeKind.Enum && type is INamedTypeSymbol enumSym && enumSym.EnumUnderlyingType != null) {
			MappedFieldType? underlying = MapCppType(enumSym.EnumUnderlyingType, allowForwardHandle);
			if (underlying != null) {
				underlying.IsEnum = true;
				return underlying;
			}
		}
		string name = type.WithNullableAnnotation(NullableAnnotation.NotAnnotated).ToDisplayString(_plain);
		switch (name) {
		case "Shalltear.Vec2":
			return new MappedFieldType { CppType = "::Shalltear::Vec2" };
		case "Shalltear.Vec3":
			return new MappedFieldType { CppType = "::Shalltear::Vec3" };
		case "Shalltear.Vec4":
			return new MappedFieldType { CppType = "::Shalltear::Vec4" };
		case "Shalltear.Quat":
			return new MappedFieldType { CppType = "::Shalltear::Quat" };
		case "Shalltear.Color":
			return new MappedFieldType { CppType = "::Shalltear::Color" };
		}
		if (type.TypeKind == TypeKind.Class && type is INamedTypeSymbol named) {
			bool isEngineObj = false;
			for (INamedTypeSymbol? cur = named; cur != null; cur = cur.BaseType) {
				string curName = cur.ToDisplayString(_plain);
				if (curName == "Shalltear.EngineObject" || curName == "EngineObject") {
					isEngineObj = true;
					break;
				}
			}
			if (isEngineObj) {
				if (TryResolveEngineType(named, out string engineCpp, out string? coreHeader) &&
					(coreHeader != null || allowForwardHandle)) {
					return new MappedFieldType {
						CppType = "::Shalltear::Handle<::Shalltear::" + engineCpp + ">",
						IsHandle = true,
						EngineTypeName = engineCpp,
						CoreHeader = coreHeader,
					};
				}
				return new MappedFieldType {
					CppType = "::Shalltear::Handle<::Shalltear::Object>",
					IsHandle = true,
				};
			}
		}
		return null;
	}

	static string EscapeIdent(string name) {
		return _cppKeywords.Contains(name) ? name + "_" : name;
	}

	static string EmitViewHeader(INamedTypeSymbol cls, bool useQualifiedNamespace) {
		string fullName = cls.ToDisplayString(_plain);
		string viewClass = cls.Name + "View";
		bool isBehavior = DerivesFrom(cls, "Shalltear.Behavior");
		string cppNamespace = "Shalltear::CSharpViews";
		if (useQualifiedNamespace && cls.ContainingNamespace != null && !cls.ContainingNamespace.IsGlobalNamespace) {
			string nsPath = cls.ContainingNamespace.ToDisplayString(_plain).Replace(".", "::", StringComparison.Ordinal);
			if (!string.IsNullOrEmpty(nsPath)) {
				cppNamespace += "::" + nsPath;
			}
		}

		List<(string name, MappedFieldType mapped)> fields = new();
		HashSet<string> seenFields = new(StringComparer.Ordinal);
		List<(IMethodSymbol method, MappedFieldType? ret, List<(string cppType, string paramName, MappedFieldType mapped)> parameters)> callables = new();
		HashSet<string> seenMethods = new(StringComparer.Ordinal);
		SortedSet<string> extraIncludes = new(StringComparer.Ordinal);
		SortedSet<string> forwardDecls = new(StringComparer.Ordinal);

		if (isBehavior) {
			extraIncludes.Add("Shalltear/Core/Scene/Component.h");
			extraIncludes.Add("Shalltear/Core/Scene/SceneObject.h");
		}

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
						MappedFieldType? mapped = MapCppType(field.Type, allowForwardHandle: true);
						if (mapped != null) {
							if (mapped.EngineTypeName != null) {
								if (mapped.CoreHeader != null) {
									extraIncludes.Add(mapped.CoreHeader);
								} else {
									forwardDecls.Add(mapped.EngineTypeName);
								}
							}
							fields.Add((field.Name, mapped));
						}
					}
				} else if (member is IPropertySymbol prop && !prop.IsStatic && !prop.IsIndexer && prop.GetMethod != null && prop.SetMethod != null) {
					bool marked = prop.GetAttributes().Any(static a => a.AttributeClass?.ToDisplayString(_plain) == "Shalltear.ReflectAttribute");
					if (marked && seenFields.Add(prop.Name)) {
						MappedFieldType? mapped = MapCppType(prop.Type, allowForwardHandle: true);
						if (mapped != null) {
							if (mapped.EngineTypeName != null) {
								if (mapped.CoreHeader != null) {
									extraIncludes.Add(mapped.CoreHeader);
								} else {
									forwardDecls.Add(mapped.EngineTypeName);
								}
							}
							fields.Add((prop.Name, mapped));
						}
					}
				} else if (member is IMethodSymbol method && !method.IsGenericMethod &&
						   method.GetAttributes().Any(static a => a.AttributeClass?.ToDisplayString(_plain) == "Shalltear.CallableAttribute")) {
					if (!seenMethods.Add(method.Name)) {
						continue;
					}
					MappedFieldType? retMapped = null;
					if (!method.ReturnsVoid) {
						retMapped = MapCppType(method.ReturnType, allowForwardHandle: false);
						if (retMapped == null) {
							continue;
						}
					}
					List<(string cppType, string paramName, MappedFieldType mapped)> mparams = new();
					bool ok = true;
					foreach (IParameterSymbol param in method.Parameters) {
						MappedFieldType? mapped = MapCppType(param.Type, allowForwardHandle: false);
						if (mapped == null) {
							ok = false;
							break;
						}
						mparams.Add((mapped.IsString ? "std::string_view" : mapped.CppType, EscapeIdent(param.Name), mapped));
					}
					if (!ok) {
						continue;
					}
					if (retMapped?.CoreHeader != null) {
						extraIncludes.Add(retMapped.CoreHeader);
					}
					foreach ((_, _, MappedFieldType pm) in mparams) {
						if (pm.CoreHeader != null) {
							extraIncludes.Add(pm.CoreHeader);
						}
					}
					callables.Add((method, retMapped, mparams));
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
		foreach (string inc in extraIncludes) {
			sb.Append("#include \"").Append(inc).Append("\"\n");
		}
		sb.Append("\n#include <bit>\n#include <string_view>\n\n");
		if (forwardDecls.Count > 0) {
			sb.Append("namespace Shalltear {\n");
			foreach (string fwd in forwardDecls) {
				sb.Append("\tclass ").Append(fwd).Append(";\n");
			}
			sb.Append("} // namespace Shalltear\n\n");
		}
		sb.Append("namespace ").Append(cppNamespace).Append(" {\n\n");
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

		foreach ((string fname, MappedFieldType mapped) in fields) {
			string pascal = char.ToUpperInvariant(fname[0]) + fname.Substring(1);
			sb.Append("\t\t").Append(mapped.CppType).Append(" Get").Append(pascal).Append("() const {\n");
			sb.Append("\t\t\t::Shalltear::Object* inst = ValidateInstance(_handle.Get());\n");
			sb.Append("\t\t\tif (!inst) {\n\t\t\t\treturn {};\n\t\t\t}\n");
			sb.Append("\t\t\t::Shalltear::Variant value;\n");
			sb.Append("\t\t\tif (!::Shalltear::GetField(inst, inst->GetType(), ::Shalltear::StringHash(\"").Append(fname).Append("\"), &value)) {\n");
			sb.Append("\t\t\t\treturn {};\n\t\t\t}\n");
			if (mapped.IsHandle) {
				sb.Append("\t\t\tconst ::Shalltear::u64* id = value.TryGet<::Shalltear::u64>();\n");
				sb.Append("\t\t\t::Shalltear::Object* resolved = id ? ::Shalltear::ObjectRegistry::Resolve(*id) : nullptr;\n");
				if (mapped.EngineTypeName != null) {
					sb.Append("\t\t\tconst ::Shalltear::TypeInfo* expected = ::Shalltear::TypeRegistry::FindType(::Shalltear::StringHash(\"").Append(mapped.EngineTypeName).Append("\"));\n");
					sb.Append("\t\t\tif (!resolved || !expected || !::Shalltear::TypeRegistry::IsDerivedFrom(resolved->GetType(), expected)) {\n\t\t\t\treturn {};\n\t\t\t}\n");
					sb.Append("\t\t\treturn std::bit_cast<").Append(mapped.CppType).Append(">(resolved->GetInstanceId());\n");
				} else {
					sb.Append("\t\t\treturn resolved ? ::Shalltear::Handle<::Shalltear::Object>(resolved) : ::Shalltear::Handle<::Shalltear::Object>{};\n");
				}
			} else if (mapped.IsEnum) {
				sb.Append("\t\t\tconst ::Shalltear::i64* ptr = value.TryGet<::Shalltear::i64>();\n");
				sb.Append("\t\t\treturn ptr ? static_cast<").Append(mapped.CppType).Append(">(*ptr) : ").Append(mapped.CppType).Append("{};\n");
			} else {
				sb.Append("\t\t\tconst ").Append(mapped.CppType).Append("* ptr = value.TryGet<").Append(mapped.CppType).Append(">();\n");
				sb.Append("\t\t\treturn ptr ? *ptr : ").Append(mapped.CppType).Append("{};\n");
			}
			sb.Append("\t\t}\n\n");

			sb.Append("\t\tvoid Set").Append(pascal).Append("(const ").Append(mapped.CppType).Append("& next) const {\n");
			sb.Append("\t\t\t::Shalltear::Object* inst = ValidateInstance(_handle.Get());\n");
			sb.Append("\t\t\tif (!inst) {\n\t\t\t\treturn;\n\t\t\t}\n");
			if (mapped.IsHandle) {
				sb.Append("\t\t\tconst ::Shalltear::Variant wrapped(next.GetInstanceId());\n");
			} else if (mapped.IsEnum) {
				sb.Append("\t\t\tconst ::Shalltear::Variant wrapped(static_cast<::Shalltear::i64>(next));\n");
			} else {
				sb.Append("\t\t\tconst ::Shalltear::Variant wrapped(next);\n");
			}
			sb.Append("\t\t\t::Shalltear::SetField(inst, inst->GetType(), ::Shalltear::StringHash(\"").Append(fname).Append("\"), &wrapped);\n");
			sb.Append("\t\t}\n\n");
		}

		foreach ((IMethodSymbol method, MappedFieldType? retMapped, List<(string cppType, string paramName, MappedFieldType mapped)> mparams) in callables) {
			string retCpp = retMapped?.CppType ?? "void";
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
		sb.Append("} // namespace ").Append(cppNamespace).Append('\n');
		return sb.ToString();
	}
}
