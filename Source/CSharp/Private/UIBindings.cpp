#include "UIBindings.h"

#include "BindingSupport.h"
#include "ManagedTypes.h"

#include "Shalltear/Core/UI/Context.h"
#include "Shalltear/Core/UI/Style.h"

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Shalltear::CSharpImpl::UIBindings {

	namespace {

		using UI::Context;
		using UI::ElementHandle;

		struct Listener {
			u64 panel = 0;
			u64 element = 0;
			UIEvent kind = UIEvent::Click;
			u64 key = 0;
		};

		constexpr u32 PRUNE_EVERY = 64;

		void (*_entry)(uint64_t key, uint8_t kind, double value, SltString text, uint64_t element) = nullptr;
		std::vector<Listener> _listeners;
		std::vector<Listener> _pending;
		u32 _dispatching = 0;
		u32 _listenCount = 0;
		str _text;
		std::array<char, UI::MAX_STYLE_VALUE_TEXT> _styleText{};

		u64 KeyOf(ElementHandle handle) noexcept {
			return handle.IsValid() ? (static_cast<u64>(handle.generation) << 32) | handle.index : 0;
		}

		ElementHandle HandleOf(const Context& context, u64 element) noexcept {
			const ElementHandle handle = context.MakeHandle(static_cast<u32>(element));
			return handle.IsValid() && handle.generation == static_cast<u32>(element >> 32) && context.IsAlive(handle) ? handle : ElementHandle{};
		}

		Context& ContextOf(UIPanelRenderer& panel) {
			Context* context = panel.GetContext();
			if (!context) {
				throw std::runtime_error("UIPanelRenderer has no UI. Enable it while the UI system runs");
			}
			return *context;
		}

		std::pair<Context&, ElementHandle> Resolve(UIPanelRenderer& panel, u64 element) {
			Context& context = ContextOf(panel);
			const ElementHandle handle = HandleOf(context, element);
			if (!handle.IsValid()) {
				throw std::runtime_error("UI element is gone (destroyed, or its panel rebuilt)");
			}
			return {context, handle};
		}

		const UI::Element& ElementOf(UIPanelRenderer& panel, u64 element) {
			const auto [context, handle] = Resolve(panel, element);
			return *context.GetElement(handle);
		}

		u64 Related(UIPanelRenderer& panel, u64 element, u32 UI::Element::* link) {
			const auto [context, handle] = Resolve(panel, element);
			return KeyOf(context.MakeHandle(context.GetElement(handle)->*link));
		}

		void Fire(u64 key, UIEvent kind, f64 value, std::string_view text, u64 element) {
			if (!_entry) {
				return;
			}
			ManagedTypes::FlushDirty();
			++_dispatching;
			_entry(key, static_cast<uint8_t>(kind), value, SltString{text.data(), static_cast<int32_t>(text.size())}, element);
			--_dispatching;
		}

		std::function<void()> Plain(u64 key, UIEvent kind, u64 element) {
			if (!key) {
				return nullptr;
			}
			return [key, kind, element] { Fire(key, kind, 0.0, {}, element); };
		}

		std::function<void(std::string_view)> Typed(u64 key, UIEvent kind, u64 element) {
			if (!key) {
				return nullptr;
			}
			return [key, kind, element](std::string_view text) { Fire(key, kind, 0.0, text, element); };
		}

		void Bind(UI::ElementEvents& events, UIEvent kind, u64 key, u64 element) {
			switch (kind) {
			case UIEvent::MouseDown:
				events.onMouseDown = Plain(key, kind, element);
				break;
			case UIEvent::MouseUp:
				events.onMouseUp = Plain(key, kind, element);
				break;
			case UIEvent::Click:
				events.onClick = Plain(key, kind, element);
				break;
			case UIEvent::RightClick:
				events.onRightClick = Plain(key, kind, element);
				break;
			case UIEvent::DoubleClick:
				events.onDoubleClick = Plain(key, kind, element);
				break;
			case UIEvent::HoverEnter:
				events.onHoverEnter = Plain(key, kind, element);
				break;
			case UIEvent::HoverLeave:
				events.onHoverLeave = Plain(key, kind, element);
				break;
			case UIEvent::PressEnd:
				events.onPressEnd = Plain(key, kind, element);
				break;
			case UIEvent::FocusGained:
				events.onFocusGained = Plain(key, kind, element);
				break;
			case UIEvent::FocusLost:
				events.onFocusLost = Plain(key, kind, element);
				break;
			case UIEvent::PopupClosed:
				events.onPopupClosed = Plain(key, kind, element);
				break;
			case UIEvent::ValueChange:
				events.onValueChange = key ? std::function<void(f64)>([key, element](f64 value) { Fire(key, UIEvent::ValueChange, value, {}, element); })
										   : nullptr;
				break;
			case UIEvent::TextChange:
				events.onTextChange = Typed(key, kind, element);
				break;
			case UIEvent::TextCommit:
				events.onTextCommit = Typed(key, kind, element);
				break;
			}
		}

		void Apply(const Listener& change) {
			UIPanelRenderer* panel = ResolveObject<UIPanelRenderer>(change.panel);
			Context* context = panel ? panel->GetContext() : nullptr;
			const ElementHandle handle = context ? HandleOf(*context, change.element) : ElementHandle{};
			if (handle.IsValid() && (change.key || context->HasEvents(handle))) {
				Bind(context->Events(handle), change.kind, change.key, change.element);
			}
		}

		void Prune() {
			std::erase_if(_listeners, [](const Listener& held) {
				UIPanelRenderer* panel = ResolveObject<UIPanelRenderer>(held.panel);
				const Context* context = panel ? panel->GetContext() : nullptr;
				return !context || !HandleOf(*context, held.element).IsValid();
			});
		}

		u64 Record(UIPanelRenderer& panel, u64 element, UIEvent kind, u64 key) {
			if (static_cast<u8>(kind) > static_cast<u8>(UIEvent::TextCommit)) {
				throw std::runtime_error(std::format("UIEvent {} isn't an event. Pass a UIEvent value", static_cast<u32>(kind)));
			}
			Resolve(panel, element);
			const Listener change{panel.GetInstanceId(), element, kind, key};
			if (_dispatching) {
				_pending.push_back(change);
			} else {
				Apply(change);
			}
			for (auto it = _listeners.begin(); it != _listeners.end(); ++it) {
				if (it->panel == change.panel && it->element == element && it->kind == kind) {
					const u64 replaced = it->key;
					if (key) {
						it->key = key;
					} else {
						_listeners.erase(it);
					}
					return replaced;
				}
			}
			if (key) {
				if (++_listenCount % PRUNE_EVERY == 0) {
					Prune();
				}
				_listeners.push_back(change);
			}
			return 0;
		}

	} // anonymous namespace

	u64 FindElement(UIPanelRenderer& panel, std::string_view id) noexcept {
		return KeyOf(panel.FindElement(id));
	}

	u64 GetRoot(UIPanelRenderer& panel) noexcept {
		const Context* context = panel.GetContext();
		return context ? KeyOf(context->GetRoot()) : 0;
	}

	u64 GetFocused(UIPanelRenderer& panel) noexcept {
		const Context* context = panel.GetContext();
		return context ? KeyOf(context->GetFocused()) : 0;
	}

	u64 GetHovered(UIPanelRenderer& panel) noexcept {
		const Context* context = panel.GetContext();
		return context ? KeyOf(context->GetHovered()) : 0;
	}

	void ClearFocus(UIPanelRenderer& panel) {
		ContextOf(panel).ClearFocus();
	}

	bool IsAlive(UIPanelRenderer& panel, u64 element) noexcept {
		const Context* context = panel.GetContext();
		return context && HandleOf(*context, element).IsValid();
	}

	u64 GetParent(UIPanelRenderer& panel, u64 element) {
		return Related(panel, element, &UI::Element::parent);
	}

	u64 GetFirstChild(UIPanelRenderer& panel, u64 element) {
		return Related(panel, element, &UI::Element::firstChild);
	}

	u64 GetNextSibling(UIPanelRenderer& panel, u64 element) {
		return Related(panel, element, &UI::Element::nextSibling);
	}

	u64 GetIdHash(UIPanelRenderer& panel, u64 element) {
		return ElementOf(panel, element).id.GetValue();
	}

	bool GetVisible(UIPanelRenderer& panel, u64 element) {
		return HasFlag(ElementOf(panel, element).flags, UI::ElementFlags::Visible);
	}

	void SetVisible(UIPanelRenderer& panel, u64 element, bool visible) {
		const auto [context, handle] = Resolve(panel, element);
		context.SetVisible(handle, visible);
	}

	bool GetHoverable(UIPanelRenderer& panel, u64 element) {
		return HasFlag(ElementOf(panel, element).flags, UI::ElementFlags::Hoverable);
	}

	void SetHoverable(UIPanelRenderer& panel, u64 element, bool hoverable) {
		const auto [context, handle] = Resolve(panel, element);
		context.SetHoverable(handle, hoverable);
	}

	bool GetFocusable(UIPanelRenderer& panel, u64 element) {
		return HasFlag(ElementOf(panel, element).flags, UI::ElementFlags::Focusable);
	}

	void SetFocusable(UIPanelRenderer& panel, u64 element, bool focusable) {
		const auto [context, handle] = Resolve(panel, element);
		context.SetFocusable(handle, focusable);
	}

	Color GetTint(UIPanelRenderer& panel, u64 element) {
		return ElementOf(panel, element).tint;
	}

	void SetTint(UIPanelRenderer& panel, u64 element, const Color& tint) {
		const auto [context, handle] = Resolve(panel, element);
		context.SetTint(handle, tint);
	}

	void AddClass(UIPanelRenderer& panel, u64 element, std::string_view name) {
		const auto [context, handle] = Resolve(panel, element);
		if (name.empty()) {
			throw std::runtime_error("UIElement.AddClass got an empty class. Pass a class name");
		}
		if (!context.HasClass(handle, StringHash(name)) && context.GetElement(handle)->classCount >= UI::MAX_ELEMENT_CLASSES) {
			throw std::runtime_error(std::format("UI element has {} classes (the most allowed). Remove one", UI::MAX_ELEMENT_CLASSES));
		}
		context.AddClass(handle, StringHash(name));
	}

	void RemoveClass(UIPanelRenderer& panel, u64 element, std::string_view name) {
		const auto [context, handle] = Resolve(panel, element);
		context.RemoveClass(handle, StringHash(name));
	}

	bool HasClass(UIPanelRenderer& panel, u64 element, std::string_view name) {
		const auto [context, handle] = Resolve(panel, element);
		return context.HasClass(handle, StringHash(name));
	}

	bool SetStyle(UIPanelRenderer& panel, u64 element, std::string_view property, std::string_view value) {
		const auto [context, handle] = Resolve(panel, element);
		UI::Style style = context.GetStyle(handle);
		if (!UI::ApplyStyleDeclaration(style, StringHash(property), value, &context.GetStyleVars())) {
			return false;
		}
		context.SetStyle(handle, style);
		return true;
	}

	std::string_view GetStyle(UIPanelRenderer& panel, u64 element, std::string_view property, bool computed) {
		const auto [context, handle] = Resolve(panel, element);
		const UI::Style& style = computed ? context.GetComputedStyle(handle) : context.GetStyle(handle);
		return UI::ReadStyleProperty(style, StringHash(property), _styleText.data(), _styleText.size());
	}

	void SetText(UIPanelRenderer& panel, u64 element, std::string_view text) {
		const auto [context, handle] = Resolve(panel, element);
		context.SetTextString(handle, text);
	}

	std::string_view GetText(UIPanelRenderer& panel, u64 element) {
		const auto [context, handle] = Resolve(panel, element);
		_text = context.GetTextString(handle);
		return _text;
	}

	void SetTextKey(UIPanelRenderer& panel, u64 element, std::string_view key, std::string_view fallback) {
		const auto [context, handle] = Resolve(panel, element);
		context.SetTextKey(handle, key, fallback);
	}

	std::string_view GetTextKey(UIPanelRenderer& panel, u64 element) {
		const auto [context, handle] = Resolve(panel, element);
		_text = context.GetTextKey(handle);
		return _text;
	}

	void SetRichText(UIPanelRenderer& panel, u64 element, bool enabled) {
		const auto [context, handle] = Resolve(panel, element);
		context.SetRichText(handle, enabled);
	}

	void ClearText(UIPanelRenderer& panel, u64 element) {
		const auto [context, handle] = Resolve(panel, element);
		context.ClearText(handle);
	}

	bool GetTextInput(UIPanelRenderer& panel, u64 element) {
		const auto [context, handle] = Resolve(panel, element);
		return context.HasTextInput(handle);
	}

	void SetTextInput(UIPanelRenderer& panel, u64 element, bool enabled) {
		const auto [context, handle] = Resolve(panel, element);
		context.SetTextInput(handle, enabled);
	}

	void SetTooltip(UIPanelRenderer& panel, u64 element, std::string_view text) {
		const auto [context, handle] = Resolve(panel, element);
		context.SetTooltip(handle, text);
	}

	void SetImage(UIPanelRenderer& panel, u64 element, TextureAsset* texture, f32 sizePx) {
		const auto [context, handle] = Resolve(panel, element);
		if (!texture) {
			context.ClearImage(handle);
			return;
		}
		context.SetImage(handle, Handle<TextureAsset>(texture), sizePx);
	}

	void ClearImage(UIPanelRenderer& panel, u64 element) {
		const auto [context, handle] = Resolve(panel, element);
		context.ClearImage(handle);
	}

	TextureAsset* GetImage(UIPanelRenderer& panel, u64 element) {
		const auto [context, handle] = Resolve(panel, element);
		return context.GetImageAsset(handle).Get();
	}

	void Focus(UIPanelRenderer& panel, u64 element) {
		const auto [context, handle] = Resolve(panel, element);
		context.SetFocus(handle);
	}

	Vec4 GetRect(UIPanelRenderer& panel, u64 element, bool content) {
		const auto [context, handle] = Resolve(panel, element);
		const UI::LayoutData& layout = context.GetLayout(handle);
		const UI::Rect& rect = content ? layout.content : layout.borderBox;
		return Vec4(rect.x, rect.y, rect.w, rect.h);
	}

	Vec2 GetScrollOffset(UIPanelRenderer& panel, u64 element) {
		return ElementOf(panel, element).scrollOffset;
	}

	void SetScrollOffset(UIPanelRenderer& panel, u64 element, const Vec2& offset) {
		const auto [context, handle] = Resolve(panel, element);
		context.SetScrollOffset(handle, offset);
	}

	void ScrollIntoView(UIPanelRenderer& panel, u64 element) {
		const auto [context, handle] = Resolve(panel, element);
		context.ScrollIntoView(handle);
	}

	u64 CreateChild(UIPanelRenderer& panel, u64 element) {
		const auto [context, handle] = Resolve(panel, element);
		const ElementHandle child = context.CreateElement();
		context.AppendChild(handle, child);
		return KeyOf(child);
	}

	void Destroy(UIPanelRenderer& panel, u64 element) {
		const auto [context, handle] = Resolve(panel, element);
		if (handle == context.GetRoot()) {
			throw std::runtime_error("UIElement.Destroy got the panel's root. Destroy its children");
		}
		context.DestroyElementNextUpdate(handle);
	}

	void ClearChildren(UIPanelRenderer& panel, u64 element) {
		const auto [context, handle] = Resolve(panel, element);
		for (u32 child = context.GetElement(handle)->firstChild; child != UI::INVALID_UI_INDEX;) {
			const ElementHandle doomed = context.MakeHandle(child);
			child = context.GetElement(doomed)->nextSibling;
			context.DestroyElementNextUpdate(doomed);
		}
	}

	u64 Instantiate(UIPanelRenderer& panel, u64 element, UIDocumentAsset& document) {
		const auto [context, handle] = Resolve(panel, element);
		UIInstantiateDesc desc;
		const UIPanelSettings* settings = panel.GetPanelSettings().Get();
		desc.font = settings && settings->font.IsValid() ? settings->font : UISystem::GetDefaultFont();
		if (settings) {
			desc.fontSizePx = settings->fontSizePx;
			desc.textColor = settings->textColor;
		}
		return KeyOf(document.Instantiate(context, handle, desc));
	}

	u64 Listen(UIPanelRenderer& panel, u64 element, UIEvent kind, u64 key) {
		if (!key) {
			throw std::runtime_error("UIElement.SetHandler got key 0");
		}
		return Record(panel, element, kind, key);
	}

	u64 Unlisten(UIPanelRenderer& panel, u64 element, UIEvent kind) {
		return Record(panel, element, kind, 0);
	}

	void AttachEvents(i64 entry) {
		_entry = reinterpret_cast<decltype(_entry)>(static_cast<intptr_t>(entry));
	}

	void FlushPending() {
		std::vector<Listener> pending = std::exchange(_pending, {});
		for (const Listener& change : pending) {
			Apply(change);
		}
	}

	void UnlistenAll() {
		_pending.clear();
		for (const Listener& held : _listeners) {
			Apply(Listener{held.panel, held.element, held.kind, 0});
		}
		_listeners.clear();
	}

	void Release() {
		UnlistenAll();
		_entry = nullptr;
		_listeners = {};
		_pending = {};
		_text = {};
	}

} // namespace Shalltear::CSharpImpl::UIBindings
