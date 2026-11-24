#pragma once

#include "Shalltear/Core/Types.h"
#include "Shalltear/Network/HTTP.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <span>

namespace Shalltear::Net::HTTPBackend {

	struct Control {
		std::atomic<bool> cancelled{false};
		std::atomic<bool> timedOut{false};

		std::mutex abortLock;
		// set during blocking call, called once by Cancel
		std::function<void()> abort;

		std::function<void(u64 received, u64 total)> progress;
		// return false to fail transfer
		std::function<bool(std::span<const u8> bytes)> write;
	};

	bool IsAvailable();
	bool SupportsAsync();
	// body streamed to control.write, everything else into response
	void Perform(const HTTP::Request& request, HTTP::Response& response, Control& control);
	void StartAsync(const HTTP::Request& request, std::shared_ptr<Control> control,
					std::function<void(HTTP::Response)> onComplete);

} // namespace Shalltear::Net::HTTPBackend
