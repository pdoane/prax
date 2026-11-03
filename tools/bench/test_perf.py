#!/usr/bin/env python3
"""Tests how tools/bench/perf.py reads the measures of runs and decides what changed.

Usage: python3 -m tools.bench.test_perf
"""

import io
import os
import sys
import unittest
from contextlib import redirect_stdout

from tools.bench import perf


def counts(instructions, cycles=None, performance=None, time=1.0):
    return {"instructions": instructions, "cycles": cycles or instructions / 4,
            "performanceInstructions": instructions if performance is None else performance,
            "performanceCycles": 0, "energy": 0, "time": time}


def results(rows, engine="prax", workload="w"):
    return {"meta": {"commit": "0" * 40, "dirty": False, "date": "2026-01-01T00:00:00", "workload": workload},
            "engines": {engine: rows}}


class MeasuresTest(unittest.TestCase):
    def test_a_rows_counts_are_its_measures(self):
        run = results([{"name": "sheet", "parse": 1, "counts": {"parse": counts(10), "write": counts(20)}},
                       {"name": "case", "time": 1, "counts": {"time": counts(30)}},
                       {"name": "load first frame", "time": 22.0}])
        self.assertEqual(set(perf.measures(run)), {"sheet parse", "sheet write", "case"})
        self.assertEqual(perf.timings(run), {"load first frame": 22.0})

    def test_the_harness_beside_a_shell_names_its_measures(self):
        run = results([{"name": "case", "time": 1, "counts": {"time": counts(30)}}], engine="prax (C)")
        self.assertEqual(list(perf.measures(run)), ["case (C)"])

    def test_another_engines_measures_are_named_by_it(self):
        row = {"name": "tree cold", "time": 1, "boxes": 4, "counts": {"time": counts(30)}}
        run = results([dict(row, shape={"laidOut": 4, "reused": 0, "measured": 0})])
        run["engines"]["yoga"] = [dict(row, counts={"time": counts(60)}, shape={"laidOut": 4})]
        self.assertEqual(set(perf.measures(run)), {"tree cold", "tree cold (yoga)"})
        output = io.StringIO()
        with redirect_stdout(output):
            perf.print_engines(run)
        self.assertRegex(output.getvalue(), r"tree cold\s+30\s+60\s+2\.00\s+2\.00\s+4/0/0\s+4/-/-")


class ChangeTest(unittest.TestCase):
    def test_a_change_past_the_threshold_is_one(self):
        self.assertTrue(perf.Change("m", [counts(100)], [counts(102)]).changed(1.0))
        self.assertFalse(perf.Change("m", [counts(100)], [counts(100.5)]).changed(1.0))

    def test_a_change_within_the_runs_spread_is_none(self):
        before = [counts(100), counts(110)]
        after = [counts(104), counts(112)]
        self.assertFalse(perf.Change("m", before, after).changed(1.0))

    def test_every_run_of_one_side_is_beyond_every_run_of_the_other(self):
        self.assertTrue(perf.Change("m", [counts(100), counts(101)], [counts(120), counts(121)]).changed(1.0))
        self.assertFalse(perf.Change("m", [counts(100), counts(130)], [counts(120), counts(121)]).changed(1.0))

    def test_runs_that_count_no_instructions_compare_their_cycles(self):
        change = perf.Change("m", [counts(0, cycles=100)], [counts(0, cycles=110)])
        self.assertEqual(change.key, "cycles")
        self.assertTrue(change.changed(1.0))
        self.assertFalse(perf.Change("m", [counts(0, cycles=100)], [counts(0, cycles=100.5)]).changed(1.0))

    def test_the_share_on_performance_cores(self):
        self.assertEqual(perf.share(counts(200, performance=50)), 25.0)


class ReportTest(unittest.TestCase):
    def report(self, before, after, everything=False):
        out = io.StringIO()
        with redirect_stdout(out):
            assessment = perf.Assessment("x", [results(before)], [results(after)], 1.0)
            changed = perf.print_report("test", [assessment], 1.0, 1, everything)
        return changed, out.getvalue()

    def test_only_what_changed_is_listed(self):
        before = [{"name": "a", "counts": {"time": counts(1000)}}, {"name": "b", "counts": {"time": counts(1000)}}]
        after = [{"name": "a", "counts": {"time": counts(1500)}}, {"name": "b", "counts": {"time": counts(1001)}}]
        changed, text = self.report(before, after)
        self.assertTrue(changed)
        self.assertIn("x: 1 changed", text)
        self.assertIn("+50.0%", text)
        self.assertNotIn("\nb ", text)

    def test_a_base_without_counts_compares_nothing(self):
        changed, text = self.report([{"name": "a", "time": 1}], [{"name": "a", "counts": {"time": counts(1)}}])
        self.assertFalse(changed)
        self.assertIn("no counts", text)


class FormatTest(unittest.TestCase):
    def test_counts_in_three_figures(self):
        self.assertEqual(perf.scaled(895), "895")
        self.assertEqual(perf.scaled(4250000), "4.25M")
        self.assertEqual(perf.scaled(65e9), "65G")

    def test_times_in_three_figures_and_their_unit(self):
        self.assertEqual(perf.duration(0.0291), "29.1us")
        self.assertEqual(perf.duration(3.6e-5), "36ns")
        self.assertEqual(perf.duration(318.4), "318ms")
        self.assertEqual(perf.duration(2242.0), "2.24s")


if __name__ == "__main__":
    unittest.main()
