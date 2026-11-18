using System;
using System.Collections.Generic;
using System.Globalization;

using Shalltear.Interop;

namespace Shalltear.Conformance;

#pragma warning disable CS0414, CS0649

sealed class FieldHolder : Behavior {
	public bool flag = true;
	public sbyte tiny = -5;
	public byte small = 200;
	public short shorter = -3000;
	public ushort word = 60000;
	public int count = -7;
	public uint mask = 0xdeadbeef;
	public long big = -5_000_000_000;
	public ulong huge = 0xfedcba9876543210;
	public float speed = 2.5f;
	public double ratio = 0.125;
	public Vec2 uv = new(0.25f, 0.75f);
	public Vec3 offset = new(1, 2, 3);
	public Vec4 plane = new(4, 5, 6, 7);
	public Quat turn = new(0.5f, -0.5f, 0.25f, 0.75f);
	public Color tint = new(0.5f, 0.25f, 0.125f, 1);
	[Reflect]
	float secret = 7.5f;
	[Reflect]
	internal int shared = 9;

	[Range(0, 10), Tooltip("Units per second"), Header("Motion")]
	public float ranged = 4;
	[Hidden]
	public int unseen = 11;
	[ReadOnly]
	public int locked = 12;
	[Transient]
	public int passing = 13;

	public static int counter = 1;
	public readonly int frozen = 2;
	public const int LIMIT = 3;
	int unmarked = 4;
	public int Property { get; set; } = 5;
	public Action? changed;
	public Dictionary<int, int>? lookup;

	protected override void OnUpdate() {
		BindingProbe.Record(string.Create(
			CultureInfo.InvariantCulture,
			$"holder {flag} {tiny} {small} {shorter} {word} {count} {mask} {big} {huge} {speed} {ratio} {uv.x} {uv.y} {offset.x} {offset.y} {offset.z} {plane.x} {plane.y} {plane.z} {plane.w} {turn.x} {turn.y} {turn.z} {turn.w} {tint.r} {tint.g} {tint.b} {tint.a} {secret} {shared} {ranged} {unseen} {locked} {passing}"));
	}
}

sealed class FieldMover : Behavior {
	public float speed = 2.5f;
	public int ticks;
	public Vec3 position;
	[Reflect]
	float charge = 1;

	static int _moves;

	protected override void OnUpdate() {
		++ticks;
		++_moves;
		position += new Vec3(speed, 0, 0);
		charge *= 2;
		BindingProbe.Record(string.Create(CultureInfo.InvariantCulture, $"mover {ticks} {position.x} {charge} {speed} {_moves}"));
	}
}

abstract class FieldBase : Behavior {
	public int rank = 21;
	[Reflect]
	protected int seed = 22;
}

sealed class FieldDerived : FieldBase {
	public int level = 23;

	protected override void OnUpdate() {
		BindingProbe.Record(string.Create(CultureInfo.InvariantCulture, $"derived {rank} {seed} {level}"));
	}
}

sealed class FieldAligned : Behavior {
	public byte lead = 1;
	public Vec4 wide = new(1, 2, 3, 4);
	public byte middle = 2;
	public Quat turn = new(5, 6, 7, 8);
	public byte tail = 3;

	protected override void OnUpdate() {
		BindingProbe.Record(string.Create(CultureInfo.InvariantCulture, $"aligned {lead} {wide.w} {middle} {turn.w} {tail}"));
	}
}

sealed class FieldGlow : Behavior {
	public float glow = 0.5f;
	public Color shade = new(1, 1, 1, 1);

	protected override void OnLateUpdate() {
		BindingProbe.Record(string.Create(CultureInfo.InvariantCulture, $"glow {glow} {shade.g}"));
	}
}
