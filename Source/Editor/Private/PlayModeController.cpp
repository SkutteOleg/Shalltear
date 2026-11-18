#include "Shalltear/Editor/PlayModeController.h"

#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Scene/Component.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/Time.h"

#include "Shalltear/Editor/EditorEvents.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/EditorSelection.h"

namespace Shalltear {

	bool PlayModeController::BeginFrameTick() {
		if (!_playing) {
			return false;
		}
		if (!_paused) {
			return true;
		}
		if (_stepFrames > 0) {
			--_stepFrames;
			return true;
		}
		return false;
	}

	void PlayModeController::PumpMainThread() {
		if (!SceneSystem::IsInitialized()) {
			_togglePending = false;
			_pauseTogglePending = false;
			_stepPending = false;
			return;
		}

		if (_togglePending) {
			_togglePending = false;
			_pauseTogglePending = false;
			_stepPending = false;
			if (_playing) {
				ExitPlay();
			} else if (Handle<Scene> prefabScene = EditorSceneOps::GetPrefabStage(); prefabScene.IsValid()) {
				SLT_LOG_WARN("Editor", "can't enter play while editing the prefab '{}'. Leave the prefab",
							 prefabScene.Get()->name);
			} else {
				EnterPlay();
			}
			return;
		}

		if (_pauseTogglePending) {
			_pauseTogglePending = false;
			if (_playing) {
				_paused = !_paused;
				_stepFrames = 0;
				SLT_LOG_INFO("Editor", "{}", _paused ? "PAUSED" : "RESUMED");
				PublishState();
			}
		}

		if (_stepPending) {
			_stepPending = false;
			if (_playing) {
				if (!_paused) {
					_paused = true;
					PublishState();
				}
				_stepFrames = 1;
			}
		}
	}

	void PlayModeController::PublishState() {
		PlayModeState state = !_playing ? PlayModeState::Editing
										: (_paused ? PlayModeState::Paused : PlayModeState::Playing);
		EditorEvents::PlayModeChanged.Publish(EventBus::GetGlobal(), {state});
	}

	void PlayModeController::EnterPlay() {
		EditorEvents::PlayStarting.Publish(EventBus::GetGlobal(), {});
		_snapshots.clear();
		_paused = false;
		_stepFrames = 0;

		auto scenes = SceneSystem::GetLoadedScenes();
		for (const Handle<Scene>& scene : scenes) {
			SceneSnapshot snapshot;
			snapshot.wasPrimary = (scene == SceneSystem::GetPrimaryScene());
			snapshot.sceneId = scene.GetInstanceId();
			if (!SceneSystem::SaveSceneToMemory(scene, snapshot.buffer)) {
				SLT_LOG_ERROR("Editor", "Play aborted: scene '{}' failed to snapshot",
							  scene.Get() ? scene.Get()->name : str("<dead>"));
				_snapshots.clear();
				return;
			}
			_snapshots.push_back(std::move(snapshot));
		}

		RebuildWorldFromSnapshots(true);

		Time::ResetFrameClock();

		_playing = true;
		SLT_LOG_INFO("Editor", "PLAY: {} scene(s) snapshotted (Ctrl+P stops)", _snapshots.size());
		PublishState();
	}

	namespace {

		bool IsA(const Object* object, const TypeInfo* base) {
			return object && TypeRegistry::IsDerivedFrom(object->GetType(), base);
		}

		class SceneRowIndex {
		public:
			explicit SceneRowIndex(Scene* scene) {
				if (scene) {
					scene->ForEachObject(&Visit, this);
				}
			}

			bool KeyOf(SceneObject& node, SceneRowKey& out) const {
				auto bucket = _byLocalId.find(node.GetLocalId());
				if (bucket == _byLocalId.end()) {
					return false;
				}
				for (u32 i = 0; i < bucket->second.size(); ++i) {
					if (bucket->second[i] == &node) {
						out = KeyAt(bucket->second, i);
						return true;
					}
				}
				return false;
			}

			SceneObject* Find(const SceneRowKey& key) const {
				auto bucket = _byLocalId.find(key.localId);
				if (bucket == _byLocalId.end()) {
					return nullptr;
				}
				u32 ordinal = 0;
				for (SceneObject* row : bucket->second) {
					if (row->GetPrefabKey() != key.prefabKey) {
						continue;
					}
					if (ordinal == key.ordinal) {
						return row;
					}
					++ordinal;
				}
				return nullptr;
			}

			template <typename Fn>
			void ForEachRow(Fn&& fn) const {
				for (const auto& [localId, rows] : _byLocalId) {
					for (u32 i = 0; i < rows.size(); ++i) {
						fn(*rows[i], KeyAt(rows, i));
					}
				}
			}
		private:
			static SceneRowKey KeyAt(const Vector<SceneObject*>& rows, u32 index) {
				u32 ordinal = 0;
				for (u32 i = 0; i < index; ++i) {
					if (rows[i]->GetPrefabKey() == rows[index]->GetPrefabKey()) {
						++ordinal;
					}
				}
				return {rows[index]->GetLocalId(), rows[index]->GetPrefabKey(), ordinal};
			}

			static void Visit(SceneObject& node, void* user) {
				SceneRowIndex& index = *static_cast<SceneRowIndex*>(user);
				auto found = index._byLocalId.find(node.GetLocalId());
				if (found == index._byLocalId.end()) {
					found = index._byLocalId.emplace(node.GetLocalId(),
													 Vector<SceneObject*>{GetMemoryResource(MemTag::Editor)})
								.first;
				}
				found->second.push_back(&node);
			}

			HashMap<u64, Vector<SceneObject*>> _byLocalId{GetMemoryResource(MemTag::Editor)};
		};

		Component* FindComponent(SceneObject& owner, const SceneRowKey& key) {
			for (u32 i = 0; i < owner.GetComponentCount(); ++i) {
				Component* component = owner.GetComponentAt(i).Get();
				if (component && component->GetLocalId() == key.localId &&
					component->GetPrefabKey() == key.prefabKey) {
					return component;
				}
			}
			return nullptr;
		}

	} // anonymous namespace

	void PlayModeController::CaptureSelection() {
		_selection.clear();
		Vector<SceneRowIndex> indices{GetMemoryResource(MemTag::Editor)};
		for (const SceneSnapshot& snapshot : _snapshots) {
			Object* raw = ObjectRegistry::Resolve(snapshot.sceneId);
			indices.emplace_back(raw ? static_cast<Scene*>(raw) : nullptr);
		}

		for (const Handle<Object>& entry : EditorSelection::GetSelection()) {
			Object* object = entry.Get();
			if (!object) {
				continue;
			}

			SceneObject* owner = nullptr;
			SelectionEntry captured;
			if (IsA(object, SceneObject::GetReflectType())) {
				owner = static_cast<SceneObject*>(object);
			} else if (IsA(object, Component::GetReflectType())) {
				Component* component = static_cast<Component*>(object);
				owner = component->GetOwner().Get();
				captured.isComponent = true;
				captured.component = {component->GetLocalId(), component->GetPrefabKey(), 0};
			} else {
				captured.survivor = entry;
				_selection.push_back(std::move(captured));
				continue;
			}

			if (!owner || owner->GetLocalId() == 0) {
				continue;
			}
			if (captured.isComponent && captured.component.localId == 0) {
				continue;
			}
			Handle<Scene> scene = owner->GetScene();
			for (u32 i = 0; i < _snapshots.size(); ++i) {
				if (_snapshots[i].sceneId != scene.GetInstanceId()) {
					continue;
				}
				if (!indices[i].KeyOf(*owner, captured.object)) {
					break;
				}
				captured.snapshotIndex = i;
				_selection.push_back(std::move(captured));
				break;
			}
		}
	}

	void PlayModeController::CaptureWorldIds() {
		_idCapture.clear();
		for (u32 i = 0; i < _snapshots.size(); ++i) {
			Object* raw = ObjectRegistry::Resolve(_snapshots[i].sceneId);
			if (!raw) {
				continue;
			}
			IdCapture sceneRow;
			sceneRow.snapshotIndex = i;
			sceneRow.isScene = true;
			sceneRow.oldId = _snapshots[i].sceneId;
			_idCapture.push_back(sceneRow);

			SceneRowIndex index{static_cast<Scene*>(raw)};
			index.ForEachRow([this, i](SceneObject& node, const SceneRowKey& key) {
				if (key.localId == 0) {
					return;
				}
				IdCapture row;
				row.snapshotIndex = i;
				row.row = key;
				row.oldId = node.GetInstanceId();
				_idCapture.push_back(row);

				for (u32 c = 0; c < node.GetComponentCount(); ++c) {
					Component* component = node.GetComponentAt(c).Get();
					if (!component || component->GetLocalId() == 0) {
						continue;
					}
					IdCapture componentRow = row;
					componentRow.isComponent = true;
					componentRow.component = {component->GetLocalId(), component->GetPrefabKey(), 0};
					componentRow.oldId = component->GetInstanceId();
					_idCapture.push_back(componentRow);
				}
			});
		}
	}

	void PlayModeController::PublishWorldRemap(const Vector<Handle<Scene>>& restored) {
		if (_idCapture.empty()) {
			return;
		}
		Vector<SceneRowIndex> indices{GetMemoryResource(MemTag::Editor)};
		for (const Handle<Scene>& scene : restored) {
			indices.emplace_back(scene.Get());
		}

		Vector<EditorIdRemap> table{GetMemoryResource(MemTag::Editor)};
		for (const IdCapture& row : _idCapture) {
			if (row.snapshotIndex >= restored.size() || !restored[row.snapshotIndex].IsValid()) {
				continue;
			}
			if (row.isScene) {
				table.push_back({row.oldId, restored[row.snapshotIndex].GetInstanceId()});
				continue;
			}
			SceneObject* found = indices[row.snapshotIndex].Find(row.row);
			if (!found) {
				continue;
			}
			if (!row.isComponent) {
				table.push_back({row.oldId, found->GetInstanceId()});
			} else if (Component* component = FindComponent(*found, row.component)) {
				table.push_back({row.oldId, component->GetInstanceId()});
			}
		}
		_idCapture.clear();
		EditorEvents::WorldRemapped.Publish(EventBus::GetGlobal(), {{table.data(), table.size()}});
	}

	void PlayModeController::RestoreSelection(const Vector<Handle<Scene>>& restored) {
		bool first = true;
		auto reselect = [&first](Handle<Object> target) {
			if (first) {
				EditorSelection::Select(target);
				first = false;
			} else {
				EditorSelection::AddToSelection(target);
			}
		};

		Vector<SceneRowIndex> indices{GetMemoryResource(MemTag::Editor)};
		for (const Handle<Scene>& scene : restored) {
			indices.emplace_back(scene.Get());
		}

		for (const SelectionEntry& entry : _selection) {
			if (entry.survivor.IsValid()) {
				reselect(entry.survivor);
				continue;
			}
			if (entry.snapshotIndex >= restored.size()) {
				continue;
			}
			SceneObject* owner = indices[entry.snapshotIndex].Find(entry.object);
			if (!owner) {
				continue;
			}
			if (!entry.isComponent) {
				reselect(Handle<Object>(owner));
				continue;
			}
			if (Component* component = FindComponent(*owner, entry.component)) {
				reselect(Handle<Object>(component));
			}
		}
		_selection.clear();
	}

	void PlayModeController::RebuildWorldFromSnapshots(bool playing) {
		CaptureSelection();
		CaptureWorldIds();

		SceneSystem::CancelAsyncLoads();
		SceneSystem::UnloadAllScenes();
		// outgoing world finalizes before the incoming one instantiates
		SceneSystem::EndFrame();
		SceneSystem::SetPlaying(playing);

		Handle<Scene> primary;
		Vector<Handle<Scene>> restoredScenes{GetMemoryResource(MemTag::Editor)};
		for (SceneSnapshot& snapshot : _snapshots) {
			Handle<Scene> restored = SceneSystem::LoadSceneFromMemory(snapshot.buffer);
			restoredScenes.push_back(restored);
			if (!restored.IsValid()) {
				SLT_LOG_ERROR("Editor", "{}: a scene snapshot failed to restore", playing ? "Play" : "Stop");
				continue;
			}
			snapshot.sceneId = restored.GetInstanceId();
			if (snapshot.wasPrimary) {
				primary = restored;
			}
		}
		if (primary.IsValid()) {
			SceneSystem::SetPrimaryScene(primary);
		}
		PublishWorldRemap(restoredScenes);
		RestoreSelection(restoredScenes);
	}

	void PlayModeController::ExitPlay() {
		RebuildWorldFromSnapshots(false);

		_snapshots.clear();
		_playing = false;
		_paused = false;
		_stepFrames = 0;
		SLT_LOG_INFO("Editor", "STOP, edit state restored");
		PublishState();
	}

} // namespace Shalltear
