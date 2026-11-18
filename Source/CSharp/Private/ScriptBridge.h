#pragma once

#include "Generated/Bindings.generated.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// must match Interop/ScriptBridge.cs + Bridge.cs
#define SLT_BRIDGE_ABI_VERSION 3

#define SLT_OK 0
#define SLT_ERR_EXCEPTION 1
#define SLT_ERR_REFUSED 2

// bit N of hookMask, must match Behavior.cs
#define SLT_HOOK_ATTACH 0
#define SLT_HOOK_DETACH 1
#define SLT_HOOK_ENABLE 2
#define SLT_HOOK_DISABLE 3
#define SLT_HOOK_DESTROY 4
#define SLT_HOOK_UPDATE 5
#define SLT_HOOK_FIXED_UPDATE 6
#define SLT_HOOK_LATE_UPDATE 7

#define SLT_TYPE_ABSTRACT 1u

// UTF-8, not null terminated
typedef struct SltString {
	const char* ptr;
	int32_t length;
} SltString;

typedef struct SltTypeDesc {
	SltString name;
	uint64_t nameHash;
	uint64_t parentHash;
	int32_t managedTypeId;
	uint32_t flags;
	uint32_t hookMask;
	// BehaviorTick bits
	uint32_t tickMask;
} SltTypeDesc;

typedef struct SltEngineAPI {
	uint32_t abiVersion;
	uint64_t layoutHash;
	void (*reportException)(SltString type, SltString message, SltString stackTrace);
	void (*log)(int32_t level, SltString message);
	// valid until next engine call on this thread
	void (*getLastError)(SltString* outText);
	int32_t (*registerManagedType)(const SltTypeDesc* type);
	int32_t (*throwForTest)(void);
	SltBindings bindings;
} SltEngineAPI;

typedef struct SltManagedAPI {
	uint32_t abiVersion;
	uint64_t layoutHash;
	int32_t (*loadUserAssemblies)(const SltString* paths, int32_t count);
	int32_t (*unloadUserAssemblies)(void);
	intptr_t (*createInstance)(int32_t managedTypeId, uint64_t nativeId);
	void (*destroyInstance)(intptr_t instance);
	void (*invokeHook)(intptr_t instance, int32_t hook);
	int32_t (*getConformanceTestCount)(void);
	// strings valid until next managed call
	int32_t (*getConformanceTestName)(int32_t index, SltString* outName);
	int32_t (*runConformanceTest)(int32_t index, SltString* outMessage);
	void (*shutdown)(void);
} SltManagedAPI;

typedef int32_t (*SltInitializeFn)(const SltEngineAPI* engine, SltManagedAPI* outManaged);

#ifdef __cplusplus
}
#endif
