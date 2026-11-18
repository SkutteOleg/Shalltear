#include "Shalltear/Core/Reflection/VariantDispatch.h"

#include "Shalltear/Core/Reflection/Variant.h"

#include "Module/EngineStateTable.h"
#include "Platform/Library.h"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace Shalltear {

	namespace {

		struct DispatchEntry {
			const VariantVTable* vtable;
			void* ownerImage;
		};

		struct DispatchData {
			std::unordered_map<StringHash, std::vector<DispatchEntry>> entries;

			mutable std::shared_mutex mutex;
		};

		DispatchData* _boundData = nullptr;

		DispatchData& GetData() noexcept {
			if (_boundData)
				return *_boundData;
			static DispatchData data;
			return data;
		}

	} // anonymous namespace

	void VariantDispatch::Register(StringHash typeId, const VariantVTable* vtable) noexcept {
		if (typeId.GetValue() == 0 || vtable == nullptr)
			return;

		auto& data = GetData();
		std::unique_lock lock(data.mutex);

		void* ownerImage = Platform::ImageBaseOf(vtable);
		std::vector<DispatchEntry>& copies = data.entries[typeId];
		for (const DispatchEntry& copy : copies) {
			if (copy.ownerImage == ownerImage)
				return;
		}
		copies.push_back(DispatchEntry{vtable, ownerImage});
	}

	void VariantDispatch::Unregister(StringHash typeId) noexcept {
		if (typeId.GetValue() == 0)
			return;

		auto& data = GetData();
		std::unique_lock lock(data.mutex);

		data.entries.erase(typeId);
	}

	size_t VariantDispatch::UnregisterAllOwnedBy(const void* imageBase) noexcept {
		if (!imageBase)
			return 0;

		auto& data = GetData();
		std::unique_lock lock(data.mutex);

		size_t removed = 0;
		for (auto it = data.entries.begin(); it != data.entries.end();) {
			std::erase_if(it->second, [imageBase](const DispatchEntry& copy) { return copy.ownerImage == imageBase; });
			if (it->second.empty()) {
				it = data.entries.erase(it);
				++removed;
			} else {
				++it;
			}
		}
		return removed;
	}

	const VariantVTable* VariantDispatch::Lookup(StringHash typeId) noexcept {
		if (typeId.GetValue() == 0)
			return nullptr;

		auto& data = GetData();
		std::shared_lock lock(data.mutex);

		auto it = data.entries.find(typeId);
		return (it != data.entries.end()) ? it->second.front().vtable : nullptr;
	}

	const void* VariantDispatch::GetOwner(StringHash typeId) noexcept {
		if (typeId.GetValue() == 0)
			return nullptr;

		auto& data = GetData();
		std::shared_lock lock(data.mutex);

		auto it = data.entries.find(typeId);
		return (it != data.entries.end() && it->second.size() == 1) ? it->second.front().ownerImage : nullptr;
	}

	size_t VariantDispatch::GetEntryCount() noexcept {
		auto& data = GetData();
		std::shared_lock lock(data.mutex);
		return data.entries.size();
	}

	namespace ModuleInternal {

		void CaptureVariantDispatchEngineState(EngineStateTable& table) {
			table.variantDispatch = &GetData();
		}

		void BindVariantDispatchEngineState(const EngineStateTable& table) {
			_boundData = static_cast<DispatchData*>(table.variantDispatch);
		}

	} // namespace ModuleInternal

} // namespace Shalltear
