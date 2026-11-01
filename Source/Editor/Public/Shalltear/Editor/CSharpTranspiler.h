#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/Editor/CompileService.h"
#include "Shalltear/Editor/EditorAPI.h"

#include <string_view>

namespace Shalltear::CSharpTranspiler {

	struct TranspileFileResult {
		str headerContent;
		str sourceContent;
		Vector<str> reflectedTypes{GetMemoryResource(MemTag::Editor)};
		Vector<CompilerDiagnostic> diagnostics{GetMemoryResource(MemTag::Editor)};
		bool succeeded = false;
	};

	struct TranspileProjectResult {
		Vector<str> written{GetMemoryResource(MemTag::Editor)};
		Vector<str> unchanged{GetMemoryResource(MemTag::Editor)};
		Vector<str> removed{GetMemoryResource(MemTag::Editor)};
		Vector<CompilerDiagnostic> diagnostics{GetMemoryResource(MemTag::Editor)};
		u32 sourceFileCount = 0;
		bool structureChanged = false;
		bool succeeded = false;
	};

	SHALLTEAR_EDITOR_API str GetGeneratedDirectory(const str& projectPath);

	SHALLTEAR_EDITOR_API bool IsRoslynLoaded();

	SHALLTEAR_EDITOR_API bool EnsureCSharpProjectFiles(const str& projectPath);

	SHALLTEAR_EDITOR_API bool HasCSharpSources(const str& projectPath);

	SHALLTEAR_EDITOR_API bool IsGeneratedOutputOutOfDate(const str& projectPath, str* outReason = nullptr);

	SHALLTEAR_EDITOR_API bool TranspileSource(std::string_view sourcePath,
											  std::string_view cSharpText,
											  TranspileFileResult& out);

	SHALLTEAR_EDITOR_API bool TranspileProject(const str& projectPath,
											   TranspileProjectResult& out);

} // namespace Shalltear::CSharpTranspiler
