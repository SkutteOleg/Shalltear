#pragma once

#include "ScriptBridge.h"

#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Time.h"
#include "Shalltear/Core/Types.h"

#include <mutex>
#include <string_view>
#include <vector>

namespace Shalltear::CSharpConformance {

	inline std::string_view View(SltString text) {
		return text.ptr && text.length > 0 ? std::string_view(text.ptr, static_cast<size_t>(text.length)) : std::string_view{};
	}

	inline FrameTime MakeFrame() {
		FrameTime frame;
		frame.deltaTime = 0.25f;
		frame.unscaledDeltaTime = 0.5f;
		frame.fixedDeltaTime = 0.02f;
		frame.time = 12.5;
		frame.unscaledTime = 25.0;
		frame.timeScale = 0.5f;
		frame.frameCount = 1234;
		return frame;
	}

	inline bool IsProbe(std::string_view caseName) {
		return caseName.substr(0, caseName.find('.')).ends_with("Probe");
	}

	struct LogRow {
		LogLevel level;
		str category;
		str message;
	};

	class RowSink : public ILogSink {
	public:
		void ReceiveLog(const LogEvent& event) override {
			std::lock_guard lock(_mutex);
			_rows.push_back({event.level, event.category, event.message});
		}
		void Flush() override {}

		u32 Count(LogLevel level, std::string_view category, std::string_view message) const {
			std::lock_guard lock(_mutex);
			u32 count = 0;
			for (const LogRow& row : _rows) {
				count += row.level == level && row.category == category && row.message == message ? 1u : 0u;
			}
			return count;
		}
		u32 CountFrom(std::string_view category) const {
			std::lock_guard lock(_mutex);
			u32 count = 0;
			for (const LogRow& row : _rows) {
				count += row.category == category ? 1u : 0u;
			}
			return count;
		}
		std::vector<LogRow> Rows() const {
			std::lock_guard lock(_mutex);
			return _rows;
		}
	private:
		mutable std::mutex _mutex;
		std::vector<LogRow> _rows;
	};

} // namespace Shalltear::CSharpConformance
