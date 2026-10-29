#!/usr/bin/env python3
"""Tests how WPT's pages map to web features, and the rulings file's own checks.

Usage: python3 -m tools.suite.wpt.test_web_features
"""

import os
import tempfile
import unittest
from unittest.mock import patch

from tools.suite.wpt import web_features


class PagesTests(unittest.TestCase):
    def pages(self, manifests, features=None):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        for path, text in manifests.items():
            os.makedirs(os.path.join(directory.name, path), exist_ok=True)
            with open(os.path.join(directory.name, path, "WEB_FEATURES.yml"), "w",
                      encoding="utf-8") as f:
                f.write(text)
        return web_features.Pages(directory.name, features or {})

    def test_the_first_rule_that_matches_decides_and_an_empty_list_names_no_feature(self):
        pages = self.pages({"css/x": "rules:\n- host-is-plain.html: []\n- host-*: [host]\n- \"*\": "
                                     "[x]\n"})
        self.assertEqual(pages.of("css/x/host-is-plain.html"), [])
        self.assertEqual(pages.of("css/x/host-context.html"), ["host"])
        self.assertEqual(pages.of("css/x/other.html"), ["x"])

    def test_a_directory_without_a_manifest_takes_its_parents(self):
        pages = self.pages({"css": "rules:\n- \"*\": [css]\n", "css/y": "rules:\n- a.html: [a]\n"})
        self.assertEqual(pages.of("css/z/b.html"), ["css"])
        self.assertEqual(pages.of("css/y/a.html"), ["a"])
        self.assertEqual(pages.of("css/y/b.html"), [])
        self.assertEqual(pages.of("dom/c.html"), [])

    def test_a_moved_feature_is_the_one_it_moved_to(self):
        features = {"old": {"kind": "moved", "redirect_target": "new"}, "new": {"kind": "feature"},
                    "both": {"kind": "split", "redirect_targets": ["first", "second"]}}
        pages = self.pages({"dom": "rules:\n- a.html: [old, both]\n"}, features)
        self.assertEqual(pages.of("dom/a.html"), ["new", "first"])


class RulingsTests(unittest.TestCase):
    def check(self, text, features):
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8") as f:
            f.write("# web-features 1.0.0\n\n" + text)
        self.addCleanup(os.unlink, f.name)
        with patch.object(web_features, "RULINGS_FILE", f.name):
            return [problem for _, problem in web_features.check(features)]

    def test_every_feature_is_ruled_once_with_a_ruling_the_file_defines(self):
        features = {"a": {"kind": "feature"}, "b": {"kind": "feature"}, "gone": {"kind": "moved"}}
        self.assertEqual(self.check("a build\nb cut  # why\n", features), [])
        self.assertEqual(self.check("a build\n", features), ["b has no ruling"])
        self.assertEqual(self.check("a build\nb maybe\n", features)[0],
                         "a line is a feature and one of build, cut, open, quickjs")
        self.assertEqual(self.check("a build\nb cut\na open\n", features), ["a is ruled twice"])
        self.assertEqual(self.check("a build\nb cut\ngone open\n", features),
                         ["gone is not a feature of web-features 1.0.0"])

    def test_a_feature_that_web_features_does_not_name_is_ruled_by_its_anchor(self):
        features = {"a": {"kind": "feature"}}
        ruling = "css-color-5/#at-profile cut  # why\n"
        self.assertEqual(self.check("a build\n" + ruling, features), [])


if __name__ == "__main__":
    unittest.main()
