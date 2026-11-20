#include "InputBindings.h"

namespace Shalltear::CSharpImpl::InputBindings {

	// must match Input.cs
	static_assert(Input::MAX_GAMEPADS == 4);
	static_assert(static_cast<u32>(GamepadButton::Count) <= 32);
	static_assert(static_cast<u32>(GamepadAxis::Count) == 6);

	bool IsKeyDown(Key key) noexcept {
		return Input::IsKeyDown(key);
	}

	bool WasKeyPressed(Key key) noexcept {
		return Input::WasKeyPressed(key);
	}

	bool WasKeyReleased(Key key) noexcept {
		return Input::WasKeyReleased(key);
	}

	KeyMod GetKeyMods() noexcept {
		return Input::GetKeyMods();
	}

	bool IsMouseButtonDown(MouseButton button) noexcept {
		return Input::IsMouseButtonDown(button);
	}

	bool WasMouseButtonPressed(MouseButton button) noexcept {
		return Input::WasMouseButtonPressed(button);
	}

	bool WasMouseButtonReleased(MouseButton button) noexcept {
		return Input::WasMouseButtonReleased(button);
	}

	Vec2I GetMousePosition() noexcept {
		return Input::GetMousePosition();
	}

	Vec2 GetMouseDelta() noexcept {
		return Input::GetMouseDelta();
	}

	Vec2 GetMouseWheel() noexcept {
		return Input::GetMouseWheel();
	}

	void FillGamepad(i32 index, std::span<SltGamepadState> state) noexcept {
		if (state.empty()) {
			return;
		}
		SltGamepadState& out = state[0];
		out = {};
		if (index < 0 || index >= Input::MAX_GAMEPADS) {
			return;
		}
		const GamepadState& pad = Input::GetGamepad(static_cast<u8>(index));
		out.connected = pad.connected ? 1 : 0;
		for (u32 button = 0; button < static_cast<u32>(GamepadButton::Count); ++button) {
			out.buttons |= pad.buttons[button] ? 1u << button : 0u;
		}
		out.leftX = pad.GetAxis(GamepadAxis::LeftX);
		out.leftY = pad.GetAxis(GamepadAxis::LeftY);
		out.rightX = pad.GetAxis(GamepadAxis::RightX);
		out.rightY = pad.GetAxis(GamepadAxis::RightY);
		out.leftTrigger = pad.GetAxis(GamepadAxis::LeftTrigger);
		out.rightTrigger = pad.GetAxis(GamepadAxis::RightTrigger);
	}

	i32 FillFrameEvents(std::span<SltInputEvent> events) noexcept {
		i32 count = 0;
		for (const InputEvent& event : Input::GetFrameEvents()) {
			if (static_cast<size_t>(count) < events.size()) {
				SltInputEvent& out = events[static_cast<size_t>(count)];
				out = {};
				out.type = static_cast<uint8_t>(event.type);
				out.mods = static_cast<uint8_t>(event.mods);
				out.button = static_cast<uint8_t>(event.button);
				out.repeat = event.repeat ? 1 : 0;
				out.key = static_cast<uint16_t>(event.key);
				out.scancode = event.scancode;
				out.gamepadIndex = event.gamepadIndex;
				out.mousePosPx = event.mousePosPx;
				out.wheel = event.wheel;
				out.codepoint = static_cast<uint32_t>(event.codepoint);
			}
			++count;
		}
		return count;
	}

} // namespace Shalltear::CSharpImpl::InputBindings
