#include "Shalltear/Editor/EditorMenus.h"

#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"

#include <algorithm>

namespace Shalltear::EditorMenus {

	namespace {

		struct Entry {
			str menu;
			MenuItemDesc item;
			const void* owner = nullptr;
		};

		struct Registry {
			Vector<Entry> entries{GetMemoryResource(MemTag::Editor)};
		};

		Registry* _registry = nullptr;

		Registry& Get() {
			if (!_registry) {
				_registry = New<Registry>(MemTag::Editor);
			}
			return *_registry;
		}

	} // anonymous namespace

	bool Register(std::string_view menuLabel, MenuItemDesc item, const void* owner) {
		if (menuLabel.empty() || item.label.empty() || !item.onSelect) {
			return false;
		}
		if (!owner) {
			SLT_LOG_WARN("Editor", "ignoring menu item '{}' with no owner to sweep it with", item.label);
			return false;
		}
		Get().entries.push_back(Entry{str(menuLabel), std::move(item), owner});
		return true;
	}

	u32 UnregisterOwnedBy(const void* owner) {
		if (!owner || !_registry) {
			return 0;
		}
		return static_cast<u32>(std::erase_if(_registry->entries, [owner](const Entry& entry) { return entry.owner == owner; }));
	}

	void AppendContributed(std::string_view menuLabel, UI::Context& context, Vector<UI::Widgets::MenuItem>& items) {
		if (!_registry) {
			return;
		}
		UI::Context* contextPtr = &context;
		for (const Entry& entry : _registry->entries) {
			if (entry.menu != menuLabel) {
				continue;
			}
			UI::Widgets::MenuItem item;
			item.label = entry.item.label;
			item.shortcut = entry.item.shortcut;
			item.enabled = !entry.item.isEnabled || entry.item.isEnabled();
			item.separatorBefore = entry.item.separatorBefore;
			item.onSelect = [contextPtr, select = entry.item.onSelect]() { select(*contextPtr); };
			items.push_back(std::move(item));
		}
	}

	void Terminate() {
		if (_registry) {
			Delete(_registry);
			_registry = nullptr;
		}
	}

} // namespace Shalltear::EditorMenus
