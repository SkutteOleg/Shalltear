#include "Shalltear/Core/Module/ModuleSystem.h"

#include "Shalltear/Core/Asset/AssetCues.h"
#include "Shalltear/Core/Asset/AssetImporter.h"
#include "Shalltear/Core/Asset/DataAsset.h"
#include "Shalltear/Core/Assert.h"
#include "Shalltear/Core/BuildInfo.h"
#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Frame/FrameStages.h"
#include "Shalltear/Core/Graphics/FramePass.h"
#include "Shalltear/Core/Graphics/TextureCodecs.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Module/ModuleAPI.h"
#include "Shalltear/Core/Module/ModuleEvents.h"
#include "Shalltear/Core/Module/TypeReplacement.h"
#include "Shalltear/Core/Object.h"
#include "Shalltear/Core/Profiler.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Reflection/VariantDispatch.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "Shalltear/Core/Settings.h"
#include "Shalltear/Core/UI/WidgetRegistry.h"

#include "Asset/AssetMigration.h"
#include "Graphics/FrameCapture.h"
#include "Module/EngineStateTable.h"
#include "Platform/Library.h"
#include "Scene/SceneMigration.h"
#include "Shalltear/Core/Serialization/HandleResolver.h"
#include "Shalltear/Core/Time.h"

#include <filesystem>
#include <format>
#include <thread>

namespace Shalltear {

	namespace {

		constexpr const char* MODULE_CACHE_DIR = "Library/ModuleCache";

		struct LoadedModule {
			str name;
			Path sourcePath;
			std::filesystem::path cachePath;
			std::filesystem::path previousCachePath;
			void* image = nullptr;
			ShalltearModuleAPI* api = nullptr;
			u32 generation = 1;
			bool active = false;
			ModuleLoadPolicy policy = ModuleLoadPolicy::CacheCopy;
		};

		struct LoaderState {
			Vector<LoadedModule> modules{GetMemoryResource(MemTag::Scripting)};
			EngineContext hostContext{};
			std::thread::id mainThread;
			u32 cacheCounter = 0;
			bool initialized = false;
		};

		LoaderState& GetState() {
			static LoaderState state;
			return state;
		}

		bool IsMainThread(const LoaderState& state) {
			return std::this_thread::get_id() == state.mainThread;
		}

#define SLT_MODULE_ASSERT_MAIN_THREAD(state) \
	SLT_CORE_ASSERT_MSG(IsMainThread(state), "ModuleSystem entry points are main-thread-only")

		LoadedModule* ResolveHandle(LoaderState& state, ModuleHandle handle) {
			if (handle.index >= state.modules.size())
				return nullptr;
			LoadedModule& entry = state.modules[handle.index];
			return (entry.active && entry.generation == handle.generation) ? &entry : nullptr;
		}

		str ToUTF8(const std::filesystem::path& path) {
			const std::u8string utf8 = path.u8string();
			return str(utf8.begin(), utf8.end());
		}

		bool CopyToCache(LoaderState& state, const Path& source, std::filesystem::path& outCache) {
			std::error_code ec;
			std::filesystem::path src = std::filesystem::absolute(source.CStr(), ec);
			if (ec || !std::filesystem::exists(src, ec)) {
				return false;
			}

			std::filesystem::path cacheDir = std::filesystem::absolute(MODULE_CACHE_DIR, ec);
			std::filesystem::create_directories(cacheDir, ec);
			if (ec) {
				SLT_LOG_ERROR("Module", "can't create module cache dir: {}", cacheDir.string());
				return false;
			}

			constexpr u32 MAX_CACHE_SLOTS = 64;
			std::filesystem::path cache;
			for (u32 attempt = 0; attempt < MAX_CACHE_SLOTS; ++attempt) {
				u32 counter = ++state.cacheCounter;
				cache = cacheDir / std::format("{}-{}{}", src.stem().string(), counter, Platform::GetSharedLibrarySuffix());
				ec = std::make_error_code(std::errc::device_or_resource_busy);
				if (Platform::ClaimLibraryFile(ToUTF8(cache))) {
					ec.clear();
					std::filesystem::copy_file(src, cache, std::filesystem::copy_options::overwrite_existing, ec);
					if (!ec) {
						break;
					}
					Platform::ReleaseLibraryFile(ToUTF8(cache));
				}
				if (attempt + 1 == MAX_CACHE_SLOTS) {
					SLT_LOG_ERROR("Module", "failed to copy module into cache: {} -> {} ({}), {} slot(s) tried",
								  src.string(), cache.string(), ec.message(), MAX_CACHE_SLOTS);
					return false;
				}
			}

			const std::string_view symbols = Platform::GetDebugSymbolsSuffix();
			std::filesystem::path pdb = src;
			pdb.replace_extension(symbols);
			if (!symbols.empty() && std::filesystem::exists(pdb, ec)) {
				std::filesystem::path cachePdb = cache;
				cachePdb.replace_extension(symbols);
				std::filesystem::copy_file(pdb, cachePdb, std::filesystem::copy_options::overwrite_existing, ec);
			}

			outCache = std::move(cache);
			return true;
		}

		bool ResolveInPlace(const Path& source, std::filesystem::path& outMapped) {
			std::error_code ec;
			std::filesystem::path src = std::filesystem::absolute(source.CStr(), ec);
			if (ec || !std::filesystem::exists(src, ec)) {
				return false;
			}
			outMapped = std::move(src);
			return true;
		}

		void* OpenImage(const std::filesystem::path& binary, str& outError) {
			const std::u8string utf8 = binary.u8string();
			return Platform::OpenLibrary(str(utf8.begin(), utf8.end()), &outError);
		}

		void DeleteCacheCopy(const std::filesystem::path& cache) {
			if (cache.empty())
				return;
			std::error_code ec;
			std::filesystem::remove(cache, ec);
			Platform::ReleaseLibraryFile(ToUTF8(cache));
			const std::string_view symbols = Platform::GetDebugSymbolsSuffix();
			if (!symbols.empty()) {
				std::filesystem::path pdb = cache;
				pdb.replace_extension(symbols);
				std::filesystem::remove(pdb, ec);
			}
		}

		void LoadCoreExtensions(void* image) {
			auto entry = reinterpret_cast<CoreExtensionRegisterFn>(
				Platform::FindSymbol(image, SLT_CORE_EXTENSION_REGISTER_SYMBOL));
			if (entry) {
				entry(image);
			}
		}

		void RegisterDeclaredImporters(void* image) {
			TypeRegistry::ForEachType(
				+[](const TypeInfo* type, void* user) {
					if (type->ownerModule != user || AssetImporterRegistry::RegisterDeclared(type, user)) {
						return;
					}
					const DeclaredAssetImporter* declared = DeclaredAssetImporter::FindDeclared(type);
					if (declared && !AssetImporterRegistry::Find(type->name)) {
						SLT_LOG_WARN("Module", "{} declares {} files another importer already reads (not in the Create menu)",
									 str(type->displayName), str(declared->GetExtension()));
					}
				},
				image);
		}

		u32 UnloadCoreExtensions(void* image) {
			u32 removed = 0;
			removed += UI::WidgetRegistry::UnregisterOwnedBy(image);
			removed += Settings::UnregisterOwnedBy(image);
			removed += FrameStages::UnregisterOwnedBy(image);
			removed += AssetImporterRegistry::UnregisterOwnedBy(image);
			removed += AssetCues::UnregisterOwnedBy(image);
			removed += FramePasses::UnregisterOwnedBy(image);
			removed += TextureCodecs::UnregisterOwnedBy(image);
			GraphicsInternal::DropCaptureCallbacks();
			return removed;
		}

		void AnnounceImageUnloading(void* image) {
			ModuleEvents::ImageUnloading.Publish(EventBus::GetGlobal(), ImageUnloadingEvent{image});
		}

		bool ValidateAndBind(LoaderState& state, void* image, const std::filesystem::path& mappedFrom, ShalltearModuleAPI** outAPI) {
			auto getAPI = reinterpret_cast<ShalltearModuleGetAPIFn>(
				Platform::FindSymbol(image, SLT_MODULE_GET_API_SYMBOL));
			if (!getAPI) {
				SLT_LOG_ERROR("Module", "{}: missing {} export (not a Shalltear module)",
							  mappedFrom.string(), SLT_MODULE_GET_API_SYMBOL);
				return false;
			}

			ShalltearModuleAPI* api = getAPI(SLT_ENGINE_CONTEXT_VERSION);
			if (!api || !api->bind || !api->registerTypes || !api->shutdown) {
				SLT_LOG_ERROR("Module", "{}: ShalltearModuleGetApi returned an incomplete api", mappedFrom.string());
				return false;
			}

			if (api->contextVersion != SLT_ENGINE_CONTEXT_VERSION) {
				SLT_LOG_ERROR("Module", "{} rejected: context version {} (host {}). Recompile the module for this engine",
							  mappedFrom.string(), api->contextVersion, SLT_ENGINE_CONTEXT_VERSION);
				return false;
			}

#ifdef NDEBUG
			constexpr u32 HOST_RELEASE_FLAG = SLT_MODULE_FLAG_RELEASE;
#else
			constexpr u32 HOST_RELEASE_FLAG = 0u;
#endif
			if ((api->flags & SLT_MODULE_FLAG_RELEASE) != HOST_RELEASE_FLAG) {
				SLT_LOG_ERROR("Module", "'{}' was built in the other configuration and can't be loaded. Recompile it in this one ({}). (rejected {})",
							  mappedFrom.filename().string(),
							  HOST_RELEASE_FLAG != 0u ? "Release" : "Debug",
							  mappedFrom.string());
				return false;
			}

			if (api->abiHash != BuildInfo::GetModuleAbiHash()) {
				SLT_LOG_ERROR("Module", "'{}' was built for a different engine and can't be loaded. Recompile it. (module ABI {:#018x}, host {:#018x}, engine {} {} / {} {}, rejected {})",
							  mappedFrom.filename().string(), api->abiHash, BuildInfo::GetModuleAbiHash(),
							  BuildInfo::GetVersionString(), BuildInfo::GetGitCommit(),
							  BuildInfo::GetCompilerId(), BuildInfo::GetCompilerVersion(),
							  mappedFrom.string());
				return false;
			}

			CaptureEngineContext(state.hostContext);
			if (!api->bind(&state.hostContext)) {
				SLT_LOG_ERROR("Module", "{} rejected: BindEngineContext failed", mappedFrom.string());
				return false;
			}

			api->registerTypes();
			LoadCoreExtensions(image);
			RegisterDeclaredImporters(image);
			*outAPI = api;
			return true;
		}

		bool RejectSceneObjectSubclasses(void* image, const std::filesystem::path& mappedFrom) {
			struct ScanState {
				const void* image;
				const TypeInfo* sceneObjectType;
				const TypeInfo* offender;
			} scan{image, TypeRegistry::FindType(StringHash("SceneObject")), nullptr};

			if (!scan.sceneObjectType)
				return true;

			TypeRegistry::ForEachType(
				+[](const TypeInfo* type, void* user) {
					auto* s = static_cast<ScanState*>(user);
					if (type->ownerModule == s->image && TypeRegistry::IsDerivedFrom(type, s->sceneObjectType) && type != s->sceneObjectType) {
						s->offender = type;
					}
				},
				&scan);

			if (scan.offender) {
				SLT_LOG_ERROR("Module", "{} rejected: registers a SceneObject-derived type ({:#018x}). Module types must be components",
							  mappedFrom.string(), scan.offender->name.GetValue());
				return false;
			}
			return true;
		}

		void AssertNoLiveModuleObjects(void* image) {
#ifndef NDEBUG
			struct SweepState {
				Platform::ImageSpan image;
				u32 liveCount;
				u64 firstId;
				const char* firstType;
			} sweep{Platform::GetImageSpan(image), 0, 0, "<none>"};

			ObjectRegistry::ForEachLiveObject(
				+[](Object* object, u64 id, void* user) {
					auto* s = static_cast<SweepState*>(user);
					const void* vptr = *reinterpret_cast<void* const*>(object);
					if (!s->image.Contains(vptr))
						return;

					if (s->liveCount == 0) {
						s->firstId = id;
						const TypeInfo* type = object->GetType();
						s->firstType = type ? type->displayName : "<unreflected>";
					}
					++s->liveCount;
				},
				&sweep);

			SLT_CORE_ASSERT_MSG(sweep.liveCount == 0,
								"module unload with {} live Object(s) -> vtable is in this image (1st is id {:#018x}, type '{}'). "
								"Destroy module types before the image unmaps! A Core type = this image compiles with Core's export macro",
								sweep.liveCount, sweep.firstId, sweep.firstType);
#else
			(void)image;
#endif
		}

		void FreeModuleImage(void* image) {
			if (!image) {
				return;
			}
			LogManager::DrainBeforeImageUnload();
			Platform::CloseLibrary(image);
		}

		void RunUnloadSweeps(LoadedModule& entry) {
			AnnounceImageUnloading(entry.image);

			entry.api->shutdown();

			AssertNoLiveModuleObjects(entry.image);

			// before the type sweep (settings own module typed instances)
			u32 extensionsSwept = UnloadCoreExtensions(entry.image);

			u32 typesSwept = TypeRegistry::UnregisterOwnedBy(entry.image);
			size_t vtablesSwept = VariantDispatch::UnregisterAllOwnedBy(entry.image);
			u32 nodesUnlinked = TypeRegistry::UnlinkPendingRegistrationsIn(entry.image);

			TypeRegistry::RegisterPendingTypes();

			SLT_LOG_INFO("Module", "unloaded '{}': {} type(s), {} vtable(s), {} registrar node(s), {} core extension(s) swept",
						 entry.name, typesSwept, vtablesSwept, nodesUnlinked, extensionsSwept);
		}

		void UnloadEntry(LoadedModule& entry) {
			AssetInternal::AssetMigrationSnapshot assets;
			{
				ScopedHandleResolver scope(SceneInternal::MigrationResolverScope());
				AssetInternal::CaptureAndDemolishModuleAssets(entry.image, assets);
			}

			RunUnloadSweeps(entry);

			{
				ScopedHandleResolver scope(SceneInternal::MigrationResolverScope());
				AssetInternal::RestoreModuleAssets(assets);
			}

			FreeModuleImage(entry.image);
			entry.image = nullptr;
			entry.api = nullptr;

			DeleteCacheCopy(entry.cachePath);
			DeleteCacheCopy(entry.previousCachePath);
			entry.cachePath.clear();
			entry.previousCachePath.clear();

			entry.active = false;
			entry.generation++;
		}

	} // anonymous namespace

	void ModuleSystem::Initialize() {
		auto& state = GetState();
		SLT_CORE_ASSERT_MSG(!state.initialized, "ModuleSystem::Initialize called twice");
		state.mainThread = std::this_thread::get_id();
		state.initialized = true;
	}

	void ModuleSystem::Terminate() {
		auto& state = GetState();
		if (!state.initialized)
			return;
		SLT_MODULE_ASSERT_MAIN_THREAD(state);

		for (size_t i = state.modules.size(); i-- > 0;) {
			if (state.modules[i].active) {
				UnloadEntry(state.modules[i]);
			}
		}
		state.modules.clear();
		state.initialized = false;
	}

	bool ModuleSystem::IsInitialized() {
		return GetState().initialized;
	}

	ModuleHandle ModuleSystem::Load(const Path& binaryPath, ModuleLoadPolicy policy) {
		auto& state = GetState();
		SLT_CORE_ASSERT_MSG(state.initialized, "ModuleSystem::Load before Initialize");
		if (!state.initialized)
			return {};
		SLT_MODULE_ASSERT_MAIN_THREAD(state);
		SLT_PROFILE_ZONE("Module.Load");

		std::filesystem::path mapped;
		std::filesystem::path owned;
		if (policy == ModuleLoadPolicy::CacheCopy) {
			if (!CopyToCache(state, binaryPath, mapped))
				return {};
			owned = mapped;
		} else if (!ResolveInPlace(binaryPath, mapped)) {
			return {};
		}

		str loadError;
		void* image = OpenImage(mapped, loadError);
		if (!image) {
			SLT_LOG_ERROR("Module", "couldn't load {} ({})", mapped.string(), loadError);
			DeleteCacheCopy(owned);
			return {};
		}

		ShalltearModuleAPI* api = nullptr;
		if (!ValidateAndBind(state, image, mapped, &api) || !RejectSceneObjectSubclasses(image, mapped)) {
			AnnounceImageUnloading(image);
			UnloadCoreExtensions(image);
			TypeRegistry::UnregisterOwnedBy(image);
			VariantDispatch::UnregisterAllOwnedBy(image);
			TypeRegistry::UnlinkPendingRegistrationsIn(image);
			TypeRegistry::RegisterPendingTypes();
			FreeModuleImage(image);
			DeleteCacheCopy(owned);
			return {};
		}

		u32 index = static_cast<u32>(state.modules.size());
		for (u32 i = 0; i < state.modules.size(); ++i) {
			if (!state.modules[i].active) {
				index = i;
				break;
			}
		}
		if (index == state.modules.size()) {
			state.modules.push_back(LoadedModule{});
		}

		LoadedModule& entry = state.modules[index];
		entry.name = api->name ? api->name : "<unnamed>";
		entry.sourcePath = binaryPath;
		entry.cachePath = std::move(owned);
		entry.image = image;
		entry.api = api;
		entry.active = true;
		entry.policy = policy;

		SLT_LOG_INFO("Module", "loaded '{}' from {} ({})", entry.name, binaryPath.ToString(),
					 entry.cachePath.empty() ? std::string("mapped in place")
											 : "cache copy " + entry.cachePath.filename().string());

		SceneInternal::RebuildPreservedComponents();
		AssetInternal::RebuildPreservedAssets();
		return ModuleHandle{index, entry.generation};
	}

	bool ModuleSystem::Unload(ModuleHandle handle) {
		auto& state = GetState();
		if (!state.initialized)
			return false;
		SLT_MODULE_ASSERT_MAIN_THREAD(state);

		LoadedModule* entry = ResolveHandle(state, handle);
		if (!entry)
			return false;

		UnloadEntry(*entry);
		return true;
	}

	bool ModuleSystem::IsLoaded(ModuleHandle handle) {
		auto& state = GetState();
		return ResolveHandle(state, handle) != nullptr;
	}

	u32 ModuleSystem::GetLoadedCount() {
		auto& state = GetState();
		u32 count = 0;
		for (const LoadedModule& entry : state.modules) {
			if (entry.active)
				++count;
		}
		return count;
	}

	str ModuleSystem::GetModuleName(ModuleHandle handle) {
		auto& state = GetState();
		LoadedModule* entry = ResolveHandle(state, handle);
		return entry ? entry->name : str{};
	}

	Path ModuleSystem::GetModuleBinaryPath(ModuleHandle handle) {
		auto& state = GetState();
		LoadedModule* entry = ResolveHandle(state, handle);
		return entry ? entry->sourcePath : Path{};
	}

	void* ModuleSystem::GetNativeHandle(ModuleHandle handle) {
		auto& state = GetState();
		LoadedModule* entry = ResolveHandle(state, handle);
		return entry ? entry->image : nullptr;
	}

	bool ModuleSystem::Reload(ModuleHandle handle, const Path* newBinaryPath) {
		auto& state = GetState();
		if (!state.initialized)
			return false;
		SLT_MODULE_ASSERT_MAIN_THREAD(state);

		SLT_PROFILE_ZONE("Module.Reload");

		LoadedModule* entry = ResolveHandle(state, handle);
		if (!entry) {
			SLT_LOG_ERROR("Module", "Reload: stale module handle");
			return false;
		}

		if (entry->policy == ModuleLoadPolicy::InPlace) {
			SLT_LOG_ERROR("Module", "Can't reload '{}' (mapped in place, no rollback image)", entry->name);
			return false;
		}

		if (!SceneInternal::MigrationPreconditionsMet()) {
			return false;
		}

		Stopwatch timer;
		timer.Restart();
		const Path source = newBinaryPath ? *newBinaryPath : entry->sourcePath;

		SceneInternal::MigrationSnapshot snapshot;
		AssetInternal::AssetMigrationSnapshot assets;
		{
			ScopedHandleResolver scope(SceneInternal::MigrationResolverScope());
			AssetInternal::CaptureAndDemolishModuleAssets(entry->image, assets);
		}
		SceneInternal::CaptureAndDemolishModuleComponents(entry->image, snapshot);

		RunUnloadSweeps(*entry);
		FreeModuleImage(entry->image);
		entry->image = nullptr;
		entry->api = nullptr;

		std::filesystem::path rollbackBinary = entry->cachePath;
		std::filesystem::path newCache;
		void* newImage = nullptr;
		str loadError;
		ShalltearModuleAPI* newAPI = nullptr;
		bool loadedNew = false;

		if (CopyToCache(state, source, newCache)) {
			newImage = OpenImage(newCache, loadError);
			if (newImage) {
				if (ValidateAndBind(state, newImage, newCache, &newAPI) && RejectSceneObjectSubclasses(newImage, newCache)) {
					loadedNew = true;
				} else {
					AnnounceImageUnloading(newImage);
					UnloadCoreExtensions(newImage);
					TypeRegistry::UnregisterOwnedBy(newImage);
					VariantDispatch::UnregisterAllOwnedBy(newImage);
					TypeRegistry::UnlinkPendingRegistrationsIn(newImage);
					TypeRegistry::RegisterPendingTypes();
					FreeModuleImage(newImage);
					newImage = nullptr;
				}
			} else {
				SLT_LOG_ERROR("Module", "Reload: couldn't load {} ({})", newCache.string(), loadError);
			}
			if (!loadedNew) {
				DeleteCacheCopy(newCache);
			}
		}

		if (!loadedNew) {
			SLT_LOG_WARN("Module", "Reload: new binary rejected, rolling back to the previous binary");
			newImage = OpenImage(rollbackBinary, loadError);
			if (newImage && ValidateAndBind(state, newImage, rollbackBinary, &newAPI)) {
				newCache = rollbackBinary;
				rollbackBinary.clear();
			} else {
				if (newImage) {
					FreeModuleImage(newImage);
				}
				{
					ScopedHandleResolver scope(SceneInternal::MigrationResolverScope());
					AssetInternal::RestoreModuleAssets(assets);
				}
				SceneInternal::RestoreModuleComponents(snapshot);
				DeleteCacheCopy(entry->cachePath);
				DeleteCacheCopy(entry->previousCachePath);
				entry->cachePath.clear();
				entry->previousCachePath.clear();
				entry->active = false;
				entry->generation++;
				SLT_LOG_ERROR("Module", "Reload: rollback failed. Module '{}' is unloaded, {} component(s) preserved unresolved",
							  entry->name, snapshot.envelopes.size());
				return false;
			}
		}

		DeleteCacheCopy(entry->previousCachePath);
		entry->previousCachePath = rollbackBinary;
		entry->cachePath = std::move(newCache);
		entry->image = newImage;
		entry->api = newAPI;
		entry->name = newAPI->name ? newAPI->name : entry->name;
		if (newBinaryPath) {
			entry->sourcePath = *newBinaryPath;
		}

		{
			ScopedHandleResolver scope(SceneInternal::MigrationResolverScope());
			AssetInternal::RestoreModuleAssets(assets);
		}

		SceneInternal::RestoreModuleComponents(snapshot);
		SceneInternal::RebuildPreservedComponents();
		AssetInternal::RebuildPreservedAssets();

		SLT_LOG_INFO("Module", "reloaded '{}' in {:.1f} ms ({} component(s) migrated){}",
					 entry->name, timer.GetElapsedMilliseconds(), snapshot.envelopes.size(),
					 loadedNew ? "" : " [ROLLBACK]");
		return loadedNew;
	}

#undef SLT_MODULE_ASSERT_MAIN_THREAD

	bool TypeReplacement::CanReplace() {
		return SceneInternal::IsMigrationLegal();
	}

	bool TypeReplacement::Replace(const void* owner, bool (*swap)(void* user), void* user) {
		if (!owner || !swap || !SceneInternal::MigrationPreconditionsMet()) {
			return false;
		}
		SLT_PROFILE_ZONE("Module.Replace");

		SceneInternal::MigrationSnapshot snapshot;
		AssetInternal::AssetMigrationSnapshot assets;
		{
			ScopedHandleResolver scope(SceneInternal::MigrationResolverScope());
			AssetInternal::CaptureAndDemolishModuleAssets(owner, assets);
		}
		SceneInternal::CaptureAndDemolishModuleComponents(owner, snapshot);

		const bool swapped = swap(user);

		{
			ScopedHandleResolver scope(SceneInternal::MigrationResolverScope());
			AssetInternal::RestoreModuleAssets(assets);
		}
		SceneInternal::RestoreModuleComponents(snapshot);
		SceneInternal::RebuildPreservedComponents();
		AssetInternal::RebuildPreservedAssets();
		return swapped;
	}

} // namespace Shalltear
