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
		areas = bindings.load_areas(bindings.BINDINGS)
		digest = bindings.input_digest(bindings.BINDINGS, Path(bindings.__file__).resolve())
		stale = bindings.find_stale(bindings.render(areas, digest))
		self.assertEqual([], [path.name for path in stale])

	def test_unknown_type_is_refused(self):
		broken = copy.deepcopy(METHOD)
		broken["params"][2]["type"] = "Vec5"
		message = self.refusal([GETTER, broken])
		self.assertTrue(message.startswith("Game.json: Describe: unknown type 'Vec5'. Use bool, i32,"), message)

	def test_unknown_return_is_refused(self):
		broken = dict(GETTER, returns="string")
		self.assertEqual("Game.json: GetSpeed: can't return 'string'. Use void, bool, i32, u32, i64, u64, f32, f64, Vec2, Vec3, Vec4, Quat, Color",
						 self.refusal([broken]))

	def test_area_is_named_like_its_file(self):
		self.assertEqual("Game.json: 'area' is 'Rules'. Name it like the file (Game)", self.refusal([GETTER], area="Rules"))

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

	def test_out_needs_a_span(self):
		broken = copy.deepcopy(METHOD)
		broken["params"][2]["out"] = True
		self.assertEqual("Game.json: Describe: 'out' is for a span, 'origin' is Vec3", self.refusal([broken]))

	def test_unknown_key_is_refused(self):
		broken = dict(GETTER, leef=True)
		self.assertTrue(self.refusal([broken]).startswith("Game.json: GetSpeed: unknown key 'leef'. Use "))

	def test_layout_hash_follows_signatures(self):
		def hashed(functions):
			return bindings.layout_hash([bindings.parse_area(self.write(functions))])

		base = hashed([GETTER, SETTER, METHOD])
		self.assertEqual(base, hashed([dict(GETTER, doc="Other words."), SETTER, METHOD]))
		self.assertNotEqual(base, hashed([SETTER, GETTER, METHOD]))
		self.assertNotEqual(base, hashed([dict(GETTER, returns="f64"), dict(SETTER, params=[{"name": "speed", "type": "f64"}]), METHOD]))
		self.assertNotEqual(base, hashed([dict(GETTER, leaf=False), SETTER, METHOD]))
		plain = copy.deepcopy(METHOD)
		del plain["params"][1]["out"]
		self.assertNotEqual(base, hashed([GETTER, SETTER, plain]))
		bindings.LOWERING += 1
		try:
			self.assertNotEqual(base, hashed([GETTER, SETTER, METHOD]))
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
					  "::Shalltear::Vec3* outResult) noexcept {\n\t\t\treturn ScriptRuntime::Guard([&] {\n", native)
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
		self.assertIn("\t\tint32_t GameFindSpin(SltQuat* outResult) noexcept {\n\t\t\treturn ScriptRuntime::Guard([&] {\n"
					  "\t\t\t\t*outResult = FromEngine(::Game::FindSpin());\n", native)

	def test_property_joins_getter_and_setter(self):
		managed = bindings.render_managed_area(bindings.parse_area(self.write([GETTER, SETTER, METHOD])))
		self.assertIn("\t/// <summary>Units per second.</summary>\n\tpublic static float Speed {\n\t\tget {\n", managed)
		self.assertIn("\t\tset {\n\t\t\tif (Bridge.OffMainThread) {\n\t\t\t\tBridge.ThrowOffMainThread(\"Game.Speed\");\n\t\t\t}\n"
					  "\t\t\tBindings.table.gameSetSpeed(value);\n\t\t}\n", managed)
		self.assertIn("\tpublic static Vec3 Describe(string label, Span<float> samples, Vec3 origin) {\n", managed)
		self.assertIn("\t\tfixed (float* samplesData = samples) {\n\t\t\tVec3 result;\n"
					  "\t\t\tint status = Bindings.table.gameDescribe(labelText.Text, samplesData, samples.Length, origin.x, origin.y, origin.z, &result);\n",
					  managed)

	def test_any_thread_skips_the_check(self):
		free = dict(GETTER, thread="any")
		managed = bindings.render_managed_area(bindings.parse_area(self.write([free])))
		self.assertNotIn("OffMainThread", managed)
		self.assertEqual("Game.json: GetSpeed: 'thread' is 'worker'. Use main/any", self.refusal([dict(GETTER, thread="worker")]))

	def test_test_area_stays_out_of_shipped_builds(self):
		area = bindings.parse_area(self.write([GETTER], test=True))
		native = bindings.render_native_area(area)
		self.assertIn("#ifdef SLT_CSHARP_TEST_HOOKS\n", native)
		self.assertIn("#else\n\nnamespace Shalltear::CSharpImpl::Generated {\n\n\tvoid FillGameBindings(SltBindings&) {}\n", native)
		self.assertIn("internal static unsafe partial class Game {\n", bindings.render_managed_area(area))

	def test_digest_follows_schemas_and_script(self):
		script = self.directory / "script.py"
		script.write_text("first", encoding="utf-8")
		self.write([GETTER])
		before = bindings.input_digest(self.directory, script)
		self.write([GETTER, SETTER])
		changed = bindings.input_digest(self.directory, script)
		script.write_text("second", encoding="utf-8")
		self.assertNotEqual(before, changed)
		self.assertNotEqual(changed, bindings.input_digest(self.directory, script))


if __name__ == "__main__":
	unittest.main()
