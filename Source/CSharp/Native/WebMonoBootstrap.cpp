#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <mono/jit/jit.h>
#include <mono/jit/mono-private-unstable.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/class.h>
#include <mono/metadata/image.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/mono-private-unstable.h>
#include <mono/metadata/object.h>
#include <mono/metadata/threads.h>
#include <mono/utils/mono-dl-fallback.h>
#include <mono/utils/mono-logger.h>

extern "C" {

void emscripten_async_call(void (*func)(void*), void* arg, int millis);
int getentropy(void* buffer, size_t length);

void* ShalltearWebMonoLookupM2N(char* cookie);
void* ShalltearWebMonoDlLoad(const char* name, int flags, char** err, void* userData);
void* ShalltearWebMonoDlSym(void* handle, const char* name, char** err, void* userData);
void* ShalltearWebMonoGetN2M(uint32_t token, const char* key, void* extraArg);
bool ShalltearReadManagedBundle(void (*onAssembly)(const char* name, const uint8_t* data, uint32_t size, void* user),
								void* user);

void mono_icall_table_init(void);
void mono_ee_interp_init(const char* opts);
void mono_marshal_ilgen_init(void);
void mono_method_builder_ilgen_init(void);
void mono_sgen_mono_ilgen_init(void);
void mono_trace_init(void);
int monoeg_g_setenv(const char* variable, const char* value, int overwrite);
#if defined(SHALLTEAR_WEB_AOT)
void ShalltearWebMonoRegisterAOTModules(void);
#endif
void mono_bundled_resources_add_assembly_resource(const char* id, const char* name, const uint8_t* data,
												  uint32_t size, void (*freeFunc)(void*, void*), void* freeData);
void mono_wasm_install_get_native_to_interp_tramp(void* (*cb)(MonoMethod*, void*));
void mono_wasm_install_interp_to_native_callback(void* (*cb)(char*));
void mono_wasm_execute_timer(void);
void mono_background_exec(void);

// WASM C++ EH bridge compiled with this toolchain's native WASM EH ABI
void mono_llvm_cpp_throw_exception(void) {
	int* marker = nullptr;
	throw marker;
}

void mono_llvm_cpp_catch_exception(void (*func)(void*), void* arg, int* caught) {
	*caught = 0;
	try {
		func(arg);
	} catch (...) {
		*caught = 1;
	}
}

void mono_jiterp_begin_catch(void*) {
}

void mono_jiterp_end_catch(void) {
}

// jiterpreter disabled via "-jiterpreter"; these hooks satisfy linker references cleanly
void mono_interp_flush_jitcall_queue(void) {
}

void mono_interp_invoke_wasm_jit_call_trampoline(void*, void*, void*, void*, void*) {
}

int mono_interp_jit_wasm_entry_trampoline(void*, void*, void*, void*, void*, void*, void*, void*) {
	return 0;
}

void mono_interp_jit_wasm_jit_call_trampoline(void*, void*, void*, void*, void*) {
}

void mono_interp_record_interp_entry(void*) {
}

int mono_interp_tier_prepare_jiterpreter(void*, void*, void*, void*, void*, void*, void*, void*) {
	return 0;
}

void mono_wasm_free_method_data(void*, void*, void*) {
}

int mono_wasm_process_current_pid(void) {
	return 1;
}

int mono_wasm_browser_entropy(uint8_t* buffer, int32_t length) {
	if (!buffer || length <= 0) {
		return 0;
	}
	int32_t offset = 0;
	while (offset < length) {
		const size_t chunk = static_cast<size_t>(length - offset) > 256 ? 256 : static_cast<size_t>(length - offset);
		if (getentropy(buffer + offset, chunk) != 0) {
			return -1;
		}
		offset += static_cast<int32_t>(chunk);
	}
	return 0;
}

static void OnMonoTimer(void*) {
	mono_wasm_execute_timer();
}

static void OnMonoBackgroundExec(void*) {
	mono_background_exec();
}

void mono_wasm_schedule_timer(int shortestDueTimeMs) {
	const int delay = shortestDueTimeMs < 0 ? 0 : shortestDueTimeMs;
	emscripten_async_call(&OnMonoTimer, nullptr, delay);
}

void schedule_background_exec(void) {
	emscripten_async_call(&OnMonoBackgroundExec, nullptr, 0);
}

void mono_wasm_pthread_set_name(int32_t) {
}

void mono_wasm_schedule_synchronization_context(void) {
}

void mono_wasm_pthread_on_pthread_attached(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) {
}

void mono_wasm_pthread_on_pthread_unregistered(int32_t) {
}

void mono_wasm_pthread_on_pthread_registered(int32_t) {
}

void mono_wasm_dump_threads(void) {
}

void mono_wasm_start_deputy_thread_async(void) {
}

void mono_wasm_start_io_thread_async(void) {
}

} // extern "C"

namespace {

	bool _runtimeBooted = false;

	void FreeAssemblyBuffer(void*, void* freeData) {
		std::free(freeData);
	}

	void* ResolveNativeToInterp(MonoMethod* method, void* extraArg) {
		MonoClass* klass = mono_method_get_class(method);
		MonoImage* image = mono_class_get_image(klass);
		MonoAssembly* assembly = mono_image_get_assembly(image);
		MonoAssemblyName* aname = mono_assembly_get_name(assembly);
		const char* asmName = mono_assembly_name_get_name(aname);
		const char* ns = mono_class_get_namespace(klass);
		const char* clsName = mono_class_get_name(klass);
		const char* methodName = mono_method_get_name(method);
		MonoMethodSignature* sig = mono_method_signature(method);
		const uint32_t paramCount = mono_signature_get_param_count(sig);
		const uint32_t token = mono_method_get_token(method);

		char keyBuf[256];
		const int needed = std::snprintf(keyBuf, sizeof(keyBuf), "%s#%u:%s:%s:%s", methodName ? methodName : "",
										 paramCount, asmName ? asmName : "", ns ? ns : "", clsName ? clsName : "");
		if (needed < 0) {
			return nullptr;
		}
		if (static_cast<size_t>(needed) < sizeof(keyBuf)) {
			return ShalltearWebMonoGetN2M(token, keyBuf, extraArg);
		}
		std::string dynamicKey(static_cast<size_t>(needed), '\0');
		std::snprintf(dynamicKey.data(), dynamicKey.size() + 1, "%s#%u:%s:%s:%s", methodName ? methodName : "",
					  paramCount, asmName ? asmName : "", ns ? ns : "", clsName ? clsName : "");
		return ShalltearWebMonoGetN2M(token, dynamicKey.c_str(), extraArg);
	}

	void* ResolveInterpToNative(char* cookie) {
		void* fn = ShalltearWebMonoLookupM2N(cookie);
		if (!fn) {
			std::fprintf(stderr, "[C#/WASM] missing M2N signature '%s'\n", cookie ? cookie : "<null>");
		}
		return fn;
	}

	bool ReadFileBytes(const std::string& path, uint8_t*& outData, uint32_t& outSize) {
		std::FILE* file = std::fopen(path.c_str(), "rb");
		if (!file) {
			return false;
		}
		std::fseek(file, 0, SEEK_END);
		const long len = std::ftell(file);
		std::fseek(file, 0, SEEK_SET);
		if (len <= 0) {
			std::fclose(file);
			return false;
		}
		uint8_t* data = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(len)));
		if (!data) {
			std::fclose(file);
			return false;
		}
		const size_t readBytes = std::fread(data, 1, static_cast<size_t>(len), file);
		std::fclose(file);
		if (readBytes != static_cast<size_t>(len)) {
			std::free(data);
			return false;
		}
		outData = data;
		outSize = static_cast<uint32_t>(len);
		return true;
	}

	std::vector<std::string> ParseManifestArray(const std::string& text) {
		std::vector<std::string> names;
		size_t pos = 0;
		while (pos < text.size()) {
			const size_t open = text.find('"', pos);
			if (open == std::string::npos) {
				break;
			}
			std::string item;
			size_t cur = open + 1;
			while (cur < text.size() && text[cur] != '"') {
				if (text[cur] == '\\' && cur + 1 < text.size()) {
					++cur;
				}
				item.push_back(text[cur++]);
			}
			if (!item.empty()) {
				names.push_back(std::move(item));
			}
			pos = cur < text.size() ? cur + 1 : text.size();
		}
		return names;
	}

	bool RegisterBundledAssemblies(const char* managedRoot) {
		uint8_t* manifestBytes = nullptr;
		uint32_t manifestSize = 0;
		const std::string manifestPath = std::string(managedRoot) + "/managed-manifest.json";
		if (!ReadFileBytes(manifestPath, manifestBytes, manifestSize)) {
			std::fprintf(stderr, "[C#/WASM] couldn't read '%s'\n", manifestPath.c_str());
			return false;
		}
		const std::string manifestText(reinterpret_cast<const char*>(manifestBytes), manifestSize);
		std::free(manifestBytes);

		const std::vector<std::string> assemblies = ParseManifestArray(manifestText);
		if (assemblies.empty()) {
			std::fprintf(stderr, "[C#/WASM] managed-manifest.json listed no assemblies\n");
			return false;
		}

		for (const std::string& name : assemblies) {
			uint8_t* data = nullptr;
			uint32_t size = 0;
			const std::string fullPath = std::string(managedRoot) + "/" + name;
			if (!ReadFileBytes(fullPath, data, size)) {
				std::fprintf(stderr, "[C#/WASM] couldn't read assembly '%s'\n", fullPath.c_str());
				return false;
			}
			char* ownedName = strdup(name.c_str());
			mono_bundled_resources_add_assembly_resource(ownedName, ownedName, data, size, &FreeAssemblyBuffer, data);
		}
		return true;
	}

	void OnPackagedAssembly(const char* name, const uint8_t* data, uint32_t size, void* user) {
		if (!name || !data || size == 0) {
			return;
		}
		uint8_t* copy = static_cast<uint8_t*>(std::malloc(size));
		if (!copy) {
			return;
		}
		std::memcpy(copy, data, size);
		char* ownedName = strdup(name);
		mono_bundled_resources_add_assembly_resource(ownedName, ownedName, copy, size, &FreeAssemblyBuffer, copy);
		if (uint32_t* count = static_cast<uint32_t*>(user)) {
			++(*count);
		}
	}

	bool RegisterPackagedAssemblies() {
		uint32_t count = 0;
		return ShalltearReadManagedBundle(&OnPackagedAssembly, &count) && count > 0;
	}

} // anonymous namespace

extern "C" int32_t ShalltearScriptsInitialize(const void* engine, void* managed) {
	if (!_runtimeBooted) {
		if (!RegisterPackagedAssemblies() && !RegisterBundledAssemblies("/app/Managed")) {
			return -1;
		}

		const char* keys[] = {"APP_CONTEXT_BASE_DIRECTORY", "RUNTIME_IDENTIFIER", "System.Globalization.Invariant"};
		const char* values[] = {"/app/Managed/", "browser-wasm", "true"};
		monovm_initialize(3, keys, values);
		mono_dl_fallback_register(&ShalltearWebMonoDlLoad, &ShalltearWebMonoDlSym, nullptr, nullptr);
		mono_wasm_install_get_native_to_interp_tramp(&ResolveNativeToInterp);
		mono_wasm_install_interp_to_native_callback(&ResolveInterpToNative);
#if defined(SHALLTEAR_WEB_AOT)
		monoeg_g_setenv("MONO_AOT_MODE", "aot", 1);
		ShalltearWebMonoRegisterAOTModules();
		mono_jit_set_aot_mode(MONO_AOT_MODE_LLVMONLY_INTERP);
#else
		mono_jit_set_aot_mode(MONO_AOT_MODE_INTERP_ONLY);
#endif
		mono_icall_table_init();
		mono_ee_interp_init("-jiterpreter");
		mono_marshal_ilgen_init();
		mono_method_builder_ilgen_init();
		mono_sgen_mono_ilgen_init();
		mono_trace_init();
		mono_jit_init_version("mono", nullptr);
		mono_thread_set_main(mono_thread_current());
		_runtimeBooted = true;
	}

	MonoImageOpenStatus status = MONO_IMAGE_OK;
	MonoAssemblyName* aname = mono_assembly_name_new("GameScripts");
	MonoAssembly* gameScripts = mono_assembly_load(aname, nullptr, &status);
	mono_assembly_name_free(aname);
	if (!gameScripts) {
		std::fprintf(stderr, "[C#/WASM] couldn't load GameScripts.dll (status=%d)\n", static_cast<int>(status));
		return -1;
	}

	MonoImage* image = mono_assembly_get_image(gameScripts);
	MonoClass* exportsClass = image ? mono_class_from_name(image, "Shalltear.Generated", "Exports") : nullptr;
	MonoMethod* initMethod = exportsClass ? mono_class_get_method_from_name(exportsClass, "Initialize", 2) : nullptr;
	if (!initMethod) {
		std::fprintf(stderr, "[C#/WASM] Shalltear.Generated.Exports.Initialize(2) not found\n");
		return -1;
	}

	void* args[2] = {const_cast<void*>(engine), managed};
	MonoObject* exception = nullptr;
	MonoObject* resultObj = mono_runtime_invoke(initMethod, nullptr, args, &exception);
	if (exception) {
		MonoObject* nested = nullptr;
		MonoString* text = mono_object_to_string(exception, &nested);
		if (text) {
			char* utf8 = mono_string_to_utf8(text);
			std::fprintf(stderr, "[C#/WASM] Exports.Initialize threw: %s\n", utf8 ? utf8 : "");
			std::free(utf8);
		}
		return -1;
	}
	if (!resultObj) {
		return -1;
	}
	return *static_cast<int32_t*>(mono_object_unbox(resultObj));
}
