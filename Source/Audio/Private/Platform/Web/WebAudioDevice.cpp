#include "Platform/AudioDevicePlatform.h"

namespace Shalltear::AudioImpl::DevicePlatform {

	namespace {

		constexpr ma_backend SYSTEM_BACKENDS[] = {ma_backend_webaudio};

	} // anonymous namespace

	std::span<const ma_backend> GetSystemBackends() {
		return SYSTEM_BACKENDS;
	}

	void ConfigureDevice(ma_device_config&) {
	}

	void LockFormat(ma_device&) {
	}

	void UnlockFormat(ma_device&) {
	}

} // namespace Shalltear::AudioImpl::DevicePlatform
