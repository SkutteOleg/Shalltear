#include "Shalltear/Editor/ChildProcess.h"

#include "Platform/EditorPlatform.h"

#include "Shalltear/Core/Log.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <vector>

namespace Shalltear {

	namespace {

		constexpr size_t MAX_QUEUED_LINES = 20000;

		constexpr size_t READ_CHUNK = 4096;

		struct Contribution {
			str name;
			str value;
			const void* owner = nullptr;
			bool searchPath = false;
		};

		std::mutex _contributionMutex;
		std::vector<Contribution> _contributions;

	} // anonymous namespace

	namespace ChildEnvironment {

		void Set(std::string_view name, std::string_view value, const void* owner) {
			std::lock_guard lock(_contributionMutex);
			for (Contribution& contribution : _contributions) {
				if (!contribution.searchPath && contribution.owner == owner && contribution.name == name) {
					contribution.value = value;
					return;
				}
			}
			_contributions.push_back({str(name), str(value), owner, false});
		}

		void PrependSearchPath(std::string_view directory, const void* owner) {
			std::lock_guard lock(_contributionMutex);
			for (const Contribution& contribution : _contributions) {
				if (contribution.searchPath && contribution.owner == owner && contribution.value == directory) {
					return;
				}
			}
			_contributions.push_back({str(), str(directory), owner, true});
		}

		u32 UnregisterOwnedBy(const void* owner) {
			std::lock_guard lock(_contributionMutex);
			return static_cast<u32>(std::erase_if(_contributions, [owner](const Contribution& contribution) { return contribution.owner == owner; }));
		}

	} // namespace ChildEnvironment

	void EditorPlatform::CollectChildEnvironment(Vector<SpawnOptions::Variable>& out) {
		std::lock_guard lock(_contributionMutex);
		str searchPath;
		for (const Contribution& contribution : _contributions) {
			if (contribution.searchPath) {
				searchPath += contribution.value;
				searchPath.push_back(GetSearchPathSeparator());
				continue;
			}
			const auto same = std::find_if(out.begin(), out.end(), [&](const SpawnOptions::Variable& variable) { return variable.name == contribution.name; });
			if (same != out.end()) {
				same->value = contribution.value;
			} else {
				out.push_back({contribution.name, contribution.value});
			}
		}
		if (!searchPath.empty()) {
			const char* inherited = std::getenv("PATH");
			out.push_back({"PATH", searchPath + (inherited ? inherited : "")});
		}
	}

	str QuoteCommandArgument(const str& value) {
		return "\"" + value + "\"";
	}

	bool StartDetachedProcess(const str& commandLine, const str& workingDirectory) {
		Vector<EditorPlatform::SpawnOptions::Variable> environment{GetMemoryResource(MemTag::Editor)};
		EditorPlatform::CollectChildEnvironment(environment);
		EditorPlatform::SpawnOptions options;
		options.workingDirectory = workingDirectory;
		options.setVariables = environment;
		EditorPlatform::SpawnedProcess spawned;
		str error;
		if (!EditorPlatform::SpawnProcess(commandLine, options, spawned, error)) {
			SLT_LOG_ERROR("Editor", "couldn't launch '{}' ({})", commandLine, error);
			return false;
		}
		EditorPlatform::ReleaseProcess(spawned.process);
		return true;
	}

	ChildProcess::~ChildProcess() {
		Detach();
	}

	bool ChildProcess::Start(const str& commandLine, const str& workingDirectory, std::span<const ChildVariable> variables) {
		if (_running) {
			return false;
		}

		Vector<EditorPlatform::SpawnOptions::Variable> environment{GetMemoryResource(MemTag::Editor)};
		EditorPlatform::CollectChildEnvironment(environment);
		for (const ChildVariable& variable : variables) {
			const auto same = std::find_if(environment.begin(), environment.end(),
										   [&variable](const EditorPlatform::SpawnOptions::Variable& set) { return set.name == variable.name; });
			if (same != environment.end()) {
				same->value = variable.value;
			} else {
				environment.push_back({variable.name, variable.value});
			}
		}
		EditorPlatform::SpawnOptions options;
		options.workingDirectory = workingDirectory;
		options.captureOutput = true;
		options.setVariables = environment;
		EditorPlatform::SpawnedProcess spawned;
		str error;
		if (!EditorPlatform::SpawnProcess(commandLine, options, spawned, error)) {
			SLT_LOG_ERROR("Editor", "child process: couldn't start '{}' ({})", commandLine, error);
			return false;
		}

		_process = spawned.process;
		_readEnd = spawned.output;
		_running = true;
		{
			std::lock_guard lock(_mutex);
			_lines.clear();
			_partial.clear();
			_droppedLines = 0;
		}

		_reader = std::thread([this, output = spawned.output]() {
			char chunk[READ_CHUNK];
			for (;;) {
				const size_t read = EditorPlatform::ReadProcessOutput(output, chunk, READ_CHUNK);
				if (read == 0) {
					break;
				}
				std::lock_guard lock(_mutex);
				for (size_t i = 0; i < read; ++i) {
					char c = chunk[i];
					if (c == '\n') {
						if (!_partial.empty() && _partial.back() == '\r') {
							_partial.pop_back();
						}
						if (_lines.size() >= MAX_QUEUED_LINES) {
							_lines.pop_front();
							++_droppedLines;
						}
						_lines.push_back(_partial);
						_partial.clear();
					} else {
						_partial.push_back(c);
					}
				}
			}
			std::lock_guard lock(_mutex);
			if (!_partial.empty()) {
				_lines.push_back(_partial);
				_partial.clear();
			}
		});
		return true;
	}

	u32 ChildProcess::DrainLines(Vector<str>& out) {
		std::lock_guard lock(_mutex);
		u32 drained = 0;
		while (!_lines.empty()) {
			out.push_back(std::move(_lines.front()));
			_lines.pop_front();
			++drained;
		}
		if (_droppedLines > 0) {
			out.push_back(std::format("... {} output line(s) dropped (queue cap)", _droppedLines));
			_droppedLines = 0;
			++drained;
		}
		return drained;
	}

	bool ChildProcess::TryFinish(i32& outExitCode) {
		if (!_running || !_process) {
			return false;
		}
		i32 exitCode = 0;
		if (!EditorPlatform::TryWaitProcess(_process, exitCode)) {
			return false;
		}
		if (_reader.joinable()) {
			_reader.join();
		}
		outExitCode = exitCode;
		CloseHandles();
		_running = false;
		return true;
	}

	void ChildProcess::Kill() {
		if (!_running || !_process) {
			return;
		}
		EditorPlatform::KillProcess(_process);
		if (_reader.joinable()) {
			_reader.join();
		}
		CloseHandles();
		_running = false;
	}

	void ChildProcess::Detach() {
		if (_reader.joinable()) {
			EditorPlatform::CancelProcessOutput(_readEnd);
			_reader.join();
		}
		CloseHandles();
		_running = false;
	}

	void ChildProcess::CloseHandles() {
		if (_readEnd) {
			EditorPlatform::CloseProcessOutput(_readEnd);
			_readEnd = nullptr;
		}
		if (_process) {
			EditorPlatform::ReleaseProcess(_process);
			_process = nullptr;
		}
	}

} // namespace Shalltear
