import fnmatch
import tempfile
import unittest
from pathlib import Path

import verify

SHARED_LISTING = """Running main() from gtest_main.cc
CoreTest.
  Handles
  Lifetime
DLLBoundaryTest.
  ExportsTheRegistry
  UnloadsCleanly
VisualEffectFixture.
  ImportsAGraph
  RendersInTheScene
Typed/RegistryTest/0.  # TypeParam = int
  Registers
Values/OrderTest.
  Holds/0  # GetParam() = 1
  Holds/1  # GetParam() = 2
"""

STATIC_LISTING = """CoreTest.
  Handles
  Lifetime
VisualEffectFixture.
  RendersInTheScene
Typed/RegistryTest/0.  # TypeParam = int
  Registers
Values/OrderTest.
  Holds/0  # GetParam() = 1
PhysicsWorldTest.
  Steps
"""


def selected(filters, declared):
	chosen = set()
	for gtest_filter in filters:
		patterns = gtest_filter.split(":")
		chosen |= {name for name in declared if any(fnmatch.fnmatchcase(name, p) for p in patterns)}
	return chosen


class SelectionTest(unittest.TestCase):

	def test_a_listing_names_every_case_without_its_parameter_comment(self):
		self.assertEqual(verify.parse_listing(SHARED_LISTING), [
			"CoreTest.Handles", "CoreTest.Lifetime",
			"DLLBoundaryTest.ExportsTheRegistry", "DLLBoundaryTest.UnloadsCleanly",
			"VisualEffectFixture.ImportsAGraph", "VisualEffectFixture.RendersInTheScene",
			"Typed/RegistryTest/0.Registers",
			"Values/OrderTest.Holds/0", "Values/OrderTest.Holds/1"])

	def test_the_filters_select_exactly_what_only_the_shared_tree_declares(self):
		shared = verify.parse_listing(SHARED_LISTING)
		static = verify.parse_listing(STATIC_LISTING)
		only = set(shared) - set(static)
		patterns = verify.filter_patterns(only, shared)
		self.assertIn("DLLBoundaryTest.*", patterns)
		self.assertIn("VisualEffectFixture.ImportsAGraph", patterns)
		self.assertNotIn("VisualEffectFixture.*", patterns)
		self.assertEqual(selected(verify.chunk(patterns), shared), only)

	def test_a_filter_too_long_for_one_process_splits_and_still_selects_exactly(self):
		declared = [f"Suite{i // 3}.Case{i}" for i in range(600)]
		only = {name for index, name in enumerate(declared) if index % 2}
		filters = verify.chunk(verify.filter_patterns(only, declared), limit=200)
		self.assertGreater(len(filters), 1)
		self.assertTrue(all(len(f) <= 200 for f in filters))
		self.assertEqual(selected(filters, declared), only)

	def test_a_pattern_longer_than_the_limit_runs_alone_rather_than_vanishing(self):
		long_name = "Suite." + "X" * 50
		self.assertEqual(verify.chunk(["A.*", long_name, "B.*"], limit=20), ["A.*", long_name, "B.*"])

	def test_nothing_only_shared_composes_no_run(self):
		self.assertEqual(verify.chunk(verify.filter_patterns(set(), ["A.B"])), [])


class ResultsTest(unittest.TestCase):

	def read(self, text):
		log = Path(tempfile.mkdtemp()) / "run.log"
		log.write_text(text, encoding="utf-8")
		return verify.read_results(log)

	def test_a_finished_log_names_its_failures_and_skips_once_each(self):
		result = self.read(
			"[ RUN      ] A.Passes\n[       OK ] A.Passes (1 ms)\n"
			"[ RUN      ] A.Skips\n[  SKIPPED ] A.Skips (0 ms)\n"
			"[ RUN      ] P/B.Fails/1\n[  FAILED  ] P/B.Fails/1, where GetParam() = 2 (3 ms)\n"
			"[==========] 3 tests from 2 test suites ran. (4 ms total)\n"
			"[  PASSED  ] 1 test.\n[  SKIPPED ] 1 test, listed below:\n[  SKIPPED ] A.Skips\n"
			"[  FAILED  ] 1 test, listed below:\n[  FAILED  ] P/B.Fails/1, where GetParam() = 2\n")
		self.assertEqual(result["ran"], 3)
		self.assertEqual(result["skipped"], {"A.Skips"})
		self.assertEqual(result["failed"], {"P/B.Fails/1"})

	def test_a_log_that_never_finished_names_the_case_it_died_in(self):
		result = self.read("[ RUN      ] A.Passes\n[       OK ] A.Passes (1 ms)\n[ RUN      ] A.Crashes\n")
		self.assertIsNone(result["ran"])
		self.assertEqual(result["last"], "A.Crashes")


class FlavorTest(unittest.TestCase):

	def tree(self, entries):
		tree = Path(tempfile.mkdtemp())
		lines = [f"{key}:STRING={value}" for key, value in entries.items()]
		(tree / "CMakeCache.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
		return tree

	def test_an_empty_build_type_is_debug(self):
		self.assertEqual(verify.flavor(self.tree({"SHALLTEAR_BUILD_SHARED": "OFF", "CMAKE_BUILD_TYPE": ""})),
						 (False, False))

	def test_a_shared_release_tree_reads_as_one(self):
		self.assertEqual(verify.flavor(self.tree({"SHALLTEAR_BUILD_SHARED": "ON", "CMAKE_BUILD_TYPE": "Release"})),
						 (True, True))


class CSharpGateTest(unittest.TestCase):

	def setUp(self):
		self.root = Path(tempfile.mkdtemp())
		self.said = []

	def tree(self, name, cache):
		tree = self.root / name
		tree.mkdir()
		(tree / "CMakeCache.txt").write_text(cache, encoding="utf-8")
		return tree

	def test_tree_without_csharp_fails_run(self):
		built = self.tree("build-static-release", "SHALLTEAR_CSHARP_ENABLED:INTERNAL=ON\n")
		off = self.tree("build", "SHALLTEAR_CSHARP:STRING=AUTO\nSHALLTEAR_CSHARP_ENABLED:INTERNAL=OFF\n")
		self.assertFalse(verify.csharp_gate((built, off), False, self.said.append))
		self.assertEqual(len(self.said), 1)
		self.assertTrue(self.said[0].startswith("build built without C#"))
		self.assertIn("fetch_prebuilt.py --dotnet", self.said[0])

	def test_flag_accepts_and_names_tree(self):
		built = self.tree("build-static-release", "SHALLTEAR_CSHARP_ENABLED:INTERNAL=ON\n")
		stale = self.tree("build", "CMAKE_BUILD_TYPE:STRING=Debug\n")
		self.assertTrue(verify.csharp_gate((built, stale), True, self.said.append))
		self.assertEqual(self.said, ["  --without-csharp: no C# cases from build"])

	def test_trees_with_csharp_pass_unmentioned(self):
		trees = (self.tree("build-static-release", "SHALLTEAR_CSHARP_ENABLED:INTERNAL=ON\n"),
				 self.tree("build", "SHALLTEAR_CSHARP_ENABLED:INTERNAL=ON\n"))
		self.assertTrue(verify.csharp_gate(trees, False, self.said.append))
		self.assertTrue(verify.csharp_gate(trees, True, self.said.append))
		self.assertEqual(self.said, [])


if __name__ == "__main__":
	unittest.main()
