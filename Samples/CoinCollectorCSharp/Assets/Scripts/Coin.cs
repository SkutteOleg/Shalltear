using Shalltear;

public sealed class Coin : Behavior {
	public int value = 1;
	public float degreesPerSecond = 90f;

	protected override void OnUpdate() {
		SceneObject coin = Owner;
		Quat step = Quat.AngleAxis(degreesPerSecond * Time.DeltaTime, Vec3.Up);
		coin.LocalRotation = (step * coin.LocalRotation).Normalized;
	}
}
