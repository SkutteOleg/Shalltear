#include "ScriptProfile.h"

#include "ManagedTypes.h"
#include "ScriptRuntime.h"

#include <iterator>

namespace Shalltear::CSharpImpl::ScriptProfile {

	namespace {

		// must match SLT_HOOK_* in ScriptBridge.h
		constexpr const char* HOOK_NAMES[] = {"OnAttach", "OnDetach", "OnEnable", "OnDisable", "OnDestroy", "OnUpdate", "OnFixedUpdate", "OnLateUpdate"};

		bool _enabled = false;
		std::vector<SltProfileRow> _scratch;

	} // anonymous namespace

	void SetEnabled(bool enabled) {
		if (_enabled == enabled || !ScriptRuntime::IsAttached()) {
			return;
		}
		_enabled = enabled;
		ScriptRuntime::GetManaged().setProfiling(enabled ? 1 : 0);
	}

	bool IsEnabled() {
		return _enabled;
	}

	void Read(std::vector<Row>& rows, Collections& collections) {
		rows.clear();
		collections = {};
		if (!_enabled || !ScriptRuntime::IsAttached()) {
			return;
		}
		if (_scratch.empty()) {
			_scratch.resize(64);
		}
		SltProfileFrame frame{};
		int32_t count = ScriptRuntime::GetManaged().readProfile(_scratch.data(), static_cast<int32_t>(_scratch.size()), &frame);
		if (count > static_cast<int32_t>(_scratch.size())) {
			_scratch.resize(static_cast<size_t>(count) * 2);
			count = ScriptRuntime::GetManaged().readProfile(_scratch.data(), static_cast<int32_t>(_scratch.size()), &frame);
		}
		for (int32_t index = 0; index < count && index < static_cast<int32_t>(_scratch.size()); ++index) {
			const SltProfileRow& read = _scratch[static_cast<size_t>(index)];
			const ManagedType* type = ManagedTypes::FindByIndex(read.managedTypeId + 1);
			if (!type) {
				continue;
			}
			rows.push_back({type->name, read.hook, read.calls, static_cast<f64>(read.nanoseconds) / 1'000'000.0, static_cast<u64>(read.bytes)});
		}
		for (size_t generation = 0; generation < std::size(collections.counts); ++generation) {
			collections.counts[generation] = static_cast<u32>(frame.collections[generation]);
		}
		collections.pauseMilliseconds = static_cast<f64>(frame.pauseNanoseconds) / 1'000'000.0;
		collections.heapBytes = static_cast<u64>(frame.heapBytes);
	}

	const char* GetHookName(i32 hook) {
		return hook >= 0 && hook < static_cast<i32>(std::size(HOOK_NAMES)) ? HOOK_NAMES[hook] : "";
	}

	void Reset() {
		_enabled = false;
		_scratch = {};
	}

} // namespace Shalltear::CSharpImpl::ScriptProfile
