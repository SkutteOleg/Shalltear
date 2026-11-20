#include "CSharpAuthoring.h"

#include "CSharpReload.h"
#include "CSharpSettings.h"
#include "DotNetToolchain.h"
#include "ManagedTypes.h"
#include "SHA512.h"

#include "Shalltear/Core/BuildInfo.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/StringUtil.h"
#include "Shalltear/Core/Time.h"

#include "Shalltear/Editor/ChildProcess.h"
#include "Shalltear/Editor/CompileStatus.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>

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
		u64 _lastPollTicks = 0;
		u32 _describedRequests = 0;
		u32 _handledRequests = 0;
		str _pendingStamp;
		i64 _loadedOutputTime = 0;
		bool _holdingReports = false;
		std::mutex _pendingMutex;
		bool _pendingCompile = false;
		u64 _lastChangeTicks = 0;

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
				if (it->is_regular_file(entry) && StringUtil::ToLower(it->path().extension().generic_string()) == ".cs") {
					rows.push_back(std::format("{} {}", it->path().lexically_relative(root).generic_string(), DescribeFile(it->path())));
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
			if (!fs::is_regular_file(fs::path(_compile.GetOutputAssembly()), ec) || !fs::is_regular_file(GetStampFile(), ec)) {
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
				_loadedOutputTime = ReadWriteTime(fs::path(_compile.GetOutputAssembly()));
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
			const i64 output = ReadWriteTime(fs::path(_compile.GetOutputAssembly()));
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
			compile.muxer = DotNetToolchain::GetMuxerPath(_options.sdkRoot);
			compile.pinStamp = std::format("{} {} {}", pin.sdkVersion, pin.runtimeVersion, BuildInfo::GetGitCommit());
			compile.optimization = CSharpSettings::GetUser().codeOptimization;
			compile.aotAnalysis = CSharpSettings::GetUser().aotAnalysis;
			compile.buildServers = _options.buildServers;
			_compile.Configure(std::move(compile));
		}

		void ArmWatcher() {
			std::error_code ec;
			const fs::path assets = fs::path(_options.projectPath) / "Assets";
			if (fs::is_directory(assets, ec)) {
				_assetsWatcher.emplace(Path(assets.generic_string()), [](const Path& changed, FileAction) {
					if (StringUtil::ToLower(changed.GetExtension()) == ".cs") {
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
		if (_options.openBuild) {
			OpenBuild();
		}
	}

	void Pump() {
		if (!_active) {
			return;
		}
		PollUserProps();
		bool compileNow = false;
		{
			std::lock_guard lock(_pendingMutex);
			compileNow = _pendingCompile && SecondsSince(_lastChangeTicks) >= _options.debounceSeconds && CSharpSettings::GetUser().autoCompile;
		}
		if (compileNow) {
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
			};
			StartDetachedProcess(QuoteCommandArgument(DotNetToolchain::GetMuxerPath(_options.sdkRoot)) + " build-server shutdown", _options.projectPath,
								 variables);
		}
		if (_active) {
			CompileStatus::UnregisterOwnedBy(_owner);
		}
		_startedServers = false;
		_active = false;
		_owner = nullptr;
		_options = {};
		std::lock_guard lock(_pendingMutex);
		_pendingCompile = false;
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
		{
			std::lock_guard lock(_pendingMutex);
			_pendingCompile = false;
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

} // namespace Shalltear::CSharpEditor::CSharpAuthoring
