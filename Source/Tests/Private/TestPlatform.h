#pragma once

#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/Input/InputCodes.h"
#include "Shalltear/Core/Math/Vector.h"
#include "Shalltear/Core/Types.h"
#include "Shalltear/Core/Window.h"

#include <string_view>
#include <vector>

namespace TestPlatform {

	using Shalltear::Handle;
	using Shalltear::i32;
	using Shalltear::Key;
	using Shalltear::MouseButton;
	using Shalltear::str;
	using Shalltear::u32;
	using Shalltear::Vec2;
	using Shalltear::Vec2I;
	using Shalltear::Window;

	void PostKey(Handle<Window> window, Key key, bool down, bool repeat = false);
	void PostText(Handle<Window> window, char32_t codepoint);
	void PostMouseMove(Handle<Window> window, Vec2I clientPx);
	void PostMouseButton(Handle<Window> window, MouseButton button, bool down, Vec2I clientPx);
	void PostMouseWheel(Handle<Window> window, Vec2 notches, Vec2I clientPx);
	void PostFocus(Handle<Window> window, bool focused);
	void PostFocusMove(Handle<Window> from, Handle<Window> to);
	void PostClose(Handle<Window> window);
	void PostWake(Handle<Window> window);

	str GetEnvironment(const char* name);
	void SetEnvironment(const char* name, const str& value);
	const char* GetUserDataVariable();
	str GetSystemSearchPath();
	char GetSearchPathSeparator();
	bool ToolDirectoriesFollowEnvironment();
	str GetAbsoluteRoot();

	size_t GetPageSize();
	Shalltear::u8* AllocateGuarded(size_t committedBytes);
	void FreeGuarded(Shalltear::u8* base, size_t committedBytes);
	void* AllocateAligned(size_t size, size_t alignment);
	void FreeAligned(void* block);

	void* ImageOf(const void* address);
	void* FindLoadedLibrary(std::string_view fileName);
	void* GetSystemImage();
	void* OpenLibrary(const str& path);
	void CloseLibrary(void* library);
	void* FindSymbol(void* library, const char* name);

	bool OpenFileRefusesDelete();
	bool StaticModulesShareTypeInfo();
	bool DenyDirectoryWrites(const str& directory);
	void AllowDirectoryWrites(const str& directory);
	bool FileNamesFoldCase();
	const char* GetCommonImportName();
	str MakeCompilerAlias(const str& compilerPath, const str& directory);
	bool MonitorsAlwaysReportRefreshRate();
	bool ChangesDisplayModes();
	bool BuildsCSharpPlayers();

	str FindSystemFont(std::string_view fileName);
	bool SystemFontsCover(char32_t codepoint);

	struct HeldFile;
	HeldFile* HoldFile(const str& path);
	void ReleaseFile(HeldFile* file);

	u32 GetProcessId();
	u32 CountProcessThreads();

	bool IsArtifactKind(const str& file, bool windowed, str& outWhy);
	str FindForeignDependency(const str& playerFile);

	struct ProcessResult {
		bool started = false;
		bool exited = false;
		i32 exitCode = -1;
	};
	ProcessResult RunProcess(const str& commandLine, u32 timeoutMs);
	str RunCommand(const str& commandLine, int& outExitCode);

	bool RunHelperProcess(int argc, char** argv, int& outExitCode);
	str GetEnvironmentProbeCommand(const char* name, const str& receiptPath, str& outUnsetText);
	str GetEditorProbeCommand(const str& receiptPath);
	str StageExternalEditor(const str& root, str& outVariable);

	struct LaunchedProcess;
	LaunchedProcess* Launch(const str& commandLine);
	void Terminate(LaunchedProcess* process);
	bool WaitForExit(LaunchedProcess* process, u32 timeoutMs, i32& outExitCode);
	bool WaitForWindow(LaunchedProcess* process, std::string_view title, u32 timeoutMs, str& outSeen);
	bool CloseWindow(LaunchedProcess* process);

	struct IconImage {
		Vec2I size;
		std::vector<Shalltear::u8> rgba;
	};
	std::vector<IconImage> ReadWindowIcon(Handle<Window> window);
	std::vector<IconImage> ReadWindowIcon(LaunchedProcess* process);
	bool ExecutablesCarryIcons();
	std::vector<IconImage> ReadExecutableIcon(const str& file);

} // namespace TestPlatform
