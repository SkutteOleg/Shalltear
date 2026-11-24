#include "Platform/Process.h"

#include "Platform/XDGUserDirs.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace Shalltear::Platform {

	str GetExecutablePath() {
		std::vector<char> buffer(1024);
		for (;;) {
			const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
			if (length < 0) {
				return {};
			}
			if (static_cast<size_t>(length) < buffer.size()) {
				return str(buffer.data(), static_cast<size_t>(length));
			}
			buffer.resize(buffer.size() * 2);
		}
	}

	str GetUserDataDirectory() {
		if (const char* config = std::getenv("XDG_CONFIG_HOME"); config && config[0] == '/') {
			return config;
		}
		const str home = GetUserHomeDirectory();
		return home.empty() ? str() : home + "/.config";
	}

	str GetUserPicturesDirectory() {
		const str home = GetUserHomeDirectory();
		const str config = GetUserDataDirectory();
		str text;
		if (!config.empty()) {
			std::ifstream file(config + "/user-dirs.dirs");
			text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
		}
		str pictures = ParseXDGUserDir(text, "XDG_PICTURES_DIR", home);
		if (pictures.empty() && !home.empty()) {
			pictures = home + "/Pictures";
		}
		return pictures;
	}

	bool RunProcessCapture(const str& executable, std::span<const str> arguments, str& outOutput, i32& outExitCode) {
		outOutput.clear();
		outExitCode = -1;
		int pipeFds[2] = {-1, -1};
		if (::pipe(pipeFds) != 0) {
			return false;
		}
		const pid_t pid = ::fork();
		if (pid < 0) {
			::close(pipeFds[0]);
			::close(pipeFds[1]);
			return false;
		}
		if (pid == 0) {
			::close(pipeFds[0]);
			::dup2(pipeFds[1], STDOUT_FILENO);
			::dup2(pipeFds[1], STDERR_FILENO);
			::close(pipeFds[1]);
			std::vector<char*> argv;
			argv.reserve(arguments.size() + 2);
			argv.push_back(const_cast<char*>(executable.c_str()));
			for (const str& arg : arguments) {
				argv.push_back(const_cast<char*>(arg.c_str()));
			}
			argv.push_back(nullptr);
			::execv(executable.c_str(), argv.data());
			::_exit(127);
		}
		::close(pipeFds[1]);
		char buffer[4096];
		for (;;) {
			const ssize_t n = ::read(pipeFds[0], buffer, sizeof(buffer));
			if (n <= 0) {
				break;
			}
			outOutput.append(buffer, static_cast<size_t>(n));
		}
		::close(pipeFds[0]);
		int status = 0;
		::waitpid(pid, &status, 0);
		outExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		return true;
	}

} // namespace Shalltear::Platform
