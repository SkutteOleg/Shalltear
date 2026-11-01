#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Module/ModuleSystem.h"
#include "Shalltear/Core/Types.h"

#include <filesystem>
#include <mutex>
#include <optional>

namespace Shalltear {

	class CompileService;

	class GameModuleHost {
	public:
		struct Config {
			str modulePath;
			str projectPath;
			bool autoCompile = true;
			str externalEditorCommand;
			Vector<str> loadedPackages;
			Vector<str> gamePackages;
		};

		void Initialize(const Config& config, CompileService* compileService);

		void Terminate();

		void PumpMainThread();

		void RearmScriptProject();

		void RequestReload();
		void RequestCompile();
		void RequestRecompile();

		static GameModuleHost* GetActive();

		bool IsActive() const { return _active; }
		bool HasModuleBinaryPath() const { return !_modulePathString.empty(); }
		bool IsModuleLoaded() const { return ModuleSystem::IsLoaded(_module); }

		const void* GetModuleBase() const { return ModuleSystem::GetNativeHandle(_module); }
		const str& GetModulePath() const { return _modulePathString; }
		const str& GetProjectPath() const { return _projectPath; }

		void SetPlaying(bool playing);
		bool HasHeldReload() const { return _heldReload; }

		bool IsAutoCompileEnabled() const { return _autoCompile; }
		void SetAutoCompileEnabled(bool enabled) { _autoCompile = enabled; }

		void ApplyChangedSetting(std::string_view categoryPath, std::string_view fieldName);

		const str& GetExternalEditorCommand() const { return _externalEditorCommand; }
	private:
		void ReloadNow();
		void ArmBinaryWatcher();
		void ArmSourceWatcher();

		bool AdmitsModuleBinary() const;
		std::filesystem::file_time_type ModuleBinaryTime() const;
		void SettleUnresolvedReports();
		void ReleaseUnresolvedReports();

		bool _active = false;
		ModuleHandle _module;
		Path _modulePath;
		str _modulePathString;
		str _projectPath;
		str _moduleFileName;
		bool _autoCompile = true;
		str _externalEditorCommand;
		Vector<str> _loadedPackages;
		Vector<str> _gamePackages;
		bool _playing = false;
		bool _heldReload = false;
		bool _pendingStartupCompile = false;
		bool _holdingUnresolvedReports = false;
		std::filesystem::file_time_type _heldBinaryTime{};

		CompileService* _compileService = nullptr;
		std::optional<FileWatcher> _binaryWatcher;
		std::optional<FileWatcher> _sourceWatcher;
		std::optional<FileWatcher> _assetsWatcher;

		std::mutex _pendingMutex;
		bool _pendingReload = false;
		u64 _lastBinaryChangeTicks = 0;
		bool _pendingCompile = false;
		u64 _lastSourceChangeTicks = 0;
	};

} // namespace Shalltear
