#!/usr/bin/env python3
"""Verify a commit before pushing (all declared tests once, in 2 trees)
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import rebuild_all
import toolchain

REPO = Path(__file__).resolve().parent.parent

FILTER_LIMIT = 8000

DEFAULT_TIMEOUT = 2400

CONFIGURE_HINT = ("cmake -B {name} -G Ninja -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++ "
				  "-DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang -DCMAKE_BUILD_TYPE={config} "
				  "-DSHALLTEAR_BUILD_SHARED={shared}")
LINUX_PRESETS = {("Release", "OFF"): "linux-static-release", ("Debug", "ON"): "linux-debug"}

TEST_BINARY = "ShalltearTests" + toolchain.EXE


def configure_hint(name, config, shared):
	if toolchain.HOST_IS_WINDOWS:
		return CONFIGURE_HINT.format(name=name, config=config, shared=shared)
	return f"cmake --preset {LINUX_PRESETS[(config, shared)]}"


def test_trees():
	return [cache.parent for cache in sorted(REPO.glob("build*/CMakeCache.txt"))
			if (cache.parent / "bin" / TEST_BINARY).is_file()]


def flavor(tree):
	shared = (rebuild_all.cache_value(tree, "SHALLTEAR_BUILD_SHARED") or "").upper() in ("ON", "TRUE", "1")
	release = (rebuild_all.cache_value(tree, "CMAKE_BUILD_TYPE") or "").lower() == "release"
	return shared, release


def pick(trees, shared, release):
	for tree in trees:
		if flavor(tree) == (shared, release):
			return tree
	return None


def csharp_gate(trees, allowed, say):
	without = [tree.name for tree in trees
			   if (rebuild_all.cache_value(tree, "SHALLTEAR_CSHARP_ENABLED") or "").upper() != "ON"]
	if without and not allowed:
		say(f"{', '.join(without)} built without C#. Run python Scripts/fetch_prebuilt.py --dotnet (or pass --without-csharp)")
		return False
	if without:
		say(f"  --without-csharp: no C# cases from {', '.join(without)}")
	return True


def parse_listing(text):
	names = []
	suite = None
	for line in text.splitlines():
		line = line.rstrip("\r")
		if not line.strip() or line.startswith("Running main"):
			continue
		if not line.startswith(" "):
			suite = line.split()[0]
		elif suite is not None:
			names.append(suite + line.split()[0])
	return names


def suite_of(name):
	return name.rsplit(".", 1)[0]


def filter_patterns(only, declared):
	only = set(only)
	by_suite = {}
	for name in declared:
		by_suite.setdefault(suite_of(name), []).append(name)
	patterns = []
	for suite, names in by_suite.items():
		chosen = [name for name in names if name in only]
		if not chosen:
			continue
		patterns.extend([f"{suite}.*"] if len(chosen) == len(names) else chosen)
	return patterns


def chunk(patterns, limit=FILTER_LIMIT):
	filters = []
	current = []
	length = 0
	for pattern in patterns:
		if current and length + 1 + len(pattern) > limit:
			filters.append(":".join(current))
			current, length = [], 0
		current.append(pattern)
		length += len(pattern) + (1 if length else 0)
	if current:
		filters.append(":".join(current))
	return filters


def list_tests(tree):
	out = subprocess.run([str(tree / "bin" / TEST_BINARY), "--gtest_list_tests"],
						 cwd=tree / "bin", capture_output=True, text=True, errors="replace")
	return parse_listing(out.stdout) if out.returncode == 0 else None


RESULT_LINE = re.compile(r"^\[  (FAILED|SKIPPED) +\] ([A-Za-z_][^ ,]*)")
STARTED_LINE = re.compile(r"^\[ RUN      \] (\S+)")
RAN_LINE = re.compile(r"^\[==========\] (\d+) tests? from \d+ test suites? ran")


def read_results(log):
	result = {"ran": None, "failed": set(), "skipped": set(), "last": None}
	for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
		match = RESULT_LINE.match(line)
		if match:
			result["failed" if match.group(1) == "FAILED" else "skipped"].add(match.group(2))
			continue
		match = STARTED_LINE.match(line)
		if match:
			result["last"] = match.group(1)
			continue
		match = RAN_LINE.match(line)
		if match:
			result["ran"] = int(match.group(1))
	return result


def run_binary(tree, arguments, log, timeout):
	with open(log, "wb") as out:
		process = subprocess.Popen([str(tree / "bin" / TEST_BINARY), *arguments],
								   cwd=tree / "bin", stdout=out, stderr=subprocess.STDOUT,
								   start_new_session=not toolchain.HOST_IS_WINDOWS)
		try:
			return process.wait(timeout=timeout)
		except subprocess.TimeoutExpired:
			toolchain.kill_tree(process)
			process.wait()
			return None


def write_list(path, names):
	path.write_text("".join(f"{n}\n" for n in sorted(names)), encoding="utf-8", newline="\n")


class Summary:

	def __init__(self, path):
		self.path = path
		path.write_text("", encoding="utf-8")

	def __call__(self, text=""):
		print(text, flush=True)
		with open(self.path, "a", encoding="utf-8", newline="\n") as out:
			out.write(text + "\n")


def report(say, label, runs, out, tree_name):
	failed, skipped, ran, clean = set(), set(), 0, True
	for code, seconds, log in runs:
		result = read_results(log)
		failed |= result["failed"]
		skipped |= result["skipped"]
		if code is None:
			say(f"  KILLED after {seconds:.0f} s, inside {result['last']}")
			clean = False
		elif result["ran"] is None:
			say(f"  CRASHED (exit {code}) inside {result['last']}")
			clean = False
		else:
			ran += result["ran"]
			clean = clean and (code == 0 or bool(result["failed"]))
	write_list(out / f"{tree_name}.skipped", skipped)
	write_list(out / f"{tree_name}.failed", failed)
	seconds = sum(run[1] for run in runs)
	say(f"  {label}: ran {ran}, passed {ran - len(skipped) - len(failed)}, "
		f"skipped {len(skipped)}, failed {len(failed)} in {seconds:.0f} s")
	for name in sorted(failed):
		say(f"    FAILED {name}")
	return clean and not failed


def build(tree, head):
	if not rebuild_all.run(["cmake", str(tree)], f"configure {tree.name}"):
		return False
	if not rebuild_all.run(["cmake", "--build", str(tree), "--parallel"], f"build {tree.name}"):
		return False
	stamp = rebuild_all.stamped_commit(tree)
	if stamp not in (None, head):
		print(f"{tree.name} built but stamped {stamp}, not {head}. Delete and reconfigure it")
		return False
	return True


def verify(args, out, say):
	toolchain.prepare("the suite")
	head = rebuild_all.head_commit()
	say(f"verify at {head or 'an unknown commit'}, logs in {out}")
	for name in ("VK_DRIVER_FILES", "VK_ICD_FILENAMES"):
		if os.environ.get(name):
			say(f"  {name} is set: the device cases run on {os.environ[name]}")

	trees = test_trees()
	primary = pick(trees, shared=False, release=True)
	secondary = pick(trees, shared=True, release=False)
	missing = [(name, config, shared) for tree, name, config, shared in
			   ((primary, "build-static-release", "Release", "OFF"), (secondary, "build", "Debug", "ON"))
			   if tree is None]
	for name, config, shared in missing:
		say(f"no {'shared' if shared == 'ON' else 'static'} {config} tree with a test binary. Configure one:")
		say("  " + configure_hint(name, config, shared))
	if missing:
		return 1

	if args.no_build:
		toolchain.kill_by_name(("ShalltearTests",))
		for tree in (primary, secondary):
			stamp = rebuild_all.stamped_commit(tree)
			if stamp not in (None, head):
				say(f"{tree.name} is stamped {stamp}, not {head}. Build it, or drop --no-build")
				return 1
	else:
		rebuild_all.kill_holders()
		for tree in (primary, secondary):
			if not build(tree, head):
				say(f"FAILED to build {tree.name}")
				return 1

	if not csharp_gate((primary, secondary), args.without_csharp, say):
		return 1

	lists = {}
	for tree in (primary, secondary):
		names = list_tests(tree)
		if names is None:
			say(f"FAILED: {tree.name}'s test binary won't list its tests")
			return 1
		lists[tree] = names
		write_list(out / f"{tree.name}.names", names)

	only = sorted(set(lists[secondary]) - set(lists[primary]))
	say(f"{primary.name}: {len(lists[primary])} declared, the full suite")
	say(f"{secondary.name}: {len(lists[secondary])} declared, {len(only)} of them only there")
	suites = {}
	for name in only:
		suites[suite_of(name)] = suites.get(suite_of(name), 0) + 1
	for suite, count in sorted(suites.items(), key=lambda item: (-item[1], item[0])):
		say(f"    {count:4} {suite}")

	started = time.monotonic()
	code = run_binary(primary, [], out / f"{primary.name}.log", args.timeout)
	clean = report(say, primary.name, [(code, time.monotonic() - started, out / f"{primary.name}.log")],
				   out, primary.name)

	runs = []
	for index, gtest_filter in enumerate(chunk(filter_patterns(only, lists[secondary]))):
		log = out / f"{secondary.name}.{index}.log"
		started = time.monotonic()
		code = run_binary(secondary, [f"--gtest_filter={gtest_filter}"], log, args.timeout)
		runs.append((code, time.monotonic() - started, log))
	if runs:
		clean = report(say, f"{secondary.name}, only there", runs, out, secondary.name) and clean
	else:
		say(f"  {secondary.name}: nothing declared only there")
	return 0 if clean else 1


def main():
	parser = argparse.ArgumentParser(description=__doc__,
									 formatter_class=argparse.RawDescriptionHelpFormatter)
	parser.add_argument("--no-build", action="store_true",
						help="run the binaries as they are (only when both were built at HEAD)")
	parser.add_argument("--without-csharp", action="store_true",
						help="accept trees built without the C# package")
	parser.add_argument("--out", type=Path, default=Path(tempfile.gettempdir()) / "shalltear-verify",
						help="where the logs, lists and summary.txt go")
	parser.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT,
						help=f"seconds before a hung binary is killed (default {DEFAULT_TIMEOUT})")
	args = parser.parse_args()

	out = args.out.resolve()
	out.mkdir(parents=True, exist_ok=True)
	say = Summary(out / "summary.txt")
	try:
		code = verify(args, out, say)
	except BaseException as error:
		say(f"ERROR {error!r}")
		code = 1
	say(f"DONE {code}")
	return code


if __name__ == "__main__":
	sys.exit(main())
