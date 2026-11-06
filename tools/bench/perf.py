"""Runs the benchmarks and reports their measures, or what changed between a base's binaries and the working tree's
(ab). Also runs the benchmarks in other ways: in browsers, and to measure what their cases keep allocated. The results
of each run are written under perf/ab, outside the repository, and the next run replaces them.

Measures are compared on the instructions that one call retired (harnessCounters). Instruction counts barely change
with the machine's load, its clocks or the kind of core that the work ran on, so a run can be compared with another run
made at any time. Where the system counts no instructions, as Windows does not without a driver, measures are compared
on cycles, which move with the clocks and the machine's load, so a comparison there wants more runs a side. Some work
still varies a little from run to run, such as the CPU work around a device's frame, so a results file holds a few runs.
A change is reported if it exceeds the threshold and the spread of the runs on each side. Beside it are shown the cycles
and the time as they were measured, and the share of the instructions retired on performance cores, which explains a
change in time when the instructions did not change. If a device did work, its time is shown too. Rows without counts,
such as the latencies of a paced load, have only times. They are listed with --all, because they vary with the machine.

A results file names the commit that it measured, whether the tree had changes, the power source, and a hash of the
workload: the benchmark's script and sources and the inputs that it read. A change of workload is reported, because a
measure with the same name may no longer measure the same thing.

The benchmarks of real pages read third_party/pages, which `./dev.sh pages roll` and `./dev.sh pages refresh` update.
The pages are inputs, so a comparison across an update reports that the workload changed.

Browsers are compared on time and are not recorded (tools/bench/browser.py).

`./dev.sh bench` runs these (tools/bench/commands.py).
"""

import datetime
import hashlib
import json
import os
import statistics
import signal
import subprocess
import sys
import time

from tools.bench import common
from tools.bench import registry
from tools.bench import scripted
from tools.bench.common import duration, scaled

PERF = common.PERF
AB = os.path.join(PERF, "ab")
RUNNER = "tools.bench.run"
MEASURE = os.path.join(common.HERE, "measure.js")

# The change in instructions, in percent, below which a measure is reported as unchanged.
THRESHOLD = 1.0

# The time limit in seconds for one run of a benchmark's runner. A run that reaches it is stopped and fails. The slowest
# runner takes a fraction of this limit, so a run that reaches it has hung.
DEADLINE = 120


def git(*args):
    return subprocess.run(["git", "-C", common.ROOT] + list(args), capture_output=True, text=True).stdout.strip()


def digest(paths):
    """Returns a hash of the files' contents, in the order given."""
    h = hashlib.sha256()
    for path in paths:
        with open(path, "rb") as f:
            h.update(f.read())
        h.update(b"\0")
    return h.hexdigest()[:16]


def load(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        return None


def record(bench, results, inputs, out):
    """Writes a run of `bench`, with its `results` by engine, to `out`. `inputs` are the files that the run read."""
    if common.failed:
        print("not recorded: the run failed", file=sys.stderr)
        return
    workload = ([bench.script, MEASURE] if bench.scripted else []) + bench.sources + list(inputs)
    meta = {
        "commit": git("rev-parse", "HEAD"),
        "dirty": bool(git("status", "--porcelain", "--untracked-files=no")),
        "date": datetime.datetime.now().isoformat(timespec="seconds"),
        "power": common.power(),
        "workload": digest(workload),
    }
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"bench": bench.name, "meta": meta, "engines": results}, f, indent=1)


# ----------------------------------------------------------------------------------------------------------------------
# Measures


def measures(results):
    """Returns the measures of a run by name. Each measure is the cost of one call, as harnessCounters counts it. A
    row's counts under "time" are the row's own measure, and its counts under another key are the measure
    "<row> <key>". The names of rows from "prax (C)", the C harness that accompanies a shell benchmark, end in
    " (C)". The names of rows from another engine end in that engine's name, such as " (yoga)"."""
    out = {}
    for engine, rows in results["engines"].items():
        suffix = engine_suffix(engine)
        for row in rows:
            for key, counts in row.get("counts", {}).items():
                name = row["name"] if key == "time" else "%s %s" % (row["name"], key)
                out[name + suffix] = counts
    return out


def engine_suffix(engine):
    """Returns the suffix of the name of a measure taken by `engine`."""
    if engine == "prax":
        return ""
    return " (C)" if engine == "prax (C)" else " (%s)" % engine


# Other engines whose rows a benchmark may measure beside the engine's own rows, as the flex benchmark measures Yoga and
# Taffy and the vterm benchmark measures libvterm. The show command compares the engine with them.
OTHERS = ("yoga", "taffy", "libvterm")


def throughput(row):
    """Returns the MiB per second at which a row processed its `bytes`, or "-" if the row has no size or time."""
    if row is None or not row.get("bytes") or not row.get("time"):
        return "-"
    return "%.0f" % (row["bytes"] / (1 << 20) / (row["time"] / 1e3))


def print_engines(results):
    """Prints each row of the engine beside the rows of the same name from the other engines. The columns are the
    engine's instructions, each other engine's instructions and cycles as multiples of the engine's, and each engine's
    time for one call. Further columns appear if the rows carry the data: MiB per second for rows that count their
    bytes, the work that each engine did from a row's shape (the boxes laid out, reused and measured), the bytes that a
    terminal's lines hold per line, and how many rows of the screen match the engine's.

    Instructions compare two builds of one engine reliably, but not two engines. Waiting, such as on memory that an
    engine allocates, costs cycles that retire no instructions, so the cycles compare the engines."""
    engines = [e for e in OTHERS if e in results["engines"]]
    by_name = {engine: {row["name"]: row for row in rows} for engine, rows in results["engines"].items()}
    ours = by_name.get("prax", {})
    every = [row for rows in results["engines"].values() for row in rows]
    sized = any("bytes" in row for row in every)
    shaped = any("shape" in row for row in every)
    screens = any("sameRows" in row for row in every)
    lined = any("lineBytes" in row for row in every)
    def line_bytes(row):
        return "%.0f" % row["lineBytes"] if row and "lineBytes" in row else "-"
    def shape(row):
        if row is None:
            return "-"
        work = row.get("shape", {})
        cells = "/".join(str(work[key]) if key in work else "-" for key in ("laidOut", "reused", "measured"))
        return cells if work else "-"
    def same(row):
        return "%d/%d" % (row["sameRows"], row["rows"]) if row and "sameRows" in row else "-"
    header = (["measure", "prax"] + engines + ["x " + e for e in engines] + ["cyc x " + e for e in engines]
              + ["time"] + ["time " + e for e in engines])
    if sized:
        header += ["MiB/s"] + ["MiB/s " + e for e in engines]
    if shaped:
        header += ["work"] + ["work " + e for e in engines]
    if lined:
        header += ["B/line"] + ["B/line " + e for e in engines]
    if screens:
        header += ["same " + e for e in engines]
    def ratio(row, mine, key):
        theirs = row["counts"]["time"].get(key, 0) if row else 0
        if not theirs or not mine:
            return "-"
        # A ratio below 0.01 gets two significant figures, because two decimals would round it to zero.
        return "%.2f" % (theirs / mine) if theirs / mine >= 0.01 else "%.2g" % (theirs / mine)
    lines = []
    for name, row in ours.items():
        counts = row["counts"]["time"]
        theirs = [by_name[e].get(name) for e in engines]
        cells = [name, scaled(counts["instructions"])]
        cells += [scaled(r["counts"]["time"]["instructions"]) if r else "-" for r in theirs]
        cells += [ratio(r, counts["instructions"], "instructions") for r in theirs]
        cells += [ratio(r, counts.get("cycles", 0), "cycles") for r in theirs]
        cells += [duration(counts["time"])] + [duration(r["counts"]["time"]["time"]) if r else "-" for r in theirs]
        if sized:
            cells += [throughput(row)] + [throughput(r) for r in theirs]
        if shaped:
            cells += [shape(row)] + [shape(r) for r in theirs]
        if lined:
            cells += [line_bytes(row)] + [line_bytes(r) for r in theirs]
        if screens:
            cells += [same(r) for r in theirs]
        lines.append(cells)
    print(common.text_table(header, lines))
    print("x: the other's instructions as a multiple of the engine's, and cyc x its cycles; above 1, the engine is "
          "faster.")
    if sized:
        print("MiB/s: the bytes processed per second, on the clock.")
    if shaped:
        print("work: laid out/reused/measured.")
    if lined:
        print("B/line: the bytes that the terminal's lines hold for each line of its screen and scrollback, after the "
              "input.")
    if screens:
        print("same: the rows of the screen whose text is the engine's, after the same input.")


def timings(results):
    """Returns the times of the rows of a run that have times and no counts, by row name."""
    out = {}
    for rows in results["engines"].values():
        for row in rows:
            if "counts" not in row and "time" in row:
                out[row["name"]] = row["time"]
    return out


def percent(was, now):
    return 100.0 * (now - was) / was if was else 0.0


def spread(values):
    """Returns how far apart the runs of one side are, as a percentage of the smallest."""
    return percent(min(values), max(values)) if len(values) > 1 and min(values) else 0.0


def share(counts):
    """Returns the percentage of the instructions that were retired on performance cores."""
    total = counts.get("instructions", 0)
    return 100.0 * counts.get("performanceInstructions", 0) / total if total else 0.0


class Change:
    """A measure on two sides. Each side is a list of the counts of its runs."""

    def __init__(self, name, before, after):
        self.name = name
        self.before = before
        self.after = after
        # The count that the measure is compared on: the instructions, unless no run counted any.
        self.key = "instructions" if any(c.get("instructions") for c in before + after) else "cycles"

    def median(self, side, key):
        return statistics.median(counts[key] for counts in side)

    def change(self):
        """Returns the change in the compared count, in percent."""
        return percent(self.median(self.before, self.key), self.median(self.after, self.key))

    def noise(self):
        return max(spread([c[self.key] for c in self.before]), spread([c[self.key] for c in self.after]))

    def changed(self, threshold):
        """Returns true if the compared count changed by more than the threshold and more than twice the spread of the
        runs on either side, with every run of one side beyond every run of the other."""
        change = self.change()
        if abs(change) < max(threshold, 2 * self.noise()):
            return False
        before = [c[self.key] for c in self.before]
        after = [c[self.key] for c in self.after]
        return min(after) > max(before) if change > 0 else max(after) < min(before)

    def gpu(self):
        """Returns the device's time after the change, in milliseconds, and the change in percent, or None if no device
        did work."""
        if not all(c.get("gpu") for c in self.before + self.after):
            return None
        was = self.median(self.before, "gpu")
        now = self.median(self.after, "gpu")
        return now, percent(was, now)

    def line(self, gpu):
        was = self.median(self.before, self.key)
        now = self.median(self.after, self.key)
        cycles = percent(self.median(self.before, "cycles"), self.median(self.after, "cycles"))
        time = statistics.median(c["time"] for c in self.after)
        cells = [self.name, scaled(was), scaled(now), "%+.1f%%" % self.change(), "%+.1f%%" % cycles,
                 "%.0f%%" % share(self.after[len(self.after) // 2]), duration(time)]
        if gpu:
            device = self.gpu()
            cells += ["-", "-"] if device is None else [duration(device[0]), "%+.1f%%" % device[1]]
        return cells


# The columns of a change. `was` and `now` are the compared count: the instructions, or the cycles where no run
# counted instructions.
HEADER = ["measure", "was", "now", "change", "cycles", "p-core", "time"]

# The columns for a device's time: the time after, and the change, which varies with the device's clocks.
GPU_HEADER = ["gpu time", "gpu change"]


class Assessment:
    """What changed in one benchmark from the runs `before` to the runs `after`, each a list of results, or why nothing
    can be compared (`note`)."""

    def __init__(self, name, before, after, threshold, note=None):
        self.name = name
        self.note = note
        self.changes = []
        self.moved = []
        self.unsteady = []
        self.gone = []
        self.new = []
        self.times = []
        if note is not None:
            return
        if any(r["meta"]["workload"] != after[0]["meta"]["workload"] for r in before):
            self.note = "workload changed"
        was = [measures(r) for r in before]
        now = [measures(r) for r in after]
        names = [n for n in now[0] if all(n in m for m in was + now)]
        if not names:
            self.note = "the base has no counts" if not any(was) else "no counts"
            return
        self.changes = [Change(n, [m[n] for m in was], [m[n] for m in now]) for n in names]
        self.moved = sorted((c for c in self.changes if c.changed(threshold)), key=lambda c: -abs(c.change()))
        # If a measure's runs disagree, its work varies from run to run, which hides smaller changes.
        self.unsteady = sorted((c for c in self.changes if c.noise() >= threshold), key=lambda c: -c.noise())
        self.gone = [n for n in was[0] if n not in now[0]]
        self.new = [n for n in now[0] if n not in was[0]]
        self.times = [(n, timings(before[0]).get(n), t) for n, t in timings(after[0]).items()]

    def row(self):
        notes = [self.note] if self.note else []
        if self.new:
            notes.append("%d new" % len(self.new))
        if self.gone:
            notes.append("%d gone" % len(self.gone))
        if not self.changes:
            return [self.name, "-", "-", "-", ", ".join(notes)]
        return [self.name, str(len(self.changes)), str(len(self.moved)) if self.moved else "-",
                str(len(self.unsteady)) if self.unsteady else "-", ", ".join(notes)]


def print_report(title, assessments, threshold, runs, everything):
    """Prints a summary line for each benchmark, then what changed in each, and with `everything`, every measure.
    Returns true if anything changed."""
    print("%s; %d run%s a side, changes past %.1f%%" % (title, runs, "" if runs == 1 else "s", threshold))
    print()
    print(common.text_table(["bench", "measures", "changed", "unsteady", "note"], [a.row() for a in assessments]))
    for a in assessments:
        shown = a.changes if everything else a.moved
        if shown:
            gpu = any(c.gpu() is not None for c in shown)
            print("\n%s: %s" % (a.name, "every measure" if everything else "%d changed" % len(a.moved)))
            print(common.text_table(HEADER + (GPU_HEADER if gpu else []), [c.line(gpu) for c in shown]))
        if everything and (a.new or a.gone):
            print("\n%s: measures new or gone" % a.name)
            print(common.text_table(["measure", ""], [[n, "new"] for n in a.new] + [[n, "gone"] for n in a.gone]))
        if everything and a.unsteady:
            print("\n%s: unsteady" % a.name)
            print(common.text_table(["measure", "spread"], [[c.name, "%.1f%%" % c.noise()] for c in a.unsteady]))
        if everything and a.times:
            print("\n%s: timed alone, which moves with the machine" % a.name)
            print(common.text_table(["row", "was", "now"],
                                   [[n, "-" if t0 is None else duration(t0), duration(t1)] for n, t0, t1 in a.times]))
    if not everything and any(a.unsteady for a in assessments):
        print("\nunsteady: measures whose runs disagree, which hides a change smaller than their spread; "
              "--all lists them")
    if not everything and any(a.new or a.gone for a in assessments):
        print("\nnew and gone: measures one side has and the other does not; --all lists them")
    return any(a.moved for a in assessments)


# ----------------------------------------------------------------------------------------------------------------------
# show


def describe(meta):
    changes = " with changes" if meta["dirty"] else ""
    return "%s%s (%s)" % (meta["commit"][:7], changes, meta["date"].replace("T", " ")[:16])


def show(name, results):
    """Prints the measures of one run of `name`, beside other engines' where the benchmark runs them."""
    print("--- %s: %s" % (name, describe(results["meta"])))
    if any(engine in results["engines"] for engine in OTHERS):
        print_engines(results)
        return
    lines = [[n, scaled(c["instructions"]), scaled(c["cycles"]), "%.0f%%" % share(c), duration(c["time"]),
              duration(c["gpu"]) if c.get("gpu") else "-"]
             for n, c in measures(results).items()]
    if lines:
        print(common.text_table(["measure", "instr", "cycles", "p-core", "time", "gpu time"], lines))
    times = timings(results)
    if times:
        print(common.text_table(["timed alone", "time"], [[n, duration(t)] for n, t in times.items()]))


# ----------------------------------------------------------------------------------------------------------------------
# run


def run_runner(name, directory, out, extra, label):
    """Runs the runner of benchmark `name` on the binaries in `directory`, records the results in `out`, and returns
    them, or None if the runner failed or passed the deadline. Prints a line named by `label` when the run ends, so
    that a long series of runs shows its progress."""
    if os.path.exists(out):
        os.remove(out)
    started = time.monotonic()
    # The runner runs in its own session, so that a run past the deadline is stopped together with the harnesses that
    # it started.
    process = subprocess.Popen([sys.executable, "-m", RUNNER, name, "--bin", directory, "--out", out] + extra,
                               cwd=common.ROOT, start_new_session=True)
    try:
        status = process.wait(DEADLINE)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()
        print("%-10s %-12s stopped after %ds" % (name, label, DEADLINE), file=sys.stderr, flush=True)
        return None
    print("%-10s %-12s %s%s" % (name, label, duration((time.monotonic() - started) * 1e3),
                                "" if status == 0 else ", failed"), file=sys.stderr, flush=True)
    results = load(out)
    return results if status == 0 else None


def run(names, directory, extra):
    """Runs each benchmark once and prints its measures. Returns true if every run succeeded."""
    os.makedirs(AB, exist_ok=True)
    ok = True
    for i, name in enumerate(names or all_benches()):
        results = run_runner(name, directory, os.path.join(AB, "%s-run.json" % name), extra, "run")
        if i:
            print()
        if results is None:
            print("--- %s: the run failed" % name)
            ok = False
            continue
        show(name, results)
    return ok


def all_benches():
    return [b.name for b in registry.BENCHES]


# ----------------------------------------------------------------------------------------------------------------------
# ab


def ab(names, base_bin, base_name, current_bin, runs, threshold, everything, extra):
    """Runs each benchmark's runner on the base binaries and the current binaries in turn, `runs` times each, and
    reports what changed."""
    os.makedirs(AB, exist_ok=True)
    assessments = []
    for name in names or all_benches():
        sides = {"base": [], "current": []}
        failed = None
        for index in range(runs):
            for side, directory in (("base", base_bin), ("current", current_bin)):
                results = run_runner(name, directory, os.path.join(AB, "%s-%s-%d.json" % (name, side, index)), extra,
                                     "%s %d/%d" % (side, index + 1, runs))
                if results is None:
                    failed = "the %s run failed" % side
                    break
                sides[side].append(results)
            if failed:
                break
        assessments.append(Assessment(name, sides["base"], sides["current"], threshold, failed))
    return print_report("working tree against %s" % base_name, assessments, threshold, runs, everything)


def scripted_named(names):
    """Returns the scripted benchmarks in `names`, or every scripted benchmark if `names` is empty."""
    benches = [b for b in registry.named(names) if b.scripted]
    if not benches:
        sys.exit("%s: no browser runs it; ./dev.sh bench list says which do" % ", ".join(names))
    return benches


def kept(benches, directory, count):
    """Prints what `count` runs of each case keep allocated (scripted.kept). The numbers must stay flat as the count
    grows. A number that grows is a leak."""
    for bench in benches:
        if not bench.cases:
            continue
        problem = scripted.missing(bench)
        if problem:
            sys.exit(problem)
        print("%s: kept over %d runs" % (bench.name, count))
        print(common.text_table(["case", "kept KB"], [[name, "%.0f" % (size / 1024)]
                                                       for name, size in scripted.kept(bench, directory, count)]))
        print()


def case_args(name, case, directory, runs=1000000):
    """Returns the pr_shell arguments that run case `case` of the scripted benchmark `name` `runs` times without
    measuring, using a prelude written under `directory`. A profile of one case runs these arguments."""
    bench = scripted_named([name])[0]
    if not bench.cases:
        sys.exit("%s: its cases do not run alone" % name)
    problem = scripted.missing(bench)
    if problem:
        sys.exit(problem)
    path = scripted.prelude(bench, directory, False, {"name": case, "runs": runs})
    page = os.path.join(common.ROOT, scripted.SHELL_PAGE)
    return ["--prelude", path, "--html", page]
