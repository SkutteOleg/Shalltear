using System;

namespace Shalltear;

/// <summary>Writer of a cooked record.</summary>
public unsafe partial class CookWriter {
	/// <summary>Writes the bytes of a value (a struct of plain fields).</summary>
	public void Write<T>(in T value)
		where T : unmanaged {
		fixed (T* data = &value) {
			WriteBytes((ulong)data, (ulong)sizeof(T));
		}
	}
}

/// <summary>Reader of a cooked record.</summary>
public unsafe partial class CookReader {
	/// <summary>Reads the bytes of a value (false past the end of the record).</summary>
	public bool Read<T>(out T value)
		where T : unmanaged {
		value = default;
		fixed (T* data = &value) {
			return ReadBytes((ulong)data, (ulong)sizeof(T));
		}
	}

	/// <summary>Fills a span with values (false past the end of the record).</summary>
	public bool ReadInto<T>(Span<T> values)
		where T : unmanaged {
		fixed (T* data = values) {
			return values.IsEmpty || ReadBytes((ulong)data, (ulong)values.Length * (ulong)sizeof(T));
		}
	}
}
