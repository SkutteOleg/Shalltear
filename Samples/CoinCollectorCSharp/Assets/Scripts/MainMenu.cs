using Shalltear;

public sealed class MainMenu : Behavior {
	public SceneAsset? firstLevel;

	protected override void OnEnable() {
		UIPanelRenderer? menu = Owner.GetComponent<UIPanelRenderer>();
		if (menu == null || !menu.IsBuilt) {
			Log.Warn("CoinCollector", "MainMenu needs a UIPanelRenderer above it on the same object");
			return;
		}
		menu.FindElement("play").SetHandler(UIEvent.Click, Play);
	}

	protected override void OnDisable() {
		UIPanelRenderer? menu = Owner.GetComponent<UIPanelRenderer>();
		if (menu != null && menu.IsBuilt) {
			menu.FindElement("play").SetHandler(UIEvent.Click, null);
		}
	}

	void Play(UIEventArgs click) {
		if (firstLevel != null) {
			SceneSystem.ReplaceScenes(firstLevel);
		}
	}
}
