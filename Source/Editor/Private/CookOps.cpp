#include "Shalltear/Editor/CookOps.h"

#include "Shalltear/Core/Asset/Asset.h"
#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Asset/AssetImporter.h"
#include "Shalltear/Core/Assert.h"
#include "Shalltear/Core/BuildInfo.h"
#include "Shalltear/Core/Graphics/MaterialAsset.h"
#include "Shalltear/Core/Graphics/ShaderAsset.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Serialization/JSON.h"

#include "Shalltear/Editor/BuildPipeline.h"

#include "LZ4Encode.h"

#include <algorithm>
#include <deque>
#include <format>
#include <map>

namespace Shalltear {

	namespace {

		struct SectionBuild {
			u32 fourcc = 0;
			u64 offset = 0;
			u64 size = 0;
			u32 elemCount = 0;
			u32 elemStride = 0;
		};

		void AlignZero(Vector<u8>& bytes, size_t alignment) {
			size_t padded = (bytes.size() + alignment - 1) / alignment * alignment;
			bytes.resize(padded, 0);
		}

		void Append(Vector<u8>& bytes, const void* data, size_t size) {
			if (size == 0) {
				return;
			}
			size_t at = bytes.size();
			bytes.resize(at + size);
			std::memcpy(bytes.data() + at, data, size);
		}

		template <typename T>
		void AppendPod(Vector<u8>& bytes, const T& value) {
			static_assert(std::is_trivially_copyable_v<T>);
			Append(bytes, &value, sizeof(T));
		}

		SectionBuild BeginSection(Vector<u8>& toc, u32 fourcc, u32 elemStride) {
			AlignZero(toc, 8);
			SectionBuild section;
			section.fourcc = fourcc;
			section.offset = toc.size();
			section.elemStride = elemStride;
			return section;
		}

		void EndSection(Vector<u8>& toc, SectionBuild& section, u32 elemCount) {
			section.size = toc.size() - section.offset;
			section.elemCount = elemCount;
		}

	} // anonymous namespace

	namespace CookOps {

		bool BuildPackage(const Vector<PakAssetSource>& assets, const PakWriteDesc& desc, Vector<u8>& out,
						  str& outError, CookReport* outReport) {
			if (desc.pageSize == 0 || desc.pageSize % PAK_SECTOR_SIZE != 0) {
				outError = std::format("page size {} isn't a non-zero multiple of the {}-byte sector",
									   desc.pageSize, PAK_SECTOR_SIZE);
				return false;
			}

			Vector<const PakDocumentSource*> documents{GetMemoryResource(MemTag::Editor)};
			for (const PakDocumentSource& document : desc.documents) {
				if (document.kind != PakDocumentKind::BootManifest && document.kind != PakDocumentKind::ProjectSettings &&
					document.kind != PakDocumentKind::Icon && document.kind != PakDocumentKind::ThirdPartyNotices) {
					outError = std::format("document kind {} isn't one this build knows how to write",
										   static_cast<u32>(document.kind));
					return false;
				}
				if (document.text.empty() || document.text.size() > 0xFFFFFFFFull) {
					outError = std::format("document kind {} is {} bytes (1 byte to 4 GiB allowed)",
										   static_cast<u32>(document.kind), document.text.size());
					return false;
				}
				documents.push_back(&document);
			}
			std::sort(documents.begin(), documents.end(),
					  [](const PakDocumentSource* a, const PakDocumentSource* b) { return a->kind < b->kind; });
			for (size_t i = 1; i < documents.size(); ++i) {
				if (documents[i - 1]->kind == documents[i]->kind) {
					outError = std::format("two documents of kind {}", static_cast<u32>(documents[i]->kind));
					return false;
				}
			}

			Vector<u32> order{GetMemoryResource(MemTag::Editor)};
			order.reserve(assets.size());
			for (u32 i = 0; i < static_cast<u32>(assets.size()); ++i) {
				order.push_back(i);
			}
			std::sort(order.begin(), order.end(),
					  [&assets](u32 a, u32 b) { return assets[a].guid < assets[b].guid; });

			for (size_t i = 1; i < order.size(); ++i) {
				if (assets[order[i - 1]].guid == assets[order[i]].guid) {
					outError = std::format("2 assets share the Guid {}: '{}' and '{}'",
										   assets[order[i]].guid.ToString(), assets[order[i - 1]].name,
										   assets[order[i]].name);
					return false;
				}
			}

			auto indexOfGuid = [&assets, &order](const Guid& guid) -> u32 {
				size_t low = 0;
				size_t high = order.size();
				while (low < high) {
					size_t mid = low + (high - low) / 2;
					const Guid& probe = assets[order[mid]].guid;
					if (probe < guid) {
						low = mid + 1;
					} else if (guid < probe) {
						high = mid;
					} else {
						return static_cast<u32>(mid);
					}
				}
				return PAK_INVALID_INDEX;
			};

			Vector<PakTypeEntry> types{GetMemoryResource(MemTag::Editor)};
			Vector<u16> typeOfAsset{GetMemoryResource(MemTag::Editor)};
			typeOfAsset.resize(order.size(), 0);
			for (size_t i = 0; i < order.size(); ++i) {
				const PakAssetSource& source = assets[order[i]];
				u16 found = 0xFFFF;
				for (u16 t = 0; t < static_cast<u16>(types.size()); ++t) {
					if (types[t].typeNameHash == source.typeNameHash) {
						found = t;
						break;
					}
				}
				if (found == 0xFFFF) {
					if (types.size() >= 0xFFFFu) {
						outError = str("a package can't hold more than 65,535 distinct asset types");
						return false;
					}
					PakTypeEntry entry{};
					entry.typeNameHash = source.typeNameHash;
					entry.recordVersion = source.recordVersion;
					entry.flags = 0;
					entry.descStride = source.typeDescStride;
					entry.layoutHash = source.layoutHash;
					found = static_cast<u16>(types.size());
					types.push_back(entry);
				} else if (types[found].recordVersion != source.recordVersion ||
						   types[found].layoutHash != source.layoutHash) {
					outError = std::format("2 assets of type {:#x} disagree about recordVersion or layoutHash",
										   source.typeNameHash);
					return false;
				}
				typeOfAsset[i] = found;
			}

			Vector<u32> withPayload{GetMemoryResource(MemTag::Editor)};
			for (u32 i = 0; i < static_cast<u32>(order.size()); ++i) {
				if (!assets[order[i]].payload.empty()) {
					withPayload.push_back(i);
				}
			}
			std::stable_sort(withPayload.begin(), withPayload.end(), [&](u32 a, u32 b) {
				return static_cast<u8>(assets[order[a]].residency) < static_cast<u8>(assets[order[b]].residency);
			});

			Vector<PakRegion> regions{GetMemoryResource(MemTag::Editor)};
			Vector<PakPage> pages{GetMemoryResource(MemTag::Editor)};
			Vector<u32> regionOfAsset{GetMemoryResource(MemTag::Editor)};
			regionOfAsset.resize(order.size(), PAK_NO_REGION);
			Vector<u8> payload{GetMemoryResource(MemTag::Editor)};

			u32 maxAlignment = 8;
			for (u32 slot : withPayload) {
				const PakAssetSource& source = assets[order[slot]];
				u32 alignment = source.payloadAlignment == 0 ? 1u : source.payloadAlignment;
				maxAlignment = std::max(maxAlignment, alignment);

				PakRegion region{};
				region.sizeRaw = source.payload.size();
				region.firstPage = static_cast<u32>(pages.size());
				region.alignment = alignment;
				region.codec = static_cast<u8>(PakCodec::None);
				region.residency = static_cast<u8>(source.residency);
				region.flags = 0;
				region.pad = 0;

				const bool streamed = source.residency == PakResidency::Streamed;
				bool anyPageCompressed = false;
				size_t written = 0;
				while (written < source.payload.size()) {
					AlignZero(payload, PAK_SECTOR_SIZE);
					size_t chunk = std::min<size_t>(desc.pageSize, source.payload.size() - written);
					PakPage page{};
					page.offset = payload.size();
					page.sizeRaw = static_cast<u32>(chunk);

					size_t encoded = 0;
					if (desc.compression != CookCompression::None && !streamed) {
						LZ4::Effort effort = desc.compression == CookCompression::LZ4HC ? LZ4::Effort::High
																						: LZ4::Effort::Fast;
						encoded = LZ4::Compress(source.payload.data() + written, chunk, effort, payload);
						if (encoded >= chunk) {
							payload.resize(static_cast<size_t>(page.offset));
							encoded = 0;
						}
					}
					if (encoded > 0) {
						page.sizeComp = static_cast<u32>(encoded);
						anyPageCompressed = true;
					} else {
						page.sizeComp = static_cast<u32>(chunk);
						Append(payload, source.payload.data() + written, chunk);
					}
					pages.push_back(page);
					written += chunk;
				}
				if (anyPageCompressed) {
					region.codec = static_cast<u8>(PakCodec::LZ4);
				}
				region.pageCount = static_cast<u32>(pages.size()) - region.firstPage;
				regionOfAsset[slot] = static_cast<u32>(regions.size());
				regions.push_back(region);
			}
			AlignZero(payload, PAK_SECTOR_SIZE);

			Vector<u8> descArena{GetMemoryResource(MemTag::Editor)};
			Vector<u32> descOffset{GetMemoryResource(MemTag::Editor)};
			descOffset.resize(order.size(), 0);
			for (size_t i = 0; i < order.size(); ++i) {
				AlignZero(descArena, 8);
				descOffset[i] = static_cast<u32>(descArena.size());
				const Vector<u8>& bytes = assets[order[i]].descriptor;
				Append(descArena, bytes.data(), bytes.size());
			}

			Vector<u8> toc{GetMemoryResource(MemTag::Editor)};
			Vector<SectionBuild> directory{GetMemoryResource(MemTag::Editor)};

			u16 sectionCount = static_cast<u16>(7 + (desc.shipDebugNames ? 1 : 0) + (desc.strings.empty() ? 0 : 1) +
												(documents.empty() ? 0 : 1));
			size_t directoryBytes = static_cast<size_t>(sectionCount) * sizeof(PakSection);
			toc.resize(directoryBytes, 0);

			SectionBuild sect;
			sect.fourcc = PAK_SECT;
			sect.offset = 0;
			sect.size = directoryBytes;
			sect.elemCount = sectionCount;
			sect.elemStride = sizeof(PakSection);
			directory.push_back(sect);

			{
				SectionBuild section = BeginSection(toc, PAK_TYPE, sizeof(PakTypeEntry));
				for (const PakTypeEntry& entry : types) {
					AppendPod(toc, entry);
				}
				EndSection(toc, section, static_cast<u32>(types.size()));
				directory.push_back(section);
			}

			{
				SectionBuild section = BeginSection(toc, PAK_IDX0, sizeof(PakAssetEntry));
				for (size_t i = 0; i < order.size(); ++i) {
					const PakAssetSource& source = assets[order[i]];
					PakAssetEntry entry{};
					entry.guid = source.guid;
					entry.descOffset = descOffset[i];
					entry.descSize = static_cast<u32>(source.descriptor.size());
					entry.regionIndex = regionOfAsset[i];
					entry.typeIndex = typeOfAsset[i];
					entry.flags = source.flags;
					if (entry.regionIndex != PAK_NO_REGION) {
						entry.flags |= static_cast<u16>(PakAssetFlags::HasPayload);
					}
					if (entry.flags & static_cast<u16>(PakAssetFlags::EditorOnly)) {
						outError = std::format("'{}' is marked EditorOnly and can't be written to a package",
											   source.name);
						return false;
					}
					entry.reserved = 0;
					AppendPod(toc, entry);
				}
				EndSection(toc, section, static_cast<u32>(order.size()));
				directory.push_back(section);
			}

			{
				SectionBuild section = BeginSection(toc, PAK_DESC, 0);
				Append(toc, descArena.data(), descArena.size());
				EndSection(toc, section, 0);
				directory.push_back(section);
			}

			{
				SectionBuild section = BeginSection(toc, PAK_PLDM, sizeof(PakRegion));
				for (const PakRegion& region : regions) {
					AppendPod(toc, region);
				}
				EndSection(toc, section, static_cast<u32>(regions.size()));
				directory.push_back(section);
			}

			{
				SectionBuild section = BeginSection(toc, PAK_PAGE, sizeof(PakPage));
				for (const PakPage& page : pages) {
					AppendPod(toc, page);
				}
				EndSection(toc, section, static_cast<u32>(pages.size()));
				directory.push_back(section);
			}

			{
				SectionBuild section = BeginSection(toc, PAK_DEPS, 0);
				Vector<u32> begins{GetMemoryResource(MemTag::Editor)};
				Vector<PakDepEdge> edges{GetMemoryResource(MemTag::Editor)};
				Vector<Guid> outgoing{GetMemoryResource(MemTag::Editor)};
				begins.reserve(order.size() + 1);
				for (size_t i = 0; i < order.size(); ++i) {
					begins.push_back(static_cast<u32>(edges.size()));

					const Vector<Guid>& declared = assets[order[i]].dependencies;
					outgoing.assign(declared.begin(), declared.end());
					std::sort(outgoing.begin(), outgoing.end());
					outgoing.erase(std::unique(outgoing.begin(), outgoing.end()), outgoing.end());
					outgoing.erase(std::remove(outgoing.begin(), outgoing.end(), assets[order[i]].guid),
								   outgoing.end());

					for (const Guid& dependency : outgoing) {
						u32 target = indexOfGuid(dependency);
						if (target == PAK_INVALID_INDEX) {
							outError = std::format("'{}' depends on {} (not in this package)",
												   assets[order[i]].name, dependency.ToString());
							return false;
						}
						PakDepEdge edge{};
						edge.targetAsset = target;
						edge.flags = static_cast<u32>(PakRefKind::Hard);
						edges.push_back(edge);
					}
				}
				begins.push_back(static_cast<u32>(edges.size()));
				Append(toc, begins.data(), begins.size() * sizeof(u32));
				Append(toc, edges.data(), edges.size() * sizeof(PakDepEdge));
				EndSection(toc, section, 0);
				directory.push_back(section);
			}

			u64 featuresOptional = 0;
			if (desc.shipDebugNames) {
				featuresOptional |= static_cast<u64>(PakFeatureOptional::DebugNames);
				SectionBuild section = BeginSection(toc, PAK_NAMS, 0);
				Vector<u32> offsets{GetMemoryResource(MemTag::Editor)};
				Vector<u8> blob{GetMemoryResource(MemTag::Editor)};
				offsets.reserve(order.size() + 1);
				for (size_t i = 0; i < order.size(); ++i) {
					offsets.push_back(static_cast<u32>(blob.size()));
					const str& name = assets[order[i]].name;
					Append(blob, name.data(), name.size());
				}
				offsets.push_back(static_cast<u32>(blob.size()));
				Append(toc, offsets.data(), offsets.size() * sizeof(u32));
				Append(toc, blob.data(), blob.size());
				EndSection(toc, section, 0);
				directory.push_back(section);
			}

			if (!desc.strings.empty()) {
				featuresOptional |= static_cast<u64>(PakFeatureOptional::StringTable);
				SectionBuild section = BeginSection(toc, PAK_STRS, 0);
				PakWriteStringBlob(toc, desc.strings);
				EndSection(toc, section, 0);
				directory.push_back(section);
			}

			if (!documents.empty()) {
				featuresOptional |= static_cast<u64>(PakFeatureOptional::Documents);
				SectionBuild section = BeginSection(toc, PAK_DOCS, sizeof(PakDocumentEntry));
				u64 cursor = documents.size() * sizeof(PakDocumentEntry);
				for (const PakDocumentSource* document : documents) {
					if (cursor + document->text.size() > 0xFFFFFFFFull) {
						outError = str("the build's documents don't fit the 4 GiB DOCS addresses");
						return false;
					}
					PakDocumentEntry entry{};
					entry.kind = static_cast<u32>(document->kind);
					entry.reserved = 0;
					entry.offset = static_cast<u32>(cursor);
					entry.size = static_cast<u32>(document->text.size());
					AppendPod(toc, entry);
					cursor += document->text.size();
				}
				for (const PakDocumentSource* document : documents) {
					Append(toc, document->text.data(), document->text.size());
				}
				EndSection(toc, section, static_cast<u32>(documents.size()));
				directory.push_back(section);
			}

			if (directory.size() != sectionCount) {
				outError = std::format("the writer counted {} section(s) and wrote {}", sectionCount,
									   directory.size());
				return false;
			}
			for (size_t i = 0; i < directory.size(); ++i) {
				PakSection entry{};
				entry.fourcc = directory[i].fourcc;
				entry.flags = 0;
				entry.offset = directory[i].offset;
				entry.size = directory[i].size;
				entry.elemCount = directory[i].elemCount;
				entry.elemStride = directory[i].elemStride;
				std::memcpy(toc.data() + i * sizeof(PakSection), &entry, sizeof(PakSection));
			}

			out.clear();
			out.resize(PAK_HEADER_SIZE, 0);
			AlignZero(out, 8);
			u64 tocOffset = out.size();
			Append(out, toc.data(), toc.size());
			AlignZero(out, PAK_SECTOR_SIZE);
			u64 payloadOffset = out.size();

			const SectionBuild* pageSection = nullptr;
			for (const SectionBuild& section : directory) {
				if (section.fourcc == PAK_PAGE) {
					pageSection = &section;
					break;
				}
			}
			for (size_t i = 0; i < pages.size(); ++i) {
				PakPage rebased = pages[i];
				rebased.offset += payloadOffset;
				std::memcpy(out.data() + tocOffset + pageSection->offset + i * sizeof(PakPage), &rebased,
							sizeof(PakPage));
			}
			Append(out, payload.data(), payload.size());

			// superblock last (it points at what follows)
			PakSuperblock sb{};
			std::memcpy(sb.magic, PAK_MAGIC, sizeof(PAK_MAGIC));
			sb.versionMajor = PAK_VERSION_MAJOR;
			sb.versionMinor = PAK_VERSION_MINOR;
			sb.headerSize = PAK_HEADER_SIZE;
			sb.featuresRequired = 0;
			sb.featuresOptional = featuresOptional;
			sb.fileSize = out.size();
			sb.tocOffset = tocOffset;
			sb.tocSizeComp = static_cast<u32>(toc.size());
			sb.tocSizeRaw = static_cast<u32>(toc.size());
			sb.tocCodec = static_cast<u8>(PakCodec::None);
			sb.tocFlags = 0;
			sb.sectionCount = sectionCount;
			sb.assetCount = static_cast<u32>(order.size());
			sb.payloadOffset = payloadOffset;
			sb.payloadSize = payload.size();
			sb.regionCount = static_cast<u32>(regions.size());
			sb.pageCount = static_cast<u32>(pages.size());
			sb.sectorSize = PAK_SECTOR_SIZE;
			sb.maxAlignment = maxAlignment;
			sb.targetId = desc.targetId;
			sb.localeId = desc.localeId;
			sb.engineHash = BuildInfo::GetEngineHash();
			sb.contentHash = PakContentHash(out.data() + PAK_HEADER_SIZE, out.size() - PAK_HEADER_SIZE);
			sb.crc32c = PakCrc32c(&sb, 4092);

			std::memcpy(out.data(), &sb, sizeof(PakSuperblock));

			if (outReport) {
				outReport->assets.clear();
				outReport->assets.resize(order.size());
				outReport->fileBytes = sb.fileSize;
				outReport->tocBytes = sb.tocSizeRaw;
				outReport->payloadBytes = sb.payloadSize;
				outReport->regionCount = sb.regionCount;
				outReport->pageCount = sb.pageCount;
				outReport->compression = static_cast<u8>(desc.compression);
				outReport->debugNamesShipped = desc.shipDebugNames;
				for (size_t i = 0; i < order.size(); ++i) {
					const PakAssetSource& source = assets[order[i]];
					CookReportAsset& line = outReport->assets[i];
					line.guid = source.guid;
					line.name = source.name;
					line.descriptorBytes = static_cast<u32>(source.descriptor.size());
					line.payloadRaw = source.payload.size();
					u32 regionIndex = regionOfAsset[i];
					if (regionIndex != PAK_NO_REGION) {
						const PakRegion& region = regions[regionIndex];
						line.pageCount = region.pageCount;
						line.codec = region.codec;
						for (u32 p = 0; p < region.pageCount; ++p) {
							line.payloadStored += pages[region.firstPage + p].sizeComp;
						}
					}
				}
			}
			return true;
		}

		bool WritePackage(const Path& outputPath, const Vector<PakAssetSource>& assets, const PakWriteDesc& desc,
						  str& outError) {
			Vector<u8> bytes{GetMemoryResource(MemTag::Editor)};
			if (!BuildPackage(assets, desc, bytes, outError)) {
				return false;
			}
			Buffer buffer(bytes.size(), MemTag::Editor);
			std::memcpy(buffer.data, bytes.data(), bytes.size());
			if (!FileSystem::WriteFile(outputPath, buffer)) {
				outError = std::format("couldn't write '{}'", outputPath.ToString());
				return false;
			}
			SLT_LOG_INFO("Cook", "wrote '{}' ({} asset(s), {} byte(s))", outputPath.ToString(), assets.size(),
						 bytes.size());
			return true;
		}

		bool CookAsset(const Asset& asset, PakStringPool& strings, PakAssetSource& out, str& outError) {
			if (!asset.assetId.IsValid()) {
				outError = std::format("'{}' has no Guid and can't be keyed in a package", asset.virtualPath);
				return false;
			}

			out.guid = asset.assetId;
			out.typeNameHash = asset.GetAssetTypeId().GetValue();
			out.name = asset.virtualPath;
			out.descriptor.clear();
			out.payload.clear();

			out.recordVersion = PAK_RECORD_VERSION;
			out.layoutHash = 0;
			out.typeDescStride = 0;

			out.dependencies.clear();

			CookWriter descriptor(out.descriptor, strings);
			CookWriter payload(out.payload, strings);
			if (!asset.Cook(descriptor, payload)) {
				outError = std::format("'{}' failed to cook", asset.virtualPath);
				return false;
			}
			out.residency = payload.GetResidency();

			for (const CookWriter* cursor : {&descriptor, &payload}) {
				for (const Guid& reference : cursor->GetReferences()) {
					out.dependencies.push_back(reference);
				}
			}
			return true;
		}

		bool IsEditorOnlyEngineReference(std::string_view virtualPath) {
			constexpr std::string_view Prefix = "engine://";
			if (!virtualPath.starts_with(Prefix)) {
				return false;
			}
			return BuildPipeline::IsEditorOnlyEngineAsset(virtualPath.substr(Prefix.size()));
		}

		bool IsConstructibleAsAnAsset(const Asset& asset, str& outWhy) {
			const TypeInfo* info = TypeRegistry::FindType(asset.GetAssetTypeId());
			if (!info) {
				outWhy = "its type isn't registered";
				return false;
			}
			if (!TypeRegistry::IsDerivedFrom(info, Asset::GetReflectType())) {
				outWhy = "its type is registered but doesn't derive from Asset";
				return false;
			}
			Object* probe = TypeRegistry::CreateObjectInstance(info, MemTag::Editor);
			if (!probe) {
				outWhy = "its type isn't constructible by reflection (SLT_REFLECT_CREATABLE is missing)";
				return false;
			}
			Delete(probe);
			return true;
		}

		u32 CollectAlwaysCookedRoots(Vector<Guid>& out, Vector<str>* failedImports) {
			u32 appended = 0;
			AssetImporterRegistry::ForEachImporter([&out, &appended, failedImports](StringHash typeId, IAssetImporter* importer, std::string_view) {
				if (!importer || !importer->IsAlwaysCooked()) {
					return;
				}
				Vector<Guid> found{GetMemoryResource(MemTag::Editor)};
				AssetDatabase::FindAssets(typeId, found);
				for (const Guid& guid : found) {
					if (std::find(out.begin(), out.end(), guid) != out.end()) {
						continue;
					}
					Asset* loaded = AssetDatabase::LoadAssetByGuid(guid);
					const bool imported = loaded && loaded->GetAssetTypeId() == typeId;
					if (loaded) {
						AssetDatabase::UnloadAsset(loaded);
					}
					if (!imported) {
						if (failedImports) {
							failedImports->push_back(AssetDatabase::LookupPath(guid).ToString());
						}
						continue;
					}
					out.push_back(guid);
					++appended;
				}
			});
			return appended;
		}

		bool ComputeClosure(const Vector<Guid>& sceneRoots, CookClosure& out, str& outError) {
			out.assets.clear();
			out.dependencies.clear();

			struct Pending {
				Guid guid;
				Guid referrer;
			};
			std::deque<Pending> worklist;
			out.roots.clear();
			for (const Guid& root : sceneRoots) {
				worklist.push_back({root, Guid{}});
				out.roots.push_back(root);
			}
			for (const Guid& builtin : AssetDatabase::GetEngineBuiltinRoots()) {
				worklist.push_back({builtin, Guid{}});
				out.roots.push_back(builtin);
			}
			std::sort(out.roots.begin(), out.roots.end());
			out.roots.erase(std::unique(out.roots.begin(), out.roots.end()), out.roots.end());

			std::map<Guid, Vector<Guid>> edges;
			std::map<Guid, str> paths;

			while (!worklist.empty()) {
				Pending item = worklist.front();
				worklist.pop_front();
				if (edges.find(item.guid) != edges.end()) {
					continue;
				}

				Vector<Guid> observed{GetMemoryResource(MemTag::Editor)};
				Asset* loaded = nullptr;
				{
					AssetLoadRecorder recorder(observed);
					loaded = AssetDatabase::LoadAssetByGuid(item.guid);
					if (loaded) {
						PakStringPool scratchStrings;
						PakAssetSource scratch;
						str ignored;
						if (CookAsset(*loaded, scratchStrings, scratch, ignored)) {
							for (const Guid& declared : scratch.dependencies) {
								observed.push_back(declared);
							}
						}
					}
				}

				if (!loaded) {
					auto referrerPath = paths.find(item.referrer);
					outError = item.referrer.IsValid()
								   ? std::format("'{}' references {} (nothing resolves it, a deleted asset is "
												 "still referenced)",
												 referrerPath != paths.end() ? referrerPath->second
																			 : item.referrer.ToString(),
												 item.guid.ToString())
								   : std::format("{} is listed in this build, nothing resolves it",
												 item.guid.ToString());
					return false;
				}

				paths[item.guid] = loaded->virtualPath;

				if (loaded->GetAssetTypeId() == StringHash("MaterialAsset")) {
					const MaterialAsset& material = *static_cast<const MaterialAsset*>(loaded);
					Vector<str> wanted{GetMemoryResource(MemTag::Editor)};
					material.GetEffectiveKeywords(wanted);
					if (!wanted.empty() && material.shaderGuid.IsValid()) {
						CookClosure::ShaderVariantRequest request;
						request.shader = material.shaderGuid;
						request.keywords.assign(wanted.begin(), wanted.end());
						out.variantRequests.push_back(std::move(request));
					}
				}

				if (IsEditorOnlyEngineReference(loaded->virtualPath)) {
					auto referrerPath = paths.find(item.referrer);
					outError = std::format("'{}' is editor-only and must not ship, but '{}' references it",
										   loaded->virtualPath,
										   referrerPath != paths.end() ? referrerPath->second
																	   : item.referrer.ToString());
					AssetDatabase::UnloadAsset(loaded);
					return false;
				}

				str why;
				if (!IsConstructibleAsAnAsset(*loaded, why)) {
					outError = std::format("'{}' can't be loaded from a package: {}", loaded->virtualPath, why);
					AssetDatabase::UnloadAsset(loaded);
					return false;
				}

				AssetDatabase::UnloadAsset(loaded);

				std::sort(observed.begin(), observed.end());
				observed.erase(std::unique(observed.begin(), observed.end()), observed.end());
				observed.erase(std::remove(observed.begin(), observed.end(), item.guid), observed.end());

				for (const Guid& dependency : observed) {
					worklist.push_back({dependency, item.guid});
				}
				edges.emplace(item.guid, std::move(observed));
			}

			out.assets.reserve(edges.size());
			out.dependencies.reserve(edges.size());
			for (auto& [guid, dependencies] : edges) {
				out.assets.push_back(guid);
				out.dependencies.push_back(std::move(dependencies));
			}
			return true;
		}

		CookClosure CookClosure::Of(const Vector<Guid>& assets) {
			CookClosure closure;
			closure.assets.assign(assets.begin(), assets.end());
			std::sort(closure.assets.begin(), closure.assets.end());
			closure.assets.erase(std::unique(closure.assets.begin(), closure.assets.end()), closure.assets.end());
			closure.dependencies.resize(closure.assets.size());
			closure.roots.assign(closure.assets.begin(), closure.assets.end());
			return closure;
		}

		namespace {
			void FillReportGraph(CookReport& report, const Vector<PakAssetSource>& sources, const Vector<Guid>& roots);
		} // anonymous namespace

		bool CookPackage(const CookClosure& closure, const PakWriteDesc& desc, Vector<u8>& out, str& outError,
						 CookReport* outReport) {
			SLT_APP_ASSERT_MSG(std::is_sorted(closure.assets.begin(), closure.assets.end()),
							   "a CookClosure's assets should be sorted");

			PakStringPool strings;
			Vector<PakAssetSource> sources{GetMemoryResource(MemTag::Editor)};
			sources.reserve(closure.assets.size());

			for (size_t i = 0; i < closure.assets.size(); ++i) {
				const Guid& guid = closure.assets[i];
				Asset* loaded = AssetDatabase::LoadAssetByGuid(guid);
				if (!loaded) {
					outError = std::format("{} didn't load", guid.ToString());
					return false;
				}
				const Asset* cookTarget = loaded;
				ShaderAsset webShader;
				if (loaded->GetAssetTypeId() == StringHash("ShaderAsset")) {
					ShaderAsset& shader = *static_cast<ShaderAsset*>(loaded);
					if (desc.shaderFormat != shader.bytecodeFormat && !shader.virtualPath.empty()) {
						const RHI::ShaderBytecodeFormat previousFormat = ShaderAsset::GetDefaultImportBytecodeFormat();
						ShaderAsset::SetDefaultImportBytecodeFormat(desc.shaderFormat);
						webShader.assetId = shader.assetId;
						webShader.virtualPath = shader.virtualPath;
						const bool imported = webShader.Import(Path(shader.virtualPath));
						ShaderAsset::SetDefaultImportBytecodeFormat(previousFormat);
						if (!imported) {
							AssetDatabase::UnloadAsset(loaded);
							outError = std::format("'{}' failed to compile for target shader format", shader.virtualPath);
							return false;
						}
						for (const CookClosure::ShaderVariantRequest& request : closure.variantRequests) {
							if (request.shader == guid) {
								webShader.GetOrCompileVariant({request.keywords.data(), request.keywords.size()});
							}
						}
						cookTarget = &webShader;
					} else {
						for (const CookClosure::ShaderVariantRequest& request : closure.variantRequests) {
							if (request.shader == guid) {
								shader.GetOrCompileVariant({request.keywords.data(), request.keywords.size()});
							}
						}
					}
				}

				PakAssetSource source;
				bool cooked = CookAsset(*cookTarget, strings, source, outError);
				AssetDatabase::UnloadAsset(loaded);
				if (!cooked) {
					return false;
				}
				if (i < closure.dependencies.size()) {
					for (const Guid& edge : closure.dependencies[i]) {
						source.dependencies.push_back(edge);
					}
				}
				sources.push_back(std::move(source));
			}

			PakWriteDesc resolved = desc;
			resolved.strings.assign(strings.Texts().begin(), strings.Texts().end());
			if (!BuildPackage(sources, resolved, out, outError, outReport)) {
				return false;
			}
			if (outReport) {
				FillReportGraph(*outReport, sources, closure.roots);
			}
			return true;
		}

		namespace {

			constexpr i64 COOK_REPORT_FORMAT_VERSION = 1;

			void AddIndexArray(JSONValue& object, const char* key, const Vector<u32>& indices) {
				if (indices.empty()) {
					return;
				}
				JSONValue array = JSONValue::MakeArray();
				for (u32 index : indices) {
					array.Push(JSONValue::MakeNumber(index));
				}
				object.AddMember(key, std::move(array));
			}

			void ReadIndexArray(const JSONValue& object, const char* key, Vector<u32>& out) {
				const JSONValue& array = object[key];
				for (size_t i = 0; i < array.Size(); ++i) {
					i64 value = array[i].AsInt(-1);
					out.push_back(value < 0 ? 0xFFFFFFFFu : static_cast<u32>(value));
				}
			}

			void FillReportGraph(CookReport& report, const Vector<PakAssetSource>& sources,
								 const Vector<Guid>& roots) {
				auto indexOf = [&report](const Guid& guid) -> u32 {
					auto it = std::lower_bound(report.assets.begin(), report.assets.end(), guid,
											   [](const CookReportAsset& line, const Guid& key) {
												   return line.guid < key;
											   });
					if (it == report.assets.end() || !(it->guid == guid)) {
						return PAK_INVALID_INDEX;
					}
					return static_cast<u32>(it - report.assets.begin());
				};

				for (const PakAssetSource& source : sources) {
					u32 self = indexOf(source.guid);
					if (self == PAK_INVALID_INDEX) {
						continue;
					}
					CookReportAsset& line = report.assets[self];

					const TypeInfo* info = TypeRegistry::FindType(StringHash(source.typeNameHash));
					line.typeName = (info && info->displayName) ? str(info->displayName)
																: std::format("{:#x}", source.typeNameHash);

					for (const Guid& edge : source.dependencies) {
						u32 target = indexOf(edge);
						if (target != PAK_INVALID_INDEX && target != self) {
							line.dependsOn.push_back(target);
						}
					}
					std::sort(line.dependsOn.begin(), line.dependsOn.end());
					line.dependsOn.erase(std::unique(line.dependsOn.begin(), line.dependsOn.end()),
										 line.dependsOn.end());
				}

				for (u32 i = 0; i < static_cast<u32>(report.assets.size()); ++i) {
					for (u32 target : report.assets[i].dependsOn) {
						report.assets[target].pulledInBy.push_back(i);
					}
				}

				for (const Guid& root : roots) {
					u32 index = indexOf(root);
					if (index != PAK_INVALID_INDEX) {
						report.assets[index].isRoot = true;
					}
				}

				Vector<u32> predecessor{GetMemoryResource(MemTag::Editor)};
				predecessor.resize(report.assets.size(), PAK_INVALID_INDEX);
				Vector<u8> seen{GetMemoryResource(MemTag::Editor)};
				seen.resize(report.assets.size(), 0);
				std::deque<u32> queue;
				for (u32 i = 0; i < static_cast<u32>(report.assets.size()); ++i) {
					if (report.assets[i].isRoot) {
						seen[i] = 1;
						queue.push_back(i);
					}
				}
				while (!queue.empty()) {
					u32 at = queue.front();
					queue.pop_front();
					for (u32 target : report.assets[at].dependsOn) {
						if (!seen[target]) {
							seen[target] = 1;
							predecessor[target] = at;
							queue.push_back(target);
						}
					}
				}
				for (u32 i = 0; i < static_cast<u32>(report.assets.size()); ++i) {
					for (u32 at = predecessor[i]; at != PAK_INVALID_INDEX; at = predecessor[at]) {
						report.assets[i].rootPath.push_back(at);
					}
					std::reverse(report.assets[i].rootPath.begin(), report.assets[i].rootPath.end());
				}
			}

		} // anonymous namespace

		str WriteReportJSON(const CookReport& report) {
			std::deque<str> pool;
			auto pooled = [&pool](str text) -> std::string_view {
				pool.push_back(std::move(text));
				return pool.back();
			};

			JSONValue root = JSONValue::MakeObject();
			root.AddMember("formatVersion", JSONValue::MakeNumber(COOK_REPORT_FORMAT_VERSION));
			root.AddMember("profile", JSONValue::MakeString(pooled(report.profileName)));
			root.AddMember("package", JSONValue::MakeString(pooled(report.packagePath)));
			root.AddMember("fileBytes", JSONValue::MakeNumber(static_cast<f64>(report.fileBytes)));
			root.AddMember("tocBytes", JSONValue::MakeNumber(static_cast<f64>(report.tocBytes)));
			root.AddMember("payloadBytes", JSONValue::MakeNumber(static_cast<f64>(report.payloadBytes)));
			root.AddMember("regionCount", JSONValue::MakeNumber(report.regionCount));
			root.AddMember("pageCount", JSONValue::MakeNumber(report.pageCount));
			root.AddMember("candidateCount", JSONValue::MakeNumber(report.candidateCount));
			root.AddMember("compression", JSONValue::MakeNumber(report.compression));
			root.AddMember("debugNamesShipped", JSONValue::MakeBool(report.debugNamesShipped));

			JSONValue assets = JSONValue::MakeArray();
			for (const CookReportAsset& line : report.assets) {
				JSONValue entry = JSONValue::MakeObject();
				entry.AddMember("guid", JSONValue::MakeString(pooled(line.guid.ToString())));
				entry.AddMember("name", JSONValue::MakeString(pooled(line.name)));
				entry.AddMember("type", JSONValue::MakeString(pooled(line.typeName)));
				entry.AddMember("descriptorBytes", JSONValue::MakeNumber(line.descriptorBytes));
				entry.AddMember("payloadRaw", JSONValue::MakeNumber(static_cast<f64>(line.payloadRaw)));
				entry.AddMember("payloadStored", JSONValue::MakeNumber(static_cast<f64>(line.payloadStored)));
				entry.AddMember("pageCount", JSONValue::MakeNumber(line.pageCount));
				entry.AddMember("codec", JSONValue::MakeNumber(line.codec));
				entry.AddMember("isRoot", JSONValue::MakeBool(line.isRoot));
				AddIndexArray(entry, "dependsOn", line.dependsOn);
				AddIndexArray(entry, "pulledInBy", line.pulledInBy);
				AddIndexArray(entry, "rootPath", line.rootPath);
				assets.Push(std::move(entry));
			}
			root.AddMember("assets", std::move(assets));
			return JSONSerialize(root, true);
		}

		bool ReadReportJSON(std::string_view json, CookReport& out, str& outError) {
			LinearAllocator arena(json.size() + 64, MemTag::Editor);
			JSONValue root = JSONParse(json, arena);
			if (!root.IsObject()) {
				outError = "the build report isn't a JSON object";
				return false;
			}
			const JSONValue& version = root["formatVersion"];
			if (!version.IsNumber() || version.AsInt() != COOK_REPORT_FORMAT_VERSION) {
				outError = std::format("the build report is format {} (this editor reads format {})",
									   version.IsNumber() ? version.AsInt() : 0, COOK_REPORT_FORMAT_VERSION);
				return false;
			}

			CookReport parsed;
			parsed.profileName = str(root["profile"].AsString());
			parsed.packagePath = str(root["package"].AsString());
			parsed.fileBytes = static_cast<u64>(root["fileBytes"].AsInt());
			parsed.tocBytes = static_cast<u64>(root["tocBytes"].AsInt());
			parsed.payloadBytes = static_cast<u64>(root["payloadBytes"].AsInt());
			parsed.regionCount = static_cast<u32>(root["regionCount"].AsInt());
			parsed.pageCount = static_cast<u32>(root["pageCount"].AsInt());
			parsed.candidateCount = static_cast<u32>(root["candidateCount"].AsInt());
			parsed.compression = static_cast<u8>(root["compression"].AsInt());
			parsed.debugNamesShipped = root["debugNamesShipped"].AsBool();

			const JSONValue& assets = root["assets"];
			for (size_t i = 0; i < assets.Size(); ++i) {
				const JSONValue& entry = assets[i];
				CookReportAsset line;
				line.guid = Guid::FromString(entry["guid"].AsString());
				line.name = str(entry["name"].AsString());
				line.typeName = str(entry["type"].AsString());
				line.descriptorBytes = static_cast<u32>(entry["descriptorBytes"].AsInt());
				line.payloadRaw = static_cast<u64>(entry["payloadRaw"].AsInt());
				line.payloadStored = static_cast<u64>(entry["payloadStored"].AsInt());
				line.pageCount = static_cast<u32>(entry["pageCount"].AsInt());
				line.codec = static_cast<u8>(entry["codec"].AsInt());
				line.isRoot = entry["isRoot"].AsBool();
				ReadIndexArray(entry, "dependsOn", line.dependsOn);
				ReadIndexArray(entry, "pulledInBy", line.pulledInBy);
				ReadIndexArray(entry, "rootPath", line.rootPath);
				parsed.assets.push_back(std::move(line));
			}

			for (const CookReportAsset& line : parsed.assets) {
				for (const Vector<u32>* set : {&line.dependsOn, &line.pulledInBy, &line.rootPath}) {
					for (u32 index : *set) {
						if (index >= parsed.assets.size()) {
							outError = std::format("the build report references asset {} of {}", index,
												   parsed.assets.size());
							return false;
						}
					}
				}
			}

			out = std::move(parsed);
			return true;
		}

	} // namespace CookOps

} // namespace Shalltear
