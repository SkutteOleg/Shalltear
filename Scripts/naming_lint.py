import argparse
import collections
import fnmatch
import hashlib
import json
import os
import sys
import time

import naming

try:
	from clang import cindex
except ImportError:
	sys.exit("clang.cindex not importable. Run pip install libclang or point PYTHONPATH at it")

LIBCLANG = r"C:\Program Files\LLVM\bin\libclang.dll"

K = cindex.CursorKind

KIND_ROW = {
	K.CLASS_DECL: "types",
	K.STRUCT_DECL: "types",
	K.UNION_DECL: "types",
	K.ENUM_DECL: "types",
	K.TYPEDEF_DECL: "types",
	K.TYPE_ALIAS_DECL: "types",
	K.CLASS_TEMPLATE: "types",
	K.CLASS_TEMPLATE_PARTIAL_SPECIALIZATION: "types",
	K.TYPE_ALIAS_TEMPLATE_DECL: "types",
	K.TEMPLATE_TYPE_PARAMETER: "templateParams",
	K.TEMPLATE_NON_TYPE_PARAMETER: "templateParams",
	K.ENUM_CONSTANT_DECL: "enumMembers",
	K.FUNCTION_DECL: "functions",
	K.CXX_METHOD: "functions",
	K.FUNCTION_TEMPLATE: "functions",
	K.CONVERSION_FUNCTION: None,
	K.CONSTRUCTOR: None,
	K.DESTRUCTOR: None,
	K.PARM_DECL: "parameters",
	K.FIELD_DECL: "members",
	K.NAMESPACE: "namespaces",
}

SKIP_SPELLINGS = ("operator", "~")

FUNCTION_KINDS = (K.FUNCTION_DECL, K.CXX_METHOD, K.FUNCTION_TEMPLATE, K.CONSTRUCTOR, K.DESTRUCTOR)

DISPOSITIONS = (
	"finding",
	"conforming",
	"unresolved",
	"nonIdentifier",
	"noBindingRow",
	"styleNotBound",
	"vendor",
	"generated",
	"reserved",
	"kept",
	"foreignNamespace",
	"foreignOverride",
)


_VENDOR_CACHE = {}
_UNDER_CACHE = {}


norm = naming.norm


def is_vendor(name, patterns):
	key = (name, id(patterns))
	hit = _VENDOR_CACHE.get(key)
	if hit is None:
		hit = any(fnmatch.fnmatchcase(name, p) for p in patterns)
		_VENDOR_CACHE[key] = hit
	return hit


def under(path, globs, root):
	key = (path, id(globs))
	hit = _UNDER_CACHE.get(key)
	if hit is None:
		rel = os.path.relpath(path, root).replace(os.sep, "/")
		hit = any(fnmatch.fnmatchcase(rel, g) for g in globs)
		_UNDER_CACHE[key] = hit
	return hit


def compile_args(entry):
	cmd = entry.get("arguments") or entry["command"].split()
	src = os.path.abspath(entry["file"])
	out, i = [], 1
	while i < len(cmd):
		a = cmd[i]
		if a == "-o":
			i += 2
			continue
		if a == "-c":
			i += 1
			continue
		if a.endswith((".cpp", ".cc", ".cxx")) and os.path.abspath(a) == src:
			i += 1
			continue
		if a == "-Xclang" and i + 3 < len(cmd) and cmd[i + 1] in ("-include-pch", "-include") \
				and cmd[i + 2] == "-Xclang":
			path = cmd[i + 3]
			if cmd[i + 1] == "-include-pch" or not os.path.exists(path):
				i += 4
				continue
			out.extend(cmd[i:i + 4])
			i += 4
			continue
		out.append(a)
		i += 1
	return out


def in_foreign_namespace(cursor):
	node = cursor.semantic_parent
	while node is not None and node.kind != K.TRANSLATION_UNIT:
		if node.kind == K.NAMESPACE and (node.spelling == "std" or node.spelling.startswith("__")):
			return True
		node = node.semantic_parent
	return False


def overrides_foreign(cursor, source_root):
	owner = cursor.semantic_parent
	if owner is None or owner.kind not in (K.CLASS_DECL, K.STRUCT_DECL, K.CLASS_TEMPLATE):
		return False
	name = cursor.spelling
	for child in owner.get_children():
		if child.kind != K.CXX_BASE_SPECIFIER:
			continue
		base = child.type.get_declaration() if child.type is not None else None
		if base is None:
			continue
		loc = base.location
		if loc is not None and loc.file is not None \
				and norm(loc.file.name).startswith(source_root):
			continue
		for member in base.get_children():
			if member.kind == K.CXX_METHOD and member.spelling == name:
				return True
	return False


def _is_constant(cursor):
	t = cursor.type
	if t is not None and t.is_const_qualified():
		return True
	return _has_constexpr(cursor)


def _leading_tokens(cursor):
	out = []
	try:
		for n, tok in enumerate(cursor.get_tokens()):
			out.append(tok.spelling)
			if n > 6:
				break
	except Exception:
		pass
	return out


def _declaration_statement(cursor):
	parent = cursor.semantic_parent
	start = cursor.extent.start
	if parent is None or parent.kind not in FUNCTION_KINDS or start.file is None:
		return None
	stack = [parent]
	while stack:
		for child in stack.pop().get_children():
			at = child.extent
			if at.start.file is None or at.start.file.name != start.file.name \
					or not at.start.offset <= start.offset <= at.end.offset:
				continue
			if child.kind == K.DECL_STMT and any(c == cursor for c in child.get_children()):
				return child
			stack.append(child)
	return None


def _has_constexpr(cursor):
	head, tokens = cursor, _leading_tokens(cursor)
	if tokens[:1] == [cursor.spelling]:
		statement = _declaration_statement(cursor)
		if statement is not None:
			head = next(statement.get_children(), cursor)
			tokens = _leading_tokens(head)
	if head.spelling in tokens:
		tokens = tokens[:tokens.index(head.spelling)]
	return "constexpr" in tokens


def _names_a_value(cursor):
	t = cursor.type
	if t is None:
		return False
	t = t.get_canonical()
	while t.kind in (cindex.TypeKind.CONSTANTARRAY, cindex.TypeKind.INCOMPLETEARRAY):
		t = t.get_array_element_type().get_canonical()
	if t.kind == cindex.TypeKind.RECORD:
		return True
	parent = cursor.semantic_parent
	return (t.kind == cindex.TypeKind.BOOL
			and parent is not None
			and parent.kind in (K.CLASS_DECL, K.STRUCT_DECL, K.CLASS_TEMPLATE))


def row_for(cursor):
	kind = cursor.kind
	if kind == K.VAR_DECL:
		parent = cursor.semantic_parent
		in_function = parent is not None and parent.kind in FUNCTION_KINDS
		constant = _has_constexpr(cursor) if in_function else _is_constant(cursor)
		if not constant:
			return "locals" if in_function else "statics"
		return "namedValues" if _names_a_value(cursor) else "constants"
	return KIND_ROW.get(kind, None)


def walk(tu, root, dic, cfg, seen_files, findings, unresolved, stats, unmapped, migration=True):
	source_root = norm(os.path.join(root, "Source"))
	stack = list(tu.cursor.get_children())
	visited = set()
	while stack:
		cursor = stack.pop()
		h = cursor.hash
		if h in visited:
			continue
		visited.add(h)
		loc = cursor.location
		if loc is None or loc.file is None:
			stack.extend(cursor.get_children())
			continue
		path = os.path.abspath(loc.file.name)
		if not norm(path).startswith(source_root):
			continue
		stack.extend(cursor.get_children())
		if under(path, cfg["vendorRegions"], root) or under(path, cfg["frozenPaths"], root):
			continue
		key = (path, loc.line, loc.column, cursor.kind)
		if key in seen_files:
			continue
		seen_files.add(key)

		name = cursor.spelling
		if not name or name.startswith(SKIP_SPELLINGS):
			continue

		stats["seen"] += 1
		if not naming.IDENTIFIER.match(name):
			stats["nonIdentifier"] += 1
			continue
		row = row_for(cursor)
		if row is None:
			stats["noBindingRow"] += 1
			unmapped[cursor.kind.name] += 1
			continue
		if is_vendor(name, cfg["vendorNames"]):
			stats["vendor"] += 1
			continue
		if is_vendor(name, cfg["generatedNames"]):
			stats["generated"] += 1
			continue
		if is_vendor(name, cfg["reservedNames"]):
			stats["reserved"] += 1
			continue
		if is_vendor(name, cfg["keptNames"]):
			stats["kept"] += 1
			continue
		if in_foreign_namespace(cursor):
			stats["foreignNamespace"] += 1
			continue
		if cursor.kind in (K.CXX_METHOD, K.FUNCTION_TEMPLATE) and overrides_foreign(cursor, source_root):
			stats["foreignOverride"] += 1
			continue
		style = cfg["binding"]["cpp"].get(row)
		if style in (None, "matchPrimaryType"):
			stats["styleNotBound"] += 1
			continue
		stats["checked"] += 1
		rel = os.path.relpath(path, root).replace(os.sep, "/")
		try:
			want = naming.recase(name, style, dic, migration=migration)
		except naming.NamingError as e:
			stats["unresolved"] += 1
			unresolved[str(e)].append((rel, loc.line, name))
			continue
		if want != name:
			stats["finding"] += 1
			findings.append({"file": rel, "line": loc.line, "row": row,
							 "style": style, "old": name, "new": want})
		else:
			stats["conforming"] += 1


def write_ledger(path, findings, unresolved, stats, unmapped, tu_count, failed, dictionary_sha):
	lines = []
	lines.append("# naming change ledger. Regenerate with Scripts/naming_lint.py --ledger")
	lines.append("# 1 line per declaration the codemod would rewrite")
	lines.append("")
	lines.append("[run]")
	lines.append("translation_units = %d" % tu_count)
	lines.append("parse_failures = %d" % len(failed))
	lines.append("named_declarations_seen = %d" % stats["seen"])
	lines.append("dictionary_sha256 = %s" % dictionary_sha)
	lines.append("")
	lines.append("[dispositions]")
	for label in DISPOSITIONS:
		lines.append("%s = %d" % (label, stats[label]))
	lines.append("TOTAL = %d" % sum(stats[label] for label in DISPOSITIONS))
	lines.append("balances = %s" % (sum(stats[label] for label in DISPOSITIONS) == stats["seen"]))
	lines.append("")
	lines.append("[unmapped_cursor_kinds]")
	lines.append("# reached no binding row. A kind here that OUGHT to be renamed is")
	lines.append("# an unhandled case (not a skip). Check this list before each phase")
	for kind, n in sorted(unmapped.items(), key=lambda kv: (-kv[1], kv[0])):
		lines.append("%s = %d" % (kind, n))
	lines.append("")
	lines.append("[unresolved]")
	for msg in sorted(unresolved):
		lines.append("%s = %d" % (msg, len(unresolved[msg])))
	lines.append("")
	lines.append("[renames]")
	for f in sorted(findings, key=lambda x: (x["file"], x["line"], x["old"], x["new"])):
		lines.append("%s:%d\t%s\t%s\t%s -> %s"
					 % (f["file"], f["line"], f["row"], f["style"], f["old"], f["new"]))
	with open(path, "wb") as fh:
		fh.write(("\n".join(lines) + "\n").encode("utf-8"))


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--build", default="build")
	ap.add_argument("--limit", type=int, default=0)
	ap.add_argument("--json", default="")
	ap.add_argument("--libclang", default=LIBCLANG)
	ap.add_argument("--verbose", action="store_true")
	ap.add_argument("--no-migration", action="store_true",
					help="disable the one-lowercase-letter re-join clause (rHS -> rhs)")
	ap.add_argument("--ledger", default="",
					help="write a deterministic, diffable change ledger to this path")
	args = ap.parse_args()

	root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
	if os.path.exists(args.libclang):
		cindex.Config.set_library_file(args.libclang)

	dic = naming.load()
	with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "naming-dictionary.json"), "rb") as f:
		dictionary_bytes = f.read()
	dictionary_sha = hashlib.sha256(dictionary_bytes).hexdigest()
	cfg = json.loads(dictionary_bytes.decode("utf-8"))
	cfg.setdefault("vendorNames", [])
	cfg.setdefault("vendorRegions", [])
	cfg.setdefault("frozenPaths", [])
	cfg.setdefault("generatedNames", [])
	cfg.setdefault("reservedNames", [])
	cfg.setdefault("keptNames", [])

	db_path = os.path.join(root, args.build, "compile_commands.json")
	with open(db_path, "rb") as f:
		entries = json.loads(f.read().decode("utf-8"))

	source_root = norm(os.path.join(root, "Source"))
	tus = [e for e in entries
		   if norm(e["file"]).startswith(source_root) and e["file"].endswith(".cpp")]
	tus.sort(key=lambda e: e["file"])
	if not tus:
		sys.exit("no Source/ translation units in %s (%d entries). Wrong --build tree, "
				 "or one configured before Source/ existed" % (db_path, len(entries)))
	if args.limit:
		tus = tus[:args.limit]

	sys.stderr.write("  %d TUs to parse (build=%s)\n" % (len(tus), args.build))
	sys.stderr.flush()
	index = cindex.Index.create()
	findings, unresolved, seen = [], collections.defaultdict(list), set()
	stats = collections.Counter()
	unmapped = collections.Counter()
	failed = []

	for i, e in enumerate(tus, 1):
		cargs = compile_args(e)
		t_parse = time.time()
		if args.verbose:
			sys.stderr.write("  [%d/%d] parsing %s (%d arg(s))\n"
							 % (i, len(tus), os.path.basename(e["file"]), len(cargs)))
			sys.stderr.flush()
		try:
			tu = index.parse(e["file"], args=cargs,
							 options=cindex.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES
							 | cindex.TranslationUnit.PARSE_INCOMPLETE)
		except cindex.TranslationUnitLoadError as err:
			failed.append((e["file"], str(err)))
			if args.verbose:
				sys.stderr.write("  [%d/%d] PARSE FAILED %.2fs  %s\n"
								 % (i, len(tus), time.time() - t_parse, os.path.basename(e["file"])))
				sys.stderr.flush()
			continue
		walk(tu, root, dic, cfg, seen, findings, unresolved, stats, unmapped, not args.no_migration)
		if i % 25 == 0 or i == len(tus):
			sys.stderr.write("  parsed %d/%d TUs, %d declaration(s) checked, %d finding(s)\n"
							 % (i, len(tus), stats["checked"], len(findings)))
			sys.stderr.flush()

	print("\n==== SUMMARY ====")
	print("  translation units parsed : %d (%d failed)" % (len(tus) - len(failed), len(failed)))
	print("  named cursors seen       : %d  (declarations and their references)" % stats["seen"])
	print("\n==== DISPOSITIONS (each named declaration goes in 1) ====")
	for label in DISPOSITIONS:
		print("  %-26s %7d" % (label, stats[label]))
	total = sum(stats[label] for label in DISPOSITIONS)
	balanced = total == stats["seen"]
	print("  %-26s %7d  %s" % ("TOTAL", total,
							   "balances" if balanced else
							   "!! LEAKS %d (a disposition is unaccounted for)" % (stats["seen"] - total)))
	print("\n==== UNMAPPED CURSOR KINDS (reached no binding row) ====")
	for kind, n in sorted(unmapped.items(), key=lambda kv: (-kv[1], kv[0]))[:12]:
		print("  %-34s %7d" % (kind, n))

	print("\n  casing findings          : %d" % len(findings))
	print("  unresolved token classes : %d" % len(unresolved))

	if unresolved:
		print("\n==== DICTIONARY GAPS (register or rename) ====")
		for msg, hits in sorted(unresolved.items(), key=lambda kv: -len(kv[1])):
			ex = hits[0]
			print("  %5d  %s\n         e.g. %s:%d  %s" % (len(hits), msg, ex[0], ex[1], ex[2]))

	if findings:
		by_change = collections.Counter("%s -> %s" % (f["old"], f["new"]) for f in findings)
		print("\n==== TOP RENAMES (distinct declarations) ====")
		for change, n in by_change.most_common(40):
			print("  %5d  %s" % (n, change))
		by_row = collections.Counter(f["row"] for f in findings)
		print("\n==== BY BINDING ROW ====")
		for r, n in by_row.most_common():
			print("  %5d  %s" % (n, r))

	if failed:
		print("\n==== TUs THAT FAILED TO PARSE ====")
		for f, err in failed[:10]:
			print("  %s: %s" % (f, err.splitlines()[0] if err else "?"))
		print("\n%d translation unit(s) failed to parse, nothing written" % len(failed))
		return 1

	if args.ledger:
		write_ledger(args.ledger, findings, unresolved, stats, unmapped, len(tus) - len(failed), failed, dictionary_sha)
		print("\nwrote ledger %s" % args.ledger)

	if args.json:
		with open(args.json, "wb") as f:
			f.write(json.dumps({"findings": findings,
								"unresolved": {k: v for k, v in unresolved.items()}},
							   indent=1).encode("utf-8"))
		print("\nwrote %s" % args.json)
	return 0


if __name__ == "__main__":
	sys.exit(main())
