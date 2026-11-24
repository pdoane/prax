#!/usr/bin/env python3
"""Runs esctest against lib/vterm, and records the cases in which the terminal answers differently on purpose.

esctest (Thomas E. Dickey's esctest2, the maintained fork of George Nachman's esctest) tests a terminal's control
sequences from a program that runs on the terminal. It writes sequences and reads the screen back through the
terminal's reports. It reads each cell's character through the checksum of a rectangle (DECRQCRA), and the cursor, the
modes and the window through their own reports. The harness (lib/vterm/tests/vterm_esctest.cpp) runs esctest on a
pseudoterminal and relays between it and a terminal without a window.

esctest runs as it does when it tests xterm, which it takes as the ideal terminal. It expects the reports of a VT520,
which xterm gives with all of its window operations allowed (xtermWinopsEnabled). It expects checksums as xterm's
patch 334 and later compute them with checksumExtension 23, which the harness's terminal uses. Its other options are
those of xterm with UTF-8.

A case is one test method, keyed "<class>/<method>". A part of the scoreboard is a class, which tests one control
sequence. A case passes if its assertions hold. esctest's own record of xterm's bugs decides nothing: a test that xterm
is known to fail passes if the terminal passes it, and a test that the terminal fails fails whatever xterm does. A test
that esctest does not try for xterm (a known bug with shouldTry off) is not counted.

A case counts if it passes or if the errata record it. A recorded case is one in which the terminal follows the
standard and esctest does not. A failing case is listed but does not fail the run. An errata entry fails the run if its
case passes or if it names no case.

Usage: python3 -m tools.suite.esctest [--binary PATH] [--tests DIR] [--match GLOB] [--verbose] [--list] [--json FILE]
                                      [--against FILE]
"""

import json
import os
import re
import subprocess
import sys
import tempfile

from tools.suite import corpus

TESTS = os.path.join(corpus.ROOT, "third_party", "esctest2", "esctest")

# esctest's options, as they describe the terminal under test.
OPTIONS = [
    "--expected-terminal=xterm",
    # The checksums of patch 334 and later, which count a cell that holds nothing as a space.
    "--xterm-checksum=334",
    # The reverse wrap of patch 380 and later, which does not wrap past the screen's top.
    "--xterm-reverse-wrap=380",
    # A VT520, which xterm emulates with decTerminalID 520.
    "--max-vt-level=5",
    "--options", "xtermWinopsEnabled",
    "--no-print-logs",
]

# How long esctest waits for each byte of a report, in seconds. A terminal that answers does so within the relay's
# round trip, so the wait costs time only if a report never comes.
TIMEOUT = 0.5

# How long a run of esctest may take, in seconds.
RUN_SECONDS = 1800

# What a case's log says of its outcome, as esctest writes it (esctest.py, RunTest).
PASSED = "Passed."
FAILED = re.compile(r"^\*\*\* TEST (\S+) FAILED:$")
EXPECTED = "Fails as expected: "
SKIPPED = "Skipped because terminal lacks requisite capability"
# What esctest raises when a test of a known xterm bug passes. Here it means that the test's assertions held.
SHOULD_HAVE_FAILED = "esctypes.InternalError: Should have failed"
NOT_TRYING = " (not trying)"

# A program that lists esctest's tests in the order that esctest runs them, without running them. It imports esctest's
# modules in the same order as esctest.py, because the modules import each other.
LIST = """
import inspect, sys
sys.path.insert(0, sys.argv[1])
import esc, escargs, esccmd, escio, esclog, esctypes, escutil
escargs.args = escargs.parser.parse_args([])
import tests
for test_class in tests.tests:
    for name, _ in inspect.getmembers(test_class(), predicate=inspect.ismethod):
        if name.startswith("test_"):
            print(test_class.__name__ + "." + name)
"""


class Case:
    """One test method of esctest."""

    def __init__(self, name):
        self.name = name
        self.part, _, self.method = name.partition(".")
        self.key = self.part + "/" + self.method


def cases(tests):
    """Returns every test of esctest, by class, as a list of (class, cases) in the order that esctest runs them."""
    names = subprocess.run([sys.executable, "-c", LIST, tests], stdout=subprocess.PIPE, text=True, check=True).stdout
    parts = {}
    for name in names.split():
        case = Case(name)
        parts.setdefault(case.part, []).append(case)
    return list(parts.items())


def outcomes(log):
    """Returns each test's outcome in an esctest log, by name, as (outcome, log lines). The outcome is "pass", "fail" or
    "skip"."""
    found = {}
    name = None
    lines = []

    def close():
        if name is None:
            return
        outcome = "fail"
        text = "\n".join(lines)
        if any(line == PASSED for line in lines):
            outcome = "pass"
        elif any(line.startswith(SKIPPED) for line in lines):
            outcome = "skip"
        elif any(line.startswith(EXPECTED) for line in lines):
            outcome = "skip" if NOT_TRYING in text else "fail"
        elif SHOULD_HAVE_FAILED in text:
            outcome = "pass"
        found[name] = (outcome, lines[:])

    for line in log.splitlines():
        if line.startswith("Run test: "):
            close()
            name = line[len("Run test: "):]
            lines = []
        elif line.startswith("*** ") and not FAILED.match(line):
            # The summary after the last test.
            close()
            name = None
        elif name is not None:
            lines.append(line)
    close()
    return found


def run(binary, tests, names):
    """Runs the esctest tests `names` through the harness, and returns their outcomes. A test that breaks esctest's
    reset can end a run before its last test. The run then starts again from the test after the one that it ended in.
    That test fails, because the log names each test before the test starts."""
    found = {}
    remaining = list(names)
    with tempfile.TemporaryDirectory() as directory:
        log = os.path.join(directory, "esctest.log")
        while remaining:
            include = "^(%s)$" % "|".join(re.escape(name) for name in remaining)
            command = [binary, sys.executable, os.path.join(tests, "esctest.py"), *OPTIONS,
                       "--logfile=" + log, "--timeout=%g" % (TIMEOUT * corpus.SLOWDOWN), "--include=" + include]
            if os.path.exists(log):
                os.remove(log)
            try:
                subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               timeout=RUN_SECONDS * corpus.SLOWDOWN)
            except subprocess.TimeoutExpired:
                pass
            ran = {}
            if os.path.exists(log):
                with open(log, encoding="utf-8", errors="replace") as f:
                    ran = outcomes(f.read())
            found.update(ran)
            left = [name for name in remaining if name not in ran]
            if left == remaining:
                # Nothing ran, so the first test cannot start.
                found[left[0]] = ("fail", ["esctest ended before the test started"])
                left = left[1:]
            remaining = left
    return found


def show(lines, cut):
    """Returns what a failing case tells: the last line of its log that names what esctest raised, cut at `cut` if it
    is not None, and its whole log, which a narrowed run prints, when `cut` is None."""
    # Python's tracebacks mark the expression that raised on a line of its own, which adds nothing to the line above.
    # esctest prefixes the traceback of a known bug of xterm.
    shown = [re.sub(r"^(KNOWN BUG|EXPECTED FAILURE \(MISSING OPTION\)): ", "", line) for line in lines]
    shown = [line for line in shown if line.strip() and line.strip().strip("~^") != ""]
    raised = [line for line in shown if re.match(r"^[\w.]+(Error|Exception|Failure|Test|BadResponse)\b", line)]
    detail = raised[-1] if raised else shown[-1] if shown else ""
    return (detail[:cut] if cut is not None else detail), (shown if cut is None else [])


def main():
    args = corpus.arguments(corpus.binary("vterm_esctest"), TESTS).parse_args()

    if not os.path.exists(os.path.join(args.tests, "esctest.py")):
        sys.exit("no esctest under %s; run ./dev.sh fetch esctest" % args.tests)
    parts = cases(args.tests)
    if args.list:
        return corpus.listing(parts)

    corpus.require(args.binary, "esctest")
    data = corpus.Data("esctest")
    selected = [case for _, part in parts for case in part if data.runs(case.key, args.match)]
    chosen = {case.key for case in selected}
    if not selected:
        sys.exit("no cases matched %s" % args.match)
    found = run(args.binary, args.tests, [case.name for case in selected])

    # esctest's failures are counted, as a scoreboard counts them, and its errata record where esctest is wrong.
    results = corpus.Results(args, data, keys={case.key for _, part in parts for case in part},
                             requires_errata=False)
    results.left = sum(1 for _, part in parts for case in part if corpus.selected(case.key, args.match)) - len(selected)
    cut = corpus.clip(args.match)
    skipped = 0
    for name, part in parts:
        for case in part:
            if case.key not in chosen:
                continue
            outcome, lines = found.get(case.name, ("fail", ["no outcome"]))
            # A case that esctest does not try is not counted.
            if outcome == "skip":
                skipped += 1
                continue
            detail, log = show(lines, cut) if outcome != "pass" else ("", [])
            results.case(case.key, int(outcome == "pass"), detail=detail, lines=log, part=name)
    if skipped:
        print("%d cases not tried" % skipped)
    return results.finish()

if __name__ == "__main__":
    sys.exit(main())
