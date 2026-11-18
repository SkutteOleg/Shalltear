#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Object.h"
#include "Shalltear/Core/Types.h"
#include "Shalltear/Core/UI/Context.h"
#include "Shalltear/Core/Window/Window.h"
#include "Shalltear/Core/Window/WindowEvents.h"

#include "Shalltear/Editor/EditorAPI.h"
#include "Shalltear/Editor/EditorInputOps.h"
#include "Shalltear/Editor/EditorListView.h"
#include "Shalltear/Editor/ProjectOps.h"

#include <span>

namespace Shalltear {

	class SHALLTEAR_EDITOR_API ProjectHub : public Object {
	public:
		ProjectHub();
		explicit ProjectHub(Path registryPath);

		static str RunModal();

		void Refresh();
		void SetFilter(str filter);
		const str& GetFilter() const { return _filter; }
		void Select(const str& projectPath);
		str GetSelectedPath() const;
		void Choose(const str& projectPath);
		const str& GetChosenPath() const { return _chosenPath; }

		bool OnCommand(PanelCommand command);

		const Vector<ProjectOps::ProjectEntry>& GetEntries() const { return _entries; }
		u32 GetVisibleCount() const;

		str GetDefaultLocation() const;

		bool CreateAndAdd(const str& parentDirectory, const str& name, bool includeCppModule = true,
						  std::span<const str> optInPackages = {});
		bool AddExisting(const str& projectPath);
		bool Remove(const str& projectPath);
		bool DeleteFromDisk(const str& projectPath);

		void BuildContent(UI::Context& context, UI::ElementHandle root);

		void OnCloseRequested(const WindowCloseRequestedEvent& event);
		void OnWindowDestroyed(const WindowDestroyedEvent& event);
	private:
		str RunWindow();
		void RunFrame();
		void RebuildRows(UI::Context& context);
		void BuildRowsEmptyState(UI::Context& context, UI::ElementHandle host);
		void DecorateRow(UI::Context& context, const ProjectOps::ProjectEntry& entry,
						 const UI::Widgets::TreeRowParts& parts);
		void RefreshStatusLine(UI::Context& context);
		void SetStatusMessage(str message);

		void SyncSelectionFromList();
		void RemoveSelected();

		static u64 RowIdOf(std::string_view projectPath);

		void OpenNewProjectPopup(UI::Context& context);
		void OpenAddExistingPopup(UI::Context& context);
		void OpenDeleteConfirmPopup(UI::Context& context, const ProjectOps::ProjectEntry& entry);
		void OpenRowMenu(UI::Context& context, const ProjectOps::ProjectEntry& entry, Vec2 anchorPx);

		Path _registryPath;
		Vector<ProjectOps::ProjectEntry> _entries{GetMemoryResource(MemTag::Editor)};
		str _filter;
		Vector<str> _selectedPaths{GetMemoryResource(MemTag::Editor)};
		str _chosenPath;
		str _statusMessage;
		bool _rowsDirty = false;
		bool _done = false;

		Handle<Window> _window;
		u64 _windowId = 0;
		EditorListView _recentsList;
		EditorInputOps::EditorShortcuts _shortcuts;
		UI::ElementHandle _statusLabel;
		UI::ElementHandle _maximizeButtonFace;
		bool _showedMaximized = false;
	};

} // namespace Shalltear
