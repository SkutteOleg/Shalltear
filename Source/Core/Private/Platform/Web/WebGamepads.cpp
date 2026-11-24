#include "Input/InputInternal.h"

#include <cmath>
#include <cstring>

#include <emscripten/html5.h>

namespace Shalltear::InputInternal {

	namespace {

		// must match WindowsGamepads.cpp and LinuxGamepads.cpp
		constexpr f32 LEFT_STICK_DEADZONE = 7849.0f / 32767.0f;
		constexpr f32 RIGHT_STICK_DEADZONE = 8689.0f / 32767.0f;
		constexpr f32 TRIGGER_THRESHOLD = 30.0f / 255.0f;

		void ApplyStickDeadzone(f32 rawX, f32 rawY, f32 deadzone, f32& outX, f32& outY) {
			const f32 magnitude = std::sqrt(rawX * rawX + rawY * rawY);
			if (magnitude <= deadzone) {
				outX = 0.0f;
				outY = 0.0f;
				return;
			}
			const f32 clamped = magnitude > 1.0f ? 1.0f : magnitude;
			const f32 normalized = (clamped - deadzone) / (1.0f - deadzone);
			outX = (rawX / magnitude) * normalized;
			outY = (rawY / magnitude) * normalized;
		}

		f32 ApplyTriggerThreshold(f32 raw) {
			return raw <= TRIGGER_THRESHOLD ? 0.0f : (raw - TRIGGER_THRESHOLD) / (1.0f - TRIGGER_THRESHOLD);
		}

		void PushConnectionEvent(u8 index, bool connected) {
			InputEvent event;
			event.type = connected ? InputEventType::GamepadConnected : InputEventType::GamepadDisconnected;
			event.gamepadIndex = index;
			PushEvent(event);
		}

	} // anonymous namespace

	void PollGamepads(bool applicationFocused) {
		State& state = GetState();
		if (emscripten_sample_gamepad_data() != EMSCRIPTEN_RESULT_SUCCESS) {
			return;
		}
		const int count = emscripten_get_num_gamepads();
		for (u8 i = 0; i < Input::MAX_GAMEPADS; ++i) {
			EmscriptenGamepadEvent pad{};
			const bool connected = static_cast<int>(i) < count &&
								   emscripten_get_gamepad_status(i, &pad) == EMSCRIPTEN_RESULT_SUCCESS &&
								   pad.connected;
			GamepadState& out = state.gamepads[i];
			if (!connected) {
				if (out.connected) {
					out = {};
					PushConnectionEvent(i, false);
				}
				continue;
			}
			if (!out.connected) {
				out.connected = true;
				PushConnectionEvent(i, true);
			}
			if (!applicationFocused) {
				std::memset(out.buttons, 0, sizeof(out.buttons));
				std::memset(out.axes, 0, sizeof(out.axes));
				continue;
			}

			auto digital = [&](int idx) -> bool {
				return idx < pad.numButtons && pad.digitalButton[idx] != 0;
			};
			auto analog = [&](int idx) -> f32 {
				return idx < pad.numButtons ? static_cast<f32>(pad.analogButton[idx]) : 0.0f;
			};
			auto axis = [&](int idx) -> f32 {
				return idx < pad.numAxes ? static_cast<f32>(pad.axis[idx]) : 0.0f;
			};

			// W3C Standard Gamepad button layout
			out.buttons[static_cast<size_t>(GamepadButton::South)] = digital(0);
			out.buttons[static_cast<size_t>(GamepadButton::East)] = digital(1);
			out.buttons[static_cast<size_t>(GamepadButton::West)] = digital(2);
			out.buttons[static_cast<size_t>(GamepadButton::North)] = digital(3);
			out.buttons[static_cast<size_t>(GamepadButton::LeftBumper)] = digital(4);
			out.buttons[static_cast<size_t>(GamepadButton::RightBumper)] = digital(5);
			out.buttons[static_cast<size_t>(GamepadButton::Back)] = digital(8);
			out.buttons[static_cast<size_t>(GamepadButton::Start)] = digital(9);
			out.buttons[static_cast<size_t>(GamepadButton::LeftThumb)] = digital(10);
			out.buttons[static_cast<size_t>(GamepadButton::RightThumb)] = digital(11);
			out.buttons[static_cast<size_t>(GamepadButton::DpadUp)] = digital(12);
			out.buttons[static_cast<size_t>(GamepadButton::DpadDown)] = digital(13);
			out.buttons[static_cast<size_t>(GamepadButton::DpadLeft)] = digital(14);
			out.buttons[static_cast<size_t>(GamepadButton::DpadRight)] = digital(15);
			out.buttons[static_cast<size_t>(GamepadButton::Guide)] = digital(16);

			ApplyStickDeadzone(axis(0), -axis(1), LEFT_STICK_DEADZONE,
							   out.axes[static_cast<size_t>(GamepadAxis::LeftX)],
							   out.axes[static_cast<size_t>(GamepadAxis::LeftY)]);
			ApplyStickDeadzone(axis(2), -axis(3), RIGHT_STICK_DEADZONE,
							   out.axes[static_cast<size_t>(GamepadAxis::RightX)],
							   out.axes[static_cast<size_t>(GamepadAxis::RightY)]);
			out.axes[static_cast<size_t>(GamepadAxis::LeftTrigger)] = ApplyTriggerThreshold(analog(6));
			out.axes[static_cast<size_t>(GamepadAxis::RightTrigger)] = ApplyTriggerThreshold(analog(7));
		}
	}

} // namespace Shalltear::InputInternal
