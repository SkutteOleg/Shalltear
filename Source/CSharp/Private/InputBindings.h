#pragma once

#include "ScriptBridge.h"

#include "Shalltear/Core/Input/Input.h"
#include "Shalltear/Core/Input/InputAction.h"
#include "Shalltear/Core/Types.h"

#include <span>

namespace Shalltear::CSharpImpl::InputBindings {

	bool IsKeyDown(Key key) noexcept;
	bool WasKeyPressed(Key key) noexcept;
	bool WasKeyReleased(Key key) noexcept;
	KeyMod GetKeyMods() noexcept;

	bool IsMouseButtonDown(MouseButton button) noexcept;
	bool WasMouseButtonPressed(MouseButton button) noexcept;
	bool WasMouseButtonReleased(MouseButton button) noexcept;

	Vec2I GetMousePosition() noexcept;
	Vec2 GetMouseDelta() noexcept;
	Vec2 GetMouseWheel() noexcept;

	void FillGamepad(i32 index, std::span<SltGamepadState> state) noexcept;
	// returned count may exceed events.size()
	i32 FillFrameEvents(std::span<SltInputEvent> events) noexcept;

} // namespace Shalltear::CSharpImpl::InputBindings
