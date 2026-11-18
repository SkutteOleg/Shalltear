#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/Editor/CompileService.h"

#include <string_view>

namespace Shalltear::CSharpEditor::CSharpDiagnostics {

	struct Diagnostic {
		CompilerDiagnosticSeverity severity = CompilerDiagnosticSeverity::Error;
		str file;
		u32 line = 0;
		u32 column = 0;
		str code;
		str message;

		bool operator==(const Diagnostic&) const = default;
	};

	bool ParseLine(std::string_view line, Diagnostic& out);

	bool AddUnique(Vector<Diagnostic>& all, HashSet<str>& seen, const Diagnostic& diagnostic);

	str Describe(const Diagnostic& diagnostic);

} // namespace Shalltear::CSharpEditor::CSharpDiagnostics
