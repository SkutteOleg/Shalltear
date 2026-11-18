#include "DotNetToolchain.h"

#include "HostFxr.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Memory.h"

#include "Shalltear/Editor/ScriptProjectOps.h"

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

		bool HoldsPinnedRuntime(const str& root) {
			std::error_code ec;
			return fs::is_regular_file(fs::path(std::format("{}/host/fxr/{}/{}", root, SLT_DOTNET_RUNTIME_VERSION, HostFxr::GetLibraryName())), ec);
		}

	} // anonymous namespace

	str FindSDKRoot() {
		const str source = ReadEngineSourceDirectory(ScriptProjectOps::GetSDKRoot());
		if (source.empty()) {
			return {};
		}
		const str root = std::format("{}/External/Prebuilt/dotnet/{}", source, SLT_DOTNET_SDK_VERSION);
		return HoldsPinnedRuntime(root) ? root : str();
	}

	str GetRuntimeAssemblyPath() {
		const fs::path executable = ScriptProjectOps::GetEditorExecutablePath();
		return (executable.parent_path() / "CSharp" / "Shalltear.Runtime.dll").generic_string();
	}

} // namespace Shalltear::CSharpEditor::DotNetToolchain
