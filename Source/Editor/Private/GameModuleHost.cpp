#include "GameModuleHost.h"

#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/Settings.h"
#include "Shalltear/Core/StringUtil.h"
#include "Shalltear/Core/Time.h"
#include "Shalltear/Core/UI/UISystem.h"

#include "Shalltear/Editor/BuildPipeline.h"
#include "Shalltear/Editor/CompileService.h"
#include "Shalltear/Editor/EditorEvents.h"
#include "Shalltear/Editor/EditorExtensions.h"
#include "Shalltear/Editor/PackageOps.h"
#include "Shalltear/Editor/ScriptProjectOps.h"
#include "Shalltear/Editor/TypeNameOps.h"

#include "EditorConfig.h"

#include <filesystem>
#include <fstream>

namespace Shalltear {

	namespace {

		constexpr f64 RELOAD_DEBOUNCE_SECONDS = 0.2;

		constexpr f64 SOURCE_DEBOUNCE_SECONDS = 0.5;

		bool IsCompileTriggeringFile(const str& fileName) {
			str lower = StringUtil::ToLower(fileName);
			if (lower == "cmakelists.txt") {
				return true;
			}
			for (const char* extension : {".h", ".hpp", ".inl", ".cpp", ".cc", ".cxx", ".cs"}) {
				if (lower.ends_with(extension)) {
					return true;
				}
			}
			return false;
		}

		f64 SecondsSince(u64 ticks) {
			return static_cast<f64>(Time::GetTicks() - ticks) / static_cast<f64>(Time::GetTicksPerSecond());
		}

	} // anonymous namespace

	namespace {
		GameModuleHost* _activeHost = nullptr;

		class SettingsWatcher : public Object {
		public:
			void OnSettingChanged(const SettingChangedEvent& event) {
				if (_activeHost) {
					_activeHost->ApplyChangedSetting(event.categoryPath, event.fieldName);
				}
			}
		};

		SettingsWatcher* _settingsWatcher = nullptr;
	} // namespace

	GameModuleHost* GameModuleHost::GetActive() {
		return _activeHost;
	}

	void GameModuleHost::Initialize(const Config& config, CompileService* compileService) {
		_activeHost = this;

		if (!_settingsWatcher) {
			_settingsWatcher = New<SettingsWatcher>(MemTag::Editor);
			Handle<SettingsWatcher> self(_settingsWatcher);
			SettingsEvents::Changed.Subscribe(EventBus::GetGlobal(), self, &SettingsWatcher::OnSettingChanged);
		}

		_projectPath = config.projectPath;
		_autoCompile = config.autoCompile;
		_externalEditorCommand = config.externalEditorCommand;
		_loadedPackages = config.loadedPackages;
		_gamePackages = config.gamePackages;
		_compileService = compileService;
		_active = !_projectPath.empty() || !config.modulePath.empty();

		ArmSourceWatcher();

		if (config.modulePath.empty()) {
			SLT_LOG_INFO("Editor", "no C++ module in this project. Scripts > Create C++ Module... adds one");
			return;
		}

		_modulePathString = config.modulePath;
		_modulePath = Path(config.modulePath);

		const bool admitted = AdmitsModuleBinary();
		if (admitted) {
			_module = ModuleSystem::Load(_modulePath);
		}
		if (ModuleSystem::IsLoaded(_module)) {
			EditorExtensions::Load(ModuleSystem::GetNativeHandle(_module));
		}

		ArmBinaryWatcher();
		TypeNameOps::Refresh();

		_pendingStartupCompile = _autoCompile && _compileService != nullptr &&
								 (!admitted || ScriptProjectOps::IsModuleBinaryOutOfDate(
												   _projectPath, ScriptProjectOps::GetActiveConfigName()));

		if (_pendingStartupCompile && !ModuleSystem::IsLoaded(_module)) {
			_holdingUnresolvedReports = true;
			_heldBinaryTime = ModuleBinaryTime();
			SceneSystem::HoldUnresolvedComponentReports();
		}

		if (!admitted) {
			if (_pendingStartupCompile) {
				SLT_LOG_INFO("Editor", "compiling this project's C++ again now");
			}
		} else if (!ModuleSystem::IsLoaded(_module)) {
			SLT_LOG_INFO("Editor", "this project's C++ hasn't been compiled yet. {}",
						 _pendingStartupCompile
							 ? "Compiling it now. Your components appear in the editor when it finishes"
							 : "Scripts > Compile (F7) builds it");
		} else if (_pendingStartupCompile) {
			SLT_LOG_INFO("Editor", "this project's C++ changed since it was last compiled. Compiling it now");
		}

		SLT_LOG_INFO("Editor", "watching {} (Ctrl+R reload, F7 compile, auto-compile {})",
					 config.modulePath, _autoCompile ? "on" : "off");
	}

	void GameModuleHost::RearmScriptProject() {
		if (_projectPath.empty()) {
			return;
		}
		_active = true;

		if (_modulePath.ToString().empty() && ScriptProjectOps::HasScriptProject(_projectPath)) {
			_modulePathString = ScriptProjectOps::GetModuleBinaryPath(_projectPath, ScriptProjectOps::GetActiveConfigName());
			_modulePath = Path(_modulePathString);
			ArmBinaryWatcher();
			SLT_LOG_INFO("Editor", "game module host armed: {}", _modulePathString);
		}
		if (!_sourceWatcher) {
			ArmSourceWatcher();
		}
	}

	void GameModuleHost::ArmBinaryWatcher() {
		std::error_code ec;
		std::filesystem::path physical = std::filesystem::absolute(_modulePath.CStr(), ec);
		_moduleFileName = physical.filename().string();
		std::filesystem::path directory = physical.parent_path();
		std::filesystem::create_directories(directory, ec);
		if (!std::filesystem::is_directory(directory, ec)) {
			SLT_LOG_WARN("Editor", "game module directory not watchable: {}", directory.generic_string());
			return;
		}
		_binaryWatcher.emplace(Path(directory.generic_string()), [this](const Path& changed, FileAction action) {
			if (action == FileAction::Removed) {
				return;
			}
			// worker thread, only stamp + flag
			if (StringUtil::ToLower(changed.GetFilename()) == StringUtil::ToLower(_moduleFileName)) {
				std::lock_guard lock(_pendingMutex);
				_pendingReload = true;
				_lastBinaryChangeTicks = Time::GetTicks();
			}
		});
	}

	void GameModuleHost::ArmSourceWatcher() {
		if (_projectPath.empty() || !_compileService) {
			return;
		}
		str sourceDirectory = ScriptProjectOps::GetSourceDirectory(_projectPath);
		std::error_code ec;
		if (!std::filesystem::is_directory(std::filesystem::path(sourceDirectory), ec)) {
			return;
		}
		_sourceWatcher.emplace(Path(sourceDirectory), [this](const Path& changed, FileAction action) {
			if (action == FileAction::Removed) {
				return;
			}
			if (!IsCompileTriggeringFile(changed.GetFilename())) {
				return;
			}
			std::lock_guard lock(_pendingMutex);
			_pendingCompile = true;
			_lastSourceChangeTicks = Time::GetTicks();
		});
	}

	void GameModuleHost::Terminate() {
		ReleaseUnresolvedReports();
		if (ModuleSystem::IsLoaded(_module)) {
			EditorExtensions::Unload(GetModuleBase());
		}

		_binaryWatcher.reset();
		_sourceWatcher.reset();
		// never kill a running compile (it owns the CMake tree)
		_compileService = nullptr;
		_loadedPackages.clear();
		_loadedPackages.shrink_to_fit();
		_gamePackages.clear();
		_gamePackages.shrink_to_fit();
		_active = false;
		if (_activeHost == this) {
			_activeHost = nullptr;
		}
		if (_settingsWatcher) {
			EventBus::GetGlobal().UnsubscribeAll(_settingsWatcher->GetInstanceId());
			Delete(_settingsWatcher);
			_settingsWatcher = nullptr;
		}
	}

	void GameModuleHost::ApplyChangedSetting(std::string_view categoryPath, std::string_view fieldName) {
		if (categoryPath != "Editor/Scripting") {
			return;
		}

		EditorConfig::ScriptPreferences preferences = EditorConfig::ReadScriptPreferences();
		if (fieldName == "autoCompile") {
			if (_autoCompile == preferences.autoCompile) {
				return;
			}
			_autoCompile = preferences.autoCompile;
			SLT_LOG_INFO("Editor", "auto-compile is now {}", _autoCompile ? "on" : "off");
			return;
		}
		if (fieldName == "externalEditorCommand") {
			_externalEditorCommand = preferences.externalEditorCommand;
		}
	}

	void GameModuleHost::PumpMainThread() {
		if (!_active) {
			return;
		}

		if (_pendingStartupCompile) {
			_pendingStartupCompile = false;
			RequestCompile();
		}
		SettleUnresolvedReports();

		bool compileNow = false;
		bool reloadNow = false;
		{
			std::lock_guard lock(_pendingMutex);
			if (_pendingCompile && _autoCompile && SecondsSince(_lastSourceChangeTicks) >= SOURCE_DEBOUNCE_SECONDS) {
				_pendingCompile = false;
				compileNow = true;
			}
			if (_pendingReload && SecondsSince(_lastBinaryChangeTicks) >= RELOAD_DEBOUNCE_SECONDS) {
				_pendingReload = false;
				reloadNow = true;
			}
		}

		if (compileNow) {
			RequestCompile();
		}
		if (reloadNow) {
			if (_playing) {
				_heldReload = true;
				SLT_LOG_INFO("Editor", "a new module binary appeared during play (reload held until Stop)");
			} else {
				ReloadNow();
			}
		}
	}

	void GameModuleHost::SetPlaying(bool playing) {
		if (_playing == playing) {
			return;
		}
		_playing = playing;
		if (!_playing && _heldReload) {
			_heldReload = false;
			ReloadNow();
		}
	}

	void GameModuleHost::RequestReload() {
		if (!_active || !HasModuleBinaryPath()) {
			return;
		}
		_heldReload = false;
		ReloadNow();
	}

	void GameModuleHost::ReloadNow() {
		if (const BuildPipeline* pipeline = BuildPipeline::GetActive(); pipeline && pipeline->IsBusy()) {
			std::lock_guard lock(_pendingMutex);
			_pendingReload = true;
			_lastBinaryChangeTicks = Time::GetTicks();
			SLT_LOG_INFO("Editor", "a new module binary appeared during a build (reload held until it finishes)");
			return;
		}

		{
			std::ifstream probe(_modulePath.CStr(), std::ios::binary);
			if (!probe.is_open()) {
				std::lock_guard lock(_pendingMutex);
				_pendingReload = true;
				_lastBinaryChangeTicks = Time::GetTicks();
				return;
			}
		}

		if (!AdmitsModuleBinary()) {
			ReleaseUnresolvedReports();
			return;
		}

		if (!ModuleSystem::IsLoaded(_module)) {
			_module = ModuleSystem::Load(_modulePath);
			if (ModuleSystem::IsLoaded(_module)) {
				EditorExtensions::Load(ModuleSystem::GetNativeHandle(_module));
			}
			TypeNameOps::Refresh();
			ReleaseUnresolvedReports();
			return;
		}

		EventBus& bus = EventBus::GetGlobal();
		EditorEvents::ModuleReloadBegan.Publish(bus, {ModuleSystem::GetNativeHandle(_module)});
		UISystem::CancelDragPayload();
		bool succeeded = ModuleSystem::Reload(_module);
		TypeNameOps::Refresh();
		EditorEvents::ModuleReloadEnded.Publish(bus, {succeeded, ModuleSystem::GetNativeHandle(_module)});
	}

	std::filesystem::file_time_type GameModuleHost::ModuleBinaryTime() const {
		std::error_code ec;
		std::filesystem::file_time_type time = std::filesystem::last_write_time(std::filesystem::path(_modulePathString), ec);
		return ec ? std::filesystem::file_time_type{} : time;
	}

	void GameModuleHost::SettleUnresolvedReports() {
		if (!_holdingUnresolvedReports || _pendingStartupCompile || !_compileService || _compileService->IsBusy()) {
			return;
		}
		const CompileState state = _compileService->GetState();
		if (state != CompileState::Succeeded || ModuleBinaryTime() == _heldBinaryTime) {
			ReleaseUnresolvedReports();
		}
	}

	void GameModuleHost::ReleaseUnresolvedReports() {
		if (!_holdingUnresolvedReports) {
			return;
		}
		_holdingUnresolvedReports = false;
		SceneSystem::ReleaseUnresolvedComponentReports();
	}

	bool GameModuleHost::AdmitsModuleBinary() const {
		Vector<PackageOps::PackageManifest> catalogue;
		Vector<str> diagnostics;
		PackageOps::LoadBuiltInCatalogue(catalogue, diagnostics);

		Vector<str> unloaded;
		if (!PackageOps::FindUnloadedPackageImports(_modulePath, catalogue, _loadedPackages, unloaded) || unloaded.empty()) {
			return true;
		}

		str names;
		for (size_t index = 0; index < unloaded.size(); ++index) {
			const PackageOps::PackageManifest* manifest = PackageOps::FindPackage(catalogue, unloaded[index]);
			const str& name = manifest && !manifest->displayName.empty() ? manifest->displayName : unloaded[index];
			if (index > 0) {
				names += index + 1 == unloaded.size() ? " and " : ", ";
			}
			names += name;
		}
		const bool one = unloaded.size() == 1;
		SLT_LOG_WARN("Editor",
					 "the editor didn't load this project's C++. It uses {}, which {} not loaded in this session. "
					 "Turn {} on in the Package Manager, restart the editor (or remove the code that uses {})",
					 names, one ? "is" : "are", one ? "it" : "them", one ? "it" : "them");
		return false;
	}

	void GameModuleHost::RequestCompile() {
		if (!_active || !_compileService) {
			return;
		}
		if (_projectPath.empty()) {
			SLT_LOG_WARN("Editor", "no project is mounted. Nothing to compile");
			return;
		}
		CompileRequest request;
		request.projectPath = _projectPath;
		request.enabledPackages.assign(_gamePackages.begin(), _gamePackages.end());
		_compileService->RequestCompile(request);
	}

	void GameModuleHost::RequestRecompile() {
		if (!_active || !_compileService || _projectPath.empty()) {
			return;
		}
		CompileRequest request;
		request.projectPath = _projectPath;
		request.recompile = true;
		request.enabledPackages.assign(_gamePackages.begin(), _gamePackages.end());
		_compileService->RequestCompile(request);
	}

} // namespace Shalltear
