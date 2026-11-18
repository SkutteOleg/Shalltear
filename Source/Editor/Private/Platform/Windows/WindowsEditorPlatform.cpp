#include "Platform/EditorPlatform.h"

#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/StringUtil.h"

#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <format>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

namespace Shalltear::EditorPlatform {

	namespace {

		struct ProcessOutput {
			HANDLE handle = nullptr;
			bool cancelled = false;
		};

		bool IsHidden(const wchar_t* entry, std::span<const std::string_view> hiddenVariables) {
			for (std::string_view name : hiddenVariables) {
				std::wstring prefix = StringUtil::UTF8ToWide(name);
				prefix.push_back(L'=');
				if (::_wcsnicmp(entry, prefix.c_str(), prefix.size()) == 0) {
					return true;
				}
			}
			return false;
		}

		bool IsSet(const wchar_t* entry, std::span<const SpawnOptions::Variable> setVariables) {
			for (const SpawnOptions::Variable& variable : setVariables) {
				std::wstring prefix = StringUtil::UTF8ToWide(variable.name);
				prefix.push_back(L'=');
				if (::_wcsnicmp(entry, prefix.c_str(), prefix.size()) == 0) {
					return true;
				}
			}
			return false;
		}

		Vector<wchar_t> BuildEnvironment(std::span<const std::string_view> hiddenVariables, std::span<const SpawnOptions::Variable> setVariables) {
			Vector<wchar_t> block{GetMemoryResource(MemTag::Editor)};
			wchar_t* strings = ::GetEnvironmentStringsW();
			if (strings == nullptr) {
				return block;
			}
			for (const wchar_t* entry = strings; *entry != L'\0';) {
				size_t length = std::wcslen(entry);
				if (!IsHidden(entry, hiddenVariables) && !IsSet(entry, setVariables)) {
					block.insert(block.end(), entry, entry + length + 1);
				}
				entry += length + 1;
			}
			::FreeEnvironmentStringsW(strings);
			for (const SpawnOptions::Variable& variable : setVariables) {
				const std::wstring entry = StringUtil::UTF8ToWide(variable.name + "=" + variable.value);
				block.insert(block.end(), entry.c_str(), entry.c_str() + entry.size() + 1);
			}
			block.push_back(L'\0');
			return block;
		}

		str EnvironmentValue(const char* name) {
			const char* value = std::getenv(name);
			return value ? str(value) : str();
		}

	} // anonymous namespace

	bool SpawnProcess(const str& commandLine, const SpawnOptions& options, SpawnedProcess& out, str& outError) {
		HANDLE readEnd = nullptr;
		HANDLE writeEnd = nullptr;
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		DWORD flags = 0;

		if (options.captureOutput) {
			SECURITY_ATTRIBUTES security{};
			security.nLength = sizeof(security);
			security.bInheritHandle = TRUE;
			if (!::CreatePipe(&readEnd, &writeEnd, &security, 0)) {
				outError = std::format("couldn't create the output pipe (error {})", ::GetLastError());
				return false;
			}
			::SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

			startup.dwFlags = STARTF_USESTDHANDLES;
			startup.hStdOutput = writeEnd;
			startup.hStdError = writeEnd;
			startup.hStdInput = nullptr;
			flags |= CREATE_NO_WINDOW;
		}

		Vector<wchar_t> environment{GetMemoryResource(MemTag::Editor)};
		if (!options.hiddenVariables.empty() || !options.setVariables.empty()) {
			environment = BuildEnvironment(options.hiddenVariables, options.setVariables);
			if (!environment.empty()) {
				flags |= CREATE_UNICODE_ENVIRONMENT;
			}
		}

		std::wstring wideCommand = StringUtil::UTF8ToWide(commandLine);
		std::wstring wideDirectory = StringUtil::UTF8ToWide(options.workingDirectory);

		PROCESS_INFORMATION process{};
		BOOL created = ::CreateProcessW(nullptr, wideCommand.data(), nullptr, nullptr,
										options.captureOutput ? TRUE : FALSE, flags,
										environment.empty() ? nullptr : environment.data(),
										wideDirectory.empty() ? nullptr : wideDirectory.c_str(),
										&startup, &process);
		const DWORD error = created ? 0 : ::GetLastError();
		if (writeEnd) {
			::CloseHandle(writeEnd);
		}
		if (!created) {
			outError = std::format("error {}", error);
			if (readEnd) {
				::CloseHandle(readEnd);
			}
			return false;
		}
		::CloseHandle(process.hThread);

		out.process = process.hProcess;
		out.output = nullptr;
		if (readEnd) {
			auto* output = new ProcessOutput();
			output->handle = readEnd;
			out.output = output;
		}
		return true;
	}

	size_t ReadProcessOutput(void* output, char* buffer, size_t capacity) {
		auto* pipe = static_cast<ProcessOutput*>(output);
		DWORD read = 0;
		if (!pipe || !::ReadFile(pipe->handle, buffer, static_cast<DWORD>(capacity), &read, nullptr)) {
			return 0;
		}
		return read;
	}

	void CancelProcessOutput(void* output) {
		auto* pipe = static_cast<ProcessOutput*>(output);
		if (pipe && !pipe->cancelled) {
			pipe->cancelled = true;
			::CloseHandle(pipe->handle);
		}
	}

	void CloseProcessOutput(void* output) {
		auto* pipe = static_cast<ProcessOutput*>(output);
		if (!pipe) {
			return;
		}
		if (!pipe->cancelled) {
			::CloseHandle(pipe->handle);
		}
		delete pipe;
	}

	bool TryWaitProcess(void* process, i32& outExitCode) {
		if (::WaitForSingleObject(static_cast<HANDLE>(process), 0) != WAIT_OBJECT_0) {
			return false;
		}
		DWORD exitCode = 0;
		::GetExitCodeProcess(static_cast<HANDLE>(process), &exitCode);
		outExitCode = static_cast<i32>(exitCode);
		return true;
	}

	void KillProcess(void* process) {
		::TerminateProcess(static_cast<HANDLE>(process), 1);
	}

	void ReleaseProcess(void* process) {
		if (process) {
			::CloseHandle(static_cast<HANDLE>(process));
		}
	}

	bool OpenWithDefaultApplication(const str& path) {
		std::wstring wide = StringUtil::UTF8ToWide(path);
		auto result = reinterpret_cast<std::uintptr_t>(
			::ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
		return result > 32;
	}

	bool RevealInFileManager(const str& path, bool isDirectory) {
		std::filesystem::path preferred{path};
		str native = preferred.make_preferred().string();
		str arguments = isDirectory ? ("\"" + native + "\"") : ("/select,\"" + native + "\"");
		std::wstring wideArguments(arguments.begin(), arguments.end());
		auto result = reinterpret_cast<std::uintptr_t>(
			::ShellExecuteW(nullptr, L"open", L"explorer.exe", wideArguments.c_str(), nullptr, SW_SHOWNORMAL));
		return result > 32;
	}

	bool ToLocalTime(std::time_t time, std::tm& out) {
		return ::localtime_s(&out, &time) == 0;
	}

	std::string_view GetHostPlatformName() {
		return "Windows";
	}

	std::string_view GetExecutableSuffix() {
		return ".exe";
	}

	std::string_view GetSharedLibrarySuffix() {
		return ".dll";
	}

	char GetSearchPathSeparator() {
		return ';';
	}

	bool FileNamesFoldCase() {
		return true;
	}

	std::string_view GetDebuggerType() {
		return "cppvsdbg";
	}

	std::string_view GetDebuggerConsoleEntry() {
		return "\"console\": \"integratedTerminal\"";
	}

	void AppendDefaultToolDirectories(Vector<str>& out) {
		str programFiles = EnvironmentValue("ProgramFiles");
		if (!programFiles.empty()) {
			out.push_back(programFiles + "/LLVM/bin");
			out.push_back(programFiles + "/CMake/bin");
			out.push_back(programFiles + "/Ninja");
		}
		str programFilesX86 = EnvironmentValue("ProgramFiles(x86)");
		if (!programFilesX86.empty()) {
			out.push_back(programFilesX86 + "/LLVM/bin");
			out.push_back(programFilesX86 + "/CMake/bin");
		}
	}

	void AppendExternalEditorCandidates(Vector<ExternalEditorCandidate>& out) {
		str localAppData = EnvironmentValue("LOCALAPPDATA");
		str programFiles = EnvironmentValue("ProgramFiles");

		constexpr const char* CODE_FOLDER = "\"{folder}\"";
		constexpr const char* CODE_FILE = "\"{folder}\" --goto \"{file}:{line}:{column}\"";
		if (!localAppData.empty()) {
			out.push_back({"VS Code", localAppData + "/Programs/Microsoft VS Code/Code.exe", CODE_FOLDER, CODE_FILE});
		}
		if (!programFiles.empty()) {
			out.push_back({"VS Code", programFiles + "/Microsoft VS Code/Code.exe", CODE_FOLDER, CODE_FILE});
			out.push_back({"CLion", programFiles + "/JetBrains/CLion/bin/clion64.exe", "\"{folder}\"", "--line {line} \"{file}\""});
			out.push_back({"Rider", programFiles + "/JetBrains/Rider/bin/rider64.exe", "\"{folder}\"", "--line {line} \"{file}\""});
		}
	}

} // namespace Shalltear::EditorPlatform
