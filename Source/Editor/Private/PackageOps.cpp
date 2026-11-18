#include "Shalltear/Editor/PackageOps.h"

#include "Shalltear/Core/Config.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Serialization/JSON.h"
#include "Shalltear/Core/Settings.h"
#include "Shalltear/Core/StringUtil.h"

#include "Shalltear/Editor/ScriptProjectOps.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "Platform/EditorPlatform.h"

namespace Shalltear {

	struct EnabledPackagesSettings {
		SLT_REFLECT_VALUE(EnabledPackagesSettings)
		SLT_REFLECT_CONSTRUCTIBLE()
		SLT_REFLECT_FIELD(enabled)
		SLT_REFLECT_FIELD(format, SLT_HIDDEN)
		SLT_END_REFLECT()

		str enabled;
		u32 format = 0;
	};

	namespace PackageOps {

		namespace {

			constexpr char ENABLED_SEPARATOR = ';';
			constexpr std::string_view SettingsCategory = SettingsCategoryPath;
			constexpr std::string_view SettingsField = "enabled";
			constexpr std::string_view FormatField = "format";
			constexpr u32 STORED_SET_FORMAT = 2;
			constexpr std::string_view IdPrefix = "dev.shalltear.";
			constexpr std::string_view RetiredIdPrefix = "com.shalltear.";

			bool IsValidName(std::string_view name) {
				if (name.empty() || name.size() > 214) {
					return false;
				}
				for (char c : name) {
					const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
					if (!ok) {
						return false;
					}
				}
				return name.front() != '.' && name.back() != '.';
			}

			bool IsValidVersion(std::string_view version) {
				u32 segments = 0;
				size_t index = 0;
				while (index <= version.size()) {
					size_t digits = 0;
					while (index + digits < version.size() && version[index + digits] >= '0' && version[index + digits] <= '9') {
						++digits;
					}
					if (digits == 0) {
						return false;
					}
					index += digits;
					++segments;
					if (index == version.size()) {
						break;
					}
					if (version[index] != '.') {
						return false;
					}
					++index;
				}
				return segments == 3;
			}

			bool ParseKind(std::string_view text, PackageKind& out) {
				if (text == "game") {
					out = PackageKind::Game;
					return true;
				}
				if (text == "engine") {
					out = PackageKind::Engine;
					return true;
				}
				if (text == "tool") {
					out = PackageKind::Tool;
					return true;
				}
				if (text == "package") {
					out = PackageKind::Package;
					return true;
				}
				return false;
			}

			bool IsAlwaysOn(const PackageManifest& manifest) {
				return manifest.kind == PackageKind::Game || (manifest.kind == PackageKind::Tool && manifest.required);
			}

			bool IsOptionalTool(const PackageManifest& manifest) { return manifest.kind == PackageKind::Tool && !manifest.required; }

			bool Contains(std::span<const str> names, std::string_view name) {
				for (const str& candidate : names) {
					if (candidate == name) {
						return true;
					}
				}
				return false;
			}

			void CollectReachable(std::span<const PackageManifest> catalogue, std::string_view name, Vector<str>& reached) {
				if (Contains(reached, name)) {
					return;
				}
				const PackageManifest* manifest = FindPackage(catalogue, name);
				if (!manifest) {
					return;
				}
				reached.emplace_back(name);
				for (const PackageDependency& dependency : manifest->dependencies) {
					CollectReachable(catalogue, dependency.name, reached);
				}
			}

			bool IsCyclic(std::span<const PackageManifest> catalogue, std::string_view name) {
				Vector<str> reached;
				const PackageManifest* manifest = FindPackage(catalogue, name);
				if (!manifest) {
					return false;
				}
				for (const PackageDependency& dependency : manifest->dependencies) {
					CollectReachable(catalogue, dependency.name, reached);
				}
				return Contains(reached, name);
			}

			bool DropFirstWithMissingDependency(Vector<PackageManifest>& catalogue, Vector<str>& diagnostics) {
				for (size_t index = 0; index < catalogue.size(); ++index) {
					for (const PackageDependency& dependency : catalogue[index].dependencies) {
						if (FindPackage(catalogue, dependency.name)) {
							continue;
						}
						diagnostics.emplace_back("package '" + catalogue[index].name + "' depends on '" + dependency.name + "' (not installed). Ignoring it");
						catalogue.erase(catalogue.begin() + static_cast<ptrdiff_t>(index));
						return true;
					}
				}
				return false;
			}

			bool DropFirstCycle(Vector<PackageManifest>& catalogue, Vector<str>& diagnostics) {
				Vector<str> cyclic;
				for (const PackageManifest& manifest : catalogue) {
					if (IsCyclic(catalogue, manifest.name)) {
						cyclic.emplace_back(manifest.name);
					}
				}
				if (cyclic.empty()) {
					return false;
				}
				for (const str& name : cyclic) {
					diagnostics.emplace_back("package '" + name + "' is part of a dependency cycle. Ignoring it");
				}
				catalogue.erase(std::remove_if(catalogue.begin(), catalogue.end(), [&](const PackageManifest& entry) { return Contains(cyclic, entry.name); }), catalogue.end());
				return true;
			}

		} // namespace

		std::string_view KindName(PackageKind kind) {
			switch (kind) {
			case PackageKind::Game:
				return "game";
			case PackageKind::Engine:
				return "engine";
			case PackageKind::Tool:
				return "tool";
			case PackageKind::Package:
				return "package";
			}
			return "package";
		}

		bool ParseManifest(std::string_view json, PackageManifest& out, str& error) {
			LinearAllocator arena(json.size() + 64, MemTag::Editor);
			JSONValue root = JSONParse(json, arena);
			if (!root.IsObject()) {
				error = "not a JSON object";
				return false;
			}

			PackageManifest parsed;

			const JSONValue& name = root["name"];
			if (!name.IsString() || !IsValidName(name.AsString())) {
				error = "missing or invalid 'name' (lowercase letters, digits, '.', '-' and '_' only)";
				return false;
			}
			parsed.name = str{name.AsString()};

			const JSONValue& version = root["version"];
			if (!version.IsString() || !IsValidVersion(version.AsString())) {
				error = "missing or invalid 'version' (expected \"major.minor.patch\")";
				return false;
			}
			parsed.version = str{version.AsString()};

			if (root.HasKey("kind")) {
				const JSONValue& kind = root["kind"];
				if (!kind.IsString() || !ParseKind(kind.AsString(), parsed.kind)) {
					error = "invalid 'kind' (expected game, engine, tool or package)";
					return false;
				}
			}

			if (root.HasKey("stage")) {
				const JSONValue& stage = root["stage"];
				FrameStage parsedStage = FrameStage::Update;
				if (!stage.IsString() || !FindFrameStage(stage.AsString(), parsedStage)) {
					error = "invalid 'stage' (expected one of";
					for (FrameStage candidate : FRAME_STAGES) {
						error += " ";
						error += GetFrameStageName(candidate);
					}
					error += ")";
					return false;
				}
				parsed.stage = parsedStage;
			}

			if (root.HasKey("required")) {
				const JSONValue& required = root["required"];
				if (!required.IsBool()) {
					error = "'required' must be true or false";
					return false;
				}
				if (required.AsBool() && parsed.kind != PackageKind::Tool) {
					error = "only a 'tool' may be 'required'. A '" + str(KindName(parsed.kind)) + "' is switched on and off by the project";
					return false;
				}
				parsed.required = required.AsBool();
			}

			if (root.HasKey("scaffold")) {
				const JSONValue& scaffold = root["scaffold"];
				if (!scaffold.IsBool()) {
					error = "'scaffold' must be true or false";
					return false;
				}
				parsed.scaffold = scaffold.AsBool();
			}

			if (root.HasKey("displayName")) {
				parsed.displayName = str{root["displayName"].AsString()};
			}
			if (root.HasKey("description")) {
				parsed.description = str{root["description"].AsString()};
			}

			if (root.HasKey("dependencies")) {
				const JSONValue& dependencies = root["dependencies"];
				if (!dependencies.IsObject()) {
					error = "'dependencies' must be an object mapping names to versions";
					return false;
				}
				for (size_t index = 0; index < dependencies.MemberCount(); ++index) {
					std::string_view key = dependencies.KeyAt(index);
					const JSONValue& value = dependencies.ChildAt(index);
					if (!IsValidName(key)) {
						error = "dependency name '" + str{key} + "' isn't a valid package name";
						return false;
					}
					if (!value.IsString() || !IsValidVersion(value.AsString())) {
						error = "dependency '" + str{key} + "' must name an exact \"major.minor.patch\" version";
						return false;
					}
					if (key == parsed.name) {
						error = "package '" + parsed.name + "' depends on itself";
						return false;
					}
					parsed.dependencies.push_back(PackageDependency{str{key}, str{value.AsString()}});
				}
			}

			out = std::move(parsed);
			return true;
		}

		bool LoadManifest(const Path& manifestFile, PackageManifest& out, str& error) {
			if (!FileSystem::Exists(manifestFile)) {
				error = "no manifest at '" + manifestFile.ToString() + "'";
				return false;
			}
			str text = FileSystem::ReadTextFile(manifestFile, MemTag::Editor);
			if (text.empty()) {
				error = "manifest '" + manifestFile.ToString() + "' is empty or unreadable";
				return false;
			}
			if (!ParseManifest(text, out, error)) {
				error = manifestFile.ToString() + ": " + error;
				return false;
			}
			out.directory = Path{manifestFile.GetDirectory()};
			return true;
		}

		const PackageManifest* FindPackage(std::span<const PackageManifest> catalogue, std::string_view name) {
			for (const PackageManifest& manifest : catalogue) {
				if (manifest.name == name) {
					return &manifest;
				}
			}
			return nullptr;
		}

		void DiscoverPackages(std::span<const Path> roots, Vector<PackageManifest>& out, Vector<str>& diagnostics) {
			out.clear();
			diagnostics.clear();

			namespace fs = std::filesystem;
			for (const Path& root : roots) {
				const Path resolved = FileSystem::Resolve(root);
				std::error_code ec;
				if (!fs::is_directory(fs::path{resolved.ToString()}, ec)) {
					continue;
				}
				Vector<str> directories;
				for (fs::directory_iterator it(fs::path{resolved.ToString()}, ec), end; !ec && it != end; it.increment(ec)) {
					if (it->is_directory(ec)) {
						directories.emplace_back(it->path().string());
					}
				}
				std::sort(directories.begin(), directories.end());

				for (const str& directory : directories) {
					const Path manifestFile{directory + "/" + str{ManifestFileName}};
					if (!FileSystem::Exists(manifestFile)) {
						continue;
					}
					PackageManifest manifest;
					str error;
					if (!LoadManifest(manifestFile, manifest, error)) {
						diagnostics.emplace_back(std::move(error));
						continue;
					}
					out.push_back(std::move(manifest));
				}
			}

			ValidateCatalogue(out, diagnostics);
		}

		void ValidateCatalogue(Vector<PackageManifest>& catalogue, Vector<str>& diagnostics) {
			for (size_t index = 0; index < catalogue.size();) {
				bool duplicate = false;
				for (size_t earlier = 0; earlier < index; ++earlier) {
					if (catalogue[earlier].name == catalogue[index].name) {
						duplicate = true;
						break;
					}
				}
				if (duplicate) {
					diagnostics.emplace_back("package '" + catalogue[index].name + "' is declared twice. Ignoring the copy at '" + catalogue[index].directory.ToString() + "'");
					catalogue.erase(catalogue.begin() + static_cast<ptrdiff_t>(index));
					continue;
				}
				++index;
			}

			bool changed = true;
			while (changed) {
				changed = DropFirstWithMissingDependency(catalogue, diagnostics) || DropFirstCycle(catalogue, diagnostics);
			}
		}

		void ResolveEnabledSet(std::span<const PackageManifest> catalogue, std::span<const str> stored, Vector<ResolvedPackage>& out) {
			out.clear();

			Vector<str> reached;
			for (const PackageManifest& manifest : catalogue) {
				if (IsAlwaysOn(manifest)) {
					CollectReachable(catalogue, manifest.name, reached);
				}
			}
			for (const str& name : stored) {
				CollectReachable(catalogue, name, reached);
			}

			for (const str& name : reached) {
				const PackageManifest* manifest = FindPackage(catalogue, name);
				ResolvedPackage entry;
				entry.name = name;
				entry.requested = Contains(stored, name);
				entry.alwaysOn = manifest && IsAlwaysOn(*manifest);
				out.push_back(std::move(entry));
			}

			for (ResolvedPackage& entry : out) {
				for (const str& other : reached) {
					const PackageManifest* dependent = FindPackage(catalogue, other);
					if (!dependent) {
						continue;
					}
					for (const PackageDependency& dependency : dependent->dependencies) {
						if (dependency.name == entry.name) {
							entry.requiredBy.emplace_back(other);
							break;
						}
					}
				}
				std::sort(entry.requiredBy.begin(), entry.requiredBy.end());
			}

			std::sort(out.begin(), out.end(), [](const ResolvedPackage& a, const ResolvedPackage& b) { return a.name < b.name; });
		}

		bool IsEnabled(std::span<const ResolvedPackage> resolution, std::string_view name) {
			for (const ResolvedPackage& entry : resolution) {
				if (entry.name == name) {
					return true;
				}
			}
			return false;
		}

		bool CanDisable(std::span<const ResolvedPackage> resolution, std::string_view name, Vector<str>& blockers) {
			blockers.clear();
			for (const ResolvedPackage& entry : resolution) {
				if (entry.name != name) {
					continue;
				}
				if (entry.alwaysOn) {
					blockers.emplace_back("the engine");
					return false;
				}
				for (const str& dependent : entry.requiredBy) {
					blockers.emplace_back(dependent);
				}
				return blockers.empty();
			}
			return true;
		}

		void LoadBuiltInCatalogue(Vector<PackageManifest>& out, Vector<str>& diagnostics) {
			out.clear();

			str executablePath = ScriptProjectOps::GetEditorExecutablePath();
			if (executablePath.empty()) {
				diagnostics.push_back("the running editor's path is unknown (no built in packages found)");
				return;
			}

			std::filesystem::path root = std::filesystem::path(executablePath).parent_path() / "Packages";
			if (!std::filesystem::exists(root)) {
				return;
			}

			const Path roots[] = {Path(root.generic_string())};
			DiscoverPackages(roots, out, diagnostics);
			ValidateCatalogue(out, diagnostics);
		}

		str GetPackageBinaryName(const PackageManifest& manifest) {
			str directory = manifest.directory.ToString();
			size_t slash = directory.find_last_of('/');
			if (slash == str::npos) {
				return directory;
			}
			return directory.substr(slash + 1);
		}

		bool FindUnloadedPackageImports(const Path& binary, std::span<const PackageManifest> catalogue,
										std::span<const str> loaded, Vector<str>& out) {
			out.clear();
			Vector<str> imported;
			if (!EditorPlatform::ReadImportedLibraries(binary, imported)) {
				return false;
			}
			for (str& library : imported) {
				library = StringUtil::ToLower(library);
			}

			for (const PackageManifest& manifest : catalogue) {
				if (std::find(loaded.begin(), loaded.end(), manifest.name) != loaded.end()) {
					continue;
				}
				const str binaryName = GetPackageBinaryName(manifest);
				if (binaryName.empty()) {
					continue;
				}
				const str library = StringUtil::ToLower(binaryName + str(EditorPlatform::GetSharedLibrarySuffix()));
				if (std::find(imported.begin(), imported.end(), library) != imported.end()) {
					out.push_back(manifest.name);
				}
			}
			std::sort(out.begin(), out.end());
			return true;
		}

		void SelectGamePackages(std::span<const PackageManifest> catalogue, std::span<const ResolvedPackage> resolution, Vector<str>& out) {
			out.clear();
			for (const ResolvedPackage& resolved : resolution) {
				const PackageManifest* manifest = FindPackage(catalogue, resolved.name);
				if (!manifest) {
					continue;
				}
				if (manifest->kind == PackageKind::Engine || manifest->kind == PackageKind::Package) {
					out.push_back(resolved.name);
				}
			}
		}

		namespace {

			str EncodeEnabledSet(std::span<const str> names) {
				Vector<str> sorted;
				for (const str& name : names) {
					if (!name.empty() && !Contains(sorted, name)) {
						sorted.emplace_back(name);
					}
				}
				std::sort(sorted.begin(), sorted.end());

				str encoded;
				for (const str& name : sorted) {
					if (!encoded.empty()) {
						encoded.push_back(ENABLED_SEPARATOR);
					}
					encoded += name;
				}
				return encoded;
			}

			EnabledPackagesSettings* Category() {
				return Settings::DeclareCategory<EnabledPackagesSettings>(SettingsCategory, SettingScope::Project);
			}

		} // namespace

		void DeclareSettings() {
			(void)Category();
		}

		void ReadEnabledSet(Vector<str>& out) {
			out.clear();
			const EnabledPackagesSettings* live = Category();
			if (!live) {
				return;
			}
			std::string_view text = live->enabled;
			size_t start = 0;
			while (start <= text.size()) {
				size_t end = text.find(ENABLED_SEPARATOR, start);
				if (end == std::string_view::npos) {
					end = text.size();
				}
				std::string_view name = text.substr(start, end - start);
				while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
					name.remove_prefix(1);
				}
				while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
					name.remove_suffix(1);
				}
				if (!name.empty()) {
					out.emplace_back(name);
				}
				if (end == text.size()) {
					break;
				}
				start = end + 1;
			}
		}

		void WriteEnabledSet(std::span<const str> names) {
			EnabledPackagesSettings* live = Category();
			if (!live) {
				return;
			}

			str encoded = EncodeEnabledSet(names);
			if (encoded == live->enabled && live->format == STORED_SET_FORMAT) {
				return;
			}

			live->enabled = std::move(encoded);
			live->format = STORED_SET_FORMAT;
			Settings::NotifyChanged(SettingsCategory, SettingsField);
			if (!Settings::Save()) {
				SLT_LOG_WARN("Editor", "the enabled package set couldn't be written");
			}
		}

		bool UpgradeStoredSet(std::span<const PackageManifest> catalogue) {
			EnabledPackagesSettings* live = Category();
			if (!live || live->format >= STORED_SET_FORMAT) {
				return false;
			}
			const u32 format = live->format;
			Vector<str> stored;
			ReadEnabledSet(stored);

			if (format < 2) {
				for (str& name : stored) {
					if (name.starts_with(RetiredIdPrefix)) {
						name = str(IdPrefix) + name.substr(RetiredIdPrefix.size());
					}
				}
			}

			if (format < 1) {
				for (const PackageManifest& manifest : catalogue) {
					if (IsOptionalTool(manifest) && !Contains(stored, manifest.name)) {
						stored.push_back(manifest.name);
					}
				}
			}

			WriteEnabledSet(stored);
			return true;
		}

		u32 ScaffoldEnabledSet(const Path& projectDocument) {
			Vector<PackageManifest> catalogue;
			Vector<str> diagnostics;
			LoadBuiltInCatalogue(catalogue, diagnostics);
			return ScaffoldEnabledSet(projectDocument, catalogue);
		}

		u32 ScaffoldEnabledSet(const Path& projectDocument, std::span<const PackageManifest> catalogue) {
			if (projectDocument.ToString().empty()) {
				return 0;
			}

			Vector<str> names;
			for (const PackageManifest& manifest : catalogue) {
				if (manifest.scaffold && (manifest.kind == PackageKind::Engine || IsOptionalTool(manifest))) {
					names.push_back(manifest.name);
				}
			}
			if (names.empty()) {
				return 0;
			}

			Config document;
			document.Load(projectDocument);
			document.SetString(SettingsCategory, SettingsField, EncodeEnabledSet(names));
			document.SetI64(SettingsCategory, FormatField, STORED_SET_FORMAT);
			if (!document.Save(projectDocument)) {
				SLT_LOG_WARN("Editor", "couldn't write the built-in package set into '{}' (new project starts with none enabled)",
							 projectDocument.ToString());
				return 0;
			}
			return static_cast<u32>(names.size());
		}

		namespace {

			int KindGroup(PackageKind kind) {
				switch (kind) {
				case PackageKind::Engine:
					return 0;
				case PackageKind::Tool:
					return 1;
				case PackageKind::Package:
					return 2;
				case PackageKind::Game:
					break;
				}
				return 3;
			}

			std::string_view RowLabel(const PackageManifest& manifest) {
				return manifest.displayName.empty() ? std::string_view(manifest.name) : std::string_view(manifest.displayName);
			}

		} // anonymous namespace

		void BuildPackageRows(std::span<const PackageManifest> catalogue, std::span<const ResolvedPackage> resolution, Vector<PackageRow>& out) {
			out.clear();

			for (const PackageManifest& manifest : catalogue) {
				if (manifest.kind == PackageKind::Game) {
					continue;
				}

				PackageRow row;
				row.name = manifest.name;
				row.displayName = str(RowLabel(manifest));
				row.version = manifest.version;
				row.description = manifest.description;
				row.kind = manifest.kind;
				row.enabled = IsEnabled(resolution, manifest.name);

				const ResolvedPackage* resolved = nullptr;
				for (const ResolvedPackage& entry : resolution) {
					if (entry.name == manifest.name) {
						resolved = &entry;
						break;
					}
				}
				row.requested = resolved && resolved->requested;

				row.required = manifest.required;
				if (IsAlwaysOn(manifest)) {
					row.canToggle = false;
				} else if (!row.enabled) {
					row.canToggle = true;
				} else {
					Vector<str> blockers;
					row.canToggle = CanDisable(resolution, manifest.name, blockers);
					if (!row.canToggle) {
						row.blockedBy = std::move(blockers);
					}
				}

				out.push_back(std::move(row));
			}

			std::sort(out.begin(), out.end(), [](const PackageRow& a, const PackageRow& b) {
				int groupA = KindGroup(a.kind);
				int groupB = KindGroup(b.kind);
				if (groupA != groupB) {
					return groupA < groupB;
				}
				return a.displayName < b.displayName;
			});
		}

		bool SetPackageEnabled(std::span<const PackageManifest> catalogue, std::string_view name, bool enabled, Vector<str>& blockers) {
			blockers.clear();

			const PackageManifest* manifest = FindPackage(catalogue, name);
			if (!manifest) {
				SLT_LOG_WARN("Editor", "no package named '{}' is in this engine's catalogue", name);
				return false;
			}
			if (IsAlwaysOn(*manifest)) {
				blockers.emplace_back("the engine");
				return false;
			}

			UpgradeStoredSet(catalogue);
			Vector<str> stored;
			ReadEnabledSet(stored);

			if (enabled) {
				if (Contains(stored, name)) {
					return true;
				}
				stored.emplace_back(name);
				WriteEnabledSet(stored);
				return true;
			}

			Vector<ResolvedPackage> resolution;
			ResolveEnabledSet(catalogue, stored, resolution);
			if (!CanDisable(resolution, name, blockers)) {
				return false;
			}

			stored.erase(std::remove(stored.begin(), stored.end(), str(name)), stored.end());
			WriteEnabledSet(stored);
			return true;
		}

	} // namespace PackageOps

} // namespace Shalltear
