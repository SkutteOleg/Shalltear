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
