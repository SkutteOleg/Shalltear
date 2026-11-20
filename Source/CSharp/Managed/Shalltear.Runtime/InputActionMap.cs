using System;
using System.Collections.Generic;

using Shalltear.Interop;

namespace Shalltear;

/// <summary>Action of an InputActionMap.</summary>
public readonly struct ActionId : IEquatable<ActionId> {
	internal const uint INVALID = ~0u;

	internal readonly uint _value;

	internal ActionId(uint value) {
		_value = value;
	}

	/// <summary>No action (FindAction's answer for an unknown name).</summary>
	public static ActionId Invalid => new(INVALID);

	/// <summary>False for Invalid.</summary>
	public bool IsValid => _value != INVALID;

	/// <summary>Same action.</summary>
	public static bool operator ==(ActionId left, ActionId right) {
		return left._value == right._value;
	}

	/// <summary>Opposite of ==.</summary>
	public static bool operator !=(ActionId left, ActionId right) {
		return left._value != right._value;
	}

	/// <inheritdoc/>
	public bool Equals(ActionId other) {
		return _value == other._value;
	}

	/// <inheritdoc/>
	public override bool Equals(object? other) {
		return other is ActionId value && _value == value._value;
	}

	/// <inheritdoc/>
	public override int GetHashCode() {
		return (int)_value;
	}
}

/// <summary>1 key, button or axis feeding an action.</summary>
public struct InputBinding {
	// must match InputBinding::Source
	/// <summary>Device read by a binding.</summary>
	public enum Source : byte {
		/// <summary>code = Key.</summary>
		Key = 0,
		/// <summary>code = MouseButton.</summary>
		MouseButton = 1,
		/// <summary>code = 0 for x, 1 for y.</summary>
		MouseDelta = 2,
		/// <summary>code = 0 for x, 1 for y.</summary>
		MouseWheel = 3,
		/// <summary>code = GamepadButton.</summary>
		GamepadButton = 4,
		/// <summary>code = GamepadAxis.</summary>
		GamepadAxis = 5,
	}

	/// <summary>Device read.</summary>
	public Source source;
	/// <summary>Key, button or axis of that device.</summary>
	public ushort code;
	/// <summary>Modifiers to hold.</summary>
	public KeyMod requiredMods;
	/// <summary>True to refuse other held modifiers.</summary>
	public bool exactMods;
	/// <summary>Multiplier of the read value.</summary>
	public float scale;
	/// <summary>Axis2D component fed (0 = x, 1 = y).</summary>
	public byte component;
	/// <summary>Gamepad read by gamepad sources.</summary>
	public byte gamepadIndex;

	/// <summary>Key binding with scale 1.</summary>
	public InputBinding() {
		source = Source.Key;
		code = 0;
		requiredMods = KeyMod.None;
		exactMods = false;
		scale = 1f;
		component = 0;
		gamepadIndex = 0;
	}

	/// <summary>Binding of a keyboard key.</summary>
	public static InputBinding Key(Key key, float scale = 1f, int component = 0) {
		return new InputBinding { source = Source.Key, code = (ushort)key, scale = scale, component = (byte)component };
	}

	/// <summary>Binding of a mouse button.</summary>
	public static InputBinding MouseButton(MouseButton button, float scale = 1f, int component = 0) {
		return new InputBinding { source = Source.MouseButton, code = (ushort)button, scale = scale, component = (byte)component };
	}

	/// <summary>Binding of pointer movement (axis 0 = x, 1 = y).</summary>
	public static InputBinding MouseDelta(int axis, float scale = 1f, int component = 0) {
		return new InputBinding { source = Source.MouseDelta, code = (ushort)axis, scale = scale, component = (byte)component };
	}

	/// <summary>Binding of the wheel (axis 0 = x, 1 = y).</summary>
	public static InputBinding MouseWheel(int axis, float scale = 1f, int component = 0) {
		return new InputBinding { source = Source.MouseWheel, code = (ushort)axis, scale = scale, component = (byte)component };
	}

	/// <summary>Binding of a gamepad button.</summary>
	public static InputBinding GamepadButton(GamepadButton button, float scale = 1f, int component = 0, int gamepadIndex = 0) {
		return new InputBinding {
			source = Source.GamepadButton, code = (ushort)button, scale = scale, component = (byte)component, gamepadIndex = (byte)gamepadIndex
		};
	}

	/// <summary>Binding of a gamepad stick or trigger axis.</summary>
	public static InputBinding GamepadAxis(GamepadAxis axis, float scale = 1f, int component = 0, int gamepadIndex = 0) {
		return new InputBinding {
			source = Source.GamepadAxis, code = (ushort)axis, scale = scale, component = (byte)component, gamepadIndex = (byte)gamepadIndex
		};
	}
}

/// <summary>Set of actions, each with the keys/buttons/axes bound to it.</summary>
public sealed class InputActionMap : IDisposable {
	static readonly List<uint> _retired = new();

	uint _id;

	/// <summary>Empty map (disabled until Enable).</summary>
	public InputActionMap() {
		_id = InputMapBindings.Create();
	}

	/// <summary>Queue the map to be freed on main thread.</summary>
	~InputActionMap() {
		lock (_retired) {
			_retired.Add(_id);
		}
	}

	internal static int RetiredCount {
		get {
			lock (_retired) {
				return _retired.Count;
			}
		}
	}

	/// <summary>Free the map now. Later calls throw.</summary>
	public void Dispose() {
		if (_id != 0) {
			InputMapBindings.Destroy(_id);
			_id = 0;
		}
		GC.SuppressFinalize(this);
	}

	/// <summary>Add an action (existing one returned for same name + type).</summary>
	public ActionId RegisterAction(string name, ActionType type) {
		ArgumentNullException.ThrowIfNull(name);
		return new ActionId(InputMapBindings.RegisterAction(_id, name, type));
	}

	/// <summary>Action with this name (Invalid when none).</summary>
	public ActionId FindAction(string name) {
		ArgumentNullException.ThrowIfNull(name);
		return new ActionId(InputMapBindings.FindAction(_id, name));
	}

	/// <summary>Add a binding to action. Bindings of an action add up.</summary>
	public void Bind(ActionId action, InputBinding binding) {
		InputMapBindings.Bind(_id, action._value, (byte)binding.source, binding.code, binding.requiredMods, binding.exactMods, binding.scale,
							  binding.component, binding.gamepadIndex);
	}

	/// <summary>Remove all bindings of action.</summary>
	public void ClearBindings(ActionId action) {
		InputMapBindings.ClearBindings(_id, action._value);
	}

	/// <summary>Start reading input (Evaluate ignored while disabled).</summary>
	public void Enable() {
		InputMapBindings.SetEnabled(_id, true);
	}

	/// <summary>Stop reading input + reset all actions.</summary>
	public void Disable() {
		InputMapBindings.SetEnabled(_id, false);
	}

	/// <summary>Read this frame's input into the actions. Once per frame.</summary>
	public void Evaluate() {
		InputMapBindings.Evaluate(_id);
	}

	/// <summary>True while a binding of action is held.</summary>
	public bool IsPressed(ActionId action) {
		return InputMapBindings.IsPressed(_id, action._value);
	}

	/// <summary>True on the Evaluate where action went down.</summary>
	public bool WasPerformed(ActionId action) {
		return InputMapBindings.WasPerformed(_id, action._value);
	}

	/// <summary>Sum of an Axis1D action's bindings.</summary>
	public float GetAxis1D(ActionId action) {
		return InputMapBindings.GetAxis1D(_id, action._value);
	}

	/// <summary>Sum of an Axis2D action's bindings per component.</summary>
	public Vec2 GetAxis2D(ActionId action) {
		return InputMapBindings.GetAxis2D(_id, action._value);
	}

	/// <summary>Number of registered actions.</summary>
	public int ActionCount => InputMapBindings.GetActionCount(_id);

	internal static void FreeRetired() {
		lock (_retired) {
			foreach (uint id in _retired) {
				InputMapBindings.Destroy(id);
			}
			_retired.Clear();
		}
	}

	internal static void Forget() {
		lock (_retired) {
			_retired.Clear();
		}
	}
}
