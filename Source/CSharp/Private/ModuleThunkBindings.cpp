#include "ModuleThunkBindings.h"

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Module/ModuleThunkRegistry.h"

#include <cstdint>

namespace Shalltear::CSharpImpl::ModuleThunkBindings {

	u64 GetTable(std::string_view moduleName) noexcept {
		if (moduleName.empty()) {
			return 0;
		}
		const str name(moduleName);
		const void* table = ModuleThunkRegistry::GetModuleThunkTable(name.c_str());
		return static_cast<u64>(reinterpret_cast<uintptr_t>(table));
	}

	u32 GetCount(std::string_view moduleName) noexcept {
		if (moduleName.empty()) {
			return 0;
		}
		const str name(moduleName);
		u32 count = 0;
		ModuleThunkRegistry::GetModuleThunkTable(name.c_str(), &count);
		return count;
	}

	u64 GetModuleEpoch(std::string_view moduleName) noexcept {
		if (moduleName.empty()) {
			return 0;
		}
		const str name(moduleName);
		u64 epoch = 0;
		ModuleThunkRegistry::GetModuleThunkTable(name.c_str(), nullptr, &epoch);
		return epoch;
	}

	u64 GetEpoch() noexcept {
		return ModuleThunkRegistry::GetEpoch();
	}

	u64 GetEpochAddress() noexcept {
		return static_cast<u64>(reinterpret_cast<uintptr_t>(ModuleThunkRegistry::GetEpochAddress()));
	}

} // namespace Shalltear::CSharpImpl::ModuleThunkBindings
