#pragma once

#include "ManagedTypes.h"
#include "ScriptBridge.h"

#include "Shalltear/Core/Types.h"

#include <vector>

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
		i32 (*unloadUserAssemblies)();
		ManagedTypes::Counters& (*getCounters)();
		const void* (*getOwnerToken)();
		void (*installComponentMenu)();
		std::vector<str> (*takeRecords)();
		const char* (*internLogOrigin)(std::string_view file);
		void (*setDriver)(i32 (*driver)(i32 op, i32 first, i32 second));
		i32 (*getInputMapCount)();
		const char* runtimeVersion;
	};

	inline constexpr const char* SCRIPT_TEST_HOOKS_SYMBOL = "ShalltearCSharpTestHooks";

	using GetScriptTestHooksFn = const ScriptTestHooks* (*)();

} // namespace Shalltear::CSharpImpl
