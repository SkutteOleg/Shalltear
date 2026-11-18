#include "DotNetToolchain.h"

#include "CSharpSettings.h"
#include "DotNetPin.generated.h"
#include "HostFxr.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Memory.h"

#include "Shalltear/Editor/ScriptProjectOps.h"

#include <cstdlib>
#include <filesystem>
#include <format>

namespace Shalltear::CSharpEditor::DotNetToolchain {

	namespace {

		namespace fs = std::filesystem;

		str ReadEngineSourceDirectory(const str& sdkRoot) {
			std::error_code ec;
			const fs::path config = fs::path(sdkRoot) / "ShalltearConfig.cmake";
			if (!fs::is_regular_file(config, ec)) {
				return {};
			}
			const str text = FileSystem::ReadTextFile(Path(config.generic_string()), MemTag::Editor);
			constexpr std::string_view Needle = "set(SHALLTEAR_ENGINE_SOURCE_DIR \"";
			const size_t start = text.find(Needle);
			if (start == str::npos) {
				return {};
			}
			const size_t end = text.find('"', start + Needle.size());
			return end == str::npos ? str() : text.substr(start + Needle.size(), end - start - Needle.size());
		}

		Info FindOnSearchPath(const Pin& pin, const str& searchPath) {
			const char separator = HostFxr::GetSearchPathSeparator();
			for (size_t start = 0; start <= searchPath.size();) {
				size_t end = searchPath.find(separator, start);
				end = end == str::npos ? searchPath.size() : end;
				if (end > start) {
					std::error_code ec;
					const fs::path muxer = fs::canonical(fs::path(searchPath.substr(start, end - start)) / HostFxr::GetMuxerName(), ec);
					if (!ec && HoldsPin(muxer.parent_path().generic_string(), pin)) {
						return {muxer.parent_path().generic_string(), Tier::SearchPath};
					}
				}
				start = end + 1;
			}
			return {};
		}

	} // anonymous namespace

	const Pin& GetPin() {
		static const Pin pin = [] {
			Pin made;
			made.sdkVersion = DotNetPinData::SDK_VERSION;
			made.runtimeVersion = DotNetPinData::RUNTIME_VERSION;
			made.targetFramework = DotNetPinData::TARGET_FRAMEWORK;
			made.hostRid = DotNetPinData::HOST_RID;
			made.archiveURL = DotNetPinData::ARCHIVE_URL;
			made.archiveSHA512 = DotNetPinData::ARCHIVE_SHA512;
			for (const DotNetPinData::Pack& pack : DotNetPinData::Packs) {
				made.packs.push_back({pack.name, pack.sha512});
			}
			return made;
		}();
		return pin;
	}

	str GetCacheRoot() {
		const Path userData = FileSystem::GetUserDataDirectory();
		return userData.ToString().empty() ? str() : (userData / "Shalltear" / "DotNet").ToString();
	}

	str GetPackagesRoot(const str& cacheRoot, const Pin& pin) {
		return cacheRoot.empty() ? str() : std::format("{}/packages/{}", cacheRoot, pin.runtimeVersion);
	}

	str GetMuxerPath(const str& sdkRoot) {
		return (fs::path(sdkRoot) / HostFxr::GetMuxerName()).generic_string();
	}

	DiscoveryOptions GetDefaultDiscovery() {
		DiscoveryOptions options;
		options.overrideRoot = CSharpSettings::GetUser().sdkPath;
		options.cacheRoot = GetCacheRoot();
		const str source = ReadEngineSourceDirectory(ScriptProjectOps::GetSDKRoot());
		if (!source.empty()) {
			options.checkoutRoot = source + "/External/Prebuilt/dotnet";
		}
		const char* searchPath = std::getenv("PATH");
		options.searchPath = searchPath ? searchPath : "";
		return options;
	}

	bool HoldsPin(const str& root, const Pin& pin) {
		if (root.empty()) {
			return false;
		}
		std::error_code ec;
		const fs::path base(root);
		return fs::is_regular_file(base / HostFxr::GetMuxerName(), ec) && fs::is_directory(base / "sdk" / pin.sdkVersion, ec) &&
			   fs::is_regular_file(base / "host" / "fxr" / pin.runtimeVersion / HostFxr::GetLibraryName(), ec);
	}

	Info Discover(const Pin& pin, const DiscoveryOptions& options) {
		if (HoldsPin(options.overrideRoot, pin)) {
			return {fs::path(options.overrideRoot).generic_string(), Tier::Override};
		}
		if (!options.cacheRoot.empty()) {
			const str root = (fs::path(options.cacheRoot) / pin.sdkVersion).generic_string();
			if (HoldsPin(root, pin)) {
				return {root, Tier::Cache};
			}
		}
		if (!options.checkoutRoot.empty()) {
			const str root = (fs::path(options.checkoutRoot) / pin.sdkVersion).generic_string();
			if (HoldsPin(root, pin)) {
				return {root, Tier::Checkout};
			}
		}
		return FindOnSearchPath(pin, options.searchPath);
	}

	str Describe(const Pin& pin, const DiscoveryOptions& options) {
		str looked;
		const auto add = [&looked](const str& place) {
			if (!place.empty()) {
				looked += looked.empty() ? "" : ", ";
				looked += "'" + place + "'";
			}
		};
		add(options.overrideRoot);
		add(options.cacheRoot.empty() ? str() : options.cacheRoot + "/" + pin.sdkVersion);
		add(options.checkoutRoot.empty() ? str() : options.checkoutRoot + "/" + pin.sdkVersion);
		looked += looked.empty() ? "PATH" : ", PATH";
		return std::format(".NET SDK {} was not found (looked in {}). Set SDK Path in Settings > Editor > C#",
						   pin.sdkVersion, looked);
	}

	str FindSDKRoot() {
		return Discover(GetPin(), GetDefaultDiscovery()).root;
	}

	str GetRuntimeAssemblyPath() {
		const fs::path executable = ScriptProjectOps::GetEditorExecutablePath();
		return (executable.parent_path() / "CSharp" / "Shalltear.Runtime.dll").generic_string();
	}

} // namespace Shalltear::CSharpEditor::DotNetToolchain
