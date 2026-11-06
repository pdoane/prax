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

import argparse
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


def show(case, lines, cut):
    """Prints a failing case. With `cut`, prints one line with what esctest raised, cut to that length. Otherwise
    prints the case's whole log."""
    # Python's tracebacks mark the expression that raised on a line of its own, which adds nothing to the line above.
    # esctest prefixes the traceback of a known bug of xterm.
    shown = [re.sub(r"^(KNOWN BUG|EXPECTED FAILURE \(MISSING OPTION\)): ", "", line) for line in lines]
    shown = [line for line in shown if line.strip() and line.strip().strip("~^") != ""]
    if cut is not None:
        raised = [line for line in shown if re.match(r"^[\w.]+(Error|Exception|Failure|Test|BadResponse)\b", line)]
        print("  %s  %s" % (case.key, (raised[-1] if raised else shown[-1] if shown else "")[:cut]))
        return
    print("  " + case.key)
    for line in shown:
        print("      " + line)


def main():
    ap = argparse.ArgumentParser()
    corpus.outcome_arguments(ap)
    ap.add_argument("--binary", default=corpus.binary("vterm_esctest"), help="the harness the cases run through")
    ap.add_argument("--tests", default=TESTS, help="esctest's directory")
    ap.add_argument("--match", metavar="GLOB",
                    help="run only the cases whose key matches this glob, as CUPTests/test_CUP_*; a glob that names a "
                         "class runs its cases, as CUPTests")
    ap.add_argument("--verbose", action="store_true", help="print every recorded difference")
    ap.add_argument("--list", action="store_true", help="count the corpus and run nothing")
    args = ap.parse_args()

    if not os.path.exists(os.path.join(args.tests, "esctest.py")):
        sys.exit("no esctest under %s; run ./dev.sh fetch esctest" % args.tests)
    parts = cases(args.tests)
    if args.list:
        return corpus.listing(parts)

    corpus.require(args.binary, "esctest")
    data = corpus.Data("esctest")
    keys = {case.key for _, part in parts for case in part}
    selected = [case for _, part in parts for case in part if data.runs(case.key, args.match)]
    chosen = {case.key for case in selected}
    left = sum(1 for _, part in parts for case in part if corpus.selected(case.key, args.match)) - len(selected)
    if not selected:
        sys.exit("no cases matched %s" % args.match)
    found = run(args.binary, args.tests, [case.name for case in selected])

    rows = []
    passed = {}
    failing = []
    problems = []
    skipped = 0
    for name, part in parts:
        agreed = count = known = 0
        for case in part:
            if case.key not in chosen:
                continue
            outcome, lines = found.get(case.name, ("fail", ["no outcome"]))
            if outcome == "skip":
                skipped += 1
                continue
            count += 1
            reason = data.recorded(case.key)
            passed[case.key] = 1 if outcome == "pass" else 0
            if outcome == "pass":
                agreed += 1
                if reason is not None:
                    problems.append("%s passes; drop its entry from the errata" % case.key)
            elif reason is not None:
                known += 1
                if args.verbose:
                    print("%s\n    %s" % (case.key, reason))
            else:
                failing.append((case, lines))
        if count:
            rows.append((name, agreed, known, count))

    corpus.report_parts(args, data, rows)
    agreed = sum(row[1] for row in rows)
    recorded = sum(row[2] for row in rows)
    count = sum(row[3] for row in rows)
    print("\n%d cases, %d passing, %d recorded as esctest getting it wrong, %d failing%s%s"
          % (count, agreed, recorded, len(failing), ", %d not tried" % skipped if skipped else "",
             ", %d left out as cut, deferred or skipped" % left if left else ""))
    corpus.outcomes(args, passed)

    if failing:
        print("\nfailing:")
        cut = corpus.clip(args.match)
        for case, lines in failing if args.match else failing[:corpus.SHOWN]:
            show(case, lines, cut)
        if not args.match and len(failing) > corpus.SHOWN:
            print("  ... and %d more; --match narrows the run and prints each failure's log" %
                  (len(failing) - corpus.SHOWN))

    stale = [] if args.match else data.stale(keys)
    problems += ["%s names no case (%s)" % (key, kind) for kind, key in stale]
    if not problems:
        return 0
    print("\n%d to look at:" % len(problems))
    for problem in problems:
        print("  " + problem)
    return 1


if __name__ == "__main__":
    sys.exit(main())
