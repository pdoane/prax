"""Compares the engine with browsers on time. Each scripted benchmark's script runs in pr_shell and in each browser
(tools/browsers/browsers.py), measured the same way, and the results are printed side by side.

Every engine is timed with tools/bench/measure.js, which runs batches of calls that last well past the coarsest clock
and takes the median of a few of them. The engine's counted measures are faster to take and steadier, but a browser
counts no instructions, so the comparison uses times taken the same way for every engine. Nothing is recorded. A
comparison is read when it is made, and what the engine's changes did is counted by `./dev.sh bench ab`
(tools/bench/perf.py). A browser loads tools/bench/browser.html. The report gives each engine's clock resolution.

Each measure also reports how many cores its engine kept busy: the time that the engine's processes ran on cores,
divided by the elapsed time. A browser's processes are its descendants and, for WebKit, the web content process that the
host names, which WebKit launches as a service.
"""


import math
import os
import shutil
import subprocess
import sys
import tempfile
import threading

from tools.bench import common
from tools.bench import scripted
from tools.browsers.browsers import BROWSERS, Server, chosen, read_processes, url_of

def config(bench):
    """Returns the configuration that tools/bench/browser.html loads for `bench`."""
    return {"name": bench.name, "script": url_of(bench.script), "measure": url_of(os.path.join(common.HERE,
                                                                                               "measure.js")),
            "inputs": {key: url_of(path) for key, path in bench.inputs().items()},
            "fonts": [{"family": family, "style": style, "url": url_of(path)} for family, style, path in bench.fonts],
            "compare": bench.compare}


def page(server, bench):
    """Returns the URL of the page that runs `bench`. The server serves that page until the next call."""
    server.config = config(bench)
    server.report = None
    server.reported.clear()
    return "http://127.0.0.1:%d/tools/bench/browser.html?%s" % (server.server_address[1], bench.name)


def run_browser(server, name, bench, timeout, directory):
    """Returns what browser `name` reported for `bench`, or None. A host is in `directory`, with the harnesses."""
    browser = BROWSERS[name]
    url = page(server, bench)
    profile = tempfile.mkdtemp(prefix="bench-%s-" % name)
    process = None
    try:
        process = browser.launch(url, profile, directory)
        server.browser = process.pid
        server.others = set()
        if browser.host:
            threading.Thread(target=read_processes, args=(process, server.others), daemon=True).start()
        report = server.wait(timeout)
        if report is None and not server.reported.is_set():
            print("  %s did not report within %ds" % (name, timeout), file=sys.stderr)
        return report
    finally:
        if process is not None:
            process.terminate()
            try:
                process.wait(10)
            except subprocess.TimeoutExpired:
                process.kill()
        shutil.rmtree(profile, ignore_errors=True)


# ----------------------------------------------------------------------------------------------------------------------
# Reporting


def times(bench, report):
    """Returns a report's measures by case. Each measure holds the median time in milliseconds, the times of the
    fastest and slowest batches, and the number of cores that the engine kept busy, or None if nothing counted them."""
    out = {}
    for row in report["rows"]:
        value = row.get(bench.compare)
        if value:
            low, high = row.get("range", {}).get(bench.compare, (value, value))
            out[row["name"]] = (value, low, high, row.get("cores", {}).get(bench.compare))
    return out


def cores(measure):
    return "-" if measure[3] is None else "%.1f" % measure[3]


def cpu_time(measure):
    """Returns the time on cores that a call took, in milliseconds, or None if the cores were not counted."""
    return None if measure[3] is None else measure[0] * measure[3]


def overlap(a, b):
    """Returns true if the batches of two measures overlap, in which case the case does not show which engine is
    faster."""
    return a[1] <= b[2] and b[1] <= a[2]


def geomean(values):
    return math.exp(sum(math.log(v) for v in values) / len(values)) if values else None


def ratio(value):
    return "-" if value is None else "%.2fx" % value


def clock(report):
    step = common.duration(float("%.2g" % report["clock"])) if report["clock"] < float("inf") else "?"
    return step + ("" if report.get("isolated", True) else ", not isolated")


def print_bench(bench, reports):
    """Prints each engine's time for each case of `bench`, with the number of cores it kept busy, and each browser's
    time as a multiple of the engine's time. Then prints, by kind of case, each browser's geometric mean of its time
    relative to the engine's and of its time on cores. Returns both means over every case, by browser."""
    measured = {engine: times(bench, report) for engine, report in reports.items()}
    browsers = [name for name in reports if name != "prax"]
    own = measured["prax"]
    print("%s: %s" % (bench.name, bench.summary))
    print("clock: " + ", ".join("%s %s" % (engine, clock(report)) for engine, report in reports.items()))

    header = ["case", "prax", "cores"] + [cell for name in browsers for cell in (name, "x", "cores")]
    lines = []
    for case, mine in own.items():
        line = [case, common.duration(mine[0]), cores(mine)]
        for name in browsers:
            theirs = measured[name].get(case)
            if theirs is None:
                line += ["-", "", ""]
                continue
            line += [common.duration(theirs[0]), ratio(theirs[0] / mine[0]) + ("?" if overlap(mine, theirs) else " "),
                     cores(theirs)]
        lines.append(line)
    print(common.text_table(header, lines))

    def mean(name, cost, kind=None):
        values = []
        for case, mine in own.items():
            theirs = measured[name].get(case)
            if theirs is None or (kind is not None and bench.kind(case) != kind):
                continue
            if cost(theirs) is not None and cost(mine) is not None:
                values.append(cost(theirs) / cost(mine))
        return geomean(values)

    def wall(measure):
        return measure[0]

    kinds = list(dict.fromkeys(bench.kind(case) for case in own))
    rows = [] if kinds == ["all"] else [[kind] + [ratio(mean(name, wall, kind)) for name in browsers] for kind in kinds]
    overall = {name: (mean(name, wall), mean(name, cpu_time)) for name in browsers}
    rows.append(["all"] + [ratio(overall[name][0]) for name in browsers])
    rows.append(["all, time on cores"] + [ratio(overall[name][1]) for name in browsers])
    print()
    print(common.text_table(["geometric mean"] + browsers, rows))
    return overall


LEGEND = """x: a browser's time over prax's; above 1x, prax is faster, and ? where the runs overlap.
cores: the cores an engine kept busy as it measured. time on cores: x for time times cores."""


# ----------------------------------------------------------------------------------------------------------------------
# Commands


def serve(bench, directory):
    """Serves the page for `bench` until interrupted, and compares the results of each browser that opens it with the
    engine's."""
    own = scripted.run_shell(bench, directory, timed=True)[0]
    server = Server()
    print("open %s in any browser; ^C stops" % page(server, bench))
    try:
        while True:
            report = server.wait()
            if report is not None:
                print("\n" + report.get("agent", ""))
                print_bench(bench, {"prax": own, "browser": report})
            server.reported.clear()
    except KeyboardInterrupt:
        return 0


def main(benches, names, directory, timeout, serving):
    for bench in benches:
        problem = scripted.missing(bench)
        if problem:
            sys.exit(problem)
    if serving:
        if len(benches) != 1:
            sys.exit("--serve serves one benchmark; name it")
        return serve(benches[0], directory)
    names = chosen(names, directory)
    print("%s; %s" % (", ".join("%s %s" % (name, BROWSERS[name].version()) for name in names), common.power()))
    print(LEGEND)
    server = Server()
    summary = []
    status = 0
    for bench in benches:
        reports = {"prax": scripted.run_shell(bench, directory, timed=True)[0]}
        for name in names:
            print("  %s: %s" % (bench.name, name), file=sys.stderr, flush=True)
            report = run_browser(server, name, bench, timeout, directory)
            if report is None:
                status = 1
            else:
                reports[name] = report
        print()
        overall = print_bench(bench, reports)
        summary.append([bench.name] + [ratio(overall[name][0]) if name in overall else "-" for name in names])
        summary.append([bench.name + ", time on cores"]
                       + [ratio(overall[name][1]) if name in overall else "-" for name in names])
    if len(benches) > 1:
        print()
        print(common.text_table(["geometric mean"] + names, summary))
    return 1 if status or common.failed else 0
