#include "Shalltear/Core/Scene/SceneAsset.h"

#include "SceneDocument.h"
#include "Serialization/SerializerInternal.h"

#include "Shalltear/Core/Asset/PakFormat.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Reflection/Walk.h"

namespace Shalltear {

	namespace SceneInternal {

		namespace {

			constexpr u32 MAX_ASSET_REF_DEPTH = 32;

			void NoteLayerAssetRefs(const OverrideLayer& layer, CookWriter& payload) {
				for (const PropertyOverrideOp& op : layer.properties) {
					if (op.value.isHandle && IsAssetGuid(op.value.handleGuid)) {
						payload.NoteAssetRef(op.value.handleGuid);
					}
				}
			}

			void NoteJSONAssetRefs(const JSONValue& value, CookWriter& payload) {
				if (value.IsObject()) {
					if (value["_handle"].AsBool()) {
						Guid guid = Guid::FromString(value["assetId"].AsString());
						if (IsAssetGuid(guid)) {
							payload.NoteAssetRef(guid);
						}
						return;
					}
					for (size_t i = 0; i < value.MemberCount(); ++i) {
						NoteJSONAssetRefs(value.ChildAt(i), payload);
					}
				} else if (value.IsArray()) {
					for (size_t i = 0; i < value.Size(); ++i) {
						NoteJSONAssetRefs(value[i], payload);
					}
				}
			}

			bool NoteBinaryObjectAssetRefs(SerializerInternal::BinaryReader& reader, const TypeInfo* fallbackType,
										   CookWriter& payload, u32 depth) {
				if (depth > MAX_ASSET_REF_DEPTH) {
					return false;
				}
				u64 typeNameHash = 0;
				u32 fieldCount = 0;
				if (!reader.ReadU64(typeNameHash) || !reader.ReadU32(fieldCount)) {
					return false;
				}
				const TypeInfo* type = TypeRegistry::FindType(StringHash(typeNameHash));
				if (!type) {
					type = fallbackType;
				}
				for (u32 i = 0; i < fieldCount; ++i) {
					u64 nameHash = 0;
					u32 fieldFlags = 0;
					u32 valueLen = 0;
					if (!reader.ReadU64(nameHash) || !reader.ReadU32(fieldFlags) || !reader.ReadU32(valueLen)) {
						return false;
					}
					const size_t valueStart = reader.Tell();
					if (valueLen > reader.Remaining()) {
						return false;
					}
					const FieldInfo* field = type ? FindField(type, StringHash(nameHash)) : nullptr;
					if ((fieldFlags & FieldFlagHandle) && !(fieldFlags & FieldFlagContainer)) {
						u64 pointee = 0;
						Guid guid{};
						if (reader.ReadU64(pointee) && reader.ReadU64(guid.low) && reader.ReadU64(guid.high)) {
							if (IsAssetGuid(guid)) {
								payload.NoteAssetRef(guid);
							}
						}
					} else if ((fieldFlags & FieldFlagSubobject) && !(fieldFlags & FieldFlagContainer)) {
						NoteBinaryObjectAssetRefs(reader, field ? field->fieldType : nullptr, payload, depth + 1);
					} else if ((fieldFlags & FieldFlagContainer) && field && field->container) {
						u32 count = 0;
						if (reader.ReadU32(count)) {
							if (field->container->elementFlags & FieldFlagHandle) {
								for (u32 k = 0; k < count; ++k) {
									u64 pointee = 0;
									Guid guid{};
									if (!reader.ReadU64(pointee) || !reader.ReadU64(guid.low) || !reader.ReadU64(guid.high)) {
										break;
									}
									if (IsAssetGuid(guid)) {
										payload.NoteAssetRef(guid);
									}
								}
							} else if (field->container->elementFlags & FieldFlagSubobject) {
								for (u32 k = 0; k < count; ++k) {
									if (!NoteBinaryObjectAssetRefs(reader, field->container->elementType, payload, depth + 1)) {
										break;
									}
								}
							}
						}
					}
					if (!reader.Seek(valueStart + valueLen)) {
						return false;
					}
				}
				return true;
			}

			void NoteBlobAssetRefs(const SceneDocument& document, const SceneDocumentBlob& blob, CookWriter& payload) {
				if (blob.isOpaque) {
					return;
				}
				if (document.binary) {
					if (!blob.binary.empty()) {
						SerializerInternal::BinaryReader reader(blob.binary.data(), blob.binary.size());
						NoteBinaryObjectAssetRefs(reader, nullptr, payload, 0);
					}
				} else {
					NoteJSONAssetRefs(blob.json, payload);
				}
			}

		} // namespace

		bool CookDocumentAsset(const SceneDocument* document, std::string_view virtualPath, CookWriter& payload) {
			if (!document) {
				SLT_LOG_ERROR("Scene", "cooking '{}': the asset holds no document", virtualPath);
				return false;
			}
			Buffer bytes = SceneInternal::WriteSceneDocumentBinary(*document);
			if (!bytes) {
				SLT_LOG_ERROR("Scene", "cooking '{}': a row's fields couldn't be re-encoded", virtualPath);
				return false;
			}
			payload.WriteBytes(bytes.data, bytes.size);

			if (document->basePrefab.IsValid()) {
				payload.NoteAssetRef(document->basePrefab);
			}
			NoteLayerAssetRefs(document->prefabOverrides, payload);
			for (const SceneDocumentRow& row : document->rows) {
				if (row.kind == SceneDocumentRow::Kind::Instance) {
					payload.NoteAssetRef(row.prefab);
					NoteLayerAssetRefs(row.overrides, payload);
				} else {
					NoteBlobAssetRefs(*document, row.fields, payload);
					for (const SceneDocumentComponentRow& componentRow : row.components) {
						NoteBlobAssetRefs(*document, componentRow.fields, payload);
					}
				}
			}
			return true;
		}

		SceneDocument* LoadCookedDocument(CookReader& payload, std::string_view virtualPath) {
			size_t size = payload.Remaining();
			const u8* bytes = payload.ReadSpan(size);
			if (payload.Failed() || !bytes) {
				return nullptr;
			}
			Buffer owned(size, MemTag::Scene);
			std::memcpy(owned.data, bytes, size);
			SceneDocument* document = SceneInternal::ParseSceneDocument(std::move(owned));
			if (!document) {
				SLT_LOG_ERROR("Scene", "cooked '{}' is a malformed document", virtualPath);
			}
			return document;
		}

	} // namespace SceneInternal

	SceneAsset::SceneAsset() = default;

	SceneAsset::~SceneAsset() {
		if (_document) {
			Delete(_document);
			_document = nullptr;
		}
	}

	bool SceneAsset::Import(const Path& sourcePath) {
		Buffer bytes = FileSystem::ReadFile(sourcePath, MemTag::Scene);
		if (!bytes) {
			SLT_LOG_ERROR("Scene", "SceneAsset: can't read '{}'", sourcePath.ToString());
			return false;
		}
		SceneDocument* document = SceneInternal::ParseSceneDocument(std::move(bytes));
		if (!document) {
			SLT_LOG_ERROR("Scene", "SceneAsset: malformed document '{}'", sourcePath.ToString());
			return false;
		}
		if (_document) {
			Delete(_document);
		}
		_document = document;
		return true;
	}

	bool SceneAsset::Export(const Path& destPath) const {
		if (!_document) {
			return false;
		}
		if (_document->binary) {
			Buffer bytes = SceneInternal::WriteSceneDocumentBinary(*_document);
			return bytes && FileSystem::WriteFile(destPath, bytes);
		}
		str text = SceneInternal::WriteSceneDocumentText(*_document);
		return !text.empty() && FileSystem::WriteTextFile(destPath, text);
	}

	bool SceneAsset::Cook(CookWriter& descriptor, CookWriter& payload) const {
		(void)descriptor;
		return SceneInternal::CookDocumentAsset(_document, virtualPath, payload);
	}

	bool SceneAsset::CookImportState(CookWriter& importState) const {
		(void)importState;
		return false;
	}

	bool SceneAsset::LoadCooked(CookReader& descriptor, CookReader& payload) {
		(void)descriptor;
		SceneDocument* document = SceneInternal::LoadCookedDocument(payload, virtualPath);
		if (!document) {
			return false;
		}
		if (_document) {
			Delete(_document);
		}
		_document = document;
		return true;
	}

} // namespace Shalltear

SLT_REGISTER_TYPE(Shalltear::SceneAsset)
