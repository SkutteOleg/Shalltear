using System;
using System.ComponentModel;

namespace Shalltear.Interop;

/// <summary>Called by generated code (base of a wrapper over a C++ component of the project).</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public abstract unsafe class NativeComponent : Component {
	// must match SltFieldKind in ScriptBridge.h
	const int KIND_BOOL = 0;
	const int KIND_F32 = 9;
	const int KIND_F64 = 10;
	const int KIND_VEC2 = 11;
	const int KIND_VEC3 = 12;
	const int KIND_VEC4 = 13;
	const int KIND_QUAT = 14;
	const int KIND_COLOR = 15;
	const int KIND_OBJECT = 18;

	/// <summary>Called by generated code.</summary>
	protected NativeComponent() {}

	/// <summary>Registers 1 wrapper class.</summary>
	protected static void Register<T>(string engineType, Func<T> create)
		where T : NativeComponent {
		if (!Bridge.IsAttached) {
			return;
		}
		ObjectCache.WrapUser(engineType, id => {
			T made = create();
			made._id = id;
			return made;
		});
	}

	ulong Read(ulong type, ulong field, int kind, out ulong second) {
		ulong* slots = stackalloc ulong[2];
		NativeFieldBindings.Read(this, type, field, kind, new Span<ulong>(slots, 2));
		second = slots[1];
		return slots[0];
	}

	void Write(ulong type, ulong field, int kind, ulong first, ulong second = 0) {
		ulong* slots = stackalloc ulong[2];
		slots[0] = first;
		slots[1] = second;
		NativeFieldBindings.Write(this, type, field, kind, new ReadOnlySpan<ulong>(slots, 2));
	}

	/// <summary>Reads a field.</summary>
	protected bool GetBoolean(ulong type, ulong field) {
		return Read(type, field, KIND_BOOL, out _) != 0;
	}

	/// <summary>Reads an integer/enum field of the given kind.</summary>
	protected long GetInteger(ulong type, ulong field, int kind) {
		return (long)Read(type, field, kind, out _);
	}

	/// <summary>Reads a field.</summary>
	protected float GetSingle(ulong type, ulong field) {
		return NativeSlots.Low(Read(type, field, KIND_F32, out _));
	}

	/// <summary>Reads a field.</summary>
	protected double GetDouble(ulong type, ulong field) {
		return BitConverter.UInt64BitsToDouble(Read(type, field, KIND_F64, out _));
	}

	/// <summary>Reads a field.</summary>
	protected Vec2 GetVec2(ulong type, ulong field) {
		ulong first = Read(type, field, KIND_VEC2, out _);
		return new Vec2(NativeSlots.Low(first), NativeSlots.High(first));
	}

	/// <summary>Reads a field.</summary>
	protected Vec3 GetVec3(ulong type, ulong field) {
		ulong first = Read(type, field, KIND_VEC3, out ulong second);
		return new Vec3(NativeSlots.Low(first), NativeSlots.High(first), NativeSlots.Low(second));
	}

	/// <summary>Reads a field.</summary>
	protected Vec4 GetVec4(ulong type, ulong field) {
		ulong first = Read(type, field, KIND_VEC4, out ulong second);
		return new Vec4(NativeSlots.Low(first), NativeSlots.High(first), NativeSlots.Low(second), NativeSlots.High(second));
	}

	/// <summary>Reads a field.</summary>
	protected Quat GetQuat(ulong type, ulong field) {
		ulong first = Read(type, field, KIND_QUAT, out ulong second);
		return new Quat(NativeSlots.Low(first), NativeSlots.High(first), NativeSlots.Low(second), NativeSlots.High(second));
	}

	/// <summary>Reads a field.</summary>
	protected Color GetColor(ulong type, ulong field) {
		ulong first = Read(type, field, KIND_COLOR, out ulong second);
		return new Color(NativeSlots.Low(first), NativeSlots.High(first), NativeSlots.Low(second), NativeSlots.High(second));
	}

	/// <summary>Reads a field.</summary>
	protected string GetText(ulong type, ulong field) {
		return NativeFieldBindings.ReadText(this, type, field);
	}

	/// <summary>Reads a field.</summary>
	protected T? GetObject<T>(ulong type, ulong field)
		where T : EngineObject {
		return ObjectCache.Find<T>(Read(type, field, KIND_OBJECT, out _));
	}

	/// <summary>Writes a field.</summary>
	protected void Set(ulong type, ulong field, bool value) {
		Write(type, field, KIND_BOOL, value ? 1UL : 0UL);
	}

	/// <summary>Writes an integer/enum field of the given kind.</summary>
	protected void SetInteger(ulong type, ulong field, int kind, long value) {
		Write(type, field, kind, (ulong)value);
	}

	/// <summary>Writes a field.</summary>
	protected void Set(ulong type, ulong field, float value) {
		Write(type, field, KIND_F32, BitConverter.SingleToUInt32Bits(value));
	}

	/// <summary>Writes a field.</summary>
	protected void Set(ulong type, ulong field, double value) {
		Write(type, field, KIND_F64, BitConverter.DoubleToUInt64Bits(value));
	}

	/// <summary>Writes a field.</summary>
	protected void Set(ulong type, ulong field, Vec2 value) {
		Write(type, field, KIND_VEC2, NativeSlots.Pack(value.x, value.y));
	}

	/// <summary>Writes a field.</summary>
	protected void Set(ulong type, ulong field, Vec3 value) {
		Write(type, field, KIND_VEC3, NativeSlots.Pack(value.x, value.y), NativeSlots.Pack(value.z, 0f));
	}

	/// <summary>Writes a field.</summary>
	protected void Set(ulong type, ulong field, Vec4 value) {
		Write(type, field, KIND_VEC4, NativeSlots.Pack(value.x, value.y), NativeSlots.Pack(value.z, value.w));
	}

	/// <summary>Writes a field.</summary>
	protected void Set(ulong type, ulong field, Quat value) {
		Write(type, field, KIND_QUAT, NativeSlots.Pack(value.x, value.y), NativeSlots.Pack(value.z, value.w));
	}

	/// <summary>Writes a field.</summary>
	protected void Set(ulong type, ulong field, Color value) {
		Write(type, field, KIND_COLOR, NativeSlots.Pack(value.r, value.g), NativeSlots.Pack(value.b, value.a));
	}

	/// <summary>Writes a field.</summary>
	protected void SetText(ulong type, ulong field, string? value) {
		NativeFieldBindings.WriteText(this, type, field, value ?? "");
	}

	/// <summary>Writes a field.</summary>
	protected void SetObject(ulong type, ulong field, EngineObject? value) {
		Write(type, field, KIND_OBJECT, value is null ? 0UL : value._id);
	}

	/// <summary>Calls a method without parameters.</summary>
	protected void Invoke(ulong type, ulong method) {
		NativeFieldBindings.Invoke(this, type, method);
	}
}
