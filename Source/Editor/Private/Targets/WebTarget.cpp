#include "Targets/WebTarget.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Graphics/RHITypes.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/StringUtil.h"

#include "Shalltear/Editor/ChildProcess.h"
#include "Shalltear/Editor/ProjectOps.h"
#include "Shalltear/Editor/ScriptProjectOps.h"
#include "Shalltear/Editor/WebPreviewServer.h"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <system_error>

namespace Shalltear {

	namespace {

		namespace fs = std::filesystem;

		constexpr std::string_view HTMLTemplateKey = "htmlTemplate";

		struct ResolvedHTMLTemplate {
			bool configured = false;
			bool valid = false;
			bool isAbsolute = false;
			bool isDirectory = false;
			str templateFile;
			str companionDir;
			str error;
		};

		void AddIssue(Vector<BuildIssue>& out, BuildIssueSeverity severity, const str& profile,
					  std::string_view key, str message) {
			out.push_back(BuildIssue{severity, profile, str(key), std::move(message)});
		}

		fs::path UTF8Path(std::string_view text) {
			return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
		}

		str UTF8String(const fs::path& path) {
			const std::u8string utf8 = path.generic_u8string();
			return str(reinterpret_cast<const char*>(utf8.data()), utf8.size());
		}

		bool IsSameOrDescendant(const fs::path& ancestor, const fs::path& candidate) {
			std::error_code ec;
			fs::path cur = candidate;
			while (!cur.empty()) {
				if (fs::equivalent(cur, ancestor, ec) && !ec) {
					return true;
				}
				ec.clear();
				const fs::path parent = cur.parent_path();
				if (parent == cur) {
					break;
				}
				cur = parent;
			}
			return false;
		}

		ResolvedHTMLTemplate ResolveHTMLTemplate(const str& projectPath, const str& value) {
			ResolvedHTMLTemplate result;
			if (value.empty()) {
				result.valid = true;
				return result;
			}
			result.configured = true;
			const str normalizedValue = Path(value).ToString();
			result.isAbsolute = Path(normalizedValue).IsAbsolute();
			if (!result.isAbsolute && projectPath.empty()) {
				result.error = "cannot resolve relative htmlTemplate '" + value + "' without a project directory";
				return result;
			}

			const fs::path candidate = result.isAbsolute
										   ? UTF8Path(normalizedValue)
										   : UTF8Path(Path(projectPath).ToString()) / UTF8Path(normalizedValue);
			std::error_code ec;
			if (fs::is_regular_file(candidate, ec) && !ec) {
				const auto size = fs::file_size(candidate, ec);
				if (ec || size == 0) {
					result.error = "'" + value + "' is empty";
					return result;
				}
				result.templateFile = UTF8String(candidate);
				result.valid = true;
				return result;
			}
			ec.clear();
			if (fs::is_directory(candidate, ec) && !ec) {
				result.isDirectory = true;
				const fs::path indexFile = candidate / "index.html";
				if (!fs::is_regular_file(indexFile, ec) || ec) {
					result.error = "'" + value + "' does not contain an index.html file";
					return result;
				}
				const auto size = fs::file_size(indexFile, ec);
				if (ec || size == 0) {
					result.error = "index.html in '" + value + "' is empty";
					return result;
				}
				result.templateFile = UTF8String(indexFile);
				result.companionDir = UTF8String(candidate);
				result.valid = true;
				return result;
			}

			result.error = "'" + value + "' is not a file or folder in this project";
			return result;
		}

		bool IsProtectedWebOutput(const str& relativeGeneric, const str& jsFile, const str& wasmFile) {
			const str lower = StringUtil::ToLower(relativeGeneric);
			return lower == "index.html" || lower == StringUtil::ToLower(jsFile) ||
				   lower == StringUtil::ToLower(wasmFile) || lower == "data/content.slpak";
		}

		bool CopyTemplateCompanions(BuildContext& context, const str& companionDir, const str& jsFile,
									const str& wasmFile) {
			std::error_code ec;
			const fs::path sourceRoot = fs::weakly_canonical(UTF8Path(companionDir), ec);
			if (ec) {
				context.AddIssue(BuildIssueSeverity::Error, HTMLTemplateKey,
								 "couldn't access HTML template folder '" + companionDir + "'");
				return false;
			}
			const fs::path stagingRoot = fs::weakly_canonical(UTF8Path(context.stagingDir), ec);
			if (ec) {
				context.AddIssue(BuildIssueSeverity::Error, HTMLTemplateKey,
								 "couldn't access Web staging folder '" + context.stagingDir + "'");
				return false;
			}
			if (IsSameOrDescendant(sourceRoot, stagingRoot)) {
				context.AddIssue(BuildIssueSeverity::Error, HTMLTemplateKey,
								 "HTML template folder cannot contain the build output directory");
				return false;
			}

			u32 copiedCount = 0;
			fs::recursive_directory_iterator it(sourceRoot, fs::directory_options::skip_permission_denied, ec);
			for (fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
				std::error_code entryEc;
				if (!it->is_regular_file(entryEc) || entryEc) {
					continue;
				}
				const fs::path relative = fs::relative(it->path(), sourceRoot, entryEc);
				if (entryEc || relative.empty()) {
					continue;
				}
				const str relStr = UTF8String(relative);
				const str lowerRel = StringUtil::ToLower(relStr);
				if (lowerRel == "index.html" || lowerRel.ends_with(".meta")) {
					continue;
				}
				if (IsProtectedWebOutput(relStr, jsFile, wasmFile)) {
					context.AddIssue(BuildIssueSeverity::Warning, HTMLTemplateKey,
									 "skipping template file '" + relStr + "' because it conflicts with build output");
					continue;
				}
				const fs::path dest = stagingRoot / relative;
				fs::create_directories(dest.parent_path(), entryEc);
				if (entryEc) {
					context.AddIssue(BuildIssueSeverity::Error, HTMLTemplateKey,
									 "couldn't create folder for template asset '" + relStr + "'");
					return false;
				}
				fs::copy_file(it->path(), dest, fs::copy_options::overwrite_existing, entryEc);
				if (entryEc) {
					context.AddIssue(BuildIssueSeverity::Error, HTMLTemplateKey,
									 "couldn't copy template asset '" + relStr + "'");
					return false;
				}
				++copiedCount;
			}
			if (ec) {
				context.AddIssue(BuildIssueSeverity::Error, HTMLTemplateKey,
								 "error reading HTML template folder '" + companionDir + "'");
				return false;
			}
			if (copiedCount > 0) {
				context.Log(std::format("copied {} HTML template companion file(s) from '{}'", copiedCount,
										companionDir));
			}
			return true;
		}

		bool ResolvedBool(const ResolvedProfile* profile, std::string_view key, bool fallback = false) {
			const ResolvedSetting* setting = profile ? profile->Find(key) : nullptr;
			if (!setting || setting->value.empty()) {
				return fallback;
			}
			const str lower = StringUtil::ToLower(setting->value);
			return lower == "true" || lower == "1" || lower == "yes" || lower == "on";
		}

		i32 ResolvedInt(const ResolvedProfile* profile, std::string_view key, i32 fallback) {
			const ResolvedSetting* setting = profile ? profile->Find(key) : nullptr;
			if (!setting || setting->value.empty()) {
				return fallback;
			}
			char* end = nullptr;
			const long parsed = std::strtol(setting->value.c_str(), &end, 10);
			return (end && *end == '\0' && parsed > 0) ? static_cast<i32>(parsed) : fallback;
		}

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
				const size_t end = text.find('"', at);
				return end == str::npos ? str{} : text.substr(at, end - at);
			}
			return {};
		}

		bool IsMatchingWebStaticSDK(const str& candidate, bool enableThreads) {
			if (candidate.empty()) {
				return false;
			}
			if (ReadPackageVariable(candidate, "SHALLTEAR_LINK_MODE") != "STATIC") {
				return false;
			}
			if (ReadPackageVariable(candidate, "SHALLTEAR_COMPILER_TARGET") != WebTarget::Triple) {
				return false;
			}
			const str threadsVar = StringUtil::ToUpper(ReadPackageVariable(candidate, "SHALLTEAR_WEB_THREADS"));
			const bool sdkThreaded = threadsVar == "ON" || threadsVar == "TRUE" || threadsVar == "1";
			return sdkThreaded == enableThreads;
		}

		str EnvValue(const char* name) {
			const char* value = std::getenv(name);
			return value ? str(value) : str{};
		}

		bool FindWebStaticSDKRoot(bool enableThreads, str& outRoot, str& outSource) {
			outRoot.clear();
			outSource.clear();

			const char* envName = enableThreads ? "SHALLTEAR_SDK_WEB_THREADS" : "SHALLTEAR_SDK_WEB";
			if (str fromEnv = EnvValue(envName); !fromEnv.empty()) {
				fromEnv = Path(fromEnv).ToString();
				if (IsMatchingWebStaticSDK(fromEnv, enableThreads)) {
					outRoot = std::move(fromEnv);
					outSource = envName;
					return true;
				}
			}

			const str sdkRoot = ScriptProjectOps::GetSDKRoot();
			if (!sdkRoot.empty()) {
				const str preferred = sdkRoot + (enableThreads ? "/Static/wasm32-threads" : "/Static/wasm32");
				if (IsMatchingWebStaticSDK(preferred, enableThreads)) {
					outRoot = preferred;
					outSource = "the SDK's Web static flavor";
					return true;
				}
				std::error_code ec;
				const fs::path staticDir(sdkRoot + "/Static");
				if (fs::is_directory(staticDir, ec)) {
					for (const fs::directory_entry& entry : fs::directory_iterator(staticDir, ec)) {
						if (!entry.is_directory(ec)) {
							continue;
						}
						str nested = entry.path().generic_string();
						if (IsMatchingWebStaticSDK(nested, enableThreads)) {
							outRoot = std::move(nested);
							outSource = "the SDK's Web static flavor";
							return true;
						}
					}
				}
			}

			const str executable = ScriptProjectOps::GetEditorExecutablePath();
			if (!executable.empty()) {
				std::error_code ec;
				const fs::path siblings = fs::path{executable}.parent_path().parent_path().parent_path();
				if (!siblings.empty() && fs::is_directory(siblings, ec)) {
					for (const fs::directory_entry& entry : fs::directory_iterator(siblings, ec)) {
						std::error_code typeEc;
						if (!entry.is_directory(typeEc) || typeEc) {
							continue;
						}
						str candidate = entry.path().generic_string();
						if (IsMatchingWebStaticSDK(candidate, enableThreads)) {
							outRoot = std::move(candidate);
							outSource = "a sibling Web static build tree";
							return true;
						}
					}
				}
			}

			if (str fallbackEnv = EnvValue("SHALLTEAR_SDK_STATIC"); !fallbackEnv.empty()) {
				fallbackEnv = Path(fallbackEnv).ToString();
				if (IsMatchingWebStaticSDK(fallbackEnv, enableThreads)) {
					outRoot = std::move(fallbackEnv);
					outSource = "SHALLTEAR_SDK_STATIC";
					return true;
				}
			}

			return false;
		}

		bool HasEmscriptenRoot(const fs::path& dir) {
			std::error_code ec;
			return fs::is_regular_file(dir / "install" / "emscripten" / "cmake" / "Modules" / "Platform" / "Emscripten.cmake", ec) ||
				   fs::is_regular_file(dir / "upstream" / "emscripten" / "cmake" / "Modules" / "Platform" / "Emscripten.cmake", ec);
		}

		str FindEmscriptenRoot(const str& sdkRoot) {
			for (const char* env : {"SHALLTEAR_EMSDK", "EMSDK"}) {
				if (str v = EnvValue(env); !v.empty() && HasEmscriptenRoot(fs::path{v})) {
					return Path(v).ToString();
				}
			}
			Vector<fs::path> probes{GetMemoryResource(MemTag::Editor)};
			if (!sdkRoot.empty()) {
				fs::path cur(sdkRoot);
				for (int depth = 0; depth < 4 && !cur.empty(); ++depth) {
					probes.push_back(cur / "Toolchain" / "emsdk");
					probes.push_back(cur / "External" / "Prebuilt" / "emsdk");
					if (cur == cur.parent_path()) {
						break;
					}
					cur = cur.parent_path();
				}
			}
			const str editorExe = ScriptProjectOps::GetEditorExecutablePath();
			if (!editorExe.empty()) {
				fs::path cur = fs::path{editorExe}.parent_path();
				for (int depth = 0; depth < 4 && !cur.empty(); ++depth) {
					probes.push_back(cur / "Toolchain" / "emsdk");
					probes.push_back(cur / "External" / "Prebuilt" / "emsdk");
					if (cur == cur.parent_path()) {
						break;
					}
					cur = cur.parent_path();
				}
			}
			for (const fs::path& candidate : probes) {
				if (HasEmscriptenRoot(candidate)) {
					return candidate.generic_string();
				}
			}
			return {};
		}

		str FindEmdawnwebgpuPort(const str& sdkRoot) {
			std::error_code ec;
			if (str v = EnvValue("SHALLTEAR_EMDAWNWEBGPU_PORT"); !v.empty() && fs::is_regular_file(fs::path{v}, ec)) {
				return Path(v).ToString();
			}
			Vector<fs::path> probes{GetMemoryResource(MemTag::Editor)};
			if (!sdkRoot.empty()) {
				fs::path cur(sdkRoot);
				for (int depth = 0; depth < 4 && !cur.empty(); ++depth) {
					probes.push_back(cur / "Toolchain" / "dawn" / "emdawnwebgpu_pkg" / "emdawnwebgpu.port.py");
					probes.push_back(cur / "External" / "Prebuilt" / "dawn" / "emdawnwebgpu_pkg" / "emdawnwebgpu.port.py");
					if (cur == cur.parent_path()) {
						break;
					}
					cur = cur.parent_path();
				}
			}
			const str editorExe = ScriptProjectOps::GetEditorExecutablePath();
			if (!editorExe.empty()) {
				fs::path cur = fs::path{editorExe}.parent_path();
				for (int depth = 0; depth < 4 && !cur.empty(); ++depth) {
					probes.push_back(cur / "Toolchain" / "dawn" / "emdawnwebgpu_pkg" / "emdawnwebgpu.port.py");
					probes.push_back(cur / "External" / "Prebuilt" / "dawn" / "emdawnwebgpu_pkg" / "emdawnwebgpu.port.py");
					if (cur == cur.parent_path()) {
						break;
					}
					cur = cur.parent_path();
				}
			}
			for (const fs::path& candidate : probes) {
				if (fs::is_regular_file(candidate, ec)) {
					return candidate.generic_string();
				}
			}
			return {};
		}

		str EscapeHTML(std::string_view text) {
			str out;
			out.reserve(text.size());
			for (char c : text) {
				switch (c) {
				case '&':
					out += "&amp;";
					break;
				case '<':
					out += "&lt;";
					break;
				case '>':
					out += "&gt;";
					break;
				case '"':
					out += "&quot;";
					break;
				default:
					out.push_back(c);
					break;
				}
			}
			return out;
		}

	} // anonymous namespace

	bool WebTarget::IsAvailable(str& outReason) const {
		outReason.clear();
		str root;
		str source;
		if (!FindWebStaticSDKRoot(false, root, source) && !FindWebStaticSDKRoot(true, root, source)) {
			outReason = "no Web (wasm32) STATIC SDK was found. Run `python Scripts/fetch_prebuilt.py --web` -> "
						"`cmake --preset web-static-release` -> `cmake --build --preset web-static-release`.";
			return false;
		}
		if (FindEmscriptenRoot(root).empty()) {
			outReason = "Emscripten SDK was not found. Run `python Scripts/fetch_prebuilt.py --web` or set EMSDK.";
			return false;
		}
		if (FindEmdawnwebgpuPort(root).empty()) {
			outReason = "emdawnwebgpu port was not found. Run `python Scripts/fetch_prebuilt.py --web`.";
			return false;
		}
		return true;
	}

	void WebTarget::DescribeSettings(Vector<BuildSettingDesc>& out) const {
		BuildSettingDesc threads;
		threads.key = "enableThreads";
		threads.type = BuildSettingType::Bool;
		threads.merge = BuildMergePolicy::Replace;
		threads.label = "Enable WebAssembly Threads";
		threads.help = "compile with pthreads and SharedArrayBuffer (requires COOP/COEP HTTP headers when hosted)";
		threads.defaultValue = "false";
		out.push_back(std::move(threads));

		BuildSettingDesc memory;
		memory.key = "initialMemoryMB";
		memory.type = BuildSettingType::Int;
		memory.merge = BuildMergePolicy::Replace;
		memory.label = "Initial Memory (MB)";
		memory.help = "initial WebAssembly linear memory heap size in megabytes (minimum 32, default 128)";
		memory.defaultValue = "128";
		out.push_back(std::move(memory));

		BuildSettingDesc growth;
		growth.key = "allowMemoryGrowth";
		growth.type = BuildSettingType::Bool;
		growth.merge = BuildMergePolicy::Replace;
		growth.label = "Allow Memory Growth";
		growth.help = "allow WebAssembly linear memory to grow dynamically above the initial heap";
		growth.defaultValue = "true";
		out.push_back(std::move(growth));

		BuildSettingDesc validation;
		validation.key = "webgpuValidation";
		validation.type = BuildSettingType::Bool;
		validation.merge = BuildMergePolicy::Replace;
		validation.label = "WebGPU Validation";
		validation.help = "enable extra WebGPU error diagnostics in browser console";
		validation.defaultValue = "false";
		out.push_back(std::move(validation));

		BuildSettingDesc aotCompile;
		aotCompile.key = "aotCompile";
		aotCompile.type = BuildSettingType::Bool;
		aotCompile.merge = BuildMergePolicy::Replace;
		aotCompile.label = "AOT Compile C# (Release)";
		aotCompile.help = "compile C# assemblies ahead of time to WebAssembly in Release builds for near-native script performance";
		aotCompile.defaultValue = "true";
		out.push_back(std::move(aotCompile));

		BuildSettingDesc htmlTemplate;
		htmlTemplate.key = str(HTMLTemplateKey);
		htmlTemplate.type = BuildSettingType::Path;
		htmlTemplate.merge = BuildMergePolicy::Replace;
		htmlTemplate.label = "HTML Template";
		htmlTemplate.help = "HTML file or template folder with index.html in this project "
							"(tokens: {{TITLE}}, {{JS_FILE}}, {{WASM_FILE}}, {{THREADS_ENABLED}}, {{HAS_MANAGED}}), "
							"empty = built-in template";
		out.push_back(std::move(htmlTemplate));
	}

	void WebTarget::Validate(const str& projectPath, const ResolvedProfile& profile, Vector<BuildIssue>& out) const {
		const bool enableThreads = ResolvedBool(&profile, "enableThreads", false);
		str root;
		str source;
		if (!FindWebStaticSDKRoot(enableThreads, root, source)) {
			AddIssue(out, BuildIssueSeverity::Error, profile.name, "enableThreads",
					 enableThreads
						 ? str("no threaded Web (wasm32-threads) STATIC SDK was found. Build preset "
							   "`web-threads-static-release` or package with `--bundle-web`.")
						 : str("no single-threaded Web (wasm32) STATIC SDK was found. Build preset "
							   "`web-static-release` or package with `--bundle-web`."));
		}
		if (FindEmscriptenRoot(root).empty()) {
			AddIssue(out, BuildIssueSeverity::Error, profile.name, "",
					 "Emscripten SDK was not found. Run `python Scripts/fetch_prebuilt.py --web` or set EMSDK.");
		}
		if (FindEmdawnwebgpuPort(root).empty()) {
			AddIssue(out, BuildIssueSeverity::Error, profile.name, "",
					 "emdawnwebgpu port was not found. Run `python Scripts/fetch_prebuilt.py --web`.");
		}
		const i32 memMB = ResolvedInt(&profile, "initialMemoryMB", 128);
		if (memMB < 32 || memMB > 2048) {
			AddIssue(out, BuildIssueSeverity::Error, profile.name, "initialMemoryMB",
					 "initialMemoryMB must be between 32 and 2048");
		}
		if (const ResolvedSetting* setting = profile.Find(HTMLTemplateKey); setting && !setting->value.empty()) {
			const ResolvedHTMLTemplate resolved = ResolveHTMLTemplate(projectPath, setting->value);
			if (resolved.isAbsolute) {
				AddIssue(out, BuildIssueSeverity::Warning, profile.name, HTMLTemplateKey,
						 "'" + setting->value + "' is an absolute path (only valid on 1 machine)");
			}
			if (!resolved.valid) {
				AddIssue(out, BuildIssueSeverity::Error, profile.name, HTMLTemplateKey, resolved.error);
			}
		}
	}

	void WebTarget::ConfigurePipeline(BuildContext& context) const {
		const bool enableThreads = ResolvedBool(context.profile, "enableThreads", false);
		const i32 initialMemMB = std::clamp(ResolvedInt(context.profile, "initialMemoryMB", 128), 32, 2048);
		const bool allowGrowth = ResolvedBool(context.profile, "allowMemoryGrowth", true);
		const bool validation = ResolvedBool(context.profile, "webgpuValidation", false);

		str root;
		str source;
		if (FindWebStaticSDKRoot(enableThreads, root, source)) {
			context.sdkRoot = root;
			context.Log("static Web SDK: " + root + " (" + source + ")");
		}

		context.entrySource = "PlayerMain.cpp";
		context.windowedSubsystem = true;
		context.executableSuffix = ".js";
		context.shaderFormat = RHI::ShaderBytecodeFormat::WGSL;

		context.defines.push_back("SLT_PLAYER");
		if (validation) {
			context.defines.push_back("SLT_WEBGPU_VALIDATION=1");
		}

		context.extraConfigureArgs.push_back(enableThreads ? "-DSHALLTEAR_WEB_THREADS=ON" : "-DSHALLTEAR_WEB_THREADS=OFF");
		const u64 memBytes = static_cast<u64>(initialMemMB) * 1024ull * 1024ull;
		context.extraConfigureArgs.push_back(std::format("-DSHALLTEAR_WEB_INITIAL_MEMORY_BYTES={}", memBytes));
		context.extraConfigureArgs.push_back(allowGrowth ? "-DSHALLTEAR_WEB_ALLOW_MEMORY_GROWTH=ON"
														 : "-DSHALLTEAR_WEB_ALLOW_MEMORY_GROWTH=OFF");

		if (const str emsdk = FindEmscriptenRoot(context.sdkRoot); !emsdk.empty()) {
			context.extraConfigureArgs.push_back("-DSHALLTEAR_EMSDK=" + QuoteCommandArgument(emsdk));
		}
		if (const str port = FindEmdawnwebgpuPort(context.sdkRoot); !port.empty()) {
			context.extraConfigureArgs.push_back("-DSHALLTEAR_EMDAWNWEBGPU_PORT=" + QuoteCommandArgument(port));
		}
	}

	str WebTarget::GetLaunchCommand(const BuildContext& context) const {
		if (context.stagingDir.empty()) {
			return {};
		}
		const bool enableThreads = ResolvedBool(context.profile, "enableThreads", false);
		return WebPreviewServer::Serve(context.stagingDir, enableThreads);
	}

	bool WebTarget::RenderIndexHTML(const str& title, const str& jsFile, const str& wasmFile,
									bool enableThreads, str& outHTML, bool hasManaged,
									const str& customTemplatePath) {
		Vector<ScriptProjectOps::TemplateToken> tokens{GetMemoryResource(MemTag::Editor)};
		tokens.push_back({"TITLE", EscapeHTML(title.empty() ? str("Shalltear") : title)});
		tokens.push_back({"JS_FILE", jsFile});
		tokens.push_back({"WASM_FILE", wasmFile});
		tokens.push_back({"THREADS_ENABLED", enableThreads ? "true" : "false"});
		tokens.push_back({"HAS_MANAGED", hasManaged ? "true" : "false"});
		if (customTemplatePath.empty()) {
			return ScriptProjectOps::RenderTemplate("WebIndex.html", tokens, outHTML);
		}
		const Path templatePath(customTemplatePath);
		if (!FileSystem::Exists(templatePath) || FileSystem::IsDirectory(templatePath)) {
			return false;
		}
		str text = FileSystem::ReadTextFile(templatePath, MemTag::Editor);
		if (text.empty()) {
			return false;
		}
		for (const ScriptProjectOps::TemplateToken& token : tokens) {
			text = StringUtil::Replace(text, "{{" + token.name + "}}", token.value);
		}
		outHTML = std::move(text);
		return true;
	}

	BuildStepStatus WebTarget::StageStep(BuildContext& context) {
		if (!context.target || context.target->GetId() != "Web") {
			return BuildStepStatus::Succeeded;
		}
		const str jsFile = context.executableName + ".js";
		const str wasmFile = context.executableName + ".wasm";
		std::error_code ec;
		if (!fs::is_regular_file(UTF8Path(context.stagingDir + "/" + jsFile), ec) ||
			!fs::is_regular_file(UTF8Path(context.stagingDir + "/" + wasmFile), ec)) {
			context.AddIssue(BuildIssueSeverity::Error, "",
							 "Web build did not produce '" + jsFile + "' and '" + wasmFile + "' in '" +
								 context.stagingDir + "'");
			return BuildStepStatus::Failed;
		}

		str productName;
		u64 created = 0;
		ProjectOps::ReadProjectFile(context.projectRoot, productName, created);
		if (productName.empty()) {
			productName = context.executableName;
		}
		const bool enableThreads = ResolvedBool(context.profile, "enableThreads", false);
		const fs::path staleManaged = UTF8Path(context.stagingDir + "/Managed");
		if (fs::exists(staleManaged, ec)) {
			fs::remove_all(staleManaged, ec);
		}
		ec.clear();

		str customTemplateValue;
		if (const ResolvedSetting* setting = context.profile ? context.profile->Find(HTMLTemplateKey) : nullptr;
			setting && !setting->value.empty()) {
			customTemplateValue = setting->value;
		}

		ResolvedHTMLTemplate htmlTemplate;
		if (!customTemplateValue.empty()) {
			htmlTemplate = ResolveHTMLTemplate(context.projectRoot, customTemplateValue);
			if (!htmlTemplate.valid) {
				context.AddIssue(BuildIssueSeverity::Error, HTMLTemplateKey, htmlTemplate.error);
				return BuildStepStatus::Failed;
			}
			if (htmlTemplate.isDirectory) {
				if (!CopyTemplateCompanions(context, htmlTemplate.companionDir, jsFile, wasmFile)) {
					return BuildStepStatus::Failed;
				}
			}
		}

		str html;
		if (!RenderIndexHTML(productName, jsFile, wasmFile, enableThreads, html, false,
							 htmlTemplate.configured ? htmlTemplate.templateFile : str{})) {
			if (htmlTemplate.configured) {
				context.AddIssue(BuildIssueSeverity::Error, HTMLTemplateKey,
								 "couldn't render HTML template '" + customTemplateValue + "'");
			} else {
				context.AddIssue(BuildIssueSeverity::Error, "",
								 "couldn't render EngineAssets/Templates/WebIndex.html.template (is engine:// mounted?)");
			}
			return BuildStepStatus::Failed;
		}
		const str indexPath = context.stagingDir + "/index.html";
		if (!FileSystem::WriteTextFile(Path(indexPath), html)) {
			context.AddIssue(BuildIssueSeverity::Error, "", "couldn't write '" + indexPath + "'");
			return BuildStepStatus::Failed;
		}
		if (htmlTemplate.configured) {
			context.Log("staged Web entry page from '" + customTemplateValue + "' -> " + indexPath);
		} else {
			context.Log("staged Web entry page -> " + indexPath);
		}
		return BuildStepStatus::Succeeded;
	}

} // namespace Shalltear
