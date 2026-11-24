#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <mono/metadata/assembly.h>
#include <mono/metadata/class.h>
#include <mono/metadata/image.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/metadata.h>
#include <mono/metadata/mono-private-unstable.h>
#include <mono/metadata/object.h>
#include <mono/utils/mono-error.h>
#include <mono/utils/mono-logger.h>

#include "pinvoke.h"
#include "runtime.h"
// avoid colliding with WebMonoBootstrap's C entry point of the same name
#define ShalltearScriptsInitialize ShalltearWebMonoGeneratedInitialize
#include "pinvoke-table.h"
#undef ShalltearScriptsInitialize
#include "wasm_m2n_invoke.g.h"
#include "wasm_m2n_extra.g.h"

extern MonoMethod* mono_marshal_get_managed_wrapper(MonoMethod* method, MonoClass* delegate_klass, MonoGCHandle target_handle, MonoError* error);
extern void* mono_compile_method(MonoMethod* method);

void mono_wasm_pinvoke_vararg_stub(void) {
}

void mono_wasm_marshal_get_managed_wrapper(const char* assemblyName, const char* namespaceName, const char* typeName,
										   const char* methodName, uint32_t token, int paramCount) {
	MonoError error;
	mono_error_init(&error);
	MonoImageOpenStatus status = MONO_IMAGE_OK;
	MonoAssemblyName* aname = mono_assembly_name_new(assemblyName);
	MonoAssembly* assembly = mono_assembly_load(aname, NULL, &status);
	mono_assembly_name_free(aname);
	if (!assembly) {
		return;
	}
	MonoImage* image = mono_assembly_get_image(assembly);
	MonoClass* klass = image ? mono_class_from_name(image, namespaceName, typeName) : NULL;
	if (!klass) {
		return;
	}
	MonoMethod* method = mono_get_method(image, token, klass);
	if (method) {
		MonoMethodSignature* sig = mono_method_signature(method);
		if (!sig || (int)mono_signature_get_param_count(sig) != paramCount || strcmp(mono_method_get_name(method), methodName) != 0) {
			method = NULL;
		}
	}
	if (!method) {
		method = mono_class_get_method_from_name(klass, methodName, paramCount);
	}
	if (!method) {
		return;
	}
	MonoMethod* wrapper = mono_marshal_get_managed_wrapper(method, NULL, 0, &error);
	if (wrapper) {
		mono_compile_method(wrapper);
	}
}

void* ShalltearWebMonoLookupM2N(char* cookie) {
	void* fn = mono_wasm_interp_to_native_callback(cookie);
	if (fn) {
		return fn;
	}
	return shalltear_wasm_extra_m2n_callback(cookie);
}

void* ShalltearWebMonoDlLoad(const char* name, int flags, char** err, void* userData) {
	(void)flags;
	(void)err;
	(void)userData;
	if (!name) {
		return NULL;
	}
	const int count = (int)(sizeof(pinvoke_tables) / sizeof(PinvokeTable));
	for (int i = 0; i < count; ++i) {
		if (strcmp(pinvoke_tables[i].name, name) == 0) {
			return &pinvoke_tables[i];
		}
	}
	return NULL;
}

void* ShalltearWebMonoDlSym(void* handle, const char* name, char** err, void* userData) {
	(void)err;
	(void)userData;
	if (!handle || !name) {
		return NULL;
	}
	PinvokeTable* table = (PinvokeTable*)handle;
	for (int i = 0; i < table->count; ++i) {
		if (strcmp(table->imports[i].name, name) == 0) {
			return table->imports[i].func;
		}
	}
	return NULL;
}

static int CompareExportKeyAndToken(const void* lhs, const void* rhs) {
	const UnmanagedExport* a = (const UnmanagedExport*)lhs;
	const UnmanagedExport* b = (const UnmanagedExport*)rhs;
	const int cmp = strcmp(a->key, b->key);
	return cmp != 0 ? cmp : (int)(a->token - b->token);
}

static int CompareExportKey(const void* lhs, const void* rhs) {
	const UnmanagedExport* a = (const UnmanagedExport*)lhs;
	const UnmanagedExport* b = (const UnmanagedExport*)rhs;
	return strcmp(a->key, b->key);
}

void* ShalltearWebMonoGetN2M(uint32_t token, const char* key, void* extraArg) {
	UnmanagedExport needle = {key, token, NULL};
	const int count = (int)(sizeof(wasm_native_to_interp_table) / sizeof(UnmanagedExport));
	UnmanagedExport* found = (UnmanagedExport*)bsearch(&needle, wasm_native_to_interp_table, (size_t)count,
													   sizeof(UnmanagedExport), CompareExportKeyAndToken);
	if (!found) {
		found = (UnmanagedExport*)bsearch(&needle, wasm_native_to_interp_table, (size_t)count,
										  sizeof(UnmanagedExport), CompareExportKey);
	}
	if (!found) {
		return NULL;
	}
	wasm_native_to_interp_ftndescs[found - wasm_native_to_interp_table] = *(InterpFtnDesc*)extraArg;
	return found->func;
}
