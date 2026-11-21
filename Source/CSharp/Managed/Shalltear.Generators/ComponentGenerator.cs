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
public sealed class ComponentGenerator : IIncrementalGenerator {
	const string BEHAVIOR = "Shalltear.Behavior";
	const string DATA_ASSET = "Shalltear.DataAsset";
	const string ASSET_MENU = "Shalltear.AssetMenuAttribute";
	const string ENGINE_OBJECT = "Shalltear.EngineObject";
	const string ENGINE_TYPE = "Shalltear.Interop.EngineTypeAttribute";
	const string LIST = "System.Collections.Generic.List<T>";
	const string SHADOW = "global::Shalltear.Interop.ShadowFields";
	const string TYPES = "global::Shalltear.Interop.ScriptTypes";
	const string PACKAGE_PREFIX = "Shalltear.";

	// must match SLT_FIELD_* + SLT_FIELD_SLOT_LIMIT in ScriptBridge.h
	const uint FIELD_HIDDEN = 1;
	const uint FIELD_READ_ONLY = 2;
	const uint FIELD_TRANSIENT = 4;
	const uint FIELD_RANGED = 8;
	const uint FIELD_LIST = 16;
	const int FIELD_SLOT_LIMIT = 960;

	// must match SltFieldKind in ScriptBridge.h
	const int KIND_STRING = 16;
	const int KIND_ENUM = 17;
	const int KIND_OBJECT = 18;
	const int KIND_VALUE = 19;

	// 8 byte slots, must fit str/ShadowList on all platforms
	const int TEXT_SLOTS = 4;
	const int LIST_SLOTS = 3;

	// bit order must match SLT_HOOK_* in ScriptBridge.h
	static readonly string[] _hooks = { "OnAttach", "OnDetach", "OnEnable", "OnDisable", "OnDestroy", "OnUpdate", "OnFixedUpdate", "OnLateUpdate" };

	// order must match SltFieldKind in ScriptBridge.h
	static readonly string[] _kindTypes = { "bool", "sbyte", "byte", "short", "ushort", "int", "uint", "long", "ulong", "float", "double", "Shalltear.Vec2", "Shalltear.Vec3", "Shalltear.Vec4", "Shalltear.Quat", "Shalltear.Color" };

	// 8 byte slots per kind, 16 byte aligned kinds padded
	static readonly int[] _kindSlots = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 3, 3, 2 };

	static readonly SymbolDisplayFormat _plain =
		SymbolDisplayFormat.FullyQualifiedFormat.WithGlobalNamespaceStyle(SymbolDisplayGlobalNamespaceStyle.Omitted);

	static readonly DiagnosticDescriptor _unsupportedField = new(
		"SLT1002", "Field type can't be serialized", "'{0}' has [Reflect] but type '{1}' can't be serialized. Use a supported type (or remove [Reflect]).",
		"Shalltear", DiagnosticSeverity.Error, true);

	static readonly DiagnosticDescriptor _tooManyFields = new(
		"SLT1004", "Too many serialized fields", "'{0}' has too many serialized fields ({1} of {2} slots). Remove some.", "Shalltear",
		DiagnosticSeverity.Error, true);

	enum Shape {
		Single,
		List,
		Array,
	}

	enum Reference {
		None,
		Engine,
		Script,
	}

	sealed class FieldType {
		public int kind;
		public Shape shape;
		public Reference reference;
		public string element = "";
		public string underlying = "";
		public string typeName = "";
		public bool isClass;
		public bool wide;
		public bool normalizes;
		public int slots;

		public string Key() {
			return string.Join("|", kind, (int)shape, (int)reference, element, underlying, typeName, isClass, wide, normalizes, slots);
		}
	}

	sealed class Field {
		public string name = "";
		public string owner = "";
		public string type = "";
		public string typeParameters = "";
		public string typeArguments = "";
		public string constraints = "";
		public bool direct;
		public uint flags;
		public float rangeMin;
		public float rangeMax;
		public string? tooltip;
		public string? header;
		public FieldType kind = new();

		public string Key() {
			return string.Join("|", name, owner, type, typeParameters, typeArguments, constraints, direct, flags,
							   rangeMin.ToString("R", CultureInfo.InvariantCulture), rangeMax.ToString("R", CultureInfo.InvariantCulture), tooltip ?? "\0",
							   header ?? "\0", kind.Key());
		}
	}

	sealed class EnumType {
		public string name = "";
		public string qualified = "";
		public int size;
		public ImmutableArray<KeyValuePair<string, long>> constants = ImmutableArray<KeyValuePair<string, long>>.Empty;

		public string Key() {
			return name + "|" + size + "|" + string.Join(",", constants.Select(static constant => constant.Key + "=" + constant.Value));
		}
	}

	sealed class ValueType {
		public string name = "";
		public string qualified = "";
		public bool isClass;
		public bool wide;
		public bool normalizes;
		public int depth;
		public int slots;
		public ImmutableArray<Field> fields = ImmutableArray<Field>.Empty;

		public string Key() {
			return string.Join("|", name, isClass, wide, normalizes, depth, slots) + "{" + string.Join(";", fields.Select(static field => field.Key())) + "}";
		}
	}

	sealed class Problem {
		public string field = "";
		public string type = "";
		public Location? location;
	}

	sealed class Collector {
		public readonly Dictionary<string, EnumType> enums = new(StringComparer.Ordinal);
		public readonly Dictionary<string, ValueType> values = new(StringComparer.Ordinal);
		public readonly HashSet<string> open = new(StringComparer.Ordinal);
		public readonly List<Problem> problems = new();
	}

	sealed class Target : IEquatable<Target> {
		public string scriptRoot = "";
		public string packages = "";
		public bool player;
		public bool hashedNames;
		public bool hasParts;

		public bool Equals(Target? other) =>
			other != null && scriptRoot == other.scriptRoot && packages == other.packages && player == other.player && hashedNames == other.hashedNames &&
			hasParts == other.hasParts;

		public override bool Equals(object? other) => Equals(other as Target);

		public override int GetHashCode() => scriptRoot.GetHashCode();
	}

	sealed class Component : IEquatable<Component> {
		public string name = "";
		public string qualified = "";
		public string parent = "";
		public bool isAbstract;
		public bool asset;
		public string menu = "";
		public string extension = "";
		public uint hookMask;
		public int depth;
		public int slots;
		public string script = "";
		public Location? location;
		public ImmutableArray<Field> fields = ImmutableArray<Field>.Empty;
		public ImmutableArray<EnumType> enums = ImmutableArray<EnumType>.Empty;
		public ImmutableArray<ValueType> values = ImmutableArray<ValueType>.Empty;
		public ImmutableArray<Problem> problems = ImmutableArray<Problem>.Empty;
		string? _key;

		string Key() {
			return _key ??= string.Join("|", name, qualified, parent, isAbstract, asset, menu, extension, hookMask, depth, slots, script) + "[" + string.Join(";", fields.Select(static field => field.Key())) +
							"][" + string.Join(";", enums.Select(static type => type.Key())) + "][" + string.Join(";", values.Select(static type => type.Key())) + "][" +
							string.Join(";", problems.Select(static problem => problem.field + ">" + problem.type)) + "]";
		}

		public bool Equals(Component? other) => other != null && Key() == other.Key();

		public override bool Equals(object? other) => Equals(other as Component);

		public override int GetHashCode() => name.GetHashCode();
	}

	public void Initialize(IncrementalGeneratorInitializationContext context) {
		IncrementalValuesProvider<Component?> classes = context.SyntaxProvider.CreateSyntaxProvider(
			static (node, _) => node is ClassDeclarationSyntax declaration && declaration.BaseList != null,
			static (syntax, cancel) => Describe(syntax.SemanticModel.GetDeclaredSymbol((ClassDeclarationSyntax)syntax.Node, cancel) as INamedTypeSymbol));
		IncrementalValuesProvider<Component> components = classes.Where(static component => component != null)!;
		IncrementalValueProvider<Target> target = context.AnalyzerConfigOptionsProvider.Combine(context.ParseOptionsProvider).Select(static (pair, _) => {
			Target target = new() { player = pair.Right.PreprocessorSymbolNames.Contains("SHALLTEAR_PLAYER") };
			if (target.player) {
				target.hashedNames = pair.Left.GlobalOptions.TryGetValue("build_property.ShalltearScriptNames", out string? names) && names == "hashed";
			} else if (pair.Left.GlobalOptions.TryGetValue("build_property.MSBuildProjectDirectory", out string? directory)) {
				target.scriptRoot = directory ?? "";
			}
			return target;
		});
		IncrementalValueProvider<string> packages = context.CompilationProvider.Select(static (compilation, _) => string.Join(";", PackageBindings(compilation)));
		IncrementalValuesProvider<bool> natives = context.SyntaxProvider.ForAttributeWithMetadataName(
			FunctionGenerator.NATIVE_FUNCTION, static (node, _) => node is MethodDeclarationSyntax, static (found, _) => true);
		IncrementalValuesProvider<bool> exports = context.SyntaxProvider.ForAttributeWithMetadataName(
			FunctionGenerator.EXPORT_TO_NATIVE, static (node, _) => node is MethodDeclarationSyntax, static (found, _) => true);
		IncrementalValueProvider<bool> written =
			context.CompilationProvider.Select(static (compilation, _) => compilation.Assembly.GetTypeByMetadataName("Shalltear.Generated.TypeTable") != null);
		IncrementalValueProvider<bool> parts = natives.Collect().Combine(exports.Collect()).Combine(written).Select(static (pair, _) => !pair.Left.Left.IsEmpty || !pair.Left.Right.IsEmpty || pair.Right);
		target = target.Combine(packages).Combine(parts).Select(static (pair, _) => new Target {
			scriptRoot = pair.Left.Left.scriptRoot,
			packages = pair.Left.Right,
			player = pair.Left.Left.player,
			hashedNames = pair.Left.Left.hashedNames,
			hasParts = pair.Right,
		});
		context.RegisterSourceOutput(components.Collect().Combine(target), static (output, pair) => {
			ImmutableArray<Component> found = pair.Left.IsDefault ? ImmutableArray<Component>.Empty : pair.Left;
			if (found.IsEmpty && !pair.Right.player && !pair.Right.hasParts) {
				return;
			}
			List<Component> ordered = new();
			HashSet<string> seen = new(StringComparer.Ordinal);
			HashSet<string> reported = new(StringComparer.Ordinal);
			foreach (Component component in found.OrderBy(static component => component.depth).ThenBy(static component => component.name, StringComparer.Ordinal)) {
				if (!seen.Add(component.name)) {
					continue;
				}
				ordered.Add(component);
				if (component.slots > FIELD_SLOT_LIMIT) {
					output.ReportDiagnostic(Diagnostic.Create(_tooManyFields, component.location, component.name, component.slots, FIELD_SLOT_LIMIT));
				}
				foreach (Problem problem in component.problems) {
					if (reported.Add(problem.field)) {
						output.ReportDiagnostic(Diagnostic.Create(_unsupportedField, problem.location, problem.field, problem.type));
					}
				}
			}
			output.AddSource("TypeTable.g.cs", SourceText.From(Render(ordered, pair.Right), Encoding.UTF8));
		});
	}

	static IEnumerable<string> PackageBindings(Compilation compilation) {
		foreach (IAssemblySymbol assembly in compilation.SourceModule.ReferencedAssemblySymbols.OrderBy(static assembly => assembly.Name, StringComparer.Ordinal)) {
			if (!assembly.Name.StartsWith(PACKAGE_PREFIX, StringComparison.Ordinal)) {
				continue;
			}
			string bindings = "Shalltear.Interop." + assembly.Name.Substring(PACKAGE_PREFIX.Length) + "Bindings";
			if (assembly.GetTypeByMetadataName(bindings) != null) {
				yield return bindings;
			}
		}
	}

	static bool IsRegistered(INamedTypeSymbol type) {
		return !type.IsGenericType && type.ContainingType == null && !type.IsStatic && !type.IsFileLocal;
	}

	static bool DerivesFrom(INamedTypeSymbol type, string ancestor) {
		for (INamedTypeSymbol? level = type; level != null; level = level.BaseType) {
			if (level.ToDisplayString(_plain) == ancestor) {
				return true;
			}
		}
		return false;
	}

	static bool IsReachable(Accessibility access) {
		return access == Accessibility.Public || access == Accessibility.Internal || access == Accessibility.ProtectedOrInternal;
	}

	static bool IsNameable(INamedTypeSymbol type) {
		for (INamedTypeSymbol? level = type; level != null; level = level.ContainingType) {
			if (level.IsFileLocal || level.IsGenericType || !IsReachable(level.DeclaredAccessibility)) {
				return false;
			}
		}
		return true;
	}

	static Component? Describe(INamedTypeSymbol? type) {
		if (type == null || !IsRegistered(type)) {
			return null;
		}
		Component component = new() {
			name = type.ToDisplayString(_plain),
			qualified = type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
			isAbstract = type.IsAbstract,
			location = type.Locations.FirstOrDefault(),
		};
		component.script = component.location?.SourceTree?.FilePath ?? "";
		Collector collector = new();
		List<Field> fields = new();
		bool behavior = false;
		bool own = true;
		for (INamedTypeSymbol? level = type; level != null; level = level.BaseType) {
			string root = level.ToDisplayString(_plain);
			if (root == BEHAVIOR || root == DATA_ASSET) {
				behavior = true;
				component.asset = root == DATA_ASSET;
				break;
			}
			for (int hook = 0; hook < _hooks.Length; ++hook) {
				if (level.GetMembers(_hooks[hook]).OfType<IMethodSymbol>().Any(static method => method.IsOverride && method.Parameters.Length == 0)) {
					component.hookMask |= 1u << hook;
				}
			}
			if (!SymbolEqualityComparer.Default.Equals(level, type) && IsRegistered(level)) {
				if (component.depth == 0) {
					component.parent = level.ToDisplayString(_plain);
				}
				++component.depth;
				own = false;
			}
			// parent's problems reported once, with parent
			Collector levelCollector = own ? collector : new Collector();
			List<Field> declared = level.GetMembers().OfType<IFieldSymbol>().Select(field => DescribeField(field, levelCollector)).Where(static field => field != null).ToList()!;
			component.slots += declared.Sum(static field => field.kind.slots);
			if (own) {
				fields.InsertRange(0, declared);
			}
		}
		if (!behavior) {
			return null;
		}
		AttributeData? menu = type.GetAttributes().FirstOrDefault(static attribute => attribute.AttributeClass?.ToDisplayString(_plain) == ASSET_MENU);
		if (component.asset && menu != null && menu.ConstructorArguments.Length == 2 && menu.ConstructorArguments[0].Value is string label &&
			menu.ConstructorArguments[1].Value is string extension) {
			component.menu = label;
			component.extension = extension;
		}
		component.fields = fields.ToImmutableArray();
		component.enums = collector.enums.Values.OrderBy(static type => type.name, StringComparer.Ordinal).ToImmutableArray();
		component.values = collector.values.Values.OrderBy(static type => type.depth).ThenBy(static type => type.name, StringComparer.Ordinal).ToImmutableArray();
		component.problems = collector.problems.ToImmutableArray();
		return component;
	}

	static Field? DescribeField(IFieldSymbol field, Collector collector) {
		if (field.IsStatic || field.IsConst || field.IsReadOnly || field.IsImplicitlyDeclared) {
			return null;
		}
		ImmutableArray<AttributeData> attributes = field.GetAttributes();
		ImmutableArray<TypedConstant> Find(string name) {
			AttributeData? found = attributes.FirstOrDefault(attribute => attribute.AttributeClass?.ToDisplayString(_plain) == name);
			return found == null ? default : found.ConstructorArguments;
		}
		bool marked = !Find("Shalltear.ReflectAttribute").IsDefault;
		if ((field.DeclaredAccessibility != Accessibility.Public && !marked) || !Find("System.NonSerializedAttribute").IsDefault) {
			return null;
		}
		FieldType? kind = Classify(field.Type, collector, false);
		if (kind == null) {
			if (marked) {
				collector.problems.Add(new Problem {
					field = field.ContainingType.ToDisplayString(_plain) + "." + field.Name,
					type = field.Type.ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat),
					location = field.Locations.FirstOrDefault(),
				});
			}
			return null;
		}

		List<ITypeParameterSymbol> parameters = new();
		List<ITypeSymbol> arguments = new();
		for (INamedTypeSymbol? level = field.ContainingType; level != null; level = level.ContainingType) {
			parameters.InsertRange(0, level.OriginalDefinition.TypeParameters);
			arguments.InsertRange(0, level.TypeArguments);
		}
		// generic type's field found by UnsafeAccessor through open type only
		IFieldSymbol open = field.OriginalDefinition;
		Field described = new() {
			name = field.Name,
			owner = open.ContainingType.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
			type = open.Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
			typeParameters = string.Join(", ", parameters.Select(static parameter => parameter.Name)),
			typeArguments = string.Join(", ", arguments.Select(static argument => argument.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat))),
			constraints = string.Concat(parameters.Select(Constrain)),
			direct = field.DeclaredAccessibility == Accessibility.Public,
			kind = kind,
		};
		described.flags |= kind.shape == Shape.Single ? 0 : FIELD_LIST;
		described.flags |= Find("Shalltear.HiddenAttribute").IsDefault ? 0 : FIELD_HIDDEN;
		described.flags |= Find("Shalltear.ReadOnlyAttribute").IsDefault ? 0 : FIELD_READ_ONLY;
		described.flags |= Find("Shalltear.TransientAttribute").IsDefault ? 0 : FIELD_TRANSIENT;
		ImmutableArray<TypedConstant> range = Find("Shalltear.RangeAttribute");
		if (!range.IsDefault && range.Length == 2 && range[0].Value is float min && range[1].Value is float max) {
			described.flags |= FIELD_RANGED;
			described.rangeMin = min;
			described.rangeMax = max;
		}
		ImmutableArray<TypedConstant> tooltip = Find("Shalltear.TooltipAttribute");
		ImmutableArray<TypedConstant> header = Find("Shalltear.HeaderAttribute");
		described.tooltip = !tooltip.IsDefault && tooltip.Length == 1 ? tooltip[0].Value as string : null;
		described.header = !header.IsDefault && header.Length == 1 ? header[0].Value as string : null;
		return described;
	}

	static FieldType? Classify(ITypeSymbol type, Collector collector, bool element) {
		if (!element) {
			ITypeSymbol? inner = null;
			Shape shape = Shape.Single;
			if (type is IArrayTypeSymbol array && array.Rank == 1) {
				inner = array.ElementType;
				shape = Shape.Array;
			} else if (type is INamedTypeSymbol list && list.IsGenericType && list.OriginalDefinition.ToDisplayString(_plain) == LIST) {
				inner = list.TypeArguments[0];
				shape = Shape.List;
			}
			if (inner != null) {
				FieldType? held = Classify(inner, collector, true);
				if (held != null) {
					held.shape = shape;
					held.slots = LIST_SLOTS;
					held.wide = false;
					held.normalizes = true;
				}
				return held;
			}
		}

		string name = type.ToDisplayString(_plain);
		string qualified = type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);
		int plain = Array.IndexOf(_kindTypes, name);
		if (plain >= 0) {
			return new FieldType { kind = plain, element = qualified, slots = _kindSlots[plain], wide = _kindSlots[plain] == 3 };
		}
		if (type.SpecialType == SpecialType.System_String) {
			return new FieldType { kind = KIND_STRING, element = "string", slots = TEXT_SLOTS, normalizes = true };
		}
		if (type is not INamedTypeSymbol named || !IsNameable(named)) {
			return null;
		}
		if (named.TypeKind == TypeKind.Enum && named.EnumUnderlyingType != null) {
			if (!collector.enums.ContainsKey(name)) {
				collector.enums.Add(name, DescribeEnum(named, name, qualified));
			}
			return new FieldType {
				kind = KIND_ENUM,
				element = qualified,
				underlying = named.EnumUnderlyingType.ToDisplayString(_plain),
				typeName = name,
				slots = 1,
			};
		}
		if (named.TypeKind == TypeKind.Class && DerivesFrom(named, ENGINE_OBJECT)) {
			AttributeData? engine = named.GetAttributes().FirstOrDefault(static attribute => attribute.AttributeClass?.ToDisplayString(_plain) == ENGINE_TYPE);
			if (engine != null && engine.ConstructorArguments.Length == 1 && engine.ConstructorArguments[0].Value is string engineName) {
				return new FieldType { kind = KIND_OBJECT, reference = Reference.Engine, element = qualified, typeName = engineName, slots = 1 };
			}
			if ((DerivesFrom(named, BEHAVIOR) || DerivesFrom(named, DATA_ASSET)) && IsRegistered(named)) {
				return new FieldType { kind = KIND_OBJECT, reference = Reference.Script, element = qualified, typeName = name, slots = 1 };
			}
			return null;
		}
		if (named.TypeKind != TypeKind.Struct && named.TypeKind != TypeKind.Class) {
			return null;
		}
		ValueType? value = DescribeValue(named, name, qualified, collector);
		if (value == null) {
			return null;
		}
		return new FieldType {
			kind = KIND_VALUE,
			element = qualified,
			typeName = name,
			isClass = value.isClass,
			wide = value.wide,
			normalizes = value.normalizes || value.isClass,
			slots = value.slots + (value.wide ? 1 : 0),
		};
	}

	static EnumType DescribeEnum(INamedTypeSymbol type, string name, string qualified) {
		SpecialType underlying = type.EnumUnderlyingType!.SpecialType;
		int size = 4;
		if (underlying == SpecialType.System_SByte || underlying == SpecialType.System_Byte) {
			size = 1;
		} else if (underlying == SpecialType.System_Int16 || underlying == SpecialType.System_UInt16) {
			size = 2;
		} else if (underlying == SpecialType.System_Int64 || underlying == SpecialType.System_UInt64) {
			size = 8;
		}
		List<KeyValuePair<string, long>> constants = new();
		foreach (IFieldSymbol constant in type.GetMembers().OfType<IFieldSymbol>()) {
			if (constant.HasConstantValue && constant.ConstantValue != null) {
				long value = constant.ConstantValue is ulong wide ? unchecked((long)wide) : Convert.ToInt64(constant.ConstantValue, CultureInfo.InvariantCulture);
				constants.Add(new KeyValuePair<string, long>(constant.Name, value));
			}
		}
		return new EnumType { name = name, qualified = qualified, size = size, constants = constants.ToImmutableArray() };
	}

	static ValueType? DescribeValue(INamedTypeSymbol type, string name, string qualified, Collector collector) {
		if (collector.values.TryGetValue(name, out ValueType? known)) {
			return known;
		}
		bool isClass = type.TypeKind == TypeKind.Class;
		bool constructible = !isClass || type.InstanceConstructors.Any(static constructor => constructor.Parameters.Length == 0 && IsReachable(constructor.DeclaredAccessibility));
		if (!type.IsSerializable || type.IsAbstract || type.IsStatic || !constructible || !collector.open.Add(name)) {
			return null;
		}
		List<Field> fields = new();
		for (INamedTypeSymbol? level = type; level != null && level.IsSerializable && level.SpecialType == SpecialType.None; level = isClass ? level.BaseType : null) {
			fields.InsertRange(0, level.GetMembers().OfType<IFieldSymbol>().Select(field => DescribeField(field, collector)).Where(static field => field != null)!);
		}
		collector.open.Remove(name);
		if (fields.Count == 0) {
			return null;
		}
		int depth = 0;
		foreach (Field field in fields) {
			if (field.kind.kind == KIND_VALUE && collector.values.TryGetValue(field.kind.typeName, out ValueType? nested)) {
				depth = Math.Max(depth, nested.depth + 1);
			}
		}
		ValueType value = new() {
			name = name,
			qualified = qualified,
			isClass = isClass,
			wide = fields.Any(static field => field.kind.wide),
			normalizes = fields.Any(static field => field.kind.normalizes),
			depth = depth,
			slots = fields.Sum(static field => field.kind.slots),
			fields = fields.ToImmutableArray(),
		};
		collector.values.Add(name, value);
		return value;
	}

	static string Constrain(ITypeParameterSymbol parameter) {
		List<string> parts = new();
		if (parameter.HasReferenceTypeConstraint) {
			parts.Add("class");
		}
		if (parameter.HasUnmanagedTypeConstraint) {
			parts.Add("unmanaged");
		} else if (parameter.HasValueTypeConstraint) {
			parts.Add("struct");
		}
		if (parameter.HasNotNullConstraint) {
			parts.Add("notnull");
		}
		parts.AddRange(parameter.ConstraintTypes.Select(static type => type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat)));
		if (parameter.HasConstructorConstraint) {
			parts.Add("new()");
		}
		return parts.Count == 0 ? "" : " where " + parameter.Name + " : " + string.Join(", ", parts);
	}

	static ulong Hash(string text) {
		ulong hash = 0xcbf29ce484222325UL;
		foreach (byte value in Encoding.UTF8.GetBytes(text)) {
			hash ^= value;
			hash *= 0x00000100000001b3UL;
		}
		return hash;
	}

	static string Hex(ulong value) {
		return "0x" + value.ToString("x16", CultureInfo.InvariantCulture) + "UL";
	}

	static string Literal(string? text) {
		return text == null ? "null" : SymbolDisplay.FormatLiteral(text, true);
	}

	static string Literal(float value) {
		if (float.IsNaN(value)) {
			return "float.NaN";
		}
		if (float.IsInfinity(value)) {
			return value > 0 ? "float.PositiveInfinity" : "float.NegativeInfinity";
		}
		return value.ToString("R", CultureInfo.InvariantCulture) + "f";
	}

	static string Name(string name, Target target) {
		return target.hashedNames ? "\"\"" : Literal(name);
	}

	static string Spec(Field field, Target target) {
		string[] spec = {
			Name(field.name, target),
			Hex(Hash(field.name)),
			field.kind.kind.ToString(CultureInfo.InvariantCulture),
			field.flags.ToString(CultureInfo.InvariantCulture) + "u",
			Literal(field.rangeMin),
			Literal(field.rangeMax),
			target.player ? "null" : Literal(field.tooltip),
			target.player ? "null" : Literal(field.header),
			field.kind.typeName.Length == 0 ? "0UL" : Hex(Hash(field.kind.typeName)),
		};
		return "new(" + string.Join(", ", spec) + ")";
	}

	sealed class Writer {
		public readonly Dictionary<string, int> values = new(StringComparer.Ordinal);
		public bool hashedNames;
		int _locals;

		public void Push(StringBuilder to, string tabs, FieldType type, string access, string at, string? cache) {
			if (type.shape != Shape.Single) {
				PushList(to, tabs, type, access, at);
				return;
			}
			switch (type.kind) {
			case KIND_STRING:
				to.Append(tabs).Append(SHADOW).Append(".Write(").Append(at).Append(", ").Append(access).Append(cache == null ? "" : ", behavior, " + cache).Append(");\n");
				break;
			case KIND_ENUM:
				to.Append(tabs).Append(SHADOW).Append(".Write(").Append(at).Append(", (").Append(type.underlying).Append(')').Append(access).Append(");\n");
				break;
			case KIND_VALUE:
				to.Append(tabs).Append("PushValue").Append(values[type.typeName]).Append('(').Append(type.isClass ? "" : "ref ").Append(access).Append(", ").Append(at).Append(");\n");
				break;
			default:
				to.Append(tabs).Append(SHADOW).Append(".Write(").Append(at).Append(", ").Append(access).Append(");\n");
				break;
			}
		}

		public void Pull(StringBuilder to, string tabs, FieldType type, string access, string at, string? cache, string member) {
			if (type.shape != Shape.Single) {
				PullList(to, tabs, type, access, at, member);
				return;
			}
			switch (type.kind) {
			case KIND_STRING:
				to.Append(tabs).Append(SHADOW).Append(".Read(").Append(at).Append(", ref ").Append(access).Append(cache == null ? "" : ", behavior, " + cache).Append(");\n");
				break;
			case KIND_ENUM: {
				string raw = "raw" + _locals++;
				to.Append(tabs).Append(SHADOW).Append(".Read(").Append(at).Append(", out ").Append(type.underlying).Append(' ').Append(raw).Append(");\n");
				to.Append(tabs).Append(access).Append(" = (").Append(type.element).Append(')').Append(raw).Append(";\n");
				break;
			}
			case KIND_OBJECT:
				to.Append(tabs).Append(SHADOW).Append(type.reference == Reference.Script ? ".ReadScript<" : ".ReadObject<").Append(type.element).Append(">(").Append(at).Append(", out ").Append(access);
				to.Append(type.reference == Reference.Script ? ", " + Literal(hashedNames ? "" : member) : "").Append(");\n");
				break;
			case KIND_VALUE:
				to.Append(tabs).Append("PullValue").Append(values[type.typeName]).Append("(ref ").Append(access).Append(", ").Append(at).Append(");\n");
				break;
			default:
				to.Append(tabs).Append(SHADOW).Append(".Read(").Append(at).Append(", out ").Append(access).Append(");\n");
				break;
			}
		}

		void PushList(StringBuilder to, string tabs, FieldType type, string access, string at) {
			switch (type.kind) {
			case KIND_STRING:
				to.Append(tabs).Append(SHADOW).Append(".WriteTexts(").Append(at).Append(", ").Append(access).Append(");\n");
				return;
			case KIND_OBJECT:
				to.Append(tabs).Append(SHADOW).Append(".WriteObjects<").Append(type.element).Append(">(").Append(at).Append(", ").Append(access).Append(");\n");
				return;
			case KIND_VALUE:
				break;
			default:
				to.Append(tabs).Append(SHADOW).Append(".WriteValues<").Append(type.element).Append(">(").Append(at).Append(", ").Append(access).Append(");\n");
				return;
			}
			int value = values[type.typeName];
			string id = (_locals++).ToString(CultureInfo.InvariantCulture);
			string span = type.shape == Shape.List ? "global::System.Runtime.InteropServices.CollectionsMarshal.AsSpan(" + access + ")" : "global::System.MemoryExtensions.AsSpan(" + access + ")";
			to.Append(tabs).Append("global::System.Span<").Append(type.element).Append("> items").Append(id).Append(" = ").Append(span).Append(";\n");
			to.Append(tabs).Append("nint data").Append(id).Append(" = ").Append(SHADOW).Append(".Resize(").Append(at).Append(", items").Append(id).Append(".Length);\n");
			to.Append(tabs).Append("for (int index").Append(id).Append(" = 0; index").Append(id).Append(" < items").Append(id).Append(".Length; ++index").Append(id).Append(") {\n");
			to.Append(tabs).Append("\tPushValue").Append(value).Append('(').Append(type.isClass ? "" : "ref ").Append("items").Append(id).Append("[index").Append(id).Append("], data").Append(id);
			to.Append(" + index").Append(id).Append(" * _valueSize").Append(value).Append(");\n").Append(tabs).Append("}\n");
		}

		void PullList(StringBuilder to, string tabs, FieldType type, string access, string at, string member) {
			switch (type.kind) {
			case KIND_STRING:
				to.Append(tabs).Append(SHADOW).Append(".ReadTexts(").Append(at).Append(", ref ").Append(access).Append(");\n");
				return;
			case KIND_OBJECT:
				to.Append(tabs).Append(SHADOW).Append(type.reference == Reference.Script ? ".ReadScripts<" : ".ReadObjects<").Append(type.element).Append(">(").Append(at).Append(", ref ").Append(access);
				to.Append(type.reference == Reference.Script ? ", " + Literal(hashedNames ? "" : member) : "").Append(");\n");
				return;
			case KIND_VALUE:
				break;
			default:
				to.Append(tabs).Append(SHADOW).Append(".ReadValues<").Append(type.element).Append(">(").Append(at).Append(", ref ").Append(access).Append(");\n");
				return;
			}
			int value = values[type.typeName];
			string id = (_locals++).ToString(CultureInfo.InvariantCulture);
			to.Append(tabs).Append("nint data").Append(id).Append(" = ").Append(SHADOW).Append(".Data(").Append(at).Append(", out int count").Append(id).Append(");\n");
			to.Append(tabs).Append("global::System.Span<").Append(type.element).Append("> items").Append(id).Append(" = ").Append(SHADOW).Append(".Prepare(ref ").Append(access).Append(", count").Append(id).Append(");\n");
			to.Append(tabs).Append("for (int index").Append(id).Append(" = 0; index").Append(id).Append(" < items").Append(id).Append(".Length; ++index").Append(id).Append(") {\n");
			to.Append(tabs).Append("\tPullValue").Append(value).Append("(ref items").Append(id).Append("[index").Append(id).Append("], data").Append(id);
			to.Append(" + index").Append(id).Append(" * _valueSize").Append(value).Append(");\n").Append(tabs).Append("}\n");
		}
	}

	static string Accessor(string name) {
		return "\t[global::System.Runtime.CompilerServices.UnsafeAccessor(global::System.Runtime.CompilerServices.UnsafeAccessorKind.Field, Name = " + Literal(name) + ")]\n";
	}

	static void RenderEnum(StringBuilder text, EnumType type, int index, Target target) {
		text.Append("\n\tstatic void RegisterEnum").Append(index).Append("() {\n");
		text.Append("\t\t").Append(TYPES).Append(".RegisterEnum(").Append(Name(type.name, target)).Append(", ").Append(Hex(Hash(type.name))).Append(", ").Append(type.size);
		text.Append(", new global::Shalltear.Interop.ScriptEnumConstant[] {\n");
		foreach (KeyValuePair<string, long> constant in type.constants) {
			text.Append("\t\t\tnew(").Append(Name(constant.Key, target)).Append(", ").Append(Hex(Hash(constant.Key))).Append(", ");
			text.Append(constant.Value.ToString(CultureInfo.InvariantCulture)).Append("L),\n");
		}
		text.Append("\t\t});\n\t}\n");
	}

	static void RenderValue(StringBuilder text, Writer writer, ValueType type, int index, Target target) {
		StringBuilder specs = new();
		StringBuilder push = new();
		StringBuilder pull = new();
		StringBuilder offsets = new();
		text.Append("\n\tstatic int _valueSize").Append(index).Append(";\n");
		for (int slot = 0; slot < type.fields.Length; ++slot) {
			Field field = type.fields[slot];
			string offset = "_value" + index + "_" + slot;
			string access = "value." + field.name;
			text.Append("\tstatic int ").Append(offset).Append(";\n");
			if (!field.direct) {
				string accessor = "ValueField" + index + "_" + slot;
				text.Append(Accessor(field.name));
				text.Append("\tstatic extern ref ").Append(field.type).Append(' ').Append(accessor).Append('(').Append(type.isClass ? "" : "ref ").Append(field.owner).Append(" value);\n");
				access = accessor + "(" + (type.isClass ? "" : "ref ") + "value)";
			}
			specs.Append("\t\t\t").Append(Spec(field, target)).Append(",\n");
			writer.Push(push, "\t\t", field.kind, access, "at + " + offset, null);
			writer.Pull(pull, "\t\t", field.kind, access, "at + " + offset, null, type.name + "." + field.name);
			offsets.Append("\t\t").Append(offset).Append(" = offsets[").Append(slot.ToString(CultureInfo.InvariantCulture)).Append("];\n");
		}
		if (type.isClass) {
			text.Append("\tstatic ").Append(type.qualified).Append(" _valueDefault").Append(index).Append(";\n");
			text.Append("\n\tstatic void PushValue").Append(index).Append('(').Append(type.qualified).Append(" value, nint at) {\n");
			text.Append("\t\tvalue ??= _valueDefault").Append(index).Append(" ??= new ").Append(type.qualified).Append("();\n");
		} else {
			text.Append("\n\tstatic void PushValue").Append(index).Append("(ref ").Append(type.qualified).Append(" value, nint at) {\n");
		}
		text.Append(push).Append("\t}\n");
		text.Append("\n\tstatic void PullValue").Append(index).Append("(ref ").Append(type.qualified).Append(" value, nint at) {\n");
		if (type.isClass) {
			text.Append("\t\tvalue ??= new ").Append(type.qualified).Append("();\n");
		}
		text.Append(pull).Append("\t}\n");
		text.Append("\n\tstatic void RegisterValue").Append(index).Append("() {\n");
		text.Append("\t\tglobal::System.Span<int> offsets = stackalloc int[").Append(type.fields.Length).Append("];\n");
		text.Append("\t\t_valueSize").Append(index).Append(" = ").Append(TYPES).Append(".RegisterValue(").Append(Name(type.name, target)).Append(", ").Append(Hex(Hash(type.name)));
		text.Append(", new global::Shalltear.Interop.ScriptField[] {\n").Append(specs).Append("\t\t}, offsets);\n");
		text.Append(offsets).Append("\t}\n");
	}

	static string ScriptOf(Component component, string root) {
		string file = component.script.Replace('\\', '/');
		string folder = root.Replace('\\', '/').TrimEnd('/') + "/";
		if (folder.Length == 1 || !file.StartsWith(folder, StringComparison.OrdinalIgnoreCase)) {
			return "null";
		}
		return Literal(file.Substring(folder.Length));
	}

	static string Render(List<Component> components, Target target) {
		List<EnumType> enums = new();
		List<ValueType> values = new();
		HashSet<string> seen = new(StringComparer.Ordinal);
		foreach (Component component in components) {
			enums.AddRange(component.enums.Where(type => seen.Add("enum " + type.name)));
			values.AddRange(component.values.Where(type => seen.Add("value " + type.name)));
		}
		enums.Sort(static (left, right) => string.CompareOrdinal(left.name, right.name));
		values.Sort(static (left, right) => left.depth != right.depth ? left.depth.CompareTo(right.depth) : string.CompareOrdinal(left.name, right.name));

		Writer writer = new() { hashedNames = target.hashedNames };
		StringBuilder text = new();
		text.Append("// <auto-generated/>\n");
		text.Append("#nullable disable\n");
		text.Append("#pragma warning disable CA2255\n");
		text.Append("namespace Shalltear.Generated;\n\n");
		text.Append(target.player ? "public " : "").Append("static partial class TypeTable {\n");
		text.Append("\tstatic bool _registered;\n");
		text.Append("\n\tstatic partial void RegisterNativeTypes();\n");
		text.Append("\n\tstatic partial void RegisterFunctions();\n");
		StringBuilder calls = new();
		for (int index = 0; index < enums.Count; ++index) {
			RenderEnum(text, enums[index], index, target);
			calls.Append("\t\tRegisterEnum").Append(index).Append("();\n");
		}
		for (int index = 0; index < values.Count; ++index) {
			writer.values.Add(values[index].name, index);
		}
		for (int index = 0; index < values.Count; ++index) {
			RenderValue(text, writer, values[index], index, target);
			calls.Append("\t\tRegisterValue").Append(index).Append("();\n");
		}
		for (int index = 0; index < components.Count; ++index) {
			Component component = components[index];
			string[] head = {
				Name(component.name, target),
				Hex(Hash(component.name)),
				component.parent.Length == 0 ? "0UL" : Hex(Hash(component.parent)),
				component.isAbstract ? "true" : "false",
				component.hookMask.ToString(CultureInfo.InvariantCulture) + "u",
				((component.hookMask >> 5) & 7u).ToString(CultureInfo.InvariantCulture) + "u",
				component.isAbstract ? "null" : "static () => new " + component.qualified + "()",
				ScriptOf(component, target.scriptRoot),
			};
			string register = "\t\t" + TYPES + ".Register<" + component.qualified + ">(" + string.Join(", ", head);
			string assetArguments = component.asset ? ", assetMenu: " + Literal(component.menu) + ", assetExtension: " + Literal(component.extension) : "";
			if (component.fields.Length == 0) {
				calls.Append(register).Append(", default, default, null, null, false, 0, out _").Append(assetArguments).Append(");\n");
				continue;
			}

			StringBuilder specs = new();
			StringBuilder push = new();
			StringBuilder pull = new();
			StringBuilder offsets = new();
			int texts = 0;
			text.Append("\n\tstatic int _textBase").Append(index).Append(";\n");
			for (int slot = 0; slot < component.fields.Length; ++slot) {
				Field field = component.fields[slot];
				string offset = "_offset" + index + "_" + slot;
				string access = "script." + field.name;
				text.Append("\tstatic int ").Append(offset).Append(";\n");
				if (!field.direct) {
					string accessor = "Field" + index + "_" + slot;
					if (field.typeParameters.Length == 0) {
						text.Append(Accessor(field.name));
						text.Append("\tstatic extern ref ").Append(field.type).Append(' ').Append(accessor).Append('(').Append(field.owner).Append(" script);\n");
						access = accessor + "(script)";
					} else {
						text.Append("\tstatic class ").Append(accessor).Append('<').Append(field.typeParameters).Append('>').Append(field.constraints).Append(" {\n");
						text.Append('\t').Append(Accessor(field.name));
						text.Append("\t\tinternal static extern ref ").Append(field.type).Append(" Get(").Append(field.owner).Append(" script);\n\t}\n");
						access = accessor + "<" + field.typeArguments + ">.Get(script)";
					}
				}
				string? cache = null;
				if (field.kind.kind == KIND_STRING && field.kind.shape == Shape.Single) {
					cache = "_textBase" + index + " + " + texts++;
				}
				specs.Append("\t\t\t").Append(Spec(field, target)).Append(",\n");
				writer.Push(push, "\t\t", field.kind, access, "shadow + " + offset, cache);
				writer.Pull(pull, "\t\t", field.kind, access, "shadow + " + offset, cache, component.name + "." + field.name);
				offsets.Append("\t\t").Append(offset).Append(" = offsets[").Append(slot.ToString(CultureInfo.InvariantCulture)).Append("];\n");
			}
			foreach ((string name, StringBuilder body) in new[] { ("Push", push), ("Pull", pull) }) {
				text.Append("\n\tstatic void ").Append(name).Append(index).Append("(global::Shalltear.EngineObject behavior, nint shadow) {\n");
				text.Append("\t\t").Append(component.qualified).Append(" script = (").Append(component.qualified).Append(")behavior;\n");
				text.Append(body).Append("\t}\n");
			}
			text.Append("\n\tstatic void Register").Append(index).Append("() {\n");
			text.Append("\t\tglobal::System.Span<int> offsets = stackalloc int[").Append(component.fields.Length).Append("];\n");
			text.Append(register).Append(", new global::Shalltear.Interop.ScriptField[] {\n").Append(specs);
			text.Append("\t\t}, offsets, Push").Append(index).Append(", Pull").Append(index).Append(", ");
			text.Append(component.fields.Any(static field => field.kind.normalizes) ? "true" : "false").Append(", ").Append(texts).Append(", out _textBase").Append(index).Append(assetArguments).Append(");\n");
			text.Append(offsets).Append("\t}\n");
			calls.Append("\t\tRegister").Append(index).Append("();\n");
		}
		text.Append("\n\t[global::System.Runtime.CompilerServices.ModuleInitializer]\n");
		text.Append(target.player ? "\tpublic" : "\tinternal").Append(" static void Register() {\n");
		text.Append("\t\tif (_registered || !").Append(TYPES).Append(".IsAttached) {\n\t\t\treturn;\n\t\t}\n");
		text.Append("\t\t_registered = true;\n");
		text.Append("\t\tRegisterNativeTypes();\n");
		foreach (string bindings in target.packages.Split(new[] { ';' }, StringSplitOptions.RemoveEmptyEntries)) {
			text.Append("\t\tglobal::").Append(bindings).Append(".Attach();\n");
		}
		text.Append(calls);
		text.Append("\t\tRegisterFunctions();\n");
		text.Append("\t}\n}\n");
		return text.ToString();
	}
}
