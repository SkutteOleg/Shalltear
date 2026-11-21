#include "Shalltear/Editor/PropertyRows.h"

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/UI/UISystem.h"

#include "Shalltear/Editor/EditorTheme.h"

#include <cstring>
#include <format>

namespace Shalltear {

	namespace PropertyRows {

		u64 ReadRawBits(const FieldInfo& field, const void* address) {
			if (field.size > 8 || !address) {
				return 0;
			}
			u64 bits = 0;
			std::memcpy(&bits, address, field.size);
			return bits;
		}

		namespace {

			using EditorTheme::ColorToken;
			using EditorTheme::MetricToken;

			Color TextDim() { return EditorTheme::GetColor(ColorToken::TextDim); }
			f32 FieldFont() { return EditorTheme::GetMetric(MetricToken::FontBody); }

			bool IsEnum(const TypeInfo* type) {
				return type && type->enumConstants != nullptr && type->enumConstantCount > 0;
			}

			struct ValueShape {
				const TypeInfo* type = nullptr;
				size_t size = 0;
			};

			ValueShape ShapeOf(const FieldInfo& field, bool element) {
				if (!element) {
					return {field.fieldType, field.size};
				}
				const ContainerInfo* container = (field.flags & FieldFlagContainer) != 0 ? field.container : nullptr;
				return container ? ValueShape{container->elementType, container->elementSize} : ValueShape{};
			}

			bool IsScalarShape(const ValueShape& shape) {
				const TypeInfo* type = shape.type;
				return NameIs(type, "bool") || NameIs(type, "str") || IsNumeric(type) || (IsEnum(type) && shape.size <= 8);
			}

			u64 ReadBits(const ValueShape& shape, const void* address) {
				if (shape.size > 8 || !address) {
					return 0;
				}
				u64 bits = 0;
				std::memcpy(&bits, address, shape.size);
				return bits;
			}

			void CollectEnumLabels(const TypeInfo& enumType, u64 rawBits,
								   Vector<std::string_view>& labels, u32& selected) {
				labels.clear();
				labels.reserve(enumType.enumConstantCount);
				for (u32 i = 0; i < enumType.enumConstantCount; ++i) {
					const char* name = enumType.enumConstants[i].displayName;
					labels.push_back(name ? std::string_view(name) : std::string_view{});
				}
				const EnumConstantInfo* current = FindEnumConstantByValue(&enumType, rawBits);
				selected = current ? static_cast<u32>(current - enumType.enumConstants)
								   : enumType.enumConstantCount;
			}

		} // namespace

		bool NameIs(const TypeInfo* type, const char* name) {
			return type && type->name == StringHash(name);
		}

		bool IsNumeric(const TypeInfo* type) {
			static const StringHash names[] = {
				StringHash("f32"), StringHash("f64"),
				StringHash("i8"), StringHash("i16"), StringHash("i32"), StringHash("i64"),
				StringHash("u8"), StringHash("u16"), StringHash("u32"), StringHash("u64")};
			for (StringHash name : names) {
				if (type && type->name == name) {
					return true;
				}
			}
			return false;
		}

		bool IsIntegral(const TypeInfo* type) {
			return IsNumeric(type) && !NameIs(type, "f32") && !NameIs(type, "f64");
		}

		bool ReadNumber(const TypeInfo* type, const void* address, f64& out) {
			if (!address) {
				return false;
			}
			if (NameIs(type, "f32")) {
				out = *static_cast<const f32*>(address);
			} else if (NameIs(type, "f64")) {
				out = *static_cast<const f64*>(address);
			} else if (NameIs(type, "i8")) {
				out = *static_cast<const i8*>(address);
			} else if (NameIs(type, "i16")) {
				out = *static_cast<const i16*>(address);
			} else if (NameIs(type, "i32")) {
				out = *static_cast<const i32*>(address);
			} else if (NameIs(type, "i64")) {
				out = static_cast<f64>(*static_cast<const i64*>(address));
			} else if (NameIs(type, "u8")) {
				out = *static_cast<const u8*>(address);
			} else if (NameIs(type, "u16")) {
				out = *static_cast<const u16*>(address);
			} else if (NameIs(type, "u32")) {
				out = *static_cast<const u32*>(address);
			} else if (NameIs(type, "u64")) {
				out = static_cast<f64>(*static_cast<const u64*>(address));
			} else {
				return false;
			}
			return true;
		}

		bool WriteNumber(const TypeInfo* type, void* address, f64 value) {
			if (!address) {
				return false;
			}
			auto unsignedValue = [value]() -> u64 { return value < 0.0 ? 0ull : static_cast<u64>(value); };
			if (NameIs(type, "f32")) {
				*static_cast<f32*>(address) = static_cast<f32>(value);
			} else if (NameIs(type, "f64")) {
				*static_cast<f64*>(address) = value;
			} else if (NameIs(type, "i8")) {
				*static_cast<i8*>(address) = static_cast<i8>(value);
			} else if (NameIs(type, "i16")) {
				*static_cast<i16*>(address) = static_cast<i16>(value);
			} else if (NameIs(type, "i32")) {
				*static_cast<i32*>(address) = static_cast<i32>(value);
			} else if (NameIs(type, "i64")) {
				*static_cast<i64*>(address) = static_cast<i64>(value);
			} else if (NameIs(type, "u8")) {
				*static_cast<u8*>(address) = static_cast<u8>(unsignedValue());
			} else if (NameIs(type, "u16")) {
				*static_cast<u16*>(address) = static_cast<u16>(unsignedValue());
			} else if (NameIs(type, "u32")) {
				*static_cast<u32*>(address) = static_cast<u32>(unsignedValue());
			} else if (NameIs(type, "u64")) {
				*static_cast<u64*>(address) = unsignedValue();
			} else {
				return false;
			}
			return true;
		}

		bool IsScalarField(const FieldInfo& field) {
			if ((field.flags & FieldFlagContainer) != 0 || !field.addrFn) {
				return false;
			}
			return IsScalarShape(ShapeOf(field, false));
		}

		str FieldLabel(const FieldInfo& field) {
			str label = field.displayName ? field.displayName : std::format("{:#x}", field.name.GetValue());
			if (!label.empty() && label[0] == '_') {
				label.erase(label.begin());
			}
			return label;
		}

		void MakeDisabled(UI::Context& context, UI::ElementHandle element) {
			context.AddClass(element, EditorTheme::Class::Disabled);
			context.SetHoverable(element, false);
		}

		ScalarRow Build(UI::Context& context, UI::ElementHandle row, FieldRef field, ValueAccess access) {
			ScalarRow built;
			built.field = field;

			const FieldInfo* info = built.field.Resolve();
			if (!info) {
				return built;
			}
			const ValueShape shape = ShapeOf(*info, access.element);
			const TypeInfo* type = shape.type;
			const bool readOnly = access.readOnly || (info->flags & FieldFlagReadOnly) != 0;
			void* address = access.address ? access.address() : nullptr;
			if (!readOnly && (!address || !(access.element ? IsScalarShape(shape) : IsScalarField(*info)))) {
				return built;
			}

			Handle<FontAsset> font = UISystem::GetDefaultFont();
			built.access = MakeShared<ValueAccess>(std::move(access));
			SharedPtr<ValueAccess> held = built.access;

			if (readOnly) {
				UI::ElementHandle value = context.CreateElement();
				context.AppendChild(row, value);
				context.SetText(value, font, FieldFont(), TextDim(), str());
				built.kind = ScalarRow::Kind::ReadOnlyText;
				built.element = value;
				Refresh(context, built);
				return built;
			}

			if (NameIs(type, "bool")) {
				built.check = UI::Widgets::Checkbox(
					context, row, *static_cast<const bool*>(address),
					[held](bool next) {
						CommittedValue committed;
						committed.kind = CommittedValue::Kind::Bool;
						committed.boolean = next;
						if (held->commit) {
							held->commit(committed);
						}
					});
				built.kind = ScalarRow::Kind::Checkbox;
				return built;
			}

			if (NameIs(type, "str")) {
				UI::Widgets::TextFieldDesc desc;
				desc.text = *static_cast<const str*>(address);
				desc.font = font;
				desc.fontSizePx = FieldFont();
				f32 minHeightPx = 0.0f;
				if (!access.element) {
					if (const AttributeInfo* multiline = FindFieldAttribute(info, "Multiline"_hash);
						multiline && multiline->value.Is<i32>()) {
						desc.multiline = true;
						const i32 lines = multiline->value.As<i32>() > 1 ? multiline->value.As<i32>() : 3;
						minHeightPx = static_cast<f32>(lines) * FieldFont() * 1.4f + 8.0f;
					} else if (const AttributeInfo* textArea = FindFieldAttribute(info, "TextArea"_hash);
							   textArea && textArea->value.Is<AttrRangeData>()) {
						desc.multiline = true;
						const f32 lines = textArea->value.As<AttrRangeData>().min > 1.0f
											  ? textArea->value.As<AttrRangeData>().min
											  : 3.0f;
						minHeightPx = lines * FieldFont() * 1.4f + 8.0f;
					}
				}
				desc.onCommit = [held](std::string_view next) {
					void* live = held->address ? held->address() : nullptr;
					if (!live || *static_cast<const str*>(live) == next) {
						return;
					}
					CommittedValue committed;
					committed.kind = CommittedValue::Kind::Text;
					committed.text = next;
					if (held->commit) {
						held->commit(committed);
					}
				};
				built.element = UI::Widgets::TextField(context, row, desc);
				if (minHeightPx > 0.0f) {
					UI::Style style = context.GetStyle(built.element);
					style.minHeight = UI::Length::Pixel(minHeightPx);
					context.SetStyle(built.element, style);
				}
				built.kind = ScalarRow::Kind::TextField;
				return built;
			}

			if (NameIs(type, "u64") || NameIs(type, "i64")) {
				bool isSigned = NameIs(type, "i64");
				auto readBits = [](const void* from) {
					u64 bits = 0;
					std::memcpy(&bits, from, sizeof(u64));
					return bits;
				};
				auto spell = [isSigned](u64 bits) {
					return isSigned ? std::to_string(static_cast<i64>(bits)) : std::to_string(bits);
				};

				UI::Widgets::TextFieldDesc desc;
				str initial = spell(readBits(address));
				desc.text = initial;
				desc.font = font;
				desc.fontSizePx = FieldFont();
				desc.filter = [isSigned](char32_t codepoint) {
					return (codepoint >= U'0' && codepoint <= U'9') || (isSigned && codepoint == U'-');
				};
				desc.onCommit = [held, isSigned, readBits](std::string_view next) {
					void* live = held->address ? held->address() : nullptr;
					if (!live) {
						return;
					}
					str typed(next);
					u64 bits = 0;
					errno = 0;
					char* end = nullptr;
					if (isSigned) {
						long long parsed = std::strtoll(typed.c_str(), &end, 10);
						if (end == typed.c_str() || errno == ERANGE) {
							return;
						}
						bits = static_cast<u64>(static_cast<i64>(parsed));
					} else {
						unsigned long long parsed = std::strtoull(typed.c_str(), &end, 10);
						if (end == typed.c_str() || errno == ERANGE) {
							return;
						}
						bits = static_cast<u64>(parsed);
					}
					if (bits == readBits(live)) {
						return;
					}
					CommittedValue committed;
					committed.kind = CommittedValue::Kind::RawBits;
					committed.rawBits = bits;
					if (held->commit) {
						held->commit(committed);
					}
				};
				built.element = UI::Widgets::TextField(context, row, desc);
				built.kind = ScalarRow::Kind::IntegerField;
				return built;
			}

			if (IsEnum(type)) {
				Vector<std::string_view> labels{GetMemoryResource(MemTag::Editor)};
				u32 selected = 0;
				CollectEnumLabels(*type, ReadBits(shape, address), labels, selected);

				UI::Widgets::DropdownDesc desc;
				desc.items = {labels.data(), labels.size()};
				desc.selected = selected;
				desc.font = font;
				desc.fontSizePx = FieldFont();
				FieldRef ref = built.field;
				desc.onSelect = [held, ref](u32 index) mutable {
					const FieldInfo* live = ref.Resolve();
					void* liveAddress = held->address ? held->address() : nullptr;
					const ValueShape liveShape = live ? ShapeOf(*live, held->element) : ValueShape{};
					if (!liveShape.type || !liveAddress || index >= liveShape.type->enumConstantCount) {
						return;
					}
					u64 before = ReadBits(liveShape, liveAddress);
					u64 after = EnumValueToBits(liveShape.type, liveShape.type->enumConstants[index].value);
					if (before == after) {
						return;
					}
					CommittedValue committed;
					committed.kind = CommittedValue::Kind::RawBits;
					committed.rawBits = after;
					if (held->commit) {
						held->commit(committed);
					}
				};
				built.dropdown = UI::Widgets::Dropdown(context, row, desc);
				context.AddClass(built.dropdown.root, StringHash("slt-fill"));
				built.kind = ScalarRow::Kind::Dropdown;
				return built;
			}

			f64 value = 0.0;
			if (!ReadNumber(type, address, value)) {
				return built;
			}
			built.decimals = IsIntegral(type) ? 0 : UI::Widgets::DEFAULT_DECIMALS;

			auto commitNumber = [held](f64 next) {
				CommittedValue committed;
				committed.kind = CommittedValue::Kind::Number;
				committed.number = next;
				if (held->commit) {
					held->commit(committed);
				}
			};
			auto beginGesture = [held]() {
				if (held->beginGesture) {
					held->beginGesture();
				}
			};
			auto endGesture = [held]() {
				if (held->endGesture) {
					held->endGesture();
				}
			};

			if (const AttributeInfo* range = FindFieldAttribute(info, "Range"_hash)) {
				if (range->value.Is<AttrRangeData>()) {
					AttrRangeData data = range->value.As<AttrRangeData>();
					UI::Widgets::SliderDesc desc;
					desc.minValue = data.min;
					desc.maxValue = data.max;
					desc.value = static_cast<f32>(value);
					desc.showValueField = true;
					desc.decimals = built.decimals;
					desc.font = font;
					desc.fontSizePx = FieldFont();
					desc.onDragBegin = beginGesture;
					desc.onDragEnd = endGesture;
					desc.onChange = [commitNumber](f32 next) { commitNumber(next); };
					built.slider = UI::Widgets::Slider(context, row, desc);
					built.kind = ScalarRow::Kind::Slider;
					built.rangeMin = data.min;
					built.rangeMax = data.max;
					return built;
				}
			}

			UI::Widgets::NumberFieldDesc desc;
			desc.value = value;
			desc.decimals = built.decimals;
			desc.font = font;
			desc.fontSizePx = FieldFont();
			desc.onScrubBegin = beginGesture;
			desc.onScrubEnd = endGesture;
			desc.onChange = [commitNumber](f64 next) { commitNumber(next); };
			built.number = UI::Widgets::NumberField(context, row, desc);
			built.kind = ScalarRow::Kind::NumberField;
			return built;
		}

		void Refresh(UI::Context& context, ScalarRow& row) {
			if (row.kind == ScalarRow::Kind::None || !row.access) {
				return;
			}
			const FieldInfo* info = row.field.Resolve();
			void* address = row.access->address ? row.access->address() : nullptr;
			if (!info) {
				return;
			}
			const ValueShape shape = ShapeOf(*info, row.access->element);
			const TypeInfo* type = shape.type;
			UI::ElementHandle focused = context.GetFocused();

			switch (row.kind) {
			case ScalarRow::Kind::ReadOnlyText: {
				str text = "(read-only)";
				f64 value = 0.0;
				if (!address) {
					text = "?";
				} else if (NameIs(type, "str")) {
					text = *static_cast<const str*>(address);
				} else if (NameIs(type, "bool")) {
					text = *static_cast<const bool*>(address) ? "true" : "false";
				} else if (IsEnum(type)) {
					const EnumConstantInfo* constant = FindEnumConstantByValue(type, ReadBits(shape, address));
					text = constant && constant->displayName ? str(constant->displayName) : str();
				} else if (ReadNumber(type, address, value)) {
					text = std::format("{:.3f}", value);
				}
				if (context.GetTextString(row.element) != text) {
					context.SetTextString(row.element, text);
				}
				return;
			}
			case ScalarRow::Kind::Checkbox:
				if (address) {
					UI::Widgets::SetCheckboxValue(context, row.check, *static_cast<const bool*>(address));
				}
				return;
			case ScalarRow::Kind::TextField: {
				if (!address || focused == row.element) {
					return;
				}
				const str& value = *static_cast<const str*>(address);
				if (context.GetTextString(row.element) != value) {
					context.SetTextString(row.element, value);
				}
				return;
			}
			case ScalarRow::Kind::IntegerField: {
				if (!address || focused == row.element) {
					return;
				}
				u64 bits = 0;
				std::memcpy(&bits, address, sizeof(u64));
				str value = NameIs(type, "i64") ? std::to_string(static_cast<i64>(bits)) : std::to_string(bits);
				if (context.GetTextString(row.element) != value) {
					context.SetTextString(row.element, value);
				}
				return;
			}
			case ScalarRow::Kind::Dropdown: {
				if (!type || !address) {
					return;
				}
				Vector<std::string_view> labels{GetMemoryResource(MemTag::Editor)};
				u32 selected = 0;
				CollectEnumLabels(*type, ReadBits(shape, address), labels, selected);
				UI::Widgets::SetDropdownSelection(context, row.dropdown, {labels.data(), labels.size()}, selected);
				return;
			}
			case ScalarRow::Kind::NumberField: {
				if (focused == row.number.field) {
					return;
				}
				f64 value = 0.0;
				if (ReadNumber(type, address, value)) {
					UI::Widgets::SetNumberFieldValue(context, row.number, value, row.decimals);
				}
				return;
			}
			case ScalarRow::Kind::Slider: {
				if (focused == row.slider.value.field) {
					return;
				}
				f64 value = 0.0;
				if (ReadNumber(type, address, value)) {
					UI::Widgets::SetSliderValue(context, row.slider, row.rangeMin, row.rangeMax, static_cast<f32>(value));
				}
				return;
			}
			default:
				return;
			}
		}

	} // namespace PropertyRows

} // namespace Shalltear
