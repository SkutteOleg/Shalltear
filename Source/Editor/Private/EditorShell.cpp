#include "EditorShell.h"

#include "Shalltear/Core/BuildInfo.h"
#include "Shalltear/Core/Graphics/RHIDevice.h"
#include "Shalltear/Core/Graphics/RenderSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/UI/UISystem.h"
#include "Shalltear/Core/UI/Widgets.h"
#include "Shalltear/Core/Window/Window.h"

#include "Shalltear/Editor/AssetBrowserOps.h"
#include "Shalltear/Editor/CompileService.h"
#include "Shalltear/Editor/CppToolchain.h"
#include "Shalltear/Editor/EditorChromeOps.h"
#include "Shalltear/Editor/EditorConsole.h"
#include "Shalltear/Editor/EditorIcons.h"
#include "Shalltear/Editor/EditorInputOps.h"
#include "Shalltear/Editor/EditorLayoutOps.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/EditorSelection.h"
#include "Shalltear/Editor/EditorTheme.h"
#include "Shalltear/Editor/EngineVersionOps.h"
#include "Shalltear/Editor/PanelManager.h"
#include "Shalltear/Editor/ProjectOps.h"
#include "Shalltear/Editor/ScriptProjectOps.h"
#include "Shalltear/Editor/UndoSystem.h"

#include "GameModuleHost.h"
#include "PackageHost.h"
#include "ScriptFlows.h"
#include "Shalltear/Editor/PlayModeController.h"
#include "SceneFileFlows.h"

#include <algorithm>
#include <format>
#include <utility>

namespace Shalltear {

	namespace {

		using EditorTheme::ColorToken;
		using EditorTheme::MetricToken;

		using EditorInputOps::ChordLabel;
		using EditorInputOps::Shortcut;

		Color TextNormal() { return EditorTheme::GetColor(ColorToken::Text); }
		Color TextDim() { return EditorTheme::GetColor(ColorToken::TextDim); }
		Color TextStrong() { return EditorTheme::GetColor(ColorToken::TextStrong); }
		Color StatusBusy() { return EditorTheme::GetColor(ColorToken::Accent); }
		Color StatusWarn() { return EditorTheme::GetColor(ColorToken::StatusWarn); }
		Color StatusError() { return EditorTheme::GetColor(ColorToken::StatusError); }
		Color ModuleRest() { return TextDim(); }
		f32 ToolbarTextPx() { return EditorTheme::GetMetric(MetricToken::Font); }

		constexpr f32 STATUS_REFRESH_SECONDS = 0.25f;

		constexpr std::string_view GRAPHICS_API_LABEL = "Vulkan";

		Color CompileGlyphColor(CompileState state, u32 warningCount) {
			switch (state) {
			case CompileState::Configuring:
			case CompileState::Compiling:
				return StatusBusy();
			case CompileState::Failed:
				return StatusError();
			case CompileState::Succeeded:
				return warningCount == 0 ? ModuleRest() : StatusWarn();
			default:
				return ModuleRest();
			}
		}

		UI::ElementHandle ParentOf(UI::Context& context, UI::ElementHandle element) {
			const UI::Element* pod = context.GetElement(element);
			return pod ? context.MakeHandle(pod->parent) : UI::ElementHandle{};
		}

	} // anonymous namespace

	void EditorShell::Initialize(Handle<Window> mainWindow, str projectName, GameModuleHost* moduleHost,
								 PlayModeController* playMode, CompileService* compileService, Hooks hooks) {
		_mainWindow = mainWindow;
		_projectName = std::move(projectName);
		_moduleHost = moduleHost;
		_playMode = playMode;
		_compileService = compileService;
		_hooks = std::move(hooks);

		UI::Context* context = UISystem::CreateContext(mainWindow);
		if (!context) {
			SLT_LOG_WARN("Editor", "shell: no UI context for the main window");
			return;
		}
		UISystem::CreateDockManager(mainWindow);
		BuildChrome(*context);
		UISystem::SetDockHost(mainWindow, _dockHost);
		_built = true;
	}

	void EditorShell::Terminate() {
		_built = false;
		_moduleHost = nullptr;
		_playMode = nullptr;
		_compileService = nullptr;
	}

	void EditorShell::BuildChrome(UI::Context& context) {
		Handle<FontAsset> font = UISystem::GetDefaultFont();

		UI::Style rootStyle;
		rootStyle.flexDirection = UI::FlexDirection::Column;
		rootStyle.alignItems = UI::Align::Stretch;
		context.SetStyle(context.GetRoot(), rootStyle);

		GameModuleHost* moduleHost = _moduleHost;
		Hooks& hooks = _hooks;
		UI::Context* contextPtr = &context;
		EditorShell* shell = this;
		Handle<Window> mainWindow = _mainWindow;

		Vector<UI::Widgets::MenuBarItemDesc> menus{GetMemoryResource(MemTag::Editor)};

		UI::Widgets::MenuBarItemDesc fileMenu;
		fileMenu.label = "File";
		fileMenu.fill = [hooks, contextPtr](Vector<UI::Widgets::MenuItem>& items) {
			bool haveScene = SceneSystem::GetPrimaryScene().IsValid();
			items.push_back({"New Scene", "", true, false, [contextPtr]() { SceneFileFlows::NewScene(*contextPtr); }});
			items.push_back({"Open Scene...", "", true, false, [contextPtr]() { SceneFileFlows::OpenScene(*contextPtr); }});
			items.push_back({"Save Scene", ChordLabel(Shortcut::SaveScene), haveScene, false, [contextPtr]() {
								 SceneFileFlows::SaveScene(*contextPtr, {});
							 }});
			items.push_back({"Save Scene As...", "", haveScene, false, [contextPtr]() {
								 SceneFileFlows::SaveSceneAs(*contextPtr, {});
							 }});
			Vector<str> recents{GetMemoryResource(MemTag::Editor)};
			EditorSceneOps::GetRecentScenes(recents);
			bool first = true;
			for (const str& recent : recents) {
				items.push_back({recent, "", true, first, [contextPtr, recent]() {
									 SceneFileFlows::OpenScenePath(*contextPtr, Path(recent));
								 }});
				first = false;
			}
			items.push_back({"Restart Editor", "", true, true, hooks.onRequestRestart});
			items.push_back({"Exit", "", true, false, hooks.onRequestQuit});
		};
		menus.push_back(std::move(fileMenu));

		UI::Widgets::MenuBarItemDesc editMenu;
		editMenu.label = "Edit";
		editMenu.fill = [](Vector<UI::Widgets::MenuItem>& items) {
			str undoLabel = UndoSystem::CanUndo() ? "Undo " + UndoSystem::GetUndoLabel() : str("Undo");
			str redoLabel = UndoSystem::CanRedo() ? "Redo " + UndoSystem::GetRedoLabel() : str("Redo");
			items.push_back({std::move(undoLabel), ChordLabel(Shortcut::Undo), UndoSystem::CanUndo(), false, []() { UndoSystem::Undo(); }});
			items.push_back({std::move(redoLabel), ChordLabel(Shortcut::Redo), UndoSystem::CanRedo(), false, []() { UndoSystem::Redo(); }});
		};
		menus.push_back(std::move(editMenu));

		UI::Widgets::MenuBarItemDesc scriptsMenu;
		scriptsMenu.label = "Scripts";
		CompileService* compileService = _compileService;
		scriptsMenu.fill = [contextPtr, moduleHost, compileService](Vector<UI::Widgets::MenuItem>& items) {
			str projectPath = ProjectOps::GetMountedProjectPath();
			bool hasCode = ScriptProjectOps::HasScriptProject(projectPath);
			bool busy = compileService && compileService->IsBusy();

			if (!hasCode) {
				items.push_back({"Create C++ Module...", "", !projectPath.empty(), false, [contextPtr]() {
									 ScriptFlows::OpenCreateModulePopup(*contextPtr);
								 }});
				items.push_back({"Toolchain...", "", true, true, [contextPtr]() {
									 ScriptFlows::OpenToolchainPopup(*contextPtr);
								 }});
				return;
			}

			items.push_back({"New C++ Component...", "", true, false, [contextPtr]() {
								 ScriptFlows::OpenCreateComponentPopup(*contextPtr);
							 }});
			items.push_back({"New C# Component...", "", true, false, [contextPtr]() {
								 ScriptFlows::OpenCreateCSharpComponentPopup(*contextPtr);
							 }});
			items.push_back({"Compile", ChordLabel(Shortcut::CompileModule), !busy, true, [moduleHost]() {
								 if (moduleHost) {
									 moduleHost->RequestCompile();
								 }
							 }});
			items.push_back({"Recompile", ChordLabel(Shortcut::RecompileModule), !busy, false, [moduleHost]() {
								 if (moduleHost) {
									 moduleHost->RequestRecompile();
								 }
							 }});
			items.push_back({"Reload Module", ChordLabel(Shortcut::ReloadModule), moduleHost && moduleHost->HasModuleBinaryPath(), false, [moduleHost]() {
								 if (moduleHost) {
									 moduleHost->RequestReload();
								 }
							 }});
			bool autoCompile = moduleHost && moduleHost->IsAutoCompileEnabled();
			items.push_back({str(autoCompile ? "[x] " : "[ ] ") + "Auto-compile on save", "", true, false, [moduleHost]() {
								 if (moduleHost) {
									 moduleHost->SetAutoCompileEnabled(!moduleHost->IsAutoCompileEnabled());
								 }
							 }});

			str editorCommand = moduleHost ? moduleHost->GetExternalEditorCommand() : str{};
			str editorName = editorCommand.empty() ? ScriptProjectOps::DetectExternalEditor().name : str{};
			items.push_back({editorName.empty() ? str("Open in External Editor") : "Open in " + editorName, "", true, true, [projectPath, editorCommand]() {
								 ScriptProjectOps::OpenInExternalEditor(projectPath, editorCommand, {}, 0, 0);
							 }});
			items.push_back({"Reveal Source Folder", "", true, false, [projectPath]() {
								 AssetBrowserOps::RevealInExplorer(Path("project://Source"));
							 }});
			items.push_back({"Regenerate Project Files", "", true, false, [projectPath]() {
								 ScriptProjectOps::ScaffoldOptions options;
								 options.sdkRoot = ScriptProjectOps::GetSDKRoot();
								 options.editorExecutable = ScriptProjectOps::GetEditorExecutablePath();
								 if (const PackageHost* packages = PackageHost::GetActive()) {
									 options.enabledPackages = packages->GetGamePackageNames();
								 }
								 ScriptProjectOps::ScaffoldResult result;
								 ScriptProjectOps::RegenerateProjectFiles(projectPath, options, result);
							 }});
			items.push_back({"Toolchain...", "", true, true, [contextPtr]() {
								 ScriptFlows::OpenToolchainPopup(*contextPtr);
							 }});
		};
		menus.push_back(std::move(scriptsMenu));

		UI::Widgets::MenuBarItemDesc windowMenu;
		windowMenu.label = "Window";
		windowMenu.fill = [hooks, mainWindow, contextPtr](Vector<UI::Widgets::MenuItem>& items) {
			items.push_back({"Hold Shift to open a 2nd one", "", false, false, {}});

			Vector<const TypeInfo*> types{GetMemoryResource(MemTag::Editor)};
			PanelManager::GetPanelTypes(types);

			auto makeRow = [mainWindow, contextPtr](const TypeInfo* type, bool separatorBefore) {
				StringHash typeName = type->name;
				return UI::Widgets::MenuItem{
					PanelManager::GetPanelTypeName(type), "", true, separatorBefore,
					[typeName, mainWindow, contextPtr]() {
						bool wantsNew = (contextPtr->GetPressMods() & KeyMod::Shift) != KeyMod::None;
						if (!wantsNew) {
							PanelManager::FocusPanelOfType(typeName);
							return;
						}
						if (const TypeInfo* live = TypeRegistry::FindType(typeName)) {
							PanelManager::OpenPanel(live, mainWindow);
						}
					}};
			};

			auto byPanelName = [](const TypeInfo* a, const TypeInfo* b) {
				return PanelManager::GetPanelTypeName(a) < PanelManager::GetPanelTypeName(b);
			};
			Vector<const TypeInfo*> loose{GetMemoryResource(MemTag::Editor)};
			Vector<std::pair<str, Vector<const TypeInfo*>>> groups{GetMemoryResource(MemTag::Editor)};
			for (const TypeInfo* type : types) {
				str category = PanelManager::GetPanelTypeCategory(type);
				if (category.empty()) {
					loose.push_back(type);
					continue;
				}
				auto found = std::find_if(groups.begin(), groups.end(),
										  [&category](const auto& entry) { return entry.first == category; });
				if (found == groups.end()) {
					groups.push_back({std::move(category), Vector<const TypeInfo*>{GetMemoryResource(MemTag::Editor)}});
					found = groups.end() - 1;
				}
				found->second.push_back(type);
			}
			std::sort(loose.begin(), loose.end(), byPanelName);
			std::sort(groups.begin(), groups.end(),
					  [](const auto& a, const auto& b) { return a.first < b.first; });

			bool firstPanelRow = true;
			for (const TypeInfo* type : loose) {
				items.push_back(makeRow(type, firstPanelRow));
				firstPanelRow = false;
			}
			bool firstGroup = true;
			for (auto& group : groups) {
				std::sort(group.second.begin(), group.second.end(), byPanelName);
				UI::Widgets::MenuItem submenu;
				submenu.label = group.first;
				submenu.separatorBefore = firstGroup;
				for (const TypeInfo* type : group.second) {
					submenu.children.push_back(makeRow(type, false));
				}
				items.push_back(std::move(submenu));
				firstGroup = false;
			}
			if (types.empty()) {
				items.push_back({"(no panel types registered)", "", false, true, {}});
			}
			items.push_back({"Save Layout Now", "", true, true, hooks.onSaveLayout});
			items.push_back({"Reset Layout", "", true, false, [mainWindow]() {
								 EditorLayoutOps::ResetLayout(mainWindow);
							 }});
		};
		menus.push_back(std::move(windowMenu));

		UI::Widgets::MenuBarItemDesc helpMenu;
		helpMenu.label = "Help";
		helpMenu.fill = [shell, contextPtr](Vector<UI::Widgets::MenuItem>& items) {
			items.push_back({"About Shalltear", "", true, false, [shell, contextPtr]() {
								 shell->OpenAboutPopup(*contextPtr);
							 }});
		};
		menus.push_back(std::move(helpMenu));

		UI::ElementHandle titleBar = context.CreateElement();
		context.AppendChild(context.GetRoot(), titleBar);
		context.AddClass(titleBar, EditorTheme::Class::TitleBar);
		context.AddClass(titleBar, EditorChromeOps::CaptionClass);
		UI::Style titleBarStyle;
		titleBarStyle.padding = {8.0f, 0.0f, 0.0f, 0.0f};
		context.SetStyle(titleBar, titleBarStyle);

		auto addZone = [&context, titleBar](bool alignEnd) {
			UI::ElementHandle zone = context.CreateElement();
			context.AppendChild(titleBar, zone);
			context.AddClass(zone, EditorTheme::Class::TitleZone);
			context.AddClass(zone, EditorChromeOps::CaptionClass);
			if (alignEnd) {
				UI::Style zoneStyle;
				zoneStyle.justifyContent = UI::Align::End;
				context.SetStyle(zone, zoneStyle);
			}
			return zone;
		};

		UI::ElementHandle leftZone = addZone(false);

		EditorIcons::AddVector(context, leftZone, EditorIcons::Mark::Logo,
							   UI::IconPalette::Tint(TextStrong()),
							   EditorTheme::GetMetric(MetricToken::MarkTitle), 10.0f, ToolbarTextPx());

		UI::ElementHandle menuBar = UI::Widgets::MenuBar(
			context, leftZone, {menus.data(), menus.size()}, font,
			EditorTheme::GetMetric(MetricToken::FontBody));
		context.AddClass(menuBar, EditorTheme::Class::TitleMenu);

		UI::ElementHandle centerZone = context.CreateElement();
		context.AppendChild(titleBar, centerZone);
		context.AddClass(centerZone, EditorChromeOps::CaptionClass);
		UI::Style centerStyle;
		centerStyle.flexDirection = UI::FlexDirection::Row;
		centerStyle.alignItems = UI::Align::Center;
		centerStyle.flexShrink = 0.0f;
		context.SetStyle(centerZone, centerStyle);

		f32 titleSize = EditorTheme::GetMetric(MetricToken::FontBody);
		auto addTitleRun = [&](std::string_view text, Color color) {
			UI::ElementHandle run = context.CreateElement();
			context.AppendChild(centerZone, run);
			context.SetHoverable(run, false);
			context.SetText(run, font, titleSize, color, text);
		};
		const str build = std::format("Shalltear {}", EngineVersionOps::DisplayLabel());
		if (!_projectName.empty()) {
			addTitleRun(_projectName, TextStrong());
			addTitleRun("  -  " + build, TextDim());
		} else {
			addTitleRun(build, TextDim());
		}

		UI::ElementHandle rightZone = addZone(true);

		auto addCaptionButton = [&context, rightZone](EditorIcons::Icon icon, std::string_view fallback,
													  std::string_view tooltip, Color rest, bool isClose,
													  std::function<void()> onClick) {
			return EditorChromeOps::CaptionButton(context, rightZone, icon, fallback, tooltip,
												  rest, isClose, std::move(onClick));
		};

		PlayModeController* playMode = _playMode;
		_playButtonFace = addCaptionButton(EditorIcons::Glyph::Play, "Play", "Play", TextNormal(), false,
										   [playMode]() {
											   if (playMode) {
												   playMode->RequestToggle();
											   }
										   })
							  .face;

		_pauseButtonFace = addCaptionButton(EditorIcons::Glyph::Pause, "Pause", "Pause", TextNormal(), false,
											[playMode]() {
												if (playMode) {
													playMode->RequestPause();
												}
											})
							   .face;

		addCaptionButton(EditorIcons::Glyph::Step, "Step", "Step one frame", TextNormal(), false,
						 [playMode]() {
							 if (playMode) {
								 playMode->RequestStep();
							 }
						 });

		EditorIcons::IconButtonParts compileParts = addCaptionButton(
			EditorIcons::Glyph::Compile, "Compile", "Compile", ModuleRest(), false,
			[moduleHost]() {
				if (moduleHost) {
					moduleHost->RequestCompile();
				}
			});
		_compileButtonFace = compileParts.face;
		UI::Style compileGap;
		compileGap.margin = {12.0f, 0.0f, 0.0f, 0.0f};
		context.SetStyle(compileParts.button, compileGap);

		_reloadButtonFace = addCaptionButton(EditorIcons::Glyph::Reload, "Reload", "Reload Module", ModuleRest(), false,
											 [moduleHost]() {
												 if (moduleHost && moduleHost->HasModuleBinaryPath()) {
													 moduleHost->RequestReload();
												 } else {
													 SLT_LOG_INFO("Editor", "this project has no C++ module. Scripts > Create C++ Module...");
												 }
											 })
								.face;

		UI::Widgets::Separator(context, rightZone);

		Hooks* hooksPtr = &_hooks;
		_maximizeButtonFace = EditorChromeOps::BuildWindowControls(
								  context, rightZone, _mainWindow,
								  [hooksPtr]() {
									  if (hooksPtr->onRequestQuit) {
										  hooksPtr->onRequestQuit();
									  }
								  })
								  .maximizeFace;

		_layoutBar = context.CreateElement();
		context.AppendChild(context.GetRoot(), _layoutBar);
		context.AddClass(_layoutBar, EditorTheme::Class::LayoutBar);
		BuildLayoutBar(context);

		_dockHost = context.CreateElement();
		context.AppendChild(context.GetRoot(), _dockHost);
		UI::Style hostStyle;
		hostStyle.height = UI::Length::Pixel(0.0f);
		hostStyle.flexGrow = 1.0f;
		f32 gutter = UI::Widgets::GetSplitterThickness();
		hostStyle.padding = {gutter, 0.0f, gutter, 0.0f};
		context.SetStyle(_dockHost, hostStyle);
		context.AddClass(_dockHost, EditorTheme::Class::DockGutter);

		UI::ElementHandle statusBar = context.CreateElement();
		context.AppendChild(context.GetRoot(), statusBar);
		context.AddClass(statusBar, EditorTheme::Class::StatusBar);

		_consoleNotice = EditorChromeOps::BuildConsoleNotice(context, statusBar, []() {
			PanelManager::FocusPanelOfType(StringHash("ConsolePanel"));
		});

		_statusLeft = context.CreateElement();
		context.AppendChild(statusBar, _statusLeft);
		UI::Style readoutStyle;
		readoutStyle.flexShrink = 0.0f;
		readoutStyle.margin = {12.0f, 0.0f, 0.0f, 0.0f};
		context.SetStyle(_statusLeft, readoutStyle);
		context.SetText(_statusLeft, font, EditorTheme::GetMetric(MetricToken::FontBody), TextDim(), "");

		_statusRight = context.CreateElement();
		context.AppendChild(statusBar, _statusRight);
		context.SetStyle(_statusRight, readoutStyle);
		context.SetText(_statusRight, font, EditorTheme::GetMetric(MetricToken::FontBody), TextDim(), "");

		UI::ElementHandle graphicsApi = context.CreateElement();
		context.AppendChild(statusBar, graphicsApi);
		context.SetStyle(graphicsApi, readoutStyle);
		context.SetText(graphicsApi, font, EditorTheme::GetMetric(MetricToken::FontBody), TextDim(), GRAPHICS_API_LABEL);
		str graphicsTooltip = std::format("Graphics API: {}", GRAPHICS_API_LABEL);
		if (RHI::Device* device = RenderSystem::GetDevice()) {
			graphicsTooltip += " on " + device->GetSelectedAdapter().name;
		}
		context.SetTooltip(graphicsApi, graphicsTooltip);
	}

	void EditorShell::OpenAboutPopup(UI::Context& context) {
		Handle<FontAsset> font = UISystem::GetDefaultFont();

		UI::ElementHandle content = context.CreateElement();
		context.AddClass(content, EditorTheme::Class::AboutCard);

		auto addLine = [&](std::string_view text, f32 size, Color color) {
			UI::ElementHandle line = context.CreateElement();
			context.AppendChild(content, line);
			UI::Style lineStyle;
			lineStyle.margin = {0.0f, 0.0f, 0.0f, 6.0f};
			context.SetStyle(line, lineStyle);
			context.SetText(line, font, size, color, text);
		};
		addLine("Shalltear Editor", EditorTheme::GetMetric(MetricToken::FontTitle), TextNormal());
		addLine(std::format("Version {} ({})", EngineVersionOps::ReleaseLabel(), BuildInfo::GetBuildConfig()), EditorTheme::GetMetric(MetricToken::FontRow), TextDim());
		addLine(std::format("Commit {}", BuildInfo::GetGitCommit()), EditorTheme::GetMetric(MetricToken::FontRow), TextDim());
		addLine(std::format("Built {}", BuildInfo::GetBuildTimestamp()), EditorTheme::GetMetric(MetricToken::FontRow), TextDim());

		UI::Context* contextPtr = &context;
		UI::Widgets::ButtonDesc okButton;
		okButton.label = "OK";
		okButton.font = font;
		okButton.fontSizePx = ToolbarTextPx();
		okButton.defaultForPopup = content;
		okButton.onClick = [contextPtr, content]() {
			contextPtr->ClosePopup(content);
		};
		UI::Widgets::Button(context, content, okButton);

		context.Events(content).onPopupClosed = [contextPtr, content]() {
			contextPtr->DestroyElementNextUpdate(content);
		};

		context.OpenPopup(content, {}, UI::PopupFlags::Center | UI::PopupFlags::Scrim);
	}

	void EditorShell::BuildLayoutBar(UI::Context& context) {
		if (!context.IsAlive(_layoutBar)) {
			return;
		}
		context.ClearChildren(_layoutBar);
		_layoutBarDirty = false;

		Handle<FontAsset> font = UISystem::GetDefaultFont();
		const f32 textPx = EditorTheme::GetMetric(MetricToken::FontBody);
		const str active = EditorLayoutOps::GetActiveLayout();
		Vector<str> names{GetMemoryResource(MemTag::Editor)};
		EditorLayoutOps::GetLayoutNames(names);

		UI::Context* contextPtr = &context;
		EditorShell* shell = this;
		Handle<Window> window = _mainWindow;

		for (const str& name : names) {
			const bool isActive = name == active;
			str layoutName = name;
			UI::Widgets::ButtonDesc tab;
			tab.label = name;
			tab.font = font;
			tab.fontSizePx = textPx;
			tab.onClick = [shell, window, layoutName]() {
				EditorLayoutOps::SwitchLayout(window, layoutName);
				shell->_layoutBarDirty = true;
			};
			UI::ElementHandle element = UI::Widgets::Button(context, _layoutBar, tab);
			context.AddClass(element, EditorTheme::Class::ToolButton);
			if (isActive) {
				context.AddClass(element, EditorTheme::Class::ToolButtonOn);
			}
			context.SetTooltip(element, std::format("Switch to the {} layout", name));

			const bool isDefault = layoutName == EditorLayoutOps::DefaultLayoutName;
			context.Events(element).onRightClick = [shell, contextPtr, window, layoutName, isDefault, font, textPx]() {
				Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::UI)};
				items.push_back({"Save Layout", "", true, false, [shell, window, layoutName]() {
									 EditorLayoutOps::SaveLayoutAs(window, layoutName);
									 shell->_layoutBarDirty = true;
								 }});
				items.push_back({"Rename...", "", !isDefault, true, [shell, contextPtr, layoutName]() {
									 shell->OpenLayoutNamePopup(*contextPtr, layoutName);
								 }});
				items.push_back({"Delete", "", !isDefault, false, [shell, layoutName]() {
									 EditorLayoutOps::DeleteLayout(layoutName);
									 shell->_layoutBarDirty = true;
								 }});
				UI::Widgets::OpenMenu(*contextPtr, contextPtr->GetPointerPosition(),
									  {items.data(), items.size()}, font, textPx);
			};
		}

		EditorIcons::IconButtonParts add = EditorIcons::IconButton(
			context, _layoutBar, EditorIcons::Glyph::Add, "+", "Save the current layout as...",
			[shell, contextPtr]() { shell->OpenLayoutNamePopup(*contextPtr, ""); }, textPx);
		context.AddClass(add.button, EditorTheme::Class::ToolButton);
	}

	void EditorShell::OpenLayoutNamePopup(UI::Context& context, std::string_view existing) {
		Handle<FontAsset> font = UISystem::GetDefaultFont();
		const f32 textPx = EditorTheme::GetMetric(MetricToken::FontBody);

		UI::ElementHandle content = context.CreateElement();
		context.AddClass(content, EditorTheme::Class::AboutCard);

		UI::ElementHandle title = context.CreateElement();
		context.AppendChild(content, title);
		UI::Style titleStyle;
		titleStyle.margin = {0.0f, 0.0f, 0.0f, 6.0f};
		context.SetStyle(title, titleStyle);
		context.SetText(title, font, EditorTheme::GetMetric(MetricToken::FontTitle), TextNormal(),
						existing.empty() ? "Save Layout As" : "Rename Layout");

		_layoutNameDraft = str(existing);
		if (existing.empty()) {
			Vector<str> taken{GetMemoryResource(MemTag::Editor)};
			EditorLayoutOps::GetLayoutNames(taken);
			auto isTaken = [&taken](const str& candidate) {
				return std::find(taken.begin(), taken.end(), candidate) != taken.end();
			};
			_layoutNameDraft = "Layout";
			for (u32 suffix = 2; isTaken(_layoutNameDraft); ++suffix) {
				_layoutNameDraft = std::format("Layout {}", suffix);
			}
		}
		str source{existing};
		EditorShell* shell = this;

		UI::Widgets::TextFieldDesc field;
		field.text = _layoutNameDraft;
		field.font = font;
		field.fontSizePx = textPx;
		field.onCommit = [shell](std::string_view text) {
			shell->_layoutNameDraft = str(text);
		};
		UI::ElementHandle input = UI::Widgets::TextField(context, content, field);
		UI::Style inputStyle = context.GetStyle(input);
		inputStyle.margin = {0.0f, 0.0f, 0.0f, 8.0f};
		context.SetStyle(input, inputStyle);

		UI::Context* contextPtr = &context;
		Handle<Window> window = _mainWindow;
		UI::Widgets::ButtonDesc okButton;
		okButton.label = existing.empty() ? "Save" : "Rename";
		okButton.font = font;
		okButton.fontSizePx = textPx;
		okButton.onClick = [shell, contextPtr, content, window, source]() {
			if (source.empty()) {
				EditorLayoutOps::SaveLayoutAs(window, shell->_layoutNameDraft);
			} else {
				EditorLayoutOps::RenameLayout(source, shell->_layoutNameDraft);
			}
			shell->_layoutBarDirty = true;
			contextPtr->ClosePopup(content);
		};
		UI::Widgets::Button(*contextPtr, content, okButton);

		context.OpenPopup(content, context.GetPointerPosition());
	}

	void EditorShell::Tick(f32 deltaSeconds) {
		if (!_built) {
			return;
		}
		UI::Context* context = UISystem::GetContext(_mainWindow);
		if (!context) {
			return;
		}

		if (_layoutBarDirty) {
			BuildLayoutBar(*context);
		}

		if (Window* window = _mainWindow.Get()) {
			bool maximized = window->IsMaximized();
			if (maximized != _wasMaximized) {
				_wasMaximized = maximized;
				EditorChromeOps::SetMaximizeGlyph(*context, _maximizeButtonFace, maximized);
			}
		}

		RefreshState(*context);

		_frameRate.Add(deltaSeconds);
		if (_frameRate.GetWindowSeconds() < STATUS_REFRESH_SECONDS) {
			return;
		}
		RefreshStatus(*context);
		_frameRate.Reset();
	}

	void EditorShell::RefreshState(UI::Context& context) {
		ChromeState now;
		now.playing = _playMode && _playMode->IsPlaying();
		now.paused = _playMode && _playMode->IsPaused();
		now.reloadHeld = _moduleHost && _moduleHost->HasHeldReload();
		now.moduleLoaded = _moduleHost && _moduleHost->IsModuleLoaded();
		now.moduleConfigured = _moduleHost && _moduleHost->HasModuleBinaryPath();
		if (_compileService) {
			now.compile = _compileService->GetState();
			now.compileErrors = _compileService->GetErrorCount();
			now.compileWarnings = _compileService->GetWarningCount();
		}
		now.selected = EditorSelection::GetSelectionCount();
		if (now.selected == 0) {
			now.selected = EditorSelection::GetAssetSelectionCount();
		}

		if (_chromeStateWritten && now == _chromeState) {
			return;
		}
		_chromeState = now;
		_chromeStateWritten = true;

		EditorIcons::SetGlyphOrText(context, _playButtonFace,
									now.playing ? EditorIcons::Glyph::Stop : EditorIcons::Glyph::Play,
									now.playing ? "Stop" : "Play", TextNormal(), ToolbarTextPx(), ToolbarTextPx());
		EditorIcons::SetGlyphOrText(context, _pauseButtonFace,
									now.paused ? EditorIcons::Glyph::Play : EditorIcons::Glyph::Pause,
									now.paused ? "Resume" : "Pause", TextNormal(), ToolbarTextPx(), ToolbarTextPx());
		context.SetTooltip(ParentOf(context, _playButtonFace), now.playing ? "Stop" : "Play");
		context.SetTooltip(ParentOf(context, _pauseButtonFace), now.paused ? "Resume" : "Pause");
		if (context.IsAlive(_compileButtonFace)) {
			CompileControlText text = DescribeCompileState(now.compile, now.compileErrors, now.compileWarnings);
			EditorIcons::SetGlyphOrText(context, _compileButtonFace, EditorIcons::Glyph::Compile,
										text.status, CompileGlyphColor(now.compile, now.compileWarnings),
										ToolbarTextPx(), ToolbarTextPx());
			context.SetTooltip(ParentOf(context, _compileButtonFace), text.tooltip);
		}
		if (context.IsAlive(_reloadButtonFace)) {
			EditorIcons::SetGlyphOrText(context, _reloadButtonFace, EditorIcons::Glyph::Reload,
										now.reloadHeld ? "Reload Now" : "Reload",
										now.reloadHeld ? StatusWarn() : ModuleRest(),
										ToolbarTextPx(), ToolbarTextPx());
			context.SetTooltip(ParentOf(context, _reloadButtonFace),
							   now.reloadHeld ? "Reload Now (a compile finished while playing)"
											  : "Reload Module");
		}

		if (context.IsAlive(_statusRight)) {
			str moduleStatus = "no module";
			if (now.moduleLoaded) {
				moduleStatus = "module loaded";
			} else if (now.moduleConfigured) {
				moduleStatus = "module not loaded";
			}
			context.SetTextString(_statusRight,
								  std::format("{}   {} selected   {}",
											  moduleStatus, now.selected,
											  now.paused ? "PAUSED" : (now.playing ? "PLAYING" : "editing")));
		}
	}

	void EditorShell::RefreshStatus(UI::Context& context) {
		ConsoleSummary summary = EditorConsole::GetSummary(LogLevel::Info);
		EditorChromeOps::SetConsoleNotice(context, _consoleNotice, summary);

		if (context.IsAlive(_statusLeft)) {
			context.SetTextString(_statusLeft,
								  std::format("{:.0f} FPS ({:.2f} ms)   {} MiB",
											  _frameRate.GetFps(), _frameRate.GetMeanMs(),
											  MemoryManager::GetTotalBytes() >> 20));
		}
	}

} // namespace Shalltear
