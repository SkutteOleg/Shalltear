#include "Shalltear/Editor/CompileService.h"

#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/StringUtil.h"
#include "Shalltear/Core/Time.h"

#include "Shalltear/Editor/EditorConsole.h"
#include "Shalltear/Editor/EditorEvents.h"
#include "Shalltear/Editor/CSharpTranspiler.h"
#include "Shalltear/Editor/ScriptProjectOps.h"

#include "ChildProcess.h"
#include "CompilerOutputSink.h"

#include <charconv>
#include <filesystem>
#include <format>
#include <system_error>

namespace Shalltear {

	namespace {

		namespace fs = std::filesystem;

		constexpr const char* CONSOLE_CATEGORY = "Compile";

		bool ParseUnsigned(std::string_view text, u32& out) {
			if (text.empty()) {
				return false;
			}
			u32 value = 0;
			auto result = std::from_chars(text.data(), text.data() + text.size(), value);
			if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
				return false;
			}
			out = value;
			return true;
		}

		bool SplitLocation(std::string_view location, str& outFile, u32& outLine, u32& outColumn) {
			outLine = 0;
			outColumn = 0;

			size_t lastColon = location.find_last_of(':');
			if (lastColon == std::string_view::npos || lastColon <= 1) {
				return false;
			}
			u32 first = 0;
			if (!ParseUnsigned(location.substr(lastColon + 1), first)) {
				return false;
			}
			std::string_view head = location.substr(0, lastColon);
			size_t secondColon = head.find_last_of(':');
			u32 second = 0;
			if (secondColon != std::string_view::npos && secondColon > 1 &&
				ParseUnsigned(head.substr(secondColon + 1), second)) {
				outFile = str(head.substr(0, secondColon));
				outLine = second;
				outColumn = first;
			} else {
				outFile = str(head);
				outLine = first;
			}
			return !outFile.empty();
		}

		bool ParseSeverityWord(std::string_view word, CompilerDiagnosticSeverity& out) {
			if (word == "error" || word == "fatal error") {
				out = CompilerDiagnosticSeverity::Error;
				return true;
			}
			if (word == "warning") {
				out = CompilerDiagnosticSeverity::Warning;
				return true;
			}
			if (word == "note" || word == "remark") {
				out = CompilerDiagnosticSeverity::Note;
				return true;
			}
			return false;
		}

		str Quote(const str& value) {
			return QuoteCommandArgument(value);
		}

		std::string_view TrimView(std::string_view text) {
			size_t first = text.find_first_not_of(" \t");
			if (first == std::string_view::npos) {
				return {};
			}
			size_t last = text.find_last_not_of(" \t\r");
			return text.substr(first, last - first + 1);
		}

		struct CMakeHeader {
			std::string_view phrase;
			std::string_view location;
			std::string_view inlineText;
			CompilerDiagnosticSeverity severity = CompilerDiagnosticSeverity::Error;
		};

		bool ParseCMakeHeader(std::string_view line, CMakeHeader& out) {
			constexpr std::pair<std::string_view, CompilerDiagnosticSeverity> KINDS[] = {
				{"CMake Deprecation Error", CompilerDiagnosticSeverity::Error},
				{"CMake Deprecation Warning", CompilerDiagnosticSeverity::Warning},
				{"CMake Error", CompilerDiagnosticSeverity::Error},
				{"CMake Warning", CompilerDiagnosticSeverity::Warning},
			};
			for (const auto& [kind, severity] : KINDS) {
				if (!line.starts_with(kind)) {
					continue;
				}
				out = CMakeHeader{};
				out.severity = severity;
				std::string_view rest = line.substr(kind.size());
				size_t phraseLength = kind.size();
				constexpr std::string_view DEV = " (dev)";
				if (rest.starts_with(DEV)) {
					rest.remove_prefix(DEV.size());
					phraseLength += DEV.size();
				}
				out.phrase = line.substr(0, phraseLength);
				if (rest.starts_with(" at ") || rest.starts_with(" in ")) {
					std::string_view where = TrimView(rest.substr(4));
					if (where.ends_with(':')) {
						where.remove_suffix(1);
					}
					if (where.ends_with(')')) {
						size_t command = where.rfind(" (");
						if (command != std::string_view::npos) {
							where = where.substr(0, command);
						}
					}
					out.location = where;
				} else if (rest.starts_with(':')) {
					out.inlineText = TrimView(rest.substr(1));
				}
				return true;
			}
			return false;
		}

		str ComposeCMakeMessage(std::string_view raw) {
			size_t end = raw.find('\n');
			CMakeHeader header;
			ParseCMakeHeader(raw.substr(0, end), header);
			str message(header.phrase);
			bool first = true;
			auto begin = [&message, &first](std::string_view text) {
				message += first ? ": " : "\n";
				message += text;
				first = false;
			};
			if (!header.inlineText.empty()) {
				begin(header.inlineText);
			}

			enum class Run : u8 {
				None,
				Paragraph,
				List,
			};
			Run run = Run::None;
			while (end != std::string_view::npos) {
				raw.remove_prefix(end + 1);
				end = raw.find('\n');
				std::string_view line = raw.substr(0, end);
				if (!line.empty() && line.back() == '\r') {
					line.remove_suffix(1);
				}
				const size_t indent = line.find_first_not_of(' ');
				if (indent == std::string_view::npos) {
					run = Run::None;
					continue;
				}
				constexpr size_t BODY_INDENT = 2;
				if (indent == BODY_INDENT) {
					std::string_view text = line.substr(indent);
					if (run == Run::Paragraph) {
						message += ' ';
						message += text;
					} else {
						begin(text);
						run = run == Run::List ? Run::List : Run::Paragraph;
					}
				} else if (indent > BODY_INDENT) {
					begin(line.substr(BODY_INDENT));
					run = run == Run::List ? Run::List : Run::None;
				} else {
					begin(TrimView(line));
					run = line.ends_with(':') ? Run::List : Run::None;
				}
			}
			return message;
		}

	} // anonymous namespace

	namespace CompilerDiagnosticParser {

		str StripAnsi(std::string_view line) {
			str out;
			out.reserve(line.size());
			for (size_t i = 0; i < line.size();) {
				if (line[i] == '\x1b') {
					size_t j = i + 1;
					if (j < line.size() && line[j] == '[') {
						++j;
						while (j < line.size() && !(line[j] >= '@' && line[j] <= '~')) {
							++j;
						}
						if (j < line.size()) {
							++j;
						}
					}
					i = j;
					continue;
				}
				out.push_back(line[i]);
				++i;
			}
			return out;
		}

		bool ParseDiagnosticLine(std::string_view rawLine, CompilerDiagnostic& out) {
			str stripped = StripAnsi(rawLine);
			std::string_view line = stripped;
			out = CompilerDiagnostic{};
			out.raw = stripped;

			if (CMakeHeader header; ParseCMakeHeader(line, header)) {
				out.severity = header.severity;
				out.hasBody = true;
				if (!header.location.empty()) {
					SplitLocation(header.location, out.file, out.line, out.column);
				}
				out.message = ComposeCMakeMessage(out.raw);
				return true;
			}

			size_t firstColon = line.find(':');
			if (firstColon != std::string_view::npos && firstColon > 1) {
				std::string_view head = line.substr(0, firstColon);
				bool looksLikeTool = head.find('/') == std::string_view::npos &&
									 head.find('\\') == std::string_view::npos &&
									 head.find(' ') == std::string_view::npos;
				if (looksLikeTool) {
					str rest = StringUtil::Trim(line.substr(firstColon + 1));
					size_t severityEnd = rest.find(':');
					if (severityEnd != std::string_view::npos) {
						CompilerDiagnosticSeverity severity{};
						if (ParseSeverityWord(rest.substr(0, severityEnd), severity)) {
							out.severity = severity;
							out.message = StringUtil::Trim(rest.substr(severityEnd + 1));
							return true;
						}
					}
				}
			}

			for (std::string_view word : {std::string_view("fatal error"), std::string_view("error"),
										  std::string_view("warning"), std::string_view("note"),
										  std::string_view("remark")}) {
				str needle = ": " + str(word) + ": ";
				size_t at = line.find(needle);
				if (at == std::string_view::npos) {
					continue;
				}
				CompilerDiagnosticSeverity severity{};
				if (!ParseSeverityWord(word, severity)) {
					continue;
				}
				str file;
				u32 lineNumber = 0;
				u32 column = 0;
				if (!SplitLocation(line.substr(0, at), file, lineNumber, column)) {
					continue;
				}
				out.severity = severity;
				out.file = std::move(file);
				out.line = lineNumber;
				out.column = column;
				out.message = StringUtil::Trim(line.substr(at + needle.size()));
				return true;
			}

			return false;
		}

		bool AppendContinuation(CompilerDiagnostic& open, std::string_view rawLine) {
			if (!open.hasBody) {
				return false;
			}
			str stripped = StripAnsi(rawLine);
			std::string_view line = stripped;
			if (!line.empty() && line.back() == '\r') {
				line.remove_suffix(1);
			}
			const bool continues = line.empty() || line.front() == ' ' || line.front() == '\t' ||
								   line.starts_with("Call Stack (most recent call first):") ||
								   line.starts_with("This warning is for project developers") ||
								   line.starts_with("This error is for project developers");
			if (!continues) {
				return false;
			}
			open.raw += '\n';
			open.raw += line;
			open.message = ComposeCMakeMessage(open.raw);
			return true;
		}

	} // namespace CompilerDiagnosticParser

	struct CompileService::Impl {
		ChildProcess process;

		CompilerOutputSink sink;

		Impl(Vector<CompilerDiagnostic>& diagnostics, u32& errorCount, u32& warningCount)
			: sink(CONSOLE_CATEGORY, diagnostics, errorCount, warningCount) {}
	};

	namespace {
		CompileService* _activeCompileService = nullptr;
	} // namespace

	CompileService* CompileService::GetActive() {
		return _activeCompileService;
	}

	CompileService::CompileService() : _impl(new Impl(_diagnostics, _errorCount, _warningCount)) {
		if (!_activeCompileService) {
			_activeCompileService = this;
		}
	}

	CompileService::~CompileService() {
		// never kill a running compile (it owns the CMake tree)
		if (_activeCompileService == this) {
			_activeCompileService = nullptr;
		}
		_impl->process.Detach();
		delete _impl;
	}

	void CompileService::RefreshToolchain() {
		CppToolchain::DiscoveryOptions options;
		_toolchain = CppToolchain::Discover(options);
	}

	bool CompileService::RequestCompile(const CompileRequest& request) {
		if (request.projectPath.empty()) {
			return false;
		}
		_enabledPackages.assign(request.enabledPackages.begin(), request.enabledPackages.end());
		if (IsBusy()) {
			_rerunQueued = true;
			return true;
		}

		_projectPath = request.projectPath;
		_configName = str(ScriptProjectOps::GetActiveConfigName());
		_recompileRequested = request.recompile;
		_errorCount = 0;
		_warningCount = 0;
		_diagnostics.clear();
		_startTicks = Time::GetTicks();

		std::error_code genEc;
		if (CSharpTranspiler::HasCSharpSources(_projectPath) ||
			fs::is_directory(fs::path(CSharpTranspiler::GetGeneratedDirectory(_projectPath)), genEc)) {
			CSharpTranspiler::TranspileProjectResult transpileResult;
			bool transpileOk = CSharpTranspiler::TranspileProject(_projectPath, transpileResult);
			for (const CompilerDiagnostic& diag : transpileResult.diagnostics) {
				_impl->sink.Publish(diag);
			}
			if (!transpileOk) {
				if (_errorCount == 0) {
					_errorCount = 1;
				}
				_lastDurationSeconds = static_cast<f64>(Time::GetTicks() - _startTicks) /
									   static_cast<f64>(Time::GetTicksPerSecond());
				SLT_LOG_ERROR("Editor", "C# transpilation FAILED ({} error(s), {} warning(s)) -> loaded module unchanged",
							  _errorCount, _warningCount);
				PublishState(CompileState::Failed);
				return false;
			}
		}

		str custom = ScriptProjectOps::ReadScriptSettings(request.projectPath).compileCommand;
		if (!custom.empty()) {
			if (!_impl->process.Start(custom, _projectPath)) {
				_errorCount = 1;
				PublishState(CompileState::Failed);
				return false;
			}
			PublishState(CompileState::Compiling);
			return true;
		}

		if (_toolchain.sdkRoot.empty() && _toolchain.cmake.status == CppToolchain::ToolStatus::Missing) {
			RefreshToolchain();
		}
		if (!_toolchain.IsComplete()) {
			SLT_LOG_ERROR("Editor", "can't compile. The toolchain is incomplete:\n{}", CppToolchain::Describe(_toolchain));
			_errorCount = 1;
			_warningCount = 0;
			_diagnostics.clear();
			PublishState(CompileState::Failed);
			return false;
		}

		str staleReason;
		if (_recompileRequested) {
			std::error_code ec;
			fs::remove_all(fs::path(ScriptProjectOps::GetCompileDirectory(_projectPath, _configName)), ec);
		} else if (ScriptProjectOps::IsCompileCacheStale(_projectPath, _configName, _toolchain.compiler.path,
														 &staleReason)) {
			str compileDirectory = ScriptProjectOps::GetCompileDirectory(_projectPath, _configName);
			SLT_LOG_INFO("Editor", "'{}' was configured for another toolchain or folder ({}). Configuring it fresh",
						 compileDirectory, staleReason);
			std::error_code ec;
			fs::remove_all(fs::path(compileDirectory), ec);
		}

		BeginConfigure();
		return true;
	}

	void CompileService::Cancel() {
		if (!IsBusy()) {
			return;
		}
		_impl->process.Kill();
		ConsumeOutput();
		SLT_LOG_INFO("Editor", "compile canceled");
		_rerunQueued = false;
		PublishState(CompileState::Failed);
	}

	void CompileService::BeginConfigure() {
		str compileDirectory = ScriptProjectOps::GetCompileDirectory(_projectPath, _configName);
		str sdkRoot = _toolchain.sdkRoot;
		str commandLine = std::format(
			"{} -S {} -B {} -G Ninja -DCMAKE_BUILD_TYPE={} -DCMAKE_CXX_COMPILER={} -DCMAKE_MAKE_PROGRAM={}",
			Quote(_toolchain.cmake.path), Quote(_projectPath), Quote(compileDirectory),
			_configName, Quote(_toolchain.compiler.path), Quote(_toolchain.ninja.path));
		if (!sdkRoot.empty()) {
			commandLine += " -DShalltear_ROOT=" + Quote(sdkRoot);
		}
		commandLine += " -DSHALLTEAR_EXPECT_ENGINE_IDENTITY=" + Quote(CppToolchain::GetEngineIdentity());

		str packageList;
		for (const str& packageId : _enabledPackages) {
			if (!packageList.empty()) {
				packageList += ";";
			}
			packageList += packageId;
		}
		commandLine += " -DSHALLTEAR_ENABLED_PACKAGES=" + Quote(packageList);

		if (!_impl->process.Start(commandLine, _projectPath)) {
			_errorCount = 1;
			PublishState(CompileState::Failed);
			return;
		}
		PublishState(CompileState::Configuring);
	}

	void CompileService::BeginCompile() {
		str compileDirectory = ScriptProjectOps::GetCompileDirectory(_projectPath, _configName);
		str commandLine = std::format("{} --build {}", Quote(_toolchain.cmake.path), Quote(compileDirectory));
		if (!_impl->process.Start(commandLine, _projectPath)) {
			_errorCount = 1;
			PublishState(CompileState::Failed);
			return;
		}
		PublishState(CompileState::Compiling);
	}

	void CompileService::PumpMainThread() {
		if (!IsBusy()) {
			if (_rerunQueued) {
				_rerunQueued = false;
				CompileRequest request;
				request.projectPath = _projectPath;
				request.enabledPackages.assign(_enabledPackages.begin(), _enabledPackages.end());
				RequestCompile(request);
			}
			return;
		}

		ConsumeOutput();

		i32 exitCode = 0;
		if (_impl->process.TryFinish(exitCode)) {
			ConsumeOutput();
			FinishStep(exitCode);
		}
	}

	void CompileService::ConsumeOutput() {
		_impl->sink.Consume(_impl->process);
	}

	void CompileService::FinishStep(i32 exitCode) {
		bool wasConfiguring = _state == CompileState::Configuring;

		if (exitCode == 0 && wasConfiguring) {
			BeginCompile();
			return;
		}

		_lastDurationSeconds = static_cast<f64>(Time::GetTicks() - _startTicks) /
							   static_cast<f64>(Time::GetTicksPerSecond());

		if (exitCode == 0) {
			SLT_LOG_INFO("Editor", "compile succeeded in {:.1f}s ({} warning(s)) -> watcher reloads",
						 _lastDurationSeconds, _warningCount);
			PublishState(CompileState::Succeeded);
			return;
		}

		_impl->sink.ReportUnparsedFailure(wasConfiguring ? "configure" : "compile", exitCode);
		SLT_LOG_ERROR("Editor", "{} FAILED after {:.1f}s ({} error(s), {} warning(s)) -> loaded module unchanged",
					  wasConfiguring ? "configure" : "compile", _lastDurationSeconds, _errorCount, _warningCount);
		PublishState(CompileState::Failed);
	}

	void CompileService::PublishState(CompileState state) {
		_state = state;
		EditorEvents::CompileStateChanged.Publish(EventBus::GetGlobal(),
												  {static_cast<u8>(state), _errorCount, _warningCount});
	}

	CompileControlText DescribeCompileState(CompileState state, u32 errorCount, u32 warningCount) {
		CompileControlText text;
		switch (state) {
		case CompileState::Configuring:
			text.status = "Configuring...";
			break;
		case CompileState::Compiling:
			text.status = "Compiling...";
			break;
		case CompileState::Succeeded:
			text.status = warningCount == 0 ? str("Up to date")
											: std::format("Up to date ({} warning(s))", warningCount);
			break;
		case CompileState::Failed:
			text.status = warningCount == 0 ? std::format("{} error(s)", errorCount)
											: std::format("{} error(s), {} warning(s)", errorCount, warningCount);
			break;
		default:
			text.status = "Compile";
			break;
		}

		switch (state) {
		case CompileState::Failed:
		case CompileState::Succeeded:
			text.tooltip = std::format("{}. Click to compile", text.status);
			break;
		default:
			text.tooltip = text.status;
			break;
		}
		return text;
	}

} // namespace Shalltear
