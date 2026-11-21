#include "BatchBuild.h"

#include "PackageHost.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Module/ModuleSystem.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/Settings.h"

#include "Shalltear/Editor/BuildPipeline.h"
#include "Shalltear/Editor/BuildProfile.h"
#include "Shalltear/Editor/BuildTarget.h"
#include "Shalltear/Editor/ProjectOps.h"
#include "Shalltear/Editor/ScriptProjectOps.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <thread>

namespace Shalltear {

	namespace {

		namespace fs = std::filesystem;

		ModuleHandle _gameModule;

		// must match EntryPoint's probe
		bool MountEngineAssets() {
			std::error_code ec;
			fs::path probe = fs::current_path(ec);
			for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
				fs::path candidate = probe / "EngineAssets";
				if (fs::is_directory(candidate, ec)) {
					FileSystem::Mount("engine", Path(candidate.generic_string()));
					return true;
				}
				fs::path parent = probe.parent_path();
				if (parent == probe) {
					break;
				}
				probe = parent;
			}
			return false;
		}

		const char* OutcomeWord(BuildOutcome outcome) {
			switch (outcome) {
			case BuildOutcome::Succeeded:
				return "SUCCEEDED";
			case BuildOutcome::Skipped:
				return "SKIPPED";
			case BuildOutcome::Cancelled:
				return "CANCELED";
			default:
				return "FAILED";
			}
		}

		bool LoadGameModule(const str& projectPath) {
			if (!ScriptProjectOps::HasScriptProject(projectPath)) {
				return true;
			}

			std::string_view configName = ScriptProjectOps::GetActiveConfigName();
			str modulePath = ScriptProjectOps::GetModuleBinaryPath(projectPath, configName);

			str reason;
			if (ScriptProjectOps::IsModuleBinaryOutOfDate(projectPath, configName, &reason)) {
				std::error_code ec;
				bool exists = fs::is_regular_file(fs::path{modulePath}, ec);
				SLT_LOG_FATAL("Build",
							  "this project's C++ {} for {} ({}). "
							  "Compile it (open the project in the editor, or run a compile) and build again",
							  exists ? "is out of date" : "hasn't been compiled", configName, reason);
				return false;
			}

			ModuleHandle module = ModuleSystem::Load(Path(modulePath));
			if (!ModuleSystem::IsLoaded(module)) {
				SLT_LOG_FATAL("Build", "this project's C++ couldn't be loaded from '{}'",
							  modulePath);
				return false;
			}

			_gameModule = module;
			SLT_LOG_INFO("Build", "loaded this project's C++ from '{}'", modulePath);
			return true;
		}

	} // anonymous namespace

	const void* GetBatchModuleBase() {
		return ModuleSystem::IsLoaded(_gameModule) ? ModuleSystem::GetNativeHandle(_gameModule) : nullptr;
	}

	BatchBuildOptions ParseBatchBuildOptions(int argc, char* argv[]) {
		BatchBuildOptions options;
		for (int i = 1; i < argc; ++i) {
			if (std::strcmp(argv[i], "--build") == 0 && i + 1 < argc) {
				options.profiles.push_back(argv[++i]);
				options.requested = true;
			} else if (std::strcmp(argv[i], "--build-all") == 0) {
				options.buildAll = true;
				options.requested = true;
			} else if (std::strcmp(argv[i], "--list-profiles") == 0) {
				options.listProfiles = true;
				options.requested = true;
			} else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
				options.outputOverride = argv[++i];
			} else if (std::strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
				options.projectPath = argv[++i];
			}
		}
		return options;
	}

	static int RunBatchBuildInner(const BatchBuildOptions& options);

	int RunBatchBuild(const BatchBuildOptions& options) {
		JobSystem::Initialize();
		LogInitConfig logConfig;
		logConfig.createConsole = true;
		LogManager::Initialize(logConfig);

		int exitCode = RunBatchBuildInner(options);

		BuildStepRegistry::Shutdown();
		BuildTargetRegistry::Shutdown();

		SceneSystem::Terminate();
		ModuleSystem::Terminate();
		Settings::Terminate();
		AssetDatabase::Terminate();
		FileSystem::Unmount("project");
		FileSystem::Unmount("engine");

		MemoryManager::LogReport();
		LogManager::Terminate();
		JobSystem::Terminate();
		return exitCode;
	}

	static int RunBatchBuildInner(const BatchBuildOptions& options) {
		int exitCode = 0;
		str projectPath = ProjectOps::NormalizePath(options.projectPath);
		if (projectPath.empty() || !fs::is_regular_file(fs::path{projectPath} / "Project.ini")) {
			SLT_LOG_FATAL("Build", "--project must point at a Shalltear project (a directory holding Project.ini). '{}' doesn't",
						  options.projectPath);
			return 2;
		}
		if (!MountEngineAssets()) {
			SLT_LOG_WARN("Build", "EngineAssets wasn't found (Engine:// is unmounted)");
		}

		FileSystem::Mount("project", Path(projectPath));

		Settings::Initialize(Settings::GetDefaultPaths());

		AssetDatabase::Initialize();
		SceneSystem::Initialize();
		SceneSystem::SetPlaying(false);

		ModuleSystem::Initialize();
		PackageHost packages;
		packages.Initialize();

		Vector<str> roster{GetMemoryResource(MemTag::Editor)};
		BuildProfileOps::ListProfiles(projectPath, roster);

		if (options.listProfiles) {
			SLT_LOG_INFO("Build", "{} profile(s) in {}", roster.size(), projectPath);
			Vector<BuildSettingDesc> coreSchema{GetMemoryResource(MemTag::Editor)};
			BuildProfileOps::GetCoreSettings(coreSchema);
			for (const str& name : roster) {
				ResolvedProfile resolved;
				BuildProfileOps::ResolveProfile(projectPath, name, coreSchema, resolved);
				str targetId = resolved.target.empty() ? BuildTargetRegistry::GetDefaultTargetId() : resolved.target;
				const IBuildTarget* target = BuildTargetRegistry::Find(targetId);
				SLT_LOG_INFO("Build", "  {} [{}]{}", name, targetId,
							 target ? "" : " (not registered on this machine)");
			}
		}

		BuildRequest request;
		request.projectPath = projectPath;
		request.outputOverride = options.outputOverride.empty() ? str{}
																: ProjectOps::NormalizePath(options.outputOverride);
		if (options.buildAll) {
			Vector<str> included{GetMemoryResource(MemTag::Editor)};
			BuildProfileOps::ListBuildAllProfiles(projectPath, included);
			for (const str& name : included) {
				request.profiles.push_back(name);
			}
			if (included.size() < roster.size()) {
				SLT_LOG_INFO("Build", "--build-all: {} of {} profile(s) (rest set includeInBuildAll = false)",
							 included.size(), roster.size());
			}
		}
		for (const str& name : options.profiles) {
			bool alreadyQueued = false;
			for (const str& queued : request.profiles) {
				alreadyQueued = alreadyQueued || queued == name;
			}
			if (!alreadyQueued) {
				request.profiles.push_back(name);
			}
		}

		if (!request.profiles.empty()) {
			SLT_LOG_INFO("Build", "batch mode: building what is on disk (unsaved editor work isn't included)");

			if (!LoadGameModule(projectPath)) {
				return 2;
			}

			BuildPipeline pipeline;
			str refusal;
			if (!pipeline.Request(request, refusal)) {
				SLT_LOG_FATAL("Build", "{}", refusal);
				exitCode = 2;
			} else {
				while (pipeline.IsBusy()) {
					pipeline.PumpMainThread();
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}

				const BuildResult& result = pipeline.GetResult();
				SLT_LOG_INFO("Build", "======== {} profile(s) ========", result.profiles.size());
				for (const BuildProfileResult& entry : result.profiles) {
					str suffix = entry.reason.empty() ? str{} : " (" + entry.reason + ")";
					SLT_LOG_INFO("Build", "  {:<24} {:<10} {:>8.1f}s  {}{}", entry.profile,
								 OutcomeWord(entry.outcome), entry.durationSeconds, entry.outputPath, suffix);
				}
				exitCode = result.FailureCount() == 0 ? 0 : 1;
			}
		} else if (!options.listProfiles) {
			SLT_LOG_FATAL("Build", "nothing to build: no --build profile and --build-all found none in {}",
						  projectPath);
			exitCode = 2;
		}

		return exitCode;
	}

} // namespace Shalltear
