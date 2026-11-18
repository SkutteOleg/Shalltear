#include "ObjectBindings.h"

#include "ManagedBehavior.h"
#include "ManagedTypes.h"

#include "Shalltear/Core/Object.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneSystem.h"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace Shalltear::CSharpImpl::ObjectBindings {

	namespace {

		struct Wanted {
			const ManagedType* type;
			const void* token;
			bool known;

			explicit Wanted(i32 scriptIndex)
				: type(ManagedTypes::FindByIndex(scriptIndex)), token(ManagedTypes::GetOwnerToken()), known(type || scriptIndex == 0) {}

			i64 ScriptOf(Component* component) const {
				if (!component || component->IsPendingDestroy() || component->GetType()->ownerModule != token) {
					return 0;
				}
				const ManagedBehavior& proxy = static_cast<const ManagedBehavior&>(*component);
				if (!proxy.GetInstance()) {
					return 0;
				}
				const ManagedType* held = &proxy.GetManagedType();
				while (type && held && held != type) {
					held = held->parent;
				}
				return held ? static_cast<i64>(proxy.GetInstance()) : 0;
			}
		};

		void RequireOutsideLifecycle(std::string_view member) {
			if (ManagedTypes::GetLifecycleDepth() > 0) {
				throw std::runtime_error(
					std::format("{} called inside OnAttach/OnDetach/OnEnable/OnDisable/OnDestroy. Call from OnUpdate", member));
			}
		}

	} // anonymous namespace

	bool IsAlive(u64 id) noexcept {
#ifdef SLT_CSHARP_TEST_HOOKS
		++ManagedTypes::GetCounters().aliveChecks;
#endif
		return ObjectRegistry::Resolve(id) != nullptr;
	}

	bool IsA(u64 id, u64 typeHash) {
		const Object* object = ObjectRegistry::Resolve(id);
		const TypeInfo* type = TypeRegistry::FindType(StringHash(typeHash));
		return object && type && TypeRegistry::IsDerivedFrom(object->GetType(), type);
	}

	void AreAlive(std::span<const u64> ids, std::span<u32> alive) {
		std::fill(alive.begin(), alive.end(), 0u);
		for (size_t index = 0; index < ids.size() && index / 32 < alive.size(); ++index) {
			if (ObjectRegistry::Resolve(ids[index])) {
				alive[index / 32] |= 1u << (index % 32);
			}
		}
	}

	i64 FindScript(SceneObject& object, i32 scriptIndex) {
		const Wanted wanted(scriptIndex);
		for (u32 index = 0; wanted.known && index < object.GetComponentCount(); ++index) {
			if (const i64 script = wanted.ScriptOf(object.GetComponentAt(index).Get())) {
				return script;
			}
		}
		return 0;
	}

	i32 FindScripts(SceneObject& object, i32 scriptIndex, std::span<i64> handles) {
		const Wanted wanted(scriptIndex);
		i32 count = 0;
		for (u32 index = 0; wanted.known && index < object.GetComponentCount(); ++index) {
			if (const i64 script = wanted.ScriptOf(object.GetComponentAt(index).Get())) {
				if (static_cast<size_t>(count) < handles.size()) {
					handles[static_cast<size_t>(count)] = script;
				}
				++count;
			}
		}
		return count;
	}

	i64 AddScript(SceneObject& object, i32 scriptIndex) {
		RequireOutsideLifecycle("SceneObject.AddComponent");
		const Wanted wanted(scriptIndex);
		if (!wanted.type) {
			throw std::runtime_error("SceneObject.AddComponent got an unknown script class");
		}
		const TypeInfo* type = wanted.type->info;
		if (type->flags & TypeFlagAbstract) {
			throw std::runtime_error(std::format("SceneObject.AddComponent can't add {} (abstract)", type->displayName));
		}
		if (object.IsPendingDestroy()) {
			throw std::runtime_error("SceneObject.AddComponent used on object pending destroy");
		}
		const i64 script = wanted.ScriptOf(object.AddComponent(type).Get());
		if (!script) {
			throw std::runtime_error(std::format("SceneObject.AddComponent got no {} instance (constructor failed)", type->displayName));
		}
		return script;
	}

	void SetActive(SceneObject& object, bool active) {
		RequireOutsideLifecycle("SceneObject.SetActive");
		if (!object.IsPendingDestroy()) {
			object.SetActive(active);
		}
	}

	void SetParent(SceneObject& object, SceneObject* parent, bool worldStays) {
		RequireOutsideLifecycle("SceneObject.SetParent");
		if (object.IsPendingDestroy()) {
			return;
		}
		if (parent && parent->IsPendingDestroy()) {
			throw std::runtime_error("SceneObject.SetParent got parent pending destroy");
		}
		for (SceneObject* ancestor = parent; ancestor; ancestor = ancestor->GetParent().Get()) {
			if (ancestor == &object) {
				throw std::runtime_error("SceneObject.SetParent got object/descendant as parent");
			}
		}
		object.SetParent(Handle<SceneObject>(parent), worldStays);
	}

	void SetSiblingIndex(SceneObject& object, i32 index) {
		RequireOutsideLifecycle("SceneObject.SiblingIndex");
		if (!object.IsPendingDestroy()) {
			object.SetSiblingIndex(index < 0 ? 0u : static_cast<u32>(index));
		}
	}

	SceneObject* GetChild(SceneObject& object, i32 index) {
		return static_cast<u32>(index) < object.GetChildCount() ? object.GetChild(static_cast<u32>(index)).Get() : nullptr;
	}

	void Destroy(u64 objectId) {
		SceneSystem::Destroy(Handle<SceneObject>(static_cast<SceneObject*>(ObjectRegistry::Resolve(objectId))));
	}

	void SetEnabled(Component& component, bool enabled) {
		RequireOutsideLifecycle("Component.SetEnabled");
		if (!component.IsPendingDestroy()) {
			component.SetEnabled(enabled);
		}
	}

	SceneObject* Instantiate(SceneObject& source, SceneObject* parent) {
		RequireOutsideLifecycle("SceneSystem.Instantiate");
		if (source.IsPendingDestroy()) {
			throw std::runtime_error("SceneSystem.Instantiate got source pending destroy");
		}
		if (parent && parent->IsPendingDestroy()) {
			throw std::runtime_error("SceneSystem.Instantiate got parent pending destroy");
		}
		SceneObject* copy = SceneSystem::Instantiate(Handle<SceneObject>(&source), Handle<SceneObject>(parent)).Get();
		if (!copy) {
			throw std::runtime_error(std::format("SceneSystem.Instantiate couldn't copy '{}'", source.name));
		}
		return copy;
	}

	SceneObject* FindObjectByName(std::string_view name) {
		for (const Handle<Scene>& scene : SceneSystem::GetLoadedScenes()) {
			if (const Scene* loaded = scene.Get()) {
				if (SceneObject* found = loaded->FindObjectByName(name).Get()) {
					return found;
				}
			}
		}
		return nullptr;
	}

} // namespace Shalltear::CSharpImpl::ObjectBindings
