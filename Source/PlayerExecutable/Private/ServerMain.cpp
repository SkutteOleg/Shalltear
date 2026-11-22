#include "BootManifest.h"
#include "LinkedPackages.h"

#include "Game/BuildStamp.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Frame/FrameStages.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Module/ModuleThunkRegistry.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/Settings.h"
#include "Shalltear/Core/Time.h"
#include "Shalltear/Core/Window/FrameLoop.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>

namespace Shalltear {

	namespace {

		struct ServerOptions {
			i32 frames = -1;
			bool thirdPartyNotices = false;
		};

		ServerOptions ParseOptions(int argc, char* argv[]) {
			ServerOptions options;
			for (int i = 1; i < argc; ++i) {
				if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
					long parsed = std::strtol(argv[++i], nullptr, 10);
					options.frames = parsed > 0 ? static_cast<i32>(parsed) : 0;
				} else if (std::strcmp(argv[i], "--third-party-notices") == 0) {
					options.thirdPartyNotices = true;
				}
			}
			return options;
		}

		str GetExecutableDirectory() {
			return FileSystem::GetExecutablePath().GetDirectory();
		}

	} // anonymous namespace

	static int RunSimulation(const ServerOptions& options) {
		SettingsPaths paths = Settings::GetDefaultPaths();
		paths.user = {};
		paths.userProject = {};
		UsePackagedProjectSettings(paths);
		Settings::Initialize(paths);

		SceneSystem::Initialize();
		ModuleThunkRegistry::RegisterPendingThunkTables();
		InitializeLinkedPackages();
		RegisterGameCoreExtensions();

		BootManifest manifest;
		str error;
		if (!ReadBootManifest(manifest, error)) {
			SLT_LOG_FATAL("Server", "{}", error);
			return 1;
		}

		SLT_LOG_INFO("Server", "{} {} (build {}, {}), {} scene(s), headless",
					 manifest.productName.empty() ? "Shalltear game" : manifest.productName,
					 BuildStamp::Version, BuildStamp::Number,
					 BuildStamp::TimeIso.empty() ? std::string_view("no build time") : BuildStamp::TimeIso,
					 manifest.scenes.size());

		if (manifest.scenes.empty()) {
			SLT_LOG_WARN("Server", "this build stages no scenes (server will tick nothing)");
		} else {
			str what;
			if (!LoadBootScene(manifest.scenes[0], what)) {
				SLT_LOG_FATAL("Server", "the boot scene didn't load: {}", what);
				return 1;
			}
		}

		FrameTimer frameTimer;
		FixedTimestep fixed(Time::GetFrame().fixedDeltaTime, Time::GetFrame().maximumDeltaTime);
		i32 framesRemaining = options.frames;
		while (framesRemaining != 0) {
			frameTimer.Tick();
			Time::BeginFrame(frameTimer.GetUnscaledDeltaTime());

			SceneSystem::BeginFrame();

			const FrameTime& frame = Time::GetFrame();
			fixed.Configure(frame.fixedDeltaTime, frame.maximumDeltaTime);

			const f32 gameDeltaSeconds = frame.ComputeGameDelta();

			u32 steps = fixed.Advance(gameDeltaSeconds);
			const FrameStageArgs stageArgs;

			for (FrameStage stage : FRAME_STAGES) {
				switch (stage) {
				case FrameStage::FixedUpdate:
					for (u32 i = 0; i < steps; ++i) {
						FrameStages::Run(stage, stageArgs);
					}
					break;
				case FrameStage::Update:
				case FrameStage::LateUpdate:
				case FrameStage::EndFrame:
					FrameStages::Run(stage, stageArgs);
					break;
				case FrameStage::UIUpdate:
				case FrameStage::Render:
					break;
				}
			}

			if (framesRemaining > 0) {
				--framesRemaining;
			}
			std::this_thread::sleep_for(std::chrono::duration<f64>(fixed.GetStepSeconds()));
		}
		SLT_LOG_INFO("Server", "server exited after {} tick(s)", Time::GetFrameCount());
		return 0;
	}

	static int RunServer(int argc, char* argv[]) {
		ServerOptions options = ParseOptions(argc, argv);

		JobSystem::Initialize();
		LogInitConfig logConfig;
		if (options.thirdPartyNotices) {
			logConfig.enableConsoleSink = false;
			logConfig.enableFileSink = false;
			logConfig.createConsole = false;
		}
		LogManager::Initialize(logConfig);
		TypeRegistry::InitializePrimitives();

		str root = GetExecutableDirectory();
		if (root.empty()) {
			SLT_LOG_FATAL("Server", "couldn't resolve this exe's directory");
			LogManager::Terminate();
			JobSystem::Terminate();
			return 1;
		}

		FileSystem::Mount("project", Path(root));
		SLT_LOG_INFO("Server", "project:// -> {}", root);

		int exitCode = 0;
		try {
			AssetDatabase::Initialize();

			str mountError;
			if (!MountStagedContent(root, mountError)) {
				SLT_LOG_FATAL("Server", "{}", mountError);
				exitCode = 1;
			} else if (options.thirdPartyNotices) {
				exitCode = PrintThirdPartyNotices(root);
			} else {
				UnpackThirdPartyNotices(root);
				exitCode = RunSimulation(options);
			}
		} catch (const std::exception& e) {
			SLT_LOG_FATAL("Core", "Exception caught: {}", e.what());
			exitCode = -1;
		} catch (...) {
			SLT_LOG_FATAL("Core", "Unknown exception caught");
			exitCode = -1;
		}

		SceneSystem::Terminate();
		Settings::Terminate();
		AssetDatabase::Terminate();
		FileSystem::Unmount("project");
		FileSystem::Unmount("engine");

		MemoryManager::LogReport();

		LogManager::Terminate();
		JobSystem::Terminate();
		return exitCode;
	}

} //namespace Shalltear

int main(int argc, char* argv[]) {
	return Shalltear::RunServer(argc, argv);
}
