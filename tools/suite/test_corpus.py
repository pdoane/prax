"""Tests of the runners' shared scoreboard: its rates and its table."""

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


if __name__ == "__main__":
    unittest.main()
