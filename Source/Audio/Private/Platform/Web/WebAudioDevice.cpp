#include "Platform/AudioDevicePlatform.h"

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

} // namespace Shalltear::AudioImpl::DevicePlatform
