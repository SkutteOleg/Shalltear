#include "Shalltear/Editor/EditorSceneOps.h"

#include "EditorSceneBuild.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/ProjectLayout.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Reflection/Walk.h"
#include "Shalltear/Core/Graphics/Camera.h"
#include "Shalltear/Core/Graphics/Environment.h"
#include "Shalltear/Core/Graphics/Light.h"
#include "Shalltear/Core/Graphics/MeshAsset.h"
#include "Shalltear/Core/Graphics/MeshRenderer.h"
#include "Shalltear/Core/Scene/Component.h"
#include "Shalltear/Core/Scene/Prefab.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/Scene/UnresolvedComponent.h"
#include "Shalltear/Core/Serialization/TextSerializer.h"

#include "Shalltear/Editor/AuthoringDefaults.h"
#include "Shalltear/Editor/BuildProfile.h"
#include "Shalltear/Editor/EditorCommands.h"
#include "Shalltear/Editor/EditorEvents.h"
#include "Shalltear/Editor/EditorSelection.h"
#include "Shalltear/Editor/SceneAnnotations.h"
#include "Shalltear/Editor/UndoSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace Shalltear {

	namespace {

		constexpr size_t RECENT_CAP = 8;

		bool IsModelPrefab(const Guid& assetId) {
			Asset* asset = assetId.IsValid() ? AssetDatabase::LoadAssetByGuid(assetId) : nullptr;
			if (!asset) {
				return false;
			}
			bool isModel = TypeRegistry::IsDerivedFrom(asset->GetType(), PrefabAsset::GetReflectType()) &&
						   static_cast<PrefabAsset*>(asset)->GetKind() == PrefabKind::Model;
			AssetDatabase::UnloadAsset(asset);
			return isModel;
		}

		class SceneOpsState : public Object {
		public:
			HashSet<u64> dirty{GetMemoryResource(MemTag::Editor)};
			HashMap<u64, str> paths{GetMemoryResource(MemTag::Editor)};
			HashMap<u64, Guid> prefabTargets{GetMemoryResource(MemTag::Editor)};
			Vector<str> recents{GetMemoryResource(MemTag::Editor)};
			Handle<Scene> stage;
			Handle<Scene> stagePriorPrimary;
			Guid previewPrefab{};
			Handle<SceneObject> previewRoot;
			bool previewEdited = false;
			u32 previewEpoch = 0;

			PlayModeState lastPlayState = PlayModeState::Editing;

			void OnPlayModeChanged(const EditorPlayModeChangedEvent& event) {
				bool worldSwapped = (lastPlayState == PlayModeState::Editing) != (event.state == PlayModeState::Editing);
				lastPlayState = event.state;
				if (worldSwapped) {
					EditorSceneOps::ReleasePrefabPreview();
				}
			}

			void OnModuleReloadBegan(const EditorModuleReloadBeganEvent&) {
				EditorSceneOps::ReleasePrefabPreview();
			}

			void OnWorldRemapped(const EditorWorldRemappedEvent& event) {
				for (const EditorIdRemap& remap : event.ids) {
					if (dirty.erase(remap.oldId) > 0) {
						dirty.insert(remap.newId);
					}
					if (auto it = paths.find(remap.oldId); it != paths.end()) {
						str path = std::move(it->second);
						paths.erase(it);
						paths[remap.newId] = std::move(path);
					}
					if (auto it = prefabTargets.find(remap.oldId); it != prefabTargets.end()) {
						Guid target = it->second;
						prefabTargets.erase(it);
						prefabTargets[remap.newId] = target;
					}
				}
			}
		};

		SceneOpsState* _state = nullptr;

		Handle<Scene> LoadSceneTracked(const Path& virtualOrPhysical, bool makePrimary) {
			Path path = FileSystem::Virtualize(virtualOrPhysical);
			Handle<Scene> scene = SceneSystem::LoadScene(path);
			if (!scene.IsValid()) {
				SLT_LOG_WARN("Editor", "failed to open scene '{}'", path.ToString());
				return {};
			}
			if (makePrimary) {
				SceneSystem::SetPrimaryScene(scene);
			}
			if (_state) {
				_state->paths[scene.GetInstanceId()] = path.ToString();
			}
			EditorSceneOps::AddRecentScene(path.ToString());
			SceneAnnotations::LoadFor(scene);
			return scene;
		}

		bool IsInsideContentRoot(const Path& virtualPath) {
			constexpr std::string_view Mount = "project://";
			std::string_view text = virtualPath.ToString();
			if (!text.starts_with(Mount)) {
				return false;
			}
			std::string_view relative = text.substr(Mount.size());
			for (std::string_view root : ProjectLayout::GetContentRoots()) {
				if (relative.size() > root.size() + 1 && relative.starts_with(root) &&
					relative[root.size()] == '/') {
					return true;
				}
			}
			return false;
		}

		bool RefusedOutsideContentRoot(const Path& virtualPath) {
			if (IsInsideContentRoot(virtualPath)) {
				return false;
			}
			SLT_LOG_ERROR("Editor",
						  "'{}' is outside the project's content roots (Asset Browser can't see it). "
						  "Save the prefab inside one",
						  virtualPath.ToString());
			return true;
		}

		void EnsureParentDirectory(const Path& path) {
			str parent = path.GetDirectory();
			if (!parent.empty()) {
				FileSystem::CreateDirectory(Path(parent));
			}
		}

		void Prune() {
			std::erase_if(_state->dirty, [](u64 id) {
				return ObjectRegistry::Resolve(id) == nullptr;
			});
			std::erase_if(_state->paths, [](const auto& pair) {
				return ObjectRegistry::Resolve(pair.first) == nullptr;
			});
			std::erase_if(_state->prefabTargets, [](const auto& pair) {
				return ObjectRegistry::Resolve(pair.first) == nullptr;
			});
		}

	} // anonymous namespace

	void EditorSceneOps::Initialize() {
		if (!_state) {
			_state = New<SceneOpsState>(MemTag::Editor);
			Handle<SceneOpsState> self(_state);
			EventBus& bus = EventBus::GetGlobal();
			EditorEvents::PlayModeChanged.Subscribe(bus, self, &SceneOpsState::OnPlayModeChanged);
			EditorEvents::ModuleReloadBegan.Subscribe(bus, self, &SceneOpsState::OnModuleReloadBegan);
			EditorEvents::WorldRemapped.Subscribe(bus, self, &SceneOpsState::OnWorldRemapped);
		}
	}

	namespace {

		struct CreateEntry {
			str group;
			str label;
			StringHash component;
			const void* owner = nullptr;
		};

		Vector<CreateEntry>* _createEntries = nullptr;

	} // anonymous namespace

	void EditorSceneOps::Terminate() {
		if (_createEntries) {
			Delete(_createEntries);
			_createEntries = nullptr;
		}
		if (_state) {
			ReleasePrefabPreview();
			EventBus::GetGlobal().UnsubscribeAll(_state->GetInstanceId());
			Delete(_state);
			_state = nullptr;
		}
	}

	void EditorSceneOps::MarkDirty(Handle<Scene> scene) {
		if (!_state || !scene.IsValid()) {
			return;
		}
		if (_state->dirty.insert(scene.GetInstanceId()).second) {
			EditorEvents::SceneDirtied.Publish(EventBus::GetGlobal(), {scene});
		}
	}

	void EditorSceneOps::MarkClean(Handle<Scene> scene) {
		if (_state) {
			_state->dirty.erase(scene.GetInstanceId());
		}
	}

	bool EditorSceneOps::IsDirty(Handle<Scene> scene) {
		return _state && _state->dirty.contains(scene.GetInstanceId());
	}

	bool EditorSceneOps::AnyDirty() {
		if (!_state) {
			return false;
		}
		Prune();
		return !_state->dirty.empty();
	}

	Path EditorSceneOps::GetScenePath(Handle<Scene> scene) {
		if (_state) {
			auto it = _state->paths.find(scene.GetInstanceId());
			if (it != _state->paths.end()) {
				return Path(it->second);
			}
		}
		if (Scene* resolved = scene.Get()) {
			if (!(resolved->GetSourceGuid() == Guid{})) {
				return AssetDatabase::LookupPath(resolved->GetSourceGuid());
			}
		}
		return {};
	}

	void EditorSceneOps::GetSceneSetup(Vector<str>& out) {
		out.clear();
		Handle<Scene> primary = SceneSystem::GetPrimaryScene();
		if (primary.IsValid()) {
			if (str path = GetScenePath(primary).ToString(); !path.empty()) {
				out.push_back(std::move(path));
			}
		}
		for (Handle<Scene> scene : SceneSystem::GetLoadedScenes()) {
			if (scene == primary) {
				continue;
			}
			if (str path = GetScenePath(scene).ToString(); !path.empty()) {
				out.push_back(std::move(path));
			}
		}
	}

	str EditorSceneOps::ChooseStartupScene(const str& projectPath, std::span<const str> remembered) {
		for (const str& path : remembered) {
			if (!path.empty() && FileSystem::Exists(Path(path))) {
				return path;
			}
		}
		Vector<str> scenes{GetMemoryResource(MemTag::Editor)};
		BuildProfileOps::GetProjectScenes(projectPath, scenes);
		return scenes.size() == 1 ? "project://" + scenes[0] : str{};
	}

	void EditorSceneOps::PopulateNewScene(Handle<Scene> scene) {
		if (!scene.IsValid()) {
			return;
		}
		auto authored = [](auto component) {
			if (Component* live = component.Get()) {
				AuthoringDefaults::Write(*live, AuthoringDefaults::StateFor(live->GetType()));
			}
		};
		if (Handle<SceneObject> camera = SceneSystem::CreateObject("Main Camera", scene); camera.IsValid()) {
			authored(camera.Get()->AddComponent<Camera>());
			camera.Get()->SetLocalPosition(Vec3(0.0f, 1.0f, -10.0f));
		}
		if (Handle<SceneObject> light = SceneSystem::CreateObject("Directional Light", scene); light.IsValid()) {
			authored(light.Get()->AddComponent<Light>());
			light.Get()->SetLocalPosition(Vec3(0.0f, 3.0f, 0.0f));
			light.Get()->SetLocalRotation(Quat::FromEuler(50.0f, -30.0f, 0.0f));
		}
		if (Handle<SceneObject> environment = SceneSystem::CreateObject("Environment", scene); environment.IsValid()) {
			authored(environment.Get()->AddComponent<Environment>());
		}
	}

	Handle<Scene> EditorSceneOps::NewScene(std::string_view name) {
		Handle<Scene> scene = SceneSystem::CreateScene(name);
		if (scene.IsValid()) {
			SceneSystem::SetPrimaryScene(scene);
			PopulateNewScene(scene);
		}
		return scene;
	}

	Handle<Scene> EditorSceneOps::OpenScene(const Path& path) {
		return LoadSceneTracked(path, true);
	}

	Handle<Scene> EditorSceneOps::LoadSceneAdditive(const Path& path) {
		return LoadSceneTracked(path, false);
	}

	bool EditorSceneOps::SaveScene(Handle<Scene> scene, const Path& virtualOrPhysical) {
		if (!scene.IsValid() || virtualOrPhysical.ToString().empty()) {
			return false;
		}
		Path path = FileSystem::Virtualize(virtualOrPhysical);
		EnsureParentDirectory(path);
		if (!SceneSystem::SaveScene(scene, path)) {
			SLT_LOG_ERROR("Editor", "failed to save scene to '{}'", path.ToString());
			return false;
		}
		MarkClean(scene);
		if (_state) {
			_state->paths[scene.GetInstanceId()] = path.ToString();
		}
		SceneAnnotations::SaveFor(scene);
		AddRecentScene(path.ToString());
		SLT_LOG_INFO("Editor", "scene saved to '{}'", path.ToString());
		return true;
	}

	void EditorSceneOps::GetRecentScenes(Vector<str>& out) {
		if (_state) {
			for (const str& path : _state->recents) {
				out.push_back(path);
			}
		}
	}

	void EditorSceneOps::AddRecentScene(std::string_view path) {
		if (!_state || path.empty()) {
			return;
		}
		std::erase(_state->recents, str(path));
		_state->recents.insert(_state->recents.begin(), str(path));
		while (_state->recents.size() > RECENT_CAP) {
			_state->recents.pop_back();
		}
	}

	namespace {

		class CreateBuiltObjectCommand final : public IEditorCommand {
		public:
			CreateBuiltObjectCommand(str name, Handle<Scene> scene, Handle<SceneObject> parent,
									 StringHash component, std::function<void(Handle<SceneObject>)> configure)
				: _name(std::move(name)), _scene(scene), _parent(parent),
				  _component(component), _configure(std::move(configure)),
				  _defaults(AuthoringDefaults::StateFor(component.GetValue() != 0 ? TypeRegistry::FindType(component) : nullptr)) {}

			void Apply() override {
				Handle<SceneObject> object = SceneSystem::CreateObject(_name, _scene);
				if (!object.IsValid()) {
					return;
				}
				if (_parent.IsValid()) {
					object.Get()->SetParent(_parent);
				}
				if (_component.GetValue() != 0) {
					if (const TypeInfo* type = TypeRegistry::FindType(_component)) {
						if (Component* added = object.Get()->AddComponent(type).Get()) {
							AuthoringDefaults::Write(*added, _defaults);
						}
					}
				}
				if (_configure) {
					_configure(object);
				}
				_created = object;
			}

			void Revert() override {
				if (_created.IsValid()) {
					SceneSystem::Destroy(_created);
				}
				_created = {};
			}

			str GetLabel() const override { return "Create " + _name; }
			Handle<Scene> GetAffectedScene() const override { return _scene; }
			Handle<SceneObject> GetCreated() const { return _created; }
		private:
			str _name;
			Handle<Scene> _scene;
			Handle<SceneObject> _parent;
			StringHash _component{};
			std::function<void(Handle<SceneObject>)> _configure;
			str _defaults;
			Handle<SceneObject> _created;
		};

		void BindPrimitiveMesh(Handle<SceneObject> object, PrimitiveShape shape) {
			str path = GetPrimitiveMeshPath(shape);
			Asset* meshAsset = path.empty() ? nullptr : AssetDatabase::LoadAsset(Path(path));
			if (!meshAsset) {
				SLT_LOG_WARN("Editor", "CreatePrimitiveObject: mesh unavailable (is engine:// mounted?)");
				return;
			}
			MeshAsset* mesh = static_cast<MeshAsset*>(meshAsset);
			Handle<MeshRenderer> renderer = object.Get()->GetComponent<MeshRenderer>();
			if (!renderer.IsValid()) {
				return;
			}
			renderer.Get()->SetMesh(Handle<MeshAsset>(mesh));
			if (!mesh->defaultMaterials.empty() && mesh->defaultMaterials[0].IsValid()) {
				if (Asset* materialAsset = AssetDatabase::LoadAssetByGuid(mesh->defaultMaterials[0])) {
					renderer.Get()->SetMaterial(0, Handle<MaterialAsset>(static_cast<MaterialAsset*>(materialAsset)));
				}
			}
		}

		Handle<SceneObject> PushBuild(str name, Handle<Scene> scene, Handle<SceneObject> parent,
									  StringHash component, std::function<void(Handle<SceneObject>)> configure) {
			Handle<Scene> targetScene = scene.IsValid() ? scene : SceneSystem::GetPrimaryScene();
			auto command = MakeUnique<CreateBuiltObjectCommand>(
				std::move(name), targetScene, parent.IsValid() ? parent : EditorSceneOps::GetDefaultParent(targetScene),
				component, std::move(configure));
			CreateBuiltObjectCommand* raw = command.get();
			UndoSystem::Push(std::move(command));
			return raw->GetCreated();
		}

	} // namespace

	Handle<SceneObject> EditorSceneBuild::Push(str name, Handle<Scene> scene, Handle<SceneObject> parent, StringHash component,
											   std::function<void(Handle<SceneObject>)> configure) {
		return PushBuild(std::move(name), scene, parent, component, std::move(configure));
	}

	Handle<SceneObject> EditorSceneOps::CreatePrimitiveObject(PrimitiveShape shape, Handle<Scene> scene,
															  Handle<SceneObject> parent) {
		if (GetPrimitiveMeshPath(shape).empty()) {
			return {};
		}
		return PushBuild(str(PrimitiveShapeName(shape)), scene, parent, StringHash("MeshRenderer"),
						 [shape](Handle<SceneObject> object) { BindPrimitiveMesh(object, shape); });
	}

	namespace {

		void SelectCreated(Handle<SceneObject> created) {
			if (created.IsValid()) {
				EditorSelection::Select(Handle<Object>(static_cast<Object*>(created.Get())));
			}
		}

		void ConfigureCreatedLight(Handle<SceneObject> object, LightType type) {
			Handle<Light> light = object.IsValid() ? object.Get()->GetComponent<Light>() : Handle<Light>{};
			if (!light.IsValid()) {
				return;
			}
			light.Get()->SetLightType(type);
			light.Get()->SetCastShadows(type == LightType::Directional);
		}

		Handle<SceneObject> CreateWithComponent(std::string_view name, StringHash component,
												Handle<Scene> scene, Handle<SceneObject> parent,
												std::function<void(Handle<SceneObject>)> configure = {}) {
			return PushBuild(str(name), scene, parent, component, std::move(configure));
		}

	} // anonymous namespace

	void EditorSceneOps::BuildCreateMenu(Handle<Scene> scene, Handle<SceneObject> parent,
										 Vector<UI::Widgets::MenuItem>& out) {
		Handle<Scene> target = scene.IsValid() ? scene : SceneSystem::GetPrimaryScene();
		Handle<SceneObject> under = parent.IsValid() ? parent : GetDefaultParent(target);

		out.push_back({"Create Empty", "", true, false, [target, under]() {
						   auto create = MakeUnique<CreateObjectCommand>("New Object", target, under);
						   CreateObjectCommand* raw = create.get();
						   UndoSystem::Push(std::move(create));
						   SelectCreated(raw->GetCreated());
					   }});

		UI::Widgets::MenuItem meshes;
		meshes.label = "3D Object";
		meshes.separatorBefore = true;
		for (u8 i = 0; i < static_cast<u8>(PrimitiveShape::Count); ++i) {
			PrimitiveShape shape = static_cast<PrimitiveShape>(i);
			UI::Widgets::MenuItem item;
			item.label = str(PrimitiveShapeName(shape));
			item.onSelect = [shape, target, under]() {
				SelectCreated(CreatePrimitiveObject(shape, target, under));
			};
			meshes.children.push_back(std::move(item));
		}
		out.push_back(std::move(meshes));

		UI::Widgets::MenuItem camera;
		camera.label = "Camera";
		camera.separatorBefore = true;
		camera.onSelect = [target, under]() {
			SelectCreated(CreateWithComponent("Camera", StringHash("Camera"), target, under));
		};
		out.push_back(std::move(camera));

		UI::Widgets::MenuItem lights;
		lights.label = "Light";
		struct LightEntry {
			const char* label;
			LightType type;
		};
		for (const LightEntry& entry : {LightEntry{"Directional Light", LightType::Directional},
										LightEntry{"Point Light", LightType::Point},
										LightEntry{"Spot Light", LightType::Spot}}) {
			UI::Widgets::MenuItem item;
			item.label = str(entry.label);
			const LightType type = entry.type;
			item.onSelect = [target, under, type]() {
				SelectCreated(CreateWithComponent("Light", StringHash("Light"), target, under,
												  [type](Handle<SceneObject> object) {
													  ConfigureCreatedLight(object, type);
												  }));
			};
			lights.children.push_back(std::move(item));
		}
		out.push_back(std::move(lights));

		UI::Widgets::MenuItem environment;
		environment.label = "Environment";
		environment.onSelect = [target, under]() {
			SelectCreated(CreateWithComponent("Environment", StringHash("Environment"), target, under));
		};
		out.push_back(std::move(environment));

		UI::Widgets::MenuItem decal;
		decal.label = "Decal";
		decal.onSelect = [target, under]() {
			SelectCreated(CreateWithComponent("Decal", StringHash("Decal"), target, under));
		};
		out.push_back(std::move(decal));

		if (!_createEntries) {
			return;
		}
		Vector<const CreateEntry*> entries{GetMemoryResource(MemTag::Editor)};
		for (const CreateEntry& entry : *_createEntries) {
			entries.push_back(&entry);
		}
		std::sort(entries.begin(), entries.end(), [](const CreateEntry* a, const CreateEntry* b) {
			return a->group != b->group ? a->group < b->group : a->label < b->label;
		});
		for (const CreateEntry* entry : entries) {
			if (out.empty() || out.back().label != entry->group || out.back().children.empty()) {
				UI::Widgets::MenuItem group;
				group.label = entry->group;
				out.push_back(std::move(group));
			}
			UI::Widgets::MenuItem item;
			item.label = entry->label;
			item.onSelect = [target, under, label = entry->label, component = entry->component]() {
				SelectCreated(CreateWithComponent(label, component, target, under));
			};
			out.back().children.push_back(std::move(item));
		}
	}

	bool EditorSceneOps::RegisterCreateEntry(std::string_view group, std::string_view label, StringHash component, const void* owner) {
		if (group.empty() || label.empty() || !TypeRegistry::FindType(component)) {
			SLT_LOG_WARN("Editor", "ignoring Create menu entry '{}/{}' (needs group/label + a registered component)", group, label);
			return false;
		}
		if (!_createEntries) {
			_createEntries = New<Vector<CreateEntry>>(MemTag::Editor, GetMemoryResource(MemTag::Editor));
		}
		for (const CreateEntry& entry : *_createEntries) {
			if (entry.group == group && entry.label == label) {
				SLT_LOG_WARN("Editor", "ignoring Create menu entry '{}/{}' (already registered)", group, label);
				return false;
			}
		}
		_createEntries->push_back({str(group), str(label), component, owner});
		return true;
	}

	u32 EditorSceneOps::UnregisterCreateEntriesOwnedBy(const void* owner) {
		if (!_createEntries || !owner) {
			return 0;
		}
		return static_cast<u32>(std::erase_if(*_createEntries, [owner](const CreateEntry& entry) { return entry.owner == owner; }));
	}

	UI::Widgets::MenuItem EditorSceneOps::BuildCreateSubmenu(std::string_view label, Handle<Scene> scene,
															 Handle<SceneObject> parent) {
		UI::Widgets::MenuItem item;
		item.label = str(label);
		BuildCreateMenu(scene, parent, item.children);
		return item;
	}

	u32 EditorSceneOps::BindDefaultMaterials(Handle<Object> renderer, u64 meshInstanceId) {
		MeshAsset* mesh = static_cast<MeshAsset*>(ObjectRegistry::Resolve(meshInstanceId));
		Object* live = renderer.Get();
		if (!live || !mesh || mesh->defaultMaterials.empty()) {
			return 0;
		}
		const FieldInfo* slots = FindField(live->GetType(), StringHash("_materials"));
		if (!slots || !slots->container || !slots->addrFn || !slots->container->sizeFn ||
			!slots->container->atFn || slots->container->elementSize > sizeof(u64)) {
			return 0;
		}
		void* slotList = slots->addrFn(live);
		const size_t slotsBound = slots->container->sizeFn(slotList);
		auto slotIsBound = [&](u32 index) -> bool {
			if (index >= slotsBound) {
				return false;
			}
			u64 bits = 0;
			std::memcpy(&bits, slots->container->atFn(slotList, index), slots->container->elementSize);
			return bits != 0 && ObjectRegistry::Resolve(bits) != nullptr;
		};
		u32 slotCount = 0;
		for (const SubMesh& submesh : mesh->submeshes) {
			if (submesh.materialSlot + 1 > slotCount) {
				slotCount = submesh.materialSlot + 1;
			}
		}
		if (slotCount > mesh->defaultMaterials.size()) {
			slotCount = static_cast<u32>(mesh->defaultMaterials.size());
		}

		u32 bound = 0;
		for (u32 slot = 0; slot < slotCount; ++slot) {
			if (slotIsBound(slot)) {
				continue;
			}
			const Guid& guid = mesh->defaultMaterials[slot];
			if (!guid.IsValid()) {
				continue;
			}
			Asset* material = AssetDatabase::LoadAssetByGuid(guid);
			if (!material) {
				continue;
			}
			UndoSystem::Push(MakeUnique<SetContainerElementCommand>(
				renderer, StringHash("_materials"), slot, 0, material->GetInstanceId(),
				static_cast<u32>(slotsBound), "Bind Material"));
			++bound;
		}
		return bound;
	}

	Handle<Scene> EditorSceneOps::OpenPrefab(const Guid& prefabId) {
		if (!prefabId.IsValid() || !_state) {
			return {};
		}
		if (IsModelPrefab(prefabId)) {
			SLT_LOG_WARN("Editor", "'{}' is imported from a model file. Drag it into a scene and override there (or make a Prefab Variant)", AssetDatabase::LookupPath(prefabId).ToString());
			return {};
		}
		if (Handle<Scene> staged = GetPrefabStage(); staged.IsValid()) {
			if (GetPrefabTarget(staged) == prefabId) {
				return staged;
			}
			LeavePrefabStage();
		}
		Path path = AssetDatabase::LookupPath(prefabId);
		str name = path.GetFilename();
		Handle<Scene> scene = SceneSystem::CreateScene(name.empty() ? str("Prefab") : name);
		if (!scene.IsValid()) {
			return {};
		}
		if (!SceneSystem::Instantiate(prefabId, scene).IsValid()) {
			SLT_LOG_WARN("Editor", "failed to open prefab '{}'", path.ToString());
			SceneSystem::UnloadScene(scene);
			return {};
		}
		_state->prefabTargets[scene.GetInstanceId()] = prefabId;
		_state->stage = scene;
		_state->stagePriorPrimary = SceneSystem::GetPrimaryScene();
		SceneSystem::SetPrimaryScene(scene);
		EditorSelection::Clear();
		return scene;
	}

	Handle<Scene> EditorSceneOps::GetPrefabStage() {
		return _state ? _state->stage : Handle<Scene>{};
	}

	Handle<SceneObject> EditorSceneOps::GetDefaultParent(Handle<Scene> scene) {
		if (!scene.IsValid() || scene.GetInstanceId() != GetPrefabStage().GetInstanceId()) {
			return {};
		}
		Scene* resolved = scene.Get();
		return resolved && resolved->GetRootCount() > 0 ? resolved->GetRoot(0) : Handle<SceneObject>{};
	}

	void EditorSceneOps::GetStageScenes(Vector<Handle<Scene>>& out) {
		out.clear();
		if (Handle<Scene> stage = GetPrefabStage(); stage.IsValid()) {
			out.push_back(stage);
			return;
		}
		for (const Handle<Scene>& scene : SceneSystem::GetLoadedScenes()) {
			out.push_back(scene);
		}
	}

	bool EditorSceneOps::LeavePrefabStage() {
		if (!_state || !_state->stage.IsValid()) {
			if (_state) {
				_state->stage = {};
				_state->stagePriorPrimary = {};
			}
			return false;
		}
		Handle<Scene> stage = _state->stage;
		Handle<Scene> restore = _state->stagePriorPrimary;
		_state->stage = {};
		_state->stagePriorPrimary = {};
		EditorSelection::Clear();
		UndoSystem::Clear();
		SceneSystem::UnloadScene(stage);
		if (restore.IsValid()) {
			SceneSystem::SetPrimaryScene(restore);
		}
		return true;
	}

	Guid EditorSceneOps::GetPrefabTarget(Handle<Scene> scene) {
		if (!_state) {
			return {};
		}
		auto it = _state->prefabTargets.find(scene.GetInstanceId());
		return it == _state->prefabTargets.end() ? Guid{} : it->second;
	}

	bool EditorSceneOps::AcceptsPrefabInstances(Handle<Scene> scene) {
		return !GetPrefabTarget(scene).IsValid();
	}

	bool EditorSceneOps::SavePrefabScene(Handle<Scene> scene) {
		Scene* resolved = scene.Get();
		if (!resolved || !GetPrefabTarget(scene).IsValid()) {
			return false;
		}
		if (resolved->GetRootCount() != 1) {
			SLT_LOG_ERROR("Editor", "'{}' has {} roots (a prefab has 1). Remove the extras to save",
						  resolved->name, resolved->GetRootCount());
			return false;
		}
		if (!SceneSystem::ApplyInstanceOverridesToBase(resolved->GetRoot(0))) {
			SLT_LOG_ERROR("Editor", "failed to apply '{}' back into its prefab", resolved->name);
			return false;
		}
		MarkClean(scene);
		SLT_LOG_INFO("Editor", "prefab saved from '{}'", resolved->name);
		return true;
	}

	namespace {

		void FlushPreviewNow() {
			if (!_state || !_state->previewEdited) {
				return;
			}
			_state->previewEdited = false;
			if (!_state->previewRoot.IsValid()) {
				return;
			}
			if (!SceneSystem::ApplyDetachedToPrefab(_state->previewRoot, _state->previewPrefab)) {
				SLT_LOG_ERROR("Editor", "failed to write an inspector edit back into its prefab");
			}
			_state->previewEpoch = SceneSystem::GetPrefabEpoch();
		}

	} // anonymous namespace

	Handle<SceneObject> EditorSceneOps::GetPrefabPreviewRoot(const Guid& prefabId) {
		if (!_state || !prefabId.IsValid()) {
			return {};
		}
		if (GetPrefabTarget(_state->stage) == prefabId) {
			Scene* stage = _state->stage.Get();
			return stage && stage->GetRootCount() > 0 ? stage->GetRoot(0) : Handle<SceneObject>{};
		}
		if (_state->previewPrefab == prefabId && _state->previewRoot.IsValid() &&
			_state->previewEpoch == SceneSystem::GetPrefabEpoch()) {
			return _state->previewRoot;
		}
		ReleasePrefabPreview();
		Handle<SceneObject> root = SceneSystem::InstantiateDetached(prefabId);
		if (!root.IsValid()) {
			return {};
		}
		_state->previewPrefab = prefabId;
		_state->previewRoot = root;
		_state->previewEpoch = SceneSystem::GetPrefabEpoch();
		return root;
	}

	Object* EditorSceneOps::ResolvePrefabRow(const Guid& prefabId, u64 rowKey) {
		if (rowKey == 0) {
			return nullptr;
		}
		SceneObject* root = GetPrefabPreviewRoot(prefabId).Get();
		if (!root) {
			return nullptr;
		}
		if (root->GetPrefabKey() == rowKey) {
			return root;
		}
		for (u32 i = 0; i < root->GetComponentCount(); ++i) {
			Component* component = root->GetComponentAt(i).Get();
			if (component && !component->IsPendingDestroy() && component->GetPrefabKey() == rowKey) {
				return component;
			}
		}
		return nullptr;
	}

	void EditorSceneOps::MarkPrefabPreviewEdited(const Guid& prefabId) {
		if (!_state) {
			return;
		}
		if (GetPrefabTarget(_state->stage) == prefabId) {
			return;
		}
		if (_state->previewPrefab == prefabId) {
			_state->previewEdited = true;
		}
	}

	void EditorSceneOps::FlushPrefabPreview() {
		if (!_state || !_state->previewEdited) {
			return;
		}
		if (UndoSystem::IsGroupOpen()) {
			return;
		}
		FlushPreviewNow();
	}

	void EditorSceneOps::ReleasePrefabPreview() {
		if (!_state) {
			return;
		}
		FlushPreviewNow();
		if (_state->previewRoot.IsValid()) {
			SceneSystem::DestroyDetached(_state->previewRoot);
		}
		_state->previewPrefab = {};
		_state->previewRoot = {};
		_state->previewEdited = false;
		_state->previewEpoch = 0;
	}

	namespace {

		Guid BasePrefabOf(const Guid& prefab) {
			Asset* asset = prefab.IsValid() ? AssetDatabase::LoadAssetByGuid(prefab) : nullptr;
			Guid base = asset && TypeRegistry::IsDerivedFrom(asset->GetType(), PrefabAsset::GetReflectType())
							? static_cast<PrefabAsset*>(asset)->GetBasePrefab()
							: Guid{};
			if (asset) {
				AssetDatabase::UnloadAsset(asset);
			}
			return base;
		}

	} // anonymous namespace

	bool EditorSceneOps::PrefabOverridesFor(Handle<SceneObject> rowHandle, PrefabInstanceDiff& out, Handle<SceneObject>* outRoot) {
		out = PrefabInstanceDiff{};
		if (outRoot) {
			*outRoot = {};
		}
		SceneObject* row = rowHandle.Get();
		if (!row) {
			return false;
		}
		SceneObject* root = row;
		while (!SceneSystem::IsPrefabInstanceRoot(Handle<SceneObject>(root))) {
			SceneObject* parent = root->GetParent().Get();
			if (!parent) {
				break;
			}
			root = parent;
		}
		Handle<SceneObject> rootHandle(root);

		Guid against{};
		if (_state && _state->previewRoot.IsValid() && _state->previewRoot.GetInstanceId() == rootHandle.GetInstanceId()) {
			against = BasePrefabOf(_state->previewPrefab);
		} else if (SceneSystem::IsPrefabInstanceRoot(rootHandle)) {
			if (Guid staged = GetPrefabTarget(root->GetScene()); staged.IsValid()) {
				against = BasePrefabOf(staged);
			}
		} else {
			return false;
		}
		if (outRoot) {
			*outRoot = rootHandle;
		}
		bool staged = (_state && _state->previewRoot.IsValid() && _state->previewRoot.GetInstanceId() == rootHandle.GetInstanceId()) ||
					  GetPrefabTarget(root->GetScene()).IsValid();
		if (staged && !against.IsValid()) {
			return true;
		}
		if (!SceneSystem::DiffPrefabInstance(rootHandle, out, against)) {
			out = PrefabInstanceDiff{};
			return false;
		}
		u64 rootId = rootHandle.GetInstanceId();
		std::erase_if(out.properties, [rootId](const PrefabInstanceDiff::Property& property) {
			return property.target.GetInstanceId() == rootId && property.fieldPath.size() == 1 &&
				   (property.fieldPath[0] == "_localPosition"_hash || property.fieldPath[0] == "_localRotation"_hash);
		});
		return true;
	}

	Guid EditorSceneOps::CreatePrefabFrom(Handle<SceneObject> root, const Path& path) {
		SceneObject* resolved = root.Get();
		if (!resolved || path.ToString().empty()) {
			return {};
		}
		if (RefusedOutsideContentRoot(path)) {
			return {};
		}
		EnsureParentDirectory(path);
		Handle<Scene> scene = resolved->GetScene();
		Guid guid = SceneSystem::SavePrefab(root, path);
		if (!guid.IsValid()) {
			SLT_LOG_ERROR("Editor", "failed to create a prefab at '{}'", path.ToString());
			return {};
		}
		MarkDirty(scene);
		SLT_LOG_INFO("Editor", "prefab created at '{}'", path.ToString());
		return guid;
	}

	bool EditorSceneOps::CanCreatePrefabVariantFrom(Handle<SceneObject> root) {
		SceneObject* resolved = root.Get();
		if (!resolved || !SceneSystem::IsPrefabInstanceRoot(root)) {
			return false;
		}
		return !GetPrefabTarget(resolved->GetScene()).IsValid();
	}

	Guid EditorSceneOps::CreatePrefabVariantFrom(Handle<SceneObject> root, const Path& path) {
		SceneObject* resolved = root.Get();
		if (!resolved || path.ToString().empty()) {
			return {};
		}
		if (!CanCreatePrefabVariantFrom(root)) {
			SLT_LOG_WARN("Editor", "'{}' can't become a variant (only the root of a prefab instance outside a prefab stage can)",
						 resolved->name);
			return {};
		}
		if (RefusedOutsideContentRoot(path)) {
			return {};
		}
		EnsureParentDirectory(path);
		Handle<Scene> scene = resolved->GetScene();
		Guid guid = SceneSystem::SavePrefabVariant(root, path);
		if (!guid.IsValid()) {
			SLT_LOG_ERROR("Editor", "failed to create a prefab variant at '{}'", path.ToString());
			return {};
		}
		MarkDirty(scene);
		SLT_LOG_INFO("Editor", "prefab variant created at '{}'", path.ToString());
		return guid;
	}

	Guid EditorSceneOps::CreatePrefabVariantOf(const Guid& basePrefab, const Path& path) {
		if (!basePrefab.IsValid() || path.ToString().empty()) {
			return {};
		}
		if (RefusedOutsideContentRoot(path)) {
			return {};
		}
		EnsureParentDirectory(path);
		Guid guid = SceneSystem::CreatePrefabVariant(basePrefab, path);
		if (!guid.IsValid()) {
			SLT_LOG_ERROR("Editor", "failed to create a prefab variant at '{}'", path.ToString());
			return {};
		}
		SLT_LOG_INFO("Editor", "prefab variant created at '{}'", path.ToString());
		return guid;
	}

	void EditorSceneOps::GetSelectedObjects(Vector<Handle<SceneObject>>& out) {
		for (const Handle<Object>& selected : EditorSelection::GetSelection()) {
			Object* raw = selected.Get();
			if (raw && TypeRegistry::IsDerivedFrom(raw->GetType(), SceneObject::GetReflectType())) {
				out.push_back(Handle<SceneObject>(static_cast<SceneObject*>(raw)));
			}
		}
	}

	u32 EditorSceneOps::DeleteObjects(std::span<const Handle<SceneObject>> objects) {
		Vector<Handle<SceneObject>> doomed{GetMemoryResource(MemTag::Editor)};
		for (Handle<SceneObject> object : objects) {
			if (object.IsValid()) {
				doomed.push_back(object);
			}
		}
		if (doomed.empty()) {
			return 0;
		}
		UndoSystem::BeginGroup("Delete Objects");
		for (Handle<SceneObject> object : doomed) {
			UndoSystem::Push(MakeUnique<DestroyObjectCommand>(object));
		}
		UndoSystem::EndGroup();
		return static_cast<u32>(doomed.size());
	}

	u32 EditorSceneOps::DeleteSelectedObjects() {
		Vector<Handle<SceneObject>> doomed{GetMemoryResource(MemTag::Editor)};
		GetSelectedObjects(doomed);
		u32 count = DeleteObjects({doomed.data(), doomed.size()});
		if (count > 0) {
			EditorSelection::Clear();
		}
		return count;
	}

	u32 EditorSceneOps::DuplicateObjects(std::span<const Handle<SceneObject>> objects) {
		Vector<Handle<SceneObject>> sources{GetMemoryResource(MemTag::Editor)};
		for (Handle<SceneObject> object : objects) {
			if (object.IsValid()) {
				sources.push_back(object);
			}
		}
		if (sources.empty()) {
			return 0;
		}
		Vector<Handle<SceneObject>> copies{GetMemoryResource(MemTag::Editor)};
		UndoSystem::BeginGroup("Duplicate Objects");
		for (Handle<SceneObject> source : sources) {
			auto command = MakeUnique<DuplicateObjectCommand>(source);
			DuplicateObjectCommand* raw = command.get();
			UndoSystem::Push(std::move(command));
			if (Handle<SceneObject> copy = raw->GetCreated(); copy.IsValid()) {
				copies.push_back(copy);
			}
		}
		UndoSystem::EndGroup();
		bool opened = false;
		for (Handle<SceneObject> copy : copies) {
			Handle<Object> object(static_cast<Object*>(copy.Get()));
			if (!opened) {
				EditorSelection::Select(object);
				opened = true;
			} else {
				EditorSelection::AddToSelection(object);
			}
		}
		return static_cast<u32>(sources.size());
	}

	u32 EditorSceneOps::DuplicateSelectedObjects() {
		Vector<Handle<SceneObject>> sources{GetMemoryResource(MemTag::Editor)};
		GetSelectedObjects(sources);
		return DuplicateObjects({sources.data(), sources.size()});
	}

	namespace {

		bool IsInSubtreeOf(const SceneObject* node, const SceneObject* ancestor) {
			for (const SceneObject* walk = node; walk; walk = walk->GetParent().Get()) {
				if (walk == ancestor) {
					return true;
				}
			}
			return false;
		}

	} // anonymous namespace

	void EditorSceneOps::GetOutermostInTreeOrder(std::span<const Handle<SceneObject>> objects,
												 Vector<Handle<SceneObject>>& out) {
		out.clear();
		HashSet<const SceneObject*> named{GetMemoryResource(MemTag::Editor)};
		for (Handle<SceneObject> object : objects) {
			if (SceneObject* resolved = object.Get(); resolved && !resolved->IsPendingDestroy()) {
				named.insert(resolved);
			}
		}
		HashSet<const SceneObject*> taken{GetMemoryResource(MemTag::Editor)};
		for (Handle<SceneObject> object : objects) {
			SceneObject* resolved = object.Get();
			if (!resolved || !named.contains(resolved) || taken.contains(resolved)) {
				continue;
			}
			bool nested = false;
			for (SceneObject* walk = resolved->GetParent().Get(); walk && !nested; walk = walk->GetParent().Get()) {
				nested = named.contains(walk);
			}
			if (!nested) {
				taken.insert(resolved);
				out.push_back(object);
			}
		}
		if (out.size() < 2) {
			return;
		}

		Vector<Scene*> scenes{GetMemoryResource(MemTag::Editor)};
		for (const Handle<Scene>& scene : SceneSystem::GetLoadedScenes()) {
			if (Scene* resolved = scene.Get()) {
				scenes.push_back(resolved);
			}
		}
		for (Handle<SceneObject> object : out) {
			Scene* scene = object.Get()->GetScene().Get();
			if (scene && std::find(scenes.begin(), scenes.end(), scene) == scenes.end()) {
				scenes.push_back(scene);
			}
		}
		HashMap<const SceneObject*, u64> rank{GetMemoryResource(MemTag::Editor)};
		u64 next = 0;
		Vector<SceneObject*> stack{GetMemoryResource(MemTag::Editor)};
		for (Scene* scene : scenes) {
			for (u32 root = scene->GetRootCount(); root > 0 && rank.size() < taken.size(); --root) {
				stack.push_back(scene->GetRoot(root - 1).Get());
			}
			while (!stack.empty() && rank.size() < taken.size()) {
				SceneObject* node = stack.back();
				stack.pop_back();
				if (!node) {
					continue;
				}
				++next;
				if (taken.contains(node)) {
					rank[node] = next;
				}
				for (u32 child = node->GetChildCount(); child > 0; --child) {
					stack.push_back(node->GetChild(child - 1).Get());
				}
			}
			stack.clear();
		}
		std::stable_sort(out.begin(), out.end(), [&rank](Handle<SceneObject> a, Handle<SceneObject> b) {
			auto at = rank.find(a.Get());
			auto bt = rank.find(b.Get());
			return (at != rank.end() ? at->second : ~0ull) < (bt != rank.end() ? bt->second : ~0ull);
		});
	}

	bool EditorSceneOps::CanMoveObjects(std::span<const Handle<SceneObject>> objects, Handle<SceneObject> newParent,
										Handle<Scene> scene) {
		SceneObject* parent = newParent.Get();
		if (newParent.IsValid() && !parent) {
			return false;
		}
		Handle<Scene> to = parent ? parent->GetScene() : scene;
		bool any = false;
		for (Handle<SceneObject> object : objects) {
			SceneObject* resolved = object.Get();
			if (!resolved || resolved->IsPendingDestroy()) {
				continue;
			}
			Handle<Scene> from = resolved->GetScene();
			if (from != to && (GetPrefabTarget(from).IsValid() || GetPrefabTarget(to).IsValid())) {
				return false;
			}
			if (parent && IsInSubtreeOf(parent, resolved)) {
				return false;
			}
			any = true;
		}
		return any;
	}

	bool EditorSceneOps::MoveObjects(std::span<const Handle<SceneObject>> objects, Handle<SceneObject> newParent,
									 Handle<Scene> scene, u32 index) {
		Vector<Handle<SceneObject>> moving{GetMemoryResource(MemTag::Editor)};
		GetOutermostInTreeOrder(objects, moving);
		if (moving.empty() || !CanMoveObjects({moving.data(), moving.size()}, newParent, scene)) {
			return false;
		}
		SceneObject* parent = newParent.Get();
		Scene* list = parent ? nullptr : scene.Get();
		if (!parent && !list) {
			return false;
		}
		auto count = [parent, list]() { return parent ? parent->GetChildCount() : list->GetRootCount(); };

		Handle<SceneObject> before;
		for (u32 i = index; i < count(); ++i) {
			Handle<SceneObject> child = parent ? parent->GetChild(i) : list->GetRoot(i);
			if (std::find(moving.begin(), moving.end(), child) == moving.end()) {
				before = child;
				break;
			}
		}

		UndoSystem::BeginGroup(moving.size() > 1 ? "Move Objects" : "Move Object");
		for (Handle<SceneObject> object : moving) {
			SceneObject* resolved = object.Get();
			if (!resolved) {
				continue;
			}
			const bool sameList = parent ? resolved->GetParent().Get() == parent
										 : !resolved->GetParent().IsValid() && resolved->GetScene().Get() == list;
			u32 at = 0;
			if (SceneObject* anchor = before.Get()) {
				at = anchor->GetSiblingIndex();
				if (sameList && resolved->GetSiblingIndex() < at) {
					--at;
				}
			} else {
				at = count() - (sameList ? 1u : 0u);
			}
			if (!sameList) {
				UndoSystem::Push(MakeUnique<ReparentCommand>(object, newParent, at));
			} else if (resolved->GetSiblingIndex() != at) {
				UndoSystem::Push(MakeUnique<SetSiblingIndexCommand>(object, at));
			}
		}
		UndoSystem::EndGroup();
		return true;
	}

	namespace {

		class CreateEmptyParentCommand final : public IEditorCommand {
		public:
			CreateEmptyParentCommand(Handle<Scene> scene, Handle<SceneObject> parent, Handle<SceneObject> anchor,
									 Vec3 at, Vector<Handle<SceneObject>> children)
				: _scene(scene), _parent(parent), _anchor(anchor), _at(at), _children(std::move(children)) {}

			void Apply() override {
				_places.clear();
				for (Handle<SceneObject> child : _children) {
					const SceneObject* live = child.Get();
					_places.push_back(live ? live->GetSiblingIndex() : 0);
				}
				_created = SceneSystem::CreateObject("New Object", _scene, _parent);
				SceneObject* made = _created.Get();
				if (!made) {
					return;
				}
				made->SetWorldPosition(_at);
				if (const SceneObject* beside = _anchor.Get()) {
					made->SetSiblingIndex(beside->GetSiblingIndex() + 1);
				}
				for (Handle<SceneObject> child : _children) {
					if (SceneObject* live = child.Get()) {
						live->SetParent(_created, true);
					}
				}
			}

			void Revert() override {
				SceneObject* made = _created.Get();
				if (made) {
					made->SetSiblingIndex(0xFFFFFFFFu);
				}
				for (size_t i = 0; i < _children.size(); ++i) {
					if (SceneObject* live = _children[i].Get()) {
						live->SetParent(_parent, true);
						live->SetSiblingIndex(_places[i]);
					}
				}
				if (made) {
					SceneSystem::Destroy(_created);
				}
				_created = {};
			}

			str GetLabel() const override { return "Create Empty Parent"; }
			Handle<Scene> GetAffectedScene() const override { return _scene; }
			Handle<SceneObject> GetCreated() const { return _created; }
		private:
			Handle<Scene> _scene;
			Handle<SceneObject> _parent;
			Handle<SceneObject> _anchor;
			Vec3 _at{};
			Vector<Handle<SceneObject>> _children;
			Vector<u32> _places{GetMemoryResource(MemTag::Editor)};
			Handle<SceneObject> _created;
		};

	} // anonymous namespace

	bool EditorSceneOps::CanCreateEmptyParent(std::span<const Handle<SceneObject>> objects) {
		const SceneObject* first = nullptr;
		for (Handle<SceneObject> object : objects) {
			const SceneObject* resolved = object.Get();
			if (!resolved || resolved->IsPendingDestroy()) {
				return false;
			}
			if (!first) {
				first = resolved;
			} else if (!(resolved->GetParent() == first->GetParent()) || !(resolved->GetScene() == first->GetScene())) {
				return false;
			}
			if (!resolved->GetParent().IsValid() && GetPrefabTarget(resolved->GetScene()).IsValid()) {
				return false;
			}
		}
		return first != nullptr;
	}

	Handle<SceneObject> EditorSceneOps::CreateEmptyParent(std::span<const Handle<SceneObject>> objects,
														  Handle<SceneObject> active) {
		if (!CanCreateEmptyParent(objects)) {
			return {};
		}
		Vector<Handle<SceneObject>> children{GetMemoryResource(MemTag::Editor)};
		for (Handle<SceneObject> object : objects) {
			if (std::find(children.begin(), children.end(), object) == children.end()) {
				children.push_back(object);
			}
		}
		std::sort(children.begin(), children.end(), [](Handle<SceneObject> a, Handle<SceneObject> b) {
			return a.Get()->GetSiblingIndex() < b.Get()->GetSiblingIndex();
		});
		const Handle<SceneObject> anchor =
			std::find(children.begin(), children.end(), active) != children.end() ? active : children.front();
		const SceneObject* resolved = anchor.Get();
		auto command = MakeUnique<CreateEmptyParentCommand>(resolved->GetScene(), resolved->GetParent(), anchor,
															resolved->GetWorldPosition(), std::move(children));
		CreateEmptyParentCommand* raw = command.get();
		UndoSystem::Push(std::move(command));
		const Handle<SceneObject> created = raw->GetCreated();
		SelectCreated(created);
		return created;
	}

	bool EditorSceneOps::RemovePreservedComponent(Handle<Component> component) {
		Component* resolved = component.Get();
		if (!resolved || resolved->GetType() != UnresolvedComponent::GetReflectType()) {
			return false;
		}
		SLT_LOG_WARN("Editor", "removing a preserved component row (type {:#018x}, data isn't recoverable)",
					 static_cast<UnresolvedComponent*>(resolved)->GetUnresolvedType().GetValue());
		Handle<Scene> scene = resolved->GetScene();
		SceneSystem::Destroy(component);
		MarkDirty(scene);
		return true;
	}

	EditorSceneOps::PreservedRowSurvey EditorSceneOps::SurveyPreservedComponents(Handle<Scene> scene) {
		PreservedRowSurvey survey;
		Scene* resolved = scene.Get();
		if (!resolved) {
			return survey;
		}
		resolved->ForEachObject(
			+[](SceneObject& object, void* user) {
				auto* out = static_cast<PreservedRowSurvey*>(user);
				for (u32 i = 0; i < object.GetComponentCount(); ++i) {
					Component* component = object.GetComponentAt(i).Get();
					if (!component || component->GetType() != UnresolvedComponent::GetReflectType()) {
						continue;
					}
					++out->rows;
					StringHash declared = static_cast<UnresolvedComponent*>(component)->GetUnresolvedType();
					if (std::find(out->types.begin(), out->types.end(), declared) == out->types.end()) {
						out->types.push_back(declared);
					}
				}
			},
			&survey);
		return survey;
	}

	EditorSceneOps::ComponentDropKind EditorSceneOps::ClassifyComponentDrop(Handle<Component> component,
																			Handle<SceneObject> target) {
		Component* dragged = component.Get();
		SceneObject* owner = dragged ? dragged->GetOwner().Get() : nullptr;
		SceneObject* destination = target.Get();
		if (!dragged || !owner || !destination || dragged->IsPendingDestroy() || destination->IsPendingDestroy()) {
			return ComponentDropKind::None;
		}
		return owner == destination ? ComponentDropKind::Reorder : ComponentDropKind::Move;
	}

	bool EditorSceneOps::DropComponentOn(Handle<Component> component, Handle<SceneObject> target, u32 index) {
		ComponentDropKind kind = ClassifyComponentDrop(component, target);
		if (kind == ComponentDropKind::None) {
			return false;
		}
		Component* dragged = component.Get();
		SceneObject* destination = target.Get();
		const TypeInfo* type = dragged->GetType();
		if (!type) {
			return false;
		}

		if (kind == ComponentDropKind::Reorder) {
			u32 current = destination->GetComponentCount();
			for (u32 i = 0; i < destination->GetComponentCount(); ++i) {
				if (destination->GetComponentAt(i) == component) {
					current = i;
					break;
				}
			}
			u32 wanted = std::min(index, destination->GetComponentCount() - 1);
			if (current < wanted) {
				--wanted;
			}
			if (current == wanted) {
				return false;
			}
			UndoSystem::Push(MakeUnique<SetComponentIndexCommand>(component, wanted));
			return true;
		}

		str stateJSON = TextSerializer::Serialize(dragged, type);
		Handle<SceneObject> destinationHandle(destination);
		UndoSystem::BeginGroup("Move Component");
		UndoSystem::Push(MakeUnique<RemoveComponentCommand>(component));
		auto add = MakeUnique<AddComponentCommand>(destinationHandle, type->name, stateJSON);
		AddComponentCommand* addRaw = add.get();
		UndoSystem::Push(std::move(add));
		Handle<Component> created = addRaw->GetCreated();
		if (created.IsValid() && index != COMPONENT_INDEX_END) {
			UndoSystem::Push(MakeUnique<SetComponentIndexCommand>(created, index));
		}
		UndoSystem::EndGroup();
		return created.IsValid();
	}

	namespace {

		class DefaultInstance {
		public:
			explicit DefaultInstance(const TypeInfo* type)
				: _object(TypeRegistry::CreateObjectInstance(type, MemTag::Editor)) {
				if (_object) {
					AuthoringDefaults::Write(*_object, AuthoringDefaults::StateFor(type));
				}
			}
			~DefaultInstance() { TypeRegistry::DestroyObjectInstance(_object); }
			DefaultInstance(const DefaultInstance&) = delete;
			DefaultInstance& operator=(const DefaultInstance&) = delete;
			Object* Get() const { return _object; }
		private:
			Object* _object = nullptr;
		};

		struct ValuePlace {
			void* holder = nullptr;
			const TypeInfo* holderType = nullptr;
			const FieldInfo* member = nullptr;
			const ContainerInfo* container = nullptr;
			size_t element = 0;
		};

		bool LocateValue(Object& object, StringHash field, std::span<const EditorSceneOps::ValueStep> steps,
						 bool grow, ValuePlace& out) {
			void* at = &object;
			const TypeInfo* type = object.GetType();
			const ContainerInfo* container = nullptr;
			for (size_t i = 0; i <= steps.size(); ++i) {
				const bool last = i == steps.size();
				const i32 element = i == 0 ? -1 : steps[i - 1].element;
				if (element >= 0) {
					if (!container || !container->sizeFn || !container->atFn) {
						return false;
					}
					const size_t index = static_cast<size_t>(element);
					if (index >= container->sizeFn(at)) {
						if (!grow || !container->resizeFn) {
							return false;
						}
						container->resizeFn(at, index + 1);
					}
					if (last) {
						out = {at, nullptr, nullptr, container, index};
						return true;
					}
					at = container->atFn(at, index);
					type = container->elementType;
					container = nullptr;
					continue;
				}
				const StringHash name = i == 0 ? field : steps[i - 1].field;
				const FieldInfo* member = container || !type ? nullptr : FindField(type, name);
				if (!member || !member->addrFn || (member->flags & FieldFlagTransient) != 0) {
					return false;
				}
				if (last) {
					out = {at, type, member, nullptr, 0};
					return true;
				}
				at = member->addrFn(at);
				type = member->fieldType;
				container = (member->flags & FieldFlagContainer) != 0 ? member->container : nullptr;
			}
			return false;
		}

		bool HoldsText(const ContainerInfo& container) {
			return container.elementType && container.elementType->name == GetStaticType<str>()->name;
		}

		str PlaceState(const ValuePlace& place) {
			if (place.member) {
				return CaptureFieldState(place.holder, place.holderType, place.member->name);
			}
			const ContainerInfo& container = *place.container;
			const void* element = container.atFn(place.holder, place.element);
			if ((container.elementFlags & FieldFlagSubobject) != 0 && container.elementType) {
				ScopedLiveHandles live;
				return TextSerializer::Serialize(element, container.elementType);
			}
			if (HoldsText(container)) {
				return "s" + *static_cast<const str*>(element);
			}
			if (container.elementSize > 8 && (container.elementFlags & FieldFlagTriviallyCopyable) == 0) {
				return {};
			}
			str bytes;
			for (u32 i = 0; i < container.elementSize; ++i) {
				bytes += std::format("{:02x}", static_cast<const u8*>(element)[i]);
			}
			return bytes;
		}

		void CopyPlace(const ValuePlace& from, const ValuePlace& to) {
			ScopedLiveHandles live;
			if (to.member) {
				TextSerializer::Deserialize(to.holder, to.holderType, PlaceState(from));
				return;
			}
			const ContainerInfo& container = *to.container;
			void* dest = container.atFn(to.holder, to.element);
			const void* source = from.container->atFn(from.holder, from.element);
			if ((container.elementFlags & FieldFlagSubobject) != 0 && container.elementType) {
				TextSerializer::Deserialize(dest, container.elementType, TextSerializer::Serialize(source, container.elementType));
				return;
			}
			if (HoldsText(container)) {
				*static_cast<str*>(dest) = *static_cast<const str*>(source);
				return;
			}
			std::memcpy(dest, source, container.elementSize);
		}

		bool ResetAgainst(Object& live, Object& fresh, EditTarget target, StringHash field,
						  std::span<const EditorSceneOps::ValueStep> steps, std::string_view label, bool write) {
			ValuePlace current;
			ValuePlace wanted;
			if (!LocateValue(live, field, steps, false, current) || !LocateValue(fresh, field, steps, true, wanted)) {
				return false;
			}
			const str currentState = PlaceState(current);
			const str wantedState = PlaceState(wanted);
			if (currentState.empty() || wantedState.empty() || currentState == wantedState) {
				return false;
			}
			if (!write) {
				return true;
			}
			if (steps.empty()) {
				Variant before;
				Variant after;
				if (GetField(&live, live.GetType(), field, &before) && GetField(&fresh, live.GetType(), field, &after)) {
					UndoSystem::Push(MakeUnique<PropertySetCommand>(target, field, std::move(before), std::move(after), label));
				} else {
					UndoSystem::Push(MakeUnique<SetFieldStateCommand>(target, field, currentState, wantedState, label));
				}
				return true;
			}
			str before = CaptureFieldState(live, field);
			CopyPlace(wanted, current);
			target.NotifyWritten();
			str after = CaptureFieldState(live, field);
			if (before.empty() || after == before) {
				return false;
			}
			UndoSystem::Push(MakeUnique<SetFieldStateCommand>(target, field, std::move(before), std::move(after), label));
			return true;
		}

	} // anonymous namespace

	bool EditorSceneOps::DiffersFromDefault(EditTarget target, StringHash field, std::span<const ValueStep> steps) {
		Object* live = target.Resolve();
		DefaultInstance fresh(live ? live->GetType() : nullptr);
		return live && fresh.Get() && ResetAgainst(*live, *fresh.Get(), target, field, steps, {}, false);
	}

	bool EditorSceneOps::ResetToDefault(EditTarget target, StringHash field, std::span<const ValueStep> steps,
										std::string_view label) {
		Object* live = target.Resolve();
		DefaultInstance fresh(live ? live->GetType() : nullptr);
		return live && fresh.Get() && ResetAgainst(*live, *fresh.Get(), target, field, steps, label, true);
	}

	u32 EditorSceneOps::ResetFieldsToDefaults(EditTarget target, std::span<const StringHash> fields, std::string_view label) {
		Object* live = target.Resolve();
		DefaultInstance fresh(live ? live->GetType() : nullptr);
		if (!live || !fresh.Get()) {
			return 0;
		}
		u32 moved = 0;
		UndoSystem::BeginGroup(label);
		for (StringHash field : fields) {
			Object* current = target.Resolve();
			if (current && ResetAgainst(*current, *fresh.Get(), target, field, {}, label, true)) {
				++moved;
			}
		}
		UndoSystem::EndGroup();
		return moved;
	}

	u32 EditorSceneOps::ResetComponentToDefaults(EditTarget component) {
		Object* live = component.Resolve();
		const TypeInfo* base = Component::GetReflectType();
		if (!live || !TypeRegistry::IsDerivedFrom(live->GetType(), base)) {
			return 0;
		}
		Vector<const TypeInfo*> chain{GetMemoryResource(MemTag::Editor)};
		for (const TypeInfo* walk = live->GetType(); walk && walk->name != base->name; walk = walk->parent) {
			chain.push_back(walk);
		}
		Vector<StringHash> fields{GetMemoryResource(MemTag::Editor)};
		for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
			for (u32 i = 0; i < (*it)->fieldCount; ++i) {
				if (((*it)->fields[i].flags & FieldFlagTransient) == 0) {
					fields.push_back((*it)->fields[i].name);
				}
			}
		}
		const TypeInfo* type = live->GetType();
		const str label = str("Reset ") + (type->displayName ? type->displayName : "Component");
		return ResetFieldsToDefaults(component, fields, label);
	}

	Vec3 EditorSceneOps::EulerForDisplay(const Quat& rotation) {
		const Vec3 derived = rotation.ToEuler();
		f64 scale = 1.0;
		for (u32 places = 0; places <= 5; ++places, scale *= 10.0) {
			const Vec3 candidate(static_cast<f32>(std::round(static_cast<f64>(derived.x) * scale) / scale),
								 static_cast<f32>(std::round(static_cast<f64>(derived.y) * scale) / scale),
								 static_cast<f32>(std::round(static_cast<f64>(derived.z) * scale) / scale));
			if (Quat::FromEuler(candidate) == rotation) {
				return candidate;
			}
		}
		return derived;
	}

} // namespace Shalltear
