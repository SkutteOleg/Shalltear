#include "CSharpAuthoring.h"

#include "CSharpSettings.h"
#include "DotNetToolchain.h"
#include "ManagedTypes.h"

#include "Shalltear/Core/BuildInfo.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/StringUtil.h"
#include "Shalltear/Core/Time.h"

#include "Shalltear/Editor/ChildProcess.h"
#include "Shalltear/Editor/CompileStatus.h"

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
		std::mutex _pendingMutex;
		bool _pendingCompile = false;
		u64 _lastChangeTicks = 0;

		f64 SecondsSince(u64 ticks) {
			return static_cast<f64>(Time::GetTicks() - ticks) / static_cast<f64>(Time::GetTicksPerSecond());
		}

		i64 ReadPropsTime() {
			std::error_code ec;
			const fs::file_time_type time = fs::last_write_time(fs::path(_options.projectPath) / "CSharp.props", ec);
			return ec ? 0 : static_cast<i64>(time.time_since_epoch().count());
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
			state.typeOwner = CSharpImpl::ManagedTypes::GetOwnerToken();
			return state;
		}

		void CompileNow(void*) {
			RequestCompile();
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
		CompileStatus::RegisterSource({"C#", &ReadState, &CompileNow, nullptr, &ReadHeading, nullptr}, owner);
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
	}

	void Shutdown() {
		_assetsWatcher.reset();
		_compile.Cancel();
		if (_active && _startedServers && _options.buildServers) {
			StartDetachedProcess(QuoteCommandArgument(DotNetToolchain::GetMuxerPath(_options.sdkRoot)) + " build-server shutdown", _options.projectPath);
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
	}

	void NotifySourceChanged() {
		std::lock_guard lock(_pendingMutex);
		_pendingCompile = true;
		_lastChangeTicks = Time::GetTicks();
	}

} // namespace Shalltear::CSharpEditor::CSharpAuthoring
