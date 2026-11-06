#!/usr/bin/env python3
"""Tests the parts of the terminals benchmark (tools/bench/terminals.py) that need no terminal: its statistics, its
table cells, and how it reads the feed's reports.

Usage: python3 -m tools.bench.test_terminals
"""

import os
import sys
import unittest

from tools.bench import terminals
from tools.bench import windows


def run(**measures):
    out = {"wall": 1000.0, "bytes": 64 << 20, "cpu": 500.0, "instructions": 1e9, "energy": 100.0, "gpu": None,
           "peak": 200e6, "footprint": 150e6}
    out.update(measures)
    return out


def result(kind, amount, runs, timeout=None):
    out = terminals.Result(terminals.Workload(kind, amount, "w"))
    out.runs = runs
    out.timeout = timeout
    return out


def measure(name):
    return next(m for m in terminals.MEASURES if m[0] == name)


class SummaryTest(unittest.TestCase):
    def test_median_and_spread(self):
        middle, spread, low, high = terminals.summary([4.0, 1.0, 2.0, None])
        self.assertEqual((middle, low, high), (2.0, 1.0, 4.0))
        self.assertAlmostEqual(spread, 1.5)

    def test_no_runs(self):
        self.assertIsNone(terminals.summary([None]))

    def test_ratio_marks_overlapping_runs(self):
        prax = terminals.summary([10.0, 12.0])
        self.assertEqual(terminals.ratio_cell(prax, terminals.summary([11.0, 13.0])), "1.09x?")
        self.assertEqual(terminals.ratio_cell(prax, terminals.summary([20.0, 22.0])), "1.91x ")
        self.assertEqual(terminals.ratio_cell(prax, None), "-")

    def test_ratio_of_a_rate_is_its_inverse(self):
        prax = terminals.summary([200.0])
        self.assertEqual(terminals.ratio_cell(prax, terminals.summary([100.0]), True), "2.00x ")


class CellTest(unittest.TestCase):
    def test_throughput_in_mib_per_second(self):
        cell = terminals.cell(result("throughput", 64 << 20, [run(wall=500.0), run(wall=500.0)]), measure("rate"))
        self.assertEqual(cell, "128 MiB/s ±0%")

    def test_timeout(self):
        timed_out = result("throughput", 64 << 20, [], timeout=10)
        self.assertEqual(terminals.cell(timed_out, measure("rate")), "> 10 s")
        self.assertEqual(terminals.cell(timed_out, measure("cpu")), "-")

    def test_paced_costs_per_second(self):
        paced = result("paced", 5, [run(wall=5000.0, cpu=1000.0, energy=2000.0)])
        self.assertEqual(terminals.cell(paced, measure("rate")), "5.00s")
        self.assertEqual(terminals.cell(paced, measure("cpu")), "200ms/s")
        self.assertEqual(terminals.cell(paced, measure("energy")), "400mW")

    def test_memory_in_megabytes(self):
        self.assertEqual(terminals.cell(result("throughput", 1, [run(peak=512e6)]), measure("peak")), "512 MB")

    def test_bytes_per_line_over_idle(self):
        record = terminals.Record(None)
        record.idle = 100e6
        fill = result("fill", 100000, [run(footprint=150e6)])
        self.assertEqual(terminals.bytes_per_line(record, fill), "500 B")


class InvalidTest(unittest.TestCase):
    def test_covered_window(self):
        runs = [run(covered=0.0, gpu=10.0), run(covered=0.5, gpu=10.0), run(covered=None, gpu=10.0)]
        terminals.invalidate(runs)
        self.assertNotIn("invalid", runs[0])
        self.assertEqual(runs[1]["invalid"], "50% of its window was covered")
        self.assertEqual(runs[2]["invalid"], "its window was not on screen")

    def test_no_gpu_time_from_a_metal_client(self):
        runs = [run(covered=0.0, gpu=0.0), run(covered=0.0, gpu=None)]
        terminals.invalidate(runs)
        self.assertEqual(runs[0]["invalid"], "no GPU time")
        self.assertNotIn("invalid", runs[1])

    def test_cpu_far_below_another_run(self):
        runs = [run(covered=0.0, cpu=500.0), run(covered=0.0, cpu=100.0), run(covered=0.0, cpu=300.0)]
        terminals.invalidate(runs)
        self.assertEqual([r.get("invalid") for r in runs], [None, "CPU time 100ms, against 500ms in another run", None])

    def test_cells_use_valid_runs(self):
        partial = result("throughput", 64 << 20, [run(wall=500.0), run(wall=250.0, invalid="no GPU time")])
        self.assertEqual(terminals.cell(partial, measure("rate")), "128 MiB/s")
        self.assertEqual(partial.invalid(), "1 of 2 runs invalid: no GPU time")
        none = result("throughput", 64 << 20, [run(invalid="no GPU time")])
        self.assertEqual(terminals.cell(none, measure("cpu")), "?")
        self.assertTrue(terminals.is_invalid(none))
        self.assertFalse(terminals.is_invalid(partial))

    def test_mean_counts_its_workloads(self):
        self.assertEqual(terminals.mean_cell([2.0, 8.0]), "4.00x (2)")
        self.assertEqual(terminals.mean_cell(None), "-")


class WindowsTest(unittest.TestCase):
    def test_union_area(self):
        self.assertEqual(windows.union_area([]), 0)
        self.assertEqual(windows.union_area([(0, 0, 10, 10), (5, 5, 15, 15)]), 175)
        self.assertEqual(windows.union_area([(0, 0, 10, 10), (2, 2, 4, 4)]), 100)

    def test_covered_share(self):
        listing = [windows.Window(1, 25, (0, 0, 1000, 1000), 1.0), windows.Window(2, 0, (0, 0, 50, 100), 1.0),
                   windows.Window(3, 0, (50, 0, 50, 100), 0.0), windows.Window(7, 0, (0, 0, 100, 100), 1.0),
                   windows.Window(4, 0, (0, 0, 100, 100), 1.0)]
        # The menu bar's level is above the applications', and a transparent window covers nothing.
        self.assertEqual(windows.covered_share(listing, {7}), 0.5)
        self.assertEqual(windows.covered_share(listing, {2, 7}), 0.0)
        self.assertEqual(windows.covered_share(listing, {4}), 1.0)
        self.assertIsNone(windows.covered_share(listing, {9}))


class ReportTest(unittest.TestCase):
    @unittest.skipIf(sys.platform == "win32", "select() reads pipes only on POSIX")
    def test_lines_and_errors(self):
        reader, writer = os.pipe()
        try:
            os.write(writer, b"ready 12\ndone 1.5 120 40 99 1\nerror no workload named x\n")
            buffer = bytearray()
            self.assertEqual(terminals.read_line(reader, buffer, 1, "ready"), ["ready", "12"])
            self.assertEqual(terminals.read_line(reader, buffer, 1, "done"), ["done", "1.5", "120", "40", "99", "1"])
            with self.assertRaisesRegex(terminals.RunFailed, "no workload named x"):
                terminals.read_line(reader, buffer, 1, "done")
            with self.assertRaisesRegex(terminals.RunFailed, "no done within"):
                terminals.read_line(reader, buffer, 0.01, "done")
        finally:
            os.close(reader)
            os.close(writer)


FOOTPRINT = """terminal [93035]: 64-bit    Footprint: 95 MB (16384 bytes per page)

  Dirty      Clean  Reclaimable    Regions    Category
    ---        ---          ---        ---    ---
  42 MB        0 B          0 B         10    IOSurface
2416 KB        0 B          0 B         10    App-Specific Tag 1
 304 KB      17 MB          0 B         92    mapped file
    0 B    2640 KB          0 B       1143    __TEXT
    ---        ---          ---        ---    ---
  95 MB      17 MB          0 B       1255    TOTAL
"""


class FootprintTest(unittest.TestCase):
    def test_categories(self):
        self.assertEqual(terminals.footprint_categories(FOOTPRINT), {
            "IOSurface": 42 << 20, "App-Specific Tag 1": 2416 << 10, "mapped file": 304 << 10, "__TEXT": 0})


if __name__ == "__main__":
    unittest.main()
