#pragma once

#include "Shalltear/Core/CoreAPI.h"
#include "Shalltear/Core/StringHash.h"
#include "Shalltear/Core/Types.h"

namespace Shalltear {

	struct VariantVTable;

	class SHALLTEAR_CORE_API VariantDispatch {
	public:
		// vtable must outlive its Variants
		static void Register(StringHash typeId, const VariantVTable* vtable) noexcept;

		// must run before FreeLibrary
		static void Unregister(StringHash typeId) noexcept;

		// count of types left with no image
		static size_t UnregisterAllOwnedBy(const void* imageBase) noexcept;

		static const VariantVTable* Lookup(StringHash typeId) noexcept;

		// null when more than 1 image registered it
		static const void* GetOwner(StringHash typeId) noexcept;

		static size_t GetEntryCount() noexcept;
	};

} // namespace Shalltear
