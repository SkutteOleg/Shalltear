#pragma once

#include "Shalltear/Core/CoreAPI.h"
#include "Shalltear/Core/Types.h"

#include <string_view>
#include <vector>

namespace Shalltear {

	class SHALLTEAR_CORE_API StringUtil {
	public:
		static std::vector<str> Split(std::string_view s, std::string_view delimiter);

		static str Join(const std::vector<str>& parts, std::string_view delimiter);

		static str TrimLeft(std::string_view s);
		static str TrimRight(std::string_view s);
		static str Trim(std::string_view s);

		static bool StartsWith(std::string_view s, std::string_view prefix);
		static bool EndsWith(std::string_view s, std::string_view suffix);
		static bool Contains(std::string_view s, std::string_view sub);

		static str ToLower(std::string_view s);
		static str ToUpper(std::string_view s);

		static str Replace(std::string_view s, std::string_view search, std::string_view replace);

		static std::wstring UTF8ToWide(std::string_view utf8Str);
		static str WideToUTF8(std::wstring_view wideStr);
	};

} // namespace Shalltear
