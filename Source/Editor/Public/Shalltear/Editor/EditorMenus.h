#pragma once

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Types.h"
#include "Shalltear/Core/UI/Widgets.h"

#include "Shalltear/Editor/EditorAPI.h"

#include <functional>
#include <string_view>

namespace Shalltear::EditorMenus {

	struct MenuItemDesc {
		str label;
		str shortcut;
		bool separatorBefore = false;
		std::function<bool()> isEnabled;
		std::function<void(UI::Context& context)> onSelect;
	};

	SHALLTEAR_EDITOR_API bool Register(std::string_view menuLabel, MenuItemDesc item, const void* owner);

	SHALLTEAR_EDITOR_API u32 UnregisterOwnedBy(const void* owner);

	SHALLTEAR_EDITOR_API void AppendContributed(std::string_view menuLabel, UI::Context& context, Vector<UI::Widgets::MenuItem>& items);

	SHALLTEAR_EDITOR_API void Terminate();

} // namespace Shalltear::EditorMenus
