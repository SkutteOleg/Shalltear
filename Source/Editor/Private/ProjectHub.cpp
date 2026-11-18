#include "Shalltear/Editor/ProjectHub.h"
#include "Shalltear/Editor/EditorChromeOps.h"
#include "Shalltear/Editor/EditorIcons.h"
#include "Shalltear/Editor/EditorTheme.h"

#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/Graphics/RenderSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/UI/UISystem.h"
#include "Shalltear/Core/UI/Widgets.h"
#include "Shalltear/Core/Window/Monitor.h"
#include "Shalltear/Core/Window/WindowSystem.h"

#include "Shalltear/Editor/PackageOps.h"
#include "Shalltear/Editor/ScriptProjectOps.h"

#include "Platform/EditorPlatform.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <memory>

namespace Shalltear {

	namespace {

		using EditorTheme::ColorToken;
		using EditorTheme::MetricToken;

		Color TextNormal() { return EditorTheme::GetColor(ColorToken::Text); }
		Color TextDim() { return EditorTheme::GetColor(ColorToken::TextDim); }
		Color TextFaint() { return EditorTheme::GetColor(ColorToken::TextFaint); }
		Color TextWarn() { return EditorTheme::GetColor(ColorToken::StatusWarn); }
		Color TextError() { return EditorTheme::GetColor(ColorToken::StatusErrorSoft); }

		constexpr f32 COLUMN_NAME = 170.0f;
		constexpr f32 COLUMN_TIME = 128.0f;
		constexpr f32 COLUMN_STATUS = 92.0f;
		f32 RowFontPx() { return EditorTheme::GetMetric(MetricToken::FontRow); }
		f32 BodyFontPx() { return EditorTheme::GetMetric(MetricToken::FontBody); }

		Handle<FontAsset> HubFont() {
			return UISystem::IsInitialized() ? UISystem::GetDefaultFont() : Handle<FontAsset>{};
		}

		str FormatTimestamp(u64 unixSeconds) {
			if (unixSeconds == 0) {
				return "-";
			}
			const std::time_t seconds = static_cast<std::time_t>(unixSeconds);
			std::tm local{};
			char text[32] = {};
			if (EditorPlatform::ToLocalTime(seconds, local) &&
				std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local) != 0) {
				return str(text);
			}
			return std::format("{:%Y-%m-%d %H:%M}Z",
							   std::chrono::sys_seconds{std::chrono::seconds{static_cast<i64>(unixSeconds)}});
		}

		const char* StatusText(ProjectOps::ProjectStatus status) {
			switch (status) {
			case ProjectOps::ProjectStatus::Missing:
				return "Missing";
			case ProjectOps::ProjectStatus::Invalid:
				return "Not a project";
			default:
				return "OK";
			}
		}

		Color StatusColor(ProjectOps::ProjectStatus status) {
			switch (status) {
			case ProjectOps::ProjectStatus::Missing:
				return TextError();
			case ProjectOps::ProjectStatus::Invalid:
				return TextWarn();
			default:
				return TextFaint();
			}
		}

		UI::ElementHandle AddCell(UI::Context& context, UI::ElementHandle row, std::string_view text,
								  f32 widthPx, Color color, f32 fontSizePx = RowFontPx()) {
			UI::ElementHandle cell = context.CreateElement();
			context.AppendChild(row, cell);
			context.AddClass(cell, EditorTheme::Class::RowText);
			UI::Style style;
			if (widthPx > 0.0f) {
				style.width = UI::Length::Pixel(widthPx);
			} else {
				style.width = UI::Length::Pixel(0.0f);
				style.flexGrow = 1.0f;
			}
			style.margin = {0.0f, 0.0f, 10.0f, 0.0f};
			style.overflow = UI::Overflow::Hidden;
			context.SetStyle(cell, style);
			context.SetText(cell, HubFont(), fontSizePx, color, text);
			context.SetHoverable(cell, false);
			return cell;
		}

		void AddLeadingSpacer(UI::Context& context, UI::ElementHandle row) {
			UI::ElementHandle spacer = context.CreateElement();
			context.AppendChild(row, spacer);
			UI::Style style;
			style.width = UI::Length::Pixel(EditorTheme::GetMetric(MetricToken::TreeGutter) +
											EditorTheme::GetMetric(MetricToken::IconGutter) +
											2.0f * EditorTheme::GetMetric(MetricToken::IconGap));
			style.flexShrink = 0.0f;
			context.SetStyle(spacer, style);
			context.SetHoverable(spacer, false);
		}

		UI::ElementHandle BeginPopupCard(UI::Context& context) {
			UI::ElementHandle card = context.CreateElement();
			UI::Style style;
			style.minWidth = UI::Length::Pixel(360.0f);
			context.SetStyle(card, style);
			context.AddClass(card, EditorTheme::Class::PopupCard);
			UI::Context* contextPtr = &context;
			context.Events(card).onPopupClosed = [contextPtr, card]() {
				contextPtr->DestroyElementNextUpdate(card);
			};
			return card;
		}

		UI::ElementHandle AddPopupLine(UI::Context& context, UI::ElementHandle card, std::string_view text,
									   f32 fontSizePx, Color color) {
			UI::ElementHandle line = context.CreateElement();
			context.AppendChild(card, line);
			UI::Style style;
			style.margin = {0.0f, 0.0f, 0.0f, 8.0f};
			context.SetStyle(line, style);
			context.SetText(line, HubFont(), fontSizePx, color, text);
			return line;
		}

		UI::ElementHandle AddPopupField(UI::Context& context, UI::ElementHandle card,
										std::string_view label, const str& initialText) {
			AddPopupLine(context, card, label, BodyFontPx(), TextDim());
			UI::Widgets::TextFieldDesc desc;
			desc.text = initialText;
			desc.font = HubFont();
			desc.fontSizePx = RowFontPx();
			UI::ElementHandle field = UI::Widgets::TextField(context, card, desc);
			UI::Style style = context.GetStyle(field);
			style.margin = {0.0f, 0.0f, 0.0f, 10.0f};
			context.SetStyle(field, style);
			return field;
		}

		void AddPopupToggle(UI::Context& context, UI::ElementHandle card, std::string_view label,
							bool initialValue, std::function<void(bool)> onChange) {
			UI::ElementHandle row = context.CreateElement();
			context.AppendChild(card, row);
			UI::Style rowStyle;
			rowStyle.flexDirection = UI::FlexDirection::Row;
			rowStyle.alignItems = UI::Align::Center;
			rowStyle.margin = {0.0f, 0.0f, 0.0f, 10.0f};
			context.SetStyle(row, rowStyle);

			UI::Widgets::Checkbox(context, row, initialValue, std::move(onChange));

			UI::ElementHandle text = context.CreateElement();
			context.AppendChild(row, text);
			UI::Style textStyle;
			textStyle.margin = {6.0f, 0.0f, 0.0f, 0.0f};
			context.SetStyle(text, textStyle);
			context.SetText(text, HubFont(), EditorTheme::GetMetric(MetricToken::FontBody), TextDim(), label);
			context.SetHoverable(text, false);
		}

		UI::ElementHandle AddButtonRow(UI::Context& context, UI::ElementHandle card) {
			UI::ElementHandle row = context.CreateElement();
			context.AppendChild(card, row);
			UI::Style style;
			style.flexDirection = UI::FlexDirection::Row;
			style.justifyContent = UI::Align::End;
			style.margin = {0.0f, 6.0f, 0.0f, 0.0f};
			context.SetStyle(row, style);
			return row;
		}

		// close the popup first (it owns the callback storage)
		void AddPopupButton(UI::Context& context, UI::ElementHandle row, UI::ElementHandle card,
							std::string_view label, std::function<void()> action, bool isDefault = false) {
			UI::Widgets::ButtonDesc desc;
			desc.label = label;
			desc.font = HubFont();
			desc.fontSizePx = RowFontPx();
			if (isDefault) {
				desc.defaultForPopup = card;
			}
			UI::Context* contextPtr = &context;
			desc.onClick = [contextPtr, card, action = std::move(action)]() {
				contextPtr->ClosePopup(card);
				if (action) {
					action();
				}
			};
			UI::ElementHandle button = UI::Widgets::Button(context, row, desc);
			UI::Style style = context.GetStyle(button);
			style.margin = {6.0f, 0.0f, 0.0f, 0.0f};
			context.SetStyle(button, style);
		}

		constexpr UI::PopupFlags DIALOG_FLAGS =
			UI::PopupFlags::NoOutsideDismiss | UI::PopupFlags::Center | UI::PopupFlags::Scrim;

	} // anonymous namespace

	ProjectHub::ProjectHub() : ProjectHub(ProjectOps::GetDefaultRegistryPath()) {}

	ProjectHub::ProjectHub(Path registryPath) : _registryPath(std::move(registryPath)) {
		Refresh();
	}

	void ProjectHub::Refresh() {
		ProjectOps::LoadRegistry(_registryPath, _entries);
		std::erase_if(_selectedPaths, [this](const str& selected) {
			for (const ProjectOps::ProjectEntry& entry : _entries) {
				if (ProjectOps::PathsEqual(entry.path, selected) && entry.IsOpenable()) {
					return false;
				}
			}
			return true;
		});
		_rowsDirty = true;
	}

	void ProjectHub::SetFilter(str filter) {
		_filter = std::move(filter);
		_statusMessage.clear();
		_rowsDirty = true;
	}

	u32 ProjectHub::GetVisibleCount() const {
		u32 count = 0;
		for (const ProjectOps::ProjectEntry& entry : _entries) {
			count += ProjectOps::MatchesFilter(entry, _filter) ? 1 : 0;
		}
		return count;
	}

	void ProjectHub::Select(const str& projectPath) {
		for (const ProjectOps::ProjectEntry& entry : _entries) {
			if (ProjectOps::PathsEqual(entry.path, projectPath) && entry.IsOpenable()) {
				_selectedPaths.clear();
				_selectedPaths.push_back(entry.path);
				_statusMessage.clear();
				_rowsDirty = true;
				return;
			}
		}
	}

	str ProjectHub::GetSelectedPath() const {
		return _selectedPaths.empty() ? str{} : _selectedPaths.back();
	}

	void ProjectHub::Choose(const str& projectPath) {
		for (const ProjectOps::ProjectEntry& entry : _entries) {
			if (!ProjectOps::PathsEqual(entry.path, projectPath)) {
				continue;
			}
			if (!entry.IsOpenable()) {
				SetStatusMessage(std::format("'{}' is {}", entry.name, StatusText(entry.status)));
				return;
			}
			_selectedPaths.clear();
			_selectedPaths.push_back(entry.path);
			_chosenPath = entry.path;
			_done = true;
			return;
		}
	}

	str ProjectHub::GetDefaultLocation() const {
		if (!_entries.empty()) {
			const str& newest = _entries.front().path;
			size_t slash = newest.find_last_of('/');
			return slash == str::npos ? newest : newest.substr(0, slash);
		}
		str home = FileSystem::GetUserHomeDirectory().ToString();
		if (home.empty()) {
			return {};
		}
		str documents = home + "/Documents";
		return FileSystem::IsDirectory(Path(documents)) ? documents : home;
	}

	bool ProjectHub::CreateAndAdd(const str& parentDirectory, const str& name, bool includeCppModule,
								  std::span<const str> optInPackages) {
		str created;
		ProjectOps::CreateOptions options;
		options.includeCppModule = includeCppModule;
		options.optInPackages.assign(optInPackages.begin(), optInPackages.end());
		options.sdkRoot = ScriptProjectOps::GetSDKRoot();
		options.editorExecutable = ScriptProjectOps::GetEditorExecutablePath();
		if (!ProjectOps::CreateProject(parentDirectory, name, options, created)) {
			SetStatusMessage(std::format("could not create '{}' in {}", name, parentDirectory));
			return false;
		}
		ProjectOps::AddProject(_registryPath, created);
		Refresh();
		Select(created);
		SetStatusMessage(std::format("created {}", created));
		Choose(created);
		return true;
	}

	bool ProjectHub::AddExisting(const str& projectPath) {
		if (!ProjectOps::AddProject(_registryPath, projectPath)) {
			SetStatusMessage(std::format("'{}' is not a Shalltear project", projectPath));
			return false;
		}
		str normalized = ProjectOps::NormalizePath(projectPath);
		Refresh();
		Select(normalized);
		SetStatusMessage(std::format("added {}", normalized));
		return true;
	}

	bool ProjectHub::Remove(const str& projectPath) {
		bool removed = ProjectOps::RemoveProject(_registryPath, projectPath);
		Refresh();
		if (removed) {
			SetStatusMessage("removed from the list (files untouched)");
		}
		return removed;
	}

	bool ProjectHub::DeleteFromDisk(const str& projectPath) {
		bool deleted = ProjectOps::DeleteProjectFromDisk(_registryPath, projectPath);
		Refresh();
		SetStatusMessage(deleted ? std::format("deleted {}", projectPath)
								 : std::format("could not delete {}", projectPath));
		return deleted;
	}

	void ProjectHub::SetStatusMessage(str message) {
		_statusMessage = std::move(message);
		_rowsDirty = true;
	}

	void ProjectHub::BuildContent(UI::Context& context, UI::ElementHandle root) {
		Handle<FontAsset> font = HubFont();
		UI::Context* contextPtr = &context;

		UI::Style rootStyle;
		rootStyle.flexDirection = UI::FlexDirection::Column;
		rootStyle.alignItems = UI::Align::Stretch;
		rootStyle.width = UI::Length::Percent(1.0f);
		rootStyle.height = UI::Length::Percent(1.0f);
		context.SetStyle(root, rootStyle);
		context.AddClass(root, EditorTheme::Class::HubBody);
		context.ClearChildren(root);

		UI::ElementHandle header = context.CreateElement();
		context.AppendChild(root, header);
		UI::Style headerStyle;
		headerStyle.padding = {16.0f, 12.0f, 16.0f, 12.0f};
		headerStyle.alignItems = UI::Align::Center;
		context.SetStyle(header, headerStyle);
		context.AddClass(header, EditorTheme::Class::ChromeBar);
		context.AddClass(header, EditorChromeOps::CaptionClass);

		UI::ElementHandle title = context.CreateElement();
		context.AppendChild(header, title);
		context.SetText(title, font, EditorTheme::GetMetric(MetricToken::FontTitle),
						EditorTheme::GetColor(ColorToken::TextStrong), "Projects");
		context.SetHoverable(title, false);

		UI::Widgets::Spacer(context, header);

		UI::Widgets::ButtonDesc newButton;
		newButton.label = "New Project...";
		newButton.font = font;
		newButton.fontSizePx = RowFontPx();
		newButton.onClick = [this, contextPtr]() { OpenNewProjectPopup(*contextPtr); };
		UI::ElementHandle newProject = UI::Widgets::Button(context, header, newButton);
		context.AddClass(newProject, EditorTheme::Class::ToolButton);

		UI::Widgets::ButtonDesc addButton;
		addButton.label = "Add Existing...";
		addButton.font = font;
		addButton.fontSizePx = RowFontPx();
		addButton.onClick = [this, contextPtr]() { OpenAddExistingPopup(*contextPtr); };
		UI::ElementHandle add = UI::Widgets::Button(context, header, addButton);
		context.AddClass(add, EditorTheme::Class::ToolButton);
		UI::Style addStyle = context.GetStyle(add);
		addStyle.margin = {6.0f, 0.0f, 0.0f, 0.0f};
		context.SetStyle(add, addStyle);

		UI::Widgets::Separator(context, header);

		Handle<Window> hubWindow = _window;
		_maximizeButtonFace = EditorChromeOps::BuildWindowControls(
								  context, header, _window,
								  [hubWindow]() { WindowSystem::RequestDestroy(hubWindow); })
								  .maximizeFace;
		_showedMaximized = false;

		UI::ElementHandle body = context.CreateElement();
		context.AppendChild(root, body);
		UI::Style bodyStyle;
		bodyStyle.flexDirection = UI::FlexDirection::Column;
		bodyStyle.alignItems = UI::Align::Stretch;
		bodyStyle.padding = {12.0f, 10.0f, 12.0f, 0.0f};
		bodyStyle.height = UI::Length::Pixel(0.0f);
		bodyStyle.flexGrow = 1.0f;
		context.SetStyle(body, bodyStyle);

		UI::ElementHandle searchRow = context.CreateElement();
		context.AppendChild(body, searchRow);
		UI::Style searchStyle;
		searchStyle.flexDirection = UI::FlexDirection::Row;
		searchStyle.alignItems = UI::Align::Center;
		searchStyle.margin = {0.0f, 0.0f, 0.0f, 10.0f};
		context.SetStyle(searchRow, searchStyle);

		UI::ElementHandle searchIcon = context.CreateElement();
		context.AppendChild(searchRow, searchIcon);
		UI::Style searchIconStyle;
		searchIconStyle.margin = {0.0f, 0.0f, 7.0f, 0.0f};
		searchIconStyle.flexShrink = 0.0f;
		context.SetStyle(searchIcon, searchIconStyle);
		EditorIcons::SetGlyphOrText(context, searchIcon, EditorIcons::Glyph::Search, "Search", TextFaint(),
									EditorTheme::GetMetric(MetricToken::IconBody),
									EditorTheme::GetMetric(MetricToken::FontBody));
		context.SetHoverable(searchIcon, false);

		UI::Widgets::TextFieldDesc filterField;
		filterField.text = _filter;
		filterField.font = font;
		filterField.fontSizePx = RowFontPx();
		filterField.onCommit = [this](std::string_view text) { SetFilter(str(text)); };
		UI::ElementHandle filter = UI::Widgets::TextField(context, searchRow, filterField);
		UI::Style filterStyle = context.GetStyle(filter);
		filterStyle.width = UI::Length::Pixel(0.0f);
		filterStyle.flexGrow = 1.0f;
		context.SetStyle(filter, filterStyle);
		context.Events(filter).onTextChange = [this](std::string_view text) { SetFilter(str(text)); };

		UI::ElementHandle columns = context.CreateElement();
		context.AppendChild(body, columns);
		UI::Style columnsStyle;
		columnsStyle.flexDirection = UI::FlexDirection::Row;
		context.SetStyle(columns, columnsStyle);
		context.AddClass(columns, EditorTheme::Class::HubColumns);
		AddLeadingSpacer(context, columns);
		AddCell(context, columns, "NAME", COLUMN_NAME, TextFaint(), EditorTheme::GetMetric(MetricToken::FontTiny));
		AddCell(context, columns, "PATH", 0.0f, TextFaint(), EditorTheme::GetMetric(MetricToken::FontTiny));
		AddCell(context, columns, "CREATED", COLUMN_TIME, TextFaint(), EditorTheme::GetMetric(MetricToken::FontTiny));
		AddCell(context, columns, "MODIFIED", COLUMN_TIME, TextFaint(), EditorTheme::GetMetric(MetricToken::FontTiny));
		AddCell(context, columns, "STATUS", COLUMN_STATUS, TextFaint(), EditorTheme::GetMetric(MetricToken::FontTiny));

		UI::ElementHandle listHost = context.CreateElement();
		context.AppendChild(body, listHost);
		UI::Style listHostStyle;
		listHostStyle.flexDirection = UI::FlexDirection::Row;
		listHostStyle.alignItems = UI::Align::Stretch;
		listHostStyle.width = UI::Length::Percent(1.0f);
		listHostStyle.height = UI::Length::Pixel(0.0f);
		listHostStyle.flexGrow = 1.0f;
		context.SetStyle(listHost, listHostStyle);

		ListViewDesc recents;
		recents.layout = ListLayout::Rows;
		recents.mode = ListSelectionMode::Multiple;
		recents.onSelectionChanged = [this]() { SyncSelectionFromList(); };
		recents.onDelete = [this]() { RemoveSelected(); };
		recents.onBuildTrailing = [this](UI::Context& context, UI::ElementHandle host) {
			BuildRowsEmptyState(context, host);
		};
		_recentsList.Build(nullptr, context, listHost, std::move(recents));

		UI::ElementHandle footer = context.CreateElement();
		context.AppendChild(root, footer);
		UI::Style footerStyle;
		footerStyle.padding = {16.0f, 8.0f, 16.0f, 8.0f};
		context.SetStyle(footer, footerStyle);
		context.AddClass(footer, EditorTheme::Class::ChromeBar);

		_statusLabel = context.CreateElement();
		context.AppendChild(footer, _statusLabel);
		context.SetText(_statusLabel, font, EditorTheme::GetMetric(MetricToken::FontBody), TextDim(), "");

		UI::Widgets::Spacer(context, footer);

		UI::Widgets::ButtonDesc openButton;
		openButton.label = "Open";
		openButton.font = font;
		openButton.fontSizePx = RowFontPx();
		openButton.onClick = [this]() {
			str active = GetSelectedPath();
			if (active.empty()) {
				SetStatusMessage("select a project first");
				return;
			}
			Choose(active);
		};
		UI::Widgets::Button(context, footer, openButton);

		RebuildRows(context);
		RefreshStatusLine(context);
	}

	u64 ProjectHub::RowIdOf(std::string_view projectPath) {
		return StringHash(projectPath).GetValue();
	}

	void ProjectHub::SyncSelectionFromList() {
		_selectedPaths.clear();
		for (u64 id : _recentsList.GetSelected()) {
			for (const ProjectOps::ProjectEntry& entry : _entries) {
				if (RowIdOf(entry.path) == id) {
					_selectedPaths.push_back(entry.path);
					break;
				}
			}
		}
		_statusMessage.clear();
	}

	void ProjectHub::RemoveSelected() {
		Vector<str> doomed{_selectedPaths, GetMemoryResource(MemTag::Editor)};
		u32 removed = 0;
		for (const str& path : doomed) {
			removed += ProjectOps::RemoveProject(_registryPath, path) ? 1u : 0u;
		}
		Refresh();
		if (removed > 0) {
			SetStatusMessage(removed == 1 ? str("removed from the list (files untouched)")
										  : std::format("removed {} from the list (files untouched)", removed));
		}
	}

	bool ProjectHub::OnCommand(PanelCommand command) {
		return _recentsList.HasFocus() && _recentsList.Command(command);
	}

	void ProjectHub::RebuildRows(UI::Context& context) {
		Vector<ListRow> listRows{GetMemoryResource(MemTag::Editor)};
		for (const ProjectOps::ProjectEntry& entry : _entries) {
			if (!ProjectOps::MatchesFilter(entry, _filter)) {
				continue;
			}
			bool openable = entry.IsOpenable();

			ListRow row;
			row.id = RowIdOf(entry.path);
			row.selectable = openable;
			row.label = entry.name;
			str path = entry.path;
			if (openable) {
				row.onActivate = [this, path](UI::Context&) { Choose(path); };
			}
			ProjectOps::ProjectEntry copy = entry;
			row.onRightClick = [this, copy](UI::Context& context) {
				OpenRowMenu(context, copy, context.GetPointerPosition());
			};
			ProjectOps::ProjectEntry decorated = entry;
			row.onDecorate = [this, decorated](UI::Context& context, const UI::Widgets::TreeRowParts& parts) {
				DecorateRow(context, decorated, parts);
			};
			listRows.push_back(std::move(row));
		}
		_recentsList.SetRows(listRows);

		Vector<u64> ids{GetMemoryResource(MemTag::Editor)};
		for (const str& selected : _selectedPaths) {
			ids.push_back(RowIdOf(selected));
		}
		_recentsList.SetSelection(ids);
	}

	void ProjectHub::DecorateRow(UI::Context& context, const ProjectOps::ProjectEntry& entry,
								 const UI::Widgets::TreeRowParts& parts) {
		bool openable = entry.IsOpenable();
		Color primary = openable ? EditorTheme::GetColor(ColorToken::TextStrong) : TextFaint();
		Color secondary = openable ? TextDim() : TextFaint();

		context.AddClass(parts.row, EditorTheme::Class::HubRow);

		EditorIcons::SetRowGutter(context, parts, false, false, secondary, EditorIcons::Glyph::Folder,
								  openable ? EditorTheme::GetColor(ColorToken::AssetFolder) : TextFaint());

		UI::Style nameStyle = context.GetStyle(parts.labelElement);
		nameStyle.width = UI::Length::Pixel(COLUMN_NAME);
		nameStyle.flexShrink = 0.0f;
		context.SetStyle(parts.labelElement, nameStyle);
		context.SetText(parts.labelElement, HubFont(), RowFontPx(), primary, entry.name);

		AddCell(context, parts.row, entry.path, 0.0f, secondary);
		AddCell(context, parts.row, FormatTimestamp(entry.createdUnix), COLUMN_TIME, secondary);
		AddCell(context, parts.row, FormatTimestamp(entry.modifiedUnix), COLUMN_TIME, secondary);
		AddCell(context, parts.row, StatusText(entry.status), COLUMN_STATUS, StatusColor(entry.status));
	}

	void ProjectHub::BuildRowsEmptyState(UI::Context& context, UI::ElementHandle host) {
		if (GetVisibleCount() > 0) {
			return;
		}
		UI::ElementHandle empty = context.CreateElement();
		context.AppendChild(host, empty);
		UI::Style emptyStyle;
		emptyStyle.padding = {6.0f, 22.0f, 6.0f, 22.0f};
		context.SetStyle(empty, emptyStyle);
		context.SetText(empty, HubFont(), RowFontPx(), TextDim(),
						_entries.empty() ? "No projects yet" : "No projects match the search");

		UI::ElementHandle hint = context.CreateElement();
		context.AppendChild(host, hint);
		UI::Style hintStyle;
		hintStyle.padding = {6.0f, 0.0f, 6.0f, 0.0f};
		context.SetStyle(hint, hintStyle);
		context.SetText(hint, HubFont(), RowFontPx(), TextFaint(),
						_entries.empty() ? "Use New Project... to create one, or Add Existing... to point at a folder that already holds a Project.ini"
										 : "Clear the search to see the rest");
	}

	void ProjectHub::RefreshStatusLine(UI::Context& context) {
		if (!context.IsAlive(_statusLabel)) {
			return;
		}
		str text = _statusMessage.empty()
					   ? std::format("{} of {} projects", GetVisibleCount(), _entries.size())
					   : _statusMessage;
		if (_statusMessage.empty() && _selectedPaths.size() > 1) {
			text += std::format(", {} selected", _selectedPaths.size());
		}
		context.SetTextString(_statusLabel, text);
	}

	void ProjectHub::OpenNewProjectPopup(UI::Context& context) {
		UI::ElementHandle card = BeginPopupCard(context);
		AddPopupLine(context, card, "New Project", 14.0f, TextNormal());

		UI::ElementHandle nameField = AddPopupField(context, card, "Name", "MyProject");
		UI::ElementHandle locationField = AddPopupField(context, card, "Location", GetDefaultLocation());

		auto includeCppModule = std::make_shared<bool>(true);
		AddPopupToggle(context, card, "Include C++ module", *includeCppModule,
					   [includeCppModule](bool value) { *includeCppModule = value; });

		Vector<PackageOps::PackageManifest> catalogue;
		Vector<str> diagnostics;
		PackageOps::LoadBuiltInCatalogue(catalogue, diagnostics);
		Vector<PackageOps::PackageManifest> offered;
		PackageOps::ListOptInPackages(catalogue, offered);
		Vector<str> remembered{GetMemoryResource(MemTag::Editor)};
		ProjectOps::LoadOptInChoice(_registryPath, remembered);
		auto optIns = std::make_shared<Vector<str>>(GetMemoryResource(MemTag::Editor));
		for (const PackageOps::PackageManifest& manifest : offered) {
			const bool ticked = std::find(remembered.begin(), remembered.end(), manifest.name) != remembered.end();
			if (ticked) {
				optIns->push_back(manifest.name);
			}
			AddPopupToggle(context, card, "Include " + (manifest.displayName.empty() ? manifest.name : manifest.displayName), ticked,
						   [optIns, name = manifest.name](bool value) {
							   std::erase(*optIns, name);
							   if (value) {
								   optIns->push_back(name);
							   }
						   });
		}

		UI::Context* contextPtr = &context;
		UI::ElementHandle buttons = AddButtonRow(context, card);
		AddPopupButton(context, buttons, card, "Cancel", {});
		AddPopupButton(
			context, buttons, card, "Create",
			[this, contextPtr, nameField, locationField, includeCppModule, optIns]() {
				ProjectOps::SaveOptInChoice(_registryPath, *optIns);
				CreateAndAdd(contextPtr->GetTextString(locationField), contextPtr->GetTextString(nameField), *includeCppModule, *optIns);
			},
			true);
		context.OpenPopup(card, {}, DIALOG_FLAGS);
		context.SetFocus(nameField);
	}

	void ProjectHub::OpenAddExistingPopup(UI::Context& context) {
		UI::ElementHandle card = BeginPopupCard(context);
		AddPopupLine(context, card, "Add Existing Project", 14.0f, TextNormal());
		AddPopupLine(context, card, "the folder holding Project.ini", 11.5f, TextFaint());
		UI::ElementHandle field = AddPopupField(context, card, "Project folder", "");

		UI::Context* contextPtr = &context;
		UI::ElementHandle buttons = AddButtonRow(context, card);
		AddPopupButton(context, buttons, card, "Cancel", {});
		AddPopupButton(context, buttons, card, "Add", [this, contextPtr, field]() { AddExisting(contextPtr->GetTextString(field)); }, true);
		context.OpenPopup(card, {}, DIALOG_FLAGS);
		context.SetFocus(field);
	}

	void ProjectHub::OpenDeleteConfirmPopup(UI::Context& context, const ProjectOps::ProjectEntry& entry) {
		UI::ElementHandle card = BeginPopupCard(context);
		AddPopupLine(context, card, "Delete Project From Disk", 14.0f, TextError());
		AddPopupLine(context, card, entry.path, 11.5f, TextDim());
		AddPopupLine(context, card, "Files in this folder are permanently deleted. This cannot be undone.",
					 11.5f, TextDim());
		UI::ElementHandle field = AddPopupField(context, card, "Type the project name to confirm", "");
		UI::ElementHandle hint = AddPopupLine(context, card, "", 11.5f, TextWarn());

		UI::ElementHandle buttons = AddButtonRow(context, card);
		AddPopupButton(context, buttons, card, "Cancel", {});

		UI::Context* contextPtr = &context;
		str expected = entry.name;
		str path = entry.path;
		UI::Widgets::ButtonDesc deleteButton;
		deleteButton.label = "Delete";
		deleteButton.font = HubFont();
		deleteButton.fontSizePx = RowFontPx();
		deleteButton.defaultForPopup = card;
		deleteButton.onClick = [this, contextPtr, card, field, hint, expected, path]() {
			if (contextPtr->GetTextString(field) != expected) {
				contextPtr->SetTextString(hint, "type \"" + expected + "\" to confirm");
				return;
			}
			contextPtr->ClosePopup(card);
			DeleteFromDisk(path);
		};
		UI::ElementHandle button = UI::Widgets::Button(context, buttons, deleteButton);
		UI::Style style = context.GetStyle(button);
		style.margin = {6.0f, 0.0f, 0.0f, 0.0f};
		context.SetStyle(button, style);

		context.OpenPopup(card, {}, DIALOG_FLAGS);
		context.SetFocus(field);
	}

	void ProjectHub::OpenRowMenu(UI::Context& context, const ProjectOps::ProjectEntry& entry, Vec2 anchorPx) {
		Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
		str path = entry.path;
		bool openable = entry.IsOpenable();
		UI::Context* contextPtr = &context;

		items.push_back({"Open", "", openable, false, [this, path]() { Choose(path); }});
		items.push_back({"Copy Path", "", true, false, [path]() {
							 WindowSystem::SetClipboardText(path);
						 }});
		items.push_back({"Remove from List", "", true, true, [this, path]() { Remove(path); }});
		items.push_back({"Delete from Disk...", "", openable, false, [this, contextPtr, entry]() {
							 OpenDeleteConfirmPopup(*contextPtr, entry);
						 }});

		UI::Widgets::OpenMenu(context, anchorPx, {items.data(), items.size()}, HubFont(), BodyFontPx());
	}

	str ProjectHub::RunModal() {
		if (!RenderSystem::IsRenderingAvailable()) {
			SLT_LOG_WARN("Editor", "no rendering device (project hub can't be shown)");
			return {};
		}
		ProjectHub hub;
		return hub.RunWindow();
	}

	str ProjectHub::RunWindow() {
		WindowDesc desc;
		desc.title = "Shalltear - Projects";
		desc.clientSize = {940, 560};
		desc.minClientSize = {620, 360};
		desc.flags = WindowFlags::Resizable | WindowFlags::ScaleToMonitor | WindowFlags::Borderless;
		for (const MonitorInfo& monitor : WindowSystem::GetMonitors()) {
			if (!monitor.primary) {
				continue;
			}
			Vec2I size{static_cast<i32>(desc.clientSize.x * monitor.scale),
					   static_cast<i32>(desc.clientSize.y * monitor.scale)};
			desc.position = Vec2I{monitor.workArea.position.x + (monitor.workArea.size.x - size.x) / 2,
								  monitor.workArea.position.y + (monitor.workArea.size.y - size.y) / 2};
			break;
		}
		_window = WindowSystem::Create(desc);
		if (!_window.IsValid()) {
			SLT_LOG_ERROR("Editor", "project hub: window creation failed");
			return {};
		}
		_windowId = _window.GetInstanceId();
		if (Window* hubWindow = _window.Get()) {
			hubWindow->SetHitTestCallback(&EditorChromeOps::ChromeHitTest, nullptr);
		}
		Handle<Window> window = _window;

		EventBus& bus = EventBus::GetGlobal();
		Handle<ProjectHub> self(this);
		WindowEvents::CloseRequested.Subscribe(bus, self, &ProjectHub::OnCloseRequested);
		WindowEvents::Destroyed.Subscribe(bus, self, &ProjectHub::OnWindowDestroyed);
		WindowSystem::SetModalTickCallback(
			[](void* user) { static_cast<ProjectHub*>(user)->RunFrame(); }, this);

		UI::Context* context = UISystem::CreateContext(_window);
		if (context) {
			UI::ElementHandle root = context->CreateElement();
			context->AppendChild(context->GetRoot(), root);
			BuildContent(*context, root);
		}
		_shortcuts.Build();

		while (!_done) {
			RunFrame();
		}

		WindowSystem::SetModalTickCallback(nullptr, nullptr);
		if (_window.IsValid()) {
			WindowSystem::RequestDestroy(_window);
			WindowSystem::PumpEvents();
		}
		UISystem::DestroyContext(window);
		bus.UnsubscribeAll(GetInstanceId());
		return _chosenPath;
	}

	void ProjectHub::RunFrame() {
		if (WindowSystem::GetFocusedWindow().IsValid()) {
			WindowSystem::PumpEvents();
		} else {
			WindowSystem::WaitEvents(0.25);
		}

		_shortcuts.Route(EditorInputOps::ApplicationFacts());
		_shortcuts.Evaluate();
		using Shortcut = EditorInputOps::Shortcut;
		if (_shortcuts.WasPerformed(Shortcut::DeleteSelection)) {
			OnCommand(PanelCommand::Delete);
		}
		if (_shortcuts.WasPerformed(Shortcut::SelectAll)) {
			OnCommand(PanelCommand::SelectAll);
		}
		if (_shortcuts.WasPerformed(Shortcut::DuplicateSelection)) {
			OnCommand(PanelCommand::Duplicate);
		}
		if (_shortcuts.WasPerformed(Shortcut::RenameSelection)) {
			OnCommand(PanelCommand::Rename);
		}

		if (UI::Context* context = UISystem::GetContext(_window)) {
			if (_rowsDirty) {
				_rowsDirty = false;
				RebuildRows(*context);
				RefreshStatusLine(*context);
			}
			if (Window* window = _window.Get(); window && _maximizeButtonFace.IsValid()) {
				bool maximized = window->IsMaximized();
				if (maximized != _showedMaximized) {
					_showedMaximized = maximized;
					EditorChromeOps::SetMaximizeGlyph(*context, _maximizeButtonFace, maximized);
				}
			}
			UISystem::Update();
		}
		RenderSystem::RenderAll();
	}

	void ProjectHub::OnCloseRequested(const WindowCloseRequestedEvent& event) {
		if (event.window == _window) {
			WindowSystem::RequestDestroy(_window);
		}
	}

	void ProjectHub::OnWindowDestroyed(const WindowDestroyedEvent& event) {
		if (event.windowId == _windowId) {
			_window = {};
			_done = true;
		}
	}

} // namespace Shalltear
