#include "HierarchyPanel.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Scene/Component.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/UI/UISystem.h"
#include "Shalltear/Core/UI/Widgets.h"

#include "Shalltear/Editor/AssetBrowserOps.h"
#include "Shalltear/Editor/EditorCommands.h"
#include "Shalltear/Editor/EditorDragData.h"
#include "Shalltear/Editor/EditorIcons.h"
#include "Shalltear/Editor/EditorInputOps.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/EditorSelection.h"
#include "Shalltear/Editor/EditorSelectionOps.h"
#include "Shalltear/Editor/EditorTheme.h"
#include "Shalltear/Editor/EditorViewportOps.h"
#include "Shalltear/Editor/PanelManager.h"
#include "Shalltear/Editor/SceneAnnotations.h"
#include "Shalltear/Editor/SceneAssetDrops.h"
#include "Shalltear/Editor/UndoSystem.h"

#include "../SceneFileFlows.h"
#include "PanelHeader.h"

#include <algorithm>

namespace Shalltear {

	namespace {

		using EditorTheme::ColorToken;
		using EditorTheme::MetricToken;

		Color TextStrong() { return EditorTheme::GetColor(ColorToken::TextStrong); }
		Color TextDim() { return EditorTheme::GetColor(ColorToken::TextDim); }
		f32 RowFont() { return EditorTheme::GetMetric(MetricToken::FontRow); }
		f32 MenuFont() { return EditorTheme::GetMetric(MetricToken::FontBody); }

		f32 PictureColumnPx() { return EditorIcons::ComponentIcons::SizePx(); }

		std::string_view RowBadge(const SceneObject& object) {
			if (object.GetComponentCount() == 0) {
				return {};
			}
			Component* first = object.GetComponentAt(0).Get();
			return first ? EditorIcons::ComponentIcons::For(first->GetType()) : std::string_view{};
		}

		void ApplyLabelTint(UI::Context& context, UI::ElementHandle row, LabelColor color) {
			StringHash swatch = EditorTheme::Class::ForLabel(static_cast<u8>(color));
			if (swatch != StringHash()) {
				context.AddClass(row, swatch);
			}
		}

		void ApplyOffState(UI::Context& context, UI::ElementHandle glyph,
						   const UI::Widgets::TreeRowParts& row) {
			context.AddClass(glyph, EditorTheme::Class::RowOff);
			context.AddClass(row.labelElement, EditorTheme::Class::RowOff);
		}

		void AddActiveToggle(UI::Context& context, const UI::Widgets::TreeRowParts& row,
							 Handle<SceneObject> objectHandle) {
			SceneObject* live = objectHandle.Get();
			bool active = live && live->IsActiveSelf();

			UI::Widgets::Spacer(context, row.row);
			UI::ElementHandle eye = context.CreateElement();
			context.AppendChild(row.row, eye);
			UI::Style eyeStyle;
			eyeStyle.flexShrink = 0.0f;
			context.SetStyle(eye, eyeStyle);
			context.AddClass(eye, EditorTheme::Class::Clickable);
			EditorIcons::SetGlyphOrText(context, eye,
										active ? EditorIcons::Glyph::Eye : EditorIcons::Glyph::EyeSlash,
										active ? "*" : "-", TextDim(),
										EditorIcons::GutterSizePx(), RowFont());
			context.SetTooltip(eye, active ? "Hide" : "Show");
			context.Events(eye).onClick = [objectHandle]() {
				if (SceneObject* target = objectHandle.Get()) {
					UndoSystem::Push(MakeUnique<SetActiveCommand>(objectHandle, !target->IsActiveSelf()));
				}
			};

			if (!active) {
				return;
			}
			UI::Context* contextPtr = &context;
			context.AddClass(eye, EditorTheme::Class::RowReveal);
			context.Events(row.row).onHoverEnter = [contextPtr, eye]() {
				contextPtr->RemoveClass(eye, EditorTheme::Class::RowReveal);
			};
			context.Events(row.row).onHoverLeave = [contextPtr, eye]() {
				contextPtr->AddClass(eye, EditorTheme::Class::RowReveal);
			};
		}

		void Mix(u64& hash, u64 value) {
			constexpr u64 PRIME = 0x00000100000001b3ull;
			for (u32 shift = 0; shift < 64; shift += 8) {
				hash ^= (value >> shift) & 0xFF;
				hash *= PRIME;
			}
		}
		void MixString(u64& hash, std::string_view text) {
			constexpr u64 PRIME = 0x00000100000001b3ull;
			for (char c : text) {
				hash ^= static_cast<u8>(c);
				hash *= PRIME;
			}
		}

		void MixObjectRow(u64& hash, SceneObject& object, bool expanded) {
			Mix(hash, object.GetInstanceId());
			MixString(hash, object.name);
			Mix(hash, object.IsActiveSelf() ? 1 : 0);
			Mix(hash, SceneSystem::IsPrefabLinked(Handle<SceneObject>(&object)) ? 1 : 0);
			MixString(hash, RowBadge(object));
			Mix(hash, object.IsPendingDestroy() ? 1 : 0);
			Mix(hash, object.GetChildCount());
			if (!expanded) {
				return;
			}
			for (u32 i = 0; i < object.GetChildCount(); ++i) {
				Mix(hash, object.GetChild(i).GetInstanceId());
			}
		}

		void MixMatchCandidate(SceneObject& object, void* user) {
			u64& hash = *static_cast<u64*>(user);
			Mix(hash, object.GetInstanceId());
			MixString(hash, object.name);
			Mix(hash, object.IsActiveSelf() ? 1 : 0);
			Mix(hash, SceneSystem::IsPrefabLinked(Handle<SceneObject>(&object)) ? 1 : 0);
			MixString(hash, RowBadge(object));
			Mix(hash, object.IsPendingDestroy() ? 1 : 0);
			Mix(hash, object.GetChildCount());
		}

		void PayloadObjects(const UI::DragPayload& payload, Vector<Handle<SceneObject>>& out) {
			out.clear();
			if (payload.type != "SceneObject"_hash || !payload.data.Is<SceneObjectDragData>()) {
				return;
			}
			for (u64 id : payload.data.As<SceneObjectDragData>().objects) {
				Object* raw = ObjectRegistry::Resolve(id);
				if (raw && TypeRegistry::IsDerivedFrom(raw->GetType(), SceneObject::GetReflectType())) {
					out.push_back(Handle<SceneObject>(static_cast<SceneObject*>(raw)));
				}
			}
		}

		bool PayloadFolder(const UI::DragPayload& payload, Handle<Scene>& outScene, u64& outFolder) {
			if (payload.type != "SceneFolder"_hash || !payload.data.Is<SceneFolderDragData>()) {
				return false;
			}
			const SceneFolderDragData& data = payload.data.As<SceneFolderDragData>();
			Object* raw = ObjectRegistry::Resolve(data.sceneId);
			if (!raw || !TypeRegistry::IsDerivedFrom(raw->GetType(), Scene::GetReflectType())) {
				return false;
			}
			outScene = Handle<Scene>(static_cast<Scene*>(raw));
			outFolder = data.folderId;
			FolderInfo info;
			return SceneAnnotations::GetFolder(outScene, outFolder, info);
		}

		Guid PayloadAssetOfType(const UI::DragPayload& payload, StringHash type) {
			if (payload.type != "Asset"_hash || !payload.data.Is<AssetDragData>()) {
				return {};
			}
			const AssetDragData& data = payload.data.As<AssetDragData>();
			return StringHash{data.assetTypeHash} == type ? data.guid : Guid{};
		}

		bool PayloadScenePath(const UI::DragPayload& payload, Path& outPath) {
			Guid guid = PayloadAssetOfType(payload, StringHash("SceneAsset"));
			if (!guid.IsValid()) {
				return false;
			}
			outPath = AssetDatabase::LookupPath(guid);
			return !outPath.ToString().empty();
		}

		Guid PayloadPrefab(const UI::DragPayload& payload, Handle<Scene> targetScene) {
			if (!EditorSceneOps::AcceptsPrefabInstances(targetScene)) {
				return {};
			}
			return PayloadAssetOfType(payload, StringHash("PrefabAsset"));
		}

		u32 GroupChildCount(Handle<Scene> scene, FolderParent parent) {
			Handle<SceneObject> anchor;
			if (parent.kind == FolderParentKind::Object) {
				Object* raw = ObjectRegistry::Resolve(parent.id);
				if (raw && TypeRegistry::IsDerivedFrom(raw->GetType(), SceneObject::GetReflectType())) {
					anchor = Handle<SceneObject>(static_cast<SceneObject*>(raw));
				}
			} else if (parent.kind == FolderParentKind::Folder) {
				anchor = SceneAnnotations::GetFolderAnchor(scene, parent.id);
			}
			if (SceneObject* resolved = anchor.Get()) {
				return resolved->GetChildCount();
			}
			Scene* resolvedScene = scene.Get();
			return resolvedScene ? resolvedScene->GetRootCount() : 0;
		}

		u32 ChildFolderCount(Handle<Scene> scene, FolderParent parent) {
			Vector<FolderInfo> folders{GetMemoryResource(MemTag::Editor)};
			SceneAnnotations::GetChildFolders(scene, parent, folders);
			return static_cast<u32>(folders.size());
		}

		void AppendPlace(Handle<Scene> scene, FolderParent parent, u32& outSlot, u32& outOrder) {
			outSlot = GroupChildCount(scene, parent);
			Vector<FolderInfo> folders{GetMemoryResource(MemTag::Editor)};
			SceneAnnotations::GetChildFolders(scene, parent, folders);
			outOrder = 0;
			for (const FolderInfo& folder : folders) {
				if (folder.slot >= outSlot) {
					++outOrder;
				}
			}
		}

		bool WouldCycle(Handle<Scene> scene, u64 folderId, FolderParent target) {
			FolderParent walk = target;
			for (u32 step = 0; step < 64 && walk.kind == FolderParentKind::Folder; ++step) {
				if (walk.id == folderId) {
					return true;
				}
				FolderInfo info;
				if (!SceneAnnotations::GetFolder(scene, walk.id, info)) {
					return false;
				}
				walk = info.parent;
			}
			return false;
		}

		bool CanPlaceFolder(Handle<Scene> dragScene, u64 dragFolder, Handle<Scene> targetScene,
							FolderParent target, bool sameParentCounts) {
			if (!dragScene.IsValid() || dragScene != targetScene) {
				return false;
			}
			FolderInfo info;
			if (!SceneAnnotations::GetFolder(targetScene, dragFolder, info)) {
				return false;
			}
			if (WouldCycle(targetScene, dragFolder, target)) {
				return false;
			}
			return sameParentCounts || !(info.parent == target);
		}

		Handle<Component> PayloadComponent(const UI::DragPayload& payload) {
			if (payload.type != "Component"_hash || !payload.data.Is<u64>()) {
				return {};
			}
			Object* raw = ObjectRegistry::Resolve(payload.data.As<u64>());
			if (!raw || !TypeRegistry::IsDerivedFrom(raw->GetType(), Component::GetReflectType())) {
				return {};
			}
			return Handle<Component>(static_cast<Component*>(raw));
		}

		StringHash PayloadScriptComponent(const UI::DragPayload& payload) {
			if (payload.type == "BrowserFile"_hash && payload.data.Is<str>()) {
				return AssetBrowserOps::ResolveScriptComponentType(payload.data.As<str>());
			}
			if (payload.type == "Asset"_hash && payload.data.Is<AssetDragData>()) {
				Path path = AssetDatabase::LookupPath(payload.data.As<AssetDragData>().guid);
				std::string_view virtualPath = path.ToString();
				constexpr std::string_view PREFIX = "project://";
				if (virtualPath.starts_with(PREFIX)) {
					return AssetBrowserOps::ResolveScriptComponentType(virtualPath.substr(PREFIX.size()));
				}
			}
			return {};
		}

	} // anonymous namespace

	u64 HierarchyPanel::ComputeStructureVersion() const {
		u64 hash = 0xcbf29ce484222325ull;
		Vector<Handle<Scene>> shown{GetMemoryResource(MemTag::Editor)};
		EditorSceneOps::GetStageScenes(shown);
		Mix(hash, EditorSceneOps::GetPrefabStage().GetInstanceId());
		for (const Handle<Scene>& scene : shown) {
			Scene* resolved = scene.Get();
			if (!resolved) {
				continue;
			}
			Mix(hash, scene.GetInstanceId());
			MixString(hash, resolved->name);
			Mix(hash, EditorSceneOps::IsDirty(scene) ? 1 : 0);
			bool expanded = !_collapsedScenes.contains(scene.GetInstanceId());
			Mix(hash, expanded ? 1 : 0);
			Mix(hash, SceneAnnotations::GetVersion(scene));
			Mix(hash, resolved->GetRootCount());
			if (!_filter.empty()) {
				resolved->ForEachObject(&MixMatchCandidate, &hash);
				continue;
			}
			if (!expanded) {
				continue;
			}
			for (u32 i = 0; i < resolved->GetRootCount(); ++i) {
				Mix(hash, resolved->GetRoot(i).GetInstanceId());
			}
		}
		if (_filter.empty()) {
			for (const ListRow& row : _rows) {
				if (!row.selectable) {
					continue;
				}
				Object* raw = ObjectRegistry::Resolve(row.id);
				if (!raw || !TypeRegistry::IsDerivedFrom(raw->GetType(), SceneObject::GetReflectType())) {
					Mix(hash, 0);
					continue;
				}
				MixObjectRow(hash, *static_cast<SceneObject*>(raw), _expanded.contains(row.id));
			}
		}
		for (u64 id : _expanded) {
			Mix(hash, id);
		}
		for (const auto& entry : _collapsedFolders) {
			for (u64 folderId : entry.second) {
				Mix(hash, FolderKey(entry.first, folderId));
			}
		}
		Mix(hash, _renameTarget);
		if (_renameTarget != 0) {
			Object* renaming = ObjectRegistry::Resolve(_renameTarget);
			bool alive = renaming &&
						 TypeRegistry::IsDerivedFrom(renaming->GetType(), SceneObject::GetReflectType()) &&
						 !static_cast<SceneObject*>(renaming)->IsPendingDestroy();
			Mix(hash, alive ? 1 : 0);
		}
		Mix(hash, _renameFolder);
		return hash;
	}

	u64 HierarchyPanel::ComputeSelectionVersion() const {
		u64 hash = 0xcbf29ce484222325ull;
		for (const Handle<Object>& selected : EditorSelection::GetSelection()) {
			Mix(hash, selected.GetInstanceId());
		}
		return hash;
	}

	void HierarchyPanel::PushSelectionToList() {
		Vector<u64> selected{GetMemoryResource(MemTag::Editor)};
		selected.reserve(EditorSelection::GetSelectionCount());
		for (const Handle<Object>& handle : EditorSelection::GetSelection()) {
			selected.push_back(handle.GetInstanceId());
		}
		_list.SetSelection(selected);
	}

	void HierarchyPanel::PublishSelectionFromList() {
		std::span<const u64> selected = _list.GetSelected();
		bool opened = false;
		for (u64 id : selected) {
			Object* row = ObjectRegistry::Resolve(id);
			if (!row) {
				continue;
			}
			if (!opened) {
				EditorSelection::Select(Handle<Object>(row));
				opened = true;
			} else {
				EditorSelection::AddToSelection(Handle<Object>(row));
			}
		}
		if (!opened) {
			EditorSelection::Clear();
		}
	}

	void HierarchyPanel::ClearSelection() {
		_list.SetSelection({});
		EditorSelection::Clear();
	}

	void HierarchyPanel::OnBuildContent(UI::Context& context, UI::ElementHandle root) {
		UI::Style rootStyle;
		rootStyle.flexDirection = UI::FlexDirection::Column;
		rootStyle.alignItems = UI::Align::Stretch;
		rootStyle.width = UI::Length::Percent(1.0f);
		rootStyle.height = UI::Length::Percent(1.0f);
		context.SetStyle(root, rootStyle);

		_headerHost = context.CreateElement();
		context.AppendChild(root, _headerHost);
		UI::Style hostStyle;
		hostStyle.width = UI::Length::Percent(1.0f);
		hostStyle.flexShrink = 0.0f;
		context.SetStyle(_headerHost, hostStyle);
		BuildHeader(context);

		ListViewDesc list;
		list.layout = ListLayout::Rows;
		list.mode = ListSelectionMode::Multiple;
		list.onSelectionChanged = [this]() { PublishSelectionFromList(); };
		list.onDelete = []() { EditorSceneOps::DeleteSelectedObjects(); };
		list.onDuplicate = []() { EditorSceneOps::DuplicateSelectedObjects(); };
		list.onRename = [this](u64 row) { return BeginRenameRow(row); };
		list.onEmptyRightClick = [this, contextPtr = &context]() {
			Handle<Scene> primary = SceneSystem::GetPrimaryScene();
			if (!primary.IsValid()) {
				return;
			}
			OpenCreateMenu(*contextPtr, primary, Handle<SceneObject>{});
		};
		list.onEmptyClick = [this]() { ClearSelection(); };
		list.onBuildTrailing = [this](UI::Context& context, UI::ElementHandle host) { _gaps.DrainInto(context, host); };
		_list.Build(this, context, root, std::move(list));

		RevealNewlySelected();
		RebuildRows(context);
		_lastVersion = ComputeStructureVersion();
		_lastSelectionVersion = ComputeSelectionVersion();
	}

	void HierarchyPanel::OnTick(f32) {
		UI::Context* context = GetPanelContext();
		if (!context) {
			return;
		}
		_gaps.SyncHits(*context);
		u64 selectionVersion = ComputeSelectionVersion();
		if (selectionVersion != _lastSelectionVersion) {
			RevealNewlySelected();
		}
		u64 version = ComputeStructureVersion();
		if (version != _lastVersion || _forceRebuild) {
			_forceRebuild = false;
			RebuildRows(*context);
			_lastVersion = ComputeStructureVersion();
			_lastSelectionVersion = ComputeSelectionVersion();
			return;
		}
		if (selectionVersion != _lastSelectionVersion) {
			_lastSelectionVersion = selectionVersion;
			PushSelectionToList();
		}
	}

	void HierarchyPanel::OnOpen() {
		Handle<HierarchyPanel> self(this);
		EditorEvents::WorldRemapped.Subscribe(EventBus::GetGlobal(), self, &HierarchyPanel::OnWorldRemapped);
		EditorEvents::PingRequested.Subscribe(EventBus::GetGlobal(), self, &HierarchyPanel::OnPingRequested);
	}

	void HierarchyPanel::OnClose() {
		EventBus::GetGlobal().UnsubscribeAll(GetInstanceId());
	}

	void HierarchyPanel::OnPingRequested(const EditorPingRequestedEvent& event) {
		if (event.target.object == 0) {
			return;
		}
		Object* raw = ObjectRegistry::Resolve(event.target.object);
		Handle<SceneObject> object = raw ? EditorViewportOps::ResolveSceneObject(Handle<Object>(raw))
										 : Handle<SceneObject>{};
		SceneObject* live = object.Get();
		if (!live || live->IsPendingDestroy()) {
			return;
		}
		Vector<Handle<Scene>> shown{GetMemoryResource(MemTag::Editor)};
		EditorSceneOps::GetStageScenes(shown);
		const u64 sceneId = live->GetScene().GetInstanceId();
		bool drawn = false;
		for (const Handle<Scene>& scene : shown) {
			drawn = drawn || scene.GetInstanceId() == sceneId;
		}
		if (!drawn) {
			return;
		}
		Reveal(*live);
		_list.PingRow(object.GetInstanceId());
		_forceRebuild = true;
		PanelManager::RevealPanel(Handle<EditorPanel>(this));
	}

	void HierarchyPanel::OnWorldRemapped(const EditorWorldRemappedEvent& event) {
		HashMap<u64, u64> moved{GetMemoryResource(MemTag::Editor)};
		moved.reserve(event.ids.size());
		for (const EditorIdRemap& row : event.ids) {
			moved[row.oldId] = row.newId;
		}
		auto follow = [&moved](u64 id) -> u64 {
			auto it = moved.find(id);
			return it != moved.end() ? it->second : 0;
		};
		auto rekey = [&follow](HashSet<u64>& set) {
			HashSet<u64> next{GetMemoryResource(MemTag::Editor)};
			for (u64 id : set) {
				if (u64 replacement = follow(id); replacement != 0) {
					next.insert(replacement);
				}
			}
			set = std::move(next);
		};

		rekey(_expanded);
		rekey(_collapsedScenes);

		HashMap<u64, HashSet<u64>> folders{GetMemoryResource(MemTag::Editor)};
		for (auto& entry : _collapsedFolders) {
			if (u64 scene = follow(entry.first); scene != 0) {
				folders.emplace(scene, std::move(entry.second));
			}
		}
		_collapsedFolders = std::move(folders);

		_renameTarget = follow(_renameTarget);
		_renameFolderScene = follow(_renameFolderScene);
		if (_renameFolderScene == 0) {
			_renameFolder = 0;
		}
	}

	void HierarchyPanel::BuildStageBreadcrumb(UI::Context& context, Handle<Scene> stage) {
		Scene* resolved = stage.Get();
		if (!resolved) {
			return;
		}
		ListRow row;
		row.id = 0;
		row.selectable = false;
		row.label = "Scenes";
		row.onClick = [](UI::Context& context) { SceneFileFlows::LeavePrefabStage(context); };
		row.onDecorate = [](UI::Context& context, const UI::Widgets::TreeRowParts& parts) {
			context.AddClass(parts.row, EditorTheme::Class::StageBreadcrumb);
			EditorIcons::SetRowGutter(context, parts, false, false, TextDim(), EditorIcons::Glyph::Back, TextDim(), PictureColumnPx());
		};
		PushRow(std::move(row));
	}

	void HierarchyPanel::BuildHeader(UI::Context& context) {
		context.ClearChildren(_headerHost);

		PanelHeader::Desc desc;
		desc.addLabel = "Create";
		desc.addTooltip = "Create";
		desc.onAdd = [this]() {
			Handle<Scene> primary = SceneSystem::GetPrimaryScene();
			UI::Context* live = GetPanelContext();
			if (!primary.IsValid() || !live) {
				return;
			}
			OpenCreateMenu(*live, primary, Handle<SceneObject>{});
		};
		desc.searchOpen = _searchOpen;
		desc.searchText = _filter;
		desc.focusSearch = _focusSearch;
		desc.onSearchToggled = [this](bool open) {
			_searchOpen = open;
			_focusSearch = open;
			if (UI::Context* live = GetPanelContext()) {
				BuildHeader(*live);
			}
			_forceRebuild = true;
		};
		desc.onSearchChanged = [this](std::string_view text) {
			if (_filter == text) {
				return;
			}
			_filter = str(text);
			_forceRebuild = true;
		};
		PanelHeader::Build(context, _headerHost, desc);
		_focusSearch = false;
	}

	void HierarchyPanel::RebuildRows(UI::Context& context) {
		_rows.clear();
		_gaps.BeginBuild(context);
		Handle<FontAsset> font = UISystem::GetDefaultFont();

		Vector<Handle<Scene>> shown{GetMemoryResource(MemTag::Editor)};
		EditorSceneOps::GetStageScenes(shown);
		if (Handle<Scene> stage = EditorSceneOps::GetPrefabStage(); stage.IsValid()) {
			BuildStageBreadcrumb(context, stage);
		}

		for (const Handle<Scene>& scene : shown) {
			Scene* resolved = scene.Get();
			if (!resolved) {
				continue;
			}

			u64 sceneId = scene.GetInstanceId();
			bool expanded = !_collapsedScenes.contains(sceneId);
			str title = resolved->name + (EditorSceneOps::IsDirty(scene) ? "*" : "");

			ListRow desc;
			desc.selectable = false;
			desc.hasChildren = resolved->GetRootCount() > 0;
			desc.expanded = expanded;
			desc.label = title;
			desc.id = sceneId;
			if (desc.hasChildren) {
				desc.onClick = [this, sceneId, expanded](UI::Context&) { SetSceneExpanded(sceneId, !expanded); };
			}
			desc.onToggleExpand = [this, sceneId](bool wanted) { SetSceneExpanded(sceneId, wanted); };
			desc.onRightClick = [this, scene](UI::Context& context) {
				OpenSceneContextMenu(context, scene);
			};
			desc.onDragOver = [scene](const UI::DragPayload& payload) {
				if (Path scenePath; PayloadScenePath(payload, scenePath)) {
					return true;
				}
				if (PayloadPrefab(payload, scene).IsValid() || SceneAssetDrops::Accepts(payload, scene)) {
					return true;
				}
				Vector<Handle<SceneObject>> dragged{GetMemoryResource(MemTag::Editor)};
				PayloadObjects(payload, dragged);
				if (!dragged.empty()) {
					return std::any_of(dragged.begin(), dragged.end(), [](Handle<SceneObject> object) {
						return SceneAnnotations::GetFolderOf(object) != 0;
					});
				}
				Handle<Scene> dragScene;
				u64 folder = 0;
				return PayloadFolder(payload, dragScene, folder) &&
					   CanPlaceFolder(dragScene, folder, scene, FolderParent::Roots(), false);
			};
			desc.onDrop = [this, scene, sceneId](const UI::DragPayload& payload, Vec2) {
				if (Path scenePath; PayloadScenePath(payload, scenePath)) {
					SceneFileFlows::AddScenePath(scenePath);
					return;
				}
				if (Guid prefab = PayloadPrefab(payload, scene); prefab.IsValid()) {
					SetSceneExpanded(sceneId, true);
					UndoSystem::Push(MakeUnique<InstantiatePrefabCommand>(prefab, scene, Handle<SceneObject>{}));
					return;
				}
				if (SceneAssetDrops::Accepts(payload, scene)) {
					SetSceneExpanded(sceneId, true);
					SceneAssetDrops::Drop(payload, scene, {});
					return;
				}
				Vector<Handle<SceneObject>> dragged{GetMemoryResource(MemTag::Editor)};
				PayloadObjects(payload, dragged);
				if (!dragged.empty()) {
					SetSceneExpanded(sceneId, true);
					SceneAnnotations::FileObjects(scene, {dragged.data(), dragged.size()}, 0);
					return;
				}
				Handle<Scene> dragScene;
				u64 folder = 0;
				if (PayloadFolder(payload, dragScene, folder) &&
					CanPlaceFolder(dragScene, folder, scene, FolderParent::Roots(), false)) {
					u32 slot = 0;
					u32 order = 0;
					AppendPlace(scene, FolderParent::Roots(), slot, order);
					SetSceneExpanded(sceneId, true);
					SceneAnnotations::MoveFolder(scene, folder, FolderParent::Roots(), slot, order);
				}
			};
			desc.onDecorate = [font, title, hasChildren = desc.hasChildren,
							   expanded](UI::Context& context, const UI::Widgets::TreeRowParts& header) {
				context.AddClass(header.row, EditorTheme::Class::SectionHeader);
				EditorIcons::SetRowGutter(context, header, hasChildren, expanded, TextDim(), EditorIcons::Glyph::AssetScene, TextDim(), PictureColumnPx());
				context.SetText(header.labelElement, font, RowFont(), TextStrong(), title);
			};
			PushRow(std::move(desc));

			if (!expanded) {
				continue;
			}
			if (!_filter.empty()) {
				BuildMatchingRows(context, scene);
				continue;
			}
			BuildGroup(context, scene, FolderParent::Roots(), 0, Handle<SceneObject>{}, 1);
		}

		_list.SetRows(_rows);
		PushSelectionToList();
	}

	void HierarchyPanel::PushRow(ListRow&& row) {
		row.onBuildLeading = _gaps.Take();
		_rows.push_back(std::move(row));
	}

	void HierarchyPanel::BuildMatchingRows(UI::Context& context, Handle<Scene> scene) {
		Scene* resolved = scene.Get();
		if (!resolved) {
			return;
		}
		auto visit = [this, &context](auto&& self, Handle<SceneObject> handle) -> void {
			SceneObject* object = handle.Get();
			if (!object || object->IsPendingDestroy()) {
				return;
			}
			if (AssetBrowserOps::MatchesFilter(object->name, _filter)) {
				BuildObjectRow(context, handle, 1);
			}
			for (u32 i = 0; i < object->GetChildCount(); ++i) {
				self(self, object->GetChild(i));
			}
		};
		for (u32 i = 0; i < resolved->GetRootCount(); ++i) {
			visit(visit, resolved->GetRoot(i));
		}
	}

	void HierarchyPanel::SetSceneExpanded(u64 sceneId, bool expanded) {
		if (expanded) {
			_collapsedScenes.erase(sceneId);
		} else {
			_collapsedScenes.insert(sceneId);
		}
	}

	void HierarchyPanel::Reveal(SceneObject& object) {
		Handle<Scene> scene = object.GetScene();
		u64 sceneId = scene.GetInstanceId();
		SetSceneExpanded(sceneId, true);
		Handle<SceneObject> current(&object);
		while (current.IsValid()) {
			for (u64 folder = SceneAnnotations::GetFolderOf(current); folder != 0;) {
				SetFolderCollapsed(sceneId, folder, false);
				FolderInfo info;
				if (!SceneAnnotations::GetFolder(scene, folder, info) || info.parent.kind != FolderParentKind::Folder) {
					break;
				}
				folder = info.parent.id;
			}
			SceneObject* resolved = current.Get();
			current = resolved ? resolved->GetParent() : Handle<SceneObject>{};
			if (current.IsValid()) {
				_expanded.insert(current.GetInstanceId());
			}
		}
	}

	void HierarchyPanel::RevealNewlySelected() {
		HashSet<u64> selected{GetMemoryResource(MemTag::Editor)};
		for (const Handle<Object>& handle : EditorSelection::GetSelection()) {
			u64 id = handle.GetInstanceId();
			selected.insert(id);
			if (_lastSelection.contains(id)) {
				continue;
			}
			Object* raw = handle.Get();
			if (raw && TypeRegistry::IsDerivedFrom(raw->GetType(), SceneObject::GetReflectType())) {
				Reveal(*static_cast<SceneObject*>(raw));
			}
		}
		_lastSelection = std::move(selected);
	}

	u64 HierarchyPanel::FolderKey(u64 sceneId, u64 folderId) {
		u64 hash = 0xcbf29ce484222325ull;
		Mix(hash, sceneId);
		Mix(hash, folderId);
		return hash;
	}

	bool HierarchyPanel::IsFolderCollapsed(u64 sceneId, u64 folderId) const {
		auto it = _collapsedFolders.find(sceneId);
		return it != _collapsedFolders.end() && it->second.contains(folderId);
	}

	void HierarchyPanel::SetFolderCollapsed(u64 sceneId, u64 folderId, bool collapsed) {
		if (collapsed) {
			auto it = _collapsedFolders.find(sceneId);
			if (it == _collapsedFolders.end()) {
				it = _collapsedFolders.emplace(sceneId, HashSet<u64>{GetMemoryResource(MemTag::Editor)}).first;
			}
			it->second.insert(folderId);
			return;
		}
		auto it = _collapsedFolders.find(sceneId);
		if (it == _collapsedFolders.end()) {
			return;
		}
		it->second.erase(folderId);
		if (it->second.empty()) {
			_collapsedFolders.erase(it);
		}
	}

	void HierarchyPanel::BuildGroup(UI::Context& context, Handle<Scene> scene, FolderParent parent,
									u64 owningFolder, Handle<SceneObject> anchor, u32 depth) {
		Vector<FolderInfo> folders{GetMemoryResource(MemTag::Editor)};
		SceneAnnotations::GetChildFolders(scene, parent, folders);

		SceneObject* anchorResolved = anchor.Get();
		Scene* sceneResolved = scene.Get();
		u32 count = anchorResolved ? anchorResolved->GetChildCount()
								   : (sceneResolved ? sceneResolved->GetRootCount() : 0);

		auto foldersAtSlot = [&folders](u32 slot) {
			u32 total = 0;
			for (const FolderInfo& folder : folders) {
				if (folder.slot == slot) {
					++total;
				}
			}
			return total;
		};

		size_t nextFolder = 0;
		auto flushFoldersUpTo = [&](u32 upTo) {
			while (nextFolder < folders.size() && folders[nextFolder].slot <= upTo) {
				const FolderInfo& folder = folders[nextFolder];
				PendInsertStrip(context, scene, parent, owningFolder, anchor, folder.slot, folder.order);
				BuildFolderRow(context, scene, folder, anchor, depth);
				++nextFolder;
			}
		};

		for (u32 i = 0; i < count; ++i) {
			Handle<SceneObject> child = anchorResolved ? anchorResolved->GetChild(i) : sceneResolved->GetRoot(i);
			if (SceneAnnotations::GetFolderOf(child) != owningFolder) {
				continue;
			}
			flushFoldersUpTo(i);
			PendInsertStrip(context, scene, parent, owningFolder, anchor, i, foldersAtSlot(i));
			BuildObjectRow(context, child, depth);
		}
		flushFoldersUpTo(0xFFFFFFFFu);
		PendInsertStrip(context, scene, parent, owningFolder, anchor, count, foldersAtSlot(count));
	}

	void HierarchyPanel::BuildFolderRow(UI::Context& context, Handle<Scene> scene, const FolderInfo& folder,
										Handle<SceneObject> anchor, u32 depth) {
		u64 sceneId = scene.GetInstanceId();
		u64 key = FolderKey(sceneId, folder.id);
		u64 folderId = folder.id;
		bool expanded = !IsFolderCollapsed(sceneId, folderId);

		if (_renameFolder == folderId && _renameFolderScene == sceneId) {
			str name = folder.name;
			_gaps.PendInFlow([this, scene, folderId, depth, name](UI::Context& context, UI::ElementHandle host) {
				UI::ElementHandle wrap = context.CreateElement();
				context.AppendChild(host, wrap);
				UI::Style wrapStyle;
				wrapStyle.flexDirection = UI::FlexDirection::Row;
				wrapStyle.padding = {4.0f + static_cast<f32>(depth) * 14.0f, 1.0f, 4.0f, 1.0f};
				context.SetStyle(wrap, wrapStyle);

				UI::Widgets::TextFieldDesc field;
				field.text = name;
				field.font = UISystem::GetDefaultFont();
				field.fontSizePx = RowFont();
				field.onCommit = [this, scene, folderId](std::string_view text) {
					if (!text.empty()) {
						SceneAnnotations::RenameFolder(scene, folderId, text);
					}
					_renameFolder = 0;
					_renameFolderScene = 0;
				};
				UI::ElementHandle fieldElement = UI::Widgets::TextField(context, wrap, field);
				if (_renameNeedsFocus) {
					_renameNeedsFocus = false;
					context.SetFocus(fieldElement);
				}
				auto chained = context.Events(fieldElement).onFocusLost;
				context.Events(fieldElement).onFocusLost = [this, chained]() {
					if (chained) {
						chained();
					}
					_renameFolder = 0;
					_renameFolderScene = 0;
				};
			});
			return;
		}

		ListRow desc;
		desc.depth = depth;
		desc.selectable = false;
		desc.hasChildren = true;
		desc.expanded = expanded;
		desc.label = folder.name;
		desc.id = key;
		desc.onToggleExpand = [this, sceneId, folderId](bool wanted) {
			SetFolderCollapsed(sceneId, folderId, !wanted);
		};
		desc.onActivate = [this, sceneId, folderId](UI::Context&) {
			_renameFolder = folderId;
			_renameFolderScene = sceneId;
			_renameNeedsFocus = true;
		};
		desc.onRightClick = [this, scene, folderId](UI::Context& context) {
			OpenFolderContextMenu(context, scene, folderId);
		};
		desc.onDragRow = [scene, folderId](Vec2) {
			if (UISystem::IsDragPayloadActive()) {
				return;
			}
			FolderInfo live;
			if (!SceneAnnotations::GetFolder(scene, folderId, live)) {
				return;
			}
			UI::DragPayload payload;
			payload.type = "SceneFolder"_hash;
			payload.data = Variant(SceneFolderDragData{scene.GetInstanceId(), folderId});
			payload.display = live.name;
			UISystem::BeginDragPayload(payload);
		};
		FolderParent asParent = FolderParent::In(folderId);
		desc.onDragOver = [scene, asParent](const UI::DragPayload& payload) {
			if (Path scenePath; PayloadScenePath(payload, scenePath)) {
				return true;
			}
			if (PayloadPrefab(payload, scene).IsValid() || SceneAssetDrops::Accepts(payload, scene)) {
				return true;
			}
			Vector<Handle<SceneObject>> objects{GetMemoryResource(MemTag::Editor)};
			PayloadObjects(payload, objects);
			if (!objects.empty()) {
				return true;
			}
			Handle<Scene> dragScene;
			u64 dragged = 0;
			return PayloadFolder(payload, dragScene, dragged) &&
				   CanPlaceFolder(dragScene, dragged, scene, asParent, false);
		};
		desc.onDrop = [scene, asParent, folderId](const UI::DragPayload& payload, Vec2) {
			if (Path scenePath; PayloadScenePath(payload, scenePath)) {
				SceneFileFlows::AddScenePath(scenePath);
				return;
			}
			if (Guid prefab = PayloadPrefab(payload, scene); prefab.IsValid()) {
				UndoSystem::BeginGroup("Instantiate Prefab");
				auto command = MakeUnique<InstantiatePrefabCommand>(prefab, scene, Handle<SceneObject>{});
				InstantiatePrefabCommand* raw = command.get();
				UndoSystem::Push(std::move(command));
				if (Handle<SceneObject> created = raw->GetCreated(); created.IsValid()) {
					Handle<SceneObject> one[] = {created};
					SceneAnnotations::FileObjects(scene, {one, 1}, folderId);
				}
				UndoSystem::EndGroup();
				return;
			}
			if (SceneAssetDrops::Accepts(payload, scene)) {
				UndoSystem::BeginGroup("Create Object");
				if (Handle<SceneObject> created = SceneAssetDrops::Drop(payload, scene, {}); created.IsValid()) {
					Handle<SceneObject> one[] = {created};
					SceneAnnotations::FileObjects(scene, {one, 1}, folderId);
				}
				UndoSystem::EndGroup();
				return;
			}
			Vector<Handle<SceneObject>> objects{GetMemoryResource(MemTag::Editor)};
			PayloadObjects(payload, objects);
			if (!objects.empty()) {
				SceneAnnotations::FileObjects(scene, {objects.data(), objects.size()}, folderId);
				return;
			}
			Handle<Scene> dragScene;
			u64 dragged = 0;
			if (PayloadFolder(payload, dragScene, dragged) &&
				CanPlaceFolder(dragScene, dragged, scene, asParent, false)) {
				u32 slot = 0;
				u32 order = 0;
				AppendPlace(scene, asParent, slot, order);
				SceneAnnotations::MoveFolder(scene, dragged, asParent, slot, order);
			}
		};

		desc.onDecorate = [label = folder.label, expanded](UI::Context& context, const UI::Widgets::TreeRowParts& row) {
			ApplyLabelTint(context, row.row, label);
			EditorIcons::SetRowGutter(context, row, true, expanded, TextDim(),
									  expanded ? EditorIcons::Glyph::FolderOpen : EditorIcons::Glyph::Folder,
									  EditorTheme::GetColor(ColorToken::AssetFolder), PictureColumnPx());
		};
		PushRow(std::move(desc));

		if (expanded) {
			BuildGroup(context, scene, asParent, folderId, anchor, depth + 1);
		}
	}

	void HierarchyPanel::PendInsertStrip(UI::Context& context, Handle<Scene> scene, FolderParent parent,
										 u64 owningFolder, Handle<SceneObject> anchor, u32 slot, u32 order) {
		EditorInsertStripDesc desc;

		desc.onDragOver = [scene, parent, anchor](const UI::DragPayload& payload) {
			Vector<Handle<SceneObject>> objects{GetMemoryResource(MemTag::Editor)};
			PayloadObjects(payload, objects);
			if (!objects.empty()) {
				return EditorSceneOps::CanMoveObjects({objects.data(), objects.size()}, anchor, scene);
			}
			Handle<Scene> dragScene;
			u64 dragged = 0;
			return PayloadFolder(payload, dragScene, dragged) &&
				   CanPlaceFolder(dragScene, dragged, scene, parent, true);
		};
		desc.onDrop = [scene, parent, owningFolder, anchor, slot, order](const UI::DragPayload& payload, Vec2) {
			Vector<Handle<SceneObject>> objects{GetMemoryResource(MemTag::Editor)};
			PayloadObjects(payload, objects);
			if (!objects.empty()) {
				UndoSystem::BeginGroup(objects.size() > 1 ? "Move Objects" : "Move Object");
				if (EditorSceneOps::MoveObjects({objects.data(), objects.size()}, anchor, scene, slot)) {
					SceneAnnotations::FileObjects(scene, {objects.data(), objects.size()}, owningFolder);
				}
				UndoSystem::EndGroup();
				return;
			}
			Handle<Scene> dragScene;
			u64 dragged = 0;
			if (!PayloadFolder(payload, dragScene, dragged) ||
				!CanPlaceFolder(dragScene, dragged, scene, parent, true)) {
				return;
			}
			FolderInfo info;
			u32 place = order;
			if (SceneAnnotations::GetFolder(scene, dragged, info) && info.parent == parent && info.slot == slot &&
				info.order < order) {
				--place;
			}
			SceneAnnotations::MoveFolder(scene, dragged, parent, slot, place);
		};
		_gaps.PendStrip(std::move(desc));
	}

	void HierarchyPanel::BuildObjectRow(UI::Context& context, Handle<SceneObject> objectHandle, u32 depth) {
		SceneObject* object = objectHandle.Get();
		if (!object || object->IsPendingDestroy()) {
			return;
		}
		Handle<FontAsset> font = UISystem::GetDefaultFont();
		u64 id = objectHandle.GetInstanceId();
		bool hasChildren = object->GetChildCount() > 0 ||
						   ChildFolderCount(object->GetScene(), FolderParent::Under(id)) > 0;
		bool expanded = hasChildren && _expanded.contains(id);

		if (_renameTarget == id) {
			str name = object->name;
			_gaps.PendInFlow([this, objectHandle, depth, font, name](UI::Context& context, UI::ElementHandle host) {
				UI::ElementHandle wrap = context.CreateElement();
				context.AppendChild(host, wrap);
				UI::Style wrapStyle;
				wrapStyle.flexDirection = UI::FlexDirection::Row;
				wrapStyle.padding = {4.0f + static_cast<f32>(depth) * 14.0f, 1.0f, 4.0f, 1.0f};
				context.SetStyle(wrap, wrapStyle);

				UI::Widgets::TextFieldDesc field;
				field.text = name;
				field.font = font;
				field.fontSizePx = RowFont();
				field.onCommit = [this, objectHandle](std::string_view text) {
					SceneObject* live = objectHandle.Get();
					if (live && !text.empty() && text != live->name) {
						UndoSystem::Push(MakeUnique<PropertySetCommand>(
							Handle<Object>(live), "name"_hash,
							Variant(live->name), Variant(str(text)), "Rename"));
					}
					_renameTarget = 0;
				};
				UI::ElementHandle fieldElement = UI::Widgets::TextField(context, wrap, field);
				if (_renameNeedsFocus) {
					_renameNeedsFocus = false;
					context.SetFocus(fieldElement);
				}
				auto chained = context.Events(fieldElement).onFocusLost;
				context.Events(fieldElement).onFocusLost = [this, chained]() {
					if (chained) {
						chained();
					}
					_renameTarget = 0;
				};
			});
			return;
		}

		ListRow desc;
		desc.id = id;
		desc.depth = depth;
		desc.hasChildren = hasChildren;
		desc.expanded = expanded;
		desc.label = object->name;
		desc.onToggleExpand = [this, id](bool wanted) {
			if (wanted) {
				_expanded.insert(id);
			} else {
				_expanded.erase(id);
			}
		};
		desc.onActivate = [id](UI::Context&) {
			EditorEvents::FollowRequested.Publish(EventBus::GetGlobal(), {id});
		};
		desc.onRightClick = [this, objectHandle](UI::Context& context) {
			OpenRowContextMenu(context, objectHandle);
		};
		desc.onDragRow = [this, objectHandle](Vec2) {
			if (UISystem::IsDragPayloadActive() || !objectHandle.Get()) {
				return;
			}
			Vector<Handle<SceneObject>> targets{GetMemoryResource(MemTag::Editor)};
			GetRowTargets(objectHandle, targets);
			Vector<Handle<SceneObject>> moving{GetMemoryResource(MemTag::Editor)};
			EditorSceneOps::GetOutermostInTreeOrder({targets.data(), targets.size()}, moving);
			if (moving.empty()) {
				return;
			}
			SceneObjectDragData data;
			for (Handle<SceneObject> object : moving) {
				data.objects.push_back(object.GetInstanceId());
			}
			UI::DragPayload payload;
			payload.type = "SceneObject"_hash;
			payload.display = moving.size() == 1 ? moving.front().Get()->name : str("<Multiple>");
			payload.data = Variant(std::move(data));
			UISystem::BeginDragPayload(payload);
		};
		desc.onDragOver = [objectHandle, this](const UI::DragPayload& payload) {
			SceneObject* host = objectHandle.Get();
			if (host && (PayloadPrefab(payload, host->GetScene()).IsValid() || SceneAssetDrops::Accepts(payload, host->GetScene()))) {
				return true;
			}
			if (payload.type == "Component"_hash) {
				return EditorSceneOps::ClassifyComponentDrop(PayloadComponent(payload), objectHandle) ==
					   EditorSceneOps::ComponentDropKind::Move;
			}
			if (StringHash scriptType = PayloadScriptComponent(payload); scriptType.GetValue() != 0) {
				return host != nullptr && !host->IsPendingDestroy();
			}
			if (payload.type == "SceneFolder"_hash) {
				Handle<Scene> dragScene;
				u64 dragged = 0;
				SceneObject* target = objectHandle.Get();
				return target != nullptr && PayloadFolder(payload, dragScene, dragged) &&
					   CanPlaceFolder(dragScene, dragged, target->GetScene(),
									  FolderParent::Under(objectHandle.GetInstanceId()), false);
			}
			Vector<Handle<SceneObject>> objects{GetMemoryResource(MemTag::Editor)};
			PayloadObjects(payload, objects);
			return EditorSceneOps::CanMoveObjects({objects.data(), objects.size()}, objectHandle, {});
		};
		desc.onDrop = [this, objectHandle, id](const UI::DragPayload& payload, Vec2) {
			SceneObject* host = objectHandle.Get();
			if (Guid prefab = host ? PayloadPrefab(payload, host->GetScene()) : Guid{}; prefab.IsValid()) {
				_expanded.insert(id);
				UndoSystem::Push(MakeUnique<InstantiatePrefabCommand>(prefab, host->GetScene(), objectHandle));
				return;
			}
			if (host && SceneAssetDrops::Accepts(payload, host->GetScene())) {
				_expanded.insert(id);
				SceneAssetDrops::Drop(payload, host->GetScene(), objectHandle);
				return;
			}
			if (payload.type == "Component"_hash) {
				EditorSceneOps::DropComponentOn(PayloadComponent(payload), objectHandle,
												EditorSceneOps::COMPONENT_INDEX_END);
				return;
			}
			if (StringHash scriptType = PayloadScriptComponent(payload); scriptType.GetValue() != 0) {
				if (host && !host->IsPendingDestroy()) {
					UndoSystem::Push(MakeUnique<AddComponentCommand>(objectHandle, scriptType));
				}
				return;
			}
			if (payload.type == "SceneFolder"_hash) {
				Handle<Scene> dragScene;
				u64 dragged = 0;
				SceneObject* target = objectHandle.Get();
				FolderParent into = FolderParent::Under(objectHandle.GetInstanceId());
				if (target && PayloadFolder(payload, dragScene, dragged) &&
					CanPlaceFolder(dragScene, dragged, target->GetScene(), into, false)) {
					u32 slot = 0;
					u32 order = 0;
					AppendPlace(target->GetScene(), into, slot, order);
					_expanded.insert(id);
					SceneAnnotations::MoveFolder(target->GetScene(), dragged, into, slot, order);
				}
				return;
			}
			Vector<Handle<SceneObject>> objects{GetMemoryResource(MemTag::Editor)};
			PayloadObjects(payload, objects);
			SceneObject* target = objectHandle.Get();
			if (!target || !EditorSceneOps::CanMoveObjects({objects.data(), objects.size()}, objectHandle, {})) {
				return;
			}
			_expanded.insert(id);
			EditorSceneOps::MoveObjects({objects.data(), objects.size()}, objectHandle, {}, target->GetChildCount());
		};

		bool linked = !EditorSceneOps::GetPrefabStage().IsValid() && SceneSystem::IsPrefabLinked(objectHandle);
		bool inScene = object->IsActiveInHierarchy();
		desc.onDecorate = [objectHandle, linked, inScene,
						   hasChildren = desc.hasChildren, expanded](UI::Context& context, const UI::Widgets::TreeRowParts& row) {
			ApplyLabelTint(context, row.row, SceneAnnotations::GetObjectLabel(objectHandle));
			std::string_view badge;
			if (!linked) {
				if (const SceneObject* live = objectHandle.Get()) {
					badge = RowBadge(*live);
				}
			}
			UI::ElementHandle glyph =
				!badge.empty()
					? EditorIcons::SetRowGutterMark(context, row, hasChildren, expanded, TextDim(), badge,
													EditorIcons::ComponentIcons::SizePx(), PictureColumnPx())
					: EditorIcons::SetRowGutter(
						  context, row, hasChildren, expanded, TextDim(),
						  linked ? EditorIcons::Glyph::AssetPrefab : EditorIcons::Glyph::SceneObject,
						  linked ? EditorTheme::GetColor(ColorToken::AssetPrefab) : TextDim(), PictureColumnPx());
			if (!inScene) {
				ApplyOffState(context, glyph, row);
			}

			AddActiveToggle(context, row, objectHandle);
		};
		PushRow(std::move(desc));

		if (expanded) {
			BuildGroup(context, object->GetScene(), FolderParent::Under(id), 0, objectHandle, depth + 1);
		}
	}

	void HierarchyPanel::GetRowTargets(Handle<SceneObject> clicked, Vector<Handle<SceneObject>>& out) const {
		SceneObject* object = clicked.Get();
		if (!object) {
			return;
		}
		Vector<u64> selection{GetMemoryResource(MemTag::Editor)};
		selection.reserve(EditorSelection::GetSelectionCount());
		for (const Handle<Object>& handle : EditorSelection::GetSelection()) {
			selection.push_back(handle.GetInstanceId());
		}
		Vector<u64> targets{GetMemoryResource(MemTag::Editor)};
		EditorSelectionOps::ResolveTargets(selection, clicked.GetInstanceId(), targets);
		for (u64 id : targets) {
			Object* raw = ObjectRegistry::Resolve(id);
			if (raw && TypeRegistry::IsDerivedFrom(raw->GetType(), SceneObject::GetReflectType())) {
				out.push_back(Handle<SceneObject>(static_cast<SceneObject*>(raw)));
			}
		}
		if (out.empty()) {
			out.push_back(clicked);
		}
	}

	UI::Widgets::MenuItem HierarchyPanel::BuildColorMenu(const Vector<Handle<SceneObject>>& targets) {
		UI::Widgets::MenuItem colors;
		colors.label = "Color";
		colors.separatorBefore = true;
		for (u8 i = 0; i < static_cast<u8>(LabelColor::Count); ++i) {
			LabelColor color = static_cast<LabelColor>(i);
			UI::Widgets::MenuItem entry;
			entry.label = str(SceneAnnotations::GetLabelName(color));
			entry.separatorBefore = (color == LabelColor::Red);
			entry.onSelect = [color, targets]() {
				SceneAnnotations::SetObjectLabels({targets.data(), targets.size()}, color);
			};
			colors.children.push_back(std::move(entry));
		}
		return colors;
	}

	void HierarchyPanel::OpenRowContextMenu(UI::Context& context, Handle<SceneObject> objectHandle) {
		SceneObject* object = objectHandle.Get();
		if (!object) {
			return;
		}
		Handle<Scene> scene = object->GetScene();

		Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
		items.push_back(EditorSceneOps::BuildCreateSubmenu("Create Child", scene, objectHandle));
		FocusAfterCreate(items.back().children);
		Vector<Handle<SceneObject>> targets{GetMemoryResource(MemTag::Editor)};
		GetRowTargets(objectHandle, targets);
		UI::Widgets::MenuItem parentItem;
		parentItem.label = "Create Empty Parent";
		parentItem.enabled = EditorSceneOps::CanCreateEmptyParent({targets.data(), targets.size()});
		parentItem.onSelect = [this, targets]() {
			Handle<SceneObject> active;
			if (Object* raw = EditorSelection::GetActive().Get();
				raw && TypeRegistry::IsDerivedFrom(raw->GetType(), SceneObject::GetReflectType())) {
				active = Handle<SceneObject>(static_cast<SceneObject*>(raw));
			}
			const Handle<SceneObject> created = EditorSceneOps::CreateEmptyParent({targets.data(), targets.size()}, active);
			if (created.IsValid()) {
				_expanded.insert(created.GetInstanceId());
			}
		};
		items.push_back(std::move(parentItem));
		items.push_back({"Duplicate", EditorInputOps::ChordLabel(EditorInputOps::Shortcut::DuplicateSelection), true,
						 false, [targets]() { EditorSceneOps::DuplicateObjects({targets.data(), targets.size()}); }});
		items.push_back({"Rename", "", true, false, [this, objectHandle]() {
							 _renameTarget = objectHandle.GetInstanceId();
							 _renameNeedsFocus = true;
						 }});
		items.push_back(BuildCreatePrefabItem(context, objectHandle));
		items.push_back(BuildCreatePrefabVariantItem(context, objectHandle));
		items.push_back({"Delete", EditorInputOps::ChordLabel(EditorInputOps::Shortcut::DeleteSelection), true, true, [targets]() {
							 EditorSceneOps::DeleteObjects({targets.data(), targets.size()});
						 }});

		items.push_back(BuildCreateFolderItem(targets));
		items.push_back(BuildColorMenu(targets));

		UI::Widgets::OpenMenu(context, context.GetPointerPosition(),
							  {items.data(), items.size()}, UISystem::GetDefaultFont(), MenuFont());
	}

	UI::Widgets::MenuItem HierarchyPanel::BuildCreatePrefabItem(UI::Context& context, Handle<SceneObject> objectHandle) {
		UI::Widgets::MenuItem item;
		item.label = "Create Prefab...";
		item.enabled = !SceneSystem::IsPrefabLinked(objectHandle);
		if (!item.enabled) {
			return item;
		}
		UI::Context* contextPtr = &context;
		item.onSelect = [contextPtr, objectHandle]() {
			SceneObject* live = objectHandle.Get();
			if (!live) {
				return;
			}
			SceneFileFlows::OpenTextInputPopup(
				*contextPtr, "Create Prefab", SeedPrefabPath(*live),
				[objectHandle](const str& path) {
					EditorSceneOps::CreatePrefabFrom(objectHandle, Path(path));
				},
				"Create (cannot be undone)");
		};
		return item;
	}

	str HierarchyPanel::SeedPrefabPath(const SceneObject& live) {
		str root = AssetBrowserOps::GetDefaultCreateFolder().ToString();
		str directory = (root.empty() ? str("project://Assets") : root) + "/Prefabs/";
		return directory + AssetBrowserOps::MakeUniqueName(Path(directory), live.name + ".prefab");
	}

	UI::Widgets::MenuItem HierarchyPanel::BuildCreatePrefabVariantItem(UI::Context& context, Handle<SceneObject> objectHandle) {
		UI::Widgets::MenuItem item;
		item.label = "Create Prefab Variant...";
		item.enabled = EditorSceneOps::CanCreatePrefabVariantFrom(objectHandle);
		if (!item.enabled) {
			return item;
		}
		UI::Context* contextPtr = &context;
		item.onSelect = [contextPtr, objectHandle]() {
			SceneObject* live = objectHandle.Get();
			if (!live) {
				return;
			}
			SceneFileFlows::OpenTextInputPopup(
				*contextPtr, "Create Prefab Variant", SeedPrefabPath(*live),
				[objectHandle](const str& path) {
					EditorSceneOps::CreatePrefabVariantFrom(objectHandle, Path(path));
				},
				"Create (cannot be undone)");
		};
		return item;
	}

	UI::Widgets::MenuItem HierarchyPanel::BuildCreateFolderItem(const Vector<Handle<SceneObject>>& targets) {
		Handle<Scene> scene;
		FolderParent parent;
		bool possible = SceneAnnotations::CanCreateFolderFrom({targets.data(), targets.size()}, scene, parent);

		UI::Widgets::MenuItem item;
		item.label = "New Folder from Selection";
		item.enabled = possible;
		item.separatorBefore = true;
		if (!possible) {
			return item;
		}
		item.onSelect = [scene, parent, targets]() {
			SceneAnnotations::CreateFolder(scene, "New Folder", parent, {targets.data(), targets.size()});
		};
		return item;
	}

	void HierarchyPanel::OpenFolderContextMenu(UI::Context& context, Handle<Scene> scene, u64 folderId) {
		u64 sceneId = scene.GetInstanceId();
		Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
		items.push_back({"New Folder", "", true, false, [scene, folderId]() {
							 SceneAnnotations::CreateFolder(scene, "New Folder", FolderParent::In(folderId), {});
						 }});
		items.push_back({"Rename", "", true, false, [this, sceneId, folderId]() {
							 _renameFolder = folderId;
							 _renameFolderScene = sceneId;
							 _renameNeedsFocus = true;
						 }});
		items.push_back({"Delete Folder", "", true, true, [scene, folderId]() {
							 SceneAnnotations::DeleteFolder(scene, folderId);
						 }});

		UI::Widgets::MenuItem colors;
		colors.label = "Color";
		colors.separatorBefore = true;
		for (u8 i = 0; i < static_cast<u8>(LabelColor::Count); ++i) {
			LabelColor color = static_cast<LabelColor>(i);
			UI::Widgets::MenuItem entry;
			entry.label = str(SceneAnnotations::GetLabelName(color));
			entry.separatorBefore = (color == LabelColor::Red);
			entry.onSelect = [scene, folderId, color]() {
				SceneAnnotations::SetFolderLabel(scene, folderId, color);
			};
			colors.children.push_back(std::move(entry));
		}
		items.push_back(std::move(colors));

		UI::Widgets::OpenMenu(context, context.GetPointerPosition(),
							  {items.data(), items.size()}, UISystem::GetDefaultFont(), MenuFont());
	}

	void HierarchyPanel::OpenCreateMenu(UI::Context& context, Handle<Scene> scene, Handle<SceneObject> parent) {
		Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
		EditorSceneOps::BuildCreateMenu(scene, parent, items);
		FocusAfterCreate(items);
		UI::Widgets::OpenMenu(context, context.GetPointerPosition(),
							  {items.data(), items.size()}, UISystem::GetDefaultFont(), MenuFont());
	}

	void HierarchyPanel::FocusAfterCreate(Vector<UI::Widgets::MenuItem>& items) {
		Handle<HierarchyPanel> self(this);
		for (UI::Widgets::MenuItem& item : items) {
			if (!item.children.empty()) {
				FocusAfterCreate(item.children);
				continue;
			}
			if (!item.onSelect) {
				continue;
			}
			item.onSelect = [self, create = std::move(item.onSelect)]() {
				create();
				if (HierarchyPanel* panel = self.Get()) {
					panel->FocusList(panel->_list);
				}
			};
		}
	}

	void HierarchyPanel::OpenSceneContextMenu(UI::Context& context, Handle<Scene> scene) {
		UI::Context* contextPtr = &context;
		Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
		items.push_back(EditorSceneOps::BuildCreateSubmenu("Create", scene, Handle<SceneObject>{}));
		FocusAfterCreate(items.back().children);
		items.push_back({"New Folder", "", true, false, [scene]() {
							 SceneAnnotations::CreateFolder(scene, "New Folder", FolderParent::Roots(), {});
						 }});
		items.push_back({"Save Scene", "", true, false, [contextPtr, scene]() {
							 SceneFileFlows::SaveScene(*contextPtr, scene);
						 }});
		items.push_back({"Save Scene As...", "", !EditorSceneOps::GetPrefabTarget(scene).IsValid(), false,
						 [contextPtr, scene]() {
							 SceneFileFlows::SaveSceneAs(*contextPtr, scene);
						 }});
		items.push_back({"Set As Primary", "", true, false, [scene]() {
							 SceneSystem::SetPrimaryScene(scene);
						 }});
		items.push_back({"Unload Scene", "", SceneSystem::GetLoadedScenes().size() > 1, true, [scene]() {
							 EditorSelection::Clear();
							 UndoSystem::Clear();
							 SceneSystem::UnloadScene(scene);
						 }});
		UI::Widgets::OpenMenu(context, context.GetPointerPosition(),
							  {items.data(), items.size()}, UISystem::GetDefaultFont(), MenuFont());
	}

	bool HierarchyPanel::BeginRenameRow(u64 instanceId) {
		Object* raw = ObjectRegistry::Resolve(instanceId);
		if (!raw || !TypeRegistry::IsDerivedFrom(raw->GetType(), SceneObject::GetReflectType())) {
			return false;
		}
		_renameTarget = instanceId;
		_renameNeedsFocus = true;
		_forceRebuild = true;
		return true;
	}

	EditorIcons::Icon HierarchyPanel::GetTabIcon() const {
		return EditorIcons::Glyph::PanelHierarchy;
	}

} // namespace Shalltear
