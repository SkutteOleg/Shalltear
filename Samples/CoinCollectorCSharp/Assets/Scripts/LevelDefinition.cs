using Shalltear;

[AssetMenu("Level Definition", ".level")]
public sealed class LevelDefinition : DataAsset {
	public float timeLimit = 30f;
	public int coinsToWin = 5;
	public SceneAsset? nextLevel;
}
