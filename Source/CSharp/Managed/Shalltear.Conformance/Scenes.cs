using System;
using System.Collections.Generic;

using Shalltear.Interop;

namespace Shalltear.Conformance;

sealed class Builder : Behavior {
	internal static readonly List<string> refusals = new();
	internal static Prefab? unit;
	internal static SceneAsset? level;

	protected override void OnEnable() {
		refusals.Add(ObjectTests.Refusal(() => SceneSystem.CreateObject("Nested")));
		refusals.Add(ObjectTests.Refusal(() => SceneSystem.Instantiate(unit!)));
		refusals.Add(ObjectTests.Refusal(() => SceneSystem.LoadScene(level!)));
	}
}

static class SceneProbe {
	static void Mark(Scene scene) {
		BindingProbe.Record("heard " + scene.Name);
	}

	static void Break(Scene scene) {
		throw new InvalidOperationException("thrown by a scene handler");
	}

	[ConformanceTest]
	internal static void KeepsListening() {
		SceneEvents.Loaded += Mark;
	}

	[ConformanceTest]
	internal static void ThrowsThenListens() {
		SceneEvents.Loaded += Break;
		SceneEvents.Loaded += Mark;
	}
}

static class SceneTests {
	const string LEVEL = "project://Assets/Level.scene";
	const string UNIT = "project://Assets/Unit.prefab";
	const string LIFECYCLE = " called inside OnAttach/OnDetach/OnEnable/OnDisable/OnDestroy. Call from OnUpdate";

	static readonly List<string> _heard = new();

	static bool Gone(EngineObject? value) {
		return value == null;
	}

	static T Stored<T>(string path)
		where T : Asset {
		T? found = ObjectCache.Find<T>(BindingProbe.FindAsset(path));
		if (Gone(found)) {
			throw new ConformanceFailure("no " + typeof(T).Name + " at " + path);
		}
		return found!;
	}

	static List<string> LoadedNames() {
		List<Scene> loaded = new();
		SceneSystem.GetLoadedScenes(loaded);
		List<string> names = new();
		foreach (Scene scene in loaded) {
			names.Add(scene.Name);
		}
		return names;
	}

	static void HearLoaded(Scene scene) {
		_heard.Add("loaded " + scene.Name);
	}

	static void HearUnloading(Scene scene) {
		_heard.Add("unloading " + scene.Name + " " + scene.State);
	}

	[ConformanceTest]
	internal static void LoadedScenesAreListed() {
		List<Scene> loaded = new();
		SceneSystem.GetLoadedScenes(loaded);
		Check.True(loaded.Count >= 1, "no loaded scene");
		Scene? primary = SceneSystem.PrimaryScene;
		Check.True(!Gone(primary), "no primary scene");
		Check.True(loaded.Contains(primary!), "primary scene isn't listed");
		Check.Equal(SceneState.Loaded, primary!.State);

		List<Scene> again = new();
		SceneSystem.GetLoadedScenes(again);
		Check.Equal(loaded.Count, again.Count);
		for (int index = 0; index < loaded.Count; ++index) {
			Check.Same(loaded[index], again[index]);
		}
		Check.Same(primary, SceneSystem.PrimaryScene);
	}

	[ConformanceTest]
	internal static void CreatedSceneTakesObjects() {
		Scene world = SceneSystem.PrimaryScene!;
		Scene made = SceneSystem.CreateScene("Scratch");
		Check.Equal("Scratch", made.Name);
		Check.True(LoadedNames().Contains("Scratch"), "created scene isn't listed");
		Check.Same(world, SceneSystem.PrimaryScene);

		SceneObject first = SceneSystem.CreateObject("First", scene: made);
		SceneObject second = SceneSystem.CreateObject("Second", first);
		Check.Equal(1, made.RootCount);
		Check.Equal(2, made.ObjectCount);
		Check.Same(first, made.GetRoot(0));
		Check.True(Gone(made.GetRoot(1)) && Gone(made.GetRoot(-1)), "root past the end");
		Check.Same(second, made.FindObjectByName("Second"));
		Check.True(Gone(made.FindObjectByName("Third")), "found an object nobody made");
		Check.Same(first, second.Parent);

		List<SceneObject> objects = new();
		made.GetObjects(objects);
		Check.Equal(2, objects.Count);
		Check.True(objects.Contains(first) && objects.Contains(second), "objects aren't the made ones");

		made.Name = "Renamed";
		Check.Equal("Renamed", made.Name);
		SceneSystem.PrimaryScene = made;
		Check.Same(made, SceneSystem.PrimaryScene);
		SceneObject third = SceneSystem.CreateObject("Third");
		Check.Equal(2, made.RootCount);
		Check.Same(third, made.GetRoot(1));
		SceneSystem.PrimaryScene = world;

		SceneSystem.UnloadScene(made);
		Check.Equal(SceneState.Unloading, made.State);
		BindingProbe.RunFrames(1);
		Check.True(Gone(made) && Gone(first) && Gone(second) && Gone(third), "unloaded scene outlived its frame");
		Check.True(!LoadedNames().Contains("Renamed"), "unloaded scene is still listed");
		bool threw = false;
		try {
			_ = made.RootCount;
		} catch (DestroyedObjectException) {
			threw = true;
		}
		Check.True(threw, "member of an unloaded scene answered");
	}

	[ConformanceTest]
	internal static void LoadSceneAddsToLoaded() {
		Scene world = SceneSystem.PrimaryScene!;
		SceneAsset level = Stored<SceneAsset>(LEVEL);
		Check.Same(level, Stored<SceneAsset>(LEVEL));

		Scene loaded = SceneSystem.LoadScene(level);
		Check.Equal("Level", loaded.Name);
		Check.Equal(SceneState.Loaded, loaded.State);
		Check.Equal(2, loaded.RootCount);
		Check.Equal(3, loaded.ObjectCount);
		SceneObject tower = loaded.FindObjectByName("Tower")!;
		Check.Equal(1, tower.ChildCount);
		Check.Equal("Flag", tower.GetChild(0)!.Name);
		Check.Same(world, SceneSystem.PrimaryScene);
		Check.True(LoadedNames().Contains("Level"), "loaded scene isn't listed");

		Scene twice = SceneSystem.LoadScene(level);
		Check.True(!ReferenceEquals(loaded, twice), "2 loads gave 1 scene");
		SceneSystem.UnloadScene(loaded);
		SceneSystem.UnloadScene(twice);
		BindingProbe.RunFrames(1);
		Check.True(Gone(loaded) && Gone(twice) && Gone(tower), "unloaded level outlived its frame");
	}

	[ConformanceTest]
	internal static void ReplaceScenesSwitchesAtFrameEnd() {
		Scene world = SceneSystem.PrimaryScene!;
		Scene scratch = SceneSystem.CreateScene("ReplaceScratch");
		SceneSystem.ReplaceScenes(Stored<SceneAsset>(LEVEL), world);
		Check.True(LoadedNames().Contains("ReplaceScratch") && !LoadedNames().Contains("Level"), "switched before frame end");

		BindingProbe.RunFrames(1);
		List<string> names = LoadedNames();
		Check.True(Gone(scratch) && !names.Contains("ReplaceScratch"), "replaced scene stayed");
		Check.True(!Gone(world) && names.Contains("Level"), "kept scene or new scene missing");
		Scene level = SceneSystem.PrimaryScene!;
		Check.Equal("Level", level.Name);

		SceneSystem.PrimaryScene = world;
		SceneSystem.UnloadScene(level);
		BindingProbe.RunFrames(1);
		Check.Same(world, SceneSystem.PrimaryScene);
	}

	[ConformanceTest]
	internal static void PrefabInstantiatesWithParentAndPose() {
		Prefab unit = Stored<Prefab>(UNIT);
		SceneObject plain = BindingProbe.CreateObject("PrefabParent");
		SceneObject first = SceneSystem.Instantiate(unit);
		Check.Equal("Unit", first.Name);
		Check.Equal(1, first.ChildCount);
		Check.Equal("Barrel", first.GetChild(0)!.Name);
		Check.Equal(7, first.GetComponent<Carrier>()!.cargo);
		Check.True(Gone(first.Parent), "instance without a parent has one");
		Check.True(SceneSystem.IsPrefabInstanceRoot(first) && SceneSystem.IsPrefabLinked(first), "instance root isn't linked");
		Check.True(SceneSystem.IsPrefabLinked(first.GetChild(0)!) && !SceneSystem.IsPrefabInstanceRoot(first.GetChild(0)!), "instance child");
		Check.True(!SceneSystem.IsPrefabLinked(plain) && !SceneSystem.IsPrefabInstanceRoot(plain), "plain object is linked");

		Quat turned = Quat.AngleAxis(90f, Vec3.Up);
		SceneObject placed = SceneSystem.Instantiate(unit, new Vec3(1f, 2f, 3f), turned, plain);
		Check.Same(plain, placed.Parent);
		Check.Equal(new Vec3(1f, 2f, 3f), placed.LocalPosition);
		Check.True(Quat.Approximately(turned, placed.LocalRotation), "rotation wasn't applied");
		Check.True(!ReferenceEquals(first, placed), "2 instances are 1 object");

		Scene made = SceneSystem.CreateScene("PrefabScene");
		SceneObject inScene = SceneSystem.Instantiate(unit, scene: made);
		Check.Same(inScene, made.GetRoot(0));
		Check.Equal(2, made.ObjectCount);

		first.Destroy();
		plain.Destroy();
		SceneSystem.UnloadScene(made);
		BindingProbe.RunFrames(1);
		Check.True(Gone(first) && Gone(placed) && Gone(inScene), "instances outlived their frame");
	}

	[ConformanceTest]
	internal static void EventsReachHandlers() {
		_heard.Clear();
		SceneEvents.Loaded += HearLoaded;
		SceneEvents.Unloading += HearUnloading;
		Scene made = SceneSystem.CreateScene("Heard");
		SceneSystem.UnloadScene(made);
		Check.Equal("loaded Heard|unloading Heard Unloading", string.Join("|", _heard));

		SceneEvents.Loaded += HearLoaded;
		Scene twice = SceneSystem.CreateScene("Twice");
		Check.Equal(4, _heard.Count);
		SceneEvents.Loaded -= HearLoaded;
		SceneEvents.Loaded -= HearLoaded;
		SceneEvents.Unloading -= HearUnloading;
		SceneSystem.UnloadScene(twice);
		Scene quiet = SceneSystem.CreateScene("Quiet");
		SceneSystem.UnloadScene(quiet);
		Check.Equal(4, _heard.Count);
		BindingProbe.RunFrames(1);
	}

	[ConformanceTest]
	internal static void LifecycleHookCallsAreRefused() {
		Builder.refusals.Clear();
		Builder.unit = Stored<Prefab>(UNIT);
		Builder.level = Stored<SceneAsset>(LEVEL);
		SceneObject host = BindingProbe.CreateObject("BuilderHost");
		host.AddComponent<Builder>();
		BindingProbe.RunFrames(1);
		Check.Equal(
			"SceneSystem.CreateObject" + LIFECYCLE + "|SceneSystem.Instantiate" + LIFECYCLE + "|SceneSystem.LoadScene" + LIFECYCLE,
			string.Join("|", Builder.refusals));
		Builder.unit = null;
		Builder.level = null;
		host.Destroy();
		BindingProbe.RunFrames(1);
	}

	[ConformanceTest]
	internal static void ComponentDestroyRemovesOneScript() {
		SceneObject host = BindingProbe.CreateObject("DestroyHost");
		Carrier carrier = host.AddComponent<Carrier>();
		Silent silent = host.AddComponent<Silent>();
		carrier.Destroy();
		BindingProbe.RunFrames(1);
		Check.True(Gone(carrier) && !Gone(silent) && !Gone(host), "wrong thing destroyed");
		Check.True(Gone(host.GetComponent<Carrier>()), "destroyed script is still found");
		Check.Same(silent, host.GetComponent<Silent>());
		carrier.Destroy();
		host.Destroy();
		BindingProbe.RunFrames(1);
	}

	[ConformanceTest]
	internal static void RepeatedSceneQueriesAllocateNothing() {
		Scene world = SceneSystem.PrimaryScene!;
		SceneObject root = SceneSystem.CreateObject("QueryRoot");
		List<Scene> loaded = new(8);
		List<SceneObject> objects = new(world.ObjectCount + 8);
		SceneSystem.GetLoadedScenes(loaded);
		world.GetObjects(objects);
		int index = objects.IndexOf(root);
		Check.True(index >= 0, "made object isn't in its scene");

		long before = GC.GetAllocatedBytesForCurrentThread();
		int hits = 0;
		for (int round = 0; round < 1000; ++round) {
			hits += ReferenceEquals(SceneSystem.PrimaryScene, world) ? 1 : 0;
			hits += world.State == SceneState.Loaded ? 1 : 0;
			hits += world.RootCount > 0 && world.ObjectCount > 0 ? 1 : 0;
			SceneSystem.GetLoadedScenes(loaded);
			world.GetObjects(objects);
			hits += ReferenceEquals(objects[index], root) ? 1 : 0;
			hits += SceneSystem.IsPrefabLinked(root) ? 0 : 1;
		}
		long allocated = GC.GetAllocatedBytesForCurrentThread() - before;
		Check.Equal(5000, hits);
		Check.Equal(0L, allocated);
		root.Destroy();
		BindingProbe.RunFrames(1);
	}
}
