#pragma once

#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/StringHash.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/Editor/EditorAPI.h"

#include <functional>

namespace Shalltear {

	class Component;
	class SceneObject;
	struct TypeInfo;
	namespace UI {
		struct DragPayload;
	} // namespace UI

	namespace ComponentDrops {

		using ResolveFn = std::function<const TypeInfo*(const Guid& asset)>;

		SHALLTEAR_EDITOR_API bool Register(StringHash assetType, ResolveFn resolve, const void* owner);
		SHALLTEAR_EDITOR_API u32 UnregisterOwnedBy(const void* owner);

		// resolve not called (true for all assets of a registered type)
		SHALLTEAR_EDITOR_API bool Accepts(const UI::DragPayload& payload);
		SHALLTEAR_EDITOR_API Handle<Component> Drop(const UI::DragPayload& payload, Handle<SceneObject> object);

		SHALLTEAR_EDITOR_API void Terminate();

	} // namespace ComponentDrops

} // namespace Shalltear
