#!/usr/bin/env python3
"""Run 1 tree's whole test suite in 1 process (or 1 per test with --each), resuming past crashes and hangs
"""

import argparse
import fnmatch
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import toolchain
import verify

DEFAULT_TIMEOUT = 600
POLL_SECONDS = 0.5
ANNOTATION_LIMIT = 20
SUMMARY_FAILURES = 50
SLOWEST = 10

RUN_LINE = re.compile(r"^\[ RUN      \] (\S+)")
END_LINE = re.compile(r"^\[ +(OK|FAILED|SKIPPED) +\] ([^ ,]+)")
SUITE_LINE = re.compile(r"^\[-{10}\] \d+ tests? from ([^ ,]+)")
MILLISECONDS = re.compile(r"\((\d+) ms\)$")
LOCATION = re.compile(r"^(.+?)(?::(\d+):|\((\d+)\):) ")


def names_in(text):
	return [name for name in re.split(r"[:\s]+", text or "") if name]


def matches(name, patterns):
	return any(fnmatch.fnmatchcase(name, pattern) for pattern in patterns)


def describe(code):
	if code is None:
		return "no exit code"
	if code < 0:
		return f"signal {-code}"
	if code > 255:
		return f"exit 0x{code:08X}"
	return f"exit {code}"


class Result:

	def __init__(self, verdict, ms=0, lines=(), note=""):
		self.verdict = verdict
		self.ms = ms
		self.lines = list(lines)
		self.note = note


class Reader:

	def __init__(self):
		self.results = {}
		self.current = None
		self.suite = None
		self.lines = []
		self.test_started = None
		self.finished = False

	def feed(self, line, now):
		match = RUN_LINE.match(line)
		if match:
			self.current, self.lines, self.test_started = match.group(1), [], now
			return
		match = END_LINE.match(line)
		if match and match.group(2) == self.current:
			ms = MILLISECONDS.search(line)
			self.results[self.current] = Result(match.group(1), int(ms.group(1)) if ms else 0, self.lines)
			self.current = None
			return
		if verify.RAN_LINE.match(line):
			self.finished = True
			return
		match = SUITE_LINE.match(line)
		if match and self.current is None and not line.endswith("total)"):
			self.suite = match.group(1)
			return
		if self.current is not None:
			self.lines.append(line)


def run_process(command, cwd, part, log, timeout, echo, poll=POLL_SECONDS):
	reader = Reader()
	with open(part, "wb") as out:
		process = subprocess.Popen(command, cwd=cwd, stdout=out, stderr=subprocess.STDOUT,
								   start_new_session=not toolchain.HOST_IS_WINDOWS)
	pending, last_output, killed, code = b"", time.monotonic(), None, None
	with open(part, "rb") as source, open(log, "ab") as combined:

		def drain(now):
			nonlocal pending, last_output
			data = source.read()
			if not data:
				return
			last_output = now
			combined.write(data)
			if echo:
				sys.stdout.buffer.write(data)
				sys.stdout.flush()
			lines = (pending + data).split(b"\n")
			pending = lines.pop()
			for line in lines:
				reader.feed(line.decode("utf-8", "replace").rstrip("\r"), now)

		while True:
			try:
				code = process.wait(timeout=poll)
			except subprocess.TimeoutExpired:
				code = None
			now = time.monotonic()
			drain(now)
			if code is not None:
				break
			begin = reader.test_started if reader.current else last_output
			if now - begin > timeout:
				toolchain.kill_tree(process)
				process.wait()
				killed = now - begin
				drain(time.monotonic())
				break
	if pending:
		reader.feed(pending.decode("utf-8", "replace").rstrip("\r"), time.monotonic())
	return code, reader, killed


def ended_badly(code, results):
	failed = any(result.verdict == "FAILED" for result in results.values())
	return code not in (0, 1) or (code == 1) != failed


def run_whole(command, cwd, chosen, declared, filters, timeout, log, echo, poll=POLL_SECONDS):
	results, problems, runs = {}, [], 0
	part = log.with_name(log.name + ".part")
	while True:
		pending = [name for name in chosen if name not in results]
		if not pending:
			break
		if runs:
			filters = verify.chunk(verify.filter_patterns(set(pending), declared))
		before = len(results)
		crashed = False
		for gtest_filter in filters:
			runs += 1
			arguments = [f"--gtest_filter={gtest_filter}"] if gtest_filter else []
			code, reader, killed = run_process(command + arguments, cwd, part, log, timeout, echo, poll)
			results.update(reader.results)
			if reader.finished and killed is None:
				if ended_badly(code, reader.results):
					problems.append(f"run {runs} ended with {describe(code)} after its last test")
				continue
			crashed = True
			how = f"killed after {killed:.0f} s" if killed is not None else f"crashed, {describe(code)}"
			if reader.current:
				results[reader.current] = Result("FAILED", 0, reader.lines, how)
			else:
				culprits = [name for name in pending if name not in results and verify.suite_of(name) == reader.suite]
				for name in culprits:
					results[name] = Result("FAILED", 0, (), f"{how} outside a test of {reader.suite}")
				if not culprits:
					last = next(reversed(reader.results), None)
					problems.append(f"run {runs} {how} " + (f"after {last}" if last else "before its 1st test"))
			break
		if not crashed or len(results) == before:
			break
	part.unlink(missing_ok=True)
	return results, problems


def run_each(command, cwd, chosen, timeout, log, echo, poll=POLL_SECONDS):
	results = {}
	part = log.with_name(log.name + ".part")
	for name in chosen:
		code, reader, killed = run_process(command + [f"--gtest_filter={name}"], cwd, part, log, timeout, echo, poll)
		result = reader.results.get(name)
		if killed is not None:
			result = Result("FAILED", 0, reader.lines, f"killed after {killed:.0f} s")
		elif result is None:
			how = "crashed" if reader.current == name else "no result"
			result = Result("FAILED", 0, reader.lines, f"{how}, {describe(code)}")
		elif ended_badly(code, reader.results):
			result = Result("FAILED", result.ms, result.lines, f"{describe(code)} after the test")
		results[name] = result
	part.unlink(missing_ok=True)
	return results, []


def fail_skips(results, pattern):
	if not pattern:
		return
	expression = re.compile(pattern)
	for result in results.values():
		if result.verdict != "SKIPPED":
			continue
		reason = next((line for line in result.lines if expression.match(line)), None)
		if reason is not None:
			result.verdict, result.note = "FAILED", f"skipped: {reason}"


def escape(text, property_value=False):
	text = text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
	return text.replace(":", "%3A").replace(",", "%2C") if property_value else text


def annotation(name, result, workspace):
	place = ""
	excerpt = [line for line in result.lines if line.strip()]
	for index, line in enumerate(excerpt):
		match = LOCATION.match(line)
		if not match:
			continue
		excerpt = excerpt[index:]
		try:
			relative = Path(match.group(1)).resolve().relative_to(Path(workspace).resolve()) if workspace else None
		except ValueError:
			relative = None
		if relative is not None:
			place = f"file={escape(relative.as_posix(), True)},line={match.group(2) or match.group(3)},"
		break
	message = "\n".join(([result.note] if result.note else []) + excerpt[:6]) or "failed"
	return f"::error {place}title={escape(name, True)}::{escape(message)}"


def cell(text):
	return text.replace("|", "\\|")


def summary_markdown(label, counts, failed, problems, slowest):
	ran, passed, skipped, failures, seconds = counts
	lines = [f"### {label}", "", "| ran | passed | skipped | failed | minutes |", "| --: | --: | --: | --: | --: |",
			 f"| {ran} | {passed} | {skipped} | {failures} | {seconds / 60:.0f} |", ""]
	if failed:
		lines += ["| failed | why |", "| --- | --- |"]
		lines += [f"| `{name}` | {cell(result.note) or 'failed'} |" for name, result in failed[:SUMMARY_FAILURES]]
		if len(failed) > SUMMARY_FAILURES:
			lines.append(f"| {len(failed) - SUMMARY_FAILURES} more | in the log |")
		lines.append("")
	lines += [f"- {cell(problem)}" for problem in problems]
	if problems:
		lines.append("")
	if slowest:
		lines += ["<details><summary>Slowest tests</summary>", "", "| test | s |", "| --- | --: |"]
		lines += [f"| `{name}` | {result.ms / 1000:.1f} |" for name, result in slowest]
		lines += ["", "</details>", ""]
	return "\n".join(lines) + "\n"


def report(label, chosen, results, problems, seconds, say, environment=os.environ):
	for name in chosen:
		results.setdefault(name, Result("FAILED", 0, (), "no result"))
	ordered = [(name, results[name]) for name in chosen]
	failed = [(name, result) for name, result in ordered if result.verdict == "FAILED"]
	passed = sum(1 for _, result in ordered if result.verdict == "OK")
	skipped = sum(1 for _, result in ordered if result.verdict == "SKIPPED")
	say(f"{label}: ran {len(ordered)}, passed {passed}, skipped {skipped}, failed {len(failed)} in {seconds:.0f} s")
	for name, result in failed:
		say(f"    FAILED {name}" + (f" ({result.note})" if result.note else ""))
	for problem in problems:
		say(f"    {problem}")
	if environment.get("GITHUB_ACTIONS") == "true":
		for name, result in failed[:ANNOTATION_LIMIT]:
			say(annotation(name, result, environment.get("GITHUB_WORKSPACE")))
		for problem in problems:
			say(f"::error title={escape(label, True)}::{escape(problem)}")
	summary = environment.get("GITHUB_STEP_SUMMARY")
	if summary:
		slowest = sorted(ordered, key=lambda item: -item[1].ms)[:SLOWEST]
		counts = (len(ordered), passed, skipped, len(failed), seconds)
		with open(summary, "a", encoding="utf-8", newline="\n") as out:
			out.write(summary_markdown(label, counts, failed, problems, slowest))
	return 0 if ordered and not failed and not problems else 1


def list_tests(command, cwd):
	out = subprocess.run(command + ["--gtest_list_tests"], cwd=cwd, capture_output=True, text=True, errors="replace")
	return verify.parse_listing(out.stdout) if out.returncode == 0 else None


def main():
	parser = argparse.ArgumentParser(description=__doc__,
									 formatter_class=argparse.RawDescriptionHelpFormatter)
	parser.add_argument("tree", type=Path, help="build tree with bin/" + verify.TEST_BINARY)
	parser.add_argument("--only", help="tests to run as gtest patterns (colon/space separated, default all)")
	parser.add_argument("--exclude", help="tests to leave out as gtest patterns (colon/space separated)")
	parser.add_argument("--fail-skips", metavar="REGEX",
						help="fail a skip with a message line matching REGEX")
	parser.add_argument("--each", action="store_true", help="1 process per test (slower, isolates each test)")
	parser.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT,
						help=f"seconds 1 test may run before its process is killed (default {DEFAULT_TIMEOUT})")
	parser.add_argument("--log", type=Path, help="combined output (default <tree>/Testing/suite.log)")
	args = parser.parse_args()

	tree = args.tree.resolve()
	binary = tree / "bin" / verify.TEST_BINARY
	if not binary.is_file():
		print(f"no {binary}. Build the tree")
		return 1
	toolchain.prepend_to_path()
	command, cwd = [str(binary)], tree / "bin"
	declared = list_tests(command, cwd)
	if not declared:
		print(f"{binary.name} won't list its tests")
		return 1

	only, exclude = names_in(args.only), names_in(args.exclude)
	for pattern in only + exclude:
		if not any(fnmatch.fnmatchcase(name, pattern) for name in declared):
			print(f"::warning::{pattern} matches no test of {tree.name}" if os.environ.get("GITHUB_ACTIONS") == "true"
				  else f"warning: {pattern} matches no test of {tree.name}")
	chosen = [name for name in declared if (not only or matches(name, only)) and not matches(name, exclude)]
	if not chosen:
		print("no tests to run")
		return 1
	print(f"{tree.name}: {len(chosen)} of {len(declared)} tests, " +
		  ("1 process per test" if args.each else "1 process (more after a crash or hang)"), flush=True)

	log = (args.log or tree / "Testing" / "suite.log").resolve()
	log.parent.mkdir(parents=True, exist_ok=True)
	log.write_bytes(b"")
	started = time.monotonic()
	if args.each:
		results, problems = run_each(command, cwd, chosen, args.timeout, log, True)
	else:
		filters = (verify.chunk(verify.filter_patterns(set(chosen), declared)) if only
				   else ["-" + ":".join(exclude) if exclude else ""])
		results, problems = run_whole(command, cwd, chosen, declared, filters, args.timeout, log, True)
	fail_skips(results, args.fail_skips)
	return report(tree.name, chosen, results, problems, time.monotonic() - started, lambda text: print(text, flush=True))


if __name__ == "__main__":
	sys.exit(main())
