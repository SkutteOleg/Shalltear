using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

using Shalltear.Interop;

namespace Shalltear.Conformance;

static class InputProbe {
	static InputActionMap? _kept;

	[MethodImpl(MethodImplOptions.NoInlining)]
	static void Drop(int count) {
		for (int index = 0; index < count; ++index) {
			InputActionMap dropped = new();
			dropped.RegisterAction("Dropped", ActionType.Button);
		}
	}

	[ConformanceTest]
	internal static void KeepsAMap() {
		_kept = new InputActionMap();
		_kept.RegisterAction("Kept", ActionType.Button);
	}

	[ConformanceTest]
	internal static void DropsMaps() {
		Drop(20);
		GC.Collect();
		GC.WaitForPendingFinalizers();
	}
}

static class InputTests {
	const int POST_KEY = 0;
	const int POST_MOUSE_BUTTON = 1;
	const int POST_MOUSE_MOVE = 2;
	const int POST_WHEEL = 3;
	const int POST_TEXT = 4;
	const int PUMP = 5;
	const int TYPED = 0x416;

	static void Pump() {
		BindingProbe.Drive(PUMP, 0, 0);
	}

	static void Post(Key key, bool down) {
		BindingProbe.Drive(POST_KEY, (int)key, down ? 1 : 0);
	}

	static void Post(MouseButton button, bool down) {
		BindingProbe.Drive(POST_MOUSE_BUTTON, (int)button, down ? 1 : 0);
	}

	static void Release(params Key[] keys) {
		foreach (Key key in keys) {
			Post(key, false);
		}
		Pump();
		Pump();
	}

	[MethodImpl(MethodImplOptions.NoInlining)]
	static void MakeAndDrop(int count) {
		for (int index = 0; index < count; ++index) {
			InputActionMap dropped = new();
			dropped.RegisterAction("Dropped", ActionType.Button);
		}
	}

	[ConformanceTest]
	internal static void KeysArePolledPerFrame() {
		Pump();
		Check.True(!Input.IsKeyDown(Key.G) && !Input.WasKeyPressed(Key.G) && !Input.WasKeyReleased(Key.G), "key down before any press");
		Post(Key.G, true);
		Pump();
		Check.True(Input.IsKeyDown(Key.G) && Input.WasKeyPressed(Key.G) && !Input.WasKeyReleased(Key.G), "press not seen");
		Check.True(!Input.IsKeyDown(Key.H), "another key reads down");
		Pump();
		Check.True(Input.IsKeyDown(Key.G) && !Input.WasKeyPressed(Key.G), "press edge lasted 2 frames");

		Check.Equal(KeyMod.None, Input.KeyMods);
		Post(Key.LeftShift, true);
		Post(Key.LeftCtrl, true);
		Pump();
		Check.Equal(KeyMod.Shift | KeyMod.Ctrl, Input.KeyMods);

		Post(Key.G, false);
		Post(Key.LeftShift, false);
		Post(Key.LeftCtrl, false);
		Pump();
		Check.True(!Input.IsKeyDown(Key.G) && Input.WasKeyReleased(Key.G), "release not seen");
		Check.Equal(KeyMod.None, Input.KeyMods);
		Pump();
		Check.True(!Input.WasKeyReleased(Key.G), "release edge lasted 2 frames");
		Check.True(!Input.IsKeyDown((Key)60000) && !Input.WasKeyPressed((Key)60000) && !Input.WasKeyReleased((Key)60000), "unknown key reads down");
	}

	[ConformanceTest]
	internal static void MouseIsPolledPerFrame() {
		BindingProbe.Drive(POST_MOUSE_MOVE, 40, 30);
		Pump();
		Vec2I first = Input.MousePosition;
		BindingProbe.Drive(POST_MOUSE_MOVE, 50, 45);
		Pump();
		Check.Equal(new Vec2I(10, 15), Input.MousePosition - first);
		Check.Equal(new Vec2(10f, 15f), Input.MouseDelta);
		Pump();
		Check.Equal(Vec2.Zero, Input.MouseDelta);

		Post(MouseButton.Right, true);
		Pump();
		Check.True(Input.IsMouseButtonDown(MouseButton.Right) && Input.WasMouseButtonPressed(MouseButton.Right), "press not seen");
		Check.True(!Input.IsMouseButtonDown(MouseButton.Left) && !Input.IsMouseButtonDown((MouseButton)200), "another button reads down");
		Post(MouseButton.Right, false);
		Pump();
		Check.True(!Input.IsMouseButtonDown(MouseButton.Right) && Input.WasMouseButtonReleased(MouseButton.Right), "release not seen");

		BindingProbe.Drive(POST_WHEEL, 0, 2);
		Pump();
		Check.Equal(new Vec2(0f, 2f), Input.MouseWheel);
		Pump();
		Check.Equal(Vec2.Zero, Input.MouseWheel);
	}

	[ConformanceTest]
	internal static void FrameEventsAreListedInOrder() {
		Pump();
		Post(Key.A, true);
		BindingProbe.Drive(POST_TEXT, TYPED, 0);
		Post(MouseButton.Left, true);
		Post(Key.A, false);
		Post(MouseButton.Left, false);
		Pump();
		List<InputEvent> events = new();
		Input.GetFrameEvents(events);
		List<string> seen = new();
		foreach (InputEvent item in events) {
			switch (item.type) {
			case InputEventType.KeyDown when item.key == Key.A:
				seen.Add("down " + (int)item.key + (item.repeat ? " repeat" : ""));
				break;
			case InputEventType.KeyUp when item.key == Key.A:
				seen.Add("up " + (int)item.key);
				break;
			case InputEventType.Text when item.codepoint == TYPED:
				seen.Add("text");
				break;
			case InputEventType.MouseDown:
				seen.Add("button " + (int)item.button);
				break;
			case InputEventType.MouseUp:
				seen.Add("release " + (int)item.button);
				break;
			}
		}
		Check.Equal("down " + (int)Key.A + "|text|button 0|up " + (int)Key.A + "|release 0", string.Join("|", seen));
		Pump();
		Input.GetFrameEvents(events);
		Check.Equal(0, events.Count);
	}

	[ConformanceTest]
	internal static void GamepadWithoutDeviceReadsZero() {
		Check.Equal(4, Input.MAX_GAMEPADS);
		foreach (int index in new[] { -1, Input.MAX_GAMEPADS, 99 }) {
			GamepadState missing = Input.GetGamepad(index);
			Check.True(!missing.connected && missing.buttons == 0, "gamepad past the last index");
			Check.True(!missing.IsDown(GamepadButton.South) && missing.GetAxis(GamepadAxis.LeftX) == 0f, "missing gamepad reads input");
		}
		for (int index = 0; index < Input.MAX_GAMEPADS; ++index) {
			GamepadState state = Input.GetGamepad(index);
			Check.True(state.connected || (state.buttons == 0 && state.leftX == 0f && state.rightTrigger == 0f), "disconnected gamepad reads input");
			Check.True(!state.IsDown((GamepadButton)200) && state.GetAxis((GamepadAxis)99) == 0f, "unknown control reads input");
		}
	}

	[ConformanceTest]
	internal static void ActionMapFollowsBindings() {
		using InputActionMap map = new();
		ActionId fire = map.RegisterAction("Fire", ActionType.Button);
		map.Bind(fire, InputBinding.Key(Key.F));
		ActionId move = map.RegisterAction("Move", ActionType.Axis2D);
		map.Bind(move, InputBinding.Key(Key.D));
		map.Bind(move, InputBinding.Key(Key.A, -1f));
		map.Bind(move, InputBinding.Key(Key.W, 1f, 1));
		ActionId zoom = map.RegisterAction("Zoom", ActionType.Axis1D);
		map.Bind(zoom, InputBinding.MouseWheel(1, 0.5f));
		Check.Equal(3, map.ActionCount);
		Check.True(fire.IsValid && fire != move && map.FindAction("Move") == move, "actions aren't apart");
		Check.True(!map.FindAction("Jump").IsValid && map.FindAction("Jump") == ActionId.Invalid, "unknown action found");
		Check.True(map.RegisterAction("Fire", ActionType.Button) == fire, "same name made a 2nd action");

		Post(Key.F, true);
		Pump();
		map.Evaluate();
		Check.True(!map.IsPressed(fire), "disabled map read input");
		map.Enable();
		Pump();
		map.Evaluate();
		Check.True(map.IsPressed(fire) && !map.WasPerformed(fire), "key held at Enable");
		Post(Key.F, false);
		Pump();
		map.Evaluate();
		Check.True(!map.IsPressed(fire), "released key still pressed");
		Post(Key.F, true);
		Pump();
		map.Evaluate();
		Check.True(map.IsPressed(fire) && map.WasPerformed(fire), "press not performed");
		Pump();
		map.Evaluate();
		Check.True(map.IsPressed(fire) && !map.WasPerformed(fire), "performed lasted 2 frames");

		Post(Key.D, true);
		Post(Key.W, true);
		Pump();
		map.Evaluate();
		Check.Equal(new Vec2(1f, 1f), map.GetAxis2D(move));
		Post(Key.A, true);
		Pump();
		map.Evaluate();
		Check.Equal(new Vec2(0f, 1f), map.GetAxis2D(move));
		BindingProbe.Drive(POST_WHEEL, 0, 2);
		Pump();
		map.Evaluate();
		Check.Equal(1f, map.GetAxis1D(zoom));

		map.ClearBindings(move);
		Pump();
		map.Evaluate();
		Check.Equal(Vec2.Zero, map.GetAxis2D(move));
		map.Disable();
		Check.True(!map.IsPressed(fire) && map.GetAxis1D(zoom) == 0f, "disabled map kept its state");
		Release(Key.F, Key.D, Key.W, Key.A);
	}

	[ConformanceTest]
	internal static void ActionMapMistakesAreRefused() {
		InputActionMap map = new();
		InputActionMap other = new();
		ActionId fire = map.RegisterAction("Fire", ActionType.Button);
		Check.Equal("InputActionMap.RegisterAction got 'Fire' again with another type",
					ObjectTests.Refusal(() => map.RegisterAction("Fire", ActionType.Axis1D)));
		Check.Equal("InputActionMap.Bind got an action of another map. Use RegisterAction's result",
					ObjectTests.Refusal(() => other.Bind(fire, InputBinding.Key(Key.F))));
		Check.Equal("InputActionMap.Bind got an action of another map. Use RegisterAction's result",
					ObjectTests.Refusal(() => map.Bind(ActionId.Invalid, InputBinding.Key(Key.F))));
		Check.Equal("InputActionMap.Bind got an unknown InputBinding.Source",
					ObjectTests.Refusal(() => map.Bind(fire, new InputBinding { source = (InputBinding.Source)9 })));
		Check.Equal(1f, new InputBinding().scale);

		map.Dispose();
		map.Dispose();
		other.Dispose();
		Check.Equal("InputActionMap.Evaluate used after Dispose", ObjectTests.Refusal(map.Evaluate));
		Check.Equal("InputActionMap.Enable used after Dispose", ObjectTests.Refusal(map.Enable));
		Check.True(!map.IsPressed(fire) && map.GetAxis2D(fire) == Vec2.Zero && map.ActionCount == 0, "disposed map reads input");
	}

	[ConformanceTest]
	internal static void DroppedMapsAreFreedOnMainThread() {
		InputActionMap.FreeRetired();
		MakeAndDrop(50);
		GC.Collect();
		GC.WaitForPendingFinalizers();
		Check.True(InputActionMap.RetiredCount > 0, "dropped maps weren't queued");
		InputActionMap.FreeRetired();
		Check.Equal(0, InputActionMap.RetiredCount);
		using InputActionMap kept = new();
		Check.True(kept.RegisterAction("Kept", ActionType.Button).IsValid, "map made after a sweep is broken");
	}

	[ConformanceTest]
	internal static void RepeatedPollsAllocateNothing() {
		using InputActionMap map = new();
		ActionId fire = map.RegisterAction("Fire", ActionType.Button);
		map.Bind(fire, InputBinding.Key(Key.F));
		ActionId move = map.RegisterAction("Move", ActionType.Axis2D);
		map.Bind(move, InputBinding.GamepadAxis(GamepadAxis.LeftX));
		map.Enable();
		List<InputEvent> events = new(64);
		Post(Key.F, true);
		Pump();
		Input.GetFrameEvents(events);
		map.Evaluate();

		long before = GC.GetAllocatedBytesForCurrentThread();
		int hits = 0;
		for (int round = 0; round < 1000; ++round) {
			hits += Input.IsKeyDown(Key.F) ? 1 : 0;
			hits += Input.WasKeyPressed(Key.H) || Input.WasKeyReleased(Key.H) ? 0 : 1;
			hits += Input.KeyMods == KeyMod.None ? 1 : 0;
			hits += Input.IsMouseButtonDown(MouseButton.Left) || Input.WasMouseButtonPressed(MouseButton.Left) ? 0 : 1;
			hits += Input.MousePosition == Input.MousePosition && Input.MouseDelta == Vec2.Zero && Input.MouseWheel == Vec2.Zero ? 1 : 0;
			hits += Input.GetGamepad(0).IsDown((GamepadButton)31) ? 0 : 1;
			Input.GetFrameEvents(events);
			map.Evaluate();
			hits += map.IsPressed(fire) && !map.WasPerformed(move) ? 1 : 0;
			hits += map.GetAxis1D(fire) == 0f && map.GetAxis2D(move).y == 0f ? 1 : 0;
		}
		long allocated = GC.GetAllocatedBytesForCurrentThread() - before;
		Check.Equal(8000, hits);
		Check.Equal(0L, allocated);
		Release(Key.F);
	}
}
