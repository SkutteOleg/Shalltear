#include "SceneBindings.h"

#include <format>
#include <stdexcept>

namespace Shalltear::CSharpImpl::SceneBindings {

	namespace {

		struct Fill {
			std::span<u64> ids;
			i32 count = 0;

			void Add(u64 id) {
				if (static_cast<size_t>(count) < ids.size()) {
					ids[static_cast<size_t>(count)] = id;
				}
				++count;
			}
		};

		void RequireParent(std::string_view member, const SceneObject* parent) {
			if (parent && parent->IsPendingDestroy()) {
				throw std::runtime_error(std::format("{} got parent pending destroy", member));
			}
		}

	} // anonymous namespace

	Scene* CreateScene(std::string_view name) {
		Scene* scene = SceneSystem::CreateScene(name).Get();
		if (!scene) {
			throw std::runtime_error(std::format("SceneSystem.CreateScene couldn't create '{}'", name));
		}
		return scene;
	}

	Scene* LoadScene(SceneAsset& scene) {
		ObjectBindings::RequireOutsideLifecycle("SceneSystem.LoadScene");
		Scene* loaded = SceneSystem::LoadSceneByGuid(scene.assetId).Get();
		if (!loaded) {
			throw std::runtime_error(std::format("SceneSystem.LoadScene couldn't load '{}'", scene.virtualPath));
		}
		return loaded;
	}

	void ReplaceScenes(SceneAsset& scene, Scene* keep) {
		SceneSystem::ReplaceScenes(scene.assetId, Handle<Scene>(keep));
	}

	i32 FillLoadedScenes(std::span<u64> ids) noexcept {
		Fill fill{ids};
		for (const Handle<Scene>& scene : SceneSystem::GetLoadedScenes()) {
			fill.Add(scene.GetInstanceId());
		}
		return fill.count;
	}

	SceneObject* GetRoot(Scene& scene, i32 index) {
		return static_cast<u32>(index) < scene.GetRootCount() ? scene.GetRoot(static_cast<u32>(index)).Get() : nullptr;
	}

	i32 FillObjects(Scene& scene, std::span<u64> ids) {
		Fill fill{ids};
		scene.ForEachObject(+[](SceneObject& object, void* user) { static_cast<Fill*>(user)->Add(object.GetInstanceId()); }, &fill);
		return fill.count;
	}

	SceneObject* CreateObject(std::string_view name, Scene* scene, SceneObject* parent) {
		ObjectBindings::RequireOutsideLifecycle("SceneSystem.CreateObject");
		RequireParent("SceneSystem.CreateObject", parent);
		SceneObject* object = SceneSystem::CreateObject(name, Handle<Scene>(scene), Handle<SceneObject>(parent)).Get();
		if (!object) {
			throw std::runtime_error("SceneSystem.CreateObject has no loaded scene to add to");
		}
		return object;
	}

	SceneObject* InstantiatePrefab(PrefabAsset& prefab, Scene* scene, SceneObject* parent, const Vec3& position, const Quat& rotation) {
		ObjectBindings::RequireOutsideLifecycle("SceneSystem.Instantiate");
		RequireParent("SceneSystem.Instantiate", parent);
		SceneObject* root = SceneSystem::Instantiate(prefab.assetId, Handle<Scene>(scene), Handle<SceneObject>(parent), position, rotation).Get();
		if (!root) {
			throw std::runtime_error(std::format("SceneSystem.Instantiate couldn't instantiate '{}'", prefab.virtualPath));
		}
		return root;
	}

} // namespace Shalltear::CSharpImpl::SceneBindings
