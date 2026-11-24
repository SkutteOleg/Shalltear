#include "Shalltear/Core/Asset/AssetDatabase.h"

#include "Shalltear/Core/Asset/Asset.h"
#include "Shalltear/Core/Asset/AssetImporter.h"
#include "Shalltear/Core/Asset/TextAsset.h"
#include "Shalltear/Core/Assert.h"
#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Graphics/FullScreenPass.h"
#include "Shalltear/Core/Graphics/GraphicsProfile.h"
#include "Shalltear/Core/Graphics/MaterialAsset.h"
#include "Shalltear/Core/Graphics/MeshAsset.h"
#include "Shalltear/Core/Graphics/ObjectPass.h"
#include "Shalltear/Core/Graphics/ShaderAsset.h"
#include "Shalltear/Core/Graphics/SkeletonAsset.h"
#include "Shalltear/Core/Graphics/SkinAsset.h"
#include "Shalltear/Core/Graphics/SpriteAsset.h"
#include "Shalltear/Core/Graphics/TextureAsset.h"
#include "Shalltear/Core/Localization.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Profiler.h"
#include "Shalltear/Core/ProjectLayout.h"
#include "Shalltear/Core/UI/FontAsset.h"
#include "Shalltear/Core/UI/StyleSheetAsset.h"
#include "Shalltear/Core/UI/UIDocument.h"
#include "Shalltear/Core/UI/UIPanelSettings.h"
#include "Shalltear/Core/UI/VectorAsset.h"
#include "StaticLinkAnchors.h"

#include "Asset/AssetBinder.h"
#include "Asset/AssetDatabaseData.h"
#include "Asset/AssetSource.h"
#include "Asset/LZ4Decode.h"
#include "Asset/EngineBuiltins.h"
#include "Asset/ImportArtifact.h"
#include "Asset/ImportArtifactStore.h"
#include "FileAccessRecorder.h"
#include "Localization/LocalizationInternal.h"
#include "Module/EngineStateTable.h"
#include "Shalltear/Core/Serialization/HandleResolver.h"
#include "Shalltear/Core/Serialization/JSON.h"
#include "Shalltear/Core/StringUtil.h"

#include "Graphics/Importers.h"

#include <charconv>
#include <chrono>
#include <deque>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string_view>
#include <unordered_map>

namespace Shalltear {

	struct ProductTableEntry {
		str key;
		StringHash type;
		Guid guid;
		bool produced = false;
		str name;
	};

	namespace {

		std::string_view ProductDisplayName(std::string_view key, std::string_view name) { return name.empty() ? key : name; }

	} // anonymous namespace

	class AssetProductBuilder {
	public:
		AssetProductBuilder(AssetDatabaseData& data, const Guid& sourceGuid, const str& sourceKey, Vector<ProductTableEntry>&& table);
		~AssetProductBuilder();

		AssetProductBuilder(const AssetProductBuilder&) = delete;
		AssetProductBuilder& operator=(const AssetProductBuilder&) = delete;

		AssetImportContext& Context() noexcept { return _context; }

		Guid Add(std::string_view key, Asset* product, std::string_view name);

		Guid Adopt(std::string_view key, const Guid& identity);

		void KeepEveryResident() noexcept { _keepResident = true; }

		bool Commit();

		const Vector<ProductTableEntry>& Table() const noexcept { return _table; }

		bool CollectProduced(Vector<ImportArtifactProduct>& out) const;
	private:
		AssetDatabaseData& _data;
		Guid _source;
		str _sourceKey;
		Vector<ProductTableEntry> _table;
		Vector<Asset*> _added;
		bool _tableChanged = false;
		bool _committed = false;
		bool _keepResident = false;
		AssetImportContext _context;
	};

	namespace {

		constexpr std::string_view ProjectScheme = "project";

		// indexed, never minted into/written to .assets.db
		constexpr std::string_view EngineScheme = "engine";

		TextAssetImporter& GetTextImporter() {
			static TextAssetImporter importer;
			return importer;
		}

		using AssetInternal::GetData;

		str HashToHex(StringHash hash) {
			return std::format("{:016x}", hash.GetValue());
		}

		bool HexToHash(std::string_view text, StringHash& out) {
			if (text.size() != 16) {
				return false;
			}
			u64 value = 0;
			auto result = std::from_chars(text.data(), text.data() + 16, value, 16);
			if (result.ec != std::errc{} || result.ptr != text.data() + 16) {
				return false;
			}
			out = StringHash(value);
			return true;
		}

		Path GetSchemeRootPhysical(std::string_view scheme) {
			Path resolved = FileSystem::Resolve(Path(str(scheme) + "://_"));
			const str& s = resolved.ToString();
			if (s.find("://") != str::npos) {
				return Path();
			}
			return Path(resolved.GetDirectory());
		}

		void ReadProductTable(const JSONValue& root, Vector<ProductTableEntry>& out) {
			const JSONValue& products = root["products"];
			if (!products.IsArray()) {
				return;
			}
			out.reserve(products.Size());
			for (size_t i = 0; i < products.Size(); ++i) {
				const JSONValue& entry = products[i];
				ProductTableEntry product;
				product.key = str(entry["key"].AsString());
				product.guid = Guid::FromString(entry["guid"].AsString());
				product.name = str(entry["name"].AsString());
				HexToHash(entry["type"].AsString(), product.type);
				if (product.key.empty() || !product.guid.IsValid()) {
					continue;
				}
				out.push_back(std::move(product));
			}
		}

		bool ReadMetaFile(const Path& physMeta, Guid& outGuid, StringHash& outType, Vector<ProductTableEntry>* outProducts = nullptr) {
			str text = FileSystem::ReadTextFile(physMeta, MemTag::Core);
			if (text.empty()) {
				return false;
			}
			LinearAllocator arena(text.size() + 64, MemTag::Core);
			JSONValue root = JSONParse(text, arena);
			if (!root.IsObject()) {
				return false;
			}
			outGuid = Guid::FromString(root["assetId"].AsString());
			outType = StringHash();
			HexToHash(root["type"].AsString(), outType);
			if (outProducts) {
				ReadProductTable(root, *outProducts);
			}
			return true;
		}

		IAssetImporter* FindImporterFor(const Path& vpath) { return AssetImporterRegistry::FindByExtension(vpath.GetExtension()); }

		JSONValue SettingsOverDefaults(const IAssetImporter& importer, const Path& vpath, const JSONValue& authored) {
			JSONValue defaults = JSONValue::MakeObject();
			importer.SerializeSettings(FileSystem::Resolve(vpath), defaults);
			const bool hasAuthored = authored.IsObject();
			JSONValue settings = JSONValue::MakeObject();
			for (size_t i = 0; i < defaults.MemberCount(); ++i) {
				const JSONValue* kept = hasAuthored ? authored.Find(defaults.KeyAt(i)) : nullptr;
				settings.AddMember(defaults.KeyAt(i), kept ? *kept : defaults.ChildAt(i));
			}
			for (size_t i = 0; hasAuthored && i < authored.MemberCount(); ++i) {
				if (!defaults.HasKey(authored.KeyAt(i))) {
					settings.AddMember(authored.KeyAt(i), authored.ChildAt(i));
				}
			}
			return settings;
		}

		bool WriteMetaFile(const Path& physMeta, const Path& vpath, const Guid& guid, StringHash typeHash,
						   const Vector<ProductTableEntry>* products = nullptr,
						   const JSONValue* settingsOverride = nullptr) {
			str existingText = FileSystem::Exists(physMeta) ? FileSystem::ReadTextFile(physMeta, MemTag::Core) : str();
			LinearAllocator arena(existingText.size() + 64, MemTag::Core);
			JSONValue existing = existingText.empty() ? JSONValue() : JSONParse(existingText, arena);
			if (!existing.IsObject()) {
				existing = JSONValue();
			}

			std::deque<str> pool;

			JSONValue root = JSONValue::MakeObject();

			pool.push_back(guid.ToString());
			root.AddMember("assetId", JSONValue::MakeString(pool.back()));

			pool.push_back(HashToHex(typeHash));
			std::string_view typeHex = pool.back();
			root.AddMember("type", JSONValue::MakeString(typeHex));

			const JSONValue& existingImporter = existing["importer"];
			JSONValue importer = JSONValue::MakeObject();
			importer.AddMember("type", JSONValue::MakeString(typeHex));

			const JSONValue* existingSettings = existingImporter.IsObject() ? existingImporter.Find("settings") : nullptr;
			if (settingsOverride && settingsOverride->IsObject()) {
				importer.AddMember("settings", *settingsOverride);
			} else if (existingSettings && existingSettings->IsObject()) {
				importer.AddMember("settings", *existingSettings);
			} else {
				JSONValue settings = JSONValue::MakeObject();
				if (IAssetImporter* forDefaults = FindImporterFor(vpath)) {
					forDefaults->SerializeSettings(FileSystem::Resolve(vpath), settings);
				}
				importer.AddMember("settings", std::move(settings));
			}

			for (size_t i = 0; i < existingImporter.MemberCount(); ++i) {
				std::string_view key = existingImporter.KeyAt(i);
				if (key != "type" && key != "settings") {
					importer.AddMember(key, existingImporter.ChildAt(i));
				}
			}
			root.AddMember("importer", std::move(importer));

			if (products) {
				JSONValue table = JSONValue::MakeArray();
				for (const ProductTableEntry& product : *products) {
					JSONValue entry = JSONValue::MakeObject();
					entry.AddMember("key", JSONValue::MakeString(product.key));
					pool.push_back(HashToHex(product.type));
					entry.AddMember("type", JSONValue::MakeString(pool.back()));
					pool.push_back(product.guid.ToString());
					entry.AddMember("guid", JSONValue::MakeString(pool.back()));
					if (!product.name.empty()) {
						entry.AddMember("name", JSONValue::MakeString(product.name));
					}
					table.Push(std::move(entry));
				}
				root.AddMember("products", std::move(table));
			} else if (const JSONValue* existingProducts = existing.Find("products")) {
				root.AddMember("products", *existingProducts);
			}

			for (size_t i = 0; i < existing.MemberCount(); ++i) {
				std::string_view key = existing.KeyAt(i);
				if (key != "assetId" && key != "type" && key != "importer" && key != "products") {
					root.AddMember(key, existing.ChildAt(i));
				}
			}

			str text = JSONSerialize(root, true);
			return FileSystem::WriteTextFile(physMeta, text);
		}

		void LoadDatabaseIndexLocked(AssetDatabaseData& data) {
			Path physDb = FileSystem::Resolve(Path(str(ProjectScheme) + "://.assets.db"));
			if (!FileSystem::Exists(physDb)) {
				return;
			}
			str text = FileSystem::ReadTextFile(physDb, MemTag::Core);
			if (text.empty()) {
				return;
			}
			LinearAllocator arena(text.size() + 64, MemTag::Core);
			JSONValue root = JSONParse(text, arena);
			if (!root.IsObject()) {
				return;
			}
			const JSONValue& entries = root["entries"];
			if (!entries.IsObject()) {
				return;
			}
			for (size_t i = 0; i < entries.MemberCount(); ++i) {
				Guid guid = Guid::FromString(entries.KeyAt(i));
				const JSONValue& value = entries.ChildAt(i);
				if (!guid.IsValid() || !value.IsString()) {
					continue;
				}
				str key = Path(str(value.AsString())).ToString();
				data.guidToPath[guid] = key;
				data.pathToGuid[key] = guid;
			}

			const JSONValue& products = root["products"];
			if (!products.IsObject()) {
				return;
			}
			for (size_t i = 0; i < products.MemberCount(); ++i) {
				Guid productGuid = Guid::FromString(products.KeyAt(i));
				const JSONValue& entry = products.ChildAt(i);
				Guid sourceGuid = Guid::FromString(entry["source"].AsString());
				std::string_view key = entry["key"].AsString();
				if (!productGuid.IsValid() || !sourceGuid.IsValid() || key.empty()) {
					continue;
				}
				data.productIndex[productGuid] = {sourceGuid, str(key), str(entry["name"].AsString())};
			}
		}

		void WriteDatabaseIndexLocked(AssetDatabaseData& data) {
			Path physDb = FileSystem::Resolve(Path(str(ProjectScheme) + "://.assets.db"));

			std::deque<str> pool;
			JSONValue root = JSONValue::MakeObject();
			root.AddMember("version", JSONValue::MakeNumber(1.0));

			JSONValue entries = JSONValue::MakeObject();
			for (const auto& [guid, path] : data.guidToPath) {
				if (!std::string_view(path).starts_with(ProjectScheme)) {
					continue;
				}
				pool.push_back(guid.ToString());
				std::string_view key = pool.back();
				pool.push_back(path);
				entries.AddMember(key, JSONValue::MakeString(pool.back()));
			}
			root.AddMember("entries", std::move(entries));

			JSONValue products = JSONValue::MakeObject();
			for (const auto& [productGuid, entry] : data.productIndex) {
				auto sourcePath = data.guidToPath.find(entry.source);
				if (sourcePath == data.guidToPath.end() || !std::string_view(sourcePath->second).starts_with(ProjectScheme)) {
					continue;
				}
				pool.push_back(productGuid.ToString());
				std::string_view key = pool.back();

				JSONValue record = JSONValue::MakeObject();
				pool.push_back(entry.source.ToString());
				record.AddMember("source", JSONValue::MakeString(pool.back()));
				record.AddMember("key", JSONValue::MakeString(entry.key));
				if (!entry.name.empty()) {
					record.AddMember("name", JSONValue::MakeString(entry.name));
				}
				products.AddMember(key, std::move(record));
			}
			root.AddMember("products", std::move(products));

			str text = JSONSerialize(root, true);
			FileSystem::WriteTextFile(physDb, text);
		}

		void RegisterPathLocked(AssetDatabaseData& data, const Guid& guid, const str& virtualKey) {
			data.pathToGuid[virtualKey] = guid;
			data.guidToPath[guid] = virtualKey;
		}

		Asset* FinishLoadLocked(AssetDatabaseData& data, Asset* asset) {
			if (!asset) {
				return nullptr;
			}
			SLT_CORE_ASSERT_MSG(asset->assetId.IsValid(),
								"an asset source returned an instance it didn't stamp with an identity");

			data.guidToAsset[asset->assetId] = asset;
			data.instanceToGuid[asset->GetInstanceId()] = asset->assetId;
			if (!asset->virtualPath.empty()) {
				RegisterPathLocked(data, asset->assetId, asset->virtualPath);
			}
			asset->AddRef();
			asset->NotifyLoaded();
			return asset;
		}

		class LooseAssetSource final : public IAssetSource {
		public:
			const char* GetSourceName() const override { return "loose"; }

			bool Contains(const Guid& guid) const override;

			Asset* Load(const Guid& guid) override;

			Asset* LoadFromPath(AssetDatabaseData& data, const str& virtualKey);

			bool RebuildProducts(AssetDatabaseData& data, const str& virtualKey);
		};

		LooseAssetSource& GetLooseSource() {
			static LooseAssetSource source;
			return source;
		}

		Asset* PackageAnswerLocked(AssetDatabaseData& data, const Guid& guid) {
			if (!guid.IsValid() || !data.package.Contains(guid)) {
				return nullptr;
			}
			return data.package.Load(guid);
		}

		struct Identity {
			Guid guid;
			StringHash type;
			IAssetImporter* importer = nullptr;
			Path physSource;
			JSONValue metaRoot;
			bool ok = false;
		};

		struct IdentityScratch {
			str metaText;
			std::optional<LinearAllocator> arena;
		};

		bool ImportIsInFlight(const AssetDatabaseData& data, const str& virtualKey) {
			for (const str& key : data.importsInFlight) {
				if (key == virtualKey) {
					return true;
				}
			}
			return false;
		}

		Identity ResolveIdentityLocked(AssetDatabaseData& data, const str& virtualKey, IdentityScratch& scratch) {
			Identity out;
			Path vpath(virtualKey);
			out.physSource = FileSystem::Resolve(vpath);
			if (!FileSystem::Exists(out.physSource)) {
				SLT_LOG_ERROR("Asset", "AssetDatabase: source '{}' doesn't exist", virtualKey);
				return out;
			}

			Path physMeta = FileSystem::Resolve(AssetDatabase::GetMetaPath(vpath));
			scratch.metaText = FileSystem::Exists(physMeta) ? FileSystem::ReadTextFile(physMeta, MemTag::Core) : str();
			scratch.arena.emplace(scratch.metaText.size() + 64, MemTag::Core);
			out.metaRoot = scratch.metaText.empty() ? JSONValue() : JSONParse(scratch.metaText, *scratch.arena);

			bool haveMeta = false;
			if (out.metaRoot.IsObject()) {
				out.guid = Guid::FromString(out.metaRoot["assetId"].AsString());
				HexToHash(out.metaRoot["type"].AsString(), out.type);
				haveMeta = out.guid.IsValid();
			}

			out.importer = FindImporterFor(vpath);
			if (!out.importer) {
				SLT_LOG_ERROR("Asset", "AssetDatabase: no importer for '{}'", virtualKey);
				return out;
			}

			const bool wrote = !haveMeta || out.type.GetValue() == 0;
			if (!haveMeta) {
				out.guid = AssetDatabase::GenerateGuid();
				out.type = out.importer->GetAssetType();
				WriteMetaFile(physMeta, vpath, out.guid, out.type);
			} else if (out.type.GetValue() == 0) {
				out.type = out.importer->GetAssetType();
				const JSONValue settings = SettingsOverDefaults(*out.importer, vpath, out.metaRoot["importer"]["settings"]);
				WriteMetaFile(physMeta, vpath, out.guid, out.type, nullptr, &settings);
			}

			if (wrote) {
				out.metaRoot = JSONValue();
				scratch.metaText = FileSystem::ReadTextFile(physMeta, MemTag::Core);
				scratch.arena.emplace(scratch.metaText.size() + 64, MemTag::Core);
				if (!scratch.metaText.empty()) {
					out.metaRoot = JSONParse(scratch.metaText, *scratch.arena);
				}
			}

			RegisterPathLocked(data, out.guid, virtualKey);
			out.ok = true;
			return out;
		}

		ImportArtifactRequest ArtifactRequestFor(const Identity& identity, const str& virtualKey) {
			ImportArtifactRequest artifact;
			artifact.guid = identity.guid;
			artifact.virtualKey = &virtualKey;
			artifact.physSource = identity.physSource;
			artifact.settingsHash = ImportArtifactHashSettings(identity.metaRoot["importer"]["settings"]);
			artifact.importerVersion = identity.importer->GetImporterVersion();
			return artifact;
		}

		struct ImportInFlightScope {
			AssetDatabaseData& data;
			ImportInFlightScope(AssetDatabaseData& owner, const str& virtualKey) : data(owner) { data.importsInFlight.push_back(virtualKey); }
			~ImportInFlightScope() { data.importsInFlight.pop_back(); }
			ImportInFlightScope(const ImportInFlightScope&) = delete;
			ImportInFlightScope& operator=(const ImportInFlightScope&) = delete;
		};

		Asset* LooseAssetSource::LoadFromPath(AssetDatabaseData& data, const str& virtualKey) {
			IdentityScratch scratch;
			if (auto indexed = data.pathToGuid.find(virtualKey); indexed != data.pathToGuid.end()) {
				if (Asset* cooked = PackageAnswerLocked(data, indexed->second)) {
					return cooked;
				}
			}

			Identity identity = ResolveIdentityLocked(data, virtualKey, scratch);
			if (!identity.ok) {
				return nullptr;
			}

			if (ImportIsInFlight(data, virtualKey)) {
				SLT_LOG_ERROR("Asset", "AssetDatabase: '{}' is already being imported (an importer can't ask for its source)", virtualKey);
				return nullptr;
			}
			ImportInFlightScope inFlight{data, virtualKey};

			if (Asset* cooked = PackageAnswerLocked(data, identity.guid)) {
				return cooked;
			}

			Path vpath(virtualKey);
			Guid guid = identity.guid;
			const JSONValue& settings = identity.metaRoot["importer"]["settings"];

			Vector<ProductTableEntry> productTable{GetMemoryResource(MemTag::Asset)};
			ReadProductTable(identity.metaRoot, productTable);

			ImportArtifactRequest artifact = ArtifactRequestFor(identity, virtualKey);

			if (Asset* cached = ImportArtifactStore::TryLoad(artifact)) {
				return cached;
			}

			FileAccessRecording accesses;
			Asset* asset = nullptr;
			AssetProductBuilder products{data, guid, virtualKey, std::move(productTable)};
			auto importBegan = std::chrono::steady_clock::now();
			{
				AssetInternal::AssetBindScope bind;
				ScopedFileAccessRecorder record{accesses};
				asset = identity.importer->ImportWithSettings(identity.physSource, vpath, settings, products.Context());
				if (asset) {
					bind.AttachTo(*asset);
				}
			}
			u64 importMicros = static_cast<u64>(
				std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - importBegan).count());
			if (!asset) {
				SLT_LOG_ERROR("Asset", "AssetDatabase: importer failed for '{}'", virtualKey);
				return nullptr;
			}

			asset->assetId = guid;
			asset->virtualPath = virtualKey;

			if (products.Commit()) {
				WriteMetaFile(FileSystem::Resolve(AssetDatabase::GetMetaPath(vpath)), vpath, guid, identity.type,
							  &products.Table());
			}

			Vector<ImportArtifactProduct> produced{GetMemoryResource(MemTag::Asset)};
			if (products.CollectProduced(produced)) {
				ImportArtifactStore::Fill(artifact, *asset, std::span<const ImportArtifactProduct>(produced), accesses, importMicros);
			}
			return asset;
		}

		bool LooseAssetSource::RebuildProducts(AssetDatabaseData& data, const str& virtualKey) {
			IdentityScratch scratch;
			Identity identity = ResolveIdentityLocked(data, virtualKey, scratch);
			if (!identity.ok) {
				return false;
			}
			if (ImportIsInFlight(data, virtualKey)) {
				SLT_LOG_ERROR("Asset", "AssetDatabase: '{}' is already being imported (an importer can't ask for its source)", virtualKey);
				return false;
			}
			ImportInFlightScope inFlight{data, virtualKey};

			const Path vpath(virtualKey);
			Vector<ProductTableEntry> productTable{GetMemoryResource(MemTag::Asset)};
			ReadProductTable(identity.metaRoot, productTable);
			AssetProductBuilder products{data, identity.guid, virtualKey, std::move(productTable)};
			products.KeepEveryResident();
			Asset* asset = nullptr;
			{
				AssetInternal::AssetBindScope bind;
				asset = identity.importer->ImportWithSettings(identity.physSource, vpath, identity.metaRoot["importer"]["settings"], products.Context());
			}
			if (!asset) {
				SLT_LOG_ERROR("Asset", "AssetDatabase: importer failed for '{}'", virtualKey);
				return false;
			}

			if (products.Commit()) {
				WriteMetaFile(FileSystem::Resolve(AssetDatabase::GetMetaPath(vpath)), vpath, identity.guid, identity.type, &products.Table());
			}
			Delete(asset);
			return true;
		}

		bool LooseAssetSource::Contains(const Guid& guid) const {
			AssetDatabaseData& data = GetData();
			return data.guidToPath.find(guid) != data.guidToPath.end();
		}

		Asset* LooseAssetSource::Load(const Guid& guid) {
			AssetDatabaseData& data = GetData();
			auto pathIt = data.guidToPath.find(guid);
			if (pathIt == data.guidToPath.end()) {
				return nullptr;
			}
			return LoadFromPath(data, pathIt->second);
		}

		IAssetSource& SourceForLocked(AssetDatabaseData& data, const Guid& guid) {
			if (data.package.Contains(guid)) {
				return data.package;
			}
			return GetLooseSource();
		}

		thread_local Vector<Guid>* _loadSink = nullptr;

		Asset* RecordLoadLocked(Asset* asset) {
			if (_loadSink && asset && asset->assetId.IsValid()) {
				_loadSink->push_back(asset->assetId);
			}
			return asset;
		}

		Asset* ReviveLocked(AssetDatabaseData& data, Asset* asset) {
			asset->AddRef();
			data.parked.erase(asset);
			return asset;
		}

		void ReleaseRefLocked(AssetDatabaseData& data, Asset* asset) {
			if (!asset) {
				return;
			}
			int remaining = asset->ReleaseRef();
			if (remaining > 0) {
				return;
			}
			if (remaining < 0) {
				SLT_LOG_WARN("Asset", "AssetDatabase: '{}' released more times than it was loaded", asset->virtualPath);
				return;
			}

			data.parked.insert(asset);
		}

		str ProductDisplayPath(const str& sourceKey, std::string_view key, std::string_view name) {
			return std::format("{} : {}", sourceKey, ProductDisplayName(key, name));
		}

		str ExtractedFileLocked(AssetDatabaseData& data, const Guid& guid) {
			auto path = data.guidToPath.find(guid);
			if (path == data.guidToPath.end()) {
				return {};
			}
			if (!FileSystem::Exists(FileSystem::Resolve(Path(path->second)))) {
				return {};
			}
			return path->second;
		}

		// never in guidToPath (a guid load would become a path load)
		void RegisterProductLocked(AssetDatabaseData& data, Asset* product, const Guid& guid, const str& displayPath) {
			product->assetId = guid;
			product->virtualPath = displayPath;
			data.guidToAsset[guid] = product;
			data.instanceToGuid[product->GetInstanceId()] = guid;
			product->NotifyLoaded();
		}

		void DestroyResidentLocked(AssetDatabaseData& data, Asset* asset) {
			data.guidToAsset.erase(asset->assetId);
			data.instanceToGuid.erase(asset->GetInstanceId());
			asset->NotifyUnloaded();
			data.parked.erase(asset);
			Delete(asset);
		}

		bool AdoptReimportLocked(AssetDatabaseData& data, Asset* live, const str& key) {
			Asset* fresh = GetLooseSource().LoadFromPath(data, key);
			if (!fresh) {
				SLT_LOG_ERROR("Asset", "AssetDatabase: re-import of '{}' failed (old instance stays)", key);
				return false;
			}
			const bool sameType = fresh->GetAssetTypeId() == live->GetAssetTypeId();
			if (sameType) {
				live->AdoptReimport(*fresh);
			} else {
				SLT_LOG_WARN("Asset", "AssetDatabase: '{}' now imports as another type. Can't re-import it while referenced", key);
			}
			fresh->NotifyUnloaded();
			Delete(fresh);
			return sameType;
		}

		Asset* LoadByPathLocked(AssetDatabaseData& data, const str& virtualKey) {
			auto pathIt = data.pathToGuid.find(virtualKey);
			if (pathIt != data.pathToGuid.end()) {
				auto assetIt = data.guidToAsset.find(pathIt->second);
				if (assetIt != data.guidToAsset.end() && assetIt->second) {
					return RecordLoadLocked(ReviveLocked(data, assetIt->second));
				}
			}
			return RecordLoadLocked(FinishLoadLocked(data, GetLooseSource().LoadFromPath(data, virtualKey)));
		}

		Asset* LoadProductLocked(AssetDatabaseData& data, const Guid& guid) {
			auto indexed = data.productIndex.find(guid);
			if (indexed == data.productIndex.end()) {
				return nullptr;
			}
			if (!ExtractedFileLocked(data, guid).empty()) {
				return nullptr;
			}

			Guid sourceGuid = indexed->second.source;
			str key = indexed->second.key;
			str name = indexed->second.name;

			auto sourcePath = data.guidToPath.find(sourceGuid);
			if (sourcePath == data.guidToPath.end()) {
				SLT_LOG_WARN("Asset", "AssetDatabase: '{}' belongs to {} (not in the idx)", key, sourceGuid);
				return nullptr;
			}
			str sourceKey = sourcePath->second;

			if (ImportIsInFlight(data, sourceKey)) {
				SLT_LOG_WARN("Asset", "AssetDatabase: '{}' was asked for while '{}' is still importing (a product exists once its import returns)", key, sourceKey);
				return nullptr;
			}

			if (FileSystem::Exists(ImportArtifactPath(sourceGuid))) {
				IdentityScratch scratch;
				Identity identity = ResolveIdentityLocked(data, sourceKey, scratch);
				if (identity.ok && identity.guid == sourceGuid) {
					Asset* cached = nullptr;
					ImportArtifactProductResult result = ImportArtifactStore::TryLoadProduct(
						ArtifactRequestFor(identity, sourceKey), key, guid, ProductDisplayPath(sourceKey, key, name), cached);
					if (result == ImportArtifactProductResult::Loaded) {
						if (_loadSink) {
							_loadSink->push_back(sourceGuid);
						}
						RegisterProductLocked(data, cached, guid, ProductDisplayPath(sourceKey, key, name));
						cached->AddRef();
						return cached;
					}
					if (result == ImportArtifactProductResult::NotProduced) {
						SLT_LOG_WARN("Asset", "AssetDatabase: '{}' is no longer in '{}'", key, sourceKey);
						return nullptr;
					}
				}
			}

			if (auto resident = data.guidToAsset.find(sourceGuid); resident != data.guidToAsset.end() && resident->second) {
				GetLooseSource().RebuildProducts(data, sourceKey);
			} else {
				Asset* source = LoadByPathLocked(data, sourceKey);
				ReleaseRefLocked(data, source);
			}

			auto produced = data.guidToAsset.find(guid);
			if (produced != data.guidToAsset.end() && produced->second) {
				return ReviveLocked(data, produced->second);
			}

			SLT_LOG_WARN("Asset", "AssetDatabase: '{}' is no longer in '{}'", key, sourceKey);
			return nullptr;
		}

		Asset* LoadByGuidLocked(AssetDatabaseData& data, const Guid& guid) {
			if (_loadSink) {
				_loadSink->push_back(guid);
			}

			auto assetIt = data.guidToAsset.find(guid);
			if (assetIt != data.guidToAsset.end() && assetIt->second) {
				return ReviveLocked(data, assetIt->second);
			}

			if (Asset* product = LoadProductLocked(data, guid)) {
				return product;
			}
			return FinishLoadLocked(data, SourceForLocked(data, guid).Load(guid));
		}

		void ScanSchemeLocked(AssetDatabaseData& data, std::string_view scheme, bool mintMissingMeta,
							  std::span<const std::string_view> contentRoots, HashSet<Guid>* seen) {
			Path rootPhys = GetSchemeRootPhysical(scheme);
			if (rootPhys.ToString().empty() || !FileSystem::IsDirectory(rootPhys)) {
				return;
			}

			namespace fs = std::filesystem;
			fs::path root(rootPhys.ToString());

			auto indexTree = [&](const fs::path& start) {
				std::error_code ec;
				for (fs::recursive_directory_iterator it(start, ec), end; it != end; it.increment(ec)) {
					if (ec) {
						break;
					}
					std::error_code fileEc;
					if (!it->is_regular_file(fileEc) || fileEc) {
						continue;
					}

					const fs::path& p = it->path();
					str filename = p.filename().string();
					if (ProjectLayout::IsReservedFileName(filename)) {
						continue;
					}

					fs::path rel = fs::relative(p, root, fileEc);
					if (fileEc) {
						continue;
					}
					str relGeneric = rel.generic_string();
					str virtualPath = str(scheme) + "://" + relGeneric;
					str key = Path(virtualPath).ToString();

					Path vpath(virtualPath);
					Path physMeta = FileSystem::Resolve(AssetDatabase::GetMetaPath(vpath));
					Guid guid{};
					StringHash type{};
					Vector<ProductTableEntry> products{GetMemoryResource(MemTag::Asset)};
					if (FileSystem::Exists(physMeta) && ReadMetaFile(physMeta, guid, type, &products) && guid.IsValid()) {
					} else if (mintMissingMeta) {
						guid = AssetDatabase::GenerateGuid();
						IAssetImporter* byExt = FindImporterFor(vpath);
						type = byExt ? byExt->GetAssetType() : StringHash();
						WriteMetaFile(physMeta, vpath, guid, type);
					} else {
						continue;
					}

					data.pathToGuid[key] = guid;
					data.guidToPath[guid] = key;
					if (seen) {
						seen->insert(guid);
					}

					for (const ProductTableEntry& product : products) {
						data.productIndex[product.guid] = {guid, product.key, product.name};
					}
				}
			};

			if (contentRoots.empty()) {
				indexTree(root);
				return;
			}
			for (std::string_view contentRoot : contentRoots) {
				std::error_code ec;
				fs::path start = root / fs::path(contentRoot);
				if (fs::is_directory(start, ec) && !ec) {
					indexTree(start);
				}
			}
		}

		void ScanAllScopesLocked(AssetDatabaseData& data) {
			HashSet<Guid> seen{GetMemoryResource(MemTag::Asset)};

			ScanSchemeLocked(data, EngineScheme, false, {}, &seen);

			Path projectRoot = GetSchemeRootPhysical(ProjectScheme);
			if (projectRoot.ToString().empty() || !FileSystem::IsDirectory(projectRoot)) {
				SLT_LOG_WARN("Asset", "AssetDatabase: project root isn't mounted or missing. Scan skipped");
				return;
			}
			ScanSchemeLocked(data, ProjectScheme, true, ProjectLayout::GetContentRoots(), &seen);
			WriteDatabaseIndexLocked(data);

			ImportArtifactSweepResult swept =
				ImportArtifactSweep(IMPORT_ARTIFACT_BUDGET_BYTES, [&seen](const Guid& guid) { return seen.count(guid) != 0; });
			if (swept.orphans > 0 || swept.evicted > 0) {
				SLT_LOG_INFO("Asset", "import artifact sweep: {} orphaned, {} evicted, {} -> {} bytes", swept.orphans,
							 swept.evicted, swept.bytesBefore, swept.bytesAfter);
			}
		}

		bool DbToGuid(u64 instanceId, Guid* outGuid, StringHash* outPointeeType) {
			auto& data = GetData();
			std::lock_guard lock(data.mutex);

			auto it = data.instanceToGuid.find(instanceId);
			if (it == data.instanceToGuid.end()) {
				return false;
			}
			if (outGuid) {
				*outGuid = it->second;
			}
			if (outPointeeType) {
				auto assetIt = data.guidToAsset.find(it->second);
				*outPointeeType = (assetIt != data.guidToAsset.end() && assetIt->second)
									  ? assetIt->second->GetAssetTypeId()
									  : StringHash();
			}
			return true;
		}

		u32 CollectGarbageLocked(AssetDatabaseData& data) {
			u32 destroyed = 0;
			while (!data.parked.empty()) {
				HashSet<Asset*> pass{GetMemoryResource(MemTag::Asset)};
				pass.swap(data.parked);
				for (Asset* asset : pass) {
					if (!asset || asset->GetRefCount() > 0) {
						continue;
					}
					data.guidToAsset.erase(asset->assetId);
					data.instanceToGuid.erase(asset->GetInstanceId());
					asset->NotifyUnloaded();
					data.parked.erase(asset);
					Delete(asset);
					++destroyed;
				}
			}
			return destroyed;
		}

		u64 DbToHandle(const Guid& guid, StringHash pointeeType) {
			(void)pointeeType;
			auto& data = GetData();
			std::lock_guard lock(data.mutex);

			auto it = data.guidToAsset.find(guid);
			return (it != data.guidToAsset.end() && it->second) ? it->second->GetInstanceId() : 0;
		}

	} // anonymous namespace

	AssetProductBuilder::AssetProductBuilder(AssetDatabaseData& data, const Guid& sourceGuid, const str& sourceKey, Vector<ProductTableEntry>&& table)
		: _data(data), _source(sourceGuid), _sourceKey(sourceKey), _table(std::move(table)),
		  _added(GetMemoryResource(MemTag::Asset)), _context(*this) {
	}

	AssetProductBuilder::~AssetProductBuilder() {
		if (_committed) {
			return;
		}
		for (Asset* product : _added) {
			DestroyResidentLocked(_data, product);
		}
	}

	Guid AssetProductBuilder::Add(std::string_view key, Asset* product, std::string_view name) {
		if (!product) {
			return Guid{};
		}
		if (key.empty()) {
			SLT_LOG_ERROR("Asset", "AssetDatabase: '{}' offered a product with no key", _sourceKey);
			Delete(product);
			return Guid{};
		}

		ProductTableEntry* entry = nullptr;
		for (ProductTableEntry& candidate : _table) {
			if (candidate.key == key) {
				entry = &candidate;
				break;
			}
		}

		if (entry && entry->produced) {
			SLT_LOG_ERROR("Asset", "AssetDatabase: '{}' added '{}' twice (product keys should be unique per source)", _sourceKey, key);
			Delete(product);
			return Guid{};
		}

		if (!entry) {
			_table.push_back(ProductTableEntry{str(key), product->GetAssetTypeId(), AssetDatabase::GenerateGuid(), false});
			entry = &_table.back();
			_tableChanged = true;
		} else if (entry->type != product->GetAssetTypeId()) {
			entry->type = product->GetAssetTypeId();
			_tableChanged = true;
		}
		if (entry->name != name) {
			entry->name = str(name);
			_tableChanged = true;
		}
		entry->produced = true;

		if (!ExtractedFileLocked(_data, entry->guid).empty()) {
			Delete(product);
			return entry->guid;
		}

		if (auto resident = _data.guidToAsset.find(entry->guid); resident != _data.guidToAsset.end() && resident->second) {
			Asset* existing = resident->second;
			if (_keepResident || existing->GetRefCount() > 0) {
				if (!_keepResident) {
					if (existing->CanAdoptReimport() && existing->GetAssetTypeId() == product->GetAssetTypeId()) {
						existing->AdoptReimport(*product);
					} else {
						SLT_LOG_WARN("Asset", "AssetDatabase: '{}' of '{}' is still referenced (loaded instance stays)", entry->key, _sourceKey);
					}
				}
				existing->virtualPath = ProductDisplayPath(_sourceKey, entry->key, entry->name);
				Delete(product);
				return entry->guid;
			}
			DestroyResidentLocked(_data, existing);
		}

		RegisterProductLocked(_data, product, entry->guid, ProductDisplayPath(_sourceKey, entry->key, entry->name));
		_added.push_back(product);
		return entry->guid;
	}

	Guid AssetProductBuilder::Adopt(std::string_view key, const Guid& identity) {
		if (key.empty() || !identity.IsValid()) {
			return Guid{};
		}

		for (const ProductTableEntry& candidate : _table) {
			if (candidate.key == key) {
				return candidate.guid;
			}
		}

		if (identity == _source) {
			SLT_LOG_ERROR("Asset", "AssetDatabase: '{}' offered its identity as the product '{}'", _sourceKey, key);
			return Guid{};
		}
		for (const ProductTableEntry& candidate : _table) {
			if (candidate.guid == identity) {
				SLT_LOG_ERROR("Asset", "AssetDatabase: '{}' offered {} for '{}' ('{}' already holds it)", _sourceKey, identity, key, candidate.key);
				return Guid{};
			}
		}
		if (auto owned = _data.productIndex.find(identity); owned != _data.productIndex.end() && owned->second.source != _source) {
			SLT_LOG_ERROR("Asset", "AssetDatabase: '{}' offered {} for '{}' (already a product of another source)", _sourceKey, identity, key);
			return Guid{};
		}

		_table.push_back(ProductTableEntry{str(key), StringHash{}, identity, false});
		_tableChanged = true;
		return identity;
	}

	bool AssetProductBuilder::CollectProduced(Vector<ImportArtifactProduct>& out) const {
		for (const ProductTableEntry& entry : _table) {
			if (!entry.produced) {
				continue;
			}
			if (!ExtractedFileLocked(_data, entry.guid).empty()) {
				continue;
			}
			auto resident = _data.guidToAsset.find(entry.guid);
			if (resident == _data.guidToAsset.end() || !resident->second) {
				out.clear();
				return false;
			}
			out.push_back(ImportArtifactProduct{entry.key, resident->second});
		}
		return true;
	}

	bool AssetProductBuilder::Commit() {
		_committed = true;
		for (const ProductTableEntry& entry : _table) {
			_data.productIndex[entry.guid] = {_source, entry.key, entry.name};
		}
		for (Asset* product : _added) {
			if (product->GetRefCount() == 0) {
				_data.parked.insert(product);
			}
		}
		return _tableChanged;
	}

	Guid AssetImportContext::AddProduct(std::string_view key, Asset* product, std::string_view name) {
		if (!_builder) {
			Delete(product);
			return Guid{};
		}
		return _builder->Add(key, product, name);
	}

	Guid AssetImportContext::AdoptProduct(std::string_view key, const Guid& identity) {
		if (!_builder) {
			return Guid{};
		}
		return _builder->Adopt(key, identity);
	}

	void DrainTypesTheLoadPathNeeds() {
		TypeRegistry::InitializePrimitives();
		Object::GetReflectType();
		Asset::GetReflectType();
		SLT_ASSET_DATABASE_ANCHORS(SLT_ANCHOR_TYPE)
		TypeRegistry::RegisterPendingTypes();
	}

	void AssetDatabase::Initialize() {
		auto& data = GetData();
		std::lock_guard lock(data.mutex);
		if (data.initialized) {
			return;
		}
		data.initialized = true;

		DrainTypesTheLoadPathNeeds();

		HandleResolver::SetHooks(&DbToGuid, &DbToHandle);

		AssetImporterRegistry::Register(StringHash("TextAsset"), &GetTextImporter(), {".txt"});
		GraphicsImporters::RegisterAll();

		LoadDatabaseIndexLocked(data);

		ScanSchemeLocked(data, EngineScheme, false, {}, nullptr);
	}

	void AssetDatabase::Terminate() {
		LocalizationInternal::Terminate();
		auto& data = GetData();
		std::lock_guard lock(data.mutex);
		if (!data.initialized) {
			return;
		}

		CollectGarbageLocked(data);

		// unload everything before destroying anything
		for (auto& [guid, asset] : data.guidToAsset) {
			if (asset) {
				asset->NotifyUnloaded();
			}
		}
		for (auto& [guid, asset] : data.guidToAsset) {
			Delete(asset);
		}
		data.parked.clear();
		data.guidToAsset.clear();
		data.instanceToGuid.clear();
		data.productIndex.clear();
		data.importsInFlight.clear();
		data.pathToGuid.clear();
		data.guidToPath.clear();
		data.unresolved.clear();
		data.initialized = false;

		// after the last asset (views/spans point into the mount)
		data.package.Unmount();

		HandleResolver::ResetHooks();
		AssetImporterRegistry::Unregister(&GetTextImporter());
		GraphicsImporters::UnregisterAll();
	}

	bool AssetDatabase::MountPackage(const Path& physicalPath, str& outError) {
		auto& data = GetData();
		std::lock_guard lock(data.mutex);

		if (data.package.IsMounted()) {
			outError = std::format("'{}' is already mounted (1 package per build)",
								   data.package.GetPackagePath());
			return false;
		}
		if (!data.package.Mount(physicalPath, outError)) {
			return false;
		}
		SLT_LOG_INFO("Asset", "AssetDatabase: mounted '{}'", physicalPath.ToString());
		return true;
	}

	bool AssetDatabase::ReadPackageDocument(PakDocumentKind kind, str& out) {
		auto& data = GetData();
		std::lock_guard lock(data.mutex);

		out.clear();
		if (!data.package.IsMounted()) {
			return false;
		}
		std::string_view document = data.package.GetDocument(kind);
		if (document.empty()) {
			return false;
		}
		out.assign(document.data(), document.size());
		return true;
	}

	bool AssetDatabase::ReadManagedBundle(void (*onAssembly)(const char* name, const u8* data, u32 size, void* user),
										  void* user) {
		if (!onAssembly) {
			return false;
		}
		str document;
		if (!ReadPackageDocument(PakDocumentKind::ManagedBundle, document)) {
			return false;
		}
		if (document.size() < sizeof(PakManagedBundleHeader)) {
			return false;
		}
		const u8* const bytes = reinterpret_cast<const u8*>(document.data());
		PakManagedBundleHeader header{};
		std::memcpy(&header, bytes, sizeof(header));
		if (header.magic != PAK_MANAGED_BUNDLE_MAGIC || header.entryCount == 0) {
			return false;
		}
		const u64 tableBytes = static_cast<u64>(header.entryCount) * sizeof(PakManagedBundleEntry);
		if (tableBytes > document.size() - sizeof(PakManagedBundleHeader)) {
			return false;
		}
		Buffer scratch;
		for (u32 i = 0; i < header.entryCount; ++i) {
			PakManagedBundleEntry entry{};
			std::memcpy(&entry, bytes + sizeof(PakManagedBundleHeader) + static_cast<size_t>(i) * sizeof(PakManagedBundleEntry),
						sizeof(entry));
			if (entry.nameSize == 0 || entry.nameOffset > document.size() || entry.nameSize > document.size() - entry.nameOffset) {
				return false;
			}
			if (entry.dataOffset > document.size() || entry.storedSize > document.size() - entry.dataOffset) {
				return false;
			}
			str name(reinterpret_cast<const char*>(bytes + entry.nameOffset), entry.nameSize);
			const u8* const stored = bytes + entry.dataOffset;
			if (entry.codec == static_cast<u8>(PakCodec::None)) {
				if (entry.storedSize != entry.rawSize) {
					return false;
				}
				onAssembly(name.c_str(), stored, entry.rawSize, user);
			} else if (entry.codec == static_cast<u8>(PakCodec::LZ4)) {
				if (static_cast<u64>(entry.rawSize) > static_cast<u64>(entry.storedSize) * LZ4_MAX_RATIO) {
					return false;
				}
				if (scratch.size < entry.rawSize) {
					scratch.Allocate(entry.rawSize, MemTag::Asset);
				}
				if (!LZ4::Decompress(stored, entry.storedSize, scratch.data, entry.rawSize)) {
					return false;
				}
				onAssembly(name.c_str(), scratch.data, entry.rawSize, user);
			} else {
				return false;
			}
		}
		return true;
	}

	Asset* AssetDatabase::LoadAsset(const Path& virtualPath) {
		SLT_PROFILE_ZONE("Asset.Load");
		auto& data = GetData();
		std::lock_guard lock(data.mutex);
		return LoadByPathLocked(data, virtualPath.ToString());
	}

	Asset* AssetDatabase::LoadAssetByGuid(const Guid& assetId) {
		if (!assetId.IsValid()) {
			return nullptr;
		}
		SLT_PROFILE_ZONE("Asset.LoadByGuid");
		auto& data = GetData();
		std::lock_guard lock(data.mutex);
		return LoadByGuidLocked(data, assetId);
	}

	Asset* AssetDatabase::FindLoadedByGuid(const Guid& assetId) {
		if (!assetId.IsValid()) {
			return nullptr;
		}
		auto& data = GetData();
		std::lock_guard lock(data.mutex);
		auto it = data.guidToAsset.find(assetId);
		return (it != data.guidToAsset.end()) ? it->second : nullptr;
	}

	bool RefuseWriteLocked(AssetDatabaseData& data, const char* what) {
		if (!data.package.IsMounted()) {
			return false;
		}
		SLT_LOG_ERROR("Asset", "AssetDatabase: {} is a source-tree operation and this build reads a package", what);
		return true;
	}

	void AssetDatabase::SaveAsset(Asset* asset) {
		if (!asset) {
			return;
		}
		auto& data = GetData();
		std::lock_guard lock(data.mutex);
		if (RefuseWriteLocked(data, "SaveAsset")) {
			return;
		}

		if (asset->assetId.IsValid() && data.unresolved.contains(asset->assetId)) {
			SLT_LOG_ERROR("Asset", "AssetDatabase: can't save '{}'. Its type is missing (state preserved unresolved)",
						  asset->virtualPath);
			return;
		}

		if (asset->assetId.IsValid() && data.productIndex.contains(asset->assetId)) {
			SLT_LOG_ERROR("Asset", "AssetDatabase: can't save '{}'. It's a product of another asset (no file)",
						  asset->virtualPath);
			return;
		}

		if (!asset->assetId.IsValid()) {
			asset->assetId = GenerateGuid();
		}
		Guid guid = asset->assetId;
		str key = Path(asset->virtualPath).ToString();

		data.pathToGuid[key] = guid;
		data.guidToPath[guid] = key;

		Path vpath(key);
		asset->Export(FileSystem::Resolve(vpath));
		WriteMetaFile(FileSystem::Resolve(GetMetaPath(vpath)), vpath, guid, asset->GetAssetTypeId());
		WriteDatabaseIndexLocked(data);

		Vector<ProductTableEntry> table{GetMemoryResource(MemTag::Asset)};
		Guid recorded;
		StringHash recordedType;
		if (ReadMetaFile(FileSystem::Resolve(GetMetaPath(vpath)), recorded, recordedType, &table) && !table.empty()) {
			GetLooseSource().RebuildProducts(data, key);
		}
	}

	bool AssetDatabase::WriteImporterSettings(const Path& virtualPath, const JSONValue& settings) {
		if (!settings.IsObject()) {
			SLT_LOG_ERROR("Asset", "AssetDatabase: WriteImporterSettings needs an object for '{}'", virtualPath.ToString());
			return false;
		}
		auto& data = GetData();
		std::lock_guard lock(data.mutex);
		if (RefuseWriteLocked(data, "WriteImporterSettings")) {
			return false;
		}

		Path physMeta = FileSystem::Resolve(GetMetaPath(virtualPath));
		Guid guid;
		StringHash type;
		if (!ReadMetaFile(physMeta, guid, type)) {
			SLT_LOG_ERROR("Asset", "AssetDatabase: '{}' has no sidecar to write settings into", virtualPath.ToString());
			return false;
		}
		return WriteMetaFile(physMeta, virtualPath, guid, type, nullptr, &settings);
	}

	bool AssetDatabase::ReimportAsset(const Path& virtualPath) {
		auto& data = GetData();
		std::lock_guard lock(data.mutex);
		if (RefuseWriteLocked(data, "ReimportAsset")) {
			return false;
		}

		str key = virtualPath.ToString();
		auto pathIt = data.pathToGuid.find(key);
		if (pathIt == data.pathToGuid.end()) {
			SLT_LOG_ERROR("Asset", "AssetDatabase: '{}' isn't a database asset. Nothing to re-import", key);
			return false;
		}
		const Guid sourceGuid = pathIt->second;

		auto resident = data.guidToAsset.find(sourceGuid);
		Asset* source = resident != data.guidToAsset.end() ? resident->second : nullptr;
		if (source && source->GetRefCount() > 0) {
			if (source->CanAdoptReimport()) {
				return AdoptReimportLocked(data, source, key);
			}
			SLT_LOG_WARN("Asset", "AssetDatabase: '{}' is still referenced. Close what uses it and re-import", key);
			return false;
		}

		if (source) {
			DestroyResidentLocked(data, source);
		}

		Asset* fresh = GetLooseSource().LoadFromPath(data, key);
		if (!fresh) {
			SLT_LOG_ERROR("Asset", "AssetDatabase: re-import of '{}' failed (no instance left)", key);
			return false;
		}
		FinishLoadLocked(data, fresh);
		ReleaseRefLocked(data, fresh);
		return true;
	}

	Guid AssetDatabase::CreateAsset(const Path& virtualPath, StringHash typeId, u32 templateIndex) {
		{
			auto& data = GetData();
			std::lock_guard lock(data.mutex);
			if (RefuseWriteLocked(data, "CreateAsset")) {
				return Guid{};
			}
		}

		IAssetImporter* importer = AssetImporterRegistry::FindByExtension(virtualPath.GetExtension());
		if (!importer || importer->GetAssetType() != typeId) {
			importer = AssetImporterRegistry::Find(typeId);
		}
		if (!importer || templateIndex >= importer->GetCreateTemplateCount()) {
			SLT_LOG_ERROR("Asset", "AssetDatabase: type {:#x} can't create '{}'", typeId.GetValue(), virtualPath.ToString());
			return Guid{};
		}

		Path physPath = FileSystem::Resolve(virtualPath);
		if (FileSystem::Exists(physPath)) {
			SLT_LOG_ERROR("Asset", "AssetDatabase: '{}' already exists", virtualPath.ToString());
			return Guid{};
		}

		str parent = physPath.GetDirectory();
		if (!parent.empty() && !FileSystem::Exists(Path(parent))) {
			FileSystem::CreateDirectory(Path(parent));
			if (!FileSystem::Exists(Path(parent))) {
				SLT_LOG_ERROR("Asset", "AssetDatabase: couldn't create directory '{}'", parent);
				return Guid{};
			}
		}

		Buffer source;
		if (importer->CreateTemplateSource(templateIndex, source)) {
			if (!source || !FileSystem::WriteFile(physPath, source)) {
				SLT_LOG_ERROR("Asset", "AssetDatabase: failed to write source for '{}'", virtualPath.ToString());
				return Guid{};
			}
		} else {
			Asset* draft = importer->CreateAsset();
			if (!draft) {
				SLT_LOG_ERROR("Asset", "AssetDatabase: importer produced no draft for '{}'", virtualPath.ToString());
				return Guid{};
			}
			bool exported = draft->Export(physPath);
			Delete(draft);
			if (!exported) {
				SLT_LOG_ERROR("Asset", "AssetDatabase: failed to write '{}'", virtualPath.ToString());
				return Guid{};
			}
		}

		Asset* imported = LoadAsset(virtualPath);
		if (!imported) {
			SLT_LOG_ERROR("Asset", "AssetDatabase: created '{}' but it failed to import", virtualPath.ToString());
			return Guid{};
		}
		Guid guid = imported->assetId;
		UnloadAsset(imported);
		return guid;
	}

	void AssetDatabase::UnloadAsset(Asset* asset) {
		if (!asset) {
			return;
		}
		auto& data = GetData();
		std::lock_guard lock(data.mutex);
		ReleaseRefLocked(data, asset);
	}

	Guid AssetDatabase::EnsureAssetIdentity(const Path& virtualPath) {
		auto& data = GetData();
		std::lock_guard lock(data.mutex);

		str key = virtualPath.ToString();
		if (auto indexed = data.pathToGuid.find(key); indexed != data.pathToGuid.end()) {
			return indexed->second;
		}

		IdentityScratch scratch;
		Identity identity = ResolveIdentityLocked(data, key, scratch);
		return identity.ok ? identity.guid : Guid{};
	}

	u32 AssetDatabase::CollectGarbage() {
		auto& data = GetData();
		std::lock_guard lock(data.mutex);
		return CollectGarbageLocked(data);
	}

	Guid AssetDatabase::GenerateGuid() {
		static std::mutex genMutex;
		static std::mt19937_64 rng(
			(static_cast<u64>(std::random_device{}()) << 32) ^ std::random_device{}() ^
			static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count()));

		std::lock_guard lock(genMutex);
		Guid guid;
		do {
			guid.low = rng();
			guid.high = rng();
		} while (!guid.IsValid());
		return guid;
	}

	Path AssetDatabase::GetMetaPath(const Path& assetPath) {
		return Path(assetPath.ToString() + ".meta");
	}

	void AssetDatabase::ScanProjectDirectory() {
		auto& data = GetData();
		std::lock_guard lock(data.mutex);
		if (RefuseWriteLocked(data, "ScanProjectDirectory")) {
			return;
		}
		ScanAllScopesLocked(data);
	}

	void AssetDatabase::RebuildDatabase() {
		auto& data = GetData();
		std::lock_guard lock(data.mutex);

		data.pathToGuid.clear();
		data.guidToPath.clear();
		data.productIndex.clear();
		ScanAllScopesLocked(data);

		for (auto& [guid, asset] : data.guidToAsset) {
			if (!asset || data.productIndex.contains(guid)) {
				continue;
			}
			auto path = data.guidToPath.find(guid);
			if (path != data.guidToPath.end() && asset->virtualPath != path->second) {
				asset->virtualPath = path->second;
			}
		}
	}

	bool AssetDatabase::IsAssetPath(const Path& path) {
		return AssetImporterRegistry::FindByExtension(path.GetExtension()) != nullptr;
	}

	Guid AssetDatabase::LookupGuid(const Path& path) {
		auto& data = GetData();
		std::lock_guard lock(data.mutex);
		auto it = data.pathToGuid.find(path.ToString());
		return (it != data.pathToGuid.end()) ? it->second : Guid{};
	}

	Path AssetDatabase::LookupPath(const Guid& assetId) {
		auto& data = GetData();
		std::lock_guard lock(data.mutex);
		auto it = data.guidToPath.find(assetId);
		return (it != data.guidToPath.end()) ? Path(it->second) : Path();
	}

	void AssetDatabase::FindAssets(StringHash typeId, Vector<Guid>& out) {
		out.clear();
		auto& data = GetData();
		std::lock_guard lock(data.mutex);
		if (!data.initialized) {
			return;
		}
		if (data.package.IsMounted()) {
			data.package.FindOfType(typeId, out);
		} else {
			for (const auto& [path, guid] : data.pathToGuid) {
				const IAssetImporter* importer = FindImporterFor(Path(path));
				if (importer && importer->GetAssetType() == typeId) {
					out.push_back(guid);
				}
			}
		}
		std::sort(out.begin(), out.end());
	}

	void AssetDatabase::GetProducts(const Guid& sourceGuid, Vector<AssetProduct>& out) {
		out.clear();
		auto& data = GetData();
		std::lock_guard lock(data.mutex);
		auto sourcePath = data.guidToPath.find(sourceGuid);
		if (sourcePath == data.guidToPath.end()) {
			return;
		}

		Vector<ProductTableEntry> table{GetMemoryResource(MemTag::Asset)};
		Guid ignoredGuid;
		StringHash ignoredType;
		Path physMeta = FileSystem::Resolve(GetMetaPath(Path(sourcePath->second)));
		if (!ReadMetaFile(physMeta, ignoredGuid, ignoredType, &table)) {
			return;
		}

		out.reserve(table.size());
		for (const ProductTableEntry& entry : table) {
			out.push_back(AssetProduct{entry.key, entry.type, entry.guid,
									   Path(ExtractedFileLocked(data, entry.guid)), entry.name});
		}
	}

	Guid AssetDatabase::GetProductSource(const Guid& productGuid) {
		auto& data = GetData();
		std::lock_guard lock(data.mutex);
		auto indexed = data.productIndex.find(productGuid);
		return (indexed != data.productIndex.end()) ? indexed->second.source : Guid{};
	}

	bool AssetDatabase::ExtractProduct(const Guid& productGuid, const Path& virtualPath) {
		auto& data = GetData();
		std::lock_guard lock(data.mutex);
		if (RefuseWriteLocked(data, "ExtractProduct")) {
			return false;
		}

		auto indexed = data.productIndex.find(productGuid);
		if (indexed == data.productIndex.end()) {
			SLT_LOG_ERROR("Asset", "AssetDatabase: {} isn't a product of anything and can't be extracted", productGuid);
			return false;
		}
		if (str existing = ExtractedFileLocked(data, productGuid); !existing.empty()) {
			SLT_LOG_ERROR("Asset", "AssetDatabase: '{}' is already extracted to '{}'", indexed->second.key, existing);
			return false;
		}

		str key = Path(virtualPath).ToString();
		if (data.pathToGuid.contains(key) || FileSystem::Exists(FileSystem::Resolve(virtualPath))) {
			SLT_LOG_ERROR("Asset", "AssetDatabase: '{}' already exists. '{}' wasn't extracted", key, indexed->second.key);
			return false;
		}

		Asset* product = LoadAssetByGuid(productGuid);
		if (!product) {
			SLT_LOG_ERROR("Asset", "AssetDatabase: '{}' didn't resolve and can't be extracted", indexed->second.key);
			return false;
		}
		StringHash type = product->GetAssetTypeId();

		Path physPath = FileSystem::Resolve(virtualPath);
		str parent = physPath.GetDirectory();
		if (!parent.empty() && !FileSystem::Exists(Path(parent))) {
			FileSystem::CreateDirectory(Path(parent));
		}
		bool exported = product->Export(physPath);
		UnloadAsset(product);
		if (!exported) {
			SLT_LOG_ERROR("Asset", "AssetDatabase: couldn't write '{}' while extracting '{}'", key, indexed->second.key);
			return false;
		}

		WriteMetaFile(FileSystem::Resolve(GetMetaPath(virtualPath)), virtualPath, productGuid, type);
		data.pathToGuid[key] = productGuid;
		data.guidToPath[productGuid] = key;
		WriteDatabaseIndexLocked(data);

		if (auto resident = data.guidToAsset.find(productGuid);
			resident != data.guidToAsset.end() && resident->second && resident->second->GetRefCount() == 0) {
			DestroyResidentLocked(data, resident->second);
		}

		SLT_LOG_INFO("Asset", "AssetDatabase: extracted '{}' of '{}' to '{}'", indexed->second.key,
					 LookupPath(indexed->second.source).ToString(), key);
		return true;
	}

	std::span<const Guid> AssetDatabase::GetEngineBuiltinRoots() {
		return {EngineBuiltins::Roots, std::size(EngineBuiltins::Roots)};
	}

	bool AssetDatabase::ValidatePackage(const Path& physicalPath, std::span<const Guid> expected, str& outError) {
		PakReader reader;
		if (!reader.Mount(physicalPath, outError)) {
			return false;
		}
		for (const Guid& guid : expected) {
			if (reader.FindAsset(guid) == PAK_INVALID_INDEX) {
				outError = std::format("the package doesn't resolve {} (cooked into it)", guid.ToString());
				return false;
			}
		}
		return true;
	}

	AssetLoadRecorder::AssetLoadRecorder(Vector<Guid>& sink) noexcept {
		SLT_CORE_ASSERT_MSG(_loadSink == nullptr, "an AssetLoadRecorder is already installed on this thread");
		if (_loadSink != nullptr) {
			return;
		}
		_loadSink = &sink;

		HandleResolver::SetObserver(
			+[](void* user, const Guid& guid, StringHash) {
				if (IsAssetGuid(guid)) {
					static_cast<Vector<Guid>*>(user)->push_back(guid);
				}
			},
			&sink);
		_installed = true;
	}

	AssetLoadRecorder::~AssetLoadRecorder() {
		if (_installed) {
			_loadSink = nullptr;
			HandleResolver::SetObserver(nullptr, nullptr);
		}
	}

	namespace ModuleInternal {

		void CaptureAssetDatabaseEngineState(EngineStateTable& table) {
			table.assetDatabase = &GetData();
		}

		void BindAssetDatabaseEngineState(const EngineStateTable& table) {
			AssetInternal::_boundData = static_cast<AssetDatabaseData*>(table.assetDatabase);
		}

	} // namespace ModuleInternal

} // namespace Shalltear

extern "C" SHALLTEAR_CORE_API bool ShalltearReadManagedBundle(
	void (*onAssembly)(const char* name, const Shalltear::u8* data, Shalltear::u32 size, void* user), void* user) {
	return Shalltear::AssetDatabase::ReadManagedBundle(onAssembly, user);
}
