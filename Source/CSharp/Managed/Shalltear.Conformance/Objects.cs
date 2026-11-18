using System;
using System.Collections.Generic;

using Shalltear.Interop;

namespace Shalltear.Conformance;

interface ILabeled {
	string Label { get; }
}

[KeepName]
sealed class Carrier : Behavior, ILabeled {
	internal static readonly List<Carrier> created = new();

	public int cargo = 3;
	public float weight = 1.5f;
	[Reflect]
	Vec3 heading = new(0, 0, 1);

	public Carrier() {
		created.Add(this);
	}

	public string Label => "carrier";

	public Vec3 Heading {
		get => heading;
		set => heading = value;
	}
}

sealed class Spawner : Behavior {
	public int spawned;
	internal SceneObject? template;
	internal SceneObject? last;

	protected override void OnUpdate() {
		if (template == null) {
			return;
		}
		SceneObject clone = SceneSystem.Instantiate(template, Owner);
		clone.Name = "Spawn " + spawned;
		clone.LocalPosition = new Vec3(spawned, 0, 0);
		clone.GetComponent<Carrier>()!.cargo = 100 + spawned;
		++spawned;
		last = clone;
	}
}

sealed class Meddler : Behavior {
	internal static readonly List<string> refusals = new();
	internal static SceneObject? other;

	void Meddle(string hook) {
		SceneObject owner = Owner;
		refusals.Add(hook + ObjectTests.Refusal(() => owner.SetActive(false)));
	}

	protected override void OnAttach() {
		Meddle("attach ");
	}

	protected override void OnDisable() {
		Meddle("disable ");
	}

	protected override void OnDetach() {
		Meddle("detach ");
	}

	protected override void OnDestroy() {
		Meddle("destroy ");
	}

	protected override void OnEnable() {
		SceneObject owner = Owner;
		refusals.Add(ObjectTests.Refusal(() => owner.SetActive(false)));
		refusals.Add(ObjectTests.Refusal(() => owner.SetParent(other)));
		refusals.Add(ObjectTests.Refusal(() => owner.SiblingIndex = 0));
		refusals.Add(ObjectTests.Refusal(() => owner.AddComponent<Silent>()));
		refusals.Add(ObjectTests.Refusal(() => SetEnabled(false)));
		refusals.Add(ObjectTests.Refusal(() => SceneSystem.Instantiate(owner)));
	}
}

static class ObjectTests {
	const string LIFECYCLE = " called inside OnAttach/OnDetach/OnEnable/OnDisable/OnDestroy. Call from OnUpdate";

	static bool Gone(EngineObject? value) {
		return value == null && null == value && !(value != null) && !value;
	}

	static bool Alive(EngineObject? value) {
		return value != null && null != value && !(value == null) && value;
	}

	static bool Equal(EngineObject? left, EngineObject? right) {
		return left == right && right == left && !(left != right);
	}

	static bool EqualsNull(EngineObject value) {
		return value.Equals(null) && !ReferenceEquals(value, null);
	}

	static void Discard(SceneObject first, SceneObject? second = null) {
		first.Destroy();
		if (second != null) {
			second.Destroy();
		}
		BindingProbe.EndFrame();
	}

	static void Throws(string member, Action call) {
		try {
			call();
		} catch (DestroyedObjectException exception) {
			Check.Equal(member + " used on destroyed object. Check for null before use", exception.Message);
			return;
		}
		Check.Fail(member + " didn't throw");
	}

	internal static string Refusal(Action call) {
		try {
			call();
		} catch (EngineException exception) {
			return exception.Message;
		}
		return "allowed";
	}

	[ConformanceTest]
	internal static void SameObjectSameReference() {
		SceneObject parent = BindingProbe.CreateObject("SameParent");
		SceneObject child = BindingProbe.CreateObject("SameChild");
		child.SetParent(parent);
		Check.Same(parent, child.Parent);
		Check.Same(child, parent.GetChild(0));
		Check.Same(child, parent.FindChildByName("SameChild"));
		Check.Same(parent, SceneSystem.FindObjectByName("SameParent"));
		Check.Same(child, SceneSystem.FindObjectByName("SameChild"));

		Silent script = child.AddComponent<Silent>();
		Check.Same(child, script.Owner);
		Check.Same(script, child.GetComponent<Silent>());
		Check.True(parent.GetChild(1) == null && parent.GetChild(-1) == null, "child outside the range");
		Check.True(SceneSystem.FindObjectByName("SameNobody") == null, "found an object nobody made");
		Discard(parent);
	}

	[ConformanceTest]
	internal static void DestroyedEqualsNull() {
		SceneObject first = BindingProbe.CreateObject("NullFirst");
		SceneObject second = BindingProbe.CreateObject("NullSecond");
		SceneObject again = first;
		Silent script = first.AddComponent<Silent>();
		int hash = first.GetHashCode();
		int scriptHash = script.GetHashCode();
		Check.True(Equal(first, again) && !Equal(first, second) && Alive(first) && !Gone(first), "live objects compare by reference");
		Check.True(first.Equals(again) && !first.Equals(second) && !EqualsNull(first), "Equals differs from ==");

		first.Destroy();
		Check.True(Alive(first) && Alive(script), "gone before end of frame");
		Check.True(first.GetComponent<Silent>() == null, "found a script pending destroy");
		BindingProbe.EndFrame();
		Check.True(Gone(first) && !Alive(first), "destroyed object isn't null");
		Check.True(EqualsNull(first) && !first.Equals(second), "Equals differs from == once destroyed");
		Check.True(Gone(script) && !Alive(script), "script outlived its object");
		Check.True(!Equal(first, second) && Alive(second), "live object equals a destroyed one");
		Check.Equal(hash, first.GetHashCode());
		Check.Equal(scriptHash, script.GetHashCode());

		Discard(second);
		Check.True(Equal(first, second) && Gone(second), "2 destroyed objects differ");
		Check.True(Equal(null, first) && Gone(null) && !Alive(null), "null differs from a destroyed object");
	}

	[ConformanceTest]
	internal static void DestroyedMemberThrows() {
		SceneObject target = BindingProbe.CreateObject("ThrowTarget");
		SceneObject other = BindingProbe.CreateObject("ThrowOther");
		Silent script = target.AddComponent<Silent>();
		Discard(target);

		Throws("SceneObject.Name", () => _ = target.Name);
		Throws("SceneObject.Name", () => target.Name = "renamed");
		Throws("SceneObject.SetActive", () => target.SetActive(false));
		Throws("SceneObject.LocalPosition", () => target.LocalPosition = default);
		Throws("SceneObject.ChildCount", () => _ = target.ChildCount);
		Throws("SceneObject.GetComponent", () => target.GetComponent<Silent>());
		Throws("SceneObject.GetComponent", () => target.GetComponent<ILabeled>());
		Throws("SceneObject.GetComponents", () => target.GetComponents(new List<Silent>()));
		Throws("SceneObject.AddComponent", () => target.AddComponent<Silent>());
		Throws("Component.Owner", () => _ = script.Owner);
		Throws("Component.SetEnabled", () => script.SetEnabled(false));
		Throws("SceneSystem.Instantiate", () => SceneSystem.Instantiate(target));

		target.Destroy();
		other.SetParent(target);
		Check.True(other.Parent == null, "destroyed parent wasn't read as null");
		SceneObject clone = SceneSystem.Instantiate(other, target);
		Check.True(clone.Parent == null, "destroyed parent wasn't read as null");
		Discard(other, clone);
	}

	[ConformanceTest]
	internal static void DestroyedMemberCrossesOnce() {
		SceneObject target = BindingProbe.CreateObject("CrossTarget");
		int checks = BindingProbe.GetAliveChecks();
		int touches = BindingProbe.GetTouches();
		Check.Equal(touches + 1, BindingProbe.Touch(target));
		Check.Equal(checks, BindingProbe.GetAliveChecks());

		target.Destroy();
		BindingProbe.EndFrame();
		bool thrown = false;
		try {
			BindingProbe.Touch(target);
		} catch (DestroyedObjectException) {
			thrown = true;
		}
		Check.True(thrown, "no DestroyedObjectException");
		Check.Equal(touches + 1, BindingProbe.GetTouches());
		Check.Equal(checks, BindingProbe.GetAliveChecks());
		SceneObject? compared = target;
		Check.True(compared == null, "destroyed object isn't null");
		Check.Equal(checks + 1, BindingProbe.GetAliveChecks());
	}

	[ConformanceTest]
	internal static void CacheDropsDeadEntriesBySlice() {
		const int COUNT = 10_000;
		const int KEEPERS = 300;
		int passes = 2 * (ObjectCache.Count / ObjectCache.SLICE + 1);
		for (int pass = 0; pass < passes; ++pass) {
			ObjectCache.Sweep();
		}
		SceneObject[] keepers = new SceneObject[KEEPERS];
		for (int index = 0; index < KEEPERS; ++index) {
			keepers[index] = BindingProbe.CreateObject("CacheKeeper" + index);
		}
		int kept = ObjectCache.Count;
		SceneObject[] made = new SceneObject[COUNT];
		for (int index = 0; index < COUNT; ++index) {
			made[index] = BindingProbe.CreateObject("CacheEntry");
		}
		Check.Equal(kept + COUNT, ObjectCache.Count);
		foreach (SceneObject entry in made) {
			entry.Destroy();
		}
		BindingProbe.EndFrame();
		Check.Equal(kept + COUNT, ObjectCache.Count);

		int frames = 0;
		int limit = (kept + COUNT) / ObjectCache.SLICE + 2;
		while (ObjectCache.Count > kept && frames < limit) {
			int asked = ObjectCache.Sweep();
			Check.True(asked > 0 && asked <= ObjectCache.SLICE, "a frame asked about " + asked);
			++frames;
		}
		Check.Equal(kept, ObjectCache.Count);
		Check.Equal(kept, ObjectCache.WrapperCount);
		Check.True(frames >= COUNT / ObjectCache.SLICE, "emptied in " + frames + " frame(s)");
		Check.Same(keepers[0], SceneSystem.FindObjectByName("CacheKeeper0"));
		Check.Same(keepers[KEEPERS - 1], SceneSystem.FindObjectByName("CacheKeeper" + (KEEPERS - 1)));
		foreach (SceneObject keeper in keepers) {
			keeper.Destroy();
		}
		BindingProbe.EndFrame();
	}

	[ConformanceTest]
	internal static void GetComponentFindsScript() {
		SceneObject host = BindingProbe.CreateObject("FindHost");
		Check.True(host.GetComponent<Carrier>() == null && host.GetComponent<ILabeled>() == null, "found a script before one was added");
		Carrier carrier = host.AddComponent<Carrier>();
		Silent silent = host.AddComponent<Silent>();
		LateReporter reporter = host.AddComponent<LateReporter>();
		HolderOfInt holder = host.AddComponent<HolderOfInt>();

		Check.Same(carrier, host.GetComponent<Carrier>());
		Check.Same(silent, host.GetComponent<Silent>());
		Check.Same(reporter, host.GetComponent<EnableCounter>());
		Check.Same(carrier, host.GetComponent<ILabeled>());
		Check.Same(holder, host.GetComponent<Holder<int>>());
		Check.Same(carrier, host.GetComponent<Behavior>());
		Check.Same(carrier, host.GetComponent<Component>());
		Check.True(host.GetComponent<UpdateOnly>() == null && host.GetComponent<IDisposable>() == null, "found a script nobody added");

		List<Behavior> every = new();
		host.GetComponents(every);
		Check.Equal(4, every.Count);
		Check.True(ReferenceEquals(every[0], carrier) && ReferenceEquals(every[1], silent) && ReferenceEquals(every[2], reporter) &&
					   ReferenceEquals(every[3], holder),
				   "scripts out of order");
		List<EnableCounter> counters = new() { reporter, reporter };
		host.GetComponents(counters);
		Check.Equal(1, counters.Count);
		Check.Same(reporter, counters[0]);
		List<ILabeled> labeled = new();
		host.GetComponents(labeled);
		Check.Equal(1, labeled.Count);
		Check.Same(carrier, labeled[0]);

		silent.SetEnabled(false);
		Check.Same(silent, host.GetComponent<Silent>());
		Discard(host);
	}

	[ConformanceTest]
	internal static void ManyScriptsAreAllFound() {
		const int COUNT = 40;
		SceneObject host = BindingProbe.CreateObject("ManyHost");
		for (int index = 0; index < COUNT; ++index) {
			host.AddComponent<Silent>();
		}
		Carrier carrier = host.AddComponent<Carrier>();
		List<Silent> found = new();
		host.GetComponents(found);
		Check.Equal(COUNT, found.Count);
		List<Behavior> every = new();
		host.GetComponents(every);
		Check.Equal(COUNT + 1, every.Count);
		Check.Same(carrier, host.GetComponent<ILabeled>());
		Discard(host);
	}

	[ConformanceTest]
	internal static void RepeatedGetComponentAllocatesNothing() {
		SceneObject parent = BindingProbe.CreateObject("AllocParent");
		SceneObject host = BindingProbe.CreateObject("AllocHost");
		host.SetParent(parent);
		Carrier carrier = host.AddComponent<Carrier>();
		List<Carrier> results = new(4);
		host.GetComponents(results);
		Check.Same(carrier, host.GetComponent<Carrier>());
		Check.Same(carrier, host.GetComponent<ILabeled>());
		Check.Same(parent, host.Parent);
		Check.Same(host, carrier.Owner);
		Check.True(Alive(host) && !Gone(host), "no host");

		long before = GC.GetAllocatedBytesForCurrentThread();
		int hits = 0;
		for (int index = 0; index < 1000; ++index) {
			hits += ReferenceEquals(host.GetComponent<Carrier>(), carrier) ? 1 : 0;
			hits += ReferenceEquals(host.GetComponent<ILabeled>(), carrier) ? 1 : 0;
			hits += ReferenceEquals(host.Parent, parent) ? 1 : 0;
			hits += ReferenceEquals(carrier.Owner, host) ? 1 : 0;
			hits += Alive(host) && !Gone(host) ? 1 : 0;
			host.GetComponents(results);
			hits += results.Count;
		}
		long allocated = GC.GetAllocatedBytesForCurrentThread() - before;
		Check.Equal(6000, hits);
		Check.Equal(0L, allocated);
		Discard(parent);
	}

	[ConformanceTest]
	internal static void AddComponentAddsScript() {
		SceneObject host = BindingProbe.CreateObject("AddHost");
		Carrier added = host.AddComponent<Carrier>();
		Check.Same(host, added.Owner);
		Check.Equal(3, added.cargo);
		Check.True(added.IsEnabledSelf && added.IsEffectivelyEnabled, "added disabled");
		Check.True(!ReferenceEquals(host.AddComponent<Carrier>(), added), "no 2nd script");
		Check.Equal("SceneObject.AddComponent can't add EnableCounter (abstract)", Refusal(() => host.AddComponent<EnableCounter>()));
		Check.Equal("SceneObject.AddComponent can't add Holder`1 (not a registered script class)", Refusal(() => host.AddComponent<Holder<int>>()));
		List<Behavior> scripts = new();
		host.GetComponents(scripts);
		Check.Equal(2, scripts.Count);

		added.SetEnabled(false);
		Check.True(!added.IsEnabledSelf && !added.IsEffectivelyEnabled, "still enabled");
		added.SetEnabled(true);
		host.SetActive(false);
		Check.True(added.IsEnabledSelf && !added.IsEffectivelyEnabled, "enabled on an inactive object");
		Check.True(!host.IsActiveSelf && !host.IsActiveInHierarchy, "still active");
		Discard(host);
	}

	[ConformanceTest]
	internal static void InstantiateCopiesFields() {
		SceneObject source = BindingProbe.CreateObject("CopySource");
		SceneObject child = BindingProbe.CreateObject("CopyChild");
		child.SetParent(source);
		Carrier carrier = source.AddComponent<Carrier>();
		carrier.cargo = 11;
		carrier.weight = 2.5f;
		carrier.Heading = new Vec3(1, 0, 0);
		child.AddComponent<Carrier>().cargo = 12;

		Carrier.created.Clear();
		SceneObject clone = SceneSystem.Instantiate(source);
		Check.Equal(2, Carrier.created.Count);
		Check.Equal(11, Carrier.created[0].cargo);
		Check.Equal(12, Carrier.created[1].cargo);
		Check.True(!ReferenceEquals(clone, source) && clone.Parent == null, "clone isn't a new root");
		Check.Equal("CopySource", clone.Name);
		Carrier copied = clone.GetComponent<Carrier>()!;
		Check.Same(Carrier.created[0], copied);
		Carrier.created.Clear();
		Check.Equal(11, copied.cargo);
		Check.Equal(2.5f, copied.weight);
		Check.Equal(new Vec3(1, 0, 0), copied.Heading);
		Check.Equal(1, clone.ChildCount);
		Check.Equal(12, clone.GetChild(0)!.GetComponent<Carrier>()!.cargo);

		copied.cargo = 13;
		BindingProbe.RunFrames(1);
		Check.Equal(13, copied.cargo);
		Check.Equal(11, carrier.cargo);

		SceneObject under = SceneSystem.Instantiate(source, clone);
		Check.Same(clone, under.Parent);
		Check.Equal(2, clone.ChildCount);
		Discard(source, clone);
	}

	[ConformanceTest]
	internal static void HierarchyFollowsCalls() {
		SceneObject root = BindingProbe.CreateObject("TreeRoot");
		SceneObject first = BindingProbe.CreateObject("TreeFirst");
		SceneObject second = BindingProbe.CreateObject("TreeSecond");
		first.SetParent(root);
		second.SetParent(root);
		Check.Equal(2, root.ChildCount);
		Check.Equal(0, first.SiblingIndex);
		Check.Equal(1, second.SiblingIndex);
		second.SiblingIndex = 0;
		Check.Same(second, root.GetChild(0));
		Check.Equal(1, first.SiblingIndex);
		second.SiblingIndex = 99;
		Check.Equal(1, second.SiblingIndex);
		second.SiblingIndex = -5;
		Check.Equal(0, second.SiblingIndex);

		root.Name = "TreeRenamed caf\u00e9";
		Check.Equal("TreeRenamed caf\u00e9", root.Name);
		Check.Same(root, SceneSystem.FindObjectByName("TreeRenamed caf\u00e9"));
		root.Name = "";
		Check.Equal("", root.Name);

		root.SetActive(false);
		Check.True(first.IsActiveSelf && !first.IsActiveInHierarchy, "child of an inactive object");
		root.SetActive(true);
		Check.True(first.IsActiveInHierarchy, "child of an active object");

		first.SetParent(null);
		Check.True(first.Parent == null, "still parented");
		Check.Equal(1, root.ChildCount);
		Discard(root, first);
	}

	[ConformanceTest]
	internal static void TransformsFollowParents() {
		SceneObject parent = BindingProbe.CreateObject("MoveParent");
		SceneObject child = BindingProbe.CreateObject("MoveChild");
		parent.LocalPosition = new Vec3(10, 0, 0);
		parent.LocalScale = new Vec3(2, 2, 2);
		child.SetParent(parent, false);
		child.LocalPosition = new Vec3(1, 2, 3);
		Check.Equal(new Vec3(1, 2, 3), child.LocalPosition);
		Check.True(Vec3.Approximately(new Vec3(12, 4, 6), child.WorldPosition, 0.0001f), child.WorldPosition.ToString());
		Check.True(Vec3.Approximately(new Vec3(2, 2, 2), child.LossyScale, 0.0001f), child.LossyScale.ToString());

		child.WorldPosition = new Vec3(10, 0, 0);
		Check.True(Vec3.Approximately(default, child.LocalPosition, 0.0001f), child.LocalPosition.ToString());

		Quat turn = Quat.AngleAxis(90, new Vec3(0, 1, 0));
		parent.LocalRotation = turn;
		child.LocalRotation = turn;
		Check.True(Quat.Approximately(turn, child.LocalRotation, 0.0001f), child.LocalRotation.ToString());
		Check.True(Quat.Approximately(turn * turn, child.WorldRotation, 0.0001f), child.WorldRotation.ToString());
		child.WorldRotation = turn;
		Check.True(Quat.Approximately(Quat.Identity, child.LocalRotation, 0.0001f), child.LocalRotation.ToString());

		child.LocalScale = new Vec3(3, 4, 5);
		Check.Equal(new Vec3(3, 4, 5), child.LocalScale);
		child.SetParent(null);
		Check.True(Vec3.Approximately(new Vec3(10, 0, 0), child.WorldPosition, 0.0001f), child.WorldPosition.ToString());
		Discard(parent, child);
	}

	[ConformanceTest]
	internal static void ScriptsUseObjectsFromHooks() {
		SceneObject root = BindingProbe.CreateObject("SpawnRoot");
		SceneObject template = BindingProbe.CreateObject("SpawnTemplate");
		template.AddComponent<Carrier>();
		Spawner spawner = root.AddComponent<Spawner>();
		spawner.template = template;
		BindingProbe.RunFrames(3);

		Check.Equal(3, spawner.spawned);
		Check.Equal(3, root.ChildCount);
		for (int index = 0; index < 3; ++index) {
			SceneObject spawn = root.GetChild(index)!;
			Check.Equal("Spawn " + index, spawn.Name);
			Check.Equal(new Vec3(index, 0, 0), spawn.LocalPosition);
			Check.Equal(100 + index, spawn.GetComponent<Carrier>()!.cargo);
		}
		Check.Same(root.GetChild(2), spawner.last);
		Check.Equal(3, template.GetComponent<Carrier>()!.cargo);
		Discard(root, template);
	}

	[ConformanceTest]
	internal static void LifecycleHookCallsAreRefused() {
		SceneObject host = BindingProbe.CreateObject("MeddleHost");
		SceneObject other = BindingProbe.CreateObject("MeddleOther");
		Meddler.refusals.Clear();
		Meddler.other = other;
		Meddler meddler = host.AddComponent<Meddler>();
		Meddler.other = null;

		string[] members = {
			"attach SceneObject.SetActive",
			"SceneObject.SetActive",
			"SceneObject.SetParent",
			"SceneObject.SiblingIndex",
			"SceneObject.AddComponent",
			"Component.SetEnabled",
			"SceneSystem.Instantiate",
		};
		Check.Equal(string.Join(LIFECYCLE + "|", members) + LIFECYCLE, string.Join("|", Meddler.refusals));
		Check.True(host.IsActiveSelf && host.Parent == null && meddler.IsEnabledSelf, "a refused call went through");
		List<Behavior> scripts = new();
		host.GetComponents(scripts);
		Check.Equal(1, scripts.Count);

		Meddler.refusals.Clear();
		Discard(host, other);
		string[] hooks = { "disable SceneObject.SetActive", "detach SceneObject.SetActive", "destroy SceneObject.SetActive" };
		Check.Equal(string.Join(LIFECYCLE + "|", hooks) + LIFECYCLE, string.Join("|", Meddler.refusals));
	}

	[ConformanceTest]
	internal static void DoomedObjectIgnoresCalls() {
		SceneObject doomed = BindingProbe.CreateObject("DoomTarget");
		SceneObject other = BindingProbe.CreateObject("DoomOther");
		SceneObject parent = BindingProbe.CreateObject("DoomParent");
		Silent script = doomed.AddComponent<Silent>();
		doomed.Destroy();
		doomed.SetActive(false);
		doomed.SetParent(other);
		doomed.SiblingIndex = 3;
		script.SetEnabled(false);
		doomed.Destroy();
		Check.True(doomed.IsActiveSelf && doomed.Parent == null && script.IsEnabledSelf, "a call reached an object pending destroy");
		Check.Equal("SceneObject.AddComponent used on object pending destroy", Refusal(() => doomed.AddComponent<Carrier>()));
		Check.Equal("SceneSystem.Instantiate got source pending destroy", Refusal(() => SceneSystem.Instantiate(doomed)));
		Check.Equal("SceneObject.SetParent got parent pending destroy", Refusal(() => other.SetParent(doomed)));
		Check.Equal("SceneSystem.Instantiate got parent pending destroy", Refusal(() => SceneSystem.Instantiate(other, doomed)));
		BindingProbe.EndFrame();

		other.SetParent(parent);
		Check.Equal("SceneObject.SetParent got object/descendant as parent", Refusal(() => parent.SetParent(other)));
		Check.Equal("SceneObject.SetParent got object/descendant as parent", Refusal(() => parent.SetParent(parent)));
		Check.Same(parent, other.Parent);
		Check.True(parent.Parent == null, "a refused parent was set");
		Discard(parent);
	}
}

static class ObjectProbe {
	[ConformanceTest]
	internal static void FillsCache() {
		for (int index = 0; index < 300; ++index) {
			BindingProbe.CreateObject("ProbeEntry").Destroy();
		}
		BindingProbe.EndFrame();
	}

	[ConformanceTest]
	internal static void ReportsCacheCount() {
		Check.Fail(ObjectCache.Count.ToString());
	}

	[ConformanceTest]
	internal static void AddsAFailingScript() {
		SceneObject host = BindingProbe.CreateObject("FailHost");
		Check.Equal("SceneObject.AddComponent got no ThrowsInConstructor instance (constructor failed)",
					ObjectTests.Refusal(() => host.AddComponent<ThrowsInConstructor>()));
		Check.True(host.GetComponent<ThrowsInConstructor>() == null, "found a script without an instance");
		host.Destroy();
		BindingProbe.EndFrame();
	}
}
