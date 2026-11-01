#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/Editor/EditorAPI.h"

#include <optional>
#include <string_view>

namespace Shalltear::ScriptProjectOps {

	SHALLTEAR_EDITOR_API std::string_view GetActiveConfigName();

	SHALLTEAR_EDITOR_API str GetEditorExecutablePath();

	SHALLTEAR_EDITOR_API str GetSDKRoot();

	SHALLTEAR_EDITOR_API str GetSourceDirectory(const str& projectPath);

	SHALLTEAR_EDITOR_API str GetModuleName(const str& projectPath);

	SHALLTEAR_EDITOR_API str GetCompileDirectory(const str& projectPath, std::string_view configName);

	SHALLTEAR_EDITOR_API str GetModuleBinaryPath(const str& projectPath, std::string_view configName);

	SHALLTEAR_EDITOR_API str GetModuleCompileStampPath(const str& projectPath, std::string_view configName);

	SHALLTEAR_EDITOR_API bool HasScriptProject(const str& projectPath);

	SHALLTEAR_EDITOR_API bool IsModuleBinaryOutOfDate(const str& projectPath, std::string_view configName,
													  str* outReason = nullptr);

	SHALLTEAR_EDITOR_API bool IsCompileCacheStale(const str& projectPath, std::string_view configName,
												  const str& compilerPath, str* outReason = nullptr);

	enum class ModuleStatus : u8 {
		NoScriptProject,
		Compiling,
		CompileFailed,
		LoadFailed,
		Loaded
	};

	struct ModuleStatusFacts {
		bool hasScriptProject = false;
		bool compiling = false;
		bool compileFailed = false;
		bool moduleLoaded = false;
	};

	SHALLTEAR_EDITOR_API ModuleStatus ClassifyModuleStatus(const ModuleStatusFacts& facts);

	SHALLTEAR_EDITOR_API ModuleStatus GetModuleStatus(const str& projectPath);

	struct ScriptSettings {
		str moduleName;
		str compileCommand;

		bool compileCommandIsLegacy = false;
	};

	SHALLTEAR_EDITOR_API ScriptSettings ReadScriptSettings(const str& projectPath);

	SHALLTEAR_EDITOR_API bool WriteScriptSettings(const str& projectPath, const ScriptSettings& settings);

	struct TemplateToken {
		str name;
		str value;
	};

	SHALLTEAR_EDITOR_API bool RenderTemplate(std::string_view templateName,
											 const Vector<TemplateToken>& tokens,
											 str& out);

	struct ScaffoldOptions {
		str moduleName;
		str sdkRoot;
		str editorExecutable;

		str toolchainDirectory;

		std::optional<Vector<str>> enabledPackages;
		bool includeStarterComponent = true;
		bool includeMachineScopedFiles = true;
	};

	struct ScaffoldResult {
		Vector<str> written;
		Vector<str> skipped;
	};

	SHALLTEAR_EDITOR_API bool ScaffoldScriptProject(const str& projectPath,
													const ScaffoldOptions& options,
													ScaffoldResult& out);

	SHALLTEAR_EDITOR_API bool RegenerateProjectFiles(const str& projectPath,
													 const ScaffoldOptions& options,
													 ScaffoldResult& out);

	SHALLTEAR_EDITOR_API bool NeedsProjectFileRegeneration(const str& projectPath, const ScaffoldOptions& options);

	struct ExternalEditor {
		str name;
		str folderCommand;
		str fileCommand;
	};

	SHALLTEAR_EDITOR_API ExternalEditor DetectExternalEditor();

	SHALLTEAR_EDITOR_API bool OpenInExternalEditor(const str& projectPath, const str& commandTemplate,
												   const str& targetPath, u32 line, u32 column);

	SHALLTEAR_EDITOR_API bool IsValidClassName(std::string_view className);

	SHALLTEAR_EDITOR_API bool CreateComponentSource(const str& projectPath,
													const str& folderRelativeToSource,
													const str& className,
													ScaffoldResult& out);

	SHALLTEAR_EDITOR_API bool CreateCSharpComponentSource(const str& projectPath,
														  const str& folderRelativeToSource,
														  const str& className,
														  ScaffoldResult& out);

} // namespace Shalltear::ScriptProjectOps
