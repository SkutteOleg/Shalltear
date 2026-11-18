#include "Shalltear/Core/Scene/SceneSystem.h"

#include "Input/InputInternal.h"
#include "Module/EngineStateTable.h"
#include "SceneInternal.h"
#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Frame/FrameStages.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Profiler.h"
#include "Shalltear/Core/Scene/Behavior.h"
#include "Shalltear/Core/Scene/Prefab.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneAsset.h"
#include "Shalltear/Core/Scene/SceneEvents.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "StaticLinkAnchors.h"

#include <algorithm>

namespace Shalltear {

	using namespace SceneInternal;

	namespace SceneInternal {

		namespace {
			SystemState* _boundState = nullptr;
		} // anonymous namespace

		SystemState& GetState() {
			if (_boundState)
				return *_boundState;
			static SystemState state;
			return state;
		}

		bool IsMainThread() {
			SystemState& state = GetState();
			return !state.initialized || std::this_thread::get_id() == state.mainThread;
		}

	} // namespace SceneInternal

	namespace {

		const u8 _FRAME_STAGE_OWNER = 0;

		void RegisterSceneFrameStages() {
			FrameStages::Register(
				FrameStage::FixedUpdate, FrameStagePosition::Normal,
				[](const FrameStageArgs&, void*) { SceneSystem::FixedUpdateAll(); },
				nullptr, &_FRAME_STAGE_OWNER);
			FrameStages::Register(
				FrameStage::Update, FrameStagePosition::Normal,
				[](const FrameStageArgs&, void*) { SceneSystem::UpdateAll(); },
				nullptr, &_FRAME_STAGE_OWNER);
			FrameStages::Register(
				FrameStage::LateUpdate, FrameStagePosition::Normal,
				[](const FrameStageArgs&, void*) { SceneSystem::LateUpdateAll(); },
				nullptr, &_FRAME_STAGE_OWNER);
			FrameStages::Register(
				FrameStage::EndFrame, FrameStagePosition::Normal,
				[](const FrameStageArgs&, void*) { SceneSystem::EndFrame(); },
				nullptr, &_FRAME_STAGE_OWNER);
		}

	} // anonymous namespace

	void SceneSystem::Initialize() {
		SystemState& state = GetState();
		if (state.initialized) {
			return;
		}
		state.mainThread = std::this_thread::get_id();

		TypeRegistry::InitializePrimitives();
		Object::GetReflectType();
		Component::GetReflectType();
		Behavior::GetReflectType();
		UnresolvedComponent::GetReflectType();
		Scene::GetReflectType();
		SLT_SCENE_SYSTEM_ANCHORS(SLT_ANCHOR_TYPE)
		TypeRegistry::RegisterPendingTypes();

		RegisterSceneImporters();
		RegisterSceneFrameStages();

		state.initialized = true;
		SLT_LOG_INFO("Scene", "SceneSystem initialized");
	}

	void SceneSystem::Terminate() {
		SystemState& state = GetState();
		if (!state.initialized) {
			return;
		}
		SLT_SCENE_ASSERT_MAIN_THREAD();

		CancelAllLoads();
		UnloadAllScenes();
		state.frameEnd.Drain();
		SceneInternal::ReleaseUnresolvedReportHold(true);

		UnregisterSceneImporters();
		FrameStages::UnregisterOwnedBy(&_FRAME_STAGE_OWNER);

		SLT_CORE_ASSERT_MSG(state.ownerById.empty(), "SceneSystem::Terminate leaked scene-owned objects");
		state.ownerById = HashMap<u64, Scene*>{GetMemoryResource(MemTag::Scene)};
		state.allScenes = Vector<Handle<Scene>>{GetMemoryResource(MemTag::Scene)};
		state.worldScenes = Vector<Handle<Scene>>{GetMemoryResource(MemTag::Scene)};
		state.activeLoads = Vector<Handle<SceneLoadOperation>>{GetMemoryResource(MemTag::Scene)};
		state.completedLoads = Vector<Handle<SceneLoadOperation>>{GetMemoryResource(MemTag::Scene)};
		state.asyncLoadBudget = 0.002;
		state.frameEnd.Reset();
		state.primary = Handle<Scene>();
		state.playing = true;
		state.initialized = false;
		SLT_LOG_INFO("Scene", "SceneSystem terminated");
	}

	bool SceneSystem::IsInitialized() {
		return GetState().initialized;
	}

	namespace SceneInternal {

		Scene* CreateSceneInternal(std::string_view name, bool visible, SceneKind kind) {
			Scene* scene = SceneInternalAccess::NewScene();
			scene->name = str(name);
			SceneInternalAccess::KindOf(*scene) = kind;
			if (visible) {
				MakeSceneVisible(*scene);
			} else {
				SceneInternalAccess::StateOf(*scene) = SceneState::Loading;
				SceneInternalAccess::ActivationAllowedOf(*scene) = false;
			}
			return scene;
		}

		void MakeSceneVisible(Scene& scene) {
			SystemState& state = GetState();
			SceneInternalAccess::StateOf(scene) = SceneState::Loaded;
			SceneInternalAccess::ActivationAllowedOf(scene) = true;
			state.allScenes.push_back(Handle<Scene>(&scene));
			if (scene.GetKind() != SceneKind::World) {
				return;
			}
			state.worldScenes.push_back(Handle<Scene>(&scene));
			if (!state.primary.Get()) {
				state.primary = Handle<Scene>(&scene);
			}
		}

		void DestroySceneNow(Scene& scene) {
			SystemState& state = GetState();
			SLT_CORE_ASSERT_MSG(SceneInternalAccess::RootsOf(scene).empty(),
								"DestroySceneNow with live roots. Doom them");

			u64 sceneId = scene.GetInstanceId();
			auto dropFrom = [sceneId](Vector<Handle<Scene>>& list) {
				auto it = std::find_if(list.begin(), list.end(),
									   [&](const Handle<Scene>& h) { return h.GetInstanceId() == sceneId; });
				if (it != list.end()) {
					list.erase(it);
				}
			};
			dropFrom(state.allScenes);
			dropFrom(state.worldScenes);
			if (state.primary.GetInstanceId() == sceneId) {
				state.primary = state.worldScenes.empty() ? Handle<Scene>() : state.worldScenes.front();
			}

			if (SceneAsset* source = SceneInternalAccess::SourceAssetOf(scene).Get()) {
				AssetDatabase::UnloadAsset(source);
			}

			Vector<Handle<Asset>>& retained = SceneInternalAccess::RetainedAssetsOf(scene);
			for (const Handle<Asset>& reference : retained) {
				Asset* asset = reference.Get();
				SLT_CORE_ASSERT_MSG(asset != nullptr, "a scene's retained asset reference outlived its asset");
				if (asset) {
					AssetDatabase::UnloadAsset(asset);
				}
			}
			retained.clear();

			bool announce = scene.GetKind() == SceneKind::World;

			scene.~Scene();
			Free(&scene);

			if (announce) {
				SceneEvents::Unloaded.Publish(EventBus::GetGlobal(), SceneUnloadedEvent{sceneId});
			}
		}

	} // namespace SceneInternal

	Handle<Scene> SceneSystem::CreateScene(std::string_view name) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		if (!IsInitialized()) {
			SLT_LOG_WARN("Scene", "CreateScene before SceneSystem::Initialize");
			return Handle<Scene>();
		}
		Scene* scene = CreateSceneInternal(name, true);
		Handle<Scene> handle(scene);
		SceneEvents::Loaded.Publish(EventBus::GetGlobal(), SceneLoadedEvent{handle});
		return handle;
	}

	Handle<Scene> SceneSystem::CreateIsolatedScene(std::string_view name) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		if (!IsInitialized()) {
			SLT_LOG_WARN("Scene", "CreateIsolatedScene before SceneSystem::Initialize");
			return Handle<Scene>();
		}
		return Handle<Scene>(CreateSceneInternal(name, true, SceneKind::Isolated));
	}

	u32 SceneSystem::GetIsolatedSceneCount() {
		SystemState& state = GetState();
		return static_cast<u32>(state.allScenes.size() - state.worldScenes.size());
	}

	void SceneSystem::UnloadScene(Handle<Scene> sceneHandle) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SystemState& state = GetState();
		Scene* scene = sceneHandle.Get();
		if (!state.initialized || !scene || scene->GetState() == SceneState::Unloading) {
			return;
		}

		SceneInternalAccess::StateOf(*scene) = SceneState::Unloading;
		if (scene->GetKind() == SceneKind::World) {
			SceneEvents::Unloading.Publish(EventBus::GetGlobal(), SceneUnloadingEvent{sceneHandle});
		}

		Vector<SceneObject*> roots{SceneInternalAccess::RootsOf(*scene),
								   GetMemoryResource(MemTag::Scene)};
		for (SceneObject* root : roots) {
			DoomSubtree(*root);
		}
		state.frameEnd.Push(FrameEndQueue::Kind::Scene, scene->GetInstanceId());
	}

	void SceneSystem::UnloadAllScenes() {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SystemState& state = GetState();
		Vector<Handle<Scene>> snapshot{state.allScenes, GetMemoryResource(MemTag::Scene)};
		for (const Handle<Scene>& handle : snapshot) {
			UnloadScene(handle);
		}
	}

	void SceneSystem::SetPlaying(bool playing) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SystemState& state = GetState();
		if (state.playing == playing) {
			return;
		}
		SLT_CORE_ASSERT_MSG(state.worldScenes.empty(),
							"SceneSystem::SetPlaying with a world scene loaded (its scripts' hooks would go unpaired)");
		state.playing = playing;
	}

	std::span<const Handle<Scene>> SceneSystem::GetLoadedScenes() {
		SystemState& state = GetState();
		return std::span<const Handle<Scene>>(state.worldScenes.data(), state.worldScenes.size());
	}

	Handle<Scene> SceneSystem::GetPrimaryScene() {
		return GetState().primary;
	}

	void SceneSystem::SetPrimaryScene(Handle<Scene> scene) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		Scene* target = scene.Get();
		if (!target) {
			return;
		}
		if (target->GetKind() != SceneKind::World) {
			SLT_LOG_WARN("Scene", "SetPrimaryScene ignored '{}' (an isolated scene isn't part of the world)", target->name);
			return;
		}
		GetState().primary = scene;
	}

	Handle<SceneObject> SceneSystem::CreateObject(std::string_view name,
												  Handle<Scene> sceneHandle, Handle<SceneObject> parentHandle) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SLT_CORE_ASSERT_MSG(IsStructuralMutationLegal(), "CreateObject is illegal inside lifecycle hooks");
		SystemState& state = GetState();
		if (!state.initialized || !IsStructuralMutationLegal()) {
			return Handle<SceneObject>();
		}

		SceneObject* parent = parentHandle.Get();
		if (parent && parent->IsPendingDestroy()) {
			SLT_CORE_ASSERT_MSG(false, "CreateObject under a doomed parent");
			return Handle<SceneObject>();
		}

		Scene* scene = parent ? SceneInternalAccess::SceneOf(*parent) : sceneHandle.Get();
		if (!scene) {
			scene = state.primary.Get();
		}
		if (!scene || scene->GetState() != SceneState::Loaded) {
			SLT_LOG_WARN("Scene", "CreateObject: no loaded target scene");
			return Handle<SceneObject>();
		}

		SceneObject* object = SceneInternalAccess::NewSceneObject();
		object->name = str(name);
		LinkSubtreeToScene(*object, *scene, parent, -1);
		DeriveActiveSubtree(*object);
		return Handle<SceneObject>(object);
	}

	namespace SceneInternal {

		namespace {

			void DoomComponentInPlace(const TypeInfo* slotType, Component& component) {
				if (component.IsPendingDestroy()) {
					return;
				}
				bool wasEffective = component.IsEffectivelyEnabled();
				SceneInternalAccess::PendingDestroyOf(component) = true;
				if (wasEffective) {
					if (TypeRegistry::IsDerivedFrom(slotType, Behavior::GetReflectType())) {
						UnregisterTicks(*static_cast<Behavior*>(&component));
					}
					HookGuard guard;
					SceneInternalAccess::OnDisable(component);
				}
			}

			void DoomSubtreeWalk(SceneObject& node) {
				if (node.IsPendingDestroy()) {
					return;
				}
				SceneInternalAccess::PendingDestroyOf(node) = true;
				for (const auto& slot : SceneInternalAccess::ComponentsOf(node)) {
					DoomComponentInPlace(slot.type, *slot.ptr);
				}
				for (SceneObject* child : SceneInternalAccess::ChildrenOf(node)) {
					DoomSubtreeWalk(*child);
				}
			}

		} // anonymous namespace

		void DoomSubtree(SceneObject& root) {
			if (root.IsPendingDestroy()) {
				return;
			}
			GetState().frameEnd.Push(FrameEndQueue::Kind::Object, root.GetInstanceId());
			DoomSubtreeWalk(root);
		}

		void DoomComponentOnly(Component& component) {
			if (component.IsPendingDestroy()) {
				return;
			}
			GetState().frameEnd.Push(FrameEndQueue::Kind::Component, component.GetInstanceId());

			const TypeInfo* slotType = nullptr;
			if (SceneObject* owner = SceneInternalAccess::OwnerOf(component)) {
				for (const auto& slot : SceneInternalAccess::ComponentsOf(*owner)) {
					if (slot.ptr == &component) {
						slotType = slot.type;
						break;
					}
				}
			}
			DoomComponentInPlace(slotType ? slotType : Component::GetReflectType(), component);
		}

		void PurgeFinalizedFromRecord(SceneObject& node) {
			bool anyKeyed = SceneInternalAccess::PrefabKeyOf(node) != 0;
			for (const auto& slot : SceneInternalAccess::ComponentsOf(node)) {
				anyKeyed = anyKeyed || SceneInternalAccess::PrefabKeyOf(*slot.ptr) != 0;
			}
			if (!anyKeyed) {
				return;
			}
			PrefabInstanceRecord* record = nullptr;
			for (SceneObject* ancestor = SceneInternalAccess::ParentOf(node); ancestor;
				 ancestor = SceneInternalAccess::ParentOf(*ancestor)) {
				u32 index = SceneInternalAccess::PrefabRecordOf(*ancestor);
				Scene* scene = SceneInternalAccess::SceneOf(*ancestor);
				if (index != 0 && scene && index <= SceneInternalAccess::InstancesOf(*scene).size()) {
					record = SceneInternalAccess::InstancesOf(*scene)[index - 1];
					break;
				}
			}
			if (!record) {
				return;
			}
			auto purge = [record](u64 key) {
				if (key == 0) {
					return;
				}
				record->correspondence.erase(key);
				record->keyPaths.erase(key);
				record->keyTypes.erase(key);
			};
			purge(SceneInternalAccess::PrefabKeyOf(node));
			for (const auto& slot : SceneInternalAccess::ComponentsOf(node)) {
				purge(SceneInternalAccess::PrefabKeyOf(*slot.ptr));
			}
		}

		void ReleaseInstanceRecord(Scene& scene, u32 recordIndex1Based) {
			Vector<PrefabInstanceRecord*>& records = SceneInternalAccess::InstancesOf(scene);
			if (recordIndex1Based == 0 || recordIndex1Based > records.size()) {
				return;
			}
			PrefabInstanceRecord*& slot = records[recordIndex1Based - 1];
			if (!slot) {
				return;
			}
			if (slot->asset) {
				AssetDatabase::UnloadAsset(slot->asset);
			}
			Delete(slot);
			slot = nullptr;
		}

	} // namespace SceneInternal

	void SceneSystem::Destroy(Handle<SceneObject> objectHandle) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SceneObject* object = objectHandle.Get();
		if (!GetState().initialized || !object) {
			return;
		}
		DoomSubtree(*object);
	}

	void SceneSystem::Destroy(Handle<Component> componentHandle) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		Component* component = componentHandle.Get();
		if (!GetState().initialized || !component) {
			return;
		}
		DoomComponentOnly(*component);
	}

	namespace {

		enum : u32 {
			TickUpdate = 0,
			TickFixed = 1,
			TickLate = 2,
		};

		template <typename Dispatch>
		void DispatchTickList(u32 listIndex, Dispatch&& dispatch) {
			SystemState& state = GetState();
			size_t sceneCount = state.worldScenes.size();
			for (size_t s = 0; s < sceneCount; ++s) {
				Scene* scene = state.worldScenes[s].Get();
				if (!scene || scene->GetState() != SceneState::Loaded) {
					continue;
				}
				Vector<Behavior*>& list = SceneInternalAccess::TickListOf(*scene, listIndex);
				size_t count = list.size();
				for (size_t i = 0; i < count; ++i) {
					Behavior* behavior = list[i];
					if (behavior && !behavior->IsPendingDestroy() && behavior->IsEffectivelyEnabled()) {
						dispatch(*behavior);
					}
				}
			}
		}

	} // anonymous namespace

	void SceneSystem::FixedUpdateAll() {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		if (!GetState().initialized) {
			return;
		}
		InputInternal::GameplayTickScope inputRoute;
		DispatchTickList(TickFixed, [](Behavior& b) { SceneInternalAccess::OnFixedUpdate(b); });
	}

	void SceneSystem::BeginFrame() {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		if (!GetState().initialized) {
			return;
		}
		PumpAsyncLoads(GetState().asyncLoadBudget);
	}

	void SceneSystem::UpdateAll() {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		if (!GetState().initialized) {
			return;
		}
		SLT_PROFILE_ZONE("Scene.Update");
		InputInternal::GameplayTickScope inputRoute;
		DispatchTickList(TickUpdate, [](Behavior& b) { SceneInternalAccess::OnUpdate(b); });
	}

	void SceneSystem::SetAsyncLoadBudget(f64 seconds) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		GetState().asyncLoadBudget = seconds;
	}

	f64 SceneSystem::GetAsyncLoadBudget() {
		return GetState().asyncLoadBudget;
	}

	void SceneSystem::CancelAsyncLoads() {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		if (!GetState().initialized) {
			return;
		}
		CancelAllLoads();
	}

	void SceneSystem::ReplaceScenes(const Guid& sceneAssetId, Handle<Scene> keep) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SystemState& state = GetState();
		if (!state.initialized) {
			return;
		}
		state.replaceRequested = true;
		state.replaceIncoming = sceneAssetId;
		state.replaceKeep = keep;
	}

	void SceneSystem::LateUpdateAll() {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		if (!GetState().initialized) {
			return;
		}
		InputInternal::GameplayTickScope inputRoute;
		DispatchTickList(TickLate, [](Behavior& b) { SceneInternalAccess::OnLateUpdate(b); });
	}

	void SceneSystem::EndFrame() {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SystemState& state = GetState();
		if (!state.initialized) {
			return;
		}
		SLT_CORE_ASSERT_MSG(!state.inEndFrame, "EndFrame re-entered");
		state.inEndFrame = true;
		SLT_PROFILE_ZONE("Scene.EndFrame");

		state.frameEnd.Drain();

		ApplyRequestedReplace();

		for (const Handle<Scene>& handle : state.allScenes) {
			if (Scene* scene = handle.Get()) {
				CompactTickLists(*scene);
			}
		}
		state.inEndFrame = false;
	}

	namespace ModuleInternal {

		void CaptureSceneSystemEngineState(EngineStateTable& table) {
			table.sceneSystem = &SceneInternal::GetState();
		}

		void BindSceneSystemEngineState(const EngineStateTable& table) {
			SceneInternal::_boundState = static_cast<SceneInternal::SystemState*>(table.sceneSystem);
		}

	} // namespace ModuleInternal

} // namespace Shalltear
