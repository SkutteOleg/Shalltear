#pragma once

#include "ScriptBridge.h"

#include "Shalltear/Core/Types.h"

namespace Shalltear::CSharpEditor::CoreCLRHost {

	// 1 runtime per process, never unloaded
	SltInitializeFn Start(const str& sdkRoot, const str& runtimeVersion, const str& runtimeAssembly, str& outError);

	bool IsStarted();

	SltInitializeFn GetEntry();

} // namespace Shalltear::CSharpEditor::CoreCLRHost
