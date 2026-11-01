#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/Editor/CompileService.h"

#include <optional>
#include <string_view>

namespace Shalltear {

	class ChildProcess;

	class CompilerOutputSink {
	public:
		CompilerOutputSink(const char* consoleCategory, Vector<CompilerDiagnostic>& diagnostics,
						   u32& errorCount, u32& warningCount);

		void Consume(ChildProcess& process);

		void CountError();

		void Publish(const CompilerDiagnostic& diagnostic);

		void ReportUnparsedFailure(std::string_view what, i32 exitCode);
	private:
		void PublishOpen();

		const char* _category;
		Vector<CompilerDiagnostic>& _diagnostics;
		u32& _errorCount;
		u32& _warningCount;
		Vector<str> _pending;
		std::optional<CompilerDiagnostic> _open;
	};

} // namespace Shalltear
