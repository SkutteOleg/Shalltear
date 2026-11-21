using Shalltear.Interop;

namespace Shalltear.Conformance;

sealed class Hoarder : Behavior {
	internal static byte[]? last;

	protected override void OnUpdate() {
		last = new byte[1000];
	}
}

sealed class Idler : Behavior {
	internal static int ticks;

	protected override void OnUpdate() {
		++ticks;
	}
}

static class ProfileProbe {
	[ConformanceTest]
	internal static void RunsTwoScripts() {
		SceneObject owner = BindingProbe.CreateObject("Profiled");
		owner.AddComponent<Hoarder>();
		owner.AddComponent<Idler>();
		BindingProbe.RunFrames(3);
		owner.Destroy();
		BindingProbe.EndFrame();
	}
}
