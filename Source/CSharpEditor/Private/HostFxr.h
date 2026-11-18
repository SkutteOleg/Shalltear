#pragma once

#include "Shalltear/Core/Types.h"

namespace Shalltear::CSharpEditor::HostFxr {

	i32 InitializeForRuntimeConfig(void* function, const str& runtimeConfig, const str& dotnetRoot, void** outContext);

	i32 LoadAssemblyAndGetFunctionPointer(void* function, const str& assembly, const char* type, const char* method, void** outFunction);

	std::string_view GetLibraryName();

	std::string_view GetMuxerName();

	char GetSearchPathSeparator();

} // namespace Shalltear::CSharpEditor::HostFxr
