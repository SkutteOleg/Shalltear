#include "Shalltear/Editor/ComponentMenu.h"

#include "Shalltear/Core/Graphics/Camera.h"
#include "Shalltear/Core/Graphics/Environment.h"
#include "Shalltear/Core/Graphics/Light.h"
#include "Shalltear/Core/Graphics/MeshRenderer.h"
#include "Shalltear/Core/Graphics/ProceduralRenderer.h"
#include "Shalltear/Core/Graphics/ReflectionProbe.h"
#include "Shalltear/Core/Graphics/Decal.h"
#include "Shalltear/Core/Graphics/Skeleton.h"
#include "Shalltear/Core/Graphics/LODGroup.h"
#include "Shalltear/Core/Graphics/ObjectShadow.h"
#include "Shalltear/Core/Graphics/SkinnedMeshRenderer.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Scene/Component.h"
#include "Shalltear/Core/UI/UIPanelRenderer.h"

namespace Shalltear::ComponentMenu {

	namespace {

		struct Entry {
			str category;
			const void* owner = nullptr;
		};

		struct State {
			HashMap<StringHash, Entry> categories{GetMemoryResource(MemTag::Editor)};
			bool seeded = false;
		};
		State* _state = nullptr;

		State& Table() {
			if (!_state) {
				_state = New<State>(MemTag::Editor);
			}
			return *_state;
		}

		void Plant(StringHash typeName, std::string_view category, bool onlyAbsent) {
			if (onlyAbsent && Table().categories.contains(typeName)) {
				return;
			}
			Register(typeName, category);
		}

		void SeedBuiltIns(bool onlyAbsent = false) {
			State& table = Table();
			if (table.seeded && !onlyAbsent) {
				return;
			}
			table.seeded = true;
			Plant(Camera::GetReflectType()->name, "Rendering", onlyAbsent);
			Plant(Light::GetReflectType()->name, "Rendering", onlyAbsent);
			Plant(Environment::GetReflectType()->name, "Rendering", onlyAbsent);
			Plant(MeshRenderer::GetReflectType()->name, "Rendering", onlyAbsent);
			Plant(SkinnedMeshRenderer::GetReflectType()->name, "Rendering", onlyAbsent);
			Plant(LODGroup::GetReflectType()->name, "Rendering", onlyAbsent);
			Plant(ObjectShadow::GetReflectType()->name, "Rendering", onlyAbsent);
			Plant(Skeleton::GetReflectType()->name, "Rendering", onlyAbsent);
			Plant(ReflectionProbe::GetReflectType()->name, "Rendering", onlyAbsent);
			Plant(Decal::GetReflectType()->name, "Rendering", onlyAbsent);
			Plant(UIPanelRenderer::GetReflectType()->name, "UI", onlyAbsent);
			Plant(ProceduralRenderer::GetReflectType()->name, "Visual Effects", onlyAbsent);
		}

	} // anonymous namespace

	void RegisterBuiltIns() {
		SeedBuiltIns();
	}

	void Register(StringHash componentTypeName, std::string_view category, const void* owner) {
		if (category.empty()) {
			return;
		}
		Table().categories[componentTypeName] = {str(category), owner};
	}

	u32 UnregisterOwnedBy(const void* owner) {
		if (!owner) {
			return 0;
		}
		if (!_state) {
			return 0;
		}
		u32 removed = 0;
		auto& categories = _state->categories;
		for (auto it = categories.begin(); it != categories.end();) {
			if (it->second.owner == owner) {
				it = categories.erase(it);
				++removed;
			} else {
				++it;
			}
		}
		if (removed > 0) {
			SeedBuiltIns(true);
		}
		return removed;
	}

	std::string_view CategoryFor(const TypeInfo* componentType) {
		if (!componentType) {
			return {};
		}
		if (componentType->attributes && componentType->attributeCount > 0) {
			const StringHash menuKey("AddComponentMenu");
			for (u32 i = 0; i < componentType->attributeCount; ++i) {
				const AttributeInfo& attr = componentType->attributes[i];
				if (attr.key == menuKey && attr.value.Is<str>()) {
					const str& menuPath = attr.value.As<str>();
					if (!menuPath.empty()) {
						const size_t slash = menuPath.rfind('/');
						return slash != str::npos ? std::string_view(menuPath).substr(0, slash)
												  : std::string_view(menuPath);
					}
				}
			}
		}
		SeedBuiltIns();
		const auto& categories = Table().categories;
		for (const TypeInfo* type = componentType; type != nullptr; type = type->parent) {
			auto it = categories.find(type->name);
			if (it != categories.end()) {
				return it->second.category;
			}
			if (type == Component::GetReflectType()) {
				break;
			}
		}
		return {};
	}

	void Shutdown() {
		Delete(_state);
		_state = nullptr;
	}

} // namespace Shalltear::ComponentMenu
