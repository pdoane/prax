#!/usr/bin/env python3
"""The WPT runner's own suite (./dev.sh suite wpt-runner): integration tests that drive the pr_shell of the build that
the runners use against the WPT checkout and its server, and the check of tools/suite/wpt/web_features.txt's rulings
against the features that the checkout's pages test.

Usage: python3 -m tools.suite.wpt.runner_tests [unittest's arguments]
"""

import io
import os
import re
import subprocess
import tempfile
import time
import unittest
from unittest.mock import patch
from contextlib import redirect_stdout
from pathlib import Path

from tools.suite import corpus
from tools.suite.wpt import web_features
from tools.suite.wpt import wpt

# A run that the tests make writes its scoreboard here, not over the checkout's scoreboard.
SCRATCH = tempfile.TemporaryDirectory()
SCOREBOARD = patch.object(wpt, "SCOREBOARD_FILE", os.path.join(SCRATCH.name, "scoreboard.md"))


def setUpModule():
    SCOREBOARD.start()


def tearDownModule():
    SCOREBOARD.stop()
    SCRATCH.cleanup()


class RunnerTests(unittest.TestCase):
    def run_page(self, script):
        with tempfile.TemporaryDirectory(dir=wpt.scratch_directory()) as directory:
            path = os.path.join(directory, "test.html")
            with open(path, "w", encoding="utf-8", newline="\n") as page:
                page.write('<script src="/resources/testharness.js"></script>\n'
                           '<script src="/resources/testharnessreport.js"></script>\n'
                           '<script>' + script + '</script>\n')
            return wpt.run(wpt.SHELL, wpt.Test(path))

    def test_completion_does_not_wait_for_harness_watchdog(self):
        start = time.monotonic()
        result = self.run_page('''
            test(() => {}, "synchronous");
            async_test(t => { setTimeout(() => t.done(), 20); }, "timer");
        ''')
        self.assertIsNone(result.harness)
        self.assertEqual([(status, name) for status, name, _ in result.results],
                         [("PASS", "synchronous"), ("PASS", "timer")])
        # A run that waited for the runner's deadline would end there, however fast the machine.
        self.assertLess(time.monotonic() - start, wpt.TIMEOUT)

    def test_runner_times_out_a_stalled_script(self):
        with patch.object(wpt, "TIMEOUT", 0.1):
            result = self.run_page('while (true) {}')
        self.assertEqual(result.harness, "TIMEOUT")

    def test_a_test_that_marks_itself_long_takes_the_long_deadline(self):
        with tempfile.TemporaryDirectory() as directory:
            marks = {"page.html": '<meta name="timeout" content="long">', "script.any.js": "// META: timeout=long\n",
                     "plain.html": "<p>", "plain.any.js": "// META: global=window\n"}
            for name, text in marks.items():
                with open(os.path.join(directory, name), "w", encoding="utf-8") as f:
                    f.write(text)
            timeouts = {name: wpt.Test(os.path.join(directory, name)).timeout for name in marks}
            self.assertEqual(timeouts, {"page.html": wpt.LONG_TIMEOUT, "script.any.js": wpt.LONG_TIMEOUT,
                                        "plain.html": wpt.TIMEOUT, "plain.any.js": wpt.TIMEOUT})
            long = wpt.Test(os.path.join(directory, "page.html"))
            self.assertEqual(wpt.deadline_options(long), ["--timeout", "%d" % (wpt.LONG_TIMEOUT * 1000)])
            self.assertEqual(wpt.deadline_options(wpt.Test(os.path.join(directory, "plain.html"))), [])

    def test_a_test_that_a_person_runs_is_manual(self):
        with tempfile.TemporaryDirectory() as directory:
            pages = {
                "animated.html": '<meta name="flags" content="ahem animated"><link rel="match" href="r.html">',
                "paged.html": "<meta content=paged name=flags><script src=/resources/testharness.js></script>",
                "plain.html": '<meta name="flags" content="ahem"><link rel="match" href="r.html">',
                "click-manual.html": "<script src=/resources/testharness.js></script>",
                "look-visual.html": '<link rel="match" href="r.html">',
                "pages-print.html": '<meta name="flags" content="paged"><link rel="match" href="r.html">',
            }
            manual = {}
            for name, text in pages.items():
                path = os.path.join(directory, name)
                with open(path, "w", encoding="utf-8") as f:
                    f.write(text)
                manual[name] = wpt.is_manual(path)
            # WPT's manifest finds a print reftest before it reads a test's flags.
            self.assertEqual(manual, {"animated.html": True, "paged.html": True, "plain.html": False,
                                      "click-manual.html": True, "look-visual.html": True,
                                      "pages-print.html": False})

    def test_subtest_exception_keeps_other_results(self):
        result = self.run_page('''
            test(() => {}, "first");
            test(() => { throw new Error("subtest failure"); }, "second");
            test(() => {}, "third");
            test(() => {}, "fourth");
        ''')
        self.assertTrue(result.complete)
        self.assertIsNone(result.harness)
        self.assertEqual([status for status, _, _ in result.results],
                         ["PASS", "FAIL", "PASS", "PASS"])
        self.assertEqual(wpt.summary(result, "page.html"), "FAIL: subtest failure")

        # A failure that ERRATA records is not named in the row's summary.
        with patch.dict(wpt.ERRATA, {"page.html:1": "recorded"}):
            self.assertEqual(wpt.summary(result, "page.html"), "")

    def test_uncaught_exception_preserves_completed_results(self):
        result = self.run_page('''
            test(() => {}, "completed");
            throw new Error("top-level failure");
        ''')
        self.assertFalse(result.complete)
        self.assertEqual(result.results, [("PASS", "completed", "")])
        self.assertEqual(result.harness, "ERROR: Error: top-level failure")
        found = [(result.path, "test.html", wpt.TESTHARNESS)]
        with patch.object(wpt, "run", return_value=result), \
             patch.object(wpt, "select", return_value=(found, {})), \
             patch("sys.argv", ["wpt.py", "--match", "test.html"]), \
             redirect_stdout(io.StringIO()):
            wpt.main()

    def test_a_crashtest_passes_a_document_that_is_not_well_formed_and_fails_any_other_failure(self):
        def outcome(stderr):
            process = subprocess.CompletedProcess([], 1, "", stderr)
            with patch.object(wpt.SHELLS, "run", return_value=process):
                return wpt.crash("shell", wpt.Test("page.xhtml")).harness
        self.assertIsNone(outcome("page.xhtml: XML Parsing Error: an element is not closed\n"))
        self.assertEqual(outcome("page.xhtml: HTML load failed: Io\n"), "CRASH: page.xhtml: HTML load failed: Io")

    def test_a_case_that_reaches_for_what_the_engine_lacks_fails(self):
        result = self.run_page('''
            test(() => {}, "inside");
            test(() => { document.createRange().getBoundingClientRect(); }, "lacking");
            test(() => { assert_true(false); }, "failing");
        ''')
        self.assertTrue(result.complete)
        self.assertEqual([status for status, _, _ in result.results], ["PASS", "FAIL", "FAIL"])
        found = [(result.path, "css-a/test.html", wpt.TESTHARNESS)]
        output = io.StringIO()
        with patch.object(wpt, "run", return_value=result), \
             patch.object(wpt, "select", return_value=(found, {})), \
             patch.dict(wpt.ERRATA, {}, clear=True), \
             patch("sys.argv", ["wpt.py", "--match", "css-a"]), redirect_stdout(output):
            self.assertEqual(wpt.main(), 0)
        self.assertRegex(output.getvalue(), r"css-a\s+1/1\s+1/3\s+33.3%")

    def test_a_file_whose_harness_reaches_for_what_the_engine_lacks_is_incomplete(self):
        result = self.run_page('''
            test(() => {}, "before");
            document.createRange().getBoundingClientRect();
        ''')
        self.assertFalse(result.complete)
        self.assertRegex(result.harness, r"^ERROR: TypeError")
        found = [(result.path, "css-a/test.html", wpt.TESTHARNESS)]
        output = io.StringIO()
        with patch.object(wpt, "run", return_value=result), \
             patch.object(wpt, "select", return_value=(found, {})), \
             patch.dict(wpt.ERRATA, {}, clear=True), \
             patch("sys.argv", ["wpt.py", "--match", "css-a"]), redirect_stdout(output):
            self.assertEqual(wpt.main(), 0)
        self.assertRegex(output.getvalue(), r"css-a\s+0/1\s+1/1")

    def test_timeout_preserves_completed_results(self):
        with patch.object(wpt, "TIMEOUT", 0.15):
            result = self.run_page('test(() => {}, "completed"); while (true) {}')
        self.assertEqual(result.harness, "TIMEOUT")
        self.assertFalse(result.complete)
        self.assertEqual(result.results, [("PASS", "completed", "")])

    def test_completion_order_does_not_change_registration_indices(self):
        result = self.run_page('''
            async_test(t => { setTimeout(() => t.done(), 20); }, "first");
            test(() => {}, "second");
        ''')
        self.assertIsNone(result.harness)
        self.assertEqual(result.result_indices, [0, 1])
        self.assertEqual([name for _, name, _ in result.results], ["first", "second"])

    def test_syntax_error_reports_error_event_and_parsing_continues(self):
        result = self.run_page('''
            setup({allow_uncaught_exception: true});
            let seen = 0;
            addEventListener("error", event => {
                test(() => {
                    assert_true(event instanceof ErrorEvent);
                    assert_true(event instanceof Event);
                    assert_true(event.message.startsWith("SyntaxError:"));
                    assert_true(event.filename.endsWith("test.html"));
                    assert_equals(event.currentTarget, window);
                }, "reported syntax error");
                ++seen;
            });
            </script><script>const = ;</script><script>
            test(() => { assert_equals(seen, 1); }, "subsequent script ran");
        ''')
        self.assertTrue(result.complete)
        self.assertIsNone(result.harness)
        self.assertEqual([status for status, _, _ in result.results], ["PASS", "PASS"])

    def test_error_event_constructor_and_dispatch(self):
        result = self.run_page('''
            let called = false;
            addEventListener("sample", event => {
                test(() => {
                    assert_true(event instanceof ErrorEvent);
                    assert_equals(event.type, "sample");
                    assert_equals(event.message, "message");
                    assert_equals(event.filename, "source.js");
                    assert_equals(event.lineno, 12);
                    assert_equals(event.colno, 7);
                }, "ErrorEvent fields survive native dispatch");
                called = true;
            });
            dispatchEvent(new ErrorEvent("sample", {
                message: "message", filename: "source.js", lineno: "12", colno: 7
            }));
            test(() => { assert_true(called); }, "listener called");
        ''')
        self.assertIsNone(result.harness)
        self.assertEqual([status for status, _, _ in result.results], ["PASS", "PASS"])

    def test_missing_completion_is_failure_even_with_results(self):
        output = 'RESULT {"index": 2, "status": "PASS", "name": "done", "message": ""}\n'
        with patch.object(wpt.SHELLS, "run", return_value=
                          subprocess.CompletedProcess([], 0, stdout=output, stderr="")):
            result = self.run_page('')
        self.assertEqual(result.results, [("PASS", "done", "")])
        self.assertEqual(result.result_indices, [2])
        self.assertEqual(result.harness, "INCOMPLETE: harness did not complete")

    def test_process_failure_after_results_is_not_success(self):
        output = ('RESULT {"index": 0, "status": "PASS", "name": "done", "message": ""}\n'
                  'HARNESS {"status": "OK", "message": ""}\n')
        with patch.object(wpt.SHELLS, "run", return_value=
                          subprocess.CompletedProcess([], -6, stdout=output,
                                                      stderr="fatal: stopped\n")):
            result = self.run_page('')
        self.assertEqual(result.results, [("PASS", "done", "")])
        self.assertEqual(result.harness, "ERROR: fatal: stopped")
        self.assertFalse(result.complete)

    def test_timeout_can_truncate_a_report_without_losing_previous_results(self):
        output = (b'RESULT {"index": 0, "status": "PASS", "name": "done", "message": ""}\n'
                  b'RESULT {"index": 1, "status":')
        with patch.object(wpt.SHELLS, "run", side_effect=
                          subprocess.TimeoutExpired([], 1, output=output)):
            result = self.run_page('')
        self.assertEqual(result.results, [("PASS", "done", "")])
        self.assertEqual(result.harness, "TIMEOUT")


class GateTests(unittest.TestCase):
    def test_scoreboard_round_trip(self):
        boards = {"css": {"css/b": wpt.Score("2/2", "1/8", 12.5),
                          "css/a": wpt.Score("1/1", "40/40", 100.0),
                          corpus.TOTAL: wpt.Score("3/3", "41/48", 85.4)},
                  "url": {"url": wpt.Score("1/1", "1/2", 50.0),
                          corpus.TOTAL: wpt.Score("1/1", "1/2", 50.0)}}
        text = wpt.format_scoreboard(boards)
        self.assertEqual(wpt.parse_scoreboard(text), boards)
        self.assertLess(text.index("| css/b"), text.index("| **total**"))

    def test_a_run_stops_on_rulings_that_do_not_match_the_suites(self):
        problems = [(0, "a-feature has no ruling"), (0, "b-feature is tested by no suite")]
        with patch.object(wpt.web_features, "data", return_value={}), \
             patch.object(wpt, "tested_features", return_value=set()), \
             patch.object(wpt.web_features, "check", return_value=problems), \
             patch.object(wpt, "select") as select, \
             patch("sys.argv", ["wpt.py", "--list"]), redirect_stdout(io.StringIO()):
            with self.assertRaises(SystemExit) as stopped:
                wpt.main()
        self.assertIn("a-feature has no ruling", str(stopped.exception.code))
        self.assertIn("b-feature is tested by no suite", str(stopped.exception.code))
        select.assert_not_called()

    def test_errata_entries_expand_their_keys(self):
        with tempfile.TemporaryDirectory() as root:
            path = os.path.join(root, "errata.txt")
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write("# a comment\n\na.html:0,3-5\nb.html\n    One sentence\n    over two "
                        "lines.\n\nc.html:2\n    Another.\n")
            errata = corpus.read_errata(path)
        self.assertEqual(sorted(errata),
                         ["a.html:0", "a.html:3", "a.html:4", "a.html:5", "b.html", "c.html:2"])
        self.assertEqual(errata["b.html"], "One sentence over two lines.")
        self.assertEqual(errata["c.html:2"], "Another.")

    def test_file_errata_counts_a_file_as_complete_and_says_so(self):
        result = wpt.Test("css-a/test.html")
        result.harness = "ERROR: setup failed"
        found = [(result.path, result.path, wpt.TESTHARNESS)]
        output = io.StringIO()
        with patch.object(wpt, "run", return_value=result), \
             patch.object(wpt, "select", return_value=(found, {})), \
             patch.dict(wpt.ERRATA, {result.path: "known corpus problem"}, clear=True), \
             patch("sys.argv", ["wpt.py", "--first-errors"]), redirect_stdout(output):
            self.assertEqual(wpt.main(), 0)
        self.assertRegex(output.getvalue(), r"css-a\s+1/1\s+0/0")
        self.assertIn("ERROR: setup failed", output.getvalue())
        self.assertIn("1 file(s) excused by ERRATA", output.getvalue())
        output = io.StringIO()
        with patch.object(wpt, "run", return_value=result), \
             patch.object(wpt, "select", return_value=(found, {})), \
             patch.dict(wpt.ERRATA, {result.path: "known corpus problem"}, clear=True), \
             patch("sys.argv", ["wpt.py", "--raw"]), redirect_stdout(output):
            wpt.main()
        self.assertRegex(output.getvalue(), r"css-a\s+0/1")

    def test_a_plain_run_of_the_debug_build_records_its_scores(self):
        result = wpt.Test("css-a/test.html")
        result.complete = True
        result.result_indices = list(range(10))
        result.results = [("PASS", "pass", "")] * 9 + [("FAIL", "fail", "wrong")]
        found = [(result.path, result.path, wpt.TESTHARNESS)]
        debug = corpus.binary("pr_shell", corpus.DEFAULT)
        other = corpus.binary("pr_shell", os.path.join(corpus.ROOT, "build-asan"))
        for argv, recorded in ((["wpt.py", "--binary", debug], True),
                               (["wpt.py", "--binary", debug, "--match", "css-a"], False),
                               (["wpt.py", "--binary", other], False)):
            with patch.object(wpt, "run", return_value=result), \
                 patch.object(wpt, "select", return_value=(found, {})), \
                 patch.object(corpus, "require"), \
                 patch.dict(wpt.ERRATA, {}, clear=True), \
                 patch.object(wpt, "record_scoreboard") as record, \
                 patch("sys.argv", argv), redirect_stdout(io.StringIO()):
                wpt.main()
            if recorded:
                record.assert_called_once_with("css", {"css-a": wpt.Score("1/1", "9/10", 90.0),
                                                       corpus.TOTAL: wpt.Score("1/1", "9/10", 90.0)})
            else:
                record.assert_not_called()


class ReftestTests(unittest.TestCase):
    SQUARE = '<div style="width: 100px; height: 100px; background-color: %s"></div>'

    def run_reftests(self, pages, tests, overrides=""):
        """Returns the output of a run of the reftests `tests` among `pages`, each given as a name and its text, with
        the fuzzy overrides `overrides`, in the form of suites/wpt/fuzzy.txt with each test named by its path."""
        with tempfile.TemporaryDirectory() as directory:
            for name, text in pages.items():
                with open(os.path.join(directory, name), "w", encoding="utf-8", newline="\n") as page:
                    page.write(text)
            fuzzy = os.path.join(directory, "fuzzy.txt")
            with open(fuzzy, "w", encoding="utf-8", newline="\n") as f:
                f.write(overrides.replace("DIR", os.path.relpath(directory, wpt.WPT).replace(os.sep, "/")))
            found = [(os.path.join(directory, name), "css-a/" + name, wpt.REFTEST) for name in tests]
            output = io.StringIO()
            with patch.object(wpt, "select", return_value=(found, {})), \
                 patch.dict(wpt.ERRATA, {}, clear=True), patch.object(wpt, "FUZZY_FILE", fuzzy), \
                 patch("sys.argv", ["wpt.py", "--match", "css-a"]), redirect_stdout(output):
                self.assertEqual(wpt.main(), 0)
            return output.getvalue()

    def test_a_page_is_captured_and_compared_with_its_references(self):
        output = self.run_reftests({
            "ref.html": self.SQUARE % "green",
            "match.html": '<link rel="match" href="ref.html">' + self.SQUARE % "green",
            "differs.html": '<link rel="match" href="ref.html">' + self.SQUARE % "red",
            "mismatch.html": '<link rel="mismatch" href="ref.html">' + self.SQUARE % "blue",
        }, ["match.html", "differs.html", "mismatch.html"])
        self.assertRegex(output, r"css-a/differs.html\s+0/1\s+reftest\n")
        self.assertRegex(output, r"css-a\s+3/3\s+2/3")

    def test_a_page_passes_by_matching_any_of_its_match_references(self):
        output = self.run_reftests({
            "green.html": self.SQUARE % "green",
            "red.html": self.SQUARE % "red",
            "either.html": '<link rel="match" href="red.html"><link rel="match" href="green.html">'
                           + self.SQUARE % "green",
            "neither.html": '<link rel="match" href="red.html"><link rel="match" href="green.html">'
                            + self.SQUARE % "blue",
            "unlike.html": '<link rel="match" href="green.html"><link rel="mismatch" href="red.html">'
                           + self.SQUARE % "red",
        }, ["either.html", "neither.html", "unlike.html"])
        self.assertRegex(output, r"css-a/neither.html\s+0/1\s+reftest\n")
        self.assertRegex(output, r"css-a/unlike.html\s+0/1\s+reftest\n")
        self.assertRegex(output, r"css-a\s+3/3\s+1/3")

    def test_a_page_passes_within_its_fuzzy_ranges_as_wptrunner_measures_them(self):
        square = '<div style="width: 100px; height: 100px; background-color: rgb(%s)"></div>'
        output = self.run_reftests({
            "ref.html": square % "0, 128, 0",
            "other.html": square % "0, 128, 0",
            "fuzzy.html": '<meta name="fuzzy" content="maxDifference=0-2;totalPixels=0-10000">'
                          '<link rel="match" href="ref.html">' + square % "0, 130, 0",
            "beyond.html": '<meta name="fuzzy" content="0-1;0-10000">'
                           '<link rel="match" href="ref.html">' + square % "0, 130, 0",
            "keyed.html": '<meta name="fuzzy" content="other.html:0-2;0-10000">'
                          '<link rel="match" href="ref.html">' + square % "0, 130, 0",
            "blue.html": '<link rel="match" href="ref.html">' + square % "0, 128, 1",
        }, ["fuzzy.html", "beyond.html", "keyed.html", "blue.html"])
        # A meta keyed by another reference does not apply. A difference of one level in blue alone has no luma, so
        # wptrunner counts no pixel as differing.
        self.assertRegex(output, r"css-a/beyond.html\s+0/1\s+reftest\n")
        self.assertRegex(output, r"css-a/keyed.html\s+0/1\s+reftest\n")
        self.assertRegex(output, r"css-a\s+4/4\s+2/4")

    def test_mismatch_references_chain_after_a_match_reference_without_the_tests_fuzzy_ranges(self):
        square = '<div style="width: 100px; height: 100px; background-color: rgb(%s)"></div>'
        links = ('<meta name="fuzzy" content="0-2;0-10000"><link rel="match" href="ref.html">'
                 '<link rel="mismatch" href="%s">')
        output = self.run_reftests({
            "ref.html": square % "0, 128, 0",
            "near.html": square % "0, 129, 0",
            "same.html": square % "0, 128, 0",
            "chained.html": links % "near.html" + square % "0, 130, 0",
            "equal.html": links % "same.html" + square % "0, 130, 0",
        }, ["chained.html", "equal.html"])
        # The mismatch reference is within the test's fuzzy ranges, but wptrunner compares it with the match reference
        # and the test without them. One that equals the match reference fails.
        self.assertRegex(output, r"css-a/equal.html\s+0/1\s+reftest\n")
        self.assertRegex(output, r"css-a\s+2/2\s+1/2")

    def test_a_fuzzy_override_replaces_the_tests_ranges(self):
        square = '<div style="width: 100px; height: 100px; background-color: rgb(%s)"></div>'
        output = self.run_reftests({
            "ref.html": square % "0, 128, 0",
            "near.html": '<meta name="fuzzy" content="0-1;0-10000"><link rel="match" href="ref.html">'
                         + square % "0, 130, 0",
        }, ["near.html"], "DIR/near.html maxDifference=0-2;totalPixels=0-10000\n    It differs by two levels.\n")
        self.assertRegex(output, r"css-a\s+1/1\s+1/1")

    def test_fuzzy_metas_apply_by_reference_and_unkeyed_to_the_rest(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "test.html")
            with open(path, "w", encoding="utf-8", newline="\n") as page:
                page.write('<meta name="fuzzy" content="a.html:maxDifference=1-2;totalPixels=3-4">'
                           '<meta name="fuzzy" content="5;6-7">')
            self.assertEqual(wpt.fuzzy(path, os.path.join(directory, "a.html")), ((1, 2), (3, 4)))
            self.assertEqual(wpt.fuzzy(path, os.path.join(directory, "b.html")), ((5, 5), (6, 7)))
        # A measure of zero passes a range that starts at zero, as wptrunner judges it.
        self.assertTrue(wpt.within(((0, 4), (10, 20)), 0, 3))
        self.assertFalse(wpt.within(((1, 4), (10, 20)), 2, 3))
        self.assertFalse(wpt.within(((0, 0), (0, 0)), 1, 1))

    # Two pages of print, each a square, the first green, that a forced break separates.
    PAGES = '<div style="break-after: page">' + SQUARE % "green" + '</div>' + SQUARE

    def test_a_print_reftest_compares_its_pages_in_order_and_fails_on_another_count(self):
        output = self.run_reftests({
            "ref.html": self.PAGES % "green",
            "same-print.html": '<link rel="match" href="ref.html">' + self.PAGES % "green",
            "fewer-print.html": '<link rel="match" href="ref.html">' + self.SQUARE % "green",
            "second-print.html": '<link rel="match" href="ref.html">' + self.PAGES % "red",
            "unlike-print.html": '<link rel="mismatch" href="ref.html">' + self.SQUARE % "green",
        }, ["same-print.html", "fewer-print.html", "second-print.html", "unlike-print.html"])
        self.assertRegex(output, r"css-a/fewer-print.html\s+0/1\s+reftest\n")
        self.assertRegex(output, r"css-a/second-print.html\s+0/1\s+reftest\n")
        self.assertRegex(output, r"css-a\s+4/4\s+2/4")

    def test_a_print_reftest_compares_the_pages_that_its_reftest_pages_metas_select(self):
        # The body has no margin, which would start the first page lower than the second.
        flush = '<style>body { margin: 0 }</style>'
        output = self.run_reftests({
            "ref.html": flush + self.SQUARE % "red",
            "pages.html": flush + self.PAGES % "red",
            "own-print.html": '<meta name="reftest-pages" content="2-"><link rel="match" href="ref.html">'
                              + flush + self.PAGES % "red",
            "keyed-print.html": '<meta name="reftest-pages" content="pages.html:2">'
                                '<link rel="match" href="pages.html">' + flush + self.SQUARE % "red",
            "all-print.html": '<link rel="match" href="ref.html">' + flush + self.PAGES % "red",
        }, ["own-print.html", "keyed-print.html", "all-print.html"])
        self.assertRegex(output, r"css-a/all-print.html\s+0/1\s+reftest\n")
        self.assertRegex(output, r"css-a\s+3/3\s+2/3")

    def test_a_print_reftest_is_printed_on_the_page_box_of_wpts_protocol(self):
        # The page is 5 by 3 inches, with margins of half an inch, so its page area is 384 by 192 px, 48 px in from the
        # page's edges.
        output = self.run_reftests({
            "ref.html": '<style>@page { margin: 0 } body { margin: 0 }</style>'
                        '<div style="margin: 48px; width: 384px; height: 192px; background: green"></div>',
            "area-print.html": '<link rel="match" href="ref.html"><style>body { margin: 0 }</style>'
                               '<div style="height: 192px; background: green"></div>',
        }, ["area-print.html"])
        self.assertRegex(output, r"css-a\s+1/1\s+1/1")

    def test_a_page_that_never_finishes_fails(self):
        output = self.run_reftests({
            "ref.html": self.SQUARE % "green",
            "waits.html": '<html class="reftest-wait"><link rel="match" href="ref.html">'
                          + self.SQUARE % "green",
        }, ["waits.html"])
        self.assertIn("reftest-wait was not removed", output)
        self.assertRegex(output, r"css-a\s+0/1\s+0/1")

    def test_a_page_that_reaches_for_what_the_engine_lacks_renders(self):
        output = self.run_reftests({
            "ref.html": self.SQUARE % "green",
            "lacking.html": '<link rel="match" href="ref.html">' + self.SQUARE % "green"
                            + "<script>document.createRange().getBoundingClientRect();</script>",
        }, ["lacking.html"])
        self.assertRegex(output, r"css-a\s+1/1\s+1/1")


class DiscoveryTests(unittest.TestCase):
    def test_finds_what_the_wpt_manifest_finds(self):
        harness = '<script src="/resources/testharness.js"></script>'
        pages = {
            "a.html": harness,
            "b-ref.html": "<p>reference",
            "b.html": '<link rel="match" href="b-ref.html"><p>reftest',
            "support/c.html": harness,
            "crashtests/d.html": "<p>crash",
            "charset/e.html": harness.encode("utf-16-le"),
            "notes.txt": harness,
            "f.any.js": "test(() => {});",
            "g.window.js": "test(() => {});",
            "h.worker.js": "test(() => {});",
        }
        with tempfile.TemporaryDirectory() as root:
            for name, text in pages.items():
                path = os.path.join(root, name)
                os.makedirs(os.path.dirname(path), exist_ok=True)
                data = b"\xff\xfe" + text if isinstance(text, bytes) else text.encode("utf-8")
                with open(path, "wb") as page:
                    page.write(data)
            found = [(name, kind) for _, name, kind in wpt.discover(root, [""])]
        self.assertEqual(found, [("a.html", wpt.TESTHARNESS),
                                 ("b.html", wpt.REFTEST),
                                 ("f.any.js", wpt.TESTHARNESS),
                                 ("g.window.js", wpt.TESTHARNESS),
                                 ("charset/e.html", wpt.TESTHARNESS),
                                 ("crashtests/d.html", wpt.CRASHTEST)])

    def test_a_test_runs_once_for_each_variant_it_names(self):
        harness = '<script src="/resources/testharness.js"></script>'
        pages = {
            "a.html": harness
            + '<meta name="variant" content="?x=1"><meta content="#y" name=variant>',
            "b.any.js": "// META: variant=?one\n// META: variant=?two\ntest(() => {});",
            "c.html": harness,
        }
        with tempfile.TemporaryDirectory() as root:
            for name, text in pages.items():
                with open(os.path.join(root, name), "w", encoding="utf-8", newline="\n") as page:
                    page.write(text)
            found = [name for _, name, _ in wpt.discover(root, [""])]
        self.assertEqual(found,
                         ["a.html?x=1", "a.html#y", "b.any.js?one", "b.any.js?two", "c.html"])
        self.assertEqual(wpt.page_of("a.html?x=1"), "a.html")
        self.assertEqual(wpt.module_of("css/css-a/a.html?x=a/b"),
                         "css/css-a")

    def test_a_script_runs_in_the_page_that_the_server_makes_with_the_scripts_its_meta_names(self):
        with tempfile.TemporaryDirectory(dir=wpt.scratch_directory()) as directory:
            with open(os.path.join(directory, "helper.js"), "w", encoding="utf-8", newline="\n") as f:
                f.write("globalThis.helped = true;\n")
            path = os.path.join(directory, "t.any.js")
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write("// META: script=helper.js\ntest(() => assert_true(helped), 'helped');\n")
            result = wpt.run(wpt.SHELL, wpt.Test(path))
        self.assertIsNone(result.harness)
        self.assertEqual([(status, name) for status, name, _ in result.results], [("PASS", "helped")])

    def test_a_plain_run_leaves_out_what_waits_on_what_the_engine_lacks(self):
        harness = '<script src="/resources/testharness.js"></script>\n'
        pages = {
            "css/css-a/parsing/a-valid.html": harness
            + "<script>test_valid_value('a', 'b')</script>",
            "css/css-a/layout.html": harness + "<script>target.offsetWidth</script>",
            "css/css-a/input.html": harness + '<script src="/resources/testdriver.js"></script>',
            "css/css-a/manual.html": harness + '<meta name="flags" content="interact">',
            "css/css-a/events.html": harness + '<script src="support/helper.js"></script>',
            "css/css-a/support/helper.js": "target.animate([], 1000).finished.then(done)",
            "css/css-a/b.html": '<link rel="match" href="b-ref.html">',
            "dom/x/input.html": harness + '<script src="/resources/testdriver.js"></script>',
        }
        with tempfile.TemporaryDirectory() as root:
            for name, text in pages.items():
                path = os.path.join(root, name)
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "w", encoding="utf-8", newline="\n") as page:
                    page.write(text)
            with patch.object(wpt, "unbuilt", return_value=None):
                default, left = wpt.select(root, ["css"])
                explicit, _ = wpt.select(root, ["css"], match="css/css-a/input.html")
                other, _ = wpt.select(root, ["dom"])
        self.assertEqual([name for _, name, _ in default],
                         ["css/css-a/b.html",
                          "css/css-a/events.html",
                          "css/css-a/input.html",
                          "css/css-a/layout.html",
                          "css/css-a/parsing/a-valid.html"])
        self.assertEqual(left, {"manual": 1})
        self.assertEqual([name for _, name, _ in explicit],
                         ["css/css-a/input.html"])
        self.assertEqual([name for _, name, _ in other], ["dom/x/input.html"])

    def test_module_grouping_and_failure_summary(self):
        names = ["css/" + name
                 for name in ["CSS2/colors/a.html", "CSS2/tables/a.html", "css-a/parsing/a.html",
                              "css-a/nested/b.html"]]
        found = [(name, name, wpt.TESTHARNESS) for name in names]
        def result(shell, test):
            test.complete = True
            test.result_indices = [0, 1]
            test.results = [("PASS", "first", ""), ("FAIL", "second", "wrong value")]
            if test.path.endswith("b.html"):
                test.harness = "TIMEOUT"
                test.complete = False
            return test
        output = io.StringIO()
        with patch.object(wpt, "select", return_value=(found, {})), \
             patch.object(wpt, "run", side_effect=result), \
             patch.dict(wpt.ERRATA, {}, clear=True), \
             patch("sys.argv", ["wpt.py"]), redirect_stdout(output):
            wpt.main()
        rows = {}
        for line in output.getvalue().splitlines():
            if line.strip():
                rows.setdefault(line.split()[0], line)
        self.assertIn("css/CSS2/colors", rows)
        self.assertIn("css/CSS2/tables", rows)
        self.assertNotIn("css/CSS2", rows)
        self.assertNotIn("css/css-a/parsing", rows)
        self.assertRegex(rows["css/css-a"], r"1/2\s+2/4\s+50.0%")
        self.assertNotIn("wrong value", output.getvalue())
        output = io.StringIO()
        with patch.object(wpt, "select", return_value=(found, {})), \
             patch.object(wpt, "run", side_effect=result), \
             patch.dict(wpt.ERRATA, {}, clear=True), \
             patch("sys.argv", ["wpt.py", "--first-errors"]), redirect_stdout(output):
            wpt.main()
        counts, errors = output.getvalue().split("First error", 1)
        self.assertNotIn("wrong value", counts)
        self.assertIn("css-a/parsing/a.html: FAIL: wrong value", errors)
        self.assertRegex(rows["total"], r"3/4\s+4/8\s+50.0%")



class ServerTests(unittest.TestCase):
    def test_a_page_of_the_wpt_checkout_loads_from_wpts_server_over_the_network(self):
        with tempfile.TemporaryDirectory(dir=wpt.WPT) as directory:
            root = Path(directory)
            base = "/" + os.path.relpath(directory, wpt.WPT).replace(os.sep, "/")
            (root / "fixtures").mkdir()
            (root / "fixtures" / "values.sub.js").write_text("print('{{GET[name]}}', '{{host}}');")
            page = root / "pages" / "test.html"
            page.parent.mkdir()
            page.write_text("<script>print(location.hostname);</script>"
                            "<script src='" + base + "/fixtures/values.sub.js?name=value'></script>")
            result = subprocess.run([wpt.SHELL, *wpt.network_options(), "--html", wpt.url_of(str(page))],
                                    capture_output=True, encoding="utf-8", timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), ["web-platform.test", "value web-platform.test"])


class RulingsTests(unittest.TestCase):
    def test_the_rulings_rule_once_on_each_feature_that_the_suites_test(self):
        features = web_features.data()
        self.assertIsNotNone(features, "no web-features data; ./dev.sh fetch webfeatures fetches it")
        problems = web_features.check(features, wpt.tested_features(wpt.WPT, features))
        self.assertEqual([], ["%s:%d: %s" % (web_features.RULINGS_FILE, max(line, 1), problem)
                              for line, problem in problems])


class MatchTests(unittest.TestCase):
    def test_a_glob_without_an_index_names_files_or_directories(self):
        self.assertTrue(corpus.selected("css-syntax/a.html:3", "css-syntax"))
        self.assertTrue(corpus.selected("css-syntax/charset/b.html:0", "css-syntax"))
        self.assertTrue(corpus.selected("css-syntax/a.html:3", "css-syntax/a.html"))
        self.assertFalse(corpus.selected("css-syntax-x/a.html:3", "css-syntax"))
        self.assertTrue(corpus.holds("css-syntax/charset/b.html", "css-syntax/charset"))
        self.assertTrue(corpus.selected("template:43", "template:4*"))
        self.assertFalse(corpus.selected("template:53", "template:4*"))


if __name__ == "__main__":
    unittest.main()
