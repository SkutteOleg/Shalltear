#include "SceneDocument.h"

#include "FragmentBuilder.h"
#include "SceneInternal.h"
#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Asset/AssetImporter.h"
#include "Shalltear/Core/BuildInfo.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Scene/Prefab.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneAsset.h"
#include "Shalltear/Core/Scene/SceneEvents.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/Serialization/HandleResolver.h"

#include <algorithm>
#include <charconv>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace Shalltear {

	using namespace SceneInternal;
	namespace SI = SerializerInternal;

	namespace {

		constexpr u32 SCENE_MAGIC = 0x53544C53;
		constexpr u16 SCENE_FORMAT_VERSION = 1;
		constexpr u16 SCENE_FLAG_PREFAB = 1u << 0;

		str U64ToHex(u64 value) {
			return std::format("{:016x}", value);
		}

		bool HexToU64(std::string_view text, u64& out) {
			if (text.size() != 16) {
				return false;
			}
			auto result = std::from_chars(text.data(), text.data() + 16, out, 16);
			return result.ec == std::errc{} && result.ptr == text.data() + 16;
		}

		str BytesToHex(const std::vector<u8>& bytes) {
			str out;
			out.reserve(bytes.size() * 2);
			for (u8 byte : bytes) {
				out += "0123456789abcdef"[byte >> 4];
				out += "0123456789abcdef"[byte & 0xF];
			}
			return out;
		}

		bool HexToBytes(std::string_view text, std::vector<u8>& out) {
			if (text.size() % 2 != 0) {
				return false;
			}
			out.clear();
			out.reserve(text.size() / 2);
			for (size_t i = 0; i < text.size(); i += 2) {
				u32 byte = 0;
				auto result = std::from_chars(text.data() + i, text.data() + i + 2, byte, 16);
				if (result.ec != std::errc{} || result.ptr != text.data() + i + 2) {
					return false;
				}
				out.push_back(static_cast<u8>(byte));
			}
			return true;
		}

	} // anonymous namespace

	namespace SceneInternal {

		void NormalizeOpaqueBlob(SceneDocumentBlob& blob, bool binaryDocument) {
			if (binaryDocument) {
				constexpr size_t HEADER_SIZE = sizeof(u64) + sizeof(u8);
				if (blob.binary.size() < HEADER_SIZE) {
					return;
				}
				u64 sentinel = 0;
				std::memcpy(&sentinel, blob.binary.data(), sizeof(sentinel));
				if (sentinel != SLT_OPAQUE_SENTINEL) {
					return;
				}
				blob.isOpaque = true;
				blob.opaqueIsBinary = blob.binary[sizeof(u64)] != 0;
				blob.opaque.assign(blob.binary.begin() + HEADER_SIZE, blob.binary.end());
				blob.binary.clear();
				return;
			}
			if (!blob.json.IsObject()) {
				return;
			}
			const JSONValue& payload = blob.json["_opaque"];
			if (!payload.IsString()) {
				return;
			}
			if (!HexToBytes(payload.AsString(), blob.opaque)) {
				SLT_LOG_WARN("Scene", "preserved blob has malformed hex (row data lost)");
				return;
			}
			blob.isOpaque = true;
			blob.opaqueIsBinary = blob.json["_opaqueBinary"].AsBool(false);
			blob.json = JSONValue();
		}

	} // namespace SceneInternal

	namespace {

		struct SaveScopeCtx {
			std::unordered_map<u64, std::pair<u64, u64>> map;
		};

		bool SaveScopeToGuid(void* user, u64 instanceId, Guid* outGuid, StringHash* outPointeeType) {
			SaveScopeCtx* ctx = static_cast<SaveScopeCtx*>(user);
			auto it = ctx->map.find(instanceId);
			if (it != ctx->map.end()) {
				*outGuid = Guid{it->second.first, SLT_LOCAL_SENTINEL};
				*outPointeeType = StringHash(it->second.second);
				return true;
			}
			if (GetState().ownerById.find(instanceId) != GetState().ownerById.end()) {
				SLT_LOG_WARN("Scene", "serialized handle to a scene object outside this document. Writing null");
				*outGuid = Guid{};
				*outPointeeType = StringHash();
				return true;
			}
			return false;
		}

		struct LoadScopeCtx {
			std::unordered_map<u64, u64> map;
			Scene* scene = nullptr;
			SceneInternal::SceneAssetBinder assets;
		};

		bool LoadScopeToHandle(void* user, const Guid& guid, StringHash, u64* outId) {
			LoadScopeCtx* ctx = static_cast<LoadScopeCtx*>(user);
			if (guid.high != SLT_LOCAL_SENTINEL) {
				return ctx->assets.Resolve(ctx->scene, guid, outId);
			}
			auto it = ctx->map.find(guid.low);
			if (it == ctx->map.end()) {
				SLT_LOG_WARN("Scene", "document handle references unknown localId {}. Resolving null", guid.low);
				*outId = 0;
				return true;
			}
			*outId = it->second;
			return true;
		}

	} // anonymous namespace

	namespace SceneInternal {

		namespace {

			std::mutex _holdMutex;
			UnresolvedReportHold* _hold = nullptr;
			u32 _holdCount = 0;

			void ReportUnconvertedRow(StringHash rowType) {
				{
					std::lock_guard lock(_holdMutex);
					if (_hold) {
						_hold->HoldUnconverted(rowType);
						return;
					}
				}
				LogUnconvertedRow(rowType);
			}

		} // anonymous namespace

		bool InstallUnresolvedReportHold(UnresolvedReportHold& hold) {
			std::lock_guard lock(_holdMutex);
			++_holdCount;
			if (_hold) {
				return false;
			}
			_hold = &hold;
			return true;
		}

		void ReleaseUnresolvedReportHold(bool everyHolder) {
			UnresolvedReportHold* hold = nullptr;
			{
				std::lock_guard lock(_holdMutex);
				_holdCount = everyHolder || _holdCount == 0 ? 0 : _holdCount - 1;
				if (_holdCount == 0) {
					hold = std::exchange(_hold, nullptr);
				}
			}
			if (hold) {
				hold->Release();
			}
		}

		void LogUnconvertedRow(StringHash rowType) {
			SLT_LOG_WARN("Scene", "converting a document: component row type {:#018x} isn't loaded (row kept verbatim, not re-encoded)",
						 rowType.GetValue());
		}

		void LogUnresolvedRows(u32 rows, const std::vector<UnresolvedTypeMention>& types, std::string_view what) {
			if (rows == 0) {
				return;
			}
			str typeList;
			for (const UnresolvedTypeMention& mention : types) {
				typeList += typeList.empty() ? "" : ", ";
				if (!mention.spelling.empty()) {
					typeList += mention.spelling;
					typeList += ' ';
				}
				typeList += std::format("{:#018x}", mention.type.GetValue());
			}
			SLT_LOG_WARN("Scene", "'{}' loaded with {} component row(s) of {} unloaded type(s) ({}). Their data is saved unchanged",
						 what, rows, types.size(), typeList);
		}

		str SceneNameFromPath(const Path& path) {
			str filename = path.GetFilename();
			str extension = path.GetExtension();
			if (!extension.empty() && filename.size() > extension.size() && filename.ends_with(extension)) {
				filename.resize(filename.size() - extension.size());
			}
			return filename;
		}

		bool IsPreservedComponentType(const TypeInfo* type) {
			return type == UnresolvedComponent::GetReflectType();
		}

		UnresolvedComponent* MakePreservedComponent(StringHash declaredType, std::span<const u8> payload,
													bool payloadIsBinary) {
			auto* preserved = New<UnresolvedComponent>(MemTag::Scene);
			SceneInternalAccess::UnresolvedTypeOf(*preserved) = declaredType;
			SceneInternalAccess::BlobIsBinaryOf(*preserved) = payloadIsBinary;
			if (payloadIsBinary) {
				SceneInternalAccess::BlobBytesOf(*preserved).assign(payload.begin(), payload.end());
			} else {
				SceneInternalAccess::BlobTextOf(*preserved).assign(payload.begin(), payload.end());
			}
			return preserved;
		}

		UnresolvedComponent* MakePreservedComponent(const SceneDocument& document, const SceneDocumentComponentRow& row) {
			if (row.fields.isOpaque) {
				return MakePreservedComponent(row.type, row.fields.opaque, row.fields.opaqueIsBinary);
			}
			if (document.binary) {
				return MakePreservedComponent(row.type, row.fields.binary, true);
			}
			str text = JSONSerialize(row.fields.json, false);
			return MakePreservedComponent(row.type, {reinterpret_cast<const u8*>(text.data()), text.size()}, false);
		}

		void ApplyPreservedBlob(std::span<const u8> payload, bool payloadIsBinary, void* instance,
								const TypeInfo* type, std::vector<SI::HandleFixupJob>& fixups) {
			if (payload.empty()) {
				return;
			}
			if (payloadIsBinary) {
				SI::BinaryReader reader(payload.data(), payload.size());
				SI::BinaryReadObject(reader, instance, type, 0, fixups);
				return;
			}
			std::string_view text(reinterpret_cast<const char*>(payload.data()), payload.size());
			LinearAllocator arena(text.size() + 64, MemTag::Scene);
			JSONValue fields = JSONParse(text, arena);
			if (!fields.IsObject()) {
				SLT_LOG_WARN("Scene", "preserved blob for type {} isn't a field object (row data lost)", type->name.GetValue());
				return;
			}
			SI::TextReadObject(instance, type, fields, 0);
		}

		bool SceneAssetBinder::Resolve(Scene* owner, const Guid& guid, u64* outId) {
			if (!owner) {
				return false;
			}
			Asset* retain = nullptr;
			if (!binder.Bind(guid, outId, &retain)) {
				return false;
			}
			if (retain) {
				SceneInternalAccess::RetainedAssetsOf(*owner).push_back(Handle<Asset>(retain));
			}
			return true;
		}

		namespace {

			bool ParseComponentRowJSON(const JSONValue& value, SceneDocumentComponentRow& out) {
				if (!value.IsObject()) {
					return false;
				}
				out.id = static_cast<u64>(value["id"].AsNumber());
				u64 typeHash = 0;
				if (!HexToU64(value["type"].AsString(), typeHash)) {
					return false;
				}
				out.type = StringHash(typeHash);
				out.fields.json = value["fields"];
				NormalizeOpaqueBlob(out.fields, false);
				return true;
			}

			bool ParseRowJSON(const JSONValue& value, SceneDocumentRow& out, SceneDocument& document) {
				if (!value.IsObject()) {
					return false;
				}
				out.id = static_cast<u64>(value["id"].AsNumber());
				out.parent = static_cast<u64>(value["parent"].AsNumber());

				if (value["kind"].AsString() == "instance") {
					out.kind = SceneDocumentRow::Kind::Instance;
					out.prefab = Guid::FromString(value["prefab"].AsString());
					if (!ParseOverrideLayerJSON(value["overrides"], out.overrides, document)) {
						return false;
					}
					return out.prefab.IsValid();
				}

				out.kind = SceneDocumentRow::Kind::Object;
				u64 typeHash = 0;
				if (!HexToU64(value["type"].AsString(), typeHash)) {
					return false;
				}
				out.type = StringHash(typeHash);
				out.fields.json = value["fields"];

				const JSONValue& components = value["components"];
				for (size_t i = 0; i < components.Size(); ++i) {
					SceneDocumentComponentRow componentRow;
					if (ParseComponentRowJSON(components[i], componentRow)) {
						out.components.push_back(std::move(componentRow));
					} else {
						SLT_LOG_WARN("Scene", "document row {}: malformed component entry {} skipped", out.id, i);
					}
				}
				return out.id != 0;
			}

			SceneDocument* ParseTextDocument(Buffer&& bytes) {
				SceneDocument* document = New<SceneDocument>(MemTag::Scene);
				document->binary = false;
				document->sourceText.assign(reinterpret_cast<const char*>(bytes.data), bytes.size);
				document->arena = New<LinearAllocator>(MemTag::Scene, document->sourceText.size() + 64, MemTag::Scene);

				JSONValue root = JSONParse(document->sourceText, *document->arena);
				if (!root.IsObject()) {
					Delete(document);
					return nullptr;
				}

				document->formatVersion = static_cast<u16>(root["formatVersion"].AsInt(1));
				HexToU64(root["engineHash"].AsString(), document->engineHash);
				document->name = str(root["name"].AsString());
				document->nextLocalId = static_cast<u64>(root["nextLocalId"].AsNumber(1.0));

				const JSONValue& prefabBlock = root["prefab"];
				if (prefabBlock.IsObject()) {
					document->isPrefab = true;
					document->basePrefab = Guid::FromString(prefabBlock["base"].AsString());
					if (!ParseOverrideLayerJSON(prefabBlock["overrides"], document->prefabOverrides, *document)) {
						Delete(document);
						return nullptr;
					}
				}

				const JSONValue& objects = root["objects"];
				if (!objects.IsArray()) {
					Delete(document);
					return nullptr;
				}
				for (size_t i = 0; i < objects.Size(); ++i) {
					SceneDocumentRow row;
					if (ParseRowJSON(objects[i], row, *document)) {
						document->rows.push_back(std::move(row));
					} else {
						SLT_LOG_WARN("Scene", "document: malformed row {} skipped", i);
					}
				}
				return document;
			}

			bool ReadLengthPrefixedBlob(SI::BinaryReader& reader, std::vector<u8>& out) {
				u32 length = 0;
				if (!reader.ReadU32(length) || length > reader.Remaining()) {
					return false;
				}
				out.assign(reader.CurrentPtr(), reader.CurrentPtr() + length);
				return reader.Skip(length);
			}

			bool ReadLengthPrefixedString(SI::BinaryReader& reader, str& out) {
				u32 length = 0;
				if (!reader.ReadU32(length) || length > reader.Remaining()) {
					return false;
				}
				out.assign(reinterpret_cast<const char*>(reader.CurrentPtr()), length);
				return reader.Skip(length);
			}

			SceneDocument* ParseBinaryDocument(Buffer&& bytes) {
				SI::BinaryReader reader(bytes.data, bytes.size);

				u32 magic = 0;
				u16 version = 0;
				u16 flags = 0;
				if (!reader.ReadU32(magic) || magic != SCENE_MAGIC || !reader.ReadU16(version) || !reader.ReadU16(flags)) {
					return nullptr;
				}
				if (version != SCENE_FORMAT_VERSION) {
					SLT_LOG_ERROR("Scene", "scene document: unsupported version {}", version);
					return nullptr;
				}

				SceneDocument* document = New<SceneDocument>(MemTag::Scene);
				document->binary = true;
				document->formatVersion = version;
				document->isPrefab = (flags & SCENE_FLAG_PREFAB) != 0;

				bool ok = reader.ReadU64(document->engineHash);
				ok = ok && reader.ReadU64(document->nextLocalId);
				ok = ok && ReadLengthPrefixedString(reader, document->name);

				if (ok && document->isPrefab) {
					ok = reader.ReadU64(document->basePrefab.low) && reader.ReadU64(document->basePrefab.high);
					ok = ok && ParseOverrideLayerBinary(reader, document->prefabOverrides, *document);
				}

				u32 rowCount = 0;
				ok = ok && reader.ReadU32(rowCount);
				for (u32 i = 0; ok && i < rowCount; ++i) {
					u32 rowLength = 0;
					if (!reader.ReadU32(rowLength) || rowLength > reader.Remaining()) {
						ok = false;
						break;
					}
					size_t rowStart = reader.Tell();

					SceneDocumentRow row;
					u8 kind = 0;
					bool rowOk = reader.ReadU64(row.id) && reader.ReadU8(kind) && reader.ReadU64(row.parent);
					if (rowOk && kind == static_cast<u8>(SceneDocumentRow::Kind::Instance)) {
						row.kind = SceneDocumentRow::Kind::Instance;
						rowOk = reader.ReadU64(row.prefab.low) && reader.ReadU64(row.prefab.high);
						rowOk = rowOk && ParseOverrideLayerBinary(reader, row.overrides, *document);
					} else if (rowOk) {
						row.kind = SceneDocumentRow::Kind::Object;
						u64 typeHash = 0;
						rowOk = reader.ReadU64(typeHash);
						row.type = StringHash(typeHash);
						rowOk = rowOk && ReadLengthPrefixedBlob(reader, row.fields.binary);
						u32 componentCount = 0;
						rowOk = rowOk && reader.ReadU32(componentCount);
						for (u32 c = 0; rowOk && c < componentCount; ++c) {
							SceneDocumentComponentRow componentRow;
							u64 componentTypeHash = 0;
							rowOk = reader.ReadU64(componentRow.id) && reader.ReadU64(componentTypeHash);
							componentRow.type = StringHash(componentTypeHash);
							rowOk = rowOk && ReadLengthPrefixedBlob(reader, componentRow.fields.binary);
							if (rowOk) {
								NormalizeOpaqueBlob(componentRow.fields, true);
								row.components.push_back(std::move(componentRow));
							}
						}
					}

					if (rowOk && row.id != 0) {
						document->rows.push_back(std::move(row));
					} else {
						SLT_LOG_WARN("Scene", "binary document: malformed row {} skipped", i);
					}
					if (!reader.Seek(rowStart + rowLength)) {
						ok = false;
					}
				}

				if (!ok) {
					Delete(document);
					return nullptr;
				}
				return document;
			}

		} // anonymous namespace

		SceneDocument* ParseSceneDocument(Buffer&& fileBytes) {
			if (!fileBytes || fileBytes.size == 0) {
				return nullptr;
			}
			if (fileBytes.data[0] == static_cast<u8>('{')) {
				return ParseTextDocument(std::move(fileBytes));
			}
			return ParseBinaryDocument(std::move(fileBytes));
		}

		str WriteSceneDocumentText(const SceneDocument& document) {
			SI::StringPool pool;
			auto pooled = [&pool](str value) -> std::string_view {
				pool.push_back(std::move(value));
				return pool.back();
			};

			JSONValue root = JSONValue::MakeObject();
			root.AddMember("_type", JSONValue::MakeString(document.isPrefab ? "Prefab" : "Scene"));
			root.AddMember("formatVersion", JSONValue::MakeNumber(static_cast<f64>(document.formatVersion)));
			root.AddMember("engineHash", JSONValue::MakeString(pooled(U64ToHex(document.engineHash))));
			root.AddMember("name", JSONValue::MakeString(document.name));
			root.AddMember("nextLocalId", JSONValue::MakeNumber(static_cast<f64>(document.nextLocalId)));

			if (document.isPrefab) {
				JSONValue prefabBlock = JSONValue::MakeObject();
				prefabBlock.AddMember("base", JSONValue::MakeString(pooled(document.basePrefab.ToString())));
				prefabBlock.AddMember("overrides", WriteOverrideLayerJSON(document.prefabOverrides, const_cast<SceneDocument&>(document)));
				root.AddMember("prefab", std::move(prefabBlock));
			}

			JSONValue objects = JSONValue::MakeArray();
			for (const SceneDocumentRow& row : document.rows) {
				JSONValue rowValue = JSONValue::MakeObject();
				rowValue.AddMember("id", JSONValue::MakeNumber(static_cast<f64>(row.id)));
				if (row.kind == SceneDocumentRow::Kind::Instance) {
					rowValue.AddMember("kind", JSONValue::MakeString("instance"));
				}
				rowValue.AddMember("parent", JSONValue::MakeNumber(static_cast<f64>(row.parent)));
				if (row.kind == SceneDocumentRow::Kind::Instance) {
					rowValue.AddMember("prefab", JSONValue::MakeString(pooled(row.prefab.ToString())));
					rowValue.AddMember("overrides", WriteOverrideLayerJSON(row.overrides, const_cast<SceneDocument&>(document)));
				} else {
					rowValue.AddMember("type", JSONValue::MakeString(pooled(U64ToHex(row.type.GetValue()))));
					rowValue.AddMember("fields", row.fields.json);
					JSONValue components = JSONValue::MakeArray();
					for (const SceneDocumentComponentRow& componentRow : row.components) {
						JSONValue componentValue = JSONValue::MakeObject();
						componentValue.AddMember("id", JSONValue::MakeNumber(static_cast<f64>(componentRow.id)));
						componentValue.AddMember("type", JSONValue::MakeString(pooled(U64ToHex(componentRow.type.GetValue()))));
						if (componentRow.fields.isOpaque) {
							JSONValue opaque = JSONValue::MakeObject();
							opaque.AddMember("_opaque", JSONValue::MakeString(pooled(BytesToHex(componentRow.fields.opaque))));
							opaque.AddMember("_opaqueBinary", JSONValue::MakeBool(componentRow.fields.opaqueIsBinary));
							componentValue.AddMember("fields", std::move(opaque));
						} else {
							componentValue.AddMember("fields", componentRow.fields.json);
						}
						components.Push(std::move(componentValue));
					}
					rowValue.AddMember("components", std::move(components));
				}
				objects.Push(std::move(rowValue));
			}
			root.AddMember("objects", std::move(objects));

			return JSONSerialize(root, true);
		}

		namespace {

			void BuildOpaqueRecord(std::vector<u8>& out, std::span<const u8> payload, bool payloadIsBinary) {
				out.clear();
				out.resize(sizeof(u64) + sizeof(u8) + payload.size());
				u64 sentinel = SLT_OPAQUE_SENTINEL;
				std::memcpy(out.data(), &sentinel, sizeof(sentinel));
				out[sizeof(u64)] = payloadIsBinary ? 1 : 0;
				if (!payload.empty()) {
					std::memcpy(out.data() + sizeof(u64) + sizeof(u8), payload.data(), payload.size());
				}
			}

			struct TranscodeScopeCtx {
				std::vector<std::pair<Guid, StringHash>> pairs;
			};

			bool TranscodeToHandle(void* user, const Guid& guid, StringHash pointeeType, u64* outId) {
				TranscodeScopeCtx* ctx = static_cast<TranscodeScopeCtx*>(user);
				for (size_t i = 0; i < ctx->pairs.size(); ++i) {
					if (ctx->pairs[i].first == guid && ctx->pairs[i].second == pointeeType) {
						*outId = i + 1;
						return true;
					}
				}
				ctx->pairs.push_back({guid, pointeeType});
				*outId = ctx->pairs.size();
				return true;
			}

			bool TranscodeToGuid(void* user, u64 instanceId, Guid* outGuid, StringHash* outPointeeType) {
				TranscodeScopeCtx* ctx = static_cast<TranscodeScopeCtx*>(user);
				if (instanceId == 0 || instanceId > ctx->pairs.size()) {
					*outGuid = Guid{};
					*outPointeeType = StringHash();
					return true;
				}
				*outGuid = ctx->pairs[instanceId - 1].first;
				*outPointeeType = ctx->pairs[instanceId - 1].second;
				return true;
			}

			class ScratchRow {
			public:
				ScratchRow(BlobRow rowKind, const TypeInfo* componentType) {
					_root = SceneInternalAccess::NewSceneObject();
					if (rowKind == BlobRow::Object) {
						_instance = _root;
						_type = SceneObject::GetReflectType();
						return;
					}
					Component* component = componentType ? CreateComponentInstance(componentType) : nullptr;
					if (!component) {
						return;
					}
					AttachComponentRaw(*_root, componentType, *component);
					_instance = component;
					_type = componentType;
				}

				~ScratchRow() { FinalizeSubtree(*_root, false); }

				ScratchRow(const ScratchRow&) = delete;
				ScratchRow& operator=(const ScratchRow&) = delete;

				void* Instance() const { return _instance; }
				const TypeInfo* Type() const { return _type; }
			private:
				SceneObject* _root = nullptr;
				void* _instance = nullptr;
				const TypeInfo* _type = nullptr;
			};

		} // anonymous namespace

		bool EncodeBlobBinary(const SceneDocument& document, const SceneDocumentBlob& blob, StringHash rowType,
							  BlobRow rowKind, std::vector<u8>& scratch, std::span<const u8>& out) {
			if (blob.isOpaque) {
				BuildOpaqueRecord(scratch, blob.opaque, blob.opaqueIsBinary);
				out = scratch;
				return true;
			}
			if (document.binary) {
				out = blob.binary;
				return true;
			}

			ScratchRow row(rowKind, TypeRegistry::FindType(rowType));
			if (!row.Instance()) {
				ReportUnconvertedRow(rowType);
				str text = JSONSerialize(blob.json, false);
				BuildOpaqueRecord(scratch, {reinterpret_cast<const u8*>(text.data()), text.size()}, false);
				out = scratch;
				return true;
			}

			TranscodeScopeCtx ctx;
			std::vector<SI::HandleFixupJob> fixups;
			SI::BinaryWriter writer;
			{
				ScopedHandleResolver scope({&TranscodeToGuid, &TranscodeToHandle, &ctx});
				ReadBlob(document, blob, row.Instance(), row.Type(), fixups);
				SI::RunHandleFixups(fixups);
				if (!SI::BinaryWriteObject(writer, row.Instance(), row.Type(), 0)) {
					return false;
				}
			}
			scratch.assign(writer.Data().begin(), writer.Data().end());
			out = scratch;
			return true;
		}

		Buffer WriteSceneDocumentBinary(const SceneDocument& document) {
			SI::BinaryWriter writer;
			writer.WriteU32(SCENE_MAGIC);
			writer.WriteU16(SCENE_FORMAT_VERSION);
			writer.WriteU16(document.isPrefab ? SCENE_FLAG_PREFAB : 0);
			writer.WriteU64(document.engineHash);
			writer.WriteU64(document.nextLocalId);
			writer.WriteU32(static_cast<u32>(document.name.size()));
			writer.WriteBytes(document.name.data(), document.name.size());

			std::vector<u8> scratch;
			std::span<const u8> record;
			auto writeFieldRecord = [&](const SceneDocumentBlob& blob, StringHash type, BlobRow kind) {
				if (!EncodeBlobBinary(document, blob, type, kind, scratch, record)) {
					return false;
				}
				writer.WriteU32(static_cast<u32>(record.size()));
				writer.WriteBytes(record.data(), record.size());
				return true;
			};

			if (document.isPrefab) {
				writer.WriteU64(document.basePrefab.low);
				writer.WriteU64(document.basePrefab.high);
				if (!WriteOverrideLayerBinary(writer, document.prefabOverrides, document)) {
					return Buffer();
				}
			}

			writer.WriteU32(static_cast<u32>(document.rows.size()));
			for (const SceneDocumentRow& row : document.rows) {
				size_t rowPatch = writer.BeginPatch();
				writer.WriteU64(row.id);
				writer.WriteU8(static_cast<u8>(row.kind));
				writer.WriteU64(row.parent);
				if (row.kind == SceneDocumentRow::Kind::Instance) {
					writer.WriteU64(row.prefab.low);
					writer.WriteU64(row.prefab.high);
					if (!WriteOverrideLayerBinary(writer, row.overrides, document)) {
						return Buffer();
					}
				} else {
					writer.WriteU64(row.type.GetValue());
					if (!writeFieldRecord(row.fields, row.type, BlobRow::Object)) {
						return Buffer();
					}
					writer.WriteU32(static_cast<u32>(row.components.size()));
					for (const SceneDocumentComponentRow& componentRow : row.components) {
						const TypeInfo* resolvedCompType = TypeRegistry::FindType(componentRow.type);
						const StringHash canonicalCompType = resolvedCompType ? resolvedCompType->name : componentRow.type;
						writer.WriteU64(componentRow.id);
						writer.WriteU64(canonicalCompType.GetValue());
						if (!writeFieldRecord(componentRow.fields, canonicalCompType, BlobRow::Component)) {
							return Buffer();
						}
					}
				}
				writer.EndPatch(rowPatch);
			}

			Buffer buffer(writer.Size(), MemTag::Scene);
			if (writer.Size() > 0) {
				std::memcpy(buffer.data, writer.Data().data(), writer.Size());
			}
			return buffer;
		}

		namespace {

			SceneDocumentBlob BuildBlob(SceneDocument& document, const void* instance, const TypeInfo* type) {
				SceneDocumentBlob blob;
				if (document.binary) {
					SI::BinaryWriter writer;
					SI::BinaryWriteObject(writer, instance, type, 0);
					blob.binary = writer.Data();
				} else {
					blob.json = SI::TextBuildObject(instance, type, document.pool, 0);
				}
				return blob;
			}

			SceneDocumentRow BuildObjectRow(SceneDocument& document, SceneObject& node, u64 parentRowId) {
				SceneDocumentRow row;
				row.id = node.GetLocalId();
				row.parent = parentRowId;
				row.kind = SceneDocumentRow::Kind::Object;
				row.type = SceneObject::GetReflectType()->name;
				row.fields = BuildBlob(document, &node, SceneObject::GetReflectType());
				for (const auto& slot : SceneInternalAccess::ComponentsOf(node)) {
					if (slot.ptr->IsPendingDestroy()) {
						continue;
					}
					row.components.push_back(BuildComponentRow(document, *slot.ptr, slot.type, slot.ptr->GetLocalId()));
				}
				return row;
			}

			void MintAndMapSubtree(SceneObject& node, Scene& scene, SaveScopeCtx& ctx) {
				if (node.IsPendingDestroy()) {
					return;
				}
				bool prefabSourced = SceneInternalAccess::PrefabKeyOf(node) != 0 ||
									 SceneInternalAccess::PrefabRecordOf(node) != 0;
				if (!prefabSourced) {
					u64& localId = SceneInternalAccess::LocalIdOf(node);
					if (localId == 0) {
						localId = SceneInternalAccess::NextLocalIdOf(scene)++;
					}
					ctx.map[node.GetInstanceId()] = {localId, SceneObject::GetReflectType()->name.GetValue()};
				}
				for (const auto& slot : SceneInternalAccess::ComponentsOf(node)) {
					Component& component = *slot.ptr;
					if (component.IsPendingDestroy() || SceneInternalAccess::PrefabKeyOf(component) != 0) {
						continue;
					}
					u64& componentLocalId = SceneInternalAccess::LocalIdOf(component);
					if (componentLocalId == 0) {
						componentLocalId = SceneInternalAccess::NextLocalIdOf(scene)++;
					}
					ctx.map[component.GetInstanceId()] = {componentLocalId, slot.type->name.GetValue()};
				}
				for (SceneObject* child : SceneInternalAccess::ChildrenOf(node)) {
					MintAndMapSubtree(*child, scene, ctx);
				}
			}

			void EmitSubtree(SceneObject& node, u64 parentRowId, SceneDocument& document, Scene& scene) {
				if (node.IsPendingDestroy()) {
					return;
				}
				if (SceneInternalAccess::PrefabRecordOf(node) != 0) {
					EmitInstanceRow(document, scene, node, parentRowId);
					return;
				}
				if (SceneInternalAccess::PrefabKeyOf(node) != 0) {
					return;
				}

				SceneDocumentRow row = BuildObjectRow(document, node, parentRowId);
				u64 rowId = row.id;
				document.rows.push_back(std::move(row));

				for (SceneObject* child : SceneInternalAccess::ChildrenOf(node)) {
					EmitSubtree(*child, rowId, document, scene);
				}
			}

		} // anonymous namespace

		str DeclaredSpellingOf(UnresolvedComponent& preserved) {
			if (SceneInternalAccess::BlobIsBinaryOf(preserved)) {
				return {};
			}
			const str& text = SceneInternalAccess::BlobTextOf(preserved);
			if (text.empty()) {
				return {};
			}
			LinearAllocator arena(text.size() + 64, MemTag::Scene);
			JSONValue fields = JSONParse(std::string_view(text), arena);
			if (!fields.IsObject()) {
				return {};
			}
			std::string_view spelling = fields["_type"].AsString();
			const StringHash declared = SceneInternalAccess::UnresolvedTypeOf(preserved);
			if (spelling.empty() || spelling == std::format("{:016x}", declared.GetValue())) {
				return {};
			}
			return str(spelling);
		}

		void CountPreservedOnNode(SceneObject& node, u32& rows, std::vector<UnresolvedTypeMention>& types) {
			for (const auto& slot : SceneInternalAccess::ComponentsOf(node)) {
				if (!IsPreservedComponentType(slot.type) || slot.ptr == nullptr) {
					continue;
				}
				++rows;
				auto& preserved = static_cast<UnresolvedComponent&>(*slot.ptr);
				StringHash declared = SceneInternalAccess::UnresolvedTypeOf(preserved);
				auto known = std::find_if(types.begin(), types.end(),
										  [declared](const UnresolvedTypeMention& seen) { return seen.type == declared; });
				if (known == types.end()) {
					types.push_back({declared, DeclaredSpellingOf(preserved), 1});
					continue;
				}
				++known->rows;
				if (known->spelling.empty()) {
					known->spelling = DeclaredSpellingOf(preserved);
				}
			}
		}

		void CountPreservedInSubtree(SceneObject& node, u32& rows, std::vector<UnresolvedTypeMention>& types) {
			CountPreservedOnNode(node, rows, types);
			for (SceneObject* child : SceneInternalAccess::ChildrenOf(node)) {
				CountPreservedInSubtree(*child, rows, types);
			}
		}

		void ReportUnresolvedRows(u32 rows, const std::vector<UnresolvedTypeMention>& types, std::string_view what) {
			if (rows == 0) {
				return;
			}
			{
				std::lock_guard lock(_holdMutex);
				if (_hold) {
					_hold->HoldRows(what, types);
					return;
				}
			}
			LogUnresolvedRows(rows, types, what);
		}

		void ReportPreservedRows(const SceneDocument& document, std::string_view what) {
			if (document.preservedRows == 0) {
				return;
			}
			SLT_LOG_WARN("Scene", "'{}' saved with {} component row(s) of an unloaded type. Written back unchanged",
						 what, document.preservedRows);
		}

		SceneDocumentComponentRow BuildComponentRow(SceneDocument& document, Component& component,
													const TypeInfo* type, u64 rowId) {
			SceneDocumentComponentRow row;
			row.id = rowId;
			if (IsPreservedComponentType(type)) {
				++document.preservedRows;
				auto& preserved = static_cast<UnresolvedComponent&>(component);
				row.type = SceneInternalAccess::UnresolvedTypeOf(preserved);
				row.fields.isOpaque = true;
				row.fields.opaqueIsBinary = SceneInternalAccess::BlobIsBinaryOf(preserved);
				if (row.fields.opaqueIsBinary) {
					const Vector<u8>& bytes = SceneInternalAccess::BlobBytesOf(preserved);
					row.fields.opaque.assign(bytes.begin(), bytes.end());
				} else {
					const str& text = SceneInternalAccess::BlobTextOf(preserved);
					row.fields.opaque.assign(text.begin(), text.end());
				}
				return row;
			}
			row.type = type->name;
			row.fields = BuildBlob(document, &component, type);
			return row;
		}

		SceneDocument* BuildDocumentFromScene(Scene& scene, bool binary) {
			SceneDocument* document = New<SceneDocument>(MemTag::Scene);
			document->binary = binary;
			document->formatVersion = SCENE_FORMAT_VERSION;
			document->engineHash = BuildInfo::GetEngineHash();
			document->name = scene.name;

			SaveScopeCtx ctx;
			for (SceneObject* root : SceneInternalAccess::RootsOf(scene)) {
				MintAndMapSubtree(*root, scene, ctx);
			}

			ScopedHandleResolver scope({&SaveScopeToGuid, nullptr, &ctx});
			for (SceneObject* root : SceneInternalAccess::RootsOf(scene)) {
				EmitSubtree(*root, 0, *document, scene);
			}
			document->nextLocalId = SceneInternalAccess::NextLocalIdOf(scene);
			ReportPreservedRows(*document, scene.name);
			return document;
		}

		SceneDocument* BuildDocumentFromSubtree(SceneObject& root, bool binary) {
			SceneDocument* document = New<SceneDocument>(MemTag::Scene);
			document->binary = binary;
			document->formatVersion = SCENE_FORMAT_VERSION;
			document->engineHash = BuildInfo::GetEngineHash();
			document->name = root.name;
			document->isPrefab = true;

			u64 nextId = 1;
			SaveScopeCtx ctx;
			struct Minter {
				u64* nextId;
				SaveScopeCtx* ctx;
				void Visit(SceneObject& node) {
					if (node.IsPendingDestroy()) {
						return;
					}
					SceneInternalAccess::LocalIdOf(node) = (*nextId)++;
					ctx->map[node.GetInstanceId()] = {node.GetLocalId(), SceneObject::GetReflectType()->name.GetValue()};
					for (const auto& slot : SceneInternalAccess::ComponentsOf(node)) {
						if (slot.ptr->IsPendingDestroy()) {
							continue;
						}
						SceneInternalAccess::LocalIdOf(*slot.ptr) = (*nextId)++;
						ctx->map[slot.ptr->GetInstanceId()] = {slot.ptr->GetLocalId(), slot.type->name.GetValue()};
					}
					for (SceneObject* child : SceneInternalAccess::ChildrenOf(node)) {
						Visit(*child);
					}
				}
			};
			Minter minter{&nextId, &ctx};
			minter.Visit(root);

			ScopedHandleResolver scope({&SaveScopeToGuid, nullptr, &ctx});
			struct Emitter {
				SceneDocument* document;
				void Visit(SceneObject& node, u64 parentRowId) {
					if (node.IsPendingDestroy()) {
						return;
					}
					SceneDocumentRow row = BuildObjectRow(*document, node, parentRowId);
					u64 rowId = row.id;
					document->rows.push_back(std::move(row));
					for (SceneObject* child : SceneInternalAccess::ChildrenOf(node)) {
						Visit(*child, rowId);
					}
				}
			};
			Emitter emitter{document};
			emitter.Visit(root, 0);
			document->nextLocalId = nextId;
			ReportPreservedRows(*document, root.name);
			return document;
		}

		SceneDocument* BuildVariantDocument(const Guid& basePrefab, const Path& path) {
			SceneDocument* document = New<SceneDocument>(MemTag::Scene);
			document->binary = false;
			document->formatVersion = SCENE_FORMAT_VERSION;
			document->engineHash = BuildInfo::GetEngineHash();
			document->name = SceneNameFromPath(path);
			document->isPrefab = true;
			document->basePrefab = basePrefab;
			document->nextLocalId = 1;
			return document;
		}

		FragmentBuilder::FragmentBuilder(std::string_view name, PrefabKind kind)
			: _kind(kind) {
			_document = New<SceneDocument>(MemTag::Scene);
			_document->binary = false;
			_document->formatVersion = SCENE_FORMAT_VERSION;
			_document->engineHash = BuildInfo::GetEngineHash();
			_document->name = str(name);
			_document->isPrefab = true;
		}

		FragmentBuilder::~FragmentBuilder() {
			if (_current) {
				FinalizeSubtree(*_current, false);
				_current = nullptr;
			}
			if (_document) {
				Delete(_document);
				_document = nullptr;
			}
		}

		SceneObject& FragmentBuilder::BeginObject() {
			SLT_CORE_ASSERT_MSG(!_current, "FragmentBuilder: BeginObject without EndObject");
			if (_current) {
				return *_current;
			}
			_current = SceneInternalAccess::NewSceneObject();
			return *_current;
		}

		Component* FragmentBuilder::AddComponent(const TypeInfo* type) {
			SLT_CORE_ASSERT_MSG(_current, "FragmentBuilder: AddComponent outside a row");
			if (!_current) {
				return nullptr;
			}
			Component* component = CreateComponentInstance(type);
			if (!component) {
				return nullptr;
			}
			AttachComponentRaw(*_current, type, *component);
			return component;
		}

		void FragmentBuilder::SetHandleToRowRaw(void* handleField, u64 rowId, const TypeInfo* pointeeType) {
			if (!handleField || rowId == 0) {
				return;
			}
			const u64 tagged = FRAGMENT_ROW_HANDLE_TAG | rowId;
			std::memcpy(handleField, &tagged, sizeof(u64));

			const u64 typeHash = pointeeType ? pointeeType->name.GetValue() : 0;
			for (auto& entry : _rowHandleTypes) {
				if (entry.first == rowId) {
					entry.second = typeHash;
					return;
				}
			}
			_rowHandleTypes.push_back({rowId, typeHash});
		}

		bool FragmentBuilder::RowScopeToGuid(void* user, u64 instanceId, Guid* outGuid, StringHash* outPointeeType) {
			if ((instanceId & FRAGMENT_ROW_HANDLE_TAG) == 0) {
				return false;
			}
			FragmentBuilder& self = *static_cast<FragmentBuilder*>(user);
			const u64 rowId = instanceId & ~FRAGMENT_ROW_HANDLE_TAG;
			*outGuid = Guid{rowId, SLT_LOCAL_SENTINEL};
			*outPointeeType = StringHash();
			for (const auto& entry : self._rowHandleTypes) {
				if (entry.first == rowId) {
					*outPointeeType = StringHash(entry.second);
					break;
				}
			}
			return true;
		}

		void FragmentBuilder::EndObject(u64 rowId, u64 parentRowId, std::span<const u64> componentRowIds) {
			SLT_CORE_ASSERT_MSG(_current, "FragmentBuilder: EndObject without BeginObject");
			if (!_current) {
				return;
			}
			SceneInternalAccess::LocalIdOf(*_current) = rowId;
			_maxRowId = std::max(_maxRowId, rowId);
			size_t componentIndex = 0;
			for (const auto& slot : SceneInternalAccess::ComponentsOf(*_current)) {
				SLT_CORE_ASSERT_MSG(componentIndex < componentRowIds.size(), "FragmentBuilder: a component row has no id");
				const u64 componentRowId = componentIndex < componentRowIds.size() ? componentRowIds[componentIndex] : 0;
				SceneInternalAccess::LocalIdOf(*slot.ptr) = componentRowId;
				_maxRowId = std::max(_maxRowId, componentRowId);
				++componentIndex;
			}

			{
				ScopedHandleResolver scope({&FragmentBuilder::RowScopeToGuid, nullptr, this});
				_document->rows.push_back(BuildObjectRow(*_document, *_current, parentRowId));
			}

			FinalizeSubtree(*_current, false);
			_current = nullptr;
		}

		PrefabAsset* FragmentBuilder::Release() {
			SLT_CORE_ASSERT_MSG(!_current, "FragmentBuilder: Release inside a row");
			if (!_document || _document->rows.empty()) {
				return nullptr;
			}
			_document->nextLocalId = _maxRowId + 1;

			PrefabAsset* asset = New<PrefabAsset>(MemTag::Asset);
			SceneInternalAccess::FragmentOf(*asset) = _document;
			SceneInternalAccess::KindOf(*asset) = _kind;
			_document = nullptr;
			return asset;
		}

		std::unordered_set<u64> ComputeLayerOwnedRows(const SceneDocument& document) {
			std::unordered_set<u64> owned;
			std::vector<u64> pending;
			auto collectLayer = [&pending](const OverrideLayer& layer) {
				for (const AddedObjectOp& op : layer.addedObjects) {
					if (op.rootRowId != 0) {
						pending.push_back(op.rootRowId);
					}
				}
			};
			collectLayer(document.prefabOverrides);
			for (const SceneDocumentRow& row : document.rows) {
				if (row.kind == SceneDocumentRow::Kind::Instance) {
					collectLayer(row.overrides);
				}
			}
			if (pending.empty()) {
				return owned;
			}
			std::unordered_multimap<u64, u64> childrenOf;
			for (const SceneDocumentRow& row : document.rows) {
				if (row.parent != 0) {
					childrenOf.emplace(row.parent, row.id);
				}
			}
			while (!pending.empty()) {
				u64 id = pending.back();
				pending.pop_back();
				if (!owned.insert(id).second) {
					continue;
				}
				auto range = childrenOf.equal_range(id);
				for (auto it = range.first; it != range.second; ++it) {
					pending.push_back(it->second);
				}
			}
			return owned;
		}

		namespace {

			void RegisterSingleNode(SceneObject& node, Scene& scene, SceneObject* parent) {
				SceneInternalAccess::ParentOf(node) = parent;
				Vector<SceneObject*>& siblings = parent ? SceneInternalAccess::ChildrenOf(*parent)
														: SceneInternalAccess::RootsOf(scene);
				siblings.push_back(&node);
				SceneInternalAccess::SceneOf(node) = &scene;
				SceneInternalAccess::ObjectsOf(scene).push_back(&node);
				GetState().ownerById[node.GetInstanceId()] = &scene;
				for (const auto& slot : SceneInternalAccess::ComponentsOf(node)) {
					GetState().ownerById[slot.ptr->GetInstanceId()] = &scene;
				}
			}

		} // anonymous namespace

		void ReadBlob(const SceneDocument& document, const SceneDocumentBlob& blob, void* instance,
					  const TypeInfo* type, std::vector<SI::HandleFixupJob>& fixups) {
			if (blob.isOpaque) {
				ApplyPreservedBlob(blob.opaque, blob.opaqueIsBinary, instance, type, fixups);
				return;
			}
			if (document.binary) {
				SI::BinaryReader reader(blob.binary.data(), blob.binary.size());
				SI::BinaryReadObject(reader, instance, type, 0, fixups);
			} else if (blob.json.IsObject()) {
				SI::TextReadObject(instance, type, blob.json, 0);
			}
		}

		struct SceneIntegration {
			const SceneDocument* document = nullptr;
			Scene* scene = nullptr;

			std::unordered_set<u64> layerOwned;
			std::unordered_set<u64> skipped;

			struct BuiltRow {
				const SceneDocumentRow* row;
				SceneObject* object;
			};
			std::vector<BuiltRow> built;
			std::vector<SceneObject*> composedRoots;
			std::unordered_map<u64, SceneObject*> objectByRowId;
			LoadScopeCtx loadCtx;
			std::vector<SI::HandleFixupJob> fixups;

			enum class Phase : u8 {
				Construct,
				Link,
				Populate,
				Finalize,
				Done,
			};
			Phase phase = Phase::Construct;
			size_t constructCursor = 0;
			size_t linkCursor = 0;
			size_t populateCursor = 0;
		};

		SceneIntegration* CreateSceneIntegration(const SceneDocument& document, Scene& scene) {
			SceneIntegration* integration = New<SceneIntegration>(MemTag::Scene);
			integration->document = &document;
			integration->scene = &scene;
			integration->loadCtx.scene = &scene;
			integration->layerOwned = ComputeLayerOwnedRows(document);
			integration->built.reserve(document.rows.size());
			return integration;
		}

		namespace {

			template <typename T>
			bool ClaimRowId(SceneIntegration& it, u64 rowId, T& target) {
				if (rowId == 0 || it.loadCtx.map.contains(rowId)) {
					if (rowId != 0) {
						SLT_LOG_ERROR("Scene", "duplicate row id {} in scene '{}' (later row loads unsaved, references to {} resolve to the 1st)",
									  rowId, it.scene->name, rowId);
					}
					SceneInternalAccess::LocalIdOf(target) = 0;
					return false;
				}
				SceneInternalAccess::LocalIdOf(target) = rowId;
				it.loadCtx.map[rowId] = target.GetInstanceId();
				return true;
			}

			void IntegrationConstructOne(SceneIntegration& it) {
				const SceneDocumentRow& row = it.document->rows[it.constructCursor++];
				if (it.layerOwned.count(row.id)) {
					return;
				}
				if (row.parent != 0 && it.skipped.count(row.parent)) {
					it.skipped.insert(row.id);
					return;
				}
				if (row.kind == SceneDocumentRow::Kind::Instance) {
					it.built.push_back({&row, nullptr});
					return;
				}

				const TypeInfo* type = TypeRegistry::FindType(row.type);
				if (!type || !type->createFn || !TypeRegistry::IsDerivedFrom(type, SceneObject::GetReflectType())) {
					SLT_LOG_WARN("Scene", "row {}: unknown or non-entity type hash {:x}. Subtree skipped", row.id, row.type.GetValue());
					it.skipped.insert(row.id);
					return;
				}
				SceneObject* object = static_cast<SceneObject*>(TypeRegistry::CreateObjectInstance(type, MemTag::Scene));
				if (ClaimRowId(it, row.id, *object)) {
					it.objectByRowId[row.id] = object;
				}

				for (const SceneDocumentComponentRow& componentRow : row.components) {
					const TypeInfo* componentType = TypeRegistry::FindType(componentRow.type);
					Component* component = componentType ? CreateComponentInstance(componentType) : nullptr;
					if (!component) {
						component = MakePreservedComponent(*it.document, componentRow);
						componentType = UnresolvedComponent::GetReflectType();
					}
					AttachComponentRaw(*object, componentType, *component);
					ClaimRowId(it, componentRow.id, *component);
				}
				it.built.push_back({&row, object});
			}

			void IntegrationLinkOne(SceneIntegration& it) {
				const SceneIntegration::BuiltRow& entry = it.built[it.linkCursor++];
				SceneObject* parent = nullptr;
				if (entry.row->parent != 0) {
					auto found = it.objectByRowId.find(entry.row->parent);
					if (found == it.objectByRowId.end()) {
						SLT_LOG_WARN("Scene", "row {}: parent {} missing. Linking as root", entry.row->id, entry.row->parent);
					} else {
						parent = found->second;
					}
				}
				if (entry.row->kind == SceneDocumentRow::Kind::Instance) {
					ScopedHandleResolver scope({nullptr, &LoadScopeToHandle, &it.loadCtx});
					if (SceneObject* composed = ComposeInstanceRow(*it.document, *entry.row, *it.scene, parent)) {
						it.composedRoots.push_back(composed);
					}
					return;
				}
				if (entry.object) {
					RegisterSingleNode(*entry.object, *it.scene, parent);
				}
			}

			void IntegrationPopulateOne(SceneIntegration& it) {
				const SceneIntegration::BuiltRow& entry = it.built[it.populateCursor++];
				if (!entry.object) {
					return;
				}
				ScopedHandleResolver scope({nullptr, &LoadScopeToHandle, &it.loadCtx});
				ReadBlob(*it.document, entry.row->fields, entry.object, SceneObject::GetReflectType(), it.fixups);
				auto& slots = SceneInternalAccess::ComponentsOf(*entry.object);
				for (size_t slotIndex = 0; slotIndex < slots.size() && slotIndex < entry.row->components.size(); ++slotIndex) {
					const TypeInfo* slotType = slots[slotIndex].type;
					if (slotType == UnresolvedComponent::GetReflectType()) {
						continue;
					}
					ReadBlob(*it.document, entry.row->components[slotIndex].fields, slots[slotIndex].ptr, slotType, it.fixups);
				}
			}

			void IntegrationFinalize(SceneIntegration& it) {
				{
					ScopedHandleResolver scope({nullptr, &LoadScopeToHandle, &it.loadCtx});
					SI::RunHandleFixups(it.fixups);
				}
				u64 maxRowId = 0;
				for (const SceneIntegration::BuiltRow& entry : it.built) {
					maxRowId = std::max(maxRowId, entry.row->id);
					for (const SceneDocumentComponentRow& componentRow : entry.row->components) {
						maxRowId = std::max(maxRowId, componentRow.id);
					}
					if (entry.object) {
						SceneInternalAccess::DeriveScaleKind(*entry.object);
						SceneInternalAccess::MarkWorldDirty(*entry.object);
					}
				}
				u64& nextLocalId = SceneInternalAccess::NextLocalIdOf(*it.scene);
				nextLocalId = std::max({nextLocalId, it.document->nextLocalId, maxRowId + 1});

				u32 unresolvedRows = 0;
				std::vector<UnresolvedTypeMention> unresolvedTypes;
				for (const SceneIntegration::BuiltRow& entry : it.built) {
					if (entry.object) {
						CountPreservedOnNode(*entry.object, unresolvedRows, unresolvedTypes);
					}
				}
				for (SceneObject* composed : it.composedRoots) {
					CountPreservedInSubtree(*composed, unresolvedRows, unresolvedTypes);
				}
				ReportUnresolvedRows(unresolvedRows, unresolvedTypes, it.scene->name);
			}

		} // anonymous namespace

		bool StepSceneIntegration(SceneIntegration& integration, std::chrono::steady_clock::time_point deadline) {
			do {
				switch (integration.phase) {
				case SceneIntegration::Phase::Construct:
					if (integration.constructCursor < integration.document->rows.size()) {
						IntegrationConstructOne(integration);
					} else {
						integration.phase = SceneIntegration::Phase::Link;
					}
					break;
				case SceneIntegration::Phase::Link:
					if (integration.linkCursor < integration.built.size()) {
						IntegrationLinkOne(integration);
					} else {
						integration.phase = SceneIntegration::Phase::Populate;
					}
					break;
				case SceneIntegration::Phase::Populate:
					if (integration.populateCursor < integration.built.size()) {
						IntegrationPopulateOne(integration);
					} else {
						integration.phase = SceneIntegration::Phase::Finalize;
					}
					break;
				case SceneIntegration::Phase::Finalize:
					IntegrationFinalize(integration);
					integration.phase = SceneIntegration::Phase::Done;
					return true;
				case SceneIntegration::Phase::Done:
					return true;
				}
			} while (std::chrono::steady_clock::now() < deadline);
			return integration.phase == SceneIntegration::Phase::Done;
		}

		f32 GetSceneIntegrationProgress(const SceneIntegration& integration) {
			size_t rowCount = std::max<size_t>(integration.document->rows.size(), 1);
			size_t builtCount = std::max<size_t>(integration.built.size(), 1);
			f32 construct = static_cast<f32>(integration.constructCursor) / static_cast<f32>(rowCount);
			f32 link = static_cast<f32>(integration.linkCursor) / static_cast<f32>(builtCount);
			f32 populate = static_cast<f32>(integration.populateCursor) / static_cast<f32>(builtCount);
			f32 progress = construct * 0.4f + link * 0.15f + populate * 0.4f;
			if (integration.phase == SceneIntegration::Phase::Done) {
				progress = 1.0f;
			}
			return progress;
		}

		void DestroySceneIntegration(SceneIntegration* integration) {
			Delete(integration);
		}

		bool InstantiateDocument(const SceneDocument& document, Scene& scene) {
			SceneIntegration* integration = CreateSceneIntegration(document, scene);
			while (!StepSceneIntegration(*integration, std::chrono::steady_clock::time_point::max())) {
			}
			DestroySceneIntegration(integration);
			return true;
		}

		namespace {

			class SceneAssetImporter final : public IAssetImporter {
			public:
				StringHash GetAssetType() const override { return StringHash("SceneAsset"); }
				Asset* Import(const Path& sourcePath) override {
					SceneAsset* asset = New<SceneAsset>(MemTag::Asset);
					if (!asset->Import(sourcePath)) {
						Delete(asset);
						return nullptr;
					}
					return asset;
				}

				bool CanCreateAsset() const override { return true; }
				std::string_view GetCreateMenuLabel() const override { return "Scene"; }
				std::string_view GetCreateDefaultFileName() const override { return "New Scene.scene"; }
				Asset* CreateAsset() override {
					SceneAsset* asset = New<SceneAsset>(MemTag::Asset);
					SceneInternalAccess::DocumentOf(*asset) = New<SceneDocument>(MemTag::Scene);
					return asset;
				}
			};

			class PrefabAssetImporter final : public IAssetImporter {
			public:
				StringHash GetAssetType() const override { return StringHash("PrefabAsset"); }
				Asset* Import(const Path& sourcePath) override {
					PrefabAsset* asset = New<PrefabAsset>(MemTag::Asset);
					if (!asset->Import(sourcePath)) {
						Delete(asset);
						return nullptr;
					}
					return asset;
				}
			};

		} // anonymous namespace

		namespace {

			SceneAssetImporter& GetSceneImporter() {
				static SceneAssetImporter importer;
				return importer;
			}
			PrefabAssetImporter& GetPrefabImporter() {
				static PrefabAssetImporter importer;
				return importer;
			}

		} // anonymous namespace

		void RegisterSceneImporters() {
			AssetImporterRegistry::Register(StringHash("SceneAsset"), &GetSceneImporter(), {".scene"});
			AssetImporterRegistry::Register(StringHash("PrefabAsset"), &GetPrefabImporter(), {".prefab"});
		}

		void UnregisterSceneImporters() {
			AssetImporterRegistry::Unregister(&GetSceneImporter());
			AssetImporterRegistry::Unregister(&GetPrefabImporter());
		}

	} // namespace SceneInternal

	void SceneSystem::ReleaseUnresolvedComponentReports() {
		SceneInternal::ReleaseUnresolvedReportHold(false);
	}

	bool SceneSystem::SaveScene(Handle<Scene> sceneHandle, const Path& path, bool binary) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SLT_CORE_ASSERT_MSG(IsStructuralMutationLegal(), "SaveScene is illegal inside lifecycle hooks");
		Scene* scene = sceneHandle.Get();
		if (!GetState().initialized || !IsStructuralMutationLegal() || !scene || scene->GetState() != SceneState::Loaded) {
			return false;
		}

		if (str named = SceneNameFromPath(path); !named.empty()) {
			scene->name = named;
		}

		// instance layers must be current before rows emit
		RefreshInstanceOverrides(*scene);

		SceneDocument* document = BuildDocumentFromScene(*scene, binary);

		const Guid source = SceneInternalAccess::SourceGuidOf(*scene);
		Guid identity = AssetDatabase::LookupGuid(path);
		if (!identity.IsValid() && source.IsValid() && AssetDatabase::LookupPath(source).ToString().empty()) {
			identity = source;
		}
		SceneAsset* asset = New<SceneAsset>(MemTag::Scene);
		asset->virtualPath = path.ToString();
		asset->assetId = identity;
		asset->_document = document;
		AssetDatabase::SaveAsset(asset);
		const Guid saved = asset->assetId;
		SceneInternalAccess::SourceGuidOf(*scene) = saved;
		Delete(asset);

		Asset* loaded = AssetDatabase::FindLoadedByGuid(saved);
		if (loaded && loaded->GetAssetTypeId() == SceneAsset::GetReflectType()->name) {
			loaded->Import(FileSystem::Resolve(path));
		}

		return FileSystem::Exists(FileSystem::Resolve(path));
	}

	Handle<Scene> SceneSystem::LoadScene(const Path& path) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SLT_CORE_ASSERT_MSG(IsStructuralMutationLegal(), "LoadScene is illegal inside lifecycle hooks");
		if (!GetState().initialized || !IsStructuralMutationLegal()) {
			return Handle<Scene>();
		}

		Buffer bytes = FileSystem::ReadFile(FileSystem::Resolve(path), MemTag::Scene);
		if (!bytes) {
			SLT_LOG_ERROR("Scene", "LoadScene: can't read '{}'", path.ToString());
			return Handle<Scene>();
		}
		SceneDocument* document = ParseSceneDocument(std::move(bytes));
		if (!document) {
			SLT_LOG_ERROR("Scene", "LoadScene: malformed document '{}'", path.ToString());
			return Handle<Scene>();
		}
		if (document->isPrefab) {
			SLT_LOG_ERROR("Scene", "LoadScene: '{}' is a prefab document", path.ToString());
			Delete(document);
			return Handle<Scene>();
		}

		Scene* scene = CreateSceneInternal(SceneNameFromPath(path), false);
		SceneInternalAccess::SourceGuidOf(*scene) = AssetDatabase::LookupGuid(path);
		InstantiateDocument(*document, *scene);
		Delete(document);

		MakeSceneVisible(*scene);
		for (SceneObject* root : SceneInternalAccess::RootsOf(*scene)) {
			DeriveActiveSubtree(*root);
		}
		for (SceneObject* root : SceneInternalAccess::RootsOf(*scene)) {
			DispatchIslandAttach(*root);
		}

		Handle<Scene> handle(scene);
		SceneEvents::Loaded.Publish(EventBus::GetGlobal(), SceneLoadedEvent{handle});
		return handle;
	}

	namespace SceneInternal {

		SceneAsset* AcquireSceneAsset(const Guid& assetId, const char* caller) {
			Asset* asset = AssetDatabase::LoadAssetByGuid(assetId);
			if (!asset) {
				SLT_LOG_ERROR("Scene", "{}: nothing resolves {}", caller, assetId.ToString());
				return nullptr;
			}
			if (asset->GetAssetTypeId() != StringHash("SceneAsset")) {
				SLT_LOG_ERROR("Scene", "{}: {} isn't a scene", caller, assetId.ToString());
				AssetDatabase::UnloadAsset(asset);
				return nullptr;
			}
			SceneAsset* sceneAsset = static_cast<SceneAsset*>(asset);
			SceneDocument* document = SceneInternalAccess::DocumentOf(*sceneAsset);
			if (!document || document->isPrefab) {
				SLT_LOG_ERROR("Scene", "{}: {} holds {}", caller, assetId.ToString(),
							  document ? "a prefab document" : "no document");
				AssetDatabase::UnloadAsset(asset);
				return nullptr;
			}
			return sceneAsset;
		}

		void ApplyRequestedReplace() {
			SystemState& state = GetState();
			if (!state.replaceRequested) {
				return;
			}
			const Guid incomingId = state.replaceIncoming;
			const Handle<Scene> keep = state.replaceKeep;
			state.replaceRequested = false;
			state.replaceIncoming = {};
			state.replaceKeep = {};

			// resolved before anything unloads
			SceneAsset* incoming = AcquireSceneAsset(incomingId, "ReplaceScenes");
			if (!incoming) {
				return;
			}

			Vector<Handle<Scene>> outgoing{state.worldScenes, GetMemoryResource(MemTag::Scene)};
			for (const Handle<Scene>& scene : outgoing) {
				if (scene.GetInstanceId() != keep.GetInstanceId()) {
					SceneSystem::UnloadScene(scene);
				}
			}
			state.frameEnd.Drain();

			Handle<Scene> loaded = SceneSystem::LoadSceneByGuid(incomingId);
			AssetDatabase::UnloadAsset(incoming);
			if (loaded.IsValid()) {
				SceneSystem::SetPrimaryScene(loaded);
			}
		}

	} // namespace SceneInternal

	Handle<Scene> SceneSystem::LoadSceneByGuid(const Guid& assetId) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SLT_CORE_ASSERT_MSG(IsStructuralMutationLegal(), "LoadSceneByGuid is illegal inside lifecycle hooks");
		if (!GetState().initialized || !IsStructuralMutationLegal()) {
			return Handle<Scene>();
		}

		SceneAsset* sceneAsset = AcquireSceneAsset(assetId, "LoadSceneByGuid");
		if (!sceneAsset) {
			return Handle<Scene>();
		}
		Asset* asset = sceneAsset;
		SceneDocument* document = SceneInternalAccess::DocumentOf(*sceneAsset);

		Path virtualPath(sceneAsset->virtualPath);
		Scene* scene = CreateSceneInternal(SceneNameFromPath(virtualPath), false);
		SceneInternalAccess::SourceGuidOf(*scene) = assetId;
		InstantiateDocument(*document, *scene);

		AssetDatabase::UnloadAsset(asset);

		MakeSceneVisible(*scene);
		for (SceneObject* root : SceneInternalAccess::RootsOf(*scene)) {
			DeriveActiveSubtree(*root);
		}
		for (SceneObject* root : SceneInternalAccess::RootsOf(*scene)) {
			DispatchIslandAttach(*root);
		}

		Handle<Scene> handle(scene);
		SceneEvents::Loaded.Publish(EventBus::GetGlobal(), SceneLoadedEvent{handle});
		return handle;
	}

	namespace {

		constexpr u32 PLAY_SNAPSHOT_MAGIC = 0x504C5453;
		constexpr u32 PLAY_SNAPSHOT_VERSION = 1;

		struct PlaySnapshotHeader {
			u32 magic;
			u32 version;
			Guid sourceGuid;
		};

	} // anonymous namespace

	bool SceneSystem::SaveSceneToMemory(Handle<Scene> sceneHandle, Buffer& outSnapshot) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SLT_CORE_ASSERT_MSG(IsStructuralMutationLegal(), "SaveSceneToMemory is illegal inside lifecycle hooks");
		Scene* scene = sceneHandle.Get();
		if (!GetState().initialized || !IsStructuralMutationLegal() || !scene || scene->GetState() != SceneState::Loaded) {
			return false;
		}

		RefreshInstanceOverrides(*scene);

		SceneDocument* document = BuildDocumentFromScene(*scene, true);
		Buffer payload = WriteSceneDocumentBinary(*document);
		Delete(document);
		if (!payload) {
			return false;
		}

		PlaySnapshotHeader header{PLAY_SNAPSHOT_MAGIC, PLAY_SNAPSHOT_VERSION,
								  SceneInternalAccess::SourceGuidOf(*scene)};
		outSnapshot.Allocate(sizeof(PlaySnapshotHeader) + payload.size, MemTag::Scene);
		std::memcpy(outSnapshot.data, &header, sizeof(header));
		std::memcpy(outSnapshot.data + sizeof(header), payload.data, payload.size);
		return true;
	}

	Handle<Scene> SceneSystem::LoadSceneFromMemory(const Buffer& snapshot) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SLT_CORE_ASSERT_MSG(IsStructuralMutationLegal(), "LoadSceneFromMemory is illegal inside lifecycle hooks");
		if (!GetState().initialized || !IsStructuralMutationLegal()) {
			return Handle<Scene>();
		}
		if (!snapshot || snapshot.size <= sizeof(PlaySnapshotHeader)) {
			SLT_LOG_ERROR("Scene", "LoadSceneFromMemory: truncated snapshot");
			return Handle<Scene>();
		}

		PlaySnapshotHeader header{};
		std::memcpy(&header, snapshot.data, sizeof(header));
		if (header.magic != PLAY_SNAPSHOT_MAGIC || header.version != PLAY_SNAPSHOT_VERSION) {
			SLT_LOG_ERROR("Scene", "LoadSceneFromMemory: not a play snapshot");
			return Handle<Scene>();
		}

		Buffer payload(snapshot.size - sizeof(header), MemTag::Scene);
		std::memcpy(payload.data, snapshot.data + sizeof(header), payload.size);
		SceneDocument* document = ParseSceneDocument(std::move(payload));
		if (!document || document->isPrefab) {
			SLT_LOG_ERROR("Scene", "LoadSceneFromMemory: malformed snapshot document");
			if (document) {
				Delete(document);
			}
			return Handle<Scene>();
		}

		Scene* scene = CreateSceneInternal(document->name, false);
		SceneInternalAccess::SourceGuidOf(*scene) = header.sourceGuid;
		InstantiateDocument(*document, *scene);
		Delete(document);

		MakeSceneVisible(*scene);
		for (SceneObject* root : SceneInternalAccess::RootsOf(*scene)) {
			DeriveActiveSubtree(*root);
		}
		for (SceneObject* root : SceneInternalAccess::RootsOf(*scene)) {
			DispatchIslandAttach(*root);
		}

		Handle<Scene> handle(scene);
		SceneEvents::Loaded.Publish(EventBus::GetGlobal(), SceneLoadedEvent{handle});
		return handle;
	}

} // namespace Shalltear
