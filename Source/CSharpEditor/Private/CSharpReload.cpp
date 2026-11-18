#include "CSharpReload.h"

#include "ManagedTypes.h"
#include "ScriptRuntime.h"

#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Module/TypeReplacement.h"
#include "Shalltear/Core/Object.h"
#include "Shalltear/Core/Time.h"
#include "Shalltear/Core/UI/UISystem.h"

#include "Shalltear/Editor/BuildPipeline.h"
#include "Shalltear/Editor/EditorEvents.h"
#include "Shalltear/Editor/TypeNameOps.h"

namespace Shalltear::CSharpEditor::CSharpReload {

	namespace {

		using CSharpImpl::ManagedTypes::GetOwnerToken;
		namespace ScriptRuntime = CSharpImpl::ScriptRuntime;

		enum class Source : u8 {
			File = 0,
			Kept,
		};

		struct Swap {
			Source source = Source::File;
			Outcome outcome = Outcome::Failed;
		};

		class Listener : public Object {
		public:
			void OnPlayModeChanged(const EditorPlayModeChangedEvent& event);
			void OnPlayStarting(const EditorPlayStartingEvent& event);
		};

		str _assembly;
		Listener* _listener = nullptr;
		bool _active = false;
		bool _pending = false;
		bool _held = false;
		bool _buildStarting = false;
		bool _playing = false;
		bool _loaded = false;
		bool _behind = false;
		Outcome _lastOutcome = Outcome::None;
		u32 _reloadCount = 0;
		u32 _lastMigrated = 0;
		f64 _lastMilliseconds = 0.0;

		bool RunSwap(void* user) {
			Swap& swap = *static_cast<Swap*>(user);
			if (ScriptRuntime::UnloadUserAssemblies() != SLT_OK) {
				SLT_LOG_WARN("C#", "previous scripts are still referenced after unload (memory not freed)");
			}
			const str paths[] = {_assembly};
			const i32 loaded = swap.source == Source::File ? ScriptRuntime::LoadUserAssemblies(paths) : ScriptRuntime::LoadKeptUserAssemblies();
			if (loaded == SLT_OK) {
				swap.outcome = Outcome::Loaded;
				return true;
			}
			ScriptRuntime::UnloadUserAssemblies();
			if (swap.source == Source::File && ScriptRuntime::LoadKeptUserAssemblies() == SLT_OK) {
				swap.outcome = Outcome::RolledBack;
			}
			return false;
		}

		bool Apply(Source source) {
			if (!TypeReplacement::CanReplace()) {
				return false;
			}
			const void* token = GetOwnerToken();
			const u64 begin = Time::GetTicks();
			const u32 migrated = CSharpImpl::ManagedTypes::GetLiveCount();
			const bool first = !_loaded;
			EventBus& bus = EventBus::GetGlobal();
			EditorEvents::ModuleReloadBegan.Publish(bus, {token});
			UISystem::CancelDragPayload();
			Swap swap;
			swap.source = source;
			CSharpImpl::ManagedTypes::SetRestoring(true);
			const bool replaced = TypeReplacement::Replace(token, &RunSwap, &swap);
			CSharpImpl::ManagedTypes::SetRestoring(false);
			CSharpImpl::ManagedTypes::FlushDirty();
			TypeNameOps::Refresh();
			EditorEvents::ModuleReloadEnded.Publish(bus, {replaced, token});

			_lastOutcome = swap.outcome;
			_lastMigrated = migrated;
			_lastMilliseconds = static_cast<f64>(Time::GetTicks() - begin) * 1000.0 / static_cast<f64>(Time::GetTicksPerSecond());
			_loaded = swap.outcome != Outcome::Failed;
			if (source == Source::File) {
				_behind = swap.outcome != Outcome::Loaded;
			}
			++_reloadCount;
			if (swap.outcome == Outcome::Loaded && first) {
				SLT_LOG_INFO("C#", "C# scripts loaded in {:.0f} ms", _lastMilliseconds);
			} else if (swap.outcome == Outcome::Loaded) {
				SLT_LOG_INFO("C#", "C# reloaded in {:.0f} ms ({} component(s) migrated)", _lastMilliseconds, migrated);
			} else if (swap.outcome == Outcome::RolledBack) {
				SLT_LOG_ERROR("C#", "new scripts couldn't be loaded (previous build loaded again). See the error above");
			} else {
				SLT_LOG_ERROR("C#", "scripts couldn't be loaded. See the error above");
			}
			return true;
		}

#ifdef SLT_CSHARP_TEST_HOOKS
		bool _buildRunningForTest = false;
#endif

		bool IsBuildRunning() {
			if (_buildStarting) {
				return false;
			}
#ifdef SLT_CSHARP_TEST_HOOKS
			if (_buildRunningForTest) {
				return true;
			}
#endif
			const BuildPipeline* pipeline = BuildPipeline::GetActive();
			return pipeline && pipeline->IsBusy();
		}

		void Listener::OnPlayModeChanged(const EditorPlayModeChangedEvent& event) {
			_playing = event.state != PlayModeState::Editing;
		}

		void Listener::OnPlayStarting(const EditorPlayStartingEvent&) {
			if (!_active || !ScriptRuntime::IsAttached()) {
				return;
			}
			if (_pending) {
				_pending = false;
				_held = false;
				if (Apply(Source::File)) {
					return;
				}
				_pending = true;
			}
			if (_loaded && !Apply(Source::Kept)) {
				SLT_LOG_WARN("C#", "script statics not reset for Play (scene busy). Press Play again");
			}
		}

	} // anonymous namespace

	void Begin(const str& assembly) {
		Shutdown();
		_assembly = assembly;
		_active = true;
		_listener = New<Listener>(MemTag::Editor);
		EditorEvents::PlayModeChanged.Subscribe(EventBus::GetGlobal(), Handle<Listener>(_listener), &Listener::OnPlayModeChanged);
		EditorEvents::PlayStarting.Subscribe(EventBus::GetGlobal(), Handle<Listener>(_listener), &Listener::OnPlayStarting);
	}

	void Shutdown() {
		if (_listener) {
			EventBus::GetGlobal().UnsubscribeAll(_listener->GetInstanceId());
			Delete(_listener);
			_listener = nullptr;
		}
		_assembly.clear();
		_active = false;
		_pending = false;
		_held = false;
		_playing = false;
		_loaded = false;
		_behind = false;
		_lastOutcome = Outcome::None;
		_reloadCount = 0;
		_lastMigrated = 0;
		_lastMilliseconds = 0.0;
	}

	void Pump() {
		if (!_active || !_pending || !ScriptRuntime::IsAttached()) {
			return;
		}
		if (_playing || IsBuildRunning()) {
			if (!_held) {
				_held = true;
				SLT_LOG_INFO("C#", "new scripts compiled during {} (reload held until it ends)", _playing ? "Play" : "a build");
			}
			return;
		}
		_held = false;
		_pending = false;
		if (!Apply(Source::File)) {
			_pending = true;
		}
	}

	void Request() {
		if (_active) {
			_pending = true;
		}
	}

	void ReloadNow() {
		if (!_active || !ScriptRuntime::IsAttached() || IsBuildRunning()) {
			return;
		}
		if (!_pending && !_loaded) {
			return;
		}
		const bool fromFile = _pending || !_loaded;
		_pending = false;
		_held = false;
		if (!Apply(fromFile ? Source::File : Source::Kept) && fromFile) {
			_pending = true;
		}
	}

#ifdef SLT_CSHARP_TEST_HOOKS
	void SetBuildRunningForTest(bool running) {
		_buildRunningForTest = running;
	}
#endif

	bool IsActive() {
		return _active;
	}

	void SetBuildStarting(bool starting) {
		_buildStarting = starting;
	}

	bool IsHeld() {
		return _held;
	}

	bool IsPending() {
		return _pending;
	}

	bool IsPlaying() {
		return _playing;
	}

	bool IsLoaded() {
		return _loaded;
	}

	bool IsBehind() {
		return _behind;
	}

	Outcome GetLastOutcome() {
		return _lastOutcome;
	}

	u32 GetReloadCount() {
		return _reloadCount;
	}

	u32 GetLastMigratedCount() {
		return _lastMigrated;
	}

	f64 GetLastMilliseconds() {
		return _lastMilliseconds;
	}

} // namespace Shalltear::CSharpEditor::CSharpReload
