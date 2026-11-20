#pragma once

#include "ObjectBindings.h"

#include "Shalltear/Core/Math/Quat.h"
#include "Shalltear/Core/Math/Vector.h"
#include "Shalltear/Core/Scene/Prefab.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneAsset.h"
#include "Shalltear/Core/Types.h"

#include <span>
#include <string_view>

// refusals thrown as std::runtime_error
namespace Shalltear::CSharpImpl::SceneBindings {

	Scene* CreateScene(std::string_view name);
	Scene* LoadScene(SceneAsset& scene);
	void ReplaceScenes(SceneAsset& scene, Scene* keep);
	// returned count may exceed ids.size()
	i32 FillLoadedScenes(std::span<u64> ids) noexcept;

	SceneObject* GetRoot(Scene& scene, i32 index);
	// returned count may exceed ids.size()
	i32 FillObjects(Scene& scene, std::span<u64> ids);

	SceneObject* CreateObject(std::string_view name, Scene* scene, SceneObject* parent);
	SceneObject* InstantiatePrefab(PrefabAsset& prefab, Scene* scene, SceneObject* parent, const Vec3& position, const Quat& rotation);

} // namespace Shalltear::CSharpImpl::SceneBindings
