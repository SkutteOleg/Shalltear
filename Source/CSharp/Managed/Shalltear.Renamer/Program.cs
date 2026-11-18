using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Mono.Cecil;
using Mono.Cecil.Cil;

namespace Shalltear.Renamer;

static class Program {
	const string ERROR = "Shalltear.Renamer : error SLT3001 : ";

	static int Main(string[] args) {
		Options? options = Options.Parse(args, out string problem);
		if (options == null) {
			Console.WriteLine(ERROR + problem);
			return 2;
		}
		try {
			Renaming renaming = new(options);
			renaming.Run();
			Console.WriteLine(renaming.Summary);
			return 0;
		} catch (Exception error) {
			Console.WriteLine(ERROR + error.Message);
			Console.WriteLine(error.StackTrace);
			return 1;
		}
	}
}

sealed class Options {
	public string outDir = "";
	public string? mapPath;
	public string? reportPath;
	public string? replacedPath;
	public string? copiesPath;
	public readonly List<string> inputs = new();
	public readonly List<string> renamed = new();
	public readonly List<string> references = new();

	public static Options? Parse(string[] args, out string problem) {
		Options options = new();
		problem = "";
		for (int index = 0; index < args.Length; ++index) {
			string argument = args[index];
			bool named = argument.StartsWith("--", StringComparison.Ordinal);
			if (named && index + 1 == args.Length) {
				problem = $"'{argument}' has no value. Pass one after it";
				return null;
			}
			switch (argument) {
			case "--out":
				options.outDir = args[++index];
				break;
			case "--map":
				options.mapPath = args[++index];
				break;
			case "--report":
				options.reportPath = args[++index];
				break;
			case "--replaced":
				options.replacedPath = args[++index];
				break;
			case "--copies":
				options.copiesPath = args[++index];
				break;
			case "--rename":
				options.renamed.Add(args[++index]);
				break;
			case "--refs-file":
				options.references.AddRange(File.ReadAllLines(args[++index]).Select(line => line.Trim()).Where(line => line.Length > 0));
				break;
			default:
				if (named) {
					problem = $"'{argument}' isn't an option. Run without arguments to list them";
					return null;
				}
				options.inputs.Add(Path.GetFullPath(argument));
				break;
			}
		}
		if (options.outDir.Length == 0 || options.inputs.Count == 0) {
			problem = "no output folder or input assembly. Pass --out <dir> [--map <file>] [--report <file>] [--replaced <file>] [--copies <file>] " +
					  "[--refs-file <file>] [--rename <assembly name>]... <assembly>...";
			return null;
		}
		return options;
	}
}

sealed class Renaming {
	const string KEEP_ATTRIBUTE = "Shalltear.KeepNameAttribute";
	const string FRIEND_ATTRIBUTE = "System.Runtime.CompilerServices.InternalsVisibleToAttribute";
	const string ACCESSOR_ATTRIBUTE = "System.Runtime.CompilerServices.UnsafeAccessorAttribute";
	const string GENERATED_ATTRIBUTE = "System.Runtime.CompilerServices.CompilerGeneratedAttribute";
	const string LETTERS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
	const string KEPT = "C# script name kept: ";

	static readonly string[] _keptNamespaces = ["System", "Microsoft", "Shalltear.Generated"];

	static readonly HashSet<string> _nameOnlyAttributes = new(StringComparer.Ordinal) {
		"System.Runtime.CompilerServices.TupleElementNamesAttribute",
		"System.Runtime.CompilerServices.CallerArgumentExpressionAttribute",
		"System.Runtime.CompilerServices.InterpolatedStringHandlerArgumentAttribute",
		"System.Diagnostics.DebuggerDisplayAttribute",
	};

	static readonly HashSet<string> _describingAttributes = new(StringComparer.Ordinal) {
		"System.Reflection.AssemblyCompanyAttribute",
		"System.Reflection.AssemblyConfigurationAttribute",
		"System.Reflection.AssemblyDescriptionAttribute",
		"System.Reflection.AssemblyInformationalVersionAttribute",
		"System.Reflection.AssemblyProductAttribute",
		"System.Reflection.AssemblyTitleAttribute",
	};

	static readonly Regex _identifier = new("[A-Za-z_][A-Za-z0-9_]*");

	sealed class SetResolver : DefaultAssemblyResolver {
		public void Add(AssemblyDefinition assembly) => RegisterAssembly(assembly);
	}

	readonly Options _options;
	readonly SetResolver _resolver = new();
	readonly List<ModuleDefinition> _modules = new();
	readonly HashSet<ModuleDefinition> _set = new();
	readonly HashSet<string> _setNames = new(StringComparer.Ordinal);
	readonly List<string> _replaced = new();
	readonly Dictionary<string, string> _typeMap = new(StringComparer.Ordinal);
	readonly List<(TypeDefinition type, string before, string fresh)> _typePlan = new();
	readonly List<TypeDefinition> _keptTypes = new();
	readonly Dictionary<TypeReference, string> _typeReferences = new(ReferenceEqualityComparer.Instance);
	readonly List<(MemberReference reference, IMemberDefinition target)> _memberReferences = new();
	readonly HashSet<MemberReference> _seenMembers = new(ReferenceEqualityComparer.Instance);
	readonly List<(MethodDefinition method, CustomAttribute attribute, IMemberDefinition target)> _accessors = new();
	readonly List<(CustomAttribute attribute, bool isField, int index, IMemberDefinition target)> _namedArguments = new();
	readonly HashSet<string> _reserved = new(StringComparer.Ordinal);
	readonly Dictionary<string, string> _virtualNames = new(StringComparer.Ordinal);
	readonly Dictionary<ModuleDefinition, string> _fileNames = new();
	readonly List<string> _map = new();
	readonly List<string> _report = new();
	readonly List<string> _written = new();
	int _shellCount;
	int _members;
	int _parameters;
	int _genericParameters;
	int _followed;

	public Renaming(Options options) {
		_options = options;
	}

	public string Summary =>
		$"C# script names renamed: {_typePlan.Count} type(s), {_members} member(s), {_parameters} parameter(s), {_genericParameters} generic parameter(s) " +
		$"in {_modules.Count - _shellCount} assembly(ies), {_followed} reference(s) followed";

	public void Run() {
		Load();
		PlanTypes();
		CollectReferences();
		ReserveVirtualNames();
		RenameDefinitions();
		RefreshTypeCollections();
		FollowReferences();
		RenameAssemblies();
		Write();
	}

	void Load() {
		_shellCount = _options.inputs.Count;
		List<string> inputs = new(_options.inputs);
		foreach (string name in _options.renamed) {
			string? match = _options.references.FirstOrDefault(path => string.Equals(Path.GetFileNameWithoutExtension(path), name, StringComparison.OrdinalIgnoreCase));
			if (match == null) {
				throw new InvalidOperationException($"no reference is named '{name}'. Pass its assembly in --refs-file");
			}
			_replaced.Add(match);
			inputs.Add(Path.GetFullPath(match));
		}
		foreach (string directory in inputs.Concat(_options.references).Select(path => Path.GetDirectoryName(Path.GetFullPath(path))!).Distinct(StringComparer.OrdinalIgnoreCase)) {
			_resolver.AddSearchDirectory(directory);
		}
		foreach (string path in inputs) {
			bool symbols = File.Exists(Path.ChangeExtension(path, ".pdb"));
			ModuleDefinition module;
			try {
				module = ModuleDefinition.ReadModule(path, new ReaderParameters {
					AssemblyResolver = _resolver,
					InMemory = true,
					// typeof arguments of attributes not renamed in deferred mode
					ReadingMode = ReadingMode.Immediate,
					ReadSymbols = symbols,
					SymbolReaderProvider = symbols ? new PortablePdbReaderProvider() : null,
				});
			} catch (BadImageFormatException error) {
				throw new InvalidOperationException($"'{path}' couldn't be read ({error.Message}). Pass only .NET assemblies");
			}
			_resolver.Add(module.Assembly);
			_modules.Add(module);
			_set.Add(module);
			_setNames.Add(module.Assembly.Name.Name);
		}
	}

	void PlanTypes() {
		HashSet<string> taken = new(StringComparer.Ordinal);
		foreach (ModuleDefinition module in _modules) {
			foreach (TypeDefinition type in module.Types) {
				taken.Add(type.FullName);
			}
		}
		int counter = 0;
		foreach (ModuleDefinition module in _modules) {
			foreach (TypeDefinition type in AllTypes(module)) {
				if (type.Name == "<Module>") {
					continue;
				}
				if (!ShouldRename(type)) {
					_keptTypes.Add(type);
					continue;
				}
				int tick = type.Name.LastIndexOf('`');
				string arity = tick > 0 ? type.Name.Substring(tick) : "";
				string fresh;
				do {
					fresh = ShortName(counter++) + arity;
				} while (!taken.Add(fresh));
				_typeMap[Key(module.Assembly.Name.Name, type.FullName)] = fresh;
				_typePlan.Add((type, type.FullName, fresh));
			}
		}
	}

	// collected from method bodies (generic member references differ per read)
	void CollectReferences() {
		foreach (ModuleDefinition module in _modules) {
			foreach (TypeReference reference in module.GetTypeReferences()) {
				CollectType(reference);
			}
			foreach (TypeDefinition type in AllTypes(module)) {
				foreach (MethodDefinition method in type.Methods) {
					NoteAccessor(method);
					foreach (MethodReference overridden in method.Overrides) {
						NoteMember(overridden);
					}
					if (!method.HasBody) {
						continue;
					}
					foreach (Instruction instruction in method.Body.Instructions) {
						if (instruction.Operand is MemberReference member && instruction.Operand is not TypeReference) {
							NoteMember(member);
						}
					}
				}
			}
			foreach (ICustomAttributeProvider provider in AttributeProviders(module)) {
				if (!provider.HasCustomAttributes) {
					continue;
				}
				foreach (CustomAttribute attribute in provider.CustomAttributes) {
					CollectAttribute(attribute);
				}
			}
		}
	}

	void CollectAttribute(CustomAttribute attribute) {
		if (!attribute.IsResolved) {
			return;
		}
		foreach (CustomAttributeArgument argument in attribute.ConstructorArguments) {
			CollectArgument(argument);
		}
		TypeDefinition? attributeType = TryResolve(attribute.AttributeType);
		for (int index = 0; index < attribute.Fields.Count; ++index) {
			CollectArgument(attribute.Fields[index].Argument);
			IMemberDefinition? target = FindMember(attributeType, attribute.Fields[index].Name, true);
			if (target != null) {
				_namedArguments.Add((attribute, true, index, target));
			}
		}
		for (int index = 0; index < attribute.Properties.Count; ++index) {
			CollectArgument(attribute.Properties[index].Argument);
			IMemberDefinition? target = FindMember(attributeType, attribute.Properties[index].Name, false);
			if (target != null) {
				_namedArguments.Add((attribute, false, index, target));
			}
		}
	}

	void ReserveVirtualNames() {
		HashSet<TypeDefinition> visited = new();
		foreach (ModuleDefinition module in _modules) {
			foreach (TypeDefinition type in AllTypes(module)) {
				ReserveOutside(type, visited);
			}
		}
		foreach (TypeDefinition type in _keptTypes) {
			foreach (MethodDefinition method in type.Methods) {
				_reserved.Add(method.Name);
			}
		}
	}

	void ReserveOutside(TypeDefinition type, HashSet<TypeDefinition> visited) {
		if (!visited.Add(type)) {
			return;
		}
		if (!_set.Contains(type.Module)) {
			foreach (MethodDefinition method in type.Methods) {
				if (method.IsVirtual) {
					_reserved.Add(method.Name);
				}
			}
		}
		List<TypeReference> ancestors = new();
		if (type.BaseType != null) {
			ancestors.Add(type.BaseType);
		}
		ancestors.AddRange(type.Interfaces.Select(implementation => implementation.InterfaceType));
		foreach (TypeReference ancestor in ancestors) {
			TypeDefinition? resolved = TryResolve(ancestor);
			if (resolved != null) {
				ReserveOutside(resolved, visited);
			}
		}
	}

	void RenameDefinitions() {
		foreach ((TypeDefinition type, string before, string fresh) in _typePlan) {
			string assembly = type.Module.Assembly.Name.Name;
			foreach (GenericParameter parameter in type.GenericParameters) {
				parameter.Name = "T" + parameter.Position;
				++_genericParameters;
			}
			bool isDelegate = type.BaseType != null && type.BaseType.FullName == "System.MulticastDelegate";
			Dictionary<string, string> memberNames = new(StringComparer.Ordinal);
			string Fresh(string old) {
				if (!memberNames.TryGetValue(old, out string? name)) {
					name = ShortName(memberNames.Count);
					memberNames[old] = name;
				}
				return name;
			}

			foreach (FieldDefinition field in type.Fields) {
				if (field.IsRuntimeSpecialName) {
					continue;
				}
				string old = field.Name;
				field.Name = Fresh(old);
				_map.Add($"member {assembly} {before}::{old} -> {field.Name}");
				++_members;
			}
			foreach (PropertyDefinition property in type.Properties) {
				string old = property.Name;
				property.Name = Fresh(old);
				_map.Add($"member {assembly} {before}::{old} -> {property.Name}");
				++_members;
			}
			foreach (EventDefinition single in type.Events) {
				string old = single.Name;
				single.Name = Fresh(old);
				_map.Add($"member {assembly} {before}::{old} -> {single.Name}");
				++_members;
			}
			Dictionary<string, string> literalNames = new(memberNames, StringComparer.Ordinal) {
				[StripArity(type.Name)] = StripArity(fresh),
			};
			foreach (MethodDefinition method in type.Methods) {
				RenameMethod(type, method, assembly, before, isDelegate, literalNames, Fresh);
			}
			type.Name = fresh;
			if (!type.IsNested) {
				type.Namespace = "";
			}
		}
		foreach ((TypeDefinition type, string before, _) in _typePlan) {
			_map.Add($"type {type.Module.Assembly.Name.Name} {before} -> {type.FullName}");
		}
		foreach (TypeDefinition type in _keptTypes.Where(type => _modules.IndexOf(type.Module) >= _shellCount && IsKeptByRequest(type))) {
			_report.Add($"{KEPT}type '{type.FullName}' ([KeepName])");
		}
		foreach (string name in _reserved.Where(name => _typePlan.Any(entry => entry.type.Methods.Any(method => method.Name == name && method.IsVirtual)))
					 .OrderBy(name => name, StringComparer.Ordinal)) {
			_report.Add($"{KEPT}method '{name}' (virtual, declared outside scripts)");
		}
		foreach (ModuleDefinition module in _modules) {
			foreach (ICustomAttributeProvider provider in AttributeProviders(module)) {
				if (provider.HasCustomAttributes) {
					RemoveAttributes(provider, _nameOnlyAttributes);
				}
			}
		}
	}

	void RenameMethod(TypeDefinition type, MethodDefinition method, string assembly, string before, bool isDelegate, Dictionary<string, string> literalNames,
					  Func<string, string> fresh) {
		foreach (GenericParameter parameter in method.GenericParameters) {
			parameter.Name = "M" + parameter.Position;
			++_genericParameters;
		}
		foreach (ParameterDefinition parameter in method.Parameters) {
			if (!string.IsNullOrEmpty(parameter.Name)) {
				parameter.Name = "";
				++_parameters;
			}
		}
		string old = method.Name;
		if (IsCompilerWrittenPrinter(type, method)) {
			RewriteLiterals(method, literalNames);
		}
		if (method.IsConstructor || method.IsRuntimeSpecialName || method.IsRuntime || isDelegate) {
			return;
		}
		if (method.IsSpecialName && old.StartsWith("op_", StringComparison.Ordinal)) {
			return;
		}
		if (method.IsVirtual && !(method.HasOverrides && method.IsPrivate)) {
			if (_reserved.Contains(old)) {
				return;
			}
			if (!_virtualNames.TryGetValue(old, out string? shared)) {
				shared = "v" + ShortName(_virtualNames.Count);
				_virtualNames[old] = shared;
				_map.Add($"virtual {old} -> {shared}");
			}
			method.Name = shared;
		} else {
			method.Name = "m" + fresh("()" + old);
			_map.Add($"member {assembly} {before}::{old}() -> {method.Name}");
		}
		++_members;
	}

	// types cached by (namespace, name) in each collection
	void RefreshTypeCollections() {
		foreach (ModuleDefinition module in _modules) {
			TypeDefinition[] types = module.Types.ToArray();
			module.Types.Clear();
			foreach (TypeDefinition type in types) {
				module.Types.Add(type);
			}
		}
	}

	void FollowReferences() {
		foreach ((TypeReference reference, string key) in _typeReferences) {
			if (!_typeMap.TryGetValue(key, out string? fresh)) {
				continue;
			}
			reference.Name = fresh;
			if (reference.DeclaringType == null) {
				reference.Namespace = "";
			}
			++_followed;
		}
		foreach ((MemberReference reference, IMemberDefinition target) in _memberReferences) {
			if (reference.Name != target.Name) {
				reference.Name = target.Name;
				++_followed;
			}
		}
		foreach ((MethodDefinition method, CustomAttribute attribute, IMemberDefinition target) in _accessors) {
			CustomAttributeNamedArgument name = new("Name", new CustomAttributeArgument(method.Module.TypeSystem.String, target.Name));
			int at = -1;
			for (int index = 0; index < attribute.Properties.Count; ++index) {
				if (attribute.Properties[index].Name == "Name") {
					at = index;
				}
			}
			if (at >= 0) {
				attribute.Properties[at] = name;
			} else {
				attribute.Properties.Add(name);
			}
			++_followed;
		}
		foreach ((CustomAttribute attribute, bool isField, int index, IMemberDefinition target) in _namedArguments) {
			Mono.Collections.Generic.Collection<CustomAttributeNamedArgument> list = isField ? attribute.Fields : attribute.Properties;
			if (list[index].Name != target.Name) {
				list[index] = new CustomAttributeNamedArgument(target.Name, list[index].Argument);
				++_followed;
			}
		}
	}

	void RenameAssemblies() {
		Dictionary<string, string> granted = FindGrantsFromOutside();
		Dictionary<string, string> assemblyMap = new(StringComparer.Ordinal);
		for (int index = _shellCount; index < _modules.Count; ++index) {
			ModuleDefinition module = _modules[index];
			string before = module.Assembly.Name.Name;
			if (granted.TryGetValue(before, out string? granter)) {
				_report.Add($"{KEPT}assembly '{before}' (internals from '{granter}')");
				continue;
			}
			string fresh = "s" + assemblyMap.Count;
			assemblyMap[before] = fresh;
			_map.Add($"assembly {before} -> {fresh}");
			module.Assembly.Name.Name = fresh;
			module.Name = fresh + ".dll";
			_fileNames[module] = fresh + ".dll";
			RemoveAttributes(module.Assembly, _describingAttributes);
			DropVersionResource(module);
			// enum types of attribute arguments resolved by new name while writing
			_resolver.Add(module.Assembly);
		}
		foreach (ModuleDefinition module in _modules) {
			foreach (AssemblyNameReference reference in module.AssemblyReferences) {
				if (assemblyMap.TryGetValue(reference.Name, out string? fresh)) {
					reference.Name = fresh;
				}
			}
			foreach (CustomAttribute attribute in module.Assembly.CustomAttributes.Where(attribute => attribute.AttributeType.FullName == FRIEND_ATTRIBUTE)) {
				if (attribute.ConstructorArguments.Count != 1 || attribute.ConstructorArguments[0].Value is not string friend) {
					continue;
				}
				int comma = friend.IndexOf(',');
				string name = (comma < 0 ? friend : friend.Substring(0, comma)).Trim();
				if (assemblyMap.TryGetValue(name, out string? fresh)) {
					attribute.ConstructorArguments[0] = new CustomAttributeArgument(attribute.ConstructorArguments[0].Type, fresh + (comma < 0 ? "" : friend.Substring(comma)));
				}
			}
		}
	}

	Dictionary<string, string> FindGrantsFromOutside() {
		Dictionary<string, string> granted = new(StringComparer.Ordinal);
		HashSet<string> setPaths = new(_options.inputs.Concat(_replaced).Select(Path.GetFullPath), StringComparer.OrdinalIgnoreCase);
		foreach (string path in _options.references) {
			if (setPaths.Contains(Path.GetFullPath(path)) || !File.Exists(path)) {
				continue;
			}
			AssemblyDefinition assembly;
			try {
				assembly = AssemblyDefinition.ReadAssembly(path, new ReaderParameters { ReadingMode = ReadingMode.Deferred });
			} catch (BadImageFormatException) {
				continue;
			}
			using AssemblyDefinition reading = assembly;
			foreach (CustomAttribute attribute in assembly.CustomAttributes.Where(attribute => attribute.AttributeType.FullName == FRIEND_ATTRIBUTE)) {
				if (attribute.ConstructorArguments.Count == 1 && attribute.ConstructorArguments[0].Value is string friend) {
					int comma = friend.IndexOf(',');
					string name = (comma < 0 ? friend : friend.Substring(0, comma)).Trim();
					if (_setNames.Contains(name)) {
						granted.TryAdd(name, assembly.Name.Name);
					}
				}
			}
		}
		return granted;
	}

	void Write() {
		Directory.CreateDirectory(_options.outDir);
		foreach (ModuleDefinition module in _modules) {
			string fileName = _fileNames.TryGetValue(module, out string? renamed) ? renamed : Path.GetFileName(module.FileName);
			string target = Path.Combine(_options.outDir, fileName);
			module.Write(target, new WriterParameters {
				WriteSymbols = module.HasSymbols,
				SymbolWriterProvider = module.HasSymbols ? new PortablePdbWriterProvider() : null,
				DeterministicMvid = true,
			});
			_written.Add(target);
		}
		_report.Insert(0, Summary);
		WriteLines(_options.mapPath, _map);
		WriteLines(_options.reportPath, _report);
		WriteLines(_options.replacedPath, _replaced);
		WriteLines(_options.copiesPath, _written.Skip(_shellCount));
	}

	static void WriteLines(string? path, IEnumerable<string> lines) {
		if (path != null) {
			Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
			File.WriteAllLines(path, lines);
		}
	}

	// version resource (old file name) copied by Cecil, with no switch
	static void DropVersionResource(ModuleDefinition module) {
		object? image = typeof(ModuleDefinition).GetField("Image", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance)?.GetValue(module);
		System.Reflection.FieldInfo? resources = image?.GetType().GetField("Win32Resources");
		if (image != null && resources != null) {
			resources.SetValue(image, Activator.CreateInstance(resources.FieldType));
		}
	}

	static void RemoveAttributes(ICustomAttributeProvider provider, HashSet<string> names) {
		for (int index = provider.CustomAttributes.Count - 1; index >= 0; --index) {
			if (names.Contains(provider.CustomAttributes[index].AttributeType.FullName)) {
				provider.CustomAttributes.RemoveAt(index);
			}
		}
	}

	static string Key(string assembly, string fullName) => assembly + "|" + fullName;

	static string StripArity(string name) {
		int tick = name.LastIndexOf('`');
		return tick > 0 ? name.Substring(0, tick) : name;
	}

	static string ShortName(int index) {
		string result = "";
		do {
			result = LETTERS[index % LETTERS.Length] + result;
			index = index / LETTERS.Length - 1;
		} while (index >= 0);
		return result;
	}

	static bool IsKeptByRequest(TypeDefinition type) {
		for (TypeDefinition? current = type; current != null; current = current.DeclaringType) {
			if (current.HasCustomAttributes && current.CustomAttributes.Any(attribute => attribute.AttributeType.FullName == KEEP_ATTRIBUTE)) {
				return true;
			}
		}
		return false;
	}

	static bool ShouldRename(TypeDefinition type) {
		TypeDefinition outer = type;
		while (outer.DeclaringType != null) {
			outer = outer.DeclaringType;
		}
		foreach (string kept in _keptNamespaces) {
			if (outer.Namespace == kept || outer.Namespace.StartsWith(kept + ".", StringComparison.Ordinal)) {
				return false;
			}
		}
		return !IsKeptByRequest(type);
	}

	static bool IsCompilerWrittenPrinter(TypeDefinition type, MethodDefinition method) {
		if ((method.Name != "ToString" && method.Name != "PrintMembers") || !method.HasBody) {
			return false;
		}
		return method.CustomAttributes.Any(attribute => attribute.AttributeType.FullName == GENERATED_ATTRIBUTE) ||
			   type.CustomAttributes.Any(attribute => attribute.AttributeType.FullName == GENERATED_ATTRIBUTE);
	}

	static void RewriteLiterals(MethodDefinition method, Dictionary<string, string> names) {
		foreach (Instruction instruction in method.Body.Instructions) {
			if (instruction.OpCode == OpCodes.Ldstr && instruction.Operand is string text) {
				instruction.Operand = _identifier.Replace(text, match => names.TryGetValue(match.Value, out string? fresh) ? fresh : match.Value);
			}
		}
	}

	static TypeDefinition? TryResolve(TypeReference reference) {
		try {
			return reference.Resolve();
		} catch (AssemblyResolutionException) {
			return null;
		}
	}

	void NoteMember(MemberReference? reference) {
		if (reference is MethodSpecification specification) {
			NoteMember(specification.ElementMethod);
			return;
		}
		if (reference == null || reference is IMemberDefinition || !_seenMembers.Add(reference)) {
			return;
		}
		IMemberDefinition? target = null;
		try {
			target = reference switch {
				MethodReference method => method.Resolve(),
				FieldReference field => field.Resolve(),
				_ => null,
			};
		} catch (AssemblyResolutionException) {
		}
		if (target != null && _set.Contains(target.DeclaringType.Module)) {
			_memberReferences.Add((reference, target));
		}
	}

	void NoteAccessor(MethodDefinition method) {
		if (!method.HasCustomAttributes || method.Parameters.Count == 0) {
			return;
		}
		foreach (CustomAttribute attribute in method.CustomAttributes) {
			if (attribute.AttributeType.FullName != ACCESSOR_ATTRIBUTE || !attribute.IsResolved || attribute.ConstructorArguments.Count != 1) {
				continue;
			}
			int kind = Convert.ToInt32(attribute.ConstructorArguments[0].Value);
			string name = method.Name;
			foreach (CustomAttributeNamedArgument property in attribute.Properties) {
				if (property.Name == "Name" && property.Argument.Value is string given) {
					name = given;
				}
			}
			TypeReference owner = method.Parameters[0].ParameterType;
			while (owner is TypeSpecification specification && owner is not GenericInstanceType) {
				owner = specification.ElementType;
			}
			TypeDefinition? declaring = TryResolve(owner);
			if (declaring == null || !_set.Contains(declaring.Module)) {
				continue;
			}
			IMemberDefinition? target = kind is 3 or 4	 ? declaring.Fields.FirstOrDefault(field => field.Name == name)
										: kind is 1 or 2 ? declaring.Methods.FirstOrDefault(candidate => candidate.Name == name)
														 : null;
			if (target != null) {
				_accessors.Add((method, attribute, target));
			}
		}
	}

	IMemberDefinition? FindMember(TypeDefinition? type, string name, bool isField) {
		for (int depth = 0; type != null && depth < 64; ++depth) {
			IMemberDefinition? found = isField ? type.Fields.FirstOrDefault(field => field.Name == name) : type.Properties.FirstOrDefault(property => property.Name == name);
			if (found != null) {
				return _set.Contains(type.Module) ? found : null;
			}
			type = type.BaseType == null ? null : TryResolve(type.BaseType);
		}
		return null;
	}

	void CollectType(TypeReference? reference) {
		switch (reference) {
		case null:
		case TypeDefinition:
		case GenericParameter:
			return;
		case GenericInstanceType generic:
			foreach (TypeReference argument in generic.GenericArguments) {
				CollectType(argument);
			}
			CollectType(generic.ElementType);
			return;
		case TypeSpecification specification:
			CollectType(specification.ElementType);
			return;
		}
		if (_typeReferences.ContainsKey(reference)) {
			return;
		}
		CollectType(reference.DeclaringType);
		TypeReference outer = reference;
		while (outer.DeclaringType != null) {
			outer = outer.DeclaringType;
		}
		if (outer.Scope is AssemblyNameReference assembly && _setNames.Contains(assembly.Name)) {
			_typeReferences[reference] = Key(assembly.Name, FullNameOf(reference));
		}
	}

	void CollectArgument(CustomAttributeArgument argument) {
		CollectType(argument.Type);
		switch (argument.Value) {
		case TypeReference type:
			CollectType(type);
			break;
		case CustomAttributeArgument boxed:
			CollectArgument(boxed);
			break;
		case CustomAttributeArgument[] many:
			foreach (CustomAttributeArgument each in many) {
				CollectArgument(each);
			}
			break;
		}
	}

	static string FullNameOf(TypeReference reference) {
		if (reference.DeclaringType != null) {
			return FullNameOf(reference.DeclaringType) + "/" + reference.Name;
		}
		return string.IsNullOrEmpty(reference.Namespace) ? reference.Name : reference.Namespace + "." + reference.Name;
	}

	static IEnumerable<TypeDefinition> AllTypes(ModuleDefinition module) {
		Stack<TypeDefinition> stack = new(module.Types.Reverse());
		while (stack.Count > 0) {
			TypeDefinition type = stack.Pop();
			yield return type;
			foreach (TypeDefinition nested in type.NestedTypes.Reverse()) {
				stack.Push(nested);
			}
		}
	}

	static IEnumerable<ICustomAttributeProvider> AttributeProviders(ModuleDefinition module) {
		yield return module.Assembly;
		yield return module;
		foreach (TypeDefinition type in AllTypes(module)) {
			yield return type;
			foreach (GenericParameter parameter in type.GenericParameters) {
				yield return parameter;
			}
			foreach (InterfaceImplementation implementation in type.Interfaces) {
				yield return implementation;
			}
			foreach (FieldDefinition field in type.Fields) {
				yield return field;
			}
			foreach (PropertyDefinition property in type.Properties) {
				yield return property;
			}
			foreach (EventDefinition single in type.Events) {
				yield return single;
			}
			foreach (MethodDefinition method in type.Methods) {
				yield return method;
				yield return method.MethodReturnType;
				foreach (ParameterDefinition parameter in method.Parameters) {
					yield return parameter;
				}
				foreach (GenericParameter parameter in method.GenericParameters) {
					yield return parameter;
				}
			}
		}
	}
}
