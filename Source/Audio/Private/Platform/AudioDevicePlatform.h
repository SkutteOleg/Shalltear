#pragma once

#include "MiniaudioShared.h"

#include <span>

namespace Shalltear::AudioImpl::DevicePlatform {

	// tried in order
	std::span<const ma_backend> GetSystemBackends();

	void ConfigureDevice(ma_device_config& config);

	// held while reading device's format
	void LockFormat(ma_device& device);
	void UnlockFormat(ma_device& device);

	// false while a browser AudioContext is suspended and no render callback can run
	bool IsCallbackActive(const ma_device& device);

} // namespace Shalltear::AudioImpl::DevicePlatform
