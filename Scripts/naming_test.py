import collections
import contextlib
import importlib.util
import io
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
import naming

codemod = None
if importlib.util.find_spec("clang") is not None:
	import naming_codemod as codemod

DRAFT = naming.Dictionary({
	"initialisms": ["API", "URL", "ID", "LOD", "GPU", "UV", "AABB", "FS", "UI",
					"SDK", "XML", "HTTP", "UTF", "SHA", "UID", "2D", "3D"],
	"fixed": ["iOS", "sRGB", "glTF", "npm", "eBay", "OAuth", "D3D11"],
	"split": {"filesystem": ["file", "system"], "datetime": ["date", "time"]},
	"prefixes": {},
})

EXAMPLES = [
	("StaleSDK",		"StaleSDK",			"staleSDK",			"STALE_SDK",		"stale-sdk"),
	("APIKey",			"APIKey",			"apiKey",			"API_KEY",			"api-key"),
	("FS",				"FS",				"fs",				"FS",				"fs"),
	("UIDocument",		"UIDocument",		"uiDocument",		"UI_DOCUMENT",		"ui-document"),
	("UserID",			"UserID",			"userID",			"USER_ID",			"user-id"),
	("XMLHTTPRequest",	"XMLHTTPRequest",	"xmlHTTPRequest",	"XML_HTTP_REQUEST",	"xml-http-request"),
	("GPUBuffer",		"GPUBuffer",		"gpuBuffer",		"GPU_BUFFER",		"gpu-buffer"),
	("AABBTree",		"AABBTree",			"aabbTree",			"AABB_TREE",		"aabb-tree"),
	("UVCoords",		"UVCoords",			"uvCoords",			"UV_COORDS",		"uv-coords"),
	("FileSystem",		"FileSystem",		"fileSystem",		"FILE_SYSTEM",		"file-system"),
	("OAuthToken",		"OAuthToken",		"oauthToken",		"OAUTH_TOKEN",		"oauth-token"),
	("iOSDevice",		"iOSDevice",		"iOSDevice",		"IOS_DEVICE",		"ios-device"),
	("sRGBToLinear",	"sRGBToLinear",		"sRGBToLinear",		"SRGB_TO_LINEAR",	"srgb-to-linear"),
	("glTFLoader",		"glTFLoader",		"glTFLoader",		"GLTF_LOADER",		"gltf-loader"),
	("TimeoutMs",		"TimeoutMs",		"timeoutMs",		"TIMEOUT_MS",		"timeout-ms"),
	("GainDb",			"GainDb",			"gainDb",			"GAIN_DB",			"gain-db"),
	("UTF8Encoder",		"UTF8Encoder",		"utf8Encoder",		"UTF8_ENCODER",		"utf8-encoder"),
	("SHA256Hash",		"SHA256Hash",		"sha256Hash",		"SHA256_HASH",		"sha256-hash"),
	("Base64Decode",	"Base64Decode",		"base64Decode",		"BASE64_DECODE",	"base64-decode"),
	("Vec2D",			"Vec2D",			"vec2D",			"VEC_2D",			"vec-2d"),
	("Physics2DWorld",	"Physics2DWorld",	"physics2DWorld",	"PHYSICS_2D_WORLD",	"physics-2d-world"),
	("IDs",				"IDs",				"ids",				"IDS",				"ids"),
	("MaxLODs",			"MaxLODs",			"maxLODs",			"MAX_LODS",			"max-lods"),
	("ParseURLs",		"ParseURLs",		"parseURLs",		"PARSE_URLS",		"parse-urls"),
]

MIGRATION = [
	("StaleSdk",		"staleSDK"),
	("uiDocument",		"uiDocument"),
	("fs",				"fs"),
	("filesystem",		"fileSystem"),
	("XMLHTTPRequest",	"xmlHTTPRequest"),
	("HTTPSecurity",	"httpSecurity"),
	("Uid",				"uid"),
	("IDS",				"ids"),
	("MaxLODs",			"maxLODs"),
	("Box2d",			"box2D"),
	("items",			"items"),
]

REJOIN = [
	("aPIKey",			"apiKey"),
	("uIDocument",		"uiDocument"),
	("xMLHTTPRequest",	"xmlHTTPRequest"),
	("sHA256Hash",		"sha256Hash"),
	("iDs",				"ids"),
	("aList",			"aList"),
	("xAxis",			"xAxis"),
	("iCount",			"iCount"),
	("wZ",				"wZ"),
	("xY",				"xY"),
	("yZ",				"yZ"),
]

REPO = [
	("Ui",						"Pascal",	"UI"),
	("UiDocument",				"Pascal",	"UIDocument"),
	("UiDocumentAsset",			"camel",	"uiDocumentAsset"),
	("JsonValue",				"Pascal",	"JSONValue"),
	("RhiFixture",				"Pascal",	"RHIFixture"),
	("GpuPerDraw",				"Pascal",	"GPUPerDraw"),
	("Utf8ToWide",				"Pascal",	"UTF8ToWide"),
	("Rgba",					"Pascal",	"RGBA"),
	("Hsv",						"Pascal",	"HSV"),
	("HsvToRgb",				"Pascal",	"HSVToRGB"),
	("Fov",						"Pascal",	"FOV"),
	("Msaa",					"Pascal",	"MSAA"),
	("Guid",					"Pascal",	"Guid"),
	("GuidComponent",			"camel",	"guidComponent"),
	("typeId",					"camel",	"typeId"),
	("assetId",					"camel",	"assetId"),
	("Physics3D",				"Pascal",	"Physics3D"),
	("UISystem",				"Pascal",	"UISystem"),
	("VkDescriptorSet",			"Pascal",	"VkDescriptorSet"),
	("SpirvReflection",			"Pascal",	"SpirvReflection"),
	("_activeHost",				"camel",	"_activeHost"),
	("ASSET_GUID",				"CONST",	"ASSET_GUID"),
	("ARENA_BYTES",				"CONST",	"ARENA_BYTES"),
	("ATLAS",					"CONST",	"ATLAS"),
	("XYZ",						"Pascal",	"XYZ"),
	("ZXY",						"Pascal",	"ZXY"),
	("RotationOrderZYX",		"Pascal",	"RotationOrderZYX"),
	("UVCoords",				"camel",	"uvCoords"),
	("PlaneXZ",					"Pascal",	"PlaneXZ"),
	("DATA_ASSET_NOT_THE_ONLY_ONE",	"Pascal",	"DataAssetNotTheOnlyOne"),
	("TICK_LATE",				"Pascal",	"TickLate"),
	("STRING_POOL",				"camel",	"stringPool"),
	("GLTF_FLOAT",				"Pascal",	"glTFFloat"),
	("g_state",					"camel",	"_state"),
	("g_guidToId",				"camel",	"_guidToId"),
	("s_cache",					"camel",	"_cache"),
	("CalculateFNV1a",			"Pascal",	"CalculateFNV1a"),
]

ERRORS = [
	("NPCID",	"Pascal"),
	("NPCIDMap","Pascal"),
	("IUIAtlasBackend", "Pascal"),
]

CONTAINS = [
	("/repo/Shalltear",						True),
	("/repo/Shalltear/Source/Core/RHI.h",	True),
	("/repo/ShalltearSDK/Public/RHI.h",		False),
	("/repo/Shalltear/../Sibling/src/a.cpp", False),
	("/repo/Sibling/src/a.cpp",				False),
	("/repo",								False),
]


MERGE_PATHS = [("/repo/Source/Widget.h", "/repo/source/widget.h"),
			   ("/repo/Source/Widget.cpp", "/repo/source/widget.cpp")]


def merge_units(units, workers):
	t = dict(renames={}, sites=collections.defaultdict(list), seen=set(),
			 stats=collections.Counter(), scope=collections.defaultdict(dict),
			 targets=collections.defaultdict(set))
	mine = [set() for _ in range(max(workers, 1))]
	noise = io.StringIO()
	for i, (paths, records) in enumerate(units):
		if workers:
			seen, kept = mine[i % workers], []
			for rec in records:
				key = codemod.record_key(rec, paths)
				if key not in seen:
					seen.add(key)
					kept.append(rec)
			records = kept
		with contextlib.redirect_stderr(noise):
			codemod.merge(paths, records, t["renames"], t["sites"], t["seen"],
						  t["stats"], t["scope"], t["targets"])
	t["reported"] = noise.getvalue()
	return t


def merge_tables(t):
	return (sorted(t["renames"].items()),
			sorted((usr, e.key()) for usr, es in t["sites"].items() for e in es),
			sorted((k, sorted(v.items())) for k, v in t["scope"].items()),
			sorted((k, sorted(v)) for k, v in t["targets"].items()),
			t["stats"]["conflicts"])


def check_merge(check):
	S, D = codemod.SITE, codemod.DECL

	decl = (D, 0, 100, "m_Width", "FIELD_DECL", "usr:width", "usr:Widget", "width")
	use = (S, 0, 140, "m_Width", "MEMBER_REF_EXPR", "usr:width")
	sibling = (D, 0, 180, "height", "FIELD_DECL", "usr:height", "usr:Widget", None)
	units = [(MERGE_PATHS, [decl, use, sibling]),
			 (MERGE_PATHS, [(S, 1, 300, "m_Width", "DECL_REF_EXPR", "usr:width")]),
			 (MERGE_PATHS, [decl, use, sibling,
							(S, 1, 420, "m_Width", "DECL_REF_EXPR", "usr:width")])]

	one = merge_tables(merge_units(units, 0))
	for workers in (1, 2, 3):
		check("MERGE %d workers" % workers, merge_tables(merge_units(units, workers)), one)

	t = merge_units(units, 2)
	check("MERGE sites", len(t["sites"]["usr:width"]), 4)
	check("MERGE unrenamed occupies", t["scope"]["usr:Widget"].get("height"), "usr:height")
	check("MERGE target", sorted(t["targets"][("usr:Widget", "width")]), ["usr:width"])

	clash = [(MERGE_PATHS, [(D, 0, 500, "VAR_DECL", "VAR_DECL", "usr:v", "usr:f", "varDecl"),
							(S, 0, 500, "VAR_DECL", "TYPE_REF", "usr:other")])]
	t = merge_units(clash, 1)
	check("MERGE key spaces", (len(t["sites"]["usr:v"]), len(t["sites"]["usr:other"])), (1, 1))

	repeat = [(MERGE_PATHS, [decl]),
			  (MERGE_PATHS, [(D, 0, 100, "m_Width", "FIELD_DECL", "usr:width", "usr:Widget", "other")])]
	t = merge_units(repeat, 2)
	check("MERGE first writer", t["renames"]["usr:width"], "width")
	check("MERGE repeat is not a conflict", t["stats"]["conflicts"], 0)

	split = [(MERGE_PATHS, [decl]),
			 (MERGE_PATHS, [(D, 1, 700, "m_Width", "FIELD_DECL", "usr:width", "usr:Widget", "other")])]
	t = merge_units(split, 2)
	check("MERGE conflict counted", t["stats"]["conflicts"], 1)
	check("MERGE conflict keeps first", t["renames"]["usr:width"], "width")
	check("MERGE conflict reported", "resolves to both width and other" in t["reported"], True)
	return 11


TRAVERSAL_SNIPPET = """namespace {
	constexpr const char* TAG_ROOT = "ui";
}
struct View { const char* p; View(const char* s) : p(s) {} };
struct Str {
	const char* p;
	Str(View v) : p(v.p) {}
	operator View() const { return View(p); }
	Str operator+(View o) const { return *this; }
};
void Sink(View a, View b);
void use() {
	Str a = Str("x") + Str(TAG_ROOT);
	Sink(TAG_ROOT, "z");
}
"""


def check_traversal(check):
	cindex = codemod.cindex
	libclang = codemod.lint.LIBCLANG
	if libclang and os.path.exists(libclang):
		with contextlib.suppress(Exception):
			cindex.Config.set_library_file(libclang)
	root = os.path.dirname(os.path.dirname(os.path.abspath(codemod.__file__)))
	tmp = tempfile.mkdtemp(dir=root)
	try:
		src = os.path.join(tmp, "TraversalProbe.cpp")
		body = TRAVERSAL_SNIPPET.encode("utf-8")
		with open(src, "wb") as f:
			f.write(body)
		try:
			tu = cindex.Index.create().parse(src, args=["-std=c++20"])
		except Exception:
			print("SKIPPED: the traversal check needs a loadable libclang")
			return 0
		if [d for d in tu.diagnostics if d.severity >= 3]:
			print("SKIPPED: the traversal probe didn't compile")
			return 0
		_, records = codemod.collect(tu, root, naming.load(), codemod.load_config(),
									 codemod.lint.norm(tmp), (codemod.lint.norm(tmp),), set())
		got = sorted(r[2] for r in records
					 if r[0] == codemod.SITE and r[3] == "TAG_ROOT")
		hidden = body.index(b"TAG_ROOT", body.index(b"Str(TAG_ROOT"))
		plain = body.index(b"TAG_ROOT", body.index(b"Sink(TAG_ROOT"))
		check("TRAVERSAL under an implicit node", got, sorted([hidden, plain]))
	finally:
		shutil.rmtree(tmp, ignore_errors=True)
	return 1


ANON_UNRELATED_A = """namespace Shalltear {
	namespace {
		int g_state = 0;
	}
	int UseA() { return g_state; }
}
"""
ANON_UNRELATED_B = """namespace Shalltear {
	namespace {
		int g_state = 0;
	}
	int UseB() { return g_state; }
}
"""
ANON_OCCUPIED = """namespace Shalltear {
	namespace {
		int _state = 0;
		int g_state = 0;
	}
	int UseC() { return _state + g_state; }
}
"""
ANON_CONVERGE = """namespace Shalltear {
	namespace {
		int g_count = 0;
		int s_count = 0;
	}
	int UseD() { return g_count + s_count; }
}
"""


def check_anon_scope(check):
	cindex = codemod.cindex
	libclang = codemod.lint.LIBCLANG
	if libclang and os.path.exists(libclang):
		with contextlib.suppress(Exception):
			cindex.Config.set_library_file(libclang)
	root = os.path.dirname(os.path.dirname(os.path.abspath(codemod.__file__)))
	tmp = tempfile.mkdtemp(dir=root)
	try:
		def collisions(sources):
			t = dict(renames={}, sites=collections.defaultdict(list), seen=set(),
					 stats=collections.Counter(), scope=collections.defaultdict(dict),
					 targets=collections.defaultdict(set))
			for name, body in sources:
				src = os.path.join(tmp, name)
				with open(src, "wb") as f:
					f.write(body.encode("utf-8"))
				tu = cindex.Index.create().parse(src, args=["-std=c++20"])
				if [d for d in tu.diagnostics if d.severity >= 3]:
					return None
				paths, records = codemod.collect(tu, root, naming.load(), codemod.load_config(),
												 codemod.lint.norm(tmp), (codemod.lint.norm(tmp),), set())
				codemod.merge(paths, records, t["renames"], t["sites"], t["seen"],
							  t["stats"], t["scope"], t["targets"])
			return [(want, why) for _, want, why in
					codemod.find_collisions(t["scope"], t["targets"], t["renames"])]

		try:
			apart = collisions([("AnonA.cpp", ANON_UNRELATED_A), ("AnonB.cpp", ANON_UNRELATED_B)])
		except Exception:
			print("SKIPPED: the anonymous-scope check needs a loadable libclang")
			return 0
		if apart is None:
			print("SKIPPED: the anonymous-scope probe didn't compile")
			return 0
		check("ANON two files don't collide", apart, [])
		check("ANON occupied spelling still collides",
			  collisions([("AnonC.cpp", ANON_OCCUPIED)]),
			  [("_state", "occupied by an existing declaration")])
		check("ANON converging pair still collides",
			  collisions([("AnonD.cpp", ANON_CONVERGE)]),
			  [("_count", "2 declarations converge on it")])
	finally:
		shutil.rmtree(tmp, ignore_errors=True)
	return 3


LOCAL_CONSTANTS_SNIPPET = """void Probe() {
	constexpr unsigned W = 4, H = 4;
	const unsigned A = 1, B = 2;
	static constexpr unsigned S = 1, T = 2;
	unsigned plain = 1, other = 2;
	auto table = [] { constexpr int L = 1; return L; };
	(void)table;
}
"""


def check_local_constants(check):
	cindex = codemod.cindex
	libclang = codemod.lint.LIBCLANG
	if libclang and os.path.exists(libclang):
		with contextlib.suppress(Exception):
			cindex.Config.set_library_file(libclang)
	root = os.path.dirname(os.path.dirname(os.path.abspath(codemod.__file__)))
	tmp = tempfile.mkdtemp(dir=root)
	try:
		src = os.path.join(tmp, "LocalConstantsProbe.cpp")
		with open(src, "wb") as f:
			f.write(LOCAL_CONSTANTS_SNIPPET.encode("utf-8"))
		try:
			tu = cindex.Index.create().parse(src, args=["-std=c++20"])
		except Exception:
			print("SKIPPED: the local constant check needs a loadable libclang")
			return 0
		if [d for d in tu.diagnostics if d.severity >= 3]:
			print("SKIPPED: the local constant probe didn't compile")
			return 0
		got = {c.spelling: codemod.lint.row_for(c) for c in tu.cursor.walk_preorder()
			   if c.kind == codemod.K.VAR_DECL}
		check("LOCALS constexpr on later declarators in a group", got,
			  {"W": "constants", "H": "constants", "A": "locals", "B": "locals",
			   "S": "constants", "T": "constants", "plain": "locals", "other": "locals",
			   "table": "locals", "L": "constants"})
	finally:
		shutil.rmtree(tmp, ignore_errors=True)
	return 1


def check_lint_units(check):
	lint = codemod.lint
	root = os.path.dirname(os.path.dirname(os.path.abspath(lint.__file__)))
	tmp = tempfile.mkdtemp(dir=root)
	try:
		header = os.path.join(tmp, "cmake_pch.hxx")
		binary = header + ".pch"
		unit = os.path.join(tmp, "Unit.cpp")
		for path in (header, binary):
			with open(path, "wb") as f:
				f.write(b"")
		entry = {"file": unit,
				 "arguments": ["clang++", "-std=c++20", "-Xclang", "-include-pch", "-Xclang", binary,
							   "-Xclang", "-include", "-Xclang", header, "-c", unit]}
		check("LINT drops a built precompiled header", lint.compile_args(entry),
			  ["-std=c++20", "-Xclang", "-include", "-Xclang", header])

		missing = os.path.join(root, "Source", "NamingLintMissingUnit.cpp")
		with open(os.path.join(tmp, "compile_commands.json"), "wb") as f:
			f.write(json.dumps([{"file": missing, "arguments": ["clang++", "-c", missing]}]).encode("utf-8"))
		ledger = os.path.join(tmp, "ledger.txt")
		argv = sys.argv
		sys.argv = ["naming_lint.py", "--build", tmp, "--ledger", ledger]
		try:
			with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
				code = lint.main()
		except Exception:
			print("SKIPPED: the lint run check needs a loadable libclang")
			return 1
		finally:
			sys.argv = argv
		check("LINT fails on a unit that doesn't parse", code, 1)
		check("LINT writes no ledger after a parse failure", os.path.exists(ledger), False)
	finally:
		shutil.rmtree(tmp, ignore_errors=True)
	return 3


def check_ledger_freshness(check):
	here = os.path.dirname(os.path.abspath(__file__))
	ledger = os.path.join(here, "naming-ledger.txt")
	dictionary = os.path.join(here, "naming-dictionary.json")
	if not os.path.exists(ledger) or not os.path.exists(dictionary):
		print("SKIPPED: no committed ledger to check against the dictionary")
		return 0
	with open(dictionary, "rb") as f:
		want = hashlib.sha256(f.read()).hexdigest()
	recorded = None
	with open(ledger, "rb") as f:
		for line in f.read().decode("utf-8").splitlines():
			if line.startswith("dictionary_sha256 = "):
				recorded = line.split(" = ", 1)[1].strip()
				break
	check("ledger records a dictionary hash", recorded is not None, True)
	check("ledger regenerated after the dictionary changed", recorded, want)
	return 2


CASED_SYSTEM_HEADERS = frozenset(["TargetConditionals.h"])
CASED_SYSTEM_DIRECTORIES = ("X11/",)


def check_system_include_casing(check):
	here = os.path.dirname(os.path.abspath(__file__))
	source = os.path.join(os.path.dirname(here), "Source")
	if not os.path.isdir(source):
		print("SKIPPED: no Source tree to scan for include casing")
		return 0
	angled = re.compile(r"#\s*include\s*<([^>]+)>")
	offenders = []
	for folder, _, names in os.walk(source):
		if "ThirdParty" in os.path.relpath(folder, source).split(os.sep):
			continue
		for name in names:
			if not name.endswith((".h", ".hpp", ".inl", ".c", ".cpp")):
				continue
			path = os.path.join(folder, name)
			with open(path, "rb") as f:
				text = f.read().decode("utf-8", "replace")
			for header in angled.findall(text):
				if header in CASED_SYSTEM_HEADERS or header.startswith(CASED_SYSTEM_DIRECTORIES):
					continue
				if any(c.isupper() for c in header):
					offenders.append("%s: <%s>" % (os.path.relpath(path, source), header))
	check("angle-bracket system includes match their shipped casing", sorted(offenders), [])
	return 1


def main():
	failures = []

	def check(label, got, want):
		if got != want:
			failures.append("%s: got %r, want %r" % (label, got, want))

	for row in EXAMPLES:
		src, pas, cam, con, keb = row
		for style, want in (("Pascal", pas), ("camel", cam), ("CONST", con), ("kebab", keb)):
			try:
				check("EXAMPLES %s -> %s" % (src, style), naming.recase(src, style, DRAFT), want)
			except naming.NamingError as e:
				failures.append("EXAMPLES %s -> %s raised %s" % (src, style, e))

	for src, want in MIGRATION:
		try:
			check("MIGRATION %s" % src, naming.recase(src, "camel", DRAFT), want)
		except naming.NamingError as e:
			failures.append("MIGRATION %s raised %s" % (src, e))

	for src, want in REJOIN:
		try:
			check("REJOIN %s" % src, naming.recase(src, "camel", DRAFT, migration=True), want)
		except naming.NamingError as e:
			failures.append("REJOIN %s raised %s" % (src, e))

	repo_dic = naming.load()
	for src, style, want in REPO:
		try:
			check("REPO %s -> %s" % (src, style), naming.recase(src, style, repo_dic), want)
		except naming.NamingError as e:
			failures.append("REPO %s -> %s raised %s" % (src, style, e))

	for src, want in (("rHS", "rhs"), ("lHS", "lhs")):
		got = naming.recase(src, "camel", repo_dic, migration=True)
		check("REPO rejoin %s" % src, got, want)

	for src, style in ERRORS:
		try:
			got = naming.recase(src, style, DRAFT)
			failures.append("ERRORS %s -> %s should have raised, got %r" % (src, style, got))
		except naming.NamingError:
			pass

	_, toks = naming.tokenize("RAD2DEG", repo_dic)
	if "2D" in toks:
		failures.append("RAD2DEG: pre-pass matched 2D inside an all-caps run -> %r" % toks)

	root = os.path.abspath("/repo/Shalltear")
	for path, want in CONTAINS:
		check("CONTAINS %s" % path, naming.contains(root, os.path.abspath(path)), want)

	total = len(EXAMPLES) * 4 + len(MIGRATION) + len(REJOIN) + len(REPO) + len(ERRORS) \
		+ len(CONTAINS) + 3
	total += check_ledger_freshness(check)
	total += check_system_include_casing(check)
	if codemod is not None:
		total += check_lint_units(check)
		total += check_merge(check)
		total += check_traversal(check)
		total += check_anon_scope(check)
		total += check_local_constants(check)
	else:
		print("SKIPPED: the codemod merge checks need libclang importable")
	if failures:
		for f in failures:
			print("FAIL  " + f)
		print("\n%d of %d checks failed" % (len(failures), total))
		return 1
	print("OK - %d checks passed" % total)
	return 0


if __name__ == "__main__":
	sys.exit(main())
