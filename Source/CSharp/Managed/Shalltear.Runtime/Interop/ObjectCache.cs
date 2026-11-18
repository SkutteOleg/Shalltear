using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace Shalltear.Interop;

internal static class ScriptIndex<T> {
	internal static int value;
}

internal static class Wrapper<T>
	where T : EngineObject {
	internal static Func<ulong, T>? create;
	internal static ulong typeHash;
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

	internal static T? Find<T>(ulong id)
		where T : EngineObject {
		if (id == 0) {
			return null;
		}
		if (_wrappers.TryGetValue(id, out EngineObject? wrapper)) {
			return wrapper as T;
		}
		Func<ulong, T>? create = Wrapper<T>.create;
		if (create == null || !ObjectBindings.IsA(id, Wrapper<T>.typeHash)) {
			return null;
		}
		T made = create(id);
		_wrappers.Add(id, made);
		_ids.Add(id);
		return made;
	}

	static void Wrap<T>(string engineType, Func<ulong, T> create)
		where T : EngineObject {
		Wrapper<T>.create = create;
		Wrapper<T>.typeHash = Bridge.HashOf(engineType);
	}

	static ObjectCache() {
		Wrap(Shalltear.SceneObject.ENGINE_TYPE, static id => new SceneObject(id));
		Wrap(Material.ENGINE_TYPE, static id => new Material(id));
		Wrap(Mesh.ENGINE_TYPE, static id => new Mesh(id));
		Wrap(Texture.ENGINE_TYPE, static id => new Texture(id));
		Wrap(Prefab.ENGINE_TYPE, static id => new Prefab(id));
		Wrap(Sprite.ENGINE_TYPE, static id => new Sprite(id));
		Wrap(TextAsset.ENGINE_TYPE, static id => new TextAsset(id));
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
