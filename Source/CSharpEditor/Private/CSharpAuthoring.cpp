#include "CSharpAuthoring.h"

#include "CSharpNativeTypes.h"
#include "CSharpProjectFiles.h"
#include "CSharpReload.h"
#include "CSharpSettings.h"
#include "DotNetToolchain.h"
#include "ManagedTypes.h"
#include "SHA512.h"

#include "Shalltear/Core/BuildInfo.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Object.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/StringUtil.h"
#include "Shalltear/Core/Time.h"

#include "Shalltear/Editor/ChildProcess.h"
#include "Shalltear/Editor/CompileStatus.h"
#include "Shalltear/Editor/EditorConsole.h"
#include "Shalltear/Editor/EditorEvents.h"
#include "Shalltear/Editor/ScriptProjectOps.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <vector>

namespace Shalltear::CSharpEditor::CSharpAuthoring {

	namespace {

		namespace fs = std::filesystem;

		constexpr f64 PROPS_POLL_SECONDS = 0.25;

		CSharpCompile _compile;
		Options _options;
		const void* _owner = nullptr;
		bool _active = false;
		bool _startedServers = false;
		std::optional<FileWatcher> _assetsWatcher;
		i64 _propsTime = 0;
		i64 _userBindingsTime = 0;
		u64 _lastPollTicks = 0;
		u32 _describedRequests = 0;
		u32 _handledRequests = 0;
		str _pendingStamp;
		i64 _loadedOutputTime = 0;
		bool _holdingReports = false;
		std::mutex _pendingMutex;
		bool _pendingCompile = false;
		bool _definitionsChanged = false;
		u64 _lastChangeTicks = 0;
		const void* _moduleBase = nullptr;
		bool _moduleReloaded = false;
		bool _heldForModule = false;

		class ModuleListener : public Object {
		public:
			void OnModuleReloadEnded(const EditorModuleReloadEndedEvent& event) { _moduleReloaded = _moduleReloaded || !TypeRegistry::IsOwnerToken(event.moduleBase); }
		};
		ModuleListener* _moduleListener = nullptr;

		str _buildFile;
		std::vector<str> _assemblyProjects;
		std::vector<str> _assemblyNames;

		f64 SecondsSince(u64 ticks) {
			return static_cast<f64>(Time::GetTicks() - ticks) / static_cast<f64>(Time::GetTicksPerSecond());
		}

		i64 ReadWriteTime(const fs::path& file) {
			std::error_code ec;
			const fs::file_time_type time = fs::last_write_time(file, ec);
			return ec ? 0 : static_cast<i64>(time.time_since_epoch().count());
		}

		i64 ReadPropsTime() {
			return ReadWriteTime(fs::path(_options.projectPath) / "CSharp.props");
		}

		i64 ReadUserBindingsTime() {
			return ReadWriteTime(fs::path(_options.projectPath) / CSharpAssemblies::NativeFolder / "UserBindings.g.cs");
		}

		str DescribeFile(const fs::path& file) {
			std::error_code ec;
			const std::uintmax_t size = fs::file_size(file, ec);
			return std::format("{} {}", ec ? 0 : size, ReadWriteTime(file));
		}

		str DescribeSources() {
			const fs::path root(_options.projectPath);
			Vector<str> rows{GetMemoryResource(MemTag::Editor)};
			std::error_code ec;
			for (fs::recursive_directory_iterator it(root / "Assets", fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
				 it.increment(ec)) {
				std::error_code entry;
				const str extension = StringUtil::ToLower(it->path().extension().generic_string());
				if (it->is_regular_file(entry) && (extension == ".cs" || extension == CSharpAssemblies::Extension)) {
					rows.push_back(std::format("{} {}", it->path().lexically_relative(root).generic_string(), DescribeFile(it->path())));
				}
			}
			const fs::path nativeDir = root / CSharpAssemblies::NativeFolder;
			for (const char* generated : {"NativeTypes.g.cs", "UserBindings.g.cs"}) {
				std::error_code entry;
				const fs::path file = nativeDir / generated;
				if (fs::is_regular_file(file, entry)) {
					rows.push_back(std::format("{} {}", file.lexically_relative(root).generic_string(), DescribeFile(file)));
				}
			}
			std::sort(rows.begin(), rows.end());
			SHA512 hash;
			for (const str& row : rows) {
				hash.Update({reinterpret_cast<const u8*>(row.data()), row.size() + 1});
			}
			return hash.Finish();
		}

		fs::path GetStampFile() {
			return fs::path(_compile.GetOutputAssembly()).replace_extension(".stamp");
		}

		str DescribeBuild() {
			const fs::path managed = fs::path(DotNetToolchain::GetRuntimeAssemblyPath()).parent_path();
			return std::format("{}\n{} {} {}\n{}\n{}\n", _compile.ComputeRestoreStamp(),
							   CSharpSettings::GetUser().codeOptimization == ScriptOptimization::Release ? "Release" : "Debug",
							   DescribeFile(managed / "Shalltear.Runtime.dll"), DescribeFile(managed / "Shalltear.Generators.dll"), _options.projectPath,
							   DescribeSources());
		}

		void DescribeStartedCompile() {
			if (_compile.GetRequestCount() != _describedRequests) {
				_describedRequests = _compile.GetRequestCount();
				_pendingStamp = DescribeBuild();
			}
		}

		void WriteBuildStamp() {
			if (!FileSystem::WriteTextFile(Path(GetStampFile().generic_string()), _pendingStamp)) {
				SLT_LOG_WARN("C#", "couldn't write '{}' (scripts compile again at next open)", GetStampFile().generic_string());
			}
		}

		bool IsLastBuildCurrent() {
			std::error_code ec;
			if (_compile.ReadOutputTime() == 0 || !fs::is_regular_file(GetStampFile(), ec)) {
				return false;
			}
			return FileSystem::ReadTextFile(Path(GetStampFile().generic_string()), MemTag::Editor) == DescribeBuild();
		}

		void ReleaseReports() {
			if (_holdingReports) {
				_holdingReports = false;
				SceneSystem::ReleaseUnresolvedComponentReports();
			}
		}

		void OpenBuild() {
			if (IsLastBuildCurrent()) {
				_loadedOutputTime = _compile.ReadOutputTime();
				CSharpReload::Request();
				CSharpReload::Pump();
				return;
			}
			_holdingReports = true;
			SceneSystem::HoldUnresolvedComponentReports();
			RequestCompile();
		}

		void HandleFinishedCompile() {
			if (_compile.IsBusy() || _compile.GetRequestCount() == _handledRequests) {
				return;
			}
			_handledRequests = _compile.GetRequestCount();
			if (_compile.GetState() != CompileState::Succeeded) {
				ReleaseReports();
				return;
			}
			const i64 output = _compile.ReadOutputTime();
			WriteBuildStamp();
			if (output != _loadedOutputTime) {
				_loadedOutputTime = output;
				CSharpReload::Request();
			}
		}

		void ApplyCompileOptions() {
			const DotNetToolchain::Pin& pin = DotNetToolchain::GetPin();
			CompileOptions compile;
			compile.projectPath = _options.projectPath;
			compile.projectFile = _options.projectFile;
			compile.buildFile = _buildFile;
			compile.assemblyProjects.assign(_assemblyProjects.begin(), _assemblyProjects.end());
			compile.assemblies.assign(_assemblyNames.begin(), _assemblyNames.end());
			compile.muxer = DotNetToolchain::GetMuxerPath(_options.sdkRoot);
			compile.pinStamp = std::format("{} {} {}", pin.sdkVersion, pin.runtimeVersion, BuildInfo::GetGitCommit());
			compile.optimization = CSharpSettings::GetUser().codeOptimization;
			compile.aotAnalysis = CSharpSettings::GetUser().aotAnalysis;
			compile.buildServers = _options.buildServers;
			_compile.Configure(std::move(compile));
		}

		void ReadAssemblies(bool write) {
			const CSharpProjectFiles::Options files = CSharpProjectFiles::GetDefaultOptions(_options.projectPath, _options.sdkRoot);
			for (const CSharpAssemblies::Problem& problem : files.assemblies.problems) {
				ConsoleOrigin origin;
				origin.file = (fs::path(_options.projectPath) / problem.file).generic_string();
				EditorConsole::Push(LogLevel::Error, "C#", std::format("'{}' {}", problem.file, problem.message), origin);
			}
			if (write) {
				CSharpProjectFiles::Result written;
				CSharpProjectFiles::Write(files, written);
			}
			_assemblyProjects.clear();
			_assemblyNames.clear();
			_buildFile.clear();
			if (!files.assemblies.definitions.empty()) {
				_buildFile = CSharpProjectFiles::GetBuildFile(files);
				for (const CSharpAssemblies::Definition& definition : files.assemblies.definitions) {
					_assemblyProjects.push_back(CSharpProjectFiles::GetAssemblyProjectFile(definition.name));
					_assemblyNames.push_back(definition.name);
				}
				_assemblyNames.push_back(fs::path(_options.projectFile).replace_extension().generic_string());
			}
			ApplyCompileOptions();
			Vector<str> outputs{GetMemoryResource(MemTag::Editor)};
			_compile.GetOutputAssemblies(outputs);
			CSharpReload::SetAssemblies(outputs);
		}

		void ArmWatcher() {
			std::error_code ec;
			const fs::path assets = fs::path(_options.projectPath) / "Assets";
			if (fs::is_directory(assets, ec)) {
				_assetsWatcher.emplace(Path(assets.generic_string()), [](const Path& changed, FileAction) {
					const str extension = StringUtil::ToLower(changed.GetExtension());
					if (extension == CSharpAssemblies::Extension) {
						NotifyDefinitionsChanged();
					} else if (extension.empty()) {
						std::lock_guard lock(_pendingMutex);
						_definitionsChanged = true;
					} else if (extension == ".cs") {
						NotifySourceChanged();
					}
				});
			}
			_propsTime = ReadPropsTime();
		}

		void PollUserProps() {
			if (!_options.watchSources || SecondsSince(_lastPollTicks) < PROPS_POLL_SECONDS) {
				return;
			}
			_lastPollTicks = Time::GetTicks();
			const i64 time = ReadPropsTime();
			if (time != _propsTime) {
				_propsTime = time;
				NotifySourceChanged();
			}
			const i64 bindingsTime = ReadUserBindingsTime();
			if (bindingsTime != _userBindingsTime) {
				_userBindingsTime = bindingsTime;
				NotifyDefinitionsChanged();
			}
		}

		CompileStatus::SourceState ReadState(void*) {
			CompileStatus::SourceState state;
			state.compile = _compile.GetState();
			state.errors = _compile.GetErrorCount();
			state.warnings = _compile.GetWarningCount();
			state.reloadHeld = CSharpReload::IsHeld();
			state.typeOwner = CSharpImpl::ManagedTypes::GetOwnerToken();
			return state;
		}

		void CompileNow(void*) {
			RequestCompile();
		}

		void ReloadNow(void*) {
			CSharpReload::ReloadNow();
		}

		str ReadHeading(void*) {
			return fs::path(_options.projectFile).stem().generic_string();
		}

	} // anonymous namespace

	void Begin(const Options& options, const void* owner) {
		Shutdown();
		_options = options;
		_owner = owner;
		_active = true;
		ApplyCompileOptions();
		if (_options.watchSources) {
			ArmWatcher();
		}
		CompileStatus::RegisterSource({"C#", &ReadState, &CompileNow, &ReloadNow, &ReadHeading, nullptr}, owner);
		CSharpReload::Begin(_compile.GetOutputAssembly());
		_moduleBase = ScriptProjectOps::GetLoadedModuleBase();
		_userBindingsTime = ReadUserBindingsTime();
		_moduleListener = New<ModuleListener>(MemTag::Editor);
		EditorEvents::ModuleReloadEnded.Subscribe(EventBus::GetGlobal(), Handle<ModuleListener>(_moduleListener), &ModuleListener::OnModuleReloadEnded);
		ReadAssemblies(false);
		if (_options.openBuild) {
			OpenBuild();
		}
	}

	void Pump() {
		if (!_active) {
			return;
		}
		PollUserProps();
		const void* moduleBase = ScriptProjectOps::GetLoadedModuleBase();
		if (moduleBase != _moduleBase || _moduleReloaded) {
			_moduleBase = moduleBase;
			_moduleReloaded = false;
			bool changed = CSharpNativeTypes::Refresh(_options.projectPath);
			const i64 bindingsTime = ReadUserBindingsTime();
			if (bindingsTime != _userBindingsTime) {
				_userBindingsTime = bindingsTime;
				changed = true;
			}
			if (changed) {
				NotifyDefinitionsChanged();
			}
		}
		bool compileNow = false;
		{
			std::lock_guard lock(_pendingMutex);
			compileNow = _pendingCompile && SecondsSince(_lastChangeTicks) >= _options.debounceSeconds && CSharpSettings::GetUser().autoCompile;
		}
		if (compileNow || _heldForModule) {
			RequestCompile();
		}
		_compile.Pump();
		DescribeStartedCompile();
		HandleFinishedCompile();
		CSharpReload::Pump();
		if (_holdingReports && CSharpReload::GetReloadCount() > 0) {
			ReleaseReports();
		}
	}

	void Shutdown() {
		_assetsWatcher.reset();
		if (_moduleListener) {
			EventBus::GetGlobal().UnsubscribeAll(_moduleListener->GetInstanceId());
			Delete(_moduleListener);
			_moduleListener = nullptr;
		}
		_moduleBase = nullptr;
		_moduleReloaded = false;
		_heldForModule = false;
		_compile.Cancel();
		ReleaseReports();
		CSharpReload::Shutdown();
		_describedRequests = _compile.GetRequestCount();
		_handledRequests = _compile.GetRequestCount();
		_pendingStamp.clear();
		_loadedOutputTime = 0;
		if (_active && _startedServers && _options.buildServers) {
			const ChildVariable variables[] = {
				{"DOTNET_ROOT", _options.sdkRoot},
				{"DOTNET_CLI_TELEMETRY_OPTOUT", "1"},
				{"DOTNET_NOLOGO", "1"},
				{"DOTNET_MULTILEVEL_LOOKUP", "0"},
			};
			StartDetachedProcess(QuoteCommandArgument(DotNetToolchain::GetMuxerPath(_options.sdkRoot)) + " build-server shutdown", _options.projectPath,
								 variables);
		}
		if (_active) {
			CompileStatus::UnregisterOwnedBy(_owner);
		}
		_startedServers = false;
		_buildFile.clear();
		_assemblyProjects = {};
		_assemblyNames = {};
		_active = false;
		_owner = nullptr;
		_options = {};
		std::lock_guard lock(_pendingMutex);
		_pendingCompile = false;
		_definitionsChanged = false;
	}

	bool IsActive() {
		return _active;
	}

	const CSharpCompile& GetCompile() {
		return _compile;
	}

	void RequestCompile() {
		if (!_active) {
			return;
		}
		_heldForModule = ScriptProjectOps::GetModuleStatus(_options.projectPath) == ScriptProjectOps::ModuleStatus::Compiling;
		if (_heldForModule) {
			return;
		}
		bool definitionsChanged = CSharpNativeTypes::Refresh(_options.projectPath);
		{
			std::lock_guard lock(_pendingMutex);
			_pendingCompile = false;
			definitionsChanged = definitionsChanged || _definitionsChanged;
			_definitionsChanged = false;
		}
		if (definitionsChanged && !_compile.IsBusy()) {
			ReadAssemblies(true);
		} else if (definitionsChanged) {
			std::lock_guard lock(_pendingMutex);
			_definitionsChanged = true;
		}
		ApplyCompileOptions();
		_startedServers = _compile.Request() || _startedServers;
		DescribeStartedCompile();
	}

	bool IsHoldingReports() {
		return _holdingReports;
	}

	void NotifySourceChanged() {
		std::lock_guard lock(_pendingMutex);
		_pendingCompile = true;
		_lastChangeTicks = Time::GetTicks();
	}

	void NotifyDefinitionsChanged() {
		std::lock_guard lock(_pendingMutex);
		_definitionsChanged = true;
		_pendingCompile = true;
		_lastChangeTicks = Time::GetTicks();
	}

} // namespace Shalltear::CSharpEditor::CSharpAuthoring
