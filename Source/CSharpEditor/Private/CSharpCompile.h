#pragma once

#include "CSharpDiagnostics.h"
#include "CSharpSettings.h"

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/Editor/ChildProcess.h"
#include "Shalltear/Editor/CompileService.h"

namespace Shalltear::CSharpEditor {

	struct CompileOptions {
		str projectPath;
		str projectFile;
		str muxer;
		str pinStamp;
		ScriptOptimization optimization = ScriptOptimization::Debug;
		bool aotAnalysis = false;
		bool buildServers = true;
	};

	class CSharpCompile {
	public:
		CSharpCompile() = default;
		~CSharpCompile();

		CSharpCompile(const CSharpCompile&) = delete;
		CSharpCompile& operator=(const CSharpCompile&) = delete;

		void Configure(CompileOptions options);

		bool Request();

		void Cancel();

		void Pump();

		CompileState GetState() const { return _state; }
		bool IsBusy() const { return _state == CompileState::Configuring || _state == CompileState::Compiling; }
		u32 GetErrorCount() const { return _errorCount; }
		u32 GetWarningCount() const { return _warningCount; }
		u32 GetRestoreCount() const { return _restoreCount; }
		u32 GetCompileCount() const { return _compileCount; }
		f64 GetLastDurationSeconds() const { return _lastDurationSeconds; }
		const Vector<CSharpDiagnostics::Diagnostic>& GetDiagnostics() const { return _diagnostics; }

		str GetOutputAssembly() const;
	private:
		str ComputeRestoreStamp() const;
		void BeginRestore();
		void BeginBuild();
		bool StartChild(const str& arguments);
		void ConsumeOutput();
		void FinishStep(i32 exitCode);
		void Fail(std::string_view what);
		void PublishState(CompileState state);

		CompileOptions _options;
		ChildProcess _child;
		CompileState _state = CompileState::Idle;
		bool _rerunQueued = false;
		u32 _errorCount = 0;
		u32 _warningCount = 0;
		u32 _restoreCount = 0;
		u32 _compileCount = 0;
		u64 _startTicks = 0;
		f64 _lastDurationSeconds = 0.0;
		str _pendingStamp;
		i64 _outputTimeBefore = 0;
		Vector<str> _lines{GetMemoryResource(MemTag::Editor)};
		Vector<str> _unparsed{GetMemoryResource(MemTag::Editor)};
		Vector<CSharpDiagnostics::Diagnostic> _diagnostics{GetMemoryResource(MemTag::Editor)};
		HashSet<str> _seen{GetMemoryResource(MemTag::Editor)};
		Vector<CSharpDiagnostics::Diagnostic> _previousWarnings{GetMemoryResource(MemTag::Editor)};
	};

} // namespace Shalltear::CSharpEditor
