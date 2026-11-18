#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Types.h"

#include <ctime>
#include <span>
#include <string_view>

namespace Shalltear::EditorDialogs {
	struct FileDialogDesc;
}

namespace Shalltear::EditorPlatform {

	struct SpawnOptions {
		str workingDirectory;
		// child's stdout + stderr, read with ReadProcessOutput
		bool captureOutput = false;
		// variable names kept out of child's environment
		std::span<const std::string_view> hiddenVariables;
		struct Variable {
			str name;
			str value;
		};
		std::span<const Variable> setVariables;
	};

	void CollectChildEnvironment(Vector<SpawnOptions::Variable>& out);

	struct SpawnedProcess {
		void* process = nullptr;
		void* output = nullptr;
	};

	bool SpawnProcess(const str& commandLine, const SpawnOptions& options, SpawnedProcess& out, str& outError);

	// 0 at end of output or after CancelProcessOutput
	size_t ReadProcessOutput(void* output, char* buffer, size_t capacity);
	// output stays valid until CloseProcessOutput
	void CancelProcessOutput(void* output);
	void CloseProcessOutput(void* output);

	bool TryWaitProcess(void* process, i32& outExitCode);
	void KillProcess(void* process);
	void ReleaseProcess(void* process);

	// modal until closed, never call from main thread
	bool RunOpenFileDialog(const EditorDialogs::FileDialogDesc& desc, Vector<str>& outPaths, str& outError);

	bool OpenWithDefaultApplication(const str& path);
	bool RevealInFileManager(const str& path, bool isDirectory);

	bool ToLocalTime(std::time_t time, std::tm& out);

	std::string_view GetHostPlatformName();
	std::string_view GetExecutableSuffix();
	std::string_view GetSharedLibrarySuffix();
	char GetSearchPathSeparator();
	bool FileNamesFoldCase();

	// VS Code launch.json "type" value + a whole "key": value entry
	std::string_view GetDebuggerType();
	std::string_view GetDebuggerConsoleEntry();

	void AppendDefaultToolDirectories(Vector<str>& out);

	struct ExternalEditorCandidate {
		const char* name;
		str path;
		const char* folderArguments;
		const char* fileArguments;
	};
	void AppendExternalEditorCandidates(Vector<ExternalEditorCandidate>& out);

	bool ReadImportedLibraries(const Path& binary, Vector<str>& out);

} // namespace Shalltear::EditorPlatform
