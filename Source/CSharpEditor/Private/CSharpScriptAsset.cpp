#include "CSharpScriptAsset.h"

#include "CSharpAuthoring.h"
#include "CSharpProjectFiles.h"
#include "CSharpReload.h"
#include "ManagedTypes.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Asset/AssetImporter.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/StringUtil.h"

#include "Shalltear/Editor/AssetBrowserOps.h"
#include "Shalltear/Editor/ProjectOps.h"
#include "Shalltear/Editor/ScriptProjectOps.h"

#include <algorithm>
#include <filesystem>

namespace Shalltear::CSharpEditor {

	bool CSharpScriptAsset::Import(const Path& sourcePath) {
		return FileSystem::Exists(sourcePath);
	}

	namespace {

		namespace fs = std::filesystem;

		constexpr std::string_view ProjectScheme = "project://";

		constexpr std::string_view Keywords[] = {
			"abstract",
			"as",
			"base",
			"bool",
			"break",
			"byte",
			"case",
			"catch",
			"char",
			"checked",
			"class",
			"const",
			"continue",
			"decimal",
			"default",
			"delegate",
			"do",
			"double",
			"else",
			"enum",
			"event",
			"explicit",
			"extern",
			"false",
			"finally",
			"fixed",
			"float",
			"for",
			"foreach",
			"goto",
			"if",
			"implicit",
			"in",
			"int",
			"interface",
			"internal",
			"is",
			"lock",
			"long",
			"namespace",
			"new",
			"null",
			"object",
			"operator",
			"out",
			"override",
			"params",
			"private",
			"protected",
			"public",
			"readonly",
			"ref",
			"return",
			"sbyte",
			"sealed",
			"short",
			"sizeof",
			"stackalloc",
			"static",
			"string",
			"struct",
			"switch",
			"this",
			"throw",
			"true",
			"try",
			"typeof",
			"uint",
			"ulong",
			"unchecked",
			"unsafe",
			"ushort",
			"using",
			"virtual",
			"void",
			"volatile",
			"while",
		};

		class CSharpScriptImporter final : public IAssetImporter {
		public:
			StringHash GetAssetType() const override { return CSharpScriptAsset::GetReflectType()->name; }

			Asset* Import(const Path& sourcePath) override {
				CSharpScriptAsset* asset = New<CSharpScriptAsset>(MemTag::Asset);
				if (!asset->Import(sourcePath)) {
					Delete(asset);
					return nullptr;
				}
				return asset;
			}
		};

		CSharpScriptImporter& GetImporter() {
			static CSharpScriptImporter importer;
			return importer;
		}

		str RelativePathOf(const Guid& script) {
			const str path = AssetDatabase::LookupPath(script).ToString();
			return path.starts_with(ProjectScheme) ? StringUtil::ToLower(std::string_view(path).substr(ProjectScheme.size())) : str();
		}

		struct ComponentQuery {
			str script;
			Vector<const TypeInfo*>* out = nullptr;
		};

		void CollectComponent(const CSharpImpl::ManagedType& type, void* user) {
			const ComponentQuery& query = *static_cast<const ComponentQuery*>(user);
			if (!type.script.empty() && StringUtil::ToLower(type.script) == query.script) {
				query.out->push_back(type.info);
			}
		}

	} // anonymous namespace

	namespace CSharpScripts {

		void RegisterImporter(const void* owner) {
			AssetImporterRegistry::Register(GetImporter().GetAssetType(), &GetImporter(), {".cs"}, owner);
		}

		void GetComponents(const Guid& script, Vector<const TypeInfo*>& out) {
			out.clear();
			ComponentQuery query{RelativePathOf(script), &out};
			if (!query.script.empty()) {
				CSharpImpl::ManagedTypes::ForEachType(&CollectComponent, &query);
			}
		}

		const TypeInfo* ResolveComponent(const Guid& script) {
			Vector<const TypeInfo*> components{GetMemoryResource(MemTag::Editor)};
			GetComponents(script, components);
			std::erase_if(components, [](const TypeInfo* type) { return (type->flags & TypeFlagAbstract) != 0; });

			const str file = AssetDatabase::LookupPath(script).GetFilename();
			const str stem = fs::path(file).stem().generic_string();
			for (const TypeInfo* type : components) {
				if (stem == type->displayName) {
					return type;
				}
			}
			if (components.size() == 1) {
				return components.front();
			}
			if (!components.empty()) {
				SLT_LOG_WARN("C#", "'{}' has {} components, none named '{}'. Pick one in Add Component", file, components.size(), stem);
				return nullptr;
			}
			const CSharpCompile& compile = CSharpAuthoring::GetCompile();
			if (compile.IsBusy()) {
				SLT_LOG_WARN("C#", "'{}' is still compiling. Drop it again when compile ends", file);
			} else if (compile.GetState() == CompileState::Failed) {
				SLT_LOG_WARN("C#", "'{}' isn't compiled (scripts have errors). Fix them, drop it again", file);
			} else if (!CSharpReload::IsLoaded()) {
				SLT_LOG_WARN("C#", "'{}' isn't loaded yet (C# scripts still starting). Drop it again in a moment", file);
			} else {
				SLT_LOG_WARN("C#", "'{}' has no component to add. Derive a class from Behavior in it", file);
			}
			return nullptr;
		}

		bool IsValidName(std::string_view name) {
			return ScriptProjectOps::IsValidClassName(name) && std::find(std::begin(Keywords), std::end(Keywords), name) == std::end(Keywords);
		}

		Guid Create(const Path& virtualDir, const str& name, Template kind) {
			if (!IsValidName(name)) {
				SLT_LOG_ERROR("C#", "'{}' can't be a C# class name. Use letters/digits/_ (no leading digit, no keyword)", name);
				return {};
			}
			str directory = virtualDir.ToString().empty() ? AssetBrowserOps::GetDefaultCreateFolder().ToString() : virtualDir.ToString();
			if (directory.empty()) {
				SLT_LOG_ERROR("C#", "couldn't create '{}.cs' (no project is open)", name);
				return {};
			}
			FileSystem::CreateDirectory(Path(directory));
			const Path target(directory + (directory.back() == '/' ? "" : "/") + name + ".cs");
			if (FileSystem::Exists(target)) {
				SLT_LOG_ERROR("C#", "'{}' already exists. Pick another name", target.GetFilename());
				return {};
			}

			Vector<ScriptProjectOps::TemplateToken> tokens{GetMemoryResource(MemTag::Editor)};
			tokens.push_back({"CLASS_NAME", name});
			tokens.push_back({"NAMESPACE", CSharpProjectFiles::GetRootNamespace(ProjectOps::GetMountedProjectPath())});
			str text;
			const char* source = kind == Template::Component ? "CSharpScript.cs" : "CSharpClass.cs";
			if (!ScriptProjectOps::RenderTemplate(source, tokens, text) || !FileSystem::WriteTextFile(target, text)) {
				SLT_LOG_ERROR("C#", "couldn't write '{}'", target.ToString());
				return {};
			}
			Guid created = AssetDatabase::EnsureAssetIdentity(target);
			if (!created.IsValid()) {
				AssetDatabase::ScanProjectDirectory();
				created = AssetDatabase::LookupGuid(target);
			}
			CSharpAuthoring::NotifySourceChanged();
			return created;
		}

		bool Open(const Guid& script) {
			const Path physical = FileSystem::Resolve(AssetDatabase::LookupPath(script));
			return !physical.ToString().empty() && AssetBrowserOps::OpenInExternalEditor(physical);
		}

	} // namespace CSharpScripts

} // namespace Shalltear::CSharpEditor

SLT_REGISTER_TYPE(Shalltear::CSharpEditor::CSharpScriptAsset)
