#include "ScriptFlows.h"

#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/UI/UISystem.h"
#include "Shalltear/Core/UI/Widgets.h"

#include "Shalltear/Editor/CompileService.h"
#include "Shalltear/Editor/CppToolchain.h"
#include "Shalltear/Editor/EditorTheme.h"
#include "Shalltear/Editor/ProjectOps.h"
#include "Shalltear/Editor/ScriptProjectOps.h"

#include "GameModuleHost.h"
#include "PackageHost.h"

#include <functional>

namespace Shalltear::ScriptFlows {

	namespace {

		Color TextDim() { return EditorTheme::GetColor(EditorTheme::ColorToken::TextDim); }
		Color TextNormal() { return EditorTheme::GetColor(EditorTheme::ColorToken::Text); }

		UI::ElementHandle BeginShellPopupCard(UI::Context& context) {
			UI::ElementHandle card = context.CreateElement();
			UI::Style style;
			style.minWidth = UI::Length::Pixel(420.0f);
			context.SetStyle(card, style);
			context.AddClass(card, EditorTheme::Class::PopupCard);
			UI::Context* contextPtr = &context;
			context.Events(card).onPopupClosed = [contextPtr, card]() {
				contextPtr->DestroyElementNextUpdate(card);
			};
			return card;
		}

		UI::ElementHandle AddShellPopupLine(UI::Context& context, UI::ElementHandle card, std::string_view text,
											f32 fontSizePx, Color color) {
			UI::ElementHandle line = context.CreateElement();
			context.AppendChild(card, line);
			UI::Style style;
			style.margin = {0.0f, 0.0f, 0.0f, 6.0f};
			context.SetStyle(line, style);
			context.SetText(line, UISystem::GetDefaultFont(), fontSizePx, color, text);
			return line;
		}

		UI::ElementHandle AddShellPopupField(UI::Context& context, UI::ElementHandle card,
											 std::string_view label, const str& initialText) {
			AddShellPopupLine(context, card, label, 11.5f, TextDim());
			UI::Widgets::TextFieldDesc desc;
			desc.text = initialText;
			desc.font = UISystem::GetDefaultFont();
			desc.fontSizePx = EditorTheme::GetMetric(EditorTheme::MetricToken::FontRow);
			UI::ElementHandle field = UI::Widgets::TextField(context, card, desc);
			UI::Style style = context.GetStyle(field);
			style.margin = {0.0f, 0.0f, 0.0f, 10.0f};
			context.SetStyle(field, style);
			return field;
		}

		UI::ElementHandle AddShellButtonRow(UI::Context& context, UI::ElementHandle card) {
			UI::ElementHandle row = context.CreateElement();
			context.AppendChild(card, row);
			UI::Style style;
			style.flexDirection = UI::FlexDirection::Row;
			style.justifyContent = UI::Align::End;
			style.margin = {0.0f, 8.0f, 0.0f, 0.0f};
			context.SetStyle(row, style);
			return row;
		}

		// close the popup first (it owns the callback storage)
		void AddShellPopupButton(UI::Context& context, UI::ElementHandle row, UI::ElementHandle card,
								 std::string_view label, std::function<void()> action, bool isDefault = false) {
			UI::Widgets::ButtonDesc desc;
			desc.label = label;
			desc.font = UISystem::GetDefaultFont();
			desc.fontSizePx = EditorTheme::GetMetric(EditorTheme::MetricToken::FontRow);
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

	void OpenToolchainPopup(UI::Context& context) {
		UI::ElementHandle card = BeginShellPopupCard(context);
		AddShellPopupLine(context, card, "Toolchain", 14.0f, TextNormal());

		CompileService* compileService = CompileService::GetActive();
		CppToolchain::ToolchainInfo info = compileService ? compileService->GetToolchain()
														  : CppToolchain::Discover();
		str described = CppToolchain::Describe(info);
		size_t start = 0;
		while (start <= described.size()) {
			size_t end = described.find('\n', start);
			if (end == str::npos) {
				end = described.size();
			}
			AddShellPopupLine(context, card, std::string_view(described).substr(start, end - start), 11.5f, TextDim());
			start = end + 1;
		}

		UI::ElementHandle buttons = AddShellButtonRow(context, card);
		AddShellPopupButton(context, buttons, card, "Close", {}, true);
		context.OpenPopup(card, {}, DIALOG_FLAGS);
	}

	void OpenCreateModulePopup(UI::Context& context) {
		GameModuleHost* moduleHost = GameModuleHost::GetActive();
		str projectPath = ProjectOps::GetMountedProjectPath();

		UI::ElementHandle card = BeginShellPopupCard(context);
		AddShellPopupLine(context, card, "Create C++ Module", 14.0f, TextNormal());
		AddShellPopupLine(context, card,
						  "Writes CMakeLists.txt, the presets, and Source/ into this project.",
						  11.5f, TextDim());
		AddShellPopupLine(context, card, projectPath, 11.0f, TextDim());

		str suggested = ScriptProjectOps::GetModuleName(projectPath);
		UI::ElementHandle nameField = AddShellPopupField(context, card, "Module name", suggested);

		UI::Context* contextPtr = &context;
		UI::ElementHandle buttons = AddShellButtonRow(context, card);
		AddShellPopupButton(context, buttons, card, "Cancel", {});
		AddShellPopupButton(context, buttons, card, "Create", [contextPtr, nameField, projectPath, moduleHost]() {
			ScriptProjectOps::ScaffoldOptions options;
			options.moduleName = contextPtr->GetTextString(nameField);
			options.sdkRoot = ScriptProjectOps::GetSDKRoot();
			options.editorExecutable = ScriptProjectOps::GetEditorExecutablePath();
			if (const PackageHost* packages = PackageHost::GetActive()) {
				options.enabledPackages = packages->GetGamePackageNames();
			}
			ScriptProjectOps::ScaffoldResult result;
			if (!ScriptProjectOps::ScaffoldScriptProject(projectPath, options, result)) {
				SLT_LOG_ERROR("Editor", "couldn't scaffold a C++ module into {}", projectPath);
				return;
			}
			for (const str& written : result.written) {
				SLT_LOG_INFO("Editor", "wrote {}", written);
			}
			if (moduleHost) {
				moduleHost->RearmScriptProject();
				moduleHost->RequestCompile();
			}
			ScriptProjectOps::OpenInExternalEditor(projectPath, moduleHost ? moduleHost->GetExternalEditorCommand() : str{}, {}, 0, 0); }, true);
		context.OpenPopup(card, {}, DIALOG_FLAGS);
		context.SetFocus(nameField);
	}

	void OpenCreateComponentPopup(UI::Context& context) {
		GameModuleHost* moduleHost = GameModuleHost::GetActive();
		str projectPath = ProjectOps::GetMountedProjectPath();

		UI::ElementHandle card = BeginShellPopupCard(context);
		AddShellPopupLine(context, card, "New C++ Component", 14.0f, TextNormal());
		AddShellPopupLine(context, card, ScriptProjectOps::GetSourceDirectory(projectPath), 11.0f, TextDim());

		UI::ElementHandle nameField = AddShellPopupField(context, card, "Class name", "CharacterController");
		UI::ElementHandle folderField = AddShellPopupField(context, card, "Folder (inside Source/, optional)", "");

		UI::Context* contextPtr = &context;
		UI::ElementHandle buttons = AddShellButtonRow(context, card);
		AddShellPopupButton(context, buttons, card, "Cancel", {});
		AddShellPopupButton(context, buttons, card, "Create", [contextPtr, nameField, folderField, projectPath, moduleHost]() {
			str className = contextPtr->GetTextString(nameField);
			str folder = contextPtr->GetTextString(folderField);
			ScriptProjectOps::ScaffoldResult result;
			if (!ScriptProjectOps::CreateComponentSource(projectPath, folder, className, result)) {
				SLT_LOG_ERROR("Editor", "couldn't create '{}' (invalid name, or the files already exist)", className);
				return;
			}
			for (const str& written : result.written) {
				SLT_LOG_INFO("Editor", "wrote {}", written);
			}
			if (!result.written.empty()) {
				ScriptProjectOps::OpenInExternalEditor(projectPath, moduleHost ? moduleHost->GetExternalEditorCommand() : str{},
													   projectPath + "/" + result.written[0], 1, 1);
			}
			if (moduleHost) {
				moduleHost->RequestCompile();
			} }, true);
		context.OpenPopup(card, {}, DIALOG_FLAGS);
		context.SetFocus(nameField);
	}

	void OpenCreateCSharpComponentPopup(UI::Context& context) {
		GameModuleHost* moduleHost = GameModuleHost::GetActive();
		str projectPath = ProjectOps::GetMountedProjectPath();

		UI::ElementHandle card = BeginShellPopupCard(context);
		AddShellPopupLine(context, card, "New C# Component", 14.0f, TextNormal());
		AddShellPopupLine(context, card, ScriptProjectOps::GetSourceDirectory(projectPath), 11.0f, TextDim());

		UI::ElementHandle nameField = AddShellPopupField(context, card, "Class name", "CharacterController");
		UI::ElementHandle folderField = AddShellPopupField(context, card, "Folder (inside Source/, optional)", "");

		UI::Context* contextPtr = &context;
		UI::ElementHandle buttons = AddShellButtonRow(context, card);
		AddShellPopupButton(context, buttons, card, "Cancel", {});
		AddShellPopupButton(context, buttons, card, "Create", [contextPtr, nameField, folderField, projectPath, moduleHost]() {
			str className = contextPtr->GetTextString(nameField);
			str folder = contextPtr->GetTextString(folderField);
			ScriptProjectOps::ScaffoldResult result;
			if (!ScriptProjectOps::CreateCSharpComponentSource(projectPath, folder, className, result)) {
				SLT_LOG_ERROR("Editor", "couldn't create C# component '{}' (invalid name, or the file already exists)", className);
				return;
			}
			for (const str& written : result.written) {
				SLT_LOG_INFO("Editor", "wrote {}", written);
			}
			if (!result.written.empty()) {
				ScriptProjectOps::OpenInExternalEditor(projectPath, moduleHost ? moduleHost->GetExternalEditorCommand() : str{},
													   projectPath + "/" + result.written[0], 1, 1);
			}
			if (moduleHost) {
				moduleHost->RequestCompile();
			} }, true);
		context.OpenPopup(card, {}, DIALOG_FLAGS);
		context.SetFocus(nameField);
	}

} // namespace Shalltear::ScriptFlows
