#include "Platform/AudioDevicePlatform.h"

namespace Shalltear::AudioImpl::DevicePlatform {

	namespace {
		constexpr ma_backend SYSTEM_BACKENDS[] = {ma_backend_wasapi};
	}

	std::span<const ma_backend> GetSystemBackends() {
		return SYSTEM_BACKENDS;
	}

	void ConfigureDevice(ma_device_config& config) {
		config.wasapi.usage = ma_wasapi_usage_pro_audio;
	}

	// WASAPI notification thread writes these under this lock. crap API
	void LockFormat(ma_device& device) {
		ma_mutex_lock(&device.wasapi.rerouteLock);
	}

	void UnlockFormat(ma_device& device) {
		ma_mutex_unlock(&device.wasapi.rerouteLock);
	}

	bool IsCallbackActive(const ma_device&) {
		return true;
	}

} // namespace Shalltear::AudioImpl::DevicePlatform
