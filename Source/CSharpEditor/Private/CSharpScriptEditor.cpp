#include "CSharpScriptEditor.h"

#include "CSharpAssemblies.h"
#include "CSharpAuthoring.h"
#include "CSharpScriptAsset.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/UI/UISystem.h"
#include "Shalltear/Core/UI/Widgets.h"

#include "Shalltear/Editor/AssetBrowserOps.h"
#include "Shalltear/Editor/AssetOpeners.h"
#include "Shalltear/Editor/ComponentDrops.h"
#include "Shalltear/Editor/EditorDialogs.h"
#include "Shalltear/Editor/EditorIcons.h"
#include "Shalltear/Editor/EditorMenus.h"
#include "Shalltear/Editor/EditorPreviews.h"
#include "Shalltear/Editor/EditorTheme.h"
#include "Shalltear/Editor/PropertyDrawers.h"

#include <sstream>

namespace Shalltear::CSharpEditor {

	namespace {

		using EditorTheme::ColorToken;
		using EditorTheme::MetricToken;

		constexpr u32 SOURCE_PREVIEW_LINES = 40;

		f32 BodyPx() {
			return EditorTheme::GetMetric(MetricToken::FontBody);
		}

		UI::ElementHandle AddLine(UI::Context& context, UI::ElementHandle parent, ColorToken color, std::string_view text, f32 topPx = 2.0f) {
			UI::ElementHandle element = context.CreateElement();
			context.AppendChild(parent, element);
			context.SetHoverable(element, false);
			UI::Style style;
			style.padding = {8.0f, topPx, 8.0f, 2.0f};
			context.SetStyle(element, style);
			context.SetText(element, UISystem::GetDefaultFont(), BodyPx(), EditorTheme::GetColor(color), text);
			return element;
		}

		void DrawScriptInspector(UI::Context& context, UI::ElementHandle parent, const Guid& guid, Asset&) {
			AddLine(context, parent, ColorToken::TextStrong, "C# Script", 8.0f);

			UI::ElementHandle actions = context.CreateElement();
			context.AppendChild(parent, actions);
			UI::Style actionsStyle;
			actionsStyle.flexDirection = UI::FlexDirection::Row;
			actionsStyle.padding = {8.0f, 4.0f, 8.0f, 4.0f};
			context.SetStyle(actions, actionsStyle);
			UI::Widgets::ButtonDesc open;
			open.label = "Open";
			open.font = UISystem::GetDefaultFont();
			open.fontSizePx = BodyPx();
			open.onClick = [guid]() { CSharpScripts::Open(guid); };
			UI::Widgets::Button(context, actions, open);

			AddLine(context, parent, ColorToken::TextDim, "Components", 8.0f);
			Vector<const TypeInfo*> components{GetMemoryResource(MemTag::Editor)};
			CSharpScripts::GetComponents(guid, components);
			for (const TypeInfo* type : components) {
				AddLine(context, parent, ColorToken::Text, type->displayName);
			}
			if (components.empty()) {
				AddLine(context, parent, ColorToken::TextDim, "None compiled from this file.");
			}

			AddLine(context, parent, ColorToken::TextDim, "Source", 8.0f);
			std::istringstream source(FileSystem::ReadTextFile(AssetDatabase::LookupPath(guid), MemTag::Editor));
			u32 shown = 0;
			str preview;
			for (str line; shown < SOURCE_PREVIEW_LINES && std::getline(source, line); ++shown) {
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				for (char& c : line) {
					c = c == '\t' ? ' ' : c;
				}
				preview += line;
				preview += '\n';
			}
			if (source.good()) {
				preview += "...";
			}
			AddLine(context, parent, ColorToken::Text, preview);
		}

		Guid CreateAndOpen(const Path& folder, const str& name, CSharpScripts::Template kind) {
			const Guid created = CSharpScripts::Create(folder, name, kind);
			if (created.IsValid()) {
				CSharpScripts::Open(created);
			}
			return created;
		}

		void AskNameThenCreate(UI::Context& context) {
			EditorDialogs::OpenTextInput(context, "New C# Script", "NewScript", "Create", [](const str& name) {
				CreateAndOpen(Path(AssetBrowserOps::GetDefaultCreateFolder().ToString() + "/Scripts"), name, CSharpScripts::Template::Component);
			});
		}

	} // anonymous namespace

	void RegisterScriptEditor(const void* owner) {
		const StringHash assetType = CSharpScriptAsset::GetReflectType()->name;

		AssetBrowserOps::RegisterCreateEntry(
			"C# Script", "NewScript", [](const Path& folder, const str& name) { return CreateAndOpen(folder, name, CSharpScripts::Template::Component); }, owner);
		AssetBrowserOps::RegisterCreateEntry(
			"C# Class", "NewClass", [](const Path& folder, const str& name) { return CreateAndOpen(folder, name, CSharpScripts::Template::Class); }, owner);
		AssetBrowserOps::RegisterRenameHook(".cs", &CSharpScripts::HandleRenamed, owner);

		ComponentDrops::Register(assetType, [](const Guid& script) { return CSharpScripts::ResolveComponent(script); }, owner);
		AssetOpeners::Register(".cs", [](const Guid& script) { CSharpScripts::Open(script); }, owner);
		EditorPreviews::AssetBadges::Register(assetType, EditorIcons::Glyph::AssetScript, ColorToken::AssetShader, owner);
		EditorPreviews::AssetBadges::Register(CSharpAssemblyDefinition::GetReflectType()->name, EditorIcons::Glyph::PanelPackages, ColorToken::AssetShader, owner);
		PropertyDrawers::RegisterAssetInspector(assetType, &DrawScriptInspector, owner);

		EditorMenus::MenuItemDesc create;
		create.label = "New C# Script...";
		create.separatorBefore = true;
		create.onSelect = &AskNameThenCreate;
		EditorMenus::Register("Scripts", std::move(create), owner);

		EditorMenus::MenuItemDesc compile;
		compile.label = "Compile C#";
		compile.isEnabled = []() { return CSharpAuthoring::IsActive() && !CSharpAuthoring::GetCompile().IsBusy(); };
		compile.onSelect = [](UI::Context&) { CSharpAuthoring::RequestCompile(); };
		EditorMenus::Register("Scripts", std::move(compile), owner);

		EditorMenus::MenuItemDesc project;
		project.label = "Open C# Project";
		project.onSelect = [](UI::Context&) { AssetBrowserOps::OpenInExternalEditor({}); };
		EditorMenus::Register("Scripts", std::move(project), owner);
	}

} // namespace Shalltear::CSharpEditor
