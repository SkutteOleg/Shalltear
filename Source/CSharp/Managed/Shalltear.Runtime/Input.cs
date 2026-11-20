using System;
using System.Buffers;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Shalltear;

public static partial class Input {
	// must match InputBindings.cpp
	/// <summary>Number of gamepad indices.</summary>
	public const int MAX_GAMEPADS = 4;

	const int STACK_EVENTS = 32;

	/// <summary>Gamepad at index (not connected past MAX_GAMEPADS).</summary>
	public static GamepadState GetGamepad(int index) {
		GamepadState state = default;
		FillGamepad(index, MemoryMarshal.CreateSpan(ref state, 1));
		return state;
	}

	/// <summary>Fill results with this frame's events in arrival order.</summary>
	[SkipLocalsInit]
	public static void GetFrameEvents(List<InputEvent> results) {
		ArgumentNullException.ThrowIfNull(results);
		results.Clear();
		Span<InputEvent> events = stackalloc InputEvent[STACK_EVENTS];
		InputEvent[]? rented = null;
		int count = FillFrameEvents(events);
		if (count > events.Length) {
			rented = ArrayPool<InputEvent>.Shared.Rent(count);
			events = rented;
			count = Math.Min(FillFrameEvents(events), events.Length);
		}
		foreach (InputEvent item in events.Slice(0, count)) {
			results.Add(item);
		}
		if (rented != null) {
			ArrayPool<InputEvent>.Shared.Return(rented);
		}
	}
}

public partial struct GamepadState {
	/// <summary>True while button is held.</summary>
	public readonly bool IsDown(GamepadButton button) {
		return (uint)button < 32 && (buttons & (1u << (int)button)) != 0;
	}

	/// <summary>Axis value (0 for an unknown axis).</summary>
	public readonly float GetAxis(GamepadAxis axis) {
		return axis switch {
			GamepadAxis.LeftX => leftX,
			GamepadAxis.LeftY => leftY,
			GamepadAxis.RightX => rightX,
			GamepadAxis.RightY => rightY,
			GamepadAxis.LeftTrigger => leftTrigger,
			GamepadAxis.RightTrigger => rightTrigger,
			_ => 0f,
		};
	}
}
