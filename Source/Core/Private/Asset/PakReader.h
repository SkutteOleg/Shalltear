#pragma once

#include "Shalltear/Core/Asset/PakFormat.h"
#include "Shalltear/Core/BuildInfo.h"

#include "Asset/LZ4Decode.h"

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Types.h"

#include <format>
#include <string_view>

namespace Shalltear {

	class PakReader {
	public:
		PakReader() = default;
		~PakReader() { Unmount(); }

		PakReader(const PakReader&) = delete;
		PakReader& operator=(const PakReader&) = delete;

		bool Mount(const Path& physicalPath, str& outError) {
			Unmount();

			FileReader file;
			if (!file.Open(physicalPath, FileSharing::ReadDelete)) {
				outError = std::format("'{}' couldn't be opened for reading", physicalPath.ToString());
				return false;
			}
			const u64 totalFileSize = file.GetSize();
			if (totalFileSize < PAK_HEADER_SIZE) {
				outError = std::format("'{}' is {} bytes (smaller than a superblock)", physicalPath.ToString(), totalFileSize);
				return false;
			}

			u64 baseOffset = 0;
			u64 fileSize = totalFileSize;
			if (totalFileSize >= PAK_HEADER_SIZE + sizeof(PakOverlayFooter)) {
				PakOverlayFooter footer{};
				if (file.ReadAt(totalFileSize - sizeof(PakOverlayFooter),
								std::span<u8>(reinterpret_cast<u8*>(&footer), sizeof(PakOverlayFooter))) &&
					std::memcmp(footer.magic, PAK_OVERLAY_MAGIC, sizeof(PAK_OVERLAY_MAGIC)) == 0 &&
					footer.pakOffset <= totalFileSize - sizeof(PakOverlayFooter) &&
					footer.pakSize == (totalFileSize - sizeof(PakOverlayFooter)) - footer.pakOffset &&
					footer.pakSize >= PAK_HEADER_SIZE) {
					baseOffset = footer.pakOffset;
					fileSize = footer.pakSize;
				}
			}

			// validate superblock before any of its offsets sizes a read
			Buffer header(PAK_HEADER_SIZE, MemTag::Core);
			if (!header || !file.ReadAt(baseOffset, std::span<u8>(header.data, header.size))) {
				outError = std::format("'{}' couldn't be read", physicalPath.ToString());
				return false;
			}
			_data = header.data;
			_residentSize = header.size;
			_size = fileSize;
			if (!ValidateSuperblock(physicalPath.ToString(), outError)) {
				Unmount();
				return false;
			}
			const u64 residentEnd = GetSuperblock().tocOffset + GetSuperblock().tocSizeRaw;
			Unmount();

			Buffer resident(static_cast<size_t>(residentEnd), MemTag::Core);
			if (!resident || !file.ReadAt(baseOffset, std::span<u8>(resident.data, resident.size))) {
				outError = std::format("'{}' couldn't be read", physicalPath.ToString());
				return false;
			}
			if (!MountResident(resident.data, resident.size, fileSize, physicalPath.ToString(), outError)) {
				return false;
			}
			_baseOffset = baseOffset;
			_resident = std::move(resident);
			_file = std::move(file);
			return true;
		}

		// caller owns data, must outlive the mount
		bool MountFromMemory(const u8* data, size_t size, std::string_view name, str& outError) {
			Unmount();

			if (!data || size < PAK_HEADER_SIZE) {
				outError = std::format("'{}' is {} bytes (smaller than a superblock)", name, size);
				return false;
			}
			return MountResident(data, size, size, name, outError);
		}

		void Unmount() {
			_file = FileReader();
			_resident.Release();
			_data = nullptr;
			_residentSize = 0;
			_baseOffset = 0;
			_size = 0;
			_mounted = false;
			_assets = nullptr;
			_assetCount = 0;
			_types = nullptr;
			_typeCount = 0;
			_regions = nullptr;
			_regionCount = 0;
			_pages = nullptr;
			_pageCount = 0;
			_edgeBegin = nullptr;
			_edges = nullptr;
			_edgeCount = 0;
			_desc = nullptr;
			_descSize = 0;
			_nameOffsets = nullptr;
			_nameBlob = nullptr;
			_nameBlobSize = 0;
			_strings = PakStringTable{};
			_documents = nullptr;
			_documentCount = 0;
			_documentSection = nullptr;
		}

		bool IsMounted() const { return _mounted; }

		const FileReader& GetFile() const { return _file; }

		const PakSuperblock& GetSuperblock() const { return *reinterpret_cast<const PakSuperblock*>(_data); }

		size_t GetResidentSize() const { return _residentSize; }

		u32 GetAssetCount() const { return _assetCount; }
		u32 GetRegionCount() const { return _regionCount; }
		u32 GetPageCount() const { return _pageCount; }

		u32 FindAsset(const Guid& guid) const {
			u32 low = 0;
			u32 high = GetAssetCount();
			while (low < high) {
				u32 mid = low + (high - low) / 2;
				const Guid& probe = _assets[mid].guid;
				if (probe < guid) {
					low = mid + 1;
				} else if (guid < probe) {
					high = mid;
				} else {
					return mid;
				}
			}
			return PAK_INVALID_INDEX;
		}

		const PakAssetEntry* GetAsset(u32 index) const {
			return index < GetAssetCount() ? &_assets[index] : nullptr;
		}

		const PakTypeEntry* GetType(u16 index) const { return index < _typeCount ? &_types[index] : nullptr; }

		const PakRegion* GetRegion(u32 index) const { return index < _regionCount ? &_regions[index] : nullptr; }

		const PakPage* GetPage(u32 index) const { return index < _pageCount ? &_pages[index] : nullptr; }

		CookReader GetDescriptor(u32 assetIndex) const {
			const PakAssetEntry* asset = GetAsset(assetIndex);
			if (!asset) {
				return CookReader();
			}
			if (asset->descOffset > _descSize || asset->descSize > _descSize - asset->descOffset) {
				return CookReader();
			}
			return CookReader(_desc + asset->descOffset, asset->descSize, _strings);
		}

		bool ReadRegion(u32 regionIndex, Buffer& out, str& outError) const {
			const PakRegion* region = GetRegion(regionIndex);
			if (!region) {
				outError = std::format("region {} is out of range", regionIndex);
				return false;
			}
			if (region->codec != static_cast<u8>(PakCodec::None) && region->codec != static_cast<u8>(PakCodec::LZ4)) {
				outError = std::format("region {} declares codec {} (unknown to this build)",
									   regionIndex, region->codec);
				return false;
			}
			// 2 passes, no allocation sized by an uncapped disk value
			u64 decodedTotal = 0;
			u32 largestCompressed = 0;
			for (u32 i = 0; i < region->pageCount; ++i) {
				const PakPage* page = GetPage(region->firstPage + i);
				if (!page) {
					outError = std::format("region {} uses page {} (out of range)", regionIndex,
										   region->firstPage + i);
					return false;
				}
				if (page->offset > _size || page->sizeComp > _size - page->offset) {
					outError = std::format("page {} doesn't fit the file", region->firstPage + i);
					return false;
				}
				const bool compressed = page->sizeComp != page->sizeRaw;
				if (compressed && region->codec == static_cast<u8>(PakCodec::None)) {
					outError = std::format("page {} is codec None and its sizes disagree", region->firstPage + i);
					return false;
				}
				if (page->sizeComp > page->sizeRaw) {
					outError = std::format("page {} says {} compressed bytes for {} raw ones",
										   region->firstPage + i, page->sizeComp, page->sizeRaw);
					return false;
				}
				if (compressed && static_cast<u64>(page->sizeRaw) > static_cast<u64>(page->sizeComp) * LZ4_MAX_RATIO) {
					outError = std::format("page {} says {} bytes out of {}, more than the codec can produce",
										   region->firstPage + i, page->sizeRaw, page->sizeComp);
					return false;
				}
				if (compressed && !IsResident(page->offset, page->sizeComp) && page->sizeComp > largestCompressed) {
					largestCompressed = page->sizeComp;
				}
				decodedTotal += page->sizeRaw;
			}
			if (decodedTotal != region->sizeRaw) {
				outError = std::format("region {} says {} bytes and its pages hold {}", regionIndex,
									   region->sizeRaw, decodedTotal);
				return false;
			}

			out.Allocate(static_cast<size_t>(region->sizeRaw), MemTag::Asset);
			Buffer scratch;
			if (largestCompressed > 0) {
				scratch.Allocate(largestCompressed, MemTag::Asset);
			}
			size_t written = 0;
			for (u32 i = 0; i < region->pageCount;) {
				const PakPage* page = GetPage(region->firstPage + i);
				if (page->sizeRaw > region->sizeRaw - written) {
					outError = std::format("page {} doesn't fit the region", region->firstPage + i);
					return false;
				}
				if (page->sizeComp != page->sizeRaw) {
					const u8* compressed = _data + page->offset;
					if (!IsResident(page->offset, page->sizeComp)) {
						if (!ReadBytes(page->offset, scratch.data, page->sizeComp)) {
							outError = std::format("page {} couldn't be read", region->firstPage + i);
							return false;
						}
						compressed = scratch.data;
					}
					if (!LZ4::Decompress(compressed, page->sizeComp, out.data + written, page->sizeRaw)) {
						outError = std::format("page {} doesn't decode", region->firstPage + i);
						return false;
					}
					written += page->sizeRaw;
					++i;
					continue;
				}
				// sizeComp == sizeRaw = stored
				u64 run = page->sizeRaw;
				u32 next = i + 1;
				while (next < region->pageCount) {
					const PakPage* following = GetPage(region->firstPage + next);
					if (following->sizeComp != following->sizeRaw || following->offset != page->offset + run ||
						following->sizeRaw > region->sizeRaw - written - run) {
						break;
					}
					run += following->sizeRaw;
					++next;
				}
				if (!ReadBytes(page->offset, out.data + written, run)) {
					outError = std::format("pages {} to {} couldn't be read", region->firstPage + i, region->firstPage + next - 1);
					return false;
				}
				written += static_cast<size_t>(run);
				i = next;
			}
			return written == region->sizeRaw;
		}

		bool LocateRegion(u32 regionIndex, u64& outOffset, u64& outSize, str& outError) const {
			const PakRegion* region = GetRegion(regionIndex);
			if (!region) {
				outError = std::format("region {} is out of range", regionIndex);
				return false;
			}
			if (region->codec != static_cast<u8>(PakCodec::None)) {
				outError = std::format("region {} is streamed and declares codec {} (streamed regions should be stored)", regionIndex, region->codec);
				return false;
			}
			u64 at = 0;
			u64 total = 0;
			for (u32 i = 0; i < region->pageCount; ++i) {
				const PakPage* page = GetPage(region->firstPage + i);
				if (!page || page->sizeComp != page->sizeRaw || page->offset > _size || page->sizeRaw > _size - page->offset) {
					outError = std::format("page {} of streamed region {} isn't a stored page inside the file", region->firstPage + i, regionIndex);
					return false;
				}
				if (i == 0) {
					at = page->offset;
				} else if (page->offset != at + total) {
					outError = std::format("streamed region {} isn't one contiguous run at page {}", regionIndex, region->firstPage + i);
					return false;
				}
				total += page->sizeRaw;
			}
			if (total != region->sizeRaw) {
				outError = std::format("region {} says {} bytes and its pages hold {}", regionIndex, region->sizeRaw, total);
				return false;
			}
			outOffset = _baseOffset + at;
			outSize = total;
			return true;
		}

		CookReader MakePayloadReader(const Buffer& payload) const {
			return CookReader(payload.data, payload.size);
		}

		// diagnostic only, empty when a profile stripped NAMS
		std::string_view GetName(u32 assetIndex) const {
			if (!_nameOffsets || assetIndex >= GetAssetCount()) {
				return {};
			}
			u32 begin = _nameOffsets[assetIndex];
			u32 end = _nameOffsets[assetIndex + 1];
			if (begin > end || end > _nameBlobSize) {
				return {};
			}
			return std::string_view(reinterpret_cast<const char*>(_nameBlob) + begin, end - begin);
		}

		// not NUL terminated, valid for the mount's life
		std::string_view GetString(u32 stringIndex) const { return _strings.Get(stringIndex); }

		u32 GetStringCount() const { return _strings.count; }
		u32 GetTypeCount() const { return _typeCount; }

		bool GetDependencies(u32 assetIndex, const PakDepEdge*& outEdges, u32& outCount) const {
			outEdges = nullptr;
			outCount = 0;
			if (!_edgeBegin || assetIndex >= GetAssetCount()) {
				return false;
			}
			u32 begin = _edgeBegin[assetIndex];
			u32 end = _edgeBegin[assetIndex + 1];
			if (begin > end || end > _edgeCount) {
				return false;
			}
			outEdges = _edges + begin;
			outCount = end - begin;
			return true;
		}

		std::string_view GetDocument(PakDocumentKind kind) const {
			for (u32 i = 0; i < _documentCount; ++i) {
				if (_documents[i].kind == static_cast<u32>(kind)) {
					return std::string_view(reinterpret_cast<const char*>(_documentSection) + _documents[i].offset,
											_documents[i].size);
				}
			}
			return {};
		}
	private:
		bool MountResident(const u8* data, size_t residentSize, u64 fileSize, std::string_view name, str& outError) {
			if (reinterpret_cast<uintptr_t>(data) % 8 != 0) {
				outError = std::format("'{}' is at an address that isn't 8-aligned", name);
				return false;
			}

			_data = data;
			_residentSize = residentSize;
			_size = fileSize;
			if (!ValidateSuperblock(name, outError) || !ValidateFeatures(outError) || !ValidateSections(outError)) {
				Unmount();
				return false;
			}
			_mounted = true;
			return true;
		}

		bool IsResident(u64 offset, u64 size) const {
			return offset <= _residentSize && size <= _residentSize - offset;
		}

		bool ReadBytes(u64 offset, u8* out, u64 size) const {
			if (IsResident(offset, size)) {
				if (size > 0) {
					std::memcpy(out, _data + offset, static_cast<size_t>(size));
				}
				return true;
			}
			return _file.IsOpen() && _file.ReadAt(_baseOffset + offset, std::span<u8>(out, static_cast<size_t>(size)));
		}

		bool ValidateSuperblock(std::string_view name, str& outError) {
			const PakSuperblock& sb = GetSuperblock();
			if (std::memcmp(sb.magic, PAK_MAGIC, sizeof(PAK_MAGIC)) != 0) {
				outError = std::format("'{}' isn't a .slpak (magic doesn't match)", name);
				return false;
			}
			if (sb.headerSize != PAK_HEADER_SIZE) {
				outError = std::format("superblock declares a {}-byte header (this build reads {})",
									   sb.headerSize, PAK_HEADER_SIZE);
				return false;
			}
			if (PakCrc32c(_data, 4092) != sb.crc32c) {
				outError = std::format("'{}' has a corrupt superblock (crc32c mismatch)", name);
				return false;
			}
			if (sb.versionMajor != PAK_VERSION_MAJOR) {
				outError = std::format("package format version {}.{} (expected {}.x)", sb.versionMajor,
									   sb.versionMinor, PAK_VERSION_MAJOR);
				return false;
			}
			if (sb.fileSize != _size) {
				outError = std::format("'{}' says it is {} bytes and is {} (the build that wrote it was likely interrupted)",
									   name, sb.fileSize, _size);
				return false;
			}
			if (sb.engineHash != BuildInfo::GetEngineHash()) {
				outError = std::format("'{}' was cooked by engine build {:016x} and this executable is {:016x}. Both "
									   "stamps captured at CMake CONFIGURE time -> whichever side is behind "
									   "needs reconfiguring (building it again should re-stamp the same value)",
									   name, sb.engineHash, BuildInfo::GetEngineHash());
				return false;
			}
			if (sb.tocOffset % 8 != 0) {
				outError = std::format("the TOC starts at offset {} (not 8-aligned)", sb.tocOffset);
				return false;
			}
			if (!FitsInFile(sb.tocOffset, sb.tocSizeComp) || !FitsInFile(sb.payloadOffset, sb.payloadSize)) {
				outError = std::format("'{}' has a section that doesn't fit inside it", name);
				return false;
			}
			if (sb.tocCodec != static_cast<u8>(PakCodec::None)) {
				outError = std::format("the TOC declares codec {} (unknown to this build)", sb.tocCodec);
				return false;
			}
			if (sb.tocSizeComp != sb.tocSizeRaw) {
				outError = str("the TOC is codec None and its sizes disagree");
				return false;
			}
			return true;
		}

		bool ValidateFeatures(str& outError) {
			u64 unknown = GetSuperblock().featuresRequired & ~PAK_SUPPORTED_REQUIRED;
			if (unknown == 0) {
				return true;
			}
			outError = std::format("this package requires '{}' (not implemented in this build)",
								   NameRequiredFeature(unknown));
			return false;
		}

		static const char* NameRequiredFeature(u64 bits) {
			if (bits & static_cast<u64>(PakFeatureRequired::MphfIndex)) {
				return "MphfIndex";
			}
			if (bits & static_cast<u64>(PakFeatureRequired::ExternalPages)) {
				return "ExternalPages";
			}
			if (bits & static_cast<u64>(PakFeatureRequired::EncryptedPages)) {
				return "EncryptedPages";
			}
			if (bits & static_cast<u64>(PakFeatureRequired::VariableSizePages)) {
				return "VariableSizePages";
			}
			return "an unnamed required feature";
		}

		bool ValidateSections(str& outError) {
			const PakSuperblock& sb = GetSuperblock();
			if (!IsResident(sb.tocOffset, sb.tocSizeRaw)) {
				outError = std::format("the TOC at offset {} runs past the {} bytes this mount holds", sb.tocOffset, _residentSize);
				return false;
			}
			const u8* toc = _data + sb.tocOffset;
			size_t tocSize = sb.tocSizeRaw;

			if (sb.sectionCount == 0 || static_cast<size_t>(sb.sectionCount) * sizeof(PakSection) > tocSize) {
				outError = std::format("the TOC declares {} section(s) (don't fit in {} bytes)",
									   sb.sectionCount, tocSize);
				return false;
			}
			const PakSection* sections = reinterpret_cast<const PakSection*>(toc);

			for (u16 i = 0; i < sb.sectionCount; ++i) {
				const PakSection& section = sections[i];
				if (section.offset > tocSize || section.size > tocSize - section.offset) {
					outError = std::format("section {} of the TOC doesn't fit inside it", i);
					return false;
				}
				if (section.elemStride != 0 &&
					static_cast<u64>(section.elemCount) * section.elemStride > section.size) {
					outError = std::format("section {} says {} element(s) of {} bytes in {}", i, section.elemCount,
										   section.elemStride, section.size);
					return false;
				}
			}

			auto find = [&](u32 fourcc, const PakSection*& out) {
				for (u16 i = 0; i < sb.sectionCount; ++i) {
					if (sections[i].fourcc == fourcc) {
						out = &sections[i];
						return true;
					}
				}
				out = nullptr;
				return false;
			};

			const PakSection* idx0 = nullptr;
			const PakSection* type = nullptr;
			const PakSection* desc = nullptr;
			const PakSection* pldm = nullptr;
			const PakSection* page = nullptr;
			const PakSection* deps = nullptr;
			if (!find(PAK_IDX0, idx0) || !find(PAK_TYPE, type) || !find(PAK_DESC, desc) || !find(PAK_PLDM, pldm) ||
				!find(PAK_PAGE, page) || !find(PAK_DEPS, deps)) {
				outError = str("the TOC is missing a required section");
				return false;
			}
			for (const PakSection* section : {idx0, type, desc, pldm, page, deps}) {
				if (section->offset % 8 != 0) {
					outError = std::format("a TOC section starts at offset {} (not 8-aligned)",
										   section->offset);
					return false;
				}
			}
			if (idx0->elemCount != sb.assetCount || idx0->elemStride != sizeof(PakAssetEntry) ||
				pldm->elemCount != sb.regionCount || pldm->elemStride != sizeof(PakRegion) ||
				page->elemCount != sb.pageCount || page->elemStride != sizeof(PakPage) ||
				type->elemStride != sizeof(PakTypeEntry)) {
				outError = str("a section's element count or stride disagrees with the superblock");
				return false;
			}

			_assets = reinterpret_cast<const PakAssetEntry*>(toc + idx0->offset);
			_assetCount = sb.assetCount;
			_types = reinterpret_cast<const PakTypeEntry*>(toc + type->offset);
			_typeCount = type->elemCount;
			_regions = reinterpret_cast<const PakRegion*>(toc + pldm->offset);
			_regionCount = sb.regionCount;
			_pages = reinterpret_cast<const PakPage*>(toc + page->offset);
			_pageCount = sb.pageCount;
			_desc = toc + desc->offset;
			_descSize = static_cast<u32>(desc->size);

			size_t beginBytes = (static_cast<size_t>(sb.assetCount) + 1) * sizeof(u32);
			if (deps->size < beginBytes || (deps->size - beginBytes) % sizeof(PakDepEdge) != 0) {
				outError = str("DEPS isn't a CSR of assetCount + 1 offsets followed by whole edges");
				return false;
			}
			_edgeBegin = reinterpret_cast<const u32*>(toc + deps->offset);
			_edges = reinterpret_cast<const PakDepEdge*>(toc + deps->offset + beginBytes);
			_edgeCount = static_cast<u32>((deps->size - beginBytes) / sizeof(PakDepEdge));

			const PakSection* nams = nullptr;
			if ((sb.featuresOptional & static_cast<u64>(PakFeatureOptional::DebugNames)) && find(PAK_NAMS, nams)) {
				size_t offsetBytes = (static_cast<size_t>(sb.assetCount) + 1) * sizeof(u32);
				if (nams->size < offsetBytes) {
					outError = str("NAMS is shorter than its offset array");
					return false;
				}
				_nameOffsets = reinterpret_cast<const u32*>(toc + nams->offset);
				_nameBlob = toc + nams->offset + offsetBytes;
				_nameBlobSize = static_cast<u32>(nams->size - offsetBytes);
			}

			const PakSection* strs = nullptr;
			if ((sb.featuresOptional & static_cast<u64>(PakFeatureOptional::StringTable)) && find(PAK_STRS, strs)) {
				if (!PakReadStringBlob(toc + strs->offset, strs->size, _strings)) {
					outError = std::format("STRS is malformed: {} bytes don't hold the table they declare", strs->size);
					return false;
				}
			}

			const PakSection* docs = nullptr;
			if ((sb.featuresOptional & static_cast<u64>(PakFeatureOptional::Documents)) && find(PAK_DOCS, docs) &&
				!ResolveDocuments(toc, *docs, outError)) {
				return false;
			}

			for (u32 i = 1; i < sb.assetCount; ++i) {
				if (!(_assets[i - 1].guid < _assets[i].guid)) {
					outError = std::format("IDX0 isn't strictly sorted at entry {}", i);
					return false;
				}
			}

			for (u32 i = 0; i < sb.assetCount; ++i) {
				const PakAssetEntry& entry = _assets[i];
				if (entry.typeIndex >= _typeCount) {
					outError = std::format("asset {} uses type {}, but there are only {}", i, entry.typeIndex,
										   _typeCount);
					return false;
				}
				if (entry.descOffset > _descSize || entry.descSize > _descSize - entry.descOffset) {
					outError = std::format("asset {}'s descriptor doesn't fit in DESC", i);
					return false;
				}
				if (entry.regionIndex != PAK_NO_REGION) {
					const PakRegion* region = GetRegion(entry.regionIndex);
					if (!region) {
						outError = std::format("asset {} uses region {}, but there are only {}", i, entry.regionIndex,
											   sb.regionCount);
						return false;
					}
					if (static_cast<u64>(region->firstPage) + region->pageCount > sb.pageCount) {
						outError = std::format("region {} runs past the page table", entry.regionIndex);
						return false;
					}
				}
			}

			for (u32 i = 0; i < _edgeCount; ++i) {
				if (_edges[i].targetAsset >= sb.assetCount) {
					outError = std::format("dependency edge {} points at asset {} (only {} exist)", i,
										   _edges[i].targetAsset, sb.assetCount);
					return false;
				}
			}
			if (sb.assetCount > 0 && _edgeBegin[sb.assetCount] != _edgeCount) {
				outError = str("the CSR sentinel doesn't mark the end of the edge array");
				return false;
			}
			return true;
		}

		bool ResolveDocuments(const u8* toc, const PakSection& docs, str& outError) {
			if (docs.offset % 8 != 0 || docs.elemStride != sizeof(PakDocumentEntry)) {
				outError = str("DOCS isn't an 8-aligned array of document entries");
				return false;
			}
			const PakDocumentEntry* entries = reinterpret_cast<const PakDocumentEntry*>(toc + docs.offset);
			const u64 entryBytes = static_cast<u64>(docs.elemCount) * sizeof(PakDocumentEntry);
			for (u32 i = 0; i < docs.elemCount; ++i) {
				const PakDocumentEntry& entry = entries[i];
				if (i > 0 && entry.kind <= entries[i - 1].kind) {
					outError = std::format("DOCS isn't strictly ascending by kind at entry {}", i);
					return false;
				}
				if (entry.offset < entryBytes || entry.offset > docs.size || entry.size > docs.size - entry.offset) {
					outError = std::format("document {} of DOCS doesn't fit inside the section", i);
					return false;
				}
			}
			_documents = entries;
			_documentCount = docs.elemCount;
			_documentSection = toc + docs.offset;
			return true;
		}

		bool FitsInFile(u64 offset, u64 size) const {
			return offset <= _size && size <= _size - offset;
		}

		// _data owns the bytes only when Mount(Path) read them
		const u8* _data = nullptr;
		size_t _residentSize = 0;
		u64 _baseOffset = 0;
		u64 _size = 0;
		Buffer _resident;
		FileReader _file;
		bool _mounted = false;

		const PakAssetEntry* _assets = nullptr;
		u32 _assetCount = 0;
		const PakTypeEntry* _types = nullptr;
		u32 _typeCount = 0;
		const PakRegion* _regions = nullptr;
		u32 _regionCount = 0;
		const PakPage* _pages = nullptr;
		u32 _pageCount = 0;
		const u8* _desc = nullptr;
		u32 _descSize = 0;
		const u32* _edgeBegin = nullptr;
		const PakDepEdge* _edges = nullptr;
		u32 _edgeCount = 0;
		const u32* _nameOffsets = nullptr;
		const u8* _nameBlob = nullptr;
		u32 _nameBlobSize = 0;
		PakStringTable _strings;
		const PakDocumentEntry* _documents = nullptr;
		u32 _documentCount = 0;
		const u8* _documentSection = nullptr;
	};

} // namespace Shalltear
