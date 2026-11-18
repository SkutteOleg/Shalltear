#include "CompilerOutputSink.h"

#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"

#include "Shalltear/Editor/EditorConsole.h"

#include "Shalltear/Editor/ChildProcess.h"

#include <format>

namespace Shalltear {

	namespace {

		LogLevel ToLogLevel(CompilerDiagnosticSeverity severity) {
			switch (severity) {
			case CompilerDiagnosticSeverity::Error:
				return LogLevel::Error;
			case CompilerDiagnosticSeverity::Warning:
				return LogLevel::Warn;
			default:
				return LogLevel::Info;
			}
		}

	} // anonymous namespace

	CompilerOutputSink::CompilerOutputSink(const char* consoleCategory, Vector<CompilerDiagnostic>& diagnostics,
										   u32& errorCount, u32& warningCount)
		: _category(consoleCategory), _diagnostics(diagnostics), _errorCount(errorCount), _warningCount(warningCount),
		  _pending(GetMemoryResource(MemTag::Editor)) {}

	void CompilerOutputSink::Consume(ChildProcess& process) {
		_pending.clear();
		process.DrainLines(_pending);

		for (const str& line : _pending) {
			if (_open && CompilerDiagnosticParser::AppendContinuation(*_open, line)) {
				continue;
			}
			PublishOpen();

			CompilerDiagnostic diagnostic;
			if (!CompilerDiagnosticParser::ParseDiagnosticLine(line, diagnostic)) {
				SLT_LOG_TRACE(_category, "{}", line);
				continue;
			}
			if (diagnostic.hasBody) {
				_open = std::move(diagnostic);
				continue;
			}
			Publish(diagnostic);
		}

		if (!process.IsRunning()) {
			PublishOpen();
		}
	}

	void CompilerOutputSink::PublishOpen() {
		if (_open) {
			Publish(*_open);
			_open.reset();
		}
	}

	void CompilerOutputSink::Publish(const CompilerDiagnostic& diagnostic) {
		if (diagnostic.severity == CompilerDiagnosticSeverity::Error) {
			++_errorCount;
		} else if (diagnostic.severity == CompilerDiagnosticSeverity::Warning) {
			++_warningCount;
		}
		_diagnostics.push_back(diagnostic);

		ConsoleOrigin origin;
		origin.file = diagnostic.file;
		origin.line = diagnostic.line;
		origin.column = diagnostic.column;

		str where = diagnostic.column != 0 ? std::format("{}:{}:{}", diagnostic.file, diagnostic.line, diagnostic.column)
										   : std::format("{}:{}", diagnostic.file, diagnostic.line);
		str message = diagnostic.file.empty() ? diagnostic.message : std::format("{}: {}", where, diagnostic.message);
		EditorConsole::Push(ToLogLevel(diagnostic.severity), _category, message, origin);
	}

	void CompilerOutputSink::CountError() {
		++_errorCount;
	}

	void CompilerOutputSink::ReportUnparsedFailure(std::string_view what, i32 exitCode) {
		if (_errorCount != 0) {
			return;
		}
		ConsoleOrigin origin;
		EditorConsole::Push(LogLevel::Error, _category,
							std::format("{} failed with exit code {} without parsed diagnostics. See the output above",
										what, exitCode),
							origin);
		++_errorCount;
	}

} // namespace Shalltear
