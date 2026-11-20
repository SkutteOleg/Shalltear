#pragma once

#include "ScriptAPI.h"
#include "ScriptBridge.h"
#include "ShadowStorage.h"

#include "Shalltear/Core/Asset/AssetImporter.h"
#include "Shalltear/Core/Reflection/TypeInfo.h"
#include "Shalltear/Core/Types.h"

#include <deque>
#include <string_view>
#include <vector>

namespace Shalltear::CSharpImpl {

	class ManagedBehavior;
	class ManagedDataAsset;

	struct ManagedType {
		const TypeInfo* info = nullptr;
		const ManagedType* parent = nullptr;
		str name;
		str script;
		std::deque<str> fieldNames;
		std::vector<ShadowInit> inits;
		i32 managedTypeId = 0;
		u32 hookMask = 0;
		u32 listenerMask = 0;
		u32 liveCount = 0;
		u32 slotEnd = 0;
		bool hasFields = false;
		bool normalizes = false;
		bool asset = false;
		IAssetImporter* importer = nullptr;
	};

	namespace ManagedTypes {

		struct TypeObserver {
			void (*registered)(const TypeInfo* type, std::string_view fullName, const void* ownerToken) = nullptr;
			void (*released)(const void* ownerToken) = nullptr;
		};

		i32 Register(const SltTypeDesc& desc);
		i32 RegisterEnum(const SltEnumDesc& desc);
		i32 RegisterValue(SltValueDesc& desc);

		void ReleaseAll();

		// scriptIndex = managedTypeId + 1
		const ManagedType* FindByIndex(i32 scriptIndex);

		const void* GetOwnerToken();
		u32 GetLiveCount();
		bool HasLiveAssets();
		void ForEachType(void (*visit)(const ManagedType& type, void* user), void* user);
		void SetTypeObserver(const TypeObserver& observer);

		void Link(ManagedBehavior& proxy);
		void Unlink(ManagedBehavior& proxy);
		void Link(ManagedDataAsset& proxy);
		void Unlink(ManagedDataAsset& proxy);

		void LinkDirty(ManagedBehavior& proxy);
		void UnlinkDirty(ManagedBehavior& proxy);
		void LinkDirty(ManagedDataAsset& proxy);
		void UnlinkDirty(ManagedDataAsset& proxy);
		SHALLTEAR_CSHARP_API void PullDirty();
		SHALLTEAR_CSHARP_API extern ManagedBehavior* dirtyHead;
		SHALLTEAR_CSHARP_API extern ManagedDataAsset* dirtyAssetHead;
		inline void FlushDirty() {
			if (dirtyHead || dirtyAssetHead) {
				PullDirty();
			}
		}

		void SetRestoring(bool restoring);
		bool IsRestoring();

		struct Counters {
			u64 hookCrossings = 0;
			u64 pushes = 0;
			u64 pulls = 0;
			u64 aliveChecks = 0;
			u64 textAssigns = 0;
		};
		Counters& GetCounters();

		i32& GetLifecycleDepth();

	} // namespace ManagedTypes

} // namespace Shalltear::CSharpImpl
