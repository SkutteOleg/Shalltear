#include "Shalltear/Editor/AssetBrowserOps.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/ProjectLayout.h"

#include "Shalltear/Core/Asset/AssetImporter.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Graphics/MaterialAsset.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Scene/Prefab.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/StringUtil.h"

#include "Shalltear/Editor/CSharpTranspiler.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/ProjectOps.h"
#include "Shalltear/Editor/ScriptProjectOps.h"

#include "GameModuleHost.h"
#include "PackageHost.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace Shalltear::AssetBrowserOps {

	namespace {

		namespace fs = std::filesystem;

		Path ResolvePhysical(const Path& virtualPath) {
			str probe = virtualPath.ToString();
			if (probe.empty()) {
				return {};
			}
			probe += probe.back() == ':' ? "//_" : "/_";
			Path resolved = FileSystem::Resolve(Path(probe));
			if (resolved.ToString().find("://") != str::npos) {
				return {};
			}
			return Path(resolved.GetDirectory());
		}

		str JoinVirtual(const Path& virtualDir, const str& name) {
			str directory = virtualDir.ToString();
			return directory + (!directory.empty() && directory.back() == ':' ? "//" : "/") + name;
		}

		fs::path ResolveProjectRoot() {
			Path resolved = ResolvePhysical(Path("project://"));
			if (resolved.ToString().empty()) {
				return {};
			}
			std::error_code ec;
			fs::path root{resolved.ToString()};
			if (!fs::is_directory(root, ec) || ec) {
				return {};
			}
			return root;
		}

		char ToLowerAscii(char c) {
			return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
		}

		bool NameLess(const str& a, const str& b) {
			for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
				char ca = ToLowerAscii(a[i]);
				char cb = ToLowerAscii(b[i]);
				if (ca != cb) {
					return ca < cb;
				}
			}
			return a.size() < b.size();
		}

		bool IsValidEntryName(const str& name) {
			if (name.empty() || name == "." || name == "..") {
				return false;
			}
			for (char c : name) {
				if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
					c == '"' || c == '<' || c == '>' || c == '|') {
					return false;
				}
			}
			return true;
		}

		str VirtualPathOf(const str& relPath) {
			return relPath.empty() ? str("project://") : "project://" + relPath;
		}

		str RelativeOf(const str& virtualPath) {
			size_t colon = virtualPath.find(':');
			if (colon == str::npos) {
				return virtualPath;
			}
			size_t rest = colon + 1;
			while (rest < virtualPath.size() && virtualPath[rest] == '/') {
				++rest;
			}
			return virtualPath.substr(rest);
		}

		bool ShellRevealPath(const fs::path& target, bool isDirectory) {
#if defined(_WIN32)
			fs::path preferred = target;
			str native = preferred.make_preferred().string();
			str arguments = isDirectory ? ("\"" + native + "\"") : ("/select,\"" + native + "\"");
			std::wstring wideArguments(arguments.begin(), arguments.end());
			auto result = reinterpret_cast<std::uintptr_t>(
				::ShellExecuteW(nullptr, L"open", L"explorer.exe", wideArguments.c_str(), nullptr, SW_SHOWNORMAL));
			return result > 32;
#else
			(void)target;
			(void)isDirectory;
			return false;
#endif
		}

		void WalkDirectory(const fs::path& directory, const str& relPrefix, u32 depth, Vector<Entry>& out) {
			struct Item {
				str name;
				bool isDirectory;
			};
			Vector<Item> items{GetMemoryResource(MemTag::Editor)};
			std::error_code ec;
			for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
				std::error_code typeEc;
				bool isDirectory = it->is_directory(typeEc);
				if (typeEc) {
					continue;
				}
				str name = it->path().filename().generic_string();
				if (!isDirectory && ProjectLayout::IsReservedFileName(name)) {
					continue;
				}
				items.push_back({std::move(name), isDirectory});
			}

			std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
				if (a.isDirectory != b.isDirectory) {
					return a.isDirectory;
				}
				return NameLess(a.name, b.name);
			});

			for (const Item& item : items) {
				Entry entry;
				entry.name = item.name;
				entry.relPath = relPrefix.empty() ? item.name : relPrefix + "/" + item.name;
				entry.depth = depth;
				entry.kind = item.isDirectory ? EntryKind::Directory : EntryKind::File;
				entry.id = EntryKeyForPath(entry.relPath);
				out.push_back(entry);
				if (item.isDirectory) {
					WalkDirectory(directory / item.name, entry.relPath, depth + 1, out);
				}
			}
		}

	} // anonymous namespace

	bool IsProjectMounted() {
		return !ResolveProjectRoot().empty();
	}

	Path GetProjectRootPhysical() {
		fs::path root = ResolveProjectRoot();
		return root.empty() ? Path() : Path(root.generic_string());
	}

	bool ScanTree(Vector<Entry>& out) {
		out.clear();
		fs::path root = ResolveProjectRoot();
		if (root.empty()) {
			return false;
		}
		for (std::string_view contentRoot : ProjectLayout::GetContentRoots()) {
			std::error_code ec;
			fs::path start = root / fs::path(contentRoot);
			if (!fs::is_directory(start, ec) || ec) {
				continue;
			}
			Entry entry;
			entry.name = str(contentRoot);
			entry.relPath = entry.name;
			entry.depth = 0;
			entry.kind = EntryKind::Directory;
			entry.id = EntryKeyForPath(entry.relPath);
			out.push_back(entry);
			WalkDirectory(start, entry.relPath, 1, out);
		}
		return true;
	}

	void FolderContents(const Vector<Entry>& tree, std::string_view folderRelPath, std::string_view filter,
						Vector<Entry>& out) {
		out.clear();
		if (folderRelPath.empty()) {
			return;
		}
		for (const Entry& entry : tree) {
			std::string_view relPath = entry.relPath;
			if (relPath.size() <= folderRelPath.size() + 1 || !relPath.starts_with(folderRelPath) ||
				relPath[folderRelPath.size()] != '/' ||
				relPath.find('/', folderRelPath.size() + 1) != std::string_view::npos) {
				continue;
			}
			if (MatchesFilter(entry.name, filter)) {
				out.push_back(entry);
			}
		}
		std::stable_partition(out.begin(), out.end(), [](const Entry& entry) { return entry.IsDirectory(); });
	}

	Path GetDefaultCreateFolder() {
		fs::path root = ResolveProjectRoot();
		if (root.empty()) {
			return {};
		}
		for (std::string_view contentRoot : ProjectLayout::GetContentRoots()) {
			std::error_code ec;
			if (fs::is_directory(root / fs::path(contentRoot), ec) && !ec) {
				return Path(str("project://") + str(contentRoot));
			}
		}
		return {};
	}

	bool CreateFolder(const Path& virtualDir, const str& name) {
		if (!IsValidEntryName(name)) {
			SLT_LOG_WARN("Editor", "CreateFolder: invalid name '{}'", name);
			return false;
		}
		Path resolved = ResolvePhysical(virtualDir);
		if (resolved.ToString().empty()) {
			return false;
		}
		std::error_code ec;
		fs::path target = fs::path{resolved.ToString()} / name;
		if (!fs::create_directory(target, ec) || ec) {
			SLT_LOG_WARN("Editor", "CreateFolder: couldn't create '{}' ({})",
						 target.generic_string(), ec ? ec.message() : str("already exists"));
			return false;
		}
		return true;
	}

	bool RenameEntry(const Path& virtualPath, const str& newName) {
		if (!IsValidEntryName(newName)) {
			SLT_LOG_WARN("Editor", "RenameEntry: invalid name '{}'", newName);
			return false;
		}
		Path resolved = ResolvePhysical(virtualPath);
		if (resolved.ToString().empty()) {
			return false;
		}
		std::error_code ec;
		fs::path source{resolved.ToString()};
		if (!fs::exists(source, ec) || ec) {
			return false;
		}
		fs::path target = source.parent_path() / newName;

		std::error_code equivalentEc;
		bool sameEntry = fs::equivalent(source, target, equivalentEc) && !equivalentEc;
		std::error_code existsEc;
		if (!sameEntry && fs::exists(target, existsEc)) {
			SLT_LOG_WARN("Editor", "RenameEntry: '{}' already exists", target.generic_string());
			return false;
		}

		bool isDirectory = fs::is_directory(source, ec);
		fs::rename(source, target, ec);
		if (ec) {
			SLT_LOG_WARN("Editor", "RenameEntry: rename failed ({})", ec.message());
			return false;
		}

		if (!isDirectory) {
			fs::path metaSource = source;
			metaSource += ".meta";
			std::error_code metaEc;
			if (fs::exists(metaSource, metaEc) && !metaEc) {
				fs::path metaTarget = target;
				metaTarget += ".meta";
				fs::rename(metaSource, metaTarget, metaEc);
				if (metaEc) {
					SLT_LOG_WARN("Editor", "RenameEntry: sidecar move failed ({}). Guid will regenerate", metaEc.message());
				}
			}

			if (StringUtil::ToLower(source.extension().generic_string()) == ".cs" &&
				StringUtil::ToLower(target.extension().generic_string()) == ".cs") {
				str oldStem = source.stem().generic_string();
				str newStem = target.stem().generic_string();
				if (oldStem != newStem &&
					ScriptProjectOps::IsValidClassName(oldStem) &&
					ScriptProjectOps::IsValidClassName(newStem)) {
					Path targetPhysical(target.generic_string());
					str content = FileSystem::ReadTextFile(targetPhysical, MemTag::Editor);
					str oldDecl = "class " + oldStem;
					str newDecl = "class " + newStem;
					if (!content.empty() && content.find(oldDecl) != str::npos) {
						content = StringUtil::Replace(content, oldDecl, newDecl);
						FileSystem::WriteTextFile(targetPhysical, content);
					}
				}
				if (GameModuleHost* moduleHost = GameModuleHost::GetActive()) {
					moduleHost->RequestCompile();
				}
			}
		}

		AssetDatabase::RebuildDatabase();
		return true;
	}

	str RenameStem(std::string_view fileName) {
		const str extension = Path(str(fileName)).GetExtension();
		return str(fileName.substr(0, fileName.size() - extension.size()));
	}

	str RenamedFileName(std::string_view fileName, std::string_view typed) {
		const str extension = Path(str(fileName)).GetExtension();
		std::string_view stem = typed;
		if (!extension.empty() && stem.size() >= extension.size() &&
			StringUtil::ToLower(stem.substr(stem.size() - extension.size())) == StringUtil::ToLower(extension)) {
			stem = stem.substr(0, stem.size() - extension.size());
		}
		if (stem.empty()) {
			return {};
		}
		return str(stem) + extension;
	}

	const Entry* ResolveTypedPath(std::span<const Entry> entries, std::string_view typed) {
		auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
		while (!typed.empty() && blank(typed.front())) {
			typed.remove_prefix(1);
		}
		while (!typed.empty() && blank(typed.back())) {
			typed.remove_suffix(1);
		}
		constexpr std::string_view SCHEME = "project://";
		if (typed.starts_with(SCHEME)) {
			typed.remove_prefix(SCHEME.size());
		}

		Vector<std::string_view> segments{GetMemoryResource(MemTag::Editor)};
		size_t start = 0;
		while (start <= typed.size()) {
			size_t end = typed.find_first_of("/\\", start);
			if (end == std::string_view::npos) {
				end = typed.size();
			}
			const std::string_view segment = typed.substr(start, end - start);
			if (segment == "..") {
				if (!segments.empty()) {
					segments.pop_back();
				}
			} else if (!segment.empty() && segment != ".") {
				segments.push_back(segment);
			}
			start = end + 1;
		}
		if (segments.empty()) {
			return nullptr;
		}
		str path;
		for (std::string_view segment : segments) {
			if (!path.empty()) {
				path += '/';
			}
			path += segment;
		}

		const str folded = StringUtil::ToLower(path);
		const Entry* caseless = nullptr;
		for (const Entry& entry : entries) {
			if (entry.kind != EntryKind::Directory && entry.kind != EntryKind::File) {
				continue;
			}
			if (entry.relPath == path) {
				return &entry;
			}
			if (!caseless && StringUtil::ToLower(entry.relPath) == folded) {
				caseless = &entry;
			}
		}
		return caseless;
	}

	void FolderHistory::Visit(std::string_view folder) {
		if (folder.empty() || (!_visits.empty() && _visits[_index] == folder)) {
			return;
		}
		if (!_visits.empty()) {
			_visits.erase(_visits.begin() + static_cast<std::ptrdiff_t>(_index) + 1, _visits.end());
		}
		_visits.emplace_back(folder);
		if (_visits.size() > CAPACITY) {
			_visits.erase(_visits.begin());
		}
		_index = _visits.size() - 1;
	}

	size_t FolderHistory::Step(int direction, const std::function<bool(std::string_view)>& exists) const {
		if (_visits.empty()) {
			return _index;
		}
		const str& here = _visits[_index];
		for (size_t i = _index; direction < 0 ? i > 0 : i + 1 < _visits.size();) {
			i = direction < 0 ? i - 1 : i + 1;
			if (_visits[i] != here && (!exists || exists(_visits[i]))) {
				return i;
			}
		}
		return _index;
	}

	str FolderHistory::PeekBack(const std::function<bool(std::string_view)>& exists) const {
		const size_t to = Step(-1, exists);
		return to == _index ? str() : _visits[to];
	}

	str FolderHistory::PeekForward(const std::function<bool(std::string_view)>& exists) const {
		const size_t to = Step(1, exists);
		return to == _index ? str() : _visits[to];
	}

	str FolderHistory::Back(const std::function<bool(std::string_view)>& exists) {
		const size_t to = Step(-1, exists);
		if (to == _index) {
			return {};
		}
		_index = to;
		return _visits[_index];
	}

	str FolderHistory::Forward(const std::function<bool(std::string_view)>& exists) {
		const size_t to = Step(1, exists);
		if (to == _index) {
			return {};
		}
		_index = to;
		return _visits[_index];
	}

	void FolderHistory::Remap(std::string_view from, std::string_view to) {
		if (from.empty()) {
			return;
		}
		for (str& visit : _visits) {
			if (visit == from) {
				visit = str(to);
			} else if (visit.size() > from.size() && visit.starts_with(from) && visit[from.size()] == '/') {
				visit = str(to) + visit.substr(from.size());
			}
		}
	}

	std::string_view FolderHistory::Current() const {
		return _visits.empty() ? std::string_view() : std::string_view(_visits[_index]);
	}

	bool DeleteEntry(const Path& virtualPath) {
		Path resolved = ResolvePhysical(virtualPath);
		if (resolved.ToString().empty()) {
			return false;
		}
		std::error_code ec;
		fs::path target{resolved.ToString()};
		if (!fs::exists(target, ec) || ec) {
			return false;
		}
		if (fs::is_directory(target, ec)) {
			fs::remove_all(target, ec);
		} else {
			fs::remove(target, ec);
			fs::path meta = target;
			meta += ".meta";
			std::error_code metaEc;
			if (fs::exists(meta, metaEc) && !metaEc) {
				fs::remove(meta, metaEc);
			}
		}
		if (ec) {
			SLT_LOG_WARN("Editor", "DeleteEntry: delete failed ({})", ec.message());
			return false;
		}
		AssetDatabase::RebuildDatabase();
		return true;
	}

	str MakeUniqueName(const Path& virtualDir, const str& desiredName) {
		Path resolved = ResolvePhysical(virtualDir);
		if (resolved.ToString().empty() || desiredName.empty()) {
			return desiredName;
		}
		fs::path directory{resolved.ToString()};
		std::error_code ec;
		if (!fs::exists(directory / desiredName, ec)) {
			return desiredName;
		}

		size_t dot = desiredName.find_last_of('.');
		str stem = dot == str::npos ? desiredName : desiredName.substr(0, dot);
		str extension = dot == str::npos ? str() : desiredName.substr(dot);
		for (u32 suffix = 1; suffix < 10000; ++suffix) {
			str candidate = stem + " " + std::to_string(suffix) + extension;
			std::error_code existsEc;
			if (!fs::exists(directory / candidate, existsEc)) {
				return candidate;
			}
		}
		return desiredName;
	}

	void GetCreateMenuEntries(Vector<CreateMenuEntry>& out) {
		out.clear();
		AssetImporterRegistry::ForEachImporter([&out](StringHash typeId, IAssetImporter* importer, std::string_view) {
			if (!importer) {
				return;
			}
			for (u32 index = 0; index < importer->GetCreateTemplateCount(); ++index) {
				CreateMenuEntry entry;
				entry.label = str(importer->GetCreateTemplateLabel(index));
				entry.typeId = typeId;
				entry.fileName = str(importer->GetCreateTemplateFileName(index));
				entry.templateIndex = index;
				if (!entry.label.empty() && !entry.fileName.empty()) {
					out.push_back(std::move(entry));
				}
			}
		});
		std::sort(out.begin(), out.end(), [](const CreateMenuEntry& a, const CreateMenuEntry& b) { return a.label < b.label; });
	}

	Guid CreateAsset(const Path& virtualDir, StringHash assetTypeId, const str& desiredName, str* outRelPath, u32 templateIndex) {
		IAssetImporter* importer = desiredName.empty() ? nullptr : AssetImporterRegistry::FindByExtension(Path(desiredName).GetExtension());
		if (!importer || importer->GetAssetType() != assetTypeId) {
			importer = AssetImporterRegistry::Find(assetTypeId);
		}
		if (!importer || templateIndex >= importer->GetCreateTemplateCount()) {
			SLT_LOG_WARN("Editor", "CreateAsset: type {:#x} isn't creatable", assetTypeId.GetValue());
			return Guid{};
		}
		str baseName = desiredName.empty() ? str(importer->GetCreateTemplateFileName(templateIndex)) : desiredName;
		if (!IsValidEntryName(baseName)) {
			SLT_LOG_WARN("Editor", "CreateAsset: invalid name '{}'", baseName);
			return Guid{};
		}

		str virtualPath = JoinVirtual(virtualDir, MakeUniqueName(virtualDir, baseName));

		Guid guid = AssetDatabase::CreateAsset(Path(virtualPath), assetTypeId, templateIndex);
		if (guid.IsValid() && outRelPath) {
			*outRelPath = RelativeOf(virtualPath);
		}
		return guid;
	}

	bool CreateCSharpScript(const Path& virtualDir, const str& className, str* outRelPath) {
		if (!ScriptProjectOps::IsValidClassName(className)) {
			SLT_LOG_WARN("Editor", "CreateCSharpScript: '{}' isn't a valid C# class name", className);
			return false;
		}
		Path resolvedDir = ResolvePhysical(virtualDir);
		fs::path projectRoot = ResolveProjectRoot();
		if (resolvedDir.ToString().empty() || projectRoot.empty()) {
			return false;
		}

		str fileName = className + ".cs";
		fs::path targetFile = fs::path(resolvedDir.ToString()) / fileName;
		std::error_code ec;
		if (fs::exists(targetFile, ec)) {
			SLT_LOG_WARN("Editor", "CreateCSharpScript: '{}' already exists", targetFile.generic_string());
			return false;
		}

		str projectPath = projectRoot.generic_string();
		str moduleName = ScriptProjectOps::GetModuleName(projectPath);
		Vector<ScriptProjectOps::TemplateToken> tokens{GetMemoryResource(MemTag::Editor)};
		tokens.push_back({"MODULE_NAME", moduleName});
		tokens.push_back({"CLASS_NAME", className});

		str scriptContent;
		if (!ScriptProjectOps::RenderTemplate("Component.cs", tokens, scriptContent)) {
			return false;
		}

		fs::create_directories(targetFile.parent_path(), ec);
		if (!FileSystem::WriteTextFile(Path(targetFile.generic_string()), scriptContent)) {
			SLT_LOG_WARN("Editor", "CreateCSharpScript: couldn't write '{}'", targetFile.generic_string());
			return false;
		}

		AssetDatabase::ScanProjectDirectory();
		CSharpTranspiler::EnsureCSharpProjectFiles(projectPath);
		if (GameModuleHost* moduleHost = GameModuleHost::GetActive()) {
			if (!ScriptProjectOps::HasScriptProject(projectPath)) {
				ScriptProjectOps::ScaffoldOptions options;
				options.moduleName = moduleName;
				options.sdkRoot = ScriptProjectOps::GetSDKRoot();
				options.editorExecutable = ScriptProjectOps::GetEditorExecutablePath();
				if (const PackageHost* packages = PackageHost::GetActive()) {
					options.enabledPackages = packages->GetGamePackageNames();
				}
				ScriptProjectOps::ScaffoldResult scaffolded;
				if (ScriptProjectOps::ScaffoldScriptProject(projectPath, options, scaffolded)) {
					moduleHost->RearmScriptProject();
				}
			}
			moduleHost->RequestCompile();
		}
		if (outRelPath) {
			*outRelPath = RelativeOf(JoinVirtual(virtualDir, fileName));
		}
		return true;
	}

	StringHash ResolveScriptComponentType(std::string_view relPath) {
		Path path{str(relPath)};
		if (StringUtil::ToLower(path.GetExtension()) != ".cs") {
			return {};
		}
		auto isCreatableComponent = [](const TypeInfo* type) {
			return type && (type->flags & TypeFlagComponent) != 0 &&
				   (type->flags & TypeFlagAbstract) == 0 && type->createFn != nullptr;
		};
		str stem = RenameStem(path.GetFilename());
		if (!stem.empty()) {
			const TypeInfo* byStem = TypeRegistry::FindType(StringHash(stem));
			if (isCreatableComponent(byStem)) {
				return byStem->name;
			}
		}
		Path physical = ResolvePhysical(Path(VirtualPathOf(str(relPath))));
		if (!physical.ToString().empty()) {
			str content = FileSystem::ReadTextFile(physical, MemTag::Editor);
			constexpr std::string_view CLASS_TOKEN = "class ";
			size_t pos = 0;
			while ((pos = content.find(CLASS_TOKEN, pos)) != str::npos) {
				if (pos > 0 && (std::isalnum(static_cast<unsigned char>(content[pos - 1])) || content[pos - 1] == '_')) {
					pos += CLASS_TOKEN.size();
					continue;
				}
				pos += CLASS_TOKEN.size();
				while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t')) {
					++pos;
				}
				size_t start = pos;
				while (pos < content.size() && (std::isalnum(static_cast<unsigned char>(content[pos])) || content[pos] == '_')) {
					++pos;
				}
				if (pos > start) {
					std::string_view candidate(content.data() + start, pos - start);
					const TypeInfo* byClass = TypeRegistry::FindType(StringHash(candidate));
					if (isCreatableComponent(byClass)) {
						return byClass->name;
					}
				}
			}
		}
		return {};
	}

	Guid CreatePrefabFromObject(Handle<SceneObject> root, const Path& virtualDir, str* outRelPath) {
		SceneObject* resolved = root.Get();
		if (!resolved) {
			return Guid{};
		}
		if (SceneSystem::IsPrefabLinked(root)) {
			SLT_LOG_WARN("Editor", "'{}' is already part of a prefab instance (can't become a 2nd prefab)",
						 resolved->name);
			return Guid{};
		}
		str baseName = str(resolved->name) + ".prefab";
		if (!IsValidEntryName(baseName)) {
			SLT_LOG_WARN("Editor", "CreatePrefabFromObject: '{}' isn't a usable file name", baseName);
			return Guid{};
		}
		str virtualPath = JoinVirtual(virtualDir, MakeUniqueName(virtualDir, baseName));
		Guid guid = EditorSceneOps::CreatePrefabFrom(root, Path(virtualPath));
		if (guid.IsValid() && outRelPath) {
			*outRelPath = RelativeOf(virtualPath);
		}
		return guid;
	}

	bool CanCreatePrefabVariant(const Path& virtualPath) {
		str extension = virtualPath.GetExtension();
		IAssetImporter* importer = extension.empty() ? nullptr : AssetImporterRegistry::FindByExtension(extension);
		return importer && importer->GetAssetType() == PrefabAsset::GetReflectType()->name;
	}

	bool IsPrefabVariant(const str& relPath) {
		Path virtualPath(VirtualPathOf(relPath));
		if (virtualPath.GetExtension() != ".prefab") {
			return false;
		}
		struct Remembered {
			std::filesystem::file_time_type stamp;
			bool isVariant = false;
		};
		static std::unordered_map<str, Remembered> _remembered;
		std::error_code ec;
		std::filesystem::file_time_type stamp =
			std::filesystem::last_write_time(FileSystem::Resolve(virtualPath).ToString(), ec);
		if (ec) {
			return false;
		}
		auto it = _remembered.find(relPath);
		if (it != _remembered.end() && it->second.stamp == stamp) {
			return it->second.isVariant;
		}
		Asset* asset = AssetDatabase::LoadAsset(virtualPath);
		bool isVariant = asset && TypeRegistry::IsDerivedFrom(asset->GetType(), PrefabAsset::GetReflectType()) &&
						 static_cast<PrefabAsset*>(asset)->GetBasePrefab().IsValid();
		if (asset) {
			AssetDatabase::UnloadAsset(asset);
		}
		_remembered[relPath] = Remembered{stamp, isVariant};
		return isVariant;
	}

	Guid CreatePrefabVariant(const Path& virtualPath, str* outRelPath) {
		if (!CanCreatePrefabVariant(virtualPath)) {
			SLT_LOG_WARN("Editor", "'{}' isn't a prefab (nothing can inherit from it)", virtualPath.ToString());
			return Guid{};
		}
		Guid base = AssetDatabase::LookupGuid(virtualPath);
		if (!base.IsValid()) {
			SLT_LOG_WARN("Editor", "CreatePrefabVariant: '{}' isn't an indexed asset", virtualPath.ToString());
			return Guid{};
		}
		str filename = virtualPath.GetFilename();
		str stem = filename.substr(0, filename.size() - virtualPath.GetExtension().size());
		Path directory{virtualPath.GetDirectory()};
		str newPath = JoinVirtual(directory, MakeUniqueName(directory, stem + " Variant.prefab"));
		Guid guid = EditorSceneOps::CreatePrefabVariantOf(base, Path(newPath));
		if (guid.IsValid() && outRelPath) {
			*outRelPath = RelativeOf(newPath);
		}
		return guid;
	}

	Guid CreatePrefabVariantFromObject(Handle<SceneObject> root, const Path& virtualDir, str* outRelPath) {
		SceneObject* resolved = root.Get();
		if (!resolved) {
			return Guid{};
		}
		if (!EditorSceneOps::CanCreatePrefabVariantFrom(root)) {
			SLT_LOG_WARN("Editor", "'{}' isn't a prefab instance root (only a whole instance can become a variant)",
						 resolved->name);
			return Guid{};
		}
		str baseName = str(resolved->name) + ".prefab";
		if (!IsValidEntryName(baseName)) {
			SLT_LOG_WARN("Editor", "CreatePrefabVariantFromObject: '{}' isn't a usable file name", baseName);
			return Guid{};
		}
		str virtualPath = JoinVirtual(virtualDir, MakeUniqueName(virtualDir, baseName));
		Guid guid = EditorSceneOps::CreatePrefabVariantFrom(root, Path(virtualPath));
		if (guid.IsValid() && outRelPath) {
			*outRelPath = RelativeOf(virtualPath);
		}
		return guid;
	}

	bool MoveEntry(const Path& virtualPath, const Path& virtualDestDir) {
		Path resolvedSource = ResolvePhysical(virtualPath);
		Path resolvedDest = ResolvePhysical(virtualDestDir);
		if (resolvedSource.ToString().empty() || resolvedDest.ToString().empty()) {
			return false;
		}
		std::error_code ec;
		fs::path source{resolvedSource.ToString()};
		fs::path destDir{resolvedDest.ToString()};
		if (!fs::exists(source, ec) || !fs::is_directory(destDir, ec)) {
			return false;
		}

		std::error_code sameEc;
		if (fs::equivalent(source.parent_path(), destDir, sameEc) && !sameEc) {
			return false;
		}

		if (fs::is_directory(source, ec)) {
			fs::path probe = destDir;
			while (!probe.empty()) {
				std::error_code equivalentEc;
				if (fs::equivalent(probe, source, equivalentEc) && !equivalentEc) {
					SLT_LOG_WARN("Editor", "MoveEntry: can't move a directory into its subtree");
					return false;
				}
				if (!probe.has_parent_path() || probe.parent_path() == probe) {
					break;
				}
				probe = probe.parent_path();
			}
		}

		fs::path target = destDir / source.filename();
		std::error_code existsEc;
		if (fs::exists(target, existsEc)) {
			SLT_LOG_WARN("Editor", "MoveEntry: '{}' already exists", target.generic_string());
			return false;
		}

		bool isDirectory = fs::is_directory(source, ec);
		fs::rename(source, target, ec);
		if (ec) {
			SLT_LOG_WARN("Editor", "MoveEntry: move failed ({})", ec.message());
			return false;
		}
		if (!isDirectory) {
			fs::path metaSource = source;
			metaSource += ".meta";
			std::error_code metaEc;
			if (fs::exists(metaSource, metaEc) && !metaEc) {
				fs::path metaTarget = target;
				metaTarget += ".meta";
				fs::rename(metaSource, metaTarget, metaEc);
				if (metaEc) {
					SLT_LOG_WARN("Editor", "MoveEntry: sidecar move failed ({}). Guid will regenerate", metaEc.message());
				}
			}
		}
		AssetDatabase::RebuildDatabase();
		return true;
	}

	str DuplicateEntry(const Path& virtualPath) {
		Path resolved = ResolvePhysical(virtualPath);
		if (resolved.ToString().empty()) {
			return {};
		}
		std::error_code ec;
		fs::path source{resolved.ToString()};
		if (!fs::exists(source, ec) || ec) {
			return {};
		}

		str parentVirtual = Path(virtualPath).GetDirectory();
		str unique = MakeUniqueName(Path(parentVirtual), source.filename().generic_string());
		fs::path target = source.parent_path() / unique;

		if (fs::is_directory(source, ec)) {
			fs::copy(source, target, fs::copy_options::recursive, ec);
			if (!ec) {
				std::error_code walkEc;
				for (fs::recursive_directory_iterator it(target, walkEc), end; !walkEc && it != end; it.increment(walkEc)) {
					if (it->path().extension() == ".meta") {
						std::error_code removeEc;
						fs::remove(it->path(), removeEc);
					}
				}
			}
		} else {
			fs::copy_file(source, target, ec);
		}
		if (ec) {
			SLT_LOG_WARN("Editor", "DuplicateEntry: copy failed ({})", ec.message());
			return {};
		}

		AssetDatabase::ScanProjectDirectory();
		str parentRel = RelativeOf(parentVirtual);
		return parentRel.empty() ? unique : parentRel + "/" + unique;
	}

	str ImportExternalFile(const Path& physicalSource, const Path& virtualDestDir) {
		std::error_code ec;
		fs::path source{physicalSource.ToString()};
		if (!fs::exists(source, ec) || fs::is_directory(source, ec)) {
			return {};
		}
		Path resolvedDest = ResolvePhysical(virtualDestDir);
		if (resolvedDest.ToString().empty()) {
			return {};
		}

		str unique = MakeUniqueName(virtualDestDir, source.filename().generic_string());
		fs::path target = fs::path{resolvedDest.ToString()} / unique;
		fs::copy_file(source, target, ec);
		if (ec) {
			SLT_LOG_WARN("Editor", "ImportExternalFile: copy failed ({})", ec.message());
			return {};
		}

		AssetDatabase::ScanProjectDirectory();
		str destRel = RelativeOf(virtualDestDir.ToString());
		return destRel.empty() ? unique : destRel + "/" + unique;
	}

	bool OpensInExternalEditor(std::string_view extension) {
		str lowered;
		lowered.reserve(extension.size());
		for (char c : extension) {
			lowered.push_back(ToLowerAscii(c));
		}
		return lowered == ".hlsl" || lowered == ".hlsli" || lowered == ".cs";
	}

	bool OpenInExternalEditor(const Path& physicalPath, u32 line) {
		GameModuleHost* moduleHost = GameModuleHost::GetActive();
		const str editorCommand = moduleHost ? moduleHost->GetExternalEditorCommand() : str{};
		return ScriptProjectOps::OpenInExternalEditor(ProjectOps::GetMountedProjectPath(), editorCommand, physicalPath.ToString(), line, 0);
	}

	bool RevealInExplorer(const Path& virtualPath) {
		Path resolved = ResolvePhysical(virtualPath);
		if (resolved.ToString().empty()) {
			return false;
		}
		std::error_code ec;
		fs::path target{resolved.ToString()};
		if (!fs::exists(target, ec) || ec) {
			return false;
		}
		return ShellRevealPath(target, fs::is_directory(target, ec));
	}

	bool MatchesFilter(std::string_view text, std::string_view filter) {
		if (filter.empty()) {
			return true;
		}
		if (filter.size() > text.size()) {
			return false;
		}
		for (size_t start = 0; start + filter.size() <= text.size(); ++start) {
			size_t i = 0;
			while (i < filter.size() && ToLowerAscii(text[start + i]) == ToLowerAscii(filter[i])) {
				++i;
			}
			if (i == filter.size()) {
				return true;
			}
		}
		return false;
	}

	bool SliderIsGrid(f32 sliderValue) {
		return sliderValue >= SLIDER_SMALLEST_GRID;
	}

	GridCellSize CellSizeForSlider(f32 sliderValue, f32 minSidePx, f32 maxSidePx, f32 labelBandPx) {
		if (!SliderIsGrid(sliderValue)) {
			return {};
		}
		f32 low = std::max(1.0f, minSidePx);
		f32 high = std::max(low, maxSidePx);
		f32 t = std::clamp((sliderValue - SLIDER_SMALLEST_GRID) / (1.0f - SLIDER_SMALLEST_GRID), 0.0f, 1.0f);
		f32 side = low + (high - low) * t;
		return {side, side + std::max(0.0f, labelBandPx)};
	}

	f32 BadgeGlyphSizeFor(f32 wellPx) {
		static constexpr f32 LADDER[] = {16.0f, 24.0f, 32.0f, 48.0f, 64.0f, 96.0f, 128.0f};

		constexpr f32 FIT = 0.85f;

		f32 budget = wellPx * FIT;
		f32 chosen = LADDER[0];
		for (f32 step : LADDER) {
			if (step > budget) {
				break;
			}
			chosen = step;
		}
		return chosen;
	}

	u32 GridColumnsFor(f32 viewportWidthPx, f32 cellWidthPx) {
		if (cellWidthPx <= 0.0f || viewportWidthPx <= 0.0f) {
			return 1;
		}
		return std::max(1u, static_cast<u32>(viewportWidthPx / cellWidthPx));
	}

	GridWindow VisibleGridWindow(u32 itemCount, u32 columns, f32 cellHeightPx, f32 scrollYPx, f32 viewportHeightPx) {
		if (itemCount == 0 || columns == 0 || cellHeightPx <= 0.0f || viewportHeightPx <= 0.0f) {
			return {};
		}
		f32 top = std::max(0.0f, scrollYPx);
		u32 firstRow = static_cast<u32>(top / cellHeightPx);
		u32 lastRow = static_cast<u32>((top + viewportHeightPx - 0.001f) / cellHeightPx);

		u32 rowCount = (itemCount + columns - 1) / columns;
		if (firstRow >= rowCount) {
			return {};
		}
		lastRow = std::min(lastRow, rowCount - 1);

		GridWindow window;
		window.firstIndex = firstRow * columns;
		u32 lastIndex = std::min(itemCount - 1, lastRow * columns + columns - 1);
		window.count = lastIndex - window.firstIndex + 1;
		return window;
	}

	void FoldOutBandSegments(std::span<const u32> depths, u32 columns, Vector<FoldOutSegment>& out) {
		out.clear();
		if (columns == 0) {
			return;
		}
		const u32 count = static_cast<u32>(depths.size());
		for (u32 index = 0; index < count;) {
			if (depths[index] == 0) {
				++index;
				continue;
			}
			const u32 rowEnd = (index / columns + 1) * columns;
			u32 last = index;
			while (last + 1 < count && last + 1 < rowEnd && depths[last + 1] != 0) {
				++last;
			}
			FoldOutSegment segment;
			segment.first = index;
			segment.last = last;
			segment.fromAbove = index % columns == 0 && index > 0 && depths[index - 1] != 0;
			segment.continuesBelow = last + 1 == rowEnd && last + 1 < count && depths[last + 1] != 0;
			out.push_back(segment);
			index = last + 1;
		}
	}

	f32 FoldButtonSizeFor(f32 cellSidePx, f32 maxCellSidePx, f32 smallPx, f32 mediumPx, f32 largePx) {
		const f32 fraction = maxCellSidePx > 0.0f ? cellSidePx / maxCellSidePx : 1.0f;
		if (fraction < 0.5f) {
			return smallPx;
		}
		return fraction < 0.75f ? mediumPx : largePx;
	}

	u64 EntryKeyForPath(std::string_view relPath) {
		return StringHash(relPath).GetValue();
	}

	namespace {

		u64 ProductRowKey(std::string_view sourceRelPath, std::string_view productKey) {
			return StringHash(str(sourceRelPath) + " : " + str(productKey)).GetValue();
		}

		u64 LinkRowKey(std::string_view sourceRelPath, std::string_view linkRelPath) {
			return StringHash(str(sourceRelPath) + " -> " + str(linkRelPath)).GetValue();
		}

		str ProductLabel(const str& key) {
			size_t slash = key.rfind('/');
			return (slash == str::npos) ? key : key.substr(slash + 1);
		}

		str ProjectRelativeOf(const Path& virtualPath) {
			std::string_view text = virtualPath.ToString();
			constexpr std::string_view SCHEME = "project://";
			if (!text.starts_with(SCHEME)) {
				return {};
			}
			return str(text.substr(SCHEME.size()));
		}

	} // anonymous namespace

	void ScanProducts(const str& relPath, u32 depth, Vector<Entry>& out) {
		out.clear();
		Guid sourceGuid = AssetDatabase::LookupGuid(Path(VirtualPathOf(relPath)));
		if (!sourceGuid.IsValid()) {
			return;
		}

		Vector<AssetProduct> products{GetMemoryResource(MemTag::Editor)};
		AssetDatabase::GetProducts(sourceGuid, products);
		out.reserve(products.size());
		for (const AssetProduct& product : products) {
			Entry entry;
			entry.name = product.name.empty() ? ProductLabel(product.key) : product.name;
			entry.relPath.clear();
			entry.depth = depth + 1;
			entry.kind = EntryKind::Product;
			entry.id = ProductRowKey(relPath, product.key);
			entry.guid = product.guid;
			entry.productKey = product.key;
			entry.assetType = product.type;
			entry.sourceRelPath = relPath;
			entry.extractedRelPath = ProjectRelativeOf(product.extractedPath);
			out.push_back(std::move(entry));
		}
	}

	namespace {

		bool IsOrDerives(StringHash candidate, StringHash type, const TypeInfo* typeInfo) {
			if (candidate == type) {
				return true;
			}
			const TypeInfo* candidateInfo = TypeRegistry::FindType(candidate);
			return candidateInfo && typeInfo && TypeRegistry::IsDerivedFrom(candidateInfo, typeInfo);
		}

		void AppendAssignable(const str& virtualPath, const str& display, StringHash type, const TypeInfo* typeInfo,
							  Vector<AssetProduct>& products, Vector<AssignableAsset>& out) {
			Guid guid = AssetDatabase::LookupGuid(Path(virtualPath));
			if (!guid.IsValid()) {
				return;
			}
			str extension = Path(virtualPath).GetExtension();
			IAssetImporter* importer = extension.empty() ? nullptr : AssetImporterRegistry::FindByExtension(extension);
			if (importer && IsOrDerives(importer->GetAssetType(), type, typeInfo)) {
				out.push_back({display, guid});
			}
			AssetDatabase::GetProducts(guid, products);
			for (const AssetProduct& product : products) {
				if (IsOrDerives(product.type, type, typeInfo)) {
					out.push_back({display + " : " + (product.name.empty() ? product.key : product.name), product.guid});
				}
			}
		}

	} // anonymous namespace

	void CollectAssignableAssets(StringHash type, Vector<AssignableAsset>& out) {
		out.clear();
		const TypeInfo* typeInfo = TypeRegistry::FindType(type);
		Vector<AssetProduct> products{GetMemoryResource(MemTag::Editor)};

		Vector<Entry> entries{GetMemoryResource(MemTag::Editor)};
		ScanTree(entries);
		for (const Entry& entry : entries) {
			if (!entry.IsDirectory()) {
				AppendAssignable(VirtualPathOf(entry.relPath), entry.relPath, type, typeInfo, products, out);
			}
		}

		fs::path engineRoot{ResolvePhysical(Path("engine://")).ToString()};
		std::error_code ec;
		if (engineRoot.empty() || !fs::is_directory(engineRoot, ec) || ec) {
			return;
		}
		Vector<str> engineFiles{GetMemoryResource(MemTag::Editor)};
		for (fs::recursive_directory_iterator it(engineRoot, fs::directory_options::skip_permission_denied, ec), end;
			 it != end; it.increment(ec)) {
			if (ec) {
				break;
			}
			if (it->is_regular_file(ec) && !ec && it->path().extension() != ".meta") {
				str relative = fs::relative(it->path(), engineRoot, ec).generic_string();
				if (!ec && !relative.empty()) {
					engineFiles.push_back(std::move(relative));
				}
			}
		}
		std::sort(engineFiles.begin(), engineFiles.end());
		for (const str& relative : engineFiles) {
			str virtualPath = "engine://" + relative;
			AppendAssignable(virtualPath, virtualPath, type, typeInfo, products, out);
		}
	}

	void ScanLinks(const str& relPath, u32 depth, Vector<Entry>& out) {
		out.clear();
		Guid sourceGuid = AssetDatabase::LookupGuid(Path(VirtualPathOf(relPath)));
		if (!sourceGuid.IsValid()) {
			return;
		}
		Vector<AssetProduct> products{GetMemoryResource(MemTag::Editor)};
		AssetDatabase::GetProducts(sourceGuid, products);
		if (products.empty()) {
			return;
		}

		HashSet<Guid> owned{GetMemoryResource(MemTag::Editor)};
		owned.insert(sourceGuid);
		for (const AssetProduct& product : products) {
			owned.insert(product.guid);
		}

		HashSet<Guid> seen{GetMemoryResource(MemTag::Editor)};
		for (const AssetProduct& product : products) {
			Asset* loaded = AssetDatabase::LoadAssetByGuid(product.guid);
			if (!loaded) {
				continue;
			}
			if (MaterialAsset* material = dynamic_cast<MaterialAsset*>(loaded)) {
				Vector<Guid> referenced{GetMemoryResource(MemTag::Editor)};
				referenced.push_back(material->shaderGuid);
				for (const MaterialAsset::TextureParam& texture : material->textures) {
					referenced.push_back(texture.texture);
				}
				for (const Guid& reference : referenced) {
					if (!reference.IsValid() || owned.contains(reference) || seen.contains(reference)) {
						continue;
					}
					str linkRelPath = ProjectRelativeOf(AssetDatabase::LookupPath(reference));
					if (linkRelPath.empty()) {
						continue;
					}
					seen.insert(reference);
					Entry entry;
					entry.name = Path(linkRelPath).GetFilename();
					entry.relPath = linkRelPath;
					entry.depth = depth + 1;
					entry.kind = EntryKind::Link;
					entry.id = LinkRowKey(relPath, linkRelPath);
					entry.guid = reference;
					entry.sourceRelPath = relPath;
					out.push_back(std::move(entry));
				}
			}
			AssetDatabase::UnloadAsset(loaded);
		}
	}

	str RefusalFor(const Entry& entry, EntryCommand command) {
		if (entry.kind == EntryKind::Product) {
			if (!entry.extractedRelPath.empty()) {
				if (command == EntryCommand::Extract) {
					return std::format("'{}' is already extracted to '{}'.", entry.name, entry.extractedRelPath);
				}
				return std::format("'{}' is extracted to '{}'. Act on it there.",
								   entry.name, entry.extractedRelPath);
			}
			switch (command) {
			case EntryCommand::Extract:
				return entry.productKey.starts_with("Materials/")
						   ? str()
						   : std::format("Only a material can be extracted today. '{}' stays inside '{}'.",
										 entry.name, Path(entry.sourceRelPath).GetFilename());
			case EntryCommand::Rename:
				return std::format("'{}' is part of '{}'. Rename it in the file (or Extract it).",
								   entry.name, Path(entry.sourceRelPath).GetFilename());
			case EntryCommand::Delete:
				return std::format("'{}' is part of '{}'. Delete it in the file (or Extract it).",
								   entry.name, Path(entry.sourceRelPath).GetFilename());
			case EntryCommand::Move:
				return std::format("'{}' is part of '{}' and has no file to move. Extract it.",
								   entry.name, Path(entry.sourceRelPath).GetFilename());
			case EntryCommand::Duplicate:
				return std::format("'{}' is part of '{}' and has no file to duplicate. Extract it.",
								   entry.name, Path(entry.sourceRelPath).GetFilename());
			}
			return {};
		}

		if (entry.kind == EntryKind::Link) {
			if (command == EntryCommand::Extract) {
				return std::format("'{}' is a file '{}' uses (not something it produced).",
								   entry.name, Path(entry.sourceRelPath).GetFilename());
			}
			return std::format("'{}' is used by '{}' and may be used by others. Act on it in '{}'.",
							   entry.name, Path(entry.sourceRelPath).GetFilename(),
							   Path(entry.relPath).GetDirectory());
		}

		if (command == EntryCommand::Extract) {
			return std::format("'{}' is not a sub-asset of anything.", entry.name);
		}
		return {};
	}

	str ExtractProduct(const Entry& entry) {
		str refusal = RefusalFor(entry, EntryCommand::Extract);
		if (!refusal.empty()) {
			SLT_LOG_WARN("Editor", "ExtractProduct: {}", refusal);
			return {};
		}

		str folder = entry.sourceRelPath + ".materials";
		str fileName = MakeUniqueName(Path(VirtualPathOf(folder)), entry.name + ".mat");
		str virtualPath = VirtualPathOf(folder) + "/" + fileName;
		if (!AssetDatabase::ExtractProduct(entry.guid, Path(virtualPath))) {
			return {};
		}
		return RelativeOf(virtualPath);
	}

	bool RevealEntry(const Entry& entry) {
		switch (entry.kind) {
		case EntryKind::Directory:
		case EntryKind::File:
		case EntryKind::Link:
			return RevealInExplorer(Path(VirtualPathOf(entry.relPath)));
		case EntryKind::Product:
			return RevealInExplorer(Path(VirtualPathOf(
				entry.extractedRelPath.empty() ? entry.sourceRelPath : entry.extractedRelPath)));
		}
		return false;
	}

} // namespace Shalltear::AssetBrowserOps
