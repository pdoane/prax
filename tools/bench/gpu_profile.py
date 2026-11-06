#!/usr/bin/env python3
"""Records a harness's work on the GPU and prints what it cost: the time that each stage of each encoder took on the
GPU, the size of each shader's compiled code, and the bytes of registers that the compiler spilled for each encoder's
pipelines.

The recording is a Metal System Trace from Instruments (xctrace), on macOS, which needs no privileges. Times are the
GPU's own, at whatever clocks it ran. They are therefore steadier for a harness that keeps the GPU busy, as draw_bench's
draw measures do, than for one that idles between frames. Code sizes and spills come from the compiler, and do not vary
from run to run. A spill is per-thread memory that a pipeline needs because it ran out of registers. A spill costs
every thread of the pipeline, whichever path the thread takes, and it lowers the pipeline's occupancy.

./dev.sh profile --gpu builds the harness and runs this. Each recording is kept under build/profile.

Usage: python3 -m tools.bench.gpu_profile --binary PATH [--seconds N] [-- HARNESS-ARGS...]
"""

import argparse
import collections
import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET

import tools

ROOT = tools.ROOT
OUT = os.path.join(ROOT, "build", "profile")


def record(binary, args, seconds):
    """Records the harness under Metal System Trace and returns the trace's path."""
    os.makedirs(OUT, exist_ok=True)
    trace = os.path.join(OUT, os.path.basename(binary) + "-gpu.trace")
    # xctrace writes a new recording only if none exists.
    shutil.rmtree(trace, ignore_errors=True)
    subprocess.run(["xctrace", "record", "--template", "Metal System Trace", "--output", trace, "--time-limit",
                    "%ds" % seconds, "--launch", "--", os.path.abspath(binary)] + args,
                   cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not os.path.exists(trace):
        sys.exit("profile: xctrace recorded nothing for %s" % binary)
    return trace


def table(trace, schema):
    """Returns the rows of one of the trace's tables as dictionaries of elements by column, with xctrace's references
    to earlier elements resolved."""
    done = subprocess.run(["xctrace", "export", "--input", trace, "--xpath",
                           '/trace-toc/run[@number="1"]/data/table[@schema="%s"]' % schema],
                          capture_output=True, check=True)
    root = ET.fromstring(done.stdout)
    elements = {}

    def resolve(e):
        if "ref" in e.attrib:
            return elements[e.attrib["ref"]]
        if "id" in e.attrib:
            elements[e.attrib["id"]] = e
        for child in e:
            resolve(child)
        return e

    rows, columns = [], []
    for node in root:
        for child in node:
            if child.tag == "schema":
                columns = [c.find("mnemonic").text for c in child.findall("col")]
            elif child.tag == "row":
                rows.append(dict(zip(columns, [resolve(c) for c in child])))
    return rows


def text(element):
    """Returns an element's value as xctrace formats it, or "" for no element."""
    return "" if element is None else element.attrib.get("fmt", element.text or "")


def pid(element):
    """Returns the process id of a process element, or None."""
    found = None if element is None else element.find("pid")
    return found.text if found is not None else None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--seconds", type=int, default=60, help="the longest recording")
    parser.add_argument("args", nargs="*")
    options = parser.parse_args()
    trace = record(options.binary, options.args, options.seconds)
    print("recording: %s" % os.path.relpath(trace, ROOT))

    # The harness's process is the one of its name that used the GPU.
    name = os.path.basename(options.binary)
    intervals = table(trace, "metal-gpu-intervals")
    process = next((pid(r.get("process")) for r in intervals if text(r.get("process")).startswith(name + " (")), None)
    mine = lambda row: process is not None and pid(row.get("process")) == process
    # The trace lists only the shaders that a pipeline compiled while it recorded.
    shaders = [r for r in table(trace, "metal-shader-profiler-shader-list") if mine(r)]

    sizes = {}
    for r in shaders:
        label = re.sub(r" \(\d+\)$", "", text(r["name"]))
        size = int(r["pc-end"].text) - int(r["pc-start"].text)
        sizes[(label, text(r["shader-type"]))] = size
    print("\n%-32s %-10s %10s" % ("shader", "stage", "code"))
    for (label, stage), size in sorted(sizes.items(), key=lambda item: -item[1]):
        print("%-32s %-10s %9.1fK" % (label, stage, size / 1024))

    spills = collections.Counter()
    for r in table(trace, "graphics-compiler-spill-events"):
        if mine(r):
            spills[int(r["spilled-bytes"].text)] += 1
    print("\nspilled bytes of an encoder's pipelines, and the encoders that spilled them")
    for size, count in sorted(spills.items()) or [(0, 0)]:
        print("%8d B %8d" % (size, count))

    # Each encoder's time on each stage: what a pass or a dispatch cost.
    stages = collections.defaultdict(list)
    for r in intervals:
        if not mine(r):
            continue
        label = re.sub(r"\d+", "#", text(r["event-label"]).split("  (")[0])
        stages[(label, text(r["channel-name"]))].append(int(r["duration"].text))
    print("\n%-48s %-10s %8s %10s %10s %10s" % ("encoder", "stage", "count", "median", "mean", "total"))
    for (label, stage), times in sorted(stages.items(), key=lambda item: -sum(item[1])):
        times.sort()
        print("%-48s %-10s %8d %9.1fus %9.1fus %9.2fms" % (label, stage, len(times), times[len(times) // 2] / 1e3,
                                                          sum(times) / len(times) / 1e3, sum(times) / 1e6))


if __name__ == "__main__":
    main()
