#pragma once

#include "Asset/AssetBinder.h"
#include "FrameEndQueue.h"
#include "Shalltear/Core/Assert.h"
#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/Scene/Behavior.h"
#include "Shalltear/Core/Scene/Component.h"
#include "Shalltear/Core/Scene/Prefab.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneAsset.h"
#include "Shalltear/Core/Scene/SceneLoadOperation.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "Shalltear/Core/Scene/UnresolvedComponent.h"
#include "Shalltear/Core/Types.h"

#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Shalltear {

	struct SceneInternalAccess {
		using Slot = SceneObject::ComponentSlot;

		static SceneObject* NewSceneObject() {
			void* memory = Allocate(sizeof(SceneObject), MemTag::Scene, alignof(SceneObject));
			return new (memory) SceneObject();
		}
		static Scene* NewScene() {
			void* memory = Allocate(sizeof(Scene), MemTag::Scene, alignof(Scene));
			return new (memory) Scene();
		}

		static Scene*& SceneOf(SceneObject& o) { return o._scene; }
		static SceneObject*& ParentOf(SceneObject& o) { return o._parent; }
		static Vector<SceneObject*>& ChildrenOf(SceneObject& o) { return o._children; }
		static Vector<Slot>& ComponentsOf(SceneObject& o) { return o._components; }
		static u64& LocalIdOf(SceneObject& o) { return o._localId; }
		static u64& PrefabKeyOf(SceneObject& o) { return o._prefabKey; }
		static u32& PrefabRecordOf(SceneObject& o) { return o._prefabRecord; }
		static bool& PendingDestroyOf(SceneObject& o) { return o._pendingDestroy; }
		static bool& ActiveSelfOf(SceneObject& o) { return o._activeSelf; }
		static bool& ActiveInHierarchyOf(SceneObject& o) { return o._activeInHierarchy; }
		static void MarkWorldDirty(SceneObject& o) { o.MarkWorldDirtyRecursive(); }
		static void DeriveScaleKind(SceneObject& o) { o.DeriveScaleKind(); }

		static SceneObject*& OwnerOf(Component& c) { return c._owner; }
		static u64& LocalIdOf(Component& c) { return c._localId; }
		static u64& PrefabKeyOf(Component& c) { return c._prefabKey; }
		static bool& PendingDestroyOf(Component& c) { return c._pendingDestroy; }
		static bool& EnabledSelfOf(Component& c) { return c._enabledSelf; }

		static u32 (&TickSlotsOf(Behavior& b))[3] { return b._tickSlot; }

		static StringHash& UnresolvedTypeOf(UnresolvedComponent& c) { return c._unresolvedType; }
		static bool& BlobIsBinaryOf(UnresolvedComponent& c) { return c._blobIsBinary; }
		static str& BlobTextOf(UnresolvedComponent& c) { return c._text; }
		static Vector<u8>& BlobBytesOf(UnresolvedComponent& c) { return c._bytes; }

		static SceneLoadOperation::Impl*& ImplOf(SceneLoadOperation& op) { return op._impl; }

		static Vector<SceneObject*>& RootsOf(Scene& s) { return s._roots; }
		static Vector<SceneObject*>& ObjectsOf(Scene& s) { return s._objects; }
		static Vector<Behavior*>& TickListOf(Scene& s, u32 index) { return s._tickLists[index]; }
		static Vector<PrefabInstanceRecord*>& InstancesOf(Scene& s) { return s._instances; }
		static Vector<Handle<Asset>>& RetainedAssetsOf(Scene& s) { return s._retainedAssets; }
		static u64& NextLocalIdOf(Scene& s) { return s._nextLocalId; }
		static Guid& SourceGuidOf(Scene& s) { return s._sourceGuid; }
		static Handle<SceneAsset>& SourceAssetOf(Scene& s) { return s._sourceAsset; }

		static SceneDocument*& DocumentOf(SceneAsset& a) { return a._document; }

		static SceneDocument*& FragmentOf(PrefabAsset& a) { return a._fragment; }
		static PrefabKind& KindOf(PrefabAsset& a) { return a._kind; }

		static SceneState& StateOf(Scene& s) { return s._state; }
		static SceneKind& KindOf(Scene& s) { return s._kind; }
		static bool& ActivationAllowedOf(Scene& s) { return s._activationAllowed; }

		static void OnAttach(Component& c) {
			if (RunsLifecycleHooks(c)) {
				c.OnAttach();
			}
		}
		static void OnDetach(Component& c) {
			if (RunsLifecycleHooks(c)) {
				c.OnDetach();
			}
		}
		static void OnEnable(Component& c) {
			if (RunsLifecycleHooks(c)) {
				c.OnEnable();
			}
		}
		static void OnDisable(Component& c) {
			if (RunsLifecycleHooks(c)) {
				c.OnDisable();
			}
		}
		static void OnDestroy(Component& c) {
			if (RunsLifecycleHooks(c)) {
				c.OnDestroy();
			}
		}
		static bool RunsLifecycleHooks(const Component& c);
		static void OnUpdate(Behavior& b) { b.OnUpdate(); }
		static void OnFixedUpdate(Behavior& b) { b.OnFixedUpdate(); }
		static void OnLateUpdate(Behavior& b) { b.OnLateUpdate(); }
	};

	namespace SceneInternal {

		struct SceneAssetBinder {
			AssetInternal::AssetBinder binder;

			bool Resolve(Scene* owner, const Guid& guid, u64* outId);
		};

		struct SystemState {
			bool initialized = false;
			std::thread::id mainThread;

			u32 hookDepth = 0;
			bool inEndFrame = false;
			bool playing = true;

			Vector<Handle<Scene>> allScenes{GetMemoryResource(MemTag::Scene)};
			Vector<Handle<Scene>> worldScenes{GetMemoryResource(MemTag::Scene)};
			Handle<Scene> primary;
			FrameEndQueue frameEnd;

			HashMap<u64, Scene*> ownerById{GetMemoryResource(MemTag::Scene)};

			Vector<Handle<SceneLoadOperation>> activeLoads{GetMemoryResource(MemTag::Scene)};
			Vector<Handle<SceneLoadOperation>> completedLoads{GetMemoryResource(MemTag::Scene)};

			f64 asyncLoadBudget = 0.002;

			bool replaceRequested = false;
			Guid replaceIncoming;
			Handle<Scene> replaceKeep;

			u32 prefabEpoch = 0;
		};

		SystemState& GetState();
		bool IsMainThread();

	} // namespace SceneInternal

	inline bool SceneInternalAccess::RunsLifecycleHooks(const Component& c) {
		if (!c._isScript) {
			return true;
		}
		const Scene* scene = c._owner ? c._owner->_scene : nullptr;
		return scene && scene->_kind == SceneKind::World && SceneInternal::GetState().playing;
	}

	namespace SceneInternal {

		struct HookGuard {
			HookGuard() { ++GetState().hookDepth; }
			~HookGuard() { --GetState().hookDepth; }
		};

		inline bool IsStructuralMutationLegal() { return GetState().hookDepth == 0; }

		Component* CreateComponentInstance(const TypeInfo* type);

		void AttachComponentRaw(SceneObject& owner, const TypeInfo* type, Component& component);

		void AttachComponentRawAt(SceneObject& owner, const TypeInfo* type, Component& component, u32 slotIndex);

		void LinkSubtreeToScene(SceneObject& root, Scene& scene, SceneObject* parent, i32 insertIndex);

		void DeriveActiveSubtree(SceneObject& root);

		void RefreshActiveSubtree(SceneObject& root);

		void DispatchIslandAttach(SceneObject& root);

		void RegisterTicks(Behavior& behavior);
		void UnregisterTicks(Behavior& behavior);
		void CompactTickLists(Scene& scene);

		void DoomSubtree(SceneObject& root);
		void DoomComponentOnly(Component& component);

		void FinalizeSubtree(SceneObject& root, bool dispatchHooks);

		Scene* CreateSceneInternal(std::string_view name, bool visible, SceneKind kind = SceneKind::World);
		void MakeSceneVisible(Scene& scene);
		void DestroySceneNow(Scene& scene); // roots must already be finalized

		void ReleaseInstanceRecord(Scene& scene, u32 recordIndex1Based);

		void PurgeFinalizedFromRecord(SceneObject& node);

		void RegisterSceneImporters();
		void UnregisterSceneImporters();

		// caller releases 1 load reference
		SceneAsset* AcquireSceneAsset(const Guid& assetId, const char* caller);

		void ApplyRequestedReplace();

		void PumpAsyncLoads(f64 budgetSeconds);

		void CancelAllLoads();

		struct UnresolvedTypeMention {
			StringHash type;
			str spelling;
			u32 rows = 0;
		};

		class UnresolvedReportHold {
		public:
			virtual ~UnresolvedReportHold() = default;
			virtual void HoldRows(std::string_view what, const std::vector<UnresolvedTypeMention>& types) = 0;
			virtual void HoldUnconverted(StringHash rowType) = 0;
			// logs types still unregistered, then deletes itself
			virtual void Release() = 0;
		};

		bool InstallUnresolvedReportHold(UnresolvedReportHold& hold);
		void ReleaseUnresolvedReportHold(bool everyHolder);

		void LogUnconvertedRow(StringHash rowType);
		void LogUnresolvedRows(u32 rows, const std::vector<UnresolvedTypeMention>& types, std::string_view what);
	} // namespace SceneInternal

} // namespace Shalltear

#define SLT_SCENE_ASSERT_MAIN_THREAD() \
	SLT_CORE_ASSERT_MSG(::Shalltear::SceneInternal::IsMainThread(), "SceneSystem operations are main-thread-only")
