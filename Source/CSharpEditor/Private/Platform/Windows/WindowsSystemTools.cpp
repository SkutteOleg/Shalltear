#include "SystemTools.h"

#include "Shalltear/Core/StringUtil.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace Shalltear::CSharpEditor {

	std::filesystem::path MakeLongPath(const str& path) {
		std::filesystem::path plain(path);
		plain.make_preferred();
		const std::wstring& native = plain.native();
		if (!plain.is_absolute() || native.starts_with(L"\\\\?\\")) {
			return plain;
		}
		if (native.starts_with(L"\\\\")) {
			return std::filesystem::path(L"\\\\?\\UNC\\" + native.substr(2));
		}
		return std::filesystem::path(L"\\\\?\\" + native);
	}

	str FindSystemTool(std::string_view name) {
		wchar_t directory[MAX_PATH] = {};
		const UINT length = ::GetSystemDirectoryW(directory, MAX_PATH);
		if (length == 0 || length >= MAX_PATH) {
			return str(name);
		}
		const std::wstring path = std::wstring(directory, length) + L"\\" + StringUtil::UTF8ToWide(str(name)) + L".exe";
		const DWORD attributes = ::GetFileAttributesW(path.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
			return str(name);
		}
		return StringUtil::WideToUTF8(path);
	}

} // namespace Shalltear::CSharpEditor
