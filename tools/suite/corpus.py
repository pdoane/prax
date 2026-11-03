"""Code that every conformance runner shares.

A runner reads its own corpus and compares its own result shapes, but the mechanism around that is shared. A case has
a key, and `--match` runs the subset whose keys match. A harness answers a batch of framed cases with one line of JSON
for each case. Every case either agrees or is recorded in the suite's errata, and a result that differs is printed
beside the expected result. The corpus, the comparison and the printing of one case belong to the runner that owns the
corpus. What the engine decides about the cases, and the scores, are the suite's data (Data).
"""

import argparse
import fnmatch
import json
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


def binary(name, build=BUILD):
    """Returns the path of the harness `name` in `build`, the build that the runners use by default."""
    return os.path.join(build, name + EXECUTABLE)

# The help for `--match`, which is the same in every runner.
MATCH_HELP = ("run only the cases whose key matches this glob, as tests16:* or template:43; a glob "
              "without an index runs the files or directories it names, as css-syntax")

# The number of cases that a run not narrowed by `--match` prints under "to look at".
SHOWN = 20


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
    - fuzzy.txt and render.txt, in a suite that compares renders: the tolerances that override a case's own, and the
      fonts and settings that the suite renders with.

    The errata, cut, deferred and skip lists are in the errata's form (read_errata), and a list that would be empty is
    absent. A key of a list names a case, or a file, which covers every case of the file. A plain run leaves out the
    cut, deferred and skipped cases, and a run narrowed by --match runs them as any other."""

    def __init__(self, name):
        self.name = name
        self.directory = os.path.join(SUITES, name)
        self.errata = read_errata(self.path("errata.txt"))
        self.cut = read_errata(self.path("cut.txt"))
        self.deferred = read_errata(self.path("deferred.txt"))
        self.skip = read_errata(self.path("skip.txt"))

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
                                                  ("deferred", self.deferred), ("skip", self.skip))
                for key in sorted(entries) if key not in keys and key not in files]

    def write_scoreboard(self, rows, unit="cases"):
        """Writes the scoreboard: a row for each part of the suite, as (name, passed, count), and their total."""
        rows = list(rows) + [("TOTAL", sum(r[1] for r in rows), sum(r[2] for r in rows))]
        lines = ["# %s scoreboard" % self.name, "",
                 "The %s that a plain run passes, by part of the suite. The run writes this table, and the diff of a"
                 % unit, "change to it is what the change did.", "",
                 "| part | passing |", "| --- | --- |"]
        lines += ["| %s | %d/%d |" % row for row in rows]
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
    ap.add_argument("--verbose", action="store_true", help="print every recorded difference")
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


def outcomes(args, passed):
    """Writes the run's outcomes, `passed`, the number of tests that each case passed by its key, if --json asks. A
    case is one test, unless the runner counts the tests of a file as WPT does. If --against names another run's
    outcomes, prints each case that passes more or fewer tests than there, and how many tests the run gained and lost.
    The two runs are taken to have selected the same cases."""
    if args.json:
        with open(args.json, "w", encoding="utf-8", newline="\n") as f:
            json.dump(passed, f, indent=1, sort_keys=True)
            f.write("\n")
    if not args.against:
        return
    with open(args.against, encoding="utf-8") as f:
        base = json.load(f)
    changed = []
    for key in sorted(set(base) | set(passed)):
        was, now = base.get(key), passed.get(key)
        if was != now:
            changed.append((key, was, now))
    lost = [c for c in changed if (c[2] or 0) < (c[1] or 0)]
    gained = [c for c in changed if (c[2] or 0) > (c[1] or 0)]
    moved = [c for c in changed if c not in lost and c not in gained]
    print("\nAgainst %s: %d case(s) lost %d test(s), %d case(s) gained %d test(s)"
          % (os.path.relpath(args.against, ROOT), len(lost), sum((w or 0) - (n or 0) for _, w, n in lost),
             len(gained), sum((n or 0) - (w or 0) for _, w, n in gained)))
    for title, rows in (("lost", lost), ("gained", gained), ("moved", moved)):
        for key, was, now in rows:
            print("  %-7s %s  %s -> %s" % (title, key, "-" if was is None else was, "-" if now is None else now))


def compare(files, args, data, width, answer, agrees, show):
    """Runs the cases of `files`, a list of (name, cases), that the run was asked for, with the suite's `data`. Prints
    a row per file and a total, writes the scoreboard if the run is a plain run of the default build, and returns the
    exit status. A case counts if it agrees or the errata record it.

    A case has a `key` and a `label`. A verbose run prints the label beside a recorded case. `answer(name, batch)`
    returns the harness's answer to each case of the batch. `agrees(case, got)` returns true if an answer is right.
    `show(case, why, got, cut)` prints a case to look at, with `got` None if there is no answer worth printing. `width`
    is the width of the key's column in that output.

    Three things need a look, and any of them fails the run: a case that disagrees and has no entry, an entry whose case
    agrees, and an entry that names no case."""
    failures = []
    totals = []
    keys = set()
    passed = {}
    left = 0
    for name, cases in files:
        keys.update(case.key for case in cases)
        batch = [case for case in cases if data.runs(case.key, args.match)]
        left += sum(1 for case in cases if selected(case.key, args.match)) - len(batch)
        if not batch:
            continue

        agreed = 0
        known = 0
        for case, got in zip(batch, answer(name, batch)):
            reason = data.recorded(case.key)
            passed[case.key] = 1 if agrees(case, got) else 0
            if passed[case.key]:
                agreed += 1
                if reason is not None:
                    failures.append((case, "agrees now; drop the entry", None))
            elif reason is None:
                failures.append((case, "differs, and the corpus is not recorded as wrong", got))
            else:
                known += 1
                if args.verbose:
                    print("%s\n    %s" % (" ".join(filter(None, (case.key, case.label))), reason))
        totals.append((name, agreed, known, len(batch)))

    if not totals:
        sys.exit("no cases matched %s" % args.match)

    column = max(len(name) for name, _, _, _ in totals)
    for name, agreed, known, count in totals:
        print("%s  %d/%d%s" % (name.ljust(column), agreed, count,
                               "   (%d recorded)" % known if known else ""))

    agreed = sum(t[1] for t in totals)
    recorded = sum(t[2] for t in totals)
    count = sum(t[3] for t in totals)
    print("\n%d cases, %d agreeing, %d recorded as the corpus getting it wrong%s"
          % (count, agreed, recorded, ", %d left out as cut or deferred" % left if left else ""))
    outcomes(args, passed)
    if writes_scoreboard(args):
        data.write_scoreboard([(name, a + k, c) for name, a, k, c in totals])

    # A narrowed run cannot tell a key that names no case from a key whose case it left out.
    stale = [] if args.match else data.stale(keys)
    if not failures and not stale:
        return 0

    print("\n%d to look at:" % (len(failures) + len(stale)))
    for kind, key in stale:
        print("  %s names no case (%s)" % (key.ljust(width), kind))
    cut = clip(args.match)
    for case, why, got in failures if args.match else failures[:SHOWN]:
        show(case, why, got, cut)
    return 1
