#pragma once

#include "ScriptBridge.h"

#include "Shalltear/Core/Graphics/TextureAsset.h"
#include "Shalltear/Core/Math/Color.h"
#include "Shalltear/Core/Math/Vector.h"
#include "Shalltear/Core/Types.h"
#include "Shalltear/Core/UI/FontAsset.h"
#include "Shalltear/Core/UI/UIDocument.h"
#include "Shalltear/Core/UI/UIPanelRenderer.h"
#include "Shalltear/Core/UI/UIPanelSettings.h"

#include <string_view>

// refusals thrown as std::runtime_error
namespace Shalltear::CSharpImpl::UIBindings {

	enum class UIEvent : u8 {
		MouseDown = 0,
		MouseUp,
		Click,
		RightClick,
		DoubleClick,
		HoverEnter,
		HoverLeave,
		PressEnd,
		FocusGained,
		FocusLost,
		PopupClosed,
		ValueChange,
		TextChange,
		TextCommit,
	};

	// element keys packed as generation << 32 | index
	u64 FindElement(UIPanelRenderer& panel, std::string_view id) noexcept;
	u64 GetRoot(UIPanelRenderer& panel) noexcept;
	u64 GetFocused(UIPanelRenderer& panel) noexcept;
	u64 GetHovered(UIPanelRenderer& panel) noexcept;
	void ClearFocus(UIPanelRenderer& panel);

	bool IsAlive(UIPanelRenderer& panel, u64 element) noexcept;
	u64 GetParent(UIPanelRenderer& panel, u64 element);
	u64 GetFirstChild(UIPanelRenderer& panel, u64 element);
	u64 GetNextSibling(UIPanelRenderer& panel, u64 element);
	u64 GetIdHash(UIPanelRenderer& panel, u64 element);

	bool GetVisible(UIPanelRenderer& panel, u64 element);
	void SetVisible(UIPanelRenderer& panel, u64 element, bool visible);
	bool GetHoverable(UIPanelRenderer& panel, u64 element);
	void SetHoverable(UIPanelRenderer& panel, u64 element, bool hoverable);
	bool GetFocusable(UIPanelRenderer& panel, u64 element);
	void SetFocusable(UIPanelRenderer& panel, u64 element, bool focusable);
	Color GetTint(UIPanelRenderer& panel, u64 element);
	void SetTint(UIPanelRenderer& panel, u64 element, const Color& tint);

	void AddClass(UIPanelRenderer& panel, u64 element, std::string_view name);
	void RemoveClass(UIPanelRenderer& panel, u64 element, std::string_view name);
	bool HasClass(UIPanelRenderer& panel, u64 element, std::string_view name);

	bool SetStyle(UIPanelRenderer& panel, u64 element, std::string_view property, std::string_view value);
	std::string_view GetStyle(UIPanelRenderer& panel, u64 element, std::string_view property, bool computed);

	void SetText(UIPanelRenderer& panel, u64 element, std::string_view text);
	std::string_view GetText(UIPanelRenderer& panel, u64 element);
	void SetTextKey(UIPanelRenderer& panel, u64 element, std::string_view key, std::string_view fallback);
	std::string_view GetTextKey(UIPanelRenderer& panel, u64 element);
	void SetRichText(UIPanelRenderer& panel, u64 element, bool enabled);
	void ClearText(UIPanelRenderer& panel, u64 element);
	bool GetTextInput(UIPanelRenderer& panel, u64 element);
	void SetTextInput(UIPanelRenderer& panel, u64 element, bool enabled);
	void SetTooltip(UIPanelRenderer& panel, u64 element, std::string_view text);

	void SetImage(UIPanelRenderer& panel, u64 element, TextureAsset* texture, f32 sizePx);
	void ClearImage(UIPanelRenderer& panel, u64 element);
	TextureAsset* GetImage(UIPanelRenderer& panel, u64 element);

	void Focus(UIPanelRenderer& panel, u64 element);
	Vec4 GetRect(UIPanelRenderer& panel, u64 element, bool content);
	Vec2 GetScrollOffset(UIPanelRenderer& panel, u64 element);
	void SetScrollOffset(UIPanelRenderer& panel, u64 element, const Vec2& offset);
	void ScrollIntoView(UIPanelRenderer& panel, u64 element);

	u64 CreateChild(UIPanelRenderer& panel, u64 element);
	void Destroy(UIPanelRenderer& panel, u64 element);
	void ClearChildren(UIPanelRenderer& panel, u64 element);
	u64 Instantiate(UIPanelRenderer& panel, u64 element, UIDocumentAsset& document);

	u64 Listen(UIPanelRenderer& panel, u64 element, UIEvent kind, u64 key);
	u64 Unlisten(UIPanelRenderer& panel, u64 element, UIEvent kind);
	// managed function pointer, must match UIEvents.cs
	void AttachEvents(i64 entry);
	void FlushPending();
	void UnlistenAll();
	void Release();

} // namespace Shalltear::CSharpImpl::UIBindings
