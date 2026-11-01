#include "Shalltear/Core/StringUtil.h"
#include "Shalltear/Core/Platform.h"

#include <algorithm>
#include <cctype>

#if defined(SLT_PLATFORM_WINDOWS)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace Shalltear {

	std::vector<str> StringUtil::Split(std::string_view s, std::string_view delimiter) {
		std::vector<str> result;
		if (delimiter.empty()) {
			result.emplace_back(s);
			return result;
		}

		size_t start = 0;
		size_t pos = s.find(delimiter);

		while (pos != std::string_view::npos) {
			if (pos > start) {
				result.emplace_back(s.substr(start, pos - start));
			}
			start = pos + delimiter.length();
			pos = s.find(delimiter, start);
		}

		if (start < s.length()) {
			result.emplace_back(s.substr(start));
		}

		return result;
	}

	str StringUtil::Join(const std::vector<str>& parts, std::string_view delimiter) {
		if (parts.empty()) {
			return "";
		}

		size_t totalLen = delimiter.length() * (parts.size() - 1);
		for (const auto& part : parts) {
			totalLen += part.length();
		}

		str result;
		result.reserve(totalLen);

		for (size_t i = 0; i < parts.size(); ++i) {
			result += parts[i];
			if (i < parts.size() - 1) {
				result += delimiter;
			}
		}
		return result;
	}

	str StringUtil::TrimLeft(std::string_view s) {
		size_t start = s.find_first_not_of(" \t\n\r\f\v");
		return (start == std::string_view::npos) ? "" : str(s.substr(start));
	}

	str StringUtil::TrimRight(std::string_view s) {
		size_t end = s.find_last_not_of(" \t\n\r\f\v");
		return (end == std::string_view::npos) ? "" : str(s.substr(0, end + 1));
	}

	str StringUtil::Trim(std::string_view s) {
		return TrimRight(TrimLeft(s));
	}

	bool StringUtil::StartsWith(std::string_view s, std::string_view prefix) {
		return s.starts_with(prefix);
	}

	bool StringUtil::EndsWith(std::string_view s, std::string_view suffix) {
		return s.ends_with(suffix);
	}

	bool StringUtil::Contains(std::string_view s, std::string_view sub) {
		return s.find(sub) != std::string_view::npos;
	}

	str StringUtil::ToLower(std::string_view s) {
		str result(s);
		std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return result;
	}

	str StringUtil::ToUpper(std::string_view s) {
		str result(s);
		std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
			return static_cast<char>(std::toupper(c));
		});
		return result;
	}

	str StringUtil::Replace(std::string_view s, std::string_view search, std::string_view replace) {
		if (search.empty()) {
			return str(s);
		}

		str result(s);
		size_t pos = 0;
		while ((pos = result.find(search, pos)) != str::npos) {
			result.replace(pos, search.length(), replace);
			pos += replace.length();
		}
		return result;
	}

	std::wstring StringUtil::UTF8ToWide(std::string_view utf8Str) {
		if (utf8Str.empty()) {
			return std::wstring();
		}

#if defined(SLT_PLATFORM_WINDOWS)
		int count = MultiByteToWideChar(CP_UTF8, 0, utf8Str.data(), static_cast<int>(utf8Str.length()), nullptr, 0);
		if (count == 0) {
			return std::wstring();
		}

		std::wstring result(count, 0);
		MultiByteToWideChar(CP_UTF8, 0, utf8Str.data(), static_cast<int>(utf8Str.length()), &result[0], count);
		return result;
#else
#error "StringUtil not yet implemented for non-Windows platforms"
#endif
	}

	str StringUtil::WideToUTF8(std::wstring_view wideStr) {
		if (wideStr.empty()) {
			return "";
		}

#if defined(SLT_PLATFORM_WINDOWS)
		int count = WideCharToMultiByte(CP_UTF8, 0, wideStr.data(), static_cast<int>(wideStr.length()), nullptr, 0, nullptr, nullptr);
		if (count == 0) {
			return "";
		}

		str result(count, 0);
		WideCharToMultiByte(CP_UTF8, 0, wideStr.data(), static_cast<int>(wideStr.length()), &result[0], count, nullptr, nullptr);
		return result;
#else
#error "StringUtil not yet implemented for non-Windows platforms"
#endif
	}

} // namespace Shalltear
