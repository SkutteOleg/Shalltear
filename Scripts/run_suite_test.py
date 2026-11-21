import json
import sys
import tempfile
import unittest
from pathlib import Path

import run_suite

FAKE_TESTS = r'''
import fnmatch
import json
import os
import sys
import time

spec = json.load(open(sys.argv[1]))
arguments = sys.argv[2:]
with open(spec["calls"], "a") as calls:
	calls.write(" ".join(arguments) + "\n")
if "--gtest_list_tests" in arguments:
	for suite, cases in spec["suites"]:
		print(suite + ".")
		for name, _ in cases:
			print("  " + name)
	sys.exit(0)
pattern = next((a.split("=", 1)[1] for a in arguments if a.startswith("--gtest_filter=")), "")
positive, _, negative = pattern.partition("-")
wanted = [p for p in positive.split(":") if p] or ["*"]
unwanted = [p for p in negative.split(":") if p]

def chosen(full):
	return (any(fnmatch.fnmatchcase(full, p) for p in wanted)
			and not any(fnmatch.fnmatchcase(full, p) for p in unwanted))

def say(text=""):
	print(text, flush=True)

BAR = "[" + "-" * 10 + "]"
failed = []
ran = 0
passed = 0
for suite, cases in spec["suites"]:
	picked = [(name, how) for name, how in cases if chosen(suite + "." + name)]
	if not picked:
		continue
	say(f"{BAR} {len(picked)} tests from {suite}")
	if suite in spec.get("setup_crash", []):
		os._exit(3)
	for name, how in picked:
		full = suite + "." + name
		ran += 1
		say(f"[ RUN      ] {full}")
		if how == "crash":
			os._exit(3)
		if how == "hang":
			time.sleep(30)
		if how == "fail":
			say("fake.cpp:7: Failure")
			say("Value of: broken")
			say(f"[  FAILED  ] {full} (2 ms)")
			failed.append(full)
		elif how.startswith("skip:"):
			say("fake.cpp(9): Skipped")
			say(how[5:])
			say()
			say(f"[  SKIPPED ] {full} (0 ms)")
		else:
			say(f"[       OK ] {full} ({spec.get('ms', 1)} ms)")
			passed += 1
	say(f"{BAR} {len(picked)} tests from {suite} (3 ms total)")
	say()
say(f"[==========] {ran} tests from 2 test suites ran. (5 ms total)")
say(f"[  PASSED  ] {passed} tests.")
if failed:
	say(f"[  FAILED  ] {len(failed)} tests, listed below:")
	for full in failed:
		say(f"[  FAILED  ] {full}")
if spec.get("exit_after"):
	os._exit(spec["exit_after"])
sys.exit(1 if failed else 0)
'''


class FakeSuite:

	def __init__(self, suites, **extra):
		self.root = Path(tempfile.mkdtemp())
		self.calls = self.root / "calls.txt"
		spec = self.root / "spec.json"
		spec.write_text(json.dumps({"suites": suites, "calls": str(self.calls), **extra}), encoding="utf-8")
		fake = self.root / "fake_tests.py"
		fake.write_text(FAKE_TESTS, encoding="utf-8")
		self.command = [sys.executable, str(fake), str(spec)]
		self.log = self.root / "suite.log"
		self.log.write_bytes(b"")
		self.declared = run_suite.list_tests(self.command, self.root)

	def runs(self):
		return [line for line in self.calls.read_text(encoding="utf-8").splitlines() if "--gtest_list_tests" not in line]

	def whole(self, filters=("",), timeout=60):
		return run_suite.run_whole(self.command, self.root, self.declared, self.declared, list(filters), timeout,
								   self.log, False, poll=0.05)

	def each(self, timeout=60):
		return run_suite.run_each(self.command, self.root, self.declared, timeout, self.log, False, poll=0.05)


def verdicts(results):
	return {name: result.verdict for name, result in results.items()}


class WholeSuiteTest(unittest.TestCase):

	def test_a_clean_suite_runs_in_a_single_process(self):
		fake = FakeSuite([["A", [["Passes", "ok"], ["Fails", "fail"]]], ["B", [["Skips", "skip:not here"]]]])
		results, problems = fake.whole()
		self.assertEqual(verdicts(results), {"A.Passes": "OK", "A.Fails": "FAILED", "B.Skips": "SKIPPED"})
		self.assertEqual(results["A.Fails"].lines, ["fake.cpp:7: Failure", "Value of: broken"])
		self.assertEqual(problems, [])
		self.assertEqual(len(fake.runs()), 1)
		self.assertIn(b"[ RUN      ] B.Skips", fake.log.read_bytes())

	def test_a_crash_fails_its_test_and_the_rest_still_run(self):
		fake = FakeSuite([["A", [["Before", "ok"], ["Crashes", "crash"], ["After", "ok"]]], ["B", [["Later", "ok"]]]])
		results, problems = fake.whole()
		self.assertEqual(verdicts(results), {"A.Before": "OK", "A.Crashes": "FAILED", "A.After": "OK", "B.Later": "OK"})
		self.assertEqual(results["A.Crashes"].note, "crashed, exit 3")
		self.assertEqual(problems, [])
		self.assertEqual(len(fake.runs()), 2)
		self.assertNotIn("A.Before", fake.runs()[1])

	def test_a_hung_test_is_killed_and_the_run_goes_on_after_it(self):
		fake = FakeSuite([["A", [["Before", "ok"], ["Hangs", "hang"], ["After", "ok"]]]])
		results, _ = fake.whole(timeout=1)
		self.assertEqual(verdicts(results), {"A.Before": "OK", "A.Hangs": "FAILED", "A.After": "OK"})
		self.assertTrue(results["A.Hangs"].note.startswith("killed after"))

	def test_a_crash_in_a_suite_setup_fails_that_suite_alone(self):
		fake = FakeSuite([["A", [["Before", "ok"]]], ["B", [["Lost", "ok"], ["AlsoLost", "ok"]]], ["C", [["Later", "ok"]]]],
						 setup_crash=["B"])
		results, _ = fake.whole()
		self.assertEqual(verdicts(results), {"A.Before": "OK", "B.Lost": "FAILED", "B.AlsoLost": "FAILED", "C.Later": "OK"})
		self.assertEqual(results["B.Lost"].note, "crashed, exit 3 outside a test of B")

	def test_a_crash_after_the_last_test_is_a_problem(self):
		fake = FakeSuite([["A", [["Passes", "ok"]]]], exit_after=5)
		results, problems = fake.whole()
		self.assertEqual(verdicts(results), {"A.Passes": "OK"})
		self.assertEqual(problems, ["run 1 ended with exit 5 after its last test"])

	def test_an_excluded_test_never_runs(self):
		fake = FakeSuite([["A", [["Kept", "ok"], ["Excluded", "fail"]]]])
		chosen = [name for name in fake.declared if name != "A.Excluded"]
		results, _ = run_suite.run_whole(fake.command, fake.root, chosen, fake.declared, ["-A.Excluded"], 60, fake.log,
										 False, poll=0.05)
		self.assertEqual(verdicts(results), {"A.Kept": "OK"})

	def test_a_resumed_run_names_whole_suites_by_pattern(self):
		fake = FakeSuite([["A", [["Crashes", "crash"], ["Rest", "ok"]]], ["B", [["Next", "ok"], ["Last", "ok"]]]])
		fake.whole()
		self.assertIn("B.*", fake.runs()[1])
		self.assertIn("A.Rest", fake.runs()[1])


class EachTest(unittest.TestCase):

	def test_each_test_gets_a_process(self):
		fake = FakeSuite([["A", [["Passes", "ok"], ["Crashes", "crash"], ["Skips", "skip:not here"]]]])
		results, _ = fake.each()
		self.assertEqual(verdicts(results), {"A.Passes": "OK", "A.Crashes": "FAILED", "A.Skips": "SKIPPED"})
		self.assertEqual(results["A.Crashes"].note, "crashed, exit 3")
		self.assertEqual(len(fake.runs()), 3)

	def test_a_test_passing_in_a_process_dying_at_exit_fails(self):
		fake = FakeSuite([["A", [["Passes", "ok"]]]], exit_after=5)
		results, _ = fake.each()
		self.assertEqual(results["A.Passes"].verdict, "FAILED")
		self.assertEqual(results["A.Passes"].note, "exit 5 after the test")


class ReportTest(unittest.TestCase):

	def setUp(self):
		self.said = []

	def test_a_skip_matching_the_pattern_fails_and_others_stay_skips(self):
		results = {"A.Device": run_suite.Result("SKIPPED", 0, ["x.cpp(9): Skipped", "no Vulkan device available"]),
				   "A.Other": run_suite.Result("SKIPPED", 0, ["x.cpp(9): Skipped", "not on this machine"])}
		run_suite.fail_skips(results, r"^(no (Vulkan )?(device|ICD)|no sibling static)")
		self.assertEqual(results["A.Device"].verdict, "FAILED")
		self.assertEqual(results["A.Device"].note, "skipped: no Vulkan device available")
		self.assertEqual(results["A.Other"].verdict, "SKIPPED")

	def test_a_test_without_a_result_fails_the_report(self):
		results = {"A.Ran": run_suite.Result("OK", 5)}
		code = run_suite.report("tree", ["A.Ran", "A.Missing"], results, [], 3, self.said.append, {})
		self.assertEqual(code, 1)
		self.assertEqual(self.said, ["tree: ran 2, passed 1, skipped 0, failed 1 in 3 s", "    FAILED A.Missing (no result)"])

	def test_a_problem_fails_a_run_without_failed_tests(self):
		code = run_suite.report("tree", ["A.Passes"], {"A.Passes": run_suite.Result("OK")}, ["run 1 ended with exit 5"], 1,
								self.said.append, {})
		self.assertEqual(code, 1)

	def test_a_clean_run_passes_and_writes_the_step_summary(self):
		summary = Path(tempfile.mkdtemp()) / "summary.md"
		results = {"A.Slow": run_suite.Result("OK", 1500), "A.Skips": run_suite.Result("SKIPPED")}
		code = run_suite.report("tree", ["A.Slow", "A.Skips"], results, [], 120, self.said.append,
								{"GITHUB_STEP_SUMMARY": str(summary)})
		self.assertEqual(code, 0)
		text = summary.read_text(encoding="utf-8")
		self.assertIn("| 2 | 1 | 1 | 0 | 2 |", text)
		self.assertIn("| `A.Slow` | 1.5 |", text)

	def test_on_actions_a_failure_becomes_an_annotation_on_its_line(self):
		workspace = Path(tempfile.mkdtemp())
		source = workspace / "Source" / "Tests" / "X.cpp"
		results = {"A.Fails": run_suite.Result("FAILED", 0, [f"{source}:12: Failure", "Value of: a, b"])}
		run_suite.report("tree", ["A.Fails"], results, [], 1, self.said.append,
						 {"GITHUB_ACTIONS": "true", "GITHUB_WORKSPACE": str(workspace)})
		self.assertEqual(self.said[-1],
						 f"::error file=Source/Tests/X.cpp,line=12,title=A.Fails::{source}:12: Failure%0AValue of: a, b")

	def test_a_bar_in_a_reason_stays_inside_its_cell(self):
		failed = [("A.Skips", run_suite.Result("FAILED", 0, (), "skipped: a|b"))]
		text = run_suite.summary_markdown("tree", (1, 0, 0, 1, 60), failed, [], [])
		self.assertIn("| `A.Skips` | skipped: a\\|b |", text)


class ArgumentsTest(unittest.TestCase):

	def test_names_split_on_colons_and_white_space(self):
		self.assertEqual(run_suite.names_in("A.B:C.D  E.F\n G.*"), ["A.B", "C.D", "E.F", "G.*"])
		self.assertEqual(run_suite.names_in(None), [])

	def test_exit_codes_read_as_written(self):
		self.assertEqual(run_suite.describe(3), "exit 3")
		self.assertEqual(run_suite.describe(3221225477), "exit 0xC0000005")
		self.assertEqual(run_suite.describe(-11), "signal 11")


if __name__ == "__main__":
	unittest.main()
