#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Math/Quat.h"
#include "Shalltear/Core/Math/Vector.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Scene/Prefab.h"
#include "Shalltear/Core/Types.h"
#include "Shalltear/Core/UI/Context.h"
#include "Shalltear/Core/UI/Widgets.h"

#include "Shalltear/Editor/EditorEvents.h"
#include "Shalltear/Editor/EditorInsertStrips.h"
#include "Shalltear/Editor/EditorPanel.h"
#include "Shalltear/Editor/EditTarget.h"
#include "Shalltear/Editor/PropertyRows.h"

#include "PreviewPane.h"

namespace Shalltear {

	class Asset;
	class Scene;
	class SceneObject;
	class Component;
	struct FieldInfo;

	class InspectorPanel : public EditorPanel {
	public:
		SLT_REFLECT_CLASS(InspectorPanel)
		SLT_REFLECT_PARENT(EditorPanel)
		SLT_REFLECT_CREATABLE()
		SLT_EDITOR_PANEL_NAME("Inspector")
		SLT_REFLECT_FIELD(locked)
		SLT_REFLECT_FIELD(lockedTargetId)
		SLT_REFLECT_FIELD(lockedAsset)
		SLT_REFLECT_FIELD(previewExpanded)
		SLT_END_REFLECT()

		EditorIcons::Icon GetTabIcon() const override;

		InspectorPanel() = default;

		void OnBuildContent(UI::Context& context, UI::ElementHandle root) override;
		void OnTick(f32 deltaSeconds) override;
		void OnSelectionChanged() override;
		void OnOpen() override;
		void OnClose() override;
		void OnWorldRemapped(const EditorWorldRemappedEvent& event);
		void OnModuleReloadBegan(const EditorModuleReloadBeganEvent& event);
		void OnPlayModeChanged(const EditorPlayModeChangedEvent& event);
		void OnModuleReloadEnded(const EditorModuleReloadEndedEvent& event);
		void OnCompileStateChanged(const EditorCompileStateChangedEvent& event);

		bool locked = false;
		u64 lockedTargetId = 0;
		Guid lockedAsset;
		bool previewExpanded = true;

		void BuildHandleRow(UI::Context& context, UI::ElementHandle row, EditTarget target,
							StringHash fieldName, StringHash pointeeName, const str& label,
							i32 elementIndex = -1, std::function<void(u64)> onAssigned = {});

		void BuildNumberElementRow(UI::Context& context, UI::ElementHandle row, EditTarget target,
								   StringHash fieldName, u32 elementIndex, const str& label);

		UI::ElementHandle MakeValueRow(UI::Context& context, UI::ElementHandle parent, std::string_view label,
									   EditTarget target, StringHash fieldName, i32 elementIndex = -1);

		void BuildPrefabAssetBody(UI::Context& context, Handle<SceneObject> object, const Guid& assetGuid);

		static InspectorPanel* GetBuildingPanel();

		void RequestRebuild() { _forceRebuild = true; }

		bool IsSectionCollapsed(u64 key) const { return _collapsedSections.contains(key); }
		void ToggleSection(u64 key) {
			if (!_collapsedSections.insert(key).second) {
				_collapsedSections.erase(key);
			}
			_forceRebuild = true;
		}

		void ReleaseDrawnAssetAndRebuild() {
			ReleaseDrawnAsset();
			_forceRebuild = true;
		}

		void BuildFieldRow(UI::Context& context, UI::ElementHandle parent, const FieldInfo& field, EditTarget target);

		void BuildHandleListRows(UI::Context& context, UI::ElementHandle parent, const FieldInfo& field, EditTarget target);

		struct NestedStep {
			i32 element = -1;
			FieldRef field{};
		};
		struct NestedPath {
			EditTarget target;
			FieldRef top{};
			Vector<NestedStep> steps{GetMemoryResource(MemTag::Editor)};
		};

		struct HandleSlot {
			EditTarget target;
			StringHash field{};
			i32 element = -1;
			SharedPtr<NestedPath> nested;
		};

		struct RowOverride {
			bool overridden = false;
			std::function<void()> onRevert;
		};
	private:
		struct Binding {
			enum class Kind : u8 {
				Scalar,
				RawNumber,
				EnabledBox,
				ActiveBox,
				TextField,
				VectorField,
				EulerField,
				ColorField,
				HandleLabel,
				NumberElement,
			};
			Kind kind = Kind::Scalar;
			EditTarget target;

			FieldRef field{};
			PropertyRows::ScalarRow scalar;
			UI::Widgets::NumberFieldParts number;
			UI::Widgets::CheckboxParts check;
			UI::Widgets::ToggleParts toggle;
			UI::Widgets::VectorFieldParts vector;
			UI::Widgets::ColorFieldParts color;
			UI::ElementHandle element;
			u32 componentCount = 3;
			i32 elementIndex = -1;
			SharedPtr<NestedPath> nested;
		};

		Handle<Object> ResolveTarget() const;
		Guid ShownAsset() const;
		Handle<SceneObject> ResolveDropTarget() const;
		void BuildComponentInsertStrip(UI::Context& context, Handle<SceneObject> owner, u32 index);
		void FillHeader(UI::Context& context);
		u64 ComputeVersion() const;
		void RebuildAll(UI::Context& context);
		void BuildObjectInspector(UI::Context& context, Handle<SceneObject> object);
		void BuildMissingComponentBanner(UI::Context& context, Handle<Scene> scene);
		void BuildComponentSection(UI::Context& context, UI::ElementHandle parent, Handle<Component> component);
		void BuildRemovedComponentSection(UI::Context& context, UI::ElementHandle parent,
										  const PrefabInstanceDiff::RemovedComponent& removed);
		void RefreshOverrides(Handle<SceneObject> object);
		RowOverride OverrideMarkFor(EditTarget target, StringHash field) const;
		bool IsAddedComponent(u64 componentId) const;
		bool HasPropertyOverrides(u64 componentId) const;
		void RevertComponentOverrides(EditTarget component, u64 componentId);
		static u64 MarkSetHash(const PrefabInstanceDiff& diff);
		void BuildAssetInspector(UI::Context& context, const Guid& guid);
		void BuildProductInspector(UI::Context& context, const Guid& guid, const Guid& sourceGuid);
		void BuildAssetFallbackBody(UI::Context& context, Asset& asset);
		void BuildAssetSections(UI::Context& context, const Guid& guid, Asset& asset, StringHash assetType);
		void OpenAddComponentPopup(UI::Context& context, Handle<SceneObject> object);
		void BuildHandleSlotRow(UI::Context& context, UI::ElementHandle row, HandleSlot slot,
								StringHash pointeeName, const str& label, std::function<void(u64)> onAssigned = {});
		void BuildNestedStructRows(UI::Context& context, UI::ElementHandle parent, const NestedPath& path,
								   const TypeInfo* structType, u32 depth);
		void BuildNestedFieldRow(UI::Context& context, UI::ElementHandle parent, const NestedPath& path,
								 const FieldInfo& field, u32 depth);
		void BuildNestedList(UI::Context& context, UI::ElementHandle header, UI::ElementHandle parent,
							 const NestedPath& path, const ContainerInfo& container, u32 depth);
		void BuildNestedElementRow(UI::Context& context, UI::ElementHandle row, SharedPtr<NestedPath> path,
								   FieldRef list, const TypeInfo* type, const str& label);
		bool BuildNestedMathRow(UI::Context& context, UI::ElementHandle row, SharedPtr<NestedPath> path,
								const TypeInfo* type, const str& label);
		u64 NestedFoldKey(const NestedPath& path) const;

		void OpenHandlePickerPopup(UI::Context& context, HandleSlot slot, StringHash pointeeName, str label,
								   std::function<void(u64)> onAssigned = {});
		void RefreshValues(UI::Context& context);

		EditTarget TargetFor(Object* object) const;

		Vector<Binding> _bindings{GetMemoryResource(MemTag::Editor)};

		Vector<UI::ElementHandle> _customRows{GetMemoryResource(MemTag::Editor)};
		HashMap<u64, Vec3> _eulerCache{GetMemoryResource(MemTag::Editor)};
		HashSet<u64> _collapsedSections{GetMemoryResource(MemTag::Editor)};
		EditorInsertStrips _gaps;
		UI::ElementHandle _header;
		UI::ElementHandle _viewport;
		UI::ElementHandle _scrollRoot;
		UI::ElementHandle _emptyState;
		PreviewPane _preview;
		u64 _lastVersion = 0;
		bool _forceRebuild = false;
		Guid _assetEditTarget{};
		Handle<SceneObject> _drawnPreviewRoot;
		Handle<Asset> _drawnAsset;
		void ReleaseDrawnAsset();

		PrefabInstanceDiff _overrides;
		Handle<SceneObject> _overridesRoot;
		Handle<SceneObject> _drawnObject;
		u64 _drawnMarks = 0;
		u64 _undoStamp = 0;

		str _assetTab;
	};

	UI::ElementHandle AddInspectorHeader(UI::Context& context, UI::ElementHandle parent, std::string_view text, Color color,
										 std::string_view markPath = {});

	void RegisterBuiltInComponentInspectors();
	void RegisterBuiltInAssetInspectors();
	void RegisterPrefabAssetInspector();
	void RegisterTextureAssetInspector();
	void TerminateTextureAssetInspector();
	void RegisterFontAssetInspector();
	void TerminateFontAssetInspector();

} // namespace Shalltear
