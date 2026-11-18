#include "CSharpCompile.h"

#include "SHA512.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Time.h"

#include "Shalltear/Editor/EditorConsole.h"
#include "Shalltear/Editor/EditorEvents.h"

#include <filesystem>
#include <format>
#include <span>

namespace Shalltear::CSharpEditor {

	namespace {

		namespace fs = std::filesystem;

		constexpr std::string_view BuildRoot = "Library/CSharp/";
		constexpr std::string_view AssetsFile = "Library/CSharp/obj/project.assets.json";
		constexpr std::string_view StampFile = "Library/CSharp/obj/Shalltear.Restore.stamp";
		constexpr std::string_view PathsFile = "Library/CSharp/Generated/Shalltear.Paths.props";
		constexpr std::string_view UserFile = "CSharp.props";
		constexpr std::string_view NoServers = " -nodeReuse:false -p:UseSharedCompilation=false";
		constexpr size_t UNPARSED_LINE_CAP = 30;

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

		str ReadIfPresent(const fs::path& file) {
			std::error_code ec;
			return fs::is_regular_file(file, ec) ? FileSystem::ReadTextFile(Path(file.generic_string()), MemTag::Editor) : str();
		}

		i64 ReadWriteTime(const str& file) {
			std::error_code ec;
			const fs::file_time_type time = fs::last_write_time(fs::path(file), ec);
			return ec ? 0 : static_cast<i64>(time.time_since_epoch().count());
		}

		void Add(SHA512& hash, std::string_view text) {
			hash.Update({reinterpret_cast<const u8*>(text.data()), text.size()});
			const u8 end = 0;
			hash.Update({&end, 1});
		}

	} // anonymous namespace

	CSharpCompile::~CSharpCompile() {
		Cancel();
	}

	void CSharpCompile::Configure(CompileOptions options) {
		_options = std::move(options);
	}

	str CSharpCompile::GetOutputAssembly() const {
		return (fs::path(_options.projectPath) / "Library" / "CSharp" / "bin" / fs::path(_options.projectFile).replace_extension(".dll"))
			.generic_string();
	}

	str CSharpCompile::ComputeRestoreStamp() const {
		const fs::path root(_options.projectPath);
		SHA512 hash;
		Add(hash, ReadIfPresent(root / _options.projectFile));
		Add(hash, ReadIfPresent(root / PathsFile));
		Add(hash, ReadIfPresent(root / UserFile));
		Add(hash, _options.pinStamp);
		return hash.Finish();
	}

	bool CSharpCompile::Request() {
		if (IsBusy()) {
			_rerunQueued = true;
			return true;
		}
		if (_options.projectPath.empty() || _options.projectFile.empty() || _options.muxer.empty()) {
			return false;
		}
		_previousWarnings.clear();
		if (_state == CompileState::Succeeded) {
			_previousWarnings = _diagnostics;
		}
		_outputTimeBefore = ReadWriteTime(GetOutputAssembly());
		_diagnostics.clear();
		_seen.clear();
		_errorCount = 0;
		_warningCount = 0;
		_startTicks = Time::GetTicks();
		_pendingStamp = ComputeRestoreStamp();

		const fs::path root(_options.projectPath);
		std::error_code ec;
		if (!fs::is_regular_file(root / AssetsFile, ec) || ReadIfPresent(root / StampFile) != _pendingStamp) {
			BeginRestore();
		} else {
			BeginBuild();
		}
		return IsBusy();
	}

	void CSharpCompile::Cancel() {
		_child.Kill();
		_rerunQueued = false;
		if (IsBusy()) {
			PublishState(CompileState::Idle);
		}
	}

	bool CSharpCompile::StartChild(const str& arguments) {
		const ChildVariable variables[] = {
			{"DOTNET_CLI_UI_LANGUAGE", "en-us"},
			{"DOTNET_CLI_USE_MSBUILD_SERVER", _options.buildServers ? "1" : "0"},
		};
		_unparsed.clear();
		str command = QuoteCommandArgument(_options.muxer) + " " + arguments;
		if (!_options.buildServers) {
			command += NoServers;
		}
		return _child.Start(command, _options.projectPath, variables);
	}

	void CSharpCompile::BeginRestore() {
		PublishState(CompileState::Configuring);
		++_restoreCount;
		if (!StartChild(std::format("restore {} -nologo -v q -p:ShalltearBuildRoot={}", QuoteCommandArgument(_options.projectFile), BuildRoot))) {
			Fail("restore");
		}
	}

	void CSharpCompile::BeginBuild() {
		PublishState(CompileState::Compiling);
		++_compileCount;
		const char* configuration = _options.optimization == ScriptOptimization::Release ? "Release" : "Debug";
		if (!StartChild(std::format("build {} -c {} -nologo -v q -clp:NoSummary -p:GenerateFullPaths=true -p:ShalltearBuildRoot={} "
									"-p:ShalltearAOTAnalysis={} --no-restore",
									QuoteCommandArgument(_options.projectFile), configuration, BuildRoot, _options.aotAnalysis))) {
			Fail("compile");
		}
	}

	void CSharpCompile::Pump() {
		if (!IsBusy()) {
			if (_rerunQueued) {
				_rerunQueued = false;
				Request();
			}
			return;
		}
		ConsumeOutput();
		i32 exitCode = 0;
		if (_child.TryFinish(exitCode)) {
			ConsumeOutput();
			FinishStep(exitCode);
		}
	}

	void CSharpCompile::ConsumeOutput() {
		_lines.clear();
		_child.DrainLines(_lines);
		for (const str& line : _lines) {
			CSharpDiagnostics::Diagnostic diagnostic;
			if (!CSharpDiagnostics::ParseLine(line, diagnostic)) {
				SLT_LOG_TRACE("C#", "{}", line);
				if (!line.empty()) {
					if (_unparsed.size() == UNPARSED_LINE_CAP) {
						_unparsed.erase(_unparsed.begin());
					}
					_unparsed.push_back(line);
				}
				continue;
			}
			if (!CSharpDiagnostics::AddUnique(_diagnostics, _seen, diagnostic)) {
				continue;
			}
			if (diagnostic.severity == CompilerDiagnosticSeverity::Error) {
				++_errorCount;
			} else if (diagnostic.severity == CompilerDiagnosticSeverity::Warning) {
				++_warningCount;
			}
			ConsoleOrigin origin;
			origin.file = diagnostic.file;
			origin.line = diagnostic.line;
			origin.column = diagnostic.column;
			EditorConsole::Push(ToLogLevel(diagnostic.severity), "C#", CSharpDiagnostics::Describe(diagnostic), origin);
		}
	}

	void CSharpCompile::FinishStep(i32 exitCode) {
		const bool restoring = _state == CompileState::Configuring;
		if (exitCode == 0 && restoring) {
			const fs::path stamp = fs::path(_options.projectPath) / StampFile;
			if (!FileSystem::WriteTextFile(Path(stamp.generic_string()), _pendingStamp)) {
				SLT_LOG_WARN("C#", "couldn't write '{}' (restore will repeat)", stamp.generic_string());
			}
			BeginBuild();
			return;
		}

		_lastDurationSeconds = static_cast<f64>(Time::GetTicks() - _startTicks) / static_cast<f64>(Time::GetTicksPerSecond());
		if (exitCode == 0) {
			if (_outputTimeBefore != 0 && ReadWriteTime(GetOutputAssembly()) == _outputTimeBefore) {
				_diagnostics = _previousWarnings;
				_warningCount = static_cast<u32>(_diagnostics.size());
			}
			SLT_LOG_INFO("C#", "compile succeeded in {:.2f} s ({} warning(s))", _lastDurationSeconds, _warningCount);
			PublishState(CompileState::Succeeded);
			return;
		}

		const char* what = restoring ? "restore" : "compile";
		if (_errorCount == 0) {
			str said = std::format("{} failed with exit code {}", what, exitCode);
			for (const str& line : _unparsed) {
				said += "\n" + line;
			}
			EditorConsole::Push(LogLevel::Error, "C#", said, {});
			++_errorCount;
		}
		SLT_LOG_ERROR("C#", "{} FAILED after {:.2f} s ({} error(s), {} warning(s)) -> loaded scripts unchanged", what, _lastDurationSeconds, _errorCount,
					  _warningCount);
		PublishState(CompileState::Failed);
	}

	void CSharpCompile::Fail(std::string_view what) {
		_lastDurationSeconds = 0.0;
		++_errorCount;
		EditorConsole::Push(LogLevel::Error, "C#", std::format("couldn't start '{}' for {}. Reopen the project", _options.muxer, what), {});
		PublishState(CompileState::Failed);
	}

	void CSharpCompile::PublishState(CompileState state) {
		_state = state;
		EditorEvents::CompileStateChanged.Publish(EventBus::GetGlobal(), {static_cast<u8>(state), _errorCount, _warningCount});
	}

} // namespace Shalltear::CSharpEditor
