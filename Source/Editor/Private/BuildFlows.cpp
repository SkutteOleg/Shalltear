#include "BuildFlows.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneSystem.h"

#include "Shalltear/Editor/BuildPipeline.h"
#include "Shalltear/Editor/BuildProfile.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/ProjectOps.h"

#include "Shalltear/Editor/ChildProcess.h"
#include "EditorConfig.h"
#include "SceneFileFlows.h"

#include <format>

namespace Shalltear::BuildFlows {

	namespace {

		struct PendingRequest {
			bool active = false;
			bool run = false;
			str projectPath;
		};

		PendingRequest _pending;

		str _gitignoreAskedFor;

		bool ScenePathCoversContent(const Handle<Scene>& scene, str& outPath) {
			Path path = EditorSceneOps::GetScenePath(scene);
			outPath = path.ToString();
			return !outPath.empty();
		}

		str OutputRootRelativeToProject(const str& projectPath, const str& stagingDir) {
			if (projectPath.empty() || stagingDir.empty()) {
				return {};
			}
			str prefix = projectPath + "/";
			if (stagingDir.size() <= prefix.size() || stagingDir.compare(0, prefix.size(), prefix) != 0) {
				return {};
			}
			str relative = stagingDir.substr(prefix.size());
			size_t slash = relative.find('/');
			return slash == str::npos ? relative : relative.substr(0, slash);
		}

		str TrimAscii(std::string_view text) {
			size_t begin = text.find_first_not_of(" \t\r\n");
			if (begin == std::string_view::npos) {
				return {};
			}
			size_t end = text.find_last_not_of(" \t\r\n");
			return str(text.substr(begin, end - begin + 1));
		}

		bool GitignoreCovers(const str& text, const str& directory) {
			str slashed = directory + "/";
			size_t start = 0;
			while (start <= text.size()) {
				size_t end = text.find('\n', start);
				if (end == str::npos) {
					end = text.size();
				}
				str line = TrimAscii(std::string_view(text).substr(start, end - start));
				start = end + 1;
				if (line.empty() || line.front() == '#') {
					continue;
				}
				if (!line.empty() && line.front() == '/') {
					line.erase(line.begin());
				}
				if (line == directory || line == slashed) {
					return true;
				}
			}
			return false;
		}

		void AppendToGitignore(const str& projectPath, const str& directory) {
			Path path(projectPath + "/.gitignore");
			str text = FileSystem::ReadTextFile(path, MemTag::Editor);
			if (!text.empty() && text.back() != '\n') {
				text += '\n';
			}
			text += "\n# built games (Shalltear). Reproducible from what is committed here\n/";
			text += directory;
			text += "/\n";
			if (FileSystem::WriteTextFile(path, text)) {
				SLT_LOG_INFO("Build", "added '/{}/' to {}", directory, path.ToString());
			} else {
				SLT_LOG_WARN("Build", "couldn't write {}", path.ToString());
			}
		}

		void MaybeOfferGitignore(UI::Context& context, const str& projectPath, const str& outputRoot) {
			if (projectPath.empty() || outputRoot.empty() || _gitignoreAskedFor == projectPath) {
				return;
			}
			EditorConfig* config = EditorConfig::GetActive();
			if (config && !config->ReadBuildPreferences().offerGitignore) {
				return;
			}
			Path gitignorePath(projectPath + "/.gitignore");
			str text = FileSystem::ReadTextFile(gitignorePath, MemTag::Editor);
			if (text.empty() || GitignoreCovers(text, outputRoot)) {
				return;
			}
			_gitignoreAskedFor = projectPath;

			Vector<str> details{GetMemoryResource(MemTag::Editor)};
			details.push_back("A build is tens of megabytes per profile and is reproducible");
			details.push_back("from what is already committed here.");
			SceneFileFlows::OpenDetailedConfirmPopup(
				context, "Add '/" + outputRoot + "/' to this project's .gitignore?", {details.data(), details.size()},
				"Add", [projectPath, outputRoot]() { AppendToGitignore(projectPath, outputRoot); },
				"Not now", {},
				"Never ask", []() {
					if (EditorConfig* active = EditorConfig::GetActive()) {
						EditorConfig::BuildPreferences preferences = active->ReadBuildPreferences();
						preferences.offerGitignore = false;
						active->WriteBuildPreferences(preferences);
					} });
		}

		void Submit(const Vector<str>& profiles, bool runAfterBuild) {
			BuildPipeline* pipeline = BuildPipeline::GetActive();
			if (!pipeline) {
				SLT_LOG_WARN("Build", "no build pipeline in this session");
				return;
			}
			BuildRequest request;
			request.projectPath = ProjectOps::GetMountedProjectPath();
			for (const str& name : profiles) {
				request.profiles.push_back(name);
			}
			str refusal;
			if (!pipeline->Request(request, refusal)) {
				SLT_LOG_WARN("Build", "{}", refusal);
				return;
			}
			_pending.active = true;
			_pending.run = runAfterBuild;
			_pending.projectPath = request.projectPath;
		}

		void SaveEveryDirtyScene() {
			for (const Handle<Scene>& scene : SceneSystem::GetLoadedScenes()) {
				if (!EditorSceneOps::IsDirty(scene)) {
					continue;
				}
				if (EditorSceneOps::GetPrefabTarget(scene).IsValid()) {
					EditorSceneOps::SavePrefabScene(scene);
					continue;
				}
				str path;
				if (!ScenePathCoversContent(scene, path)) {
					SLT_LOG_WARN("Build", "scene '{}' has never been saved (not in the build)",
								 scene.Get() ? scene.Get()->name : str("?"));
					continue;
				}
				EditorSceneOps::SaveScene(scene, Path(path));
			}
		}

	} // anonymous namespace

	void GetUnsavedWork(Vector<str>& out) {
		out.clear();
		for (const Handle<Scene>& scene : SceneSystem::GetLoadedScenes()) {
			if (!EditorSceneOps::IsDirty(scene)) {
				continue;
			}
			Scene* resolved = scene.Get();
			str name = resolved ? resolved->name : str("?");
			if (EditorSceneOps::GetPrefabTarget(scene).IsValid()) {
				out.push_back("prefab stage '" + name + "' - not applied");
				continue;
			}
			str path;
			if (!ScenePathCoversContent(scene, path)) {
				out.push_back("scene '" + name + "' was never saved (not on disk)");
				continue;
			}
			out.push_back("scene '" + name + "' - " + path);
		}
	}

	void RequestBuild(UI::Context& context, const Vector<str>& profiles, bool runAfterBuild) {
		if (profiles.empty()) {
			return;
		}
		Vector<str> unsaved{GetMemoryResource(MemTag::Editor)};
		GetUnsavedWork(unsaved);
		if (unsaved.empty()) {
			Submit(profiles, runAfterBuild);
			return;
		}

		Vector<str> queued{GetMemoryResource(MemTag::Editor)};
		for (const str& name : profiles) {
			queued.push_back(name);
		}
		SceneFileFlows::OpenDetailedConfirmPopup(
			context, "A build reads what is on disk. This is not saved:", {unsaved.data(), unsaved.size()},
			"Save All and Build", [queued, runAfterBuild]() {
				SaveEveryDirtyScene();
				Submit(queued, runAfterBuild); },
			"Build Anyway", [queued, runAfterBuild]() { Submit(queued, runAfterBuild); });
	}

	bool IsRunPending() {
		return _pending.active && _pending.run;
	}

	void PumpMainThread(UI::Context* context) {
		if (!_pending.active) {
			return;
		}
		BuildPipeline* pipeline = BuildPipeline::GetActive();
		if (!pipeline || pipeline->IsBusy()) {
			return;
		}

		bool run = _pending.run;
		str projectPath = _pending.projectPath;
		_pending = PendingRequest{};

		const BuildResult& result = pipeline->GetResult();
		const BuildProfileResult* lastSuccess = nullptr;
		for (const BuildProfileResult& entry : result.profiles) {
			if (entry.outcome == BuildOutcome::Succeeded) {
				lastSuccess = &entry;
			}
		}
		if (!lastSuccess) {
			return;
		}

		if (run) {
			if (lastSuccess->launchCommand.empty()) {
				SLT_LOG_WARN("Build", "'{}' built, but its target produces nothing runnable on this machine",
							 lastSuccess->profile);
			} else if (StartDetachedProcess(lastSuccess->launchCommand, lastSuccess->outputPath)) {
				SLT_LOG_INFO("Build", "running {}", lastSuccess->launchCommand);
			}
		}

		if (context) {
			MaybeOfferGitignore(*context, projectPath,
								OutputRootRelativeToProject(projectPath, lastSuccess->outputPath));
		}
	}

} // namespace Shalltear::BuildFlows
