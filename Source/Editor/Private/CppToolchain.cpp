#include "Shalltear/Editor/CppToolchain.h"

#include "Shalltear/Core/BuildInfo.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/StringUtil.h"

#include "Shalltear/Editor/ScriptProjectOps.h"

#include "Shalltear/Editor/ChildProcess.h"
#include "Platform/EditorPlatform.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <system_error>
#include <thread>

namespace Shalltear::CppToolchain {

	namespace {

		namespace fs = std::filesystem;

		constexpr int PROBE_TIMEOUT_MS = 4000;
		constexpr int PROBE_POLL_MS = 10;

		str EnvironmentValue(const char* name) {
			const char* value = std::getenv(name);
			return value ? str(value) : str();
		}

		Vector<str> SplitSearchPath(const str& value) {
			Vector<str> parts{GetMemoryResource(MemTag::Editor)};
			size_t start = 0;
			while (start <= value.size()) {
				size_t end = value.find(EditorPlatform::GetSearchPathSeparator(), start);
				if (end == str::npos) {
					end = value.size();
				}
				if (end > start) {
					parts.push_back(value.substr(start, end - start));
				}
				start = end + 1;
			}
			return parts;
		}

		str ReadPackageConfigText(const str& sdkRoot) {
			if (sdkRoot.empty()) {
				return {};
			}
			std::error_code ec;
			fs::path config = fs::path(sdkRoot) / "ShalltearConfig.cmake";
			if (!fs::is_regular_file(config, ec)) {
				config = fs::path(sdkRoot) / "Lib" / "cmake" / "Shalltear" / "ShalltearConfig.cmake";
			}
			if (!fs::is_regular_file(config, ec)) {
				return {};
			}
			return FileSystem::ReadTextFile(Path(config.generic_string()), MemTag::Editor);
		}

		str ReadPackageVariable(const str& text, std::string_view name) {
			str needle = "set(" + str(name) + " \"";
			size_t start = text.find(needle);
			if (start == str::npos) {
				return {};
			}
			start += needle.size();
			size_t end = text.find('"', start);
			return end == str::npos ? str() : text.substr(start, end - start);
		}

		Vector<str> RecordedToolchainDirectories(const str& configText) {
			Vector<str> directories{GetMemoryResource(MemTag::Editor)};
			str compiler = ReadPackageVariable(configText, "SHALLTEAR_COMPILER_PATH");
			if (compiler.empty()) {
				return directories;
			}
			std::error_code ec;
			if (!fs::is_regular_file(fs::path(compiler), ec)) {
				return directories;
			}
			directories.push_back(fs::path(compiler).parent_path().generic_string());
			return directories;
		}

		Vector<str> BundledToolchainDirectories(const str& sdkRoot) {
			Vector<str> directories{GetMemoryResource(MemTag::Editor)};
			if (sdkRoot.empty()) {
				return directories;
			}
			std::error_code ec;
			fs::path probe = fs::path(sdkRoot);
			for (int depth = 0; depth < 4 && !probe.empty(); ++depth) {
				if (fs::is_directory(probe / "Toolchain", ec)) {
					str install = probe.generic_string();
					directories.push_back(install + "/Toolchain/LLVM/bin");
					directories.push_back(install + "/Toolchain/CMake/bin");
					directories.push_back(install + "/Toolchain/Ninja");
					directories.push_back(install + "/Toolchain/emsdk/install/emscripten");
					directories.push_back(install + "/Toolchain/emsdk/upstream/emscripten");
					return directories;
				}
				fs::path parent = probe.parent_path();
				if (parent == probe) {
					break;
				}
				probe = parent;
			}
			return directories;
		}

		Vector<str> DefaultSearchDirectories() {
			Vector<str> directories{GetMemoryResource(MemTag::Editor)};
			EditorPlatform::AppendDefaultToolDirectories(directories);
			return directories;
		}

		str FindInDirectories(const Vector<str>& directories, const Vector<std::string_view>& executableNames) {
			std::error_code ec;
			for (std::string_view executableName : executableNames) {
				for (const str& directory : directories) {
					if (directory.empty()) {
						continue;
					}
					fs::path candidate = fs::path(directory) / (str(executableName) + str(EditorPlatform::GetExecutableSuffix()));
					if (fs::is_regular_file(candidate, ec)) {
						return candidate.generic_string();
					}
				}
			}
			return {};
		}

		str ProbeVersion(const str& toolPath) {
			ChildProcess process;
			if (!process.Start("\"" + toolPath + "\" --version", {})) {
				return {};
			}
			Vector<str> lines{GetMemoryResource(MemTag::Editor)};
			i32 exitCode = 0;
			for (int waited = 0; waited < PROBE_TIMEOUT_MS; waited += PROBE_POLL_MS) {
				process.DrainLines(lines);
				if (process.TryFinish(exitCode)) {
					process.DrainLines(lines);
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(PROBE_POLL_MS));
			}
			if (process.IsRunning()) {
				process.Kill();
				return {};
			}
			for (const str& line : lines) {
				str trimmed = StringUtil::Trim(line);
				if (!trimmed.empty()) {
					return trimmed;
				}
			}
			return {};
		}

		ToolInfo ResolveTool(std::string_view name, const Vector<std::string_view>& executableNames, const str& overridePath,
							 const Vector<str>& bundledDirectories, const Vector<str>& pathDirectories,
							 const Vector<str>& extraDirectories, bool probeVersions) {
			ToolInfo info;
			info.name = str(name);

			std::error_code ec;
			if (!overridePath.empty() && fs::is_regular_file(fs::path(overridePath), ec)) {
				info.path = fs::path(overridePath).generic_string();
			}
			if (info.path.empty()) {
				info.path = FindInDirectories(bundledDirectories, executableNames);
			}
			if (info.path.empty()) {
				info.path = FindInDirectories(pathDirectories, executableNames);
			}
			if (info.path.empty()) {
				info.path = FindInDirectories(extraDirectories, executableNames);
			}
			if (info.path.empty()) {
				info.status = ToolStatus::Missing;
				return info;
			}

			if (!probeVersions) {
				info.status = ToolStatus::Found;
				return info;
			}
			info.version = ProbeVersion(info.path);
			info.status = info.version.empty() ? ToolStatus::Unusable : ToolStatus::Found;
			return info;
		}

		void AppendToolLine(str& out, const ToolInfo& tool) {
			switch (tool.status) {
			case ToolStatus::Found:
				out += std::format("  {:<9} {}  ({})\n", tool.name, tool.version, tool.path);
				break;
			case ToolStatus::Unusable:
				out += std::format("  {:<9} FOUND BUT UNUSABLE at {} (--version did not answer)\n", tool.name, tool.path);
				break;
			default:
				out += std::format("  {:<9} NOT FOUND\n", tool.name);
				break;
			}
		}

	} // namespace

	bool ReadSDKIdentity(const str& sdkRoot, ToolchainInfo& out) {
		if (sdkRoot.empty()) {
			return false;
		}
		str text = ReadPackageConfigText(sdkRoot);
		if (text.empty()) {
			return false;
		}

		out.sdkRoot = sdkRoot;
		out.sdkCompilerId = ReadPackageVariable(text, "SHALLTEAR_COMPILER_ID");
		out.sdkCompilerVersion = ReadPackageVariable(text, "SHALLTEAR_COMPILER_VERSION");
		out.sdkCompilerTarget = ReadPackageVariable(text, "SHALLTEAR_COMPILER_TARGET");
		out.sdkLinkMode = ReadPackageVariable(text, "SHALLTEAR_LINK_MODE");
		out.sdkToolchainFile = ReadPackageVariable(text, "SHALLTEAR_TOOLCHAIN_FILE");
		constexpr std::string_view PackageRoot = "${SHALLTEAR_PACKAGE_ROOT}";
		if (out.sdkToolchainFile.starts_with(PackageRoot)) {
			out.sdkToolchainFile = sdkRoot + out.sdkToolchainFile.substr(PackageRoot.size());
		}
		return true;
	}

	std::string_view GetCompilerDriverName(const ToolchainInfo& info) {
		if (info.sdkCompilerTarget == "wasm32-unknown-emscripten") {
			return "em++";
		}
		return info.sdkToolchainFile.empty() ? "x86_64-w64-mingw32-clang++" : "clang++";
	}

	str GetEngineIdentity() {
		return std::format("{}|{}", BuildInfo::GetVersionString(), BuildInfo::GetGitCommit());
	}

	ToolchainInfo Discover(const DiscoveryOptions& options) {
		str sdkRoot = options.sdkRoot.empty() ? ScriptProjectOps::GetSDKRoot() : options.sdkRoot;

		str configText = ReadPackageConfigText(sdkRoot);
		Vector<str> bundledDirectories = BundledToolchainDirectories(sdkRoot);
		for (const str& recorded : RecordedToolchainDirectories(configText)) {
			bundledDirectories.push_back(recorded);
		}
		Vector<str> pathDirectories = SplitSearchPath(EnvironmentValue("PATH"));
		Vector<str> extraDirectories = options.extraSearchDirectories.empty()
										   ? DefaultSearchDirectories()
										   : options.extraSearchDirectories;

		ToolchainInfo info;
		ReadSDKIdentity(sdkRoot, info);
		Vector<std::string_view> cmakeNames{GetMemoryResource(MemTag::Editor)};
		cmakeNames.push_back("cmake");
		Vector<std::string_view> ninjaNames{GetMemoryResource(MemTag::Editor)};
		ninjaNames.push_back("ninja");
		Vector<std::string_view> compilerNames{GetMemoryResource(MemTag::Editor)};
		compilerNames.push_back(GetCompilerDriverName(info));
		if (compilerNames.back() != "clang++") {
			compilerNames.push_back("clang++");
		}

		info.cmake = ResolveTool("cmake", cmakeNames, options.cmakePath, bundledDirectories, pathDirectories, extraDirectories, options.probeVersions);
		info.ninja = ResolveTool("ninja", ninjaNames, options.ninjaPath, bundledDirectories, pathDirectories, extraDirectories, options.probeVersions);
		info.compiler = ResolveTool("clang++", compilerNames, options.compilerPath, bundledDirectories, pathDirectories, extraDirectories, options.probeVersions);
		return info;
	}

	ToolchainInfo Discover() {
		return Discover(DiscoveryOptions{});
	}

	str Describe(const ToolchainInfo& info) {
		str out;
		AppendToolLine(out, info.cmake);
		AppendToolLine(out, info.ninja);
		AppendToolLine(out, info.compiler);

		if (info.sdkRoot.empty()) {
			out += "\nSDK: not located (the editor couldn't find its ShalltearConfig.cmake).\n";
		} else {
			out += std::format("\nSDK: {}\n  built with {} {} for {} ({} link)\n",
							   info.sdkRoot, info.sdkCompilerId, info.sdkCompilerVersion,
							   info.sdkCompilerTarget.empty() ? str("an unrecorded target") : info.sdkCompilerTarget,
							   info.sdkLinkMode);
		}

		if (info.IsComplete()) {
			out += "\nAll tools present.\n";
			return out;
		}

		out += "\nTo fix:\n";
		if (info.cmake.status != ToolStatus::Found) {
			out += "  cmake    install CMake 3.20+ and put it on PATH (https://cmake.org/download/)\n";
		}
		if (info.ninja.status != ToolStatus::Found) {
			out += "  ninja    install Ninja and put it on PATH (https://ninja-build.org/)\n";
		}
		if (info.compiler.status != ToolStatus::Found) {
			out += std::format("  clang++  install LLVM/Clang {} and put its bin/ on PATH (https://releases.llvm.org/)\n",
							   info.sdkCompilerVersion.empty() ? str("(matching this SDK)") : info.sdkCompilerVersion);
		}
		out += "Or set explicit paths in Edit > Settings > Editor > Scripting.\n";
		return out;
	}

} // namespace Shalltear::CppToolchain
