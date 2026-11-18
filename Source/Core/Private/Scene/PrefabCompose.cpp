#include "SceneDocument.h"
#include "SceneInternal.h"
#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/BuildInfo.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Reflection/Walk.h"
#include "Shalltear/Core/Scene/Behavior.h"
#include "Shalltear/Core/Scene/Prefab.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/Serialization/HandleResolver.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace Shalltear {

	using namespace SceneInternal;
	namespace SI = SerializerInternal;

	namespace {

		u64 FoldPath(u64 seed, const PrefabTargetPath& path) {
			u64 key = seed;
			for (const PrefabFileRef& hop : path.hops) {
				key = FoldRef(key, hop.file, hop.localId);
			}
			return FoldRef(key, path.leaf.file, path.leaf.localId);
		}

		struct ComposeContext {
			Scene* scene = nullptr;
			bool detached = false;

			SceneInternal::SceneAssetBinder assets;

			std::unordered_map<u64, Object*> correspondence;
			std::unordered_map<u64, PrefabTargetPath> keyPaths;
			std::unordered_map<u64, u64> keyTypes;
			std::unordered_map<u64, u64> idToKey;
			std::unordered_map<u64, const TypeInfo*> keyTypeInfos;

			std::unordered_map<u64, Object*> reusePool;
			const HashMap<u64, u64>* reuseTypes = nullptr;

			std::unordered_set<u64> newObjectIds;
			std::vector<std::pair<Component*, const TypeInfo*>> newComponents;

			std::vector<Guid> activeStack;

			bool failed = false;
		};

		struct ChainDomain {
			std::unordered_map<u64, u64> relToAbs;
			u64 absSeed = 0;
		};

		struct DomainStackEntry {
			ChainDomain* domain;
			u64 relSeed;
		};

		void RegisterKeyInDomains(std::vector<DomainStackEntry>& domains, const Guid& file, u64 rowId, u64 absKey) {
			for (DomainStackEntry& entry : domains) {
				entry.domain->relToAbs[FoldRef(entry.relSeed, file, rowId)] = absKey;
			}
		}

		constexpr u32 MAX_CHAIN_DEPTH = 32;

		// caller must release 1 reference per element
		bool WalkChain(const Guid& leafGuid, std::vector<PrefabAsset*>& outChainRootFirst) {
			std::unordered_set<Guid> seen;
			std::vector<PrefabAsset*> leafFirst;
			Guid cursor = leafGuid;
			while (cursor.IsValid()) {
				if (leafFirst.size() >= MAX_CHAIN_DEPTH) {
					SLT_LOG_ERROR("Scene", "prefab chain exceeds depth {}", MAX_CHAIN_DEPTH);
					break;
				}
				if (!seen.insert(cursor).second) {
					SLT_LOG_ERROR("Scene", "prefab chain cycle at {}", cursor);
					break;
				}
				Asset* asset = AssetDatabase::LoadAssetByGuid(cursor);
				if (!asset || asset->GetAssetTypeId() != PrefabAsset::GetReflectType()->name) {
					SLT_LOG_ERROR("Scene", "prefab {} not loadable", cursor);
					if (asset) {
						AssetDatabase::UnloadAsset(asset);
					}
					break;
				}
				PrefabAsset* prefab = static_cast<PrefabAsset*>(asset);
				leafFirst.push_back(prefab);
				if (!prefab->GetFragment()) {
					SLT_LOG_ERROR("Scene", "prefab {} has no parsed fragment", cursor);
					break;
				}
				cursor = prefab->GetBasePrefab();
				if (!cursor.IsValid()) {
					outChainRootFirst.assign(leafFirst.rbegin(), leafFirst.rend());
					return true;
				}
			}
			for (PrefabAsset* prefab : leafFirst) {
				AssetDatabase::UnloadAsset(prefab);
			}
			return false;
		}

		struct FragmentBuildResult {
			SceneObject* root = nullptr;
			std::unordered_map<u64, u64> localIds;
			struct BuiltRow {
				const SceneDocumentRow* row;
				SceneObject* object;
				bool reused;
				std::vector<std::pair<u64, Component*>> components;
			};
			std::vector<BuiltRow> built;
		};

		struct LoadScopeCtx {
			std::unordered_map<u64, u64>* map;
			Scene* scene;
			SceneInternal::SceneAssetBinder* assets;
		};

		bool ComposeLoadScopeToHandle(void* user, const Guid& guid, StringHash, u64* outId) {
			LoadScopeCtx* ctx = static_cast<LoadScopeCtx*>(user);
			if (guid.high != SLT_LOCAL_SENTINEL) {
				return ctx->assets->Resolve(ctx->scene, guid, outId);
			}
			auto it = ctx->map->find(guid.low);
			*outId = it != ctx->map->end() ? it->second : 0;
			return true;
		}

		SceneObject* ComposeChain(ComposeContext& ctx, const Guid& prefabGuid,
								  const OverrideLayer* instanceLayer, const SceneDocument* instanceLayerDoc,
								  const Guid& instanceLayerFile, bool stampInstanceLayer,
								  SceneObject* parent, i32 insertIndex,
								  u64 absSeed, std::vector<DomainStackEntry> parentDomains,
								  const std::vector<PrefabFileRef>& hops,
								  std::vector<Guid>* outChainGuids, PrefabAsset** outLeafAsset);

		SceneObject* BuildFragmentRows(ComposeContext& ctx, const SceneDocument& document,
									   const std::vector<const SceneDocumentRow*>& rows,
									   const Guid& declaringFile, u64 absSeed,
									   std::vector<DomainStackEntry>& domains,
									   const std::vector<PrefabFileRef>& hops,
									   SceneObject* rootParent, i32 rootInsertIndex,
									   bool stampKeys) {
			FragmentBuildResult result;
			std::unordered_map<u64, SceneObject*> objectByRowId;
			struct DeferredInstance {
				const SceneDocumentRow* row;
			};

			for (const SceneDocumentRow* row : rows) {
				if (row->kind == SceneDocumentRow::Kind::Instance) {
					continue;
				}
				std::vector<std::pair<u64, Component*>> builtComponents;
				const TypeInfo* type = TypeRegistry::FindType(row->type);
				if (!type || !type->createFn || !TypeRegistry::IsDerivedFrom(type, SceneObject::GetReflectType())) {
					SLT_LOG_WARN("Scene", "prefab row {}: unknown entity type. Subtree skipped", row->id);
					continue;
				}

				u64 key = stampKeys ? FoldRef(absSeed, declaringFile, row->id) : 0;
				SceneObject* object = nullptr;
				bool reused = false;
				if (key != 0) {
					auto poolIt = ctx.reusePool.find(key);
					if (poolIt != ctx.reusePool.end()) {
						auto typeIt = ctx.reuseTypes ? ctx.reuseTypes->find(key) : HashMap<u64, u64>::const_iterator{};
						bool typeMatches = ctx.reuseTypes && typeIt != ctx.reuseTypes->end() && typeIt->second == row->type.GetValue();
						if (typeMatches) {
							object = static_cast<SceneObject*>(poolIt->second);
							reused = true;
						}
						ctx.reusePool.erase(poolIt);
					}
				}
				if (!object) {
					object = static_cast<SceneObject*>(TypeRegistry::CreateObjectInstance(type, MemTag::Scene));
					ctx.newObjectIds.insert(object->GetInstanceId());
				}

				SceneInternalAccess::LocalIdOf(*object) = stampKeys ? row->id : 0;
				SceneInternalAccess::PrefabKeyOf(*object) = key;
				objectByRowId[row->id] = object;
				result.localIds[row->id] = object->GetInstanceId();
				if (key != 0) {
					ctx.correspondence[key] = object;
					ctx.keyTypes[key] = row->type.GetValue();
					ctx.keyTypeInfos[key] = type;
					ctx.idToKey[object->GetInstanceId()] = key;
					PrefabTargetPath path;
					for (const PrefabFileRef& hop : hops) {
						path.hops.push_back(hop);
					}
					path.leaf = PrefabFileRef{declaringFile, row->id};
					ctx.keyPaths[key] = std::move(path);
					RegisterKeyInDomains(domains, declaringFile, row->id, key);
				}

				for (const SceneDocumentComponentRow& componentRow : row->components) {
					const TypeInfo* componentType = TypeRegistry::FindType(componentRow.type);
					u64 componentKey = stampKeys ? FoldRef(absSeed, declaringFile, componentRow.id) : 0;
					Component* component = nullptr;
					if (componentKey != 0) {
						auto poolIt = ctx.reusePool.find(componentKey);
						if (poolIt != ctx.reusePool.end()) {
							auto typeIt = ctx.reuseTypes ? ctx.reuseTypes->find(componentKey) : HashMap<u64, u64>::const_iterator{};
							bool typeMatches = ctx.reuseTypes && typeIt != ctx.reuseTypes->end() && typeIt->second == componentRow.type.GetValue();
							if (typeMatches) {
								bool pooledIsPreserved = IsPreservedComponentType(poolIt->second->GetType());
								if (pooledIsPreserved != (componentType == nullptr)) {
									typeMatches = false;
								}
							}
							if (typeMatches) {
								component = static_cast<Component*>(poolIt->second);
							}
							ctx.reusePool.erase(poolIt);
						}
					}
					if (!component) {
						component = componentType ? CreateComponentInstance(componentType) : nullptr;
						if (!component) {
							component = MakePreservedComponent(document, componentRow);
							componentType = UnresolvedComponent::GetReflectType();
						}
						AttachComponentRaw(*object, componentType, *component);
						ctx.newComponents.push_back({component, componentType});
					}
					SceneInternalAccess::LocalIdOf(*component) = stampKeys ? componentRow.id : 0;
					SceneInternalAccess::PrefabKeyOf(*component) = componentKey;
					result.localIds[componentRow.id] = component->GetInstanceId();
					builtComponents.push_back({componentRow.id, component});
					if (componentKey != 0) {
						ctx.correspondence[componentKey] = component;
						ctx.keyTypes[componentKey] = componentRow.type.GetValue();
						ctx.keyTypeInfos[componentKey] = componentType;
						ctx.idToKey[component->GetInstanceId()] = componentKey;
						PrefabTargetPath path;
						for (const PrefabFileRef& hop : hops) {
							path.hops.push_back(hop);
						}
						path.leaf = PrefabFileRef{declaringFile, componentRow.id};
						ctx.keyPaths[componentKey] = std::move(path);
						RegisterKeyInDomains(domains, declaringFile, componentRow.id, componentKey);
					}
				}
				result.built.push_back({row, object, reused, std::move(builtComponents)});
			}

			SceneObject* builtRoot = nullptr;
			for (const SceneDocumentRow* row : rows) {
				SceneObject* parent = nullptr;
				bool isSubsetRoot = false;
				if (row->parent != 0) {
					auto it = objectByRowId.find(row->parent);
					parent = it != objectByRowId.end() ? it->second : nullptr;
					if (!parent) {
						isSubsetRoot = true;
						parent = rootParent;
					}
				} else {
					isSubsetRoot = true;
					parent = rootParent;
				}

				if (row->kind == SceneDocumentRow::Kind::Instance) {
					u64 nestedSeed = FoldRef(absSeed, declaringFile, row->id);
					std::vector<PrefabFileRef> nestedHops = hops;
					nestedHops.push_back(PrefabFileRef{declaringFile, row->id});
					std::vector<DomainStackEntry> nestedDomains = domains;
					for (DomainStackEntry& entry : nestedDomains) {
						entry.relSeed = FoldRef(entry.relSeed, declaringFile, row->id);
					}
					SceneObject* nestedRoot = ComposeChain(ctx, row->prefab, &row->overrides, &document,
														   declaringFile, true,
														   parent, isSubsetRoot ? rootInsertIndex : -1,
														   nestedSeed, nestedDomains, nestedHops, nullptr, nullptr);
					if (isSubsetRoot && !builtRoot) {
						builtRoot = nestedRoot;
					}
					continue;
				}

				auto builtIt = std::find_if(result.built.begin(), result.built.end(),
											[&](const FragmentBuildResult::BuiltRow& b) { return b.row == row; });
				if (builtIt == result.built.end()) {
					continue;
				}
				SceneObject& object = *builtIt->object;
				i32 index = isSubsetRoot ? rootInsertIndex : -1;

				if (builtIt->reused) {
					if (SceneInternalAccess::ParentOf(object) != parent) {
						SceneObject* oldParent = SceneInternalAccess::ParentOf(object);
						Vector<SceneObject*>& oldSiblings = oldParent ? SceneInternalAccess::ChildrenOf(*oldParent)
																	  : SceneInternalAccess::RootsOf(*SceneInternalAccess::SceneOf(object));
						oldSiblings.erase(std::find(oldSiblings.begin(), oldSiblings.end(), &object));
						SceneInternalAccess::ParentOf(object) = parent;
						Vector<SceneObject*>& siblings = parent ? SceneInternalAccess::ChildrenOf(*parent)
																: SceneInternalAccess::RootsOf(*SceneInternalAccess::SceneOf(object));
						siblings.push_back(&object);
					}
				} else if (ctx.scene && !ctx.detached) {
					SceneInternalAccess::ParentOf(object) = parent;
					Vector<SceneObject*>& siblings = parent ? SceneInternalAccess::ChildrenOf(*parent)
															: SceneInternalAccess::RootsOf(*ctx.scene);
					if (index < 0 || static_cast<size_t>(index) >= siblings.size()) {
						siblings.push_back(&object);
					} else {
						siblings.insert(siblings.begin() + index, &object);
					}
					SceneInternalAccess::SceneOf(object) = ctx.scene;
					SceneInternalAccess::ObjectsOf(*ctx.scene).push_back(&object);
					GetState().ownerById[object.GetInstanceId()] = ctx.scene;
					for (const auto& slot : SceneInternalAccess::ComponentsOf(object)) {
						GetState().ownerById[slot.ptr->GetInstanceId()] = ctx.scene;
					}
				} else {
					SceneInternalAccess::ParentOf(object) = parent;
					if (parent) {
						SceneInternalAccess::ChildrenOf(*parent).push_back(&object);
					}
				}
				if (isSubsetRoot && !builtRoot) {
					builtRoot = &object;
				}
			}

			{
				LoadScopeCtx scopeCtx{&result.localIds, ctx.scene, &ctx.assets};
				ScopedHandleResolver scope({nullptr, &ComposeLoadScopeToHandle, &scopeCtx});
				std::vector<SI::HandleFixupJob> fixups;
				for (const FragmentBuildResult::BuiltRow& built : result.built) {
					ReadBlob(document, built.row->fields, built.object, SceneObject::GetReflectType(), fixups);
					for (const SceneDocumentComponentRow& componentRow : built.row->components) {
						for (const auto& [rowId, component] : built.components) {
							if (rowId != componentRow.id) {
								continue;
							}
							for (const auto& slot : SceneInternalAccess::ComponentsOf(*built.object)) {
								if (slot.ptr == component) {
									if (!IsPreservedComponentType(slot.type)) {
										ReadBlob(document, componentRow.fields, component, slot.type, fixups);
									}
									break;
								}
							}
							break;
						}
					}
				}
				SI::RunHandleFixups(fixups);
			}
			return builtRoot;
		}

		std::vector<const SceneDocumentRow*> CollectSubtreeRows(const SceneDocument& document, u64 rootRowId) {
			std::unordered_set<u64> wanted{rootRowId};
			bool grew = true;
			while (grew) {
				grew = false;
				for (const SceneDocumentRow& row : document.rows) {
					if (row.parent != 0 && wanted.count(row.parent) && !wanted.count(row.id)) {
						wanted.insert(row.id);
						grew = true;
					}
				}
			}
			std::vector<const SceneDocumentRow*> rows;
			for (const SceneDocumentRow& row : document.rows) {
				if (wanted.count(row.id)) {
					rows.push_back(&row);
				}
			}
			return rows;
		}

		void RemoveKeyedEntry(ComposeContext& ctx, u64 key) {
			auto it = ctx.correspondence.find(key);
			if (it != ctx.correspondence.end()) {
				ctx.idToKey.erase(it->second->GetInstanceId());
				ctx.correspondence.erase(it);
			}
			ctx.keyPaths.erase(key);
			ctx.keyTypes.erase(key);
			ctx.keyTypeInfos.erase(key);
		}

		void ApplyLayer(ComposeContext& ctx, ChainDomain& domain, const OverrideLayer& layer,
						const SceneDocument* declaringDoc, const Guid& declaringFile,
						std::vector<DomainStackEntry>& domains, const std::vector<PrefabFileRef>& hops,
						bool stampAdditions) {
			auto resolveTarget = [&](const PrefabTargetPath& path) -> std::pair<u64, Object*> {
				u64 relKey = FoldPath(0, path);
				auto absIt = domain.relToAbs.find(relKey);
				if (absIt == domain.relToAbs.end()) {
					return {0, nullptr};
				}
				auto it = ctx.correspondence.find(absIt->second);
				return {absIt->second, it != ctx.correspondence.end() ? it->second : nullptr};
			};

			for (const RemovedObjectOp& op : layer.removedObjects) {
				auto [key, target] = resolveTarget(op.target);
				if (!target) {
					continue;
				}
				SceneObject* object = static_cast<SceneObject*>(target);
				bool isNewObject = ctx.newObjectIds.count(object->GetInstanceId()) != 0;
				struct KeyReaper {
					ComposeContext* ctx;
					void Visit(SceneObject& node) {
						auto idIt = ctx->idToKey.find(node.GetInstanceId());
						if (idIt != ctx->idToKey.end()) {
							RemoveKeyedEntry(*ctx, idIt->second);
						}
						ctx->newObjectIds.erase(node.GetInstanceId());
						for (const auto& slot : SceneInternalAccess::ComponentsOf(node)) {
							auto slotIt = ctx->idToKey.find(slot.ptr->GetInstanceId());
							if (slotIt != ctx->idToKey.end()) {
								RemoveKeyedEntry(*ctx, slotIt->second);
							}
							Component* component = slot.ptr;
							ctx->newComponents.erase(std::remove_if(ctx->newComponents.begin(), ctx->newComponents.end(),
																	[&](const auto& pair) { return pair.first == component; }),
													 ctx->newComponents.end());
						}
						for (SceneObject* child : SceneInternalAccess::ChildrenOf(node)) {
							Visit(*child);
						}
					}
				};
				KeyReaper reaper{&ctx};
				reaper.Visit(*object);

				if (isNewObject) {
					FinalizeSubtree(*object, false);
				} else {
					DoomSubtree(*object);
				}
			}
			for (const RemovedComponentOp& op : layer.removedComponents) {
				auto [key, target] = resolveTarget(op.target);
				if (!target) {
					continue;
				}
				Component* component = static_cast<Component*>(target);
				RemoveKeyedEntry(ctx, key);
				bool isNew = std::find_if(ctx.newComponents.begin(), ctx.newComponents.end(),
										  [&](const auto& pair) { return pair.first == component; }) != ctx.newComponents.end();
				if (isNew || ctx.newObjectIds.count(SceneInternalAccess::OwnerOf(*component)->GetInstanceId())) {
					SceneObject* owner = SceneInternalAccess::OwnerOf(*component);
					auto& slots = SceneInternalAccess::ComponentsOf(*owner);
					slots.erase(std::find_if(slots.begin(), slots.end(),
											 [&](const auto& slot) { return slot.ptr == component; }));
					ctx.newComponents.erase(std::remove_if(ctx.newComponents.begin(), ctx.newComponents.end(),
														   [&](const auto& pair) { return pair.first == component; }),
											ctx.newComponents.end());
					GetState().ownerById.erase(component->GetInstanceId());
					TypeRegistry::DestroyObjectInstance(component);
				} else {
					DoomComponentOnly(*component);
				}
			}

			for (const AddedObjectOp& op : layer.addedObjects) {
				if (op.liveRoot.GetInstanceId() != 0) {
					continue;
				}
				if (!declaringDoc || op.rootRowId == 0) {
					continue;
				}
				auto [parentKey, parentTarget] = resolveTarget(op.parent);
				if (!parentTarget) {
					SLT_LOG_WARN("Scene", "added-object op: parent path unresolved. Skipped");
					continue;
				}
				std::vector<const SceneDocumentRow*> rows = CollectSubtreeRows(*declaringDoc, op.rootRowId);
				if (rows.empty()) {
					continue;
				}
				BuildFragmentRows(ctx, *declaringDoc, rows, declaringFile, domain.absSeed, domains, hops,
								  static_cast<SceneObject*>(parentTarget), static_cast<i32>(op.insertIndex), stampAdditions);
			}
			for (const AddedComponentOp& op : layer.addedComponents) {
				if (op.liveComponent.GetInstanceId() != 0) {
					continue;
				}
				if (!declaringDoc || op.componentRowId == 0) {
					continue;
				}
				auto [ownerKey, ownerTarget] = resolveTarget(op.owner);
				if (!ownerTarget) {
					SLT_LOG_WARN("Scene", "added-component op: owner path unresolved. Skipped");
					continue;
				}
				const SceneDocumentComponentRow* componentRow = nullptr;
				for (const SceneDocumentComponentRow& loose : declaringDoc->looseComponents) {
					if (loose.id == op.componentRowId) {
						componentRow = &loose;
						break;
					}
				}
				if (!componentRow) {
					continue;
				}
				SceneObject* owner = static_cast<SceneObject*>(ownerTarget);
				u64 key = stampAdditions ? FoldRef(domain.absSeed, declaringFile, componentRow->id) : 0;

				Component* component = nullptr;
				if (key != 0) {
					auto poolIt = ctx.reusePool.find(key);
					if (poolIt != ctx.reusePool.end()) {
						auto typeIt = ctx.reuseTypes ? ctx.reuseTypes->find(key) : HashMap<u64, u64>::const_iterator{};
						bool typeMatches = ctx.reuseTypes && typeIt != ctx.reuseTypes->end() &&
										   typeIt->second == componentRow->type.GetValue();
						if (typeMatches && IsPreservedComponentType(poolIt->second->GetType()) !=
											   (TypeRegistry::FindType(componentRow->type) == nullptr)) {
							typeMatches = false;
						}
						if (typeMatches) {
							component = static_cast<Component*>(poolIt->second);
						}
						ctx.reusePool.erase(poolIt);
					}
				}
				const TypeInfo* componentType = TypeRegistry::FindType(componentRow->type);
				bool preserved = false;
				if (!component) {
					component = componentType ? CreateComponentInstance(componentType) : nullptr;
					if (!component) {
						component = MakePreservedComponent(*declaringDoc, *componentRow);
						componentType = UnresolvedComponent::GetReflectType();
						preserved = true;
					}
					AttachComponentRaw(*owner, componentType, *component);
					ctx.newComponents.push_back({component, componentType});
				}
				SceneInternalAccess::LocalIdOf(*component) = stampAdditions ? componentRow->id : 0;
				SceneInternalAccess::PrefabKeyOf(*component) = key;
				if (key != 0) {
					ctx.correspondence[key] = component;
					ctx.keyTypes[key] = componentRow->type.GetValue();
					ctx.keyTypeInfos[key] = componentType;
					ctx.idToKey[component->GetInstanceId()] = key;
					PrefabTargetPath path;
					for (const PrefabFileRef& hop : hops) {
						path.hops.push_back(hop);
					}
					path.leaf = PrefabFileRef{declaringFile, componentRow->id};
					ctx.keyPaths[key] = std::move(path);
					RegisterKeyInDomains(domains, declaringFile, componentRow->id, key);
				}
				if (!preserved && !IsPreservedComponentType(componentType)) {
					std::vector<SI::HandleFixupJob> fixups;
					ReadBlob(*declaringDoc, componentRow->fields, component, componentType, fixups);
					SI::RunHandleFixups(fixups);
				}
			}

			for (const PropertyOverrideOp& op : layer.properties) {
				auto [key, target] = resolveTarget(op.target);
				if (!target || op.fieldPath.empty()) {
					continue;
				}
				auto typeIt = ctx.keyTypeInfos.find(key);
				if (typeIt == ctx.keyTypeInfos.end()) {
					continue;
				}
				void* instance = target;
				const TypeInfo* type = typeIt->second;
				const FieldInfo* field = nullptr;
				bool walkOk = true;
				SyncFieldsBeforeRead(target, typeIt->second);
				for (size_t segment = 0; segment < op.fieldPath.size(); ++segment) {
					field = FindField(type, op.fieldPath[segment]);
					if (!field || !field->addrFn) {
						walkOk = false;
						break;
					}
					if (segment + 1 < op.fieldPath.size()) {
						if (!(field->flags & FieldFlagSubobject)) {
							walkOk = false;
							break;
						}
						instance = field->addrFn(instance);
						type = field->fieldType;
					}
				}
				if (!walkOk || !field) {
					SLT_LOG_WARN("Scene", "property op: field path unresolved. Skipped");
					continue;
				}
				void* fieldPtr = field->addrFn(instance);
				if (op.value.isHandle) {
					u64 resolved = 0;
					const Guid& guid = op.value.handleGuid;
					if (Object* live = op.value.liveTarget.Get()) {
						resolved = live->GetInstanceId();
					} else if (guid.high == SLT_PREFAB_KEY_SENTINEL) {
						auto absIt = domain.relToAbs.find(guid.low);
						if (absIt != domain.relToAbs.end()) {
							auto corrIt = ctx.correspondence.find(absIt->second);
							resolved = corrIt != ctx.correspondence.end() ? corrIt->second->GetInstanceId() : 0;
						}
					} else if (guid.high == SLT_LOCAL_SENTINEL) {
						resolved = HandleResolver::ToHandle(guid, op.value.handlePointee);
					} else if (guid.IsValid() && !ctx.assets.Resolve(ctx.scene, guid, &resolved)) {
						resolved = HandleResolver::ToHandle(guid, op.value.handlePointee);
					}
					*reinterpret_cast<u64*>(fieldPtr) = resolved;
				} else {
					SetFieldValue(field, instance, &op.value.direct);
				}
				SyncFieldsAfterWrite(target, typeIt->second);
			}
		}

		SceneObject* ComposeChain(ComposeContext& ctx, const Guid& prefabGuid,
								  const OverrideLayer* instanceLayer, const SceneDocument* instanceLayerDoc,
								  const Guid& instanceLayerFile, bool stampInstanceLayer,
								  SceneObject* parent, i32 insertIndex,
								  u64 absSeed, std::vector<DomainStackEntry> parentDomains,
								  const std::vector<PrefabFileRef>& hops,
								  std::vector<Guid>* outChainGuids, PrefabAsset** outLeafAsset) {
			for (const Guid& active : ctx.activeStack) {
				if (active == prefabGuid) {
					SLT_LOG_ERROR("Scene", "nested prefab cycle at {}. Row skipped", prefabGuid);
					return nullptr;
				}
			}
			ctx.activeStack.push_back(prefabGuid);

			std::vector<PrefabAsset*> chain;
			if (!WalkChain(prefabGuid, chain)) {
				ctx.activeStack.pop_back();
				return nullptr;
			}

			ChainDomain domain;
			domain.absSeed = absSeed;
			std::vector<DomainStackEntry> domains = std::move(parentDomains);
			domains.push_back({&domain, 0});

			const SceneDocument& rootDoc = *chain.front()->GetFragment();
			Guid rootFile = chain.front()->assetId;
			std::unordered_set<u64> layerOwned = ComputeLayerOwnedRows(rootDoc);
			std::vector<const SceneDocumentRow*> rows;
			u32 rootRows = 0;
			for (const SceneDocumentRow& row : rootDoc.rows) {
				if (layerOwned.count(row.id)) {
					continue;
				}
				if (row.parent == 0) {
					++rootRows;
				}
				rows.push_back(&row);
			}
			SLT_CORE_ASSERT_MSG(rootRows == 1, "root prefab must declare 1 root object");

			SceneObject* root = BuildFragmentRows(ctx, rootDoc, rows, rootFile, absSeed, domains, hops,
												  parent, insertIndex, true);
			if (!root) {
				for (PrefabAsset* prefab : chain) {
					AssetDatabase::UnloadAsset(prefab);
				}
				ctx.activeStack.pop_back();
				return nullptr;
			}

			for (PrefabAsset* prefab : chain) {
				ApplyLayer(ctx, domain, prefab->GetOverrides(), prefab->GetFragment(), prefab->assetId,
						   domains, hops, true);
			}
			if (instanceLayer) {
				ApplyLayer(ctx, domain, *instanceLayer, instanceLayerDoc, instanceLayerFile,
						   domains, hops, stampInstanceLayer);
			}

			if (outChainGuids) {
				for (PrefabAsset* prefab : chain) {
					outChainGuids->push_back(prefab->assetId);
				}
			}
			for (size_t i = 0; i < chain.size(); ++i) {
				bool isLeaf = i + 1 == chain.size();
				if (isLeaf && outLeafAsset) {
					*outLeafAsset = chain[i];
				} else {
					AssetDatabase::UnloadAsset(chain[i]);
				}
			}
			ctx.activeStack.pop_back();
			return root;
		}

		void DeriveAndDispatch(ComposeContext& ctx, SceneObject& root, bool freshIsland) {
			SceneInternalAccess::MarkWorldDirty(root);
			struct ScaleDeriver {
				static void Visit(SceneObject& node) {
					SceneInternalAccess::DeriveScaleKind(node);
					for (SceneObject* child : SceneInternalAccess::ChildrenOf(node)) {
						Visit(*child);
					}
				}
			};
			ScaleDeriver::Visit(root);

			if (ctx.detached) {
				return;
			}
			DeriveActiveSubtree(root);

			if (freshIsland) {
				DispatchIslandAttach(root);
				return;
			}
			for (const auto& [component, componentType] : ctx.newComponents) {
				if (component->IsPendingDestroy()) {
					continue;
				}
				{
					HookGuard guard;
					SceneInternalAccess::OnAttach(*component);
				}
				if (component->IsEffectivelyEnabled()) {
					if (TypeRegistry::IsDerivedFrom(componentType, Behavior::GetReflectType())) {
						RegisterTicks(*static_cast<Behavior*>(component));
					}
					HookGuard guard;
					SceneInternalAccess::OnEnable(*component);
				}
			}
		}

		PrefabInstanceRecord* FindRecord(Scene& scene, SceneObject& root) {
			u32 index = SceneInternalAccess::PrefabRecordOf(root);
			if (index == 0 || index > SceneInternalAccess::InstancesOf(scene).size()) {
				return nullptr;
			}
			return SceneInternalAccess::InstancesOf(scene)[index - 1];
		}

		void StoreContextInRecord(ComposeContext& ctx, PrefabInstanceRecord& record) {
			record.correspondence.clear();
			record.keyPaths.clear();
			record.keyTypes.clear();
			for (const auto& [key, object] : ctx.correspondence) {
				record.correspondence[key] = object;
			}
			for (const auto& [key, path] : ctx.keyPaths) {
				record.keyPaths[key] = path;
			}
			for (const auto& [key, typeHash] : ctx.keyTypes) {
				record.keyTypes[key] = typeHash;
			}
		}

		PrefabInstanceRecord* CreateRecord(ComposeContext& ctx, Scene& scene, SceneObject& root,
										   const Guid& prefabGuid, std::vector<Guid>&& chainGuids,
										   PrefabAsset* leafAsset, u64 sceneRowId) {
			PrefabInstanceRecord* record = New<PrefabInstanceRecord>(MemTag::Scene);
			record->prefabGuid = prefabGuid;
			for (const Guid& guid : chainGuids) {
				record->chainGuids.push_back(guid);
			}
			record->root = Handle<SceneObject>(&root);
			record->asset = leafAsset;
			record->sceneRowId = sceneRowId;
			StoreContextInRecord(ctx, *record);

			Vector<PrefabInstanceRecord*>& records = SceneInternalAccess::InstancesOf(scene);
			records.push_back(record);
			SceneInternalAccess::PrefabRecordOf(root) = static_cast<u32>(records.size());
			return record;
		}

		struct DiffSink {
			PrefabInstanceDiff* out = nullptr;
			Object* liveObject = nullptr;
		};

		void EmitPropertyOp(OverrideLayer& layer, const PrefabTargetPath& target,
							const std::vector<StringHash>& fieldPath, const FieldInfo* field,
							const void* liveInstance, bool isHandleValue, const Guid& handleGuid, StringHash handlePointee,
							u64 liveTargetId,
							const void* stagingInstance, u64 baseHandleId, const DiffSink* sink) {
			PropertyOverrideOp op;
			op.target = target;
			for (StringHash segment : fieldPath) {
				op.fieldPath.push_back(segment);
			}
			op.fieldPath.push_back(field->name);
			if (isHandleValue) {
				op.value.isHandle = true;
				op.value.handleGuid = handleGuid;
				op.value.handlePointee = handlePointee;
				op.value.liveTarget = Handle<Object>(ObjectRegistry::Resolve(liveTargetId));
			} else {
				Variant boxed;
				GetFieldValue(field, liveInstance, &boxed);
				op.value.direct = std::move(boxed);
			}
			if (sink && sink->out) {
				PrefabInstanceDiff::Property property;
				property.target = Handle<Object>(sink->liveObject);
				property.row = target;
				for (StringHash segment : op.fieldPath) {
					property.fieldPath.push_back(segment);
				}
				property.isHandle = isHandleValue;
				if (isHandleValue) {
					property.baseHandleId = baseHandleId;
				} else {
					GetFieldValue(field, stagingInstance, &property.base);
				}
				sink->out->properties.push_back(std::move(property));
			}
			layer.properties.push_back(std::move(op));
		}

		struct DiffMaps {
			const std::unordered_map<u64, u64>* liveIdToKey;
			const std::unordered_map<u64, u64>* stagingIdToKey;
			const HashMap<u64, Object*>* liveByKey;
		};

		void DiffInstanceFields(const void* live, const void* staging, const TypeInfo* type,
								const PrefabTargetPath& target, std::vector<StringHash>& fieldPath,
								const DiffMaps& maps, OverrideLayer& out, const DiffSink* sink) {
			std::vector<const FieldInfo*> fields;
			SI::CollectSerializedFields(type, fields);
			for (const FieldInfo* field : fields) {
				if (!field->addrFn) {
					continue;
				}
				const void* livePtr = field->addrFn(const_cast<void*>(live));
				const void* stagingPtr = field->addrFn(const_cast<void*>(staging));
				switch (SI::CategorizeField(field)) {
				case SI::FieldCategory::Handle: {
					u64 liveId = *reinterpret_cast<const u64*>(livePtr);
					u64 stagingId = *reinterpret_cast<const u64*>(stagingPtr);
					auto liveKeyIt = maps.liveIdToKey->find(liveId);
					auto stagingKeyIt = maps.stagingIdToKey->find(stagingId);
					bool liveKeyed = liveKeyIt != maps.liveIdToKey->end();
					bool stagingKeyed = stagingKeyIt != maps.stagingIdToKey->end();
					bool equal;
					if (liveKeyed && stagingKeyed) {
						equal = liveKeyIt->second == stagingKeyIt->second;
					} else if (!liveKeyed && !stagingKeyed) {
						equal = liveId == stagingId;
					} else {
						equal = false;
					}
					if (!equal) {
						Guid guid{};
						StringHash pointee{};
						u64 liveTargetId = 0;
						if (liveKeyed) {
							guid = Guid{liveKeyIt->second, SLT_PREFAB_KEY_SENTINEL};
						} else if (liveId != 0 && !HandleResolver::ToGuid(liveId, &guid, &pointee)) {
							guid = Guid{};
							liveTargetId = liveId;
						}
						u64 baseHandleId = stagingId;
						if (stagingKeyed && sink && sink->out) {
							auto counterpart = maps.liveByKey->find(stagingKeyIt->second);
							baseHandleId = counterpart != maps.liveByKey->end() ? counterpart->second->GetInstanceId() : 0;
						}
						EmitPropertyOp(out, target, fieldPath, field, live, true, guid, pointee, liveTargetId, staging, baseHandleId, sink);
					}
					break;
				}
				case SI::FieldCategory::Subobject:
					fieldPath.push_back(field->name);
					DiffInstanceFields(livePtr, stagingPtr, field->fieldType, target, fieldPath, maps, out, sink);
					fieldPath.pop_back();
					break;
				case SI::FieldCategory::Str:
					if (*reinterpret_cast<const str*>(livePtr) != *reinterpret_cast<const str*>(stagingPtr)) {
						EmitPropertyOp(out, target, fieldPath, field, live, false, Guid{}, StringHash(), 0, staging, 0, sink);
					}
					break;
				case SI::FieldCategory::Enum:
				case SI::FieldCategory::Guid:
				case SI::FieldCategory::MathPrim:
				case SI::FieldCategory::Prim:
				case SI::FieldCategory::Trivial:
					if (std::memcmp(livePtr, stagingPtr, field->size) != 0) {
						EmitPropertyOp(out, target, fieldPath, field, live, false, Guid{}, StringHash(), 0, staging, 0, sink);
					}
					break;
				default:
					break;
				}
			}
		}

		Object* LiveCounterpartOf(const ComposeContext& stagingCtx, const PrefabInstanceRecord& record, Object* stagingObject) {
			if (!stagingObject) {
				return nullptr;
			}
			auto keyIt = stagingCtx.idToKey.find(stagingObject->GetInstanceId());
			if (keyIt == stagingCtx.idToKey.end()) {
				return nullptr;
			}
			auto liveIt = record.correspondence.find(keyIt->second);
			return liveIt != record.correspondence.end() ? liveIt->second : nullptr;
		}

		bool DiffInstanceInternal(PrefabInstanceRecord& record, const Guid& stagingGuid,
								  OverrideLayer& fresh, PrefabInstanceDiff* sink) {
			SceneObject* liveRoot = record.root.Get();
			if (!liveRoot || liveRoot->IsPendingDestroy()) {
				return false;
			}

			ComposeContext stagingCtx;
			stagingCtx.detached = true;
			SceneObject* stagingRoot = ComposeChain(stagingCtx, stagingGuid, nullptr, nullptr,
													Guid{}, false, nullptr, -1, 0, {}, {}, nullptr, nullptr);
			if (!stagingRoot) {
				return false;
			}
			if (SceneInternalAccess::PrefabKeyOf(*liveRoot) != SceneInternalAccess::PrefabKeyOf(*stagingRoot)) {
				SLT_LOG_WARN("Scene", "prefab diff: {} doesn't compose the root row the live graph came from", stagingGuid);
				FinalizeSubtree(*stagingRoot, false);
				return false;
			}

			std::unordered_map<u64, u64> liveIdToKey;
			for (const auto& [key, object] : record.correspondence) {
				liveIdToKey[object->GetInstanceId()] = key;
			}
			DiffMaps maps{&liveIdToKey, &stagingCtx.idToKey, &record.correspondence};

			std::vector<u64> stagingKeys;
			stagingKeys.reserve(stagingCtx.correspondence.size());
			for (const auto& [key, object] : stagingCtx.correspondence) {
				stagingKeys.push_back(key);
			}
			std::sort(stagingKeys.begin(), stagingKeys.end());

			for (u64 key : stagingKeys) {
				Object* stagingObject = stagingCtx.correspondence[key];
				auto liveIt = record.correspondence.find(key);
				Object* liveObject = liveIt != record.correspondence.end() ? liveIt->second : nullptr;
				bool liveIsObject = stagingCtx.keyTypes[key] == SceneObject::GetReflectType()->name.GetValue();
				bool liveGone = !liveObject;
				if (!liveGone) {
					if (liveIsObject) {
						liveGone = static_cast<SceneObject*>(liveObject)->IsPendingDestroy();
					} else {
						liveGone = static_cast<Component*>(liveObject)->IsPendingDestroy();
					}
				}
				const PrefabTargetPath& path = stagingCtx.keyPaths[key];
				if (liveGone) {
					if (liveIsObject) {
						SceneObject* stagingNode = static_cast<SceneObject*>(stagingObject);
						SceneObject* stagingParent = SceneInternalAccess::ParentOf(*stagingNode);
						bool parentAlsoGone = false;
						if (stagingParent) {
							auto parentKeyIt = stagingCtx.idToKey.find(stagingParent->GetInstanceId());
							if (parentKeyIt != stagingCtx.idToKey.end()) {
								auto parentLiveIt = record.correspondence.find(parentKeyIt->second);
								parentAlsoGone = parentLiveIt == record.correspondence.end() ||
												 static_cast<SceneObject*>(parentLiveIt->second)->IsPendingDestroy();
							}
						}
						if (!parentAlsoGone) {
							RemovedObjectOp op;
							op.target = path;
							fresh.removedObjects.push_back(std::move(op));
							if (sink) {
								PrefabInstanceDiff::RemovedObject removed;
								removed.parent = Handle<SceneObject>(static_cast<SceneObject*>(LiveCounterpartOf(stagingCtx, record, stagingParent)));
								removed.row = path;
								removed.name = stagingNode->name;
								sink->removedObjects.push_back(std::move(removed));
							}
						}
					} else {
						Component* stagingComponent = static_cast<Component*>(stagingObject);
						SceneObject* stagingOwner = SceneInternalAccess::OwnerOf(*stagingComponent);
						auto ownerKeyIt = stagingCtx.idToKey.find(stagingOwner->GetInstanceId());
						bool ownerAlsoGone = false;
						if (ownerKeyIt != stagingCtx.idToKey.end()) {
							auto ownerLiveIt = record.correspondence.find(ownerKeyIt->second);
							ownerAlsoGone = ownerLiveIt == record.correspondence.end() ||
											static_cast<SceneObject*>(ownerLiveIt->second)->IsPendingDestroy();
						}
						if (!ownerAlsoGone) {
							RemovedComponentOp op;
							op.target = path;
							fresh.removedComponents.push_back(std::move(op));
							if (sink) {
								PrefabInstanceDiff::RemovedComponent removed;
								removed.owner = Handle<SceneObject>(static_cast<SceneObject*>(LiveCounterpartOf(stagingCtx, record, stagingOwner)));
								removed.row = path;
								removed.type = stagingCtx.keyTypeInfos[key]->name;
								const auto& slots = SceneInternalAccess::ComponentsOf(*stagingOwner);
								for (u32 i = 0; i < slots.size(); ++i) {
									if (slots[i].ptr == stagingComponent) {
										removed.index = i;
										break;
									}
								}
								sink->removedComponents.push_back(std::move(removed));
							}
						}
					}
					continue;
				}
				const TypeInfo* type = stagingCtx.keyTypeInfos[key];
				std::vector<StringHash> fieldPath;
				DiffSink rowSink{sink, liveObject};
				SyncFieldsBeforeRead(liveObject, type);
				SyncFieldsBeforeRead(stagingObject, type);
				DiffInstanceFields(liveObject, stagingObject, type, path, fieldPath, maps, fresh, sink ? &rowSink : nullptr);
			}

			struct AdditionScanner {
				PrefabInstanceRecord* record;
				std::unordered_map<u64, u64>* liveIdToKey;
				const std::unordered_map<u64, Object*>* stagingByKey;
				OverrideLayer* fresh;
				PrefabInstanceDiff* sink;

				bool IsAddition(u64 key) const {
					return key == 0 || stagingByKey->find(key) == stagingByKey->end();
				}

				void Visit(SceneObject& node, bool nodeIsAddition) {
					if (!nodeIsAddition) {
						for (const auto& slot : SceneInternalAccess::ComponentsOf(node)) {
							u64 key = SceneInternalAccess::PrefabKeyOf(*slot.ptr);
							if (slot.ptr->IsPendingDestroy() || !IsAddition(key)) {
								continue;
							}
							if (sink) {
								sink->addedComponents.push_back(Handle<Component>(slot.ptr));
							}
							if (key != 0) {
								continue;
							}
							AddedComponentOp op;
							auto keyIt = liveIdToKey->find(node.GetInstanceId());
							if (keyIt == liveIdToKey->end()) {
								continue;
							}
							op.owner = record->keyPaths[keyIt->second];
							op.liveComponent = Handle<Component>(slot.ptr);
							fresh->addedComponents.push_back(std::move(op));
						}
					}
					for (SceneObject* child : SceneInternalAccess::ChildrenOf(node)) {
						if (child->IsPendingDestroy()) {
							continue;
						}
						u64 childKey = SceneInternalAccess::PrefabKeyOf(*child);
						bool childIsAddition = IsAddition(childKey);
						if (childIsAddition && !nodeIsAddition) {
							if (sink) {
								sink->addedObjects.push_back(Handle<SceneObject>(child));
							}
							auto keyIt = liveIdToKey->find(node.GetInstanceId());
							if (childKey == 0 && keyIt != liveIdToKey->end()) {
								AddedObjectOp op;
								op.parent = record->keyPaths[keyIt->second];
								op.insertIndex = child->GetSiblingIndex();
								op.liveRoot = Handle<SceneObject>(child);
								fresh->addedObjects.push_back(std::move(op));
							}
						}
						if (!childIsAddition) {
							Visit(*child, false);
						}
					}
				}
			};
			AdditionScanner scanner{&record, &liveIdToKey, &stagingCtx.correspondence, &fresh, sink};
			scanner.Visit(*liveRoot, false);

			FinalizeSubtree(*stagingRoot, false);
			return true;
		}

		bool RecordInstanceOverridesInternal(PrefabInstanceRecord& record) {
			OverrideLayer fresh;
			if (!DiffInstanceInternal(record, record.prefabGuid, fresh, nullptr)) {
				return false;
			}
			record.liveOverrides = std::move(fresh);
			return true;
		}

		void CollectGraphCorrespondence(SceneObject& root, PrefabInstanceRecord& record) {
			struct KeyWalker {
				PrefabInstanceRecord* record;
				void Visit(SceneObject& node) {
					if (node.IsPendingDestroy()) {
						return;
					}
					if (u64 key = SceneInternalAccess::PrefabKeyOf(node); key != 0) {
						record->correspondence[key] = &node;
					}
					for (const auto& slot : SceneInternalAccess::ComponentsOf(node)) {
						if (slot.ptr->IsPendingDestroy()) {
							continue;
						}
						if (u64 key = SceneInternalAccess::PrefabKeyOf(*slot.ptr); key != 0) {
							record->correspondence[key] = slot.ptr;
						}
					}
					for (SceneObject* child : SceneInternalAccess::ChildrenOf(node)) {
						Visit(*child);
					}
				}
			};
			KeyWalker{&record}.Visit(root);
		}

		bool SamePath(const PrefabTargetPath& a, const PrefabTargetPath& b) {
			if (!(a.leaf == b.leaf) || a.hops.size() != b.hops.size()) {
				return false;
			}
			for (size_t i = 0; i < a.hops.size(); ++i) {
				if (!(a.hops[i] == b.hops[i])) {
					return false;
				}
			}
			return true;
		}

		void RemergeRecord(Scene& scene, u32 recordIndex1Based) {
			PrefabInstanceRecord* record = SceneInternalAccess::InstancesOf(scene)[recordIndex1Based - 1];
			if (!record) {
				return;
			}
			SceneObject* liveRoot = record->root.Get();
			if (!liveRoot || liveRoot->IsPendingDestroy()) {
				return;
			}

			ComposeContext ctx;
			ctx.scene = &scene;
			for (const auto& [key, object] : record->correspondence) {
				ctx.reusePool[key] = object;
			}
			ctx.reuseTypes = &record->keyTypes;

			SceneObject* parent = SceneInternalAccess::ParentOf(*liveRoot);
			std::vector<Guid> chainGuids;
			SceneObject* composedRoot = ComposeChain(ctx, record->prefabGuid, &record->liveOverrides, nullptr,
													 Guid{}, false, parent, -1, 0, {}, {}, &chainGuids, nullptr);
			if (!composedRoot) {
				SLT_LOG_ERROR("Scene", "re-merge failed for prefab {}", record->prefabGuid);
				return;
			}
			SLT_CORE_ASSERT_MSG(composedRoot == liveRoot, "re-merge must reuse the instance root");

			for (const auto& [key, object] : ctx.reusePool) {
				auto typeIt = record->keyTypes.find(key);
				bool isObject = typeIt != record->keyTypes.end() &&
								typeIt->second == SceneObject::GetReflectType()->name.GetValue();
				if (isObject) {
					SceneObject* gone = static_cast<SceneObject*>(object);
					if (!gone->IsPendingDestroy()) {
						DoomSubtree(*gone);
					}
				} else {
					Component* gone = static_cast<Component*>(object);
					if (!gone->IsPendingDestroy()) {
						DoomComponentOnly(*gone);
					}
				}
			}

			record->chainGuids = Vector<Guid>{GetMemoryResource(MemTag::Scene)};
			for (const Guid& guid : chainGuids) {
				record->chainGuids.push_back(guid);
			}
			StoreContextInRecord(ctx, *record);
			DeriveAndDispatch(ctx, *liveRoot, false);
		}

	} // anonymous namespace

	namespace SceneInternal {

		void RefreshInstanceOverrides(Scene& scene) {
			Vector<PrefabInstanceRecord*>& records = SceneInternalAccess::InstancesOf(scene);
			for (PrefabInstanceRecord* record : records) {
				if (record) {
					RecordInstanceOverridesInternal(*record);
				}
			}
		}

		SceneObject* ComposeInstanceRow(const SceneDocument& document, const SceneDocumentRow& row,
										Scene& scene, SceneObject* parent) {
			ComposeContext ctx;
			ctx.scene = &scene;

			std::vector<Guid> chainGuids;
			PrefabAsset* leafAsset = nullptr;
			SceneObject* root = ComposeChain(ctx, row.prefab, &row.overrides, &document,
											 Guid{}, false, parent, -1, 0, {}, {}, &chainGuids, &leafAsset);
			if (!root) {
				SLT_LOG_WARN("Scene", "instance row {}: prefab {} failed to compose. Skipped", row.id, row.prefab);
				return nullptr;
			}

			PrefabInstanceRecord* record = CreateRecord(ctx, scene, *root, row.prefab,
														std::move(chainGuids), leafAsset, row.id);

			record->liveOverrides = row.overrides;
			for (AddedObjectOp& op : record->liveOverrides.addedObjects) {
				op.liveRoot = Handle<SceneObject>();
				op.rootRowId = 0;
			}
			for (AddedComponentOp& op : record->liveOverrides.addedComponents) {
				op.liveComponent = Handle<Component>();
				op.componentRowId = 0;
			}

			SceneInternalAccess::MarkWorldDirty(*root);
			struct ScaleDeriver {
				static void Visit(SceneObject& node) {
					SceneInternalAccess::DeriveScaleKind(node);
					for (SceneObject* child : SceneInternalAccess::ChildrenOf(node)) {
						Visit(*child);
					}
				}
			};
			ScaleDeriver::Visit(*root);
			return root;
		}

		void EmitInstanceRow(SceneDocument& document, Scene& scene, SceneObject& instanceRoot, u64 parentId) {
			PrefabInstanceRecord* record = FindRecord(scene, instanceRoot);
			if (!record) {
				SLT_LOG_WARN("Scene", "instance '{}' has no record. Not saved", instanceRoot.name);
				return;
			}
			if (record->sceneRowId == 0) {
				record->sceneRowId = SceneInternalAccess::NextLocalIdOf(scene)++;
			}

			SceneDocumentRow row;
			row.id = record->sceneRowId;
			row.parent = parentId;
			row.kind = SceneDocumentRow::Kind::Instance;
			row.prefab = record->prefabGuid;

			std::unordered_map<u64, u64> liveIdToKey;
			for (const auto& [key, object] : record->correspondence) {
				liveIdToKey[object->GetInstanceId()] = key;
			}

			OverrideLayer& live = record->liveOverrides;
			for (const PropertyOverrideOp& op : live.properties) {
				row.overrides.properties.push_back(op);
				PropertyOverrideOp& fileOp = row.overrides.properties.back();
				if (Object* target = fileOp.value.liveTarget.Get()) {
					if (!HandleResolver::ToGuid(target->GetInstanceId(), &fileOp.value.handleGuid,
												&fileOp.value.handlePointee)) {
						SLT_LOG_WARN("Scene", "an override on '{}' points at something this file can't reference (another scene's row, or a row inside a prefab instance). Writing null",
									 instanceRoot.name);
						fileOp.value.handleGuid = Guid{};
					}
				}
				fileOp.value.liveTarget = Handle<Object>();
			}
			for (const RemovedObjectOp& op : live.removedObjects) {
				row.overrides.removedObjects.push_back(op);
			}
			for (const RemovedComponentOp& op : live.removedComponents) {
				row.overrides.removedComponents.push_back(op);
			}

			struct SubtreeEmitter {
				SceneDocument* document;
				Scene* scene;
				void Visit(SceneObject& node, u64 parentRowId) {
					if (node.IsPendingDestroy()) {
						return;
					}
					u64& localId = SceneInternalAccess::LocalIdOf(node);
					if (localId == 0) {
						localId = SceneInternalAccess::NextLocalIdOf(*scene)++;
					}
					SceneDocumentRow subtreeRow;
					subtreeRow.id = localId;
					subtreeRow.parent = parentRowId;
					subtreeRow.kind = SceneDocumentRow::Kind::Object;
					subtreeRow.type = SceneObject::GetReflectType()->name;
					if (document->binary) {
						SI::BinaryWriter writer;
						SI::BinaryWriteObject(writer, &node, SceneObject::GetReflectType(), 0);
						subtreeRow.fields.binary = writer.Data();
					} else {
						subtreeRow.fields.json = SI::TextBuildObject(&node, SceneObject::GetReflectType(), document->pool, 0);
					}
					for (const auto& slot : SceneInternalAccess::ComponentsOf(node)) {
						if (slot.ptr->IsPendingDestroy()) {
							continue;
						}
						u64& componentLocalId = SceneInternalAccess::LocalIdOf(*slot.ptr);
						if (componentLocalId == 0) {
							componentLocalId = SceneInternalAccess::NextLocalIdOf(*scene)++;
						}
						subtreeRow.components.push_back(
							BuildComponentRow(*document, *slot.ptr, slot.type, componentLocalId));
					}
					u64 rowId = subtreeRow.id;
					document->rows.push_back(std::move(subtreeRow));
					for (SceneObject* child : SceneInternalAccess::ChildrenOf(node)) {
						Visit(*child, rowId);
					}
				}
			};

			for (const AddedObjectOp& op : live.addedObjects) {
				SceneObject* liveRoot = op.liveRoot.Get();
				if (!liveRoot || liveRoot->IsPendingDestroy()) {
					continue;
				}
				AddedObjectOp fileOp;
				fileOp.parent = op.parent;
				fileOp.insertIndex = op.insertIndex;
				SubtreeEmitter emitter{&document, &scene};
				size_t firstRow = document.rows.size();
				emitter.Visit(*liveRoot, 0);
				if (document.rows.size() > firstRow) {
					fileOp.rootRowId = document.rows[firstRow].id;
					row.overrides.addedObjects.push_back(std::move(fileOp));
				}
			}

			for (const AddedComponentOp& op : live.addedComponents) {
				Component* liveComponent = op.liveComponent.Get();
				if (!liveComponent || liveComponent->IsPendingDestroy()) {
					continue;
				}
				SceneObject* owner = SceneInternalAccess::OwnerOf(*liveComponent);
				const TypeInfo* componentType = nullptr;
				for (const auto& slot : SceneInternalAccess::ComponentsOf(*owner)) {
					if (slot.ptr == liveComponent) {
						componentType = slot.type;
						break;
					}
				}
				if (!componentType) {
					continue;
				}
				u64& componentLocalId = SceneInternalAccess::LocalIdOf(*liveComponent);
				if (componentLocalId == 0) {
					componentLocalId = SceneInternalAccess::NextLocalIdOf(scene)++;
				}
				SceneDocumentComponentRow componentRow =
					BuildComponentRow(document, *liveComponent, componentType, componentLocalId);
				AddedComponentOp fileOp;
				fileOp.owner = op.owner;
				fileOp.componentRowId = componentRow.id;
				document.looseComponents.push_back(std::move(componentRow));
				row.overrides.addedComponents.push_back(std::move(fileOp));
			}

			document.rows.push_back(std::move(row));
		}

	} // namespace SceneInternal

	Handle<SceneObject> SceneSystem::Instantiate(const Guid& prefabId, Handle<Scene> sceneHandle,
												 Handle<SceneObject> parentHandle, const Vec3& position, const Quat& rotation) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SLT_CORE_ASSERT_MSG(IsStructuralMutationLegal(), "Instantiate is illegal inside lifecycle hooks");
		SystemState& state = GetState();
		if (!state.initialized || !IsStructuralMutationLegal() || !prefabId.IsValid()) {
			return Handle<SceneObject>();
		}

		SceneObject* parent = parentHandle.Get();
		if (parent && parent->IsPendingDestroy()) {
			return Handle<SceneObject>();
		}
		Scene* scene = parent ? SceneInternalAccess::SceneOf(*parent) : sceneHandle.Get();
		if (!scene) {
			scene = state.primary.Get();
		}
		if (!scene || scene->GetState() != SceneState::Loaded) {
			SLT_LOG_WARN("Scene", "Instantiate: no loaded target scene");
			return Handle<SceneObject>();
		}

		ComposeContext ctx;
		ctx.scene = scene;
		std::vector<Guid> chainGuids;
		PrefabAsset* leafAsset = nullptr;
		SceneObject* root = ComposeChain(ctx, prefabId, nullptr, nullptr, Guid{}, false,
										 parent, -1, 0, {}, {}, &chainGuids, &leafAsset);
		if (!root) {
			return Handle<SceneObject>();
		}
		CreateRecord(ctx, *scene, *root, prefabId, std::move(chainGuids), leafAsset, 0);

		if (position != Vec3::Zero() || rotation != Quat::Identity()) {
			root->SetLocalPosition(position);
			root->SetLocalRotation(rotation);
		}

		DeriveAndDispatch(ctx, *root, true);
		return Handle<SceneObject>(root);
	}

	Handle<SceneObject> SceneSystem::InstantiateDetached(const Guid& prefabId) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SLT_CORE_ASSERT_MSG(IsStructuralMutationLegal(), "InstantiateDetached is illegal inside lifecycle hooks");
		SystemState& state = GetState();
		if (!state.initialized || !IsStructuralMutationLegal() || !prefabId.IsValid()) {
			return Handle<SceneObject>();
		}

		ComposeContext ctx;
		ctx.detached = true;
		SceneObject* root = ComposeChain(ctx, prefabId, nullptr, nullptr, Guid{}, false,
										 nullptr, -1, 0, {}, {}, nullptr, nullptr);
		if (!root) {
			return Handle<SceneObject>();
		}
		DeriveAndDispatch(ctx, *root, true);
		return Handle<SceneObject>(root);
	}

	void SceneSystem::DestroyDetached(Handle<SceneObject> rootHandle) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SceneObject* root = rootHandle.Get();
		if (!root) {
			return;
		}
		if (SceneInternalAccess::SceneOf(*root) != nullptr || SceneInternalAccess::ParentOf(*root) != nullptr) {
			SLT_LOG_WARN("Scene", "DestroyDetached on an object that belongs to a scene. Use Destroy");
			return;
		}
		FinalizeSubtree(*root, false);
	}

	Handle<SceneObject> SceneSystem::Instantiate(Handle<SceneObject> sourceHandle, Handle<SceneObject> parentHandle) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SLT_CORE_ASSERT_MSG(IsStructuralMutationLegal(), "Instantiate is illegal inside lifecycle hooks");
		SystemState& state = GetState();
		SceneObject* source = sourceHandle.Get();
		if (!state.initialized || !IsStructuralMutationLegal() || !source || source->IsPendingDestroy()) {
			return Handle<SceneObject>();
		}
		Scene* sourceScene = SceneInternalAccess::SceneOf(*source);
		SceneObject* parent = parentHandle.Get();
		Scene* targetScene = parent ? SceneInternalAccess::SceneOf(*parent) : sourceScene;
		if (!targetScene) {
			return Handle<SceneObject>();
		}

		if (sourceScene && SceneInternalAccess::PrefabRecordOf(*source) != 0) {
			PrefabInstanceRecord* record = FindRecord(*sourceScene, *source);
			if (record) {
				RecordInstanceOverridesInternal(*record);

				ComposeContext ctx;
				ctx.scene = targetScene;
				std::vector<Guid> chainGuids;
				PrefabAsset* leafAsset = nullptr;
				SceneObject* root = ComposeChain(ctx, record->prefabGuid, &record->liveOverrides, nullptr,
												 Guid{}, false, parent, -1, 0, {}, {}, &chainGuids, &leafAsset);
				if (!root) {
					return Handle<SceneObject>();
				}
				for (const AddedObjectOp& op : record->liveOverrides.addedObjects) {
					SceneObject* addedRoot = op.liveRoot.Get();
					if (!addedRoot) {
						continue;
					}
					u64 relKey = FoldPath(0, op.parent);
					auto corrIt = ctx.correspondence.find(relKey);
					if (corrIt == ctx.correspondence.end()) {
						continue;
					}
					Handle<SceneObject> cloned = Instantiate(Handle<SceneObject>(addedRoot),
															 Handle<SceneObject>(static_cast<SceneObject*>(corrIt->second)));
					if (SceneObject* clonedRoot = cloned.Get()) {
						clonedRoot->SetSiblingIndex(op.insertIndex);
					}
				}
				CreateRecord(ctx, *targetScene, *root, record->prefabGuid, std::move(chainGuids), leafAsset, 0);
				DeriveAndDispatch(ctx, *root, true);
				return Handle<SceneObject>(root);
			}
		}

		std::unordered_map<u64, u64> savedIds;
		struct IdSnapshotter {
			std::unordered_map<u64, u64>* saved;
			void Visit(SceneObject& node) {
				(*saved)[node.GetInstanceId()] = node.GetLocalId();
				for (const auto& slot : SceneInternalAccess::ComponentsOf(node)) {
					(*saved)[slot.ptr->GetInstanceId()] = slot.ptr->GetLocalId();
				}
				for (SceneObject* child : SceneInternalAccess::ChildrenOf(node)) {
					Visit(*child);
				}
			}
		};
		IdSnapshotter snapshotter{&savedIds};
		snapshotter.Visit(*source);

		SceneDocument* document = BuildDocumentFromSubtree(*source, false);

		struct IdRestorer {
			const std::unordered_map<u64, u64>* saved;
			void Visit(SceneObject& node) {
				SceneInternalAccess::LocalIdOf(node) = saved->at(node.GetInstanceId());
				for (const auto& slot : SceneInternalAccess::ComponentsOf(node)) {
					SceneInternalAccess::LocalIdOf(*slot.ptr) = saved->at(slot.ptr->GetInstanceId());
				}
				for (SceneObject* child : SceneInternalAccess::ChildrenOf(node)) {
					Visit(*child);
				}
			}
		};
		IdRestorer restorer{&savedIds};
		restorer.Visit(*source);

		struct IdScrubber {
			static void Visit(SceneObject& node) {
				SceneInternalAccess::LocalIdOf(node) = 0;
				for (const auto& slot : SceneInternalAccess::ComponentsOf(node)) {
					SceneInternalAccess::LocalIdOf(*slot.ptr) = 0;
				}
				for (SceneObject* child : SceneInternalAccess::ChildrenOf(node)) {
					Visit(*child);
				}
			}
		};

		document->isPrefab = false;
		Scene* scene = targetScene;
		u32 rootCountBefore = static_cast<u32>(SceneInternalAccess::RootsOf(*scene).size());
		InstantiateDocument(*document, *scene);
		Delete(document);
		Vector<SceneObject*>& roots = SceneInternalAccess::RootsOf(*scene);
		if (roots.size() <= rootCountBefore) {
			return Handle<SceneObject>();
		}
		SceneObject* clone = roots[rootCountBefore];
		IdScrubber::Visit(*clone);
		if (parent) {
			roots.erase(roots.begin() + rootCountBefore);
			SceneInternalAccess::ParentOf(*clone) = parent;
			SceneInternalAccess::ChildrenOf(*parent).push_back(clone);
		}
		DeriveActiveSubtree(*clone);
		DispatchIslandAttach(*clone);
		return Handle<SceneObject>(clone);
	}

	static bool PathHoldsAModel(const Path& path, const char* op) {
		Guid existing = AssetDatabase::LookupGuid(path);
		if (!existing.IsValid()) {
			return false;
		}
		Asset* occupant = AssetDatabase::LoadAssetByGuid(existing);
		bool isModel = occupant && TypeRegistry::IsDerivedFrom(occupant->GetType(), PrefabAsset::GetReflectType()) &&
					   static_cast<PrefabAsset*>(occupant)->GetKind() == PrefabKind::Model;
		if (occupant) {
			AssetDatabase::UnloadAsset(occupant);
		}
		if (isModel) {
			SLT_LOG_WARN("Scene", "{}: '{}' is a model file (its prefab is regenerated on import)", op, path.ToString());
		}
		return isModel;
	}

	// takes ownership of document
	static Guid WritePrefabDocument(SceneDocument* document, const Path& path) {
		PrefabAsset* wrapper = New<PrefabAsset>(MemTag::Scene);
		wrapper->virtualPath = path.ToString();
		wrapper->assetId = AssetDatabase::LookupGuid(path);
		SceneInternalAccess::FragmentOf(*wrapper) = document;
		AssetDatabase::SaveAsset(wrapper);
		Guid guid = wrapper->assetId;
		Delete(wrapper);
		return guid;
	}

	bool SceneSystem::IsPrefabLinked(Handle<SceneObject> object) {
		SceneObject* node = object.Get();
		if (!node) {
			return false;
		}
		return SceneInternalAccess::PrefabRecordOf(*node) != 0 || SceneInternalAccess::PrefabKeyOf(*node) != 0;
	}

	bool SceneSystem::IsPrefabInstanceRoot(Handle<SceneObject> object) {
		SceneObject* node = object.Get();
		return node && SceneInternalAccess::PrefabRecordOf(*node) != 0;
	}

	Guid SceneSystem::SavePrefab(Handle<SceneObject> rootHandle, const Path& path) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SLT_CORE_ASSERT_MSG(IsStructuralMutationLegal(), "SavePrefab is illegal inside lifecycle hooks");
		SystemState& state = GetState();
		SceneObject* root = rootHandle.Get();
		if (!state.initialized || !IsStructuralMutationLegal() || !root || root->IsPendingDestroy()) {
			return Guid{};
		}
		Scene* scene = SceneInternalAccess::SceneOf(*root);
		if (!scene) {
			return Guid{};
		}
		if (IsPrefabLinked(rootHandle)) {
			SLT_LOG_WARN("Scene", "SavePrefab: '{}' is already prefab-linked", root->name);
			return Guid{};
		}
		if (PathHoldsAModel(path, "SavePrefab")) {
			return Guid{};
		}

		Guid guid = WritePrefabDocument(BuildDocumentFromSubtree(*root, false), path);

		PrefabAsset* leafAsset = static_cast<PrefabAsset*>(AssetDatabase::LoadAssetByGuid(guid));
		if (!leafAsset) {
			SLT_LOG_ERROR("Scene", "SavePrefab: reload of '{}' failed", path.ToString());
			return Guid{};
		}

		ComposeContext ctx;
		ctx.scene = scene;
		struct KeyStamper {
			ComposeContext* ctx;
			Guid file;
			void Visit(SceneObject& node) {
				if (node.IsPendingDestroy()) {
					return;
				}
				u64 key = FoldRef(0, file, node.GetLocalId());
				SceneInternalAccess::PrefabKeyOf(node) = key;
				ctx->correspondence[key] = &node;
				ctx->keyTypes[key] = SceneObject::GetReflectType()->name.GetValue();
				ctx->keyTypeInfos[key] = SceneObject::GetReflectType();
				ctx->idToKey[node.GetInstanceId()] = key;
				PrefabTargetPath path;
				path.leaf = PrefabFileRef{file, node.GetLocalId()};
				ctx->keyPaths[key] = std::move(path);
				for (const auto& slot : SceneInternalAccess::ComponentsOf(node)) {
					if (slot.ptr->IsPendingDestroy()) {
						continue;
					}
					u64 componentKey = FoldRef(0, file, slot.ptr->GetLocalId());
					SceneInternalAccess::PrefabKeyOf(*slot.ptr) = componentKey;
					ctx->correspondence[componentKey] = slot.ptr;
					ctx->keyTypes[componentKey] = slot.type->name.GetValue();
					ctx->keyTypeInfos[componentKey] = slot.type;
					ctx->idToKey[slot.ptr->GetInstanceId()] = componentKey;
					PrefabTargetPath componentPath;
					componentPath.leaf = PrefabFileRef{file, slot.ptr->GetLocalId()};
					ctx->keyPaths[componentKey] = std::move(componentPath);
				}
				for (SceneObject* child : SceneInternalAccess::ChildrenOf(node)) {
					Visit(*child);
				}
			}
		};
		KeyStamper stamper{&ctx, guid};
		stamper.Visit(*root);

		std::vector<Guid> chainGuids{guid};
		CreateRecord(ctx, *scene, *root, guid, std::move(chainGuids), leafAsset, 0);
		return guid;
	}

	static bool MergeRecordIntoLeaf(PrefabInstanceRecord* record, SceneDocument* leafDoc) {
		PrefabAsset* leaf = record->asset;

		if (leaf && leaf->GetKind() == PrefabKind::Model) {
			SLT_LOG_WARN("Scene", "'{}' is a model prefab, rebuilt from its source file on every import. Override on the instance (or make a Prefab Variant)", leaf->virtualPath);
			return false;
		}

		OverrideLayer applied = record->liveOverrides;

		std::unordered_map<u64, u64> liveIdToKey;
		for (const auto& [key, object] : record->correspondence) {
			liveIdToKey[object->GetInstanceId()] = key;
		}

		OverrideLayer& target = leafDoc->prefabOverrides;
		leafDoc->isPrefab = true;

		for (const PropertyOverrideOp& op : applied.properties) {
			auto sameOp = [&](const PropertyOverrideOp& existing) {
				if (!(existing.target.leaf == op.target.leaf) || existing.target.hops.size() != op.target.hops.size() ||
					existing.fieldPath.size() != op.fieldPath.size()) {
					return false;
				}
				for (size_t i = 0; i < existing.target.hops.size(); ++i) {
					if (!(existing.target.hops[i] == op.target.hops[i])) {
						return false;
					}
				}
				for (size_t i = 0; i < existing.fieldPath.size(); ++i) {
					if (existing.fieldPath[i] != op.fieldPath[i]) {
						return false;
					}
				}
				return true;
			};
			auto it = std::find_if(target.properties.begin(), target.properties.end(), sameOp);
			size_t slot = static_cast<size_t>(it - target.properties.begin());
			if (it != target.properties.end()) {
				*it = op;
			} else {
				target.properties.push_back(op);
			}
			PropertyOverrideOp& promoted = target.properties[slot];
			if (promoted.value.liveTarget.GetInstanceId() != 0) {
				SLT_LOG_WARN("Scene", "an override pointing at a scene object can't be applied to a prefab (field written null in '{}')",
							 leaf ? leaf->virtualPath : str("<prefab>"));
				promoted.value.liveTarget = Handle<Object>();
				promoted.value.handleGuid = Guid{};
			}
		}
		for (const RemovedObjectOp& op : applied.removedObjects) {
			target.removedObjects.push_back(op);
		}
		for (const RemovedComponentOp& op : applied.removedComponents) {
			target.removedComponents.push_back(op);
		}

		for (const AddedObjectOp& op : applied.addedObjects) {
			SceneObject* liveRoot = op.liveRoot.Get();
			if (!liveRoot || liveRoot->IsPendingDestroy()) {
				continue;
			}
			struct LeafEmitter {
				SceneDocument* document;
				Guid file;
				PrefabInstanceRecord* record;
				void Visit(SceneObject& node, u64 parentRowId) {
					if (node.IsPendingDestroy()) {
						return;
					}
					u64 rowId = document->nextLocalId++;
					SceneInternalAccess::LocalIdOf(node) = rowId;
					u64 key = FoldRef(0, file, rowId);
					SceneInternalAccess::PrefabKeyOf(node) = key;
					record->correspondence[key] = &node;
					record->keyTypes[key] = SceneObject::GetReflectType()->name.GetValue();
					PrefabTargetPath keyPath;
					keyPath.leaf = PrefabFileRef{file, rowId};
					record->keyPaths[key] = std::move(keyPath);

					SceneDocumentRow row;
					row.id = rowId;
					row.parent = parentRowId;
					row.kind = SceneDocumentRow::Kind::Object;
					row.type = SceneObject::GetReflectType()->name;
					if (document->binary) {
						SI::BinaryWriter writer;
						SI::BinaryWriteObject(writer, &node, SceneObject::GetReflectType(), 0);
						row.fields.binary = writer.Data();
					} else {
						row.fields.json = SI::TextBuildObject(&node, SceneObject::GetReflectType(), document->pool, 0);
					}
					for (const auto& slot : SceneInternalAccess::ComponentsOf(node)) {
						if (slot.ptr->IsPendingDestroy()) {
							continue;
						}
						u64 componentRowId = document->nextLocalId++;
						SceneInternalAccess::LocalIdOf(*slot.ptr) = componentRowId;
						u64 componentKey = FoldRef(0, file, componentRowId);
						SceneInternalAccess::PrefabKeyOf(*slot.ptr) = componentKey;
						record->correspondence[componentKey] = slot.ptr;
						record->keyTypes[componentKey] = slot.type->name.GetValue();
						PrefabTargetPath componentPath;
						componentPath.leaf = PrefabFileRef{file, componentRowId};
						record->keyPaths[componentKey] = std::move(componentPath);

						SceneDocumentComponentRow componentRow;
						componentRow.id = componentRowId;
						componentRow.type = slot.type->name;
						if (document->binary) {
							SI::BinaryWriter writer;
							SI::BinaryWriteObject(writer, slot.ptr, slot.type, 0);
							componentRow.fields.binary = writer.Data();
						} else {
							componentRow.fields.json = SI::TextBuildObject(slot.ptr, slot.type, document->pool, 0);
						}
						row.components.push_back(std::move(componentRow));
					}
					u64 emittedId = row.id;
					document->rows.push_back(std::move(row));
					for (SceneObject* child : SceneInternalAccess::ChildrenOf(node)) {
						Visit(*child, emittedId);
					}
				}
			};
			LeafEmitter emitter{leafDoc, leaf->assetId, record};
			size_t firstRow = leafDoc->rows.size();
			emitter.Visit(*liveRoot, 0);
			if (leafDoc->rows.size() > firstRow) {
				AddedObjectOp fileOp;
				fileOp.parent = op.parent;
				fileOp.insertIndex = op.insertIndex;
				fileOp.rootRowId = leafDoc->rows[firstRow].id;
				target.addedObjects.push_back(std::move(fileOp));
			}
		}
		for (const AddedComponentOp& op : applied.addedComponents) {
			Component* liveComponent = op.liveComponent.Get();
			if (!liveComponent || liveComponent->IsPendingDestroy()) {
				continue;
			}
			SceneObject* owner = SceneInternalAccess::OwnerOf(*liveComponent);
			const TypeInfo* componentType = nullptr;
			for (const auto& slot : SceneInternalAccess::ComponentsOf(*owner)) {
				if (slot.ptr == liveComponent) {
					componentType = slot.type;
					break;
				}
			}
			if (!componentType) {
				continue;
			}
			u64 rowId = leafDoc->nextLocalId++;
			SceneInternalAccess::LocalIdOf(*liveComponent) = rowId;
			u64 key = FoldRef(0, leaf->assetId, rowId);
			SceneInternalAccess::PrefabKeyOf(*liveComponent) = key;
			record->correspondence[key] = liveComponent;
			record->keyTypes[key] = componentType->name.GetValue();
			PrefabTargetPath keyPath;
			keyPath.leaf = PrefabFileRef{leaf->assetId, rowId};
			record->keyPaths[key] = std::move(keyPath);

			SceneDocumentComponentRow componentRow;
			componentRow.id = rowId;
			componentRow.type = componentType->name;
			if (leafDoc->binary) {
				SI::BinaryWriter writer;
				SI::BinaryWriteObject(writer, liveComponent, componentType, 0);
				componentRow.fields.binary = writer.Data();
			} else {
				componentRow.fields.json = SI::TextBuildObject(liveComponent, componentType, leafDoc->pool, 0);
			}
			AddedComponentOp fileOp;
			fileOp.owner = op.owner;
			fileOp.componentRowId = rowId;
			leafDoc->looseComponents.push_back(std::move(componentRow));
			target.addedComponents.push_back(std::move(fileOp));
		}

		AssetDatabase::SaveAsset(leaf);
		SceneSystem::NotifyPrefabChanged(leaf->assetId);
		return true;
	}

	bool SceneSystem::ApplyInstanceOverridesToBase(Handle<SceneObject> instanceRootHandle) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SLT_CORE_ASSERT_MSG(IsStructuralMutationLegal(), "ApplyInstanceOverridesToBase is illegal inside lifecycle hooks");
		SystemState& state = GetState();
		SceneObject* instanceRoot = instanceRootHandle.Get();
		if (!state.initialized || !IsStructuralMutationLegal() || !instanceRoot) {
			return false;
		}
		Scene* scene = SceneInternalAccess::SceneOf(*instanceRoot);
		PrefabInstanceRecord* record = scene ? FindRecord(*scene, *instanceRoot) : nullptr;
		if (!record || !record->asset || !record->asset->_fragment) {
			return false;
		}

		for (const Handle<Scene>& sceneHandle : state.allScenes) {
			if (Scene* loadedScene = sceneHandle.Get()) {
				RefreshInstanceOverrides(*loadedScene);
			}
		}

		if (!MergeRecordIntoLeaf(record, record->asset->_fragment)) {
			return false;
		}
		RecordInstanceOverridesInternal(*record);
		return true;
	}

	bool SceneSystem::ApplyDetachedToPrefab(Handle<SceneObject> rootHandle, const Guid& prefabId) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SLT_CORE_ASSERT_MSG(IsStructuralMutationLegal(), "ApplyDetachedToPrefab is illegal inside lifecycle hooks");
		SystemState& state = GetState();
		SceneObject* root = rootHandle.Get();
		if (!state.initialized || !IsStructuralMutationLegal() || !root || !prefabId.IsValid()) {
			return false;
		}
		if (SceneInternalAccess::SceneOf(*root) != nullptr || SceneInternalAccess::ParentOf(*root) != nullptr) {
			SLT_LOG_WARN("Scene", "ApplyDetachedToPrefab on an object that belongs to a scene. Use ApplyInstanceOverridesToBase");
			return false;
		}

		Asset* asset = AssetDatabase::LoadAssetByGuid(prefabId);
		PrefabAsset* leaf = asset && TypeRegistry::IsDerivedFrom(asset->GetType(), PrefabAsset::GetReflectType())
								? static_cast<PrefabAsset*>(asset)
								: nullptr;
		if (!leaf || !leaf->_fragment) {
			if (asset) {
				AssetDatabase::UnloadAsset(asset);
			}
			return false;
		}

		for (const Handle<Scene>& sceneHandle : state.allScenes) {
			if (Scene* loadedScene = sceneHandle.Get()) {
				RefreshInstanceOverrides(*loadedScene);
			}
		}

		PrefabInstanceRecord record;
		record.prefabGuid = prefabId;
		record.root = rootHandle;
		record.asset = leaf;
		CollectGraphCorrespondence(*root, record);

		if (!RecordInstanceOverridesInternal(record)) {
			AssetDatabase::UnloadAsset(leaf);
			return false;
		}
		bool merged = MergeRecordIntoLeaf(&record, leaf->_fragment);
		if (merged) {
			RecordInstanceOverridesInternal(record);
		}
		AssetDatabase::UnloadAsset(leaf);
		return merged;
	}

	Guid SceneSystem::CreatePrefabVariant(const Guid& basePrefab, const Path& path) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		if (!GetState().initialized || !basePrefab.IsValid() || path.ToString().empty()) {
			return Guid{};
		}
		std::vector<PrefabAsset*> chain;
		if (!WalkChain(basePrefab, chain)) {
			SLT_LOG_WARN("Scene", "CreatePrefabVariant: {} isn't a loadable prefab", basePrefab);
			return Guid{};
		}
		Guid existing = AssetDatabase::LookupGuid(path);
		bool closesCycle = false;
		for (PrefabAsset* prefab : chain) {
			closesCycle = closesCycle || (existing.IsValid() && prefab->assetId == existing);
			AssetDatabase::UnloadAsset(prefab);
		}
		if (closesCycle) {
			SLT_LOG_WARN("Scene", "CreatePrefabVariant: '{}' is already in the base's chain (a variant there would inherit from itself)", path.ToString());
			return Guid{};
		}
		if (PathHoldsAModel(path, "CreatePrefabVariant")) {
			return Guid{};
		}
		return WritePrefabDocument(BuildVariantDocument(basePrefab, path), path);
	}

	Guid SceneSystem::SavePrefabVariant(Handle<SceneObject> rootHandle, const Path& path) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SLT_CORE_ASSERT_MSG(IsStructuralMutationLegal(), "SavePrefabVariant is illegal inside lifecycle hooks");
		SystemState& state = GetState();
		SceneObject* root = rootHandle.Get();
		if (!state.initialized || !IsStructuralMutationLegal() || !root || root->IsPendingDestroy() || path.ToString().empty()) {
			return Guid{};
		}
		Scene* scene = SceneInternalAccess::SceneOf(*root);
		PrefabInstanceRecord* record = scene ? FindRecord(*scene, *root) : nullptr;
		if (!record || !record->asset || !record->asset->_fragment) {
			SLT_LOG_WARN("Scene", "SavePrefabVariant: '{}' isn't a prefab instance root", root->name);
			return Guid{};
		}
		if (Guid existing = AssetDatabase::LookupGuid(path); existing.IsValid()) {
			for (const Guid& chainGuid : record->chainGuids) {
				if (chainGuid == existing) {
					SLT_LOG_WARN("Scene", "SavePrefabVariant: '{}' is already in the instance's chain (a variant there would inherit from itself)", path.ToString());
					return Guid{};
				}
			}
		}
		if (PathHoldsAModel(path, "SavePrefabVariant")) {
			return Guid{};
		}

		// captured before the record is repointed
		if (!RecordInstanceOverridesInternal(*record)) {
			return Guid{};
		}

		Guid guid = WritePrefabDocument(BuildVariantDocument(record->prefabGuid, path), path);
		Asset* loaded = guid.IsValid() ? AssetDatabase::LoadAssetByGuid(guid) : nullptr;
		PrefabAsset* variant = loaded && TypeRegistry::IsDerivedFrom(loaded->GetType(), PrefabAsset::GetReflectType())
								   ? static_cast<PrefabAsset*>(loaded)
								   : nullptr;
		if (!variant || !variant->_fragment) {
			SLT_LOG_ERROR("Scene", "SavePrefabVariant: reload of '{}' failed", path.ToString());
			if (loaded) {
				AssetDatabase::UnloadAsset(loaded);
			}
			return Guid{};
		}

		AssetDatabase::UnloadAsset(record->asset);
		record->asset = variant;
		record->prefabGuid = guid;
		record->chainGuids.push_back(guid);

		if (!MergeRecordIntoLeaf(record, variant->_fragment)) {
			SLT_LOG_ERROR("Scene", "SavePrefabVariant: '{}' was written but the instance's overrides didn't merge into it", path.ToString());
			return guid;
		}
		RecordInstanceOverridesInternal(*record);
		return guid;
	}

	bool SceneSystem::RevertInstanceOverrides(Handle<SceneObject> instanceRootHandle) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SLT_CORE_ASSERT_MSG(IsStructuralMutationLegal(), "RevertInstanceOverrides is illegal inside lifecycle hooks");
		SceneObject* instanceRoot = instanceRootHandle.Get();
		if (!GetState().initialized || !IsStructuralMutationLegal() || !instanceRoot) {
			return false;
		}
		Scene* scene = SceneInternalAccess::SceneOf(*instanceRoot);
		PrefabInstanceRecord* record = scene ? FindRecord(*scene, *instanceRoot) : nullptr;
		if (!record) {
			return false;
		}
		record->liveOverrides = OverrideLayer{};
		RemergeRecord(*scene, SceneInternalAccess::PrefabRecordOf(*instanceRoot));
		return true;
	}

	bool SceneSystem::DiffPrefabInstance(Handle<SceneObject> rootHandle, PrefabInstanceDiff& out, const Guid& against) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		out = PrefabInstanceDiff{};
		SceneObject* root = rootHandle.Get();
		if (!GetState().initialized || !root || root->IsPendingDestroy()) {
			return false;
		}
		Scene* scene = SceneInternalAccess::SceneOf(*root);
		if (PrefabInstanceRecord* record = scene ? FindRecord(*scene, *root) : nullptr) {
			Guid staging = against.IsValid() ? against : record->prefabGuid;
			if (against.IsValid() && against != record->prefabGuid) {
				bool inChain = false;
				for (const Guid& chainGuid : record->chainGuids) {
					inChain = inChain || chainGuid == against;
				}
				if (!inChain) {
					SLT_LOG_WARN("Scene", "DiffPrefabInstance: {} isn't in the instance's prefab chain", against);
					return false;
				}
			}
			OverrideLayer layer;
			return DiffInstanceInternal(*record, staging, layer, &out);
		}
		if (scene || SceneInternalAccess::ParentOf(*root) != nullptr || !against.IsValid()) {
			return false;
		}
		PrefabInstanceRecord record;
		record.prefabGuid = against;
		record.root = rootHandle;
		CollectGraphCorrespondence(*root, record);
		OverrideLayer layer;
		return DiffInstanceInternal(record, against, layer, &out);
	}

	bool SceneSystem::RevertInstanceOverrides(Handle<SceneObject> instanceRootHandle, std::span<const PrefabTargetPath> rows) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SLT_CORE_ASSERT_MSG(IsStructuralMutationLegal(), "RevertInstanceOverrides is illegal inside lifecycle hooks");
		SceneObject* instanceRoot = instanceRootHandle.Get();
		if (!GetState().initialized || !IsStructuralMutationLegal() || !instanceRoot) {
			return false;
		}
		Scene* scene = SceneInternalAccess::SceneOf(*instanceRoot);
		PrefabInstanceRecord* record = scene ? FindRecord(*scene, *instanceRoot) : nullptr;
		if (!record) {
			return false;
		}
		if (!RecordInstanceOverridesInternal(*record)) {
			return false;
		}
		auto named = [rows](const PrefabTargetPath& path) {
			for (const PrefabTargetPath& row : rows) {
				if (SamePath(row, path)) {
					return true;
				}
			}
			return false;
		};
		OverrideLayer& live = record->liveOverrides;
		std::erase_if(live.properties, [&named](const PropertyOverrideOp& op) { return named(op.target); });
		std::erase_if(live.removedComponents, [&named](const RemovedComponentOp& op) { return named(op.target); });
		std::erase_if(live.removedObjects, [&named](const RemovedObjectOp& op) { return named(op.target); });
		RemergeRecord(*scene, SceneInternalAccess::PrefabRecordOf(*instanceRoot));
		return true;
	}

	u32 SceneSystem::GetPrefabEpoch() {
		return GetState().prefabEpoch;
	}

	void SceneSystem::NotifyPrefabChanged(const Guid& prefabId) {
		SLT_SCENE_ASSERT_MAIN_THREAD();
		SystemState& state = GetState();
		if (!state.initialized || !prefabId.IsValid()) {
			return;
		}
		++state.prefabEpoch;
		for (const Handle<Scene>& sceneHandle : state.allScenes) {
			Scene* scene = sceneHandle.Get();
			if (!scene) {
				continue;
			}
			Vector<PrefabInstanceRecord*>& records = SceneInternalAccess::InstancesOf(*scene);
			size_t count = records.size();
			for (size_t i = 0; i < count; ++i) {
				PrefabInstanceRecord* record = records[i];
				if (!record) {
					continue;
				}
				bool affected = record->prefabGuid == prefabId;
				for (const Guid& chainGuid : record->chainGuids) {
					affected = affected || chainGuid == prefabId;
				}
				if (affected) {
					RemergeRecord(*scene, static_cast<u32>(i + 1));
				}
			}
		}
	}

} // namespace Shalltear
