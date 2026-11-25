using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Shalltear.Interop;

// must match ScriptFunctionDetail::Slot in ScriptFunctions.h and Slots in SlotSupport.h
internal static unsafe class SlotValues {
	internal static void Put(ulong* slots, Vec2 value) {
		slots[0] = NativeSlots.Pack(value.x, value.y);
	}

	internal static void Put(ulong* slots, Vec3 value) {
		slots[0] = NativeSlots.Pack(value.x, value.y);
		slots[1] = NativeSlots.Pack(value.z, 0f);
	}

	internal static void Put(ulong* slots, Vec4 value) {
		slots[0] = NativeSlots.Pack(value.x, value.y);
		slots[1] = NativeSlots.Pack(value.z, value.w);
	}

	internal static void Put(ulong* slots, Quat value) {
		slots[0] = NativeSlots.Pack(value.x, value.y);
		slots[1] = NativeSlots.Pack(value.z, value.w);
	}

	internal static void Put(ulong* slots, Color value) {
		slots[0] = NativeSlots.Pack(value.r, value.g);
		slots[1] = NativeSlots.Pack(value.b, value.a);
	}

	internal static void Put(ulong* slots, Vec2I value) {
		slots[0] = (uint)value.x | ((ulong)(uint)value.y << 32);
	}

	internal static Vec2 Vec2(ulong* slots) {
		return new Vec2(NativeSlots.Low(slots[0]), NativeSlots.High(slots[0]));
	}

	internal static Vec3 Vec3(ulong* slots) {
		return new Vec3(NativeSlots.Low(slots[0]), NativeSlots.High(slots[0]), NativeSlots.Low(slots[1]));
	}

	internal static Vec4 Vec4(ulong* slots) {
		return new Vec4(NativeSlots.Low(slots[0]), NativeSlots.High(slots[0]), NativeSlots.Low(slots[1]), NativeSlots.High(slots[1]));
	}

	internal static Quat Quat(ulong* slots) {
		return new Quat(NativeSlots.Low(slots[0]), NativeSlots.High(slots[0]), NativeSlots.Low(slots[1]), NativeSlots.High(slots[1]));
	}

	internal static Color Color(ulong* slots) {
		return new Color(NativeSlots.Low(slots[0]), NativeSlots.High(slots[0]), NativeSlots.Low(slots[1]), NativeSlots.High(slots[1]));
	}

	internal static Vec2I Vec2I(ulong* slots) {
		return new Vec2I((int)(uint)slots[0], (int)(uint)(slots[0] >> 32));
	}

	internal static string Text(ulong* slots) {
		return slots[1] == 0 ? "" : Encoding.UTF8.GetString((byte*)(nint)slots[0], (int)slots[1]);
	}

	internal static T[] Array<T>(ulong* slots)
		where T : unmanaged {
		int count = (int)slots[1];
		if (count == 0) {
			return System.Array.Empty<T>();
		}
		T[] made = new T[count];
		new ReadOnlySpan<T>((void*)(nint)slots[0], count).CopyTo(made);
		return made;
	}

	internal static T[] Objects<T>(ulong* slots)
		where T : EngineObject {
		int count = (int)slots[1];
		if (count == 0) {
			return System.Array.Empty<T>();
		}
		ulong* ids = (ulong*)(nint)slots[0];
		T[] made = new T[count];
		for (int index = 0; index < count; ++index) {
			made[index] = ObjectCache.Find<T>(ids[index])!;
		}
		return made;
	}

	internal static string[] Texts(ulong* slots) {
		int count = (int)slots[1];
		if (count == 0) {
			return System.Array.Empty<string>();
		}
		ulong* pairs = (ulong*)(nint)slots[0];
		string[] made = new string[count];
		for (int index = 0; index < count; ++index) {
			made[index] = Text(pairs + index * 2);
		}
		return made;
	}

	internal static T[] Natives<T>(ulong* slots, Func<nint, bool, T> make, bool owned, object? owner)
		where T : NativeObject {
		int count = (int)slots[1];
		if (count == 0) {
			return System.Array.Empty<T>();
		}
		ulong* pointers = (ulong*)(nint)slots[0];
		T[] made = new T[count];
		for (int index = 0; index < count; ++index) {
			made[index] = make((nint)pointers[index], owned);
			made[index]._owner = owner;
		}
		return made;
	}
}

internal unsafe ref struct SlotList {
	void* _data;

	internal readonly void* Data => _data;

	internal static SlotList OfObjects<T>(ReadOnlySpan<T> items)
		where T : EngineObject? {
		SlotList list = default;
		if (items.Length == 0) {
			return list;
		}
		ulong* ids = (ulong*)NativeMemory.Alloc((nuint)items.Length, sizeof(ulong));
		for (int index = 0; index < items.Length; ++index) {
			ids[index] = items[index] is null ? 0UL : items[index]!._id;
		}
		list._data = ids;
		return list;
	}

	internal static SlotList OfNatives<T>(ReadOnlySpan<T> items)
		where T : NativeObject? {
		SlotList list = default;
		if (items.Length == 0) {
			return list;
		}
		nint* pointers = (nint*)NativeMemory.Alloc((nuint)items.Length, (nuint)sizeof(nint));
		for (int index = 0; index < items.Length; ++index) {
			pointers[index] = items[index] is null ? 0 : (nint)(nuint)items[index]!.Pointer;
		}
		list._data = pointers;
		return list;
	}

	internal static SlotList OfTexts(ReadOnlySpan<string> items) {
		SlotList list = default;
		if (items.Length == 0) {
			return list;
		}
		int bytes = 0;
		for (int index = 0; index < items.Length; ++index) {
			bytes += Encoding.UTF8.GetByteCount(items[index] ?? "");
		}
		ulong* pairs = (ulong*)NativeMemory.Alloc((nuint)(items.Length * 2 * sizeof(ulong) + bytes));
		byte* text = (byte*)(pairs + items.Length * 2);
		int left = bytes;
		for (int index = 0; index < items.Length; ++index) {
			int written = Encoding.UTF8.GetBytes(items[index] ?? "", new Span<byte>(text, left));
			pairs[index * 2] = (ulong)text;
			pairs[index * 2 + 1] = (ulong)written;
			text += written;
			left -= written;
		}
		list._data = pairs;
		return list;
	}

	public void Dispose() {
		if (_data != null) {
			NativeMemory.Free(_data);
			_data = null;
		}
	}
}
