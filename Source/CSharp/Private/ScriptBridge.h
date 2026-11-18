#pragma once

#include "Generated/Bindings.generated.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// must match Interop/ScriptBridge.cs + Bridge.cs
#define SLT_BRIDGE_ABI_VERSION 4

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

// must match ComponentGenerator.cs
typedef enum SltFieldKind {
	SLT_KIND_BOOL,
	SLT_KIND_I8,
	SLT_KIND_U8,
	SLT_KIND_I16,
	SLT_KIND_U16,
	SLT_KIND_I32,
	SLT_KIND_U32,
	SLT_KIND_I64,
	SLT_KIND_U64,
	SLT_KIND_F32,
	SLT_KIND_F64,
	SLT_KIND_VEC2,
	SLT_KIND_VEC3,
	SLT_KIND_VEC4,
	SLT_KIND_QUAT,
	SLT_KIND_COLOR,
	SLT_KIND_COUNT
} SltFieldKind;

#define SLT_FIELD_HIDDEN 1u
#define SLT_FIELD_READ_ONLY 2u
#define SLT_FIELD_TRANSIENT 4u
#define SLT_FIELD_RANGED 8u

// 8 byte slots per component type, must match ComponentGenerator.cs
#define SLT_FIELD_SLOT_LIMIT 960

// UTF-8, not null terminated
typedef struct SltString {
	const char* ptr;
	int32_t length;
} SltString;

typedef struct SltFieldDesc {
	SltString name;
	uint64_t nameHash;
	int32_t kind;
	uint32_t flags;
	float rangeMin;
	float rangeMax;
	SltString tooltip;
	SltString header;
	// written by engine, bytes from instance start
	int32_t offset;
} SltFieldDesc;

typedef struct SltTypeDesc {
	SltString name;
	uint64_t nameHash;
	uint64_t parentHash;
	int32_t managedTypeId;
	uint32_t flags;
	uint32_t hookMask;
	// BehaviorTick bits
	uint32_t tickMask;
	SltFieldDesc* fields;
	int32_t fieldCount;
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
	void (*pushFields)(intptr_t instance, void* shadow);
	void (*pullFields)(intptr_t instance, const void* shadow, uint64_t nativeId);
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
