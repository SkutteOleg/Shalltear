using System;

using Shalltear.Interop;

namespace Shalltear;

public static partial class AssetDatabase {
	/// <summary>Load the asset at path or take 1 more reference to it (null when no file/not a T). Release with Unload.</summary>
	public static T? Load<T>(string path)
		where T : Asset {
		ulong id = LoadKey(path);
		T? found = ObjectCache.Find<T>(id);
		if (id != 0 && found is null) {
			UnloadKey(id);
		}
		return found;
	}

	/// <summary>Release a reference from Load/Create (asset unloaded with the last one).</summary>
	public static void Unload(Asset asset) {
		ArgumentNullException.ThrowIfNull(asset);
		UnloadKey(asset._id);
	}

	/// <summary>Write asset to its file.</summary>
	public static void Save(Asset asset) {
		ArgumentNullException.ThrowIfNull(asset);
		SaveKey(asset._id);
	}

	/// <summary>Make a file of a new T at path + load it, like the Create menu (release with Unload).</summary>
	public static T Create<T>(string path)
		where T : Asset {
		ulong type = ScriptTypeHash<T>.value != 0 ? ScriptTypeHash<T>.value : EngineTypeOf<T>.hash;
		return ObjectCache.Find<T>(CreateKey(path, type)) ?? throw new EngineException("AssetDatabase.Create couldn't make " + path + ". See the log");
	}
}
