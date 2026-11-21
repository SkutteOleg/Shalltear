#include "Shalltear/Editor/BuildProfile.h"

#include "Shalltear/Core/Config.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/ProjectLayout.h"
#include "Shalltear/Core/Serialization/JSON.h"

#include "Shalltear/Editor/ScriptProjectOps.h"

#include "EditorConfig.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace Shalltear {

	namespace {

		namespace fs = std::filesystem;

		constexpr const char* PROFILE_EXTENSION = ".buildprofile";
		constexpr const char* PROFILES_DIRECTORY = "BuildProfiles";
		constexpr const char* PROJECT_FILE = "Project.ini";
		constexpr const char* BUILD_SECTION = "Build";

		constexpr const char* COUNTER_SECTION = "Counters";

		char ToLowerAscii(char c) {
			return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
		}

		bool NamesEqual(std::string_view a, std::string_view b) {
			if (a.size() != b.size()) {
				return false;
			}
			for (size_t i = 0; i < a.size(); ++i) {
				if (ToLowerAscii(a[i]) != ToLowerAscii(b[i])) {
					return false;
				}
			}
			return true;
		}

		void AddIssue(Vector<BuildIssue>& out, BuildIssueSeverity severity, str profile, str key, str message) {
			out.push_back(BuildIssue{severity, std::move(profile), std::move(key), std::move(message)});
		}

		str EncodeString(std::string_view value) {
			return JSONSerialize(JSONValue::MakeString(value), false);
		}

		str EncodeList(const Vector<str>& items) {
			JSONValue array = JSONValue::MakeArray();
			for (const str& item : items) {
				array.Push(JSONValue::MakeString(item));
			}
			return JSONSerialize(array, true);
		}

		str DecodeString(const str& json) {
			LinearAllocator arena(json.size() + 64, MemTag::Editor);
			JSONValue value = JSONParse(json, arena);
			if (value.IsString()) {
				return str(value.AsString());
			}
			return json;
		}

		bool DecodeList(const str& json, Vector<str>& out) {
			LinearAllocator arena(json.size() + 64, MemTag::Editor);
			JSONValue value = JSONParse(json, arena);
			if (!value.IsArray()) {
				return false;
			}
			for (size_t i = 0; i < value.Size(); ++i) {
				const JSONValue& entry = value[i];
				out.push_back(entry.IsString() ? str(entry.AsString()) : JSONSerialize(entry, false));
			}
			return true;
		}

		void AppendIndented(str& out, const str& json, u32 indent) {
			for (size_t i = 0; i < json.size(); ++i) {
				out += json[i];
				if (json[i] == '\n') {
					for (u32 t = 0; t < indent; ++t) {
						out += '\t';
					}
				}
			}
		}

		str FileStem(const str& path) {
			str name = Path(path).GetFilename();
			size_t dot = name.find_last_of('.');
			return dot == str::npos ? name : name.substr(0, dot);
		}

		bool IsReservedDeviceName(std::string_view name) {
			static constexpr std::string_view Devices[] = {
				"con", "prn", "aux", "nul",
				"com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
				"lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};

			str lowered;
			lowered.reserve(name.size());
			for (char c : name) {
				lowered += ToLowerAscii(c);
			}
			for (std::string_view device : Devices) {
				if (lowered == device) {
					return true;
				}
			}
			return false;
		}

		bool ReadEnvironment(const str& name, str& out) {
			const char* value = std::getenv(name.c_str());
			if (!value || !*value) {
				return false;
			}
			out.assign(value);
			return true;
		}

		str ExpandValue(std::string_view raw, std::string_view key, const str& profileName, Vector<BuildIssue>& issues) {
			str out;
			out.reserve(raw.size());
			for (size_t i = 0; i < raw.size();) {
				if (raw[i] != '$') {
					out += raw[i++];
					continue;
				}
				if (i + 2 < raw.size() && raw[i + 1] == '$' && raw[i + 2] == '{') {
					out += "${";
					i += 3;
					continue;
				}
				if (i + 1 >= raw.size() || raw[i + 1] != '{') {
					out += raw[i++];
					continue;
				}
				size_t close = raw.find('}', i + 2);
				if (close == std::string_view::npos) {
					AddIssue(issues, BuildIssueSeverity::Error, profileName, str(key),
							 "unterminated '${' in the value. Write '$${' for a literal one");
					out.append(raw.substr(i));
					break;
				}
				std::string_view token = raw.substr(i + 2, close - i - 2);
				if (token.rfind("ENV:", 0) == 0) {
					str variable(token.substr(4));
					str value;
					if (variable.empty()) {
						AddIssue(issues, BuildIssueSeverity::Error, profileName, str(key),
								 "'${ENV:}' has no variable name");
						out.append(raw.substr(i, close - i + 1));
					} else if (ReadEnvironment(variable, value)) {
						out += value;
					} else {
						AddIssue(issues, BuildIssueSeverity::Error, profileName, str(key),
								 "environment variable '" + variable + "' is not set");
						out.append(raw.substr(i, close - i + 1));
					}
				} else if (token == "AUTO") {
					if (key != "buildNumber") {
						AddIssue(issues, BuildIssueSeverity::Error, profileName, str(key),
								 "'${AUTO}' is only legal in 'buildNumber'");
					}
					out.append(raw.substr(i, close - i + 1));
				} else {
					AddIssue(issues, BuildIssueSeverity::Warning, profileName, str(key),
							 "unknown expansion '${" + str(token) + "}'. Write '$${' for a literal one");
					out.append(raw.substr(i, close - i + 1));
				}
				i = close + 1;
			}
			return out;
		}

	} // anonymous namespace

	const BuildProfile::Setting* BuildProfile::Find(std::string_view key) const {
		for (const Setting& setting : _settings) {
			if (setting.key == key) {
				return &setting;
			}
		}
		return nullptr;
	}

	void BuildProfile::Set(std::string_view key, str json) {
		for (Setting& setting : _settings) {
			if (setting.key == key) {
				setting.json = std::move(json);
				return;
			}
		}
		_settings.push_back(Setting{str(key), std::move(json)});
	}

	bool BuildProfile::Has(std::string_view key) const {
		return Find(key) != nullptr;
	}

	bool BuildProfile::Remove(std::string_view key) {
		for (size_t i = 0; i < _settings.size(); ++i) {
			if (_settings[i].key == key) {
				_settings.erase(_settings.begin() + static_cast<ptrdiff_t>(i));
				return true;
			}
		}
		return false;
	}

	std::string_view BuildProfile::SettingKeyAt(size_t index) const {
		return index < _settings.size() ? std::string_view(_settings[index].key) : std::string_view{};
	}

	str BuildProfile::GetString(std::string_view key, std::string_view fallback) const {
		const Setting* setting = Find(key);
		return setting ? DecodeString(setting->json) : str(fallback);
	}

	void BuildProfile::SetString(std::string_view key, std::string_view value) {
		Set(key, EncodeString(value));
	}

	bool BuildProfile::GetList(std::string_view key, Vector<str>& out) const {
		const Setting* setting = Find(key);
		if (!setting) {
			return false;
		}
		return DecodeList(setting->json, out);
	}

	void BuildProfile::SetList(std::string_view key, const Vector<str>& items) {
		Set(key, EncodeList(items));
	}

	class BuildProfileReader {
	public:
		static bool Read(const str& text, const str& name, BuildProfile& out) {
			LinearAllocator arena(text.size() + 64, MemTag::Editor);
			JSONValue root = JSONParse(text, arena);
			if (!root.IsObject()) {
				return false;
			}

			BuildProfile profile;
			profile.name = name;
			for (size_t i = 0; i < root.MemberCount(); ++i) {
				std::string_view key = root.KeyAt(i);
				const JSONValue& value = root.ChildAt(i);
				if (key == "formatVersion") {
					profile.formatVersion = static_cast<i32>(value.AsInt(1));
				} else if (key == "inherits") {
					profile.inherits = str(value.AsString());
				} else if (key == "target") {
					profile.target = str(value.AsString());
				} else if (key == "settings") {
					for (size_t s = 0; s < value.MemberCount(); ++s) {
						profile._settings.push_back(
							BuildProfile::Setting{str(value.KeyAt(s)), JSONSerialize(value.ChildAt(s), true)});
					}
				} else {
					profile._extra.push_back(BuildProfile::Setting{str(key), JSONSerialize(value, true)});
				}
			}
			out = std::move(profile);
			return true;
		}
	};

	class BuildProfileWriter {
	public:
		static str Write(const BuildProfile& profile) {
			str out = "{\n";
			out += "\t\"formatVersion\": " + std::format("{}", profile.formatVersion);

			if (!profile.inherits.empty()) {
				out += ",\n\t\"inherits\": " + EncodeString(profile.inherits);
			}
			if (!profile.target.empty()) {
				out += ",\n\t\"target\": " + EncodeString(profile.target);
			}

			out += ",\n\t\"settings\": {";
			if (profile._settings.empty()) {
				out += "}";
			} else {
				out += "\n";
				for (size_t i = 0; i < profile._settings.size(); ++i) {
					const BuildProfile::Setting& setting = profile._settings[i];
					out += "\t\t" + EncodeString(setting.key) + ": ";
					AppendIndented(out, setting.json, 2);
					out += i + 1 < profile._settings.size() ? ",\n" : "\n";
				}
				out += "\t}";
			}

			for (const BuildProfile::Setting& extra : profile._extra) {
				out += ",\n\t" + EncodeString(extra.key) + ": ";
				AppendIndented(out, extra.json, 1);
			}

			out += "\n}\n";
			return out;
		}
	};

	namespace BuildProfileOps {

		void GetCoreSettings(Vector<BuildSettingDesc>& out) {
			auto add = [&out](const char* key, BuildSettingType type, BuildMergePolicy merge, const char* label,
							  const char* help, const char* defaultValue = "") {
				BuildSettingDesc desc;
				desc.key = key;
				desc.type = type;
				desc.merge = merge;
				desc.label = label;
				desc.help = help;
				desc.defaultValue = defaultValue;
				out.push_back(std::move(desc));
			};

			add("scenes", BuildSettingType::SceneList, BuildMergePolicy::Replace,
				"Scenes", "ordered, idx 0 is the boot scene. Only listed scenes are staged");

			add("includeInBuildAll", BuildSettingType::Bool, BuildMergePolicy::Replace,
				"Include in Build All", "excluded profiles are skipped by Build All/--build-all. Building one by name still works", "true");

			add("defines", BuildSettingType::StringList, BuildMergePolicy::Union,
				"Scripting Defines", "preprocessor defines for the player (accumulated down the inheritance chain)");

			add("configuration", BuildSettingType::Enum, BuildMergePolicy::Replace,
				"Configuration", "the CMake configuration the player is compiled in", "Release");
			out.back().enumValues.push_back("Debug");
			out.back().enumValues.push_back("Release");

			add("outputPath", BuildSettingType::Path, BuildMergePolicy::Replace,
				"Output Path", "project-relative. An absolute one is legal but flagged (only valid on 1 machine)", "Builds");

			add("executableName", BuildSettingType::String, BuildMergePolicy::Replace,
				"Executable Name", "the artifact's file name without an extension, empty = the project name");
			add("version", BuildSettingType::String, BuildMergePolicy::Replace,
				"Version", "a display version, expands ${ENV:...}");
			add("buildNumber", BuildSettingType::String, BuildMergePolicy::Replace,
				"Build Number", "expands ${ENV:...} and ${AUTO}, the only key ${AUTO} is legal in");
			add("buildCommand", BuildSettingType::String, BuildMergePolicy::Replace,
				"Build Command", "replaces the configure and compile pair wholesale");
			add("preBuildCommand", BuildSettingType::String, BuildMergePolicy::Replace,
				"Pre-Build Command", "run before the pipeline's 1st step");
			add("postBuildCommand", BuildSettingType::String, BuildMergePolicy::Replace,
				"Post-Build Command", "run after staging, alongside registered PostBuild steps (not instead of them)");

			add("stripDebugNames", BuildSettingType::Bool, BuildMergePolicy::Replace,
				"Strip Debug Names", "leave asset paths out of the cooked package. Smaller + a failed load logs a Guid (not a path)", "false");

			add("compressionMethod", BuildSettingType::Enum, BuildMergePolicy::Replace,
				"Compression Method", "how the cooked package's payload pages are compressed. Default resolves per target", "Default");
			out.back().enumValues.push_back("Default");
			out.back().enumValues.push_back("LZ4");
			out.back().enumValues.push_back("LZ4HC");
		}

		str GetProfilesDirectory(const str& projectPath) {
			return projectPath.empty() ? str{} : projectPath + "/" + PROFILES_DIRECTORY;
		}

		str GetProfilePath(const str& projectPath, const str& name) {
			str directory = GetProfilesDirectory(projectPath);
			return directory.empty() ? str{} : directory + "/" + name + PROFILE_EXTENSION;
		}

		bool IsValidProfileName(std::string_view name) {
			if (name.empty()) {
				return false;
			}
			for (char c : name) {
				if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
					c == '"' || c == '<' || c == '>' || c == '|' || static_cast<unsigned char>(c) < 0x20) {
					return false;
				}
			}
			if (name.front() == '.' || name.front() == ' ' || name.back() == '.' || name.back() == ' ') {
				return false;
			}
			return !IsReservedDeviceName(name);
		}

		bool LoadProfile(const str& filePath, BuildProfile& out) {
			str text = FileSystem::ReadTextFile(Path(filePath), MemTag::Editor);
			if (text.empty()) {
				return false;
			}
			if (!BuildProfileReader::Read(text, FileStem(filePath), out)) {
				SLT_LOG_WARN("Editor", "LoadProfile: '{}' isn't a build profile object", filePath);
				return false;
			}
			return true;
		}

		bool SaveProfile(const str& filePath, const BuildProfile& profile) {
			std::error_code ec;
			fs::create_directories(fs::path{filePath}.parent_path(), ec);
			return FileSystem::WriteTextFile(Path(filePath), BuildProfileWriter::Write(profile));
		}

		namespace {

			Path ProjectFilePath(const str& projectPath) {
				return Path(projectPath + "/" + PROJECT_FILE);
			}

			bool Contains(const Vector<str>& names, std::string_view name) {
				for (const str& entry : names) {
					if (NamesEqual(entry, name)) {
						return true;
					}
				}
				return false;
			}

			bool ContainsExact(const Vector<str>& values, std::string_view value) {
				for (const str& entry : values) {
					if (entry == value) {
						return true;
					}
				}
				return false;
			}

			void ScanProfileFiles(const str& projectPath, Vector<str>& out) {
				std::error_code ec;
				for (fs::directory_iterator it(fs::path{GetProfilesDirectory(projectPath)}, ec), end;
					 !ec && it != end; it.increment(ec)) {
					std::error_code typeEc;
					if (it->is_directory(typeEc) || typeEc) {
						continue;
					}
					if (it->path().extension() != PROFILE_EXTENSION) {
						continue;
					}
					out.push_back(it->path().stem().generic_string());
				}
				std::sort(out.begin(), out.end(), [](const str& a, const str& b) {
					for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
						char lhs = ToLowerAscii(a[i]);
						char rhs = ToLowerAscii(b[i]);
						if (lhs != rhs) {
							return lhs < rhs;
						}
					}
					return a.size() < b.size();
				});
			}

		} // anonymous namespace

		void ListProfiles(const str& projectPath, Vector<str>& out) {
			out.clear();
			if (projectPath.empty()) {
				return;
			}

			Vector<str> onDisk{GetMemoryResource(MemTag::Editor)};
			ScanProfileFiles(projectPath, onDisk);

			Config config;
			if (config.Load(ProjectFilePath(projectPath))) {
				for (u32 i = 0;; ++i) {
					str key = std::format("profile.{}", i);
					if (!config.Has(BUILD_SECTION, key)) {
						break;
					}
					str name = config.GetString(BUILD_SECTION, key);
					if (name.empty() || Contains(out, name) || !Contains(onDisk, name)) {
						continue;
					}
					out.push_back(std::move(name));
				}
			}

			for (str& name : onDisk) {
				if (!Contains(out, name)) {
					out.push_back(std::move(name));
				}
			}
		}

		bool WriteProfileRoster(const str& projectPath, const Vector<str>& names) {
			if (projectPath.empty()) {
				return false;
			}
			Path iniPath = ProjectFilePath(projectPath);
			Config config;
			config.Load(iniPath);

			config.RemoveSection(BUILD_SECTION);
			for (size_t i = 0; i < names.size(); ++i) {
				config.SetString(BUILD_SECTION, std::format("profile.{}", i), names[i]);
			}
			return config.Save(iniPath);
		}

		void ListBuildAllProfiles(const str& projectPath, Vector<str>& out) {
			out.clear();
			Vector<str> roster{GetMemoryResource(MemTag::Editor)};
			ListProfiles(projectPath, roster);
			if (roster.empty()) {
				return;
			}
			Vector<BuildSettingDesc> schema{GetMemoryResource(MemTag::Editor)};
			GetCoreSettings(schema);
			for (const str& name : roster) {
				ResolvedProfile resolved;
				ResolveProfile(projectPath, name, schema, resolved);
				const ResolvedSetting* included = resolved.Find("includeInBuildAll");
				if (!included || included->value != "false") {
					out.push_back(name);
				}
			}
		}

		void GetProjectScenes(const str& projectPath, Vector<str>& out) {
			out.clear();
			if (projectPath.empty()) {
				return;
			}
			std::error_code ec;
			for (std::string_view contentRoot : ProjectLayout::GetContentRoots()) {
				fs::path rootPath = fs::path{projectPath} / fs::path{contentRoot};
				for (fs::recursive_directory_iterator it(rootPath, ec), end; !ec && it != end; it.increment(ec)) {
					std::error_code fileEc;
					if (!it->is_regular_file(fileEc) || fileEc || it->path().extension() != ".scene") {
						continue;
					}
					str absolute = it->path().generic_string();
					str prefix = projectPath + "/";
					out.push_back(absolute.size() > prefix.size() && absolute.compare(0, prefix.size(), prefix) == 0
									  ? absolute.substr(prefix.size())
									  : std::move(absolute));
				}
				ec.clear();
			}
			std::sort(out.begin(), out.end());
		}

		namespace {

			str FindRootProfile(const str& projectPath, const Vector<str>& exclude) {
				Vector<str> names{GetMemoryResource(MemTag::Editor)};
				ListProfiles(projectPath, names);
				for (const str& candidate : names) {
					if (Contains(exclude, candidate)) {
						continue;
					}
					BuildProfile profile;
					if (LoadProfile(GetProfilePath(projectPath, candidate), profile) && profile.inherits.empty()) {
						return candidate;
					}
				}
				return {};
			}

		} // anonymous namespace

		void ResolveProfile(const str& projectPath, const str& name, const Vector<BuildSettingDesc>& schema,
							ResolvedProfile& out) {
			out = ResolvedProfile{};
			out.name = name;

			Vector<BuildProfile> loaded{GetMemoryResource(MemTag::Editor)};
			Vector<str> visited{GetMemoryResource(MemTag::Editor)};
			str current = name;
			bool substituted = false;
			while (!current.empty()) {
				if (Contains(visited, current)) {
					AddIssue(out.issues, BuildIssueSeverity::Error, name, {},
							 "inheritance cycle: '" + loaded.back().name + "' inherits '" + current +
								 "' (already in the chain)");
					break;
				}
				visited.push_back(current);

				BuildProfile profile;
				if (!LoadProfile(GetProfilePath(projectPath, current), profile)) {
					if (loaded.empty()) {
						AddIssue(out.issues, BuildIssueSeverity::Error, name, {},
								 "no profile named '" + current + "'");
						return;
					}
					str fallback = substituted ? str{} : FindRootProfile(projectPath, visited);
					AddIssue(out.issues, BuildIssueSeverity::Warning, name, {},
							 "'" + loaded.back().name + "' inherits '" + current +
								 "' (this project doesn't have it). Resolving " +
								 (fallback.empty() ? str("the truncated chain") : "'" + fallback + "'") + " instead");
					if (fallback.empty()) {
						break;
					}
					substituted = true;
					current = fallback;
					continue;
				}
				current = profile.inherits;
				loaded.push_back(std::move(profile));
			}

			std::reverse(loaded.begin(), loaded.end());
			for (const BuildProfile& profile : loaded) {
				out.chain.push_back(profile.name);
			}

			for (const BuildProfile& profile : loaded) {
				if (!profile.target.empty()) {
					out.target = profile.target;
					out.targetSource = profile.name;
				}
			}

			for (const BuildSettingDesc& desc : schema) {
				ResolvedSetting resolved;
				resolved.key = desc.key;
				resolved.type = desc.type;
				resolved.merge = desc.merge;
				resolved.rawValue = desc.defaultValue;
				resolved.rawItems = desc.defaultItems;
				resolved.itemSources.resize(resolved.rawItems.size());

				bool isList = desc.type == BuildSettingType::StringList || desc.type == BuildSettingType::SceneList;
				for (const BuildProfile& profile : loaded) {
					if (!profile.Has(desc.key)) {
						continue;
					}
					if (!isList || desc.merge == BuildMergePolicy::Replace) {
						if (isList) {
							resolved.rawItems.clear();
							profile.GetList(desc.key, resolved.rawItems);
							resolved.itemSources.assign(resolved.rawItems.size(), profile.name);
						} else {
							resolved.rawValue = profile.GetString(desc.key);
						}
						resolved.source = profile.name;
						continue;
					}

					Vector<str> contributed{GetMemoryResource(MemTag::Editor)};
					profile.GetList(desc.key, contributed);
					for (str& entry : contributed) {
						if (!ContainsExact(resolved.rawItems, entry)) {
							resolved.rawItems.push_back(std::move(entry));
							resolved.itemSources.push_back(profile.name);
						}
					}
					resolved.source = profile.name;
				}

				resolved.value = ExpandValue(resolved.rawValue, desc.key, name, out.issues);
				for (const str& item : resolved.rawItems) {
					resolved.items.push_back(ExpandValue(item, desc.key, name, out.issues));
				}
				out.settings.push_back(std::move(resolved));
			}
		}

		namespace {

			str FirstSegment(const str& path) {
				size_t start = 0;
				while (start < path.size() && (path[start] == '/' || path[start] == '\\')) {
					++start;
				}
				size_t end = start;
				while (end < path.size() && path[end] != '/' && path[end] != '\\') {
					++end;
				}
				return path.substr(start, end - start);
			}

			void ValidateOutputPath(const str& projectPath, const str& value, const str& profileName,
									Vector<BuildIssue>& out) {
				if (value.empty()) {
					AddIssue(out, BuildIssueSeverity::Error, profileName, "outputPath",
							 "the output path is empty");
					return;
				}

				str relative = value;
				if (Path(value).IsAbsolute()) {
					AddIssue(out, BuildIssueSeverity::Warning, profileName, "outputPath",
							 "'" + value + "' is an absolute path (only valid on 1 machine)");
					str normalized = Path(value).ToString();
					str root = Path(projectPath).ToString();
					if (normalized.size() > root.size() + 1 && normalized.compare(0, root.size(), root) == 0) {
						relative = normalized.substr(root.size() + 1);
					} else {
						return;
					}
				}

				str segment = FirstSegment(relative);
				for (std::string_view contentRoot : ProjectLayout::GetContentRoots()) {
					if (NamesEqual(segment, contentRoot)) {
						AddIssue(out, BuildIssueSeverity::Error, profileName, "outputPath",
								 "'" + value + "' is inside the content root '" + str(contentRoot) + "'");
						return;
					}
				}
			}

			void ValidateScenes(const str& projectPath, const ResolvedSetting& setting, const str& profileName,
								Vector<BuildIssue>& out) {
				if (setting.items.empty()) {
					AddIssue(out, BuildIssueSeverity::Warning, profileName, setting.key,
							 "no scenes are listed (nothing to load)");
					return;
				}
				std::error_code ec;
				for (const str& scene : setting.items) {
					fs::path candidate = Path(scene).IsAbsolute() ? fs::path{scene}
																  : fs::path{projectPath} / fs::path{scene};
					if (!fs::is_regular_file(candidate, ec) || ec) {
						ec.clear();
						AddIssue(out, BuildIssueSeverity::Error, profileName, setting.key,
								 "'" + scene + "' is not a file in this project");
					}
				}
			}

		} // anonymous namespace

		void ValidateProfile(const str& projectPath, const ResolvedProfile& profile,
							 const Vector<BuildSettingDesc>& schema, Vector<BuildIssue>& out) {
			if (!IsValidProfileName(profile.name)) {
				AddIssue(out, BuildIssueSeverity::Error, profile.name, {},
						 "'" + profile.name + "' is not a usable file name for a profile");
			}
			for (const BuildIssue& issue : profile.issues) {
				out.push_back(issue);
			}

			for (const ResolvedSetting& setting : profile.settings) {
				if (setting.type == BuildSettingType::Enum) {
					const BuildSettingDesc* desc = nullptr;
					for (const BuildSettingDesc& candidate : schema) {
						if (candidate.key == setting.key) {
							desc = &candidate;
						}
					}
					if (desc && !desc->enumValues.empty() && !ContainsExact(desc->enumValues, setting.value)) {
						AddIssue(out, BuildIssueSeverity::Error, profile.name, setting.key,
								 "'" + setting.value + "' is not one of this setting's values");
					}
				}

				if (setting.key == "outputPath") {
					ValidateOutputPath(projectPath, setting.value, profile.name, out);
				}

				if (setting.type == BuildSettingType::SceneList) {
					ValidateScenes(projectPath, setting, profile.name, out);
				}
			}
		}

		namespace {

			str ReadPackageVariable(const str& sdkRoot, std::string_view name) {
				for (const char* relative : {"/ShalltearConfig.cmake", "/Lib/cmake/Shalltear/ShalltearConfig.cmake"}) {
					str text = FileSystem::ReadTextFile(Path(sdkRoot + relative), MemTag::Editor);
					if (text.empty()) {
						continue;
					}
					const str marker = "set(" + str(name) + " \"";
					size_t at = text.find(marker);
					if (at == str::npos) {
						continue;
					}
					at += marker.size();
					size_t end = text.find('"', at);
					return end == str::npos ? str{} : text.substr(at, end - at);
				}
				return {};
			}

			bool IsStaticPackage(const str& candidate) {
				return !candidate.empty() && ReadPackageVariable(candidate, "SHALLTEAR_LINK_MODE") == "STATIC";
			}

			bool IsBuiltFor(const str& candidate, std::string_view targetTriple) {
				return targetTriple.empty() || ReadPackageVariable(candidate, "SHALLTEAR_COMPILER_TARGET") == targetTriple;
			}

			bool IsInstalledPackage(const str& sdkRoot) {
				std::error_code ec;
				return !sdkRoot.empty() &&
					   fs::exists(fs::path{sdkRoot} / "Lib" / "cmake" / "Shalltear" / "ShalltearConfig.cmake", ec);
			}

			bool FindSiblingStaticTree(std::string_view targetTriple, str& out) {
				str executable = ScriptProjectOps::GetEditorExecutablePath();
				if (executable.empty()) {
					return false;
				}
				std::error_code ec;
				fs::path binDirectory = fs::path{executable}.parent_path();
				fs::path treeRoot = binDirectory.parent_path();
				fs::path siblings = treeRoot.parent_path();
				if (siblings.empty()) {
					return false;
				}
				for (fs::directory_iterator it(siblings, ec), end; !ec && it != end; it.increment(ec)) {
					std::error_code typeEc;
					if (!it->is_directory(typeEc) || typeEc) {
						continue;
					}
					str candidate = it->path().generic_string();
					if (IsStaticPackage(candidate) && IsBuiltFor(candidate, targetTriple)) {
						out = std::move(candidate);
						return true;
					}
				}
				return false;
			}

			bool ReadStaticSDKEnvironment(str& out) {
				str value;
				if (!ReadEnvironment("SHALLTEAR_SDK_STATIC", value)) {
					return false;
				}
				out = Path(value).ToString();
				return true;
			}

		} // anonymous namespace

		void FindStaticSDKRoot(StaticSDKProbe& out, std::string_view targetTriple) {
			out = StaticSDKProbe{};

			str configured = EditorConfig::ReadBuildPreferences().staticSDKRoot;
			if (!configured.empty()) {
				if (!IsStaticPackage(configured)) {
					out.source = "the Static SDK Root setting";
					out.fix = "'" + configured + "' is not a STATIC Shalltear package. Fix or clear Static SDK Root in the Settings window (Editor > Build).";
					return;
				}
				if (IsBuiltFor(configured, targetTriple)) {
					out.source = "the Static SDK Root setting";
					out.root = std::move(configured);
					return;
				}
			}

			str sdkRoot = ScriptProjectOps::GetSDKRoot();

			if (IsStaticPackage(sdkRoot) && IsBuiltFor(sdkRoot, targetTriple)) {
				out.root = sdkRoot;
				out.source = "the running editor's package";
				return;
			}

			if (!sdkRoot.empty()) {
				str installed = sdkRoot + "/Static";
				if (IsStaticPackage(installed) && IsBuiltFor(installed, targetTriple)) {
					out.root = std::move(installed);
					out.source = "the SDK's Static flavor";
					return;
				}
				std::error_code ec;
				const fs::path staticDir(installed);
				if (fs::is_directory(staticDir, ec)) {
					for (const fs::directory_entry& entry : fs::directory_iterator(staticDir, ec)) {
						if (!entry.is_directory(ec)) {
							continue;
						}
						str nested = entry.path().generic_string();
						if (IsStaticPackage(nested) && IsBuiltFor(nested, targetTriple)) {
							out.root = std::move(nested);
							out.source = "the SDK's Static flavor";
							return;
						}
					}
				}
			}

			str sibling;
			if (FindSiblingStaticTree(targetTriple, sibling)) {
				out.root = std::move(sibling);
				out.source = "a sibling static build tree";
				return;
			}

			str fromEnvironment;
			if (ReadStaticSDKEnvironment(fromEnvironment)) {
				if (!IsStaticPackage(fromEnvironment)) {
					out.source = "SHALLTEAR_SDK_STATIC";
					out.fix = "SHALLTEAR_SDK_STATIC is '" + fromEnvironment + "' (not a STATIC Shalltear package).";
					return;
				}
				if (IsBuiltFor(fromEnvironment, targetTriple)) {
					out.source = "SHALLTEAR_SDK_STATIC";
					out.root = std::move(fromEnvironment);
					return;
				}
			}

			out.fix = IsInstalledPackage(sdkRoot)
						  ? "this SDK ships both flavors. Set Static SDK Root (Settings window, Editor > "
							"Build) to '" +
								sdkRoot + "/Static'."
						  : "dev checkout only exports flavor it was built in. Produce the static one with "
							"`python Scripts/build_sdk.py` (or point Static SDK Root at a static build tree).";
		}

		bool MigrateStaticSDKRootToUserScope(const str& projectPath) {
			if (projectPath.empty()) {
				return false;
			}
			Config project;
			if (!project.Load(ProjectFilePath(projectPath))) {
				return false;
			}
			str configured = project.GetString(BUILD_SECTION, "staticSdkRoot");
			if (!project.Has(BUILD_SECTION, "staticSdkRoot")) {
				return false;
			}

			if (!configured.empty()) {
				EditorConfig::BuildPreferences preferences = EditorConfig::ReadBuildPreferences();
				if (preferences.staticSDKRoot.empty()) {
					preferences.staticSDKRoot = configured;
					EditorConfig::WriteBuildPreferences(preferences);
				} else {
					configured.clear();
				}
			}

			project.Remove(BUILD_SECTION, "staticSdkRoot");
			if (!project.Save(ProjectFilePath(projectPath))) {
				SLT_LOG_WARN("Build", "moved [Build] staticSdkRoot into the settings store but couldn't rewrite Project.ini (removed next launch)");
				return true;
			}
			if (!configured.empty()) {
				SLT_LOG_INFO("Build", "moved [Build] staticSdkRoot out of the committed Project.ini into User scope: {}", configured);
			} else {
				SLT_LOG_INFO("Build", "removed an empty [Build] staticSdkRoot from Project.ini (now a User scope setting)");
			}
			return true;
		}

		u32 NextBuildNumber(const str& projectPath, std::string_view targetId) {
			if (projectPath.empty() || targetId.empty()) {
				return 0;
			}
			Path countersPath(projectPath + "/Library/BuildCounters.ini");

			Config counters;
			counters.Load(countersPath);

			i64 next = counters.GetI64(COUNTER_SECTION, targetId, 0) + 1;
			counters.SetI64(COUNTER_SECTION, targetId, next);

			std::error_code ec;
			fs::create_directories(fs::path{projectPath} / "Library", ec);
			if (!counters.Save(countersPath)) {
				SLT_LOG_WARN("Build", "couldn't persist the build counter to {} (next build reuses {})",
							 countersPath.ToString(), next);
			}
			return static_cast<u32>(next);
		}

		bool CreateProfile(const str& projectPath, const str& name, std::string_view inherits) {
			if (!IsValidProfileName(name)) {
				SLT_LOG_WARN("Editor", "CreateProfile: '{}' isn't a usable profile name", name);
				return false;
			}
			Vector<str> names{GetMemoryResource(MemTag::Editor)};
			ListProfiles(projectPath, names);
			if (Contains(names, name)) {
				SLT_LOG_WARN("Editor", "CreateProfile: '{}' already exists", name);
				return false;
			}

			BuildProfile profile;
			profile.name = name;
			profile.inherits = str(inherits);
			if (inherits.empty()) {
				Vector<str> scenes{GetMemoryResource(MemTag::Editor)};
				GetProjectScenes(projectPath, scenes);
				if (!scenes.empty()) {
					profile.SetList("scenes", scenes);
				}
			}
			if (!SaveProfile(GetProfilePath(projectPath, name), profile)) {
				return false;
			}
			names.push_back(name);
			return WriteProfileRoster(projectPath, names);
		}

		bool DuplicateProfile(const str& projectPath, const str& source, const str& newName) {
			if (!IsValidProfileName(newName)) {
				SLT_LOG_WARN("Editor", "DuplicateProfile: '{}' isn't a usable profile name", newName);
				return false;
			}
			Vector<str> names{GetMemoryResource(MemTag::Editor)};
			ListProfiles(projectPath, names);
			if (!Contains(names, source)) {
				SLT_LOG_WARN("Editor", "DuplicateProfile: no profile named '{}'", source);
				return false;
			}
			if (Contains(names, newName)) {
				SLT_LOG_WARN("Editor", "DuplicateProfile: '{}' already exists", newName);
				return false;
			}

			std::error_code ec;
			str destination = GetProfilePath(projectPath, newName);
			fs::create_directories(fs::path{destination}.parent_path(), ec);
			ec.clear();
			fs::copy_file(fs::path{GetProfilePath(projectPath, source)}, fs::path{destination},
						  fs::copy_options::overwrite_existing, ec);
			if (ec) {
				SLT_LOG_WARN("Editor", "DuplicateProfile: couldn't write '{}' ({})", destination, ec.message());
				return false;
			}

			size_t at = names.size();
			for (size_t i = 0; i < names.size(); ++i) {
				if (NamesEqual(names[i], source)) {
					at = i + 1;
					break;
				}
			}
			names.insert(names.begin() + static_cast<ptrdiff_t>(at), newName);
			return WriteProfileRoster(projectPath, names);
		}

		void GetProfileChildren(const str& projectPath, const str& name, Vector<str>& out) {
			out.clear();
			Vector<str> names{GetMemoryResource(MemTag::Editor)};
			ListProfiles(projectPath, names);
			for (const str& candidate : names) {
				if (NamesEqual(candidate, name)) {
					continue;
				}
				BuildProfile profile;
				if (LoadProfile(GetProfilePath(projectPath, candidate), profile) &&
					NamesEqual(profile.inherits, name)) {
					out.push_back(candidate);
				}
			}
		}

		bool DeleteProfile(const str& projectPath, const str& name, Vector<str>* outReparented) {
			Vector<str> names{GetMemoryResource(MemTag::Editor)};
			ListProfiles(projectPath, names);
			if (!Contains(names, name)) {
				SLT_LOG_WARN("Editor", "DeleteProfile: no profile named '{}'", name);
				return false;
			}

			BuildProfile doomed;
			str grandparent;
			if (LoadProfile(GetProfilePath(projectPath, name), doomed)) {
				grandparent = doomed.inherits;
			}

			Vector<str> children{GetMemoryResource(MemTag::Editor)};
			GetProfileChildren(projectPath, name, children);

			std::error_code ec;
			if (!fs::remove(fs::path{GetProfilePath(projectPath, name)}, ec) || ec) {
				SLT_LOG_WARN("Editor", "DeleteProfile: couldn't delete '{}' ({})", name,
							 ec ? ec.message() : str("the file is gone"));
				return false;
			}

			for (const str& child : children) {
				BuildProfile profile;
				str path = GetProfilePath(projectPath, child);
				if (!LoadProfile(path, profile)) {
					continue;
				}
				profile.inherits = grandparent;
				SaveProfile(path, profile);
			}
			if (outReparented) {
				*outReparented = children;
			}

			Vector<str> remaining{GetMemoryResource(MemTag::Editor)};
			for (const str& entry : names) {
				if (!NamesEqual(entry, name)) {
					remaining.push_back(entry);
				}
			}
			return WriteProfileRoster(projectPath, remaining);
		}

		bool RenameProfile(const str& projectPath, const str& oldName, const str& newName) {
			if (!IsValidProfileName(newName)) {
				SLT_LOG_WARN("Editor", "RenameProfile: '{}' isn't a usable profile name", newName);
				return false;
			}
			Vector<str> names{GetMemoryResource(MemTag::Editor)};
			ListProfiles(projectPath, names);
			if (!Contains(names, oldName)) {
				SLT_LOG_WARN("Editor", "RenameProfile: no profile named '{}'", oldName);
				return false;
			}
			if (!NamesEqual(oldName, newName) && Contains(names, newName)) {
				SLT_LOG_WARN("Editor", "RenameProfile: '{}' already exists", newName);
				return false;
			}

			std::error_code ec;
			fs::rename(fs::path{GetProfilePath(projectPath, oldName)}, fs::path{GetProfilePath(projectPath, newName)}, ec);
			if (ec) {
				SLT_LOG_WARN("Editor", "RenameProfile: couldn't rename '{}' ({})", oldName, ec.message());
				return false;
			}

			for (const str& child : names) {
				if (NamesEqual(child, oldName)) {
					continue;
				}
				BuildProfile profile;
				str path = GetProfilePath(projectPath, child);
				if (!LoadProfile(path, profile) || !NamesEqual(profile.inherits, oldName)) {
					continue;
				}
				profile.inherits = newName;
				SaveProfile(path, profile);
			}

			for (str& entry : names) {
				if (NamesEqual(entry, oldName)) {
					entry = newName;
				}
			}
			return WriteProfileRoster(projectPath, names);
		}

		bool ExportProfile(const str& projectPath, const str& name, const str& destinationPath) {
			std::error_code ec;
			fs::path destination{destinationPath};
			fs::create_directories(destination.parent_path(), ec);
			ec.clear();
			fs::copy_file(fs::path{GetProfilePath(projectPath, name)}, destination,
						  fs::copy_options::overwrite_existing, ec);
			if (ec) {
				SLT_LOG_WARN("Editor", "ExportProfile: couldn't write '{}' ({})", destinationPath, ec.message());
				return false;
			}
			return true;
		}

		bool ImportProfile(const str& projectPath, const str& sourcePath, str& outName, Vector<BuildIssue>& outIssues) {
			str name = FileStem(sourcePath);
			if (!IsValidProfileName(name)) {
				AddIssue(outIssues, BuildIssueSeverity::Error, name, {},
						 "'" + name + "' is not a usable file name for a profile");
				return false;
			}

			Vector<str> names{GetMemoryResource(MemTag::Editor)};
			ListProfiles(projectPath, names);
			if (Contains(names, name)) {
				AddIssue(outIssues, BuildIssueSeverity::Error, name, {},
						 "this project already has a profile named '" + name + "'");
				return false;
			}

			BuildProfile incoming;
			if (!LoadProfile(sourcePath, incoming)) {
				AddIssue(outIssues, BuildIssueSeverity::Error, name, {},
						 "'" + sourcePath + "' is not a build profile");
				return false;
			}

			std::error_code ec;
			str destination = GetProfilePath(projectPath, name);
			fs::create_directories(fs::path{destination}.parent_path(), ec);
			ec.clear();
			fs::copy_file(fs::path{sourcePath}, fs::path{destination}, fs::copy_options::overwrite_existing, ec);
			if (ec) {
				AddIssue(outIssues, BuildIssueSeverity::Error, name, {},
						 "could not copy the profile in (" + ec.message() + ")");
				return false;
			}

			if (!incoming.inherits.empty() && !Contains(names, incoming.inherits)) {
				AddIssue(outIssues, BuildIssueSeverity::Warning, name, {},
						 "it inherits '" + incoming.inherits +
							 "' (this project doesn't have it, resolves for the root)");
			}
			if (incoming.Has("outputPath")) {
				str outputPath = incoming.GetString("outputPath");
				if (Path(outputPath).IsAbsolute()) {
					AddIssue(outIssues, BuildIssueSeverity::Warning, name, "outputPath",
							 "'" + outputPath + "' is an absolute path (only valid on 1 machine)");
				}
			}

			names.push_back(name);
			WriteProfileRoster(projectPath, names);
			outName = std::move(name);
			return true;
		}

	} // namespace BuildProfileOps

} // namespace Shalltear
