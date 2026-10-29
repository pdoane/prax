#!/usr/bin/env python3
"""Profiles a harness and prints where its time goes: the functions that took the most samples, on their own and
including what they call, and the source lines. Each sample is attributed to the innermost line, including lines that
inlining hides. With --base-binary, the same harness from another build also runs, and each row shows the shares of
both sides and how they changed.

The profile is Instruments' Time Profiler (xctrace), on macOS, which samples each thread's stack every millisecond. A
sample measures time, not instructions, so work that waits on memory takes more samples than its instruction count
suggests. Compare shares, not counts: two builds run their harness for the same time, and the faster one does more
work in that time. Lines come from the binary's debug information, which dsymutil gathers and llvm-symbolizer reads.
The default build keeps debug information, and the release build does not.

./dev.sh profile builds the harnesses and runs this. Each recording is kept under build/profile.

Usage: python3 -m tools.bench.profile --binary PATH [--base-binary PATH] [--seconds N] [--under NAME] [--rows N]
                          [-- HARNESS-ARGS...]
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

# The places to look for llvm-symbolizer: on the path, or where Homebrew installs LLVM.
SYMBOLIZERS = ["llvm-symbolizer", "/opt/homebrew/opt/llvm/bin/llvm-symbolizer",
               "/usr/local/opt/llvm/bin/llvm-symbolizer"]


def symbolizer():
    for candidate in SYMBOLIZERS:
        found = shutil.which(candidate)
        if found:
            return found
    sys.exit("profile: llvm-symbolizer is required (brew install llvm)")


def record(binary, args, seconds, name):
    """Records the harness under the Time Profiler and returns the XML of the exported samples."""
    os.makedirs(OUT, exist_ok=True)
    trace = os.path.join(OUT, name + ".trace")
    # xctrace writes a new recording only if none exists.
    shutil.rmtree(trace, ignore_errors=True)
    subprocess.run(["xctrace", "record", "--template", "Time Profiler", "--output", trace, "--time-limit",
                    "%ds" % seconds, "--launch", "--", os.path.abspath(binary)] + args,
                   cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not os.path.exists(trace):
        sys.exit("profile: xctrace recorded nothing for %s" % binary)
    xml = os.path.join(OUT, name + ".xml")
    with open(xml, "w") as f:
        subprocess.run(["xctrace", "export", "--input", trace, "--xpath",
                        '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]'], stdout=f, check=True)
    return xml


def samples(xml, binary):
    """Returns each sample's stack, innermost first, as (name, address) frames. The address is relative to the
    harness's image if the frame is in the harness, and None otherwise."""
    root = ET.parse(xml).getroot()
    image = os.path.basename(binary)
    frames, stacks, out = {}, {}, []
    harness, load = None, None
    for row in root.iter("row"):
        backtrace = row.find("tagged-backtrace")
        if backtrace is None:
            continue
        if "ref" in backtrace.attrib:
            out.append(stacks[backtrace.attrib["ref"]])
            continue
        stack = []
        for frame in backtrace.findall("frame"):
            if "ref" in frame.attrib:
                stack.append(frames[frame.attrib["ref"]])
                continue
            binary_node = frame.find("binary")
            mine = False
            if binary_node is not None:
                if "ref" in binary_node.attrib:
                    mine = binary_node.attrib["ref"] == harness
                elif binary_node.attrib.get("name") == image:
                    harness = binary_node.attrib["id"]
                    load = int(binary_node.attrib["load-addr"], 16)
                    mine = True
            address = int(frame.attrib["addr"], 16) - load + 0x100000000 if mine else None
            entry = (frame.attrib.get("name", "?"), address)
            frames[frame.attrib["id"]] = entry
            stack.append(entry)
        stacks[backtrace.attrib["id"]] = stack
        out.append(stack)
    return out


def lines(binary, addresses, name):
    """Returns the innermost inlined function and line of each address, by address."""
    dsym = os.path.join(OUT, name + ".dSYM")
    subprocess.run(["dsymutil", binary, "-o", dsym], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    dwarf = os.path.join(dsym, "Contents", "Resources", "DWARF", os.path.basename(binary))
    order = sorted(addresses)
    done = subprocess.run([symbolizer(), "--obj=" + dwarf, "--inlines", "--functions=short"],
                          input="\n".join(hex(a) for a in order), capture_output=True, text=True, check=True)
    out = {}
    for address, block in zip(order, done.stdout.strip().split("\n\n")):
        parts = block.strip().split("\n")
        location = re.sub(r"^.*/", "", parts[1]) if len(parts) > 1 else "?"
        out[address] = "%s %s" % (re.sub(r":\d+$", "", location), parts[0])
    return out


def short(name):
    name = re.sub(r"pr_impl::|\(anonymous namespace\)::", "", name)
    return re.sub(r"\(.*", "", name)[:60]


def tally(binary, name, stacks, under):
    """Returns the share of the samples that each function took on its own and including what it calls, and the share
    that each innermost line took. If `under` is given, only samples whose stack holds `under` are counted."""
    if under:
        stacks = [s for s in stacks if any(under in name for name, _ in s)]
    total = max(1, len(stacks))
    own, inclusive, leaves = collections.Counter(), collections.Counter(), collections.Counter()
    for stack in stacks:
        if not stack:
            continue
        own[short(stack[0][0])] += 1
        for name in set(short(n) for n, _ in stack):
            inclusive[name] += 1
        if stack[0][1] is not None:
            leaves[stack[0][1]] += 1
    resolved = lines(binary, leaves, name) if leaves else {}
    by_line = collections.Counter()
    for address, count in leaves.items():
        by_line[resolved.get(address, "?")] += count
    share = lambda counter: {k: 100.0 * v / total for k, v in counter.items()}
    return len(stacks), share(own), share(inclusive), share(by_line)


def table(title, now, base, rows):
    print("\n-- %s" % title)
    keys = sorted(set(now) | set(base or {}), key=lambda k: -max(now.get(k, 0), (base or {}).get(k, 0)))[:rows]
    for key in keys:
        if base is None:
            print("%6.1f%%  %s" % (now.get(key, 0), key))
        else:
            was, is_ = base.get(key, 0), now.get(key, 0)
            print("%6.1f%% %6.1f%% %+6.1f  %s" % (was, is_, is_ - was, key))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--binary", required=True)
    ap.add_argument("--base-binary")
    ap.add_argument("--seconds", type=int, default=8)
    ap.add_argument("--under", help="only the samples under the function whose name holds this")
    ap.add_argument("--rows", type=int, default=30)
    ap.add_argument("args", nargs="*")
    args = ap.parse_args()
    if not shutil.which("xctrace"):
        sys.exit("profile: xctrace is required (Xcode, on macOS)")

    sides = [("now", args.binary)] + ([("base", args.base_binary)] if args.base_binary else [])
    results = {}
    for side, binary in sides:
        name = "%s-%s" % (os.path.basename(binary), side)
        results[side] = tally(binary, name, samples(record(binary, args.args, args.seconds, name), binary), args.under)
        print("%s: %d samples%s" % (side, results[side][0], " under " + args.under if args.under else ""))
    base = results.get("base")
    if base is not None:
        print("\nshares of each side's samples: base, now, and the change")
    now = results["now"]
    table("functions, on their own", now[1], base[1] if base else None, args.rows)
    table("functions, with what they call", now[2], base[2] if base else None, args.rows)
    table("lines, innermost", now[3], base[3] if base else None, args.rows)


if __name__ == "__main__":
    main()
