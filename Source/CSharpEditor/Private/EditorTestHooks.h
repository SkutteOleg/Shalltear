#pragma once

#include "CSharpAuthoring.h"
#include "CSharpCompile.h"
#include "CSharpDiagnostics.h"
#include "CSharpProjectFiles.h"
#include "CSharpReload.h"
#include "CSharpScriptAsset.h"
#include "CSharpScriptEditor.h"
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
		void (*registerAttachConfiguration)(const void* owner);
		void (*beginSetup)(CSharpSetup::Options options, const void* owner);
		void (*pumpSetup)();
		void (*shutdownSetup)();
		CSharpSetup::State (*getSetupState)();
		const str& (*getSetupSDKRoot)();
		void (*declareSettings)(const void* owner);
		bool (*parseDiagnostic)(std::string_view line, CSharpDiagnostics::Diagnostic& out);
		bool (*addUniqueDiagnostic)(Vector<CSharpDiagnostics::Diagnostic>& all, HashSet<str>& seen, const CSharpDiagnostics::Diagnostic& diagnostic);
		str (*describeDiagnostic)(const CSharpDiagnostics::Diagnostic& diagnostic);
		void (*configureCompile)(const CompileOptions& options);
		bool (*requestCompile)();
		void (*cancelCompile)();
		void (*pumpCompile)();
		const CSharpCompile& (*getCompile)();
		str (*getCompileOutput)();
		void (*beginAuthoring)(const CSharpAuthoring::Options& options, const void* owner);
		void (*pumpAuthoring)();
		void (*shutdownAuthoring)();
		bool (*isAuthoringActive)();
		const CSharpCompile& (*getAuthoringCompile)();
		void (*notifySourceChanged)();
		bool (*isHoldingReports)();
		void (*beginReload)(const str& assembly);
		void (*shutdownReload)();
		void (*pumpReload)();
		void (*requestReload)();
		void (*reloadNow)();
		bool (*isReloadHeld)();
		void (*setBuildRunning)(bool running);
		bool (*areScriptsLoaded)();
		CSharpReload::Outcome (*getReloadOutcome)();
		u32 (*getReloadCount)();
		u32 (*getReloadMigratedCount)();
		f64 (*getReloadMilliseconds)();
		void (*registerScriptImporter)(const void* owner);
		void (*registerScriptEditor)(const void* owner);
		void (*getScriptComponents)(const Guid& script, Vector<const TypeInfo*>& out);
		const TypeInfo* (*resolveScriptComponent)(const Guid& script);
		bool (*isValidScriptName)(std::string_view name);
		Guid (*createScript)(const Path& virtualDir, const str& name, CSharpScripts::Template kind);
		const char* muxerName;
		const char* hostLibraryName;
	};

	inline constexpr const char* EDITOR_TEST_HOOKS_SYMBOL = "ShalltearCSharpEditorTestHooks";

	using GetEditorTestHooksFn = const EditorTestHooks* (*)();

} // namespace Shalltear::CSharpEditor
