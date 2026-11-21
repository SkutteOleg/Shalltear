using Shalltear.Interop;

namespace Shalltear;

/// <summary>Asset loaded by engine.</summary>
public abstract partial class Asset : EngineObject {
	private protected Asset() {}

	/// <summary>Project path of the asset's file ("" for one made in memory).</summary>
	public string Path => AssetBindings.GetPath(_id);

	/// <summary>Id of the asset's file as text ("" for one made in memory).</summary>
	public string AssetId => AssetBindings.GetAssetId(_id);

	/// <summary>Where the asset is in loading.</summary>
	public AssetLoadState LoadState => AssetBindings.GetLoadState(_id);

	/// <summary>Number of references keeping the asset loaded.</summary>
	public int RefCount => AssetBindings.GetRefCount(_id);

	/// <summary>Create an in-memory transient asset of type T (not owned by AssetDatabase).</summary>
	public static T CreateTransient<T>(string name = "")
		where T : Asset {
		ulong typeHash = EngineTypeOf<T>.hash;
		if (typeHash == 0) {
			typeHash = ScriptTypeHash<T>.value;
		}
		if (typeHash == 0) {
			throw new EngineException("Asset.CreateTransient can't create " + typeof(T).Name + " (not a registered asset type)");
		}
		ulong id = ProceduralAssetBindings.CreateTransient(typeHash, name ?? string.Empty);
		return ObjectCache.Find<T>(id)
			?? throw new EngineException("Asset.CreateTransient couldn't wrap " + typeof(T).Name);
	}

	/// <summary>Destroy an in-memory transient asset created by CreateTransient.</summary>
	public static void DestroyTransient(Asset? asset) {
		if (asset is null) {
			return;
		}
		ProceduralAssetBindings.DestroyTransient(asset._id);
	}
}

/// <summary>Material asset.</summary>
[EngineType(ENGINE_TYPE)]
public sealed partial class Material : Asset {
	internal const string ENGINE_TYPE = "MaterialAsset";

	internal Material(ulong id) {
		_id = id;
	}
}

/// <summary>Mesh asset.</summary>
[EngineType(ENGINE_TYPE)]
public sealed partial class Mesh : Asset {
	internal const string ENGINE_TYPE = "MeshAsset";

	internal Mesh(ulong id) {
		_id = id;
	}
}

/// <summary>Texture asset.</summary>
[EngineType(ENGINE_TYPE)]
public sealed partial class Texture : Asset {
	internal const string ENGINE_TYPE = "TextureAsset";

	internal Texture(ulong id) {
		_id = id;
	}
}

/// <summary>Sprite asset.</summary>
[EngineType(ENGINE_TYPE)]
public sealed partial class Sprite : Asset {
	internal const string ENGINE_TYPE = "SpriteAsset";

	internal Sprite(ulong id) {
		_id = id;
	}
}
