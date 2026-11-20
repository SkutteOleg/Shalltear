#include "TestsPCH.h"

#include "Platform/Windows/PESubsystem.h"
#include "TestHelperProcess.h"
#include "TestPlatform.h"

#include "Shalltear/Core/StringUtil.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <tlhelp32.h>

#include <cstdio>

namespace TestPlatform {

	using namespace Shalltear;

	namespace {

		struct NativeKey {
			WPARAM virtualKey = 0;
			u16 scancode = 0;
			bool extended = false;
		};

		constexpr u16 LETTER_SCANCODES[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
											  0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
		constexpr u16 DIGIT_SCANCODES[10] = {0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A};
		constexpr u16 FUNCTION_SCANCODES[24] = {0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,
												0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x76};
		constexpr u16 KEYPAD_SCANCODES[10] = {0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49};

		bool Between(Key key, Key first, Key last, u16& outIndex) {
			if (key < first || key > last) {
				return false;
			}
			outIndex = static_cast<u16>(static_cast<u16>(key) - static_cast<u16>(first));
			return true;
		}

		NativeKey ToNative(Key key) {
			u16 index = 0;
			if (Between(key, Key::A, Key::Z, index)) {
				return {static_cast<WPARAM>('A' + index), LETTER_SCANCODES[index], false};
			}
			if (Between(key, Key::D0, Key::D9, index)) {
				return {static_cast<WPARAM>('0' + index), DIGIT_SCANCODES[index], false};
			}
			if (Between(key, Key::F1, Key::F24, index)) {
				return {static_cast<WPARAM>(VK_F1 + index), FUNCTION_SCANCODES[index], false};
			}
			if (Between(key, Key::Kp0, Key::Kp9, index)) {
				return {static_cast<WPARAM>(VK_NUMPAD0 + index), KEYPAD_SCANCODES[index], false};
			}
			switch (key) {
			case Key::KpDecimal:
				return {VK_DECIMAL, 0x53, false};
			case Key::KpDivide:
				return {VK_DIVIDE, 0x35, true};
			case Key::KpMultiply:
				return {VK_MULTIPLY, 0x37, false};
			case Key::KpSubtract:
				return {VK_SUBTRACT, 0x4A, false};
			case Key::KpAdd:
				return {VK_ADD, 0x4E, false};
			case Key::KpEnter:
				return {VK_RETURN, 0x1C, true};
			case Key::KpEqual:
				return {VK_OEM_NEC_EQUAL, 0x59, false};
			case Key::Escape:
				return {VK_ESCAPE, 0x01, false};
			case Key::Enter:
				return {VK_RETURN, 0x1C, false};
			case Key::Tab:
				return {VK_TAB, 0x0F, false};
			case Key::Backspace:
				return {VK_BACK, 0x0E, false};
			case Key::Insert:
				return {VK_INSERT, 0x52, true};
			case Key::Delete:
				return {VK_DELETE, 0x53, true};
			case Key::Home:
				return {VK_HOME, 0x47, true};
			case Key::End:
				return {VK_END, 0x4F, true};
			case Key::PageUp:
				return {VK_PRIOR, 0x49, true};
			case Key::PageDown:
				return {VK_NEXT, 0x51, true};
			case Key::Left:
				return {VK_LEFT, 0x4B, true};
			case Key::Right:
				return {VK_RIGHT, 0x4D, true};
			case Key::Up:
				return {VK_UP, 0x48, true};
			case Key::Down:
				return {VK_DOWN, 0x50, true};
			case Key::Space:
				return {VK_SPACE, 0x39, false};
			case Key::CapsLock:
				return {VK_CAPITAL, 0x3A, false};
			case Key::ScrollLock:
				return {VK_SCROLL, 0x46, false};
			case Key::NumLock:
				return {VK_NUMLOCK, 0x45, true};
			case Key::PrintScreen:
				return {VK_SNAPSHOT, 0x37, true};
			case Key::Pause:
				return {VK_PAUSE, 0x45, false};
			case Key::Menu:
				return {VK_APPS, 0x5D, true};
			case Key::LeftShift:
				return {VK_SHIFT, 0x2A, false};
			case Key::RightShift:
				return {VK_SHIFT, 0x36, false};
			case Key::LeftCtrl:
				return {VK_CONTROL, 0x1D, false};
			case Key::RightCtrl:
				return {VK_CONTROL, 0x1D, true};
			case Key::LeftAlt:
				return {VK_MENU, 0x38, false};
			case Key::RightAlt:
				return {VK_MENU, 0x38, true};
			case Key::LeftSuper:
				return {VK_LWIN, 0x5B, true};
			case Key::RightSuper:
				return {VK_RWIN, 0x5C, true};
			case Key::Apostrophe:
				return {VK_OEM_7, 0x28, false};
			case Key::Comma:
				return {VK_OEM_COMMA, 0x33, false};
			case Key::Minus:
				return {VK_OEM_MINUS, 0x0C, false};
			case Key::Period:
				return {VK_OEM_PERIOD, 0x34, false};
			case Key::Slash:
				return {VK_OEM_2, 0x35, false};
			case Key::Semicolon:
				return {VK_OEM_1, 0x27, false};
			case Key::Equal:
				return {VK_OEM_PLUS, 0x0D, false};
			case Key::LeftBracket:
				return {VK_OEM_4, 0x1A, false};
			case Key::Backslash:
				return {VK_OEM_5, 0x2B, false};
			case Key::RightBracket:
				return {VK_OEM_6, 0x1B, false};
			case Key::GraveAccent:
				return {VK_OEM_3, 0x29, false};
			default:
				return {};
			}
		}

		HWND Native(Handle<Window> window) {
			return window.IsValid() ? static_cast<HWND>(window->GetNativeHandle().window) : nullptr;
		}

		LPARAM At(Vec2I clientPx) {
			return MAKELPARAM(clientPx.x, clientPx.y);
		}

		HANDLE KillOnCloseJob() {
			HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
			if (job) {
				JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
				limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
				::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
			}
			return job;
		}

		struct WindowSearch {
			DWORD id = 0;
			std::wstring title;
			HWND found = nullptr;
			str seen;
		};

		BOOL CALLBACK FindWindowOfProcess(HWND window, LPARAM parameter) {
			WindowSearch* search = reinterpret_cast<WindowSearch*>(parameter);
			DWORD owner = 0;
			::GetWindowThreadProcessId(window, &owner);
			if (owner != search->id || !::IsWindowVisible(window) || ::GetWindow(window, GW_OWNER) != nullptr) {
				return TRUE;
			}
			wchar_t text[256] = {};
			::GetWindowTextW(window, text, static_cast<int>(std::size(text)));
			search->seen += "\n    \"" + StringUtil::WideToUTF8(text) + "\"";
			if (search->title == text) {
				search->found = window;
				return FALSE;
			}
			return TRUE;
		}

	} // anonymous namespace

	struct LaunchedProcess {
		HANDLE process = nullptr;
		DWORD id = 0;
		HWND window = nullptr;
	};

	struct HeldFile {
		HANDLE handle = INVALID_HANDLE_VALUE;
	};

	void PostKey(Handle<Window> window, Key key, bool down, bool repeat) {
		const NativeKey native = ToNative(key);
		if (native.virtualKey == 0) {
			ADD_FAILURE() << "no native key for Key " << static_cast<u32>(key);
			return;
		}
		LPARAM lParam = 1;
		lParam |= static_cast<LPARAM>(native.scancode & 0xFF) << 16;
		if (native.extended) {
			lParam |= static_cast<LPARAM>(1) << 24;
		}
		if (repeat || !down) {
			lParam |= static_cast<LPARAM>(1) << 30;
		}
		if (!down) {
			lParam |= static_cast<LPARAM>(1) << 31;
		}
		::PostMessageW(Native(window), down ? WM_KEYDOWN : WM_KEYUP, native.virtualKey, lParam);
	}

	void PostText(Handle<Window> window, char32_t codepoint) {
		if (codepoint >= 0x10000) {
			const char32_t offset = codepoint - 0x10000;
			::PostMessageW(Native(window), WM_CHAR, 0xD800 + (offset >> 10), 1);
			::PostMessageW(Native(window), WM_CHAR, 0xDC00 + (offset & 0x3FF), 1);
			return;
		}
		::PostMessageW(Native(window), WM_CHAR, static_cast<WPARAM>(codepoint), 1);
	}

	void PostMouseMove(Handle<Window> window, Vec2I clientPx) {
		::PostMessageW(Native(window), WM_MOUSEMOVE, 0, At(clientPx));
	}

	void PostMouseButton(Handle<Window> window, MouseButton button, bool down, Vec2I clientPx) {
		UINT message = 0;
		WPARAM wParam = 0;
		switch (button) {
		case MouseButton::Left:
			message = down ? WM_LBUTTONDOWN : WM_LBUTTONUP;
			wParam = down ? MK_LBUTTON : 0;
			break;
		case MouseButton::Right:
			message = down ? WM_RBUTTONDOWN : WM_RBUTTONUP;
			wParam = down ? MK_RBUTTON : 0;
			break;
		case MouseButton::Middle:
			message = down ? WM_MBUTTONDOWN : WM_MBUTTONUP;
			wParam = down ? MK_MBUTTON : 0;
			break;
		case MouseButton::X1:
		case MouseButton::X2: {
			const WORD which = button == MouseButton::X1 ? XBUTTON1 : XBUTTON2;
			message = down ? WM_XBUTTONDOWN : WM_XBUTTONUP;
			wParam = MAKEWPARAM(down ? (which == XBUTTON1 ? MK_XBUTTON1 : MK_XBUTTON2) : 0, which);
			break;
		}
		default:
			ADD_FAILURE() << "no native button for MouseButton " << static_cast<u32>(button);
			return;
		}
		::PostMessageW(Native(window), message, wParam, At(clientPx));
	}

	void PostMouseWheel(Handle<Window> window, Vec2 notches, Vec2I clientPx) {
		POINT screen{clientPx.x, clientPx.y};
		::ClientToScreen(Native(window), &screen);
		const LPARAM at = MAKELPARAM(screen.x, screen.y);
		if (notches.y != 0.0f) {
			const i32 delta = static_cast<i32>(notches.y * static_cast<f32>(WHEEL_DELTA));
			::PostMessageW(Native(window), WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(delta)), at);
		}
		if (notches.x != 0.0f) {
			const i32 delta = static_cast<i32>(notches.x * static_cast<f32>(WHEEL_DELTA));
			::PostMessageW(Native(window), WM_MOUSEHWHEEL, MAKEWPARAM(0, static_cast<WORD>(delta)), at);
		}
	}

	void PostFocus(Handle<Window> window, bool focused) {
		::PostMessageW(Native(window), focused ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
	}

	void PostFocusMove(Handle<Window> from, Handle<Window> to) {
		::PostMessageW(Native(from), WM_KILLFOCUS, reinterpret_cast<WPARAM>(Native(to)), 0);
		::PostMessageW(Native(to), WM_SETFOCUS, reinterpret_cast<WPARAM>(Native(from)), 0);
	}

	void PostClose(Handle<Window> window) {
		::PostMessageW(Native(window), WM_CLOSE, 0, 0);
	}

	void PostWake(Handle<Window> window) {
		::PostMessageW(Native(window), WM_NULL, 0, 0);
	}

	str GetEnvironment(const char* name) {
		size_t length = 0;
		char buffer[32768];
		if (::getenv_s(&length, buffer, sizeof(buffer), name) == 0 && length > 0) {
			return str(buffer, length - 1);
		}
		return {};
	}

	void SetEnvironment(const char* name, const str& value) {
		::_putenv_s(name, value.c_str());
		::SetEnvironmentVariableA(name, value.empty() ? nullptr : value.c_str());
	}

	const char* GetUserDataVariable() {
		return "LOCALAPPDATA";
	}

	str GetSystemSearchPath() {
		return "C:\\Windows\\system32";
	}

	char GetSearchPathSeparator() {
		return ';';
	}

	bool ToolDirectoriesFollowEnvironment() {
		return true;
	}

	str GetAbsoluteRoot() {
		return "C:/";
	}

	size_t GetPageSize() {
		SYSTEM_INFO info{};
		::GetSystemInfo(&info);
		return info.dwPageSize;
	}

	u8* AllocateGuarded(size_t committedBytes) {
		u8* base = static_cast<u8*>(::VirtualAlloc(nullptr, committedBytes + GetPageSize(), MEM_RESERVE, PAGE_NOACCESS));
		if (base) {
			::VirtualAlloc(base, committedBytes, MEM_COMMIT, PAGE_READWRITE);
		}
		return base;
	}

	void FreeGuarded(u8* base, size_t) {
		if (base) {
			::VirtualFree(base, 0, MEM_RELEASE);
		}
	}

	void* ImageOf(const void* address) {
		HMODULE image = nullptr;
		if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
								  static_cast<LPCWSTR>(address), &image)) {
			return nullptr;
		}
		return image;
	}

	void* FindLoadedLibrary(std::string_view fileName) {
		return ::GetModuleHandleA(str(fileName).c_str());
	}

	void* GetSystemImage() {
		return ::GetModuleHandleW(L"kernel32.dll");
	}

	void* OpenLibrary(const str& path) {
		return ::LoadLibraryW(StringUtil::UTF8ToWide(path).c_str());
	}

	void CloseLibrary(void* library) {
		if (library) {
			::FreeLibrary(static_cast<HMODULE>(library));
		}
	}

	void* FindSymbol(void* library, const char* name) {
		return library ? reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(library), name)) : nullptr;
	}

	bool MonitorsAlwaysReportRefreshRate() {
		return true;
	}

	bool ChangesDisplayModes() {
		return true;
	}

	bool BuildsCSharpPlayers() {
		return true;
	}

	void* AllocateAligned(size_t size, size_t alignment) {
		return ::_aligned_malloc(size, alignment);
	}

	void FreeAligned(void* block) {
		::_aligned_free(block);
	}

	bool FileNamesFoldCase() {
		return true;
	}

	const char* GetCommonImportName() {
		return "KERNEL32";
	}

	str MakeCompilerAlias(const str& compilerPath, const str&) {
		return (std::filesystem::path(compilerPath).parent_path() / "x86_64-w64-mingw32-c++.exe").generic_string();
	}

	bool StaticModulesShareTypeInfo() {
		return true;
	}

	bool DenyDirectoryWrites(const str& directory) {
		int exitCode = 0;
		RunCommand("icacls \"" + directory + "\" /deny \"*S-1-1-0:(OI)(CI)(WD,AD,WEA,WA)\" /T /C", exitCode);
		return exitCode == 0;
	}

	void AllowDirectoryWrites(const str& directory) {
		int exitCode = 0;
		RunCommand("icacls \"" + directory + "\" /remove:d \"*S-1-1-0\" /T /C", exitCode);
	}

	bool DenyFileReads(const str& file) {
		int exitCode = 0;
		RunCommand("icacls \"" + file + "\" /deny \"*S-1-1-0:(RD)\"", exitCode);
		return exitCode == 0;
	}

	void AllowFileReads(const str& file) {
		int exitCode = 0;
		RunCommand("icacls \"" + file + "\" /remove:d \"*S-1-1-0\"", exitCode);
	}

	bool OpenFileRefusesDelete() {
		return true;
	}

	str FindSystemFont(std::string_view fileName) {
		const str path = GetEnvironment("WINDIR") + "/Fonts/" + str(fileName);
		return std::filesystem::exists(path) ? path : str();
	}

	bool SystemFontsCover(char32_t) {
		return true;
	}

	HeldFile* HoldFile(const str& path) {
		const HANDLE handle =
			::CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE) {
			return nullptr;
		}
		HeldFile* held = new HeldFile;
		held->handle = handle;
		return held;
	}

	void ReleaseFile(HeldFile* file) {
		if (file) {
			::CloseHandle(file->handle);
			delete file;
		}
	}

	u32 GetProcessId() {
		return ::GetCurrentProcessId();
	}

	u32 CountProcessThreads() {
		const DWORD self = ::GetCurrentProcessId();
		const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snapshot == INVALID_HANDLE_VALUE) {
			return 0;
		}
		THREADENTRY32 entry{};
		entry.dwSize = sizeof(entry);
		u32 count = 0;
		if (::Thread32First(snapshot, &entry)) {
			do {
				if (entry.th32OwnerProcessID == self) {
					++count;
				}
			} while (::Thread32Next(snapshot, &entry));
		}
		::CloseHandle(snapshot);
		return count;
	}

	bool IsArtifactKind(const str& file, bool windowed, str& outWhy) {
		const u16 subsystem = PESubsystem::Read(file);
		outWhy = "PE subsystem " + std::to_string(subsystem);
		return subsystem == (windowed ? PESubsystem::GUI : PESubsystem::CONSOLE);
	}

	str FindForeignDependency(const str&) {
		return {};
	}

	ProcessResult RunProcess(const str& commandLine, u32 timeoutMs) {
		ProcessResult result;
		std::wstring mutableLine = StringUtil::UTF8ToWide(commandLine);
		HANDLE job = KillOnCloseJob();
		if (!job) {
			return result;
		}

		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION process{};
		if (!::CreateProcessW(nullptr, mutableLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
							  nullptr, nullptr, &startup, &process)) {
			::CloseHandle(job);
			return result;
		}
		result.started = true;
		::AssignProcessToJobObject(job, process.hProcess);
		::ResumeThread(process.hThread);

		if (::WaitForSingleObject(process.hProcess, timeoutMs) == WAIT_OBJECT_0) {
			DWORD exitCode = static_cast<DWORD>(-1);
			::GetExitCodeProcess(process.hProcess, &exitCode);
			result.exited = true;
			result.exitCode = static_cast<i32>(exitCode);
		} else {
			::TerminateProcess(process.hProcess, 1);
		}
		::CloseHandle(process.hThread);
		::CloseHandle(process.hProcess);
		::CloseHandle(job);
		return result;
	}

	str RunCommand(const str& commandLine, int& outExitCode) {
		const str command = "\"" + commandLine + " 2>&1\"";
		str output;
		FILE* pipe = ::_popen(command.c_str(), "r");
		if (!pipe) {
			outExitCode = -1;
			return output;
		}
		char buffer[512];
		while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
			output += buffer;
		}
		outExitCode = ::_pclose(pipe);
		return output;
	}

	bool RunHelperProcess(int argc, char** argv, int& outExitCode) {
		for (int arg = 1; arg < argc; ++arg) {
			if (std::strcmp(argv[arg], TestHelpers::WINDOW_HOST_FLAG) == 0) {
				outExitCode = TestHelpers::RunWindowHost();
				return true;
			}
			if (std::strcmp(argv[arg], TestHelpers::SHOW_STATE_FLAG) == 0) {
				outExitCode = TestHelpers::RunShowStateProbe();
				return true;
			}
			if (std::strcmp(argv[arg], TestHelpers::DATA_ASSET_RELOAD_FLAG) == 0) {
				outExitCode = TestHelpers::RunDataAssetReloadProbe(arg + 1 < argc ? argv[arg + 1] : nullptr);
				return true;
			}
#ifdef SLT_TEST_CSHARP_AOT
			if (std::strcmp(argv[arg], TestHelpers::CSHARP_SHIM_FLAG) == 0 && arg + 3 < argc) {
				outExitCode = TestHelpers::RunCSharpShimProbe(argv[arg + 1], argv[arg + 2], argv[arg + 3]);
				return true;
			}
#endif
		}
		return false;
	}

	str GetEnvironmentProbeCommand(const char* name, const str& receiptPath, str& outUnsetText) {
		outUnsetText = "[%" + str(name) + "%]";
		return "cmd.exe /c \"echo [%" + str(name) + "%]> \"" + receiptPath + "\"\"";
	}

	str GetEditorProbeCommand(const str& receiptPath) {
		return "cmd.exe /c \"echo [{file}] [{line}]> \"" + receiptPath + "\"\"";
	}

	str StageExternalEditor(const str& root, str& outVariable) {
		const std::filesystem::path programs = std::filesystem::path(root) / "AppData" / "Programs" / "Microsoft VS Code";
		std::filesystem::create_directories(programs);
		std::ofstream stub(programs / "Code.exe", std::ios::binary);
		stub << "stub";
		outVariable = "LOCALAPPDATA";
		return (std::filesystem::path(root) / "AppData").generic_string();
	}

	LaunchedProcess* Launch(const str& commandLine) {
		str mutableLine = commandLine;
		STARTUPINFOA startup{};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION info{};
		if (!::CreateProcessA(nullptr, mutableLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) {
			return nullptr;
		}
		::CloseHandle(info.hThread);
		LaunchedProcess* launched = new LaunchedProcess;
		launched->process = info.hProcess;
		launched->id = info.dwProcessId;
		return launched;
	}

	void Terminate(LaunchedProcess* process) {
		if (!process) {
			return;
		}
		::TerminateProcess(process->process, 1);
		::CloseHandle(process->process);
		delete process;
	}

	bool WaitForExit(LaunchedProcess* process, u32 timeoutMs, i32& outExitCode) {
		if (!process || ::WaitForSingleObject(process->process, timeoutMs) != WAIT_OBJECT_0) {
			return false;
		}
		DWORD exitCode = static_cast<DWORD>(-1);
		::GetExitCodeProcess(process->process, &exitCode);
		outExitCode = static_cast<i32>(exitCode);
		return true;
	}

	bool WaitForWindow(LaunchedProcess* process, std::string_view title, u32 timeoutMs, str& outSeen) {
		if (!process) {
			return false;
		}
		const DWORD deadline = ::GetTickCount() + timeoutMs;
		while (::GetTickCount() < deadline) {
			WindowSearch search;
			search.id = process->id;
			search.title = StringUtil::UTF8ToWide(title);
			::EnumWindows(&FindWindowOfProcess, reinterpret_cast<LPARAM>(&search));
			if (search.found) {
				process->window = search.found;
				return true;
			}
			outSeen = search.seen;
			::Sleep(100);
		}
		return false;
	}

	bool CloseWindow(LaunchedProcess* process) {
		return process && process->window && ::PostMessageW(process->window, WM_CLOSE, 0, 0) != 0;
	}

	namespace {

		bool ReadIcon(HICON icon, IconImage& out) {
			ICONINFO info{};
			if (!icon || !::GetIconInfo(icon, &info)) {
				return false;
			}
			bool read = false;
			BITMAP bitmap{};
			if (info.hbmColor && ::GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) != 0) {
				BITMAPINFO header{};
				header.bmiHeader.biSize = sizeof(header.bmiHeader);
				header.bmiHeader.biWidth = bitmap.bmWidth;
				header.bmiHeader.biHeight = -bitmap.bmHeight;
				header.bmiHeader.biPlanes = 1;
				header.bmiHeader.biBitCount = 32;
				header.bmiHeader.biCompression = BI_RGB;
				std::vector<u8> bgra(static_cast<size_t>(bitmap.bmWidth) * static_cast<size_t>(bitmap.bmHeight) * 4);
				HDC dc = ::CreateCompatibleDC(nullptr);
				read = ::GetDIBits(dc, info.hbmColor, 0, static_cast<UINT>(bitmap.bmHeight), bgra.data(), &header,
								   DIB_RGB_COLORS) == bitmap.bmHeight;
				::DeleteDC(dc);
				out.size = {bitmap.bmWidth, bitmap.bmHeight};
				out.rgba.resize(bgra.size());
				for (size_t i = 0; i < bgra.size(); i += 4) {
					out.rgba[i] = bgra[i + 2];
					out.rgba[i + 1] = bgra[i + 1];
					out.rgba[i + 2] = bgra[i];
					out.rgba[i + 3] = bgra[i + 3];
				}
			}
			if (info.hbmColor) {
				::DeleteObject(info.hbmColor);
			}
			if (info.hbmMask) {
				::DeleteObject(info.hbmMask);
			}
			return read;
		}

		std::vector<IconImage> ReadIcons(HWND hwnd) {
			std::vector<IconImage> out;
			if (!hwnd) {
				return out;
			}
			const std::pair<WPARAM, int> kinds[] = {{ICON_BIG, GCLP_HICON}, {ICON_SMALL, GCLP_HICONSM}};
			for (const auto& [message, classIndex] : kinds) {
				DWORD_PTR result = 0;
				::SendMessageTimeoutW(hwnd, WM_GETICON, message, 0, SMTO_ABORTIFHUNG, 2000, &result);
				HICON icon = reinterpret_cast<HICON>(result);
				if (!icon) {
					icon = reinterpret_cast<HICON>(::GetClassLongPtrW(hwnd, classIndex));
				}
				IconImage image;
				if (ReadIcon(icon, image)) {
					out.push_back(std::move(image));
				}
			}
			return out;
		}

		struct IconGroup {
			const u8* bytes = nullptr;
			DWORD size = 0;
		};

		BOOL CALLBACK FindIconGroup(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR user) {
			IconGroup& group = *reinterpret_cast<IconGroup*>(user);
			HRSRC found = ::FindResourceW(module, name, type);
			HGLOBAL loaded = found ? ::LoadResource(module, found) : nullptr;
			group.bytes = loaded ? static_cast<const u8*>(::LockResource(loaded)) : nullptr;
			group.size = found ? ::SizeofResource(module, found) : 0;
			return FALSE;
		}

	} // anonymous namespace

	std::vector<IconImage> ReadWindowIcon(Handle<Window> window) {
		return window.IsValid() ? ReadIcons(static_cast<HWND>(window->GetNativeHandle().window)) : std::vector<IconImage>{};
	}

	std::vector<IconImage> ReadWindowIcon(LaunchedProcess* process) {
		return process ? ReadIcons(process->window) : std::vector<IconImage>{};
	}

	bool ExecutablesCarryIcons() {
		return true;
	}

	std::vector<IconImage> ReadExecutableIcon(const str& file) {
		std::vector<IconImage> out;
		HMODULE module = ::LoadLibraryExW(StringUtil::UTF8ToWide(file).c_str(), nullptr,
										  LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
		if (!module) {
			return out;
		}
		IconGroup group;
		::EnumResourceNamesW(module, reinterpret_cast<LPCWSTR>(RT_GROUP_ICON), &FindIconGroup, reinterpret_cast<LONG_PTR>(&group));
		const u32 count = group.bytes && group.size >= 6 ? static_cast<u32>(group.bytes[4] | (group.bytes[5] << 8)) : 0;
		for (u32 i = 0; i < count && 6 + 14 * static_cast<size_t>(i + 1) <= group.size; ++i) {
			const u8* entry = group.bytes + 6 + 14 * static_cast<size_t>(i);
			const WORD id = static_cast<WORD>(entry[12] | (entry[13] << 8));
			const int edge = entry[0] == 0 ? 256 : entry[0];
			HRSRC found = ::FindResourceW(module, MAKEINTRESOURCEW(id), reinterpret_cast<LPCWSTR>(RT_ICON));
			HGLOBAL loaded = found ? ::LoadResource(module, found) : nullptr;
			BYTE* bytes = loaded ? static_cast<BYTE*>(::LockResource(loaded)) : nullptr;
			HICON icon = bytes ? ::CreateIconFromResourceEx(bytes, ::SizeofResource(module, found), TRUE, 0x00030000, edge,
															edge, LR_DEFAULTCOLOR)
							   : nullptr;
			IconImage image;
			if (ReadIcon(icon, image)) {
				out.push_back(std::move(image));
			}
			if (icon) {
				::DestroyIcon(icon);
			}
		}
		::FreeLibrary(module);
		return out;
	}

} // namespace TestPlatform
