"""Runs a build's unit tests: the programs that the build lists in tests.json (pr_add_test), several at once.

A test is a test case. Most programs are doctest binaries, each of which runs all of its cases in one process and
reports each case itself, as doctest does when it runs alone. A case that takes longer than its time is named by the
binary's watchdog (tools/harness/test_main.cpp), which ends the binary. The other programs, such as those that a
harness drives (tools/harness/net_peer.py), are a test each.

Each program's output is kept until it ends, and then printed whole: a line for a program that passes, and everything
that it printed for one that fails, so that what programs print at once never interleaves. The programs that took
longest the last time start first, so that none is left to run alone at the end. PR_TEST_TIMES names a directory
that keeps those times past the build, as CI's cache does. A doctest binary times its cases, and the run ends with
the slowest of them, then a line that counts the cases that doctest's summaries count.
"""

import concurrent.futures
import json
import os
import re
import subprocess
import sys
import time

# A program that is not a doctest binary, or one that hangs outside its cases, ends after this many seconds unless its
# test sets its own time.
PROGRAM_TIMEOUT = 1200

DOCTEST_SUMMARY = re.compile(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed\s*\|\s*(\d+) skipped")
# doctest's console writes a block, between lines of `=`, for each case that failed and for each message of a case that
# passed. A block that names an error, a crash or an exception is a failure.
DOCTEST_BLOCK = re.compile(r"^={20,}$", re.MULTILINE)
DOCTEST_CASE = re.compile(r"^TEST CASE:\s+(.+?)\s*$", re.MULTILINE)
DOCTEST_ERROR = re.compile(r"\b(?:ERROR|CRASHED|THREW|exception)\b")
# The line that doctest's --duration writes as each case ends: its seconds and its name.
DOCTEST_DURATION = re.compile(r"^([0-9.]+) s: (.+?)\s*$", re.MULTILINE)
# The number of the slowest cases that a run lists at its end, and the least that a listed case takes, in seconds.
SLOWEST = 12
SLOWEST_SECONDS = 0.1
TIMED_OUT = re.compile(r'^TIMEOUT: the test case "(.+)" ran longer', re.MULTILINE)
UNITTEST_RAN = re.compile(r"^Ran (\d+) tests? in", re.MULTILINE)
UNITTEST_FAILED = re.compile(r"^(?:FAIL|ERROR): (\S+ \(.+?\))", re.MULTILINE)
UNITTEST_SKIPPED = re.compile(r"^(?:OK|FAILED) \(.*?skipped=(\d+)", re.MULTILINE)


class Result:
    """What became of a program: whether it passed, its output, how long it took, its cases (passed, failed, skipped)
    and the names of those that failed. A doctest binary's cases are those that doctest's summaries count, and a case
    that ran out of time is counted from its watchdog's line (tools/harness/test_main.cpp), since it ends the binary
    before the summary. A Python program's cases are unittest's tests. Any other program is a case itself."""

    def __init__(self, test, passed, output, seconds, why=""):
        self.test = test
        self.passed = passed
        self.output = output
        self.seconds = seconds
        self.why = why
        counts = [0, 0, 0]
        failures = []
        for block in DOCTEST_BLOCK.split(output):
            named = DOCTEST_CASE.search(block)
            if named and DOCTEST_ERROR.search(block):
                failures.append(named.group(1))
        failures += TIMED_OUT.findall(output)
        summaries = list(DOCTEST_SUMMARY.finditer(output))
        ran = UNITTEST_RAN.findall(output)
        if summaries:
            for found in summaries:
                for i in range(3):
                    counts[i] += int(found.group(i + 2))
        elif ran:
            failures += UNITTEST_FAILED.findall(output)
            counts[2] = sum(int(n) for n in UNITTEST_SKIPPED.findall(output))
            counts[1] = len(set(UNITTEST_FAILED.findall(output)))
            counts[0] = sum(int(n) for n in ran) - counts[1] - counts[2]
        elif not TIMED_OUT.search(output):
            counts[0 if passed else 1] = 1
        counts[1] += len(TIMED_OUT.findall(output))
        if not passed and not failures:
            failures = ["(%s)" % why]
        # A case that fails in each of its subcases is named once.
        self.failures = list(dict.fromkeys(failures))
        self.cases = counts
        self.durations = [(float(found.group(1)), found.group(2)) for found in DOCTEST_DURATION.finditer(output)]


def run_test(test, filters, case_timeout, slowdown):
    """Runs one test's program, with doctest's `filters` if it is a doctest binary, and returns its Result. The
    program's own limit is `slowdown` times longer, as `case_timeout` already is."""
    # A doctest binary times each case, so that a run can name its slowest.
    command = test["command"] + (filters + ["--duration=true"] if test["kind"] == "doctest" else [])
    environment = dict(os.environ)
    environment.update(test["environment"])
    environment["PR_TEST_CASE_TIMEOUT"] = str(case_timeout)
    # Under a sanitizer, the allocator cannot reserve its address ranges where it asks, and says so on the standard
    # error, which some tests compare. The warning says nothing about the program under test.
    if slowdown > 1:
        environment.setdefault("MIMALLOC_SHOW_ERRORS", "0")
    start = time.monotonic()
    limit = (test["timeout"] or PROGRAM_TIMEOUT) * slowdown
    try:
        done = subprocess.run(command, cwd=test["directory"], env=environment, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=limit)
    except subprocess.TimeoutExpired as expired:
        output = (expired.output or b"").decode("utf-8", "replace")
        return Result(test, False, output, time.monotonic() - start, "it ran longer than %d seconds" % limit)
    except OSError as error:
        return Result(test, False, "", time.monotonic() - start, "it cannot start: %s" % error)
    output = done.stdout.decode("utf-8", "replace")
    seconds = time.monotonic() - start
    if done.returncode != 0:
        return Result(test, False, output, seconds, "it exited %d" % done.returncode)
    if test["pass"] and not re.search(test["pass"], output):
        return Result(test, False, output, seconds, "its output has no match of %r" % test["pass"])
    return Result(test, True, output, seconds)


def load(path, default):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def run(directory, jobs, case_timeout, filters=(), slowdown=1, extra=()):
    """Runs the unit tests of the build in `directory`, with the programs `extra` (as tests.json describes them), `jobs`
    programs at a time, giving each test case `case_timeout` seconds. With doctest's `filters`, such as -tc=<name>, runs
    only the doctest binaries' cases that they choose. A build that runs `slowdown` times slower, as a sanitizer's does,
    has every limit that many times longer. Returns true if every test passed."""
    case_timeout *= slowdown
    tests = load(os.path.join(directory, "tests.json"), None)
    if tests is None:
        raise SystemExit("no tests.json in %s; configure the build" % directory)
    tests += extra
    filters = list(filters)
    if filters:
        tests = [test for test in tests if test["kind"] == "doctest"]
    # The times of the last run, kept in the build or, where PR_TEST_TIMES names a directory that outlives the build as
    # CI's cache does, in a file of the build's own there.
    kept = os.environ.get("PR_TEST_TIMES")
    times_path = (os.path.join(kept, os.path.basename(os.path.normpath(directory)) + ".json") if kept
                  else os.path.join(directory, "test_times.json"))
    times = load(times_path, {})
    tests.sort(key=lambda test: -times.get(test["name"], 0))

    start = time.monotonic()
    results = []
    width = max(len(test["name"]) for test in tests)
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(run_test, test, filters, case_timeout, slowdown) for test in tests]
        for future in concurrent.futures.as_completed(futures):
            result = future.result()
            results.append(result)
            passed, failed, skipped = result.cases
            outcome = "ok    " if result.passed else "FAILED"
            line = "%s  %s %5d cases %7.2fs" % (outcome, result.test["name"].ljust(width), passed + failed,
                                               result.seconds)
            if result.passed:
                print(line, flush=True)
                continue
            print("\n%s  (%s)\n%s\n%s\n" % (line, result.why, "-" * 120, result.output.rstrip()), flush=True)

    for result in results:
        times[result.test["name"]] = round(result.seconds, 2)
    os.makedirs(os.path.dirname(times_path), exist_ok=True)
    with open(times_path, "w", encoding="utf-8") as f:
        json.dump(times, f, indent=1, sort_keys=True)

    # Each failure's whole output, so that a run's failures can be read after the run without running them again.
    failing = sorted((result for result in results if not result.passed), key=lambda result: result.test["name"])
    log_path = os.path.normpath(os.path.join(directory, "tests.log"))
    with open(log_path, "w", encoding="utf-8", newline="\n") as f:
        for result in failing:
            f.write("%s (%s)\n%s\n%s\n\n" % (result.test["name"], result.why, "-" * 120, result.output.rstrip()))
    failed = [result.test["name"] for result in failing]
    totals = [sum(result.cases[i] for result in results) for i in range(3)]
    # doctest counts the cases that the filters leave out as skipped, so a filtered run counts only the cases it ran.
    if filters:
        summary = "%d test cases: %d passed, %d failed" % (totals[0] + totals[1], totals[0], totals[1])
    else:
        summary = "%d test cases: %d passed, %d failed, %d skipped" % (sum(totals), totals[0], totals[1], totals[2])
    # The slowest cases, which are where a run's time goes once no program stands out.
    slowest = sorted(((seconds, result.test["name"], name) for result in results for seconds, name in result.durations
                      if seconds >= SLOWEST_SECONDS), reverse=True)[:SLOWEST]
    if slowest:
        print("\nslowest cases:")
        for seconds, program, name in slowest:
            print("  %6.2fs  %s: %s" % (seconds, program, name))
    # The programs' time added up, beside the run's, shows how much of the run waits on its slowest program.
    print("\n%s, in %d programs and %.0fs (%.0fs of programs)" % (summary, len(results), time.monotonic() - start,
                                                            sum(result.seconds for result in results)))
    if failing:
        print("failed:")
        for result in failing:
            for name in result.failures:
                print("    %s: %s" % (result.test["name"], name))
        print("each failure's output is in %s" % log_path)
    # What runs after this prints to the same stream, and must come after the summary.
    sys.stdout.flush()
    return not failed
