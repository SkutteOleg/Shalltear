#pragma once

#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Reflection/TypeInfo.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Types.h"
#include "Shalltear/Core/UI/Context.h"
#include "Shalltear/Core/UI/Widgets.h"

#include "Shalltear/Editor/EditorAPI.h"

#include <functional>
#include <string_view>

namespace Shalltear {

	namespace PropertyRows {

		struct CommittedValue {
			enum class Kind : u8 {
				Number = 0,
				Bool,
				Text,
				RawBits
			};
			Kind kind = Kind::Number;
			f64 number = 0.0;
			bool boolean = false;
			std::string_view text;
			u64 rawBits = 0;
		};

		struct ValueAccess {
			std::function<void*()> address;
			std::function<void(const CommittedValue&)> commit;

			std::function<void()> beginGesture;
			std::function<void()> endGesture;

			bool readOnly = false;
			// address = 1 element, field = its container
			bool element = false;
		};

		struct ScalarRow {
			enum class Kind : u8 {
				None = 0,
				ReadOnlyText,
				Checkbox,
				NumberField,
				Slider,
				TextField,
				Dropdown,
				// 64 bit as text (never widened to f64)
				IntegerField
			};

			Kind kind = Kind::None;
			FieldRef field{};

			SharedPtr<ValueAccess> access;

			UI::Widgets::CheckboxParts check;
			UI::Widgets::NumberFieldParts number;
			UI::Widgets::SliderParts slider;
			UI::Widgets::DropdownParts dropdown;
			UI::ElementHandle element;
			f32 rangeMin = 0.0f;
			f32 rangeMax = 1.0f;
			i32 decimals = 0;
		};

		SHALLTEAR_EDITOR_API bool NameIs(const TypeInfo* type, const char* name);
		SHALLTEAR_EDITOR_API bool IsNumeric(const TypeInfo* type);
		SHALLTEAR_EDITOR_API bool IsIntegral(const TypeInfo* type);

		SHALLTEAR_EDITOR_API bool ReadNumber(const TypeInfo* type, const void* address, f64& out);

		SHALLTEAR_EDITOR_API bool WriteNumber(const TypeInfo* type, void* address, f64 value);

		SHALLTEAR_EDITOR_API u64 ReadRawBits(const FieldInfo& field, const void* address);

		SHALLTEAR_EDITOR_API bool IsScalarField(const FieldInfo& field);

		SHALLTEAR_EDITOR_API str FieldLabel(const FieldInfo& field);

		SHALLTEAR_EDITOR_API void MakeDisabled(UI::Context& context, UI::ElementHandle element);

		SHALLTEAR_EDITOR_API ScalarRow Build(UI::Context& context, UI::ElementHandle row,
											 FieldRef field, ValueAccess access);

		SHALLTEAR_EDITOR_API void Refresh(UI::Context& context, ScalarRow& row);

	} // namespace PropertyRows

} // namespace Shalltear
