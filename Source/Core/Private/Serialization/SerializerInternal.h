#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Reflection/TypeInfo.h"
#include "Shalltear/Core/Serialization/JSON.h"
#include "Shalltear/Core/StringHash.h"
#include "Shalltear/Core/Types.h"

#include <cstring>
#include <deque>
#include <vector>

namespace Shalltear {
	namespace SerializerInternal {

		// test Handle and Guid before the trivial fallbacks
		enum class FieldCategory {
			Handle,
			Subobject,
			Str,
			Guid,
			MathPrim,
			Prim,
			Enum,
			Trivial,
			Container,
			Unsupported,
		};

		struct ValueDesc {
			FieldCategory category = FieldCategory::Unsupported;
			const TypeInfo* type = nullptr;
			size_t size = 0;
			const ContainerInfo* container = nullptr;
			StringHash debugName;
		};

		FieldCategory CategorizeType(const TypeInfo* type, u32 flags);
		FieldCategory CategorizeField(const FieldInfo* field);

		ValueDesc DescribeField(const FieldInfo* field);
		ValueDesc DescribeElement(const ContainerInfo& container, StringHash debugName);

		void CollectSerializedFields(const TypeInfo* type, std::vector<const FieldInfo*>& out);

		str HashToHex(StringHash hash);
		bool HexToHash(std::string_view text, StringHash& out);

		enum class PrimKind {
			None,
			Bool,
			I8,
			I16,
			I32,
			I64,
			U8,
			U16,
			U32,
			U64,
			F32,
			F64,
		};

		PrimKind ClassifyPrim(const TypeInfo* type);
		bool IsStrType(const TypeInfo* type);

		u64 ReadEnumBits(const void* ptr, size_t size);
		void WriteEnumBits(void* ptr, size_t size, u64 raw);

		using StringPool = std::deque<str>;

		JSONValue TextBuildObject(const void* instance, const TypeInfo* type, StringPool& pool, u32 depth);

		// load scope must be complete (handle fields resolve immediately)
		bool TextReadObject(void* instance, const TypeInfo* type, const JSONValue& obj, u32 depth);

		// host byte order
		class BinaryWriter {
		public:
			explicit BinaryWriter(Vector<Guid>* assetRefs = nullptr) : _assetRefs(assetRefs) {}

			void WriteU8(u8 v) { Append(&v, sizeof(v)); }
			void WriteU16(u16 v) { Append(&v, sizeof(v)); }
			void WriteU32(u32 v) { Append(&v, sizeof(v)); }
			void WriteU64(u64 v) { Append(&v, sizeof(v)); }

			void WriteBytes(const void* p, size_t n) { Append(p, n); }

			void NoteAssetRef(const Guid& guid) {
				if (_assetRefs) {
					_assetRefs->push_back(guid);
				}
			}

			size_t BeginPatch() {
				size_t offset = _data.size();
				u32 placeholder = 0;
				Append(&placeholder, sizeof(placeholder));
				return offset;
			}

			void EndPatch(size_t offset) {
				u32 length = static_cast<u32>(_data.size() - offset - sizeof(u32));
				std::memcpy(_data.data() + offset, &length, sizeof(length));
			}

			const std::vector<u8>& Data() const { return _data; }
			size_t Size() const { return _data.size(); }
		private:
			void Append(const void* p, size_t n) {
				if (n == 0) {
					return;
				}
				const u8* bytes = static_cast<const u8*>(p);
				_data.insert(_data.end(), bytes, bytes + n);
			}

			std::vector<u8> _data;
			Vector<Guid>* _assetRefs = nullptr;
		};

		class BinaryReader {
		public:
			BinaryReader(const u8* data, size_t size) : _data(data), _size(size) {}

			bool ReadU8(u8& v) { return ReadRaw(&v, sizeof(v)); }
			bool ReadU16(u16& v) { return ReadRaw(&v, sizeof(v)); }
			bool ReadU32(u32& v) { return ReadRaw(&v, sizeof(v)); }
			bool ReadU64(u64& v) { return ReadRaw(&v, sizeof(v)); }
			bool ReadBytes(void* dst, size_t n) { return ReadRaw(dst, n); }

			const u8* CurrentPtr() const { return _data + _cursor; }

			bool Skip(size_t n) {
				if (n > Remaining()) {
					return false;
				}
				_cursor += n;
				return true;
			}

			bool Seek(size_t pos) {
				if (pos > _size) {
					return false;
				}
				_cursor = pos;
				return true;
			}

			size_t Tell() const { return _cursor; }
			size_t Remaining() const { return _size - _cursor; }
		private:
			bool ReadRaw(void* dst, size_t n) {
				if (n > Remaining()) {
					return false;
				}
				std::memcpy(dst, _data + _cursor, n);
				_cursor += n;
				return true;
			}

			const u8* _data;
			size_t _size;
			size_t _cursor = 0;
		};

		struct HandleFixupJob {
			void* fieldAddr;
			StringHash pointeeType;
			Guid assetId;
			// when set, fieldAddr = patched object (call AfterWrite)
			const TypeInfo* writtenType = nullptr;
		};

		Buffer BinarySerialize(const void* instance, const TypeInfo* type, Vector<Guid>* outAssetRefs);

		bool BinaryWriteObject(BinaryWriter& writer, const void* instance, const TypeInfo* type, u32 depth);

		bool BinaryReadObject(BinaryReader& reader, void* instance, const TypeInfo* type, u32 depth, std::vector<HandleFixupJob>& fixups);

		void RunHandleFixups(const std::vector<HandleFixupJob>& fixups);

	} // namespace SerializerInternal
} // namespace Shalltear
