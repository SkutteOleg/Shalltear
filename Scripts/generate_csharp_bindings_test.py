import copy
import json
import tempfile
import unittest
from pathlib import Path

import generate_csharp_bindings as bindings

GETTER = {
	"id": "GetSpeed",
	"cpp": "::Game::GetSpeed()",
	"returns": "f32",
	"params": [],
	"leaf": True,
	"csharp": {"member": "Speed", "kind": "getter"},
	"doc": "Units per second.",
}
SETTER = {
	"id": "SetSpeed",
	"cpp": "::Game::SetSpeed(speed)",
	"returns": "void",
	"params": [{"name": "speed", "type": "f32"}],
	"leaf": True,
	"csharp": {"member": "Speed", "kind": "setter"},
}
METHOD = {
	"id": "Describe",
	"cpp": "::Game::Describe(label, samples, origin)",
	"returns": "Vec3",
	"params": [{"name": "label", "type": "string"}, {"name": "samples", "type": "span<f32>", "out": True},
			   {"name": "origin", "type": "Vec3"}],
	"csharp": {"member": "Describe", "kind": "method"},
	"doc": "Fills samples.",
}
ATTACH = {
	"id": "Attach",
	"cpp": "::Game::Attach(*target, parent, slot)",
	"returns": "SceneObject",
	"params": [{"name": "target", "type": "SceneObject"}, {"name": "slot", "type": "i32"},
			   {"name": "parent", "type": "SceneObject", "optional": True}],
	"csharp": {"member": "Attach", "kind": "method"},
	"doc": "Attached copy.",
}
LABEL = {
	"id": "GetLabel",
	"cpp": "self->label",
	"returns": "string",
	"params": [],
	"csharp": {"member": "Label", "kind": "getter"},
	"doc": "Label.",
}


class BindingsTest(unittest.TestCase):
	def setUp(self):
		self.temp = tempfile.TemporaryDirectory()
		self.directory = Path(self.temp.name)

	def tearDown(self):
		self.temp.cleanup()

	def write(self, functions, name="Game", **extra):
		area = {"area": name, "include": "Game.h", "class": "Shalltear.Game", "doc": "Game rules.", "functions": functions}
		area.update(extra)
		path = self.directory / f"{name}.json"
		path.write_text(json.dumps(area), encoding="utf-8")
		return path

	def refusal(self, functions, **extra):
		path = self.write(functions, **extra)
		with self.assertRaises(bindings.SchemaError) as caught:
			bindings.parse_area(path)
		return str(caught.exception)

	def test_generated_files_are_current(self):
		sets = bindings.load_sets(bindings.find_sets(bindings.ROOT))
		digest = bindings.sets_digest(sets, Path(bindings.__file__).resolve())
		stale = bindings.find_stale(bindings.render_sets(sets, digest), sets)
		self.assertEqual([], [path.name for path in stale])

	def test_unknown_type_is_refused(self):
		broken = copy.deepcopy(METHOD)
		broken["params"][2]["type"] = "Vec5"
		message = self.refusal([GETTER, broken])
		self.assertTrue(message.startswith("Game.json: Describe: unknown type 'Vec5'. Use bool, i32,"), message)
		self.assertTrue(message.endswith(", SceneObject, Component, Material, Mesh, Texture, Sprite, string, span<T>"), message)

	def test_unknown_return_is_refused(self):
		expected = "Use void, bool, i32, u32, i64, u64, f32, f64, u8, u16, Vec2, Vec3, Vec4, Quat, Color, Vec2I, SceneObject, Material, Mesh, Texture, Sprite, string"
		self.assertEqual("Game.json: GetSpeed: can't return 'span<f32>'. " + expected, self.refusal([dict(GETTER, returns="span<f32>")]))
		self.assertEqual("Game.json: GetSpeed: can't return 'Component'. " + expected, self.refusal([dict(GETTER, returns="Component", leaf=False)]))

	def test_area_is_named_like_its_file(self):
		self.assertEqual("Game.json: 'area' is 'Rules'. Name it like the file (Game)", self.refusal([GETTER], area="Rules"))

	def test_table_member_lowers_leading_initialism(self):
		area = bindings.parse_area(self.write([GETTER], name="UIGame"))
		self.assertEqual("uiGameGetSpeed", area.functions[0].table_member)

	def test_duplicate_id_is_refused(self):
		twin = dict(METHOD, id="GetSpeed")
		self.assertEqual("Game.json: GetSpeed: 'id' is used twice", self.refusal([GETTER, twin]))

	def test_member_named_like_its_class_is_refused(self):
		broken = dict(GETTER, csharp={"member": "Game", "kind": "getter"})
		self.assertEqual("Game.json: GetSpeed: member 'Game' has the class name. Rename the member", self.refusal([broken]))

	def test_setter_matches_its_getter(self):
		broken = dict(SETTER, params=[{"name": "speed", "type": "f64"}])
		self.assertEqual("Game.json: SetSpeed: sets f64, 'Speed' gets f32", self.refusal([GETTER, broken]))

	def test_public_function_needs_doc(self):
		broken = {key: value for key, value in GETTER.items() if key != "doc"}
		self.assertEqual("Game.json: GetSpeed: 'doc' is missing", self.refusal([broken]))
		bindings.parse_area(self.write([broken], test=True))
		bindings.parse_area(self.write([broken], internal=True))

	def test_out_needs_a_span(self):
		broken = copy.deepcopy(METHOD)
		broken["params"][2]["out"] = True
		self.assertEqual("Game.json: Describe: 'out' is for a span, 'origin' is Vec3", self.refusal([broken]))

	def test_optional_needs_an_object(self):
		broken = dict(SETTER, params=[{"name": "speed", "type": "f32", "optional": True}])
		self.assertEqual("Game.json: SetSpeed: 'optional' is for an object, 'speed' is f32", self.refusal([broken]))

	def test_unknown_key_is_refused(self):
		broken = dict(GETTER, leef=True)
		self.assertTrue(self.refusal([broken]).startswith("Game.json: GetSpeed: unknown key 'leef'. Use "))

	def test_layout_hash_follows_signatures(self):
		def hashed(functions):
			return bindings.layout_hash([bindings.parse_area(self.write(functions))])

		base = hashed([GETTER, SETTER, METHOD, ATTACH])
		self.assertEqual(base, hashed([dict(GETTER, doc="Other words."), SETTER, METHOD, ATTACH]))
		self.assertNotEqual(base, hashed([SETTER, GETTER, METHOD, ATTACH]))
		self.assertNotEqual(base, hashed([dict(GETTER, returns="f64"), dict(SETTER, params=[{"name": "speed", "type": "f64"}]), METHOD, ATTACH]))
		self.assertNotEqual(base, hashed([dict(GETTER, leaf=False), SETTER, METHOD, ATTACH]))
		plain = copy.deepcopy(METHOD)
		del plain["params"][1]["out"]
		self.assertNotEqual(base, hashed([GETTER, SETTER, plain, ATTACH]))
		required = copy.deepcopy(ATTACH)
		del required["params"][2]["optional"]
		self.assertNotEqual(base, hashed([GETTER, SETTER, METHOD, required]))
		bindings.LOWERING += 1
		try:
			self.assertNotEqual(base, hashed([GETTER, SETTER, METHOD, ATTACH]))
		finally:
			bindings.LOWERING -= 1

	def test_leaf_call_returns_directly(self):
		area = bindings.parse_area(self.write([GETTER, SETTER, METHOD]))
		getter, _, method = area.functions
		self.assertEqual("delegate* unmanaged[SuppressGCTransition]<float>", bindings.cs_pointer(getter))
		self.assertEqual("delegate* unmanaged<SltString, float*, int, float, float, float, Vec3*, int>", bindings.cs_pointer(method))
		native = bindings.render_native_area(area)
		self.assertIn("\t\tfloat GameGetSpeed() noexcept {\n\t\t\tstatic_assert(noexcept(::Game::GetSpeed()));\n", native)
		self.assertIn("\t\tint32_t GameDescribe(SltString labelIn, float* samplesData, int32_t samplesCount, float originX, float originY, float originZ, "
					  "::Shalltear::Vec3* outResult) noexcept {\n\t\t\tconst int32_t status = ScriptRuntime::Guard([&] {\n", native)
		self.assertIn("\t\t\t\tconst ::Shalltear::Vec3 origin(originX, originY, originZ);\n"
					  "\t\t\t\t::new (static_cast<void*>(outResult)) ::Shalltear::Vec3(::Game::Describe(label, samples, origin));\n", native)

	def test_wide_value_returns_as_a_copy(self):
		spin = {"id": "GetSpin", "cpp": "::Game::GetSpin()", "returns": "Quat", "params": [], "leaf": True,
				"csharp": {"member": "Spin", "kind": "getter"}, "doc": "Spin."}
		aim = dict(spin, id="GetAim", cpp="::Game::GetAim()", returns="Vec3", csharp={"member": "Aim", "kind": "getter"})
		slow = dict(spin, id="FindSpin", cpp="::Game::FindSpin()", leaf=False, csharp={"member": "FindSpin", "kind": "method"})
		native = bindings.render_native_area(bindings.parse_area(self.write([spin, aim, slow])))
		self.assertIn("\t\tSltQuat GameGetSpin() noexcept {\n\t\t\tstatic_assert(noexcept(::Game::GetSpin()));\n"
					  "\t\t\treturn FromEngine(::Game::GetSpin());\n", native)
		self.assertIn("\t\t::Shalltear::Vec3 GameGetAim() noexcept {\n\t\t\tstatic_assert(noexcept(::Game::GetAim()));\n"
					  "\t\t\treturn ::Game::GetAim();\n", native)
		self.assertIn("\t\tint32_t GameFindSpin(SltQuat* outResult) noexcept {\n\t\t\tconst int32_t status = ScriptRuntime::Guard([&] {\n"
					  "\t\t\t\t*outResult = FromEngine(::Game::FindSpin());\n", native)

	def test_property_joins_getter_and_setter(self):
		managed = bindings.render_managed_area(bindings.parse_area(self.write([GETTER, SETTER, METHOD])))
		self.assertIn("\t/// <summary>Units per second.</summary>\n\tpublic static float Speed {\n\t\tget {\n", managed)
		self.assertIn("\t\tset {\n\t\t\tif (Bridge.OffMainThread) {\n\t\t\t\tBridge.ThrowOffMainThread(\"Game.Speed\");\n\t\t\t}\n"
					  "\t\t\tBindings.table.gameSetSpeed(value);\n\t\t}\n", managed)
		self.assertIn("\tpublic static Vec3 Describe(string label, Span<float> samples, Vec3 origin) {\n", managed)
		self.assertIn("\t\tfixed (float* samplesData = samples) {\n\t\t\tVec3 result;\n"
					  "\t\t\tint status = Bindings.table.gameDescribe(labelText.Text, samplesData, samples.Length, origin.x, origin.y, origin.z, &result);\n"
					  "\t\t\tif (status != Bridge.OK) {\n\t\t\t\tthrow Bridge.LastError();\n", managed)

	def test_any_thread_skips_the_check(self):
		free = dict(GETTER, thread="any")
		managed = bindings.render_managed_area(bindings.parse_area(self.write([free])))
		self.assertNotIn("OffMainThread", managed)
		self.assertEqual("Game.json: GetSpeed: 'thread' is 'worker'. Use main/any", self.refusal([dict(GETTER, thread="worker")]))

	def test_main_thread_call_pulls_marked_scripts(self):
		slow = dict(GETTER, leaf=False)
		native = bindings.render_native_area(bindings.parse_area(self.write([slow])))
		self.assertIn("\t\t\t\t*outResult = static_cast<float>(::Game::GetSpeed());\n\t\t\t});\n"
					  "\t\t\tManagedTypes::FlushDirty();\n\t\t\treturn status;\n\t\t}\n", native)
		free = bindings.render_native_area(bindings.parse_area(self.write([dict(slow, thread="any")])))
		self.assertIn("\t\t\treturn ScriptRuntime::Guard([&] {\n", free)
		self.assertNotIn("FlushDirty", free)

	def test_object_is_resolved_before_the_call(self):
		area = bindings.parse_area(self.write([ATTACH]))
		self.assertEqual("delegate* unmanaged<ulong, int, ulong, ulong*, int>", bindings.cs_pointer(area.functions[0]))
		native = bindings.render_native_area(area)
		self.assertIn("\t\tint32_t GameAttach(uint64_t targetId, int32_t slot, uint64_t parentId, uint64_t* outResult) noexcept {\n"
					  "\t\t\t::Shalltear::SceneObject* const target = ResolveObject<::Shalltear::SceneObject>(targetId);\n"
					  "\t\t\tif (!target) {\n\t\t\t\treturn SLT_ERR_DESTROYED;\n\t\t\t}\n"
					  "\t\t\t::Shalltear::SceneObject* const parent = ResolveObject<::Shalltear::SceneObject>(parentId);\n"
					  "\t\t\tconst int32_t status = ScriptRuntime::Guard([&] {\n"
					  "\t\t\t\t*outResult = IdOf(::Game::Attach(*target, parent, slot));\n", native)
		managed = bindings.render_managed_area(area)
		self.assertIn("\tpublic static SceneObject? Attach(SceneObject target, int slot, SceneObject? parent = null) {\n", managed)
		self.assertIn("\t\tArgumentNullException.ThrowIfNull(target);\n\t\tulong result;\n"
					  "\t\tint status = Bindings.table.gameAttach(target._id, slot, parent is null ? 0UL : parent._id, &result);\n"
					  "\t\tif (status != Bridge.OK) {\n\t\t\tthrow Bridge.Failure(status, \"Game.Attach\");\n\t\t}\n"
					  "\t\treturn ObjectCache.SceneObject(result);\n", managed)

	def test_returned_object_may_be_declared_present(self):
		managed = bindings.render_managed_area(bindings.parse_area(self.write([dict(ATTACH, nullable=False)])))
		self.assertIn("\tpublic static SceneObject Attach(SceneObject target, int slot, SceneObject? parent = null) {\n", managed)
		self.assertIn("\t\treturn ObjectCache.SceneObject(result)!;\n", managed)
		self.assertEqual("Game.json: GetSpeed: 'nullable' is for a returned object, this returns f32", self.refusal([dict(GETTER, nullable=False)]))

	def test_instance_area_passes_its_object(self):
		relayed = dict(ATTACH, csharp={"member": "Attach", "kind": "method", "relay": True})
		area = bindings.parse_area(self.write([LABEL, relayed], instance="SceneObject", **{"class": "Shalltear.SceneObject"}))
		self.assertEqual("delegate* unmanaged<ulong, SltString*, int>", bindings.cs_pointer(area.functions[0]))
		native = bindings.render_native_area(area)
		self.assertIn("\t\tint32_t GameGetLabel(uint64_t selfId, SltString* outResult) noexcept {\n"
					  "\t\t\t::Shalltear::SceneObject* const self = ResolveObject<::Shalltear::SceneObject>(selfId);\n"
					  "\t\t\tif (!self) {\n\t\t\t\treturn SLT_ERR_DESTROYED;\n\t\t\t}\n"
					  "\t\t\tconst int32_t status = ScriptRuntime::Guard([&] {\n"
					  "\t\t\t\t*outResult = ScriptRuntime::KeepString(self->label);\n", native)
		managed = bindings.render_managed_area(area)
		self.assertIn("public unsafe partial class SceneObject {\n\t/// <summary>Label.</summary>\n\tpublic string Label {\n", managed)
		self.assertIn("\t\t\tint status = Bindings.table.gameGetLabel(_id, &result);\n\t\t\tif (status != Bridge.OK) {\n"
					  "\t\t\t\tthrow Bridge.Failure(status, \"SceneObject.Label\");\n\t\t\t}\n\t\t\treturn Bridge.Text(result);\n", managed)
		self.assertIn("\tpublic SceneObject? Attach(SceneObject target, int slot, SceneObject? parent, string member) {\n", managed)
		self.assertIn("\t\t\tBridge.ThrowOffMainThread(member);\n", managed)
		self.assertIn("\t\t\tthrow Bridge.Failure(status, member);\n", managed)
		self.assertEqual("Game.json: 'instance' is 'Widget'. Use SceneObject, Component, Material, Mesh, Texture, Sprite", self.refusal([LABEL], instance="Widget"))
		self.assertEqual("Game.json: GetLabel: 'relay' needs a method",
						 self.refusal([dict(LABEL, csharp={"member": "Label", "kind": "getter", "relay": True})], instance="SceneObject"))

	def test_leaf_with_an_object_keeps_its_status(self):
		area = bindings.parse_area(self.write([dict(ATTACH, leaf=True), GETTER]))
		self.assertEqual("delegate* unmanaged[SuppressGCTransition]<ulong, int, ulong, ulong*, int>", bindings.cs_pointer(area.functions[0]))
		native = bindings.render_native_area(area)
		self.assertIn("\t\t\tif (!target) {\n\t\t\t\treturn SLT_ERR_DESTROYED;\n\t\t\t}\n"
					  "\t\t\t::Shalltear::SceneObject* const parent = ResolveObject<::Shalltear::SceneObject>(parentId);\n"
					  "\t\t\treturn ScriptRuntime::Guard([&] {\n"
					  "\t\t\t\t*outResult = IdOf(::Game::Attach(*target, parent, slot));\n\t\t\t});\n\t\t}\n", native)
		self.assertNotIn("FlushDirty", native)
		self.assertIn("\t\tfloat GameGetSpeed() noexcept {\n", native)
		managed = bindings.render_managed_area(area)
		self.assertIn("\t\t\tthrow Bridge.Failure(status, \"Game.Attach\");\n", managed)

		member = bindings.parse_area(self.write([GETTER], instance="SceneObject", **{"class": "Shalltear.SceneObject"}))
		self.assertEqual("delegate* unmanaged[SuppressGCTransition]<ulong, float*, int>", bindings.cs_pointer(member.functions[0]))
		self.assertEqual("Game.json: GetSpeed: 'leaf' can't return a string", self.refusal([dict(GETTER, returns="string")]))

	def test_test_area_stays_out_of_shipped_builds(self):
		area = bindings.parse_area(self.write([GETTER], test=True))
		native = bindings.render_native_area(area)
		self.assertIn("#ifdef SLT_CSHARP_TEST_HOOKS\n", native)
		self.assertIn("#else\n\nnamespace Shalltear::CSharpImpl::Generated {\n\n\tvoid FillGameBindings(SltBindings&) {}\n", native)
		self.assertIn("internal static unsafe partial class Game {\n", bindings.render_managed_area(area))

	def test_internal_area_ships_without_public_members(self):
		area = bindings.parse_area(self.write([GETTER, ATTACH], internal=True))
		self.assertNotIn("SLT_CSHARP_TEST_HOOKS", bindings.render_native_area(area))
		managed = bindings.render_managed_area(area)
		self.assertIn("internal static unsafe partial class Game {\n", managed)
		self.assertNotIn("public", managed)

	def test_declared_type_becomes_a_wrapper(self):
		widget = {"name": "Widget", "cpp": "::Game::Widget", "engine": "GameWidget", "base": "Asset", "doc": "Widget asset."}
		find = {"id": "Find", "cpp": "::Game::Find(*near)", "returns": "Widget", "params": [{"name": "near", "type": "Widget"}],
				"csharp": {"member": "Find", "kind": "method"}, "doc": "Nearest widget."}
		area = bindings.parse_area(self.write([find], types=[widget]))
		native = bindings.render_native_area(area)
		self.assertIn("\t\t\t::Game::Widget* const near = ResolveObject<::Game::Widget>(nearId);\n", native)
		self.assertIn("\t\t\t\t*outResult = IdOf(::Game::Find(*near));\n", native)
		managed = bindings.render_managed_area(area)
		self.assertIn("/// <summary>Widget asset.</summary>\n[EngineType(ENGINE_TYPE)]\npublic sealed partial class Widget : Asset {\n"
					  "\tinternal const string ENGINE_TYPE = \"GameWidget\";\n\n\tinternal Widget(ulong id) {\n\t\t_id = id;\n\t}\n}\n", managed)
		self.assertIn("\tpublic static Widget? Find(Widget near) {\n", managed)
		self.assertIn("\t\treturn ObjectCache.Find<Widget>(result);\n", managed)
		self.assertIn("\t\tWrap(Widget.ENGINE_TYPE, static id => new Widget(id));\n", bindings.render_managed_wrappers([area]))

		member = bindings.parse_area(self.write([LABEL], types=[widget], instance="Widget", **{"class": "Shalltear.Widget"}))
		self.assertIn("public unsafe partial class Widget {\n", bindings.render_managed_area(member))
		self.assertEqual("Game.json: Widget: 'base' is 'Thing'. Use EngineObject, Asset, Component or an abstract type",
						 self.refusal([find], types=[dict(widget, base="Thing")]))
		self.assertEqual("Game.json: Widget: type is declared twice", self.refusal([find], types=[widget, widget]))
		self.assertEqual("Game.json: SceneObject: type is declared twice", self.refusal([find], types=[widget, dict(widget, name="SceneObject")]))

	def test_enum_crosses_as_its_integer(self):
		mode = {"name": "Mode", "cpp": "::Game::Mode", "underlying": "u8", "doc": "How it runs.",
				"values": [{"name": "Idle", "value": 0, "doc": "Stopped."}, {"name": "Busy", "value": 200, "doc": "Running."}]}
		turn = {"id": "Turn", "cpp": "::Game::Turn(mode)", "returns": "Mode", "params": [{"name": "mode", "type": "Mode"}],
				"csharp": {"member": "Turn", "kind": "method"}, "doc": "Next mode."}
		peek = dict(turn, id="Peek", cpp="::Game::Peek()", params=[], leaf=True, csharp={"member": "Current", "kind": "getter"})
		area = bindings.parse_area(self.write([turn, peek], enums=[mode]))
		self.assertEqual("delegate* unmanaged<byte, byte*, int>", bindings.cs_pointer(area.functions[0]))
		native = bindings.render_native_area(area)
		self.assertIn("\tstatic_assert(sizeof(::Game::Mode) == sizeof(uint8_t));\n"
					  "\tstatic_assert(static_cast<int64_t>(::Game::Mode::Idle) == 0);\n"
					  "\tstatic_assert(static_cast<int64_t>(::Game::Mode::Busy) == 200);\n", native)
		self.assertIn("\t\tint32_t GameTurn(uint8_t modeIn, uint8_t* outResult) noexcept {\n", native)
		self.assertIn("\t\t\t\tconst ::Game::Mode mode = static_cast<::Game::Mode>(modeIn);\n"
					  "\t\t\t\t*outResult = static_cast<uint8_t>(::Game::Turn(mode));\n", native)
		self.assertIn("\t\t\treturn static_cast<uint8_t>(::Game::Peek());\n", native)
		managed = bindings.render_managed_area(area)
		self.assertIn("/// <summary>How it runs.</summary>\npublic enum Mode : byte {\n\t/// <summary>Stopped.</summary>\n\tIdle = 0,\n"
					  "\t/// <summary>Running.</summary>\n\tBusy = 200,\n}\n", managed)
		self.assertIn("\tpublic static Mode Turn(Mode mode) {\n", managed)
		self.assertIn("\t\tint status = Bindings.table.gameTurn((byte)mode, &result);\n", managed)
		self.assertIn("\t\treturn (Mode)result;\n", managed)
		self.assertIn("\t\t\treturn (Mode)Bindings.table.gamePeek();\n", managed)

		moved = copy.deepcopy(mode)
		moved["values"][1]["value"] = 201
		self.assertNotEqual(bindings.layout_hash([area]), bindings.layout_hash([bindings.parse_area(self.write([turn, peek], enums=[moved]))]))
		self.assertEqual("Game.json: Mode: 'underlying' is 'i64'. Use u8, u16, i32, u32", self.refusal([turn], enums=[dict(mode, underlying="i64")]))
		self.assertEqual("Game.json: Mode: 'values' is empty", self.refusal([turn], enums=[dict(mode, values=[])]))

	def test_struct_fills_a_span(self):
		mode = {"name": "Mode", "cpp": "::Game::Mode", "underlying": "u16", "doc": "How it runs.",
				"values": [{"name": "Idle", "value": 0, "doc": "Stopped."}]}
		hit = {"name": "Hit", "doc": "1 hit.", "fields": [
			{"name": "solid", "type": "bool", "doc": "Blocks."}, {"name": "mode", "type": "Mode", "doc": "Its mode."},
			{"name": "cell", "type": "Vec2I", "doc": "Where."}, {"name": "weight", "type": "f64", "doc": "How heavy."}]}
		fill = {"id": "Fill", "cpp": "::Game::Fill(hits)", "returns": "i32", "params": [{"name": "hits", "type": "span<Hit>", "out": True}],
				"leaf": True, "csharp": {"member": "Fill", "kind": "method"}, "doc": "Hits found."}
		area = bindings.parse_area(self.write([fill], enums=[mode], structs=[hit]))
		self.assertEqual("delegate* unmanaged[SuppressGCTransition]<Hit*, int, int>", bindings.cs_pointer(area.functions[0]))
		header = bindings.render_native_header([area], 0)
		self.assertIn("typedef struct SltHit {\n\tuint8_t solid;\n\tuint16_t mode;\n\t::Shalltear::Vec2I cell;\n\tdouble weight;\n} SltHit;\n", header)
		self.assertIn("\tint32_t (*gameFill)(SltHit* hits, int32_t hitsCount);\n", header)
		table = bindings.render_native_table([area])
		self.assertIn("\tstatic_assert(sizeof(SltHit) == 24 && alignof(SltHit) == 8);\n\tstatic_assert(offsetof(SltHit, solid) == 0);\n"
					  "\tstatic_assert(offsetof(SltHit, mode) == 2);\n\tstatic_assert(offsetof(SltHit, cell) == 4);\n"
					  "\tstatic_assert(offsetof(SltHit, weight) == 16);\n", table)
		native = bindings.render_native_area(area)
		self.assertIn("\t\t\tconst std::span<SltHit> hits(hitsData, static_cast<size_t>(hitsCount));\n", native)
		managed = bindings.render_managed_area(area)
		self.assertIn("/// <summary>1 hit.</summary>\n[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]\n"
					  "public partial struct Hit {\n\t/// <summary>Blocks.</summary>\n\tpublic bool solid;\n\t/// <summary>Its mode.</summary>\n"
					  "\tpublic Mode mode;\n", managed)
		self.assertIn("\tpublic static int Fill(Span<Hit> hits) {\n", managed)
		self.assertIn("\t\tfixed (Hit* hitsData = hits) {\n\t\t\treturn Bindings.table.gameFill(hitsData, hits.Length);\n", managed)
		self.assertIn('\t\tif (sizeof(Hit) != 24) {\n\t\t\treturn "Hit size " + sizeof(Hit) + " (engine 24)";\n', bindings.render_managed_table([area]))

		wide = dict(hit, fields=hit["fields"] + [{"name": "tag", "type": "u8", "doc": "Tag."}])
		self.assertNotEqual(bindings.layout_hash([area]), bindings.layout_hash([bindings.parse_area(self.write([fill], enums=[mode], structs=[wide]))]))
		self.assertEqual("Game.json: Hit: field 'name' can't be string. Use a scalar, a value type, an enum or a struct",
						 self.refusal([fill], structs=[dict(hit, fields=[{"name": "name", "type": "string", "doc": "Name."}])]))
		self.assertEqual("Game.json: Hit: 'fields' is empty", self.refusal([fill], structs=[dict(hit, fields=[])]))

	def test_flag_enum_is_marked(self):
		mods = {"name": "Mods", "cpp": "::Game::Mods", "underlying": "u8", "doc": "Held keys.", "flags": True,
				"values": [{"name": "None", "value": 0, "doc": "None."}, {"name": "Shift", "value": 2, "doc": "Shift."}]}
		held = {"id": "Held", "cpp": "::Game::Held()", "returns": "Mods", "params": [], "csharp": {"member": "Held", "kind": "getter"}, "doc": "Held."}
		managed = bindings.render_managed_area(bindings.parse_area(self.write([held], enums=[mods])))
		self.assertIn("/// <summary>Held keys.</summary>\n[Flags]\npublic enum Mods : byte {\n", managed)
		plain = bindings.render_managed_area(bindings.parse_area(self.write([held], enums=[{key: value for key, value in mods.items() if key != "flags"}])))
		self.assertNotIn("[Flags]", plain)

	def test_integer_value_crosses_as_ints(self):
		move = {"id": "Move", "cpp": "::Game::Move(cell)", "returns": "Vec2I", "params": [{"name": "cell", "type": "Vec2I"}], "leaf": True,
				"csharp": {"member": "Move", "kind": "method"}, "doc": "Next cell."}
		area = bindings.parse_area(self.write([move]))
		self.assertEqual("delegate* unmanaged[SuppressGCTransition]<int, int, Vec2I>", bindings.cs_pointer(area.functions[0]))
		self.assertIn("\t\t::Shalltear::Vec2I GameMove(int32_t cellX, int32_t cellY) noexcept {\n"
					  "\t\t\tconst ::Shalltear::Vec2I cell(cellX, cellY);\n", bindings.render_native_area(area))

	def test_internal_member_sits_in_a_public_class(self):
		hidden = {key: value for key, value in GETTER.items() if key != "doc"}
		hidden["csharp"] = {"member": "Speed", "kind": "getter", "access": "internal"}
		managed = bindings.render_managed_area(bindings.parse_area(self.write([hidden, METHOD])))
		self.assertIn("\tinternal static float Speed {\n", managed)
		self.assertIn("\tpublic static Vec3 Describe(", managed)
		self.assertEqual("Game.json: GetSpeed: 'access' is 'private'. Use internal",
						 self.refusal([dict(GETTER, csharp={"member": "Speed", "kind": "getter", "access": "private"})]))

	def test_keyword_parameter_is_refused(self):
		broken = dict(SETTER, params=[{"name": "event", "type": "f32"}])
		self.assertEqual("Game.json: SetSpeed: parameter 'event' is a C# keyword. Rename it", self.refusal([broken]))

	def test_instance_parameter_is_refused(self):
		twice = dict(LABEL, params=[{"name": "self", "type": "SceneObject"}])
		self.assertEqual("Game.json: GetLabel: parameter 'self' is the SceneObject passed by the area. Leave it out",
						 self.refusal([twice], instance="SceneObject"))

	def test_trailing_optional_objects_default_to_null(self):
		place = dict(ATTACH, params=[{"name": "target", "type": "SceneObject", "optional": True}, {"name": "slot", "type": "i32"},
									 {"name": "parent", "type": "SceneObject", "optional": True},
									 {"name": "anchor", "type": "SceneObject", "optional": True}],
					 cpp="::Game::Place(target, slot, parent, anchor)")
		managed = bindings.render_managed_area(bindings.parse_area(self.write([place])))
		self.assertIn("\tpublic static SceneObject? Attach(SceneObject? target, int slot, SceneObject? parent = null, SceneObject? anchor = null) {\n", managed)

	def test_trailing_scalars_and_enums_take_defaults(self):
		mode = {"name": "Mode", "cpp": "::Game::Mode", "underlying": "u8", "doc": "How it moves.",
				"values": [{"name": "Walk", "value": 0, "doc": "Slow."}, {"name": "Run", "value": 1, "doc": "Fast."}]}
		move = dict(ATTACH, params=[{"name": "target", "type": "SceneObject"}, {"name": "fade", "type": "f32", "default": -1},
									{"name": "mode", "type": "Mode", "default": "Run"}, {"name": "loop", "type": "bool", "default": True},
									{"name": "half", "type": "f64", "default": 0.5}, {"name": "parent", "type": "SceneObject", "optional": True}],
					cpp="::Game::Move(*target, fade, mode, loop, half, parent)")
		managed = bindings.render_managed_area(bindings.parse_area(self.write([move], enums=[mode])))
		self.assertIn("Attach(SceneObject target, float fade = -1f, Mode mode = Mode.Run, bool loop = true, double half = 0.5, "
					  "SceneObject? parent = null) {\n", managed)

	def test_default_is_refused_where_csharp_takes_none(self):
		early = dict(ATTACH, params=[{"name": "target", "type": "SceneObject"}, {"name": "slot", "type": "i32", "default": 0},
									 {"name": "parent", "type": "SceneObject"}])
		self.assertEqual("Game.json: Attach: 'default' on 'slot' needs one on each later parameter", self.refusal([early]))
		unsigned = dict(SETTER, params=[{"name": "speed", "type": "u32", "default": -1}], csharp={"member": "Push", "kind": "method"})
		self.assertEqual("Game.json: SetSpeed: 'default' of 'speed' must be a number of type u32", self.refusal([unsigned]))
		text = dict(LABEL, id="Find", returns="i32", params=[{"name": "name", "type": "string", "default": "x"}],
					csharp={"member": "Find", "kind": "method"})
		self.assertEqual("Game.json: Find: 'default' is for a scalar or an enum, 'name' is string", self.refusal([text]))
		setter = dict(SETTER, params=[{"name": "speed", "type": "f32", "default": 1}])
		self.assertEqual("Game.json: SetSpeed: 'default' needs a method without 'relay'", self.refusal([setter]))

	def test_digest_follows_schemas_and_script(self):
		script = self.directory / "script.py"
		script.write_text("first", encoding="utf-8")
		self.write([GETTER])
		package = self.directory / "package"
		package.mkdir()
		sets = [bindings.BindingSet("", self.directory, self.directory, self.directory), bindings.BindingSet("Game", package, package, package)]
		before = bindings.sets_digest(sets, script)
		self.write([GETTER, SETTER])
		changed = bindings.sets_digest(sets, script)
		(package / "Extra.json").write_text("{}", encoding="utf-8")
		added = bindings.sets_digest(sets, script)
		script.write_text("second", encoding="utf-8")
		self.assertNotEqual(before, changed)
		self.assertNotEqual(changed, added)
		self.assertNotEqual(added, bindings.sets_digest(sets, script))

	def package_sets(self, functions, **extra):
		core = self.directory / "core"
		game = self.directory / "Source" / "Game" / "Bindings"
		core.mkdir()
		game.mkdir(parents=True)
		(core / "Base.json").write_text(json.dumps({"area": "Base", "include": "Base.h", "class": "Shalltear.Base", "doc": "Base.", "functions": [METHOD]}),
										encoding="utf-8")
		area = {"area": "Game", "include": "Game.h", "class": "Shalltear.Game", "doc": "Game rules.", "functions": functions}
		area.update(extra)
		(game / "Game.json").write_text(json.dumps(area), encoding="utf-8")
		found = bindings.find_sets(self.directory)
		self.assertEqual(["", "Game"], [binding_set.name for binding_set in found])
		found[0] = bindings.BindingSet("", core, self.directory / "native", self.directory / "managed")
		return found

	def test_package_set_gets_its_table(self):
		sets = self.package_sets([GETTER, SETTER])
		core, game = sets
		self.assertEqual(self.directory / "Source" / "GameCSharp" / "Private" / "Generated", game.native)
		self.assertEqual(self.directory / "Source" / "CSharp" / "Managed" / "Shalltear.Game" / "Generated", game.managed)
		files = bindings.render_sets(bindings.load_sets(sets), 7)

		header = files[game.native / "GameBindings.generated.h"]
		self.assertIn('#pragma once\n\n#include "ScriptBridge.h"\n\n#define SLT_GAME_LAYOUT_HASH 0x', header)
		self.assertNotIn("SLT_BINDINGS_INPUT_DIGEST", header)
		self.assertIn("typedef struct SltGameBindings {\n\tfloat (*gameGetSpeed)(void);\n\tvoid (*gameSetSpeed)(float speed);\n} SltGameBindings;\n", header)
		self.assertIn("\tvoid FillGameTable(SltGameBindings& table);\n", header)
		table = files[game.native / "GameBindings.generated.cpp"]
		self.assertIn('#include "GameBindings.generated.h"\n', table)
		self.assertNotIn("::Shalltear::Vec3", table)
		self.assertIn("\tvoid FillGameTable(SltGameBindings& table) {\n\t\tFillGameBindings(table);\n\t}\n", table)
		thunks = files[game.native / "Game.generated.cpp"]
		self.assertIn('#include "BindingSupport.h"\n\n#include "GameBindings.generated.h"\n#include "Game.h"\n', thunks)
		self.assertIn("\tvoid FillGameBindings(SltGameBindings& table) {\n", thunks)

		holder = files[game.managed / "GameBindings.g.cs"]
		self.assertIn("internal unsafe struct SltGameBindings {\n\tpublic delegate* unmanaged[SuppressGCTransition]<float> gameGetSpeed;\n", holder)
		self.assertIn("public static unsafe partial class GameBindings {\n", holder)
		self.assertIn("\tstatic partial void Attached();\n", holder)
		self.assertIn('\tpublic static void Attach() {\n\t\ttable = *(SltGameBindings*)Bridge.FindBindings("Game", LAYOUT_HASH, sizeof(SltGameBindings));\n'
					  "\t\tAttached();\n\t}\n", holder)
		self.assertIn("\t\t\treturn GameBindings.table.gameGetSpeed();\n", files[game.managed / "Game.g.cs"])

		self.assertIn("#define SLT_BRIDGE_LAYOUT_HASH 0x", files[core.native / "Bindings.generated.h"])
		self.assertIn("#define SLT_BINDINGS_INPUT_DIGEST 0x0000000000000007ull\n", files[core.native / "Bindings.generated.h"])
		self.assertNotIn("gameGetSpeed", files[core.native / "Bindings.generated.h"])
		self.assertNotIn("gameGetSpeed", files[core.managed / "Bindings.g.cs"])
		self.assertIn("Bindings.table.baseDescribe(", files[core.managed / "Base.g.cs"])

		game.managed.mkdir(parents=True)
		(game.managed / "Old.g.cs").write_text("stale", encoding="utf-8")
		self.assertIn(game.managed / "Old.g.cs", bindings.find_stale(files, sets))

	def test_area_name_is_taken_once_over_all_sets(self):
		sets = self.package_sets([GETTER])
		(sets[0].bindings / "Game.json").write_text(
			json.dumps({"area": "Game", "include": "Game.h", "class": "Shalltear.Game", "doc": "Game rules.", "functions": [GETTER]}), encoding="utf-8")
		with self.assertRaises(bindings.SchemaError) as caught:
			bindings.load_sets(sets)
		self.assertEqual("Game.json: area is declared twice (" + self.directory.name + " + Game)", str(caught.exception))

	def test_abstract_type_takes_derived_wrappers(self):
		shape = {"name": "Shape", "cpp": "::Game::Shape", "engine": "GameShape", "base": "Component", "doc": "Any shape.", "abstract": True}
		box = {"name": "Box", "cpp": "::Game::Box", "engine": "GameBox", "base": "Shape", "doc": "Box shape."}
		grow = {"id": "Grow", "cpp": "self->Grow()", "returns": "void", "params": [], "csharp": {"member": "Grow", "kind": "method"}, "doc": "Grow."}
		first = self.write([grow], name="Box", types=[box], instance="Box", **{"class": "Shalltear.Box"})
		second = self.write([grow], name="Shape", types=[shape], instance="Shape", **{"class": "Shalltear.Shape"})
		derived, base = bindings.load_files([first, second])
		self.assertIn("/// <summary>Any shape.</summary>\n[EngineType(ENGINE_TYPE)]\npublic abstract partial class Shape : Component {\n"
					  "\tinternal const string ENGINE_TYPE = \"GameShape\";\n\n\tprivate protected Shape() {}\n}\n", bindings.render_managed_area(base))
		self.assertIn("public sealed partial class Box : Shape {\n\tinternal new const string ENGINE_TYPE = \"GameBox\";\n\n\tinternal Box(ulong id) {\n",
					  bindings.render_managed_area(derived))
		self.assertIn("\t\t\t::Game::Shape* const self = ResolveObject<::Game::Shape>(selfId);\n", bindings.render_native_area(base))
		wrappers = bindings.render_managed_wrappers([derived, base])
		self.assertIn("\t\tWrap(Box.ENGINE_TYPE, static id => new Box(id));\n\t\tDeclare<Shape>(Shape.ENGINE_TYPE);\n", wrappers)
		self.assertEqual("Game.json: Lid: 'base' is 'Box'. Use EngineObject, Asset, Component or an abstract type",
						 self.refusal([GETTER], types=[box, shape, dict(box, name="Lid", base="Box")]))

	def test_struct_field_can_be_hidden_or_nested(self):
		point = {"name": "Point", "doc": "1 point.", "fields": [{"name": "depth", "type": "f32", "doc": "How deep."}]}
		hit = {"name": "Hit", "doc": "1 hit.", "fields": [
			{"name": "ownerId", "type": "u64", "access": "internal"}, {"name": "first", "type": "Point", "access": "internal"},
			{"name": "count", "type": "i32", "doc": "Points."}]}
		fill = {"id": "Fill", "cpp": "::Game::Fill(hits)", "returns": "i32", "params": [{"name": "hits", "type": "span<Hit>", "out": True}],
				"csharp": {"member": "Fill", "kind": "method"}, "doc": "Hits found."}
		area = bindings.parse_area(self.write([fill], structs=[point, hit]))
		header = bindings.render_native_header([area], 0)
		self.assertIn("typedef struct SltHit {\n\tuint64_t ownerId;\n\tSltPoint first;\n\tint32_t count;\n} SltHit;\n", header)
		self.assertIn("\tstatic_assert(sizeof(SltHit) == 16 && alignof(SltHit) == 8);\n\tstatic_assert(offsetof(SltHit, ownerId) == 0);\n"
					  "\tstatic_assert(offsetof(SltHit, first) == 8);\n\tstatic_assert(offsetof(SltHit, count) == 12);\n", bindings.render_native_table([area]))
		self.assertIn("public partial struct Hit {\n\tinternal ulong _ownerId;\n\tinternal Point _first;\n\t/// <summary>Points.</summary>\n"
					  "\tpublic int count;\n}\n", bindings.render_managed_area(area))
		self.assertEqual("Game.json: Hit: field 'ownerId' has access 'public'. Use internal (or leave it out)",
						 self.refusal([fill], structs=[dict(hit, fields=[{"name": "ownerId", "type": "u64", "access": "public"}])]))
		self.assertEqual("Game.json: Hit: 'doc' is missing", self.refusal([fill], structs=[dict(hit, fields=[{"name": "ownerId", "type": "u64"}])]))
		self.assertEqual("Game.json: Hit: field 'first' can't be Point. Use a scalar, a value type, an enum or a struct",
						 self.refusal([fill], structs=[hit, point]))

	def test_handle_crosses_as_one_integer(self):
		voice = {"name": "Voice", "cpp": "::Game::Voice", "doc": "1 voice."}
		moment = {"name": "Moment", "cpp": "::Game::Moment", "doc": "A time.", "value": {"name": "frames", "doc": "Frames since start."}}
		start = {"id": "Start", "cpp": "::Game::Start(at)", "returns": "Voice", "params": [{"name": "at", "type": "Moment"}],
				 "csharp": {"member": "Start", "kind": "method"}, "doc": "Start at a time."}
		ends = {"id": "GetEnd", "cpp": "::Game::GetEnd(voice)", "returns": "Moment", "params": [{"name": "voice", "type": "Voice"}],
				"csharp": {"member": "GetEnd", "kind": "method"}, "doc": "When it ends."}
		area = bindings.parse_area(self.write([start, ends], handles=[voice, moment]))
		self.assertIn("\tint32_t (*gameStart)(uint64_t at, uint64_t* outResult);\n", bindings.render_native_header([area], 0))
		native = bindings.render_native_area(area)
		self.assertIn("\tstatic_assert(sizeof(::Game::Voice) == sizeof(uint64_t) && std::is_trivially_copyable_v<::Game::Voice>);\n", native)
		self.assertIn("\t\t\tconst ::Game::Moment at = std::bit_cast<::Game::Moment>(atIn);\n", native)
		self.assertIn("\t\t\t\t*outResult = std::bit_cast<uint64_t>(::Game::Start(at));\n", native)
		managed = bindings.render_managed_area(area)
		self.assertIn("/// <summary>1 voice.</summary>\npublic readonly partial struct Voice : IEquatable<Voice> {\n\tinternal readonly ulong _bits;\n\n"
					  "\tinternal Voice(ulong bits) {\n\t\t_bits = bits;\n\t}\n", managed)
		self.assertIn("public readonly partial struct Moment : IEquatable<Moment> {\n\t/// <summary>Frames since start.</summary>\n"
					  "\tpublic readonly ulong frames;\n\n\t/// <summary>Frames since start.</summary>\n\tpublic Moment(ulong frames) {\n", managed)
		self.assertIn("\tpublic static Voice Start(Moment at) {\n", managed)
		self.assertIn("gameStart(at.frames, &result);", managed)
		self.assertIn("\t\treturn new Voice(result);\n", managed)
		self.assertIn("gameGetEnd(voice._bits, &result);", managed)
		self.assertNotEqual(bindings.layout_hash([area]), bindings.layout_hash([bindings.parse_area(self.write([start, ends], handles=[moment, voice]))]))
		self.assertEqual("Game.json: Voice: type is declared twice", self.refusal([start, ends], handles=[voice, moment, voice]))
		self.assertEqual("Game.json: Voice: unknown key 'underlying'. Use cpp, doc, name, value",
						 self.refusal([start, ends], handles=[dict(voice, underlying="u32"), moment]))

	def test_hand_written_asset_wrapper_is_returned(self):
		skin = {"id": "GetSkin", "cpp": "::Game::GetSkin()", "returns": "Mesh", "params": [], "csharp": {"member": "Skin", "kind": "getter"}, "doc": "Skin."}
		dress = {"id": "SetSkin", "cpp": "::Game::SetSkin(skin)", "returns": "void", "params": [{"name": "skin", "type": "Mesh", "optional": True}],
				 "csharp": {"member": "Skin", "kind": "setter"}}
		area = bindings.parse_area(self.write([skin, dress]))
		self.assertIn("\t\t\t::Shalltear::MeshAsset* const skin = ResolveObject<::Shalltear::MeshAsset>(skinId);\n", bindings.render_native_area(area))
		managed = bindings.render_managed_area(area)
		self.assertIn("\tpublic static Mesh? Skin {\n", managed)
		self.assertIn("\t\t\treturn ObjectCache.Find<Mesh>(result);\n", managed)
		self.assertIn("can't return 'Component'", self.refusal([dict(skin, returns="Component")]))


if __name__ == "__main__":
	unittest.main()
