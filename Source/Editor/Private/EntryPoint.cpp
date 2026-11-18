#include "Shalltear/Editor/EntryPoint.h"

#include "Shalltear/Core/Asset.h"
#include "Shalltear/Core/Asset/ImportTier.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Graphics.h"
#include "Shalltear/Core/Graphics/ShaderCompileQueue.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Module.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/Settings.h"
#include "Shalltear/Core/UI.h"
#include "Shalltear/Core/Window.h"

#include "Shalltear/Editor/BuildPipeline.h"
#include "Shalltear/Editor/BuildTarget.h"
#include "Shalltear/Editor/AuthoringDefaults.h"
#include "Shalltear/Editor/ComponentMenu.h"
#include "Shalltear/Editor/EditorFonts.h"
#include "Shalltear/Editor/EditorIcons.h"
#include "Shalltear/Editor/EditorTheme.h"
#include "Shalltear/Editor/SettingsOps.h"

#include "BatchBuild.h"
#include "Shalltear/Editor/ChildProcess.h"
#include "EditorApplication.h"
#include "EditorGrid.h"

#include <cstring>
#include <exception>
#include <filesystem>

namespace Shalltear {

	namespace {

		void MountEngineAssets() {
			namespace fs = std::filesystem;
			std::error_code ec;
			fs::path probe = fs::current_path(ec);
			for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
				fs::path candidate = probe / "EngineAssets";
				if (fs::is_directory(candidate, ec)) {
					FileSystem::Mount("engine", Path(candidate.generic_string()));
					SLT_LOG_INFO("Editor", "engine:// -> {}", candidate.generic_string());
					return;
				}
				fs::path parent = probe.parent_path();
				if (parent == probe) {
					break;
				}
				probe = parent;
			}
			SLT_LOG_WARN("Editor", "EngineAssets directory not found. Engine:// is unmounted (built-in shaders unavailable)");
		}

		bool WantsConsole(int argc, char* argv[]) {
			for (int i = 1; i < argc; ++i) {
				if (std::strcmp(argv[i], "--console") == 0) {
					return true;
				}
			}
			return false;
		}

	} // anonymous namespace

	int Run(int argc, char* argv[]) {
		// before either path's 1st import, never in a player
		Shalltear::ImportTier::Install();

		BatchBuildOptions batch = ParseBatchBuildOptions(argc, argv);
		if (batch.requested) {
			return RunBatchBuild(batch);
		}

		Shalltear::JobSystem::Initialize();
		Shalltear::LogInitConfig logConfig;
		logConfig.createConsole = WantsConsole(argc, argv);
		Shalltear::LogManager::Initialize(logConfig);
		Shalltear::TypeRegistry::InitializePrimitives();
		Shalltear::MountEngineAssets();
		Shalltear::AssetDatabase::Initialize();
		Shalltear::WindowSystem::Initialize();

		int exitCode = 0;
		str relaunchCommandLine;
		try {
			EditorApplication app(argc, argv);
			app.MountRequestedProject();

			Shalltear::SettingsPaths settingsPaths = Shalltear::Settings::GetDefaultPaths();
			Shalltear::EditorApplication::MigrateLegacySettingsFiles(settingsPaths);
			Shalltear::Settings::Initialize(settingsPaths);

			Shalltear::RenderSystem::Initialize();
			Shalltear::UISystem::Initialize();
			Shalltear::UISystem::SetContextInitializer(
				[](Shalltear::UI::Context& context) { Shalltear::EditorTheme::ApplyToContext(context); });
			Shalltear::SceneSystem::Initialize();
			Shalltear::SceneSystem::SetPlaying(false);
			Shalltear::ModuleSystem::Initialize();

			if (app.Initialize()) {
				app.ArmShaderHotReload();
				app.Run();
				app.Terminate();
			}
			relaunchCommandLine = app.GetRelaunchCommandLine();
		} catch (const std::exception& e) {
			SLT_LOG_FATAL("Core", "Exception caught: {}", e.what());
			exitCode = -1;
		} catch (...) {
			SLT_LOG_FATAL("Core", "Unknown exception caught");
			exitCode = -1;
		}

		Shalltear::SceneSystem::Terminate();
		Shalltear::ModuleSystem::Terminate();
		// after the modules (registry must outlive their declarations)
		Shalltear::Settings::Terminate();
		Shalltear::EditorIcons::Shutdown();
		Shalltear::ComponentMenu::Shutdown();
		Shalltear::AuthoringDefaults::Shutdown();
		Shalltear::EditorFonts::Shutdown();
		Shalltear::EditorGridPass::Shutdown();
		Shalltear::ShaderCompileQueue::Terminate();
		Shalltear::UISystem::Terminate();
		Shalltear::AssetDatabase::Terminate();
		Shalltear::RenderSystem::Terminate();
		Shalltear::WindowSystem::Terminate();
		Shalltear::FileSystem::Unmount("engine");

		Shalltear::EditorTheme::Shutdown();

		Shalltear::BuildStepRegistry::Shutdown();
		Shalltear::BuildTargetRegistry::Shutdown();

		Shalltear::MemoryManager::LogReport();

		if (!relaunchCommandLine.empty()) {
			SLT_LOG_INFO("Editor", "restarting: {}", relaunchCommandLine);
			Shalltear::StartDetachedProcess(relaunchCommandLine, {});
		}

		Shalltear::LogManager::Terminate();
		Shalltear::JobSystem::Terminate();
		return exitCode;
	}

} //namespace Shalltear
