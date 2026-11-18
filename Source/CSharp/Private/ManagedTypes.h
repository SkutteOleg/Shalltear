#pragma once

#include "ScriptBridge.h"

#include "Shalltear/Core/Reflection/TypeInfo.h"
#include "Shalltear/Core/Types.h"

#include <deque>
#include <string_view>

namespace Shalltear::CSharpImpl {

	class ManagedBehavior;

	struct ManagedType {
		const TypeInfo* info = nullptr;
		str name;
		std::deque<str> fieldNames;
		i32 managedTypeId = 0;
		u32 hookMask = 0;
		u32 liveCount = 0;
		u32 slotEnd = 0;
		bool hasFields = false;
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

		void LinkDirty(ManagedBehavior& proxy);
		void UnlinkDirty(ManagedBehavior& proxy);
		void FlushDirty();

		struct Counters {
			u64 hookCrossings = 0;
			u64 pushes = 0;
			u64 pulls = 0;
		};
		Counters& GetCounters();

	} // namespace ManagedTypes

} // namespace Shalltear::CSharpImpl
