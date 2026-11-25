#include "Platform/AudioDevicePlatform.h"

#include <emscripten.h>

#include <algorithm>

namespace Shalltear::AudioImpl::DevicePlatform {

	namespace {

		constexpr ma_backend SYSTEM_BACKENDS[] = {ma_backend_webaudio};
		constexpr ma_uint32 WEB_MIN_PERIOD_MS = 20;

	} // anonymous namespace

	std::span<const ma_backend> GetSystemBackends() {
		return SYSTEM_BACKENDS;
	}

	void ConfigureDevice(ma_device_config& config) {
		// WebAudio mixes on the browser main thread; a 20 ms period halves callback churn
		config.periodSizeInMilliseconds = std::max(config.periodSizeInMilliseconds, WEB_MIN_PERIOD_MS);
	}

	void LockFormat(ma_device&) {
	}

	void UnlockFormat(ma_device&) {
	}

	bool IsCallbackActive(const ma_device& device) {
		// clang-format off
		return EM_ASM_INT({
			var ma = window.miniaudio;
			if (!ma || !ma.get_device_by_index) {
				return 0;
			}
			var dev = ma.get_device_by_index($0);
			return (dev && dev.webaudio && dev.webaudio.state === "running") ? 1 : 0;
		}, device.webaudio.deviceIndex) != 0;
		// clang-format on
	}

} // namespace Shalltear::AudioImpl::DevicePlatform
