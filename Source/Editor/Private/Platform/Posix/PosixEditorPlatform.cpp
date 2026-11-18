#include "Platform/Posix/PosixSpawn.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

namespace Shalltear::EditorPlatform {

	namespace {

		struct Process {
			pid_t pid = -1;
			bool ownsGroup = false;
		};

		struct ProcessOutput {
			int descriptor = -1;
			int wake[2] = {-1, -1};
		};

		struct Released {
			std::mutex mutex;
			std::vector<pid_t> pids;
		};

		Released& GetReleased() {
			static Released released;
			return released;
		}

		// child nobody waits for stays a zombie
		void ReapReleased() {
			Released& released = GetReleased();
			std::lock_guard lock(released.mutex);
			for (size_t i = 0; i < released.pids.size();) {
				int status = 0;
				if (::waitpid(released.pids[i], &status, WNOHANG) != 0) {
					released.pids.erase(released.pids.begin() + static_cast<std::ptrdiff_t>(i));
				} else {
					++i;
				}
			}
		}

		bool IsHidden(const char* entry, std::span<const std::string_view> hiddenVariables) {
			for (std::string_view name : hiddenVariables) {
				if (std::strncmp(entry, name.data(), name.size()) == 0 && entry[name.size()] == '=') {
					return true;
				}
			}
			return false;
		}

		int ExitCodeOf(int status) {
			if (WIFEXITED(status)) {
				return WEXITSTATUS(status);
			}
			return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
		}

	} // anonymous namespace

	Vector<str> SplitCommandLine(const str& commandLine) {
		Vector<str> arguments{GetMemoryResource(MemTag::Editor)};
		str current;
		bool quoted = false;
		bool open = false;
		for (size_t i = 0; i < commandLine.size(); ++i) {
			const char c = commandLine[i];
			if (c == '\\' && i + 1 < commandLine.size() && commandLine[i + 1] == '"') {
				current.push_back('"');
				open = true;
				++i;
			} else if (c == '"') {
				quoted = !quoted;
				open = true;
			} else if (!quoted && (c == ' ' || c == '\t')) {
				if (open) {
					arguments.push_back(std::move(current));
					current.clear();
					open = false;
				}
			} else {
				current.push_back(c);
				open = true;
			}
		}
		if (open) {
			arguments.push_back(std::move(current));
		}
		return arguments;
	}

	bool SpawnArguments(const Vector<str>& arguments, const SpawnOptions& options, SpawnedProcess& out, str& outError) {
		ReapReleased();
		if (arguments.empty()) {
			outError = "empty command line";
			return false;
		}

		int pipeEnds[2] = {-1, -1};
		int wake[2] = {-1, -1};
		if (options.captureOutput && (::pipe2(pipeEnds, O_CLOEXEC) != 0 || ::pipe2(wake, O_CLOEXEC | O_NONBLOCK) != 0)) {
			outError = std::string("couldn't create the output pipe (") + std::strerror(errno) + ")";
			if (pipeEnds[0] >= 0) {
				::close(pipeEnds[0]);
				::close(pipeEnds[1]);
			}
			return false;
		}

		posix_spawn_file_actions_t actions;
		::posix_spawn_file_actions_init(&actions);
		::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
		if (options.captureOutput) {
			::posix_spawn_file_actions_adddup2(&actions, pipeEnds[1], STDOUT_FILENO);
			::posix_spawn_file_actions_adddup2(&actions, pipeEnds[1], STDERR_FILENO);
		}
		if (!options.workingDirectory.empty()) {
			::posix_spawn_file_actions_addchdir_np(&actions, options.workingDirectory.c_str());
		}

		posix_spawnattr_t attributes;
		::posix_spawnattr_init(&attributes);
		// own group to kill children too, own session to outlive editor
		::posix_spawnattr_setpgroup(&attributes, 0);
		::posix_spawnattr_setflags(&attributes, options.captureOutput ? POSIX_SPAWN_SETPGROUP : POSIX_SPAWN_SETSID);

		std::vector<char*> argv;
		for (const str& argument : arguments) {
			argv.push_back(const_cast<char*>(argument.c_str()));
		}
		argv.push_back(nullptr);

		std::vector<str> set;
		set.reserve(options.setVariables.size());
		std::vector<char*> environment;
		for (char** entry = environ; entry && *entry; ++entry) {
			const bool replaced = std::any_of(options.setVariables.begin(), options.setVariables.end(), [entry](const SpawnOptions::Variable& variable) {
				return std::strncmp(*entry, variable.name.c_str(), variable.name.size()) == 0 && (*entry)[variable.name.size()] == '=';
			});
			if (!replaced && !IsHidden(*entry, options.hiddenVariables)) {
				environment.push_back(*entry);
			}
		}
		for (const SpawnOptions::Variable& variable : options.setVariables) {
			environment.push_back(set.emplace_back(variable.name + "=" + variable.value).data());
		}
		environment.push_back(nullptr);

		pid_t pid = -1;
		const int result = ::posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), environment.data());
		::posix_spawn_file_actions_destroy(&actions);
		::posix_spawnattr_destroy(&attributes);
		if (pipeEnds[1] >= 0) {
			::close(pipeEnds[1]);
		}
		if (result != 0) {
			outError = std::strerror(result);
			if (pipeEnds[0] >= 0) {
				::close(pipeEnds[0]);
				::close(wake[0]);
				::close(wake[1]);
			}
			return false;
		}

		auto* process = new Process();
		process->pid = pid;
		process->ownsGroup = options.captureOutput;
		out.process = process;
		out.output = nullptr;
		if (options.captureOutput) {
			auto* output = new ProcessOutput();
			output->descriptor = pipeEnds[0];
			output->wake[0] = wake[0];
			output->wake[1] = wake[1];
			out.output = output;
		}
		return true;
	}

	bool SpawnProcess(const str& commandLine, const SpawnOptions& options, SpawnedProcess& out, str& outError) {
		return SpawnArguments(SplitCommandLine(commandLine), options, out, outError);
	}

	size_t ReadProcessOutput(void* output, char* buffer, size_t capacity) {
		auto* pipe = static_cast<ProcessOutput*>(output);
		if (!pipe) {
			return 0;
		}
		for (;;) {
			pollfd waits[2] = {{pipe->descriptor, POLLIN, 0}, {pipe->wake[0], POLLIN, 0}};
			if (::poll(waits, 2, -1) < 0) {
				if (errno == EINTR) {
					continue;
				}
				return 0;
			}
			if (waits[1].revents != 0) {
				return 0;
			}
			const ssize_t read = ::read(pipe->descriptor, buffer, capacity);
			if (read < 0 && errno == EINTR) {
				continue;
			}
			return read > 0 ? static_cast<size_t>(read) : 0;
		}
	}

	void CancelProcessOutput(void* output) {
		if (auto* pipe = static_cast<ProcessOutput*>(output)) {
			const char wake = 1;
			[[maybe_unused]] const ssize_t written = ::write(pipe->wake[1], &wake, 1);
		}
	}

	void CloseProcessOutput(void* output) {
		auto* pipe = static_cast<ProcessOutput*>(output);
		if (!pipe) {
			return;
		}
		::close(pipe->descriptor);
		::close(pipe->wake[0]);
		::close(pipe->wake[1]);
		delete pipe;
	}

	bool TryWaitProcess(void* process, i32& outExitCode) {
		auto* child = static_cast<Process*>(process);
		if (!child || child->pid < 0) {
			return false;
		}
		int status = 0;
		const pid_t waited = ::waitpid(child->pid, &status, WNOHANG);
		if (waited == 0) {
			return false;
		}
		child->pid = -1;
		outExitCode = waited > 0 ? ExitCodeOf(status) : 1;
		return true;
	}

	void KillProcess(void* process) {
		auto* child = static_cast<Process*>(process);
		if (!child || child->pid < 0) {
			return;
		}
		::kill(child->ownsGroup ? -child->pid : child->pid, SIGKILL);
		int status = 0;
		::waitpid(child->pid, &status, 0);
		child->pid = -1;
	}

	void ReleaseProcess(void* process) {
		auto* child = static_cast<Process*>(process);
		if (!child) {
			return;
		}
		if (child->pid >= 0) {
			Released& released = GetReleased();
			std::lock_guard lock(released.mutex);
			released.pids.push_back(child->pid);
		}
		delete child;
		ReapReleased();
	}

	bool ToLocalTime(std::time_t time, std::tm& out) {
		return ::localtime_r(&time, &out) != nullptr;
	}

	std::string_view GetExecutableSuffix() {
		return {};
	}

	char GetSearchPathSeparator() {
		return ':';
	}

	std::string_view GetDebuggerType() {
		return "cppdbg";
	}

	std::string_view GetDebuggerConsoleEntry() {
		return "\"externalConsole\": false";
	}

} // namespace Shalltear::EditorPlatform
