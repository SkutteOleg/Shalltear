#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Serialization/HandleResolver.h"
#include "Shalltear/Core/Types.h"

#include <vector>

namespace Shalltear {
	class Scene;
} // namespace Shalltear

namespace Shalltear::SceneInternal {

	inline constexpr u64 SLT_MIGRATION_SENTINEL = 0x534C544D49475241ULL;

	struct MigrationEnvelope {
		u64 ownerId = 0;
		u64 componentId = 0;
		u64 typeNameHash = 0;
		u64 localId = 0;
		u64 prefabKey = 0;
		u32 slotIndex = 0;
		bool restored = false;
		std::vector<u8> blob;
	};

	struct MigrationSnapshot {
		Vector<MigrationEnvelope> envelopes{GetMemoryResource(MemTag::Scripting)};
		Vector<Scene*> affectedScenes{GetMemoryResource(MemTag::Scripting)};
	};

	bool MigrationPreconditionsMet();
	bool IsMigrationLegal();

	HandleResolverScope MigrationResolverScope();

	void CaptureAndDemolishModuleComponents(const void* moduleImage, MigrationSnapshot& out);

	u32 RebuildPreservedComponents();

	void RestoreModuleComponents(MigrationSnapshot& snapshot);

} // namespace Shalltear::SceneInternal
