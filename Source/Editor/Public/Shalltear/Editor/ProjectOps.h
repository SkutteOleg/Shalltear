#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/Editor/EditorAPI.h"

#include <span>
#include <string_view>

namespace Shalltear::ProjectOps {

	enum class ProjectStatus : u8 {
		Ok = 0,
		Missing,
		Invalid,
	};

	struct ProjectEntry {
		str path;
		str name;
		u64 createdUnix = 0;
		u64 modifiedUnix = 0;
		u64 lastOpenedUnix = 0;
		ProjectStatus status = ProjectStatus::Ok;

		bool IsOpenable() const { return status == ProjectStatus::Ok; }
	};

	SHALLTEAR_EDITOR_API str NormalizePath(const str& path);

	SHALLTEAR_EDITOR_API bool PathsEqual(const str& a, const str& b);

	SHALLTEAR_EDITOR_API bool IsValidProjectName(const str& name);

	SHALLTEAR_EDITOR_API ProjectStatus GetStatus(const str& projectPath);
	SHALLTEAR_EDITOR_API bool IsProjectDirectory(const str& projectPath);

	SHALLTEAR_EDITOR_API bool ReadProjectFile(const str& projectPath, str& outName, u64& outCreatedUnix);

	SHALLTEAR_EDITOR_API u64 GetModifiedTime(const str& projectPath);

	SHALLTEAR_EDITOR_API bool CreateProject(const str& parentDirectory, const str& name, str& outPath);

	struct CreateOptions {
		bool includeCppModule = true;
		str sdkRoot;
		str editorExecutable;
		Vector<str> optInPackages{GetMemoryResource(MemTag::Editor)};
	};

	SHALLTEAR_EDITOR_API bool CreateProject(const str& parentDirectory, const str& name,
											const CreateOptions& options, str& outPath);

	SHALLTEAR_EDITOR_API Path GetDefaultRegistryPath();

	SHALLTEAR_EDITOR_API void LoadRegistry(const Path& registryPath, Vector<ProjectEntry>& out);
	SHALLTEAR_EDITOR_API bool SaveRegistry(const Path& registryPath, const Vector<ProjectEntry>& entries);

	SHALLTEAR_EDITOR_API void LoadOptInChoice(const Path& registryPath, Vector<str>& out);
	SHALLTEAR_EDITOR_API bool SaveOptInChoice(const Path& registryPath, std::span<const str> names);

	SHALLTEAR_EDITOR_API bool AddProject(const Path& registryPath, const str& projectPath);

	SHALLTEAR_EDITOR_API bool RemoveProject(const Path& registryPath, const str& projectPath);

	SHALLTEAR_EDITOR_API void TouchProject(const Path& registryPath, const str& projectPath);

	SHALLTEAR_EDITOR_API bool DeleteProjectFromDisk(const Path& registryPath, const str& projectPath);

	SHALLTEAR_EDITOR_API bool MatchesFilter(const ProjectEntry& entry, std::string_view filter);

	SHALLTEAR_EDITOR_API bool MountProject(const str& projectPath);

	SHALLTEAR_EDITOR_API str GetMountedProjectPath();

	SHALLTEAR_EDITOR_API str BuildRelaunchCommandLine(const str& executablePath,
													  std::span<const str> arguments,
													  const str& projectPath);

} // namespace Shalltear::ProjectOps
