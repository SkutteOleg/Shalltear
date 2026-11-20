using Shalltear;

public sealed class PlayerController : Behavior, IPhysicsListener {
	public float speed = 6f;
	public AudioClip? pickupSound;

	readonly InputActionMap _controls = new();
	readonly ActionId _move;

	public PlayerController() {
		_move = _controls.RegisterAction("Move", ActionType.Axis2D);
		_controls.Bind(_move, InputBinding.Key(Key.D, 1f, 0));
		_controls.Bind(_move, InputBinding.Key(Key.A, -1f, 0));
		_controls.Bind(_move, InputBinding.Key(Key.W, 1f, 1));
		_controls.Bind(_move, InputBinding.Key(Key.S, -1f, 1));
		_controls.Bind(_move, InputBinding.GamepadAxis(GamepadAxis.LeftX, 1f, 0));
		_controls.Bind(_move, InputBinding.GamepadAxis(GamepadAxis.LeftY, 1f, 1));
	}

	public int Coins { get; private set; }

	protected override void OnEnable() {
		_controls.Enable();
	}

	protected override void OnDisable() {
		_controls.Disable();
	}

	protected override void OnDestroy() {
		_controls.Dispose();
	}

	protected override void OnUpdate() {
		_controls.Evaluate();
		Vec2 input = _controls.GetAxis2D(_move);
		if (input.Length > 1f) {
			input = input.Normalized;
		}

		Rigidbody? body = Owner.GetComponent<Rigidbody>();
		if (body == null) {
			return;
		}
		Vec3 velocity = body.LinearVelocity;
		velocity.x = input.x * speed;
		velocity.z = input.y * speed;
		body.LinearVelocity = velocity;
	}

	public void OnTriggerEnter(Collider other) {
		SceneObject touched = other.Owner;
		Coin? coin = touched.GetComponent<Coin>();
		if (coin == null) {
			return;
		}

		Coins += coin.value;
		AudioSource? audio = Owner.GetComponent<AudioSource>();
		if (audio != null && pickupSound != null) {
			audio.PlayOneShot(pickupSound);
		}
		touched.Destroy();
	}
}
