#include "HostFxr.h"

#include <cstdint>

namespace Shalltear::CSharpEditor::HostFxr {

	namespace {

		struct InitializeParameters {
			size_t size;
			const char* hostPath;
			const char* dotnetRoot;
		};

		using InitializeFn = i32 (*)(const char* runtimeConfig, const InitializeParameters* parameters, void** outContext);
		using LoadFn = i32 (*)(const char* assembly, const char* type, const char* method, const char* delegateType,
							   void* reserved, void** outFunction);

		// hostfxr's UNMANAGEDCALLERSONLY_METHOD
		const char* const UNMANAGED_CALLERS_ONLY = reinterpret_cast<const char*>(static_cast<intptr_t>(-1));

	} // anonymous namespace

	i32 InitializeForRuntimeConfig(void* function, const str& runtimeConfig, const str& dotnetRoot, void** outContext) {
		const InitializeParameters parameters{sizeof(InitializeParameters), nullptr, dotnetRoot.c_str()};
		return reinterpret_cast<InitializeFn>(function)(runtimeConfig.c_str(), &parameters, outContext);
	}

	i32 LoadAssemblyAndGetFunctionPointer(void* function, const str& assembly, const char* type, const char* method, void** outFunction) {
		return reinterpret_cast<LoadFn>(function)(assembly.c_str(), type, method, UNMANAGED_CALLERS_ONLY, nullptr, outFunction);
	}

	std::string_view GetLibraryName() {
		return "libhostfxr.so";
	}

	std::string_view GetMuxerName() {
		return "dotnet";
	}

	char GetSearchPathSeparator() {
		return ':';
	}

} // namespace Shalltear::CSharpEditor::HostFxr
