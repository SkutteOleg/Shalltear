#include "CSharpSetup.h"

#include "CSharpProjectFiles.h"
#include "CSharpSettings.h"
#include "CoreCLRHost.h"
#include "ScriptRuntime.h"

#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Object.h"
#include "Shalltear/Core/Settings.h"

#include "Shalltear/Editor/ChildProcess.h"
#include "Shalltear/Editor/EditorTasks.h"
#include "Shalltear/Editor/ProjectOps.h"

#include <chrono>
#include <memory>
#include <optional>

namespace Shalltear::CSharpEditor::CSharpSetup {

	namespace {

		Options _options;
		const void* _owner = nullptr;
		State _state = State::Idle;
		str _sdkRoot;
		std::unique_ptr<DotNetAcquire> _acquire;
		u64 _task = 0;
		AcquireStep _loggedStep = AcquireStep::Idle;

		bool StartHostedRuntime(const str& sdkRoot, str& outError) {
			const SltInitializeFn entry =
				CoreCLRHost::Start(sdkRoot, DotNetToolchain::GetPin().runtimeVersion, DotNetToolchain::GetRuntimeAssemblyPath(), outError);
			return entry && CSharpImpl::ScriptRuntime::Attach(entry, outError);
		}

		void EndTask() {
			if (_task != 0) {
				EditorTasks::End(_task);
				_task = 0;
			}
		}

		void WriteProjectFiles() {
			if (_options.projectPath.empty() || _sdkRoot.empty()) {
				return;
			}
			CSharpProjectFiles::Result files;
			if (!CSharpProjectFiles::Write(CSharpProjectFiles::GetDefaultOptions(_options.projectPath, _sdkRoot), files)) {
				SLT_LOG_WARN("C#", "couldn't write C# project files into '{}'", _options.projectPath);
			}
			for (const str& written : files.written) {
				SLT_LOG_INFO("C#", "wrote {}", written);
			}
		}

		class SettingsWatcher : public Object {
		public:
			void OnSettingChanged(const SettingChangedEvent& event) {
				if (event.categoryPath == CSharpSettings::ProjectCategory) {
					WriteProjectFiles();
				}
			}
		};

		SettingsWatcher* _watcher = nullptr;

		void BringUp(const str& sdkRoot) {
			_sdkRoot = sdkRoot;
			ChildEnvironment::Set("DOTNET_ROOT", sdkRoot, _owner);
			ChildEnvironment::PrependSearchPath(sdkRoot, _owner);
			WriteProjectFiles();

			const auto begin = std::chrono::steady_clock::now();
			str error;
			if (!_options.startRuntime(sdkRoot, error)) {
				_state = State::Failed;
				SLT_LOG_ERROR("C#", "{}", error);
				return;
			}
			_state = State::Ready;
			SLT_LOG_INFO("C#", "C# runtime ready in {:.0f} ms",
						 std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - begin).count());
		}

		void ReportStep() {
			const AcquireStatus& status = _acquire->GetStatus();
			std::optional<f32> fraction;
			if (status.step == AcquireStep::Downloading && status.bytesTotal > 0) {
				fraction = static_cast<f32>(static_cast<f64>(status.bytesDone) / static_cast<f64>(status.bytesTotal));
			}
			const str text = DescribeAcquireStep(status, _options.acquire.pin);
			EditorTasks::Report(_task, text, fraction);
			if (status.step != _loggedStep && status.step > AcquireStep::Downloading) {
				SLT_LOG_INFO("C#", "{}", text);
			}
			_loggedStep = status.step;
		}

	} // anonymous namespace

	Options GetDefaultOptions() {
		Options options;
		options.discovery = DotNetToolchain::GetDefaultDiscovery();
		options.acquire.pin = DotNetToolchain::GetPin();
		options.acquire.cacheRoot = DotNetToolchain::GetCacheRoot();
		options.projectPath = ProjectOps::GetMountedProjectPath();
		options.startRuntime = &StartHostedRuntime;
		return options;
	}

	void Begin(Options options, const void* owner) {
		Shutdown();
		_options = std::move(options);
		_owner = owner;
		ChildEnvironment::Set("DOTNET_CLI_TELEMETRY_OPTOUT", "1", owner);
		ChildEnvironment::Set("DOTNET_NOLOGO", "1", owner);
		_watcher = New<SettingsWatcher>(MemTag::Editor);
		SettingsEvents::Changed.Subscribe(EventBus::GetGlobal(), Handle<SettingsWatcher>(_watcher), &SettingsWatcher::OnSettingChanged);

		const DotNetToolchain::Pin& pin = _options.acquire.pin;
		const DotNetToolchain::Info found = DotNetToolchain::Discover(pin, _options.discovery);
		if (!_options.discovery.overrideRoot.empty() && found.tier != DotNetToolchain::Tier::Override) {
			SLT_LOG_WARN("C#", "SDK Path '{}' has no .NET SDK {} (ignored)", _options.discovery.overrideRoot, pin.sdkVersion);
		}
		if (found.tier != DotNetToolchain::Tier::None) {
			BringUp(found.root);
			return;
		}
		if (_options.acquire.cacheRoot.empty()) {
			_state = State::Failed;
			SLT_LOG_ERROR("C#", "{}", DotNetToolchain::Describe(pin, _options.discovery));
			return;
		}

		SLT_LOG_INFO("C#", "downloading .NET SDK {} into '{}' (C# scripts start when done)", pin.sdkVersion, _options.acquire.cacheRoot);
		_state = State::Acquiring;
		_loggedStep = AcquireStep::Idle;
		_acquire = std::make_unique<DotNetAcquire>(_options.acquire);
		_acquire->Start();
		_task = EditorTasks::Begin({DescribeAcquireStep(_acquire->GetStatus(), pin),
									[]() {
										if (_acquire) {
											_acquire->Cancel();
										}
									}},
								   owner);
	}

	void Pump() {
		if (_state != State::Acquiring || !_acquire) {
			return;
		}
		_acquire->Pump();
		if (!_acquire->IsFinished()) {
			ReportStep();
			return;
		}

		const AcquireStatus status = _acquire->GetStatus();
		const str installed = _acquire->GetInstalledRoot();
		_acquire.reset();
		EndTask();
		switch (status.step) {
		case AcquireStep::Done:
			SLT_LOG_INFO("C#", ".NET SDK {} installed into '{}'", _options.acquire.pin.sdkVersion, installed);
			BringUp(installed);
			break;
		case AcquireStep::Cancelled:
			_state = State::Cancelled;
			SLT_LOG_WARN("C#", ".NET SDK {} download cancelled. Reopen the project to download it", _options.acquire.pin.sdkVersion);
			break;
		default:
			_state = State::Failed;
			SLT_LOG_ERROR("C#", "{}", status.error);
			break;
		}
	}

	void Shutdown() {
		_acquire.reset();
		EndTask();
		if (_watcher) {
			EventBus::GetGlobal().UnsubscribeAll(_watcher->GetInstanceId());
			Delete(_watcher);
			_watcher = nullptr;
		}
		if (_owner) {
			ChildEnvironment::UnregisterOwnedBy(_owner);
		}
		_options = {};
		_owner = nullptr;
		_state = State::Idle;
		_sdkRoot.clear();
	}

	State GetState() {
		return _state;
	}

	const str& GetSDKRoot() {
		return _sdkRoot;
	}

} // namespace Shalltear::CSharpEditor::CSharpSetup
