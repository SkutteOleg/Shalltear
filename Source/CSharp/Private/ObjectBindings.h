#pragma once

#include "Shalltear/Core/Scene/Component.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/Types.h"

#include <span>
#include <string_view>

// refusals thrown as std::runtime_error
namespace Shalltear::CSharpImpl::ObjectBindings {

	bool IsAlive(u64 id) noexcept;

	// ids[N] alive -> bit N % 32 of alive[N / 32]
	void AreAlive(std::span<const u64> ids, std::span<u32> alive);

	// GC handles owned by proxies
	i64 FindScript(SceneObject& object, i32 scriptIndex);
	// returned count may exceed handles.size()
	i32 FindScripts(SceneObject& object, i32 scriptIndex, std::span<i64> handles);
	i64 AddScript(SceneObject& object, i32 scriptIndex);

	void SetActive(SceneObject& object, bool active);
	void SetParent(SceneObject& object, SceneObject* parent, bool worldStays);
	void SetSiblingIndex(SceneObject& object, i32 index);
	SceneObject* GetChild(SceneObject& object, i32 index);
	void Destroy(u64 objectId);
	void SetEnabled(Component& component, bool enabled);

	SceneObject* Instantiate(SceneObject& source, SceneObject* parent);
	SceneObject* FindObjectByName(std::string_view name);

} // namespace Shalltear::CSharpImpl::ObjectBindings
