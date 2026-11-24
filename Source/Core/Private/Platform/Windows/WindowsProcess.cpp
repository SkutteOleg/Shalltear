#include "Platform/Process.h"

#include "Platform/Windows/WindowsHeaders.h"

#include <shlobj.h>

#include "Shalltear/Core/StringUtil.h"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <string>

namespace Shalltear::Platform {

	namespace {

		str ReadEnvironmentPath(const wchar_t* name) {
			wchar_t buffer[MAX_PATH];
			DWORD length = ::GetEnvironmentVariableW(name, buffer, MAX_PATH);
			if (length == 0 || length >= MAX_PATH) {
				return {};
			}
			return StringUtil::WideToUTF8(std::wstring_view(buffer, length));
		}

	} // anonymous namespace

	str GetExecutablePath() {
		wchar_t buffer[MAX_PATH * 2] = {};
		DWORD length = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
		if (length == 0 || length >= std::size(buffer)) {
			return {};
		}
		return std::filesystem::path(std::wstring(buffer, length)).generic_string();
	}

	str GetUserDataDirectory() {
		str local = ReadEnvironmentPath(L"LOCALAPPDATA");
		if (local.empty()) {
			str profile = ReadEnvironmentPath(L"USERPROFILE");
			if (profile.empty()) {
				return {};
			}
			local = profile + "/AppData/Local";
		}
		return local;
	}

	str GetUserHomeDirectory() {
		return ReadEnvironmentPath(L"USERPROFILE");
	}

	str GetUserPicturesDirectory() {
		// FOLDERID_Pictures (without linking uuid)
		constexpr GUID Pictures = {0x33E28130, 0x4E1E, 0x4676, {0x83, 0x5A, 0x98, 0x39, 0x5C, 0x3B, 0xC3, 0xBB}};
		PWSTR wide = nullptr;
		const HRESULT result = ::SHGetKnownFolderPath(Pictures, KF_FLAG_DEFAULT, nullptr, &wide);
		const std::u8string utf8 = SUCCEEDED(result) && wide ? std::filesystem::path(wide).generic_u8string() : std::u8string();
		::CoTaskMemFree(wide);
		return str(reinterpret_cast<const char*>(utf8.data()), utf8.size());
	}

	str GetUserLanguage() {
		ULONG count = 0;
		ULONG size = 0;
		if (::GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) && size > 1) {
			std::wstring names(size, L'\0');
			if (::GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, names.data(), &size) && count > 0) {
				return StringUtil::WideToUTF8(names.c_str());
			}
		}
		wchar_t name[LOCALE_NAME_MAX_LENGTH];
		return ::GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0 ? StringUtil::WideToUTF8(name) : str{};
	}

	bool AcquireStandardOutput(bool createConsole) {
		HANDLE existing = ::GetStdHandle(STD_OUTPUT_HANDLE);
		if (existing != nullptr && existing != INVALID_HANDLE_VALUE &&
			::GetFileType(existing) != FILE_TYPE_UNKNOWN) {
			return true;
		}

		auto adopt = []() {
			FILE* reopened = nullptr;
			::freopen_s(&reopened, "CONOUT$", "w", stdout);
			::freopen_s(&reopened, "CONOUT$", "w", stderr);
			std::cout.clear();
			std::cerr.clear();
		};

		if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
			adopt();
			return true;
		}
		if (createConsole && ::AllocConsole()) {
			adopt();
			return true;
		}
		return false;
	}

	void PrepareConsoleOutput() {
		HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
		if (hOut != INVALID_HANDLE_VALUE) {
			DWORD mode = 0;
			if (::GetConsoleMode(hOut, &mode)) {
				mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
				::SetConsoleMode(hOut, mode);
			}
		}
		::SetConsoleOutputCP(CP_UTF8);
	}

	void ToLocalTime(std::time_t time, std::tm& out) {
		::localtime_s(&out, &time);
	}

	bool RunProcessCapture(const str& executable, std::span<const str> arguments, str& outOutput, i32& outExitCode) {
		outOutput.clear();
		outExitCode = -1;

		auto quoteArg = [](const str& arg) {
			bool needsQuotes = arg.empty();
			for (char c : arg) {
				if (c == ' ' || c == '\t' || c == '"') {
					needsQuotes = true;
					break;
				}
			}
			if (!needsQuotes) {
				return arg;
			}
			str result = "\"";
			size_t backslashes = 0;
			for (char c : arg) {
				if (c == '\\') {
					++backslashes;
				} else if (c == '"') {
					result.append(backslashes * 2 + 1, '\\');
					result.push_back('"');
					backslashes = 0;
				} else {
					result.append(backslashes, '\\');
					backslashes = 0;
					result.push_back(c);
				}
			}
			result.append(backslashes * 2, '\\');
			result.push_back('"');
			return result;
		};

		str commandLine = quoteArg(executable);
		for (const str& arg : arguments) {
			commandLine.push_back(' ');
			commandLine += quoteArg(arg);
		}
		std::wstring wideCommand = StringUtil::UTF8ToWide(commandLine);

		SECURITY_ATTRIBUTES sa{};
		sa.nLength = sizeof(sa);
		sa.bInheritHandle = TRUE;
		HANDLE readPipe = nullptr;
		HANDLE writePipe = nullptr;
		if (!::CreatePipe(&readPipe, &writePipe, &sa, 0)) {
			return false;
		}
		::SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

		STARTUPINFOW si{};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdOutput = writePipe;
		si.hStdError = writePipe;
		si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

		PROCESS_INFORMATION pi{};
		BOOL created = ::CreateProcessW(nullptr, wideCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
		::CloseHandle(writePipe);
		if (!created) {
			::CloseHandle(readPipe);
			return false;
		}

		char buffer[4096];
		DWORD bytesRead = 0;
		while (::ReadFile(readPipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
			outOutput.append(buffer, bytesRead);
		}
		::CloseHandle(readPipe);

		::WaitForSingleObject(pi.hProcess, INFINITE);
		DWORD code = 1;
		::GetExitCodeProcess(pi.hProcess, &code);
		outExitCode = static_cast<i32>(code);
		::CloseHandle(pi.hThread);
		::CloseHandle(pi.hProcess);
		return true;
	}

	bool SupportsBackgroundThreads() {
		return true;
	}

	bool IsExternallyPaced() {
		return false;
	}

	void RunMainLoop(bool (*step)(void*), void (*onExit)(int, void*), void* user) {
		while (step && step(user)) {
		}
		if (onExit) {
			onExit(0, user);
		}
	}

} // namespace Shalltear::Platform
