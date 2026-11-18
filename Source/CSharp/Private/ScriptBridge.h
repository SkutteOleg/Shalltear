#pragma once

#include "Generated/Bindings.generated.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// must match Interop/ScriptBridge.cs + Bridge.cs
#define SLT_BRIDGE_ABI_VERSION 2

#define SLT_OK 0
#define SLT_ERR_EXCEPTION 1

// UTF-8, not null terminated
typedef struct SltString {
	const char* ptr;
	int32_t length;
} SltString;

typedef struct SltEngineAPI {
	uint32_t abiVersion;
	uint64_t layoutHash;
	void (*reportException)(SltString type, SltString message, SltString stackTrace);
	void (*log)(int32_t level, SltString message);
	// valid until next engine call on this thread
	void (*getLastError)(SltString* outText);
	int32_t (*throwForTest)(void);
	SltBindings bindings;
} SltEngineAPI;

typedef struct SltManagedAPI {
	uint32_t abiVersion;
	uint64_t layoutHash;
	int32_t (*loadUserAssemblies)(const SltString* paths, int32_t count);
	int32_t (*unloadUserAssemblies)(void);
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
