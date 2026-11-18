#include "SceneMigration.h"

#include "SceneDocument.h"
#include "SceneInternal.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Reflection/VariantDispatch.h"
#include "Shalltear/Core/Scene/Prefab.h"
#include "Shalltear/Core/Serialization/HandleResolver.h"

#include <algorithm>

namespace Shalltear::SceneInternal {

	namespace SI = SerializerInternal;

	namespace {

		bool MigrationToGuid(void*, u64 instanceId, Guid* outGuid, StringHash* outPointeeType) {
			SystemState& state = GetState();

			bool sceneOwned = state.ownerById.contains(instanceId);
			if (!sceneOwned) {
				for (const Handle<Scene>& scene : state.allScenes) {
					if (scene.GetInstanceId() == instanceId) {
						sceneOwned = true;
						break;
					}
				}
			}
			if (!sceneOwned) {
				return false;
			}

			if (outGuid) {
				*outGuid = Guid{instanceId, SLT_MIGRATION_SENTINEL};
			}
			if (outPointeeType) {
				*outPointeeType = StringHash();
			}
			return true;
		}

		bool MigrationToHandle(void*, const Guid& guid, StringHash, u64* outId) {
			if (guid.high != SLT_MIGRATION_SENTINEL) {
				return false;
			}
			if (outId) {
				*outId = guid.low;
			}
			return true;
		}

		PrefabInstanceRecord* FindEnclosingRecord(SceneObject& owner) {
			for (SceneObject* ancestor = &owner; ancestor;
				 ancestor = SceneInternalAccess::ParentOf(*ancestor)) {
				u32 index = SceneInternalAccess::PrefabRecordOf(*ancestor);
				Scene* scene = SceneInternalAccess::SceneOf(*ancestor);
				if (index != 0 && scene && index <= SceneInternalAccess::InstancesOf(*scene).size()) {
					return SceneInternalAccess::InstancesOf(*scene)[index - 1];
				}
			}
			return nullptr;
		}

		void ScrubModuleVariantsFromRecords(Scene& scene, const void* moduleImage) {
			for (PrefabInstanceRecord* record : SceneInternalAccess::InstancesOf(scene)) {
				if (!record) {
					continue;
				}
				auto& properties = record->liveOverrides.properties;
				auto stale = std::remove_if(properties.begin(), properties.end(),
											[&](const PropertyOverrideOp& op) {
												return !op.value.isHandle &&
													   VariantDispatch::GetOwner(op.value.direct.GetTypeId()) == moduleImage;
											});
				if (stale != properties.end()) {
					properties.erase(stale, properties.end());
				}
			}
		}

		struct RebuildScopeCtx {
			std::unordered_map<u64, u64> localIds;
			Scene* scene = nullptr;
			SceneAssetBinder assets;
		};

		bool RebuildScopeToHandle(void* user, const Guid& guid, StringHash, u64* outId) {
			auto* ctx = static_cast<RebuildScopeCtx*>(user);
			if (guid.high == SLT_MIGRATION_SENTINEL) {
				*outId = guid.low;
				return true;
			}
			if (guid.high != SLT_LOCAL_SENTINEL) {
				return ctx->assets.Resolve(ctx->scene, guid, outId);
			}
			auto it = ctx->localIds.find(guid.low);
			if (it == ctx->localIds.end()) {
				SLT_LOG_WARN("Module", "preserved blob references localId {} that no live row has. Resolving null", guid.low);
				*outId = 0;
				return true;
			}
			*outId = it->second;
			return true;
		}

		void MapSceneLocalIds(Scene& scene, std::unordered_map<u64, u64>& out) {
			struct Walk {
				std::unordered_map<u64, u64>* map;
			} walk{&out};

			scene.ForEachObject(
				+[](SceneObject& object, void* user) {
					auto* w = static_cast<Walk*>(user);
					if (SceneInternalAccess::PrefabKeyOf(object) == 0 && SceneInternalAccess::PrefabRecordOf(object) == 0) {
						if (u64 localId = SceneInternalAccess::LocalIdOf(object); localId != 0) {
							(*w->map)[localId] = object.GetInstanceId();
						}
					}
					for (const auto& slot : SceneInternalAccess::ComponentsOf(object)) {
						if (SceneInternalAccess::PrefabKeyOf(*slot.ptr) != 0) {
							continue;
						}
						if (u64 localId = SceneInternalAccess::LocalIdOf(*slot.ptr); localId != 0) {
							(*w->map)[localId] = slot.ptr->GetInstanceId();
						}
					}
				},
				&walk);
		}

	} // anonymous namespace

	bool MigrationPreconditionsMet() {
		SystemState& state = GetState();
		if (!state.initialized) {
			return true;
		}
		if (!state.activeLoads.empty()) {
			SLT_LOG_WARN("Module", "reload deferred: {} scene load operation(s) in flight", state.activeLoads.size());
			return false;
		}
		if (!state.frameEnd.IsEmpty()) {
			SLT_LOG_WARN("Module", "reload deferred: frame-end queue not drained (call between frames)");
			return false;
		}
		return true;
	}

	bool IsMigrationLegal() {
		const SystemState& state = GetState();
		return !state.initialized || (state.activeLoads.empty() && state.frameEnd.IsEmpty());
	}

	HandleResolverScope MigrationResolverScope() {
		return HandleResolverScope{&MigrationToGuid, &MigrationToHandle, nullptr};
	}

	u32 RebuildPreservedComponents() {
		SystemState& state = GetState();
		if (!state.initialized) {
			return 0;
		}
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SLT_CORE_ASSERT_MSG(IsStructuralMutationLegal(), "the upgrade sweep is illegal inside lifecycle hooks");
		if (!IsStructuralMutationLegal()) {
			return 0;
		}

		u32 reclaimed = 0;
		for (const Handle<Scene>& sceneHandle : state.allScenes) {
			Scene* scene = sceneHandle.Get();
			if (!scene) {
				continue;
			}

			struct Candidate {
				SceneObject* owner;
				UnresolvedComponent* preserved;
				const TypeInfo* type;
				u32 slotIndex;
			};
			struct Collect {
				std::vector<Candidate>* out;
			};
			std::vector<Candidate> candidates;
			Collect collect{&candidates};

			scene->ForEachObject(
				+[](SceneObject& object, void* user) {
					auto* c = static_cast<Collect*>(user);
					const auto& slots = SceneInternalAccess::ComponentsOf(object);
					for (u32 slotIndex = 0; slotIndex < slots.size(); ++slotIndex) {
						if (!IsPreservedComponentType(slots[slotIndex].type)) {
							continue;
						}
						auto& preserved = static_cast<UnresolvedComponent&>(*slots[slotIndex].ptr);
						if (preserved.IsPendingDestroy()) {
							continue;
						}
						const TypeInfo* type = TypeRegistry::FindType(preserved.GetUnresolvedType());
						if (!type) {
							continue;
						}
						c->out->push_back({&object, &preserved, type, slotIndex});
					}
				},
				&collect);

			if (candidates.empty()) {
				continue;
			}

			std::vector<Component*> rebuilt;
			{
				RebuildScopeCtx ctx;
				ctx.scene = scene;
				MapSceneLocalIds(*scene, ctx.localIds);
				ScopedHandleResolver scope(HandleResolverScope{nullptr, &RebuildScopeToHandle, &ctx});
				std::vector<SI::HandleFixupJob> fixups;

				for (const Candidate& candidate : candidates) {
					Component* component = CreateComponentInstance(candidate.type);
					if (!component) {
						continue;
					}

					const bool blobIsBinary = SceneInternalAccess::BlobIsBinaryOf(*candidate.preserved);
					std::vector<u8> blob;
					if (blobIsBinary) {
						const Vector<u8>& bytes = SceneInternalAccess::BlobBytesOf(*candidate.preserved);
						blob.assign(bytes.begin(), bytes.end());
					} else {
						const str& text = SceneInternalAccess::BlobTextOf(*candidate.preserved);
						blob.assign(text.begin(), text.end());
					}
					const u64 componentId = candidate.preserved->GetInstanceId();
					const u64 localId = SceneInternalAccess::LocalIdOf(*candidate.preserved);
					const u64 prefabKey = SceneInternalAccess::PrefabKeyOf(*candidate.preserved);

					auto& slots = SceneInternalAccess::ComponentsOf(*candidate.owner);
					auto it = std::find_if(slots.begin(), slots.end(),
										   [&](const SceneInternalAccess::Slot& s) { return s.ptr == candidate.preserved; });
					const u32 slotIndex = it != slots.end() ? static_cast<u32>(it - slots.begin()) : candidate.slotIndex;
					if (it != slots.end()) {
						slots.erase(it);
					}
					bool detached = ObjectRegistry::DetachPreservingSlot(*candidate.preserved);
					SLT_CORE_ASSERT_MSG(detached, "upgrade sweep failed to detach a preserved row's registry slot");
					TypeRegistry::DestroyObjectInstance(candidate.preserved);

					if (!ObjectRegistry::RebindPreservedSlot(*component, componentId)) {
						TypeRegistry::DestroyObjectInstance(component);
						SLT_LOG_ERROR("Module", "upgrade sweep lost row {:#018x}: preserved slot couldn't be rebound", componentId);
						continue;
					}

					SceneInternalAccess::LocalIdOf(*component) = localId;
					SceneInternalAccess::PrefabKeyOf(*component) = prefabKey;
					AttachComponentRawAt(*candidate.owner, candidate.type, *component, slotIndex);
					ApplyPreservedBlob(blob, blobIsBinary, component, candidate.type, fixups);

					if (prefabKey != 0) {
						if (PrefabInstanceRecord* record = FindEnclosingRecord(*candidate.owner)) {
							record->correspondence[prefabKey] = component;
						}
					}
					rebuilt.push_back(component);
				}

				SI::RunHandleFixups(fixups);
			}

			for (Component* component : rebuilt) {
				HookGuard guard;
				SceneInternalAccess::OnAttach(*component);
			}
			for (Component* component : rebuilt) {
				if (component->IsPendingDestroy() || !component->IsEffectivelyEnabled()) {
					continue;
				}
				if (TypeRegistry::IsDerivedFrom(component->GetType(), Behavior::GetReflectType())) {
					RegisterTicks(*static_cast<Behavior*>(component));
				}
				HookGuard guard;
				SceneInternalAccess::OnEnable(*component);
			}
			reclaimed += static_cast<u32>(rebuilt.size());
		}

		if (reclaimed > 0) {
			SLT_LOG_INFO("Module", "{} preserved component row(s) rebuilt", reclaimed);
		}
		return reclaimed;
	}

	void CaptureAndDemolishModuleComponents(const void* moduleImage, MigrationSnapshot& out) {
		SystemState& state = GetState();
		if (!state.initialized) {
			return;
		}
		SLT_SCENE_ASSERT_MAIN_THREAD();

		struct CaptureWalk {
			const void* image;
			MigrationSnapshot* snapshot;
		} walk{moduleImage, &out};

		for (const Handle<Scene>& sceneHandle : state.allScenes) {
			Scene* scene = sceneHandle.Get();
			if (!scene) {
				continue;
			}
			size_t before = out.envelopes.size();

			scene->ForEachObject(
				+[](SceneObject& object, void* user) {
					auto* w = static_cast<CaptureWalk*>(user);
					const auto& slots = SceneInternalAccess::ComponentsOf(object);
					for (u32 slotIndex = 0; slotIndex < slots.size(); ++slotIndex) {
						const auto& slot = slots[slotIndex];
						if (!slot.type || slot.type->ownerModule != w->image) {
							continue;
						}
						Component& component = *slot.ptr;

						MigrationEnvelope envelope;
						envelope.ownerId = object.GetInstanceId();
						envelope.componentId = component.GetInstanceId();
						envelope.typeNameHash = slot.type->name.GetValue();
						envelope.localId = SceneInternalAccess::LocalIdOf(component);
						envelope.prefabKey = SceneInternalAccess::PrefabKeyOf(component);
						envelope.slotIndex = slotIndex;
						w->snapshot->envelopes.push_back(std::move(envelope));
					}
				},
				&walk);

			if (out.envelopes.size() != before) {
				out.affectedScenes.push_back(scene);
			}
		}

		if (out.envelopes.empty()) {
			return;
		}

		// disable before capture (OnDisable writes land in the snapshot)
		for (const MigrationEnvelope& envelope : out.envelopes) {
			Component* component = static_cast<Component*>(ObjectRegistry::Resolve(envelope.componentId));
			if (!component) {
				continue;
			}
			const TypeInfo* type = TypeRegistry::FindType(StringHash(envelope.typeNameHash));
			bool isBehavior = type && TypeRegistry::IsDerivedFrom(type, Behavior::GetReflectType());

			if (component->IsEffectivelyEnabled() && !component->IsPendingDestroy()) {
				HookGuard guard;
				SceneInternalAccess::OnDisable(*component);
			}
			if (isBehavior) {
				UnregisterTicks(*static_cast<Behavior*>(component));
			}
		}

		{
			ScopedHandleResolver scope(MigrationResolverScope());
			for (MigrationEnvelope& envelope : out.envelopes) {
				Component* component = static_cast<Component*>(ObjectRegistry::Resolve(envelope.componentId));
				const TypeInfo* type = TypeRegistry::FindType(StringHash(envelope.typeNameHash));
				if (!component || !type) {
					continue;
				}
				SI::BinaryWriter writer;
				SI::BinaryWriteObject(writer, component, type, 0);
				envelope.blob.assign(writer.Data().begin(), writer.Data().end());
			}
		}

		// before module code page can go away

		for (Scene* scene : out.affectedScenes) {
			ScrubModuleVariantsFromRecords(*scene, moduleImage);
		}

		for (const MigrationEnvelope& envelope : out.envelopes) {
			Component* component = static_cast<Component*>(ObjectRegistry::Resolve(envelope.componentId));
			if (!component) {
				continue;
			}
			SceneObject* owner = SceneInternalAccess::OwnerOf(*component);

			if (envelope.prefabKey != 0 && owner) {
				if (PrefabInstanceRecord* record = FindEnclosingRecord(*owner)) {
					record->correspondence.erase(envelope.prefabKey);
				}
			}

			if (owner) {
				auto& slots = SceneInternalAccess::ComponentsOf(*owner);
				auto it = std::find_if(slots.begin(), slots.end(),
									   [&](const SceneInternalAccess::Slot& s) { return s.ptr == component; });
				if (it != slots.end()) {
					slots.erase(it);
				}
			}

			state.ownerById.erase(envelope.componentId);

			bool detached = ObjectRegistry::DetachPreservingSlot(*component);
			SLT_CORE_ASSERT_MSG(detached, "migration demolition failed to detach a component's registry slot");
			TypeRegistry::DestroyObjectInstance(component);
		}

		state.frameEnd.Drain();
	}

	void RestoreModuleComponents(MigrationSnapshot& snapshot) {
		SystemState& state = GetState();
		if (!state.initialized || snapshot.envelopes.empty()) {
			return;
		}
		SLT_SCENE_ASSERT_MAIN_THREAD();

		u32 restored = 0;
		u32 preservedCount = 0;
		u32 dropped = 0;

		{
			ScopedHandleResolver scope(MigrationResolverScope());
			std::vector<SI::HandleFixupJob> fixups;

			for (MigrationEnvelope& envelope : snapshot.envelopes) {
				auto drop = [&](const char* reason) {
					SLT_LOG_WARN("Module", "reload dropped component {:#018x} (type {:#018x}): {}",
								 envelope.componentId, envelope.typeNameHash, reason);
					ObjectRegistry::ReleaseReservedSlot(envelope.componentId);
					++dropped;
				};

				auto* owner = static_cast<SceneObject*>(ObjectRegistry::Resolve(envelope.ownerId));
				if (!owner || owner->IsPendingDestroy()) {
					drop("owner destroyed during demolition");
					continue;
				}

				const TypeInfo* type = TypeRegistry::FindType(StringHash(envelope.typeNameHash));
				Component* component = type ? CreateComponentInstance(type) : nullptr;
				const bool preserved = component == nullptr;
				if (preserved) {
					component = MakePreservedComponent(StringHash(envelope.typeNameHash), envelope.blob, true);
				}

				if (!ObjectRegistry::RebindPreservedSlot(*component, envelope.componentId)) {
					TypeRegistry::DestroyObjectInstance(component);
					drop("preserved registry slot could not be rebound");
					continue;
				}

				SceneInternalAccess::LocalIdOf(*component) = envelope.localId;
				SceneInternalAccess::PrefabKeyOf(*component) = envelope.prefabKey;

				AttachComponentRawAt(*owner, preserved ? UnresolvedComponent::GetReflectType() : type,
									 *component, envelope.slotIndex);

				if (!preserved) {
					SI::BinaryReader reader(envelope.blob.data(), envelope.blob.size());
					if (!SI::BinaryReadObject(reader, component, type, 0, fixups)) {
						SLT_LOG_WARN("Module", "reload: component {:#018x} blob only partially applied (layout drift)",
									 envelope.componentId);
					}
				}

				if (envelope.prefabKey != 0) {
					if (PrefabInstanceRecord* record = FindEnclosingRecord(*owner)) {
						record->correspondence[envelope.prefabKey] = component;
					}
				}

				envelope.restored = true;
				preserved ? ++preservedCount : ++restored;
			}

			SI::RunHandleFixups(fixups);
		}

		// after all envelopes are placed, OnEnable (never OnAttach)
		for (const MigrationEnvelope& envelope : snapshot.envelopes) {
			if (!envelope.restored) {
				continue;
			}
			auto* component = static_cast<Component*>(ObjectRegistry::Resolve(envelope.componentId));
			if (!component || component->IsPendingDestroy() || !component->IsEffectivelyEnabled()) {
				continue;
			}
			const TypeInfo* type = TypeRegistry::FindType(StringHash(envelope.typeNameHash));
			if (type && TypeRegistry::IsDerivedFrom(type, Behavior::GetReflectType())) {
				RegisterTicks(*static_cast<Behavior*>(component));
			}
			HookGuard guard;
			SceneInternalAccess::OnEnable(*component);
		}

		for (Scene* scene : snapshot.affectedScenes) {
			RefreshInstanceOverrides(*scene);
		}

		SLT_LOG_INFO("Module", "reload migration: {} component(s) restored, {} preserved unresolved, {} dropped",
					 restored, preservedCount, dropped);
	}

} // namespace Shalltear::SceneInternal
