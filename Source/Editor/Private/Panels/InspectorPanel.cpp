#include "InspectorPanel.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Asset/AssetImporter.h"
#include "Shalltear/Core/Graphics/MeshRenderer.h"
#include "Shalltear/Core/Graphics/ReflectionProbe.h"
#include "Shalltear/Core/Graphics/SkinnedMeshRenderer.h"
#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/Scene/UnresolvedComponent.h"
#include "Shalltear/Core/Serialization/TextSerializer.h"
#include "Shalltear/Core/UI/UISystem.h"

#include "Shalltear/Editor/EditorIcons.h"
#include "Shalltear/Core/ProjectLayout.h"
#include "Shalltear/Editor/AssetBrowserOps.h"
#include "Shalltear/Editor/CompileService.h"
#include "Shalltear/Editor/ComponentMenu.h"
#include "Shalltear/Editor/EditorAssetOps.h"
#include "Shalltear/Editor/EditorCommands.h"
#include "Shalltear/Editor/EditorTheme.h"
#include "Shalltear/Editor/EditorDragData.h"
#include "Shalltear/Editor/EditorEvents.h"
#include "Shalltear/Editor/EditorSceneOps.h"
#include "Shalltear/Editor/EditorSelection.h"
#include "Shalltear/Editor/EditorTransformGesture.h"
#include "Shalltear/Editor/PanelManager.h"
#include "Shalltear/Editor/ObjectField.h"
#include "Shalltear/Editor/ProjectOps.h"
#include "Shalltear/Editor/PropertyDrawers.h"
#include "Shalltear/Editor/ReflectionProbeBake.h"
#include "Shalltear/Editor/ScriptProjectOps.h"
#include "Shalltear/Editor/TypeNameOps.h"
#include "Shalltear/Editor/UndoSystem.h"

#include "GameModuleHost.h"
#include "PanelEmptyState.h"
#include "PanelHeader.h"
#include "SceneFileFlows.h"
#include "ScriptFlows.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace Shalltear {

	namespace {

		using EditorTheme::ColorToken;
		using EditorTheme::MetricToken;

		Color TextNormal() { return EditorTheme::GetColor(ColorToken::Text); }
		Color TextDim() { return EditorTheme::GetColor(ColorToken::TextDim); }
		Color TextStrong() { return EditorTheme::GetColor(ColorToken::TextStrong); }
		Color OverrideColor() { return EditorTheme::GetColor(ColorToken::OverrideMark); }
		f32 FieldFont() { return EditorTheme::GetMetric(MetricToken::FontBody); }
		f32 RowFont() { return EditorTheme::GetMetric(MetricToken::FontRow); }

		constexpr u32 ADD_COMPONENT_ROW_CAP = 24;

		UI::ElementHandle AppendCaretGutter(UI::Context& context, UI::ElementHandle header) {
			UI::ElementHandle gutter = context.CreateElement();
			context.AppendChild(header, gutter);
			context.SetHoverable(gutter, false);
			UI::Style style;
			style.flexShrink = 0.0f;
			style.width = UI::Length::Pixel(EditorTheme::GetMetric(MetricToken::IconGutter));
			style.margin = {0.0f, 0.0f, EditorTheme::GetMetric(MetricToken::IconGap), 0.0f};
			context.SetStyle(gutter, style);
			return gutter;
		}

		InspectorPanel* _buildingPanel = nullptr;

		struct DrawnAssetRelease {
			Handle<Asset> previous;
			~DrawnAssetRelease() {
				if (Asset* asset = previous.Get()) {
					AssetDatabase::UnloadAsset(asset);
				}
			}
		};

		FieldRef RefTo(const FieldInfo& field) {
			return FieldRef{field.ownerType ? field.ownerType->name : StringHash{}, field.name};
		}

		FieldRef RefOn(const EditTarget& target, StringHash fieldName) {
			Object* object = target.Resolve();
			const TypeInfo* type = object ? object->GetType() : nullptr;
			return FieldRef{type ? type->name : StringHash{}, fieldName};
		}

		using PropertyRows::FieldLabel;
		using PropertyRows::IsIntegral;
		using PropertyRows::IsNumeric;
		using PropertyRows::MakeDisabled;
		using PropertyRows::NameIs;

		str TypeLabel(const TypeInfo* type) {
			if (type && type->displayName) {
				return type->displayName;
			}
			return type ? std::format("{:#x}", type->name.GetValue()) : str("?");
		}

		bool IsAssetIdentityField(const FieldInfo* field) {
			const TypeInfo* owner = field ? field->ownerType : nullptr;
			for (const TypeInfo* walk = Asset::GetReflectType(); walk; walk = walk->parent) {
				if (walk == owner) {
					return true;
				}
			}
			return false;
		}

		bool HasDrawableFields(const TypeInfo* type) {
			for (const TypeInfo* walk = type; walk; walk = walk->parent) {
				for (u32 i = 0; i < walk->fieldCount; ++i) {
					const FieldInfo& field = walk->fields[i];
					if ((field.flags & FieldFlagEditorVisible) != 0 && !IsAssetIdentityField(&field)) {
						return true;
					}
				}
			}
			return false;
		}

		bool IsAssetPointee(const TypeInfo* pointee) {
			const TypeInfo* assetBase = TypeRegistry::FindType(StringHash("Asset"));
			return pointee && assetBase && TypeRegistry::IsDerivedFrom(pointee, assetBase);
		}

		bool AssetPayloadFits(const AssetDragData& data, const TypeInfo* pointee) {
			if (!pointee) {
				return false;
			}
			StringHash declared{data.assetTypeHash};
			if (declared == pointee->name) {
				return true;
			}
			const TypeInfo* declaredType = TypeRegistry::FindType(declared);
			return declaredType && TypeRegistry::IsDerivedFrom(declaredType, pointee);
		}

		Object* DraggedObject(const UI::DragPayload& payload) {
			if (payload.type == "Component"_hash) {
				return payload.data.Is<u64>() ? ObjectRegistry::Resolve(payload.data.As<u64>()) : nullptr;
			}
			if (payload.type != "SceneObject"_hash || !payload.data.Is<SceneObjectDragData>()) {
				return nullptr;
			}
			const Vector<u64>& objects = payload.data.As<SceneObjectDragData>().objects;
			return objects.empty() ? nullptr : ObjectRegistry::Resolve(objects.front());
		}

		Variant NumericVariant(const TypeInfo* fieldType, f64 value) {
			if (NameIs(fieldType, "f32")) {
				return Variant(static_cast<f32>(value));
			}
			if (NameIs(fieldType, "f64")) {
				return Variant(value);
			}
			if (NameIs(fieldType, "i8")) {
				return Variant(static_cast<i8>(value));
			}
			if (NameIs(fieldType, "i16")) {
				return Variant(static_cast<i16>(value));
			}
			if (NameIs(fieldType, "i32")) {
				return Variant(static_cast<i32>(value));
			}
			if (NameIs(fieldType, "i64")) {
				return Variant(static_cast<i64>(value));
			}
			if (NameIs(fieldType, "u8")) {
				return Variant(static_cast<u8>(value));
			}
			if (NameIs(fieldType, "u16")) {
				return Variant(static_cast<u16>(value));
			}
			if (NameIs(fieldType, "u32")) {
				return Variant(static_cast<u32>(value < 0 ? 0 : value));
			}
			return Variant(static_cast<u64>(value < 0 ? 0 : value));
		}

		u64 ReadRawBits(const EditTarget& target, const FieldInfo& field) {
			Object* object = target.Resolve();
			if (!object || !field.addrFn || field.size > 8) {
				return 0;
			}

			if (!TypeRegistry::IsDerivedFrom(object->GetType(), field.ownerType)) {
				return 0;
			}
			u64 bits = 0;
			std::memcpy(&bits, field.addrFn(object), field.size);
			return bits;
		}

		u64 ReadHandleBits(const EditTarget& target, const FieldInfo& field, i32 element) {
			if (element < 0) {
				return ReadRawBits(target, field);
			}
			Object* object = target.Resolve();
			if (!object || !field.addrFn || !(field.flags & FieldFlagContainer) || !field.container) {
				return 0;
			}
			if (!TypeRegistry::IsDerivedFrom(object->GetType(), field.ownerType)) {
				return 0;
			}
			const ContainerInfo* container = field.container;
			if (!container->sizeFn || !container->atFn || container->elementSize > 8) {
				return 0;
			}
			void* containerPtr = field.addrFn(object);
			if (static_cast<size_t>(element) >= container->sizeFn(containerPtr)) {
				return 0;
			}
			u64 bits = 0;
			std::memcpy(&bits, container->atFn(containerPtr, static_cast<size_t>(element)), container->elementSize);
			return bits;
		}

		f32 ReadElementFloat(const EditTarget& target, const FieldInfo& field, i32 element) {
			if (element < 0 || !field.container || field.container->elementSize != sizeof(f32)) {
				return 0.0f;
			}
			u64 bits = ReadHandleBits(target, field, element);
			f32 value = 0.0f;
			std::memcpy(&value, &bits, sizeof(f32));
			return value;
		}

		u32 ContainerCount(const EditTarget& target, const FieldInfo& field) {
			Object* object = target.Resolve();
			if (!object || !field.addrFn || !field.container || !field.container->sizeFn) {
				return 0;
			}
			return static_cast<u32>(field.container->sizeFn(field.addrFn(object)));
		}

		void PushHandleWrite(const EditTarget& target, const FieldInfo& field, StringHash fieldName,
							 i32 element, u64 before, u64 after, const str& label) {
			if (before == after) {
				return;
			}
			if (element < 0) {
				UndoSystem::Push(MakeUnique<SetRawFieldCommand>(target, fieldName, before, after, 8, label));
				return;
			}
			UndoSystem::Push(MakeUnique<SetContainerElementCommand>(
				target, fieldName, static_cast<u32>(element), before, after,
				ContainerCount(target, field), label));
		}

		const PropertyDrawerFn* CustomDrawerFor(const FieldInfo& field) {
			const StringHash declaringName = field.ownerType ? field.ownerType->name : StringHash{};
			const PropertyDrawerFn* custom = PropertyDrawers::FindFieldDrawerFor(declaringName, field.name);
			return custom ? custom : PropertyDrawers::FindFieldDrawer(field.fieldType ? field.fieldType->name : StringHash{});
		}

		constexpr u32 MAX_NESTED_DEPTH = 8;

		bool TypeIsOrDerives(const TypeInfo* type, const TypeInfo* owner) {
			if (!owner) {
				return false;
			}
			for (const TypeInfo* walk = type; walk; walk = walk->parent) {
				if (walk->name == owner->name) {
					return true;
				}
			}
			return false;
		}

		void* ResolveNested(InspectorPanel::NestedPath& path, const TypeInfo** outType = nullptr,
							const ContainerInfo** outContainer = nullptr) {
			Object* object = path.target.Resolve();
			const FieldInfo* top = path.top.Resolve();
			if (!object || !top || !top->addrFn || !TypeRegistry::IsDerivedFrom(object->GetType(), top->ownerType)) {
				return nullptr;
			}
			void* at = top->addrFn(object);
			const TypeInfo* type = top->fieldType;
			const ContainerInfo* container = (top->flags & FieldFlagContainer) != 0 ? top->container : nullptr;
			for (InspectorPanel::NestedStep& step : path.steps) {
				if (step.element >= 0) {
					if (!container || !container->sizeFn || !container->atFn ||
						static_cast<size_t>(step.element) >= container->sizeFn(at)) {
						return nullptr;
					}
					at = container->atFn(at, static_cast<size_t>(step.element));
					type = container->elementType;
					container = nullptr;
					continue;
				}
				const FieldInfo* field = step.field.Resolve();
				if (!field || !field->addrFn || container || !TypeIsOrDerives(type, field->ownerType)) {
					return nullptr;
				}
				at = field->addrFn(at);
				type = field->fieldType;
				container = (field->flags & FieldFlagContainer) != 0 ? field->container : nullptr;
			}
			if (outType) {
				*outType = type;
			}
			if (outContainer) {
				*outContainer = container;
			}
			return at;
		}

		template <typename Mutate>
		void CommitNested(InspectorPanel::NestedPath& path, std::string_view label, Mutate&& mutate) {
			Object* object = path.target.Resolve();
			const FieldInfo* top = path.top.Resolve();
			void* at = ResolveNested(path);
			if (!object || !top || !at) {
				return;
			}
			str before = CaptureFieldState(*object, top->name);
			if (before.empty() || !mutate(at)) {
				return;
			}
			str after = CaptureFieldState(*object, top->name);
			if (after == before) {
				return;
			}
			UndoSystem::Push(MakeUnique<SetFieldStateCommand>(path.target, top->name, std::move(before), std::move(after), label));
		}

		bool WriteCommitted(const FieldInfo& field, void* at, const PropertyRows::CommittedValue& value) {
			switch (value.kind) {
			case PropertyRows::CommittedValue::Kind::Bool: {
				bool& current = *static_cast<bool*>(at);
				if (current == value.boolean) {
					return false;
				}
				current = value.boolean;
				return true;
			}
			case PropertyRows::CommittedValue::Kind::Number: {
				f64 before = 0.0;
				if (!PropertyRows::ReadNumber(field.fieldType, at, before) || before == value.number) {
					return false;
				}
				return PropertyRows::WriteNumber(field.fieldType, at, value.number);
			}
			case PropertyRows::CommittedValue::Kind::Text: {
				str& current = *static_cast<str*>(at);
				if (current == value.text) {
					return false;
				}
				current = str(value.text);
				return true;
			}
			case PropertyRows::CommittedValue::Kind::RawBits: {
				if (field.size > 8 || std::memcmp(at, &value.rawBits, field.size) == 0) {
					return false;
				}
				std::memcpy(at, &value.rawBits, field.size);
				return true;
			}
			}
			return false;
		}

		bool RemoveElement(const ContainerInfo& container, void* sequence, size_t index) {
			if (!container.sizeFn || !container.resizeFn || !container.atFn) {
				return false;
			}
			const size_t count = container.sizeFn(sequence);
			const bool structs = (container.elementFlags & FieldFlagSubobject) != 0 && container.elementType;
			if (index >= count || (!structs && container.elementSize > 8)) {
				return false;
			}
			for (size_t i = index; i + 1 < count; ++i) {
				void* source = container.atFn(sequence, i + 1);
				void* dest = container.atFn(sequence, i);
				if (structs) {
					ScopedLiveHandles live;
					TextSerializer::Deserialize(dest, container.elementType, TextSerializer::Serialize(source, container.elementType));
				} else {
					std::memcpy(dest, source, container.elementSize);
				}
			}
			container.resizeFn(sequence, count - 1);
			return true;
		}

		u64 ReadSlot(const InspectorPanel::HandleSlot& slot) {
			if (slot.nested) {
				u64 bits = 0;
				if (void* at = ResolveNested(*slot.nested)) {
					std::memcpy(&bits, at, sizeof(u64));
				}
				return bits;
			}
			Object* live = slot.target.Resolve();
			const FieldInfo* field = live ? FindField(live->GetType(), slot.field) : nullptr;
			return field ? ReadHandleBits(slot.target, *field, slot.element) : 0;
		}

		void WriteSlot(const InspectorPanel::HandleSlot& slot, u64 before, u64 after, const str& label) {
			if (before == after) {
				return;
			}
			if (slot.nested) {
				CommitNested(*slot.nested, label, [after](void* at) {
					std::memcpy(at, &after, sizeof(u64));
					return true;
				});
				return;
			}
			Object* live = slot.target.Resolve();
			const FieldInfo* field = live ? FindField(live->GetType(), slot.field) : nullptr;
			if (field) {
				PushHandleWrite(slot.target, *field, slot.field, slot.element, before, after, label);
			}
		}

		void* ResolveFieldAddress(const EditTarget& target, FieldRef& ref, const FieldInfo** outField) {
			Object* object = target.Resolve();
			const FieldInfo* field = ref.Resolve();
			if (outField) {
				*outField = field;
			}
			if (!object || !field || !field->addrFn) {
				return nullptr;
			}
			if (!TypeRegistry::IsDerivedFrom(object->GetType(), field->ownerType)) {
				return nullptr;
			}
			return field->addrFn(object);
		}

		PropertyRows::ValueAccess MakeObjectAccess(EditTarget target, FieldRef ref) {
			PropertyRows::ValueAccess access;
			access.address = [target, ref]() mutable -> void* {
				return ResolveFieldAddress(target, ref, nullptr);
			};
			access.beginGesture = []() { UndoSystem::BeginGroup("Edit Property"); };
			access.endGesture = []() { UndoSystem::EndGroup(); };
			access.commit = [target, ref](const PropertyRows::CommittedValue& value) mutable {
				const FieldInfo* field = nullptr;
				void* address = ResolveFieldAddress(target, ref, &field);
				if (!address || !field) {
					return;
				}
				switch (value.kind) {
				case PropertyRows::CommittedValue::Kind::Bool: {
					const bool before = *static_cast<const bool*>(address);
					if (before != value.boolean) {
						UndoSystem::Push(MakeUnique<PropertySetCommand>(
							target, ref.field, Variant(before), Variant(value.boolean)));
					}
					return;
				}
				case PropertyRows::CommittedValue::Kind::Number: {
					f64 before = 0.0;
					if (PropertyRows::ReadNumber(field->fieldType, address, before) && before != value.number) {
						UndoSystem::Push(MakeUnique<PropertySetCommand>(
							target, ref.field,
							NumericVariant(field->fieldType, before),
							NumericVariant(field->fieldType, value.number)));
					}
					return;
				}
				case PropertyRows::CommittedValue::Kind::Text: {
					const str& before = *static_cast<const str*>(address);
					if (before != value.text) {
						UndoSystem::Push(MakeUnique<PropertySetCommand>(
							target, ref.field, Variant(before), Variant(str(value.text))));
					}
					return;
				}
				case PropertyRows::CommittedValue::Kind::RawBits: {
					if (field->size > 8) {
						return;
					}
					u64 before = 0;
					std::memcpy(&before, address, field->size);
					if (before != value.rawBits) {
						UndoSystem::Push(MakeUnique<SetRawFieldCommand>(
							target, ref.field, before, value.rawBits, static_cast<u32>(field->size)));
					}
					return;
				}
				}
			};
			return access;
		}

		struct RowValue {
			EditTarget target;
			StringHash field{};
			Vector<EditorSceneOps::ValueStep> steps{GetMemoryResource(MemTag::Editor)};
		};

		RowValue ValueAt(const InspectorPanel::NestedPath& path) {
			RowValue value;
			value.target = path.target;
			value.field = path.top.field;
			for (const InspectorPanel::NestedStep& step : path.steps) {
				EditorSceneOps::ValueStep spelled;
				spelled.element = step.element;
				spelled.field = step.element >= 0 ? StringHash{} : step.field.field;
				value.steps.push_back(spelled);
			}
			return value;
		}

		void OpenRowMenu(UI::Context& context, const std::function<void()>& revert, const RowValue* value,
						 const str& undoLabel) {
			Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
			if (revert) {
				UI::Widgets::MenuItem item;
				item.label = "Revert";
				item.onSelect = revert;
				items.push_back(item);
			}
			if (value) {
				UI::Widgets::MenuItem item;
				item.label = "Reset";
				item.enabled = EditorSceneOps::DiffersFromDefault(value->target, value->field, value->steps);
				item.onSelect = [reset = *value, undoLabel]() {
					EditorSceneOps::ResetToDefault(reset.target, reset.field, reset.steps, undoLabel);
				};
				items.push_back(item);
			}
			if (!items.empty()) {
				UI::Widgets::OpenMenu(context, context.GetPointerPosition(), {items.data(), items.size()},
									  UISystem::GetDefaultFont(), FieldFont());
			}
		}

		struct RowFold {
			bool collapsed = false;
			std::function<void()> onToggle;
		};

		UI::ElementHandle MakeRow(UI::Context& context, UI::ElementHandle parent, std::string_view label,
								  const InspectorPanel::RowOverride* override = nullptr, const RowFold* fold = nullptr,
								  const RowValue* value = nullptr) {
			UI::ElementHandle row = context.CreateElement();
			context.AppendChild(parent, row);
			UI::Style rowStyle;
			rowStyle.flexDirection = UI::FlexDirection::Row;
			rowStyle.alignItems = UI::Align::Center;
			rowStyle.padding = {8.0f, 1.5f, 8.0f, 1.5f};
			rowStyle.width = UI::Length::Percent(1.0f);
			context.SetStyle(row, rowStyle);

			bool overridden = override && override->overridden;
			if (overridden) {
				UI::ElementHandle mark = context.CreateElement();
				context.AppendChild(row, mark);
				context.AddClass(mark, EditorTheme::Class::OverrideMark);
				context.SetHoverable(mark, false);
				UI::Style markStyle;
				markStyle.position = UI::Position::Absolute;
				markStyle.left = UI::Length::Pixel(2.0f);
				markStyle.top = UI::Length::Pixel(2.0f);
				markStyle.bottom = UI::Length::Pixel(2.0f);
				markStyle.width = UI::Length::Pixel(2.0f);
				context.SetStyle(mark, markStyle);
			}
			std::function<void()> revert = overridden ? override->onRevert : std::function<void()>{};
			if (revert || value) {
				context.Events(row).onRightClick = [contextPtr = &context, revert, reset = value ? *value : RowValue{},
													hasValue = value != nullptr, undoLabel = "Reset " + str(label)]() {
					OpenRowMenu(*contextPtr, revert, hasValue ? &reset : nullptr, undoLabel);
				};
			}

			f32 labelWidth = 110.0f;
			if (fold) {
				const f32 gutter = EditorTheme::GetMetric(MetricToken::IconGutter);
				const f32 gap = EditorTheme::GetMetric(MetricToken::IconGap);
				UI::ElementHandle caret = context.CreateElement();
				context.AppendChild(row, caret);
				EditorIcons::SetGlyphOrText(context, caret,
											fold->collapsed ? EditorIcons::Glyph::CaretRight : EditorIcons::Glyph::CaretDown,
											fold->collapsed ? ">" : "v", TextDim(), gutter, FieldFont());
				UI::Style caretStyle = context.GetStyle(caret);
				caretStyle.flexShrink = 0.0f;
				caretStyle.width = UI::Length::Pixel(gutter);
				caretStyle.margin = {0.0f, 0.0f, gap, 0.0f};
				caretStyle.textAlign = UI::Align::Center;
				caretStyle.cursor = SystemCursor::Hand;
				context.SetStyle(caret, caretStyle);
				context.Events(caret).onClick = fold->onToggle;
				labelWidth -= gutter + gap;
			}

			UI::ElementHandle labelElement = context.CreateElement();
			context.AppendChild(row, labelElement);
			context.AddClass(labelElement, EditorTheme::Class::FieldLabel);
			UI::Style labelStyle;
			labelStyle.width = UI::Length::Pixel(labelWidth);
			context.SetStyle(labelElement, labelStyle);
			context.SetHoverable(labelElement, false);
			context.SetText(labelElement, UISystem::GetDefaultFont(), FieldFont(), overridden ? TextStrong() : TextDim(), label);
			return row;
		}

		UI::ElementHandle MakeNestedBody(UI::Context& context, UI::ElementHandle parent) {
			UI::ElementHandle body = context.CreateElement();
			context.AppendChild(parent, body);
			UI::Style style;
			style.flexDirection = UI::FlexDirection::Column;
			style.width = UI::Length::Percent(1.0f);
			style.padding.left = EditorTheme::GetMetric(MetricToken::IconGutter) + EditorTheme::GetMetric(MetricToken::IconGap);
			context.SetStyle(body, style);
			return body;
		}

		void RevertProperty(EditTarget target, StringHash field, Variant base, u64 baseHandleId, bool isHandle) {
			Object* live = target.Resolve();
			if (!live) {
				return;
			}
			if (isHandle) {
				const FieldInfo* info = RefOn(target, field).Resolve();
				if (!info) {
					return;
				}
				u64 before = ReadRawBits(target, *info);
				if (before != baseHandleId) {
					UndoSystem::Push(MakeUnique<SetRawFieldCommand>(target, field, before, baseHandleId, sizeof(u64), "Revert"));
				}
				return;
			}
			Variant current;
			if (!GetField(live, live->GetType(), field, &current)) {
				return;
			}
			UndoSystem::Push(MakeUnique<PropertySetCommand>(target, field, std::move(current), std::move(base), "Revert"));
		}

		u64 UndoStamp() {
			return (static_cast<u64>(UndoSystem::GetUndoDepth()) << 32) ^ static_cast<u64>(UndoSystem::GetRedoDepth());
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

	} // anonymous namespace

	UI::ElementHandle AddInspectorHeader(UI::Context& context, UI::ElementHandle parent, std::string_view text, Color color,
										 std::string_view markPath) {
		UI::ElementHandle header = context.CreateElement();
		context.AppendChild(parent, header);
		UI::Style style;
		style.flexDirection = UI::FlexDirection::Row;
		style.alignItems = UI::Align::Center;
		style.padding = {8.0f, 5.0f, 8.0f, 3.0f};
		context.SetStyle(header, style);
		if (!markPath.empty()) {
			EditorIcons::AddVector(context, header, markPath, UI::IconPalette{},
								   EditorIcons::ComponentIcons::SizePx(),
								   EditorTheme::GetMetric(MetricToken::IconGap), RowFont());
		}
		UI::ElementHandle label = context.CreateElement();
		context.AppendChild(header, label);
		context.SetHoverable(label, false);
		UI::Style labelStyle;
		labelStyle.flexGrow = 1.0f;
		context.SetStyle(label, labelStyle);
		context.SetText(label, UISystem::GetDefaultFont(), RowFont(), color, text);
		return header;
	}

	Handle<SceneObject> InspectorPanel::ResolveDropTarget() const {
		Object* target = ResolveTarget().Get();
		if (!target) {
			return {};
		}
		if (TypeRegistry::IsDerivedFrom(target->GetType(), SceneObject::GetReflectType())) {
			return Handle<SceneObject>(static_cast<SceneObject*>(target));
		}
		if (TypeRegistry::IsDerivedFrom(target->GetType(), Component::GetReflectType())) {
			return static_cast<Component*>(target)->GetOwner();
		}
		return {};
	}

	void InspectorPanel::OnOpen() {
		EditorEvents::WorldRemapped.Subscribe(EventBus::GetGlobal(), Handle<InspectorPanel>(this),
											  &InspectorPanel::OnWorldRemapped);
		EditorEvents::ModuleReloadBegan.Subscribe(EventBus::GetGlobal(), Handle<InspectorPanel>(this),
												  &InspectorPanel::OnModuleReloadBegan);
		EditorEvents::PlayModeChanged.Subscribe(EventBus::GetGlobal(), Handle<InspectorPanel>(this),
												&InspectorPanel::OnPlayModeChanged);
		EditorEvents::ModuleReloadEnded.Subscribe(EventBus::GetGlobal(), Handle<InspectorPanel>(this),
												  &InspectorPanel::OnModuleReloadEnded);
		EditorEvents::CompileStateChanged.Subscribe(EventBus::GetGlobal(), Handle<InspectorPanel>(this),
													&InspectorPanel::OnCompileStateChanged);
	}

	void InspectorPanel::OnModuleReloadBegan(const EditorModuleReloadBeganEvent&) {
		if (UI::Context* context = GetPanelContext()) {
			_preview.ReleaseForReload(*context);
		} else {
			_preview.ReleaseLive();
		}
	}

	void InspectorPanel::OnPlayModeChanged(const EditorPlayModeChangedEvent&) {
		_preview.ReleaseLive();
	}

	void InspectorPanel::OnModuleReloadEnded(const EditorModuleReloadEndedEvent&) {
		_forceRebuild = true;
	}

	void InspectorPanel::OnCompileStateChanged(const EditorCompileStateChangedEvent&) {
		_forceRebuild = true;
	}

	void InspectorPanel::OnClose() {
		EventBus::GetGlobal().UnsubscribeAll(GetInstanceId());
		if (UI::Context* context = GetPanelContext()) {
			for (UI::ElementHandle row : _customRows) {
				PropertyDrawers::DropRow(*context, row);
			}
		}
		_customRows.clear();
		ReleaseDrawnAsset();
		_preview.Release();
	}

	void InspectorPanel::OnWorldRemapped(const EditorWorldRemappedEvent& event) {
		for (const EditorIdRemap& row : event.ids) {
			if (_collapsedSections.erase(row.oldId) > 0) {
				_collapsedSections.insert(row.newId);
			}
		}

		if (lockedTargetId == 0) {
			return;
		}
		for (const EditorIdRemap& row : event.ids) {
			if (row.oldId == lockedTargetId) {
				lockedTargetId = row.newId;
				return;
			}
		}
		lockedTargetId = 0;
	}

	Handle<Object> InspectorPanel::ResolveTarget() const {
		if (locked && lockedTargetId != 0) {
			if (Object* pinned = ObjectRegistry::Resolve(lockedTargetId)) {
				return Handle<Object>(pinned);
			}
		}
		if (locked && lockedAsset.IsValid()) {
			return {};
		}
		return EditorSelection::GetActive();
	}

	Guid InspectorPanel::ShownAsset() const {
		if (locked && (lockedAsset.IsValid() || lockedTargetId != 0)) {
			return lockedAsset;
		}
		return EditorSelection::HasAssetSelection() ? EditorSelection::GetSelectedAsset() : Guid{};
	}

	u64 InspectorPanel::ComputeVersion() const {
		u64 hash = 0xcbf29ce484222325ull;
		auto mix = [&hash](u64 value) {
			constexpr u64 PRIME = 0x00000100000001b3ull;
			for (u32 shift = 0; shift < 64; shift += 8) {
				hash ^= (value >> shift) & 0xFF;
				hash *= PRIME;
			}
		};
		mix(locked ? 1 : 0);
		mix(lockedTargetId);
		mix(lockedAsset.low);
		mix(lockedAsset.high);
		mix(EditorSelection::GetSelectionCount());
		mix(EditorSelection::GetAssetSelectionCount());
		mix(EditorSelection::GetSelectedAsset().low);
		mix(EditorSelection::GetSelectedAsset().high);
		if (const Guid shown = ShownAsset(); shown.IsValid()) {
			mix(StringHash(AssetDatabase::LookupPath(shown).ToString()).GetValue());
		}
		mix(_drawnPreviewRoot.IsValid() ? _drawnPreviewRoot.GetInstanceId() : 0);

		auto mixShape = [&mix](const TypeInfo* type) {
			if (!type) {
				return;
			}
			mix(type->name.GetValue());
			mix(type->moduleEpoch);
			mix(type->layoutHash);
			mix(type->fieldCount);
		};

		std::function<void(const TypeInfo*, void*, u32)> mixNestedLengths = [&](const TypeInfo* type, void* at, u32 depth) {
			if (!at || depth >= MAX_NESTED_DEPTH) {
				return;
			}
			for (const TypeInfo* walk = type; walk; walk = walk->parent) {
				for (u32 i = 0; i < walk->fieldCount; ++i) {
					const FieldInfo& field = walk->fields[i];
					if (!field.addrFn) {
						continue;
					}
					void* value = field.addrFn(at);
					const ContainerInfo* container = (field.flags & FieldFlagContainer) != 0 ? field.container : nullptr;
					if (container && container->sizeFn) {
						const size_t count = container->sizeFn(value);
						mix(count);
						if ((container->elementFlags & FieldFlagSubobject) != 0 && container->atFn) {
							for (size_t element = 0; element < count; ++element) {
								mixNestedLengths(container->elementType, container->atFn(value, element), depth + 1);
							}
						}
					} else if ((field.flags & FieldFlagSubobject) != 0) {
						mixNestedLengths(field.fieldType, value, depth + 1);
					}
				}
			}
		};
		auto mixRowInputs = [&mix, &mixNestedLengths](Object* instance) {
			for (const TypeInfo* walk = instance ? instance->GetType() : nullptr; walk; walk = walk->parent) {
				for (u32 i = 0; i < walk->fieldCount; ++i) {
					const FieldInfo& field = walk->fields[i];
					if (!field.addrFn || CustomDrawerFor(field)) {
						continue;
					}
					if (field.flags & FieldFlagContainer) {
						if (field.container && field.container->sizeFn) {
							const size_t count = field.container->sizeFn(field.addrFn(instance));
							mix(count);
							if ((field.container->elementFlags & FieldFlagSubobject) != 0 && field.container->atFn) {
								for (size_t element = 0; element < count; ++element) {
									mixNestedLengths(field.container->elementType,
													 field.container->atFn(field.addrFn(instance), element), 1);
								}
							}
						}
					} else if (field.flags & FieldFlagSubobject) {
						mixNestedLengths(field.fieldType, field.addrFn(instance), 1);
					} else if (field.flags & FieldFlagHandle) {
						mix(*static_cast<const u64*>(field.addrFn(instance)));
					}
				}
			}
		};

		Asset* drawn = _drawnAsset.Get();
		mix(drawn ? _drawnAsset.GetInstanceId() : 0);
		mixShape(drawn ? drawn->GetType() : nullptr);
		mixRowInputs(drawn);

		Handle<Object> target = ResolveTarget();
		mix(target.GetInstanceId());
		Object* object = target.Get();
		if (object && TypeRegistry::IsDerivedFrom(object->GetType(), SceneObject::GetReflectType())) {
			SceneObject* sceneObject = static_cast<SceneObject*>(object);
			mix(sceneObject->GetComponentCount());
			for (u32 i = 0; i < sceneObject->GetComponentCount(); ++i) {
				Handle<Component> component = sceneObject->GetComponentAt(i);
				mix(component.GetInstanceId());
				if (Component* resolved = component.Get()) {
					mixShape(resolved->GetType());
					mixRowInputs(resolved);
				}
			}
		}
		return hash;
	}

	void InspectorPanel::OnBuildContent(UI::Context& context, UI::ElementHandle root) {
		UI::Style rootStyle;
		rootStyle.flexDirection = UI::FlexDirection::Column;
		rootStyle.alignItems = UI::Align::Stretch;
		rootStyle.width = UI::Length::Percent(1.0f);
		rootStyle.height = UI::Length::Percent(1.0f);
		context.SetStyle(root, rootStyle);

		_header = PanelHeader::BuildStrip(context, root);
		FillHeader(context);

		UI::Widgets::ScrollViewParts scroll = UI::Widgets::ScrollView(context, root);
		UI::Style scrollStyle = context.GetStyle(scroll.root);
		scrollStyle.flexGrow = 1.0f;
		context.SetStyle(scroll.root, scrollStyle);
		_viewport = scroll.viewport;
		_scrollRoot = scroll.root;
		_emptyState = PanelEmptyState::Build(context, root);

		_preview.Build(context, root);

		RebuildAll(context);
		_lastVersion = ComputeVersion();
		_preview.Tick(context);
	}

	void InspectorPanel::OnTick(f32) {
		UI::Context* context = GetPanelContext();
		if (!context || !context->IsAlive(_viewport)) {
			return;
		}
		EditorSceneOps::FlushPrefabPreview();
		_gaps.SyncHits(*context);
		if (u64 undoStamp = UndoStamp(); undoStamp != _undoStamp && !UndoSystem::IsGroupOpen()) {
			_undoStamp = undoStamp;
			if (_drawnObject.IsValid()) {
				PrefabInstanceDiff fresh;
				EditorSceneOps::PrefabOverridesFor(_drawnObject, fresh);
				if (MarkSetHash(fresh) != _drawnMarks) {
					_forceRebuild = true;
				}
			}
		}
		u64 version = ComputeVersion();
		if (version != _lastVersion || _forceRebuild) {
			_forceRebuild = false;
			RebuildAll(*context);
			_lastVersion = ComputeVersion();
		} else {
			RefreshValues(*context);
		}
		_preview.Tick(*context);
	}

	void InspectorPanel::OnSelectionChanged() {
		_forceRebuild = true;
	}

	void InspectorPanel::RebuildAll(UI::Context& context) {
		context.ClearChildren(_viewport);
		_gaps.BeginBuild(context);
		_bindings.clear();
		_customRows.clear();
		DrawnAssetRelease releasePrevious{_drawnAsset};
		_drawnAsset = {};
		_drawnPreviewRoot = {};
		_drawnObject = {};
		_overridesRoot = {};
		_overrides = PrefabInstanceDiff{};
		_drawnMarks = 0;

		_preview.Rebuild(context, ShownAsset(),
						 previewExpanded, [this]() {
							 previewExpanded = !previewExpanded;
							 _forceRebuild = true;
						 });

		Handle<SceneObject> dropTarget = ResolveDropTarget();
		context.Events(_viewport).onDragOver = [dropTarget](const UI::DragPayload& payload) {
			return EditorSceneOps::ClassifyComponentDrop(PayloadComponent(payload), dropTarget) ==
				   EditorSceneOps::ComponentDropKind::Move;
		};
		context.Events(_viewport).onDrop = [dropTarget](const UI::DragPayload& payload, Vec2) {
			Handle<Component> dragged = PayloadComponent(payload);
			if (EditorSceneOps::ClassifyComponentDrop(dragged, dropTarget) ==
				EditorSceneOps::ComponentDropKind::Move) {
				EditorSceneOps::DropComponentOn(dragged, dropTarget, EditorSceneOps::COMPONENT_INDEX_END);
			}
		};

		FillHeader(context);

		if (const Guid shown = ShownAsset(); shown.IsValid()) {
			PanelEmptyState::Hide(context, _emptyState, _scrollRoot);
			BuildAssetInspector(context, shown);
			return;
		}

		Handle<Object> target = ResolveTarget();
		Object* object = target.Get();
		if (!object) {
			PanelEmptyState::Show(context, _emptyState, _scrollRoot,
								  {EditorIcons::Glyph::PanelInspector, "Nothing selected", {}});
			return;
		}
		PanelEmptyState::Hide(context, _emptyState, _scrollRoot);
		if (EditorSelection::GetSelectionCount() > 1 && !locked) {
			AddInspectorHeader(context, _viewport,
							   std::format("{} objects selected (showing active)", EditorSelection::GetSelectionCount()), TextNormal());
		}

		if (TypeRegistry::IsDerivedFrom(object->GetType(), SceneObject::GetReflectType())) {
			BuildObjectInspector(context, Handle<SceneObject>(static_cast<SceneObject*>(object)));
		} else if (TypeRegistry::IsDerivedFrom(object->GetType(), Component::GetReflectType())) {
			Component* component = static_cast<Component*>(object);
			RefreshOverrides(component->GetOwner());
			BuildComponentSection(context, _viewport, Handle<Component>(component));
		} else {
			AddInspectorHeader(context, _viewport, TypeLabel(object->GetType()), TextStrong());
		}
	}

	void InspectorPanel::FillHeader(UI::Context& context) {
		if (!context.IsAlive(_header)) {
			return;
		}
		PanelHeader::ClearStrip(context, _header);
		PanelHeader::ToolButtonDesc lock;
		lock.icon = locked ? EditorIcons::Glyph::Lock : EditorIcons::Glyph::Unlock;
		lock.fallback = locked ? "Unlock" : "Lock";
		lock.tooltip = locked ? "Unlock this Inspector to follow the selection again"
							  : "Lock this Inspector to what it is showing";
		lock.checked = locked;
		lock.onClick = [this]() {
			const Guid showing = locked ? Guid{} : ShownAsset();
			locked = !locked;
			lockedAsset = locked ? showing : Guid{};
			lockedTargetId = locked && !showing.IsValid() ? EditorSelection::GetActive().GetInstanceId() : 0;
			_forceRebuild = true;
		};
		PanelHeader::ToolButton(context, _header, lock);
	}

	void InspectorPanel::BuildMissingComponentBanner(UI::Context& context, Handle<Scene> scene) {
		EditorSceneOps::PreservedRowSurvey survey = EditorSceneOps::SurveyPreservedComponents(scene);
		if (survey.rows == 0) {
			return;
		}

		str names;
		for (StringHash type : survey.types) {
			names += (names.empty() ? "" : ", ") + TypeNameOps::MissingTypeLabel(type);
		}
		str message = std::format("{} component{} in this scene couldn't be loaded: {}.",
								  survey.rows, survey.rows == 1 ? "" : "s", names);
		bool focusConsole = false;
		switch (ScriptProjectOps::GetModuleStatus(ProjectOps::GetMountedProjectPath())) {
		case ScriptProjectOps::ModuleStatus::NoScriptProject:
			message += " This project has no C++ module (the scene was authored for one).";
			break;
		case ScriptProjectOps::ModuleStatus::Compiling:
			message += " Compiling. They return when the compile finishes.";
			break;
		case ScriptProjectOps::ModuleStatus::CompileFailed:
			message += " The last compile failed. Open the Console.";
			focusConsole = true;
			break;
		case ScriptProjectOps::ModuleStatus::LoadFailed:
			message += " The module is not loaded. See the Console.";
			focusConsole = true;
			break;
		case ScriptProjectOps::ModuleStatus::Loaded:
			message += " Their types are not in the loaded module.";
			break;
		}
		message += " Their data is saved unchanged.";

		UI::ElementHandle banner = context.CreateElement();
		context.AppendChild(_viewport, banner);
		UI::Style bannerStyle;
		bannerStyle.padding = UI::Edges::All(8.0f);
		bannerStyle.width = UI::Length::Percent(1.0f);
		context.SetStyle(banner, bannerStyle);
		context.SetText(banner, UISystem::GetDefaultFont(), FieldFont(),
						EditorTheme::GetColor(ColorToken::StatusWarn), message);
		if (focusConsole) {
			context.AddClass(banner, StringHash("slt-tree-row"));
			context.Events(banner).onClick = []() {
				PanelManager::FocusPanelOfType(StringHash("ConsolePanel"));
			};
		} else {
			context.SetHoverable(banner, false);
		}
	}

	void InspectorPanel::BuildObjectInspector(UI::Context& context, Handle<SceneObject> objectHandle) {
		SceneObject* object = objectHandle.Get();
		Handle<FontAsset> font = UISystem::GetDefaultFont();
		EditTarget objectTarget = TargetFor(object);
		bool structural = !_assetEditTarget.IsValid();

		RefreshOverrides(objectHandle);

		BuildMissingComponentBanner(context, object->GetScene());

		UI::ElementHandle headerRow = context.CreateElement();
		context.AppendChild(_viewport, headerRow);
		UI::Style headerStyle;
		headerStyle.flexDirection = UI::FlexDirection::Row;
		headerStyle.alignItems = UI::Align::Center;
		headerStyle.padding = {8.0f, 6.0f, 8.0f, 6.0f};
		context.SetStyle(headerRow, headerStyle);

		UI::Widgets::TextFieldDesc nameField;
		nameField.text = object->name;
		nameField.font = font;
		nameField.fontSizePx = EditorTheme::GetMetric(MetricToken::Font);
		nameField.onCommit = [objectTarget](std::string_view text) {
			Object* live = objectTarget.Resolve();
			if (live && !text.empty() && text != static_cast<SceneObject*>(live)->name) {
				UndoSystem::Push(MakeUnique<PropertySetCommand>(
					objectTarget, "name"_hash, Variant(static_cast<SceneObject*>(live)->name),
					Variant(str(text)), "Rename"));
			}
		};
		UI::ElementHandle nameElement = UI::Widgets::TextField(context, headerRow, nameField);
		{
			Binding binding;
			binding.kind = Binding::Kind::TextField;
			binding.target = objectTarget;
			binding.field = RefOn(objectTarget, "name"_hash);
			binding.element = nameElement;
			_bindings.push_back(binding);
		}

		UI::Style nameStyle = context.GetStyle(nameElement);
		nameStyle.flexGrow = 1.0f;
		nameStyle.margin = {0.0f, 0.0f, EditorTheme::GetMetric(MetricToken::IconGap), 0.0f};
		context.SetStyle(nameElement, nameStyle);

		UI::Widgets::ToggleParts active = UI::Widgets::Toggle(
			context, headerRow, object->IsActiveSelf(), [objectTarget](bool value) {
				UndoSystem::Push(MakeUnique<SetActiveCommand>(objectTarget, value));
			});
		{
			Binding binding;
			binding.kind = Binding::Kind::ActiveBox;
			binding.target = objectTarget;
			binding.toggle = active;
			_bindings.push_back(binding);
		}

		UI::ElementHandle trs = context.CreateElement();
		context.AppendChild(_viewport, trs);
		UI::Style trsStyle;
		trsStyle.flexDirection = UI::FlexDirection::Column;
		trsStyle.width = UI::Length::Percent(1.0f);
		context.SetStyle(trs, trsStyle);
		UI::ElementHandle trsHeader = AddInspectorHeader(context, trs, "Transform", TextStrong(), EditorIcons::Mark::Transform);
		PanelHeader::OverflowButton(context, trsHeader, [objectTarget](Vector<UI::Widgets::MenuItem>& items) {
			UI::Widgets::MenuItem reset;
			reset.label = "Reset";
			reset.onSelect = [objectTarget]() {
				const StringHash fields[] = {"_localPosition"_hash, "_localRotation"_hash, "_localScale"_hash};
				EditorSceneOps::ResetFieldsToDefaults(objectTarget, fields, "Reset Transform");
			};
			items.push_back(reset);
		});
		EditTarget asObject = objectTarget;
		u64 objectId = objectHandle.GetInstanceId();

		auto vectorRow = [&](const char* label, StringHash field, Vec3 value, Binding::Kind kind) {
			RowOverride mark = OverrideMarkFor(asObject, field);
			RowValue place;
			place.target = asObject;
			place.field = field;
			UI::ElementHandle row = MakeRow(context, trs, label, &mark, nullptr, &place);
			UI::Widgets::VectorFieldDesc desc;
			desc.componentCount = 3;
			desc.values[0] = value.x;
			desc.values[1] = value.y;
			desc.values[2] = value.z;
			desc.font = font;
			desc.fontSizePx = FieldFont();
			desc.onEditBegin = [objectHandle]() {
				UndoSystem::BeginGroup("Edit Transform");
				const Handle<SceneObject> held[] = {objectHandle};
				EditorTransformGesture::Begin(held);
			};
			desc.onEditEnd = []() {
				UndoSystem::EndGroup();
				EditorTransformGesture::End();
			};
			UI::Widgets::VectorFieldParts parts{};

			if (kind == Binding::Kind::EulerField) {
				desc.onChange = [this, objectTarget, objectId, field](u32 component, f64 componentValue) {
					SceneObject* live = static_cast<SceneObject*>(objectTarget.Resolve());
					if (!live) {
						return;
					}
					Vec3& euler = _eulerCache[objectId];
					euler[component] = static_cast<f32>(componentValue);
					Quat before = live->GetLocalRotation();
					Quat after = Quat::FromEuler(euler);
					UndoSystem::Push(MakeUnique<PropertySetCommand>(
						objectTarget, field, Variant(before), Variant(after), "Rotate"));
				};
			} else {
				desc.onChange = [objectTarget, field](u32 component, f64 componentValue) {
					Object* live = objectTarget.Resolve();
					if (!live) {
						return;
					}
					Variant current;
					if (!GetField(live, live->GetType(), field, &current) || !current.Is<Vec3>()) {
						return;
					}
					Vec3 before = current.As<Vec3>();
					Vec3 after = before;
					after[component] = static_cast<f32>(componentValue);
					UndoSystem::Push(MakeUnique<PropertySetCommand>(
						objectTarget, field, Variant(before), Variant(after), "Edit Transform"));
				};
			}
			parts = UI::Widgets::VectorField(context, row, desc);

			Binding binding;
			binding.kind = kind;
			binding.target = asObject;
			binding.field = RefOn(asObject, field);
			binding.vector = parts;
			binding.componentCount = 3;
			_bindings.push_back(binding);
		};

		vectorRow("Position", "_localPosition"_hash, object->GetLocalPosition(), Binding::Kind::VectorField);
		Vec3 euler = _eulerCache.contains(objectId) ? _eulerCache[objectId]
													: EditorSceneOps::EulerForDisplay(object->GetLocalRotation());
		_eulerCache[objectId] = euler;
		vectorRow("Rotation", "_localRotation"_hash, euler, Binding::Kind::EulerField);
		vectorRow("Scale", "_localScale"_hash, object->GetLocalScale(), Binding::Kind::VectorField);

		auto removedAt = [&](u32 slot, bool andPast) {
			for (const PrefabInstanceDiff::RemovedComponent& removed : _overrides.removedComponents) {
				if (removed.owner.GetInstanceId() != objectHandle.GetInstanceId()) {
					continue;
				}
				if (removed.index == slot || (andPast && removed.index > slot)) {
					UI::Widgets::Separator(context, _viewport);
					BuildRemovedComponentSection(context, _viewport, removed);
				}
			}
		};
		for (u32 i = 0; i < object->GetComponentCount(); ++i) {
			removedAt(i, false);
			Handle<Component> component = object->GetComponentAt(i);
			if (component.IsValid() && !component.Get()->IsPendingDestroy()) {
				UI::Widgets::Separator(context, _viewport);
				if (structural) {
					BuildComponentInsertStrip(context, objectHandle, i);
				}
				BuildComponentSection(context, _viewport, component);
			}
		}
		removedAt(object->GetComponentCount(), true);
		UI::Widgets::Separator(context, _viewport);
		if (structural) {
			BuildComponentInsertStrip(context, objectHandle, object->GetComponentCount());
		}

		UI::ElementHandle buttonRow = context.CreateElement();
		context.AppendChild(_viewport, buttonRow);
		UI::Style buttonRowStyle;
		buttonRowStyle.flexDirection = UI::FlexDirection::Row;
		buttonRowStyle.justifyContent = UI::Align::Center;
		buttonRowStyle.padding = {10.0f, 0.0f, 10.0f, 10.0f};
		context.SetStyle(buttonRow, buttonRowStyle);
		UI::Widgets::ButtonDesc addButton;
		addButton.label = "Add Component";
		addButton.font = font;
		addButton.fontSizePx = FieldFont();
		if (structural) {
			addButton.onClick = [this, contextPtr = &context, objectHandle]() {
				OpenAddComponentPopup(*contextPtr, objectHandle);
			};
		}
		UI::ElementHandle addElement = UI::Widgets::Button(context, buttonRow, addButton);
		if (!structural) {
			MakeDisabled(context, addElement);
		}
	}

	void InspectorPanel::BuildComponentInsertStrip(UI::Context& context, Handle<SceneObject> owner, u32 index) {
		EditorInsertStripDesc desc;
		desc.onDragOver = [owner, index](const UI::DragPayload& payload) {
			Handle<Component> dragged = PayloadComponent(payload);
			if (EditorSceneOps::ClassifyComponentDrop(dragged, owner) == EditorSceneOps::ComponentDropKind::None) {
				return false;
			}
			SceneObject* resolved = owner.Get();
			if (resolved && dragged.IsValid()) {
				for (u32 i = 0; i < resolved->GetComponentCount(); ++i) {
					if (resolved->GetComponentAt(i) == dragged) {
						return index != i && index != i + 1;
					}
				}
			}
			return true;
		};
		desc.onDrop = [owner, index](const UI::DragPayload& payload, Vec2) {
			EditorSceneOps::DropComponentOn(PayloadComponent(payload), owner, index);
		};
		_gaps.PendStrip(std::move(desc));
		_gaps.DrainInto(context, _viewport);
	}

	void InspectorPanel::BuildComponentSection(UI::Context& context, UI::ElementHandle parent, Handle<Component> componentHandle) {
		Component* component = componentHandle.Get();
		if (!component) {
			return;
		}
		const TypeInfo* type = component->GetType();
		Handle<FontAsset> font = UISystem::GetDefaultFont();
		u64 componentId = componentHandle.GetInstanceId();
		EditTarget componentTarget = TargetFor(component);
		bool structural = !_assetEditTarget.IsValid();

		UnresolvedComponent* preserved = type == UnresolvedComponent::GetReflectType()
											 ? static_cast<UnresolvedComponent*>(component)
											 : nullptr;

		UI::ElementHandle card = context.CreateElement();
		context.AppendChild(parent, card);
		UI::Style cardStyle;
		cardStyle.flexDirection = UI::FlexDirection::Column;
		cardStyle.width = UI::Length::Percent(1.0f);
		context.SetStyle(card, cardStyle);
		context.AddClass(card, EditorTheme::Class::ComponentCard);

		const ComponentInspectorFn* customInspector = preserved ? nullptr : PropertyDrawers::FindComponentInspector(type->name);
		Vector<const FieldInfo*> fields{GetMemoryResource(MemTag::Editor)};
		if (!preserved && !customInspector) {
			ForEachField(component, type, [&fields](const FieldInfo* field, const Variant*) { fields.push_back(field); }, FieldWalkFlagIncludeInherited | FieldWalkFlagExcludeHidden);
		}
		const bool hasBody = preserved || customInspector || !fields.empty();

		bool collapsed = !hasBody || _collapsedSections.contains(componentId);

		UI::ElementHandle header = context.CreateElement();
		context.AppendChild(card, header);
		UI::Style headerStyle;
		headerStyle.flexDirection = UI::FlexDirection::Row;
		headerStyle.alignItems = UI::Align::Center;
		headerStyle.padding = {8.0f, 0.0f, 8.0f, collapsed ? 0.0f : 4.0f};
		context.SetStyle(header, headerStyle);

		UI::ElementHandle caret = AppendCaretGutter(context, header);
		if (hasBody) {
			EditorIcons::SetGlyphOrText(context, caret,
										collapsed ? EditorIcons::Glyph::CaretRight : EditorIcons::Glyph::CaretDown,
										collapsed ? ">" : "v", TextDim(),
										EditorTheme::GetMetric(MetricToken::IconGutter), RowFont());
			UI::Style caretStyle = context.GetStyle(caret);
			caretStyle.textAlign = UI::Align::Center;
			caretStyle.cursor = SystemCursor::Hand;
			context.SetStyle(caret, caretStyle);
			context.SetHoverable(caret, true);
		}

		EditorIcons::ComponentIcons::Add(context, header, type, 0.0f, 0.0f, RowFont());
		if (IsAddedComponent(componentId)) {
			EditorIcons::Add(context, header, EditorIcons::Glyph::Add, OverrideColor(),
							 EditorIcons::BodySizePx(), EditorTheme::GetMetric(MetricToken::IconGap), RowFont());
		}

		if (hasBody) {
			context.Events(caret).onClick = [this, componentId]() {
				if (!_collapsedSections.insert(componentId).second) {
					_collapsedSections.erase(componentId);
				}
				_forceRebuild = true;
			};
		}

		UI::ElementHandle title = context.CreateElement();
		context.AppendChild(header, title);
		UI::Style titleStyle;
		titleStyle.margin = {0.0f, 0.0f, EditorTheme::GetMetric(MetricToken::IconGap), 0.0f};
		titleStyle.flexGrow = 1.0f;
		context.SetStyle(title, titleStyle);
		if (preserved) {
			context.SetText(title, font, RowFont(), EditorTheme::GetColor(ColorToken::StatusWarn),
							"Missing: " + TypeNameOps::MissingTypeLabel(preserved->GetUnresolvedType()));
		} else {
			context.SetText(title, font, RowFont(), TextStrong(), TypeLabel(type));
		}
		if (!preserved && structural) {
			context.AddClass(title, EditorTheme::Class::DragHandle);
			context.Events(title).onPressDrag = [componentHandle](Vec2) {
				Component* live = componentHandle.Get();
				if (!live || UISystem::IsDragPayloadActive()) {
					return;
				}
				UI::DragPayload payload;
				payload.type = "Component"_hash;
				payload.data = Variant(componentHandle.GetInstanceId());
				payload.display = TypeLabel(live->GetType());
				UISystem::BeginDragPayload(payload);
			};
		}

		if (!preserved) {
			UI::Widgets::ToggleParts enabled = UI::Widgets::Toggle(
				context, header, component->IsEnabledSelf(), [componentTarget](bool value) {
					UndoSystem::Push(MakeUnique<SetEnabledCommand>(componentTarget, value));
				});
			UI::Style switchStyle = context.GetStyle(enabled.track);
			switchStyle.margin.right = EditorTheme::GetMetric(MetricToken::IconGap);
			context.SetStyle(enabled.track, switchStyle);

			Binding binding;
			binding.kind = Binding::Kind::EnabledBox;
			binding.target = componentTarget;
			binding.toggle = enabled;
			_bindings.push_back(binding);
		}

		u32 slot = 0;
		Handle<SceneObject> owner;
		if (SceneObject* owning = component->GetOwner().Get()) {
			owner = Handle<SceneObject>(owning);
			for (u32 i = 0; i < owning->GetComponentCount(); ++i) {
				if (owning->GetComponentAt(i) == componentHandle) {
					slot = i;
					break;
				}
			}
		}
		PanelHeader::OverflowButton(context, header, [this, componentHandle, componentTarget, owner, slot, structural, preserved, contextPtr = &context](Vector<UI::Widgets::MenuItem>& items) {
			UI::Widgets::MenuItem reset;
			reset.label = "Reset";
			reset.enabled = preserved == nullptr;
			reset.onSelect = [componentTarget]() {
				EditorSceneOps::ResetComponentToDefaults(componentTarget);
			};
			items.push_back(reset);

			SceneObject* owning = owner.Get();
			u32 count = owning ? owning->GetComponentCount() : 0;
			UI::Widgets::MenuItem up;
			up.label = "Move Up";
			up.separatorBefore = true;
			up.enabled = structural && slot > 0;
			up.onSelect = [componentHandle, owner, slot]() {
				EditorSceneOps::DropComponentOn(componentHandle, owner, slot - 1);
			};
			items.push_back(up);

			UI::Widgets::MenuItem down;
			down.label = "Move Down";
			down.enabled = structural && count > 0 && slot + 1 < count;
			down.onSelect = [componentHandle, owner, slot]() {
				EditorSceneOps::DropComponentOn(componentHandle, owner, slot + 2);
			};
			items.push_back(down);

			UI::Widgets::MenuItem remove;
			remove.label = "Remove Component";
			remove.enabled = structural || preserved != nullptr;
			if (preserved) {
				remove.onSelect = [componentHandle, contextPtr]() {
					Component* live = componentHandle.Get();
					if (!live || live->GetType() != UnresolvedComponent::GetReflectType()) {
						return;
					}
					str missing = TypeNameOps::MissingTypeLabel(
						static_cast<UnresolvedComponent*>(live)->GetUnresolvedType());
					SceneFileFlows::OpenConfirmPopup(
						*contextPtr,
						"Remove the missing '" + missing + "' component? Its saved data cannot be recovered.",
						"Remove", [componentHandle]() { EditorSceneOps::RemovePreservedComponent(componentHandle); },
						"", {});
				};
			} else {
				remove.onSelect = [componentHandle]() {
					UndoSystem::Push(MakeUnique<RemoveComponentCommand>(componentHandle));
				};
			}
			items.push_back(remove);

			u64 componentId = componentHandle.GetInstanceId();
			if (IsAddedComponent(componentId)) {
				UI::Widgets::MenuItem revertAdded;
				revertAdded.label = "Revert Added Component";
				revertAdded.separatorBefore = true;
				revertAdded.enabled = structural;
				revertAdded.onSelect = [componentHandle]() {
					UndoSystem::Push(MakeUnique<RemoveComponentCommand>(componentHandle));
				};
				items.push_back(revertAdded);
			} else if (HasPropertyOverrides(componentId)) {
				UI::Widgets::MenuItem revertModified;
				revertModified.label = "Revert Modified Component";
				revertModified.separatorBefore = true;
				revertModified.onSelect = [this, componentHandle, componentTarget]() {
					RevertComponentOverrides(componentTarget, componentHandle.GetInstanceId());
				};
				items.push_back(revertModified);
			}
		});

		if (collapsed) {
			return;
		}

		if (preserved) {
			UI::ElementHandle note = context.CreateElement();
			context.AppendChild(card, note);
			UI::Style noteStyle;
			noteStyle.padding = {8.0f, 0.0f, 8.0f, 8.0f};
			context.SetStyle(note, noteStyle);
			context.SetText(note, font, FieldFont(), TextDim(),
							"This type is not loaded. Its data is saved unchanged. The "
							"component returns as soon as a compile defines the type again.");
			return;
		}
		if (customInspector) {
			InspectorPanel* previous = _buildingPanel;
			_buildingPanel = this;
			(*customInspector)(context, card, componentHandle);
			_buildingPanel = previous;
			return;
		}
		for (const FieldInfo* field : fields) {
			BuildFieldRow(context, card, *field, componentTarget);
		}
	}

	void InspectorPanel::RefreshOverrides(Handle<SceneObject> object) {
		_drawnObject = object;
		EditorSceneOps::PrefabOverridesFor(object, _overrides, &_overridesRoot);
		_drawnMarks = MarkSetHash(_overrides);
		_undoStamp = UndoStamp();
	}

	InspectorPanel::RowOverride InspectorPanel::OverrideMarkFor(EditTarget target, StringHash field) const {
		RowOverride mark;
		Object* object = target.Resolve();
		if (!object) {
			return mark;
		}
		u64 objectId = object->GetInstanceId();
		for (const PrefabInstanceDiff::Property& property : _overrides.properties) {
			if (property.target.GetInstanceId() != objectId || property.fieldPath.empty() || property.fieldPath[0] != field) {
				continue;
			}
			mark.overridden = true;
			if (property.fieldPath.size() == 1 && (property.isHandle || !property.base.IsEmpty())) {
				mark.onRevert = [target, field, base = property.base, baseId = property.baseHandleId, isHandle = property.isHandle]() {
					RevertProperty(target, field, base, baseId, isHandle);
				};
			}
		}
		return mark;
	}

	bool InspectorPanel::IsAddedComponent(u64 componentId) const {
		for (const Handle<Component>& added : _overrides.addedComponents) {
			if (added.GetInstanceId() == componentId) {
				return true;
			}
		}
		return false;
	}

	bool InspectorPanel::HasPropertyOverrides(u64 componentId) const {
		for (const PrefabInstanceDiff::Property& property : _overrides.properties) {
			if (property.target.GetInstanceId() == componentId) {
				return true;
			}
		}
		return false;
	}

	void InspectorPanel::RevertComponentOverrides(EditTarget component, u64 componentId) {
		UndoSystem::BeginGroup("Revert Component");
		for (const PrefabInstanceDiff::Property& property : _overrides.properties) {
			if (property.target.GetInstanceId() != componentId || property.fieldPath.size() != 1) {
				continue;
			}
			if (!property.isHandle && property.base.IsEmpty()) {
				continue;
			}
			RevertProperty(component, property.fieldPath[0], property.base, property.baseHandleId, property.isHandle);
		}
		UndoSystem::EndGroup();
	}

	u64 InspectorPanel::MarkSetHash(const PrefabInstanceDiff& diff) {
		u64 hash = 0xcbf29ce484222325ull;
		auto mix = [&hash](u64 value) {
			constexpr u64 PRIME = 0x00000100000001b3ull;
			for (u32 shift = 0; shift < 64; shift += 8) {
				hash ^= (value >> shift) & 0xFF;
				hash *= PRIME;
			}
		};
		for (const PrefabInstanceDiff::Property& property : diff.properties) {
			mix(property.target.GetInstanceId());
			mix(property.fieldPath.empty() ? 0 : property.fieldPath[0].GetValue());
		}
		for (const Handle<Component>& added : diff.addedComponents) {
			mix(added.GetInstanceId());
		}
		for (const PrefabInstanceDiff::RemovedComponent& removed : diff.removedComponents) {
			mix(removed.owner.GetInstanceId());
			mix(removed.row.leaf.localId);
			mix(removed.index);
		}
		return hash;
	}

	void InspectorPanel::BuildRemovedComponentSection(UI::Context& context, UI::ElementHandle parent,
													  const PrefabInstanceDiff::RemovedComponent& removed) {
		Handle<FontAsset> font = UISystem::GetDefaultFont();
		const TypeInfo* type = TypeRegistry::FindType(removed.type);
		bool structural = !_assetEditTarget.IsValid();

		UI::ElementHandle card = context.CreateElement();
		context.AppendChild(parent, card);
		UI::Style cardStyle;
		cardStyle.flexDirection = UI::FlexDirection::Column;
		cardStyle.width = UI::Length::Percent(1.0f);
		context.SetStyle(card, cardStyle);
		context.AddClass(card, EditorTheme::Class::ComponentCard);

		UI::ElementHandle header = context.CreateElement();
		context.AppendChild(card, header);
		UI::Style headerStyle;
		headerStyle.flexDirection = UI::FlexDirection::Row;
		headerStyle.alignItems = UI::Align::Center;
		headerStyle.padding = {8.0f, 0.0f, 8.0f, 0.0f};
		context.SetStyle(header, headerStyle);

		AppendCaretGutter(context, header);

		EditorIcons::ComponentIcons::Add(context, header, type, 0.0f, 0.0f, RowFont());
		EditorIcons::Add(context, header, EditorIcons::Glyph::Remove, OverrideColor(),
						 EditorIcons::BodySizePx(), EditorTheme::GetMetric(MetricToken::IconGap), RowFont());

		UI::ElementHandle title = context.CreateElement();
		context.AppendChild(header, title);
		context.AddClass(title, EditorTheme::Class::Disabled);
		UI::Style titleStyle;
		titleStyle.margin = {0.0f, 0.0f, EditorTheme::GetMetric(MetricToken::IconGap), 0.0f};
		titleStyle.flexGrow = 1.0f;
		context.SetStyle(title, titleStyle);
		str label = type ? TypeLabel(type) : TypeNameOps::MissingTypeLabel(removed.type);
		context.SetText(title, font, RowFont(), TextDim(), label + " (Removed)");

		PanelHeader::OverflowButton(context, header, [root = _overridesRoot, owner = removed.owner, row = removed.row, typeName = removed.type, structural](Vector<UI::Widgets::MenuItem>& items) {
			UI::Widgets::MenuItem revert;
			revert.label = "Revert Removed Component";
			revert.enabled = structural && root.IsValid();
			revert.onSelect = [root, owner, row, typeName]() {
				UndoSystem::Push(MakeUnique<RevertRemovedComponentCommand>(root, owner, row, typeName));
			};
			items.push_back(revert);
		});
	}

	EditTarget InspectorPanel::TargetFor(Object* object) const {
		if (!object) {
			return {};
		}
		if (!_assetEditTarget.IsValid()) {
			return Handle<Object>(object);
		}
		u64 rowKey = 0;
		const TypeInfo* type = object->GetType();
		if (TypeRegistry::IsDerivedFrom(type, SceneObject::GetReflectType())) {
			rowKey = static_cast<SceneObject*>(object)->GetPrefabKey();
		} else if (TypeRegistry::IsDerivedFrom(type, Component::GetReflectType())) {
			rowKey = static_cast<Component*>(object)->GetPrefabKey();
		}
		return rowKey != 0 ? EditTarget::PrefabRow(_assetEditTarget, rowKey) : EditTarget(Handle<Object>(object));
	}

	void InspectorPanel::BuildPrefabAssetBody(UI::Context& context, Handle<SceneObject> object, const Guid& assetGuid) {
		SceneObject* resolved = object.Get();
		if (!resolved || !assetGuid.IsValid()) {
			return;
		}
		Guid previous = _assetEditTarget;
		_assetEditTarget = assetGuid;
		_drawnPreviewRoot = object;
		BuildObjectInspector(context, object);
		_assetEditTarget = previous;
	}

	InspectorPanel* InspectorPanel::GetBuildingPanel() {
		return _buildingPanel;
	}

	namespace {

		void CollectCreatableKinds(const TypeInfo* base, Vector<const TypeInfo*>& out) {
			if (!base) {
				return;
			}
			struct Scan {
				const TypeInfo* base;
				Vector<const TypeInfo*>* out;
			} scan{base, &out};
			TypeRegistry::ForEachType(
				+[](const TypeInfo* type, void* user) {
					auto* s = static_cast<Scan*>(user);
					if (type->createFn && TypeRegistry::IsDerivedFrom(type, s->base)) {
						s->out->push_back(type);
					}
				},
				&scan);
			std::sort(out.begin(), out.end(), [](const TypeInfo* a, const TypeInfo* b) {
				return std::string_view(a->displayName ? a->displayName : "") <
					   std::string_view(b->displayName ? b->displayName : "");
			});
		}

		str DirectoryForNewAsset(EditTarget target) {
			str fallback = "project://";
			std::span<const std::string_view> roots = ProjectLayout::GetContentRoots();
			if (!roots.empty()) {
				fallback += str(roots[0]);
			}
			Object* object = target.Resolve();
			if (!object) {
				return fallback;
			}
			Handle<Scene> scene;
			const TypeInfo* type = object->GetType();
			if (TypeRegistry::IsDerivedFrom(type, Component::GetReflectType())) {
				scene = static_cast<Component*>(object)->GetScene();
			} else if (TypeRegistry::IsDerivedFrom(type, SceneObject::GetReflectType())) {
				scene = static_cast<SceneObject*>(object)->GetScene();
			}
			Scene* resolved = scene.Get();
			if (!resolved || !resolved->GetSourceGuid().IsValid()) {
				return fallback;
			}
			Path scenePath = AssetDatabase::LookupPath(resolved->GetSourceGuid());
			str text = scenePath.ToString();
			size_t slash = text.find_last_of('/');
			return slash == str::npos ? fallback : text.substr(0, slash);
		}

		str ExtensionForKind(const TypeInfo* kind, IAssetImporter*& outImporter) {
			outImporter = nullptr;
			for (const TypeInfo* walk = kind; walk && !outImporter; walk = walk->parent) {
				outImporter = AssetImporterRegistry::Find(walk->name);
			}
			if (!outImporter) {
				return {};
			}
			str extension;
			IAssetImporter* owner = outImporter;
			AssetImporterRegistry::ForEach([&extension, owner](StringHash, IAssetImporter* candidate, std::string_view primary) {
				if (candidate == owner && extension.empty()) {
					extension = str(primary);
				}
			});
			return extension;
		}

		u64 MintAssetOfKind(const TypeInfo* kind, EditTarget target) {
			if (!kind || !kind->createFn) {
				return 0;
			}
			IAssetImporter* importer = nullptr;
			str extension = ExtensionForKind(kind, importer);
			if (extension.empty()) {
				SLT_LOG_WARN("Editor", "no registered importer gives '{}' a file extension",
							 kind->displayName ? kind->displayName : "?");
				return 0;
			}

			str directory = DirectoryForNewAsset(target);
			Object* instance = TypeRegistry::CreateObjectInstance(kind, MemTag::Editor);
			if (!instance) {
				return 0;
			}
			str name = str("New ") + (kind->displayName ? kind->displayName : "Asset") + extension;
			const str unique = AssetBrowserOps::MakeUniqueName(Path(directory), name);
			str virtualPath = directory + (!directory.empty() && directory.back() == ':' ? "//" : "/") + unique;
			const bool written = static_cast<Asset*>(instance)->Export(Path(virtualPath));
			TypeRegistry::DestroyObjectInstance(instance);
			if (!written) {
				SLT_LOG_WARN("Editor", "couldn't write '{}'", virtualPath);
				return 0;
			}
			Asset* loaded = AssetDatabase::LoadAsset(Path(virtualPath));
			if (!loaded) {
				SLT_LOG_WARN("Editor", "'{}' was written but didn't import", virtualPath);
				return 0;
			}
			return loaded->GetInstanceId();
		}

	} // anonymous namespace

	void InspectorPanel::BuildHandleListRows(UI::Context& context, UI::ElementHandle parent,
											 const FieldInfo& field, EditTarget target) {
		Object* object = target.Resolve();
		if (!object || !field.addrFn || !field.container || !field.container->sizeFn) {
			return;
		}
		const TypeInfo* pointee = TypeRegistry::FindType(HandlePointeeName(*field.container->elementType));
		const u32 count = static_cast<u32>(field.container->sizeFn(field.addrFn(object)));
		const StringHash fieldName = field.name;
		const str label = FieldLabel(field);

		for (u32 element = 0; element < count; ++element) {
			str rowLabel = label + " " + std::to_string(element);
			RowValue value;
			value.target = target;
			value.field = fieldName;
			value.steps.push_back({static_cast<i32>(element), {}});
			UI::ElementHandle row = MakeRow(context, parent, rowLabel, nullptr, nullptr, &value);
			BuildHandleRow(context, row, target, fieldName,
						   pointee ? pointee->name : StringHash{}, rowLabel, static_cast<i32>(element));

			UI::Widgets::ButtonDesc remove;
			remove.label = "-";
			remove.font = UISystem::GetDefaultFont();
			remove.fontSizePx = FieldFont();
			remove.onClick = [target, fieldName, element]() {
				UndoSystem::Push(MakeUnique<RemoveContainerElementCommand>(target, fieldName, element, "Remove Element"));
			};
			UI::Widgets::Button(context, row, remove);
		}

		Vector<const TypeInfo*> kinds{GetMemoryResource(MemTag::Editor)};
		CollectCreatableKinds(pointee, kinds);

		UI::ElementHandle buttonRow = context.CreateElement();
		context.AppendChild(parent, buttonRow);
		UI::Style buttonRowStyle;
		buttonRowStyle.flexDirection = UI::FlexDirection::Row;
		buttonRowStyle.justifyContent = UI::Align::Center;
		context.SetStyle(buttonRow, buttonRowStyle);

		const str addLabel = str("Add ") +
							 (!kinds.empty() && pointee && pointee->displayName ? str(pointee->displayName) : label);
		UI::Widgets::ButtonDesc add;
		add.label = addLabel;
		add.font = UISystem::GetDefaultFont();
		add.fontSizePx = FieldFont();
		if (kinds.empty()) {
			add.onClick = [target, fieldName, count]() {
				UndoSystem::Push(MakeUnique<SetContainerElementCommand>(target, fieldName, count, 0ull, 0ull,
																		count, "Add Element"));
			};
		} else if (kinds.size() == 1) {
			const TypeInfo* only = kinds[0];
			add.onClick = [target, fieldName, count, only]() {
				const u64 minted = MintAssetOfKind(only, target);
				if (minted != 0) {
					UndoSystem::Push(MakeUnique<SetContainerElementCommand>(target, fieldName, count, 0ull, minted,
																			count, "Add Element"));
				}
			};
		} else {
			Vector<const TypeInfo*> menuKinds = kinds;
			add.onClick = [target, fieldName, count, menuKinds, contextPtr = &context]() {
				Vector<UI::Widgets::MenuItem> items{GetMemoryResource(MemTag::Editor)};
				for (const TypeInfo* kind : menuKinds) {
					UI::Widgets::MenuItem item;
					item.label = kind->displayName ? kind->displayName : "?";
					item.onSelect = [target, fieldName, count, kind]() {
						const u64 minted = MintAssetOfKind(kind, target);
						if (minted != 0) {
							UndoSystem::Push(MakeUnique<SetContainerElementCommand>(target, fieldName, count, 0ull,
																					minted, count, "Add Element"));
						}
					};
					items.push_back(item);
				}
				UI::Widgets::OpenMenu(*contextPtr, contextPtr->GetPointerPosition(),
									  {items.data(), items.size()}, UISystem::GetDefaultFont(), FieldFont());
			};
		}
		UI::Widgets::Button(context, buttonRow, add);
	}

	void InspectorPanel::BuildHandleRow(UI::Context& context, UI::ElementHandle row, EditTarget incoming,
										StringHash fieldName, StringHash pointeeName, const str& label,
										i32 elementIndex, std::function<void(u64)> onAssigned) {
		HandleSlot slot;
		slot.target = incoming.IsPrefabRow() ? incoming : TargetFor(incoming.Resolve());
		slot.field = fieldName;
		slot.element = elementIndex;
		BuildHandleSlotRow(context, row, std::move(slot), pointeeName, label, std::move(onAssigned));
	}

	void InspectorPanel::BuildHandleSlotRow(UI::Context& context, UI::ElementHandle row, HandleSlot slot,
											StringHash pointeeName, const str& label, std::function<void(u64)> onAssigned) {
		ObjectField::Desc field;
		const str tooltip = "Select " + label;

		field.accepts = [pointeeName](const UI::DragPayload& payload) {
			const TypeInfo* pointeeType = TypeRegistry::FindType(pointeeName);
			if (payload.type == "Asset"_hash) {
				return payload.data.Is<AssetDragData>() &&
					   AssetPayloadFits(payload.data.As<AssetDragData>(), pointeeType);
			}
			Object* dragged = DraggedObject(payload);
			return dragged && pointeeType && TypeRegistry::IsDerivedFrom(dragged->GetType(), pointeeType);
		};
		field.onDrop = [slot, pointeeName, label, onAssigned](const UI::DragPayload& payload, Vec2) {
			const TypeInfo* pointeeType = TypeRegistry::FindType(pointeeName);
			if (!slot.target.Resolve() || !pointeeType) {
				return;
			}

			u64 assign = 0;
			if (payload.type == "Asset"_hash) {
				if (!payload.data.Is<AssetDragData>()) {
					return;
				}
				Asset* asset = AssetDatabase::LoadAssetByGuid(payload.data.As<AssetDragData>().guid);
				if (!asset || !TypeRegistry::IsDerivedFrom(asset->GetType(), pointeeType)) {
					return;
				}
				assign = asset->GetInstanceId();
			} else {
				Object* dragged = DraggedObject(payload);
				if (!dragged || !TypeRegistry::IsDerivedFrom(dragged->GetType(), pointeeType)) {
					return;
				}
				assign = dragged->GetInstanceId();
			}

			u64 before = ReadSlot(slot);
			bool grouped = static_cast<bool>(onAssigned) && before != assign;
			if (grouped) {
				UndoSystem::BeginGroup("Set " + label);
			}
			WriteSlot(slot, before, assign, "Set " + label);
			if (grouped) {
				if (assign != 0) {
					onAssigned(assign);
				}
				UndoSystem::EndGroup();
			}
		};

		field.resolveTarget = [slot]() -> EditorPingTarget {
			return PingTargetForInstance(ReadSlot(slot));
		};

		UI::Context* contextPtr = &context;
		field.pickTooltip = tooltip;
		field.onPick = [this, contextPtr, slot, pointeeName, label, onAssigned]() {
			OpenHandlePickerPopup(*contextPtr, slot, pointeeName, label, onAssigned);
		};

		ObjectField::Parts built = ObjectField::Build(context, row, field);

		Binding binding;
		binding.kind = Binding::Kind::HandleLabel;
		binding.target = slot.target;
		binding.field = RefOn(slot.target, slot.field);
		binding.element = built.caption;
		binding.elementIndex = slot.element;
		binding.nested = slot.nested;
		_bindings.push_back(std::move(binding));
	}

	u64 InspectorPanel::NestedFoldKey(const NestedPath& path) const {
		Object* object = path.target.Resolve();
		str identity = object ? std::format("{:x}", object->GetInstanceId()) : str("0");
		if (object && TypeRegistry::IsDerivedFrom(object->GetType(), Asset::GetReflectType()) &&
			static_cast<Asset*>(object)->assetId.IsValid()) {
			identity = static_cast<Asset*>(object)->assetId.ToString();
		}
		str key = std::format("nested:{}:{:x}", identity, path.top.field.GetValue());
		for (const NestedStep& step : path.steps) {
			key += step.element >= 0 ? std::format("/{}", step.element) : std::format(".{:x}", step.field.field.GetValue());
		}
		return StringHash(key).GetValue();
	}

	void InspectorPanel::BuildNestedStructRows(UI::Context& context, UI::ElementHandle parent, const NestedPath& path,
											   const TypeInfo* structType, u32 depth) {
		if (!structType) {
			return;
		}
		UI::ElementHandle body = MakeNestedBody(context, parent);
		if (depth >= MAX_NESTED_DEPTH) {
			UI::ElementHandle deep = context.CreateElement();
			context.AppendChild(body, deep);
			context.SetText(deep, UISystem::GetDefaultFont(), FieldFont(), TextDim(), "(nested too deeply to draw)");
			return;
		}
		Vector<const TypeInfo*> chain{GetMemoryResource(MemTag::Editor)};
		for (const TypeInfo* walk = structType; walk; walk = walk->parent) {
			chain.push_back(walk);
		}
		for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
			for (u32 i = 0; i < (*it)->fieldCount; ++i) {
				const FieldInfo& field = (*it)->fields[i];
				if ((field.flags & FieldFlagEditorVisible) == 0 || (field.flags & FieldFlagTransient) != 0) {
					continue;
				}
				BuildNestedFieldRow(context, body, path, field, depth);
			}
		}
	}

	void InspectorPanel::BuildNestedFieldRow(UI::Context& context, UI::ElementHandle parent, const NestedPath& path,
											 const FieldInfo& field, u32 depth) {
		NestedPath sub = path;
		NestedStep step;
		step.field = RefTo(field);
		sub.steps.push_back(step);
		const str label = FieldLabel(field);

		if (const AttributeInfo* header = FindFieldAttribute(&field, "Header"_hash); header && header->value.Is<str>()) {
			AddInspectorHeader(context, parent, header->value.As<str>(), TextStrong());
		}
		const AttributeInfo* tooltip = FindFieldAttribute(&field, "Tooltip"_hash);
		const bool hasTooltip = tooltip && tooltip->value.Is<str>();

		const ContainerInfo* container = (field.flags & FieldFlagContainer) != 0 ? field.container : nullptr;
		const bool list = container && container->elementType &&
						  (container->elementFlags & (FieldFlagSubobject | FieldFlagHandle)) != 0;
		const bool structField = (field.flags & FieldFlagSubobject) != 0 && field.fieldType;
		const RowValue value = ValueAt(sub);
		const RowValue* resettable = (field.flags & FieldFlagReadOnly) == 0 ? &value : nullptr;
		if (list || structField) {
			const u64 key = NestedFoldKey(sub);
			RowFold fold;
			fold.collapsed = IsSectionCollapsed(key);
			fold.onToggle = [this, key]() { ToggleSection(key); };
			UI::ElementHandle header = MakeRow(context, parent, label, nullptr, &fold, resettable);
			if (hasTooltip) {
				context.SetTooltip(header, tooltip->value.As<str>());
			}
			if (list) {
				BuildNestedList(context, header, fold.collapsed ? UI::ElementHandle{} : parent, sub, *container, depth + 1);
			} else if (!fold.collapsed) {
				BuildNestedStructRows(context, parent, sub, field.fieldType, depth + 1);
			}
			return;
		}

		UI::ElementHandle row = MakeRow(context, parent, label, nullptr, nullptr, resettable);
		if (hasTooltip) {
			context.SetTooltip(row, tooltip->value.As<str>());
		}
		SharedPtr<NestedPath> shared = MakeShared<NestedPath>(std::move(sub));

		if ((field.flags & FieldFlagReadOnly) != 0 || PropertyRows::IsScalarField(field)) {
			PropertyRows::ValueAccess access;
			access.address = [shared]() -> void* { return ResolveNested(*shared); };
			access.beginGesture = []() { UndoSystem::BeginGroup("Edit Property"); };
			access.endGesture = []() { UndoSystem::EndGroup(); };
			FieldRef leaf = RefTo(field);
			const str undoLabel = "Set " + label;
			access.commit = [shared, leaf, undoLabel](const PropertyRows::CommittedValue& value) mutable {
				const FieldInfo* info = leaf.Resolve();
				if (!info) {
					return;
				}
				CommitNested(*shared, undoLabel, [info, &value](void* at) { return WriteCommitted(*info, at, value); });
			};
			PropertyRows::ScalarRow scalar = PropertyRows::Build(context, row, RefTo(field), std::move(access));
			if (scalar.kind != PropertyRows::ScalarRow::Kind::None) {
				Binding binding;
				binding.kind = Binding::Kind::Scalar;
				binding.target = path.target;
				binding.field = RefTo(field);
				binding.scalar = std::move(scalar);
				binding.nested = shared;
				_bindings.push_back(std::move(binding));
				return;
			}
		}

		if ((field.flags & FieldFlagHandle) != 0 && field.fieldType) {
			const TypeInfo* pointee = TypeRegistry::FindType(HandlePointeeName(*field.fieldType));
			HandleSlot slot;
			slot.target = path.target;
			slot.nested = shared;
			BuildHandleSlotRow(context, row, std::move(slot), pointee ? pointee->name : StringHash{}, label);
			return;
		}

		UI::ElementHandle unsupported = context.CreateElement();
		context.AppendChild(row, unsupported);
		context.SetText(unsupported, UISystem::GetDefaultFont(), FieldFont(), TextDim(), "(" + TypeLabel(field.fieldType) + ", no drawer)");
	}

	void InspectorPanel::BuildNestedList(UI::Context& context, UI::ElementHandle header, UI::ElementHandle parent,
										 const NestedPath& path, const ContainerInfo& container, u32 depth) {
		NestedPath resolving = path;
		void* sequence = ResolveNested(resolving);
		const u32 count = sequence && container.sizeFn ? static_cast<u32>(container.sizeFn(sequence)) : 0;

		UI::ElementHandle size = context.CreateElement();
		context.AppendChild(header, size);
		context.SetHoverable(size, false);
		context.SetText(size, UISystem::GetDefaultFont(), FieldFont(), TextDim(), std::to_string(count));
		if (!parent.IsValid() || !sequence) {
			return;
		}
		UI::ElementHandle body = MakeNestedBody(context, parent);
		if (depth >= MAX_NESTED_DEPTH) {
			return;
		}

		const bool structs = (container.elementFlags & FieldFlagSubobject) != 0;
		const TypeInfo* pointee = structs ? nullptr : TypeRegistry::FindType(HandlePointeeName(*container.elementType));
		SharedPtr<NestedPath> list = MakeShared<NestedPath>(path);

		for (u32 index = 0; index < count; ++index) {
			NestedPath element = path;
			NestedStep step;
			step.element = static_cast<i32>(index);
			element.steps.push_back(step);
			const str elementLabel = std::format("Element {}", index);
			const RowValue value = ValueAt(element);

			UI::ElementHandle row;
			bool expanded = false;
			if (structs) {
				const u64 key = NestedFoldKey(element);
				RowFold fold;
				fold.collapsed = IsSectionCollapsed(key);
				fold.onToggle = [this, key]() { ToggleSection(key); };
				row = MakeRow(context, body, elementLabel, nullptr, &fold, &value);
				expanded = !fold.collapsed;
			} else {
				row = MakeRow(context, body, elementLabel, nullptr, nullptr, &value);
				HandleSlot slot;
				slot.target = path.target;
				slot.nested = MakeShared<NestedPath>(element);
				BuildHandleSlotRow(context, row, std::move(slot), pointee ? pointee->name : StringHash{}, elementLabel);
			}

			UI::Widgets::ButtonDesc remove;
			remove.label = "-";
			remove.font = UISystem::GetDefaultFont();
			remove.fontSizePx = FieldFont();
			remove.onClick = [list, index]() {
				const ContainerInfo* live = nullptr;
				if (!ResolveNested(*list, nullptr, &live) || !live) {
					return;
				}
				CommitNested(*list, "Remove Element", [live, index](void* at) { return RemoveElement(*live, at, index); });
			};
			UI::Widgets::Button(context, row, remove);

			if (structs && expanded) {
				BuildNestedStructRows(context, body, element, container.elementType, depth);
			}
		}

		UI::ElementHandle buttonRow = context.CreateElement();
		context.AppendChild(body, buttonRow);
		UI::Style buttonRowStyle;
		buttonRowStyle.flexDirection = UI::FlexDirection::Row;
		buttonRowStyle.justifyContent = UI::Align::Center;
		context.SetStyle(buttonRow, buttonRowStyle);
		const str addLabel = "Add " + TypeLabel(structs ? container.elementType : pointee);
		UI::Widgets::ButtonDesc add;
		add.label = addLabel;
		add.font = UISystem::GetDefaultFont();
		add.fontSizePx = FieldFont();
		add.onClick = [list]() {
			const ContainerInfo* live = nullptr;
			if (!ResolveNested(*list, nullptr, &live) || !live || !live->sizeFn || !live->resizeFn) {
				return;
			}
			CommitNested(*list, "Add Element", [live](void* at) {
				live->resizeFn(at, live->sizeFn(at) + 1);
				return true;
			});
		};
		UI::Widgets::Button(context, buttonRow, add);
	}

	UI::ElementHandle InspectorPanel::MakeValueRow(UI::Context& context, UI::ElementHandle parent, std::string_view label,
												   EditTarget incoming, StringHash fieldName, i32 elementIndex) {
		RowValue value;
		value.target = incoming.IsPrefabRow() ? incoming : TargetFor(incoming.Resolve());
		value.field = fieldName;
		if (elementIndex >= 0) {
			value.steps.push_back({elementIndex, {}});
		}
		return MakeRow(context, parent, label, nullptr, nullptr, &value);
	}

	void InspectorPanel::BuildNumberElementRow(UI::Context& context, UI::ElementHandle row, EditTarget incoming,
											   StringHash fieldName, u32 elementIndex, const str& label) {
		EditTarget target = incoming.IsPrefabRow() ? incoming : TargetFor(incoming.Resolve());
		FieldRef ref = RefOn(target, fieldName);
		const FieldInfo* field = ref.Resolve();
		if (!field) {
			return;
		}

		UI::Widgets::NumberFieldDesc number;
		number.value = static_cast<f64>(ReadElementFloat(target, *field, static_cast<i32>(elementIndex)));
		number.decimals = 2;
		number.dragScale = 0.5;
		number.font = UISystem::GetDefaultFont();
		number.fontSizePx = FieldFont();
		str undoLabel = "Set " + label;
		number.onChange = [target, ref, elementIndex, undoLabel](f64 next) mutable {
			const FieldInfo* liveField = ref.Resolve();
			if (!liveField || !liveField->container || !liveField->addrFn) {
				return;
			}
			f32 before = ReadElementFloat(target, *liveField, static_cast<i32>(elementIndex));
			f32 after = static_cast<f32>(next);
			if (before == after) {
				return;
			}
			Object* live = target.Resolve();
			if (!live) {
				return;
			}
			u32 count = static_cast<u32>(liveField->container->sizeFn(liveField->addrFn(live)));
			u64 beforeBits = 0;
			u64 afterBits = 0;
			std::memcpy(&beforeBits, &before, sizeof(f32));
			std::memcpy(&afterBits, &after, sizeof(f32));
			UndoSystem::Push(MakeUnique<SetContainerElementCommand>(
				target, ref.field, elementIndex, beforeBits, afterBits, count, undoLabel));
		};
		number.onScrubBegin = [] { UndoSystem::BeginGroup("Set Blend Shape"); };
		number.onScrubEnd = [] { UndoSystem::EndGroup(); };

		Binding binding;
		binding.kind = Binding::Kind::NumberElement;
		binding.target = target;
		binding.field = ref;
		binding.number = UI::Widgets::NumberField(context, row, number);
		binding.elementIndex = static_cast<i32>(elementIndex);
		_bindings.push_back(std::move(binding));
	}

	void InspectorPanel::BuildFieldRow(UI::Context& context, UI::ElementHandle parent, const FieldInfo& field, EditTarget target) {
		Handle<FontAsset> font = UISystem::GetDefaultFont();

		if (const AttributeInfo* header = FindFieldAttribute(&field, "Header"_hash)) {
			if (header->value.Is<str>()) {
				AddInspectorHeader(context, parent, header->value.As<str>(), TextStrong());
			}
		}

		RowOverride mark = OverrideMarkFor(target, field.name);
		RowValue value;
		value.target = target;
		value.field = field.name;
		const RowValue* resettable = (field.flags & FieldFlagReadOnly) == 0 ? &value : nullptr;

		const PropertyDrawerFn* custom = CustomDrawerFor(field);

		const bool structList = (field.flags & FieldFlagContainer) != 0 && field.container &&
								(field.container->elementFlags & FieldFlagSubobject) != 0 && field.container->elementType;
		const bool subobject = (field.flags & FieldFlagSubobject) != 0 && field.fieldType;
		if (!custom && (structList || subobject)) {
			NestedPath path;
			path.target = target;
			path.top = RefTo(field);
			const u64 key = NestedFoldKey(path);
			RowFold fold;
			fold.collapsed = IsSectionCollapsed(key);
			fold.onToggle = [this, key]() { ToggleSection(key); };
			UI::ElementHandle header = MakeRow(context, parent, FieldLabel(field), &mark, &fold, resettable);
			if (const AttributeInfo* tooltip = FindFieldAttribute(&field, "Tooltip"_hash); tooltip && tooltip->value.Is<str>()) {
				context.SetTooltip(header, tooltip->value.As<str>());
			}
			if (structList) {
				BuildNestedList(context, header, fold.collapsed ? UI::ElementHandle{} : parent, path, *field.container, 0);
			} else if (!fold.collapsed) {
				BuildNestedStructRows(context, parent, path, field.fieldType, 0);
			}
			return;
		}

		UI::ElementHandle row = MakeRow(context, parent, FieldLabel(field), &mark, nullptr, resettable);

		if (const AttributeInfo* tooltip = FindFieldAttribute(&field, "Tooltip"_hash)) {
			if (tooltip->value.Is<str>()) {
				context.SetTooltip(row, tooltip->value.As<str>());
			}
		}

		if (custom) {
			_customRows.push_back(row);
			(*custom)(context, row, RefTo(field), target);
			return;
		}

		bool readOnly = (field.flags & FieldFlagReadOnly) != 0;
		const TypeInfo* fieldType = field.fieldType;
		StringHash fieldName = field.name;
		FieldRef ref = RefTo(field);
		Object* object = target.Resolve();
		const TypeInfo* ownerType = object ? object->GetType() : nullptr;

		if ((field.flags & FieldFlagContainer) != 0 && field.container &&
			(field.container->elementFlags & FieldFlagHandle) != 0 && field.container->elementType) {
			BuildHandleListRows(context, parent, field, target);
			return;
		}

		if (!object || !fieldType) {
			return;
		}

		auto pushBinding = [this](Binding binding) {
			_bindings.push_back(std::move(binding));
		};

		if (readOnly || PropertyRows::IsScalarField(field)) {
			PropertyRows::ScalarRow scalar = PropertyRows::Build(context, row, ref, MakeObjectAccess(target, ref));
			if (scalar.kind != PropertyRows::ScalarRow::Kind::None) {
				Binding binding;
				binding.kind = Binding::Kind::Scalar;
				binding.target = target;
				binding.field = ref;
				binding.scalar = std::move(scalar);
				pushBinding(std::move(binding));
				return;
			}
		}

		auto vectorField = [&](u32 count, auto read, auto write) {
			f64 values[4] = {};
			read(values);
			UI::Widgets::VectorFieldDesc desc;
			desc.componentCount = count;
			for (u32 i = 0; i < count; ++i) {
				desc.values[i] = values[i];
			}
			desc.font = font;
			desc.fontSizePx = FieldFont();
			desc.onEditBegin = []() { UndoSystem::BeginGroup("Edit Property"); };
			desc.onEditEnd = []() { UndoSystem::EndGroup(); };
			desc.onChange = write;
			UI::Widgets::VectorFieldParts parts = UI::Widgets::VectorField(context, row, desc);
			Binding binding;
			binding.kind = Binding::Kind::VectorField;
			binding.target = target;
			binding.field = ref;
			binding.vector = parts;
			binding.componentCount = count;
			pushBinding(std::move(binding));
		};

		if (NameIs(fieldType, "Vec2") || NameIs(fieldType, "Vec3") || NameIs(fieldType, "Vec4")) {
			u32 count = NameIs(fieldType, "Vec2") ? 2u : NameIs(fieldType, "Vec3") ? 3u
																				   : 4u;
			vectorField(
				count,
				[&](f64* out) {
					Variant current;
					if (!GetField(object, ownerType, fieldName, &current)) {
						return;
					}
					if (count == 2 && current.Is<Vec2>()) {
						Vec2 v = current.As<Vec2>();
						out[0] = v.x;
						out[1] = v.y;
					}
					if (count == 3 && current.Is<Vec3>()) {
						Vec3 v = current.As<Vec3>();
						out[0] = v.x;
						out[1] = v.y;
						out[2] = v.z;
					}
					if (count == 4 && current.Is<Vec4>()) {
						Vec4 v = current.As<Vec4>();
						out[0] = v.x;
						out[1] = v.y;
						out[2] = v.z;
						out[3] = v.w;
					}
				},
				[target, fieldName, count](u32 component, f64 next) {
					Object* live = target.Resolve();
					if (!live) {
						return;
					}
					Variant current;
					if (!GetField(live, live->GetType(), fieldName, &current)) {
						return;
					}
					if (count == 2 && current.Is<Vec2>()) {
						Vec2 before = current.As<Vec2>();
						Vec2 after = before;
						(&after.x)[component] = static_cast<f32>(next);
						UndoSystem::Push(MakeUnique<PropertySetCommand>(target, fieldName, Variant(before), Variant(after)));
					} else if (count == 3 && current.Is<Vec3>()) {
						Vec3 before = current.As<Vec3>();
						Vec3 after = before;
						after[component] = static_cast<f32>(next);
						UndoSystem::Push(MakeUnique<PropertySetCommand>(target, fieldName, Variant(before), Variant(after)));
					} else if (count == 4 && current.Is<Vec4>()) {
						Vec4 before = current.As<Vec4>();
						Vec4 after = before;
						(&after.x)[component] = static_cast<f32>(next);
						UndoSystem::Push(MakeUnique<PropertySetCommand>(target, fieldName, Variant(before), Variant(after)));
					}
				});
			return;
		}

		if (NameIs(fieldType, "Quat")) {
			vectorField(
				3,
				[&](f64* out) {
					Variant current;
					if (!GetField(object, ownerType, fieldName, &current) || !current.Is<Quat>()) {
						return;
					}
					Vec3 angles = EditorSceneOps::EulerForDisplay(current.As<Quat>());
					out[0] = angles.x;
					out[1] = angles.y;
					out[2] = angles.z;
				},
				[target, fieldName](u32 component, f64 next) {
					Object* live = target.Resolve();
					if (!live) {
						return;
					}
					Variant current;
					if (!GetField(live, live->GetType(), fieldName, &current) || !current.Is<Quat>()) {
						return;
					}
					const Quat before = current.As<Quat>();
					Vec3 angles = EditorSceneOps::EulerForDisplay(before);
					angles[component] = static_cast<f32>(next);
					UndoSystem::Push(MakeUnique<PropertySetCommand>(target, fieldName, Variant(before),
																	Variant(Quat::FromEuler(angles))));
				});
			return;
		}

		if (NameIs(fieldType, "Vec2I") || NameIs(fieldType, "Vec3I") || NameIs(fieldType, "Vec4I")) {
			u32 count = NameIs(fieldType, "Vec2I") ? 2u : NameIs(fieldType, "Vec3I") ? 3u
																					 : 4u;
			vectorField(
				count,
				[&](f64* out) {
					Variant current;
					if (!GetField(object, ownerType, fieldName, &current)) {
						return;
					}
					if (count == 2 && current.Is<Vec2I>()) {
						Vec2I v = current.As<Vec2I>();
						out[0] = v.x;
						out[1] = v.y;
					} else if (count == 3 && current.Is<Vec3I>()) {
						Vec3I v = current.As<Vec3I>();
						out[0] = v.x;
						out[1] = v.y;
						out[2] = v.z;
					} else if (count == 4 && current.Is<Vec4I>()) {
						Vec4I v = current.As<Vec4I>();
						out[0] = v.x;
						out[1] = v.y;
						out[2] = v.z;
						out[3] = v.w;
					}
				},
				[target, fieldName, count](u32 component, f64 next) {
					Object* live = target.Resolve();
					if (!live) {
						return;
					}
					Variant current;
					if (!GetField(live, live->GetType(), fieldName, &current)) {
						return;
					}
					const i32 rounded = static_cast<i32>(next < 0.0 ? next - 0.5 : next + 0.5);
					if (count == 2 && current.Is<Vec2I>()) {
						Vec2I before = current.As<Vec2I>();
						Vec2I after = before;
						(&after.x)[component] = rounded;
						UndoSystem::Push(MakeUnique<PropertySetCommand>(target, fieldName, Variant(before), Variant(after)));
					} else if (count == 3 && current.Is<Vec3I>()) {
						Vec3I before = current.As<Vec3I>();
						Vec3I after = before;
						(&after.x)[component] = rounded;
						UndoSystem::Push(MakeUnique<PropertySetCommand>(target, fieldName, Variant(before), Variant(after)));
					} else if (count == 4 && current.Is<Vec4I>()) {
						Vec4I before = current.As<Vec4I>();
						Vec4I after = before;
						(&after.x)[component] = rounded;
						UndoSystem::Push(MakeUnique<PropertySetCommand>(target, fieldName, Variant(before), Variant(after)));
					}
				});
			return;
		}

		if (NameIs(fieldType, "Color")) {
			Variant current;
			Color value = GetField(object, ownerType, fieldName, &current) && current.Is<Color>() ? current.As<Color>() : Color::White();
			UI::Widgets::ColorFieldParts parts = UI::Widgets::ColorSwatchField(
				context, row, value, font, FieldFont(),
				[target, fieldName](Color next) {
					Object* live = target.Resolve();
					if (!live) {
						return;
					}
					Variant before;
					GetField(live, live->GetType(), fieldName, &before);
					UndoSystem::Push(MakeUnique<PropertySetCommand>(
						target, fieldName, before, Variant(next)));
				},
				[]() { UndoSystem::BeginGroup("Edit Color"); },
				[]() { UndoSystem::EndGroup(); });
			Binding binding;
			binding.kind = Binding::Kind::ColorField;
			binding.target = target;
			binding.field = ref;
			binding.color = parts;
			pushBinding(std::move(binding));
			return;
		}

		if ((field.flags & FieldFlagHandle) != 0) {
			const TypeInfo* pointee = TypeRegistry::FindType(HandlePointeeName(*fieldType));
			BuildHandleRow(context, row, target, fieldName,
						   pointee ? pointee->name : StringHash{}, FieldLabel(field));
			return;
		}

		if ((field.flags & FieldFlagEnum) != 0 && (field.flags & FieldFlagTriviallyCopyable) != 0 && field.size <= 8) {
			u64 bits = ReadRawBits(target, field);
			UI::Widgets::NumberFieldDesc number;
			number.value = static_cast<f64>(bits);
			number.decimals = 0;
			number.font = font;
			number.fontSizePx = FieldFont();
			u32 size = static_cast<u32>(field.size);
			number.onChange = [target, ref, size](f64 next) mutable {
				const FieldInfo* liveField = ref.Resolve();
				if (!liveField) {
					return;
				}
				u64 before = ReadRawBits(target, *liveField);
				u64 after = static_cast<u64>(next < 0 ? 0 : next);
				if (before != after) {
					UndoSystem::Push(MakeUnique<SetRawFieldCommand>(target, ref.field, before, after, size));
				}
			};
			UI::Widgets::NumberFieldParts parts = UI::Widgets::NumberField(context, row, number);
			Binding binding;
			binding.kind = Binding::Kind::RawNumber;
			binding.target = target;
			binding.field = ref;
			binding.number = parts;
			pushBinding(std::move(binding));
			return;
		}

		UI::ElementHandle unsupported = context.CreateElement();
		context.AppendChild(row, unsupported);
		context.SetText(unsupported, font, FieldFont(), TextDim(), "(" + TypeLabel(fieldType) + ", no drawer)");
	}

	void InspectorPanel::BuildProductInspector(UI::Context& context, const Guid& guid, const Guid& sourceGuid) {
		Handle<FontAsset> font = UISystem::GetDefaultFont();
		auto line = [&](std::string_view label, str value) {
			UI::ElementHandle row = MakeRow(context, _viewport, label);
			UI::ElementHandle text = context.CreateElement();
			context.AppendChild(row, text);
			context.SetText(text, font, FieldFont(), TextDim(), value);
		};

		Path sourcePath = AssetDatabase::LookupPath(sourceGuid);
		Vector<AssetProduct> products{GetMemoryResource(MemTag::Editor)};
		AssetDatabase::GetProducts(sourceGuid, products);
		const AssetProduct* self = nullptr;
		for (const AssetProduct& product : products) {
			if (product.guid == guid) {
				self = &product;
				break;
			}
		}

		line("Source", sourcePath.ToString().empty() ? str("(not in database)") : sourcePath.ToString());
		line("Key", self ? self->key : str("(not in the source's table)"));
		line("Guid", std::format("{:016x}{:016x}", guid.high, guid.low));

		const TypeInfo* producedType = self ? TypeRegistry::FindType(self->type) : nullptr;
		const AssetInspectorFn* custom = producedType ? PropertyDrawers::FindAssetInspector(self->type) : nullptr;
		if (!custom) {
			line("Type", producedType ? TypeLabel(producedType) : str("(unknown)"));
		}

		if (self && !self->extractedPath.ToString().empty()) {
			line("Extracted to", self->extractedPath.ToString());
		}

		if (!custom && !HasDrawableFields(producedType)) {
			return;
		}
		Asset* asset = AssetDatabase::LoadAssetByGuid(guid);
		if (!asset) {
			line("State", std::format("no longer in '{}'", Path(sourcePath.GetFilename()).ToString()));
			return;
		}
		_drawnAsset = Handle<Asset>(asset);
		if (!custom) {
			BuildAssetFallbackBody(context, *asset);
			return;
		}
		InspectorPanel* previous = _buildingPanel;
		_buildingPanel = this;
		(*custom)(context, _viewport, guid, *asset);
		_buildingPanel = previous;
	}

	void InspectorPanel::BuildAssetInspector(UI::Context& context, const Guid& guid) {
		u32 assetCount = EditorSelection::GetAssetSelectionCount();
		if (assetCount > 1 && !locked) {
			AddInspectorHeader(context, _viewport, std::format("{} assets selected (showing active)", assetCount), TextNormal());
		}
		AddInspectorHeader(context, _viewport, "Asset", TextStrong());
		Handle<FontAsset> font = UISystem::GetDefaultFont();
		auto line = [&](std::string_view label, str value) {
			UI::ElementHandle row = MakeRow(context, _viewport, label);
			UI::ElementHandle text = context.CreateElement();
			context.AppendChild(row, text);
			context.SetText(text, font, FieldFont(), TextDim(), value);
		};
		Path path = AssetDatabase::LookupPath(guid);

		Guid productSource = AssetDatabase::GetProductSource(guid);
		if (productSource.IsValid()) {
			BuildProductInspector(context, guid, productSource);
			return;
		}

		line("Path", path.ToString().empty() ? str("(not in database)") : path.ToString());
		line("Guid", std::format("{:016x}{:016x}", guid.high, guid.low));

		str extension = Path(path.GetFilename()).GetExtension();
		IAssetImporter* importer = extension.empty() ? nullptr : AssetImporterRegistry::FindByExtension(extension);
		if (!importer) {
			line("Type", "(no importer for '" + extension + "')");
			return;
		}
		const AssetInspectorFn* custom = PropertyDrawers::FindAssetInspector(importer->GetAssetType());
		const TypeInfo* producedType = TypeRegistry::FindType(importer->GetAssetType());
		if (!custom) {
			line("Type", producedType ? TypeLabel(producedType) : std::format("{:#x}", importer->GetAssetType().GetValue()));
		}
		line("Extension", extension);

		if (!custom && !HasDrawableFields(producedType)) {
			return;
		}
		Asset* asset = AssetDatabase::LoadAssetByGuid(guid);
		if (!asset) {
			return;
		}
		_drawnAsset = Handle<Asset>(asset);
		if (!custom) {
			BuildAssetFallbackBody(context, *asset);
			BuildAssetSections(context, guid, *asset, importer->GetAssetType());
			return;
		}
		InspectorPanel* previous = _buildingPanel;
		_buildingPanel = this;
		(*custom)(context, _viewport, guid, *asset);
		_buildingPanel = previous;
		BuildAssetSections(context, guid, *asset, importer->GetAssetType());
	}

	void InspectorPanel::BuildAssetSections(UI::Context& context, const Guid& guid, Asset& asset, StringHash assetType) {
		Vector<AssetInspectorSection> sections{GetMemoryResource(MemTag::Editor)};
		PropertyDrawers::GetAssetInspectorSections(assetType, sections);
		if (sections.empty()) {
			return;
		}

		Vector<str> labels{GetMemoryResource(MemTag::Editor)};
		for (const AssetInspectorSection& section : sections) {
			if (section.applies && !section.applies(guid, asset)) {
				continue;
			}
			if (std::find(labels.begin(), labels.end(), section.label) == labels.end()) {
				labels.push_back(section.label);
			}
		}
		if (labels.empty()) {
			return;
		}

		if (std::find(labels.begin(), labels.end(), _assetTab) == labels.end()) {
			_assetTab = labels.front();
		}

		Handle<FontAsset> font = UISystem::GetDefaultFont();

		UI::ElementHandle strip = context.CreateElement();
		context.AppendChild(_viewport, strip);
		{
			UI::Style style;
			style.flexDirection = UI::FlexDirection::Row;
			style.width = UI::Length::Percent(1.0f);
			style.margin = {8.0f, 6.0f, 0.0f, 2.0f};
			context.SetStyle(strip, style);
		}

		for (const str& label : labels) {
			const bool on = label == _assetTab;
			UI::Widgets::ButtonDesc desc;
			desc.label = label;
			desc.font = font;
			desc.fontSizePx = EditorTheme::GetMetric(EditorTheme::MetricToken::FontBody);
			desc.onClick = [this, label]() {
				_assetTab = label;
				RequestRebuild();
			};
			UI::ElementHandle button = UI::Widgets::Button(context, strip, desc);
			context.AddClass(button, EditorTheme::Class::ToolButton);
			if (on) {
				context.AddClass(button, EditorTheme::Class::ToolButtonOn);
			}
		}

		UI::ElementHandle body = context.CreateElement();
		context.AppendChild(_viewport, body);
		{
			UI::Style style;
			style.flexDirection = UI::FlexDirection::Column;
			style.width = UI::Length::Percent(1.0f);
			context.SetStyle(body, style);
		}

		InspectorPanel* previous = _buildingPanel;
		_buildingPanel = this;
		for (const AssetInspectorSection& section : sections) {
			if (section.label != _assetTab) {
				continue;
			}
			if (section.applies && !section.applies(guid, asset)) {
				continue;
			}
			section.draw(context, body, guid, asset);
		}
		_buildingPanel = previous;
	}

	void InspectorPanel::BuildAssetFallbackBody(UI::Context& context, Asset& asset) {
		const TypeInfo* type = asset.GetType();
		Handle<FontAsset> font = UISystem::GetDefaultFont();

		Vector<const FieldInfo*> fields{GetMemoryResource(MemTag::Editor)};
		ForEachField(
			&asset, type,
			[&fields](const FieldInfo* field, const Variant*) {
				if (!IsAssetIdentityField(field)) {
					fields.push_back(field);
				}
			},
			FieldWalkFlagIncludeInherited | FieldWalkFlagExcludeHidden);

		if (fields.empty()) {
			return;
		}

		UI::ElementHandle card = context.CreateElement();
		context.AppendChild(_viewport, card);
		UI::Style cardStyle;
		cardStyle.flexDirection = UI::FlexDirection::Column;
		cardStyle.width = UI::Length::Percent(1.0f);
		cardStyle.margin = {0.0f, 4.0f, 0.0f, 0.0f};
		context.SetStyle(card, cardStyle);
		context.AddClass(card, EditorTheme::Class::SectionCard);
		AddInspectorHeader(context, card, TypeLabel(type), TextStrong());

		EditTarget target = Handle<Object>(&asset);
		for (const FieldInfo* field : fields) {
			BuildFieldRow(context, card, *field, target);
		}

		Handle<Asset> subject(&asset);
		UI::ElementHandle actions = MakeRow(context, card, "");
		UI::Widgets::ButtonDesc save;
		save.label = "Save";
		save.font = font;
		save.fontSizePx = FieldFont();
		save.onClick = [subject]() {
			if (!subject.Get()) {
				SLT_LOG_WARN("Editor", "Inspector: the asset being saved no longer exists");
				return;
			}
			EditorAssetOps::SaveAsset(subject);
		};
		UI::Widgets::Button(context, actions, save);
	}

	void InspectorPanel::ReleaseDrawnAsset() {
		if (Asset* drawn = _drawnAsset.Get()) {
			AssetDatabase::UnloadAsset(drawn);
		}
		_drawnAsset = {};
	}

	void InspectorPanel::OpenAddComponentPopup(UI::Context& context, Handle<SceneObject> objectHandle) {
		Handle<FontAsset> font = UISystem::GetDefaultFont();
		UI::Context* contextPtr = &context;

		UI::ElementHandle card = context.CreateElement();
		UI::Style cardStyle;
		cardStyle.minWidth = UI::Length::Pixel(240.0f);
		context.SetStyle(card, cardStyle);
		context.AddClass(card, EditorTheme::Class::PopupCardCompact);
		context.Events(card).onPopupClosed = [contextPtr, card]() {
			contextPtr->DestroyElementNextUpdate(card);
		};

		auto listHost = std::make_shared<UI::ElementHandle>();
		auto listContainer = std::make_shared<UI::ElementHandle>();
		auto openCategory = std::make_shared<str>();
		auto rebuild = std::make_shared<std::function<void(std::string_view)>>();
		auto rebuildList = [contextPtr, listHost, listContainer, objectHandle, font, card, openCategory,
							rebuild](std::string_view filter) {
			UI::Context& ctx = *contextPtr;
			if (!ctx.IsAlive(*listHost)) {
				return;
			}
			if (ctx.IsAlive(*listContainer)) {
				UI::Style gone;
				gone.display = UI::Display::None;
				ctx.SetStyle(*listContainer, gone);
				ctx.DestroyElementNextUpdate(*listContainer);
			}
			*listContainer = ctx.CreateElement();
			ctx.AppendChild(*listHost, *listContainer);
			UI::Style listStyle;
			listStyle.flexDirection = UI::FlexDirection::Column;
			ctx.SetStyle(*listContainer, listStyle);
			std::string_view category = filter.empty() ? std::string_view(*openCategory) : std::string_view{};

			Vector<const TypeInfo*> all{GetMemoryResource(MemTag::Editor)};
			TypeRegistry::ForEachType(
				[](const TypeInfo* type, void* user) {
					static_cast<Vector<const TypeInfo*>*>(user)->push_back(type);
				},
				&all);
			auto matches = [&filter](const TypeInfo* type) {
				if ((type->flags & TypeFlagComponent) == 0 || (type->flags & TypeFlagAbstract) != 0 || !type->createFn) {
					return false;
				}
				if (filter.empty()) {
					return true;
				}
				str name = TypeLabel(type);
				auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
				str haystack = name;
				str needle{filter};
				std::transform(haystack.begin(), haystack.end(), haystack.begin(), lower);
				std::transform(needle.begin(), needle.end(), needle.begin(), lower);
				return haystack.find(needle) != str::npos;
			};

			GameModuleHost* host = GameModuleHost::GetActive();
			const void* moduleBase = host ? host->GetModuleBase() : nullptr;

			Vector<const TypeInfo*> userTypes{GetMemoryResource(MemTag::Editor)};
			Vector<const TypeInfo*> engineTypes{GetMemoryResource(MemTag::Editor)};
			Vector<std::pair<str, Vector<const TypeInfo*>>> engineGroups{GetMemoryResource(MemTag::Editor)};
			for (const TypeInfo* type : all) {
				if (!matches(type)) {
					continue;
				}
				if (moduleBase && type->ownerModule == moduleBase) {
					userTypes.push_back(type);
					continue;
				}
				std::string_view group = ComponentMenu::CategoryFor(type);
				if (!category.empty()) {
					if (category == group) {
						engineTypes.push_back(type);
					}
					continue;
				}
				if (!filter.empty() || group.empty()) {
					engineTypes.push_back(type);
					continue;
				}
				auto found = std::find_if(engineGroups.begin(), engineGroups.end(),
										  [group](const auto& entry) { return entry.first == group; });
				if (found == engineGroups.end()) {
					engineGroups.push_back({str(group), Vector<const TypeInfo*>{GetMemoryResource(MemTag::Editor)}});
					found = engineGroups.end() - 1;
				}
				found->second.push_back(type);
			}
			auto byLabel = [](const TypeInfo* a, const TypeInfo* b) { return TypeLabel(a) < TypeLabel(b); };
			std::sort(userTypes.begin(), userTypes.end(), byLabel);
			std::sort(engineTypes.begin(), engineTypes.end(), byLabel);
			std::sort(engineGroups.begin(), engineGroups.end(),
					  [](const auto& a, const auto& b) { return a.first < b.first; });

			u32 shown = 0;
			u32 withheld = 0;
			u32 groupRows = 0;
			auto addHeading = [&](std::string_view text) {
				UI::ElementHandle heading = ctx.CreateElement();
				ctx.AppendChild(*listContainer, heading);
				UI::Style style;
				style.margin = {0.0f, 4.0f, 0.0f, 2.0f};
				ctx.SetStyle(heading, style);
				ctx.SetText(heading, font, FieldFont(), TextDim(), text);
				ctx.SetHoverable(heading, false);
			};
			auto addGroup = [&](const Vector<const TypeInfo*>& types, std::string_view heading) {
				if (types.empty()) {
					return;
				}
				if (!heading.empty()) {
					addHeading(heading);
				}
				for (const TypeInfo* type : types) {
					if (shown >= ADD_COMPONENT_ROW_CAP) {
						++withheld;
						continue;
					}
					++shown;
					UI::ElementHandle rowElement = ctx.CreateElement();
					ctx.AddClass(rowElement, StringHash("slt-tree-row"));
					ctx.AppendChild(*listContainer, rowElement);
					EditorIcons::ComponentIcons::Add(ctx, rowElement, type, 0.0f, 0.0f, FieldFont());
					UI::ElementHandle rowLabel = ctx.CreateElement();
					ctx.AppendChild(rowElement, rowLabel);
					ctx.SetHoverable(rowLabel, false);
					ctx.SetText(rowLabel, font, FieldFont(), TextNormal(), TypeLabel(type));
					StringHash typeName = type->name;
					ctx.Events(rowElement).onClick = [contextPtr, card, objectHandle, typeName]() {
						contextPtr->ClosePopup(card);
						if (objectHandle.IsValid()) {
							UndoSystem::Push(MakeUnique<AddComponentCommand>(objectHandle, typeName));
						}
					};
				}
			};

			str projectPath = ProjectOps::GetMountedProjectPath();
			str userHeading = "This project";
			if (!projectPath.empty()) {
				str moduleName = ScriptProjectOps::GetModuleName(projectPath);
				if (!moduleName.empty()) {
					userHeading = moduleName;
				}
			}

			auto addNavigationRow = [&](std::string_view label, str destination) {
				UI::ElementHandle rowElement = ctx.CreateElement();
				ctx.AddClass(rowElement, StringHash("slt-tree-row"));
				ctx.AppendChild(*listContainer, rowElement);
				UI::ElementHandle rowLabel = ctx.CreateElement();
				ctx.AppendChild(rowElement, rowLabel);
				ctx.SetHoverable(rowLabel, false);
				ctx.SetText(rowLabel, font, FieldFont(), TextNormal(), label);
				ctx.Events(rowElement).onClick = [openCategory, rebuild, destination]() {
					*openCategory = destination;
					if (*rebuild) {
						(*rebuild)({});
					}
				};
			};

			if (!category.empty()) {
				addNavigationRow(str("\xE2\x80\xB9  ") + str(category), str{});
				addGroup(engineTypes, {});
			} else {
				addGroup(userTypes, userHeading);
				if (!engineGroups.empty() || !engineTypes.empty()) {
					addHeading("Engine");
				}
				for (const auto& group : engineGroups) {
					addNavigationRow(group.first + "  \xE2\x80\xBA", group.first);
					++groupRows;
				}
				addGroup(engineTypes, {});
			}

			if (withheld > 0) {
				UI::ElementHandle more = ctx.CreateElement();
				ctx.AppendChild(*listContainer, more);
				ctx.SetText(more, font, FieldFont(), TextDim(),
							std::format("... {} more (keep typing to narrow)", withheld));
				ctx.SetHoverable(more, false);
			}

			if (shown == 0 && groupRows == 0) {
				str message = filter.empty() ? str("(no components available)") : str("(no matching components)");
				bool focusConsole = false;
				CompileService* compile = CompileService::GetActive();
				switch (ScriptProjectOps::GetModuleStatus(projectPath)) {
				case ScriptProjectOps::ModuleStatus::NoScriptProject:
					message = "this project has no C++ module. Scripts > Create C++ Module...";
					break;
				case ScriptProjectOps::ModuleStatus::Compiling:
					message = "compiling. Your components will appear when the compile finishes";
					break;
				case ScriptProjectOps::ModuleStatus::CompileFailed:
					message = std::format("the last compile failed ({} error(s)). Open the Console",
										  compile ? compile->GetErrorCount() : 0);
					focusConsole = true;
					break;
				case ScriptProjectOps::ModuleStatus::LoadFailed:
					message = "the module compiled but failed to load. See the Console";
					focusConsole = true;
					break;
				case ScriptProjectOps::ModuleStatus::Loaded:
					break;
				}
				UI::ElementHandle none = ctx.CreateElement();
				ctx.AppendChild(*listContainer, none);
				if (focusConsole) {
					ctx.AddClass(none, StringHash("slt-tree-row"));
					ctx.Events(none).onClick = [contextPtr, card]() {
						contextPtr->ClosePopup(card);
						PanelManager::FocusPanelOfType(StringHash("ConsolePanel"));
					};
				} else {
					ctx.SetHoverable(none, false);
				}
				ctx.SetText(none, font, FieldFont(), TextDim(), message);
			}
		};

		*rebuild = rebuildList;

		UI::Widgets::TextFieldDesc search;
		search.font = font;
		search.fontSizePx = FieldFont();
		UI::ElementHandle searchField = UI::Widgets::TextField(context, card, search);
		context.Events(searchField).onTextChange = [rebuildList](std::string_view text) {
			rebuildList(text);
		};

		*listHost = context.CreateElement();
		context.AppendChild(card, *listHost);
		UI::Style hostStyle;
		hostStyle.flexDirection = UI::FlexDirection::Column;
		hostStyle.margin = {0.0f, 6.0f, 0.0f, 0.0f};
		context.SetStyle(*listHost, hostStyle);
		rebuildList({});

		{
			if (ScriptProjectOps::HasScriptProject(ProjectOps::GetMountedProjectPath())) {
				UI::Widgets::Separator(context, card);

				UI::ElementHandle create = context.CreateElement();
				context.AddClass(create, StringHash("slt-tree-row"));
				context.AppendChild(card, create);
				context.SetText(create, font, FieldFont(), TextDim(), "New C++ Component...");
				context.Events(create).onClick = [contextPtr, card]() {
					contextPtr->ClosePopup(card);
					ScriptFlows::OpenCreateComponentPopup(*contextPtr);
				};

				UI::ElementHandle createCs = context.CreateElement();
				context.AddClass(createCs, StringHash("slt-tree-row"));
				context.AppendChild(card, createCs);
				context.SetText(createCs, font, FieldFont(), TextDim(), "New C# Component...");
				context.Events(createCs).onClick = [contextPtr, card]() {
					contextPtr->ClosePopup(card);
					ScriptFlows::OpenCreateCSharpComponentPopup(*contextPtr);
				};
			}
		}

		context.OpenPopup(card, context.GetPointerPosition());
		context.SetFocus(searchField);
	}

	void InspectorPanel::OpenHandlePickerPopup(UI::Context& context, HandleSlot slot, StringHash pointeeName, str label,
											   std::function<void(u64)> onAssigned) {
		const TypeInfo* pointee = TypeRegistry::FindType(pointeeName);
		Handle<FontAsset> font = UISystem::GetDefaultFont();
		UI::Context* contextPtr = &context;

		struct Candidate {
			str display;
			u64 instanceId = 0;
			Guid assetGuid{};
		};
		Vector<Candidate> candidates{GetMemoryResource(MemTag::Editor)};

		if (IsAssetPointee(pointee)) {
			Vector<AssetBrowserOps::AssignableAsset> assignable{GetMemoryResource(MemTag::Editor)};
			AssetBrowserOps::CollectAssignableAssets(pointeeName, assignable);
			for (AssetBrowserOps::AssignableAsset& asset : assignable) {
				candidates.push_back({std::move(asset.display), 0, asset.guid});
			}
		} else if (pointee) {
			struct WalkState {
				const TypeInfo* pointee;
				Vector<Candidate>* out;
			} state{pointee, &candidates};
			for (const Handle<Scene>& scene : SceneSystem::GetLoadedScenes()) {
				Scene* live = scene.Get();
				if (!live) {
					continue;
				}
				live->ForEachObject(
					[](SceneObject& object, void* user) {
						WalkState& walk = *static_cast<WalkState*>(user);
						if (TypeRegistry::IsDerivedFrom(object.GetType(), walk.pointee)) {
							walk.out->push_back({object.name, object.GetInstanceId(), {}});
						}
						for (u32 i = 0; i < object.GetComponentCount(); ++i) {
							Component* component = object.GetComponentAt(i).Get();
							if (component && TypeRegistry::IsDerivedFrom(component->GetType(), walk.pointee)) {
								walk.out->push_back({object.name + " / " + TypeLabel(component->GetType()),
													 component->GetInstanceId(),
													 {}});
							}
						}
					},
					&state);
			}
		}

		UI::ElementHandle card = context.CreateElement();
		UI::Style cardStyle;
		cardStyle.minWidth = UI::Length::Pixel(240.0f);
		context.SetStyle(card, cardStyle);
		context.AddClass(card, EditorTheme::Class::PopupCardCompact);
		context.Events(card).onPopupClosed = [contextPtr, card]() {
			contextPtr->DestroyElementNextUpdate(card);
		};

		UI::ElementHandle title = context.CreateElement();
		context.AppendChild(card, title);
		context.SetText(title, font, FieldFont(), TextDim(),
						pointee ? "Select " + TypeLabel(pointee) : str("Select"));

		auto assign = [contextPtr, card, slot, label, onAssigned](u64 instanceId) {
			contextPtr->ClosePopup(card);
			if (!slot.target.Resolve()) {
				return;
			}
			u64 before = ReadSlot(slot);
			str writeLabel = instanceId == 0 ? "Clear " + label : "Set " + label;
			bool grouped = static_cast<bool>(onAssigned) && before != instanceId;
			if (grouped) {
				UndoSystem::BeginGroup(writeLabel);
			}
			WriteSlot(slot, before, instanceId, writeLabel);
			if (grouped) {
				if (instanceId != 0) {
					onAssigned(instanceId);
				}
				UndoSystem::EndGroup();
			}
		};

		auto addRow = [&](const str& text, std::function<void()> onClick) {
			UI::ElementHandle rowElement = context.CreateElement();
			context.AddClass(rowElement, StringHash("slt-tree-row"));
			context.AppendChild(card, rowElement);
			context.SetText(rowElement, font, FieldFont(), TextNormal(), text);
			context.Events(rowElement).onClick = std::move(onClick);
		};

		addRow("None", [assign]() { assign(0); });

		u32 shown = 0;
		for (const Candidate& candidate : candidates) {
			if (shown >= 40) {
				break;
			}
			++shown;
			if (candidate.assetGuid.IsValid()) {
				addRow(candidate.display, [assign, guid = candidate.assetGuid]() {
					Asset* asset = AssetDatabase::LoadAssetByGuid(guid);
					assign(asset ? asset->GetInstanceId() : 0);
				});
			} else {
				addRow(candidate.display, [assign, id = candidate.instanceId]() { assign(id); });
			}
		}
		if (shown == 0) {
			UI::ElementHandle none = context.CreateElement();
			context.AppendChild(card, none);
			context.SetText(none, font, FieldFont(), TextDim(), "(no candidates)");
		}
		if (candidates.size() > shown) {
			UI::ElementHandle more = context.CreateElement();
			context.AppendChild(card, more);
			context.SetText(more, font, FieldFont(), TextDim(),
							std::format("... {} more", candidates.size() - shown));
		}

		context.OpenPopup(card, context.GetPointerPosition());
	}

	void InspectorPanel::RefreshValues(UI::Context& context) {
		PropertyDrawers::RefreshRows(context);

		UI::ElementHandle focused = context.GetFocused();
		for (Binding& binding : _bindings) {
			Object* object = binding.target.Resolve();
			if (!object) {
				continue;
			}
			const TypeInfo* type = object->GetType();
			switch (binding.kind) {
			case Binding::Kind::Scalar:
				PropertyRows::Refresh(context, binding.scalar);
				break;
			case Binding::Kind::RawNumber: {
				if (focused == binding.number.field) {
					break;
				}
				const FieldInfo* field = binding.field.Resolve();
				if (field) {
					UI::Widgets::SetNumberFieldValue(context, binding.number,
													 static_cast<f64>(ReadRawBits(binding.target, *field)), 0);
				}
				break;
			}
			case Binding::Kind::NumberElement: {
				if (focused == binding.number.field) {
					break;
				}
				const FieldInfo* field = binding.field.Resolve();
				if (field) {
					UI::Widgets::SetNumberFieldValue(
						context, binding.number,
						static_cast<f64>(ReadElementFloat(binding.target, *field, binding.elementIndex)), 2);
				}
				break;
			}
			case Binding::Kind::EnabledBox: {
				if (TypeRegistry::IsDerivedFrom(type, Component::GetReflectType())) {
					UI::Widgets::SetToggleValue(context, binding.toggle,
												static_cast<Component*>(object)->IsEnabledSelf());
				}
				break;
			}
			case Binding::Kind::ActiveBox: {
				if (TypeRegistry::IsDerivedFrom(type, SceneObject::GetReflectType())) {
					UI::Widgets::SetToggleValue(context, binding.toggle,
												static_cast<SceneObject*>(object)->IsActiveSelf());
				}
				break;
			}
			case Binding::Kind::TextField: {
				if (focused == binding.element) {
					break;
				}
				Variant current;
				if (GetField(object, type, binding.field.field, &current) && current.Is<str>()) {
					if (context.GetTextString(binding.element) != current.As<str>()) {
						context.SetTextString(binding.element, current.As<str>());
					}
				}
				break;
			}
			case Binding::Kind::VectorField: {
				bool editing = false;
				for (u32 i = 0; i < binding.componentCount; ++i) {
					editing |= focused == binding.vector.components[i].field;
				}
				if (editing) {
					break;
				}
				Variant current;
				if (!GetField(object, type, binding.field.field, &current)) {
					break;
				}
				f64 values[4] = {};
				bool valid = false;
				if (binding.componentCount == 2 && current.Is<Vec2>()) {
					Vec2 v = current.As<Vec2>();
					values[0] = v.x;
					values[1] = v.y;
					valid = true;
				} else if (binding.componentCount == 3 && current.Is<Vec3>()) {
					Vec3 v = current.As<Vec3>();
					values[0] = v.x;
					values[1] = v.y;
					values[2] = v.z;
					valid = true;
				} else if (binding.componentCount == 4 && current.Is<Vec4>()) {
					Vec4 v = current.As<Vec4>();
					values[0] = v.x;
					values[1] = v.y;
					values[2] = v.z;
					values[3] = v.w;
					valid = true;
				} else if (binding.componentCount == 3 && current.Is<Quat>()) {
					Vec3 v = EditorSceneOps::EulerForDisplay(current.As<Quat>());
					values[0] = v.x;
					values[1] = v.y;
					values[2] = v.z;
					valid = true;
				}
				if (valid) {
					UI::Widgets::SetVectorFieldValues(context, binding.vector, binding.componentCount, values, UI::Widgets::DEFAULT_DECIMALS);
				}
				break;
			}
			case Binding::Kind::EulerField: {
				bool editing = false;
				for (u32 i = 0; i < 3; ++i) {
					editing |= focused == binding.vector.components[i].field;
				}
				if (editing || !TypeRegistry::IsDerivedFrom(type, SceneObject::GetReflectType())) {
					break;
				}
				SceneObject* sceneObject = static_cast<SceneObject*>(object);
				u64 id = object->GetInstanceId();
				Quat rotation = sceneObject->GetLocalRotation();
				Vec3& cached = _eulerCache[id];
				Quat fromCache = Quat::FromEuler(cached);
				if (!(fromCache == rotation)) {
					cached = EditorSceneOps::EulerForDisplay(rotation);
				}
				f64 values[3] = {cached.x, cached.y, cached.z};
				UI::Widgets::SetVectorFieldValues(context, binding.vector, 3, values, UI::Widgets::DEFAULT_DECIMALS);
				break;
			}
			case Binding::Kind::ColorField: {
				bool editing = false;
				for (u32 i = 0; i < 4; ++i) {
					editing |= focused == binding.color.vector.components[i].field;
				}
				if (editing) {
					break;
				}
				Variant current;
				if (GetField(object, type, binding.field.field, &current) && current.Is<Color>()) {
					UI::Widgets::SetColorSwatchFieldValue(context, binding.color, current.As<Color>());
				}
				break;
			}
			case Binding::Kind::HandleLabel: {
				u64 id = 0;
				if (binding.nested) {
					if (void* at = ResolveNested(*binding.nested)) {
						std::memcpy(&id, at, sizeof(u64));
					}
				} else if (const FieldInfo* field = binding.field.Resolve()) {
					id = ReadHandleBits(binding.target, *field, binding.elementIndex);
				} else {
					break;
				}
				str caption = "None";
				if (id != 0) {
					if (Object* pointee = ObjectRegistry::Resolve(id)) {
						if (TypeRegistry::IsDerivedFrom(pointee->GetType(), SceneObject::GetReflectType())) {
							caption = static_cast<SceneObject*>(pointee)->name;
						} else if (const TypeInfo* assetBase = TypeRegistry::FindType(StringHash("Asset"));
								   assetBase && TypeRegistry::IsDerivedFrom(pointee->GetType(), assetBase)) {
							Asset* asset = static_cast<Asset*>(pointee);
							Path path = AssetDatabase::LookupPath(asset->assetId);
							if (!path.ToString().empty()) {
								caption = path.GetFilename();
							} else if (size_t mark = asset->virtualPath.find(" : "); mark != str::npos) {
								caption = asset->virtualPath.substr(mark + 3);
							} else {
								caption = TypeLabel(pointee->GetType());
							}
						} else {
							caption = TypeLabel(pointee->GetType());
						}
					} else {
						caption = std::format("(missing {:#x})", id);
					}
				}
				if (context.GetTextString(binding.element) != caption) {
					context.SetTextString(binding.element, caption);
				}
				break;
			}
			}
		}
	}

	void BuildRenderingLayerMaskRow(UI::Context& context, UI::ElementHandle parent, const TypeInfo* type,
									Handle<Object> target, InspectorPanel* panel) {
		if (!type || !panel) {
			return;
		}
		for (u32 index = 0; index < type->fieldCount; ++index) {
			const FieldInfo& field = type->fields[index];
			if (field.name == StringHash("_renderingLayerMask")) {
				panel->BuildFieldRow(context, parent, field, target);
				return;
			}
		}
	}

	void RegisterBuiltInComponentInspectors() {
		PropertyDrawers::Initialize();

		PropertyDrawers::RegisterComponentInspector(
			StringHash("MeshRenderer"),
			[](UI::Context& context, UI::ElementHandle parent, Handle<Component> componentHandle) {
				MeshRenderer* renderer = static_cast<MeshRenderer*>(componentHandle.Get());
				InspectorPanel* panel = InspectorPanel::GetBuildingPanel();
				if (!renderer || !panel) {
					return;
				}
				Handle<Object> target(static_cast<Object*>(renderer));

				UI::ElementHandle meshRow = panel->MakeValueRow(context, parent, "Mesh", target, StringHash("_mesh"));
				panel->BuildHandleRow(context, meshRow, target, StringHash("_mesh"),
									  StringHash("MeshAsset"), "Mesh", -1,
									  [target](u64 meshInstanceId) {
										  EditorSceneOps::BindDefaultMaterials(target, meshInstanceId);
									  });

				u32 slotCount = 1;
				if (MeshAsset* mesh = renderer->GetMesh().Get()) {
					for (const SubMesh& submesh : mesh->submeshes) {
						if (submesh.materialSlot + 1 > slotCount) {
							slotCount = submesh.materialSlot + 1;
						}
					}
				}
				for (u32 slot = 0; slot < slotCount; ++slot) {
					str label = std::format("Material {}", slot);
					UI::ElementHandle row = panel->MakeValueRow(context, parent, label, target, StringHash("_materials"), static_cast<i32>(slot));
					panel->BuildHandleRow(context, row, target, StringHash("_materials"),
										  StringHash("MaterialAsset"), label, static_cast<i32>(slot));
				}
				BuildRenderingLayerMaskRow(context, parent, MeshRenderer::GetReflectType(), target, panel);
			});

		PropertyDrawers::RegisterComponentInspector(
			StringHash("SkinnedMeshRenderer"),
			[](UI::Context& context, UI::ElementHandle parent, Handle<Component> componentHandle) {
				SkinnedMeshRenderer* renderer = static_cast<SkinnedMeshRenderer*>(componentHandle.Get());
				InspectorPanel* panel = InspectorPanel::GetBuildingPanel();
				if (!renderer || !panel) {
					return;
				}
				Handle<Object> target(static_cast<Object*>(renderer));

				UI::ElementHandle meshRow = panel->MakeValueRow(context, parent, "Mesh", target, StringHash("_mesh"));
				panel->BuildHandleRow(context, meshRow, target, StringHash("_mesh"),
									  StringHash("MeshAsset"), "Mesh", -1,
									  [target](u64 meshInstanceId) {
										  EditorSceneOps::BindDefaultMaterials(target, meshInstanceId);
									  });

				UI::ElementHandle skinRow = panel->MakeValueRow(context, parent, "Skin", target, StringHash("_skin"));
				panel->BuildHandleRow(context, skinRow, target, StringHash("_skin"),
									  StringHash("SkinAsset"), "Skin");

				UI::ElementHandle skeletonRow = panel->MakeValueRow(context, parent, "Skeleton", target, StringHash("_skeleton"));
				panel->BuildHandleRow(context, skeletonRow, target, StringHash("_skeleton"),
									  StringHash("Skeleton"), "Skeleton");

				MeshAsset* mesh = renderer->GetMesh().Get();
				u32 slotCount = 1;
				if (mesh) {
					for (const SubMesh& submesh : mesh->submeshes) {
						if (submesh.materialSlot + 1 > slotCount) {
							slotCount = submesh.materialSlot + 1;
						}
					}
				}
				for (u32 slot = 0; slot < slotCount; ++slot) {
					str label = std::format("Material {}", slot);
					UI::ElementHandle row = panel->MakeValueRow(context, parent, label, target, StringHash("_materials"), static_cast<i32>(slot));
					panel->BuildHandleRow(context, row, target, StringHash("_materials"),
										  StringHash("MaterialAsset"), label, static_cast<i32>(slot));
				}

				BuildRenderingLayerMaskRow(context, parent, SkinnedMeshRenderer::GetReflectType(), target, panel);

				if (!mesh || mesh->blendShapes.empty()) {
					return;
				}
				UI::ElementHandle header = MakeRow(context, parent, "BlendShapes");
				(void)header;
				for (size_t shape = 0; shape < mesh->blendShapes.size(); ++shape) {
					const str& name = mesh->blendShapes[shape].name;
					str label = name.empty() ? std::format("Shape {}", shape) : str(name);
					UI::ElementHandle row = panel->MakeValueRow(context, parent, label, target, StringHash("_blendShapeWeights"), static_cast<i32>(shape));
					panel->BuildNumberElementRow(context, row, target, StringHash("_blendShapeWeights"),
												 static_cast<u32>(shape), label);
				}
			});

		PropertyDrawers::RegisterComponentInspector(
			StringHash("ReflectionProbe"),
			[](UI::Context& context, UI::ElementHandle parent, Handle<Component> componentHandle) {
				ReflectionProbe* probe = static_cast<ReflectionProbe*>(componentHandle.Get());
				InspectorPanel* panel = InspectorPanel::GetBuildingPanel();
				if (!probe || !panel) {
					return;
				}
				Handle<Object> target(static_cast<Object*>(probe));
				Vector<const FieldInfo*> fields{GetMemoryResource(MemTag::Editor)};
				ForEachField(probe, probe->GetType(), [&fields](const FieldInfo* field, const Variant*) { fields.push_back(field); }, FieldWalkFlagIncludeInherited | FieldWalkFlagExcludeHidden);
				for (const FieldInfo* field : fields) {
					panel->BuildFieldRow(context, parent, *field, target);
				}

				UI::ElementHandle buttonRow = context.CreateElement();
				context.AppendChild(parent, buttonRow);
				UI::Style buttonRowStyle;
				buttonRowStyle.flexDirection = UI::FlexDirection::Row;
				buttonRowStyle.justifyContent = UI::Align::End;
				buttonRowStyle.padding = {8.0f, 4.0f, 8.0f, 4.0f};
				context.SetStyle(buttonRow, buttonRowStyle);
				const Handle<ReflectionProbe> probeHandle(probe);
				UI::Widgets::ButtonDesc bake;
				bake.label = "Bake";
				bake.font = UISystem::GetDefaultFont();
				bake.fontSizePx = FieldFont();
				bake.onClick = [probeHandle]() { ReflectionProbeBake::Request(probeHandle); };
				UI::ElementHandle button = UI::Widgets::Button(context, buttonRow, bake);
				const str why = ReflectionProbeBake::WhyNotBakeable(probeHandle);
				if (!why.empty()) {
					MakeDisabled(context, button);
					context.SetTooltip(button, why);
				}
			});
	}

	EditorIcons::Icon InspectorPanel::GetTabIcon() const {
		return EditorIcons::Glyph::PanelInspector;
	}

} // namespace Shalltear
