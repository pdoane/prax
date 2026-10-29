#!/usr/bin/env python3
"""Runs one benchmark once on the harnesses under --bin and records its rows in the file given by --out.
tools/bench/perf.py runs this script once for each run that it records, each in its own process, so that a run that
hangs is stopped together with the harnesses that it started.

Usage: python3 -m tools.bench.run BENCH --out FILE [--bin DIR] [BENCH-OPTIONS...]
"""

import argparse
import sys

from tools.bench import common
from tools.bench import perf
from tools.bench import registry
from tools.bench import scripted


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in registry.BY_NAME:
        sys.exit("usage: run.py BENCH --out FILE [--bin DIR] [options]; ./dev.sh bench list names them")
    bench = registry.BY_NAME[sys.argv[1]]
    parser = argparse.ArgumentParser(prog="run.py " + bench.name, description=bench.summary)
    parser.add_argument("--bin", default=common.RELEASE, metavar="DIR", help="where the harnesses are")
    parser.add_argument("--out", required=True, metavar="FILE", help="record the rows in FILE")
    bench.options(parser)
    args = parser.parse_args(sys.argv[2:])
    args.bench = bench
    results, inputs = scripted.run(args) if bench.scripted else bench.run(args)
    perf.record(bench, results, inputs, args.out)
    return 1 if common.failed else 0


if __name__ == "__main__":
    sys.exit(main())
