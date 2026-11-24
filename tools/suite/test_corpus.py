"""Tests of the runners' shared scoreboard: its rates and its table."""

import argparse
from contextlib import redirect_stdout
import io
import json
import os
import tempfile
import unittest
from unittest.mock import patch

from tools.suite import corpus


class ScoreboardTests(unittest.TestCase):
    def test_a_rate_is_measured_to_a_tenth_rounded_down(self):
        self.assertEqual(corpus.pass_rate(2, 3), 66.6)
        self.assertEqual(corpus.pass_rate(0, 0), 100.0)

    def test_a_table_lines_up_its_columns(self):
        lines = corpus.table(("Part", "Cases"), [("a", "1/2"), ("longer", "10/10")])
        self.assertEqual(lines, [
            "| Part   | Cases |",
            "|--------|------:|",
            "| a      |   1/2 |",
            "| longer | 10/10 |",
        ])

    def test_a_scoreboard_rates_each_part_and_ends_with_the_total(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(corpus, "SUITES", directory):
            data = corpus.Data("sample")
            data.write_scoreboard([("one", 1, 2), ("two", 3, 3)])
            with open(data.path(corpus.SCOREBOARD), encoding="utf-8") as f:
                text = f.read()
        self.assertTrue(text.startswith("# sample scoreboard\n"))
        self.assertIn("| Part      | Cases | Passing |\n", text)
        self.assertIn("| one       |   1/2 |   50.0% |\n", text)
        self.assertTrue(text.endswith("| **total** |   4/5 |   80.0% |\n"))


class OutcomeTests(unittest.TestCase):
    def compare(self, base, passed, statuses=None):
        """Returns what --against prints for a run of `passed` and `statuses` against the outcomes `base`."""
        with tempfile.TemporaryDirectory() as directory:
            against = os.path.join(directory, "base.json")
            with open(against, "w", encoding="utf-8") as f:
                json.dump(base, f)
            args = argparse.Namespace(json=None, against=against)
            out = io.StringIO()
            with redirect_stdout(out):
                corpus.outcomes(args, passed, statuses)
        return out.getvalue().splitlines()

    def test_a_case_whose_status_changed_is_reported_though_its_passes_did_not(self):
        lines = self.compare({"a": {"passed": 0, "status": "OK"}, "b": {"passed": 2, "status": "OK"}},
                             {"a": 0, "b": 3}, {"a": "TIMEOUT", "b": "OK"})
        self.assertIn("0 case(s) lost 0 test(s), 1 case(s) gained 1 test(s), 1 case(s) changed status", lines[1])
        self.assertEqual(lines[2:], ["  gained  b  2 -> 3", "  status  a  OK -> TIMEOUT"])

    def test_a_base_without_statuses_compares_by_passes(self):
        lines = self.compare({"a": 1}, {"a": 0}, {"a": "ERROR"})
        self.assertIn("1 case(s) lost 1 test(s), 0 case(s) gained 0 test(s), 0 case(s) changed status", lines[1])
        self.assertEqual(lines[2:], ["  lost    a  1 -> 0"])

    def test_outcomes_are_written_with_their_statuses(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "run.json")
            corpus.outcomes(argparse.Namespace(json=path, against=None), {"a": 1, "b": 2}, {"a": "OK"})
            with open(path, encoding="utf-8") as f:
                self.assertEqual(json.load(f), {"a": {"passed": 1, "status": "OK"}, "b": 2})


class ResultsTests(unittest.TestCase):
    def run_cases(self, cases, errata="", requires_errata=True, timing="", **options):
        """Returns the exit status and the output of a run of `cases`, each (key, passed, count, status) and then the
        outcomes of its later tries if it has any, with the errata `errata` and the timing list `timing`, each in the
        errata's form."""
        with tempfile.TemporaryDirectory() as directory, patch.object(corpus, "SUITES", directory):
            os.makedirs(os.path.join(directory, "sample"))
            for name, text in (("errata.txt", errata), ("timing.txt", timing)):
                if text:
                    with open(os.path.join(directory, "sample", name), "w", encoding="utf-8") as f:
                        f.write(text)
            args = argparse.Namespace(json=None, against=None, match=None, verbose=False, failures=False,
                                      shaper=None, hinting=None, **options)
            data = corpus.Data("sample")
            results = corpus.Results(args, data, keys={case[0] for case in cases}, requires_errata=requires_errata)
            out = io.StringIO()
            with redirect_stdout(out), patch.object(corpus, "BUILD", "elsewhere"):
                for key, passed, count, status, *tries in cases:
                    results.case(key, passed, count, status, detail="why", part="part", tries=tries[0] if tries else ())
                code = results.finish()
        return code, out.getvalue()

    def test_a_case_line_is_the_same_whatever_the_suite(self):
        self.assertEqual(corpus.case_line("a", 1, 1), "a    1/1    PASS")
        self.assertEqual(corpus.case_line("a", 2, 3, detail="no"), "a    2/3    FAIL: no")
        self.assertEqual(corpus.case_line("a", 0, 1, "TIMEOUT"), "a    0/1    TIMEOUT")

    def test_a_case_that_fails_without_an_entry_fails_a_suite_that_requires_one(self):
        code, out = self.run_cases([("a:0", 1, 1, corpus.OK), ("a:1", 0, 1, corpus.OK)])
        self.assertEqual(code, 1)
        self.assertIn("1 failing without an entry in the errata:\n  a:1    0/1    FAIL: why", out)

    def test_a_scoreboard_suite_counts_its_failures(self):
        code, out = self.run_cases([("a:0", 0, 1, "TIMEOUT")], requires_errata=False)
        self.assertEqual(code, 0)
        self.assertIn("part  0/1    0.0%", out)

    def test_a_case_whose_tries_differ_fails_the_run(self):
        code, out = self.run_cases([("a:0", 0, 1, corpus.OK, [(1, 1, corpus.OK)])], requires_errata=False)
        self.assertEqual(code, 1)
        self.assertIn("a:0 differs between runs (0/1 OK, 1/1 OK); fix it, or list it in timing.txt with why", out)

    def test_a_case_whose_tries_agree_counts_as_it_ran(self):
        code, out = self.run_cases([("a:0", 0, 1, "TIMEOUT", [(0, 1, "TIMEOUT")])], requires_errata=False)
        self.assertEqual(code, 0)
        self.assertIn("part  0/1    0.0%", out)

    def test_a_case_that_depends_on_timing_counts_its_best_try(self):
        code, out = self.run_cases([("a:0", 0, 1, "TIMEOUT", [(1, 1, corpus.OK)])], requires_errata=False,
                                   timing="a:0\n    its server's delay races its timer.\n")
        self.assertEqual(code, 0)
        self.assertIn("part  1/1  100.0%", out)

    def test_an_entry_excuses_a_case_and_is_looked_at_once_it_passes(self):
        code, out = self.run_cases([("a:0", 0, 1, corpus.OK), ("b:0", 1, 1, corpus.OK)],
                                   errata="a:0\n    the corpus is wrong.\n\nb:0\n    the corpus was wrong.\n")
        self.assertEqual(code, 1)
        self.assertIn("part  2/2  100.0%   (1 recorded)", out)
        self.assertIn("1 to look at:\n  b:0 agrees now; drop its entry", out)


class FuzzyTests(unittest.TestCase):
    def test_a_shift_is_read_beside_the_ranges_which_ignore_it(self):
        self.assertEqual(corpus.fuzzy_shift("shift=1;maxDifference=0-2;totalPixels=0-90"), 1)
        self.assertEqual(corpus.fuzzy_content_ranges("shift=1;maxDifference=0-2;totalPixels=0-90"), ((0, 2), (0, 90)))
        self.assertEqual(corpus.fuzzy_shift("maxDifference=0-2;totalPixels=0-90"), 0)

    def test_a_shift_other_than_a_pixel_is_refused(self):
        with self.assertRaises(SystemExit):
            corpus.fuzzy_shift("shift=2;maxDifference=0-2;totalPixels=0-90")


if __name__ == "__main__":
    unittest.main()
