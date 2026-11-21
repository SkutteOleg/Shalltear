#pragma once

#include "Shalltear/Core/CoreAPI.h"
#include "Shalltear/Core/Types.h"

#include <atomic>

namespace Shalltear {

	struct SHALLTEAR_CORE_API ThunkRegistrarNode {
		const char* moduleName;
		const void* const* table;
		u32 count;
		ThunkRegistrarNode* next;

		ThunkRegistrarNode(const char* moduleName, const void* const* table, u32 count) noexcept;
	};

	class SHALLTEAR_CORE_API ModuleThunkRegistry {
	public:
		static constexpr u32 UNKNOWN_COUNT = 0xFFFFFFFFu;

		static void AddPendingThunkTable(ThunkRegistrarNode* node) noexcept;
		static void RegisterPendingThunkTables();
		static u32 UnlinkPendingThunkTablesIn(const void* imageBase) noexcept;
		static u32 UnregisterThunkTablesOwnedBy(const void* owner);

		static void RegisterModuleThunkTable(const char* moduleName, const void* table, u32 count = UNKNOWN_COUNT,
											 const void* owner = nullptr);
		static void UnregisterModuleThunkTable(const char* moduleName, const void* owner = nullptr);
		static const void* GetModuleThunkTable(const char* moduleName, u32* outCount = nullptr,
											   u64* outEpoch = nullptr) noexcept;

		static u64 GetEpoch() noexcept;
		static const std::atomic<u64>* GetEpochAddress() noexcept;
		static void Clear();
	};

} // namespace Shalltear

#define SLT_DETAIL_THUNK_REG_NAME_INNER(a, b) a##b
#define SLT_DETAIL_THUNK_REG_NAME(a, b) SLT_DETAIL_THUNK_REG_NAME_INNER(a, b)

#define SLT_REGISTER_MODULE_THUNKS(ModuleNameLiteral, TableArray)                                       \
	static ::Shalltear::ThunkRegistrarNode SLT_DETAIL_THUNK_REG_NAME(g_sltThunkRegistrar, __COUNTER__){ \
		ModuleNameLiteral, TableArray, static_cast<::Shalltear::u32>(sizeof(TableArray) / sizeof((TableArray)[0]))};
