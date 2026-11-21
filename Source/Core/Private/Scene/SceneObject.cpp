#include "Shalltear/Core/Scene/SceneObject.h"

#include "SceneInternal.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Math/MathUtil.h"
#include "Shalltear/Core/Scene/Behavior.h"
#include "Shalltear/Core/Scene/Scene.h"

#include <algorithm>
#include <thread>

namespace Shalltear {

	using namespace SceneInternal;

	namespace {

		Behavior* AsBehavior(const TypeInfo* slotType, Component* component) {
			return TypeRegistry::IsDerivedFrom(slotType, Behavior::GetReflectType())
					   ? static_cast<Behavior*>(component)
					   : nullptr;
		}

		constexpr u32 MAX_REQUIRE_DEPTH = 16;

		struct ActiveRequireStack {
			const TypeInfo* types[MAX_REQUIRE_DEPTH]{};
			u32 count = 0;

			bool Contains(const TypeInfo* type) const noexcept {
				for (u32 i = 0; i < count; ++i) {
					if (types[i] == type) {
						return true;
					}
				}
				return false;
			}
		};

		thread_local ActiveRequireStack _requireStack;

		struct RequireScopeGuard {
			bool pushed = false;

			explicit RequireScopeGuard(const TypeInfo* type) noexcept {
				if (type && _requireStack.count < MAX_REQUIRE_DEPTH) {
					_requireStack.types[_requireStack.count++] = type;
					pushed = true;
				}
			}

			~RequireScopeGuard() {
				if (pushed && _requireStack.count > 0) {
					--_requireStack.count;
				}
			}
		};

		const TypeInfo* FindDisallowMultipleOwner(const TypeInfo* type) {
			const StringHash key("DisallowMultipleComponent");
			for (const TypeInfo* cur = type; cur != nullptr; cur = cur->parent) {
				if (!cur->attributes || cur->attributeCount == 0) {
					continue;
				}
				for (u32 i = 0; i < cur->attributeCount; ++i) {
					if (cur->attributes[i].key == key) {
						return cur;
					}
				}
			}
			return nullptr;
		}

	} // anonymous namespace

	SceneObject::SceneObject() = default;

	SceneObject::~SceneObject() = default;

	Handle<Scene> SceneObject::GetScene() const {
		return Handle<Scene>(_scene);
	}

	void SceneObject::SetActive(bool active) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SLT_CORE_ASSERT_MSG(IsStructuralMutationLegal(), "SetActive is illegal inside lifecycle hooks");
		SLT_CORE_ASSERT_MSG(!_pendingDestroy, "SetActive on a doomed object");
		if (!IsStructuralMutationLegal() || _pendingDestroy) {
			return;
		}
		if (_activeSelf == active) {
			return;
		}
		_activeSelf = active;
		RefreshActiveSubtree(*this);
	}

	Handle<SceneObject> SceneObject::GetParent() const {
		return Handle<SceneObject>(_parent);
	}

	void SceneObject::SetParent(Handle<SceneObject> parent, bool worldStays) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SLT_CORE_ASSERT_MSG(IsStructuralMutationLegal(), "SetParent is illegal inside lifecycle hooks");
		SLT_CORE_ASSERT_MSG(!_pendingDestroy, "SetParent on a doomed object");
		if (!IsStructuralMutationLegal() || _pendingDestroy || !_scene) {
			return;
		}

		SceneObject* newParent = parent.Get();
		if (newParent == _parent) {
			return;
		}
		if (newParent) {
			SLT_CORE_ASSERT_MSG(!newParent->_pendingDestroy, "SetParent under a doomed parent");
			if (newParent->_pendingDestroy || !newParent->_scene) {
				return;
			}
			for (SceneObject* ancestor = newParent; ancestor; ancestor = ancestor->_parent) {
				if (ancestor == this) {
					SLT_CORE_ASSERT_MSG(false, "SetParent would create a hierarchy cycle");
					return;
				}
			}
		}

		Vec3 worldPosition{};
		Quat worldRotation = Quat::Identity();
		Vec3 worldScale{1.0f, 1.0f, 1.0f};
		if (worldStays) {
			worldPosition = GetWorldPosition();
			worldRotation = GetWorldRotation();
			worldScale = GetLossyScale();
		}

		Vector<SceneObject*>& oldSiblings = _parent ? _parent->_children : _scene->_roots;
		oldSiblings.erase(std::find(oldSiblings.begin(), oldSiblings.end(), this));

		// tick unregistration runs while the old scene pointer is set
		Scene* targetScene = newParent ? newParent->_scene : _scene;
		const bool migrated = targetScene != _scene;
		if (migrated) {
			struct Migrator {
				Scene* from;
				Scene* to;
				void Visit(SceneObject& node) {
					for (const auto& slot : node._components) {
						if (Behavior* behavior = AsBehavior(slot.type, slot.ptr)) {
							UnregisterTicks(*behavior);
						}
						GetState().ownerById[slot.ptr->GetInstanceId()] = to;
					}
					Vector<SceneObject*>& objects = from->_objects;
					objects.erase(std::find(objects.begin(), objects.end(), &node));
					to->_objects.push_back(&node);
					GetState().ownerById[node.GetInstanceId()] = to;
					node._scene = to;
					for (SceneObject* child : node._children) {
						Visit(*child);
					}
				}
			};
			Migrator migrator{_scene, targetScene};
			migrator.Visit(*this);
		}

		_parent = newParent;
		if (newParent) {
			newParent->_children.push_back(this);
		} else {
			_scene->_roots.push_back(this);
		}

		if (worldStays) {
			if (_parent) {
				_localPosition = _parent->InverseTransformPoint(worldPosition);
				_localRotation = _parent->GetWorldRotation().Inverse() * worldRotation;
				Vec3 parentScale = _parent->GetLossyScale();
				_localScale = Vec3(
					Math::Abs(parentScale.x) > Math::EPSILON ? worldScale.x / parentScale.x : worldScale.x,
					Math::Abs(parentScale.y) > Math::EPSILON ? worldScale.y / parentScale.y : worldScale.y,
					Math::Abs(parentScale.z) > Math::EPSILON ? worldScale.z / parentScale.z : worldScale.z);
			} else {
				_localPosition = worldPosition;
				_localRotation = worldRotation;
				_localScale = worldScale;
			}
			DeriveScaleKind();
		}

		MarkWorldDirtyRecursive();
		RefreshActiveSubtree(*this);

		if (migrated) {
			struct TickSweeper {
				static void Visit(SceneObject& node) {
					for (const auto& slot : node._components) {
						if (slot.ptr->IsEffectivelyEnabled() && !slot.ptr->IsPendingDestroy()) {
							if (Behavior* behavior = AsBehavior(slot.type, slot.ptr)) {
								RegisterTicks(*behavior);
							}
						}
					}
					for (SceneObject* child : node._children) {
						Visit(*child);
					}
				}
			};
			TickSweeper::Visit(*this);
		}
	}

	Handle<SceneObject> SceneObject::GetChild(u32 index) const {
		if (index >= _children.size()) {
			return Handle<SceneObject>();
		}
		return Handle<SceneObject>(_children[index]);
	}

	u32 SceneObject::GetSiblingIndex() const {
		if (!_parent && !_scene) {
			return 0;
		}
		const Vector<SceneObject*>& siblings = _parent ? _parent->_children : _scene->_roots;
		auto it = std::find(siblings.begin(), siblings.end(), this);
		return it != siblings.end() ? static_cast<u32>(it - siblings.begin()) : 0;
	}

	void SceneObject::SetSiblingIndex(u32 index) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SLT_CORE_ASSERT_MSG(IsStructuralMutationLegal(), "SetSiblingIndex is illegal inside lifecycle hooks");
		SLT_CORE_ASSERT_MSG(!_pendingDestroy, "SetSiblingIndex on a doomed object");
		if (!IsStructuralMutationLegal() || _pendingDestroy || (!_parent && !_scene)) {
			return;
		}
		Vector<SceneObject*>& siblings = _parent ? _parent->_children : _scene->_roots;
		auto it = std::find(siblings.begin(), siblings.end(), this);
		if (it == siblings.end()) {
			return;
		}
		siblings.erase(it);
		index = std::min(index, static_cast<u32>(siblings.size()));
		siblings.insert(siblings.begin() + index, this);
	}

	Handle<SceneObject> SceneObject::FindChildByName(std::string_view childName) const {
		for (SceneObject* child : _children) {
			if (!child->_pendingDestroy && child->name == childName) {
				return Handle<SceneObject>(child);
			}
		}
		return Handle<SceneObject>();
	}

	void SceneObject::SetLocalPosition(const Vec3& position) {
		_localPosition = position;
		MarkWorldDirtyRecursive();
	}

	void SceneObject::SetLocalRotation(const Quat& rotation) {
		_localRotation = rotation;
		MarkWorldDirtyRecursive();
	}

	void SceneObject::SetLocalScale(const Vec3& scale) {
		_localScale = scale;
		DeriveScaleKind();
		MarkWorldDirtyRecursive();
	}

	void SceneObject::NotifyLocalTransformWritten() {
		DeriveScaleKind();
		MarkWorldDirtyRecursive();
	}

	const Mat4& SceneObject::GetWorldMatrix() const {
		WorldState state = _worldState.load(std::memory_order_acquire);
		while (state != WorldState::Clean) {
			if (state == WorldState::Dirty && _worldState.compare_exchange_weak(state, WorldState::Computing, std::memory_order_acquire)) {
				Mat4 local = Mat4::TRS(_localPosition, _localRotation, _localScale);
				_worldMatrix = _parent ? _parent->GetWorldMatrix() * local : local;
				_worldState.store(WorldState::Clean, std::memory_order_release);
				break;
			}
			if (state == WorldState::Computing) {
				std::this_thread::yield();
				state = _worldState.load(std::memory_order_acquire);
			}
		}
		return _worldMatrix;
	}

	Vec3 SceneObject::GetWorldPosition() const {
		const Mat4& world = GetWorldMatrix();
		return Vec3(world(0, 3), world(1, 3), world(2, 3));
	}

	void SceneObject::SetWorldPosition(const Vec3& position) {
		SetLocalPosition(_parent ? _parent->InverseTransformPoint(position) : position);
	}

	Quat SceneObject::GetWorldRotation() const {
		Quat rotation = _localRotation;
		for (const SceneObject* ancestor = _parent; ancestor; ancestor = ancestor->_parent) {
			rotation = ancestor->_localRotation * rotation;
		}
		return rotation;
	}

	void SceneObject::SetWorldRotation(const Quat& rotation) {
		SetLocalRotation(_parent ? _parent->GetWorldRotation().Inverse() * rotation : rotation);
	}

	Vec3 SceneObject::GetLossyScale() const {
		Vec3 scale = _localScale;
		for (const SceneObject* ancestor = _parent; ancestor; ancestor = ancestor->_parent) {
			scale *= ancestor->_localScale;
		}
		return scale;
	}

	Vec3 SceneObject::TransformPoint(const Vec3& point) const {
		return GetWorldMatrix().TransformPoint(point);
	}

	Vec3 SceneObject::TransformDirection(const Vec3& direction) const {
		return GetWorldRotation() * direction;
	}

	Vec3 SceneObject::InverseTransformPoint(const Vec3& point) const {
		return GetWorldMatrix().Inverse().TransformPoint(point);
	}

	void SceneObject::MarkWorldDirtyRecursive() const {
		if (_worldState.load(std::memory_order_relaxed) == WorldState::Dirty) {
			return;
		}
		_worldState.store(WorldState::Dirty, std::memory_order_relaxed);
		for (const SceneObject* child : _children) {
			child->MarkWorldDirtyRecursive();
		}
	}

	void SceneObject::DeriveScaleKind() {
		if (_localScale.x < 0.0f || _localScale.y < 0.0f || _localScale.z < 0.0f) {
			_scaleKind = ScaleKind::Negative;
		} else if (Math::Approximately(_localScale.x, 1.0f) && Math::Approximately(_localScale.y, 1.0f) && Math::Approximately(_localScale.z, 1.0f)) {
			_scaleKind = ScaleKind::Identity;
		} else if (Math::Approximately(_localScale.x, _localScale.y) && Math::Approximately(_localScale.y, _localScale.z)) {
			_scaleKind = ScaleKind::Uniform;
		} else {
			_scaleKind = ScaleKind::NonUniform;
		}
	}

	Handle<Component> SceneObject::AddComponent(const TypeInfo* type) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SLT_CORE_ASSERT_MSG(IsStructuralMutationLegal(), "AddComponent is illegal inside lifecycle hooks");
		SLT_CORE_ASSERT_MSG(!_pendingDestroy, "AddComponent on a doomed object");
		if (!IsStructuralMutationLegal() || _pendingDestroy) {
			return Handle<Component>();
		}

		if (type) {
			if (const TypeInfo* disallowed = FindDisallowMultipleOwner(type)) {
				for (const ComponentSlot& slot : _components) {
					if (!slot.ptr->IsPendingDestroy() && TypeRegistry::IsDerivedFrom(slot.type, disallowed)) {
						SLT_LOG_WARN("Scene", "AddComponent: '{}' disallows multiple instances on '{}'",
									 type->displayName ? type->displayName : "component", name);
						return Handle<Component>();
					}
				}
			}

			RequireScopeGuard requireGuard(type);
			const StringHash requireKey("RequireComponent");
			for (const TypeInfo* cur = type; cur != nullptr; cur = cur->parent) {
				if (!cur->attributes || cur->attributeCount == 0) {
					continue;
				}
				for (u32 i = 0; i < cur->attributeCount; ++i) {
					const AttributeInfo& attr = cur->attributes[i];
					if (attr.key != requireKey || !attr.value.Is<str>()) {
						continue;
					}
					const str& reqName = attr.value.As<str>();
					if (reqName.empty()) {
						continue;
					}
					const TypeInfo* reqType = TypeRegistry::FindType(StringHash(reqName));
					if (!reqType || !(reqType->flags & TypeFlagComponent) || (reqType->flags & TypeFlagAbstract) ||
						!reqType->createFn || _requireStack.Contains(reqType)) {
						continue;
					}
					bool alreadyPresent = false;
					for (const ComponentSlot& slot : _components) {
						if (!slot.ptr->IsPendingDestroy() && TypeRegistry::IsDerivedFrom(slot.type, reqType)) {
							alreadyPresent = true;
							break;
						}
					}
					if (!alreadyPresent) {
						AddComponent(reqType);
					}
				}
			}
		}

		Component* component = CreateComponentInstance(type);
		if (!component) {
			return Handle<Component>();
		}
		AttachComponentRaw(*this, type, *component);

		{
			HookGuard guard;
			SceneInternalAccess::OnAttach(*component);
		}
		if (component->IsEffectivelyEnabled()) {
			if (Behavior* behavior = AsBehavior(type, component)) {
				RegisterTicks(*behavior);
			}
			HookGuard guard;
			SceneInternalAccess::OnEnable(*component);
		}
		return Handle<Component>(component);
	}

	Handle<Component> SceneObject::GetComponentAt(u32 index) const {
		if (index >= _components.size()) {
			return Handle<Component>();
		}
		return Handle<Component>(_components[index].ptr);
	}

	void SceneObject::SetComponentIndex(Handle<Component> component, u32 index) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SLT_CORE_ASSERT_MSG(IsStructuralMutationLegal(), "SetComponentIndex is illegal inside lifecycle hooks");
		SLT_CORE_ASSERT_MSG(!_pendingDestroy, "SetComponentIndex on a doomed object");
		Component* moved = component.Get();
		if (!IsStructuralMutationLegal() || _pendingDestroy || !moved || moved->IsPendingDestroy()) {
			return;
		}
		auto it = std::find_if(_components.begin(), _components.end(),
							   [moved](const ComponentSlot& slot) { return slot.ptr == moved; });
		if (it == _components.end()) {
			return;
		}
		ComponentSlot slot = *it;
		_components.erase(it);
		index = std::min(index, static_cast<u32>(_components.size()));
		_components.insert(_components.begin() + index, slot);
	}

	namespace SceneInternal {

		Component* CreateComponentInstance(const TypeInfo* type) {
			if (!type) {
				SLT_LOG_WARN("Scene", "AddComponent: null type");
				return nullptr;
			}
			if (!(type->flags & TypeFlagComponent) || (type->flags & TypeFlagAbstract) || !type->createFn) {
				SLT_LOG_WARN("Scene", "AddComponent: type hash {:x} isn't a concrete creatable component", type->name.GetValue());
				return nullptr;
			}
			if (!TypeRegistry::IsDerivedFrom(type, Component::GetReflectType())) {
				SLT_LOG_WARN("Scene", "AddComponent: type hash {:x} doesn't derive from Component", type->name.GetValue());
				return nullptr;
			}
			Object* instance = TypeRegistry::CreateObjectInstance(type, MemTag::Scene);
			return static_cast<Component*>(instance);
		}

		void AttachComponentRaw(SceneObject& owner, const TypeInfo* type, Component& component) {
			SceneInternalAccess::OwnerOf(component) = &owner;
			SceneInternalAccess::ComponentsOf(owner).push_back({type, &component});
			if (Scene* scene = SceneInternalAccess::SceneOf(owner)) {
				GetState().ownerById[component.GetInstanceId()] = scene;
			}
		}

		void AttachComponentRawAt(SceneObject& owner, const TypeInfo* type, Component& component, u32 slotIndex) {
			SceneInternalAccess::OwnerOf(component) = &owner;
			Vector<SceneInternalAccess::Slot>& slots = SceneInternalAccess::ComponentsOf(owner);
			size_t at = std::min<size_t>(slotIndex, slots.size());
			slots.insert(slots.begin() + at, {type, &component});
			if (Scene* scene = SceneInternalAccess::SceneOf(owner)) {
				GetState().ownerById[component.GetInstanceId()] = scene;
			}
		}

		void LinkSubtreeToScene(SceneObject& root, Scene& scene, SceneObject* parent, i32 insertIndex) {
			SceneInternalAccess::ParentOf(root) = parent;
			Vector<SceneObject*>& siblings = parent ? SceneInternalAccess::ChildrenOf(*parent)
													: SceneInternalAccess::RootsOf(scene);
			if (insertIndex < 0 || static_cast<size_t>(insertIndex) >= siblings.size()) {
				siblings.push_back(&root);
			} else {
				siblings.insert(siblings.begin() + insertIndex, &root);
			}

			struct Registrar {
				Scene* scene;
				void Visit(SceneObject& node) {
					SceneInternalAccess::SceneOf(node) = scene;
					SceneInternalAccess::ObjectsOf(*scene).push_back(&node);
					GetState().ownerById[node.GetInstanceId()] = scene;
					for (const auto& slot : SceneInternalAccess::ComponentsOf(node)) {
						GetState().ownerById[slot.ptr->GetInstanceId()] = scene;
					}
					for (SceneObject* child : SceneInternalAccess::ChildrenOf(node)) {
						Visit(*child);
					}
				}
			};
			Registrar registrar{&scene};
			registrar.Visit(root);
		}

		void DeriveActiveSubtree(SceneObject& root) {
			bool parentActive = SceneInternalAccess::ParentOf(root)
									? SceneInternalAccess::ParentOf(root)->IsActiveInHierarchy()
									: true;
			struct Deriver {
				static void Visit(SceneObject& node, bool parentActive) {
					SceneInternalAccess::ActiveInHierarchyOf(node) = parentActive && node.IsActiveSelf();
					for (SceneObject* child : SceneInternalAccess::ChildrenOf(node)) {
						Visit(*child, node.IsActiveInHierarchy());
					}
				}
			};
			Deriver::Visit(root, parentActive);
		}

		void RefreshActiveSubtree(SceneObject& root) {
			struct Refresher {
				static void Visit(SceneObject& node, bool parentActive) {
					bool wasActive = node.IsActiveInHierarchy();
					bool nowActive = parentActive && node.IsActiveSelf();
					SceneInternalAccess::ActiveInHierarchyOf(node) = nowActive;

					if (wasActive != nowActive && !node.IsPendingDestroy()) {
						for (const auto& slot : SceneInternalAccess::ComponentsOf(node)) {
							Component& component = *slot.ptr;
							if (component.IsPendingDestroy() || !component.IsEnabledSelf()) {
								continue;
							}
							if (TypeRegistry::IsDerivedFrom(slot.type, Behavior::GetReflectType())) {
								Behavior& behavior = *static_cast<Behavior*>(slot.ptr);
								if (nowActive) {
									RegisterTicks(behavior);
								} else {
									UnregisterTicks(behavior);
								}
							}
							HookGuard guard;
							if (nowActive) {
								SceneInternalAccess::OnEnable(component);
							} else {
								SceneInternalAccess::OnDisable(component);
							}
						}
					}
					for (SceneObject* child : SceneInternalAccess::ChildrenOf(node)) {
						Visit(*child, nowActive);
					}
				}
			};
			bool parentActive = SceneInternalAccess::ParentOf(root)
									? SceneInternalAccess::ParentOf(root)->IsActiveInHierarchy()
									: true;
			Refresher::Visit(root, parentActive);
		}

		void DispatchIslandAttach(SceneObject& root) {
			struct Attacher {
				static void Visit(SceneObject& node) {
					for (const auto& slot : SceneInternalAccess::ComponentsOf(node)) {
						HookGuard guard;
						SceneInternalAccess::OnAttach(*slot.ptr);
					}
					for (SceneObject* child : SceneInternalAccess::ChildrenOf(node)) {
						Visit(*child);
					}
				}
			};
			Attacher::Visit(root);

			struct Enabler {
				static void Visit(SceneObject& node) {
					if (node.IsActiveInHierarchy()) {
						for (const auto& slot : SceneInternalAccess::ComponentsOf(node)) {
							Component& component = *slot.ptr;
							if (!component.IsEnabledSelf() || component.IsPendingDestroy()) {
								continue;
							}
							if (TypeRegistry::IsDerivedFrom(slot.type, Behavior::GetReflectType())) {
								RegisterTicks(*static_cast<Behavior*>(slot.ptr));
							}
							HookGuard guard;
							SceneInternalAccess::OnEnable(component);
						}
					}
					for (SceneObject* child : SceneInternalAccess::ChildrenOf(node)) {
						Visit(*child);
					}
				}
			};
			Enabler::Visit(root);
		}

	} // namespace SceneInternal

} // namespace Shalltear

SLT_REGISTER_TYPE(Shalltear::SceneObject)
