#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/BuildInfo.h"
#include "Shalltear/Core/Jobs/JobSystem.h"

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include "Module/EngineStateTable.h"
#include "Platform/Process.h"

namespace Shalltear {

	struct LogManagerData {
		std::vector<std::shared_ptr<ILogSink>> sinks;
		std::atomic<LogLevel> globalLevel{LogLevel::Trace};

		std::vector<LogEvent> eventQueue;
		std::mutex queueMutex;
		std::condition_variable queueCondition;
		JobHandle workerJob;

		u64 enqueued = 0;
		u64 delivered = 0;
		std::condition_variable drainCondition;

		std::atomic<bool> isRunning{false};
		std::atomic<bool> isTerminated{false};
		bool asyncWorker = false;
	};

	static LogManagerData* _boundContext = nullptr;

	static LogManagerData& GetLogContext() {
		if (_boundContext)
			return *_boundContext;
		static LogManagerData context;
		return context;
	}

	static void AsyncLogWorker() {
		auto& data = GetLogContext();

		while (true) {
			std::vector<LogEvent> localQueue;
			std::vector<std::shared_ptr<ILogSink>> localSinks;

			{
				std::unique_lock<std::mutex> lock(data.queueMutex);
				data.queueCondition.wait(lock, [&data] {
					return !data.eventQueue.empty() || !data.isRunning.load(std::memory_order_relaxed);
				});

				if (!data.isRunning.load(std::memory_order_relaxed) && data.eventQueue.empty())
					break;

				localQueue.swap(data.eventQueue);
				localSinks = data.sinks;
			}

			for (const auto& event : localQueue)
				for (const auto& sink : localSinks)
					sink->ReceiveLog(event);

			if (!localQueue.empty())
				for (const auto& sink : localSinks)
					sink->Flush();

			{
				std::lock_guard<std::mutex> lock(data.queueMutex);
				data.delivered += localQueue.size();
			}
			data.drainCondition.notify_all();
		}
	}

	static str FormatTimestamp(const std::chrono::system_clock::time_point& timestamp) {
		auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()) % 1000;
		std::time_t nowC = std::chrono::system_clock::to_time_t(timestamp);
		std::tm parts;

		Platform::ToLocalTime(nowC, parts);

		std::ostringstream oss;
		oss << std::put_time(&parts, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << milliseconds.count();
		return oss.str();
	}
	static const char* GetLevelName(LogLevel level) {
		switch (level) {
		case LogLevel::Trace:
			return "TRACE";
		case LogLevel::Debug:
			return "DEBUG";
		case LogLevel::Info:
			return "INFO";
		case LogLevel::Warn:
			return "WARN";
		case LogLevel::Error:
			return "ERROR";
		case LogLevel::Fatal:
			return "FATAL";
		default:
			return "UNKNOWN";
		}
	}

	ConsoleSink::ConsoleSink() {
		Platform::PrepareConsoleOutput();
	}

	void ConsoleSink::ReceiveLog(const LogEvent& event) {
		const char* colorPrefix = "";
		const char* colorSuffix = "\033[0m";

		switch (event.level) {
		case LogLevel::Trace:
			colorPrefix = "\033[90m";
			break;
		case LogLevel::Debug:
			colorPrefix = "\033[37m";
			break;
		case LogLevel::Info:
			colorPrefix = "\033[32m";
			break;
		case LogLevel::Warn:
			colorPrefix = "\033[33m";
			break;
		case LogLevel::Error:
			colorPrefix = "\033[31m";
			break;
		case LogLevel::Fatal:
			colorPrefix = "\033[41;37m";
			break;
		default:
			colorSuffix = "";
			break;
		}

		str rawPath(event.file);
		size_t slashPos = rawPath.find_last_of("/\\");
		str filename = (slashPos != str::npos) ? rawPath.substr(slashPos + 1) : rawPath;

		std::ostringstream oss;
		oss << colorPrefix
			<< "[" << FormatTimestamp(event.timestamp) << "] "
			<< "[" << event.category << "] "
			<< "[" << GetLevelName(event.level) << "] "
			<< "[" << filename << ":" << event.line << "]: "
			<< event.message
			<< colorSuffix << "\n";

		std::cout << oss.str();
	}
	void ConsoleSink::Flush() {
		std::cout.flush();
	}

	FileSink::FileSink(const str& filePathStr) {
		std::filesystem::path filePath(filePathStr);
		std::error_code ec;
		if (filePath.has_parent_path()) {
			std::filesystem::create_directories(filePath.parent_path(), ec);
		}

		_fileStream.open(filePath, std::ios::out | std::ios::app);
		if (!_fileStream.is_open()) {
			std::cerr << "Failed to open log file: " << filePath << "\n";
			return;
		}

		std::time_t launch = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		std::tm parts;
		Platform::ToLocalTime(launch, parts);
		_fileStream << "\n================================================================\n"
					<< "SESSION " << std::put_time(&parts, "%Y-%m-%d %H:%M:%S")
					<< " | Shalltear " << BuildInfo::GetVersionString()
					<< " (" << BuildInfo::GetGitCommit() << ", " << BuildInfo::GetBuildConfig() << ")\n"
					<< "================================================================\n";
		_fileStream.flush();
	}
	FileSink::~FileSink() {
		if (_fileStream.is_open())
			_fileStream.close();
	}

	void FileSink::ReceiveLog(const LogEvent& event) {
		if (!_fileStream.is_open())
			return;

		str rawPath(event.file);
		size_t slashPos = rawPath.find_last_of("/\\");
		str filename = (slashPos != str::npos) ? rawPath.substr(slashPos + 1) : rawPath;

		std::ostringstream output;
		output << "[" << FormatTimestamp(event.timestamp) << "] "
			   << "[" << event.category << "] "
			   << "[" << GetLevelName(event.level) << "] "
			   << "[" << filename << ":" << event.line << " (" << event.function << ")]: "
			   << event.message << "\n";

		_fileStream << output.str();
	}
	void FileSink::Flush() {
		if (_fileStream.is_open())
			_fileStream.flush();
	}

	void LogManager::Initialize() {
		Initialize(LogInitConfig{});
	}

	void LogManager::Initialize(const LogInitConfig& config) {
		auto& data = GetLogContext();

		bool expected = false;
		if (!data.isRunning.compare_exchange_strong(expected, true))
			return;

		data.isTerminated.store(false, std::memory_order_relaxed);

		if (config.enableConsoleSink && Platform::AcquireStandardOutput(config.createConsole))
			AddSink(std::make_shared<ConsoleSink>());
		if (config.enableFileSink)
			AddSink(std::make_shared<FileSink>(config.fileSinkPath));

		data.asyncWorker = JobSystem::GetWorkerCount() > 0;
		if (data.asyncWorker) {
			data.workerJob = JobSystem::Submit([]() { AsyncLogWorker(); }, JobType::LongRunning);
		}

		if (!data.sinks.empty())
			SLT_LOG_INFO("Core", "Log subsystem initialized");
	}

	void LogManager::DrainBeforeImageUnload() {
		auto& data = GetLogContext();
		if (!data.asyncWorker) {
			return;
		}
		std::unique_lock<std::mutex> lock(data.queueMutex);
		if (data.enqueued == data.delivered) {
			return;
		}

		if (!data.isRunning.load(std::memory_order_relaxed)) {
			const size_t dropped = data.eventQueue.size();
			data.eventQueue.clear();
			data.delivered = data.enqueued;
			lock.unlock();
			static std::atomic<bool> reported{false};
			bool expected = false;
			if (dropped > 0 && reported.compare_exchange_strong(expected, true)) {
				std::fprintf(stderr, "[Module] dropped %zu log event(s) queued before the log started (a module unloaded under them)\n", dropped);
			}
			return;
		}

		const u64 target = data.enqueued;
		data.queueCondition.notify_one();
		data.drainCondition.wait_for(lock, std::chrono::seconds(2), [&data, target] { return data.delivered >= target; });
	}

	void LogManager::Terminate() {
		auto& data = GetLogContext();

		if (!data.isRunning.load(std::memory_order_relaxed))
			return;

		SLT_LOG_INFO("Core", "Shutting down log subsystem...");

		data.isTerminated.store(true, std::memory_order_relaxed);

		// under the queue mutex or the worker misses the wakeup. stupid bug
		{
			std::lock_guard<std::mutex> lock(data.queueMutex);
			bool expected = true;
			if (!data.isRunning.compare_exchange_strong(expected, false))
				return;
		}

		data.queueCondition.notify_one();

		if (data.asyncWorker) {
			JobSystem::WaitFor(data.workerJob);
			data.asyncWorker = false;
		}

		{
			std::lock_guard<std::mutex> lock(data.queueMutex);
			data.sinks.clear();
		}
	}

	void LogManager::AddSink(std::shared_ptr<ILogSink> sink) {
		auto& data = GetLogContext();
		std::lock_guard<std::mutex> lock(data.queueMutex);
		data.sinks.push_back(sink);
	}

	bool LogManager::RemoveSink(const std::shared_ptr<ILogSink>& sink) {
		auto& data = GetLogContext();
		std::lock_guard<std::mutex> lock(data.queueMutex);
		auto it = std::find(data.sinks.begin(), data.sinks.end(), sink);
		if (it == data.sinks.end())
			return false;
		data.sinks.erase(it);
		return true;
	}

	void LogManager::RemoveAllSinks() {
		auto& data = GetLogContext();
		std::lock_guard<std::mutex> lock(data.queueMutex);
		data.sinks.clear();
	}

	void LogManager::SetGlobalLevel(LogLevel level) {
		GetLogContext().globalLevel.store(level, std::memory_order_relaxed);
	}

	LogLevel LogManager::GetGlobalLevel() {
		return GetLogContext().globalLevel.load(std::memory_order_relaxed);
	}

	void LogManager::VDispatch(LogLevel level,
							   const char* category,
							   const char* file,
							   u32 line,
							   const char* function,
							   std::string_view format,
							   std::format_args args) {
		auto& data = GetLogContext();

		if (data.isTerminated.load(std::memory_order_relaxed))
			return;

		LogEvent event;
		event.level = level;
		event.category = category;
		event.message = std::vformat(format, args);
		event.timestamp = std::chrono::system_clock::now();
		event.threadId = std::this_thread::get_id();
		event.file = file;
		event.line = line;
		event.function = function;

		if (!data.asyncWorker) {
			std::vector<std::shared_ptr<ILogSink>> localSinks;
			{
				std::lock_guard<std::mutex> lock(data.queueMutex);
				localSinks = data.sinks;
			}
			for (const auto& sink : localSinks) {
				sink->ReceiveLog(event);
				sink->Flush();
			}
			return;
		}

		{
			std::lock_guard<std::mutex> lock(data.queueMutex);

			data.eventQueue.push_back(std::move(event));
			++data.enqueued;
		}

		data.queueCondition.notify_one();
	}

	namespace ModuleInternal {

		void CaptureLogEngineState(EngineStateTable& table) {
			table.logManager = &GetLogContext();
		}

		void BindLogEngineState(const EngineStateTable& table) {
			_boundContext = static_cast<LogManagerData*>(table.logManager);
		}

	} // namespace ModuleInternal

} //namespace Shalltear
