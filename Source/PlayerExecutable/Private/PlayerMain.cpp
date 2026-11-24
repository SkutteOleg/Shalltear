#include "BootManifest.h"
#include "LinkedPackages.h"

#include "Game/BuildStamp.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Asset/PakFormat.h"
#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Frame/FrameStages.h"
#include "Shalltear/Core/Graphics/RHIDevice.h"
#include "Shalltear/Core/Graphics/RenderSystem.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Module/ModuleThunkRegistry.h"
#include "Shalltear/Core/Object.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/Screen.h"
#include "Shalltear/Core/Settings.h"
#include "Shalltear/Core/Time.h"
#include "Shalltear/Core/UI/UISystem.h"
#include "Shalltear/Core/Window/FrameLoop.h"
#include "Shalltear/Core/Window/Window.h"
#include "Shalltear/Core/Window/WindowEvents.h"
#include "Shalltear/Core/Window/WindowSystem.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>

namespace Shalltear {

	namespace {

		struct PlayerOptions {
			i32 frames = -1;
			i32 loseDeviceFrame = -1;
			u32 failedRebuilds = 0;
			bool console = false;
			bool thirdPartyNotices = false;
			ScreenLaunch screen;
		};

		PlayerOptions ParseOptions(int argc, char* argv[]) {
			PlayerOptions options;
			auto number = [](const char* text) { return static_cast<i32>(std::strtol(text, nullptr, 10)); };
			for (int i = 1; i < argc; ++i) {
				if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
					long parsed = std::strtol(argv[++i], nullptr, 10);
					options.frames = parsed > 0 ? static_cast<i32>(parsed) : 0;
				} else if (std::strcmp(argv[i], "--console") == 0) {
					options.console = true;
				} else if (std::strcmp(argv[i], "--third-party-notices") == 0) {
					options.thirdPartyNotices = true;
				} else if (std::strcmp(argv[i], "--lose-device") == 0 && i + 1 < argc) {
					options.loseDeviceFrame = number(argv[++i]);
				} else if (std::strcmp(argv[i], "--fail-rebuilds") == 0 && i + 1 < argc) {
					const i32 parsed = number(argv[++i]);
					options.failedRebuilds = parsed > 0 ? static_cast<u32>(parsed) : 0;
				} else if (std::strcmp(argv[i], "--screen-width") == 0 && i + 1 < argc) {
					options.screen.width = number(argv[++i]);
				} else if (std::strcmp(argv[i], "--screen-height") == 0 && i + 1 < argc) {
					options.screen.height = number(argv[++i]);
				} else if (std::strcmp(argv[i], "--screen-fullscreen") == 0 && i + 1 < argc) {
					options.screen.fullScreen = number(argv[++i]) != 0;
				} else if (std::strcmp(argv[i], "--monitor") == 0 && i + 1 < argc) {
					options.screen.monitor = number(argv[++i]);
				}
			}
			return options;
		}

		class PlayerWindowPolicy : public Object {
		public:
			void Subscribe() {
				WindowEvents::CloseRequested.Subscribe(EventBus::GetGlobal(), Handle<PlayerWindowPolicy>(this),
													   &PlayerWindowPolicy::OnCloseRequested);
			}
			void Unsubscribe() { EventBus::GetGlobal().UnsubscribeAll(GetInstanceId()); }

			void OnCloseRequested(const WindowCloseRequestedEvent& event) {
				SLT_LOG_INFO("Player", "close requested. Shutting down");
				WindowSystem::RequestDestroy(event.window);
			}
		};

		struct PlayerSession {
			PlayerOptions options;
			BootManifest manifest;
			Handle<Window> window;
			std::optional<PlayerWindowPolicy> quitPolicy;
			std::optional<FramePacer> pacer;
			std::optional<FixedTimestep> fixed;
			i32 framesRemaining = -1;
			i32 frameIndex = 0;
			bool running = false;
			int exitCode = 0;
		};

		int _lastExitCode = 0;

		str GetExecutableDirectory() {
			return FileSystem::GetExecutablePath().GetDirectory();
		}

		void ApplyPackagedIcon(Window& window) {
			str document;
			if (!AssetDatabase::ReadPackageDocument(PakDocumentKind::Icon, document)) {
				return;
			}
			const u8* bytes = reinterpret_cast<const u8*>(document.data());
			Vector<WindowIconImage> images{GetMemoryResource(MemTag::Core)};
			size_t at = 0;
			while (at < document.size()) {
				PakIconImage header{};
				if (document.size() - at < sizeof(header)) {
					break;
				}
				std::memcpy(&header, bytes + at, sizeof(header));
				at += sizeof(header);
				const u64 texels = static_cast<u64>(header.width) * header.height;
				if (header.width == 0 || header.height == 0 || texels > (document.size() - at) / 4) {
					break;
				}
				const size_t texelBytes = static_cast<size_t>(texels) * 4;
				images.push_back(WindowIconImage{Vec2I(static_cast<i32>(header.width), static_cast<i32>(header.height)),
												 std::span<const u8>(bytes + at, texelBytes)});
				at += texelBytes;
			}
			if (at != document.size()) {
				SLT_LOG_WARN("Player", "package's icon is malformed (default icon used)");
				return;
			}
			window.SetIcon(images);
		}

		bool StepPlayerFrame(void* user) {
			auto* session = static_cast<PlayerSession*>(user);
			if (!session || WindowSystem::GetWindowCount() == 0 || session->framesRemaining == 0) {
				return false;
			}

			try {
				if (!session->pacer->BeginFrame()) {
					return WindowSystem::GetWindowCount() > 0 && session->framesRemaining != 0;
				}
				if (session->frameIndex++ == session->options.loseDeviceFrame && RenderSystem::IsRenderingAvailable()) {
					RenderSystem::GetDevice()->SimulateLoss(session->options.failedRebuilds);
				}
				Time::BeginFrame(session->pacer->GetUnscaledDeltaTime());

				SceneSystem::BeginFrame();

				const FrameTime& frame = Time::GetFrame();
				session->fixed->Configure(frame.fixedDeltaTime, frame.maximumDeltaTime);

				// not Time::GetDeltaTime (clock advances during the walk)
				const f32 gameDeltaSeconds = frame.ComputeGameDelta();

				const u32 steps = session->fixed->Advance(gameDeltaSeconds);
				FrameStageArgs stageArgs;

				for (FrameStage stage : FRAME_STAGES) {
					switch (stage) {
					case FrameStage::FixedUpdate:
						for (u32 i = 0; i < steps; ++i) {
							FrameStages::Run(stage, stageArgs);
						}
						break;
					case FrameStage::Update:
					case FrameStage::UIUpdate:
					case FrameStage::LateUpdate:
					case FrameStage::EndFrame:
						FrameStages::Run(stage, stageArgs);
						break;
					case FrameStage::Render:
						AssetDatabase::CollectGarbage();
						FrameStages::Run(stage, stageArgs);
						break;
					}
				}

				if (session->framesRemaining > 0 && --session->framesRemaining == 0) {
					SLT_LOG_INFO("Player", "--frames run complete after {} frame(s). Closing",
								 Time::GetFrameCount());
					WindowSystem::RequestDestroy(session->window);
				}
			} catch (const std::exception& e) {
				SLT_LOG_FATAL("Core", "Exception caught: {}", e.what());
				session->exitCode = -1;
				return false;
			} catch (...) {
				SLT_LOG_FATAL("Core", "Unknown exception caught");
				session->exitCode = -1;
				return false;
			}

			return WindowSystem::GetWindowCount() > 0 && session->framesRemaining != 0;
		}

		void ShutdownPlayerSession(int code, void* user) {
			auto* session = static_cast<PlayerSession*>(user);
			if (!session) {
				return;
			}
			if (session->exitCode == 0 && code != 0) {
				session->exitCode = code;
			}
			if (session->quitPolicy) {
				session->quitPolicy->Unsubscribe();
				session->quitPolicy.reset();
			}
			if (session->running) {
				SLT_LOG_INFO("Player", "player exited after {} frame(s)", Time::GetFrameCount());
			}
			session->manifest = {};

			// not the reverse of init
			SceneSystem::Terminate();
			Settings::Terminate();
			UISystem::Terminate();
			AssetDatabase::Terminate();
			RenderSystem::Terminate();
			WindowSystem::Terminate();
			FileSystem::Unmount("project");
			FileSystem::Unmount("engine");

			// MemTag::Editor must be 0 in a player
			MemoryManager::LogReport();

			LogManager::Terminate();
			JobSystem::Terminate();

			_lastExitCode = session->exitCode;
			delete session;
		}

		bool InitializeGame(PlayerSession& session) {
			str error;
			if (!ReadBootManifest(session.manifest, error)) {
				SLT_LOG_FATAL("Player", "{}", error);
				session.exitCode = 1;
				return false;
			}

			SettingsPaths paths = Settings::GetDefaultPaths();
			paths.user = {};
			paths.userProject = {};
			UsePackagedProjectSettings(paths);
			const Path userData = FileSystem::GetUserDataDirectory();
			if (!session.manifest.productName.empty() && !userData.ToString().empty()) {
				paths.userProject = userData / session.manifest.productName / "Settings.ini";
			} else {
				SLT_LOG_WARN("Player", "this build has no product name (player preferences can't be saved)");
			}
			Settings::Initialize(paths);

			WindowSystem::Initialize();
			RenderSystem::Initialize();
			UISystem::Initialize();
			SceneSystem::Initialize();

			InitializeLinkedPackages();
			RegisterGameCoreExtensions();

			SLT_LOG_INFO("Player", "{} {} (build {}, {}), {} scene(s)",
						 session.manifest.productName.empty() ? "Shalltear game" : session.manifest.productName,
						 BuildStamp::Version, BuildStamp::Number,
						 BuildStamp::TimeIso.empty() ? std::string_view("no build time") : BuildStamp::TimeIso,
						 session.manifest.scenes.size());

			ScreenLaunch launch = session.options.screen;
			launch.title = session.manifest.productName.empty() ? str("Shalltear") : session.manifest.productName;
			session.window = Screen::CreateGameWindow(launch);
			if (!session.window.IsValid()) {
				SLT_LOG_FATAL("Player", "main window creation failed");
				session.exitCode = 1;
				return false;
			}
			ApplyPackagedIcon(*session.window.Get());

			session.quitPolicy.emplace();
			session.quitPolicy->Subscribe();

			if (session.manifest.scenes.empty()) {
				SLT_LOG_WARN("Player", "this build stages no scenes (window will be empty)");
			} else {
				str what;
				if (!LoadBootScene(session.manifest.scenes[0], what)) {
					SLT_LOG_FATAL("Player", "the boot scene didn't load: {}", what);
					session.exitCode = 1;
					return false;
				}
			}

			session.pacer.emplace(session.options.frames > 0);
			session.fixed.emplace(Time::GetFrame().fixedDeltaTime, Time::GetFrame().maximumDeltaTime);
			session.framesRemaining = session.options.frames;
			session.frameIndex = 0;
			session.running = true;
			return true;
		}

	} // anonymous namespace

	static int RunPlayer(int argc, char* argv[]) {
		_lastExitCode = 0;
		PlayerOptions options = ParseOptions(argc, argv);

		JobSystem::Initialize();
		LogInitConfig logConfig;
		if (options.thirdPartyNotices) {
			logConfig.enableConsoleSink = false;
			logConfig.enableFileSink = false;
			logConfig.createConsole = false;
		} else {
			logConfig.createConsole = options.console;
		}
		LogManager::Initialize(logConfig);
		TypeRegistry::InitializePrimitives();
		ModuleThunkRegistry::RegisterPendingThunkTables();

		str root = GetExecutableDirectory();
		if (root.empty()) {
			SLT_LOG_FATAL("Player", "couldn't resolve this exe's directory");
			LogManager::Terminate();
			JobSystem::Terminate();
			return 1;
		}

		FileSystem::Mount("project", Path(root));
		SLT_LOG_INFO("Player", "project:// -> {}", root);

		auto* session = new PlayerSession();
		session->options = options;

		bool readyToRun = false;
		try {
			AssetDatabase::Initialize();

			str mountError;
			if (!MountStagedContent(root, mountError)) {
				SLT_LOG_FATAL("Player", "{}", mountError);
				session->exitCode = 1;
			} else if (options.thirdPartyNotices) {
				session->exitCode = PrintThirdPartyNotices(root);
			} else {
				UnpackThirdPartyNotices(root);
				readyToRun = InitializeGame(*session);
			}
		} catch (const std::exception& e) {
			SLT_LOG_FATAL("Core", "Exception caught: {}", e.what());
			session->exitCode = -1;
		} catch (...) {
			SLT_LOG_FATAL("Core", "Unknown exception caught");
			session->exitCode = -1;
		}

		if (!readyToRun) {
			ShutdownPlayerSession(session->exitCode, session);
			return _lastExitCode;
		}

		RunMainLoop(&StepPlayerFrame, &ShutdownPlayerSession, session);
		return _lastExitCode;
	}

} // namespace Shalltear

int main(int argc, char* argv[]) {
	return Shalltear::RunPlayer(argc, argv);
}
