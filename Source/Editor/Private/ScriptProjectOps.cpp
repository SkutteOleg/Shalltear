#include "Shalltear/Editor/ScriptProjectOps.h"

#include "Shalltear/Editor/CompileService.h"
#include "Shalltear/Editor/CppToolchain.h"
#include "Shalltear/Editor/CSharpTranspiler.h"

#include "GameModuleHost.h"

#include "Shalltear/Core/BuildInfo.h"
#include "Shalltear/Core/Config.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/StringUtil.h"

#include <cctype>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <format>
#include <iterator>
#include <system_error>
#include <unordered_set>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#undef CreateDirectory
#endif

namespace Shalltear::ScriptProjectOps {

	namespace {

		namespace fs = std::filesystem;

		constexpr const char* PROJECT_FILE = "Project.ini";
		constexpr const char* SCRIPT_SECTION = "Script";
		constexpr const char* SOURCE_DIRECTORY = "Source";

		constexpr const char* RETIRED_COMPILE_DIRECTORY = "Library/Build";

		constexpr const char* COMPILE_COMMAND_KEY = "compileCommand";
		constexpr const char* LEGACY_COMPILE_COMMAND_KEY = "buildCommand";

		enum class FileKind : u8 {
			Yours,
			Managed,
			MachineScoped,
		};

		struct GeneratedFile {
			const char* relativePath;
			const char* templateName;
			FileKind kind;
		};

		constexpr GeneratedFile ProjectFiles[] = {
			{"CMakeLists.txt", "CMakeLists.txt", FileKind::Yours},
			{".gitignore", "gitignore", FileKind::Yours},
			{".clang-format", "clang-format", FileKind::Yours},
			{"CMakePresets.json", "CMakePresets.json", FileKind::Managed},
			{"CMakeUserPresets.json", "CMakeUserPresets.json", FileKind::MachineScoped},
			{".clangd", "clangd", FileKind::MachineScoped},
			{".vscode/settings.json", "vscode-settings.json", FileKind::MachineScoped},
			{".vscode/launch.json", "vscode-launch.json", FileKind::MachineScoped},
			{".vscode/extensions.json", "vscode-extensions.json", FileKind::MachineScoped},
		};

		constexpr bool IsRegenerated(FileKind kind) {
			return kind != FileKind::Yours;
		}

		str Join(const str& root, std::string_view relative) {
			if (relative.empty()) {
				return root;
			}
			return root + "/" + str(relative);
		}

		bool WriteIfAbsent(const str& projectPath, std::string_view relative, const str& contents, ScaffoldResult& out) {
			str absolute = Join(projectPath, relative);
			std::error_code ec;
			if (fs::exists(fs::path(absolute), ec)) {
				out.skipped.push_back(str(relative));
				return true;
			}
			fs::create_directories(fs::path(absolute).parent_path(), ec);
			if (!FileSystem::WriteTextFile(Path(absolute), contents)) {
				SLT_LOG_WARN("Editor", "script scaffold: couldn't write '{}'", absolute);
				return false;
			}
			out.written.push_back(str(relative));
			return true;
		}

		bool MatchesOnDisk(const str& projectPath, std::string_view relative, const str& contents) {
			str absolute = Join(projectPath, relative);
			std::error_code ec;
			if (!fs::exists(fs::path(absolute), ec)) {
				return false;
			}
			return FileSystem::ReadTextFile(Path(absolute), MemTag::Editor) == contents;
		}

		bool WriteIfDifferent(const str& projectPath, std::string_view relative, const str& contents, ScaffoldResult& out) {
			if (MatchesOnDisk(projectPath, relative, contents)) {
				out.skipped.push_back(str(relative));
				return true;
			}
			str absolute = Join(projectPath, relative);
			std::error_code ec;
			fs::create_directories(fs::path(absolute).parent_path(), ec);
			if (!FileSystem::WriteTextFile(Path(absolute), contents)) {
				SLT_LOG_WARN("Editor", "script scaffold: couldn't write '{}'", absolute);
				return false;
			}
			out.written.push_back(str(relative));
			return true;
		}

		Vector<TemplateToken> BuildTokens(const str& projectPath, const str& moduleName, const ScaffoldOptions& options) {
			Vector<TemplateToken> tokens{GetMemoryResource(MemTag::Editor)};
			str upper = StringUtil::ToUpper(moduleName);
			for (char& c : upper) {
				if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
					c = '_';
				}
			}

			str editorExecutable = options.editorExecutable;
			str editorDirectory;
			if (!editorExecutable.empty()) {
				editorDirectory = fs::path(editorExecutable).parent_path().generic_string();
			}

			str engineAssetsDirectory;
			if (str schemas = FileSystem::Resolve(Path("engine://Schemas")).ToString(); schemas.find("://") == str::npos) {
				engineAssetsDirectory = fs::path(schemas).parent_path().generic_string();
			}

			Vector<str> toolchainDirectories{GetMemoryResource(MemTag::Editor)};
			auto addDirectory = [&toolchainDirectories](const str& toolPath) {
				if (toolPath.empty()) {
					return;
				}
				str directory = fs::path(toolPath).parent_path().generic_string();
				if (directory.empty()) {
					return;
				}
				for (const str& existing : toolchainDirectories) {
					if (existing == directory) {
						return;
					}
				}
				toolchainDirectories.push_back(directory);
			};

			str compilerDirectory;
			str cmakePath;
			if (!options.toolchainDirectory.empty()) {
				toolchainDirectories.push_back(options.toolchainDirectory);
				compilerDirectory = options.toolchainDirectory;
				str beside = options.toolchainDirectory + "/cmake.exe";
				std::error_code ec;
				if (fs::exists(fs::path(beside), ec)) {
					cmakePath = beside;
				}
			} else {
				CppToolchain::DiscoveryOptions discovery;
				discovery.sdkRoot = options.sdkRoot;
				discovery.probeVersions = false;
				CppToolchain::ToolchainInfo resolved = CppToolchain::Discover(discovery);
				addDirectory(resolved.compiler.path);
				addDirectory(resolved.ninja.path);
				addDirectory(resolved.cmake.path);
				cmakePath = resolved.cmake.path;
				if (!resolved.compiler.path.empty()) {
					compilerDirectory = fs::path(resolved.compiler.path).parent_path().generic_string();
				}
			}

			str clangdPath = "clangd";
			if (!compilerDirectory.empty()) {
				str beside = compilerDirectory + "/clangd.exe";
				std::error_code ec;
				if (fs::exists(fs::path(beside), ec)) {
					clangdPath = beside;
				}
			}

			str toolchainPath;
			for (const str& directory : toolchainDirectories) {
				toolchainPath += directory + ";";
			}
			toolchainPath += "$penv{PATH}";

			str packagesEntry;
			if (options.enabledPackages) {
				str packageList;
				for (const str& packageId : *options.enabledPackages) {
					if (!packageList.empty()) {
						packageList += ";";
					}
					packageList += packageId;
				}
				packagesEntry = ", \"SHALLTEAR_ENABLED_PACKAGES\": \"" + packageList + "\"";
			}

			tokens.push_back({"MODULE_NAME", moduleName});
			tokens.push_back({"MODULE_UPPER", upper});
			tokens.push_back({"ENABLED_PACKAGES_ENTRY", packagesEntry});
			tokens.push_back({"PROJECT_ROOT", projectPath});
			tokens.push_back({"CONFIG", str(GetActiveConfigName())});
			tokens.push_back({"SDK_ROOT", options.sdkRoot});
			tokens.push_back({"CLANGD_PATH", clangdPath});
			tokens.push_back({"CMAKE_PATH", cmakePath});
			tokens.push_back({"TOOLCHAIN_PATH", toolchainPath});
			tokens.push_back({"EDITOR_EXECUTABLE", editorExecutable});
			tokens.push_back({"EDITOR_DIRECTORY", editorDirectory});
			tokens.push_back({"ENGINE_ASSETS_DIRECTORY", engineAssetsDirectory});
			return tokens;
		}

		str EnvironmentValue(const char* name) {
			size_t length = 0;
			char buffer[4096];
			if (::getenv_s(&length, buffer, sizeof(buffer), name) != 0 || length == 0) {
				return {};
			}
			return str(buffer, length - 1);
		}

#if defined(_WIN32)
		constexpr const wchar_t* HOSTILE_CHILD_VARIABLES[] = {
			L"ELECTRON_RUN_AS_NODE=",
		};

		Vector<wchar_t> BuildEditorEnvironment() {
			Vector<wchar_t> block{GetMemoryResource(MemTag::Editor)};
			wchar_t* strings = ::GetEnvironmentStringsW();
			if (strings == nullptr) {
				return block;
			}
			for (const wchar_t* entry = strings; *entry != L'\0';) {
				size_t length = std::wcslen(entry);
				bool hostile = false;
				for (const wchar_t* prefix : HOSTILE_CHILD_VARIABLES) {
					if (::_wcsnicmp(entry, prefix, std::wcslen(prefix)) == 0) {
						hostile = true;
						break;
					}
				}
				if (!hostile) {
					block.insert(block.end(), entry, entry + length + 1);
				}
				entry += length + 1;
			}
			::FreeEnvironmentStringsW(strings);
			block.push_back(L'\0');
			return block;
		}
#endif

		str SanitizeModuleName(std::string_view raw) {
			str result;
			for (char c : raw) {
				if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
					result.push_back(c);
				}
			}
			if (!result.empty() && std::isdigit(static_cast<unsigned char>(result.front()))) {
				result.insert(result.begin(), '_');
			}
			return result;
		}

		struct RenderedFile {
			str relativePath;
			str contents;
		};

		bool RenderRegeneratedFiles(const str& projectPath, const ScaffoldOptions& options, Vector<RenderedFile>& out) {
			if (projectPath.empty()) {
				return false;
			}
			str moduleName = SanitizeModuleName(options.moduleName);
			if (moduleName.empty()) {
				moduleName = ReadScriptSettings(projectPath).moduleName;
			}
			Vector<TemplateToken> tokens = BuildTokens(projectPath, moduleName, options);

			for (const GeneratedFile& file : ProjectFiles) {
				if (!IsRegenerated(file.kind)) {
					continue;
				}
				str contents;
				if (!RenderTemplate(file.templateName, tokens, contents)) {
					return false;
				}
				out.push_back({str(file.relativePath), std::move(contents)});
			}
			return true;
		}

		const std::unordered_set<std::string_view>& CppKeywords() {
			static const std::unordered_set<std::string_view> keywords = {
				"abstract", "alignas", "alignof", "and", "as", "asm", "auto", "base", "bool", "break",
				"byte", "case", "catch", "char", "checked", "class", "concept", "const", "consteval",
				"constexpr", "continue", "decimal", "decltype", "default", "delegate", "delete", "do",
				"double", "else", "enum", "event", "explicit", "export", "extern", "false", "finally",
				"fixed", "float", "for", "foreach", "friend", "goto", "if", "implicit", "in", "inline",
				"int", "interface", "internal", "is", "lock", "long", "mutable", "namespace", "new",
				"noexcept", "not", "null", "nullptr", "object", "operator", "or", "out", "override",
				"params", "private", "protected", "public", "readonly", "ref", "register", "requires",
				"return", "sbyte", "sealed", "short", "signed", "sizeof", "stackalloc", "static",
				"string", "struct", "switch", "template", "this", "throw", "true", "try", "typedef",
				"typeid", "typename", "typeof", "uint", "ulong", "unchecked", "union", "unsafe",
				"unsigned", "ushort", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor"};
			return keywords;
		}

		str ReadCompileCache(const str& projectPath, std::string_view configName) {
			return FileSystem::ReadTextFile(
				Path(Join(GetCompileDirectory(projectPath, configName), "CMakeCache.txt")), MemTag::Editor);
		}

		str CacheEntry(const str& text, std::string_view key) {
			const str needle = str(key) + ":";
			size_t at = 0;
			for (;;) {
				at = text.find(needle, at);
				if (at == str::npos) {
					return {};
				}
				if (at == 0 || text[at - 1] == '\n') {
					break;
				}
				at += needle.size();
			}
			size_t equals = text.find('=', at + needle.size());
			if (equals == str::npos) {
				return {};
			}
			size_t end = text.find('\n', equals);
			str value = text.substr(equals + 1, end == str::npos ? str::npos : end - equals - 1);
			if (!value.empty() && value.back() == '\r') {
				value.pop_back();
			}
			return value;
		}

		str RecordedEngineIdentity(const str& projectPath, std::string_view configName) {
			return CacheEntry(ReadCompileCache(projectPath, configName), "SHALLTEAR_EXPECT_ENGINE_IDENTITY");
		}

		str ComparablePath(std::string_view path, bool foldCase) {
			str generic = fs::path(path).lexically_normal().generic_string();
			if (foldCase) {
				generic = StringUtil::ToLower(generic);
			}
			while (generic.size() > 1 && generic.back() == '/') {
				generic.pop_back();
			}
			return generic;
		}

	} // namespace

	std::string_view GetActiveConfigName() {
		std::string_view configured = BuildInfo::GetBuildConfig();
		if (configured == "Debug" || configured == "Release") {
			return configured;
		}
#ifdef NDEBUG
		return "Release";
#else
		return "Debug";
#endif
	}

	str GetEditorExecutablePath() {
#if defined(_WIN32)
		wchar_t buffer[MAX_PATH * 2] = {};
		DWORD length = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
		if (length == 0 || length >= std::size(buffer)) {
			return {};
		}
		return fs::path(std::wstring(buffer, length)).generic_string();
#else
		return {};
#endif
	}

	str GetSDKRoot() {
		str executable = GetEditorExecutablePath();
		if (executable.empty()) {
			return {};
		}
		std::error_code ec;
		fs::path probe = fs::path(executable).parent_path();
		for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
			if (fs::exists(probe / "ShalltearConfig.cmake", ec) ||
				fs::exists(probe / "Lib" / "cmake" / "Shalltear" / "ShalltearConfig.cmake", ec)) {
				return probe.generic_string();
			}
			fs::path parent = probe.parent_path();
			if (parent == probe) {
				break;
			}
			probe = parent;
		}
		return {};
	}

	str GetSourceDirectory(const str& projectPath) {
		return Join(projectPath, SOURCE_DIRECTORY);
	}

	str GetModuleName(const str& projectPath) {
		return ReadScriptSettings(projectPath).moduleName;
	}

	str GetCompileDirectory(const str& projectPath, std::string_view configName) {
		return Join(projectPath, "Library/Compile/") + str(configName);
	}

	str GetModuleBinaryPath(const str& projectPath, std::string_view configName) {
		return Join(projectPath, "Library/Binaries/") + str(configName) + "/" + GetModuleName(projectPath) + ".dll";
	}

	str GetModuleCompileStampPath(const str& projectPath, std::string_view configName) {
		return Join(projectPath, "Library/Binaries/") + str(configName) + "/" + GetModuleName(projectPath) + ".compiled";
	}

	bool HasScriptProject(const str& projectPath) {
		if (projectPath.empty()) {
			return false;
		}
		std::error_code ec;
		return fs::exists(fs::path(Join(projectPath, "CMakeLists.txt")), ec) &&
			   fs::is_directory(fs::path(GetSourceDirectory(projectPath)), ec);
	}

	bool IsModuleBinaryOutOfDate(const str& projectPath, std::string_view configName, str* outReason) {
		auto stale = [outReason](str reason) {
			if (outReason != nullptr) {
				*outReason = std::move(reason);
			}
			return true;
		};

		if (!HasScriptProject(projectPath)) {
			return false;
		}
		std::error_code ec;
		fs::path binary(GetModuleBinaryPath(projectPath, configName));
		fs::file_time_type builtAt = fs::last_write_time(binary, ec);
		if (ec) {
			return stale(std::format("no module binary at '{}'", binary.generic_string()));
		}

		fs::path stampPath(GetModuleCompileStampPath(projectPath, configName));
		std::error_code stampEc;
		fs::file_time_type verifiedAt = fs::last_write_time(stampPath, stampEc);
		if (stampEc) {
			verifiedAt = builtAt;
		}

		str recorded = RecordedEngineIdentity(projectPath, configName);
		if (recorded != CppToolchain::GetEngineIdentity()) {
			return stale(std::format("it was built by engine {} (this editor is {})",
									 recorded.empty() ? str("(unrecorded)") : recorded, CppToolchain::GetEngineIdentity()));
		}

		std::error_code buildFileEc;
		if (fs::last_write_time(fs::path(Join(projectPath, "CMakeLists.txt")), buildFileEc) > verifiedAt && !buildFileEc) {
			return stale("'CMakeLists.txt' changed since the last compile");
		}

		fs::path source(GetSourceDirectory(projectPath));
		for (fs::recursive_directory_iterator it(source, fs::directory_options::skip_permission_denied, ec), end;
			 it != end && !ec; it.increment(ec)) {
			if (it->is_directory(ec)) {
				continue;
			}
			std::error_code entryEc;
			if (fs::last_write_time(it->path(), entryEc) > verifiedAt && !entryEc) {
				return stale(std::format("'{}' changed since the last compile", it->path().generic_string()));
			}
		}
		fs::path assets(Join(projectPath, "Assets"));
		if (fs::is_directory(assets, ec)) {
			for (fs::recursive_directory_iterator it(assets, fs::directory_options::skip_permission_denied, ec), end;
				 it != end && !ec; it.increment(ec)) {
				if (!it->is_regular_file(ec) ||
					StringUtil::ToLower(it->path().extension().generic_string()) != ".cs") {
					continue;
				}
				std::error_code entryEc;
				if (fs::last_write_time(it->path(), entryEc) > verifiedAt && !entryEc) {
					return stale(std::format("'{}' changed since the last compile", it->path().generic_string()));
				}
			}
		}
		if (CSharpTranspiler::IsGeneratedOutputOutOfDate(projectPath, outReason)) {
			return true;
		}
		return false;
	}

	bool IsCompileCacheStale(const str& projectPath, std::string_view configName, const str& compilerPath,
							 str* outReason) {
		const str text = ReadCompileCache(projectPath, configName);
		if (text.empty()) {
			return false;
		}
		const str compileDirectory = GetCompileDirectory(projectPath, configName);
		// CMake compares compilers case sensitively, directories not
		const struct {
			std::string_view key;
			std::string_view wanted;
			bool foldCase;
		} expected[] = {
			{"CMAKE_CXX_COMPILER", compilerPath, false},
			{"CMAKE_HOME_DIRECTORY", projectPath, true},
			{"CMAKE_CACHEFILE_DIR", compileDirectory, true},
		};
		for (const auto& [key, wanted, foldCase] : expected) {
			const str recorded = CacheEntry(text, key);
			if (wanted.empty() || recorded.empty() || !fs::path(recorded).is_absolute() ||
				ComparablePath(recorded, foldCase) == ComparablePath(wanted, foldCase)) {
				continue;
			}
			if (outReason != nullptr) {
				*outReason = std::format("{} was '{}'", key, recorded);
			}
			return true;
		}
		return false;
	}

	ModuleStatus ClassifyModuleStatus(const ModuleStatusFacts& facts) {
		if (!facts.hasScriptProject) {
			return ModuleStatus::NoScriptProject;
		}
		if (facts.compiling) {
			return ModuleStatus::Compiling;
		}
		if (facts.compileFailed) {
			return ModuleStatus::CompileFailed;
		}
		if (!facts.moduleLoaded) {
			return ModuleStatus::LoadFailed;
		}
		return ModuleStatus::Loaded;
	}

	ModuleStatus GetModuleStatus(const str& projectPath) {
		const CompileService* compile = CompileService::GetActive();
		const GameModuleHost* host = GameModuleHost::GetActive();
		ModuleStatusFacts facts;
		facts.hasScriptProject = HasScriptProject(projectPath);
		facts.compiling = compile && compile->IsBusy();
		facts.compileFailed = compile && compile->GetState() == CompileState::Failed;
		facts.moduleLoaded = host && host->IsModuleLoaded();
		return ClassifyModuleStatus(facts);
	}

	ScriptSettings ReadScriptSettings(const str& projectPath) {
		ScriptSettings settings;
		Config config;
		bool loaded = config.Load(Path(Join(projectPath, PROJECT_FILE)));

		settings.moduleName = SanitizeModuleName(config.GetString(SCRIPT_SECTION, "module"));
		if (settings.moduleName.empty() && loaded) {
			settings.moduleName = SanitizeModuleName(config.GetString("Project", "name"));
		}
		if (settings.moduleName.empty()) {
			str folder = projectPath;
			size_t slash = folder.find_last_of('/');
			settings.moduleName = SanitizeModuleName(slash == str::npos ? folder : folder.substr(slash + 1));
		}
		if (settings.moduleName.empty()) {
			settings.moduleName = "Game";
		}
		settings.compileCommand = config.GetString(SCRIPT_SECTION, COMPILE_COMMAND_KEY);
		if (settings.compileCommand.empty()) {
			settings.compileCommand = config.GetString(SCRIPT_SECTION, LEGACY_COMPILE_COMMAND_KEY);
			settings.compileCommandIsLegacy = !settings.compileCommand.empty();
		}
		return settings;
	}

	bool WriteScriptSettings(const str& projectPath, const ScriptSettings& settings) {
		Path file(Join(projectPath, PROJECT_FILE));
		Config config;
		config.Load(file);
		config.SetString(SCRIPT_SECTION, "module", settings.moduleName);
		config.Remove(SCRIPT_SECTION, LEGACY_COMPILE_COMMAND_KEY);
		if (settings.compileCommand.empty()) {
			config.Remove(SCRIPT_SECTION, COMPILE_COMMAND_KEY);
		} else {
			config.SetString(SCRIPT_SECTION, COMPILE_COMMAND_KEY, settings.compileCommand);
		}
		return config.Save(file);
	}

	bool RenderTemplate(std::string_view templateName, const Vector<TemplateToken>& tokens, str& out) {
		Path virtualPath(str("engine://Templates/") + str(templateName) + ".template");
		if (!FileSystem::Exists(FileSystem::Resolve(virtualPath))) {
			SLT_LOG_WARN("Editor", "script template '{}' not found (engine:// unmounted?)", templateName);
			return false;
		}
		str text = FileSystem::ReadTextFile(FileSystem::Resolve(virtualPath), MemTag::Editor);
		if (text.empty()) {
			SLT_LOG_WARN("Editor", "script template '{}' is empty", templateName);
			return false;
		}
		for (const TemplateToken& token : tokens) {
			text = StringUtil::Replace(text, "{{" + token.name + "}}", token.value);
		}
		out = std::move(text);
		return true;
	}

	bool ScaffoldScriptProject(const str& projectPath, const ScaffoldOptions& options, ScaffoldResult& out) {
		if (projectPath.empty()) {
			return false;
		}
		str moduleName = SanitizeModuleName(options.moduleName);
		if (moduleName.empty()) {
			moduleName = ReadScriptSettings(projectPath).moduleName;
		}
		Vector<TemplateToken> tokens = BuildTokens(projectPath, moduleName, options);

		for (const GeneratedFile& file : ProjectFiles) {
			if (file.kind == FileKind::MachineScoped && !options.includeMachineScopedFiles) {
				continue;
			}
			str contents;
			if (!RenderTemplate(file.templateName, tokens, contents)) {
				return false;
			}
			if (!WriteIfAbsent(projectPath, file.relativePath, contents, out)) {
				return false;
			}
		}

		str moduleSource;
		if (!RenderTemplate("Module.cpp", tokens, moduleSource)) {
			return false;
		}
		if (!WriteIfAbsent(projectPath, str(SOURCE_DIRECTORY) + "/" + moduleName + ".cpp", moduleSource, out)) {
			return false;
		}

		if (options.includeStarterComponent) {
			ScaffoldResult componentResult;
			if (!CreateComponentSource(projectPath, "", "ExampleComponent", componentResult)) {
				SLT_LOG_INFO("Editor", "script scaffold: starter component already present, left alone");
			}
			for (const str& path : componentResult.written) {
				out.written.push_back(path);
			}
		}

		ScriptSettings settings = ReadScriptSettings(projectPath);
		settings.moduleName = moduleName;
		if (!WriteScriptSettings(projectPath, settings)) {
			return false;
		}

		SLT_LOG_INFO("Editor", "scaffolded C++ module '{}' into {} ({} file(s) written, {} left alone)",
					 moduleName, projectPath, out.written.size(), out.skipped.size());
		return true;
	}

	bool RegenerateProjectFiles(const str& projectPath, const ScaffoldOptions& options, ScaffoldResult& out) {
		Vector<RenderedFile> rendered{GetMemoryResource(MemTag::Editor)};
		if (!RenderRegeneratedFiles(projectPath, options, rendered)) {
			return false;
		}
		const size_t writtenBefore = out.written.size();
		const size_t skippedBefore = out.skipped.size();
		for (const RenderedFile& file : rendered) {
			if (!WriteIfDifferent(projectPath, file.relativePath, file.contents, out)) {
				return false;
			}
		}

		const size_t written = out.written.size() - writtenBefore;
		const size_t current = out.skipped.size() - skippedBefore;
		if (written == 0) {
			SLT_LOG_INFO("Editor", "project files are already up to date ({} checked)", current);
		} else {
			str names;
			for (size_t i = writtenBefore; i < out.written.size(); ++i) {
				names += (names.empty() ? "" : ", ") + out.written[i];
			}
			SLT_LOG_INFO("Editor", "regenerated {} of {} project file(s): {}", written, written + current, names);
		}
		if (CSharpTranspiler::HasCSharpSources(projectPath)) {
			CSharpTranspiler::EnsureCSharpProjectFiles(projectPath);
		}
		std::error_code orphanEc;
		str orphaned = Join(projectPath, RETIRED_COMPILE_DIRECTORY);
		if (fs::is_directory(fs::path(orphaned), orphanEc)) {
			SLT_LOG_INFO("Editor", "{} is unused (the compile tree is Library/Compile). Safe to delete",
						 orphaned);
		}
		return true;
	}

	bool NeedsProjectFileRegeneration(const str& projectPath, const ScaffoldOptions& options) {
		if (!HasScriptProject(projectPath)) {
			return false;
		}
		Vector<RenderedFile> rendered{GetMemoryResource(MemTag::Editor)};
		if (!RenderRegeneratedFiles(projectPath, options, rendered)) {
			return false;
		}
		for (const RenderedFile& file : rendered) {
			if (!MatchesOnDisk(projectPath, file.relativePath, file.contents)) {
				return true;
			}
		}
		return false;
	}

	ExternalEditor DetectExternalEditor() {
		ExternalEditor detected;
#if defined(_WIN32)
		std::error_code ec;
		str localAppData = EnvironmentValue("LOCALAPPDATA");
		str programFiles = EnvironmentValue("ProgramFiles");

		struct Candidate {
			const char* name;
			str path;
			const char* folderArguments;
			const char* fileArguments;
		};
		Vector<Candidate> candidates{GetMemoryResource(MemTag::Editor)};

		constexpr const char* CODE_FOLDER = "\"{folder}\"";
		constexpr const char* CODE_FILE = "\"{folder}\" --goto \"{file}:{line}:{column}\"";
		if (!localAppData.empty()) {
			candidates.push_back({"VS Code", localAppData + "/Programs/Microsoft VS Code/Code.exe", CODE_FOLDER, CODE_FILE});
		}
		if (!programFiles.empty()) {
			candidates.push_back({"VS Code", programFiles + "/Microsoft VS Code/Code.exe", CODE_FOLDER, CODE_FILE});
			candidates.push_back({"CLion", programFiles + "/JetBrains/CLion/bin/clion64.exe", "\"{folder}\"", "--line {line} \"{file}\""});
			candidates.push_back({"Rider", programFiles + "/JetBrains/Rider/bin/rider64.exe", "\"{folder}\"", "--line {line} \"{file}\""});
		}
		for (const Candidate& candidate : candidates) {
			if (fs::is_regular_file(fs::path(candidate.path), ec)) {
				str quoted = "\"" + candidate.path + "\" ";
				detected.name = candidate.name;
				detected.folderCommand = quoted + candidate.folderArguments;
				detected.fileCommand = quoted + candidate.fileArguments;
				break;
			}
		}
#endif
		return detected;
	}

	bool OpenInExternalEditor(const str& projectPath, const str& commandTemplate,
							  const str& targetPath, u32 line, u32 column) {
		bool hasFileTarget = !targetPath.empty();
		str target = hasFileTarget ? targetPath : projectPath;
		if (target.empty()) {
			return false;
		}
		str command = commandTemplate;
		if (command.empty()) {
			ExternalEditor detected = DetectExternalEditor();
			command = hasFileTarget ? detected.fileCommand : detected.folderCommand;
		}

#if defined(_WIN32)
		if (command.empty()) {
			std::wstring wide = StringUtil::UTF8ToWide(target);
			auto result = reinterpret_cast<std::uintptr_t>(
				::ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
			return result > 32;
		}

		command = StringUtil::Replace(command, "{file}", target);
		command = StringUtil::Replace(command, "{folder}", projectPath);
		command = StringUtil::Replace(command, "{line}", std::to_string(line == 0 ? 1u : line));
		command = StringUtil::Replace(command, "{column}", std::to_string(column == 0 ? 1u : column));

		std::wstring wideCommand = StringUtil::UTF8ToWide(command);
		Vector<wchar_t> environment = BuildEditorEnvironment();
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION process{};
		if (!::CreateProcessW(nullptr, wideCommand.data(), nullptr, nullptr, FALSE,
							  environment.empty() ? 0u : CREATE_UNICODE_ENVIRONMENT,
							  environment.empty() ? nullptr : environment.data(),
							  projectPath.empty() ? nullptr : StringUtil::UTF8ToWide(projectPath).c_str(),
							  &startup, &process)) {
			SLT_LOG_WARN("Editor", "couldn't launch the external editor: {}", command);
			return false;
		}
		::CloseHandle(process.hThread);
		::CloseHandle(process.hProcess);
		return true;
#else
		(void)command;
		return false;
#endif
	}

	bool IsValidClassName(std::string_view className) {
		if (className.empty() || className.size() > 128) {
			return false;
		}
		if (!(std::isalpha(static_cast<unsigned char>(className.front())) || className.front() == '_')) {
			return false;
		}
		for (char c : className) {
			if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
				return false;
			}
		}
		return !CppKeywords().contains(className);
	}

	bool CreateComponentSource(const str& projectPath, const str& folderRelativeToSource, const str& className, ScaffoldResult& out) {
		if (!IsValidClassName(className)) {
			SLT_LOG_WARN("Editor", "CreateComponentSource: '{}' isn't a valid C++ class name", className);
			return false;
		}
		if (folderRelativeToSource.find("..") != str::npos ||
			folderRelativeToSource.starts_with("/") ||
			folderRelativeToSource.find(':') != str::npos) {
			SLT_LOG_WARN("Editor", "CreateComponentSource: folder '{}' escapes Source/", folderRelativeToSource);
			return false;
		}

		str relativeDirectory = str(SOURCE_DIRECTORY);
		if (!folderRelativeToSource.empty()) {
			relativeDirectory += "/" + folderRelativeToSource;
		}
		str headerRelative = relativeDirectory + "/" + className + ".h";
		str sourceRelative = relativeDirectory + "/" + className + ".cpp";
		str scriptRelative = relativeDirectory + "/" + className + ".cs";

		std::error_code ec;
		if (fs::exists(fs::path(Join(projectPath, headerRelative)), ec) ||
			fs::exists(fs::path(Join(projectPath, sourceRelative)), ec) ||
			fs::exists(fs::path(Join(projectPath, scriptRelative)), ec)) {
			SLT_LOG_WARN("Editor", "CreateComponentSource: '{}' already exists in {}", className, relativeDirectory);
			return false;
		}

		ScaffoldOptions options;
		Vector<TemplateToken> tokens = BuildTokens(projectPath, ReadScriptSettings(projectPath).moduleName, options);
		tokens.push_back({"CLASS_NAME", className});

		str header;
		str source;
		if (!RenderTemplate("Component.h", tokens, header) || !RenderTemplate("Component.cpp", tokens, source)) {
			return false;
		}
		return WriteIfAbsent(projectPath, headerRelative, header, out) &&
			   WriteIfAbsent(projectPath, sourceRelative, source, out);
	}

	bool CreateCSharpComponentSource(const str& projectPath,
									 const str& folderRelativeToSource,
									 const str& className,
									 ScaffoldResult& out) {
		if (!IsValidClassName(className)) {
			SLT_LOG_WARN("Editor", "CreateCSharpComponentSource: '{}' isn't a valid C# class name", className);
			return false;
		}
		if (folderRelativeToSource.find("..") != str::npos ||
			folderRelativeToSource.starts_with("/") ||
			folderRelativeToSource.find(':') != str::npos) {
			SLT_LOG_WARN("Editor", "CreateCSharpComponentSource: folder '{}' escapes Source/", folderRelativeToSource);
			return false;
		}

		str relativeDirectory = str(SOURCE_DIRECTORY);
		if (!folderRelativeToSource.empty()) {
			relativeDirectory += "/" + folderRelativeToSource;
		}
		str headerRelative = relativeDirectory + "/" + className + ".h";
		str sourceRelative = relativeDirectory + "/" + className + ".cpp";
		str scriptRelative = relativeDirectory + "/" + className + ".cs";

		std::error_code ec;
		if (fs::exists(fs::path(Join(projectPath, headerRelative)), ec) ||
			fs::exists(fs::path(Join(projectPath, sourceRelative)), ec) ||
			fs::exists(fs::path(Join(projectPath, scriptRelative)), ec)) {
			SLT_LOG_WARN("Editor", "CreateCSharpComponentSource: '{}' already exists in {}", className, relativeDirectory);
			return false;
		}

		ScaffoldOptions options;
		Vector<TemplateToken> tokens = BuildTokens(projectPath, ReadScriptSettings(projectPath).moduleName, options);
		tokens.push_back({"CLASS_NAME", className});

		str script;
		if (!RenderTemplate("Component.cs", tokens, script)) {
			return false;
		}
		if (!WriteIfAbsent(projectPath, scriptRelative, script, out)) {
			return false;
		}
		CSharpTranspiler::EnsureCSharpProjectFiles(projectPath);
		return true;
	}

} // namespace Shalltear::ScriptProjectOps
