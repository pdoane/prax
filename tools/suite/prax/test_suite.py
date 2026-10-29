#!/usr/bin/env python3
"""Regression tests for the checks of our pages (suite.py), and for how their stills are captured (suite_stills.py).

Usage: python3 -m tools.suite.prax.test_suite
"""

import unittest

from tools.suite.prax import suite
from tools.suite.prax import suite_stills


class Headings(unittest.TestCase):
    def test_numbers_that_webref_leaves_out(self):
        # CSS 2's chapters from 10 on have their numbers in their titles, and appendix E's sections have none.
        self.assertEqual(suite.heading("CSS2", "visudet")[:2], ("10", "Visual formatting model details"))
        self.assertEqual(suite.heading("CSS2", "stacking-defs")[:2], ("E.1", "Definitions"))
        self.assertEqual(suite.heading("CSS2", "stacking-notes")[:2], ("E.3", "Notes"))
        self.assertEqual(suite.by_number("CSS2", "10.3.7"), "abs-non-replaced-width")

    def test_anchors_name_their_page(self):
        # Each of SVG 2's chapters is a page of its own with an `Introduction`, and webref has no heading for a chapter.
        self.assertEqual(suite.heading("SVG2", "shapes.html#Introduction")[:2], ("10.1", "Introduction and definitions"))
        self.assertEqual(suite.heading("SVG2", "paths.html#Introduction")[0], "9.1")
        self.assertIsNone(suite.heading("SVG2", "Introduction"))
        self.assertEqual(suite.chapter("SVG2", "shapes.html"), "10")

    def test_names(self):
        self.assertEqual(suite.spec_name("CSS2"), "CSS 2")
        self.assertEqual(suite.spec_name("css-fonts-4"), "CSS Fonts 4")
        self.assertEqual(suite.spec_name("css-ui-3"), "CSS Basic User Interface 3")
        self.assertEqual(suite.spec_name("SVG2"), "SVG 2")


class Directories(unittest.TestCase):
    def test_every_level_a_directory_walks(self):
        self.assertEqual(suite.parse_directory("css-sizing css-sizing-3 css-sizing-4"),
                         ("css-sizing", ["css-sizing-3", "css-sizing-4"], None))
        self.assertEqual(suite.parse_directory("css2-16-text CSS2/#text"), ("css2-16-text", ["CSS2"], "text"))
        self.assertEqual(suite.parse_directory("svg2-10-basic-shapes SVG2/shapes.html"),
                         ("svg2-10-basic-shapes", ["SVG2"], "shapes.html"))

    def test_only_the_base_level_names_a_heading(self):
        self.assertIsNone(suite.parse_directory("css-sizing css-sizing-3 css-sizing-4/#intro"))
        self.assertIsNone(suite.parse_directory("css-sizing-3/#intro"))

    def test_a_directory_that_walks_no_specification(self):
        self.assertEqual(suite.parse_directory("workloads"), ("workloads", [], None))


class Eyebrows(unittest.TestCase):
    section = ("CSS 2", "8.3", "Margin properties: margin-top, margin-right, margin-bottom, margin-left, and margin", "")

    def test_title_whole_short_or_qualified(self):
        for given in ("CSS 2 · 8.3 Margin properties: margin-top, margin-right, margin-bottom, margin-left, and margin",
                      "CSS 2 · 8.3 Margin properties", "CSS 2 · 8.3 margin properties, collapsing"):
            with self.subTest(given=given):
                self.assertIsNone(suite.eyebrow_problem(given, self.section))

    def test_quotes_and_case_are_free(self):
        expected = ("CSS 2", "9.2.4", "The display property", "")
        self.assertIsNone(suite.eyebrow_problem("CSS 2 · 9.2.4 The 'display' property", expected))

    def test_number_is_fixed(self):
        message, right = suite.eyebrow_problem("CSS 2 · 8.2 Margin properties", self.section)
        self.assertEqual(right, "CSS 2 · 8.3 Margin properties")

    def test_other_title_is_reported(self):
        message, right = suite.eyebrow_problem("CSS 2 · 8.3 Padding properties", self.section)
        self.assertIsNone(right)

    def test_supports(self):
        expected = ("CSS 2", "10", "Visual formatting model details", " · Support")
        self.assertIsNone(suite.eyebrow_problem("CSS 2 · 10 Visual formatting model details · Support", expected))
        self.assertIsNotNone(suite.eyebrow_problem("CSS 2 · 10 Visual formatting model details", expected))
        self.assertIsNone(suite.eyebrow_problem("CSS Fonts 4 · Support", ("CSS Fonts 4", None, None, " · Support")))

    def test_chapter_without_a_heading(self):
        expected = ("SVG 2", "10", None, " · Support")
        self.assertIsNone(suite.eyebrow_problem("SVG 2 · 10 Basic Shapes · Support", expected))
        self.assertEqual(suite.eyebrow_problem("SVG 2 · 9 Basic Shapes · Support", expected)[1],
                         "SVG 2 · 10 Basic Shapes · Support")


class Citations(unittest.TestCase):
    def test_number_is_fixed_from_the_anchor(self):
        text = ('<a href="https://drafts.csswg.org/css2/#abs-non-replaced-width">CSS 2 §10.3.6</a>, '
                '<a href="https://drafts.csswg.org/css2/#abs-non-replaced-height">§10.6.4</a>')
        problems = []
        self.assertEqual(suite.check_citations("page.html", text, problems, False), text)
        self.assertEqual(len(problems), 1)
        fixed = suite.check_citations("page.html", text, [], True)
        self.assertIn(">CSS 2 §10.3.7</a>", fixed)
        self.assertIn(">§10.6.4</a>", fixed)

    def test_page_of_svg_2(self):
        text = '<a href="https://svgwg.org/svg2-draft/shapes.html#RectElement">SVG 2 §10.3</a>'
        problems = []
        suite.check_citations("page.html", text, problems, False)
        self.assertEqual(len(problems), 1)
        self.assertIn(">SVG 2 §10.2</a>", suite.check_citations("page.html", text, [], True))

    def test_name_is_the_specification(self):
        problems = []
        suite.check_citations("page.html", '<a href="https://drafts.csswg.org/css-ui-4/#outline-props">'
                              "CSS UI 4 §5.1</a>", problems, False)
        self.assertTrue(any("is named 'CSS Basic User Interface 4'" in p[2] for p in problems))


class Issues(unittest.TestCase):
    def test_link_names_the_issue(self):
        problems = []
        suite.check_issues("page.html", '<a href="https://github.com/w3c/csswg-drafts/issues/5742">csswg-drafts#5742'
                           '</a>', problems)
        self.assertEqual(problems, [])
        suite.check_issues("page.html", '<a href="https://github.com/w3c/csswg-drafts/issues/5742">issue</a>', problems)
        self.assertEqual(len(problems), 1)

    def test_unlinked_issue(self):
        for text in ("as csswg-drafts#8284 says", "(csswg-drafts issue 5742)", "issue #1234"):
            with self.subTest(text=text):
                problems = []
                suite.check_issues("page.html", text, problems)
                self.assertEqual(len(problems), 1)


class Sources(unittest.TestCase):
    def test_test_suite_cited(self):
        for text in ("as WPT's tentative test expects", "no WPT test covers the case", "web-platform-tests"):
            with self.subTest(text=text):
                problems = []
                suite.check_sources("page.html", text, problems)
                self.assertEqual(len(problems), 1)

    def test_fixture_path(self):
        problems = []
        suite.check_sources("page.html", '<img src="../../../third_party/wpt/css/css-color/support/009900.png">',
                            problems)
        self.assertEqual(problems, [])


class Declared(unittest.TestCase):
    def test_values_and_descriptors_are_not_properties(self):
        page = ('<style>@font-feature-values F { @styleset { bullet: 1 } } @font-face { src: url(a.woff) }'
                ' .a { float: left; background: url(data:image/svg+xml,a:b) }</style>'
                '<div style="clear: both; outline-top: 1px; content: \'x: y\'">')
        self.assertEqual(suite.declared(page), {"float", "background", "clear", "content"})


class Stills(unittest.TestCase):
    def test_moving_figures_are_held_still(self):
        # A capture loads a copy of the page that resolves its URLs against the page's directory and pauses every
        # `.pr-motion` stage's animations, ahead of the page's own style sheets.
        document = suite_stills.with_base("<!doctype html><html><head><link rel=stylesheet href=../shared/page.css>",
                                          "/suites/prax/css-transforms/", suite_stills.FREEZE)
        self.assertEqual(document, '<!doctype html><html><head><base href="/suites/prax/css-transforms/">'
                         + suite_stills.FREEZE + "<link rel=stylesheet href=../shared/page.css>")
        self.assertIn("animation-play-state: paused !important", suite_stills.FREEZE)


if __name__ == "__main__":
    unittest.main()
