using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

namespace Shalltear.Renamer;

internal static class WasmCallConvNormalizer {
	sealed class DirResolver : DefaultAssemblyResolver {
		public DirResolver(string dir) {
			AddSearchDirectory(dir);
		}
	}

	public static int Run(string dir, MethodCallingConvention targetCallConv) {
		if (!Directory.Exists(dir)) {
			Console.Error.WriteLine($"normalize-wasm-callconv: directory not found: '{dir}'");
			return 2;
		}

		string fullDir = Path.GetFullPath(dir);
		DirResolver resolver = new(fullDir);
		int filesRewritten = 0;
		int sitesRewritten = 0;
		int pointersRewritten = 0;

		foreach (string path in Directory.EnumerateFiles(fullDir, "*.dll", SearchOption.TopDirectoryOnly)
			.OrderBy(static p => Path.GetFileName(p), StringComparer.OrdinalIgnoreCase)) {
			ModuleDefinition module;
			try {
				module = ModuleDefinition.ReadModule(path, new ReaderParameters {
					AssemblyResolver = resolver,
					InMemory = true,
					ReadingMode = ReadingMode.Immediate,
				});
			} catch (BadImageFormatException) {
				continue;
			}

			using (module) {
				int moduleSites = 0;
				int modulePtrs = 0;
				HashSet<TypeReference> visited = new(ReferenceEqualityComparer.Instance);

				foreach (TypeDefinition type in AllTypes(module)) {
					foreach (FieldDefinition field in type.Fields) {
						field.FieldType = RewriteType(field.FieldType, targetCallConv, visited, ref modulePtrs);
					}
					foreach (PropertyDefinition prop in type.Properties) {
						prop.PropertyType = RewriteType(prop.PropertyType, targetCallConv, visited, ref modulePtrs);
					}
					foreach (EventDefinition ev in type.Events) {
						ev.EventType = RewriteType(ev.EventType, targetCallConv, visited, ref modulePtrs);
					}
					foreach (MethodDefinition method in type.Methods) {
						method.ReturnType = RewriteType(method.ReturnType, targetCallConv, visited, ref modulePtrs);
						foreach (ParameterDefinition param in method.Parameters) {
							param.ParameterType = RewriteType(param.ParameterType, targetCallConv, visited, ref modulePtrs);
						}
						if (!method.HasBody) {
							continue;
						}
						foreach (VariableDefinition variable in method.Body.Variables) {
							variable.VariableType = RewriteType(variable.VariableType, targetCallConv, visited, ref modulePtrs);
						}
						foreach (Instruction instruction in method.Body.Instructions) {
							if (instruction.Operand is CallSite site) {
								if (RewriteCallSite(site, targetCallConv, visited, ref modulePtrs)) {
									++moduleSites;
								}
							} else if (instruction.Operand is FieldReference fieldRef) {
								fieldRef.FieldType = RewriteType(fieldRef.FieldType, targetCallConv, visited, ref modulePtrs);
							} else if (instruction.Operand is MethodReference methodRef) {
								methodRef.ReturnType = RewriteType(methodRef.ReturnType, targetCallConv, visited, ref modulePtrs);
								foreach (ParameterDefinition param in methodRef.Parameters) {
									param.ParameterType = RewriteType(param.ParameterType, targetCallConv, visited, ref modulePtrs);
								}
							} else if (instruction.Operand is TypeReference typeRef) {
								TypeReference rewritten = RewriteType(typeRef, targetCallConv, visited, ref modulePtrs);
								if (!ReferenceEquals(rewritten, typeRef)) {
									instruction.Operand = rewritten;
								}
							}
						}
					}
				}

				if (moduleSites > 0 || modulePtrs > 0) {
					module.Write(path, new WriterParameters {
						DeterministicMvid = true,
					});
					++filesRewritten;
					sitesRewritten += moduleSites;
					pointersRewritten += modulePtrs;
				}
			}
		}

		Console.WriteLine($"WASM callconv normalized: {filesRewritten} assembly(ies), {sitesRewritten} calli site(s), {pointersRewritten} function pointer(s) -> {targetCallConv}");
		return 0;
	}

	static bool IsNonDefaultUnmanaged(MethodCallingConvention conv) {
		return conv is MethodCallingConvention.Unmanaged
			or MethodCallingConvention.C
			or MethodCallingConvention.StdCall
			or MethodCallingConvention.ThisCall
			or MethodCallingConvention.FastCall;
	}

	static bool IsCallConvModifier(TypeReference modifier) {
		return modifier.Namespace == "System.Runtime.CompilerServices" &&
			   modifier.Name.StartsWith("CallConv", StringComparison.Ordinal);
	}

	static TypeReference StripCallConvModifiers(TypeReference type, out bool stripped) {
		stripped = false;
		while (type is OptionalModifierType opt && IsCallConvModifier(opt.ModifierType)) {
			type = opt.ElementType;
			stripped = true;
		}
		return type;
	}

	static bool RewriteCallSite(CallSite site, MethodCallingConvention target, HashSet<TypeReference> visited, ref int ptrs) {
		bool changed = false;
		site.ReturnType = StripCallConvModifiers(site.ReturnType, out bool strippedRet);
		if (strippedRet) {
			changed = true;
		}
		site.ReturnType = RewriteType(site.ReturnType, target, visited, ref ptrs);
		foreach (ParameterDefinition param in site.Parameters) {
			param.ParameterType = StripCallConvModifiers(param.ParameterType, out bool strippedParam);
			if (strippedParam) {
				changed = true;
			}
			param.ParameterType = RewriteType(param.ParameterType, target, visited, ref ptrs);
		}
		if (IsNonDefaultUnmanaged(site.CallingConvention) && site.CallingConvention != target) {
			site.CallingConvention = target;
			changed = true;
		}
		return changed;
	}

	static TypeReference RewriteType(TypeReference type, MethodCallingConvention target, HashSet<TypeReference> visited, ref int ptrs) {
		if (type == null || !visited.Add(type)) {
			return type!;
		}
		switch (type) {
		case FunctionPointerType fn: {
			bool changed = false;
			fn.ReturnType = StripCallConvModifiers(fn.ReturnType, out bool strippedRet);
			if (strippedRet) {
				changed = true;
			}
			fn.ReturnType = RewriteType(fn.ReturnType, target, visited, ref ptrs);
			foreach (ParameterDefinition param in fn.Parameters) {
				param.ParameterType = StripCallConvModifiers(param.ParameterType, out bool strippedParam);
				if (strippedParam) {
					changed = true;
				}
				param.ParameterType = RewriteType(param.ParameterType, target, visited, ref ptrs);
			}
			if (IsNonDefaultUnmanaged(fn.CallingConvention) && fn.CallingConvention != target) {
				fn.CallingConvention = target;
				changed = true;
			}
			if (changed) {
				++ptrs;
			}
			return fn;
		}
		case GenericInstanceType generic:
			for (int i = 0; i < generic.GenericArguments.Count; ++i) {
				generic.GenericArguments[i] = RewriteType(generic.GenericArguments[i], target, visited, ref ptrs);
			}
			return generic;
		case PointerType ptr: {
			TypeReference elem = RewriteType(ptr.ElementType, target, visited, ref ptrs);
			return ReferenceEquals(elem, ptr.ElementType) ? ptr : new PointerType(elem);
		}
		case ByReferenceType byRef: {
			TypeReference elem = RewriteType(byRef.ElementType, target, visited, ref ptrs);
			return ReferenceEquals(elem, byRef.ElementType) ? byRef : new ByReferenceType(elem);
		}
		case ArrayType array: {
			TypeReference elem = RewriteType(array.ElementType, target, visited, ref ptrs);
			if (ReferenceEquals(elem, array.ElementType)) {
				return array;
			}
			ArrayType updated = new(elem);
			if (array.IsVector) {
				return updated;
			}
			updated.Dimensions.Clear();
			foreach (ArrayDimension dim in array.Dimensions) {
				updated.Dimensions.Add(dim);
			}
			return updated;
		}
		case OptionalModifierType opt: {
			TypeReference elem = RewriteType(opt.ElementType, target, visited, ref ptrs);
			return ReferenceEquals(elem, opt.ElementType) ? opt : new OptionalModifierType(opt.ModifierType, elem);
		}
		case RequiredModifierType req: {
			TypeReference elem = RewriteType(req.ElementType, target, visited, ref ptrs);
			return ReferenceEquals(elem, req.ElementType) ? req : new RequiredModifierType(req.ModifierType, elem);
		}
		case PinnedType pinned: {
			TypeReference elem = RewriteType(pinned.ElementType, target, visited, ref ptrs);
			return ReferenceEquals(elem, pinned.ElementType) ? pinned : new PinnedType(elem);
		}
		default:
			return type;
		}
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
}
