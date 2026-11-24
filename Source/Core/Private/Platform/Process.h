#pragma once

#include "Shalltear/Core/Types.h"

#include <ctime>
#include <span>

namespace Shalltear::Platform {

	str GetExecutablePath();

	str GetUserDataDirectory();
	str GetUserHomeDirectory();
	str GetUserPicturesDirectory();

	// raw OS form ("en-US", "ru_RU.UTF-8")
	str GetUserLanguage();

	bool AcquireStandardOutput(bool createConsole);
	void PrepareConsoleOutput();

	void ToLocalTime(std::time_t time, std::tm& out);

	bool RunProcessCapture(const str& executable, std::span<const str> arguments, str& outOutput, i32& outExitCode);

	bool SupportsBackgroundThreads();

	bool IsExternallyPaced();

	void RunMainLoop(bool (*step)(void*), void (*onExit)(int, void*), void* user);

} // namespace Shalltear::Platform
