#pragma once

#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/Types.h"
#include "Shalltear/Core/UI/Context.h"

#include "Shalltear/Editor/CompileService.h"
#include "Shalltear/Editor/EditorChromeOps.h"
#include "Shalltear/Editor/EditorFrameRate.h"

#include <functional>

namespace Shalltear {

	class Window;
	class CompileService;
	class GameModuleHost;
	class PlayModeController;

	class EditorShell {
	public:
		struct Hooks {
			std::function<void()> onRequestQuit;
			std::function<void()> onRequestRestart;
			std::function<void()> onSaveLayout;
		};

		SHALLTEAR_EDITOR_API void Initialize(Handle<Window> mainWindow, str projectName, GameModuleHost* moduleHost,
											 PlayModeController* playMode, CompileService* compileService, Hooks hooks);
		SHALLTEAR_EDITOR_API void Terminate();

		SHALLTEAR_EDITOR_API void Tick(f32 deltaSeconds);

		bool IsBuilt() const { return _built; }
	private:
		void BuildChrome(UI::Context& context);
		void BuildLayoutBar(UI::Context& context);
		void OpenLayoutNamePopup(UI::Context& context, std::string_view existing);
		void OpenAboutPopup(UI::Context& context);
		void OpenToolchainPopup(UI::Context& context);
		void OpenCreateComponentPopup(UI::Context& context);
		void OpenCreateModulePopup(UI::Context& context);
		void RefreshState(UI::Context& context);
		void RefreshStatus(UI::Context& context);
		void RefreshTasks(UI::Context& context);

		Handle<Window> _mainWindow;
		str _projectName;
		GameModuleHost* _moduleHost = nullptr;
		PlayModeController* _playMode = nullptr;
		CompileService* _compileService = nullptr;
		Hooks _hooks;

		UI::ElementHandle _dockHost;
		UI::ElementHandle _layoutBar;
		bool _layoutBarDirty = false;
		str _layoutNameDraft;
		UI::ElementHandle _playButtonFace;
		UI::ElementHandle _pauseButtonFace;
		UI::ElementHandle _maximizeButtonFace;
		bool _wasMaximized = false;
		UI::ElementHandle _compileButtonFace;
		UI::ElementHandle _reloadButtonFace;
		UI::ElementHandle _statusLeft;
		UI::ElementHandle _statusRight;
		EditorChromeOps::ConsoleNoticeParts _consoleNotice;

		struct TaskNotice {
			UI::ElementHandle root;
			UI::ElementHandle label;
			UI::ElementHandle track;
			UI::ElementHandle fill;
			UI::ElementHandle cancel;
			u64 generation = 0;
			u64 shown = 0;
			bool written = false;
		};
		TaskNotice _taskNotice;

		struct ChromeState {
			bool playing = false;
			bool paused = false;
			bool reloadHeld = false;
			bool moduleLoaded = false;
			bool moduleConfigured = false;
			CompileState compile = CompileState::Idle;
			u32 compileErrors = 0;
			u32 compileWarnings = 0;
			u32 selected = 0;

			bool operator==(const ChromeState&) const = default;
		};
		ChromeState _chromeState;
		bool _chromeStateWritten = false;

		EditorFrameRate::Meter _frameRate;
		bool _built = false;
	};

} // namespace Shalltear
