using System;

using Shalltear;

public sealed class LevelController : Behavior {
	public LevelDefinition? level;
	public SceneObject? player;
	public SceneAsset? menuScene;

	readonly InputActionMap _controls = new();
	readonly ActionId _pause;
	float _timeLeft;
	bool _paused;
	bool _finished;
	int _shownCoins = -1;
	int _shownSeconds = -1;

	public LevelController() {
		_pause = _controls.RegisterAction("Pause", ActionType.Button);
		_controls.Bind(_pause, InputBinding.Key(Key.Escape));
		_controls.Bind(_pause, InputBinding.GamepadButton(GamepadButton.Start));
	}

	protected override void OnEnable() {
		_timeLeft = level != null ? level.timeLimit : 0f;
		_controls.Enable();

		UIPanelRenderer? hud = GetHud();
		if (hud != null) {
			hud.FindElement("resume").SetHandler(UIEvent.Click, click => SetPaused(false));
			hud.FindElement("menu").SetHandler(UIEvent.Click, click => GoTo(menuScene));
		}
		SetPaused(false);
	}

	protected override void OnDisable() {
		_controls.Disable();
		Time.TimeScale = 1f;
		UIPanelRenderer? hud = GetHud();
		if (hud != null) {
			hud.FindElement("resume").SetHandler(UIEvent.Click, null);
			hud.FindElement("menu").SetHandler(UIEvent.Click, null);
		}
	}

	protected override void OnDestroy() {
		_controls.Dispose();
	}

	UIPanelRenderer? GetHud() {
		UIPanelRenderer? hud = Owner.GetComponent<UIPanelRenderer>();
		return hud != null && hud.IsBuilt ? hud : null;
	}

	protected override void OnUpdate() {
		_controls.Evaluate();
		if (_controls.WasPerformed(_pause) && !_finished) {
			SetPaused(!_paused);
		}

		PlayerController? hero = player != null ? player.GetComponent<PlayerController>() : null;
		if (level == null || hero == null || _finished) {
			return;
		}

		_timeLeft = MathF.Max(0f, _timeLeft - Time.DeltaTime);

		int seconds = (int)MathF.Ceiling(_timeLeft);
		UIPanelRenderer? hud = GetHud();
		if (hud != null && (hero.Coins != _shownCoins || seconds != _shownSeconds)) {
			_shownCoins = hero.Coins;
			_shownSeconds = seconds;
			hud.FindElement("coins").Text = $"Coins: {hero.Coins} / {level.coinsToWin}";
			hud.FindElement("timer").Text = $"Time: {seconds}";
		}

		if (hero.Coins >= level.coinsToWin) {
			GoTo(level.nextLevel != null ? level.nextLevel : menuScene);
		} else if (_timeLeft <= 0f) {
			GoTo(menuScene);
		}
	}

	void SetPaused(bool paused) {
		_paused = paused;
		Time.TimeScale = paused ? 0f : 1f;
		AudioSource? audio = player != null ? player.GetComponent<AudioSource>() : null;
		if (audio != null) {
			if (paused) {
				audio.Pause();
			} else {
				audio.UnPause();
			}
		}
		UIPanelRenderer? hud = GetHud();
		if (hud != null) {
			hud.FindElement("pause-menu").SetStyle("display", paused ? "flex" : "none");
		}
	}

	void GoTo(SceneAsset? scene) {
		if (scene == null || _finished) {
			return;
		}
		_finished = true;
		SceneSystem.ReplaceScenes(scene);
	}
}
