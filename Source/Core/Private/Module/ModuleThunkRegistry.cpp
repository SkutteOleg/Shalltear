#include "Shalltear/Core/Module/ModuleThunkRegistry.h"

#include "Module/EngineStateTable.h"
#include "Platform/Library.h"

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Memory.h"

#include <mutex>
#include <shared_mutex>

namespace Shalltear {

	namespace {

		struct ThunkTableEntry {
			const void* table = nullptr;
			u32 count = 0;
			const void* owner = nullptr;
			u64 epoch = 0;
		};

		struct ThunkRegistryState {
			mutable std::shared_mutex mutex;
			HashMap<str, ThunkTableEntry> tables{GetMemoryResource(MemTag::Core)};
			std::atomic<u64> epoch{1};
		};

		ThunkRegistryState* _boundState = nullptr;
		std::atomic<ThunkRegistrarNode*> _pendingHead{nullptr};

		ThunkRegistryState& GetState() noexcept {
			if (_boundState) {
				return *_boundState;
			}
			static ThunkRegistryState state;
			return state;
		}

		const void* ResolveOwner(const void* owner, const void* addressHint) {
			if (owner) {
				return owner;
			}
			if (addressHint) {
				if (const void* base = Platform::ImageBaseOf(addressHint)) {
					return base;
				}
			}
			return nullptr;
		}

	} // anonymous namespace

	ThunkRegistrarNode::ThunkRegistrarNode(const char* name, const void* const* thunkTable, u32 slotCount) noexcept
		: moduleName(name), table(thunkTable), count(slotCount), next(nullptr) {
		ModuleThunkRegistry::AddPendingThunkTable(this);
	}

	void ModuleThunkRegistry::AddPendingThunkTable(ThunkRegistrarNode* node) noexcept {
		if (!node) {
			return;
		}
		ThunkRegistrarNode* head = _pendingHead.load(std::memory_order_relaxed);
		do {
			node->next = head;
		} while (!_pendingHead.compare_exchange_weak(head, node, std::memory_order_release, std::memory_order_relaxed));
	}

	void ModuleThunkRegistry::RegisterPendingThunkTables() {
		ThunkRegistrarNode* list = _pendingHead.exchange(nullptr, std::memory_order_acq_rel);
		while (list) {
			ThunkRegistrarNode* next = list->next;
			list->next = nullptr;
			if (list->moduleName && list->table) {
				const void* owner = ResolveOwner(nullptr, list);
				RegisterModuleThunkTable(list->moduleName, list->table, list->count, owner);
			}
			list = next;
		}
	}

	u32 ModuleThunkRegistry::UnlinkPendingThunkTablesIn(const void* imageBase) noexcept {
		if (!imageBase) {
			return 0;
		}
		const Platform::ImageSpan span = Platform::GetImageSpan(imageBase);
		if (!span.begin || !span.end) {
			return 0;
		}
		ThunkRegistrarNode* list = _pendingHead.exchange(nullptr, std::memory_order_acq_rel);
		u32 unlinked = 0;
		while (list) {
			ThunkRegistrarNode* next = list->next;
			list->next = nullptr;
			if (span.Contains(list)) {
				++unlinked;
			} else {
				AddPendingThunkTable(list);
			}
			list = next;
		}
		return unlinked;
	}

	u32 ModuleThunkRegistry::UnregisterThunkTablesOwnedBy(const void* owner) {
		if (!owner) {
			return 0;
		}
		ThunkRegistryState& state = GetState();
		std::unique_lock lock(state.mutex);
		u32 removed = 0;
		for (auto it = state.tables.begin(); it != state.tables.end();) {
			if (it->second.owner == owner) {
				it = state.tables.erase(it);
				++removed;
			} else {
				++it;
			}
		}
		if (removed > 0) {
			state.epoch.fetch_add(1, std::memory_order_acq_rel);
		}
		return removed;
	}

	void ModuleThunkRegistry::RegisterModuleThunkTable(const char* moduleName, const void* table, u32 count,
													   const void* owner) {
		if (!moduleName || moduleName[0] == '\0' || !table) {
			return;
		}
		ThunkRegistryState& state = GetState();
		std::unique_lock lock(state.mutex);
		const u64 nextEpoch = state.epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
		const void* resolvedOwner = ResolveOwner(owner, table);
		state.tables[str(moduleName)] = ThunkTableEntry{table, count, resolvedOwner, nextEpoch};
	}

	void ModuleThunkRegistry::UnregisterModuleThunkTable(const char* moduleName, const void* owner) {
		if (!moduleName || moduleName[0] == '\0') {
			return;
		}
		ThunkRegistryState& state = GetState();
		std::unique_lock lock(state.mutex);
		auto it = state.tables.find(str(moduleName));
		if (it == state.tables.end()) {
			return;
		}
		if (owner && it->second.owner != owner) {
			return;
		}
		state.tables.erase(it);
		state.epoch.fetch_add(1, std::memory_order_acq_rel);
	}

	const void* ModuleThunkRegistry::GetModuleThunkTable(const char* moduleName, u32* outCount,
														 u64* outEpoch) noexcept {
		if (outCount) {
			*outCount = 0;
		}
		if (outEpoch) {
			*outEpoch = 0;
		}
		if (!moduleName || moduleName[0] == '\0') {
			return nullptr;
		}
		ThunkRegistryState& state = GetState();
		std::shared_lock lock(state.mutex);
		auto it = state.tables.find(str(moduleName));
		if (it == state.tables.end()) {
			return nullptr;
		}
		if (outCount) {
			*outCount = it->second.count;
		}
		if (outEpoch) {
			*outEpoch = it->second.epoch;
		}
		return it->second.table;
	}

	u64 ModuleThunkRegistry::GetEpoch() noexcept {
		return GetState().epoch.load(std::memory_order_acquire);
	}

	const std::atomic<u64>* ModuleThunkRegistry::GetEpochAddress() noexcept {
		return &GetState().epoch;
	}

	void ModuleThunkRegistry::Clear() {
		ThunkRegistryState& state = GetState();
		std::unique_lock lock(state.mutex);
		state.tables.clear();
		state.epoch.fetch_add(1, std::memory_order_acq_rel);
	}

	namespace ModuleInternal {

		void CaptureModuleThunkRegistryEngineState(EngineStateTable& table) {
			table.moduleThunkRegistry = &GetState();
		}

		void BindModuleThunkRegistryEngineState(const EngineStateTable& table) {
			_boundState = static_cast<ThunkRegistryState*>(table.moduleThunkRegistry);
		}

	} // namespace ModuleInternal

} // namespace Shalltear
