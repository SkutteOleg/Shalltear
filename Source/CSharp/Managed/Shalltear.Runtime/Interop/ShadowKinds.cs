using System;
using System.Buffers;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Unicode;

namespace Shalltear.Interop;

public static unsafe partial class ShadowFields {
#pragma warning disable CS1591
	const int STACK_CHARS = 256;

	public static void Write(nint field, string? value, Behavior script, int cache) {
		string?[] synced = script._texts ??= new string?[script._scriptType!._texts];
		if (ReferenceEquals(synced[cache], value)) {
			return;
		}
		Write(field, value);
		synced[cache] = value;
	}

	public static void Read(nint field, ref string value, Behavior script, int cache) {
		string?[] synced = script._texts ??= new string?[script._scriptType!._texts];
		Read(field, ref value);
		synced[cache] = value;
	}

	[SkipLocalsInit]
	public static void Write(nint field, string? value) {
		using UTF8Scratch text = new(value ?? "", stackalloc byte[UTF8Scratch.STACK_BYTES]);
		Bridge.StringAssign(field, text.Text);
	}

	[SkipLocalsInit]
	public static void Read(nint field, ref string value) {
		SltString text = Bridge.StringRead(field);
		if (text.length == 0) {
			value = "";
			return;
		}
		ReadOnlySpan<byte> bytes = new(text.ptr, text.length);
		if (value != null && bytes.Length <= STACK_CHARS) {
			Span<char> chars = stackalloc char[STACK_CHARS];
			if (Utf8.ToUtf16(bytes, chars, out _, out int written) == OperationStatus.Done && chars.Slice(0, written).SequenceEqual(value)) {
				return;
			}
		}
		value = Encoding.UTF8.GetString(bytes);
	}

	[MethodImpl(MethodImplOptions.AggressiveInlining)]
	public static void Write(nint field, EngineObject? value) => *(ulong*)field = value is null ? 0UL : value._id;

	public static void ReadObject<T>(nint field, out T value)
		where T : EngineObject {
		value = ObjectCache.Find<T>(*(ulong*)field)!;
	}

	public static void ReadScript<T>(nint field, out T value, string member)
		where T : Component {
		value = ScriptOf<T>(*(ulong*)field, member)!;
	}

	static T? ScriptOf<T>(ulong id, string member)
		where T : Component {
		if (id == 0) {
			return null;
		}
		nint handle = Bridge.ScriptOf(id);
		object? script = handle == 0 ? null : GCHandle.FromIntPtr(handle).Target;
		if (script is T typed) {
			return typed;
		}
		if (script != null || ObjectBindings.IsAlive(id)) {
			string found = script != null ? script.GetType().Name : "another object";
			Log.Warn("C#", "'" + member + "' got " + found + " (read as null). Assign a " + typeof(T).Name);
		}
		return null;
	}

	public static nint Resize(nint field, int count) => Bridge.ListResize(field, count);

	public static nint Data(nint field, out int count) => Bridge.ListData(field, out count);

	public static Span<T> Prepare<T>(ref List<T> list, int count) {
		list ??= new List<T>(count);
		CollectionsMarshal.SetCount(list, count);
		return CollectionsMarshal.AsSpan(list);
	}

	public static Span<T> Prepare<T>(ref T[] array, int count) {
		if (array == null || array.Length != count) {
			array = count == 0 ? [] : new T[count];
		}
		return array;
	}

	public static void WriteValues<T>(nint field, List<T> list)
		where T : unmanaged {
		WriteValues<T>(field, CollectionsMarshal.AsSpan(list));
	}

	public static void WriteValues<T>(nint field, T[] array)
		where T : unmanaged {
		WriteValues<T>(field, array.AsSpan());
	}

	static void WriteValues<T>(nint field, ReadOnlySpan<T> items)
		where T : unmanaged {
		nint data = Bridge.ListResize(field, items.Length);
		items.CopyTo(new Span<T>((void*)data, items.Length));
	}

	public static void ReadValues<T>(nint field, ref List<T> list)
		where T : unmanaged {
		nint data = Bridge.ListData(field, out int count);
		new ReadOnlySpan<T>((void*)data, count).CopyTo(Prepare(ref list, count));
	}

	public static void ReadValues<T>(nint field, ref T[] array)
		where T : unmanaged {
		nint data = Bridge.ListData(field, out int count);
		new ReadOnlySpan<T>((void*)data, count).CopyTo(Prepare(ref array, count));
	}

	public static void WriteTexts(nint field, List<string> list) => WriteTexts(field, CollectionsMarshal.AsSpan(list));

	public static void WriteTexts(nint field, string[] array) => WriteTexts(field, array.AsSpan());

	static void WriteTexts(nint field, ReadOnlySpan<string> items) {
		nint data = Bridge.ListResize(field, items.Length);
		for (int index = 0; index < items.Length; ++index) {
			Write(data + index * Bridge.TextSize, items[index]);
		}
	}

	public static void ReadTexts(nint field, ref List<string> list) {
		nint data = Bridge.ListData(field, out int count);
		ReadTexts(data, Prepare(ref list, count));
	}

	public static void ReadTexts(nint field, ref string[] array) {
		nint data = Bridge.ListData(field, out int count);
		ReadTexts(data, Prepare(ref array, count));
	}

	static void ReadTexts(nint data, Span<string> target) {
		for (int index = 0; index < target.Length; ++index) {
			Read(data + index * Bridge.TextSize, ref target[index]);
		}
	}

	public static void WriteObjects<T>(nint field, List<T> list)
		where T : EngineObject {
		WriteObjects<T>(field, CollectionsMarshal.AsSpan(list));
	}

	public static void WriteObjects<T>(nint field, T[] array)
		where T : EngineObject {
		WriteObjects<T>(field, array.AsSpan());
	}

	static void WriteObjects<T>(nint field, ReadOnlySpan<T> items)
		where T : EngineObject {
		ulong* data = (ulong*)Bridge.ListResize(field, items.Length);
		for (int index = 0; index < items.Length; ++index) {
			data[index] = items[index] is null ? 0UL : items[index]._id;
		}
	}

	public static void ReadObjects<T>(nint field, ref List<T> list)
		where T : EngineObject {
		ulong* data = (ulong*)Bridge.ListData(field, out int count);
		FindObjects(data, Prepare(ref list, count));
	}

	public static void ReadObjects<T>(nint field, ref T[] array)
		where T : EngineObject {
		ulong* data = (ulong*)Bridge.ListData(field, out int count);
		FindObjects(data, Prepare(ref array, count));
	}

	static void FindObjects<T>(ulong* data, Span<T> target)
		where T : EngineObject {
		for (int index = 0; index < target.Length; ++index) {
			target[index] = ObjectCache.Find<T>(data[index])!;
		}
	}

	public static void ReadScripts<T>(nint field, ref List<T> list, string member)
		where T : Component {
		ulong* data = (ulong*)Bridge.ListData(field, out int count);
		FindScripts(data, Prepare(ref list, count), member);
	}

	public static void ReadScripts<T>(nint field, ref T[] array, string member)
		where T : Component {
		ulong* data = (ulong*)Bridge.ListData(field, out int count);
		FindScripts(data, Prepare(ref array, count), member);
	}

	static void FindScripts<T>(ulong* data, Span<T> target, string member)
		where T : Component {
		for (int index = 0; index < target.Length; ++index) {
			target[index] = ScriptOf<T>(data[index], member)!;
		}
	}
#pragma warning restore CS1591
}
