#pragma once

#include "CSharpProjectFiles.h"
#include "CSharpSetup.h"
#include "DotNetAcquire.h"
#include "DotNetToolchain.h"

#include "Shalltear/Core/Types.h"

#include <filesystem>

namespace Shalltear::CSharpEditor {

	struct EditorTestHooks {
		str (*hashFile)(const str& path);
		const DotNetToolchain::Pin& (*getPin)();
		DotNetToolchain::Info (*discover)(const DotNetToolchain::Pin& pin, const DotNetToolchain::DiscoveryOptions& options);
		str (*describe)(const DotNetToolchain::Pin& pin, const DotNetToolchain::DiscoveryOptions& options);
		bool (*acquire)(const AcquireOptions& options, AcquireStep cancelAt, AcquireStatus& outStatus);
		str (*describeStep)(const AcquireStatus& status, const DotNetToolchain::Pin& pin);
		str (*findSystemTool)(std::string_view name);
		std::filesystem::path (*makeLongPath)(const str& path);
		CSharpProjectFiles::Options (*getDefaultProjectOptions)(const str& projectPath, const str& dotnetRoot);
		bool (*writeProjectFiles)(const CSharpProjectFiles::Options& options, CSharpProjectFiles::Result& out);
		void (*beginSetup)(CSharpSetup::Options options, const void* owner);
		void (*pumpSetup)();
		void (*shutdownSetup)();
		CSharpSetup::State (*getSetupState)();
		const str& (*getSetupSDKRoot)();
		void (*declareSettings)(const void* owner);
		const char* muxerName;
		const char* hostLibraryName;
	};

	inline constexpr const char* EDITOR_TEST_HOOKS_SYMBOL = "ShalltearCSharpEditorTestHooks";

	using GetEditorTestHooksFn = const EditorTestHooks* (*)();

} // namespace Shalltear::CSharpEditor
