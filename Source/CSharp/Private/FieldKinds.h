#pragma once

#include "Shalltear/Core/Reflection/TypeInfo.h"
#include "Shalltear/Core/Types.h"

namespace Shalltear::CSharpImpl {

	struct FieldKindInfo {
		const TypeInfo* type;
		u32 size;
		u32 alignment;
	};

	namespace FieldKinds {

		const FieldKindInfo* Find(i32 kind);
		const FieldKindInfo& GetText();

		const TypeInfo* FindEngineHandle(u64 pointeeHash);

	} // namespace FieldKinds

} // namespace Shalltear::CSharpImpl
