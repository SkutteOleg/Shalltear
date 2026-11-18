#include "HostFxr.h"

#include "Shalltear/Core/StringUtil.h"

#include <algorithm>
#include <cstdint>

namespace Shalltear::CSharpEditor::HostFxr {

	namespace {

		struct InitializeParameters {
			size_t size;
			const wchar_t* hostPath;
			const wchar_t* dotnetRoot;
		};

		using InitializeFn = i32 (*)(const wchar_t* runtimeConfig, const InitializeParameters* parameters, void** outContext);
		using LoadFn = i32 (*)(const wchar_t* assembly, const wchar_t* type, const wchar_t* method, const wchar_t* delegateType,
							   void* reserved, void** outFunction);

		// hostfxr's UNMANAGEDCALLERSONLY_METHOD
		const wchar_t* const UNMANAGED_CALLERS_ONLY = reinterpret_cast<const wchar_t*>(static_cast<intptr_t>(-1));

		// CoreCLR fails to start (E_INVALIDARG) on forward slashes
		std::wstring NativePath(const str& path) {
			std::wstring wide = StringUtil::UTF8ToWide(path);
			std::replace(wide.begin(), wide.end(), L'/', L'\\');
			return wide;
		}

	} // anonymous namespace

	i32 InitializeForRuntimeConfig(void* function, const str& runtimeConfig, const str& dotnetRoot, void** outContext) {
		const std::wstring config = NativePath(runtimeConfig);
		const std::wstring root = NativePath(dotnetRoot);
		const InitializeParameters parameters{sizeof(InitializeParameters), nullptr, root.c_str()};
		return reinterpret_cast<InitializeFn>(function)(config.c_str(), &parameters, outContext);
	}

	i32 LoadAssemblyAndGetFunctionPointer(void* function, const str& assembly, const char* type, const char* method, void** outFunction) {
		return reinterpret_cast<LoadFn>(function)(NativePath(assembly).c_str(), StringUtil::UTF8ToWide(type).c_str(),
												  StringUtil::UTF8ToWide(method).c_str(), UNMANAGED_CALLERS_ONLY, nullptr, outFunction);
	}

	std::string_view GetLibraryName() {
		return "hostfxr.dll";
	}

} // namespace Shalltear::CSharpEditor::HostFxr
