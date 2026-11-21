#pragma once

#include "ScriptBridge.h"

#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Time.h"
#include "Shalltear/Core/Types.h"

#include <filesystem>
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

#ifdef SHALLTEAR_PHYSICS_BACKEND_STUB
	inline constexpr int32_t PASS_SET_SIZE = 130;
#else
	inline constexpr int32_t PASS_SET_SIZE = 143;
#endif

	inline bool IsInPassSet(std::string_view caseName) {
#ifdef SHALLTEAR_PHYSICS_BACKEND_STUB
		if (caseName.starts_with("PhysicsTests.")) {
			return false;
		}
#endif
		return !IsProbe(caseName);
	}

	bool StandUpPackages(const void* owner);
	bool UnloadBridge(std::string_view name);

	class StagedAssets {
	public:
		StagedAssets();
		~StagedAssets();

		StagedAssets(const StagedAssets&) = delete;
		StagedAssets& operator=(const StagedAssets&) = delete;

		bool IsStaged() const { return _staged; }
	private:
		std::filesystem::path _root;
		bool _staged = false;
	};

	using DriverFn = i32 (*)(i32 op, i32 first, i32 second);
	using AssetFinderFn = u64 (*)(std::string_view path);

	inline constexpr int32_t AUDIO_CASES = 15;

	class AudioStore {
	public:
		AudioStore();
		~AudioStore();

		AudioStore(const AudioStore&) = delete;
		AudioStore& operator=(const AudioStore&) = delete;
	private:
		std::filesystem::path _root;
	};

	bool StandUpAudio(const void* owner);
	void ShutDownAudio();

	class AudioRig {
	public:
		AudioRig(void (*setDriver)(DriverFn), void (*setAssetFinder)(AssetFinderFn));
		~AudioRig();

		AudioRig(const AudioRig&) = delete;
		AudioRig& operator=(const AudioRig&) = delete;

		bool IsStaged() const { return _staged; }
	private:
		void (*_setDriver)(DriverFn);
		void (*_setAssetFinder)(AssetFinderFn);
		std::filesystem::path _root;
		bool _staged = false;
	};

	class InputRig {
	public:
		explicit InputRig(void (*setDriver)(DriverFn));
		~InputRig();

		InputRig(const InputRig&) = delete;
		InputRig& operator=(const InputRig&) = delete;

		bool IsReady() const { return _ready; }
	private:
		void (*_setDriver)(DriverFn);
		bool _ready = false;
	};

	class UIRig {
	public:
		UIRig();
		~UIRig();

		UIRig(const UIRig&) = delete;
		UIRig& operator=(const UIRig&) = delete;

		bool IsReady() const { return _ready; }
	private:
		bool _ready = false;
		bool _mounted = false;
	};

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
