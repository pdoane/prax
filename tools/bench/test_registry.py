#!/usr/bin/env python3
"""Tests that tools/bench/registry.py names what the benchmarks need in forms that the tools know.

Usage: python3 -m tools.bench.test_registry
"""

import unittest

from tools.bench import registry
from tools.fetch import checkouts


class RegistryTest(unittest.TestCase):
    def test_every_benchmark_fetches_only_checkouts(self):
        # A dependency that the build fetches itself, as CMake fetches Yoga and Taffy, is not a checkout.
        for bench in registry.BENCHES:
            for name in bench.fetch:
                with self.subTest(bench=bench.name, checkout=name):
                    self.assertIn(name, checkouts.CHECKOUTS)


if __name__ == "__main__":
    unittest.main()
