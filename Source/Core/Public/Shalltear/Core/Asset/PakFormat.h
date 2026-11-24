#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/CoreAPI.h"
#include "Shalltear/Core/Types.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace Shalltear {

	enum class PakFeatureRequired : u64 {
		None = 0,
		MphfIndex = 1ull << 0,
		ExternalPages = 1ull << 1,
		EncryptedPages = 1ull << 2,
		VariableSizePages = 1ull << 3
	};

	enum class PakFeatureOptional : u64 {
		None = 0,
		DebugNames = 1ull << 0,
		PageHashes = 1ull << 1,
		Signed = 1ull << 2,
		StringTable = 1ull << 3,
		Documents = 1ull << 4
	};

	constexpr u64 PAK_SUPPORTED_REQUIRED = 0;

	enum class PakCodec : u8 {
		None = 0,
		LZ4 = 1
	};

	constexpr u32 LZ4_MIN_MATCH = 4;
	constexpr u32 LZ4_LAST_LITERALS = 5;
	constexpr u32 LZ4_MATCH_FLOOR = 12;
	constexpr u32 LZ4_MAX_OFFSET = 65535;

	constexpr u64 LZ4_MAX_RATIO = 255;

	enum class PakResidency : u8 {
		Resident = 0,
		Hot = 1,
		Streamed = 2
	};

	// 4096 bytes, field order never changes
	struct PakSuperblock {
		u8 magic[8];
		u16 versionMajor;
		u16 versionMinor;
		u32 headerSize;
		u64 featuresRequired;
		u64 featuresOptional;
		u64 fileSize;
		u64 tocOffset;
		u32 tocSizeComp;
		u32 tocSizeRaw;
		u8 tocCodec;
		u8 tocFlags;
		u16 sectionCount;
		u32 assetCount;
		u64 payloadOffset;
		u64 payloadSize;
		u32 regionCount;
		u32 pageCount;
		u32 sectorSize;
		u32 maxAlignment;
		u32 targetId;
		u32 localeId;
		u64 engineHash;
		u64 contentHash;
		u8 reserved[3972];
		u32 crc32c;
	};

	constexpr u8 PAK_MAGIC[8] = {'S', 'L', 'T', 'P', 0x0D, 0x0A, 0x1A, 0x0A};

	constexpr u8 PAK_OVERLAY_MAGIC[8] = {'S', 'L', 'T', 'P', 'K', 'O', 'V', '1'};

	struct PakOverlayFooter {
		u64 exeSize = 0;
		u64 pakOffset = 0;
		u64 pakSize = 0;
		u8 magic[8] = {};
	};
	static_assert(sizeof(PakOverlayFooter) == 32);

	constexpr u16 PAK_VERSION_MAJOR = 1;
	constexpr u16 PAK_VERSION_MINOR = 1;
	constexpr u32 PAK_HEADER_SIZE = 4096;
	constexpr u32 PAK_SECTOR_SIZE = 4096;

	constexpr u32 PAK_DEFAULT_PAGE_SIZE = 128 * 1024;

	// bump when any type's cooked layout changes
	constexpr u16 PAK_RECORD_VERSION = 11;

	constexpr u32 PAK_NO_REGION = 0xFFFFFFFFu;

	constexpr u32 PAK_INVALID_INDEX = 0xFFFFFFFEu;

	static_assert(sizeof(PakSuperblock) == 4096);
	static_assert(alignof(PakSuperblock) == 8);
	static_assert(std::is_trivially_copyable_v<PakSuperblock>);
	static_assert(offsetof(PakSuperblock, magic) == 0);
	static_assert(offsetof(PakSuperblock, versionMajor) == 8);
	static_assert(offsetof(PakSuperblock, versionMinor) == 10);
	static_assert(offsetof(PakSuperblock, headerSize) == 12);
	static_assert(offsetof(PakSuperblock, featuresRequired) == 16);
	static_assert(offsetof(PakSuperblock, featuresOptional) == 24);
	static_assert(offsetof(PakSuperblock, fileSize) == 32);
	static_assert(offsetof(PakSuperblock, tocOffset) == 40);
	static_assert(offsetof(PakSuperblock, tocSizeComp) == 48);
	static_assert(offsetof(PakSuperblock, tocSizeRaw) == 52);
	static_assert(offsetof(PakSuperblock, tocCodec) == 56);
	static_assert(offsetof(PakSuperblock, tocFlags) == 57);
	static_assert(offsetof(PakSuperblock, sectionCount) == 58);
	static_assert(offsetof(PakSuperblock, assetCount) == 60);
	static_assert(offsetof(PakSuperblock, payloadOffset) == 64);
	static_assert(offsetof(PakSuperblock, payloadSize) == 72);
	static_assert(offsetof(PakSuperblock, regionCount) == 80);
	static_assert(offsetof(PakSuperblock, pageCount) == 84);
	static_assert(offsetof(PakSuperblock, sectorSize) == 88);
	static_assert(offsetof(PakSuperblock, maxAlignment) == 92);
	static_assert(offsetof(PakSuperblock, targetId) == 96);
	static_assert(offsetof(PakSuperblock, localeId) == 100);
	static_assert(offsetof(PakSuperblock, engineHash) == 104);
	static_assert(offsetof(PakSuperblock, contentHash) == 112);
	static_assert(offsetof(PakSuperblock, reserved) == 120);
	static_assert(offsetof(PakSuperblock, crc32c) == 4092);
	static_assert(std::has_unique_object_representations_v<PakSuperblock>);

	struct PakSection {
		u32 fourcc;
		u32 flags;
		u64 offset;
		u64 size;
		u32 elemCount;
		u32 elemStride;
	};
	static_assert(sizeof(PakSection) == 32);
	static_assert(offsetof(PakSection, fourcc) == 0);
	static_assert(offsetof(PakSection, flags) == 4);
	static_assert(offsetof(PakSection, offset) == 8);
	static_assert(offsetof(PakSection, size) == 16);
	static_assert(offsetof(PakSection, elemCount) == 24);
	static_assert(offsetof(PakSection, elemStride) == 28);
	static_assert(std::has_unique_object_representations_v<PakSection>);

	constexpr u32 PakFourCC(char a, char b, char c, char d) {
		return static_cast<u32>(static_cast<u8>(a)) | (static_cast<u32>(static_cast<u8>(b)) << 8) |
			   (static_cast<u32>(static_cast<u8>(c)) << 16) | (static_cast<u32>(static_cast<u8>(d)) << 24);
	}

	constexpr u32 PAK_SECT = PakFourCC('S', 'E', 'C', 'T');
	constexpr u32 PAK_TYPE = PakFourCC('T', 'Y', 'P', 'E');
	constexpr u32 PAK_IDX0 = PakFourCC('I', 'D', 'X', '0');
	constexpr u32 PAK_DESC = PakFourCC('D', 'E', 'S', 'C');
	constexpr u32 PAK_STRS = PakFourCC('S', 'T', 'R', 'S');
	constexpr u32 PAK_DEPS = PakFourCC('D', 'E', 'P', 'S');
	constexpr u32 PAK_PLDM = PakFourCC('P', 'L', 'D', 'M');
	constexpr u32 PAK_PAGE = PakFourCC('P', 'A', 'G', 'E');
	constexpr u32 PAK_NAMS = PakFourCC('N', 'A', 'M', 'S');
	constexpr u32 PAK_DOCS = PakFourCC('D', 'O', 'C', 'S');

	struct PakTypeEntry {
		u64 typeNameHash;
		u16 recordVersion;
		u16 flags;
		u32 descStride;
		u64 layoutHash;
	};
	static_assert(sizeof(PakTypeEntry) == 24);
	static_assert(offsetof(PakTypeEntry, typeNameHash) == 0);
	static_assert(offsetof(PakTypeEntry, recordVersion) == 8);
	static_assert(offsetof(PakTypeEntry, flags) == 10);
	static_assert(offsetof(PakTypeEntry, descStride) == 12);
	static_assert(offsetof(PakTypeEntry, layoutHash) == 16);
	static_assert(std::has_unique_object_representations_v<PakTypeEntry>);

	enum class PakAssetFlags : u16 {
		None = 0,
		HasPayload = 1 << 0,
		InlineDesc = 1 << 1,
		Generated = 1 << 2,
		EditorOnly = 1 << 3
	};

	struct PakAssetEntry {
		Guid guid;
		u32 descOffset;
		u32 descSize;
		u32 regionIndex;
		u16 typeIndex;
		u16 flags;
		u64 reserved;
	};
	static_assert(sizeof(PakAssetEntry) == 40);
	static_assert(sizeof(Guid) == 16);
	static_assert(std::is_trivially_copyable_v<PakAssetEntry>);
	static_assert(offsetof(PakAssetEntry, guid) == 0);
	static_assert(offsetof(PakAssetEntry, descOffset) == 16);
	static_assert(offsetof(PakAssetEntry, descSize) == 20);
	static_assert(offsetof(PakAssetEntry, regionIndex) == 24);
	static_assert(offsetof(PakAssetEntry, typeIndex) == 28);
	static_assert(offsetof(PakAssetEntry, flags) == 30);
	static_assert(offsetof(PakAssetEntry, reserved) == 32);
	static_assert(std::has_unique_object_representations_v<PakAssetEntry>);
	static_assert(std::has_unique_object_representations_v<Guid>);

	struct PakRegion {
		u64 sizeRaw;
		u32 firstPage;
		u32 pageCount;
		u32 alignment;
		u8 codec;
		u8 residency;
		u8 flags;
		u8 pad;
	};
	static_assert(sizeof(PakRegion) == 24);
	static_assert(offsetof(PakRegion, sizeRaw) == 0);
	static_assert(offsetof(PakRegion, firstPage) == 8);
	static_assert(offsetof(PakRegion, pageCount) == 12);
	static_assert(offsetof(PakRegion, alignment) == 16);
	static_assert(offsetof(PakRegion, codec) == 20);
	static_assert(offsetof(PakRegion, residency) == 21);
	static_assert(offsetof(PakRegion, flags) == 22);
	static_assert(offsetof(PakRegion, pad) == 23);
	static_assert(std::has_unique_object_representations_v<PakRegion>);

	struct PakPage {
		u64 offset;
		u32 sizeComp;
		u32 sizeRaw;
	};
	static_assert(sizeof(PakPage) == 16);
	static_assert(offsetof(PakPage, offset) == 0);
	static_assert(offsetof(PakPage, sizeComp) == 8);
	static_assert(offsetof(PakPage, sizeRaw) == 12);
	static_assert(std::has_unique_object_representations_v<PakPage>);

	struct PakDepEdge {
		u32 targetAsset;
		u32 flags;
	};
	static_assert(sizeof(PakDepEdge) == 8);
	static_assert(offsetof(PakDepEdge, targetAsset) == 0);
	static_assert(offsetof(PakDepEdge, flags) == 4);
	static_assert(std::has_unique_object_representations_v<PakDepEdge>);

	enum class PakRefKind : u32 {
		Hard = 0,
		Soft = 1,
		Weak = 2,
		EditorOnly = 3
	};

	enum class PakDocumentKind : u32 {
		BootManifest = 1,
		ProjectSettings = 2,
		Icon = 3,
		ThirdPartyNotices = 4,
		ManagedBundle = 5
	};

	constexpr u32 PAK_MANAGED_BUNDLE_MAGIC = PakFourCC('S', 'L', 'M', 'B');

	struct PakManagedBundleHeader {
		u32 magic;
		u32 entryCount;
	};
	static_assert(sizeof(PakManagedBundleHeader) == 8);
	static_assert(offsetof(PakManagedBundleHeader, magic) == 0);
	static_assert(offsetof(PakManagedBundleHeader, entryCount) == 4);
	static_assert(std::has_unique_object_representations_v<PakManagedBundleHeader>);

	struct PakManagedBundleEntry {
		u32 nameOffset;
		u32 nameSize;
		u32 dataOffset;
		u32 rawSize;
		u32 storedSize;
		u8 codec;
		u8 pad[3];
	};
	static_assert(sizeof(PakManagedBundleEntry) == 24);
	static_assert(offsetof(PakManagedBundleEntry, nameOffset) == 0);
	static_assert(offsetof(PakManagedBundleEntry, nameSize) == 4);
	static_assert(offsetof(PakManagedBundleEntry, dataOffset) == 8);
	static_assert(offsetof(PakManagedBundleEntry, rawSize) == 12);
	static_assert(offsetof(PakManagedBundleEntry, storedSize) == 16);
	static_assert(offsetof(PakManagedBundleEntry, codec) == 20);
	static_assert(std::has_unique_object_representations_v<PakManagedBundleEntry>);

	// Icon document = PakIconImage + WindowIconImage texels per image
	struct PakIconImage {
		u32 width;
		u32 height;
	};
	static_assert(sizeof(PakIconImage) == 8);
	static_assert(offsetof(PakIconImage, width) == 0);
	static_assert(offsetof(PakIconImage, height) == 4);
	static_assert(std::has_unique_object_representations_v<PakIconImage>);

	struct PakDocumentEntry {
		u32 kind;
		u32 reserved;
		u32 offset;
		u32 size;
	};
	static_assert(sizeof(PakDocumentEntry) == 16);
	static_assert(offsetof(PakDocumentEntry, kind) == 0);
	static_assert(offsetof(PakDocumentEntry, reserved) == 4);
	static_assert(offsetof(PakDocumentEntry, offset) == 8);
	static_assert(offsetof(PakDocumentEntry, size) == 12);
	static_assert(std::has_unique_object_representations_v<PakDocumentEntry>);

	namespace PakCRCDetail {

		constexpr std::array<std::array<u32, 256>, 8> MakeCrc32cTables() {
			std::array<std::array<u32, 256>, 8> table{};
			for (u32 i = 0; i < 256; ++i) {
				u32 c = i;
				for (int bit = 0; bit < 8; ++bit) {
					c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1u)));
				}
				table[0][i] = c;
			}
			for (u32 i = 0; i < 256; ++i) {
				for (size_t k = 1; k < 8; ++k) {
					u32 prev = table[k - 1][i];
					table[k][i] = (prev >> 8) ^ table[0][prev & 0xFFu];
				}
			}
			return table;
		}

		inline constexpr std::array<std::array<u32, 256>, 8> CRC32CTables = MakeCrc32cTables();

	} // namespace PakCRCDetail

	constexpr u32 PakCrc32c(const void* data, size_t size, u32 seed = 0) {
		const u8* bytes = static_cast<const u8*>(data);
		u32 crc = ~seed;
		while (size >= 8) {
			u64 word = static_cast<u64>(bytes[0]) | (static_cast<u64>(bytes[1]) << 8) |
					   (static_cast<u64>(bytes[2]) << 16) | (static_cast<u64>(bytes[3]) << 24) |
					   (static_cast<u64>(bytes[4]) << 32) | (static_cast<u64>(bytes[5]) << 40) |
					   (static_cast<u64>(bytes[6]) << 48) | (static_cast<u64>(bytes[7]) << 56);
			word ^= static_cast<u64>(crc);
			crc = PakCRCDetail::CRC32CTables[7][word & 0xFFu] ^ PakCRCDetail::CRC32CTables[6][(word >> 8) & 0xFFu] ^
				  PakCRCDetail::CRC32CTables[5][(word >> 16) & 0xFFu] ^
				  PakCRCDetail::CRC32CTables[4][(word >> 24) & 0xFFu] ^
				  PakCRCDetail::CRC32CTables[3][(word >> 32) & 0xFFu] ^
				  PakCRCDetail::CRC32CTables[2][(word >> 40) & 0xFFu] ^
				  PakCRCDetail::CRC32CTables[1][(word >> 48) & 0xFFu] ^
				  PakCRCDetail::CRC32CTables[0][(word >> 56) & 0xFFu];
			bytes += 8;
			size -= 8;
		}
		while (size-- > 0) {
			crc = (crc >> 8) ^ PakCRCDetail::CRC32CTables[0][(crc ^ *bytes++) & 0xFFu];
		}
		return ~crc;
	}

	constexpr u64 PakContentHash(const void* data, size_t size, u64 seed = 0xCBF29CE484222325ull) {
		const u8* bytes = static_cast<const u8*>(data);
		u64 hash = seed;
		for (size_t i = 0; i < size; ++i) {
			hash ^= bytes[i];
			hash *= 0x100000001B3ull;
		}
		return hash;
	}

	// non owning
	struct PakStringTable {
		const u32* offsets = nullptr;
		const u8* blob = nullptr;
		u32 count = 0;
		u32 blobSize = 0;

		std::string_view Get(u32 index) const {
			if (!offsets || !blob || index >= count) {
				return {};
			}
			u32 begin = offsets[index];
			u32 end = offsets[index + 1];
			if (begin > end || end > blobSize) {
				return {};
			}
			return std::string_view(reinterpret_cast<const char*>(blob) + begin, end - begin);
		}
	};

	class PakStringPool {
	public:
		PakStringPool() = default;

		u32 Intern(std::string_view text) {
			u64 hash = PakContentHash(text.data(), text.size());
			for (u32 i = 0; i < static_cast<u32>(_hashes.size()); ++i) {
				if (_hashes[i] == hash && _texts[i] == text) {
					return i;
				}
			}
			_texts.emplace_back(text);
			_hashes.push_back(hash);
			return static_cast<u32>(_texts.size() - 1);
		}

		u32 Count() const { return static_cast<u32>(_texts.size()); }

		const Vector<str>& Texts() const { return _texts; }
	private:
		Vector<str> _texts{GetMemoryResource(MemTag::Editor)};
		Vector<u64> _hashes{GetMemoryResource(MemTag::Editor)};
	};

	// count -> count + 1 offsets -> bytes

	inline void PakWriteStringBlob(Vector<u8>& out, const Vector<str>& texts) {
		u32 count = static_cast<u32>(texts.size());
		size_t at = out.size();
		size_t offsetBytes = sizeof(u32) + (static_cast<size_t>(count) + 1) * sizeof(u32);
		size_t blobBytes = 0;
		for (const str& text : texts) {
			blobBytes += text.size();
		}
		out.resize(at + offsetBytes + blobBytes);

		u8* base = out.data() + at;
		std::memcpy(base, &count, sizeof(u32));
		u8* blob = base + offsetBytes;
		u32 cursor = 0;
		for (u32 i = 0; i < count; ++i) {
			std::memcpy(base + sizeof(u32) + static_cast<size_t>(i) * sizeof(u32), &cursor, sizeof(u32));
			if (!texts[i].empty()) {
				std::memcpy(blob + cursor, texts[i].data(), texts[i].size());
			}
			cursor += static_cast<u32>(texts[i].size());
		}
		std::memcpy(base + sizeof(u32) + static_cast<size_t>(count) * sizeof(u32), &cursor, sizeof(u32));
	}

	// untrusted, count checked before offset forms
	inline bool PakReadStringBlob(const u8* data, size_t size, PakStringTable& out) {
		out = PakStringTable{};
		if (!data || size < sizeof(u32)) {
			return false;
		}
		u32 count = 0;
		std::memcpy(&count, data, sizeof(u32));
		size_t offsetBytes = sizeof(u32) + (static_cast<size_t>(count) + 1) * sizeof(u32);
		if (size < offsetBytes) {
			return false;
		}
		out.count = count;
		out.offsets = reinterpret_cast<const u32*>(data + sizeof(u32));
		out.blob = data + offsetBytes;
		out.blobSize = static_cast<u32>(size - offsetBytes);
		return true;
	}

	class FileReader;

	struct PakPayloadLocator {
		// valid during LoadCooked only. Reopen to read later
		const FileReader* file = nullptr;
		u64 offset = 0;
		u64 size = 0;
	};

	class CookWriter {
	public:
		CookWriter(Vector<u8>& out, PakStringPool& strings) : _out(&out), _strings(&strings) {}

		template <typename T>
		void Write(const T& value) {
			static_assert(std::is_trivially_copyable_v<T>, "a cooked record holds bytes (not objects)");
			WriteBytes(&value, sizeof(T));
		}

		void WriteBytes(const void* data, size_t size) {
			if (size == 0) {
				return;
			}
			size_t at = _out->size();
			_out->resize(at + size);
			std::memcpy(_out->data() + at, data, size);
		}

		void AlignTo(size_t alignment) {
			if (alignment <= 1) {
				return;
			}
			size_t at = _out->size();
			size_t padded = (at + alignment - 1) / alignment * alignment;
			_out->resize(padded, 0);
		}

		// only way a string reaches a package
		void WriteString(std::string_view text) { Write(_strings->Intern(text)); }

		void WriteAssetRef(const Guid& guid) {
			Write(guid);
			if (guid.IsValid()) {
				_references.push_back(guid);
			}
		}

		void NoteAssetRef(const Guid& guid) {
			if (guid.IsValid()) {
				_references.push_back(guid);
			}
		}

		const Vector<Guid>& GetReferences() const { return _references; }

		void SetResidency(PakResidency residency) { _residency = residency; }
		PakResidency GetResidency() const { return _residency; }

		size_t Size() const { return _out->size(); }
	private:
		Vector<u8>* _out;
		PakStringPool* _strings;
		Vector<Guid> _references{GetMemoryResource(MemTag::Asset)};
		PakResidency _residency = PakResidency::Hot;
	};

	class CookReader {
	public:
		CookReader() = default;
		CookReader(const u8* data, size_t size, const PakStringTable& strings = {}, const PakPayloadLocator* locator = nullptr)
			: _data(data), _size(size), _strings(strings), _locator(locator) {}

		template <typename T>
		bool Read(T& out) {
			static_assert(std::is_trivially_copyable_v<T>, "a cooked record holds bytes (not objects)");
			return ReadBytes(&out, sizeof(T));
		}

		bool ReadBytes(void* out, size_t size) {
			if (_failed) {
				return false;
			}
			if (size > _size - _at) {
				_failed = true;
				return false;
			}
			if (size > 0) {
				std::memcpy(out, _data + _at, size);
			}
			_at += size;
			return true;
		}

		// valid as long as the mount, null on a short range
		const u8* ReadSpan(size_t size) {
			if (_failed || size > _size - _at) {
				_failed = true;
				return nullptr;
			}
			const u8* at = _data + _at;
			_at += size;
			return at;
		}

		template <typename T>
		bool ReadInto(Vector<T>& out, size_t count) {
			static_assert(std::is_trivially_copyable_v<T>, "a cooked record holds bytes (not objects)");
			size_t size = count * sizeof(T);
			const u8* bytes = ReadSpan(size);
			if (_failed) {
				return false;
			}
			out.resize(count);
			if (size > 0) {
				std::memcpy(out.data(), bytes, size);
			}
			return true;
		}

		bool Skip(size_t size) {
			if (_failed) {
				return false;
			}
			if (size > _size - _at) {
				_failed = true;
				return false;
			}
			_at += size;
			return true;
		}

		std::string_view ReadString() {
			u32 index = 0;
			if (!Read(index)) {
				return {};
			}
			if (index >= _strings.count) {
				_failed = true;
				return {};
			}
			return _strings.Get(index);
		}

		size_t Remaining() const { return _failed ? 0 : _size - _at; }
		size_t Size() const { return _size; }
		bool Failed() const { return _failed; }

		const PakPayloadLocator* GetLocator() const { return _locator; }
	private:
		const u8* _data = nullptr;
		size_t _size = 0;
		size_t _at = 0;
		bool _failed = false;
		PakStringTable _strings;
		const PakPayloadLocator* _locator = nullptr;
	};

} // namespace Shalltear
