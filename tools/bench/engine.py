"""Runs the engine benchmarks. Their harnesses measure what only the engine exposes, so no browser runs them. Each run
takes the parsed arguments, with the directory of the harnesses as `args.bin`, and returns its rows by engine and the
files that it read. tools/bench/registry.py says what each benchmark measures, and the harness's source says how."""

import concurrent.futures
import glob
import json
import os
import subprocess
import sys

from tools.bench import common

def by_engine(args, harness, extra):
    """Runs a harness whose rows each name the engine that measured them, and returns the rows by engine, without that
    member."""
    engines = {}
    for row in common.run_json([common.binary(args.bin, harness)] + extra, harness):
        engines.setdefault(row.pop("engine"), []).append(row)
    return engines


# ----------------------------------------------------------------------------------------------------------------------
# frame

# The stylesheet that the page of cards uses.
BOOTSTRAP = os.path.join(common.ROOT, "third_party", "css", "bootstrap.css")

# The number of cards on the large page. The harness's default is 100. The cost of a change should depend on what the
# change reaches, not on what the page holds, so a row whose cost grows with the page is a defect.
LARGE = 2000


def frame(args):
    if not os.path.exists(BOOTSTRAP):
        sys.exit("no stylesheets under third_party/css; run ./dev.sh fetch css")
    harness = common.binary(args.bin, "frame_bench")
    rows = common.run_json([harness, BOOTSTRAP], "frame_bench")
    for row in common.run_json([harness, "--cards", str(LARGE), BOOTSTRAP], "frame_bench"):
        # Loading is measured on the default page alone.
        if row["name"].startswith("frame "):
            row["name"] += " (%d cards)" % LARGE
            rows.append(row)
    return {"prax": rows}, [BOOTSTRAP]


# ----------------------------------------------------------------------------------------------------------------------
# lists

def lists(args):
    return {"prax": common.run_json([common.binary(args.bin, "list_bench")], "list_bench")}, []


# ----------------------------------------------------------------------------------------------------------------------
# print

def print_document(args):
    return {"prax": common.run_json([common.binary(args.bin, "print_bench")], "print_bench")}, []


# ----------------------------------------------------------------------------------------------------------------------
# ui

def ui(args):
    """Measures frames of the generated trees and, where the build has a GPU device, whole frames of the editor
    fixture (tools/bench/editor_bench)."""
    rows = common.run_json([common.binary(args.bin, "ui_bench")], "ui_bench")
    path = os.path.join(args.bin, "editor_bench")
    if os.path.exists(path):
        rows += common.run_json([path], "editor_bench")
    return {"prax": rows}, []


# ----------------------------------------------------------------------------------------------------------------------
# text-edit

def text_edit(args):
    return {"prax": common.run_json([common.binary(args.bin, "text_edit_bench")], "text_edit_bench")}, []


def text_view(args):
    return {"prax": common.run_json([common.binary(args.bin, "text_view_bench")], "text_view_bench")}, []


# ----------------------------------------------------------------------------------------------------------------------
# flex

def flex_options(parser):
    parser.add_argument("trees", nargs="*", help="trees by the start of the name; every tree by default")


def flex(args):
    """Each row is one engine's measure of a tree, filed under the engine that measured it. lib/layout's rows are filed
    as prax, beside the rows of Yoga and Taffy."""
    return by_engine(args, "flex_bench", args.trees), []


# ----------------------------------------------------------------------------------------------------------------------
# vterm

def vterm_options(parser):
    parser.add_argument("workloads", nargs="*", help="workloads by the start of the name; every workload by default")


def vterm(args):
    """Runs vterm_bench and groups its rows by engine. Each row is one engine's measure of a workload. The rows of
    lib/vterm are filed as prax, beside those of libvterm."""
    return by_engine(args, "vterm_bench", args.workloads), []


# ----------------------------------------------------------------------------------------------------------------------
# page-changes

# The number of elements changed on each page. It is large enough that each kind of change meets several kinds of
# element, and small enough that the slowest page finishes quickly.
ELEMENTS = 6

# The time limit in seconds for one page's changes. The slowest page takes a few seconds, so a page that reaches the
# limit has hung. The run names that page and fails instead of waiting for it.
DEADLINE = 60


def page_changes_options(parser):
    parser.add_argument("--pages", default=common.PAGES, help="a directory of captured pages; third_party/pages")
    parser.add_argument("--elements", type=int, default=ELEMENTS)
    parser.add_argument("--jobs", type=int, default=os.cpu_count())
    parser.add_argument("names", nargs="*", help="pages by a word of the name; every page by default")


def run_page(harness, capture, page, elements):
    """Returns the harness's report for one page, or None if the harness failed."""
    try:
        done = subprocess.run([harness, "--no-verify", "--counts", "--elements", str(elements), capture, page],
                              capture_output=True, text=True, timeout=DEADLINE)
    except subprocess.TimeoutExpired:
        print("%s: stopped after %ds" % (os.path.basename(page), DEADLINE), file=sys.stderr)
        return None
    if done.returncode != 0:
        lines = done.stderr.strip().splitlines()
        print("%s: %s" % (os.path.basename(page), lines[-1] if lines else "exited %d" % done.returncode),
              file=sys.stderr)
        return None
    return json.loads(done.stdout)


def add(total, counts):
    """Adds each measure in `counts` into `total`. Both are maps from a measure's name to its counters."""
    for key, values in counts.items():
        into = total.setdefault(key, {})
        for name, value in values.items():
            into[name] = into.get(name, 0) + value


def page_changes(args):
    """Each row is one kind of change on one page. Its counts are the total cost of the frames after those changes. The
    rows named "all" sum each kind of change over all pages. Pages run concurrently, one process each."""
    directory = args.pages
    harness = common.binary(args.bin, "invalidation")
    if not os.path.exists(os.path.join(directory, "pages.json")):
        sys.exit("no pages under %s; run ./dev.sh fetch pages" % directory)
    names = [os.path.basename(page)[:-len(".html")] for page in common.pages(directory)[:-1]]
    if args.names:
        names = [n for n in names if any(word in n for word in args.names)]

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        reports = list(pool.map(lambda n: run_page(harness, os.path.join(directory, n + ".json"),
                                                os.path.join(directory, n + ".html"), args.elements), names))
    if any(report is None for report in reports):
        common.failed = True

    rows = []
    totals = {}
    frames = {}
    for name, report in zip(names, reports):
        if report is None:
            continue
        for kind in report["kinds"]:
            rows.append({"name": "%s %s" % (name, kind["name"]), "frames": kind["frames"],
                         "time": kind["counts"]["time"]["time"], "counts": kind["counts"]})
            add(totals.setdefault(kind["name"], {}), kind["counts"])
            frames[kind["name"]] = frames.get(kind["name"], 0) + kind["frames"]
    for kind, counts in totals.items():
        rows.append({"name": "all " + kind, "frames": frames[kind], "time": counts["time"]["time"], "counts": counts})
    return {"prax": rows}, common.pages(directory) + common.captures(directory)


# ----------------------------------------------------------------------------------------------------------------------
# long-doc

DOCS = os.path.join(common.ROOT, "third_party", "docs")


def long_doc_options(parser):
    parser.add_argument("--job-budget", help="the harness's option, to compare configurations")
    parser.add_argument("--restyle-threads", help="the same")
    parser.add_argument("documents", nargs="*", help="documents under third_party/docs by name; all by default")


def long_doc(args):
    """Each row holds the cost of a measure's median sample, and the latencies of the paced load."""
    harness = common.binary(args.bin, "doc_bench")
    documents = sorted(glob.glob(os.path.join(DOCS, "*.html")))
    if args.documents:
        documents = [d for d in documents if os.path.splitext(os.path.basename(d))[0] in args.documents]
    if not documents:
        sys.exit("no documents under third_party/docs; run ./dev.sh fetch docs")
    options = []
    for flag, value in [("--job-budget", args.job_budget), ("--restyle-threads", args.restyle_threads)]:
        if value is not None:
            options += [flag, value]
    rows = []
    for document in documents:
        name = os.path.splitext(os.path.basename(document))[0]
        for row in common.run_json([harness, document] + options, "doc_bench"):
            row["name"] = name + " " + row["name"]
            rows.append(row)
    return {"prax": rows}, documents


# ----------------------------------------------------------------------------------------------------------------------
# draw


def draw_options(parser):
    parser.add_argument("--renderer", action="append", default=[], metavar="NAME",
                        help="a renderer configuration to measure; the viewer's default, fragment-tiles, by default")
    parser.add_argument("--collection", action="append", default=[], metavar="WORD",
                        help="a collection to measure, by a word of its name; the features by default")
    parser.add_argument("--pixel-ratio", type=float, default=None,
                        help="target pixels per unit of the drawings; 2 by default")
    parser.add_argument("--images", default=None, metavar="DIR",
                        help="a directory to write what each row rendered into, as a PNG named for the row")
    parser.add_argument("drawings", nargs="*", help="drawings by number or by a word of the name; all by default")


def draw(args):
    """Each row is a drawing on one renderer configuration. It holds the cost of recording the draw list, the CPU cost
    of rendering it, the time that a device spent executing it, and what the render produced. A drawing that changes
    what it draws keeps its row's name but measures something else, so the sources of the drawings are the run's
    inputs."""
    command = [common.binary(args.bin, "draw_bench")]
    for renderer in args.renderer:
        command += ["--renderer", renderer]
    for collection in args.collection:
        command += ["--collection", collection]
    if args.pixel_ratio is not None:
        command += ["--pixel-ratio", str(args.pixel_ratio)]
    if args.images is not None:
        os.makedirs(args.images, exist_ok=True)
        command += ["--images", os.path.abspath(args.images)]
    rows = common.run_json(command + args.drawings, "draw_bench")
    drawings = os.path.join(common.ROOT, "examples", "draw_features")
    return {"prax": rows}, sorted(glob.glob(os.path.join(drawings, "*.cpp")) + glob.glob(os.path.join(drawings, "*.h")))


# ----------------------------------------------------------------------------------------------------------------------
# image-decode

IMAGES = os.path.join(common.ROOT, "third_party", "wpt", "images")

# Images in the directory that the decoders refuse, as browsers do: the ones that WPT breaks on purpose.
REFUSED = {"broken.png", "undecodable.png"}


def image_decode_options(parser):
    parser.add_argument("images", nargs="*", help="images by a word of the name; all images by default")


def image_decode(args):
    """Each row is an image. It holds the image's size and pixel count, the cost of decoding it into RGBA8 and, for a
    PNG, the cost of inflating its pixel data alone."""
    files = sorted(f for f in glob.glob(os.path.join(IMAGES, "*.png")) + glob.glob(os.path.join(IMAGES, "*.jpg"))
                   if os.path.basename(f) not in REFUSED)
    if args.images:
        files = [f for f in files if any(word in os.path.basename(f) for word in args.images)]
    return {"prax": common.run_json([common.binary(args.bin, "image_bench")] + files, "image_bench")}, files


# ----------------------------------------------------------------------------------------------------------------------
# exr

# The images of OpenEXR's test images that the exr benchmark writes again in each compression, and the compressions,
# by the names that the harness reads.
EXR_IMAGES = os.path.join(common.ROOT, "third_party", "openexr-images")
EXR_SOURCES = ("ScanLines/Desk.exr", "ScanLines/StillLife.exr", "ScanLines/Tree.exr", "ScanLines/Cannon.exr")
EXR_COMPRESSIONS = ("none", "rle", "zips", "zip", "piz", "pxr24", "b44", "b44a", "dwaa", "dwab")


def exr_options(parser):
    parser.add_argument("cases", nargs="*", help="cases by a word of the name, as piz or Desk; every case by default")


def exr(args):
    """Runs exr_compare over each image in each compression and groups its rows by engine: lib/exr's filed as prax,
    beside OpenEXR's core library's, filed as openexr. Each row is one engine's cost of decoding one file. The files are
    written by OpenEXR's encoder under the build, once, from OpenEXR's test images."""
    harness = common.binary(args.bin, "exr_compare")
    cache = os.path.join(args.bin, "exr_streams")
    os.makedirs(cache, exist_ok=True)
    sources = [os.path.join(EXR_IMAGES, source) for source in EXR_SOURCES]
    if not all(os.path.exists(source) for source in sources):
        sys.exit("OpenEXR's test images are missing; run ./dev.sh fetch openexr_images")
    paths = []
    for source in sources:
        stem = os.path.splitext(os.path.basename(source))[0]
        for compression in EXR_COMPRESSIONS:
            path = os.path.join(cache, "%s.%s.scanlines.exr" % (stem, compression))
            if not os.path.exists(path):
                subprocess.run([harness, "--encode", compression, "scanlines", source, path], check=True)
            if not args.cases or any(word in os.path.basename(path) for word in args.cases):
                paths.append(path)
    return by_engine(args, "exr_compare", ["--bench"] + paths), sources


# ----------------------------------------------------------------------------------------------------------------------
# numbers

def numbers_options(parser):
    parser.add_argument("cases", nargs="*", help="cases by the start of the name, as format f64; every case by default")


def numbers(args):
    """Runs number_bench and groups its rows by engine. Each row is one engine's cost of one conversion in a case:
    lib/core's rows are filed as prax, beside those of the standard library's <charconv>, filed as std."""
    return by_engine(args, "number_bench", args.cases), []


# ----------------------------------------------------------------------------------------------------------------------
# compress

# The inputs that every format compresses: a stylesheet for text, and a font for binary data.
COMPRESS_INPUTS = [BOOTSTRAP, os.path.join(common.ROOT, "third_party", "fonts", "Arimo.ttf")]

# The size of the small input, the start of the first input: under the 4 KB below which Apple's LZFSE encoder writes
# LZVN, and the size of a stream that a font's table or an emoji's image holds.
COMPRESS_SMALL = 2048


def compress_options(parser):
    parser.add_argument("cases", nargs="*", help="cases by the start of the name, as bootstrap.css lzfse; every case by "
                        "default")


def compress(args):
    """Runs compress_bench and groups its rows by engine. Each row is one engine's cost of decoding one stream:
    lib/compress's rows are filed as prax, beside those of each format's reference library, filed as zlib, brotli and
    zstd, and of Apple's libcompression, filed as apple, on Apple's platforms."""
    inputs = [arg for path in COMPRESS_INPUTS for arg in ("--input", path)]
    return by_engine(args, "compress_bench", inputs + ["--small", str(COMPRESS_SMALL)] + args.cases), []


# The Silesia corpus, which the corpus benchmark encodes and decodes, and where it keeps the reference encoders' streams
# between runs: under the build, since they are what that build's references wrote.
SILESIA = os.path.join(common.ROOT, "third_party", "silesia")


def compress_corpus(args):
    """Runs compress_bench over the Silesia corpus, encoding as well as decoding, and groups its rows by engine as
    compress does. The rows named "all <case>" are each engine's totals over the corpus."""
    from tools.fetch import checkouts
    paths = [os.path.join(SILESIA, name) for name in checkouts.SILESIA_FILES]
    cache = os.path.join(args.bin, "compress_streams")
    os.makedirs(cache, exist_ok=True)
    inputs = [arg for path in paths for arg in ("--input", path)]
    return by_engine(args, "compress_bench", inputs + ["--encode", "--cache", cache] + args.cases), paths


# ----------------------------------------------------------------------------------------------------------------------
# format

def format_options(parser):
    parser.add_argument("cases", nargs="*", help="cases by the start of the name, as buffer float; every case by default")


def format_strings(args):
    """Each row is one engine's cost of one call in a case: pr/format.h's rows are filed as prax, beside those of the
    standard library's <format>, filed as std."""
    return by_engine(args, "format_bench", args.cases), []
