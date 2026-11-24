#!/usr/bin/env python3
"""Compares lib/exr's decoder with OpenEXR's core library over the OpenEXR project's test images, and files that
OpenEXR's encoder writes in every compression and layout.

The corpora are the `openexr_images` checkout, whose files are images of each layout, channel type and compression of
the file format, and damaged files of its fuzzing; the recompressed part: some of the checkout's images written again
by OpenEXR's encoder in scan lines and in tiles, in each of its compressions; and the written part: images that the
harness makes, of channels of each pixel type, subsampled chroma and layers, which OpenEXR's encoder writes in scan
lines, tiles, mip chains and rip maps, in each of its compressions (WRITTEN). The last two are written under the
build's directory. The
exr_compare harness decodes each file with both decoders (tools/suite/exr/exr_compare.cpp): every channel of every
level of every part, in the form that it is stored in.

A case is one file, and its key is its path under its corpus's part, as openexr-images/ScanLines/Desk.exr, or for the
recompressed part the source's name, the compression and the layout, as recompressed/Desk.piz.tiles.exr, and for the
written part the image's channels and extent, the compression and the layout, as
written/chroma.84x70.piz.scanlines.exr. It passes if
the two decoders agree that it decodes or that it does not, and if every sample that both decode is OpenEXR's, bit for
bit. A file whose only parts are deep data, which lib/exr does not read, passes if lib/exr refuses it as
NotSupported. The samples of a data window outside its display window, which lib/exr's first level does not hold,
are not compared; the notes count them.

The run fails on a case that fails without an entry in the errata, a recorded case that passes, or an entry that names
no case.
"""

import json
import os
import subprocess
import sys

from tools.suite import corpus

# The parts of the suite drawn from checkouts: the name of each, and its corpus under third_party.
PARTS = (("openexr-images", "openexr-images"),)

# The images of the checkout that the recompressed part writes again, the compressions, by the names that the harness
# reads, and the layouts.
SOURCES = ("ScanLines/Desk.exr", "ScanLines/Tree.exr", "TestImages/WideColorGamut.exr", "LuminanceChroma/Garden.exr",
           "MultiView/Balls.exr")
COMPRESSIONS = ("none", "rle", "zips", "zip", "piz", "pxr24", "b44", "b44a", "dwaa", "dwab")
LAYOUTS = ("scanlines", "tiles")

# The images that the written part makes, by their channels and extent, and the layouts that each is written in, in
# every compression: extents that leave partial blocks and tiles, subsampled chroma, layers, and levels of whole tiles,
# since OpenEXR's encoder writes the partial tiles of a level past the first at the extent that they have in the first.
WRITTEN = (("rgba", 83, 71, ("scanlines",)), ("chroma", 84, 70, ("scanlines",)), ("rgba", 150, 97, ("tiles",)),
           ("rgba", 256, 128, ("mip", "rip")), ("layers", 77, 45, ("scanlines", "tiles")))


def is_image(directory, name):
    """Returns true if a file of the checkouts is an OpenEXR file: by its extension, or as any file of the damaged files
    of OpenEXR's fuzzing."""
    return name.endswith(".exr") or os.path.basename(directory) == "Damaged"


def cases(binary):
    """Returns each case's key and the path of its file, by part. The recompressed part's files are written under the
    harness's directory once."""
    found = []
    for part, directory in PARTS:
        root = os.path.join(corpus.ROOT, "third_party", directory)
        files = []
        for walk, _, names in os.walk(root):
            if ".git" in walk.split(os.sep):
                continue
            for name in names:
                if is_image(walk, name):
                    path = os.path.join(walk, name)
                    files.append((part + "/" + os.path.relpath(path, root).replace(os.sep, "/"), path))
        found.append((part, sorted(files)))
    images = os.path.join(corpus.ROOT, "third_party", "openexr-images")
    streams = os.path.join(os.path.dirname(binary), "exr_streams")
    files = []
    if os.path.isdir(images) and os.path.exists(binary):
        os.makedirs(streams, exist_ok=True)
        for source in SOURCES:
            stem = os.path.splitext(os.path.basename(source))[0]
            for compression in COMPRESSIONS:
                for layout in LAYOUTS:
                    name = "%s.%s.%s.exr" % (stem, compression, layout)
                    path = os.path.join(streams, name)
                    if not os.path.exists(path):
                        subprocess.run([binary, "--encode", compression, layout, os.path.join(images, source), path])
                    if os.path.exists(path):
                        files.append(("recompressed/" + name, path))
    found.append(("recompressed", sorted(files)))
    files = []
    if os.path.exists(binary):
        os.makedirs(streams, exist_ok=True)
        for channels, width, height, layouts in WRITTEN:
            for compression in COMPRESSIONS:
                for layout in layouts:
                    name = "%s.%dx%d.%s.%s.exr" % (channels, width, height, compression, layout)
                    path = os.path.join(streams, name)
                    if not os.path.exists(path):
                        subprocess.run([binary, "--make", channels, str(width), str(height), compression, layout, path])
                    if os.path.exists(path):
                        files.append(("written/" + name, path))
    found.append(("written", sorted(files)))
    return found


def answer(binary, path):
    """Returns the harness's answer for the file at `path`. A file that ends the harness is answered with the status
    Crash."""
    done = subprocess.run([binary, path], stdout=subprocess.PIPE)
    lines = done.stdout.decode("utf-8", "replace").splitlines()
    if done.returncode == 0 and len(lines) == 1:
        return json.loads(lines[0])
    return {"status": "Crash", "reference": False, "parts": 0, "compared": 0, "differ": 0, "outside": 0,
            "shapes": False, "deep": 0, "compressions": []}


def passes(result):
    """Returns true if lib/exr decodes the file as OpenEXR does, or refuses it as OpenEXR does."""
    decoded = result["status"] == ""
    readable = result["parts"] - result["deep"]
    if result["status"] == "NotSupported" and result["reference"] and readable == 0:
        return True
    if decoded != result["reference"]:
        return False
    return not decoded or (result["differ"] == 0 and result["shapes"])


def main():
    ap = corpus.arguments(corpus.binary("exr_compare"), None)
    args = ap.parse_args()
    suite = corpus.Data("exr")
    parts = cases(args.binary)
    if not parts[0][1]:
        sys.exit("OpenEXR corpora missing; run ./dev.sh fetch openexr_images")
    keys = [key for _, files in parts for key, _ in files]
    chosen = [(part, [(key, path) for key, path in files if suite.runs(key, args.match)]) for part, files in parts]
    if not any(files for _, files in chosen):
        sys.exit("no exr cases selected")
    if args.list:
        return corpus.listing([(part, files) for part, files in chosen])
    corpus.require(args.binary, "exr")

    outcome = corpus.Results(args, suite, keys=set(keys))
    totals = {"files": 0, "compared": 0, "outside": 0, "refused": 0, "deep": 0}
    compressions = {}
    for part, files in chosen:
        for key, path in files:
            result = answer(args.binary, path)
            outcome.case(key, int(passes(result)), status="CRASH" if result["status"] == "Crash" else corpus.OK,
                         detail=json.dumps(result), part=part)
            totals["files"] += 1
            totals["compared"] += result["compared"]
            totals["outside"] += result["outside"]
            totals["refused"] += 1 if result["status"] != "" and not result["reference"] else 0
            totals["deep"] += 1 if result["status"] == "NotSupported" and result["reference"] else 0
            if result["status"] == "":
                for compression in result["compressions"]:
                    compressions[compression] = compressions.get(compression, 0) + 1

    decoded = ", ".join("%s %d" % (name, compressions[name]) for name in COMPRESSIONS if name in compressions)
    notes = ["Of the %d files, %d are refused by both decoders, and %d hold deep data alone, which lib/exr does not"
             % (totals["files"], totals["refused"], totals["deep"]),
             "read. %d samples are compared, and %d of data windows outside their display windows are not. The parts"
             % (totals["compared"], totals["outside"]),
             "that decode are, by compression: %s." % decoded]
    return outcome.finish(notes=notes)


if __name__ == "__main__":
    sys.exit(main())
