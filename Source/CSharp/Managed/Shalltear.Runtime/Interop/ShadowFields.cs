using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace Shalltear.Interop;

/// <summary>Called by generated code.</summary>
[EditorBrowsable(EditorBrowsableState.Never)]
public static unsafe partial class ShadowFields {
#pragma warning disable CS1591
	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Write(nint field, bool value) => *(byte*)field = value ? (byte)1 : (byte)0;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Write(nint field, sbyte value) => *(sbyte*)field = value;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Write(nint field, byte value) => *(byte*)field = value;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Write(nint field, short value) => *(short*)field = value;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Write(nint field, ushort value) => *(ushort*)field = value;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Write(nint field, int value) => *(int*)field = value;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Write(nint field, uint value) => *(uint*)field = value;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Write(nint field, long value) => *(long*)field = value;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Write(nint field, ulong value) => *(ulong*)field = value;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Write(nint field, float value) => *(float*)field = value;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Write(nint field, double value) => *(double*)field = value;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Write(nint field, Vec2 value) => *(Vec2*)field = value;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Write(nint field, Vec3 value) => *(Vec3*)field = value;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Write(nint field, Vec4 value) => *(Vec4*)field = value;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Write(nint field, Quat value) => *(Quat*)field = value;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Write(nint field, Color value) => *(Color*)field = value;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Read(nint field, out bool value) => value = *(byte*)field != 0;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Read(nint field, out sbyte value) => value = *(sbyte*)field;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Read(nint field, out byte value) => value = *(byte*)field;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Read(nint field, out short value) => value = *(short*)field;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Read(nint field, out ushort value) => value = *(ushort*)field;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Read(nint field, out int value) => value = *(int*)field;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Read(nint field, out uint value) => value = *(uint*)field;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Read(nint field, out long value) => value = *(long*)field;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Read(nint field, out ulong value) => value = *(ulong*)field;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Read(nint field, out float value) => value = *(float*)field;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Read(nint field, out double value) => value = *(double*)field;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Read(nint field, out Vec2 value) => value = *(Vec2*)field;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Read(nint field, out Vec3 value) => value = *(Vec3*)field;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Read(nint field, out Vec4 value) => value = *(Vec4*)field;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Read(nint field, out Quat value) => value = *(Quat*)field;

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Read(nint field, out Color value) => value = *(Color*)field;
#pragma warning restore CS1591
}
