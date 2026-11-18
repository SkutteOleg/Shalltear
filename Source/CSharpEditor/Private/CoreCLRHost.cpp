#include "CoreCLRHost.h"

#include "HostFxr.h"
#include "HostLibrary.h"

#include <format>

namespace Shalltear::CSharpEditor::CoreCLRHost {

	namespace {

		// hostfxr_delegate_type::hdt_load_assembly_and_get_function_pointer
		constexpr i32 LOAD_ASSEMBLY_AND_GET_FUNCTION_POINTER = 5;

		constexpr std::string_view AssemblySuffix = ".dll";

		using GetDelegateFn = i32 (*)(void* context, i32 type, void** outDelegate);
		using CloseFn = i32 (*)(void* context);

		bool _started = false;
		SltInitializeFn _entry = nullptr;

	} // anonymous namespace

	SltInitializeFn Start(const str& sdkRoot, const str& runtimeVersion, const str& runtimeAssembly, str& outError) {
		if (_started) {
			outError = "CoreCLR already started in this process (1 runtime allowed)";
			return nullptr;
		}
		if (!runtimeAssembly.ends_with(AssemblySuffix)) {
			outError = std::format("{} is not an assembly", runtimeAssembly);
			return nullptr;
		}

		const str library = std::format("{}/host/fxr/{}/{}", sdkRoot, runtimeVersion, HostFxr::GetLibraryName());
		str reason;
		void* hostfxr = CSharpImpl::OpenHostLibrary(library, reason);
		if (!hostfxr) {
			outError = std::format("{} didn't load ({})", library, reason);
			return nullptr;
		}
		void* initialize = CSharpImpl::FindHostSymbol(hostfxr, "hostfxr_initialize_for_runtime_config");
		const auto getDelegate = reinterpret_cast<GetDelegateFn>(CSharpImpl::FindHostSymbol(hostfxr, "hostfxr_get_runtime_delegate"));
		const auto close = reinterpret_cast<CloseFn>(CSharpImpl::FindHostSymbol(hostfxr, "hostfxr_close"));
		if (!initialize || !getDelegate || !close) {
			outError = std::format("{} has no hosting entry points", library);
			return nullptr;
		}

		const str runtimeConfig = runtimeAssembly.substr(0, runtimeAssembly.size() - AssemblySuffix.size()) + ".runtimeconfig.json";
		void* context = nullptr;
		i32 result = HostFxr::InitializeForRuntimeConfig(initialize, runtimeConfig, sdkRoot, &context);
		if (result < 0 || !context) {
			outError = std::format("hostfxr couldn't initialize from {} (0x{:08x})", runtimeConfig, static_cast<u32>(result));
			return nullptr;
		}

		_started = true;
		void* load = nullptr;
		result = getDelegate(context, LOAD_ASSEMBLY_AND_GET_FUNCTION_POINTER, &load);
		close(context);
		if (result != 0 || !load) {
			outError = std::format("CoreCLR didn't start from {} (0x{:08x})", sdkRoot, static_cast<u32>(result));
			return nullptr;
		}

		void* entry = nullptr;
		result = HostFxr::LoadAssemblyAndGetFunctionPointer(load, runtimeAssembly, "Shalltear.Interop.Bridge, Shalltear.Runtime", "Initialize", &entry);
		if (result != 0 || !entry) {
			outError = std::format("{} has no bridge entry (0x{:08x})", runtimeAssembly, static_cast<u32>(result));
			return nullptr;
		}
		_entry = reinterpret_cast<SltInitializeFn>(entry);
		return _entry;
	}

	bool IsStarted() {
		return _started;
	}

	SltInitializeFn GetEntry() {
		return _entry;
	}

} // namespace Shalltear::CSharpEditor::CoreCLRHost
