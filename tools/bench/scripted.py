"""Runs a scripted benchmark's script in pr_shell, and its C harness beside it.

tools/bench/shell.html loads the script after tools/bench/measure.js. A prelude written beside the harnesses supplies
the globals that the loader supplies (measure.js describes them): the inputs, read from their files, the script's text,
and a __REPORT__ function that prints the rows as one line of JSON.
"""

import json
import os

from tools.bench import common

SHELL_PAGE = os.path.join("tools", "bench", "shell.html")


def prelude(bench, directory, timed, only):
    path = os.path.join(directory, "%s-prelude.js" % bench.name)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("globalThis.__INPUT__ = {\n")
        for key, file in bench.inputs().items():
            f.write("    %s: readFile(%s),\n" % (json.dumps(key), json.dumps(file)))
        f.write("};\n")
        f.write("globalThis.__SCRIPT__ = readFile(%s);\n" % json.dumps(bench.script))
        f.write("globalThis.__TIMED__ = %s;\n" % json.dumps(timed))
        if only is not None:
            f.write("globalThis.__ONLY__ = %s;\n" % json.dumps(only))
        f.write("globalThis.__REPORT__ = function (rows) {\n"
                "    print(JSON.stringify({rows: rows, clock: clockResolution()}));\n"
                "};\n")
    return path


def missing(bench):
    """Returns a message that names the inputs of `bench` that are not on disk, or None if all are present."""
    absent = [path for path in bench.inputs().values() if not os.path.exists(path)]
    if absent:
        return "no %s; run ./dev.sh fetch %s" % (os.path.relpath(absent[0], common.ROOT), " ".join(bench.fetch))
    return None


def run_shell(bench, directory, timed=False, only=None):
    """Returns what the script reported in pr_shell, and the peak resident size of the process in bytes. The report is
    {rows, clock}, where `clock` is the resolution of the clock that the script measured with, in milliseconds."""
    shell = common.binary(directory, "pr_shell")
    done, peak = common.run_peak([shell, "--prelude", prelude(bench, directory, timed, only),
                                  "--html", os.path.join(common.ROOT, SHELL_PAGE)])
    return json.loads(common.reported(done, "pr_shell").splitlines()[-1]), peak


def run(args):
    """Returns the counted rows of the script in pr_shell and the rows of the C harness, and the files that they
    read."""
    problem = missing(args.bench)
    if problem:
        raise SystemExit(problem)
    bench = args.bench
    results = {"prax": run_shell(bench, args.bin)[0]["rows"]}
    if bench.native:
        command = [common.binary(args.bin, bench.native)] + list(bench.inputs().values())
        results["prax (C)"] = common.run_json(command, bench.native)
    return results, list(bench.inputs().values())


def kept(bench, directory, count):
    """Measures what `count` runs of each case leave allocated. Each case runs `count` times in its own process, and
    again zero times, and the difference between the peak resident sizes of the two processes is what those runs kept.
    A case that frees what it discards keeps almost nothing, whatever the count. Returns [name, bytes] for each case."""
    names = [row["name"] for row in run_shell(bench, directory)[0]["rows"]]
    out = []
    for name in names:
        _, idle = run_shell(bench, directory, only={"name": name, "runs": 0})
        _, busy = run_shell(bench, directory, only={"name": name, "runs": count})
        out.append([name, max(0, busy - idle)])
    return out
