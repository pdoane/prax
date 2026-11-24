"""Code that every conformance runner shares.

A runner reads its own corpus and compares its own result shapes, but the mechanism around that is shared. A case has
a key, and `--match` runs the subset whose keys match. A harness answers a batch of framed cases with one line of JSON
for each case. The corpus, the comparison and the lines that tell how a case differs belong to the runner that owns the
corpus. Everything else that a run reports, the same in every suite, belongs to its Results: each case's line, the
errata's account, the rows and the scoreboard, the outcomes that --json writes and --against compares, and what is to
be looked at. What the engine decides about the cases, and the scores, are the suite's data (Data).
"""

import argparse
import concurrent.futures
import fnmatch
import json
import math
import os
import subprocess
import sys

import tools

ROOT = tools.ROOT

# The build that the harnesses come from: the default build, unless PR_BUILD names another, as dev.sh does for Debug and
# the sanitizer build. PR_SLOWDOWN is how many times longer that build's harnesses take than the default build's, and
# scales a runner's deadlines.
DEFAULT = os.path.join(ROOT, "build")
BUILD = os.environ.get("PR_BUILD", DEFAULT)
SLOWDOWN = float(os.environ.get("PR_SLOWDOWN", "1"))


# The suffix of an executable's file name.
EXECUTABLE = ".exe" if os.name == "nt" else ""


def shown_path(path):
    """Returns `path` as a run shows it: relative to the checkout, or absolute where no relative path reaches it, as
    on another drive."""
    try:
        return os.path.relpath(path, ROOT)
    except ValueError:
        return os.path.abspath(path)


def binary(name, build=BUILD):
    """Returns the path of the harness `name` in `build`, the build that the runners use by default."""
    return os.path.join(build, name + EXECUTABLE)

# The help for `--match`, which is the same in every runner.
MATCH_HELP = ("run only the cases whose key matches this glob, as tests16:* or template:43; a glob "
              "without an index runs the files or directories it names, as css-syntax")

# The number of cases that a run not narrowed by `--match` prints under "to look at".
SHOWN = 20


def read_fuzzy_overrides(path):
    """Returns the fuzzy overrides that a file records, for each case by key, or none if there is no file. An entry is
    its overrides, one per line, each a case's key and a fuzzy meta's content (web-platform-tests docs, "Fuzzy
    Matching"), then its reason on indented lines. The entries are sorted by their first line, and a case has one
    override for each reference that it keys, as metas do."""
    overrides = {}
    if not os.path.exists(path):
        return overrides
    heads = []
    pending = 0
    reasoned = False
    with open(path, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            line = line.rstrip()
            if not line or line.startswith("#"):
                continue
            if line[0].isspace():
                if pending == 0:
                    sys.exit("%s:%d: a sentence with no override before it" % (path, number))
                reasoned = True
                continue
            if reasoned:
                pending = 0
                reasoned = False
            if pending == 0:
                if heads and line < heads[-1]:
                    sys.exit("%s:%d: an entry out of order; they are sorted by their first line" % (path, number))
                heads.append(line)
            pending += 1
            fields = line.split()
            if len(fields) != 2:
                sys.exit("%s:%d: an override is a test and a fuzzy meta's content" % (path, number))
            overrides.setdefault(fields[0], []).append(fields[1])
    if pending and not reasoned:
        sys.exit("%s: an override with no sentence after it" % path)
    return overrides


def fuzzy_content_ranges(content):
    """Returns the ranges that a fuzzy meta's content gives, without any reference's URL that keys it, as
    ((lowest, highest) largest channel difference, (fewest, most) differing pixels). A range's values may be named, or
    given in that order."""
    named = {}
    positional = []
    for part in content.split(";"):
        name, _, value = part.rpartition("=")
        low, _, high = value.strip().partition("-")
        bounds = (int(low), int(high or low))
        if name.strip():
            named[name.strip()] = bounds
        else:
            positional.append(bounds)
    ranges = [named[n] if n in named else positional.pop(0) for n in ("maxDifference", "totalPixels")]
    return (ranges[0], ranges[1])


def fuzzy_shift(content):
    """Returns how far, in pixels, an override's content lets the drawing move before a pixel differs: its `shift`,
    which is 0 or 1 and 0 if absent. An override with a shift is judged by the harness's shifted measure, which compares
    each pixel with the other image's pixels within that distance. WPT's metas have no shift."""
    for part in content.split(";"):
        name, _, value = part.partition("=")
        if name.strip() == "shift":
            shift = int(value)
            if shift not in (0, 1):
                sys.exit("a fuzzy override's shift is 0 or 1, not %d" % shift)
            return shift
    return 0


def within_fuzzy(allowed, difference, pixels):
    """Returns true if a reference's difference from the test is within the fuzzy ranges `allowed`, as wptrunner judges
    it: no difference at all, a range that starts at zero for a measure that is zero, or both measures in range."""
    (low, high), (fewest, most) = allowed
    if allowed == ((0, 0), (0, 0)):
        return difference == 0 and pixels == 0
    return ((pixels == 0 and fewest == 0) or (difference == 0 and low == 0)
            or (low <= difference <= high and fewest <= pixels <= most))


def read_errata(path):
    """Returns the cases that an errata file records, each key with the sentence that settles it, or none if there is no
    file. An entry is its keys, one per line at the start of the line, then its sentence on indented lines. A key is a
    file, or a file and its indices, as in `page.html:0,3-5`. The entries are sorted by their first line, so that each
    case has one place, and none is recorded twice."""
    errata = {}
    if not os.path.exists(path):
        return errata
    keys = []
    reason = []
    heads = []

    def close():
        for key in keys:
            if key in errata:
                sys.exit("%s: %s is recorded twice" % (path, key))
            errata[key] = " ".join(reason)
        keys.clear()
        reason.clear()

    with open(path, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            line = line.rstrip()
            if not line or line.startswith("#"):
                continue
            if line[0].isspace():
                if not keys:
                    sys.exit("%s:%d: a sentence with no case before it" % (path, number))
                reason.append(line.strip())
                continue
            if reason:
                close()
            if not keys:
                if heads and line < heads[-1]:
                    sys.exit("%s:%d: an entry out of order; they are sorted by their first line"
                             % (path, number))
                heads.append(line)
            name, _, indices = line.partition(":")
            if not indices:
                keys.append(name)
                continue
            for part in indices.split(","):
                first, _, last = part.partition("-")
                keys += ["%s:%d" % (name, index)
                         for index in range(int(first), int(last or first) + 1)]
    if keys and not reason:
        sys.exit("%s: a case with no sentence after it" % path)
    close()
    return errata


# The suites' data, a directory for each suite.
SUITES = os.path.join(ROOT, "suites")

# The scoreboard that a plain run writes into a suite's data.
SCOREBOARD = "scoreboard.md"
# The row of a scoreboard's table that covers the run as a whole.
TOTAL = "total"


def pass_rate(passed, count):
    """Returns `passed` as a percentage of `count`, rounded down to a tenth, as a scoreboard records it. A part with
    nothing to pass passes in full."""
    return math.floor(100.0 * passed / count * 10) / 10 if count else 100.0


def table(header, rows):
    """Returns the lines of a scoreboard table with `header` and `rows`, which are tuples of cells as text. The cells
    are padded so that the columns line up: the first column to the left, as names, and the others to the right, as
    counts."""
    cells = [header] + list(rows)
    widths = [max(len(row[i]) for row in cells) for i in range(len(header))]
    lines = []
    for i, row in enumerate(cells):
        lines.append("| %s | %s |" % (row[0].ljust(widths[0]),
                                      " | ".join(row[j].rjust(widths[j]) for j in range(1, len(header)))))
        if i == 0:
            lines.append("|%s|%s|" % ("-" * (widths[0] + 2),
                                      "|".join("-" * (widths[j] + 1) + ":" for j in range(1, len(header)))))
    return lines


def file_of(key):
    """Returns the file that a case's key names: the key without its index."""
    return key.rsplit(":", 1)[0]


class Data:
    """A suite's data, in suites/<name>: what the engine decides about the suite's cases, and the suite's scores.

    - errata.txt: the cases in which the engine is right and the reference is wrong, each with the reason.
    - cut.txt: the cases that test what the engine has decided not to build. The reason starts with what is cut.
    - deferred.txt: the cases that wait on what the engine will build. The reason starts with what they need.
    - skip.txt: the cases that we choose not to run. A case may cost more than it tests, such as a benchmark of a JIT
      that takes the interpreter past the deadline, or its result may vary for a reason that the engine does not
      control. The reason says which.
    - scoreboard.md: what a plain run of the default build passes. The run writes it, and its diff is what a change did.
    - fuzzy.txt and render.txt, in a suite that compares renders: the fuzzy ranges that judge a case in place of the
      suite's own limits, each with why the engine's render differs from the reference and why that is not an error
      (read_fuzzy_overrides), and the fonts and settings that the suite renders with.
    - timing.txt: the cases that we expect to pass but whose result depends on timing, each with why. A run that takes
      such a case's result on more than one try counts the best of them (Results.case), where a case that is not listed
      and whose tries differ fails the run.

    The errata, cut, deferred, skip and timing lists are in the errata's form (read_errata), and a list that would be
    empty is absent. A key of a list names a case, or a file, which covers every case of the file. A plain run leaves
    out the cut, deferred and skipped cases, and a run narrowed by --match runs them as any other."""

    def __init__(self, name):
        self.name = name
        self.directory = os.path.join(SUITES, name)
        self.errata = read_errata(self.path("errata.txt"))
        self.cut = read_errata(self.path("cut.txt"))
        self.deferred = read_errata(self.path("deferred.txt"))
        self.skip = read_errata(self.path("skip.txt"))
        self.timing = read_errata(self.path("timing.txt"))
        self.fuzzy = read_fuzzy_overrides(self.path("fuzzy.txt"))

    def path(self, name):
        """Returns the path of one of the suite's files."""
        return os.path.join(self.directory, name)

    def optional(self, name):
        """Returns the path of one of the suite's files, or None if the suite has no such file."""
        path = self.path(name)
        return path if os.path.exists(path) else None

    def recorded(self, key):
        """Returns the errata's reason for the case, or None if the errata do not record it."""
        return self.errata.get(key, self.errata.get(file_of(key)))

    def left_out(self, key):
        """Returns why a plain run leaves the case out, as ("cut", "deferred" or "skip", reason), or None if it runs."""
        for kind, entries in (("cut", self.cut), ("deferred", self.deferred), ("skip", self.skip)):
            reason = entries.get(key, entries.get(file_of(key)))
            if reason is not None:
                return kind, reason
        return None

    def runs(self, key, match):
        """Returns true if a run narrowed by `match`, or a plain run if it is None, runs the case."""
        return selected(key, match) and (match is not None or self.left_out(key) is None)

    def stale(self, keys):
        """Returns each entry of the lists that names neither a case of `keys` nor the file of one, with its list."""
        files = {file_of(key) for key in keys}
        return [(name, key) for name, entries in (("errata", self.errata), ("cut", self.cut),
                                                  ("deferred", self.deferred), ("skip", self.skip),
                                                  ("timing", self.timing),
                                                  ("fuzzy", self.fuzzy))
                for key in sorted(entries) if key not in keys and key not in files]

    def write_scoreboard(self, rows, notes=()):
        """Writes the scoreboard: a row for each part of the suite, from (name, passed, count), and their total, each
        with its pass rate. `notes` are lines that follow the table, for what the run counts apart from its parts."""
        rows = list(rows)
        total = (TOTAL, sum(r[1] for r in rows), sum(r[2] for r in rows))
        cells = [(name, "%d/%d" % (passed, count), "%.1f%%" % pass_rate(passed, count))
                 for name, passed, count in rows]
        name, passed, count = total
        cells.append(("**%s**" % name, "%d/%d" % (passed, count), "%.1f%%" % pass_rate(passed, count)))
        lines = ["# %s scoreboard" % self.name, "",
                 "The cases that a plain run passes, by part of the suite, and the rate. The run writes this table, and",
                 "the diff of a change to it is what the change did.", ""]
        lines += table(("Part", "Cases", "Passing"), cells)
        if notes:
            lines += ["", *notes]
        os.makedirs(self.directory, exist_ok=True)
        with open(self.path(SCOREBOARD), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")


def writes_scoreboard(args):
    """Returns true if the run is a plain run of the default build, which writes the suite's scoreboard."""
    return args.match is None and not text_backends(args) and BUILD == DEFAULT


def outcome_arguments(ap):
    """Adds the options of a run's outcomes, which every runner takes: writing them, and comparing them with another
    run's (outcomes)."""
    ap.add_argument("--json", metavar="FILE", help="write each case's outcome, for another run's --against")
    ap.add_argument("--against", metavar="FILE",
                    help="list each case that passes more or fewer tests than in another run's --json")


def arguments(binary, tests):
    """Returns a parser for the options that every runner takes, with the harness and the corpus that it reads by
    default."""
    ap = argparse.ArgumentParser()
    outcome_arguments(ap)
    ap.add_argument("--binary", default=binary, help="the harness the cases run through")
    ap.add_argument("--tests", default=tests, help="the corpus")
    ap.add_argument("--match", metavar="GLOB", help=MATCH_HELP)
    ap.add_argument("--verbose", action="store_true", help="print every case, and each entry that excuses one")
    ap.add_argument("--failures", action="store_true", help="print every case that fails, and how")
    ap.add_argument("--list", action="store_true", help="count the corpus and run nothing")
    ap.add_argument("--shaper", choices=["default", "harfbuzz", "kb", "coretext", "simple"],
                    help="the shaper the harness sets text with, lib/text's default unless named; a run with one "
                         "leaves the scoreboard alone")
    ap.add_argument("--hinting", choices=["none", "light", "full"],
                    help="how the harness hints fonts with lib/font's default hinter, not at all unless named; a run "
                         "with one leaves the scoreboard alone")
    return ap


def text_backends(args):
    """Returns the options that hand a harness the run's text backends (pr/text.h's shapers and pr/font.h's hintings),
    if it names any."""
    options = []
    for name in ("shaper", "hinting"):
        if getattr(args, name, None):
            options += ["--" + name, getattr(args, name)]
    return options


def names(name, pattern):
    """Returns true if `pattern` matches the file `name` or a directory that holds it."""
    parts = name.replace(os.sep, "/").split("/")
    return any(fnmatch.fnmatch("/".join(parts[:i]), pattern) for i in range(1, len(parts) + 1))


def selected(key, match):
    """Returns true if the run was asked for `key`. No pattern selects every case, and a pattern without an index
    selects every case in the files that it names."""
    if match is None:
        return True
    if ":" not in match:
        return names(key.rsplit(":", 1)[0], match)
    return fnmatch.fnmatch(key, match)


def holds(name, match):
    """Returns true if the file `name` can hold a case that the run was asked for: if the part of the pattern before
    the index names the file or a directory that holds it."""
    return match is None or names(name, match.split(":", 1)[0])


def clip(match):
    """Returns the length at which a printed answer is cut, or None. A run narrowed to a few cases prints them whole,
    because reading them is the reason for narrowing it."""
    return None if match else 160


def require(binary, suite):
    """Stops the run if the harness is not built."""
    if not os.path.exists(binary):
        sys.exit("no harness at %s; run ./dev.sh suite %s" % (binary, suite))


def listing(files, detail=None):
    """Prints the output of `--list`: the number of cases in each of `files`, a list of (name, cases), and their total.
    `detail` adds the runner's own counts to a file's line."""
    width = max(len(name) for name, _ in files)
    for name, cases in files:
        extra = ", " + detail(cases) if detail else ""
        print("%s  %5d cases%s" % (name.ljust(width), len(cases), extra))
    print("\n%d cases" % sum(len(cases) for _, cases in files))
    return 0


def frame(data, *fields):
    """Returns one case as a harness reads it: the byte count and `fields` on one line, then the bytes."""
    return b" ".join((b"%d" % len(data),) + fields) + b"\n" + data


def answers(command, framed, count):
    """Returns the harness's answer to each of the `count` cases in `framed`, as parsed JSON."""
    done = subprocess.run(command, input=framed, stdout=subprocess.PIPE)
    if done.returncode != 0:
        sys.exit("%s exited %d" % (" ".join(command), done.returncode))

    lines = done.stdout.decode("utf-8", "replace").splitlines()
    if len(lines) != count:
        sys.exit("%s answered %d of %d cases" % (" ".join(command), len(lines), count))
    return [json.loads(line) for line in lines]


def outcomes(args, passed, statuses=None):
    """Writes the run's outcomes, if --json asks: `passed`, the number of tests that each case passed by its key, and
    `statuses`, if the runner gives them, how each case ended, by its key, such as OK, or TIMEOUT for a file whose
    harness never finished. A case is one test, unless the runner counts the tests of a file as WPT does. Each case is
    written as its number, or as {"passed": number, "status": status} where it has a status. If --against names
    another run's outcomes, prints each case that passes more or fewer tests than there, and how many tests the run
    gained and lost, then each case whose status changed, which a case can do without its passes changing. The two runs
    are taken to have selected the same cases."""
    statuses = statuses or {}
    if args.json:
        written = {key: {"passed": count, "status": statuses[key]} if key in statuses else count
                   for key, count in passed.items()}
        with open(args.json, "w", encoding="utf-8", newline="\n") as f:
            json.dump(written, f, indent=1, sort_keys=True)
            f.write("\n")
    if not args.against:
        return
    with open(args.against, encoding="utf-8") as f:
        base = json.load(f)
    base_passed = {key: value["passed"] if isinstance(value, dict) else value for key, value in base.items()}
    base_statuses = {key: value["status"] for key, value in base.items() if isinstance(value, dict)}
    changed = []
    for key in sorted(set(base_passed) | set(passed)):
        was, now = base_passed.get(key), passed.get(key)
        if was != now:
            changed.append((key, was, now))
    lost = [c for c in changed if (c[2] or 0) < (c[1] or 0)]
    gained = [c for c in changed if (c[2] or 0) > (c[1] or 0)]
    moved = [c for c in changed if c not in lost and c not in gained]
    # A base that recorded no statuses, as a runner from before statuses writes, compares by passes alone.
    restatused = [(key, base_statuses[key], statuses[key]) for key in sorted(set(base_statuses) & set(statuses))
                  if base_statuses[key] != statuses[key]]
    print("\nAgainst %s: %d case(s) lost %d test(s), %d case(s) gained %d test(s), %d case(s) changed status"
          % (shown_path(args.against), len(lost), sum((w or 0) - (n or 0) for _, w, n in lost),
             len(gained), sum((n or 0) - (w or 0) for _, w, n in gained), len(restatused)))
    for title, rows in (("lost", lost), ("gained", gained), ("moved", moved), ("status", restatused)):
        for key, was, now in rows:
            print("  %-7s %s  %s -> %s" % (title, key, "-" if was is None else was, "-" if now is None else now))


class Check:
    """A case that a harness checks by the words that it is given, and answers with a line of JSON, {"ok": <bool>,
    "detail": <text>}, whose detail says what went wrong when the check fails (check_cases)."""

    def __init__(self, key, words):
        self.key = key
        self.label = ""
        self.words = list(words)


def check_cases(args, data, parts, batch=1):
    """Runs the checks of `parts`, a list of (name, checks), through the harness `args.binary`, `batch` checks to a run
    of it and as many runs at once as the machine has cores, and returns the run's exit status (compare)."""
    width = max(len(case.key) for _, cases in parts for case in cases)

    def answer(_, chosen):
        def run(start):
            some = chosen[start:start + batch]
            return answers([args.binary] + [word for case in some for word in case.words], b"", len(some))

        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
            return [got for part in pool.map(run, range(0, len(chosen), batch)) for got in part]

    def show(_, got, cut):
        detail = got["detail"] if got else ""
        if cut is not None and len(detail) > cut:
            detail = detail[:cut] + "..."
        return [detail] if detail else []

    return compare(parts, args, data, width, answer, lambda case, got: got["ok"], show)


# What starts each line that a harness writes on stderr to say that the system's time decided some of a run, as a
# deadline on the system's clock does (tools/shell/page_host.cpp). A runner runs such a case again, with fewer at once,
# and its Results compare the tries (Results.case).
TIMING = "timing: "


def timing_of(stderr):
    """Returns what a harness's stderr says of the system's time deciding the run, a line for each time, without the
    prefix (TIMING)."""
    return [line[len(TIMING):] for line in (stderr or "").splitlines() if line.startswith(TIMING)]


# How a case's run ended, whatever it passed: it ran to its end, which is OK, or the reason that it did not, in a word,
# as TIMEOUT, CRASH or ERROR, as the runner's harness tells it.
OK = "OK"


def case_line(key, passed, count, status=OK, detail="", width=0):
    """Returns the line that reports a case, the same in every suite: its key, the tests that it passed of its tests,
    and how it ended, followed by what went wrong. A case that ran to its end is PASS, or FAIL if it failed a test, and
    any other case is its status."""
    word = status if status != OK else "FAIL" if passed < count else "PASS"
    return ("%s  %3d/%-3d  %s%s" % (key.ljust(width), passed, count, word, ": " + detail if detail else "")).rstrip()


class Results:
    """The cases of a run, as every runner reports them. The runner owns its corpus, its harness and what agreeing
    means, and gives each case to the run's results (case): its key, the tests that it passed of its tests, which are
    one unless the runner counts a file's tests as WPT does, its status, and what went wrong, in a line and in lines
    below it. The results keep the rest, the same for every suite:

    - The errata's account. An entry for a case, or for its file, excuses each test that the case fails, and the tests
      that pass or are excused count. A case that passes despite its entry, and an entry that names no case of `keys`,
      are to be looked at. In a suite that `requires_errata`, a case that fails without an entry is too; a suite whose
      errata record only where the corpus is wrong, such as WPT, counts its failures in its scoreboard instead.
    - The output. --verbose prints every case's line, and --failures, or --match, every failing case's line with its
      lines below. Each failing case to look at is listed at the end, the first SHOWN unless the run is narrowed.
    - The run's rows, by part of the suite, and its scoreboard, which a plain run of the default build writes.
    - The run's outcomes, each case's passes and status, which --json writes and --against compares (outcomes).

    The run fails if anything is to be looked at."""

    def __init__(self, args, data, keys=None, requires_errata=True, width=0, out=print):
        self.out = out           # prints a case's lines, as a runner that shows its progress prints around it
        self.args = args
        self.data = data
        self.keys = keys
        self.requires_errata = requires_errata
        self.width = width
        self.passed = {}
        self.statuses = {}
        self.parts = {}          # by part, in the order first met: [tests that count, tests, cases recorded]
        self.failing = []        # (line, lines) of each case that fails without an entry, in a suite that requires one
        self.problems = []       # what else is to be looked at
        self.left = 0            # the cases that the run was asked for and leaves out, as cut, deferred or skipped
        self.failed = 0          # the cases that failed a test or did not run to their end
        self.shown = bool(args.verbose or getattr(args, "failures", False) or args.match)

    def case(self, key, passed, count=1, status=OK, detail="", lines=(), part="", excused=None, label="", tries=()):
        """Records a case and returns the tests of it that count: those that it passed and those whose failure the
        errata excuse. An entry for the case or its file excuses every test that it fails, unless `excused` gives the
        number, for a runner whose entries excuse some of a case's tests, as WPT's entries for subtests do; that runner
        looks at its own entries (look).

        `tries` are the outcomes, each (passed, count, status), of the case's runs after the first, which a runner makes
        when timing may have decided the first (timing). If they differ from the first, a case that timing.txt lists
        counts the best of them, and any other case is to be looked at, since its result is not the engine's alone."""
        if tries and any(tried != (passed, count, status) for tried in tries):
            outcomes = [(passed, count, status)] + list(tries)
            if self.data.timing.get(key, self.data.timing.get(file_of(key))) is not None:
                passed, count, status = max(outcomes, key=lambda o: (o[2] == OK, o[0] / o[1] if o[1] else 1.0))
            else:
                self.problems.append("%s differs between runs (%s); fix it, or list it in timing.txt with why"
                                     % (key, ", ".join("%d/%d %s" % outcome for outcome in outcomes)))
        failed = passed < count or status != OK
        self.failed += 1 if failed else 0
        reason = None
        if excused is None:
            reason = self.data.recorded(key)
            excused = count - passed if failed and reason is not None else 0
            if reason is not None and not failed:
                self.problems.append("%s agrees now; drop its entry" % key)
        self.passed[key] = passed
        self.statuses[key] = status
        tally = self.parts.setdefault(part, [0, 0, 0])
        tally[0] += passed + excused
        tally[1] += count
        tally[2] += 1 if excused else 0
        line = case_line(key, passed, count, status, detail, self.width)
        if self.args.verbose or (failed and self.shown):
            self.out(line)
            for text in ([label] if label and self.args.verbose else []) + list(lines):
                self.out("    " + text)
            if reason is not None and excused and self.args.verbose:
                self.out("    recorded: " + reason)
        if failed and not excused and self.requires_errata:
            self.failing.append((line, list(lines)))
        return passed + excused

    def look(self, text):
        """Adds something to be looked at, which fails the run."""
        self.problems.append(text)

    def finish(self, rows=None, notes=(), table=True):
        """Ends the run: prints its rows and a summary, writes its scoreboard and its outcomes, and lists what is to be
        looked at. `rows`, a list of (part, tests that count, tests), replaces the rows of the parts that the cases
        named, and `notes` follow them on the scoreboard. A runner with a table of its own passes `table` false, and
        prints and records its rows itself. Returns the run's exit status."""
        args = self.args
        if table:
            if rows is None:
                rows = [(name, counted, count) for name, (counted, count, _) in self.parts.items()]
            column = max(len(name) for name, _, _ in rows) if rows else 0
            counts = ["%d/%d" % (counted, count) for _, counted, count in rows]
            width = max(len(text) for text in counts) if counts else 0
            for (name, counted, count), text in zip(rows, counts):
                recorded = self.parts.get(name, [0, 0, 0])[2]
                print("%s  %s  %5.1f%%%s" % (name.ljust(column), text.rjust(width), pass_rate(counted, count),
                                             "   (%d recorded)" % recorded if recorded else ""))
            for note in notes:
                print(note)
            if writes_scoreboard(args):
                self.data.write_scoreboard(rows, notes)
        recorded = sum(tally[2] for tally in self.parts.values())
        print("\n%d cases: %d pass, %d fail and are recorded in the errata, %d fail without an entry%s"
              % (len(self.passed), len(self.passed) - self.failed, recorded, self.failed - recorded,
                 ", %d left out as cut, deferred or skipped" % self.left if self.left else ""))
        outcomes(args, self.passed, self.statuses)
        if self.failing and not self.shown:
            print("\n%d failing without an entry in the errata:" % len(self.failing))
            for line, lines in self.failing[:SHOWN]:
                print("  " + line)
                for text in lines:
                    print("      " + text)
            if len(self.failing) > SHOWN:
                print("  ... and %d more; --failures prints each" % (len(self.failing) - SHOWN))
        # A narrowed run cannot tell an entry that names no case from one whose case it left out.
        if self.keys is not None and not args.match:
            self.problems += ["%s names no case (%s)" % (key, kind) for kind, key in self.data.stale(self.keys)]
        if self.problems:
            print("\n%d to look at:" % len(self.problems))
            for problem in self.problems:
                print("  " + problem)
        return int(bool(self.failing or self.problems))


def compare(files, args, data, width, answer, agrees, show):
    """Runs the cases of `files`, a list of (name, cases), that the run was asked for, with the suite's `data`, and
    returns the run's exit status (Results). A case has a `key` and a `label`, which a verbose run prints.
    `answer(name, batch)` returns the harness's answer to each case of the batch, `agrees(case, got)` returns true if an
    answer is right, and `show(case, got, cut)` returns the lines that tell how a case that does not agree differs,
    each cut at `cut` characters if it is not None. `width` is the width of the key's column."""
    results = Results(args, data, keys={case.key for _, cases in files for case in cases}, width=width)
    cut = clip(args.match)
    for name, cases in files:
        batch = [case for case in cases if data.runs(case.key, args.match)]
        results.left += sum(1 for case in cases if selected(case.key, args.match)) - len(batch)
        if not batch:
            continue
        for case, got in zip(batch, answer(name, batch)):
            ok = agrees(case, got)
            results.case(case.key, int(ok), lines=() if ok else show(case, got, cut), part=name,
                         label=case.label or "")
    if not results.parts:
        sys.exit("no cases matched %s" % args.match)
    return results.finish()
