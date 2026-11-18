#pragma once

#include "ScriptBridge.h"

#include "Shalltear/Core/Types.h"

namespace Shalltear::CSharpImpl {

	struct ScriptTestHooks {
		str (*findSDKRoot)();
		str (*getRuntimeAssemblyPath)();
		SltInitializeFn (*startHost)(const str& sdkRoot, const str& runtimeVersion, const str& runtimeAssembly, str& outError);
		SltInitializeFn (*getHostEntry)();
		SltEngineAPI (*makeEngineAPI)();
		bool (*attachAs)(SltEngineAPI engine, SltInitializeFn initialize, str& outError);
		void (*detach)();
		bool (*isAttached)();
		const SltManagedAPI& (*getManaged)();
		const char* runtimeVersion;
	};

	inline constexpr const char* SCRIPT_TEST_HOOKS_SYMBOL = "ShalltearCSharpTestHooks";

	using GetScriptTestHooksFn = const ScriptTestHooks* (*)();

} // namespace Shalltear::CSharpImpl
