#pragma once

#include "ScriptBridge.h"

#include "Shalltear/Core/Reflection/TypeInfo.h"
#include "Shalltear/Core/Types.h"

#include <string_view>

namespace Shalltear::CSharpImpl {

	class ManagedBehavior;

	struct ManagedType {
		const TypeInfo* info = nullptr;
		str name;
		i32 managedTypeId = 0;
		u32 hookMask = 0;
		u32 liveCount = 0;
	};

	namespace ManagedTypes {

		struct TypeObserver {
			void (*registered)(const TypeInfo* type, std::string_view fullName, const void* ownerToken) = nullptr;
			void (*released)(const void* ownerToken) = nullptr;
		};

		i32 Register(const SltTypeDesc& desc);

		void ReleaseAll();

		const void* GetOwnerToken();
		void SetTypeObserver(const TypeObserver& observer);

		void Link(ManagedBehavior& proxy);
		void Unlink(ManagedBehavior& proxy);

		u64 GetHookCrossings();
		void CountHookCrossing();

	} // namespace ManagedTypes

} // namespace Shalltear::CSharpImpl
