#include "EditorApplication.h"

#include "Shalltear/Core/Assert.h"
#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/Graphics/RHIDevice.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/UI/Font.h"

#include "Shalltear/Editor/AssetBrowserOps.h"
#include "Shalltear/Editor/AssetOpeners.h"
#include "Shalltear/Editor/MaterialInspectorOps.h"
#include "Shalltear/Editor/CompileStatus.h"
#include "Shalltear/Editor/ComponentDrops.h"
#include "Shalltear/Editor/EditorAssetOps.h"
#include "Shalltear/Core/Localization.h"
#include "Shalltear/Editor/EditorAssetWatch.h"
#include "Shalltear/Editor/EditorDialogs.h"
#include "Shalltear/Editor/EditorChromeOps.h"
#include "Shalltear/Editor/EditorConsole.h"
#include "Shalltear/Editor/EditorDocuments.h"
#include "Shalltear/Editor/EditorLayoutOps.h"
#include "Shalltear/Editor/EditorMenus.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/EditorViewportOps.h"
#include "Shalltear/Editor/SceneAnnotations.h"
#include "Shalltear/Editor/EditorSelection.h"
#include "Shalltear/Editor/PanelManager.h"
#include "Shalltear/Editor/ProjectHub.h"
#include "Shalltear/Editor/ProjectOps.h"
#include "Shalltear/Editor/EditorGizmos.h"
#include "Shalltear/Editor/EditorPreviews.h"
#include "Shalltear/Editor/EditorVariantCompiles.h"
#include "Shalltear/Editor/PropertyDrawers.h"
#include "Shalltear/Editor/ReflectionProbeBake.h"
#include "Shalltear/Editor/SettingsOps.h"
#include "Shalltear/Editor/ProfilerModules.h"
#include "Shalltear/Editor/RuntimeViewToolbar.h"
#include "Shalltear/Editor/SceneAssetDrops.h"
#include "Shalltear/Editor/SceneViewEffects.h"
#include "Shalltear/Editor/SceneViewOverlays.h"
#include "Shalltear/Editor/SettingsPresentation.h"
#include "Shalltear/Editor/ScriptProjectOps.h"
#include "Shalltear/Editor/UIBuilderOps.h"
#include "Shalltear/Editor/UIDocumentOps.h"
#include "Shalltear/Editor/UndoSystem.h"

#include "BuildFlows.h"
#include "Panels/BuildProfilesPanel.h"
#include "Panels/InspectorPanel.h"
#include "Panels/RuntimeViewPanel.h"
#include "Panels/SceneViewPanel.h"
#include "SceneFileFlows.h"
#include "UIBuilderCommands.h"

#include <filesystem>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <format>

namespace Shalltear {

	namespace {

		f64 ParseEpochSeconds(int argc, char* argv[]) {
			for (int i = 1; i + 1 < argc; ++i) {
				if (std::strcmp(argv[i], "--frame-cost-epoch") == 0) {
					f64 seconds = std::strtod(argv[i + 1], nullptr);
					return seconds > 0.0 ? seconds : EditorFrameCost::DEFAULT_EPOCH_SECONDS;
				}
			}
			return EditorFrameCost::DEFAULT_EPOCH_SECONDS;
		}

		constexpr i32 DEFAULT_SMOKE_FRAMES = 240;

		i32 ParseSmokeFrames(int argc, char* argv[]) {
			for (int i = 1; i + 1 < argc; ++i) {
				if (std::strcmp(argv[i], "--smoke-frames") == 0) {
					i32 frames = std::atoi(argv[i + 1]);
					return frames > 0 ? frames : DEFAULT_SMOKE_FRAMES;
				}
			}
			return DEFAULT_SMOKE_FRAMES;
		}

		void CloseFloatingDockWindow(Handle<Window> window) {
			UI::DockManager* dockManager = UISystem::GetDockManager(window);
			if (!dockManager) {
				return;
			}
			Vector<StringHash> docked{GetMemoryResource(MemTag::Editor)};
			dockManager->GetDockedPanels(docked);
			for (StringHash panelId : docked) {
				PanelManager::CloseByDockId(panelId);
			}
		}

		UI::ElementHandle ChromeFloatingDockWindow(Handle<Window> window) {
			UI::Context* context = UISystem::GetContext(window);
			UI::DockManager* dockManager = UISystem::GetDockManager(window);
			if (!context || !dockManager) {
				SLT_LOG_WARN("Editor", "floating dock window has no UI to chrome");
				return {};
			}

			dockManager->SetTabBarClass(EditorChromeOps::CaptionStripClass);

			EditorChromeOps::FloatingChromeParts parts = EditorChromeOps::BuildFloatingChrome(
				*context, window, [window]() { CloseFloatingDockWindow(window); });
			UISystem::SetDockHost(window, parts.dockHost);

			if (Window* target = window.Get()) {
				target->SetHitTestCallback(&EditorChromeOps::ChromeHitTest, nullptr);
			}
			return parts.maximizeFace;
		}

	} // namespace

	EditorApplication::EditorApplication(int argc, char* argv[]) : _frameCost(Time::GetTicksPerSecond(), ParseEpochSeconds(argc, argv)) {
		for (int i = 1; i < argc; ++i) {
			_arguments.push_back(argv[i]);
		}
		for (int i = 1; i < argc; ++i) {
			if (std::strcmp(argv[i], "--smoke") == 0) {
				_smokeFramesRemaining = ParseSmokeFrames(argc, argv);
			}
			if (std::strcmp(argv[i], "--module") == 0 && i + 1 < argc) {
				_modulePath = argv[++i];
			}
			if (std::strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
				_projectPath = argv[++i];
			}
			if (std::strcmp(argv[i], "--frame-cost") == 0) {
				_frameCostEnabled = true;
			}
			if (std::strcmp(argv[i], "--zones") == 0) {
				_zoneDumpEnabled = true;
			}
			if (std::strcmp(argv[i], "--no-throttle") == 0) {
				_throttleWhenUnfocused = false;
			}
			if (std::strcmp(argv[i], "--lose-device") == 0 && i + 1 < argc) {
				_loseDeviceFrame = std::atoi(argv[++i]);
			}
			if (std::strcmp(argv[i], "--fail-rebuilds") == 0 && i + 1 < argc) {
				const i32 parsed = std::atoi(argv[++i]);
				_failedRebuilds = parsed > 0 ? static_cast<u32>(parsed) : 0;
			}
		}
	}

	void EditorApplication::AdoptProject(const str& path) {
		_projectPath = ProjectOps::NormalizePath(path);
		ProjectOps::TouchProject(ProjectOps::GetDefaultRegistryPath(), _projectPath);
		u64 created = 0;
		ProjectOps::ReadProjectFile(_projectPath, _projectName, created);
		if (_projectName.empty()) {
			_projectName = Path(_projectPath).GetFilename();
		}
		_projectMounted = true;
		SLT_LOG_INFO("Editor", "project '{}' mounted from {}", _projectName, _projectPath);
	}

	bool EditorApplication::MountRequestedProject() {
		if (_projectMounted || _projectPath.empty()) {
			return _projectMounted;
		}
		if (!ProjectOps::MountProject(_projectPath)) {
			_projectPath.clear();
			return false;
		}
		AdoptProject(_projectPath);
		return true;
	}

	void EditorApplication::MigrateLegacySettingsFiles(const SettingsPaths& paths) {
		SettingsOps::MigrateLegacyEngineIni(FileSystem::Resolve(Path("project://Engine.ini")), paths.project);
	}

	bool EditorApplication::ResolveProject() {
		bool projectLess = false;
		if (!MountRequestedProject()) {
			if (_smokeFramesRemaining > 0) {
				projectLess = true;
			} else {
				str chosen = ProjectHub::RunModal();
				if (chosen.empty()) {
					SLT_LOG_INFO("Editor", "no project selected. Exiting");
					return false;
				}
				if (!ProjectOps::MountProject(chosen)) {
					return false;
				}
				AdoptProject(chosen);

				SettingsPaths paths = Settings::GetDefaultPaths();
				MigrateLegacySettingsFiles(paths);
				Settings::Initialize(paths);
			}
		}

		// packages before the scan (importers prevent typeless sidecars)
		_packageHost.Initialize();

		if (projectLess) {
			return true;
		}

		// after SceneSystem::Initialize (sidecars get their type)
		AssetDatabase::ScanProjectDirectory();

		SettingsOps::ListProjectQualityProfiles();
		return true;
	}

	bool EditorApplication::Initialize() {
		if (!ResolveProject()) {
			return false;
		}

		_presentPolicy.SetMode(PresentMode());
		_presentPolicy.Install();

		WindowDesc desc;
		desc.title = _projectName.empty() ? str("Shalltear Editor")
										  : "Shalltear Editor - " + _projectName;
		desc.clientSize = {1280, 720};
		desc.flags = WindowFlags::Resizable | WindowFlags::AcceptFileDrops |
					 WindowFlags::ScaleToMonitor | WindowFlags::Borderless;
		_mainWindow = WindowSystem::Create(desc);
		SLT_APP_ASSERT_MSG(_mainWindow.IsValid(), "Main window creation failed");
		_mainWindowId = _mainWindow.GetInstanceId();

		if (Window* mainWindow = _mainWindow.Get()) {
			mainWindow->SetHitTestCallback(&EditorChromeOps::ChromeHitTest, nullptr);
		}

		UISystem::FloatingDockWindowPolicy floating;
		floating.extraFlags = WindowFlags::Borderless;
		floating.onCreated = [this](Handle<Window> window) {
			UI::ElementHandle face = ChromeFloatingDockWindow(window);
			if (face.IsValid()) {
				_floatingChrome.push_back({window, face});
			}
		};
		UISystem::SetFloatingDockWindowPolicy(std::move(floating));

		EventBus& bus = EventBus::GetGlobal();
		Handle<EditorApplication> self(this);
		WindowEvents::CloseRequested.Subscribe(bus, self, &EditorApplication::OnCloseRequested);
		WindowEvents::Destroyed.Subscribe(bus, self, &EditorApplication::OnWindowDestroyed);
		WindowEvents::Resized.Subscribe(bus, self, &EditorApplication::OnWindowResized);
		WindowEvents::StateChanged.Subscribe(bus, self, &EditorApplication::OnWindowStateChanged);
		WindowEvents::FilesDropped.Subscribe(bus, self, &EditorApplication::OnFilesDropped);
		if (_frameCostEnabled) {
			WindowEvents::DragBegan.Subscribe(bus, self, &EditorApplication::OnFrameCostDragBegan);
			WindowEvents::DragMoved.Subscribe(bus, self, &EditorApplication::OnFrameCostDragMoved);
			WindowEvents::DragEnded.Subscribe(bus, self, &EditorApplication::OnFrameCostDragEnded);
		}

		WindowSystem::SetModalTickCallback(
			[](void* user) { static_cast<EditorApplication*>(user)->RunFrame(); }, this);

		_shortcuts.Build();

		EditorConsole::Initialize();
		EditorSceneOps::Initialize();
		EditorLayoutOps::InitializeLayoutStore(FileSystem::GetUserDataDirectory().ToString().empty()
												   ? Path()
												   : FileSystem::GetUserDataDirectory() / "Shalltear" / "Layouts");
		EditorAssetOps::Initialize();
		SceneAnnotations::Initialize();
		PropertyDrawers::Initialize();
		EditorAssetWatch::Initialize();
		EditorPreviews::Initialize();
		EditorPreviewStore::Initialize();
		ReflectionProbeBake::Initialize();
		EditorGizmos::Initialize();
		EditorSelection::Initialize();
		UndoSystem::Initialize();
		PanelManager::Initialize();
		PanelManager::SetDefaultWindow(_mainWindow);
		RegisterBuiltInPanels();
		EditorIcons::InstallWidgetMarks();
		SettingsPresentation::RegisterBuiltIns();
		EditorConfig::DeclareCategories();

		if (const u32 extended = _packageHost.LoadEditorExtensions(); extended > 0) {
			SLT_LOG_INFO("Editor", "{} package(s) contributed editor extensions", extended);
		}

		_config.LoadSettings();
		_config.MigrateLegacyScriptSettings(_projectPath);
		EditorConfig::ApplyModuleSettings(_modulePath);

		EditorConfig::ScriptPreferences scriptPreferences = _config.ReadScriptPreferences();
		ScriptProjectOps::ScaffoldOptions regenerateOptions;
		regenerateOptions.sdkRoot = ScriptProjectOps::GetSDKRoot();
		regenerateOptions.editorExecutable = ScriptProjectOps::GetEditorExecutablePath();
		regenerateOptions.enabledPackages = _packageHost.GetGamePackageNames();
		if (!_projectPath.empty() && ScriptProjectOps::NeedsProjectFileRegeneration(_projectPath, regenerateOptions)) {
			ScriptProjectOps::ScaffoldResult regenerated;
			ScriptProjectOps::RegenerateProjectFiles(_projectPath, regenerateOptions, regenerated);
		}
		if (_modulePath.empty() && ScriptProjectOps::HasScriptProject(_projectPath)) {
			_modulePath = ScriptProjectOps::GetModuleBinaryPath(_projectPath, ScriptProjectOps::GetActiveConfigName());
		}

		if (_smokeFramesRemaining <= 0) {
			_compileService.RefreshToolchain();
		}

		GameModuleHost::Config moduleConfig;
		moduleConfig.modulePath = _modulePath;
		moduleConfig.projectPath = _projectPath;
		moduleConfig.autoCompile = scriptPreferences.autoCompile;
		moduleConfig.externalEditorCommand = scriptPreferences.externalEditorCommand;
		moduleConfig.loadedPackages = _packageHost.GetLoadedNames();
		moduleConfig.gamePackages = _packageHost.GetGamePackageNames();
		_moduleHost.Initialize(moduleConfig, _smokeFramesRemaining > 0 ? nullptr : &_compileService);

		if (_smokeFramesRemaining > 0) {
			BuildSmokeScene();
		}

		{
			UIScreenDesc nowhere;
			nowhere.target = UIScreenDesc::Target::None;
			UISystem::SetScreen(nowhere);
		}

		if (RenderSystem::IsRenderingAvailable()) {
			RenderSystem::SetSceneCamerasRenderToWindows(false);

			EditorShell::Hooks hooks;
			hooks.onRequestQuit = [this]() { RequestQuit(); };
			hooks.onRequestRestart = [this]() { RequestRestart(); };
			hooks.onSaveLayout = [this]() { _config.SaveNow(_mainWindow); };
			_shell.Initialize(_mainWindow, _projectName, &_moduleHost, &_playMode, &_compileService, std::move(hooks));
			if (_smokeFramesRemaining > 0 || !_config.RestoreLayout(_mainWindow)) {
				EditorLayoutOps::BuildDefaultLayout(_mainWindow);
			}
		}

		RestoreStartupScene();

		SLT_LOG_INFO("Editor", "Editor initialized");
		return true;
	}

	void EditorApplication::RestoreStartupScene() {
		if (_smokeFramesRemaining > 0 || _projectPath.empty()) {
			return;
		}
		if (str died = _config.TakePendingScene(); !died.empty()) {
			SLT_LOG_ERROR("Editor", "the last session didn't survive opening '{}' (starting with an empty scene). Open it from File > Open Scene to retry", died);
			EditorSceneOps::NewScene("Untitled");
			return;
		}

		str chosen = EditorSceneOps::ChooseStartupScene(_projectPath, _config.GetRestoredSceneSetup());
		if (!chosen.empty()) {
			_config.SetPendingScene(chosen);
			Handle<Scene> opened = EditorSceneOps::OpenScene(Path(chosen));
			_config.SetPendingScene({});
			if (opened.IsValid()) {
				return;
			}
			SLT_LOG_ERROR("Editor", "couldn't open '{}' at startup (starting with an empty scene)", chosen);
		}
		EditorSceneOps::NewScene("Untitled");
	}

	void EditorApplication::Terminate() {
		if (_frameCostEnabled) {
			_frameCost.Finish();
			_frameCost.EndModalPeriod(Time::GetTicks());
			ReportFrameCostGestures();
			ReportFrameCostModalPeriods();
			_frameCost.CloseEpoch(SampleFrameCostCounters(), true);
			ReportFrameCostEpochs();
			ReportFrameCostSummary();
		}

		_moduleHost.Terminate();
		_packageHost.Terminate();
		_shell.Terminate();
		PanelManager::Terminate();
		// after the panels (builder holds a document reference)
		CloseSmokeUIBuilder();
		ReleaseSmokeUIDocument();
		EditorDocuments::Terminate();
		AssetOpeners::Terminate();
		EditorMenus::Terminate();
		MaterialInspectorOps::TerminateShaderSources();
		TerminateTextureAssetInspector();
		TerminateFontAssetInspector();
		UndoSystem::Terminate();
		EditorSelection::Terminate();
		SceneAnnotations::Terminate();
		EditorSceneOps::Terminate();
		EditorLayoutOps::ShutdownLayoutStore();
		EditorAssetOps::Terminate();
		EditorGizmos::Terminate();
		EditorPreviewStore::Terminate();
		ReflectionProbeBake::Terminate();
		EditorVariantCompiles::Terminate();
		EditorPreviews::Terminate();
		EditorAssetWatch::Terminate();
		PropertyDrawers::Terminate();
		SettingsPresentation::Terminate();
		RuntimeViewToolbar::Terminate();
		SceneViewEffects::Terminate();
		SceneAssetDrops::Terminate();
		ComponentDrops::Terminate();
		SceneViewOverlays::Terminate();
		ProfilerModules::Terminate();
		EditorConsole::Terminate();
		EventBus::GetGlobal().UnsubscribeAll(GetInstanceId());
		WindowSystem::SetModalTickCallback(nullptr, nullptr);

		if (_smokeScene.IsValid()) {
			SLT_APP_ASSERT_MSG(_smokeStats.drawCalls > 0, "smoke run rendered 0 draws");
			SLT_APP_ASSERT_MSG(_smokeUIStats.drawCalls > 0, "smoke run rendered 0 UI draws");
			SLT_APP_ASSERT_MSG(_smokeViewPixels > 1, "smoke run produced a blank Scene View target");
			SLT_LOG_INFO("Editor", "Smoke stats: {} draws ({} UI draws, {} UI quads), {} triangles, {} MiB VRAM, {} view colors",
						 _smokeStats.drawCalls, _smokeUIStats.drawCalls, _smokeUIStats.quads,
						 _smokeStats.triangles, _smokeStats.vramUsedBytes >> 20, _smokeViewPixels);
			SceneSystem::UnloadScene(_smokeScene);
			SceneSystem::EndFrame();
			_smokeScene = {};
		}
		if (Asset* mesh = _smokeMesh.Get()) {
			mesh->NotifyUnloaded();
			Delete(mesh);
			_smokeMesh = {};
		}

		SLT_LOG_INFO("Editor", "Editor terminated after {} frame(s)", Time::GetFrameCount());
	}

	namespace {

		str FormatDistribution(const EditorFrameCost::Recorder& recorder,
							   const EditorFrameCost::Distribution& distribution) {
			return std::format("n={} min={:.2f} p50={:.1f} p95={:.1f} p99={:.1f} max={:.2f} mean={:.3f}",
							   distribution.frames, recorder.MinMs(distribution),
							   recorder.PercentileMs(distribution, 0.50),
							   recorder.PercentileMs(distribution, 0.95),
							   recorder.PercentileMs(distribution, 0.99),
							   recorder.MaxMs(distribution), recorder.MeanMs(distribution));
		}

		str FormatHistogram(const EditorFrameCost::Recorder& recorder,
							const EditorFrameCost::Distribution& distribution) {
			str out;
			for (u32 bucket = 0; bucket <= EditorFrameCost::BUCKET_COUNT; ++bucket) {
				if (distribution.buckets[bucket] == 0) {
					continue;
				}
				if (!out.empty()) {
					out += ' ';
				}
				if (bucket == EditorFrameCost::BUCKET_COUNT) {
					out += std::format("ovf:{}", distribution.buckets[bucket]);
				} else {
					out += std::format("{:.1f}:{}", recorder.BucketFloorMs(bucket), distribution.buckets[bucket]);
				}
			}
			return out;
		}

	} // namespace

	void EditorApplication::ReportFrameCostGestures() {
		EditorFrameCost::Gesture gesture;
		while (_frameCost.TakeGesture(gesture)) {
			using EditorFrameCost::Stage;
			const EditorFrameCost::Distribution& frame = gesture.stages[static_cast<u32>(Stage::Frame)];
			const EditorFrameCost::Distribution& ui = gesture.stages[static_cast<u32>(Stage::UI)];
			const EditorFrameCost::Distribution& render = gesture.stages[static_cast<u32>(Stage::Render)];
			SLT_LOG_INFO("FrameCost", "[gesture {}] {} frames ({} moved, {}) | frame {} | ui p50={:.1f} | render p50={:.1f}",
						 gesture.index, gesture.frames, gesture.movedFrames,
						 gesture.moved ? "DRAG" : "press, no motion",
						 FormatDistribution(_frameCost, frame),
						 _frameCost.PercentileMs(ui, 0.50), _frameCost.PercentileMs(render, 0.50));
		}
	}

	void EditorApplication::ReportFrameCostModalPeriods() {
		using EditorFrameCost::Stage;
		EditorFrameCost::ModalPeriod period;
		while (_frameCost.TakeModalPeriod(period)) {
			const EditorFrameCost::Distribution& frame = period.stages[static_cast<u32>(Stage::Frame)];
			const EditorFrameCost::Distribution& ui = period.stages[static_cast<u32>(Stage::UI)];
			const EditorFrameCost::Distribution& render = period.stages[static_cast<u32>(Stage::Render)];
			const f64 ticksPerSecond = static_cast<f64>(Time::GetTicksPerSecond());
			const f64 spanSeconds = ticksPerSecond > 0.0 ? static_cast<f64>(period.spanTicks) / ticksPerSecond : 0.0;
			const f64 insideMs = ticksPerSecond > 0.0 ? static_cast<f64>(period.insideTicks) * 1000.0 / ticksPerSecond : 0.0;
			const f64 spanMs = spanSeconds * 1000.0;
			SLT_LOG_INFO("FrameCost",
						 "[modal {}] {:.2f}s | {} frames ({:.1f}/s) | {} moves ({:.1f}/s, {:.2f}/frame) | {} live resizes | inside {:.1f}ms of {:.1f}ms ({:.1f}%)",
						 period.index, spanSeconds, period.frames,
						 spanSeconds > 0.0 ? static_cast<f64>(period.frames) / spanSeconds : 0.0,
						 period.moves,
						 spanSeconds > 0.0 ? static_cast<f64>(period.moves) / spanSeconds : 0.0,
						 period.frames > 0 ? static_cast<f64>(period.moves) / static_cast<f64>(period.frames) : 0.0,
						 period.resizes, insideMs, spanMs,
						 spanMs > 0.0 ? insideMs * 100.0 / spanMs : 0.0);
			SLT_LOG_INFO("FrameCost", "[modal {}.stages] frame {} | ui mean={:.3f} | render mean={:.3f}",
						 period.index, FormatDistribution(_frameCost, frame),
						 _frameCost.MeanMs(ui), _frameCost.MeanMs(render));
		}
	}

	EditorFrameCost::Counters EditorApplication::SampleFrameCostCounters() const {
		using EditorFrameCost::Counter;
		EditorFrameCost::Counters counters;
		auto set = [&counters](Counter counter, u64 value) {
			counters.values[static_cast<u32>(counter)] = value;
		};
		auto add = [&counters](Counter counter, u64 value) {
			counters.values[static_cast<u32>(counter)] += value;
		};

		UISystem::ForEachContext([&add](Handle<Window>, UI::Context& context) {
			const UI::LayoutStats& layout = context.GetLayoutStats();
			add(Counter::MeasureCalls, layout.measureCalls);
			add(Counter::MeasureWork, layout.measureWork);
			add(Counter::ArrangeCalls, layout.arrangeCalls);
			add(Counter::ArrangeWork, layout.arrangeWork);
			add(Counter::LayoutPasses, layout.passes);
			const UI::PaintStats& paint = context.GetPaintStats();
			add(Counter::ElementsVisited, paint.elementsVisited);
			add(Counter::ElementsPainted, paint.elementsPainted);
			add(Counter::ScopesWalked, paint.scopesWalked);
			add(Counter::ScopesSorted, paint.scopesSorted);
			const UI::TreeStats& tree = context.GetTreeStats();
			add(Counter::LinkOps, tree.linkOps);
			add(Counter::LinkWork, tree.linkWork);
		});

		if (UI::IFontSystem* fonts = UISystem::GetFontSystem()) {
			const UI::TextStats text = fonts->GetTextStats();
			set(Counter::TextWalks, text.walks);
			set(Counter::GlyphsResolved, text.glyphsResolved);
			set(Counter::RunsBuilt, text.runsBuilt);
		}

		UISystem::Stats ui = UISystem::GetStats();
		set(Counter::UIDrawCalls, ui.drawCalls);
		set(Counter::UIQuads, ui.quads);
		set(Counter::UIContexts, ui.contexts);

		GraphicsStats graphics = RenderSystem::GetStats();
		set(Counter::VramBytes, graphics.vramUsedBytes);
		set(Counter::LiveBuffers, graphics.liveBuffers);
		set(Counter::LiveTextures, graphics.liveTextures);
		set(Counter::LiveSamplers, graphics.liveSamplers);
		set(Counter::LivePipelines, graphics.livePipelines);
		return counters;
	}

	void EditorApplication::ReportFrameCostEpochs() {
		using EditorFrameCost::Stage;
		static constexpr const char* COUNTER_NAMES[] = {
			"measureCalls", "measureWork", "arrangeCalls", "arrangeWork", "layoutPasses",
			"elementsVisited", "elementsPainted", "scopesWalked", "scopesSorted",
			"linkOps", "linkWork",
			"textWalks", "glyphsResolved", "runsBuilt",
			"uiDrawCalls", "uiQuads", "uiContexts",
			"vramBytes", "liveBuffers", "liveTextures", "liveSamplers", "livePipelines"};
		static_assert(std::size(COUNTER_NAMES) == EditorFrameCost::COUNTER_COUNT,
					  "a counter without a name would publish as a bare number");

		EditorFrameCost::Epoch epoch;
		while (_frameCost.TakeEpoch(epoch)) {
			const EditorFrameCost::Distribution& frame = epoch.stages[static_cast<u32>(Stage::Frame)];
			const EditorFrameCost::Distribution& ui = epoch.stages[static_cast<u32>(Stage::UI)];
			const EditorFrameCost::Distribution& render = epoch.stages[static_cast<u32>(Stage::Render)];
			f64 spanSeconds = static_cast<f64>(epoch.spanTicks) / static_cast<f64>(Time::GetTicksPerSecond());
			SLT_LOG_INFO("FrameCost", "[epoch {}{}] t={:.1f}s {} frames ({} pointer) | frame {} | ui mean={:.3f} | render mean={:.3f}",
						 epoch.index, epoch.closedAtShutdown ? " SHUTDOWN, partial" : "", spanSeconds,
						 epoch.frames, epoch.pointerFrames, FormatDistribution(_frameCost, frame),
						 _frameCost.MeanMs(ui), _frameCost.MeanMs(render));

			str line;
			for (u32 i = 0; i < EditorFrameCost::COUNTER_COUNT; ++i) {
				if (!line.empty()) {
					line += ' ';
				}
				if (EditorFrameCost::COUNTER_IS_LEVEL[i]) {
					line += std::format("{}={}", COUNTER_NAMES[i], epoch.counters.values[i]);
				} else {
					f64 perFrame = epoch.frames == 0 ? 0.0
													 : static_cast<f64>(epoch.delta.values[i]) / static_cast<f64>(epoch.frames);
					line += std::format("{}/f={:.2f}", COUNTER_NAMES[i], perFrame);
				}
			}
			SLT_LOG_INFO("FrameCost", "[epoch {}.counters] {}", epoch.index, line);
		}
	}

	void EditorApplication::ReportFrameCostSummary() const {
		using EditorFrameCost::Class;
		using EditorFrameCost::Exclusion;
		using EditorFrameCost::Stage;
		static constexpr const char* STAGE_NAMES[] = {"pump", "ui", "update", "render", "frame"};

		static constexpr const char* CLASS_NAMES[] = {"steady", "drag", "modal"};
		static_assert(std::size(CLASS_NAMES) == EditorFrameCost::CLASS_COUNT,
					  "a class was added without a name for the report");

		SLT_LOG_INFO("FrameCost",
					 "[summary] excluded: warmup={} modal={} throttled={} | gestures discarded={} lost={} | modal periods lost={} unpaired frames={}",
					 _frameCost.GetExcluded(Exclusion::Warmup), _frameCost.GetExcluded(Exclusion::Modal),
					 _frameCost.GetExcluded(Exclusion::Throttled), _frameCost.GetDiscardedGestures(),
					 _frameCost.GetLostGestures(), _frameCost.GetLostModalPeriods(),
					 _frameCost.GetUnpairedModalFrames());
		for (u32 frameClass = 0; frameClass < EditorFrameCost::CLASS_COUNT; ++frameClass) {
			const char* className = CLASS_NAMES[frameClass];
			for (u32 stage = 0; stage < EditorFrameCost::STAGE_COUNT; ++stage) {
				const EditorFrameCost::Distribution& distribution =
					_frameCost.Get(static_cast<Class>(frameClass), static_cast<Stage>(stage));
				SLT_LOG_INFO("FrameCost", "[{}.{}] {}", className, STAGE_NAMES[stage],
							 FormatDistribution(_frameCost, distribution));
				if (distribution.frames > 0) {
					SLT_LOG_INFO("FrameCost", "[{}.{}.hist] {}", className, STAGE_NAMES[stage],
								 FormatHistogram(_frameCost, distribution));
				}
			}
		}
	}

	void EditorApplication::BuildSmokeUIDocument() {
		if (!_projectMounted) {
			SLT_LOG_INFO("Editor", "smoke: no project mounted -> no UI builder document");
			return;
		}
		const str directory = "project://Library/Editor/SmokeUI";
		const str documentPath = directory + "/Smoke.slxml";
		const str sheetPath = directory + "/Smoke.slss";
		FileSystem::CreateDirectory(FileSystem::Resolve(Path(directory)));

		if (const TypeInfo* type = TypeRegistry::FindType(StringHash("UIBuilderPanel"))) {
			_smokeUIPanel = PanelManager::OpenPanel(type, _mainWindow);
			UI::DockManager* dock = UISystem::GetDockManager(_mainWindow);
			if (_smokeUIPanel.IsValid() && dock) {
				dock->DockPanel(_smokeUIPanel.Get()->GetDockId(), dock->GetRootNode(), UI::DockZone::Bottom);
			}
		}

		FileSystem::WriteTextFile(FileSystem::Resolve(Path(sheetPath)), str(".card {\n\tpadding: 8px;\n}\n"));
		FileSystem::WriteTextFile(FileSystem::Resolve(Path(documentPath)),
								  str("<ui version=\"1\">\n\t<sheet src=\"") + sheetPath +
									  "\"/>\n\t<panel id=\"smoke-root\" class=\"card\" style=\"width: 120px\">\n"
									  "\t\t<panel id=\"smoke-child\">smoke</panel>\n\t</panel>\n</ui>");
		if (!UIBuilderOps::OpenDocumentInBuilder(documentPath)) {
			SLT_LOG_WARN("Editor", "smoke: the UI builder didn't open its document");
			return;
		}
		_smokeUIDirectory = directory;

		Asset* asset = AssetDatabase::LoadAsset(Path(documentPath));
		UIDocumentAsset* document = asset && asset->GetAssetTypeId() == StringHash("UIDocumentAsset")
										? static_cast<UIDocumentAsset*>(asset)
										: nullptr;
		if (!document) {
			if (asset) {
				AssetDatabase::UnloadAsset(asset);
			}
			return;
		}
		UIDocumentOps::Snapshot before;
		UIDocumentOps::Capture(*document, before);
		str error;
		if (UIDocumentOps::Duplicate(*document, 1, error)) {
			UIDocumentOps::Snapshot after;
			UIDocumentOps::Capture(*document, after);
			UndoSystem::Push(MakeUnique<UISetDocumentCommand>(Handle<UIDocumentAsset>(document), std::move(before),
															  std::move(after), "Smoke Duplicate"));
		} else {
			SLT_LOG_WARN("Editor", "smoke: the builder edit failed: {}", error);
		}
		AssetDatabase::UnloadAsset(asset);
		SLT_LOG_INFO("Editor", "Smoke UI builder opened, edited and queued for close (undo depth {})",
					 UndoSystem::GetUndoDepth());
	}

	void EditorApplication::CloseSmokeUIBuilder() {
		if (_smokeUIPanel.IsValid()) {
			PanelManager::ClosePanel(_smokeUIPanel);
			_smokeUIPanel = {};
		}
	}

	void EditorApplication::ReleaseSmokeUIDocument() {
		if (_smokeUIDirectory.empty()) {
			return;
		}
		std::error_code ec;
		std::filesystem::remove_all(std::filesystem::path(FileSystem::Resolve(Path(_smokeUIDirectory)).ToString()), ec);
		_smokeUIDirectory.clear();
	}

	void EditorApplication::BuildSmokeScene() {
		if (!RenderSystem::IsRenderingAvailable()) {
			return;
		}
		_smokeScene = SceneSystem::CreateScene("Smoke");

		Handle<SceneObject> cameraObject = SceneSystem::CreateObject("Camera", _smokeScene);
		cameraObject.Get()->SetLocalPosition(Vec3(0.0f, 1.5f, -4.0f));
		cameraObject.Get()->AddComponent<Camera>();

		Handle<SceneObject> lightObject = SceneSystem::CreateObject("Sun", _smokeScene);
		lightObject.Get()->SetLocalRotation(Quat::FromEuler(Vec3(50.0f, -30.0f, 0.0f)));
		lightObject.Get()->AddComponent<Light>();

		MeshAsset* mesh = New<MeshAsset>(MemTag::Asset);
		const Vec3 corners[8] = {
			{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}};
		const u32 faces[6][4] = {
			{0, 3, 2, 1},
			{5, 6, 7, 4},
			{4, 7, 3, 0},
			{1, 2, 6, 5},
			{3, 7, 6, 2},
			{4, 0, 1, 5},
		};
		const Vec3 normals[6] = {
			{0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}};
		for (u32 face = 0; face < 6; ++face) {
			u32 base = static_cast<u32>(mesh->vertices.size());
			for (u32 i = 0; i < 4; ++i) {
				MeshVertex vertex;
				vertex.position = corners[faces[face][i]];
				vertex.normal = normals[face];
				vertex.u = (i == 1 || i == 2) ? 1.0f : 0.0f;
				vertex.v = (i >= 2) ? 1.0f : 0.0f;
				mesh->bounds.Encapsulate(vertex.position);
				mesh->vertices.push_back(vertex);
			}
			const u32 quad[6] = {0, 1, 2, 0, 2, 3};
			for (u32 index : quad) {
				mesh->indices.push_back(base + index);
			}
		}
		mesh->submeshes.push_back(SubMesh{0, static_cast<u32>(mesh->indices.size()), 0, mesh->bounds});
		mesh->NotifyLoaded();
		_smokeMesh = Handle<MeshAsset>(mesh);

		Handle<SceneObject> cubeObject = SceneSystem::CreateObject("Cube", _smokeScene);
		cubeObject.Get()->AddComponent<MeshRenderer>().Get()->SetMesh(_smokeMesh);

		SLT_LOG_INFO("Editor", "Smoke scene built (lit cube, error material)");
	}

	u32 EditorApplication::CaptureSmokeViewPixels() const {
		Vector<Handle<EditorPanel>> panels{GetMemoryResource(MemTag::Editor)};
		PanelManager::GetOpenPanels(panels);
		for (Handle<EditorPanel> handle : panels) {
			SceneViewPanel* sceneView = nullptr;
			if (EditorPanel* panel = handle.Get()) {
				if (panel->GetType() == SceneViewPanel::GetReflectType()) {
					sceneView = static_cast<SceneViewPanel*>(panel);
				}
			}
			if (!sceneView) {
				continue;
			}
			Vector<u8> pixels{GetMemoryResource(MemTag::Editor)};
			Vec2I size{};
			if (!RenderSystem::ReadbackViewTarget(sceneView->GetViewTarget(), pixels, size) || size.x <= 0) {
				return 0;
			}
			HashSet<u32> distinct{GetMemoryResource(MemTag::Editor)};
			const u32* words = reinterpret_cast<const u32*>(pixels.data());
			size_t count = pixels.size() / 4;
			for (size_t i = 0; i < count && distinct.size() < 64; ++i) {
				distinct.insert(words[i]);
			}
			return static_cast<u32>(distinct.size());
		}
		return 0;
	}

	void EditorApplication::FocusRuntimeView() {
		PlayModeFocus mode = EditorConfig::ReadPlayPreferences().focusRuntimeView;
		if (mode == PlayModeFocus::Never) {
			return;
		}
		if (mode == PlayModeFocus::OnlyWithCamera && !EditorViewportOps::FindActiveSceneCamera().IsValid()) {
			return;
		}

		const TypeInfo* runtimeViewType = RuntimeViewPanel::GetReflectType();
		_tabsBeforePlay.clear();
		EditorLayoutOps::BringPanelsForward(runtimeViewType, _tabsBeforePlay);

		Vector<Handle<EditorPanel>> panels{GetMemoryResource(MemTag::Editor)};
		PanelManager::GetOpenPanels(panels);
		bool keyboardHandedOver = false;
		for (Handle<EditorPanel> handle : panels) {
			EditorPanel* panel = handle.Get();
			if (!panel || panel->GetType() != runtimeViewType) {
				continue;
			}
			auto* view = static_cast<RuntimeViewPanel*>(panel);
			view->DisarmGamePointer();
			if (keyboardHandedOver) {
				continue;
			}
			view->RequestViewportFocus();
			keyboardHandedOver = true;
		}
	}

	void EditorApplication::RestoreViewAfterPlay() {
		if (EditorConfig::ReadPlayPreferences().restoreViewOnStop) {
			EditorLayoutOps::RestoreForwardPanels({_tabsBeforePlay.data(), _tabsBeforePlay.size()});
		}
		_tabsBeforePlay.clear();
	}

	void EditorApplication::RouteGameplayInput() {
		EditorInputOps::RouteFacts facts = EditorInputOps::ApplicationFacts();
		facts.viewFocused = RuntimeViewHasInputFocus();
		const RuntimeViewPointer pointer = AdvanceRuntimeViewPointer();
		facts.pointerInside = pointer.aimed;
		const bool routed = EditorInputOps::ShouldOpen(EditorInputOps::Routes::GameplayInput, facts);
		Input::SetGameplayInputRouted(routed);

		Input::SetGameplayPointerRouted(
			EditorInputOps::ShouldOpen(EditorInputOps::Routes::GameplayPointer, facts));

		UISystem::SetCursorModeYielded(routed && _playMode.IsPlaying() && pointer.owned);
	}

	EditorApplication::RuntimeViewPointer EditorApplication::AdvanceRuntimeViewPointer() {
		const TypeInfo* runtimeViewType = RuntimeViewPanel::GetReflectType();
		Vector<Handle<EditorPanel>> panels{GetMemoryResource(MemTag::Editor)};
		PanelManager::GetOpenPanels(panels);
		RuntimeViewPointer result;
		for (Handle<EditorPanel> handle : panels) {
			EditorPanel* panel = handle.Get();
			if (!panel || panel->GetType() != runtimeViewType) {
				continue;
			}
			auto* view = static_cast<RuntimeViewPanel*>(panel);
			view->AdvanceGamePointer();
			const bool focused = view->HasInputFocus();
			result.aimed = result.aimed || (focused && view->HasGamePointerAim());
			result.owned = result.owned || (focused && view->HasGamePointer());
		}
		return result;
	}

	bool EditorApplication::RuntimeViewHasInputFocus() const {
		const TypeInfo* runtimeViewType = RuntimeViewPanel::GetReflectType();
		Vector<Handle<EditorPanel>> panels{GetMemoryResource(MemTag::Editor)};
		PanelManager::GetOpenPanels(panels);
		for (Handle<EditorPanel> handle : panels) {
			EditorPanel* panel = handle.Get();
			if (!panel || panel->GetType() != runtimeViewType) {
				continue;
			}
			if (static_cast<const RuntimeViewPanel*>(panel)->HasInputFocus()) {
				return true;
			}
		}
		return false;
	}

	void EditorApplication::Run() {
		while (!_quit) {
			RunFrame();
		}
	}

	void EditorApplication::DumpZonesIfDue() {
		if (!_zoneDumpEnabled) {
			return;
		}
		if (!Profiler::IsEnabled()) {
			Profiler::SetEnabled(true);
			return;
		}
		const Profiler::FrameResults frame = Profiler::GetFrameResults();
		++_zoneWindowFrames;

		f64 total = 0.0;
		for (u32 i = 0; i < frame.count; ++i) {
			if (std::strcmp(frame.zones[i].name, ZONE_FRAME_NAME) == 0) {
				total = frame.zones[i].milliseconds;
				break;
			}
		}
		if (total > _zoneWorstMs) {
			_zoneWorstMs = total;
			_zoneWorstFrame = _zoneWindowFrames;
			_zoneWorstDroppedSites = frame.droppedSites;
			_zoneWorstDroppedDepth = frame.droppedDepth;
			_zoneWorst.assign(frame.zones, frame.zones + frame.count);
		}

		if (--_zoneDumpCountdown > 0) {
			return;
		}
		_zoneDumpCountdown = ZONE_DUMP_PERIOD;

		str line;
		for (const Profiler::ZoneResult& zone : _zoneWorst) {
			if (zone.calls == 0) {
				continue;
			}
			line += std::format("{}={:.3f}ms/{} ", zone.name, zone.milliseconds, zone.calls);
		}
		SLT_LOG_INFO("Perf", "zones worst {}/{} {:.3f}ms: {}(dropped sites {}, depth {})",
					 _zoneWorstFrame, _zoneWindowFrames, _zoneWorstMs, line, _zoneWorstDroppedSites, _zoneWorstDroppedDepth);

		_zoneWorstMs = -1.0;
		_zoneWorstFrame = 0;
		_zoneWindowFrames = 0;
		_zoneWorst.clear();
	}

	InteractionMode EditorApplication::PresentMode() const {
		return _smokeFramesRemaining >= 0 ? InteractionMode::NoThrottling : EditorConfig::ReadGeneralPreferences().interactionMode;
	}

	void EditorApplication::RunFrame() {
		struct FrameDepthGuard {
			i32& depth;
			explicit FrameDepthGuard(i32& d) : depth(d) { ++depth; }
			~FrameDepthGuard() { --depth; }
		} depthGuard{_frameDepth};

		if (_frameDepth > 1) {
			++_frameCostModalTicks;
		}
		if (_frameDepth == 1) {
			Profiler::NextFrame();
			DumpZonesIfDue();
			_presentPolicy.SetMode(PresentMode());
			if (_frameIndex++ == _loseDeviceFrame && RenderSystem::IsRenderingAvailable()) {
				RenderSystem::GetDevice()->SimulateLoss(_failedRebuilds);
			}
		}
		SLT_PROFILE_ZONE("Editor.Frame");

		bool measure = _frameCostEnabled && _frameDepth == 1;
		bool measureModal = _frameCostEnabled && _frameDepth > 1;
		u64 modalTicksAtEntry = _frameCostModalTicks;
		u64 frameStartTicks = (measure || measureModal) ? Time::GetTicks() : 0;

		bool throttle = !WindowSystem::GetFocusedWindow().IsValid() && _smokeFramesRemaining < 0 &&
						_throttleWhenUnfocused;
		if (!throttle) {
			_throttledFrames = 0;
			if (_throttleAnnounced) {
				_throttleAnnounced = false;
				SLT_LOG_TRACE("Editor", "throttle released: a window has focus again");
			}
		} else if (!_throttleAnnounced && ++_throttledFrames >= THROTTLE_ANNOUNCE_FRAMES) {
			_throttleAnnounced = true;
			SLT_LOG_TRACE("Editor",
						  "throttled: no window has focus -> frames are event driven at ~{:.0f} FPS -> posted "
						  "input waits for the next event (idle, not hang). Pass --no-throttle to keep "
						  "rendering while unfocused",
						  1.0 / THROTTLE_WAIT_SECONDS);
		}
		{
			SLT_PROFILE_ZONE("Editor.Pump");
			if (throttle) {
				WindowSystem::WaitEvents(THROTTLE_WAIT_SECONDS);
			} else {
				WindowSystem::PumpEvents();
			}
		}

		u64 pumpEndTicks = (measure || measureModal) ? Time::GetTicks() : 0;
		bool pointerDown = measure && Input::IsMouseButtonDown(MouseButton::Left);
		Vec2 pointerDelta = measure ? Input::GetMouseDelta() : Vec2{};

		_frameTimer.Tick();
		u32 fixedSteps = 0;
		if (_frameDepth == 1) {
			_tickGameplay = _playMode.BeginFrameTick();
			fixedSteps = _clock.BeginFrame(EditorFrameClock::KindFromGate(_tickGameplay, _playMode.IsPaused()),
										   _frameTimer.GetUnscaledDeltaTime());
		}
		const bool tickGameplay = _tickGameplay;
		SceneSystem::BeginFrame();
		RouteGameplayInput();

		FrameStageArgs stageArgs;
		u64 updateEndTicks = 0;

		for (FrameStage stage : FRAME_STAGES) {
			switch (stage) {
			case FrameStage::FixedUpdate:
				if (tickGameplay) {
					for (u32 i = 0; i < fixedSteps; ++i) {
						FrameStages::Run(stage, stageArgs);
					}
				}
				break;
			case FrameStage::Update: {
				if (tickGameplay) {
					SLT_PROFILE_ZONE("Editor.Scene");
					FrameStages::Run(stage, stageArgs);
				}
				break;
			}
			case FrameStage::UIUpdate:
				UIUpdate(stageArgs);
				break;
			case FrameStage::LateUpdate:
				if (tickGameplay) {
					FrameStages::Run(stage, stageArgs);
				}
				break;
			case FrameStage::EndFrame:
				FrameStages::Run(stage, stageArgs);
				RunFrameBoundary();
				PanelManager::SubmitViews(Time::GetUnscaledDeltaTime());
				updateEndTicks = (measure || measureModal) ? Time::GetTicks() : 0;
				break;
			case FrameStage::Render: {
				SLT_PROFILE_ZONE("Editor.Render");
				FrameStages::Run(stage, stageArgs);
				break;
			}
			}
		}

		if (measure || measureModal) {
			u64 renderEndTicks = Time::GetTicks();
			EditorFrameCost::Sample sample;
			auto& ticks = sample.stageTicks;
			ticks[static_cast<u32>(EditorFrameCost::Stage::Pump)] = pumpEndTicks - frameStartTicks;
			ticks[static_cast<u32>(EditorFrameCost::Stage::UI)] = _frameCostUITicks;
			ticks[static_cast<u32>(EditorFrameCost::Stage::Update)] = updateEndTicks - pumpEndTicks;
			ticks[static_cast<u32>(EditorFrameCost::Stage::Render)] = renderEndTicks - updateEndTicks;
			ticks[static_cast<u32>(EditorFrameCost::Stage::Frame)] = renderEndTicks - frameStartTicks;
			sample.pointerDown = pointerDown;
			sample.pointerMoved = pointerDelta.x != 0.0f || pointerDelta.y != 0.0f;
			sample.modal = _frameCostModalTicks != modalTicksAtEntry;
			sample.throttled = throttle;
			sample.startTicks = frameStartTicks;
			if (measureModal) {
				_frameCost.RecordModal(sample);
			} else {
				_frameCost.Record(sample);
				ReportFrameCostGestures();
				ReportFrameCostModalPeriods();
				if (_frameCost.EpochElapsed()) {
					_frameCost.CloseEpoch(SampleFrameCostCounters());
					ReportFrameCostEpochs();
				}
			}
		}
		_frameCostUITicks = 0;

		if (_smokeFramesRemaining == 200) {
			BuildSmokeUIDocument();
		}
		if (_smokeFramesRemaining == 60) {
			CloseSmokeUIBuilder();
		}

		if (_smokeFramesRemaining > 0 && --_smokeFramesRemaining == 0) {
			_smokeStats = RenderSystem::GetStats();
			_smokeUIStats = UISystem::GetStats();
			_smokeViewPixels = CaptureSmokeViewPixels();
			SLT_LOG_INFO("Editor", "Smoke run complete. Closing the main window");
			WindowSystem::RequestDestroy(_mainWindow);
		}
	}

	// inside EndFrame (module swaps + play transitions are safe there)
	void EditorApplication::RunFrameBoundary() {
		if (_frameDepth != 1) {
			return;
		}
		_compileService.PumpMainThread();
		_buildPipeline.PumpMainThread();
		BuildFlows::PumpMainThread(UISystem::GetContext(_mainWindow));
		_moduleHost.SetPlaying(_playMode.IsPlaying());
		_moduleHost.PumpMainThread();
		_playMode.PumpMainThread();
		if (_playMode.IsPlaying() != _wasPlaying) {
			_clock.DropBankedTime();
		}
		if (_playMode.IsPlaying() && !_wasPlaying) {
			FocusRuntimeView();
		} else if (!_playMode.IsPlaying() && _wasPlaying) {
			RestoreViewAfterPlay();
		}
		_wasPlaying = _playMode.IsPlaying();

		AssetDatabase::CollectGarbage();
	}

	void EditorApplication::UIUpdate(const FrameStageArgs& stageArgs) {
		SLT_PROFILE_ZONE("Editor.Update");
		f32 deltaSeconds = Time::GetUnscaledDeltaTime();

		EditorAssetWatch::PollChanges();
		for (const Path& changed : EditorAssetWatch::GetChangedThisFrame()) {
			const IAssetImporter* importer = AssetImporterRegistry::FindByExtension(changed.GetExtension());
			if (importer && importer->GetAssetType() == StringTableAsset::GetReflectType()->name) {
				Localization::ReloadTables();
				break;
			}
		}
		EditorAssetOps::ReloadChanged(EditorAssetWatch::GetChangedThisFrame(),
									  [this](const Path& path, std::function<void()> reload) {
										  UI::Context* context = UISystem::GetContext(_mainWindow);
										  if (!context) {
											  return;
										  }
										  SceneFileFlows::OpenConfirmPopup(
											  *context,
											  "'" + path.GetFilename() + "' was changed outside the editor, it has unsaved changes here. Reload it and lose them?",
											  "Reload", std::move(reload), "", {}, "Keep Changes");
									  });
		EditorPreviewStore::Drain();
		ReflectionProbeBake::Drain();
		EditorVariantCompiles::Drain();
		EditorDialogs::Drain();

		{
			UIScreenDesc nowhere;
			nowhere.target = UIScreenDesc::Target::None;
			UISystem::SetScreen(nowhere);
		}

		{
			SLT_PROFILE_ZONE("Editor.Panels");
			{
				SLT_PROFILE_ZONE("Editor.PanelTick");
				PanelManager::TickAll(deltaSeconds);
			}
			{
				SLT_PROFILE_ZONE("Editor.Shell");
				_shell.Tick(deltaSeconds);
			}
		}

		u64 uiStartTicks = _frameCostEnabled ? Time::GetTicks() : 0;
		{
			SLT_PROFILE_ZONE("Editor.UI");
			FrameStages::Run(FrameStage::UIUpdate, stageArgs);
		}
		if (_frameCostEnabled) {
			_frameCostUITicks = Time::GetTicks() - uiStartTicks;
		}

		_shortcuts.Route(EditorInputOps::ApplicationFacts());
		_shortcuts.Evaluate();
		using Shortcut = EditorInputOps::Shortcut;
		auto performed = [this](Shortcut shortcut) {
			return _shortcuts.WasPerformed(shortcut);
		};

		if (performed(Shortcut::ReloadModule)) {
			CompileStatus::RequestReload();
		}
		if (performed(Shortcut::CompileModule)) {
			CompileStatus::RequestCompile();
		}
		if (performed(Shortcut::RecompileModule)) {
			_moduleHost.RequestRecompile();
		}
		if (performed(Shortcut::OpenBuildProfiles)) {
			PanelManager::FocusPanelOfType(StringHash("BuildProfilesPanel"));
		}
		if (performed(Shortcut::BuildAndRun)) {
			str profile = BuildProfilesPanel::GetSharedSelection();
			UI::Context* context = UISystem::GetContext(_mainWindow);
			if (profile.empty() || !context) {
				PanelManager::FocusPanelOfType(StringHash("BuildProfilesPanel"));
				SLT_LOG_INFO("Build", "select a build profile, then press Ctrl+B to build and run it");
			} else {
				Vector<str> profiles{GetMemoryResource(MemTag::Editor)};
				profiles.push_back(profile);
				BuildFlows::RequestBuild(*context, profiles, true);
			}
		}
		if (performed(Shortcut::TogglePlay)) {
			_playMode.RequestToggle();
		}
		if (performed(Shortcut::TogglePause)) {
			_playMode.RequestPause();
		}
		if (performed(Shortcut::Step)) {
			_playMode.RequestStep();
		}
		if (performed(Shortcut::Undo)) {
			UndoSystem::Undo();
		}
		if (performed(Shortcut::Redo)) {
			UndoSystem::Redo();
		}
		if (UI::Context* context = UISystem::GetContext(_mainWindow)) {
			if (performed(Shortcut::SaveScene)) {
				SceneFileFlows::SaveScene(*context, {});
			}
			if (performed(Shortcut::SaveSceneAs)) {
				SceneFileFlows::SaveSceneAs(*context, {});
			}
		}
		auto dispatch = [](PanelCommand command) {
			if (EditorPanel* panel = PanelManager::GetFocusedPanel().Get()) {
				panel->OnCommand(command);
			}
		};
		if (performed(Shortcut::DeleteSelection)) {
			dispatch(PanelCommand::Delete);
		}
		if (performed(Shortcut::RenameSelection)) {
			dispatch(PanelCommand::Rename);
		}
		if (performed(Shortcut::SelectAll)) {
			dispatch(PanelCommand::SelectAll);
		}
		if (performed(Shortcut::DuplicateSelection)) {
			dispatch(PanelCommand::Duplicate);
		}

		if (_smokeFramesRemaining < 0) {
			_config.TickAutosave(deltaSeconds, _mainWindow);
			SceneAnnotations::FlushDirty();
		}
	}

	void EditorApplication::ArmShaderHotReload() {
		str library = FileSystem::Resolve(Path("project://Library")).ToString();
		str libraryPrefix = library.find("://") == str::npos ? library + "/" : str();
		for (char& c : libraryPrefix) {
			c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		auto armWatcher = [this, libraryPrefix](const Path& physical) {
			if (physical.ToString().find("://") != str::npos || !FileSystem::IsDirectory(physical)) {
				return;
			}
			_shaderWatchers.emplace_back(physical, [libraryPrefix](const Path& changed, FileAction action) {
				if (action == FileAction::Removed) {
					return;
				}
				str path = changed.ToString();
				str key = path;
				for (char& c : key) {
					c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
				if (!libraryPrefix.empty() && key.starts_with(libraryPrefix)) {
					return;
				}
				RenderSystem::NotifyShaderFileChanged(path);
			});
		};
		armWatcher(FileSystem::Resolve(Path("engine://Shaders")));
		armWatcher(AssetBrowserOps::GetProjectRootPhysical());
	}

	void EditorApplication::RequestQuit() {
		_relaunchCommandLine.clear();
		QuitInternal();
	}

	void EditorApplication::QuitInternal() {
		UI::Context* context = UISystem::GetContext(_mainWindow);
		if (context && !_quitConfirmed) {
			PanelManager::ConfirmUnsavedPanelsThen(*context, [this, context]() {
				SceneFileFlows::ConfirmUnsavedThen(*context, [this]() {
					_quitConfirmed = true;
					QuitInternal();
				});
			});
			return;
		}
		if (!_layoutSaved && _shell.IsBuilt() && _smokeFramesRemaining < 0) {
			_config.SaveNow(_mainWindow);
			_layoutSaved = true;
		}
		WindowSystem::RequestDestroy(_mainWindow);
	}

	void EditorApplication::RequestRestart() {
		str executable = ScriptProjectOps::GetEditorExecutablePath();
		str project = _projectPath.empty() ? ProjectOps::GetMountedProjectPath() : _projectPath;
		str line = ProjectOps::BuildRelaunchCommandLine(executable, {_arguments.data(), _arguments.size()}, project);
		if (line.empty()) {
			SLT_LOG_ERROR("Editor", "can't restart: the editor's exe path is unavailable");
			return;
		}
		_relaunchCommandLine = std::move(line);
		QuitInternal();
	}

	void EditorApplication::OnCloseRequested(const WindowCloseRequestedEvent& event) {
		if (event.window == _mainWindow) {
			RequestQuit();
		} else {
			WindowSystem::RequestDestroy(event.window);
		}
	}

	void EditorApplication::OnWindowDestroyed(const WindowDestroyedEvent& event) {
		if (event.windowId == _mainWindowId) {
			_quit = true;
		}
		std::erase_if(_floatingChrome, [](const FloatingChrome& entry) {
			return !entry.window.IsValid();
		});
	}

	void EditorApplication::OnWindowStateChanged(const WindowStateEvent& event) {
		for (const FloatingChrome& entry : _floatingChrome) {
			if (entry.window == event.window) {
				if (UI::Context* context = UISystem::GetContext(entry.window)) {
					EditorChromeOps::SetMaximizeGlyph(*context, entry.maximizeFace,
													  event.state == WindowState::Maximized);
				}
				break;
			}
		}
	}

	void EditorApplication::OnWindowResized(const WindowResizedEvent& event) {
		if (!event.live) {
			SLT_LOG_DEBUG("Editor", "Main window resized to {}x{}",
						  event.clientSizePx.x, event.clientSizePx.y);
		}
		if (_frameCostEnabled && event.live) {
			_frameCost.NoteModalResize();
		}
	}

	void EditorApplication::OnFrameCostDragBegan(const WindowDragBeganEvent& event) {
		(void)event;
		_frameCost.BeginModalPeriod(Time::GetTicks());
	}

	void EditorApplication::OnFrameCostDragMoved(const WindowDragMovedEvent& event) {
		(void)event;
		_frameCost.NoteModalMove();
	}

	void EditorApplication::OnFrameCostDragEnded(const WindowDragEndedEvent& event) {
		(void)event;
		_frameCost.EndModalPeriod(Time::GetTicks());
	}

	void EditorApplication::OnFilesDropped(const FilesDroppedEvent& event) {
		for (const str& path : event.paths) {
			SLT_LOG_INFO("Editor", "File dropped: {}", path);
		}
	}

} // namespace Shalltear
