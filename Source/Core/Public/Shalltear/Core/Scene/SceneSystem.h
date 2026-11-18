#pragma once

#include "Shalltear/Core/CoreAPI.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/Math/Quat.h"
#include "Shalltear/Core/Math/Vector.h"
#include "Shalltear/Core/Types.h"

#include <span>
#include <string_view>

namespace Shalltear {

	class Scene;
	class SceneObject;
	class Component;
	class SceneLoadOperation;
	struct PrefabInstanceDiff;
	struct PrefabTargetPath;

	class SHALLTEAR_CORE_API SceneSystem {
	public:
		static void Initialize();
		static void Terminate();
		static bool IsInitialized();

		static Handle<Scene> CreateScene(std::string_view name);

		static Handle<Scene> CreateIsolatedScene(std::string_view name);
		static u32 GetIsolatedSceneCount();
		static Handle<Scene> LoadScene(const Path& path);

		// only load that works in a cooked game
		static Handle<Scene> LoadSceneByGuid(const Guid& assetId);
		static Handle<SceneLoadOperation> LoadSceneAsync(const Path& path);
		static void SetAsyncLoadBudget(f64 seconds);
		static f64 GetAsyncLoadBudget();
		static void CancelAsyncLoads();

		// switches at the end of this frame
		static void ReplaceScenes(const Guid& sceneAssetId, Handle<Scene> keep = {});

		static void UnloadScene(Handle<Scene> scene);
		static void UnloadAllScenes();

		static void SetPlaying(bool playing);
		static std::span<const Handle<Scene>> GetLoadedScenes();
		static Handle<Scene> GetPrimaryScene();
		static void SetPrimaryScene(Handle<Scene> scene);
		static bool SaveScene(Handle<Scene> scene, const Path& path, bool binary = false);

		static bool SaveSceneToMemory(Handle<Scene> scene, Buffer& outSnapshot);
		static Handle<Scene> LoadSceneFromMemory(const Buffer& snapshot);

		// release reports rows only for types still unregistered then
		static void HoldUnresolvedComponentReports();
		// reported once all holds are released
		static void ReleaseUnresolvedComponentReports();

		static Handle<SceneObject> CreateObject(std::string_view name,
												Handle<Scene> scene = {}, Handle<SceneObject> parent = {});
		static Handle<SceneObject> Instantiate(const Guid& prefabId,
											   Handle<Scene> scene = {}, Handle<SceneObject> parent = {},
											   const Vec3& position = {}, const Quat& rotation = Quat::Identity());
		static Handle<SceneObject> Instantiate(Handle<SceneObject> source, Handle<SceneObject> parent = {});
		static Handle<SceneObject> InstantiateDetached(const Guid& prefabId);
		static void DestroyDetached(Handle<SceneObject> root);
		static bool ApplyDetachedToPrefab(Handle<SceneObject> root, const Guid& prefabId);
		static void Destroy(Handle<SceneObject> object);
		static void Destroy(Handle<Component> component);

		static bool IsPrefabLinked(Handle<SceneObject> object);
		static Guid SavePrefab(Handle<SceneObject> root, const Path& path);
		static bool IsPrefabInstanceRoot(Handle<SceneObject> object);
		static Guid CreatePrefabVariant(const Guid& basePrefab, const Path& path);
		static Guid SavePrefabVariant(Handle<SceneObject> instanceRoot, const Path& path);
		static bool ApplyInstanceOverridesToBase(Handle<SceneObject> instanceRoot);
		static bool RevertInstanceOverrides(Handle<SceneObject> instanceRoot);
		static bool DiffPrefabInstance(Handle<SceneObject> instanceRoot, PrefabInstanceDiff& out,
									   const Guid& against = Guid{});
		static bool RevertInstanceOverrides(Handle<SceneObject> instanceRoot, std::span<const PrefabTargetPath> rows);
		static void NotifyPrefabChanged(const Guid& prefabId);
		static u32 GetPrefabEpoch();

		static void BeginFrame();
		static void FixedUpdateAll();
		static void UpdateAll();
		static void LateUpdateAll();
		static void EndFrame();
	};

} // namespace Shalltear
