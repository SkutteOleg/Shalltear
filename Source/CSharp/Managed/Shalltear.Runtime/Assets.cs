using Shalltear.Interop;

namespace Shalltear;

/// <summary>Asset loaded by engine.</summary>
public abstract class Asset : EngineObject {
	private protected Asset() {}
}

/// <summary>Material asset.</summary>
[EngineType(ENGINE_TYPE)]
public sealed class Material : Asset {
	internal const string ENGINE_TYPE = "MaterialAsset";

	internal Material(ulong id) {
		_id = id;
	}
}

/// <summary>Mesh asset.</summary>
[EngineType(ENGINE_TYPE)]
public sealed class Mesh : Asset {
	internal const string ENGINE_TYPE = "MeshAsset";

	internal Mesh(ulong id) {
		_id = id;
	}
}

/// <summary>Texture asset.</summary>
[EngineType(ENGINE_TYPE)]
public sealed class Texture : Asset {
	internal const string ENGINE_TYPE = "TextureAsset";

	internal Texture(ulong id) {
		_id = id;
	}
}

/// <summary>Prefab asset.</summary>
[EngineType(ENGINE_TYPE)]
public sealed class Prefab : Asset {
	internal const string ENGINE_TYPE = "PrefabAsset";

	internal Prefab(ulong id) {
		_id = id;
	}
}

/// <summary>Sprite asset.</summary>
[EngineType(ENGINE_TYPE)]
public sealed class Sprite : Asset {
	internal const string ENGINE_TYPE = "SpriteAsset";

	internal Sprite(ulong id) {
		_id = id;
	}
}

/// <summary>Text file asset.</summary>
[EngineType(ENGINE_TYPE)]
public sealed partial class TextAsset : Asset {
	internal const string ENGINE_TYPE = "TextAsset";

	internal TextAsset(ulong id) {
		_id = id;
	}
}
