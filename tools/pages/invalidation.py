#!/usr/bin/env python3
"""Runs the invalidation harness (tools/pages/invalidation.cpp) over the captured pages corpus, and reports what it
found.

The corpus is the pages under third_party/pages that tools/pages/pages.py captured with everything that they load. Each
page is stored beside a JSON file that names its URL and the file that each of its resources was saved to. The harness
loads each page without its scripts and makes a series of changes to elements spread through it. After each change it
checks the presentation against a full recomputation, and counts what the change cost and how much of that cost was
wasted.

A page fails if its presentation failed a check, or if the harness did not finish. For each page, the table gives the
number of elements that it holds and the number of frames checked, the average number of instructions per frame, the
share of restyled elements whose style did not change, and the number of boxes painted again although nothing that
they paint changed. The run fails if any page fails, and lists each failure.

Usage: python3 -m tools.pages.invalidation [--binary PATH] [--pages DIR] [--elements N] [--no-verify] [--jobs N]
                               [--timeout S] [PAGE]...
"""

import argparse
import concurrent.futures
import glob
import json
import os
import subprocess
import sys

from tools.suite import corpus

PAGES = os.path.join(corpus.ROOT, "third_party", "pages")
HARNESS = corpus.binary("invalidation")


def check(binary, capture, page, args):
    """Runs the harness on one page. Returns the harness's report, or a failure."""
    command = [binary, "--elements", str(args.elements)]
    if args.no_verify:
        command.append("--no-verify")
    try:
        done = subprocess.run(command + [capture, page], capture_output=True, text=True,
                              timeout=args.timeout * corpus.SLOWDOWN)
    except subprocess.TimeoutExpired:
        return {"error": "timed out"}
    if done.returncode != 0:
        lines = done.stderr.strip().splitlines()
        return {"error": lines[-1] if lines else "exited %d" % done.returncode}
    try:
        return json.loads(done.stdout)
    except json.JSONDecodeError:
        return {"error": "reported nothing"}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    corpus.outcome_arguments(ap)
    ap.add_argument("--binary", default=HARNESS, help="the invalidation harness")
    ap.add_argument("--pages", dest="directory", default=PAGES, help="the directory of captured pages")
    ap.add_argument("--elements", type=int, default=60, help="how many elements of each page to change")
    ap.add_argument("--no-verify", action="store_true", help="count without checking")
    ap.add_argument("--jobs", type=int, default=os.cpu_count(), help="how many pages run at once")
    ap.add_argument("--timeout", type=float, default=300, help="seconds a page may take")
    ap.add_argument("pages", nargs="*", help="pages under third_party/pages by name; every captured one by default")
    args = ap.parse_args()
    corpus.require(args.binary, "invalidation")

    captures = sorted(glob.glob(os.path.join(args.directory, "*.json")))
    captures = [c for c in captures if os.path.basename(c) != "pages.json"]
    if args.pages:
        captures = [c for c in captures
                    if os.path.splitext(os.path.basename(c))[0] in args.pages
                    or os.path.basename(c)[:-len(".json")] + ".html" in args.pages]
    if not captures:
        sys.exit("no captured pages under %s; run ./dev.sh fetch pages" % args.directory)

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        running = {c: pool.submit(check, args.binary, c, c[:-len(".json")] + ".html", args)
                   for c in captures}
        reports = {c: f.result() for c, f in running.items()}

    width = max(len(os.path.basename(c)[:-len(".json")]) for c in captures)
    print("%s  %8s  %6s  %10s  %9s  %10s" % ("page".ljust(width), "elements", "frames", "instr/frame",
                                            "restyled=", "repainted="))
    failed = []
    totals = {"frames": 0, "instructions": 0, "styled": 0, "styledUnchanged": 0, "repaintedUnchanged": 0}
    for capture in captures:
        name = os.path.basename(capture)[:-len(".json")]
        report = reports[capture]
        if "error" in report:
            failed.append((name, report["error"]))
            print("%s  %s" % (name.ljust(width), report["error"]))
            continue
        if report["failures"]:
            failed.append((name, "%d frame(s) failed; first: %s" % (report["failures"], report["firstFailure"])))
        for key in totals:
            totals[key] += report[key]
        frames = max(report["frames"], 1)
        unchanged = 100.0 * report["styledUnchanged"] / max(report["styled"], 1)
        print("%s  %8d  %6d  %10.0fk  %8.1f%%  %10d" % (name.ljust(width), report["elements"], report["frames"],
                                                     report["instructions"] / frames / 1000, unchanged,
                                                     report["repaintedUnchanged"]))
    frames = max(totals["frames"], 1)
    print("%s  %8s  %6d  %10.0fk  %8.1f%%  %10d" % ("total".ljust(width), "", totals["frames"],
                                                 totals["instructions"] / frames / 1000,
                                                 100.0 * totals["styledUnchanged"] / max(totals["styled"], 1),
                                                 totals["repaintedUnchanged"]))
    kinds = {}
    for report in reports.values():
        for kind in report.get("kinds", []):
            into = kinds.setdefault(kind["name"], {"frames": 0, "instructions": 0, "styled": 0,
                                                   "styledUnchanged": 0, "blocks": 0, "commands": 0})
            for key in into:
                into[key] += kind[key]
    if kinds:
        print("\n%-14s  %6s  %11s  %13s  %9s  %12s  %14s" % ("change", "frames", "instr/frame", "styled/frame",
                                                          "restyled=", "blocks/frame", "commands/frame"))
        for name, kind in kinds.items():
            frames = max(kind["frames"], 1)
            print("%-14s  %6d  %10.0fk  %13.1f  %8.1f%%  %12.1f  %14.1f"
                  % (name, kind["frames"], kind["instructions"] / frames / 1000, kind["styled"] / frames,
                     100.0 * kind["styledUnchanged"] / max(kind["styled"], 1), kind["blocks"] / frames,
                     kind["commands"] / frames))
    print("\nrestyled=: restyled elements whose style did not change. repainted=: boxes painted again although "
          "nothing they paint changed.")
    # A page is a case, which passes if no check failed.
    failing = {name for name, _ in failed}
    corpus.outcomes(args, {os.path.basename(c)[:-len(".json")]: 0 if os.path.basename(c)[:-len(".json")] in failing
                           else 1 for c in captures})
    if failed:
        print("\n%d page(s) failed:" % len(failed))
        for name, why in failed:
            print("  %s\n      %s" % (name, why))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
