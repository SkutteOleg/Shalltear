#include "Shalltear/Core/Serialization/BinarySerializer.h"

#include "SerializerInternal.h"
#include "Shalltear/Core/BuildInfo.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"

#include <cstring>
#include <vector>

namespace Shalltear {

	namespace {

		constexpr u32 BINARY_MAGIC = 0x534C5442;
		constexpr u16 BINARY_VERSION = 1;
		constexpr u32 BINARY_HEADER_SIZE = 24;

	} // anonymous namespace

	namespace SerializerInternal {

		Buffer BinarySerialize(const void* instance, const TypeInfo* type, Vector<Guid>* outAssetRefs) {
			if (!instance || !type) {
				return Buffer();
			}

			BinaryWriter writer(outAssetRefs);
			writer.WriteU32(BINARY_MAGIC);
			writer.WriteU16(BINARY_VERSION);
			writer.WriteU16(0);
			writer.WriteU64(BuildInfo::GetEngineHash());
			writer.WriteU32(0);
			writer.WriteU32(0);

			if (!BinaryWriteObject(writer, instance, type, 0)) {
				return Buffer();
			}

			Buffer buffer(writer.Size(), MemTag::Core);
			if (writer.Size() > 0) {
				std::memcpy(buffer.data, writer.Data().data(), writer.Size());
			}
			return buffer;
		}

	} // namespace SerializerInternal

	Buffer BinarySerializer::Serialize(const void* instance, const TypeInfo* type) {
		return SerializerInternal::BinarySerialize(instance, type, nullptr);
	}

	bool BinarySerializer::Deserialize(void* instance, const TypeInfo* type, const Buffer& buffer) {
		if (!instance || !type || !buffer.data || buffer.size < BINARY_HEADER_SIZE) {
			return false;
		}

		SerializerInternal::BinaryReader reader(buffer.data, buffer.size);

		u32 magic = 0;
		u16 version = 0;
		u16 flags = 0;
		u64 engineHash = 0;
		u32 typeTableOffset = 0;
		u32 reserved = 0;
		if (!reader.ReadU32(magic) || !reader.ReadU16(version) || !reader.ReadU16(flags) ||
			!reader.ReadU64(engineHash) || !reader.ReadU32(typeTableOffset) || !reader.ReadU32(reserved)) {
			return false;
		}
		(void)flags;
		(void)engineHash;
		(void)typeTableOffset;
		(void)reserved;

		if (magic != BINARY_MAGIC) {
			SLT_LOG_ERROR("Serialization", "BinarySerializer: bad magic 0x{:08x}", magic);
			return false;
		}
		if (version != BINARY_VERSION) {
			SLT_LOG_ERROR("Serialization", "BinarySerializer: unsupported version {}", version);
			return false;
		}

		std::vector<SerializerInternal::HandleFixupJob> fixups;
		if (!SerializerInternal::BinaryReadObject(reader, instance, type, 0, fixups)) {
			return false;
		}

		SerializerInternal::RunHandleFixups(fixups);
		return true;
	}

} // namespace Shalltear
