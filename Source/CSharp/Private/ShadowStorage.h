#pragma once

#include "Shalltear/Core/Reflection/TypeInfo.h"
#include "Shalltear/Core/Types.h"

#include <vector>

namespace Shalltear::CSharpImpl {

	struct ElementOps;

	struct ShadowInit {
		u32 offset = 0;
		const ElementOps* list = nullptr;
	};

	struct ElementOps {
		u32 size = 0;
		u32 alignment = 0;
		bool text = false;
		std::vector<ShadowInit> inits;
	};

	struct ShadowList {
		u8* data = nullptr;
		u32 count = 0;
		u32 capacity = 0;
		const ElementOps* ops = nullptr;
	};

	namespace ShadowStorage {

		// memory must be zeroed before Construct
		void Construct(void* block, const std::vector<ShadowInit>& inits);
		void Destroy(void* block, const std::vector<ShadowInit>& inits);

		// valid until next Resize of this list
		void* Resize(ShadowList& list, size_t count);

		void FillContainer(ContainerInfo& container, const TypeInfo* elementType, u32 elementSize);

	} // namespace ShadowStorage

} // namespace Shalltear::CSharpImpl
