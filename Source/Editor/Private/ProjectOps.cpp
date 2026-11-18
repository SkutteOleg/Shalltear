#include "Shalltear/Editor/ProjectOps.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/BuildInfo.h"
#include "Shalltear/Core/Config.h"
#include "Shalltear/Core/Graphics/GraphicsProfile.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneSystem.h"

#include "Shalltear/Editor/AssetBrowserOps.h"
#include "Shalltear/Editor/BuildProfile.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/PackageOps.h"
#include "Shalltear/Editor/ScriptProjectOps.h"

#include "Shalltear/Editor/ChildProcess.h"
#include "Platform/EditorPlatform.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <system_error>

namespace Shalltear::ProjectOps {

	namespace {

		namespace fs = std::filesystem;

		constexpr const char* PROJECT_FILE = "Project.ini";
		constexpr const char* PROJECT_SETTINGS_FILE = "ProjectSettings.ini";
		constexpr i64 REGISTRY_VERSION = 1;

		constexpr const char* FIRST_SCENE_NAME = "Main";

		char ToLowerAscii(char c) {
			return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
		}

		bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle) {
			if (needle.empty()) {
				return true;
			}
			if (needle.size() > haystack.size()) {
				return false;
			}
			for (size_t start = 0; start + needle.size() <= haystack.size(); ++start) {
				size_t i = 0;
				while (i < needle.size() && ToLowerAscii(haystack[start + i]) == ToLowerAscii(needle[i])) {
					++i;
				}
				if (i == needle.size()) {
					return true;
				}
			}
			return false;
		}

		Path ProjectFilePath(const str& projectPath) {
			return Path(projectPath + "/" + PROJECT_FILE);
		}

		u64 NowUnix() {
			auto now = std::chrono::system_clock::now().time_since_epoch();
			auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();
			return seconds > 0 ? static_cast<u64>(seconds) : 0;
		}

		u64 ReadStamp(const Config& config, std::string_view section, std::string_view key) {
			i64 value = config.GetI64(section, key, 0);
			return value > 0 ? static_cast<u64>(value) : 0;
		}

		str FolderName(const str& projectPath) {
			size_t slash = projectPath.find_last_of('/');
			return slash == str::npos ? projectPath : projectPath.substr(slash + 1);
		}

		void RefreshEntry(ProjectEntry& entry) {
			entry.status = GetStatus(entry.path);
			if (entry.status == ProjectStatus::Ok) {
				str name;
				u64 created = 0;
				if (ReadProjectFile(entry.path, name, created)) {
					if (!name.empty()) {
						entry.name = name;
					}
					if (created > 0) {
						entry.createdUnix = created;
					}
				}
				if (entry.createdUnix == 0) {
					entry.createdUnix = FileSystem::GetCreationTimeUnix(Path(entry.path));
				}
			}
			if (entry.name.empty()) {
				entry.name = FolderName(entry.path);
			}
			u64 modified = GetModifiedTime(entry.path);
			entry.modifiedUnix = modified > entry.lastOpenedUnix ? modified : entry.lastOpenedUnix;
		}

		void ReadRegistryFile(const Path& registryPath, Vector<ProjectEntry>& out) {
			out.clear();
			Config config;
			if (!config.Load(registryPath)) {
				return;
			}
			if (config.GetI64("Hub", "version", 0) != REGISTRY_VERSION) {
				SLT_LOG_INFO("Editor", "project registry version mismatch (starting a fresh list)");
				return;
			}
			for (u32 i = 0;; ++i) {
				str section = std::format("Project.{}", i);
				if (!config.HasSection(section)) {
					break;
				}
				str path = NormalizePath(config.GetString(section, "path"));
				if (path.empty()) {
					continue;
				}
				ProjectEntry entry;
				entry.path = std::move(path);
				entry.name = config.GetString(section, "name");
				entry.createdUnix = ReadStamp(config, section, "created");
				entry.lastOpenedUnix = ReadStamp(config, section, "lastOpened");
				out.push_back(std::move(entry));
			}
		}

		i32 IndexOf(const Vector<ProjectEntry>& entries, const str& normalizedPath) {
			for (size_t i = 0; i < entries.size(); ++i) {
				if (PathsEqual(entries[i].path, normalizedPath)) {
					return static_cast<i32>(i);
				}
			}
			return -1;
		}

		bool ScaffoldFirstScene(const str& root) {
			if (!SceneSystem::IsInitialized()) {
				return false;
			}
			Handle<Scene> scene = SceneSystem::CreateScene(FIRST_SCENE_NAME);
			if (!scene.IsValid()) {
				return false;
			}
			EditorSceneOps::PopulateNewScene(scene);
			str path = root + "/Assets/Scenes/" + FIRST_SCENE_NAME + ".scene";
			bool saved = SceneSystem::SaveScene(scene, Path(path));
			if (!saved) {
				SLT_LOG_WARN("Editor", "CreateProject: couldn't write the first scene to '{}'", path);
			}
			SceneSystem::UnloadScene(scene);
			return saved;
		}

		bool ScaffoldQualityProfiles(const str& root) {
			struct Tier {
				const char* fileName;
				i32 msaa;
				i32 pcfTaps;
				i32 atlas;
				i32 cascades;
				f32 shadowDistance;
				f32 firstCascadeDistance;
				f32 lodBias;
				bool lodCrossFade;
			};
			static constexpr Tier Tiers[] = {
				{"1 Low.slprofile", 1, 3, 1024, 2, 40.0f, 10.0f, 0.4f, false},
				{"2 Medium.slprofile", 2, 5, 2048, 3, 60.0f, 6.0f, 0.7f, true},
				{"3 High.slprofile", 4, 5, 2048, 4, 60.0f, 4.0f, 1.0f, true},
				{"4 Ultra.slprofile", 4, 7, 4096, 4, 120.0f, 8.0f, 2.0f, true},
			};

			std::error_code ec;
			const std::filesystem::path settings = std::filesystem::path{root} / "Assets" / "Settings";
			if (!std::filesystem::create_directories(settings, ec) && ec) {
				SLT_LOG_WARN("Editor", "CreateProject: couldn't create '{}' ({})", settings.generic_string(), ec.message());
				return false;
			}

			u32 written = 0;
			for (const Tier& tier : Tiers) {
				GraphicsProfile* profile = New<GraphicsProfile>(MemTag::Asset);
				profile->msaa = tier.msaa;
				profile->shadowPcfTaps = tier.pcfTaps;
				profile->shadowAtlasSize = tier.atlas;
				profile->shadowCascades = tier.cascades;
				profile->shadowDistance = tier.shadowDistance;
				profile->shadowFirstCascadeDistance = tier.firstCascadeDistance;
				profile->lodBias = tier.lodBias;
				profile->lodCrossFade = tier.lodCrossFade;
				profile->tonemap = TonemapCurve::Neutral;
				const bool saved = profile->Export(Path((settings / tier.fileName).generic_string()));
				Delete(profile);
				if (!saved) {
					SLT_LOG_WARN("Editor", "CreateProject: couldn't write '{}'", tier.fileName);
					continue;
				}
				++written;
			}
			return written == std::size(Tiers);
		}

	} // anonymous namespace

	str NormalizePath(const str& path) {
		if (path.empty()) {
			return {};
		}
		std::error_code ec;
		fs::path absolute = fs::absolute(fs::path{path}, ec);
		if (ec) {
			return {};
		}
		str text = absolute.lexically_normal().generic_string();
		while (text.size() > 1 && text.back() == '/' && !(text.size() == 3 && text[1] == ':')) {
			text.pop_back();
		}
		return text;
	}

	bool PathsEqual(const str& a, const str& b) {
		if (a.size() != b.size()) {
			return false;
		}
		if (!EditorPlatform::FileNamesFoldCase()) {
			return a == b;
		}
		for (size_t i = 0; i < a.size(); ++i) {
			if (ToLowerAscii(a[i]) != ToLowerAscii(b[i])) {
				return false;
			}
		}
		return true;
	}

	bool IsValidProjectName(const str& name) {
		if (name.empty() || name == "." || name == "..") {
			return false;
		}
		for (char c : name) {
			if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
				c == '"' || c == '<' || c == '>' || c == '|') {
				return false;
			}
		}
		return name.back() != '.' && name.back() != ' ';
	}

	ProjectStatus GetStatus(const str& projectPath) {
		str normalized = NormalizePath(projectPath);
		if (normalized.empty()) {
			return ProjectStatus::Missing;
		}
		std::error_code ec;
		if (!fs::is_directory(fs::path{normalized}, ec) || ec) {
			return ProjectStatus::Missing;
		}
		if (!fs::is_regular_file(fs::path{normalized} / PROJECT_FILE, ec) || ec) {
			return ProjectStatus::Invalid;
		}
		return ProjectStatus::Ok;
	}

	bool IsProjectDirectory(const str& projectPath) {
		return GetStatus(projectPath) == ProjectStatus::Ok;
	}

	bool ReadProjectFile(const str& projectPath, str& outName, u64& outCreatedUnix) {
		str normalized = NormalizePath(projectPath);
		if (normalized.empty()) {
			return false;
		}
		Config config;
		if (!config.Load(ProjectFilePath(normalized))) {
			return false;
		}
		outName = config.GetString("Project", "name");
		outCreatedUnix = ReadStamp(config, "Project", "created");
		return true;
	}

	u64 GetModifiedTime(const str& projectPath) {
		str normalized = NormalizePath(projectPath);
		if (normalized.empty()) {
			return 0;
		}
		u64 projectFile = FileSystem::GetLastWriteTimeUnix(ProjectFilePath(normalized));
		u64 assets = FileSystem::GetLastWriteTimeUnix(Path(normalized + "/Assets"));
		return projectFile > assets ? projectFile : assets;
	}

	bool CreateProject(const str& parentDirectory, const str& name, str& outPath) {
		if (!IsValidProjectName(name)) {
			SLT_LOG_WARN("Editor", "CreateProject: invalid project name '{}'", name);
			return false;
		}
		str parent = NormalizePath(parentDirectory);
		if (parent.empty()) {
			SLT_LOG_WARN("Editor", "CreateProject: no location given");
			return false;
		}
		str root = parent + "/" + name;
		std::error_code ec;
		if (fs::exists(fs::path{root}, ec)) {
			SLT_LOG_WARN("Editor", "CreateProject: '{}' already exists", root);
			return false;
		}
		if (!fs::create_directories(fs::path{root} / "Assets" / "Scenes", ec) || ec) {
			SLT_LOG_WARN("Editor", "CreateProject: couldn't create '{}' ({})", root,
						 ec ? ec.message() : str("already exists"));
			return false;
		}

		if (ScaffoldQualityProfiles(root)) {
			Config graphics;
			graphics.SetI64("Graphics", "qualityLevel", 2);
			if (!graphics.Save(Path(root + "/" + PROJECT_SETTINGS_FILE))) {
				SLT_LOG_WARN("Editor", "CreateProject: couldn't write the quality level into {}", PROJECT_SETTINGS_FILE);
			}
		}

		const u32 features = PackageOps::ScaffoldEnabledSet(Path(root + "/" + PROJECT_SETTINGS_FILE));

		Config config;
		config.SetString("Project", "name", name);
		config.SetI64("Project", "created", static_cast<i64>(NowUnix()));
		config.SetString("Project", "engineVersion", BuildInfo::GetVersionString());
		if (!config.Save(ProjectFilePath(root))) {
			SLT_LOG_WARN("Editor", "CreateProject: couldn't write {} in '{}'", PROJECT_FILE, root);
			fs::remove_all(fs::path{root}, ec);
			return false;
		}

		ScaffoldFirstScene(root);

		if (!BuildProfileOps::CreateProfile(root, "Default")) {
			SLT_LOG_WARN("Editor", "created '{}' but couldn't scaffold its Default build profile", root);
		}

		outPath = root;
		SLT_LOG_INFO("Editor", "created project '{}' at {} with {} built-in feature(s) enabled", name, root, features);
		return true;
	}

	bool CreateProject(const str& parentDirectory, const str& name, const CreateOptions& options, str& outPath) {
		if (!CreateProject(parentDirectory, name, outPath)) {
			return false;
		}
		if (!options.includeCppModule) {
			return true;
		}

		ScriptProjectOps::ScaffoldOptions scaffold;
		scaffold.moduleName = name;
		scaffold.sdkRoot = options.sdkRoot;
		scaffold.editorExecutable = options.editorExecutable;
		ScriptProjectOps::ScaffoldResult result;
		if (!ScriptProjectOps::ScaffoldScriptProject(outPath, scaffold, result)) {
			SLT_LOG_WARN("Editor", "created '{}' but couldn't scaffold its C++ module", outPath);
		}
		return true;
	}

	Path GetDefaultRegistryPath() {
		Path userData = FileSystem::GetUserDataDirectory();
		if (userData.ToString().empty()) {
			return Path("Library/Shalltear/Projects.ini");
		}
		return userData / "Shalltear" / "Projects.ini";
	}

	void LoadRegistry(const Path& registryPath, Vector<ProjectEntry>& out) {
		ReadRegistryFile(registryPath, out);
		for (ProjectEntry& entry : out) {
			RefreshEntry(entry);
		}
	}

	bool SaveRegistry(const Path& registryPath, const Vector<ProjectEntry>& entries) {
		Config config;
		config.SetI64("Hub", "version", REGISTRY_VERSION);
		for (size_t i = 0; i < entries.size(); ++i) {
			const ProjectEntry& entry = entries[i];
			str section = std::format("Project.{}", i);
			config.SetString(section, "path", entry.path);
			config.SetString(section, "name", entry.name);
			config.SetI64(section, "created", static_cast<i64>(entry.createdUnix));
			config.SetI64(section, "lastOpened", static_cast<i64>(entry.lastOpenedUnix));
		}
		str directory = registryPath.GetDirectory();
		if (!directory.empty()) {
			FileSystem::CreateDirectory(Path(directory));
		}
		if (!config.Save(registryPath)) {
			SLT_LOG_WARN("Editor", "couldn't write the project registry at {}", registryPath.ToString());
			return false;
		}
		return true;
	}

	bool AddProject(const Path& registryPath, const str& projectPath) {
		str normalized = NormalizePath(projectPath);
		if (!IsProjectDirectory(normalized)) {
			SLT_LOG_WARN("Editor", "'{}' isn't a Shalltear project ({} missing)", normalized, PROJECT_FILE);
			return false;
		}
		Vector<ProjectEntry> entries{GetMemoryResource(MemTag::Editor)};
		ReadRegistryFile(registryPath, entries);
		i32 existing = IndexOf(entries, normalized);
		ProjectEntry entry;
		if (existing >= 0) {
			entry = entries[static_cast<size_t>(existing)];
			entries.erase(entries.begin() + existing);
		}
		entry.path = normalized;
		RefreshEntry(entry);
		entries.insert(entries.begin(), std::move(entry));
		return SaveRegistry(registryPath, entries);
	}

	bool RemoveProject(const Path& registryPath, const str& projectPath) {
		str normalized = NormalizePath(projectPath);
		Vector<ProjectEntry> entries{GetMemoryResource(MemTag::Editor)};
		ReadRegistryFile(registryPath, entries);
		i32 existing = IndexOf(entries, normalized);
		if (existing < 0) {
			return false;
		}
		entries.erase(entries.begin() + existing);
		return SaveRegistry(registryPath, entries);
	}

	void TouchProject(const Path& registryPath, const str& projectPath) {
		str normalized = NormalizePath(projectPath);
		if (normalized.empty()) {
			return;
		}
		Vector<ProjectEntry> entries{GetMemoryResource(MemTag::Editor)};
		ReadRegistryFile(registryPath, entries);
		ProjectEntry entry;
		i32 existing = IndexOf(entries, normalized);
		if (existing >= 0) {
			entry = entries[static_cast<size_t>(existing)];
			entries.erase(entries.begin() + existing);
		}
		entry.path = normalized;
		entry.lastOpenedUnix = NowUnix();
		RefreshEntry(entry);
		entries.insert(entries.begin(), std::move(entry));
		SaveRegistry(registryPath, entries);
	}

	bool DeleteProjectFromDisk(const Path& registryPath, const str& projectPath) {
		str normalized = NormalizePath(projectPath);
		if (!IsProjectDirectory(normalized)) {
			SLT_LOG_WARN("Editor", "won't delete '{}': not a Shalltear project", normalized);
			return false;
		}
		if (PathsEqual(normalized, GetMountedProjectPath())) {
			SLT_LOG_WARN("Editor", "won't delete '{}': it's the open project", normalized);
			return false;
		}
		std::error_code ec;
		fs::remove_all(fs::path{normalized}, ec);
		if (ec) {
			SLT_LOG_WARN("Editor", "delete failed for '{}' ({})", normalized, ec.message());
			return false;
		}
		RemoveProject(registryPath, normalized);
		SLT_LOG_INFO("Editor", "deleted project '{}' from disk", normalized);
		return true;
	}

	bool MatchesFilter(const ProjectEntry& entry, std::string_view filter) {
		return ContainsIgnoreCase(entry.name, filter) || ContainsIgnoreCase(entry.path, filter);
	}

	bool MountProject(const str& projectPath) {
		str normalized = NormalizePath(projectPath);
		ProjectStatus status = GetStatus(normalized);
		if (status != ProjectStatus::Ok) {
			SLT_LOG_ERROR("Editor", "cannot open '{}': {}", normalized,
						  status == ProjectStatus::Missing ? "the path doesn't exist" : "not a Shalltear project");
			return false;
		}
		FileSystem::Mount("project", Path(normalized));
		SLT_LOG_INFO("Editor", "project:// -> {}", normalized);
		return true;
	}

	str GetMountedProjectPath() {
		return NormalizePath(AssetBrowserOps::GetProjectRootPhysical().ToString());
	}

	str BuildRelaunchCommandLine(const str& executablePath, std::span<const str> arguments,
								 const str& projectPath) {
		if (executablePath.empty()) {
			return {};
		}
		str line = QuoteCommandArgument(executablePath);
		for (size_t i = 0; i < arguments.size(); ++i) {
			if (arguments[i] == "--project") {
				++i;
				continue;
			}
			line += " " + QuoteCommandArgument(arguments[i]);
		}
		if (!projectPath.empty()) {
			line += " --project " + QuoteCommandArgument(projectPath);
		}
		return line;
	}

} // namespace Shalltear::ProjectOps
