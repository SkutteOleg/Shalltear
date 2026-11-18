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

	// must match SLT_FIELD_* + SLT_FIELD_SLOT_LIMIT in ScriptBridge.h
	const uint FIELD_HIDDEN = 1;
	const uint FIELD_READ_ONLY = 2;
	const uint FIELD_TRANSIENT = 4;
	const uint FIELD_RANGED = 8;
	const int FIELD_SLOT_LIMIT = 960;

	// bit order must match SLT_HOOK_* in ScriptBridge.h
	static readonly string[] _hooks = { "OnAttach", "OnDetach", "OnEnable", "OnDisable", "OnDestroy", "OnUpdate", "OnFixedUpdate", "OnLateUpdate" };

	// order must match SltFieldKind in ScriptBridge.h
	static readonly string[] _kindTypes = { "bool", "sbyte", "byte", "short", "ushort", "int", "uint", "long", "ulong", "float", "double", "Shalltear.Vec2", "Shalltear.Vec3", "Shalltear.Vec4", "Shalltear.Quat", "Shalltear.Color" };

	// 8 byte slots per kind, 16 byte aligned kinds padded
	static readonly int[] _kindSlots = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 3, 3, 2 };

	static readonly SymbolDisplayFormat _plain =
		SymbolDisplayFormat.FullyQualifiedFormat.WithGlobalNamespaceStyle(SymbolDisplayGlobalNamespaceStyle.Omitted);

	static readonly DiagnosticDescriptor _tooManyFields = new(
		"SLT1004", "Too many serialized fields", "'{0}' has too many serialized fields ({1} of {2} slots). Remove some.", "Shalltear",
		DiagnosticSeverity.Error, true);

	sealed class Field : IEquatable<Field> {
		public string name = "";
		public string owner = "";
		public string type = "";
		public string typeParameters = "";
		public string typeArguments = "";
		public string constraints = "";
		public int kind;
		public bool direct;
		public uint flags;
		public float rangeMin;
		public float rangeMax;
		public string? tooltip;
		public string? header;

		public bool Equals(Field? other) {
			return other != null && name == other.name && owner == other.owner && type == other.type && typeParameters == other.typeParameters &&
				   typeArguments == other.typeArguments && constraints == other.constraints && kind == other.kind && direct == other.direct &&
				   flags == other.flags && rangeMin.Equals(other.rangeMin) && rangeMax.Equals(other.rangeMax) && tooltip == other.tooltip &&
				   header == other.header;
		}

		public override bool Equals(object? other) => Equals(other as Field);

		public override int GetHashCode() => name.GetHashCode();
	}

	sealed class Component : IEquatable<Component> {
		public string name = "";
		public string qualified = "";
		public string parent = "";
		public bool isAbstract;
		public uint hookMask;
		public int depth;
		public int slots;
		public Location? location;
		public ImmutableArray<Field> fields = ImmutableArray<Field>.Empty;

		public bool Equals(Component? other) {
			return other != null && name == other.name && qualified == other.qualified && parent == other.parent && isAbstract == other.isAbstract &&
				   hookMask == other.hookMask && depth == other.depth && slots == other.slots && fields.SequenceEqual(other.fields);
		}

		public override bool Equals(object? other) => Equals(other as Component);

		public override int GetHashCode() => name.GetHashCode();
	}

	public void Initialize(IncrementalGeneratorInitializationContext context) {
		IncrementalValuesProvider<Component?> classes = context.SyntaxProvider.CreateSyntaxProvider(
			static (node, _) => node is ClassDeclarationSyntax declaration && declaration.BaseList != null,
			static (syntax, cancel) => Describe(syntax.SemanticModel.GetDeclaredSymbol((ClassDeclarationSyntax)syntax.Node, cancel) as INamedTypeSymbol));
		IncrementalValuesProvider<Component> components = classes.Where(static component => component != null)!;
		context.RegisterSourceOutput(components.Collect(), static (output, found) => {
			if (found.IsDefaultOrEmpty) {
				return;
			}
			List<Component> ordered = new();
			HashSet<string> seen = new(StringComparer.Ordinal);
			foreach (Component component in found.OrderBy(static component => component.depth).ThenBy(static component => component.name, StringComparer.Ordinal)) {
				if (!seen.Add(component.name)) {
					continue;
				}
				ordered.Add(component);
				if (component.slots > FIELD_SLOT_LIMIT) {
					output.ReportDiagnostic(Diagnostic.Create(_tooManyFields, component.location, component.name, component.slots, FIELD_SLOT_LIMIT));
				}
			}
			output.AddSource("TypeTable.g.cs", SourceText.From(Render(ordered), Encoding.UTF8));
		});
	}

	static bool IsRegistered(INamedTypeSymbol type) {
		return !type.IsGenericType && type.ContainingType == null && !type.IsStatic && !type.IsFileLocal;
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
		List<Field> fields = new();
		bool behavior = false;
		bool own = true;
		for (INamedTypeSymbol? level = type; level != null; level = level.BaseType) {
			if (level.ToDisplayString(_plain) == BEHAVIOR) {
				behavior = true;
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
			List<Field> declared = level.GetMembers().OfType<IFieldSymbol>().Select(DescribeField).Where(static field => field != null).ToList()!;
			component.slots += declared.Sum(static field => _kindSlots[field.kind]);
			if (own) {
				fields.InsertRange(0, declared);
			}
		}
		if (!behavior) {
			return null;
		}
		component.fields = fields.ToImmutableArray();
		return component;
	}

	static Field? DescribeField(IFieldSymbol field) {
		if (field.IsStatic || field.IsConst || field.IsReadOnly || field.IsImplicitlyDeclared) {
			return null;
		}
		ImmutableArray<AttributeData> attributes = field.GetAttributes();
		ImmutableArray<TypedConstant> Find(string name) {
			AttributeData? found = attributes.FirstOrDefault(attribute => attribute.AttributeClass?.ToDisplayString(_plain) == "Shalltear." + name + "Attribute");
			return found == null ? default : found.ConstructorArguments;
		}
		if (field.DeclaredAccessibility != Accessibility.Public && Find("Reflect").IsDefault) {
			return null;
		}
		int kind = Array.IndexOf(_kindTypes, field.Type.ToDisplayString(_plain));
		if (kind < 0) {
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
			kind = kind,
			direct = field.DeclaredAccessibility == Accessibility.Public,
		};
		described.flags |= Find("Hidden").IsDefault ? 0 : FIELD_HIDDEN;
		described.flags |= Find("ReadOnly").IsDefault ? 0 : FIELD_READ_ONLY;
		described.flags |= Find("Transient").IsDefault ? 0 : FIELD_TRANSIENT;
		ImmutableArray<TypedConstant> range = Find("Range");
		if (!range.IsDefault && range.Length == 2 && range[0].Value is float min && range[1].Value is float max) {
			described.flags |= FIELD_RANGED;
			described.rangeMin = min;
			described.rangeMax = max;
		}
		ImmutableArray<TypedConstant> tooltip = Find("Tooltip");
		ImmutableArray<TypedConstant> header = Find("Header");
		described.tooltip = !tooltip.IsDefault && tooltip.Length == 1 ? tooltip[0].Value as string : null;
		described.header = !header.IsDefault && header.Length == 1 ? header[0].Value as string : null;
		return described;
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

	static string Render(List<Component> components) {
		StringBuilder text = new();
		text.Append("// <auto-generated/>\n");
		text.Append("#pragma warning disable CA2255\n");
		text.Append("namespace Shalltear.Generated;\n\n");
		text.Append("static class TypeTable {\n");
		text.Append("\tstatic bool _registered;\n");
		StringBuilder calls = new();
		for (int index = 0; index < components.Count; ++index) {
			Component component = components[index];
			string[] head = {
				Literal(component.name),
				Hex(Hash(component.name)),
				component.parent.Length == 0 ? "0UL" : Hex(Hash(component.parent)),
				component.isAbstract ? "true" : "false",
				component.hookMask.ToString(CultureInfo.InvariantCulture) + "u",
				((component.hookMask >> 5) & 7u).ToString(CultureInfo.InvariantCulture) + "u",
				component.isAbstract ? "null" : "static () => new " + component.qualified + "()",
			};
			if (component.fields.Length == 0) {
				calls.Append("\t\tglobal::Shalltear.Interop.ScriptTypes.Register<").Append(component.qualified).Append(">(");
				calls.Append(string.Join(", ", head)).Append(", default, default, null, null);\n");
				continue;
			}

			StringBuilder specs = new();
			StringBuilder push = new();
			StringBuilder pull = new();
			StringBuilder offsets = new();
			text.Append('\n');
			for (int slot = 0; slot < component.fields.Length; ++slot) {
				Field field = component.fields[slot];
				string offset = "_offset" + index + "_" + slot;
				string access = "script." + field.name;
				text.Append("\tstatic int ").Append(offset).Append(";\n");
				if (!field.direct) {
					string accessor = "Field" + index + "_" + slot;
					string attribute =
						"[global::System.Runtime.CompilerServices.UnsafeAccessor(global::System.Runtime.CompilerServices.UnsafeAccessorKind.Field, Name = " +
						Literal(field.name) + ")]\n";
					if (field.typeParameters.Length == 0) {
						text.Append('\t').Append(attribute);
						text.Append("\tstatic extern ref ").Append(field.type).Append(' ').Append(accessor).Append('(').Append(field.owner).Append(" script);\n");
						access = accessor + "(script)";
					} else {
						text.Append("\tstatic class ").Append(accessor).Append('<').Append(field.typeParameters).Append('>').Append(field.constraints).Append(" {\n");
						text.Append("\t\t").Append(attribute);
						text.Append("\t\tinternal static extern ref ").Append(field.type).Append(" Get(").Append(field.owner).Append(" script);\n\t}\n");
						access = accessor + "<" + field.typeArguments + ">.Get(script)";
					}
				}
				string[] spec = {
					Literal(field.name),
					Hex(Hash(field.name)),
					field.kind.ToString(CultureInfo.InvariantCulture),
					field.flags.ToString(CultureInfo.InvariantCulture) + "u",
					Literal(field.rangeMin),
					Literal(field.rangeMax),
					Literal(field.tooltip),
					Literal(field.header),
				};
				specs.Append("\t\t\tnew(").Append(string.Join(", ", spec)).Append("),\n");
				push.Append("\t\tglobal::Shalltear.Interop.ShadowFields.Write(shadow + ").Append(offset).Append(", ").Append(access).Append(");\n");
				pull.Append("\t\tglobal::Shalltear.Interop.ShadowFields.Read(shadow + ").Append(offset).Append(", out ").Append(access).Append(");\n");
				offsets.Append("\t\t").Append(offset).Append(" = offsets[").Append(slot.ToString(CultureInfo.InvariantCulture)).Append("];\n");
			}
			foreach ((string name, StringBuilder body) in new[] { ("Push", push), ("Pull", pull) }) {
				text.Append("\n\tstatic void ").Append(name).Append(index).Append("(global::Shalltear.Behavior behavior, nint shadow) {\n");
				text.Append("\t\t").Append(component.qualified).Append(" script = (").Append(component.qualified).Append(")behavior;\n");
				text.Append(body).Append("\t}\n");
			}
			text.Append("\n\tstatic void Register").Append(index).Append("() {\n");
			text.Append("\t\tglobal::System.Span<int> offsets = stackalloc int[").Append(component.fields.Length).Append("];\n");
			text.Append("\t\tglobal::Shalltear.Interop.ScriptTypes.Register<").Append(component.qualified).Append(">(").Append(string.Join(", ", head));
			text.Append(", new global::Shalltear.Interop.ScriptField[] {\n").Append(specs);
			text.Append("\t\t}, offsets, Push").Append(index).Append(", Pull").Append(index).Append(");\n");
			text.Append(offsets).Append("\t}\n");
			calls.Append("\t\tRegister").Append(index).Append("();\n");
		}
		text.Append("\n\t[global::System.Runtime.CompilerServices.ModuleInitializer]\n");
		text.Append("\tinternal static void Register() {\n");
		text.Append("\t\tif (_registered || !global::Shalltear.Interop.ScriptTypes.IsAttached) {\n\t\t\treturn;\n\t\t}\n");
		text.Append("\t\t_registered = true;\n");
		text.Append(calls);
		text.Append("\t}\n}\n");
		return text.ToString();
	}
}
