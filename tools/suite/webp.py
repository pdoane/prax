#!/usr/bin/env python3
"""Compares lib/webp's decoder with libwebp's over libwebp's test data, and over files that libwebp's encoders write.

The corpora are the `libwebp_test_data` checkout, whose files are the conformance vectors of VP8 and images of the
container's and the lossless format's features, and the written part: files that libwebp's encoders write for each
feature, lossless, lossy, with alpha and animated, which the harness writes under the build's directory once
(tools/suite/webp/webp_written.h). The webp_compare harness decodes each file with both decoders
(tools/suite/webp/webp_compare.cpp): every frame as Rgba8, as libwebp's WebPDecodeRGBA and its animation decoder give
them, and a still lossy image's planes.

A case is one file, and its key is its path under its corpus's part, as libwebp-test-data/test.webp or
written/lossy/64x48.q75.m4.f1s60h0.g4.p0.n50.webp, whose name gives the settings that wrote it. It passes if the
two decoders agree that it decodes or that it does not, and if lib/webp's frames, durations and play count are
libwebp's, byte for byte, and its planes of a lossy image are too. An image that says it has no alpha is opaque, and its
alpha is not compared; the notes count the pixels whose alpha libwebp decodes as other than opaque there.

The run fails on a case that fails without an entry in the errata, a recorded case that passes, or an entry that names
no case. With --time, it prints what each decoder takes for each file and in all, which no scoreboard records.
"""

import json
import os
import subprocess
import sys

from tools.suite import corpus

# The number of files that one run of the harness compares.
BATCH = 16

# The parts of the suite: the name of each, and its corpus under third_party.
PARTS = (("libwebp-test-data", os.path.join("libwebp-test-data")),)


def cases(binary):
    """Returns each case's key and the path of its file, by part. The written part's files are written under the
    harness's directory once."""
    found = []
    for part, directory in PARTS:
        root = os.path.join(corpus.ROOT, "third_party", directory)
        files = []
        for walk, _, names in os.walk(root):
            for name in names:
                if name.endswith(".webp"):
                    path = os.path.join(walk, name)
                    files.append((part + "/" + os.path.relpath(path, root).replace(os.sep, "/"), path))
        found.append((part, sorted(files)))
    written = os.path.join(os.path.dirname(binary), "webp_written")
    if os.path.exists(binary) and not os.path.isdir(written):
        subprocess.run([binary, "--write", written], check=True)
    files = []
    for walk, _, names in os.walk(written):
        for name in names:
            path = os.path.join(walk, name)
            files.append(("written/" + os.path.relpath(path, written).replace(os.sep, "/"), path))
    found.append(("written", sorted(files)))
    return found


def answers(binary, paths, times):
    """Returns the harness's answer for the file at each of `paths`. A file that ends the harness is answered with the
    status Crash."""
    options = ["--time"] if times else []
    results = []
    for start in range(0, len(paths), BATCH):
        batch = paths[start:start + BATCH]
        done = subprocess.run([binary, *options, *batch], stdout=subprocess.PIPE)
        lines = done.stdout.decode("utf-8", "replace").splitlines()
        if done.returncode == 0 and len(lines) == len(batch):
            results += [json.loads(line) for line in lines]
            continue
        for path in batch:
            done = subprocess.run([binary, *options, path], stdout=subprocess.PIPE)
            lines = done.stdout.decode("utf-8", "replace").splitlines()
            if done.returncode == 0 and len(lines) == 1:
                results.append(json.loads(lines[0]))
            else:
                results.append({"status": "Crash", "reference": False, "frames": 0, "bytes": 0, "differ": 0,
                                "largest": 0, "hidden": 0, "planes": "none"})
    return results


def passes(result):
    """Returns true if lib/webp decodes the file as libwebp does, or refuses it as libwebp does."""
    decoded = result["status"] == ""
    if decoded != result["reference"]:
        return False
    return not decoded or (result["differ"] == 0 and result["planes"] != "differ")


def main():
    ap = corpus.arguments(corpus.binary("webp_compare"), None)
    ap.add_argument("--time", action="store_true", help="print what each decoder takes for each file")
    args = ap.parse_args()
    suite = corpus.Data("webp")
    parts = cases(args.binary)
    if not parts[0][1]:
        sys.exit("WebP corpora missing; run ./dev.sh fetch libwebp_test_data")
    keys = [key for _, files in parts for key, _ in files]
    chosen = [(part, [(key, path) for key, path in files if suite.runs(key, args.match)]) for part, files in parts]
    if not any(files for _, files in chosen):
        sys.exit("no webp cases selected")
    if args.list:
        return corpus.listing([(part, files) for part, files in chosen])
    corpus.require(args.binary, "webp")

    outcome = corpus.Results(args, suite, keys=set(keys))
    totals = {"files": 0, "frames": 0, "bytes": 0, "planes": 0, "hidden": 0, "refused": 0}
    spent = {"prax": 0.0, "libwebp": 0.0}
    for part, files in chosen:
        results = answers(args.binary, [path for _, path in files], args.time)
        for (key, _), result in zip(files, results):
            outcome.case(key, int(passes(result)), status="CRASH" if result["status"] == "Crash" else corpus.OK,
                         detail=json.dumps(result), part=part)
            totals["files"] += 1
            totals["frames"] += result["frames"]
            totals["bytes"] += result["bytes"]
            totals["planes"] += 1 if result["planes"] == "equal" else 0
            totals["hidden"] += result["hidden"]
            totals["refused"] += 1 if result["status"] != "" and not result["reference"] else 0
            if "time" in result:
                for name in spent:
                    spent[name] += result["time"][name]
                print("%s  lib/webp %.3f ms, libwebp %.3f ms" % (key, result["time"]["prax"],
                                                                  result["time"]["libwebp"]))

    notes =["Of the %d files, %d decode to %d frames, whose %d bytes of Rgba8 are compared, and %d are refused by both" %
             (totals["files"], totals["files"] - totals["refused"], totals["frames"], totals["bytes"],
              totals["refused"]),
             "decoders. %d still lossy images' planes are compared. Of the images that say they have no alpha, %d" %
             (totals["planes"], totals["hidden"]),
             "pixels decode to alpha other than opaque in libwebp, which lib/webp takes as opaque."]
    if args.time:
        print("In all: lib/webp %.1f ms, libwebp %.1f ms" % (spent["prax"], spent["libwebp"]))
    return outcome.finish(notes=notes)


if __name__ == "__main__":
    sys.exit(main())
