#include "CSharpProjectFiles.h"

#include "CSharpSettings.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/StringUtil.h"

#include "Shalltear/Editor/ProjectOps.h"
#include "Shalltear/Editor/ScriptProjectOps.h"

#include <cctype>
#include <filesystem>
#include <sstream>

namespace Shalltear::CSharpEditor::CSharpProjectFiles {

	namespace {

		namespace fs = std::filesystem;

		constexpr std::string_view IgnoredLines[] = {"/Library/", "/*.Scripts.csproj", "/*.slnx", "/global.json", "/.vscode/launch.json"};

		str SafeName(std::string_view raw) {
			str result;
			for (const char c : raw) {
				result.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_');
			}
			if (result.empty() || std::isdigit(static_cast<unsigned char>(result.front()))) {
				result.insert(result.begin(), '_');
			}
			return result;
		}

		str ReadIfPresent(const fs::path& file) {
			std::error_code ec;
			return fs::is_regular_file(file, ec) ? FileSystem::ReadTextFile(Path(file.generic_string()), MemTag::Editor) : str();
		}

		bool WriteIfDifferent(const Options& options, const str& relative, const str& contents, Result& out) {
			const fs::path file = fs::path(options.projectPath) / relative;
			std::error_code ec;
			if (fs::is_regular_file(file, ec) && ReadIfPresent(file) == contents) {
				return true;
			}
			fs::create_directories(file.parent_path(), ec);
			if (!FileSystem::WriteTextFile(Path(file.generic_string()), contents)) {
				SLT_LOG_WARN("C#", "couldn't write '{}'", file.generic_string());
				return false;
			}
			out.written.push_back(relative);
			return true;
		}

		str EscapeXML(const str& text) {
			str escaped;
			for (const char c : text) {
				if (c == '&') {
					escaped += "&amp;";
				} else if (c == '<') {
					escaped += "&lt;";
				} else if (c == '>') {
					escaped += "&gt;";
				} else {
					escaped.push_back(c);
				}
			}
			return escaped;
		}

		str EscapeJSON(const str& text) {
			str escaped;
			for (const char c : text) {
				if (c == '"' || c == '\\') {
					escaped.push_back('\\');
				}
				escaped.push_back(c);
			}
			return escaped;
		}

		bool Render(const char* name, const Options& options, bool xml, str& out) {
			const auto path = [xml](const str& value) { return xml ? EscapeXML(value) : EscapeJSON(value); };
			Vector<ScriptProjectOps::TemplateToken> tokens{GetMemoryResource(MemTag::Editor)};
			tokens.push_back({"PROJECT_NAME", SafeName(options.projectName)});
			tokens.push_back({"ENGINE_ROOT", path(options.engineRoot)});
			tokens.push_back({"MANAGED_DIRECTORY", path(options.managedDirectory)});
			tokens.push_back({"DOTNET_ROOT", path(options.dotnetRoot)});
			tokens.push_back({"PACKAGES_ROOT", path(options.packagesRoot)});
			tokens.push_back({"DOTNET_SDK_VERSION", options.pin.sdkVersion});
			tokens.push_back({"DOTNET_RUNTIME_VERSION", options.pin.runtimeVersion});
			tokens.push_back({"TARGET_FRAMEWORK", options.pin.targetFramework});
			tokens.push_back({"ALLOW_UNSAFE", options.allowUnsafeCode ? "true" : "false"});
			tokens.push_back({"INVARIANT_GLOBALIZATION", options.invariantGlobalization ? "true" : "false"});
			tokens.push_back({"GENERATED_MARK", str(GeneratedMark)});
			return ScriptProjectOps::RenderTemplate(name, tokens, out);
		}

		str RenderLaunchFile(const Options& options) {
			return str(GeneratedMark) + "\n{\n\t\"version\": \"0.2.0\",\n\t\"configurations\": [\n\t\t" +
				   StringUtil::Replace(GetAttachConfiguration(options.editorExecutable), "\n", "\n\t\t") + "\n\t]\n}\n";
		}

		bool AddIgnoredLines(const Options& options, Result& out) {
			const fs::path file = fs::path(options.projectPath) / ".gitignore";
			str text = ReadIfPresent(file);
			const char* ending = text.find("\r\n") != str::npos ? "\r\n" : "\n";
			str added;
			for (const std::string_view wanted : IgnoredLines) {
				bool present = false;
				std::istringstream lines(text);
				for (str line; std::getline(lines, line);) {
					if (!line.empty() && line.back() == '\r') {
						line.pop_back();
					}
					present = present || line == wanted;
				}
				if (!present) {
					added += wanted;
					added += ending;
				}
			}
			if (added.empty()) {
				return true;
			}
			if (!text.empty() && text.back() != '\n') {
				text += ending;
			}
			return WriteIfDifferent(options, ".gitignore", text + added, out);
		}

	} // anonymous namespace

	namespace {

		str ReadProjectName(const str& projectPath) {
			str name;
			u64 created = 0;
			if (!ProjectOps::ReadProjectFile(projectPath, name, created) || name.empty()) {
				name = fs::path(projectPath).filename().generic_string();
			}
			return name;
		}

	} // anonymous namespace

	str GetRootNamespace(const str& projectPath) {
		return SafeName(ReadProjectName(projectPath));
	}

	Options GetDefaultOptions(const str& projectPath, const str& dotnetRoot) {
		Options options;
		options.projectPath = projectPath;
		options.projectName = ReadProjectName(projectPath);
		options.dotnetRoot = dotnetRoot;
		const str props = FileSystem::Resolve(Path("engine://CSharp/Shalltear.CSharp.props")).ToString();
		if (props.find("://") == str::npos) {
			options.engineRoot = fs::path(props).parent_path().parent_path().parent_path().generic_string();
		}
		options.managedDirectory = fs::path(DotNetToolchain::GetRuntimeAssemblyPath()).parent_path().generic_string();
		std::error_code ec;
		const fs::path folder = fs::path(dotnetRoot).parent_path().parent_path() / "nuget";
		options.packagesRoot = fs::is_directory(folder, ec) ? folder.generic_string()
															: DotNetToolchain::GetPackagesRoot(DotNetToolchain::GetCacheRoot(), DotNetToolchain::GetPin());
		options.editorExecutable = ScriptProjectOps::GetEditorExecutablePath();
		options.pin = DotNetToolchain::GetPin();
		options.allowUnsafeCode = CSharpSettings::GetProject().allowUnsafeCode;
		options.invariantGlobalization = CSharpSettings::GetProject().invariantGlobalization;
		return options;
	}

	str GetProjectFile(const Options& options) {
		return SafeName(options.projectName) + ".Scripts.csproj";
	}

	str GetSolutionFile(const Options& options) {
		return SafeName(options.projectName) + ".slnx";
	}

	str GetAttachConfiguration(const str& editorExecutable) {
		return "{\n\t\"name\": \"Attach C# (editor)\",\n\t\"type\": \"coreclr\",\n\t\"request\": \"attach\",\n\t\"processName\": \"" +
			   EscapeJSON(fs::path(editorExecutable).filename().generic_string()) + "\"\n}";
	}

	void RegisterAttachConfiguration(const void* owner) {
		ScriptProjectOps::RegisterDebugConfiguration(GetAttachConfiguration(ScriptProjectOps::GetEditorExecutablePath()), owner);
	}

	bool Write(const Options& options, Result& out) {
		if (options.projectPath.empty() || options.engineRoot.empty() || options.dotnetRoot.empty() || options.managedDirectory.empty()) {
			return false;
		}
		const fs::path root(options.projectPath);
		std::error_code ec;
		str project;
		str solution;
		str paths;
		str global;
		str user;
		if (!Render("CSharpProject.csproj", options, true, project) || !Render("CSharpSolution.slnx", options, true, solution) ||
			!Render("CSharpPaths.props", options, true, paths) || !Render("CSharpGlobal.json", options, false, global) ||
			!Render("CSharpUser.props", options, true, user)) {
			return false;
		}

		bool written = WriteIfDifferent(options, "Library/CSharp/Generated/Shalltear.Paths.props", paths, out);
		written = WriteIfDifferent(options, GetProjectFile(options), project, out) && written;
		written = WriteIfDifferent(options, GetSolutionFile(options), solution, out) && written;

		const str existingGlobal = ReadIfPresent(root / "global.json");
		if (existingGlobal.empty() || existingGlobal.starts_with(GeneratedMark)) {
			written = WriteIfDifferent(options, "global.json", global, out) && written;
		}
		if (!options.editorExecutable.empty() && !ScriptProjectOps::HasScriptProject(options.projectPath)) {
			const str existingLaunch = ReadIfPresent(root / ".vscode" / "launch.json");
			if (existingLaunch.empty() || existingLaunch.starts_with(GeneratedMark)) {
				written = WriteIfDifferent(options, ".vscode/launch.json", RenderLaunchFile(options), out) && written;
			}
		}
		if (!fs::exists(root / "CSharp.props", ec)) {
			written = WriteIfDifferent(options, "CSharp.props", user, out) && written;
			written = AddIgnoredLines(options, out) && written;
		}
		return written;
	}

} // namespace Shalltear::CSharpEditor::CSharpProjectFiles
