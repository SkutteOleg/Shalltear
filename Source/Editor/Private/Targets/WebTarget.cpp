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

		void AddIssue(Vector<BuildIssue>& out, BuildIssueSeverity severity, const str& profile, const char* key,
					  str message) {
			out.push_back(BuildIssue{severity, profile, str(key), std::move(message)});
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
	}

	void WebTarget::Validate(const str&, const ResolvedProfile& profile, Vector<BuildIssue>& out) const {
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
									bool enableThreads, str& outHTML, bool hasManaged) {
		Vector<ScriptProjectOps::TemplateToken> tokens{GetMemoryResource(MemTag::Editor)};
		tokens.push_back({"TITLE", EscapeHTML(title.empty() ? str("Shalltear") : title)});
		tokens.push_back({"JS_FILE", jsFile});
		tokens.push_back({"WASM_FILE", wasmFile});
		tokens.push_back({"THREADS_ENABLED", enableThreads ? "true" : "false"});
		tokens.push_back({"HAS_MANAGED", hasManaged ? "true" : "false"});
		return ScriptProjectOps::RenderTemplate("WebIndex.html", tokens, outHTML);
	}

	BuildStepStatus WebTarget::StageStep(BuildContext& context) {
		if (!context.target || context.target->GetId() != "Web") {
			return BuildStepStatus::Succeeded;
		}
		const str jsFile = context.executableName + ".js";
		const str wasmFile = context.executableName + ".wasm";
		std::error_code ec;
		if (!fs::is_regular_file(fs::path{context.stagingDir + "/" + jsFile}, ec) ||
			!fs::is_regular_file(fs::path{context.stagingDir + "/" + wasmFile}, ec)) {
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
		bool hasManaged = fs::is_regular_file(fs::path{context.stagingDir + "/Managed/managed-manifest.json"}, ec);
		if (!hasManaged) {
			constexpr std::string_view PREFIX = "-DSHALLTEAR_CSHARP_STATIC_LINK_CMAKE=";
			for (const str& arg : context.extraConfigureArgs) {
				if (arg.starts_with(PREFIX) && arg.size() > PREFIX.size()) {
					hasManaged = true;
					break;
				}
			}
		}

		str html;
		if (!RenderIndexHTML(productName, jsFile, wasmFile, enableThreads, html, hasManaged)) {
			context.AddIssue(BuildIssueSeverity::Error, "",
							 "couldn't render EngineAssets/Templates/WebIndex.html.template (is engine:// mounted?)");
			return BuildStepStatus::Failed;
		}
		const str indexPath = context.stagingDir + "/index.html";
		if (!FileSystem::WriteTextFile(Path(indexPath), html)) {
			context.AddIssue(BuildIssueSeverity::Error, "", "couldn't write '" + indexPath + "'");
			return BuildStepStatus::Failed;
		}
		context.Log("staged Web entry page -> " + indexPath);
		return BuildStepStatus::Succeeded;
	}

} // namespace Shalltear
