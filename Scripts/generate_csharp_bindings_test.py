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
		expected = "Use void, bool, i32, u32, i64, u64, f32, f64, u8, u16, i8, i16, Vec2, Vec3, Vec4, Quat, Color, Vec2I, SceneObject, Material, Mesh, Texture, Sprite, string"
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
		self.assertEqual("Game.json: Mode: 'underlying' is 'f32'. Use u8, u16, i32, u32, i8, i16, i64, u64", self.refusal([turn], enums=[dict(mode, underlying="f32")]))
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

	def test_native_fields_cross_on_any_thread(self):
		recipe = {"name": "Recipe", "cpp": "::Game::Recipe", "owned": True}
		weight = {"id": "GetWeight", "cpp": "self->weight", "returns": "f32", "params": [], "csharp": {"member": "Weight", "kind": "getter"}}
		weigh = {"id": "SetWeight", "cpp": "self->weight = value", "returns": "void", "params": [{"name": "value", "type": "f32"}],
				 "csharp": {"member": "Weight", "kind": "setter"}}
		cook = {"id": "Cook", "cpp": "self->Cook()", "returns": "void", "params": [], "csharp": {"member": "Cook", "kind": "method"}}
		chef = {"id": "GetChef", "cpp": "self->chef", "returns": "SceneObject", "params": [], "csharp": {"member": "Chef", "kind": "getter"}}
		size = {"id": "GetSize", "cpp": "self->items.size()", "returns": "u64", "params": [], "csharp": {"member": "Size", "kind": "getter"}}
		pinned = dict(weight, id="GetHeat", thread="main", csharp={"member": "Heat", "kind": "getter"})
		area = bindings.parse_area(self.write([weight, weigh, cook, chef, size, pinned], slots=True, natives=[recipe], instance="Recipe",
											  **{"class": "Shalltear.Recipe"}))
		self.assertEqual([True, True, False, False, False, False], [function.any_thread for function in area.functions[:6]])
		managed = bindings.render_managed_area(area)
		self.assertNotIn('ThrowOffMainThread("Recipe.Weight")', managed)
		self.assertIn('ThrowOffMainThread("Recipe.Cook")', managed)
		self.assertIn('ThrowOffMainThread("Recipe.Chef")', managed)
		native = bindings.render_native_area(area)
		self.assertIn("\t\tint32_t GameGetWeight(const uint64_t* arguments, uint64_t* results) noexcept {\n\t\t\treturn ScriptRuntime::Guard([&] {\n", native)
		self.assertIn("\t\t\t\tself->Cook();\n\t\t\t});\n\t\t\tManagedTypes::FlushDirty();\n", native)

	def test_package_test_area_stubs_its_table(self):
		sets = self.package_sets([GETTER], test=True)
		files = bindings.render_sets(bindings.load_sets(sets), 7)
		thunks = files[sets[1].native / "Game.generated.cpp"]
		self.assertIn("#else\n\nnamespace Shalltear::CSharpImpl::Generated {\n\n\tvoid FillGameBindings(SltGameBindings&) {}\n", thunks)

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
		self.assertIn("\t\tWrap(Box.ENGINE_TYPE, static id => new Box(id));\n\t\tWrap<Shape>(Shape.ENGINE_TYPE, static id => new OpaqueShape(id));\n", wrappers)
		self.assertIn("internal sealed class OpaqueShape : Shape {\n\tinternal OpaqueShape(ulong id) {\n\t\t_id = id;\n\t}\n}\n", bindings.render_managed_area(base))
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
						 self.refusal([fill], structs=[hit]))

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


	def test_slots_area_takes_structs_lists_natives_and_outs(self):
		shape = {"name": "Shape", "cpp": "::Game::Shape", "size": 16, "align": 4,
				 "fields": [{"name": "width", "type": "f32", "offset": 0}, {"name": "at", "type": "Vec3", "offset": 4, "cpp": "position"}]}
		tone = {"name": "Tone", "cpp": "::Game::Tone", "underlying": "i8", "values": [{"name": "Low", "value": -2}]}
		recipe = {"name": "Recipe", "cpp": "::Game::Recipe", "owned": True, "create": True}
		grow = {"id": "Grow", "cpp": "::Game::Grow(shape, tone, count, label)", "returns": "Shape",
				"params": [{"name": "shape", "type": "Shape"}, {"name": "tone", "type": "Tone"}, {"name": "count", "type": "i16", "out": True},
						   {"name": "label", "type": "string", "out": True}],
				"csharp": {"member": "Grow", "kind": "method"}}
		mix = {"id": "Mix", "cpp": "::Game::Mix(*recipe, parts, names)", "returns": "list<Shape>",
			   "params": [{"name": "recipe", "type": "Recipe"}, {"name": "parts", "type": "list<f64>"}, {"name": "names", "type": "list<string>"}],
			   "csharp": {"member": "Mix", "kind": "method"}}
		make = {"id": "Make", "cpp": "::Game::Make()", "returns": "Recipe", "params": [], "owns": True, "nullable": False,
				"csharp": {"member": "Make", "kind": "method"}}
		area = bindings.parse_area(self.write([grow, mix, make], slots=True, structs=[shape], enums=[tone], natives=[recipe], include=["Game.h", "Recipe.h"]))
		self.assertEqual(["Grow", "Mix", "Make", "RecipeDelete", "RecipeNew"], [function.id for function in area.functions])

		native = bindings.render_native_area(area)
		self.assertIn('#include "SlotSupport.h"\n\n#include "Game.h"\n#include "Recipe.h"\n', native)
		self.assertIn("\tstatic_assert(sizeof(::Game::Shape) == 16 && std::is_trivially_copyable_v<::Game::Shape>);\n", native)
		self.assertIn("\tstatic_assert(offsetof(::Game::Shape, position) == 4);\n", native)
		self.assertIn("\t\tint32_t GameGrow(const uint64_t* arguments, uint64_t* results) noexcept {\n", native)
		self.assertIn("\t\t\t\tconst ::Game::Shape shape = Slots::Struct<::Game::Shape>(arguments[0]);\n", native)
		self.assertIn("\t\t\t\tconst ::Game::Tone tone = static_cast<::Game::Tone>(static_cast<int8_t>(arguments[1]));\n", native)
		self.assertIn("\t\t\t\tint16_t count{};\n\t\t\t\tstr label{};\n", native)
		self.assertIn("\t\t\t\tSlots::StructOut<::Game::Shape>(results[0], ::Game::Grow(shape, tone, count, label));\n", native)
		self.assertIn("\t\t\t\tresults[1] = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(count)));\n\t\t\t\tSlots::Text(results + 2, label);\n", native)
		self.assertIn("\t\t\t\tconst auto names = Slots::Views(arguments + 3);\n", native)
		self.assertIn("\t\t\t\tSlots::KeepList<::Game::Shape>(results, ::Game::Mix(*recipe, parts, names));\n", native)
		self.assertIn("\t\t\t\tresults[0] = Slots::Box<::Game::Recipe>(::Game::Make());\n", native)
		self.assertIn("\t\tint32_t GameRecipeNew([[maybe_unused]] const uint64_t* arguments, uint64_t* results) noexcept {\n", native)
		self.assertIn("\t\t\t\tSlots::ForgetFields(target), delete target;\n", native)
		self.assertIn("\t\t\t\tresults[0] = Slots::Pointer<::Game::Recipe>(new ::Game::Recipe());\n", native)

		managed = bindings.render_managed_area(area)
		self.assertIn("#pragma warning disable CS1591\n", managed)
		self.assertIn("[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Explicit, Size = 16)]\npublic partial struct Shape {\n", managed)
		self.assertIn("\t[System.Runtime.InteropServices.FieldOffset(4)]\n\tpublic Vec3 at;\n", managed)
		self.assertIn("public sealed unsafe partial class Recipe : NativeObject {\n\tinternal Recipe(nint pointer, bool owned) : base(pointer, owned ? Bindings.table.gameRecipeDelete : null) {}\n\n"
					  "\t/// <summary>Wrapper of the C++ object at an address (not owned, null at 0).</summary>\n\tpublic static Recipe? FromAddress(ulong address) {\n"
					  "\t\treturn address == 0 ? null : new Recipe((nint)address, false);\n\t}\n\n\tpublic Recipe() : this(Create(), true) {}\n", managed)
		self.assertIn("\tpublic static Shape Grow(in Shape shape, Tone tone, out short count, out string label) {\n", managed)
		self.assertIn("\t\tShape shapeCopy = shape;\n\t\tulong* slots = stackalloc ulong[6];\n", managed)
		self.assertIn("\t\tslots[1] = (ulong)(long)(sbyte)tone;\n", managed)
		self.assertIn("\t\tint status = Bindings.table.gameGrow(slots, slots + 2);\n", managed)
		self.assertIn("\t\tcount = (short)slots[3];\n\t\tlabel = SlotValues.Text(slots + 4);\n\t\treturn result;\n", managed)
		self.assertIn("\tpublic static Shape[] Mix(Recipe recipe, ReadOnlySpan<double> parts, ReadOnlySpan<string> names) {\n", managed)
		self.assertIn("\t\tusing SlotList namesList = SlotList.OfTexts(names);\n", managed)
		self.assertIn("\t\tfixed (double* partsData = parts) {\n", managed)
		self.assertIn("\t\t\treturn SlotValues.Array<Shape>(slots + 5);\n", managed)
		self.assertIn("\tpublic static Recipe Make() {\n", managed)
		self.assertIn("\t\treturn NativeObject.Wrap(slots[0], true, static (pointer, owned) => new Recipe(pointer, owned), null)!;\n", managed)
		self.assertNotIn("RecipeDelete(", managed)

		self.assertIn("\tint32_t (*gameGrow)(const uint64_t* arguments, uint64_t* results);\n", bindings.render_native_header([area], 0))
		self.assertIn("\tpublic delegate* unmanaged<ulong*, ulong*, int> gameRecipeNew;\n", bindings.render_managed_table([area]))
		moved = dict(shape, size=20, fields=[shape["fields"][0], dict(shape["fields"][1], offset=8)])
		other = bindings.parse_area(self.write([grow, mix, make], slots=True, structs=[moved], enums=[tone], natives=[recipe]))
		self.assertNotEqual(bindings.layout_hash([area]), bindings.layout_hash([other]))

	def test_slots_area_takes_callbacks_constants_and_out_lists(self):
		recipe = {"name": "Recipe", "cpp": "::Game::Recipe", "owned": True}
		tone = {"name": "Tone", "cpp": "::Game::Tone", "underlying": "i8", "values": [{"name": "Low", "value": -2}]}
		shape = {"name": "Shape", "cpp": "::Game::Shape", "size": 8, "align": 4,
				 "fields": [{"name": "width", "type": "f32", "offset": 0}, {"name": "tone", "type": "Tone", "offset": 4}]}
		constants = [{"name": "LIMIT", "cpp": "::Game::LIMIT", "type": "i32", "value": 42},
					 {"name": "MASK", "cpp": "::Game::MASK", "type": "u64", "value": 0xFFFFFFFFFFFFFFFF},
					 {"name": "FLOOR", "cpp": "::Game::FLOOR", "type": "i64", "value": -0x8000000000000000},
					 {"name": "SCALE", "cpp": "::Game::SCALE", "type": "f32", "value": "0x1.99999a0000000p-4"},
					 {"name": "FAR", "cpp": "::Game::FAR", "type": "f64", "value": "inf"},
					 {"name": "ON", "cpp": "::Game::ON", "type": "bool", "value": True},
					 {"name": "Title", "cpp": "::Game::Title", "type": "string", "value": "a \"game\""}]
		primes = {"id": "Primes", "cpp": "::Game::Primes(count, outPrimes, outRecipes)", "returns": "void",
				  "params": [{"name": "count", "type": "i32"}, {"name": "outPrimes", "type": "list<i32>", "out": True, "cpp": "::Game::Vector<int>"},
							 {"name": "outRecipes", "type": "list<Recipe>", "out": True, "cpp": "::Game::Vector<::Game::Recipe>"}],
				  "csharp": {"member": "Primes", "kind": "method"}}
		recipes = {"id": "Recipes", "cpp": "::Game::Recipes()", "returns": "list<Recipe>", "params": [], "owns": True, "csharp": {"member": "Recipes", "kind": "method"}}
		visitor = "std::function<bool(::Shalltear::SceneObject*, const ::Game::Shape&, ::Game::Tone, std::string_view)>"
		visit = {"id": "Visit", "cpp": "::Game::Visit(*root, visitor, done)", "returns": "i32",
				 "params": [{"name": "root", "type": "SceneObject"}, {"name": "visitor", "type": "fn<bool;SceneObject;Shape;Tone;string>", "cpp": visitor},
							{"name": "done", "type": "fn<void>", "optional": True, "cpp": "std::function<void()>"}],
				 "csharp": {"member": "Visit", "kind": "method"}}
		name = {"id": "Name", "cpp": "::Game::Name(make)", "returns": "void",
				"params": [{"name": "make", "type": "fn<string;Recipe;Vec3>", "cpp": "std::function<::Shalltear::str(const ::Game::Recipe&, ::Shalltear::Vec3)>"}],
				"csharp": {"member": "Name", "kind": "method"}}
		area = bindings.parse_area(self.write([primes, recipes, visit, name], slots=True, structs=[shape], enums=[tone], natives=[recipe], constants=constants))

		native = bindings.render_native_area(area)
		self.assertIn("\tstatic_assert(static_cast<int64_t>(::Game::LIMIT) == 42ll);\n\tstatic_assert(static_cast<uint64_t>(::Game::MASK) == 18446744073709551615ull);\n"
					  "\tstatic_assert(static_cast<int64_t>(::Game::FLOOR) == (-9223372036854775807ll - 1));\n"
					  "\tstatic_assert(static_cast<float>(::Game::SCALE) == 0x1.99999a0000000p-4f);\n"
					  "\tstatic_assert((::Game::FAR) == std::numeric_limits<double>::infinity());\n\tstatic_assert((::Game::ON) == true);\n"
					  "\tstatic_assert(std::string_view(::Game::Title) == std::string_view(\"a \\\"game\\\"\"));\n", native)
		self.assertIn("\t\t\t\t::Game::Vector<int> outPrimes{};\n\t\t\t\t::Game::Vector<::Game::Recipe> outRecipes{};\n\t\t\t\t::Game::Primes(count, outPrimes, outRecipes);\n"
					  "\t\t\t\tSlots::KeepList<int32_t>(results, outPrimes);\n\t\t\t\tSlots::KeepBoxes<::Game::Recipe>(results + 2, outRecipes);\n", native)
		self.assertIn("\t\t\t\tSlots::KeepBoxes<::Game::Recipe>(results, ::Game::Recipes());\n", native)
		self.assertIn(f"\t\t\t\t{visitor} visitor;\n\t\t\t\tif (const auto visitorHold = Slots::HoldCallback(arguments[1])) {{\n"
					  f"\t\t\t\t\tvisitor = [visitorHold](const auto& a0, const auto& a1, const auto& a2, const auto& a3) -> Slots::ResultOf<{visitor}> {{\n"
					  "\t\t\t\t\t\tuint64_t callArguments[5]{};\n\t\t\t\t\t\tuint64_t callResults[1]{};\n\t\t\t\t\t\tcallArguments[0] = Slots::IdOfItem(a0);\n"
					  "\t\t\t\t\t\tcallArguments[1] = Slots::Pointer<::Game::Shape>(a1);\n"
					  "\t\t\t\t\t\tcallArguments[2] = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(a2)));\n"
					  "\t\t\t\t\t\tSlots::Text(callArguments + 3, a3);\n"
					  "\t\t\t\t\t\tif (Slots::InvokeCallback(*visitorHold, callArguments, callResults) != SLT_OK) {\n"
					  f"\t\t\t\t\t\t\treturn Slots::ResultOf<{visitor}>();\n\t\t\t\t\t\t}}\n"
					  f"\t\t\t\t\t\treturn static_cast<Slots::ResultOf<{visitor}>>(callResults[0] != 0);\n\t\t\t\t\t}};\n\t\t\t\t}}\n", native)
		self.assertIn("\t\t\t\t\tdone = [doneHold]() -> Slots::ResultOf<std::function<void()>> {\n", native)
		self.assertIn("\t\t\t\t\t\t\treturn;\n", native)
		self.assertIn("\t\t\t\t\t\tcallArguments[0] = Slots::Pointer<::Game::Recipe>(a0);\n\t\t\t\t\t\tSlots::Write(callArguments + 1, ::Shalltear::Vec3(a1));\n", native)
		self.assertIn("(Slots::View(callResults));\n", native)

		managed = bindings.render_managed_area(area)
		self.assertIn("\tpublic const int LIMIT = 42;\n\tpublic const ulong MASK = 18446744073709551615;\n\tpublic const long FLOOR = long.MinValue;\n"
					  "\tpublic const float SCALE = 0.1f;\n\tpublic const double FAR = double.PositiveInfinity;\n\tpublic const bool ON = true;\n"
					  "\tpublic const string Title = \"a \\\"game\\\"\";\n", managed)
		self.assertIn("\tpublic static void Primes(int count, out int[] outPrimes, out Recipe[] outRecipes) {\n", managed)
		self.assertIn("\t\toutPrimes = SlotValues.Array<int>(slots + 1);\n"
					  "\t\toutRecipes = SlotValues.Natives(slots + 3, static (pointer, owned) => new Recipe(pointer, owned), true, null);\n", managed)
		self.assertIn("\t\treturn SlotValues.Natives(slots, static (pointer, owned) => new Recipe(pointer, owned), true, null);\n", managed)
		self.assertIn("\tpublic static int Visit(SceneObject root, Func<SceneObject, Shape, Tone, string, bool> visitor, Action? done) {\n", managed)
		self.assertIn("\t\tslots[1] = SlotCallbacks.Keep(VisitVisitorCallback(visitor));\n\t\tslots[2] = SlotCallbacks.Keep(done is null ? null : VisitDoneCallback(done));\n",
					  managed)
		self.assertIn("\t\tif (status != Bridge.OK) {\n\t\t\tSlotCallbacks.Drop(slots[1]);\n\t\t\tSlotCallbacks.Drop(slots[2]);\n", managed)
		self.assertIn("\tstatic SlotCallback VisitVisitorCallback(Func<SceneObject, Shape, Tone, string, bool> handler) {\n\t\treturn (slots, results) => {\n"
					  "\t\t\tresults[0] = handler(ObjectCache.SceneObject(slots[0])!, (slots[1] == 0 ? default : *(Shape*)slots[1]), (Tone)(sbyte)slots[2], "
					  "SlotValues.Text(slots + 3)) ? 1UL : 0UL;\n",
					  managed)
		self.assertIn("\tstatic SlotCallback VisitDoneCallback(Action handler) {\n\t\treturn (slots, results) => {\n\t\t\thandler();\n", managed)
		self.assertIn("\t\t\tSlotCallbacks.KeepText(results, handler(NativeObject.Wrap(slots[0], false, static (pointer, owned) => new Recipe(pointer, owned), null)!, "
					  "SlotValues.Vec3(slots + 1)));\n", managed)

	def test_callbacks_take_written_values_and_return_structs_and_natives(self):
		recipe = {"name": "Recipe", "cpp": "::Game::Recipe", "owned": True}
		shape = {"name": "Shape", "cpp": "::Game::Shape", "size": 8, "align": 4,
				 "fields": [{"name": "width", "type": "f32", "offset": 0}, {"name": "count", "type": "i32", "offset": 4}]}

		def taking(name, token, cpp):
			return {"id": name, "cpp": f"::Game::{name}(handler)", "returns": "void", "params": [{"name": "handler", "type": token, "cpp": cpp}],
					"csharp": {"member": name, "kind": "method"}}

		fill = taking("Fill", "fn<void;ref:i32;ref:Shape;span<f32>;list<i32>;u64>",
					  "std::function<void(int&, ::Game::Shape&, std::span<float>, const std::vector<int>&, const void*)>")
		shaped = taking("Shaped", "fn<Shape;i32>", "std::function<::Game::Shape(int)>")
		cook = taking("Cook", "fn<give:Recipe>", "std::function<::Game::Recipe*()>")
		named = taking("Named", "fn<copy:Recipe>", "std::function<::Game::Recipe()>")
		peek = taking("Peek", "fn<u64;Recipe>", "std::function<void*(const ::Game::Recipe&)>")
		counts = taking("Counts", "fn<list<i32>>", "std::function<std::vector<int>()>")
		books = taking("Books", "fn<list<Recipe>>", "std::function<std::vector<::Game::Recipe>()>")
		area = bindings.parse_area(self.write([fill, shaped, cook, named, peek, counts, books], slots=True, structs=[shape], natives=[recipe]))

		native = bindings.render_native_area(area)
		self.assertIn("\t\t\t\t\thandler = [handlerHold](auto& a0, auto& a1, const auto& a2, const auto& a3, const auto& a4) -> ", native)
		self.assertIn("\t\t\t\t\t\tcallArguments[0] = Slots::Bits(&a0);\n\t\t\t\t\t\tcallArguments[1] = Slots::Bits(&a1);\n"
					  "\t\t\t\t\t\tcallArguments[2] = Slots::Bits(a2.data());\n\t\t\t\t\t\tcallArguments[3] = a2.size();\n"
					  "\t\t\t\t\t\tSlots::KeepList<int32_t>(callArguments + 4, a3);\n\t\t\t\t\t\tcallArguments[6] = Slots::Bits(a4);\n", native)
		self.assertIn("\t\t\t\t\t\t::Game::Shape made{};\n\t\t\t\t\t\tcallResults[0] = Slots::Bits(&made);\n", native)
		self.assertIn("\t\t\t\t\t\treturn static_cast<Slots::ResultOf<std::function<::Game::Shape(int)>>>(made);\n", native)
		self.assertIn("\t\t\t\t\t\treturn static_cast<Slots::ResultOf<std::function<::Game::Recipe*()>>>(reinterpret_cast<::Game::Recipe*>("
					  "static_cast<uintptr_t>(callResults[0])));\n", native)
		self.assertIn("\t\t\t\t\t\tconst auto* const made = reinterpret_cast<::Game::Recipe*>(static_cast<uintptr_t>(callResults[0]));\n"
					  "\t\t\t\t\t\treturn made ? static_cast<Slots::ResultOf<std::function<::Game::Recipe()>>>(*made) : "
					  "Slots::ResultOf<std::function<::Game::Recipe()>>();\n", native)
		self.assertIn("\t\t\t\t\t\treturn Slots::FromBits<Slots::ResultOf<std::function<void*(const ::Game::Recipe&)>>>(callResults[0]);\n", native)

		self.assertIn("\t\t\t\t\t\tconst auto* const items = reinterpret_cast<const int32_t*>(static_cast<uintptr_t>(callResults[0]));\n"
					  "\t\t\t\t\t\treturn Slots::ResultOf<std::function<std::vector<int>()>>(items, items + callResults[1]);\n", native)
		self.assertIn("\t\t\t\t\t\treturn Slots::Copies<Slots::ResultOf<std::function<std::vector<::Game::Recipe>()>>>(std::span<::Game::Recipe* const>("
					  "reinterpret_cast<::Game::Recipe* const*>(static_cast<uintptr_t>(callResults[0])), static_cast<size_t>(callResults[1])));\n", native)

		managed = bindings.render_managed_area(area)
		self.assertIn("\tpublic static void Counts(Func<int[]?> handler) {\n", managed)
		self.assertIn("\t\t\tSlotCallbacks.KeepArray(results, handler());\n", managed)
		self.assertIn("\t\t\tSlotCallbacks.KeepNatives(results, handler());\n", managed)
		self.assertIn("\tpublic static void Fill(Action<Ref<int>, Ref<Shape>, Span<float>, int[], ulong> handler) {\n", managed)
		self.assertIn("\t\t\thandler(new Ref<int>(slots[0]), new Ref<Shape>(slots[1]), new Span<float>((void*)slots[2], (int)slots[3]), "
					  "SlotValues.Array<int>(slots + 4), (ulong)slots[6]);\n", managed)
		self.assertIn("\tpublic static void Shaped(Func<int, Shape> handler) {\n", managed)
		self.assertIn("\t\t\t*(Shape*)results[0] = handler((int)slots[0]);\n", managed)
		self.assertIn("\tpublic static void Cook(Func<Recipe?> handler) {\n", managed)
		self.assertIn("\t\t\tRecipe? made = handler();\n\t\t\tresults[0] = made is null ? 0UL : made.Give(\"handler\");\n", managed)
		self.assertIn("\t\t\tRecipe? made = handler();\n\t\t\tresults[0] = made is null ? 0UL : made.Pointer;\n", managed)

		for token in ("fn<give:Shape>", "fn<copy:i32>", "fn<void;ref:string>", "fn<void;ref:Recipe>", "fn<void;list<Shape>;give:Recipe>"):
			message = self.refusal([dict(cook, params=[dict(cook["params"][0], type=token)])], slots=True, structs=[shape], natives=[recipe])
			self.assertTrue(message.startswith(f"Game.json: Cook: unknown type '{token}'. Use bool, i32,"), message)

	def test_natives_are_shared_adopted_borrowed_and_found_by_address(self):
		recipe = {"name": "Recipe", "cpp": "::Game::Recipe", "owned": True, "create": True}
		share = {"id": "Share", "cpp": "::Game::Share()", "returns": "Recipe", "params": [], "shared": True, "csharp": {"member": "Share", "kind": "method"}}
		adopt = {"id": "Adopt", "cpp": "::Game::Adopt().release()", "returns": "Recipe", "params": [], "adopts": True, "csharp": {"member": "Adopt", "kind": "method"}}
		keep = {"id": "Keep", "cpp": "::Game::Keep(first, second, std::move(third))", "returns": "void",
				"params": [{"name": "first", "type": "Recipe", "optional": True, "shared": "take"},
						   {"name": "second", "type": "Recipe", "optional": True, "shared": "alias"},
						   {"name": "third", "type": "Recipe", "optional": True, "shared": "unique"}],
				"csharp": {"member": "Keep", "kind": "method"}}
		found = {"id": "Found", "cpp": "::Game::Found(recipes)", "returns": "void",
				 "params": [{"name": "recipes", "type": "list<Recipe>", "out": True, "cpp": "std::vector<const ::Game::Recipe*>", "borrowed": True}],
				 "csharp": {"member": "Found", "kind": "method"}}
		points = {"id": "Points", "cpp": "::Game::Points(points)", "returns": "void", "params": [{"name": "points", "type": "span<Vec3>", "out": True}],
				  "csharp": {"member": "Points", "kind": "method"}}
		prelude = ["class GameSink final : public ::Game::Sink {", "};"]
		area = bindings.parse_area(self.write([share, adopt, keep, found, points], slots=True, natives=[recipe], prelude=prelude))

		native = bindings.render_native_area(area)
		self.assertIn("\tnamespace {\n\n\t\tclass GameSink final : public ::Game::Sink {\n\t\t};\n\n\t} // anonymous namespace\n", native)
		self.assertIn("\t\t\t\tresults[0] = Slots::KeepShared<::Game::Recipe>(::Game::Share());\n", native)
		self.assertIn("\t\t\t\tresults[0] = Slots::Pointer<::Game::Recipe>(::Game::Adopt().release());\n", native)
		self.assertIn("\t\t\t\tconst std::shared_ptr<::Game::Recipe> first = Slots::Shared<::Game::Recipe>(arguments[0]);\n"
					  "\t\t\t\tconst std::shared_ptr<::Game::Recipe> second = Slots::Shared<::Game::Recipe>(arguments[1]);\n"
					  "\t\t\t\tstd::unique_ptr<::Game::Recipe> third(reinterpret_cast<::Game::Recipe*>(static_cast<uintptr_t>(arguments[2])));\n", native)
		self.assertIn("\t\t\t\tstd::vector<const ::Game::Recipe*> recipes{};\n\t\t\t\t::Game::Found(recipes);\n"
					  "\t\t\t\tSlots::KeepPointers(results, recipes);\n", native)
		self.assertIn("\t\t\t\tconst std::span<::Shalltear::Vec3> points(reinterpret_cast<::Shalltear::Vec3*>(static_cast<uintptr_t>(arguments[0])), "
					  "static_cast<size_t>(arguments[1]));\n", native)

		managed = bindings.render_managed_area(area)
		self.assertIn("\t\treturn NativeObject.WrapShared(slots[0], static (pointer, owned) => new Recipe(pointer, owned));\n", managed)
		self.assertIn("\t\treturn NativeObject.Wrap(slots[0], true, static (pointer, owned) => new Recipe(pointer, owned), null);\n", managed)
		self.assertIn("\t\tslots[0] = first is null ? 0UL : first.Share(true);\n", managed)
		self.assertIn("\t\tslots[1] = second is null ? 0UL : second.Share(false);\n", managed)
		self.assertIn("\t\tslots[2] = third is null ? 0UL : third.Give(\"Game.Keep\");\n", managed)
		self.assertIn("\t\trecipes = SlotValues.Natives(slots, static (pointer, owned) => new Recipe(pointer, owned), false, null);\n", managed)
		self.assertIn("\tpublic static void Points(Span<Vec3> points) {\n", managed)

		self.assertEqual("Game.json: Share: 'shared' is for a returned shared_ptr of a native, without 'owns' and 'adopts'",
						 self.refusal([dict(share, owns=True)], slots=True, natives=[recipe]))
		self.assertEqual("Game.json: Found: Recipe is copied for the script. Mark the native 'owned'",
						 self.refusal([dict(found, params=[dict(found["params"][0], borrowed=False)])], slots=True, natives=[dict(recipe, owned=False, create=False)]))
		self.assertNotEqual(bindings.layout_hash([area]),
							bindings.layout_hash([bindings.parse_area(self.write([dict(share, shared=False), adopt, keep, found, points], slots=True, natives=[recipe]))]))

	def test_callback_constant_and_out_list_rules(self):
		recipe = {"name": "Recipe", "cpp": "::Game::Recipe"}
		walk = {"id": "Walk", "cpp": "::Game::Walk(step)", "returns": "void", "params": [{"name": "step", "type": "fn<void;i32>", "cpp": "std::function<void(int)>"}],
				"csharp": {"member": "Walk", "kind": "method"}}
		self.assertEqual("Game.json: Walk: callback 'step' needs 'cpp' (std::function type) + no 'out'",
						 self.refusal([dict(walk, params=[{"name": "step", "type": "fn<void;i32>"}])], slots=True))
		self.assertEqual("Game.json: Walk: a callback needs a method", self.refusal([dict(walk, csharp={"member": "Walk", "kind": "setter"})], slots=True))
		self.assertIn("Game.json: Walk: unknown type 'fn<void;i32>'.", self.refusal([dict(walk, doc="Walks.")]))
		self.assertIn("Game.json: Walk: unknown type 'fn<list<string>>'.", self.refusal([dict(walk, params=[{"name": "step", "type": "fn<list<string>>", "cpp": "x"}])], slots=True))
		count = {"id": "Count", "cpp": "::Game::Count(value)", "returns": "void", "params": [{"name": "value", "type": "i32", "cpp": "int"}],
				 "csharp": {"member": "Count", "kind": "method"}}
		self.assertEqual("Game.json: Count: 'cpp' is the container of an out list or the type of a callback, 'value' is i32", self.refusal([count], slots=True))
		fill = {"id": "Fill", "cpp": "::Game::Fill(items)", "returns": "void", "params": [{"name": "items", "type": "list<i32>", "out": True}],
				"csharp": {"member": "Fill", "kind": "method"}}
		self.assertEqual("Game.json: Fill: 'out' is for a span, 'items' is list<i32>", self.refusal([fill], slots=True))
		made = {"id": "Made", "cpp": "::Game::Made()", "returns": "list<Recipe>", "params": [], "owns": True, "csharp": {"member": "Made", "kind": "method"}}
		self.assertEqual("Game.json: Made: Recipe is copied for the script. Mark the native 'owned'", self.refusal([made], slots=True, natives=[recipe]))
		many = {"id": "Many", "cpp": "::Game::Many()", "returns": "void",
				"params": [{"name": f"list{index}", "type": "list<i32>", "out": True, "cpp": "::Game::Vector<int>"} for index in range(9)],
				"csharp": {"member": "Many", "kind": "method"}}
		self.assertEqual("Game.json: Many: returns more than 8 lists or 16 strings", self.refusal([many], slots=True))
		limit = {"name": "LIMIT", "cpp": "::Game::LIMIT", "type": "u8", "value": 256}
		self.assertEqual("Game.json: LIMIT: 'value' 256 isn't a u8. Use bool/an integer type/f32/f64 (hex text, inf, -inf, nan)/string",
						 self.refusal([], slots=True, constants=[limit]))
		self.assertEqual("Game.json: LIMIT: 'constants' needs objects in a slots area", self.refusal([], constants=[dict(limit, value=1)]))
		self.assertEqual("Game.json: Walk: member 'Walk' is declared twice", self.refusal([walk], slots=True, constants=[dict(limit, name="Walk", value=1)]))

	def test_struct_may_name_a_struct_declared_later(self):
		point = {"name": "Point", "cpp": "::Game::Point", "size": 8, "align": 4, "fields": [{"name": "x", "type": "f32", "offset": 0}, {"name": "y", "type": "f32", "offset": 4}]}
		line = {"name": "Line", "cpp": "::Game::Line", "size": 16, "align": 4, "fields": [{"name": "start", "type": "Point", "offset": 0}, {"name": "end", "type": "Point", "offset": 8}]}
		area = bindings.parse_area(self.write([], slots=True, structs=[line, point]))
		self.assertEqual(["Point", "Line"], [declared.name for declared in area.structs])
		self.assertEqual("Game.json: Line: field 'start' can't be Point. Use a scalar, a value type, an enum or a struct", self.refusal([], slots=True, structs=[line]))

	def test_slot_kinds_are_refused_outside_a_slots_area(self):
		shape = {"name": "Shape", "doc": "Size.", "fields": [{"name": "width", "type": "f32", "doc": "Width."}]}
		grow = {"id": "Grow", "cpp": "::Game::Grow(shape)", "returns": "f32", "params": [{"name": "shape", "type": "Shape"}],
				"csharp": {"member": "Grow", "kind": "method"}, "doc": "Grown."}
		self.assertTrue(self.refusal([grow], structs=[shape]).startswith("Game.json: Grow: unknown type 'Shape'. Use bool,"))
		listed = dict(grow, params=[{"name": "values", "type": "list<f32>"}])
		self.assertTrue(self.refusal([listed]).startswith("Game.json: Grow: unknown type 'list<f32>'. Use bool,"))
		counted = dict(grow, params=[{"name": "count", "type": "i32", "out": True}])
		self.assertEqual("Game.json: Grow: 'out' is for a span, 'count' is i32", self.refusal([counted]))
		self.assertEqual("Game.json: Shape: 'cpp' is for a struct of a slots area",
						 self.refusal([GETTER], structs=[dict(shape, cpp="::Game::Shape", size=4, align=4)]))
		self.assertEqual("Game.json: Recipe: 'natives' needs objects in a slots area", self.refusal([GETTER], natives=[{"name": "Recipe", "cpp": "::Game::Recipe"}]))
		self.assertEqual("Game.json: 'include' is a list. Use 1 header (or a slots area)", self.refusal([GETTER], include=["Game.h"]))

	def test_slots_area_rules(self):
		plain = {key: value for key, value in GETTER.items() if key not in ("leaf", "doc")}
		bindings.parse_area(self.write([plain], slots=True))
		self.assertEqual("Game.json: GetSpeed: 'leaf' isn't for a slots area", self.refusal([dict(plain, leaf=True)], slots=True))
		self.assertEqual("Game.json: Recipe: 'create' needs 'owned'",
						 self.refusal([plain], slots=True, natives=[{"name": "Recipe", "cpp": "::Game::Recipe", "create": True}]))
		self.assertEqual("Game.json: Shape: 'size', 'align' and each 'offset' must be numbers inside the struct",
						 self.refusal([plain], slots=True, structs=[{"name": "Shape", "cpp": "::Game::Shape", "size": 4, "align": 4,
																	 "fields": [{"name": "at", "type": "Vec3", "offset": 0}]}]))
		self.assertEqual("Game.json: GetSpeed: 'owns' is for a returned native object, this returns f32", self.refusal([dict(plain, owns=True)], slots=True))
		outward = dict(plain, params=[{"name": "count", "type": "i32", "out": True}])
		self.assertEqual("Game.json: GetSpeed: an 'out' parameter needs a method", self.refusal([outward], slots=True))


if __name__ == "__main__":
	unittest.main()
