#include "CSharpNativeTypes.h"

#include "FieldKinds.h"
#include "ScriptBridge.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Reflection/Primitives.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Scene/Component.h"
#include "Shalltear/Core/Scene/SceneObject.h"

#include "Shalltear/Editor/ScriptProjectOps.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <limits>

namespace Shalltear::CSharpEditor::CSharpNativeTypes {

	namespace {

		namespace fs = std::filesystem;

		constexpr std::string_view Keywords =
			" abstract as base bool break byte case catch char checked class const continue decimal default delegate do double else enum event explicit "
			"extern false finally fixed float for foreach goto if implicit in int interface internal is lock long namespace new null object operator out "
			"override params private protected public readonly ref return sbyte sealed short sizeof stackalloc static string struct switch this throw true "
			"try typeof uint ulong unchecked unsafe ushort using virtual void volatile while ";

		// must match members of NativeComponent and its bases
		constexpr std::string_view Taken =
			" Owner IsEnabledSelf IsEffectivelyEnabled SetEnabled Destroy OnAttach OnDetach OnEnable OnDisable OnDestroy Equals GetHashCode ToString GetType "
			"MemberwiseClone Finalize Register RegisterWrapper Invoke Set SetInteger SetText SetObject GetBoolean GetInteger GetSingle GetDouble GetVec2 "
			"GetVec3 GetVec4 GetQuat GetColor GetText GetObject TYPE ";

		bool Lists(std::string_view words, std::string_view word) {
			return words.find(" " + str(word) + " ") != std::string_view::npos;
		}

		struct Name {
			str space;
			str type;
			bool valid = false;
		};

		bool IsIdentifier(std::string_view text) {
			if (text.empty() || std::isdigit(static_cast<unsigned char>(text.front()))) {
				return false;
			}
			if (!std::all_of(text.begin(), text.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; })) {
				return false;
			}
			return !Lists(Keywords, text);
		}

		Name Split(std::string_view qualified) {
			Name name;
			name.valid = true;
			size_t at = 0;
			while (true) {
				const size_t next = qualified.find("::", at);
				const std::string_view part = qualified.substr(at, next == std::string_view::npos ? next : next - at);
				if (!IsIdentifier(part)) {
					name.valid = false;
					return name;
				}
				if (next == std::string_view::npos) {
					name.type = part;
					return name;
				}
				name.space += name.space.empty() ? "" : ".";
				name.space += part;
				at = next + 2;
			}
		}

		str Global(const Name& name) {
			return "global::" + (name.space.empty() ? name.type : name.space + "." + name.type);
		}

		str Pascal(std::string_view field) {
			while (!field.empty() && field.front() == '_') {
				field.remove_prefix(1);
			}
			if (field.starts_with("m_")) {
				field.remove_prefix(2);
			}
			str name(field);
			if (!name.empty()) {
				name.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(name.front())));
			}
			return name;
		}

		struct Member {
			str text;
			str name;
		};

		struct Wrapper {
			const TypeInfo* type = nullptr;
			Name name;
			Vector<Member> members{GetMemoryResource(MemTag::Editor)};
		};

		const char* IntegerType(i32 kind) {
			switch (kind) {
			case SLT_KIND_I8:
				return "sbyte";
			case SLT_KIND_U8:
				return "byte";
			case SLT_KIND_I16:
				return "short";
			case SLT_KIND_U16:
				return "ushort";
			case SLT_KIND_I32:
				return "int";
			case SLT_KIND_U32:
				return "uint";
			case SLT_KIND_I64:
				return "long";
			default:
				return "ulong";
			}
		}

		const char* EnumBase(const TypeInfo& type) {
			if (type.size > sizeof(i32)) {
				return "long";
			}
			for (u32 index = 0; index < type.enumConstantCount; ++index) {
				if (type.enumConstants[index].value > std::numeric_limits<i32>::max()) {
					return "uint";
				}
			}
			return "int";
		}

		struct ValueKind {
			i32 kind;
			const char* type;
			const char* getter;
		};
		constexpr ValueKind ValueKinds[] = {
			{SLT_KIND_BOOL, "bool", "GetBoolean"},
			{SLT_KIND_F32, "float", "GetSingle"},
			{SLT_KIND_F64, "double", "GetDouble"},
			{SLT_KIND_VEC2, "global::Shalltear.Vec2", "GetVec2"},
			{SLT_KIND_VEC3, "global::Shalltear.Vec3", "GetVec3"},
			{SLT_KIND_VEC4, "global::Shalltear.Vec4", "GetVec4"},
			{SLT_KIND_QUAT, "global::Shalltear.Quat", "GetQuat"},
			{SLT_KIND_COLOR, "global::Shalltear.Color", "GetColor"},
		};

		str Property(std::string_view type, std::string_view name, const str& getter, const str& setter, bool readOnly) {
			str text = std::format("\t\tpublic {} {} {{\n\t\t\tget => {};\n", type, name, getter);
			if (!readOnly) {
				text += std::format("\t\t\tset => {};\n", setter);
			}
			return text + "\t\t}\n";
		}

		str DescribeField(const FieldInfo& field, std::string_view name, std::span<const Wrapper> wrappers, Vector<const TypeInfo*>& enums) {
			const str hash = std::format("0x{:016x}UL", field.name.GetValue());
			const bool readOnly = (field.flags & FieldFlagReadOnly) != 0;
			if (field.container || !field.fieldType) {
				return {};
			}
			for (const ValueKind& value : ValueKinds) {
				if (field.fieldType == CSharpImpl::FieldKinds::Find(value.kind)->type) {
					return Property(value.type, name, std::format("{}(TYPE, {})", value.getter, hash), std::format("Set(TYPE, {}, value)", hash), readOnly);
				}
			}
			for (i32 kind = SLT_KIND_I8; kind <= SLT_KIND_U64; ++kind) {
				if (field.fieldType == CSharpImpl::FieldKinds::Find(kind)->type) {
					return Property(IntegerType(kind), name, std::format("({})GetInteger(TYPE, {}, {})", IntegerType(kind), hash, kind),
									std::format("SetInteger(TYPE, {}, {}, (long)value)", hash, kind), readOnly);
				}
			}
			if (field.fieldType == CSharpImpl::FieldKinds::GetText().type) {
				return Property("string", name, std::format("GetText(TYPE, {})", hash), std::format("SetText(TYPE, {}, value)", hash), readOnly);
			}
			if (field.fieldType->flags & TypeFlagEnum) {
				const Name enumName = Split(field.fieldType->displayName ? field.fieldType->displayName : "");
				if (!enumName.valid || field.size > sizeof(i64) || field.fieldType->enumConstantCount == 0) {
					return {};
				}
				if (std::find(enums.begin(), enums.end(), field.fieldType) == enums.end()) {
					enums.push_back(field.fieldType);
				}
				return Property(Global(enumName), name, std::format("({})GetInteger(TYPE, {}, {})", Global(enumName), hash, static_cast<i32>(SLT_KIND_ENUM)),
								std::format("SetInteger(TYPE, {}, {}, (long)value)", hash, static_cast<i32>(SLT_KIND_ENUM)), readOnly);
			}
			if (field.fieldType->flags & TypeFlagHandle) {
				const TypeInfo* pointee = TypeRegistry::FindType(HandlePointeeName(*field.fieldType));
				str type = "global::Shalltear.EngineObject";
				const auto wrapped = std::find_if(wrappers.begin(), wrappers.end(), [pointee](const Wrapper& wrapper) { return wrapper.type == pointee; });
				if (wrapped != wrappers.end()) {
					type = Global(wrapped->name);
				} else if (pointee == SceneObject::GetReflectType()) {
					type = "global::Shalltear.SceneObject";
				} else if (pointee && TypeRegistry::IsDerivedFrom(pointee, Component::GetReflectType())) {
					type = "global::Shalltear.Component";
				}
				return Property(type + "?", name, std::format("GetObject<{}>(TYPE, {})", type, hash), std::format("SetObject(TYPE, {}, value)", hash), readOnly);
			}
			return {};
		}

		void Describe(Wrapper& wrapper, std::span<const Wrapper> wrappers, Vector<const TypeInfo*>& enums) {
			const auto taken = [&wrapper](std::string_view name) {
				return name == wrapper.name.type || Lists(Taken, name) ||
					   std::any_of(wrapper.members.begin(), wrapper.members.end(), [name](const Member& member) { return member.name == name; });
			};
			for (const TypeInfo* type = wrapper.type; type && type->ownerModule == wrapper.type->ownerModule; type = type->parent) {
				for (u32 index = 0; index < type->fieldCount; ++index) {
					const FieldInfo& field = type->fields[index];
					const str name = Pascal(field.displayName ? field.displayName : "");
					if (!IsIdentifier(name) || taken(name)) {
						continue;
					}
					str text = DescribeField(field, name, wrappers, enums);
					if (!text.empty()) {
						wrapper.members.push_back({std::move(text), name});
					}
				}
				for (u32 index = 0; index < type->methodCount; ++index) {
					const MethodInfo& method = type->methods[index];
					const str name = method.displayName ? method.displayName : "";
					if (method.paramCount != 0 || !IsIdentifier(name) || taken(name)) {
						continue;
					}
					wrapper.members.push_back({std::format("\t\tpublic void {}() {{\n\t\t\tInvoke(TYPE, 0x{:016x}UL);\n\t\t}}\n", name, method.name.GetValue()), name});
				}
			}
		}

	} // anonymous namespace

	void Collect(const void* moduleBase, Vector<const TypeInfo*>& out) {
		out.clear();
		if (!moduleBase) {
			return;
		}
		struct Query {
			const void* owner;
			Vector<const TypeInfo*>* out;
		};
		Query query{moduleBase, &out};
		TypeRegistry::ForEachType(
			[](const TypeInfo* type, void* user) {
				const Query& query = *static_cast<const Query*>(user);
				if (type->ownerModule == query.owner && (type->flags & TypeFlagComponent)) {
					query.out->push_back(type);
				}
			},
			&query);
	}

	str Render(std::span<const TypeInfo* const> types) {
		Vector<Wrapper> wrappers{GetMemoryResource(MemTag::Editor)};
		for (const TypeInfo* type : types) {
			Wrapper wrapper;
			wrapper.type = type;
			wrapper.name = Split(type && type->displayName ? type->displayName : "");
			if (type && wrapper.name.valid && StringHash(type->displayName) == type->name) {
				wrappers.push_back(std::move(wrapper));
			}
		}
		if (wrappers.empty()) {
			return {};
		}
		std::sort(wrappers.begin(), wrappers.end(), [](const Wrapper& left, const Wrapper& right) {
			return left.name.space != right.name.space ? left.name.space < right.name.space : left.name.type < right.name.type;
		});
		Vector<const TypeInfo*> enums{GetMemoryResource(MemTag::Editor)};
		for (Wrapper& wrapper : wrappers) {
			Describe(wrapper, wrappers, enums);
		}

		str body;
		str space;
		bool opened = false;
		const auto open = [&](const str& wanted) {
			if (opened && wanted == space) {
				return;
			}
			if (opened && !space.empty()) {
				body += "}\n\n";
			}
			if (!wanted.empty()) {
				body += "namespace " + wanted + " {\n";
			}
			space = wanted;
			opened = true;
		};
		std::sort(enums.begin(), enums.end(), [](const TypeInfo* left, const TypeInfo* right) { return str(left->displayName) < str(right->displayName); });
		for (const TypeInfo* declared : enums) {
			const Name name = Split(declared->displayName);
			open(name.space);
			body += std::format("\tpublic enum {} : {} {{\n", name.type, EnumBase(*declared));
			for (u32 index = 0; index < declared->enumConstantCount; ++index) {
				const EnumConstantInfo& constant = declared->enumConstants[index];
				if (constant.displayName && IsIdentifier(constant.displayName)) {
					body += std::format("\t\t{} = {},\n", constant.displayName, constant.value);
				}
			}
			body += "\t}\n\n";
		}
		str registrations;
		for (const Wrapper& wrapper : wrappers) {
			open(wrapper.name.space);
			body += std::format("\t[global::Shalltear.Interop.EngineType(\"{}\")]\n", wrapper.type->displayName);
			body += std::format("\tpublic sealed class {} : global::Shalltear.Interop.NativeComponent {{\n", wrapper.name.type);
			body += std::format("\t\tconst ulong TYPE = 0x{:016x}UL;\n\n\t\t{}() {{}}\n", wrapper.type->name.GetValue(), wrapper.name.type);
			for (const Member& member : wrapper.members) {
				body += "\n" + member.text;
			}
			body += std::format("\n\t\tinternal static void RegisterWrapper() {{\n\t\t\tRegister(\"{}\", static () => new {}());\n\t\t}}\n\t}}\n\n",
								wrapper.type->displayName, wrapper.name.type);
			registrations += std::format("\t\t\t{}.RegisterWrapper();\n", Global(wrapper.name));
		}
		if (!space.empty()) {
			body += "}\n\n";
		}
		return "// <auto-generated/>\n#nullable enable\n\n" + body +
			   "namespace Shalltear.Generated {\n\tstatic partial class TypeTable {\n\t\tstatic partial void RegisterNativeTypes() {\n" + registrations +
			   "\t\t}\n\t}\n}\n";
	}

	bool Write(const str& projectPath, std::span<const TypeInfo* const> types) {
		const fs::path file = fs::path(projectPath) / GeneratedFile;
		const str text = Render(types);
		std::error_code ec;
		const bool exists = fs::is_regular_file(file, ec);
		if (text.empty()) {
			return exists && fs::remove(file, ec);
		}
		if (exists && FileSystem::ReadTextFile(Path(file.generic_string()), MemTag::Editor) == text) {
			return false;
		}
		fs::create_directories(file.parent_path(), ec);
		if (!FileSystem::WriteTextFile(Path(file.generic_string()), text)) {
			SLT_LOG_WARN("C#", "couldn't write '{}' (C# scripts won't see the project's C++ components)", file.generic_string());
			return false;
		}
		return true;
	}

	bool Refresh(const str& projectPath) {
		const void* moduleBase = ScriptProjectOps::GetLoadedModuleBase();
		if (!moduleBase && ScriptProjectOps::HasScriptProject(projectPath)) {
			return false;
		}
		Vector<const TypeInfo*> types{GetMemoryResource(MemTag::Editor)};
		Collect(moduleBase, types);
		return Write(projectPath, types);
	}

} // namespace Shalltear::CSharpEditor::CSharpNativeTypes
