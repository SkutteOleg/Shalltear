using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;

using Shalltear.Interop;

namespace Shalltear.Conformance;

#pragma warning disable CS0169, CS0414, CS0649

enum Pace {
	Idle,
	Walk,
	Run = 10,
}

enum Tiny : byte {
	Off,
	On = 200,
}

enum Reach : long {
	Near = -5,
	Far = 5_000_000_000,
}

[Serializable]
struct Stats {
	public int hp;
	public float armor;
	public string? title;
	public Vec3 offset;
	public Pace pace;
	[Reflect]
	int secret;

	public int Secret {
		readonly get => secret;
		set => secret = value;
	}

	public readonly string Describe() {
		return string.Create(CultureInfo.InvariantCulture, $"{hp}/{armor}/{title ?? "null"}/{offset.x}/{pace}/{secret}");
	}
}

[Serializable]
struct Wide {
	public byte lead;
	public Quat turn;
}

[Serializable]
struct Tail {
	public Quat turn;
	public byte tail;
}

[Serializable]
sealed class Loadout {
	public string? weapon = "none";
	public int ammo = 7;
	public List<int>? mods = new();
	public Stats stats;
	public SceneObject? holder;

	public string Describe() {
		string held = holder == null ? "null" : holder.Name;
		return string.Create(CultureInfo.InvariantCulture, $"{weapon ?? "null"}/{ammo}/{Kinds.List(mods)}/{stats.Describe()}/{held}");
	}
}

[Serializable]
sealed class Chain {
	public int id = 4;
	public Chain? next;
}

[Serializable]
struct Bare {
	int kept;
}

static class Kinds {
	internal static string List<T>(IEnumerable<T>? items) {
		return items == null ? "null" : "[" + string.Join(",", items.Select(static item => string.Create(CultureInfo.InvariantCulture, $"{item}"))) + "]";
	}

	internal static string Names(IEnumerable<SceneObject?>? items) {
		return items == null ? "null" : "[" + string.Join(",", items.Select(static item => item == null ? "null" : item.Name)) + "]";
	}
}

sealed class KindHolder : Behavior {
	public int step;
	public string? title = "start";
	public string? note;
	[Reflect]
	string? hidden = "h";
	public Pace pace = Pace.Walk;
	public Tiny tiny = Tiny.On;
	public Reach reach = Reach.Near;
	public SceneObject? target;
	public Carrier? carrier;
	public TextAsset? text;
	public Stats stats = new() { hp = 5, armor = 1.5f, title = "base" };
	public Wide wide = new() { lead = 3, turn = new Quat(0, 0, 0, 1) };
	public Loadout? loadout;
	public Chain? chain;
	public List<int>? numbers = new() { 1, 2, 3 };
	public float[]? weights = { 0.5f, 1.5f };
	public List<string?>? names = new() { "a", "b" };
	public string?[]? tags;
	public List<Pace>? paces = new() { Pace.Run };
	public List<Vec3>? points;
	public Quat[]? turns;
	public List<SceneObject?>? targets;
	public Carrier?[]? carriers;
	public List<Stats>? history;
	public Loadout?[]? loadouts;
	public List<List<int>>? nested;
	public Dictionary<int, int>? map;
	public Bare bare;
	[NonSerialized]
	public int ignored = 9;

	void Change() {
		SceneObject owner = Owner;
		Carrier? own = owner.GetComponent<Carrier>();
		title = "changed \u00e9";
		note = "noted";
		hidden = "moved";
		pace = Pace.Run;
		tiny = Tiny.Off;
		reach = Reach.Far;
		target = owner;
		carrier = own;
		stats = new Stats { hp = 50, armor = 2.5f, title = "boss", offset = new Vec3(1, 2, 3), pace = Pace.Run, Secret = 77 };
		wide = new Wide { lead = 9, turn = new Quat(1, 0, 0, 0) };
		loadout = new Loadout { weapon = "bow", ammo = 3, mods = new() { 4, 5 }, stats = new Stats { hp = 1, title = "in" }, holder = owner };
		numbers = new() { 9, 8 };
		weights = new[] { 2.5f };
		names = new() { "x", "", "z\u00e9" };
		tags = new[] { "t1", "t2" };
		paces = new() { Pace.Idle, Pace.Run };
		points = new() { new Vec3(1, 2, 3) };
		turns = new[] { new Quat(0, 1, 0, 0) };
		targets = new() { owner, null };
		carriers = new[] { own };
		history = new() { new Stats { hp = 1, title = "one" }, new Stats { hp = 2, title = "two" } };
		loadouts = new[] { new Loadout { weapon = "axe" }, null };
	}

	string Describe() {
		StringBuilder row = new("kinds");
		void Add(string key, string value) {
			row.Append(' ').Append(key).Append('=').Append(value.Replace(' ', '_'));
		}
		Add("title", title ?? "null");
		Add("note", note ?? "null");
		Add("hidden", hidden ?? "null");
		Add("pace", pace.ToString());
		Add("tiny", tiny.ToString());
		Add("reach", reach.ToString());
		Add("target", target == null ? "null" : target.Name);
		Add("carrier", carrier == null ? "null" : carrier.cargo.ToString(CultureInfo.InvariantCulture));
		Add("text", text == null ? "null" : text.Text);
		Add("stats", stats.Describe());
		Add("wide", string.Create(CultureInfo.InvariantCulture, $"{wide.lead}/{wide.turn.x}/{wide.turn.w}"));
		Add("loadout", loadout == null ? "null" : loadout.Describe());
		Add("chain", chain == null ? "null" : chain.id.ToString(CultureInfo.InvariantCulture) + (chain.next == null ? "" : "+"));
		Add("numbers", Kinds.List(numbers));
		Add("weights", Kinds.List(weights));
		Add("names", Kinds.List(names));
		Add("tags", Kinds.List(tags));
		Add("paces", Kinds.List(paces));
		Add("points", points == null ? "null" : Kinds.List(points.Select(static point => point.x + point.y + point.z)));
		Add("turns", turns == null ? "null" : Kinds.List(turns.Select(static turn => turn.y)));
		Add("targets", Kinds.Names(targets));
		Add("carriers", carriers == null ? "null" : Kinds.List(carriers.Select(static each => each == null ? -1 : each.cargo)));
		Add("history", history == null ? "null" : Kinds.List(history.Select(static each => each.Describe())));
		Add("loadouts", loadouts == null ? "null" : Kinds.List(loadouts.Select(static each => each == null ? "null" : each.Describe())));
		Add("ignored", ignored.ToString(CultureInfo.InvariantCulture));
		return row.ToString();
	}

	protected override void OnUpdate() {
		if (step == 1) {
			Change();
			step = 0;
		}
		BindingProbe.Record(Describe());
	}
}

sealed class TextPair : Behavior {
	public string first = "one";
	public string? second;
	int _updates;
	string? _seen;

	protected override void OnUpdate() {
		++_updates;
		BindingProbe.Record($"texts {first} {second} {_updates} {ReferenceEquals(first, _seen)}");
		_seen = first;
		second += _updates;
	}
}

class TextRoot : Behavior {
	public string? root = "r";
	public string? blank;
	public List<int> marks = new() { 5 };
}

sealed class PlainLeaf : TextRoot {
	public int count;

	protected override void OnUpdate() {
		BindingProbe.Record("plain " + (blank == null ? "null" : "text") + " " + marks.Count.ToString(CultureInfo.InvariantCulture));
	}
}

sealed class TextLeaf : TextRoot {
	public string? leaf = "l";
	public string? twig = "t";
	public List<string?> words = new() { "w" };
	public int step;

	protected override void OnUpdate() {
		if (step == 1) {
			string shared = "same" + step.ToString(CultureInfo.InvariantCulture);
			root = shared;
			leaf = shared;
			twig = shared;
			step = 0;
		}
		if (step == 2) {
			leaf = null;
			words[0] = null;
			step = 0;
		}
		BindingProbe.Record($"leaf {root} {leaf} {twig} {marks.Count}");
	}
}

sealed class TailHolder : Behavior {
	public Tail one = new() { tail = 7 };
	public int after = 11;
	public List<Tail> many = new() { new Tail { tail = 1 }, new Tail { tail = 2, turn = new Quat(0, 0, 1, 0) } };

	protected override void OnUpdate() {
		BindingProbe.Record(string.Create(CultureInfo.InvariantCulture, $"tails {one.tail} {after} {many.Count} {many[^1].tail} {many[^1].turn.z}"));
	}
}

sealed class ChainOnly : Behavior {
	public Chain? chain;

	protected override void OnUpdate() {
		BindingProbe.Record("only " + (chain == null ? "null" : chain.id.ToString(CultureInfo.InvariantCulture)));
	}
}

sealed class Pointer : Behavior {
	public Carrier? aimed;
	public List<Carrier?>? many;

	protected override void OnUpdate() {
		string list = many == null ? "null" : Kinds.List(many.Select(static each => each == null ? -1 : each.cargo));
		BindingProbe.Record("pointer " + (aimed == null ? "null" : aimed.cargo.ToString(CultureInfo.InvariantCulture)) + " " + list);
	}
}
