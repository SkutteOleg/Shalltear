using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace Shalltear.Interop;

internal static class ScriptIndex<T> {
	internal static int value;
}

internal static class ObjectCache {
	internal const int SLICE = 256;

	static readonly Dictionary<ulong, EngineObject> _wrappers = new();
	static readonly List<ulong> _ids = new();
	static int _cursor;

	internal static int Count => _ids.Count;
	internal static int WrapperCount => _wrappers.Count;

	internal static SceneObject? SceneObject(ulong id) {
		if (id == 0) {
			return null;
		}
		if (!_wrappers.TryGetValue(id, out EngineObject? wrapper)) {
			wrapper = new SceneObject(id);
			_wrappers.Add(id, wrapper);
			_ids.Add(id);
		}
		return (SceneObject)wrapper;
	}

	internal static int Sweep() {
		if (_cursor >= _ids.Count) {
			_cursor = 0;
		}
		int length = Math.Min(SLICE, _ids.Count - _cursor);
		if (length == 0) {
			return 0;
		}
		Span<uint> alive = stackalloc uint[(SLICE + 31) / 32];
		ObjectBindings.AreAlive(CollectionsMarshal.AsSpan(_ids).Slice(_cursor, length), alive);
		int kept = 0;
		for (int index = 0; index < length; ++index) {
			ulong id = _ids[_cursor + index];
			if ((alive[index / 32] & (1u << (index % 32))) != 0) {
				_ids[_cursor + kept++] = id;
			} else {
				_wrappers.Remove(id);
			}
		}
		int removed = length - kept;
		int moved = Math.Min(removed, _ids.Count - _cursor - length);
		for (int index = 0; index < moved; ++index) {
			_ids[_cursor + kept + index] = _ids[_ids.Count - 1 - index];
		}
		_ids.RemoveRange(_ids.Count - removed, removed);
		_cursor += kept;
		return length;
	}

	internal static void Clear() {
		_wrappers.Clear();
		_ids.Clear();
		_cursor = 0;
	}
}
