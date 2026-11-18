#include "ScriptLog.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_set>

namespace Shalltear::CSharpImpl {

	namespace {

		struct TextHash {
			using is_transparent = void;

			size_t operator()(std::string_view text) const noexcept { return std::hash<std::string_view>{}(text); }
		};

		std::mutex _originsMutex;
		std::unordered_set<std::string, TextHash, std::equal_to<>> _origins;

	} // anonymous namespace

	const char* InternLogOrigin(std::string_view file) {
		if (file.empty()) {
			return "";
		}
		std::lock_guard lock(_originsMutex);
		const auto found = _origins.find(file);
		return (found != _origins.end() ? found : _origins.emplace(file).first)->c_str();
	}

	i32 GetLogLevel() noexcept {
		return static_cast<i32>(LogManager::GetGlobalLevel());
	}

	void WriteLog(i32 level, std::string_view category, std::string_view message, std::string_view file, i32 line) {
		const LogLevel clamped = static_cast<LogLevel>(std::clamp<i32>(level, 0, static_cast<i32>(LogLevel::Fatal)));
		if (clamped < LogManager::GetGlobalLevel()) {
			return;
		}
		const str name(category);
		LogManager::Dispatch(clamped, name.c_str(), InternLogOrigin(file), static_cast<u32>(std::max<i32>(line, 0)), "", "{}", message);
	}

} // namespace Shalltear::CSharpImpl
