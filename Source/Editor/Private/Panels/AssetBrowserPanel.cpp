#include "AssetBrowserPanel.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Asset/AssetImporter.h"
#include "Shalltear/Core/Input/Input.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/ProjectLayout.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/StringUtil.h"
#include "Shalltear/Core/UI/UISystem.h"
#include "Shalltear/Core/Window/WindowEvents.h"
#include "Shalltear/Core/Window/WindowSystem.h"
#include "Shalltear/Core/UI/Widgets.h"

#include "Shalltear/Editor/AssetOpeners.h"
#include "Shalltear/Editor/EditorAssetWatch.h"
#include "Shalltear/Editor/EditorDragData.h"
#include "Shalltear/Editor/EditorEvents.h"
#include "Shalltear/Editor/EditorIcons.h"
#include "Shalltear/Editor/EditorPreviews.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/EditorSelection.h"
#include "Shalltear/Editor/EditorInputOps.h"
#include "Shalltear/Editor/EditorTheme.h"
#include "Shalltear/Editor/PanelManager.h"
#include "Shalltear/Editor/UIBuilderOps.h"

#include "../GameModuleHost.h"
#include "../SceneFileFlows.h"
#include "PanelHeader.h"

#include <algorithm>

namespace Shalltear {

	namespace {

		using EditorTheme::ColorToken;
		using EditorTheme::MetricToken;

		void DraggedSceneObjects(const UI::DragPayload& payload, Vector<Handle<SceneObject>>& out) {
			out.clear();
			if (payload.type != "SceneObject"_hash || !payload.data.Is<SceneObjectDragData>()) {
				return;
			}
			for (u64 id : payload.data.As<SceneObjectDragData>().objects) {
				Object* raw = ObjectRegistry::Resolve(id);
				if (!raw || !TypeRegistry::IsDerivedFrom(raw->GetType(), SceneObject::GetReflectType())) {
					out.clear();
					return;
				}
				out.push_back(Handle<SceneObject>(static_cast<SceneObject*>(raw)));
			}
		}

		Color TextNormal() { return EditorTheme::GetColor(ColorToken::Text); }
		Color TextDim() { return EditorTheme::GetColor(ColorToken::TextDim); }
		Color TextStrong() { return EditorTheme::GetColor(ColorToken::TextStrong); }
		Color FolderIcon() { return EditorTheme::GetColor(ColorToken::AssetFolder); }
		f32 RowFont() { return EditorTheme::GetMetric(MetricToken::FontRow); }
		f32 CellFont() { return EditorTheme::GetMetric(MetricToken::FontSmall); }
		f32 MenuFont() { return EditorTheme::GetMetric(MetricToken::FontBody); }

		str ParentPath(const str& relPath) {
			size_t slash = relPath.find_last_of('/');
			return slash == str::npos ? str() : relPath.substr(0, slash);
		}

		u64 FolderKey(const str& relPath) {
			return StringHash(std::string_view(relPath)).GetValue();
		}

		str VirtualPathOf(const str& relPath) {
			return relPath.empty() ? str("project://") : "project://" + relPath;
		}

		bool IsVariantRow(const AssetBrowserOps::Entry& entry) {
			return entry.kind == AssetBrowserOps::EntryKind::File && AssetBrowserOps::IsPrefabVariant(entry.relPath);
		}

		EditorPreviews::TypeBadge BadgeForEntry(const AssetBrowserOps::Entry& entry, bool isVariant) {
			if (entry.IsDirectory()) {
				return {EditorIcons::Glyph::Folder, FolderIcon()};
			}
			if (entry.kind == AssetBrowserOps::EntryKind::Product && entry.assetType.GetValue() != 0) {
				return EditorPreviews::BadgeForAssetType(entry.assetType);
			}
			EditorPreviews::TypeBadge badge = EditorPreviews::BadgeForFileName(entry.name);
			if (isVariant) {
				badge.icon = EditorIcons::Glyph::AssetPrefabVariant;
			}
			return badge;
		}

		using EditorInputOps::PanelChord;

		str StepTooltip(std::string_view word, PanelChord chord) {
			return std::format("{} ({})", word, EditorInputOps::ChordLabel(chord));
		}

	} // anonymous namespace

	AssetBrowserPanel::AssetBrowserPanel() {
		for (std::string_view contentRoot : ProjectLayout::GetContentRoots()) {
			_expandedFolders.insert(FolderKey(str(contentRoot)));
		}
		_backAction = EditorInputOps::BindPanelChord(_navigationKeys, PanelChord::AssetBrowserBack);
		_forwardAction = EditorInputOps::BindPanelChord(_navigationKeys, PanelChord::AssetBrowserForward);
		_upAction = EditorInputOps::BindPanelChord(_navigationKeys, PanelChord::AssetBrowserUp);
	}

	void AssetBrowserPanel::EnsureScanned() {
		if (!_scanned) {
			_scanned = true;
			Rescan();
		}
	}

	void AssetBrowserPanel::Rescan() {
		_mounted = AssetBrowserOps::ScanTree(_entries);
		Vector<AssetBrowserOps::Entry> carried{GetMemoryResource(MemTag::Editor)};
		for (const auto& [id, row] : _rowById) {
			if (row.kind == AssetBrowserOps::EntryKind::Product || row.kind == AssetBrowserOps::EntryKind::Link) {
				carried.push_back(row);
			}
		}
		_rowById.clear();
		for (const AssetBrowserOps::Entry& entry : _entries) {
			_rowById[entry.id] = entry;
		}
		for (AssetBrowserOps::Entry& row : carried) {
			if (EntryExists(row.sourceRelPath)) {
				_rowById[row.id] = std::move(row);
			}
		}
		_foldOutRows.clear();
		_hasProducts.clear();
		if (_mounted) {
			bool alive = false;
			for (const AssetBrowserOps::Entry& entry : _entries) {
				if (entry.IsDirectory() && entry.relPath == selectedFolder) {
					alive = true;
					break;
				}
			}
			if (!alive) {
				selectedFolder = _entries.empty() ? str() : _entries.front().relPath;
			}

			bool pruned = std::erase_if(_selectedFiles, [this](u64 id) {
							  const AssetBrowserOps::Entry* row = RowOf(id);
							  return !row || (row->kind == AssetBrowserOps::EntryKind::File && !EntryExists(row->relPath));
						  }) > 0;
			std::erase_if(_selectedFolders, [this](u64 id) { return PathOf(id).empty(); });
			if (pruned) {
				PushAssetSelection();
			}
		}
		_dirty = true;
	}

	str AssetBrowserPanel::PathOf(u64 id) const {
		const AssetBrowserOps::Entry* row = RowOf(id);
		return row ? row->relPath : str();
	}

	const AssetBrowserOps::Entry* AssetBrowserPanel::RowOf(u64 id) const {
		auto it = _rowById.find(id);
		return it == _rowById.end() ? nullptr : &it->second;
	}

	bool AssetBrowserPanel::HasFoldOut(const AssetBrowserOps::Entry& entry) {
		if (entry.kind != AssetBrowserOps::EntryKind::File) {
			return false;
		}
		u64 stamp = FileSystem::GetLastWriteTime(
			FileSystem::Resolve(AssetDatabase::GetMetaPath(Path(VirtualPathOf(entry.relPath)))));
		auto memo = _hasProducts.find(entry.id);
		if (memo != _hasProducts.end() && memo->second.stamp == stamp) {
			return memo->second.has;
		}
		Vector<AssetBrowserOps::Entry> products{GetMemoryResource(MemTag::Editor)};
		AssetBrowserOps::ScanProducts(entry.relPath, entry.depth, products);
		bool has = !products.empty();
		_hasProducts[entry.id] = {stamp, has};
		_foldOutRows.erase(entry.id);
		return has;
	}

	const Vector<AssetBrowserOps::Entry>& AssetBrowserPanel::FoldOutRowsFor(const AssetBrowserOps::Entry& entry) {
		auto memo = _foldOutRows.find(entry.id);
		if (memo != _foldOutRows.end()) {
			return memo->second;
		}
		Vector<AssetBrowserOps::Entry> rows{GetMemoryResource(MemTag::Editor)};
		AssetBrowserOps::ScanProducts(entry.relPath, entry.depth, rows);
		Vector<AssetBrowserOps::Entry> links{GetMemoryResource(MemTag::Editor)};
		AssetBrowserOps::ScanLinks(entry.relPath, entry.depth, links);
		for (AssetBrowserOps::Entry& link : links) {
			rows.push_back(std::move(link));
		}
		for (const AssetBrowserOps::Entry& row : rows) {
			_rowById[row.id] = row;
		}
		return _foldOutRows.emplace(entry.id, std::move(rows)).first->second;
	}

	bool AssetBrowserPanel::EntryExists(const str& relPath) const {
		for (const AssetBrowserOps::Entry& entry : _entries) {
			if (!entry.IsDirectory() && entry.relPath == relPath) {
				return true;
			}
		}
		return false;
	}

	bool AssetBrowserPanel::FolderExists(const str& relPath) const {
		for (const AssetBrowserOps::Entry& entry : _entries) {
			if (entry.IsDirectory() && entry.relPath == relPath) {
				return true;
			}
		}
		return false;
	}

	void AssetBrowserPanel::VisibleFileRows(Vector<AssetBrowserOps::Entry>& out) {
		out.clear();
		Vector<AssetBrowserOps::Entry> contents{GetMemoryResource(MemTag::Editor)};
		AssetBrowserOps::FolderContents(_entries, selectedFolder, _filter, contents);
		for (const AssetBrowserOps::Entry& entry : contents) {
			out.push_back(entry);
			if (entry.IsDirectory()) {
				continue;
			}
			if (!_expandedAssets.contains(entry.id)) {
				continue;
			}
			for (const AssetBrowserOps::Entry& row : FoldOutRowsFor(entry)) {
				out.push_back(row);
			}
		}
	}

	bool AssetBrowserPanel::IsFileSelected(const str& relPath) const {
		u64 id = FolderKey(relPath);
		return std::find(_selectedFiles.begin(), _selectedFiles.end(), id) != _selectedFiles.end();
	}

	void AssetBrowserPanel::SelectOnlyFile(const str& relPath) {
		_selectedFiles.assign(1, FolderKey(relPath));
		_fileHighlightDirty = true;
		PushAssetSelection();
		FocusList(_fileList);
	}

	void AssetBrowserPanel::PublishFileSelectionFromList() {
		std::span<const u64> selected = _fileList.GetSelected();
		_selectedFiles.assign(selected.begin(), selected.end());
		PushAssetSelection();
	}

	void AssetBrowserPanel::PushFileSelectionToList() {
		_fileList.SetSelection(_selectedFiles);
	}

	void AssetBrowserPanel::PublishFolderSelectionFromList() {
		std::span<const u64> selected = _folderList.GetSelected();
		_selectedFolders.assign(selected.begin(), selected.end());
		str active = selected.empty() ? str() : PathOf(selected.back());
		if (!active.empty() && active != selectedFolder) {
			selectedFolder = active;
			LeaveFolderSelection();
			_dirty = true;
		}
	}

	void AssetBrowserPanel::PushFolderSelectionToList() {
		u64 active = selectedFolder.empty() ? 0 : FolderKey(selectedFolder);
		if (active != 0 && (_selectedFolders.empty() || _selectedFolders.back() != active)) {
			_selectedFolders.assign(1, active);
		}
		_folderList.SetSelection(_selectedFolders);
	}

	namespace {

		bool IsSelectableBrowserPath(const Path& virtualPath) {
			return AssetDatabase::IsAssetPath(virtualPath) ||
				   StringUtil::ToLower(virtualPath.GetExtension()) == ".cs";
		}

	} // anonymous namespace

	void AssetBrowserPanel::PushAssetSelection() {
		Vector<Guid> guids{GetMemoryResource(MemTag::Editor)};
		guids.reserve(_selectedFiles.size());
		Guid activeGuid{};
		u64 activeId = _selectedFiles.empty() ? 0 : _selectedFiles.back();
		for (u64 id : _selectedFiles) {
			const AssetBrowserOps::Entry* row = RowOf(id);
			if (!row) {
				continue;
			}
			Guid guid = row->guid;
			if (!guid.IsValid()) {
				Path virtualPath{VirtualPathOf(row->relPath)};
				if (!IsSelectableBrowserPath(virtualPath)) {
					continue;
				}
				guid = AssetDatabase::LookupGuid(virtualPath);
			}
			if (!guid.IsValid()) {
				continue;
			}
			if (id == activeId) {
				activeGuid = guid;
				continue;
			}
			guids.push_back(guid);
		}
		if (activeGuid.IsValid()) {
			guids.push_back(activeGuid);
		}
		_publishingSelection = true;
		EditorSelection::SelectAssets(guids);
		_publishingSelection = false;
	}

	void AssetBrowserPanel::OnSelectionChanged() {
		if (_publishingSelection) {
			return;
		}
		SyncSelectionFromEditor();
	}

	void AssetBrowserPanel::SyncSelectionFromEditor() {
		_selectedFiles.clear();
		constexpr std::string_view PREFIX = "project://";
		for (const Guid& guid : EditorSelection::GetSelectedAssets()) {
			Path path = AssetDatabase::LookupPath(guid);
			const str& virtualPath = path.ToString();
			if (virtualPath.starts_with(PREFIX)) {
				_selectedFiles.push_back(AssetBrowserOps::EntryKeyForPath(virtualPath.substr(PREFIX.size())));
				continue;
			}
			for (const auto& [id, row] : _rowById) {
				if (row.guid == guid) {
					_selectedFiles.push_back(id);
					break;
				}
			}
		}
		_fileHighlightDirty = true;
	}

	void AssetBrowserPanel::SelectedEntryPaths(Vector<str>& out) const {
		out.clear();
		out.reserve(_selectedFiles.size());
		for (u64 id : _selectedFiles) {
			const AssetBrowserOps::Entry* row = RowOf(id);
			if (row && (row->kind == AssetBrowserOps::EntryKind::File || row->IsDirectory()) && !row->relPath.empty()) {
				out.push_back(row->relPath);
			}
		}
	}

	void AssetBrowserPanel::SelectedFolderPaths(Vector<str>& out) const {
		out.clear();
		out.reserve(_selectedFolders.size());
		for (u64 id : _selectedFolders) {
			str relPath = PathOf(id);
			if (!relPath.empty()) {
				out.push_back(std::move(relPath));
			}
		}
	}

	bool AssetBrowserPanel::ReportRefusals(const Vector<u64>& ids, AssetBrowserOps::EntryCommand command) {
		u32 refused = 0;
		for (u64 id : ids) {
			const AssetBrowserOps::Entry* row = RowOf(id);
			if (!row) {
				continue;
			}
			str refusal = AssetBrowserOps::RefusalFor(*row, command);
			if (refusal.empty()) {
				continue;
			}
			SLT_LOG_WARN("Editor", "{}", refusal);
			++refused;
		}
		return refused > 0 && refused == ids.size();
	}

	void AssetBrowserPanel::ExtractProductRow(const AssetBrowserOps::Entry& entry) {
		str created = AssetBrowserOps::ExtractProduct(entry);
		if (created.empty()) {
			return;
		}
		Rescan();
		SelectOnlyFile(created);
	}

	void AssetBrowserPanel::SelectionTargets(const str& clickedRelPath, Vector<str>& out) const {
		if (!IsFileSelected(clickedRelPath)) {
			out.clear();
			out.push_back(clickedRelPath);
			return;
		}
		SelectedEntryPaths(out);
	}

	void AssetBrowserPanel::RemapViewState(const str& oldRelPath, const str& newRelPath) {
		str prefix = oldRelPath + "/";
		auto rebase = [&](const str& path) {
			return path == oldRelPath ? newRelPath : newRelPath + path.substr(oldRelPath.size());
		};
		auto covered = [&](const str& path) {
			return path == oldRelPath || path.starts_with(prefix);
		};

		HashSet<u64> movedFolders{GetMemoryResource(MemTag::Editor)};
		HashMap<u64, u64> movedKeys{GetMemoryResource(MemTag::Editor)};
		for (const AssetBrowserOps::Entry& entry : _entries) {
			if (!covered(entry.relPath)) {
				continue;
			}
			u64 oldKey = FolderKey(entry.relPath);
			movedKeys[oldKey] = FolderKey(rebase(entry.relPath));
			if (entry.IsDirectory() && _expandedFolders.erase(oldKey) != 0) {
				movedFolders.insert(FolderKey(rebase(entry.relPath)));
			}
		}
		for (u64 key : movedFolders) {
			_expandedFolders.insert(key);
		}

		if (covered(selectedFolder)) {
			selectedFolder = rebase(selectedFolder);
		}
		_history.Remap(oldRelPath, newRelPath);
		auto rekey = [&](Vector<u64>& ids) {
			for (u64& id : ids) {
				auto it = movedKeys.find(id);
				if (it != movedKeys.end()) {
					id = it->second;
				}
			}
		};
		rekey(_selectedFiles);
		rekey(_selectedFolders);
	}

	void AssetBrowserPanel::OnBuildContent(UI::Context& context, UI::ElementHandle root) {
		_contentRoot = root;
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

		UI::Widgets::SplitDesc split;
		split.sizing = UI::Widgets::SplitSizing::FirstFixed;
		split.position = folderPaneWidth;
		split.minFirstPx = 120.0f;
		split.minSecondPx = 160.0f;
		split.onPositionChanged = [this](f32 width) { folderPaneWidth = width; };
		UI::Widgets::SplitParts panes = UI::Widgets::Split(context, root, split);

		UI::Style splitStyle = context.GetStyle(panes.root);
		splitStyle.height = UI::Length::Pixel(0.0f);
		context.SetStyle(panes.root, splitStyle);

		UI::ElementHandle folderPane = panes.first;

		ListViewDesc folders;
		folders.layout = ListLayout::Rows;
		folders.mode = ListSelectionMode::Multiple;
		folders.onSelectionChanged = [this]() { PublishFolderSelectionFromList(); };
		folders.onDelete = [this]() {
			if (UI::Context* live = GetPanelContext()) {
				Vector<str> targets{GetMemoryResource(MemTag::Editor)};
				SelectedFolderPaths(targets);
				DeleteFolders(*live, targets);
			}
		};
		folders.onRename = [this](u64 row) {
			UI::Context* live = GetPanelContext();
			return live && BeginRenameFolder(*live, PathOf(row));
		};
		_folderList.Build(this, context, folderPane, std::move(folders));

		_filePane = panes.second;
		UI::ElementHandle filePane = _filePane;

		_fileChromeHost = context.CreateElement();
		context.AppendChild(filePane, _fileChromeHost);
		UI::Style chromeStyle;
		chromeStyle.flexDirection = UI::FlexDirection::Column;
		chromeStyle.width = UI::Length::Percent(1.0f);
		chromeStyle.flexShrink = 0.0f;
		context.SetStyle(_fileChromeHost, chromeStyle);

		_fileListHost = context.CreateElement();
		context.AppendChild(filePane, _fileListHost);
		UI::Style listHostStyle;
		listHostStyle.flexDirection = UI::FlexDirection::Column;
		listHostStyle.width = UI::Length::Percent(1.0f);
		listHostStyle.flexGrow = 1.0f;
		context.SetStyle(_fileListHost, listHostStyle);
		_fileListBuilt = false;
		_fileListLayout = ListLayout::Rows;
		_fileListCellWidthPx = 0.0f;
		_fileListCellHeightPx = 0.0f;
		EnsureFileList(context);

		UI::Context* paneContext = &context;
		context.Events(filePane).onDragOver = [this](const UI::DragPayload& payload) {
			return AcceptsDropInto(payload, selectedFolder);
		};
		context.Events(filePane).onDrop = [this, paneContext](const UI::DragPayload& payload, Vec2) {
			DropInto(*paneContext, payload, selectedFolder);
		};

		_footerHost = context.CreateElement();
		context.AppendChild(filePane, _footerHost);
		context.AddClass(_footerHost, StringHash("ed-panel-footer"));
		BuildFooter(context);

		_buildContext = &context;
		EnsureScanned();
		RebuildPanes(context);
	}

	void AssetBrowserPanel::OnOpen() {
		Handle<AssetBrowserPanel> self(this);
		WindowEvents::FilesDropped.Subscribe(EventBus::GetGlobal(), self, &AssetBrowserPanel::OnFilesDropped);
		EditorEvents::PingRequested.Subscribe(EventBus::GetGlobal(), self, &AssetBrowserPanel::OnPingRequested);
		SyncSelectionFromEditor();
	}

	void AssetBrowserPanel::OnFilesDropped(const FilesDroppedEvent& event) {
		if (!_mounted || !_filePane.IsValid() || event.paths.empty()) {
			return;
		}
		UI::Context* context = UISystem::GetContext(event.window);
		if (!context || context != _buildContext) {
			return;
		}
		Vec2 point{static_cast<f32>(event.clientPosPx.x), static_cast<f32>(event.clientPosPx.y)};
		if (!context->GetLayout(_filePane).borderBox.Contains(point)) {
			return;
		}

		str folder = DropFolderAt(*context, point);
		if (folder.empty()) {
			return;
		}
		u32 imported = 0;
		for (const str& path : event.paths) {
			if (!AssetBrowserOps::ImportExternalFile(Path(path), Path(VirtualPathOf(folder))).empty()) {
				++imported;
			}
		}
		if (imported > 0) {
			SLT_LOG_INFO("Editor", "Asset Browser: imported {} file(s) into '{}'", imported, folder);
			Rescan();
		}
	}

	str AssetBrowserPanel::DropFolderAt(UI::Context& context, Vec2 point) const {
		for (UI::ElementHandle hit = context.HitTest(point); context.IsAlive(hit) && hit != _filePane;) {
			const UI::Element* element = context.GetElement(hit);
			if (!element) {
				break;
			}
			if (element->userTag == 0) {
				hit = element->parent == UI::INVALID_UI_INDEX ? UI::ElementHandle{} : context.MakeHandle(element->parent);
				continue;
			}
			const AssetBrowserOps::Entry* row = RowOf(element->userTag);
			if (row && row->IsDirectory()) {
				return row->relPath;
			}
			break;
		}
		return selectedFolder;
	}

	bool AssetBrowserPanel::AcceptsDropInto(const UI::DragPayload& payload, const str& folderRelPath) const {
		if (folderRelPath.empty()) {
			return false;
		}
		if (payload.type == "Asset"_hash) {
			if (!payload.data.Is<AssetDragData>()) {
				return false;
			}
			Path source = AssetDatabase::LookupPath(payload.data.As<AssetDragData>().guid);
			const str& sourcePath = source.ToString();
			constexpr std::string_view PREFIX = "project://";
			if (!sourcePath.starts_with(PREFIX)) {
				return false;
			}
			return ParentPath(sourcePath.substr(PREFIX.size())) != folderRelPath;
		}
		if (payload.type == "BrowserFile"_hash && payload.data.Is<str>()) {
			const str& relPath = payload.data.As<str>();
			return !relPath.empty() && ParentPath(relPath) != folderRelPath;
		}
		Vector<Handle<SceneObject>> dragged{GetMemoryResource(MemTag::Editor)};
		DraggedSceneObjects(payload, dragged);
		return !dragged.empty() && std::all_of(dragged.begin(), dragged.end(), [](Handle<SceneObject> object) {
			return !SceneSystem::IsPrefabLinked(object) || EditorSceneOps::CanCreatePrefabVariantFrom(object);
		});
	}

	void AssetBrowserPanel::DropInto(UI::Context& context, const UI::DragPayload& payload, const str& folderRelPath) {
		if (!AcceptsDropInto(payload, folderRelPath)) {
			return;
		}
		Path folder{VirtualPathOf(folderRelPath)};
		if (payload.type == "SceneObject"_hash) {
			Vector<Handle<SceneObject>> dragged{GetMemoryResource(MemTag::Editor)};
			DraggedSceneObjects(payload, dragged);
			const u32 instances = static_cast<u32>(std::count_if(dragged.begin(), dragged.end(), [](Handle<SceneObject> object) {
				return SceneSystem::IsPrefabLinked(object);
			}));
			if (dragged.size() == 1 && instances == 1) {
				OfferPrefabVariant(context, dragged.front(), folderRelPath);
				return;
			}
			if (instances > 0) {
				const str prompt = instances == dragged.size()
									   ? std::format("These {} objects are prefab instances. Create a Prefab Variant from each?", instances)
									   : std::format("{} of these objects are prefab instances. Create a Prefab Variant from each of them, and a prefab from each of the others?",
													 instances);
				SceneFileFlows::OpenConfirmPopup(
					context, prompt, "Create", [this, dragged, folderRelPath]() { CreatePrefabsFrom(dragged, folderRelPath); },
					"", {});
				return;
			}
			CreatePrefabsFrom(dragged, folderRelPath);
			return;
		}
		if (payload.type == "BrowserFile"_hash && payload.data.Is<str>()) {
			const str& oldRelPath = payload.data.As<str>();
			if (!AssetBrowserOps::MoveEntry(Path(VirtualPathOf(oldRelPath)), folder)) {
				return;
			}
			RemapViewState(oldRelPath, folderRelPath + "/" + Path(oldRelPath).GetFilename());
			Rescan();
			return;
		}
		Path source = AssetDatabase::LookupPath(payload.data.As<AssetDragData>().guid);
		const str& sourcePath = source.ToString();
		if (!AssetBrowserOps::MoveEntry(source, folder)) {
			return;
		}
		str oldRelPath = sourcePath.substr(str("project://").size());
		RemapViewState(oldRelPath, folderRelPath + "/" + Path(oldRelPath).GetFilename());
		Rescan();
	}

	void AssetBrowserPanel::OnPingRequested(const EditorPingRequestedEvent& event) {
		const Guid asset = event.target.asset;
		if (!asset.IsValid()) {
			return;
		}
		EnsureScanned();

		const Guid source = AssetDatabase::GetProductSource(asset);
		const Guid fileGuid = source.IsValid() ? source : asset;

		constexpr std::string_view PREFIX = "project://";
		Path path = AssetDatabase::LookupPath(fileGuid);
		const str& virtualPath = path.ToString();
		if (!virtualPath.starts_with(PREFIX)) {
			return;
		}
		const str relPath = virtualPath.substr(PREFIX.size());
		if (!EntryExists(relPath)) {
			return;
		}

		NavigateToFolder(ParentPath(relPath));
		if (source.IsValid()) {
			_expandedAssets.insert(FolderKey(relPath));
		}

		_fileList.PingRow(RowIdForAsset(asset));
		_dirty = true;

		PanelManager::RevealPanel(Handle<EditorPanel>(this));
	}

	void AssetBrowserPanel::NavigateToFolder(const str& folder) {
		if (folder == selectedFolder) {
			return;
		}
		selectedFolder = folder;
		for (str walk = folder; !walk.empty(); walk = ParentPath(walk)) {
			_expandedFolders.insert(FolderKey(walk));
		}
		_selectedFolders.assign(1, FolderKey(folder));
		_dirty = true;
	}

	void AssetBrowserPanel::OpenFolder(const str& relPath) {
		if (!FolderExists(relPath)) {
			return;
		}
		NavigateToFolder(relPath);
		LeaveFolderSelection();
	}

	bool AssetBrowserPanel::GoBack() {
		const str folder = _history.Back([this](std::string_view path) { return FolderExists(str(path)); });
		if (folder.empty()) {
			return false;
		}
		OpenFolder(folder);
		return true;
	}

	bool AssetBrowserPanel::GoForward() {
		const str folder = _history.Forward([this](std::string_view path) { return FolderExists(str(path)); });
		if (folder.empty()) {
			return false;
		}
		OpenFolder(folder);
		return true;
	}

	bool AssetBrowserPanel::GoUp() {
		const str parent = ParentPath(selectedFolder);
		if (parent.empty() || !FolderExists(parent)) {
			return false;
		}
		const str left = selectedFolder;
		OpenFolder(parent);
		SelectOnlyFile(left);
		_revealAfterBuild = FolderKey(left);
		return true;
	}

	void AssetBrowserPanel::RouteNavigation(UI::Context& context) {
		EditorInputOps::RouteFacts facts = EditorInputOps::ApplicationFacts(context);
		facts.viewFocused = PanelManager::GetFocusedPanel().Get() == this;
		EditorInputOps::ApplyRoute(_navigationKeys, EditorInputOps::Routes::PanelKeyboard, facts);
		_navigationKeys.Evaluate();
		if (_navigationKeys.WasPerformed(_backAction)) {
			GoBack();
		} else if (_navigationKeys.WasPerformed(_forwardAction)) {
			GoForward();
		} else if (_navigationKeys.WasPerformed(_upAction)) {
			GoUp();
		}

		for (const InputEvent& event : Input::GetFrameEvents()) {
			const bool side = event.button == MouseButton::X1 || event.button == MouseButton::X2;
			if (event.type != InputEventType::MouseDown || !side || UISystem::GetContext(event.window) != &context) {
				continue;
			}
			const Vec2 point{static_cast<f32>(event.mousePosPx.x), static_cast<f32>(event.mousePosPx.y)};
			facts.pointerInside = IsInsidePanel(context, context.HitTest(point));
			if (!EditorInputOps::ShouldOpen(EditorInputOps::Routes::PanelPointer, facts)) {
				continue;
			}
			if (event.button == MouseButton::X1) {
				GoBack();
			} else {
				GoForward();
			}
		}
	}

	bool AssetBrowserPanel::IsInsidePanel(UI::Context& context, UI::ElementHandle handle) const {
		for (UI::ElementHandle walk = handle; context.IsAlive(walk);) {
			if (walk == _contentRoot) {
				return true;
			}
			const UI::Element* element = context.GetElement(walk);
			if (!element || element->parent == UI::INVALID_UI_INDEX) {
				return false;
			}
			walk = context.MakeHandle(element->parent);
		}
		return false;
	}

	void AssetBrowserPanel::LeaveFolderSelection() {
		SyncSelectionFromEditor();
	}

	void AssetBrowserPanel::ClearFileSelection() {
		_selectedFiles.clear();
		_fileList.SetSelection(_selectedFiles);
		PushAssetSelection();
	}

	u64 AssetBrowserPanel::RowIdForAsset(const Guid& guid) {
		Vector<AssetBrowserOps::Entry> rows{GetMemoryResource(MemTag::Editor)};
		VisibleFileRows(rows);
		for (const AssetBrowserOps::Entry& entry : rows) {
			Guid rowGuid = entry.guid;
			if (!rowGuid.IsValid()) {
				Path virtualPath{VirtualPathOf(entry.relPath)};
				if (IsSelectableBrowserPath(virtualPath)) {
					rowGuid = AssetDatabase::LookupGuid(virtualPath);
				}
			}
			if (rowGuid.IsValid() && rowGuid == guid) {
				return entry.id;
			}
		}
		return 0;
	}

	void AssetBrowserPanel::OnTick(f32 deltaSeconds) {
		EnsureScanned();

		if (!_mounted) {
			_mountPollDelay -= deltaSeconds;
			if (_mountPollDelay <= 0.0f) {
				_mountPollDelay = 1.0f;
				if (AssetBrowserOps::IsProjectMounted()) {
					Rescan();
				}
			}
		}

		if (!EditorAssetWatch::GetChangedThisFrame().empty() || EditorAssetWatch::DidOverflowThisFrame()) {
			_rescanDelay = 0.5f;
		}
		if (_rescanDelay > 0.0f) {
			_rescanDelay -= deltaSeconds;
			if (_rescanDelay <= 0.0f) {
				_rescanDelay = -1.0f;
				AssetDatabase::RebuildDatabase();
				Rescan();
			}
		}

		_foldOutPollDelay -= deltaSeconds;
		if (_foldOutPollDelay <= 0.0f) {
			_foldOutPollDelay = 0.5f;
			Vector<u64> remembered{GetMemoryResource(MemTag::Editor)};
			remembered.reserve(_hasProducts.size());
			for (const auto& [id, memo] : _hasProducts) {
				remembered.push_back(id);
			}
			for (u64 id : remembered) {
				const AssetBrowserOps::Entry* row = RowOf(id);
				auto memo = _hasProducts.find(id);
				if (!row || memo == _hasProducts.end()) {
					continue;
				}
				bool before = memo->second.has;
				if (HasFoldOut(*row) != before) {
					_dirty = true;
				}
			}
		}

		if (selectedFolder != _history.Current()) {
			const bool first = _history.Current().empty();
			_history.Visit(selectedFolder);
			if (!first) {
				_dirty = true;
			}
		}

		UI::Context* context = GetPanelContext();
		if (!context || !context->IsAlive(_fileListHost)) {
			return;
		}
		if (_editingPath && !_focusPathField && context->IsAlive(_pathField) && context->GetFocused() != _pathField) {
			_editingPath = false;
			_dirty = true;
		}
		RouteNavigation(*context);
		if (_revealAfterBuild != 0 && !_dirty) {
			_fileList.RevealRow(_revealAfterBuild);
			_revealAfterBuild = 0;
		}

		if (_dirty) {
			RebuildPanes(*context);
			return;
		}
		if (_fileHighlightDirty) {
			_fileHighlightDirty = false;
			PushFileSelectionToList();
		}

		RequestVisiblePreviews(*context);
	}

	void AssetBrowserPanel::BuildHeader(UI::Context& context) {
		context.ClearChildren(_headerHost);

		UI::Context* headerContext = &context;
		PanelHeader::Desc desc;
		desc.addLabel = "Create";
		desc.addTooltip = "Create Asset";
		desc.onAdd = [this, headerContext]() {
			OpenCreateMenu(*headerContext, headerContext->GetPointerPosition());
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
			_dirty = true;
		};
		desc.onSearchChanged = [this](std::string_view text) {
			if (_filter == text) {
				return;
			}
			_filter = str(text);
			_dirty = true;
		};
		PanelHeader::Build(context, _headerHost, desc);
		_focusSearch = false;
	}

	void AssetBrowserPanel::RebuildPanes(UI::Context& context) {
		_dirty = false;
		_builtEntries.clear();
		_entryIndexById.clear();
		EnsureFileList(context);
		BuildFolderRows(context);
		if (!_editingPath || !context.IsAlive(_pathField)) {
			BuildFileChrome(context);
		}
		BuildFileEntries(context);
	}

	void AssetBrowserPanel::BuildFolderRows(UI::Context& context) {
		Vector<ListRow> rows{GetMemoryResource(MemTag::Editor)};
		if (!_mounted) {
			_folderList.SetRows(rows);
			return;
		}

		HashSet<u64> hasSubfolders{GetMemoryResource(MemTag::Editor)};
		for (const AssetBrowserOps::Entry& entry : _entries) {
			if (!entry.IsDirectory()) {
				continue;
			}
			str parent = ParentPath(entry.relPath);
			if (!parent.empty()) {
				hasSubfolders.insert(FolderKey(parent));
			}
		}

		HashMap<u64, bool> rowVisible{GetMemoryResource(MemTag::Editor)};
		for (const AssetBrowserOps::Entry& entry : _entries) {
			if (!entry.IsDirectory()) {
				continue;
			}
			u64 key = FolderKey(entry.relPath);
			str parent = ParentPath(entry.relPath);
			bool visible;
			if (parent.empty()) {
				visible = true;
			} else {
				u64 parentKey = FolderKey(parent);
				auto it = rowVisible.find(parentKey);
				visible = it != rowVisible.end() && it->second && _expandedFolders.contains(parentKey);
			}
			rowVisible[key] = visible;
			if (!visible) {
				continue;
			}

			ListRow row;
			row.id = key;
			row.depth = entry.depth;
			row.hasChildren = hasSubfolders.contains(key);
			row.expanded = _expandedFolders.contains(key);
			row.label = entry.name;
			row.onToggleExpand = [this, key](bool) {
				if (!_expandedFolders.erase(key)) {
					_expandedFolders.insert(key);
				}
				_dirty = true;
			};
			row.onRightClick = [this, relPath = entry.relPath, isContentRoot = parent.empty()](UI::Context& context) {
				OpenFolderContextMenu(context, relPath, isContentRoot);
			};
			row.onDragOver = [this, relPath = entry.relPath](const UI::DragPayload& payload) {
				return AcceptsDropInto(payload, relPath);
			};
			row.onDrop = [this, relPath = entry.relPath](const UI::DragPayload& payload, Vec2) {
				if (UI::Context* live = GetPanelContext()) {
					DropInto(*live, payload, relPath);
				}
			};
			row.onDecorate = [hasChildren = row.hasChildren, expanded = row.expanded,
							  isContentRoot = parent.empty()](UI::Context& context, const UI::Widgets::TreeRowParts& parts) {
				EditorIcons::SetRowGutter(context, parts, hasChildren, expanded, TextDim(),
										  isContentRoot ? EditorIcons::Glyph::FolderOpen : EditorIcons::Glyph::Folder,
										  FolderIcon());
			};
			rows.push_back(std::move(row));
		}
		_folderList.SetRows(rows);
		PushFolderSelectionToList();
	}

	AssetBrowserPanel::FileEntryActions AssetBrowserPanel::MakeFileActions(const AssetBrowserOps::Entry& entry) {
		FileEntryActions actions;

		if (entry.IsDirectory()) {
			actions.onDoubleClick = [this, relPath = entry.relPath](UI::Context&) { OpenFolder(relPath); };
			actions.onRightClick = [this, entry](UI::Context& context) { OpenFileContextMenu(context, entry); };
			actions.onDragOver = [this, relPath = entry.relPath](const UI::DragPayload& payload) {
				return AcceptsDropInto(payload, relPath);
			};
			actions.onDrop = [this, relPath = entry.relPath](const UI::DragPayload& payload, Vec2) {
				if (UI::Context* live = GetPanelContext()) {
					DropInto(*live, payload, relPath);
				}
			};
			return actions;
		}

		str extension = Path(entry.name).GetExtension();
		if (AssetOpeners::Has(extension)) {
			actions.onDoubleClick = [relPath = entry.relPath](UI::Context&) {
				AssetOpeners::Open(Path(VirtualPathOf(relPath)));
			};
		} else if (extension == ".scene") {
			actions.onDoubleClick = [relPath = entry.relPath](UI::Context& context) {
				SceneFileFlows::OpenScenePath(context, Path(VirtualPathOf(relPath)));
			};
		} else if (extension == ".prefab") {
			actions.onDoubleClick = [relPath = entry.relPath](UI::Context& context) {
				SceneFileFlows::OpenPrefabPath(context, Path(VirtualPathOf(relPath)));
			};
		} else if (extension == ".slxml") {
			actions.onDoubleClick = [relPath = entry.relPath](UI::Context&) {
				UIBuilderOps::OpenDocumentInBuilder(VirtualPathOf(relPath));
			};
		} else if (AssetBrowserOps::OpensInExternalEditor(extension)) {
			actions.onDoubleClick = [relPath = entry.relPath](UI::Context&) {
				AssetBrowserOps::OpenInExternalEditor(FileSystem::Resolve(Path(VirtualPathOf(relPath))));
			};
		}

		actions.onRightClick = [this, entry](UI::Context& context) {
			OpenFileContextMenu(context, entry);
		};

		actions.onDrag = [entry](Vec2) {
			if (UISystem::IsDragPayloadActive()) {
				return;
			}
			Guid guid = entry.guid;
			StringHash type = entry.assetType;
			if (!guid.IsValid()) {
				Path virtualPath{VirtualPathOf(entry.relPath)};
				guid = AssetDatabase::LookupGuid(virtualPath);
			}
			if (!guid.IsValid()) {
				if (entry.kind == AssetBrowserOps::EntryKind::File && !entry.relPath.empty()) {
					UI::DragPayload payload;
					payload.type = "BrowserFile"_hash;
					payload.data = Variant(entry.relPath);
					payload.display = entry.name;
					UISystem::BeginDragPayload(payload);
				}
				return;
			}
			if (type.GetValue() == 0) {
				str extension2 = Path(entry.name).GetExtension();
				IAssetImporter* importer = extension2.empty() ? nullptr : AssetImporterRegistry::FindByExtension(extension2);
				type = importer ? importer->GetAssetType() : StringHash{};
			}
			AssetDragData data;
			data.guid = guid;
			data.assetTypeHash = type.GetValue();
			UI::DragPayload payload;
			payload.type = "Asset"_hash;
			payload.data = Variant(data);
			payload.display = entry.name;
			UISystem::BeginDragPayload(payload);
		};

		return actions;
	}

	void AssetBrowserPanel::BuildFileChrome(UI::Context& context) {
		context.ClearChildren(_fileChromeHost);
		Handle<FontAsset> font = UISystem::GetDefaultFont();

		auto line = [&](std::string_view text, Color color) {
			UI::ElementHandle info = context.CreateElement();
			context.AppendChild(_fileChromeHost, info);
			UI::Style infoStyle;
			infoStyle.padding = UI::Edges::All(10.0f);
			context.SetStyle(info, infoStyle);
			context.SetText(info, font, RowFont(), color, text);
		};

		if (!_mounted) {
			line("No project mounted. Start with --project <path> or run from a directory containing Assets/",
				 TextDim());
			return;
		}
		if (selectedFolder.empty()) {
			line("This project has no Assets/ folder. Create one to store content", TextDim());
			return;
		}

		UI::ElementHandle bar = context.CreateElement();
		context.AppendChild(_fileChromeHost, bar);
		UI::Style barStyle;
		barStyle.flexDirection = UI::FlexDirection::Row;
		barStyle.alignItems = UI::Align::Center;
		barStyle.padding = {2.0f, 1.0f, 8.0f, 1.0f};
		context.SetStyle(bar, barStyle);
		context.AddClass(bar, EditorTheme::Class::SectionHeader);

		auto exists = [this](std::string_view folder) { return FolderExists(str(folder)); };
		auto step = [&](EditorIcons::Icon icon, std::string_view fallback, const str& tooltip, bool live,
						std::function<void()> onClick) {
			PanelHeader::ToolButtonDesc desc;
			desc.icon = icon;
			desc.fallback = fallback;
			desc.tooltip = tooltip;
			desc.onClick = std::move(onClick);
			EditorIcons::IconButtonParts parts = PanelHeader::ToolButton(context, bar, desc);
			if (!live) {
				context.AddClass(parts.button, EditorTheme::Class::Disabled);
				context.SetHoverable(parts.button, false);
			}
		};
		step(EditorIcons::Glyph::Back, "<", StepTooltip("Back", PanelChord::AssetBrowserBack), !_history.PeekBack(exists).empty(),
			 [this]() { GoBack(); });
		step(EditorIcons::Glyph::Forward, ">", StepTooltip("Forward", PanelChord::AssetBrowserForward), !_history.PeekForward(exists).empty(),
			 [this]() { GoForward(); });
		if (_editingPath) {
			BuildPathField(context, bar);
		} else {
			BuildCrumbs(context, bar);
		}

		Vector<AssetBrowserOps::Entry> contents{GetMemoryResource(MemTag::Editor)};
		AssetBrowserOps::FolderContents(_entries, selectedFolder, {}, contents);
		if (contents.empty()) {
			line("(empty folder)", TextDim());
		}
	}

	void AssetBrowserPanel::BuildCrumbs(UI::Context& context, UI::ElementHandle bar) {
		Handle<FontAsset> font = UISystem::GetDefaultFont();
		UI::Context* contextPtr = &context;

		UI::ElementHandle crumbs = context.CreateElement();
		context.AppendChild(bar, crumbs);
		UI::Style crumbsStyle;
		crumbsStyle.flexDirection = UI::FlexDirection::Row;
		crumbsStyle.alignItems = UI::Align::Center;
		crumbsStyle.flexGrow = 1.0f;
		crumbsStyle.overflow = UI::Overflow::Hidden;
		crumbsStyle.margin = {4.0f, 0.0f, 0.0f, 0.0f};
		context.SetStyle(crumbs, crumbsStyle);

		HashSet<u64> parents{GetMemoryResource(MemTag::Editor)};
		for (const AssetBrowserOps::Entry& entry : _entries) {
			if (entry.IsDirectory()) {
				parents.insert(FolderKey(ParentPath(entry.relPath)));
			}
		}
		Vector<str> trail{GetMemoryResource(MemTag::Editor)};
		for (str walk = selectedFolder; !walk.empty(); walk = ParentPath(walk)) {
			trail.insert(trail.begin(), walk);
		}
		for (size_t i = 0; i < trail.size(); ++i) {
			const str folder = trail[i];
			const bool shown = i + 1 == trail.size();
			UI::ElementHandle crumb = context.CreateElement();
			context.AppendChild(crumbs, crumb);
			context.AddClass(crumb, EditorTheme::Class::PathCrumb);
			context.AddClass(crumb, EditorTheme::Class::RowText);
			context.SetText(crumb, font, RowFont(), shown ? TextStrong() : TextNormal(), Path(folder).GetFilename());
			if (shown) {
				UI::Style shownStyle;
				shownStyle.flexShrink = 0.0f;
				context.SetStyle(crumb, shownStyle);
			}
			context.Events(crumb).onClick = [this, folder]() { OpenFolder(folder); };

			if (shown && !parents.contains(FolderKey(folder))) {
				continue;
			}
			auto self = std::make_shared<UI::ElementHandle>();
			const str next = shown ? str() : trail[i + 1];
			PanelHeader::ToolButtonDesc desc;
			desc.icon = EditorIcons::Glyph::CaretRight;
			desc.fallback = ">";
			desc.tooltip = "Subfolders";
			desc.onClick = [this, contextPtr, folder, next, self]() {
				const UI::Rect box = contextPtr->GetLayout(*self).borderBox;
				OpenSubfolderMenu(*contextPtr, folder, next, {box.x, box.y + box.h});
			};
			*self = PanelHeader::ToolButton(context, crumbs, desc).button;
		}

		UI::ElementHandle rest = context.CreateElement();
		context.AppendChild(crumbs, rest);
		UI::Style restStyle;
		restStyle.flexGrow = 1.0f;
		restStyle.minWidth = UI::Length::Pixel(24.0f);
		restStyle.alignSelf = UI::Align::Stretch;
		restStyle.cursor = SystemCursor::IBeam;
		context.SetStyle(rest, restStyle);
		context.SetTooltip(rest, "Click to type a path");
		context.Events(rest).onClick = [this]() {
			_editingPath = true;
			_focusPathField = true;
			_dirty = true;
		};
	}

	void AssetBrowserPanel::BuildPathField(UI::Context& context, UI::ElementHandle bar) {
		UI::Context* contextPtr = &context;
		UI::Widgets::TextFieldDesc desc;
		desc.text = selectedFolder;
		desc.font = UISystem::GetDefaultFont();
		desc.fontSizePx = RowFont();
		desc.onCommit = [this, contextPtr](std::string_view text) {
			if (contextPtr->GetFocused() == _pathField) {
				GoToTypedPath(*contextPtr, str(text));
			}
		};
		_pathField = UI::Widgets::TextField(context, bar, desc);
		UI::Style style = context.GetStyle(_pathField);
		style.margin = {4.0f, 0.0f, 0.0f, 0.0f};
		context.SetStyle(_pathField, style);
		context.Events(_pathField).onTextChange = [this, contextPtr](std::string_view) {
			contextPtr->RemoveClass(_pathField, EditorTheme::Class::FieldRefused);
		};
		if (_focusPathField) {
			context.SetFocus(_pathField);
			_focusPathField = false;
		}
	}

	void AssetBrowserPanel::OpenSubfolderMenu(UI::Context& context, const str& folder, const str& current, Vec2 anchorPx) {
		Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
		for (const AssetBrowserOps::Entry& entry : _entries) {
			if (!entry.IsDirectory() || ParentPath(entry.relPath) != folder) {
				continue;
			}
			UI::Widgets::MenuItem item;
			item.label = entry.name;
			item.checked = entry.relPath == current;
			item.onSelect = [this, path = entry.relPath]() { OpenFolder(path); };
			items.push_back(std::move(item));
		}
		if (items.empty()) {
			UI::Widgets::MenuItem none;
			none.label = "No subfolders";
			none.enabled = false;
			items.push_back(std::move(none));
		}
		UI::Widgets::OpenMenu(context, anchorPx, {items.data(), items.size()}, UISystem::GetDefaultFont(), MenuFont());
	}

	bool AssetBrowserPanel::GoToTypedPath(UI::Context& context, const str& typed) {
		const AssetBrowserOps::Entry* entry = AssetBrowserOps::ResolveTypedPath(_entries, typed);
		if (!entry) {
			context.AddClass(_pathField, EditorTheme::Class::FieldRefused);
			context.SetTooltip(_pathField, std::format("Nothing at \"{}\" in this project", typed));
			return false;
		}
		const str relPath = entry->relPath;
		const bool folder = entry->IsDirectory();
		_editingPath = false;
		if (folder) {
			OpenFolder(relPath);
		} else {
			OpenFolder(ParentPath(relPath));
			SelectOnlyFile(relPath);
			_revealAfterBuild = FolderKey(relPath);
		}
		_dirty = true;
		return true;
	}

	void AssetBrowserPanel::BuildFileEntries(UI::Context& context) {
		Vector<ListRow> rows{GetMemoryResource(MemTag::Editor)};
		if (!_mounted || selectedFolder.empty()) {
			_fileList.SetRows(rows);
			return;
		}

		Vector<AssetBrowserOps::Entry> visible{GetMemoryResource(MemTag::Editor)};
		VisibleFileRows(visible);
		rows.reserve(visible.size());
		_builtEntries.reserve(visible.size());

		const bool grid = AssetBrowserOps::SliderIsGrid(iconSize);
		for (const AssetBrowserOps::Entry& entry : visible) {
			FileEntryActions actions = MakeFileActions(entry);

			ListRow row;
			row.id = entry.id;
			row.label = entry.name;
			row.depth = (entry.kind == AssetBrowserOps::EntryKind::Product ||
						 entry.kind == AssetBrowserOps::EntryKind::Link)
							? 1u
							: 0u;
			row.hasChildren = HasFoldOut(entry);
			row.expanded = _expandedAssets.contains(entry.id);
			if (row.hasChildren) {
				row.onToggleExpand = [this, id = entry.id](bool) {
					if (!_expandedAssets.erase(id)) {
						_expandedAssets.insert(id);
					}
					_dirty = true;
				};
			}
			row.onActivate = actions.onDoubleClick;
			row.onRightClick = actions.onRightClick;
			row.onDragRow = actions.onDrag;
			row.onDragOver = actions.onDragOver;
			row.onDrop = actions.onDrop;
			if (!grid) {
				row.onDecorate = [badge = BadgeForEntry(entry, IsVariantRow(entry)), hasChildren = row.hasChildren,
								  expanded = row.expanded, isLink = entry.kind == AssetBrowserOps::EntryKind::Link](
									 UI::Context& context, const UI::Widgets::TreeRowParts& parts) {
					EditorIcons::SetRowGutter(context, parts, hasChildren, expanded,
											  TextDim(), badge.icon, badge.color);
					if (isLink && context.IsAlive(parts.labelElement)) {
						context.SetText(parts.labelElement, UISystem::GetDefaultFont(), RowFont(), TextDim(),
										context.GetTextString(parts.labelElement));
					}
				};
			}
			rows.push_back(std::move(row));

			Guid guid = entry.guid;
			if (grid && !guid.IsValid() && !entry.IsDirectory()) {
				Path virtualPath{VirtualPathOf(entry.relPath)};
				if (AssetDatabase::IsAssetPath(virtualPath)) {
					guid = AssetDatabase::LookupGuid(virtualPath);
				}
			}
			_entryIndexById[entry.id] = static_cast<u32>(_builtEntries.size());
			_builtEntries.push_back({entry.id, entry.relPath, guid, {}, {}, {}});
		}

		_fileList.SetRows(rows);
		PushFileSelectionToList();
	}

	ListViewDesc AssetBrowserPanel::MakeFileListDesc() {
		AssetBrowserOps::GridCellSize cell = AssetBrowserOps::CellSizeForSlider(
			iconSize, EditorTheme::GetMetric(MetricToken::BrowserCellMin),
			EditorTheme::GetMetric(MetricToken::BrowserCellMax),
			EditorTheme::GetMetric(MetricToken::BrowserCellLabel));
		f32 pad = EditorTheme::GetMetric(MetricToken::BrowserCellPad);

		ListViewDesc desc;
		desc.layout = AssetBrowserOps::SliderIsGrid(iconSize) ? ListLayout::Grid : ListLayout::Rows;
		desc.mode = ListSelectionMode::Multiple;
		desc.cellWidthPx = cell.widthPx;
		desc.cellHeightPx = cell.heightPx;
		desc.cellPaddingPx = pad;
		desc.onSelectionChanged = [this]() { PublishFileSelectionFromList(); };
		desc.onDelete = [this]() {
			if (UI::Context* live = GetPanelContext()) {
				ReportRefusals(_selectedFiles, AssetBrowserOps::EntryCommand::Delete);
				Vector<str> targets{GetMemoryResource(MemTag::Editor)};
				SelectedEntryPaths(targets);
				DeleteFiles(*live, targets);
			}
		};
		desc.onDuplicate = [this]() {
			ReportRefusals(_selectedFiles, AssetBrowserOps::EntryCommand::Duplicate);
			Vector<str> targets{GetMemoryResource(MemTag::Editor)};
			SelectedEntryPaths(targets);
			DuplicateFiles(targets);
		};
		desc.onRename = [this](u64 row) {
			UI::Context* live = GetPanelContext();
			if (!live) {
				return false;
			}
			Vector<u64> one{GetMemoryResource(MemTag::Editor)};
			one.push_back(row);
			if (ReportRefusals(one, AssetBrowserOps::EntryCommand::Rename)) {
				return true;
			}
			return BeginRenameEntry(*live, PathOf(row));
		};
		desc.onEmptyRightClick = [this]() {
			if (UI::Context* live = GetPanelContext()) {
				OpenCreateMenu(*live, live->GetPointerPosition());
			}
		};
		desc.onEmptyClick = [this]() { ClearFileSelection(); };
		desc.onBuildCell = [this](UI::Context& context, u64 id, UI::ElementHandle well) { BuildCellPicture(context, id, well); };
		desc.onDecorateCellLabel = [this](UI::Context& context, u64 id, UI::ElementHandle label) {
			const AssetBrowserOps::Entry* row = RowOf(id);
			if (row && row->kind == AssetBrowserOps::EntryKind::Link && context.IsAlive(label)) {
				context.SetText(label, UISystem::GetDefaultFont(), CellFont(), TextDim(), context.GetTextString(label));
			}
		};
		return desc;
	}

	void AssetBrowserPanel::EnsureFileList(UI::Context& context) {
		ListViewDesc desc = MakeFileListDesc();
		if (_fileListBuilt) {
			if (desc.layout != _fileListLayout || desc.cellWidthPx != _fileListCellWidthPx ||
				desc.cellHeightPx != _fileListCellHeightPx) {
				_fileListLayout = desc.layout;
				_fileListCellWidthPx = desc.cellWidthPx;
				_fileListCellHeightPx = desc.cellHeightPx;
				_fileList.SetLayout(desc.layout, desc.cellWidthPx, desc.cellHeightPx, desc.cellPaddingPx);
			}
			return;
		}
		_fileListBuilt = true;
		_fileListLayout = desc.layout;
		_fileListCellWidthPx = desc.cellWidthPx;
		_fileListCellHeightPx = desc.cellHeightPx;
		context.ClearChildren(_fileListHost);
		_fileList.Build(this, context, _fileListHost, std::move(desc));
	}

	void AssetBrowserPanel::BuildCellPicture(UI::Context& context, u64 id, UI::ElementHandle well) {
		auto it = _entryIndexById.find(id);
		if (it == _entryIndexById.end()) {
			return;
		}
		BuiltEntry& built = _builtEntries[it->second];
		f32 pad = EditorTheme::GetMetric(MetricToken::BrowserCellPad);
		f32 wellSide = std::max(1.0f, _fileListCellWidthPx - pad * 2.0f);

		auto squareWell = [wellSide](UI::Style style) {
			style.width = UI::Length::Pixel(wellSide);
			style.height = UI::Length::Pixel(wellSide);
			style.flexShrink = 0.0f;
			return style;
		};

		const StringHash castsShadow("ed-asset-cell-picture");
		UI::ElementHandle image = context.CreateElement();
		context.AppendChild(well, image);
		context.SetHoverable(image, false);
		context.AddClass(image, castsShadow);
		UI::Style imageStyle = squareWell({});
		imageStyle.display = UI::Display::None;
		context.SetStyle(image, imageStyle);

		UI::ElementHandle badge = context.CreateElement();
		context.AppendChild(well, badge);
		context.SetHoverable(badge, false);
		context.AddClass(badge, castsShadow);
		const AssetBrowserOps::Entry* row = RowOf(id);
		bool isVariant = row && IsVariantRow(*row);
		EditorPreviews::TypeBadge typeBadge = row ? BadgeForEntry(*row, isVariant)
												  : EditorPreviews::BadgeForFileName(Path(built.relPath).GetFilename());
		f32 glyphPx = AssetBrowserOps::BadgeGlyphSizeFor(wellSide);
		EditorIcons::SetGlyphOrText(context, badge, typeBadge.icon, "?", typeBadge.color, glyphPx, glyphPx);
		UI::Style badgeStyle = squareWell(context.GetStyle(badge));
		badgeStyle.textAlign = UI::Align::Center;
		badgeStyle.textAlignVertical = UI::Align::Center;
		context.SetStyle(badge, badgeStyle);

		UI::ElementHandle variantMark{};
		if (isVariant) {
			variantMark = context.CreateElement();
			context.AppendChild(well, variantMark);
			context.SetHoverable(variantMark, false);
			context.AddClass(variantMark, StringHash("ed-asset-cell-variant"));
			f32 markPx = EditorIcons::GutterSizePx();
			EditorIcons::SetGlyph(context, variantMark, EditorIcons::Glyph::AssetPrefabVariant,
								  EditorTheme::GetColor(ColorToken::AssetPrefab), markPx);
			UI::Style markStyle = context.GetStyle(variantMark);
			markStyle.position = UI::Position::Absolute;
			markStyle.right = UI::Length::Pixel(2.0f);
			markStyle.top = UI::Length::Pixel(2.0f);
			markStyle.width = UI::Length::Pixel(markPx);
			markStyle.height = UI::Length::Pixel(markPx);
			markStyle.flexShrink = 0.0f;
			markStyle.display = UI::Display::None;
			context.SetStyle(variantMark, markStyle);
		}

		built.image = image;
		built.badge = badge;
		built.variantMark = variantMark;
		built.painted = {};

		AssetPreviewEntry cached;
		if (built.guid.IsValid() && EditorPreviewStore::Peek(built.guid, cached) && cached.texture.IsValid()) {
			imageStyle.background = UI::Brush::ImageBrush(cached.texture, cached.sampler);
			imageStyle.display = UI::Display::Flex;
			context.SetStyle(image, imageStyle);
			SetBadgeShown(context, built, false);
			built.painted = cached.texture;
		}
	}

	void AssetBrowserPanel::RequestVisiblePreviews(UI::Context& context) {
		if (_fileListLayout != ListLayout::Grid || _builtEntries.empty()) {
			return;
		}

		Vector<u64> onScreen{GetMemoryResource(MemTag::Editor)};
		_fileList.GetVisibleRows(onScreen);

		u32 asked = std::min(static_cast<u32>(onScreen.size()), EditorPreviewStore::GetCapacity());

		for (u32 slot = 0; slot < asked; ++slot) {
			auto it = _entryIndexById.find(onScreen[slot]);
			if (it == _entryIndexById.end()) {
				continue;
			}
			BuiltEntry& built = _builtEntries[it->second];
			if (!built.guid.IsValid() || !context.IsAlive(built.image)) {
				continue;
			}
			AssetPreviewEntry preview;
			if (EditorPreviewStore::Request(built.guid, preview)) {
				if (built.painted == preview.texture) {
					continue;
				}
				UI::Style style = context.GetStyle(built.image);
				style.background = UI::Brush::ImageBrush(preview.texture, preview.sampler);
				style.display = UI::Display::Flex;
				context.SetStyle(built.image, style);
				SetBadgeShown(context, built, false);
				built.painted = preview.texture;
			} else if (built.painted.IsValid()) {
				UI::Style style = context.GetStyle(built.image);
				style.display = UI::Display::None;
				context.SetStyle(built.image, style);
				SetBadgeShown(context, built, true);
				built.painted = {};
			}
		}
	}

	void AssetBrowserPanel::SetBadgeShown(UI::Context& context, const BuiltEntry& built, bool shown) {
		auto setDisplay = [&context](UI::ElementHandle element, bool visible) {
			if (!context.IsAlive(element)) {
				return;
			}
			UI::Style style = context.GetStyle(element);
			UI::Display wanted = visible ? UI::Display::Flex : UI::Display::None;
			if (style.display == wanted) {
				return;
			}
			style.display = wanted;
			context.SetStyle(element, style);
		};
		setDisplay(built.badge, shown);
		setDisplay(built.variantMark, !shown);
	}

	void AssetBrowserPanel::BuildFooter(UI::Context& context) {
		context.ClearChildren(_footerHost);

		UI::Widgets::Spacer(context, _footerHost);

		UI::Widgets::SliderDesc desc;
		desc.fontSizePx = CellFont();
		desc.minValue = 0.0f;
		desc.maxValue = 1.0f;
		desc.value = std::clamp(iconSize, 0.0f, 1.0f);
		desc.onChange = [this](f32 value) {
			if (iconSize == value) {
				return;
			}
			iconSize = value;
			_dirty = true;
		};
		UI::ElementHandle sliderHost = context.CreateElement();
		context.AppendChild(_footerHost, sliderHost);
		context.AddClass(sliderHost, StringHash("ed-browser-slider"));
		UI::Style hostStyle;
		hostStyle.width = UI::Length::Pixel(EditorTheme::GetMetric(MetricToken::BrowserSliderWidth));
		hostStyle.flexGrow = 0.0f;
		hostStyle.flexShrink = 0.0f;
		context.SetStyle(sliderHost, hostStyle);
		_sliderParts = UI::Widgets::Slider(context, sliderHost, desc);
	}

	void AssetBrowserPanel::DeleteFiles(UI::Context& context, const Vector<str>& targets) {
		if (targets.empty()) {
			return;
		}
		str prompt = targets.size() > 1 ? std::format("Delete {} items?", targets.size())
					 : FolderExists(targets.front())
						 ? "Delete folder '" + targets.front() + "' and all its contents?"
						 : "Delete '" + Path(targets.front()).GetFilename() + "'?";
		SceneFileFlows::OpenConfirmPopup(
			context, prompt, "Delete",
			[this, targets]() {
				for (const str& relPath : targets) {
					AssetBrowserOps::DeleteEntry(Path(VirtualPathOf(relPath)));
				}
				Rescan();
			},
			"", {});
	}

	void AssetBrowserPanel::DuplicateFiles(const Vector<str>& targets) {
		str lastCreated;
		for (const str& relPath : targets) {
			str created = AssetBrowserOps::DuplicateEntry(Path(VirtualPathOf(relPath)));
			if (!created.empty()) {
				lastCreated = created;
			}
		}
		if (lastCreated.empty()) {
			return;
		}
		Rescan();
		SelectOnlyFile(lastCreated);
	}

	void AssetBrowserPanel::CreatePrefabVariants(const Vector<str>& targets) {
		str lastCreated;
		for (const str& relPath : targets) {
			str created;
			if (AssetBrowserOps::CreatePrefabVariant(Path(VirtualPathOf(relPath)), &created).IsValid()) {
				lastCreated = created;
			}
		}
		if (lastCreated.empty()) {
			return;
		}
		Rescan();
		SelectOnlyFile(lastCreated);
	}

	void AssetBrowserPanel::CreatePrefabsFrom(const Vector<Handle<SceneObject>>& objects, const str& folderRelPath) {
		const Path folder{VirtualPathOf(folderRelPath)};
		str lastCreated;
		for (Handle<SceneObject> object : objects) {
			str created;
			const Guid made = SceneSystem::IsPrefabLinked(object)
								  ? AssetBrowserOps::CreatePrefabVariantFromObject(object, folder, &created)
								  : AssetBrowserOps::CreatePrefabFromObject(object, folder, &created);
			if (made.IsValid()) {
				lastCreated = created;
			}
		}
		if (lastCreated.empty()) {
			return;
		}
		Rescan();
		SelectOnlyFile(lastCreated);
	}

	void AssetBrowserPanel::OfferPrefabVariant(UI::Context& context, Handle<SceneObject> root, const str& folderRelPath) {
		SceneObject* resolved = root.Get();
		if (!resolved || folderRelPath.empty()) {
			return;
		}
		Path folder{VirtualPathOf(folderRelPath)};
		SceneFileFlows::OpenConfirmPopup(
			context, "'" + str(resolved->name) + "' is a prefab instance. Create a Prefab Variant from it?",
			"Prefab Variant", [this, root, folder]() {
				str relPath;
				if (AssetBrowserOps::CreatePrefabVariantFromObject(root, folder, &relPath).IsValid()) {
					Rescan();
					SelectOnlyFile(relPath);
				}
			},
			"", {});
	}

	void AssetBrowserPanel::DeleteFolders(UI::Context& context, const Vector<str>& targets) {
		Vector<str> doomed{GetMemoryResource(MemTag::Editor)};
		for (const str& relPath : targets) {
			if (!ParentPath(relPath).empty()) {
				doomed.push_back(relPath);
			}
		}
		if (doomed.empty()) {
			return;
		}
		str prompt = doomed.size() > 1
						 ? std::format("Delete {} folders and all their contents?", doomed.size())
						 : "Delete folder '" + doomed.front() + "' and all its contents?";
		SceneFileFlows::OpenConfirmPopup(
			context, prompt, "Delete",
			[this, doomed]() {
				for (const str& relPath : doomed) {
					if (!AssetBrowserOps::DeleteEntry(Path(VirtualPathOf(relPath)))) {
						continue;
					}
					if (selectedFolder == relPath || selectedFolder.starts_with(relPath + "/")) {
						selectedFolder.clear();
					}
				}
				Rescan();
			},
			"", {});
	}

	bool AssetBrowserPanel::BeginRenameEntry(UI::Context& context, const str& relPath) {
		if (FolderExists(relPath)) {
			return BeginRenameFolder(context, relPath);
		}
		if (relPath.empty() || !EntryExists(relPath)) {
			return false;
		}
		str virtualPath = VirtualPathOf(relPath);
		const str fileName = Path(relPath).GetFilename();
		SceneFileFlows::OpenTextInputPopup(
			context, "Rename", AssetBrowserOps::RenameStem(fileName), [this, virtualPath, relPath, fileName](const str& typed) {
				const str newName = AssetBrowserOps::RenamedFileName(fileName, typed);
				if (newName.empty() || !AssetBrowserOps::RenameEntry(Path(virtualPath), newName)) {
					return;
				}
				str parent = ParentPath(relPath);
				RemapViewState(relPath, parent.empty() ? newName : parent + "/" + newName);
				Rescan();
			});
		return true;
	}

	bool AssetBrowserPanel::BeginRenameFolder(UI::Context& context, const str& relPath) {
		if (relPath.empty() || ParentPath(relPath).empty()) {
			return false;
		}
		str virtualDir = VirtualPathOf(relPath);
		SceneFileFlows::OpenTextInputPopup(
			context, "Rename Folder", Path(relPath).GetFilename(), [this, virtualDir, relPath](const str& newName) {
				if (!AssetBrowserOps::RenameEntry(Path(virtualDir), newName)) {
					return;
				}
				str parent = ParentPath(relPath);
				RemapViewState(relPath, parent.empty() ? newName : parent + "/" + newName);
				Rescan();
			});
		return true;
	}

	Path AssetBrowserPanel::SelectedFolderVirtual() const {
		if (selectedFolder.empty()) {
			return {};
		}
		return Path(VirtualPathOf(selectedFolder));
	}

	void AssetBrowserPanel::CreateAssetInSelectedFolder(StringHash assetTypeId, const str& fileName, u32 templateIndex) {
		str relPath;
		Guid guid = AssetBrowserOps::CreateAsset(SelectedFolderVirtual(), assetTypeId, fileName, &relPath, templateIndex);
		if (!guid.IsValid()) {
			return;
		}
		Rescan();
		SelectOnlyFile(relPath);
	}

	void AssetBrowserPanel::BuildCreateItems(UI::Context& context, Vector<UI::Widgets::MenuItem>& out) {
		UI::Context* contextPtr = &context;

		UI::Widgets::MenuItem folder;
		folder.label = "New Folder";
		folder.onSelect = [this, contextPtr]() {
			SceneFileFlows::OpenTextInputPopup(*contextPtr, "New Folder", "New Folder", [this](const str& name) {
				if (AssetBrowserOps::CreateFolder(SelectedFolderVirtual(), name)) {
					Rescan();
				}
			});
		};
		out.push_back(std::move(folder));

		UI::Widgets::MenuItem create;
		create.label = "Create";
		create.separatorBefore = true;
		Vector<UI::Widgets::MenuItem> children{GetMemoryResource(MemTag::Editor)};

		UI::Widgets::MenuItem csScript;
		csScript.label = "C# Script";
		csScript.onSelect = [this, contextPtr]() {
			SceneFileFlows::OpenTextInputPopup(*contextPtr, "New C# Script", "NewScript", [this](const str& className) {
				str relPath;
				if (!AssetBrowserOps::CreateCSharpScript(SelectedFolderVirtual(), className, &relPath)) {
					return;
				}
				Rescan();
				SelectOnlyFile(relPath);
				Path physical = FileSystem::Resolve(Path("project://" + relPath));
				AssetBrowserOps::OpenInExternalEditor(physical, 1);
				if (GameModuleHost* moduleHost = GameModuleHost::GetActive()) {
					moduleHost->RearmScriptProject();
					moduleHost->RequestCompile();
				}
			});
		};
		children.push_back(std::move(csScript));

		Vector<AssetBrowserOps::CreateMenuEntry> entries{GetMemoryResource(MemTag::Editor)};
		AssetBrowserOps::GetCreateMenuEntries(entries);
		for (const AssetBrowserOps::CreateMenuEntry& entry : entries) {
			Vector<UI::Widgets::MenuItem>* level = &children;
			std::string_view rest = entry.label;
			for (size_t slash = rest.find('/'); slash != std::string_view::npos; slash = rest.find('/')) {
				const std::string_view submenu = rest.substr(0, slash);
				rest = rest.substr(slash + 1);
				auto found = std::find_if(level->begin(), level->end(),
										  [submenu](const UI::Widgets::MenuItem& item) { return item.label == submenu; });
				if (found == level->end()) {
					UI::Widgets::MenuItem parent;
					parent.label = str(submenu);
					level->push_back(std::move(parent));
					found = level->end() - 1;
				}
				level = &found->children;
			}
			UI::Widgets::MenuItem item;
			item.label = str(rest);
			const StringHash typeId = entry.typeId;
			const str fileName = entry.fileName;
			const u32 templateIndex = entry.templateIndex;
			item.onSelect = [this, typeId, fileName, templateIndex]() { CreateAssetInSelectedFolder(typeId, fileName, templateIndex); };
			level->push_back(std::move(item));
		}
		create.children = std::move(children);
		create.enabled = !create.children.empty();
		out.push_back(std::move(create));
	}

	void AssetBrowserPanel::OpenCreateMenu(UI::Context& context, Vec2 anchorPx) {
		Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
		BuildCreateItems(context, items);

		UI::Context* contextPtr = &context;
		UI::Widgets::MenuItem reveal;
		reveal.label = "Show in Explorer";
		reveal.separatorBefore = true;
		reveal.onSelect = [this]() { AssetBrowserOps::RevealInExplorer(SelectedFolderVirtual()); };
		items.push_back(std::move(reveal));

		UI::Widgets::MenuItem refresh;
		refresh.label = "Refresh";
		refresh.onSelect = [this]() {
			AssetDatabase::RebuildDatabase();
			Rescan();
		};
		items.push_back(std::move(refresh));

		UI::Widgets::OpenMenu(*contextPtr, anchorPx, {items.data(), items.size()}, UISystem::GetDefaultFont(), MenuFont());
	}

	void AssetBrowserPanel::OpenFolderContextMenu(UI::Context& context, const str& relPath, bool isRoot) {
		UI::Context* contextPtr = &context;
		str virtualDir = VirtualPathOf(relPath);

		Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
		BuildCreateItems(context, items);
		items.push_back({"Show in Explorer", "", true, true, [virtualDir]() {
							 AssetBrowserOps::RevealInExplorer(Path(virtualDir));
						 }});
		if (!isRoot) {
			Vector<str> targets{GetMemoryResource(MemTag::Editor)};
			u64 clickedId = FolderKey(relPath);
			if (std::find(_selectedFolders.begin(), _selectedFolders.end(), clickedId) != _selectedFolders.end()) {
				SelectedFolderPaths(targets);
			} else {
				targets.push_back(relPath);
			}
			bool many = targets.size() > 1;
			items.push_back({"Rename", EditorInputOps::ChordLabel(EditorInputOps::Shortcut::RenameSelection), !many, false,
							 [this, contextPtr, relPath]() { BeginRenameFolder(*contextPtr, relPath); }});
			items.push_back({many ? std::format("Delete {} Folders", targets.size()) : str("Delete"),
							 EditorInputOps::ChordLabel(EditorInputOps::Shortcut::DeleteSelection), true, true,
							 [this, contextPtr, targets]() { DeleteFolders(*contextPtr, targets); }});
		}
		UI::Widgets::OpenMenu(context, context.GetPointerPosition(),
							  {items.data(), items.size()}, UISystem::GetDefaultFont(), MenuFont());
	}

	void AssetBrowserPanel::OpenFoldOutContextMenu(UI::Context& context, const AssetBrowserOps::Entry& entry) {
		UI::Context* contextPtr = &context;
		Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
		bool isProduct = entry.kind == AssetBrowserOps::EntryKind::Product;

		if (isProduct) {
			bool canExtract = AssetBrowserOps::RefusalFor(entry, AssetBrowserOps::EntryCommand::Extract).empty();
			items.push_back({"Extract", "", canExtract, false,
							 [this, entry]() { ExtractProductRow(entry); }});
		} else {
			items.push_back({"Select File", "", true, false,
							 [this, relPath = entry.relPath]() { SelectOnlyFile(relPath); }});
		}

		items.push_back({"Copy Path", "", true, false, [entry]() {
							 WindowSystem::SetClipboardText(
								 entry.kind == AssetBrowserOps::EntryKind::Product
									 ? std::format("{} : {}", VirtualPathOf(entry.sourceRelPath), entry.productKey)
									 : VirtualPathOf(entry.relPath));
						 }});
		items.push_back({"Show in Explorer", "", true, false, [entry]() {
							 AssetBrowserOps::RevealEntry(entry);
						 }});

		Vector<u64> one{GetMemoryResource(MemTag::Editor)};
		one.push_back(entry.id);
		items.push_back({"Rename", EditorInputOps::ChordLabel(EditorInputOps::Shortcut::RenameSelection), false, false,
						 []() {}});
		items.push_back({"Delete", EditorInputOps::ChordLabel(EditorInputOps::Shortcut::DeleteSelection), false, true,
						 []() {}});
		UI::Widgets::OpenMenu(context, context.GetPointerPosition(),
							  {items.data(), items.size()}, UISystem::GetDefaultFont(), MenuFont());
		ReportRefusals(one, AssetBrowserOps::EntryCommand::Delete);
	}

	void AssetBrowserPanel::OpenFileContextMenu(UI::Context& context, const AssetBrowserOps::Entry& entry) {
		if (entry.kind == AssetBrowserOps::EntryKind::Product || entry.kind == AssetBrowserOps::EntryKind::Link) {
			OpenFoldOutContextMenu(context, entry);
			return;
		}
		UI::Context* contextPtr = &context;
		str virtualPath = VirtualPathOf(entry.relPath);

		Vector<str> targets{GetMemoryResource(MemTag::Editor)};
		SelectionTargets(entry.relPath, targets);
		bool many = targets.size() > 1;

		Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
		items.push_back({"Rename", EditorInputOps::ChordLabel(EditorInputOps::Shortcut::RenameSelection), !many, false,
						 [this, contextPtr, relPath = entry.relPath]() { BeginRenameEntry(*contextPtr, relPath); }});
		items.push_back({many ? std::format("Duplicate {} Items", targets.size()) : str("Duplicate"),
						 EditorInputOps::ChordLabel(EditorInputOps::Shortcut::DuplicateSelection), true, false,
						 [this, targets]() { DuplicateFiles(targets); }});
		bool allPrefabs = true;
		for (const str& target : targets) {
			allPrefabs = allPrefabs && AssetBrowserOps::CanCreatePrefabVariant(Path(VirtualPathOf(target)));
		}
		items.push_back({many ? std::format("Create {} Prefab Variants", targets.size()) : str("Create Prefab Variant"),
						 "", allPrefabs, false, [this, targets]() { CreatePrefabVariants(targets); }});
		items.push_back({"Copy Path", "", true, false, [targets]() {
							 str joined;
							 for (const str& relPath : targets) {
								 if (!joined.empty()) {
									 joined += "\n";
								 }
								 joined += VirtualPathOf(relPath);
							 }
							 WindowSystem::SetClipboardText(joined);
						 }});
		items.push_back({"Show in Explorer", "", true, false, [virtualPath]() {
							 AssetBrowserOps::RevealInExplorer(Path(virtualPath));
						 }});
		items.push_back({many ? std::format("Delete {} Items", targets.size()) : str("Delete"),
						 EditorInputOps::ChordLabel(EditorInputOps::Shortcut::DeleteSelection), true, true,
						 [this, contextPtr, targets]() { DeleteFiles(*contextPtr, targets); }});
		UI::Widgets::OpenMenu(context, context.GetPointerPosition(),
							  {items.data(), items.size()}, UISystem::GetDefaultFont(), MenuFont());
	}

	EditorIcons::Icon AssetBrowserPanel::GetTabIcon() const {
		return EditorIcons::Glyph::PanelAssets;
	}

} // namespace Shalltear
