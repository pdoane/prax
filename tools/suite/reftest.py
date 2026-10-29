#!/usr/bin/env python3
"""Renders the suites whose references are images, and counts the renders that match them.

A suite here is a corpus of documents, each beside the image that it should render as:

- resvg: the resvg test suite (third_party/resvg-tests), each test against the PNG beside it. The exception is the
  tests whose title marks undefined behavior, "(UB)", whose PNG says so instead of drawing what the test should.
- lottie-web, skottie and rlottie: the animations of a Lottie player's own tests (third_party/lottie), each against the
  player that the corpus was written for: lottie-web's SVG renderer and Skottie, each in a page of tools/suite/lottie in
  the system's browser (lottie_reference), and rlottie (rlottie_reference). Each animation is compared at four whole
  frames, at each quarter of its length. rlottie's corpus leaves out the repros of its fuzzing bugs, malformed files
  that hang or crash it, which draw nothing to compare. The references are captured once per player version into
  third_party/lottie-references/<suite>/<version>. A player that reads no dotLottie archive plays the archive's first
  animation from its files extracted there.
- lottie-ios: lottie-ios's samples, each against the snapshots that lottie-ios's own tests took of its main thread
  engine, at the frames that the snapshots show.

The reftest harness renders each document with the cpu backend. It compares the render with its reference as ThorVG's
SVG compliance suite does, under that suite's limits for the corpus (tools/harness/compare.h), so the figures mean the
same as ThorVG's. tools/suite/wpt/wpt.py drives the same harness for WPT's reftests, under WPT's rules. The comparison
finds the cases that need a person to look at them, in the viewer. It does not decide them.

Each suite renders as suites/<suite>/render.txt says (tools/suite/suite_render.h): the fonts that the suite's text is
set in, in addition to an SVG document's own fonts, and the background that a Lottie animation is drawn over.

A case's key is its path within its corpus, and for a Lottie animation the index of its frame after a `:`. The suite's
data (corpus.Data) records the cases in which the engine differs from the reference by choice, and those that a plain
run leaves out. A key without the part from its `:` onward records every frame of the file. A recorded case counts as
passing, and an entry whose cases all pass anyway is stale. A plain run of the default build writes the suite's
scoreboard, which is committed with the change that alters it.
"""

import concurrent.futures
import json
import os
import re
import subprocess
import sys
import zipfile
from urllib.parse import quote

from tools.suite import corpus

THIRD_PARTY = os.path.join(corpus.ROOT, "third_party")
HARNESS = corpus.binary("reftest")

# Matches what makes a document more than a still image to render: it runs scripts, animates, handles events or plays
# media, or its reftest waits or prints.
DYNAMIC = re.compile(r"<(?:\w+:)?(script|handler|listener|animate\w*|set|audio|video|animation|"
                     r"discard)\b|reftest-wait|reftest-print", re.I)


# Matches a resvg test whose title marks its behavior as undefined.
UNDEFINED = re.compile(r"<title>[^<]*\(UB\)")


class Case:
    """A document against its reference. `size` is `<width>x<height>`, or `-` for the reference PNG's size. `time` is
    when an animation is rendered: a frame, counted from the in point, for a Lottie animation."""

    def __init__(self, key, source, reference, size="-", time=None):
        self.key = key
        self.source = source
        self.reference = reference
        self.size = size
        self.time = time


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def resvg_cases():
    root = os.path.join(THIRD_PARTY, "resvg-tests", "tests")
    if not os.path.isdir(root):
        sys.exit("resvg corpus missing; run ./dev.sh fetch resvg_tests")
    cases = []
    for directory, subdirectories, files in os.walk(root):
        subdirectories.sort()
        for name in sorted(files):
            if not name.endswith(".svg"):
                continue
            source = os.path.join(directory, name)
            reference = source[:-4] + ".png"
            if os.path.exists(reference) and not UNDEFINED.search(read(source)):
                cases.append(Case(os.path.relpath(source, root), source, reference))
    return cases


# ----------------------------------------------------------------------------------------------------------------------
# Lottie

LOTTIE = os.path.join(THIRD_PARTY, "lottie")
LOTTIE_REFERENCES = os.path.join(THIRD_PARTY, "lottie-references")

# Where in an animation the frames that a player's suite compares lie, as fractions of its length.
LOTTIE_FRACTIONS = (0, 0.25, 0.5, 0.75)

# The largest side that a player's suite renders an animation at, which both the engine and the player fit it into.
LOTTIE_LARGEST = 512


class Animation:
    """A Lottie animation of a corpus: its file, its path within the corpus, and what the suites read of it."""

    def __init__(self, source, relative, data):
        self.source = source
        self.relative = relative
        self.width = float(data["w"])
        self.height = float(data["h"])
        self.in_point = float(data.get("ip", 0))
        self.length = float(data["op"]) - self.in_point


def lottie_data(source):
    """Returns the animation that a Lottie file plays, as JSON: the file itself, or a dotLottie archive's first
    animation."""
    if not source.endswith(".lottie"):
        with open(source, encoding="utf-8") as f:
            return json.load(f)
    with zipfile.ZipFile(source) as archive:
        return json.loads(archive.read(lottie_entry(archive)))


def lottie_entry(archive):
    """Returns a dotLottie archive's first animation, as its manifest lists the animations, or else as its folders hold
    them."""
    names = archive.namelist()
    ids = []
    if "manifest.json" in names:
        ids = [a.get("id") for a in json.loads(archive.read("manifest.json")).get("animations", [])]
    for id in ids:
        for folder in ("animations/", "a/"):
            if "%s%s.json" % (folder, id) in names:
                return "%s%s.json" % (folder, id)
    for name in names:
        for folder in ("animations/", "a/"):
            if name.startswith(folder) and name.endswith(".json") and "/" not in name[len(folder):]:
                return name
    raise KeyError("no animation")


def lottie_extract(source, directory):
    """Extracts a dotLottie archive's first animation and its files to where a player reads them: the animation at the
    root, and its asset folders from the archive's root."""
    with zipfile.ZipFile(source) as archive:
        data = json.loads(archive.read(lottie_entry(archive)))
        for asset in data.get("assets", []):
            if "u" in asset:
                asset["u"] = asset["u"].lstrip("/")
        for name in archive.namelist():
            if not name.endswith("/") and not name.startswith("__MACOSX") and "/" in name:
                target = os.path.join(directory, name)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target, "wb") as f:
                    f.write(archive.read(name))
    path = os.path.join(directory, "animation.json")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f)
    return path


def lottie_animations(corpus_directory):
    """Returns the animations of a corpus, a directory of third_party/lottie, in path order. A file that is not an
    animation the engine could size is left out."""
    root = os.path.join(LOTTIE, corpus_directory)
    if not os.path.isdir(root):
        sys.exit("Lottie corpus missing; run ./dev.sh fetch lottie_corpora")
    animations = []
    for folder, subdirectories, files in os.walk(root):
        subdirectories.sort()
        for file in sorted(files):
            if not file.endswith((".json", ".lottie")):
                continue
            source = os.path.join(folder, file)
            try:
                data = lottie_data(source)
                animation = Animation(source, os.path.relpath(source, root), data)
                float(data.get("fr", 30))
            except (ValueError, KeyError, TypeError, AttributeError, UnicodeDecodeError, zipfile.BadZipFile):
                continue
            if animation.width <= 0 or animation.height <= 0 or "layers" not in data:
                continue
            animations.append(animation)
    return animations


def fitted_size(animation):
    """Returns the size that a player's suite renders the animation at: its own, fitted into the largest size."""
    scale = min(1.0, LOTTIE_LARGEST / max(animation.width, animation.height))
    return "%dx%d" % (max(1, round(animation.width * scale)), max(1, round(animation.height * scale)))


class Player:
    """A Lottie player that draws a suite's references: its corpus, a directory of third_party/lottie, and its version,
    which keys the references captured from it. `capture(cases, jobs)` captures the reference of each case. A player
    that reads only JSON plays a dotLottie archive's first animation from its files extracted beside the references."""

    def __init__(self, corpus_directory, version, capture, reads_archives=False, excluded=()):
        self.corpus = corpus_directory
        self.version = version
        self.capture = capture
        self.reads_archives = reads_archives
        self.excluded = excluded        # the corpus's directories that hold no animation to compare


def player_cases(suite, player):
    """Returns the cases of a player's suite: each animation at the whole frame at each quarter of its length, fitted
    into the largest size, against the player's render of that frame."""
    version = player.version()
    references = os.path.join(LOTTIE_REFERENCES, suite, version)
    cases = []
    for animation in lottie_animations(player.corpus):
        if animation.relative.split(os.sep)[0] in player.excluded:
            continue
        played = animation.source
        if animation.source.endswith(".lottie") and not player.reads_archives:
            played = os.path.join(references, "extracted", animation.relative, "animation.json")
        for index, fraction in enumerate(LOTTIE_FRACTIONS):
            frame = round(fraction * max(animation.length, 0))
            case = Case("%s:%d" % (animation.relative, index), animation.source,
                        os.path.join(references, "%s@%d.png" % (animation.relative, index)), fitted_size(animation),
                        "%d" % frame)
            case.played = played
            case.animation = animation
            cases.append(case)
    return cases


def capture_missing(suite, cases, jobs, binary_name, frame_of):
    """Captures the reference of each case that has none yet with the harness `binary_name`, whose case is the lines
    that `frame_of(case)` returns. The frames of one animation go to one harness."""
    missing = [c for c in cases if not os.path.exists(c.reference)]
    if not missing:
        return
    binary = corpus.binary(binary_name)
    corpus.require(binary, suite)
    print("capturing %d references with %s" % (len(missing), binary_name), file=sys.stderr)
    for case in missing:
        os.makedirs(os.path.dirname(case.reference), exist_ok=True)
        if case.played != case.source and not os.path.exists(case.played):
            lottie_extract(case.source, os.path.dirname(case.played))

    def capture(batch):
        framed = b"".join(corpus.frame("\n".join(frame_of(c)).encode()) for c in batch)
        return corpus.answers([binary], framed, len(batch))

    size = max(1, (len(missing) + jobs - 1) // jobs)
    size += -size % len(LOTTIE_FRACTIONS)
    batches = [missing[i:i + size] for i in range(0, len(missing), size)]
    with concurrent.futures.ThreadPoolExecutor(len(batches)) as pool:
        for batch, answers in zip(batches, pool.map(capture, batches)):
            for case, answer in zip(batch, answers):
                if answer["status"]:
                    print("no reference for %s: %s" % (case.key, answer["status"]), file=sys.stderr)


def page_url(page, played):
    """Returns the URL of a player page of tools/suite/lottie that plays the animation at `played`."""
    path = os.path.join(corpus.ROOT, "tools", "suite", "lottie", page)
    return "file://" + quote(path) + "?src=" + quote("file://" + quote(played), safe="")


def browser_capture(suite, page):
    """Returns the capture of a player that a page of tools/suite/lottie plays in the system's browser. At most four
    browsers run at once."""
    def capture(cases, jobs):
        capture_missing(suite, cases, max(1, min(jobs, 4)), "lottie_reference",
                        lambda c: (page_url(page, c.played), c.size, c.time, c.reference))
    return capture


def rlottie_capture(cases, jobs):
    capture_missing("rlottie", cases, jobs, "rlottie_reference", lambda c: (c.played, c.size, c.time, c.reference))


def version_file(path):
    """Returns the version that a fetched player records in third_party/<path>."""
    def version():
        file = os.path.join(THIRD_PARTY, path)
        if not os.path.exists(file):
            sys.exit("%s missing; run ./dev.sh fetch" % os.path.relpath(file, corpus.ROOT))
        return read(file).strip()
    return version


def checkout_commit(path):
    """Returns the commit of the checkout third_party/<path>."""
    def version():
        return subprocess.run(["git", "-C", os.path.join(THIRD_PARTY, path), "rev-parse", "HEAD"], check=True,
                              stdout=subprocess.PIPE, text=True).stdout.strip()
    return version


# The players of the suites that capture their references: each corpus and the player that it was written for.
PLAYERS = {
    "lottie-web": Player("lottie-web", version_file("lottie-web/version"), browser_capture("lottie-web", "player.html")),
    "skottie": Player("skia/resources/skottie", version_file("canvaskit/version"),
                      browser_capture("skottie", "skottie.html")),
    "rlottie": Player("rlottie/example/resource", checkout_commit("lottie/rlottie"), rlottie_capture,
                      excluded=("test",)),
}

# lottie-ios's samples, and the snapshots of them that its tests took with the engine whose snapshots it compares.
LOTTIE_IOS_SAMPLES = "lottie-ios/Tests/Samples"
LOTTIE_IOS_SNAPSHOTS = os.path.join(LOTTIE, "lottie-ios", "Tests", "__Snapshots__", "SnapshotTests")
LOTTIE_IOS_ENGINE = "testMainThreadRenderingEngine"


def lottie_ios_snapshots(engine):
    """Returns the snapshots that lottie-ios's tests took with `engine`, by the sample they show, as the name the tests
    give it (its path within the samples, without its extension and with dashes for slashes), each as a list of
    (frame, kind, file), where kind is "progress" for a frame given as a share of the length, in percent, or "frame"."""
    if not os.path.isdir(LOTTIE_IOS_SNAPSHOTS):
        sys.exit("lottie-ios's snapshots missing; run ./dev.sh fetch lottie_corpora")
    snapshots = {}
    for file in sorted(os.listdir(LOTTIE_IOS_SNAPSHOTS)):
        if not file.startswith(engine + ".") or not file.endswith(".png"):
            continue
        name, _, at = file[len(engine) + 1:-4].rpartition("-")
        kind = "progress"
        if name.endswith("-Frame"):
            name, kind = name[:-len("-Frame")], "frame"
        try:
            snapshots.setdefault(name, []).append((int(at), kind, os.path.join(LOTTIE_IOS_SNAPSHOTS, file)))
        except ValueError:
            continue
    return snapshots


# How far before the out point the engine draws a snapshot at a progress of 100%, in frames.
LOTTIE_IOS_END = 1e-3


def lottie_ios_cases():
    """Returns the cases of the lottie-ios suite: each sample at each frame that a snapshot shows, at the snapshot's
    size. A progress is a frame along the animation's length, which lottie-ios draws between whole frames. A progress
    of 100% is the out point, where lottie-ios still draws the layers that end there, as the end of the animation. A
    layer is drawn before its out point, so the engine draws the instant before it, as Skottie clamps a frame there."""
    snapshots = lottie_ios_snapshots(LOTTIE_IOS_ENGINE)
    cases = []
    for animation in lottie_animations(LOTTIE_IOS_SAMPLES):
        name = os.path.splitext(animation.relative)[0].replace(os.sep, "-")
        frames = []
        for at, kind, file in snapshots.get(name, []):
            frame = at - animation.in_point if kind == "frame" else at / 100 * animation.length
            if kind == "progress" and at == 100:
                frame = max(animation.length - LOTTIE_IOS_END, 0)
            frames.append((frame, file))
        for index, (frame, file) in enumerate(sorted(frames)):
            case = Case("%s:%d" % (animation.relative, index), animation.source, file, "-", "%.6f" % frame)
            case.animation = animation
            cases.append(case)
    return cases


def top_module(suite):
    """Returns the part of a Lottie suite that a key reports under: the corpus's directory that holds the animation,
    or the suite's name for an animation at the corpus's root."""
    return lambda key: key.split("/")[0] if "/" in key else suite


class Suite:
    """A suite: its cases, the part of the suite that a case reports under, ThorVG's limits for the corpus, and the
    capture of any references that the run must make first."""

    def __init__(self, cases, module_of, limits, capture=None):
        self.cases = cases
        self.module_of = module_of
        self.limits = limits
        self.capture = capture


# Each suite. The limits are ThorVG's (thorvg.svg-compliance config/suites.json): the mean absolute error of the
# blurred images, the share of pixels that differ, and the share of pixels that either image draws on.
LIMITS = (5.0, 0.02, 0.15)
SUITES = {
    "resvg": Suite(resvg_cases, lambda key: "/".join(key.split("/")[:2]), LIMITS),
    "lottie-ios": Suite(lottie_ios_cases, top_module("lottie-ios"), LIMITS),
}
for name, player in PLAYERS.items():
    SUITES[name] = Suite(lambda name=name, player=player: player_cases(name, player), top_module(name), LIMITS,
                         player.capture)


def write_stem(directory, key):
    """Returns where the renders of the case `key` are written under `directory`: the path of its file without the
    extension, and the index of a case of a file after an `@`. A render adds `.png`, its reference `-ref.png` and their
    difference `-diff.png`."""
    name, _, index = key.partition(":")
    return os.path.join(directory, os.path.splitext(name)[0] + ("@" + index if index else ""))


def run(binary, cases, write, jobs, settings=None, text=()):
    """Returns the harness's answer to each case. The cases are split among `jobs` harnesses that render as the
    suite's `settings` say, with the text backends that the options `text` name (corpus.text_backends), or the
    harness's defaults. If `write` names a directory, each render, reference render and difference is written there."""
    command = [binary] + (["--render", settings] if settings else []) + list(text)
    def frame(case):
        render = reference = diff = "-"
        if write:
            stem = write_stem(write, case.key)
            os.makedirs(os.path.dirname(stem), exist_ok=True)
            render, reference, diff = stem + ".png", stem + "-ref.png", stem + "-diff.png"
        lines = (case.source, case.reference, case.size, render, reference, diff)
        if case.time is not None:
            lines += (case.time,)
        return corpus.frame("\n".join(lines).encode())

    def answer(batch):
        return corpus.answers(command, b"".join(frame(c) for c in batch), len(batch))

    if not cases:
        return []
    size = max(1, (len(cases) + jobs - 1) // jobs)
    batches = [cases[i:i + size] for i in range(0, len(cases), size)]
    with concurrent.futures.ThreadPoolExecutor(len(batches)) as pool:
        return [a for answers in pool.map(answer, batches) for a in answers]


def within(answer, limits):
    """Returns true if a render is within the limits of its reference."""
    mae, changed, content = limits
    return (answer["status"] == "" and "mae" in answer and answer["mae"] <= mae
            and answer["changed"] <= changed and answer["content"] <= content)


def main():
    ap = corpus.arguments(HARNESS, None)
    ap.add_argument("suite", choices=list(SUITES), help="which suite to run")
    ap.add_argument("--failures", action="store_true", help="list every case that fails")
    ap.add_argument("--write", metavar="DIR", help="write each render, its reference and their difference here")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4,
                    help="how many harnesses run at once")
    args = ap.parse_args()
    suite = SUITES[args.suite]
    data = corpus.Data(args.suite)
    every = suite.cases()
    cases = [c for c in every if data.runs(c.key, args.match)]
    if not cases:
        sys.exit("no cases matched %s" % args.match)
    left = sum(1 for c in every if corpus.selected(c.key, args.match)) - len(cases)
    if args.list:
        print("%d cases" % len(cases))
        return 0
    corpus.require(args.binary, args.suite)
    if suite.capture is not None:
        suite.capture(cases, args.jobs)

    modules = {}
    failing = []
    recorded_cases = 0
    # Each entry that the run reaches, and whether any of its cases fails.
    entries = {}
    outcomes = {}
    for case, answer in zip(cases, run(args.binary, cases, args.write, max(1, args.jobs),
                                   data.optional("render.txt"), corpus.text_backends(args))):
        ok = within(answer, suite.limits)
        outcomes[case.key] = 1 if ok else 0
        entry = case.key if case.key in data.errata else corpus.file_of(case.key)
        recorded = entry in data.errata
        if recorded:
            entries[entry] = entries.get(entry, False) or not ok
            recorded_cases += 1
        counts = modules.setdefault(suite.module_of(case.key), [0, 0])
        counts[0] += 1 if ok or recorded else 0
        counts[1] += 1
        if not ok and not recorded:
            failing.append((case, answer))

    rows = [(name, *counts) for name, counts in sorted(modules.items())]
    total = ("TOTAL", sum(c[0] for c in modules.values()), sum(c[1] for c in modules.values()))
    width = max(len(name) for name, _, _ in rows + [total])
    for name, passed, count in rows + [total]:
        print("%s  %5d/%d" % (name.ljust(width), passed, count))
    print("\n%d of %d cases pass, %d of them recorded in the errata%s."
          % (total[1], total[2], recorded_cases, "; %d left out as cut or deferred" % left if left else ""))
    statuses = {}
    for _, answer in failing:
        if answer["status"]:
            statuses[answer["status"]] = statuses.get(answer["status"], 0) + 1
    for status, count in sorted(statuses.items()):
        print("%d cases answered %s" % (count, status))
    corpus.outcomes(args, outcomes)
    if args.failures:
        for case, answer in failing:
            print("  %s  changed %.4f, content %.4f, mae %.3f%s" % (
                case.key, answer.get("changed", 1), answer.get("content", 1),
                answer.get("mae", 255), "  " + answer["status"] if answer["status"] else ""))
    stale = ["%s passes; drop its entry" % key for key, fails in sorted(entries.items()) if not fails]
    if args.match is None:
        stale += ["%s names no case (%s)" % (key, kind) for kind, key in data.stale({c.key for c in every})]
    for line in stale:
        print("stale: " + line)
    if corpus.writes_scoreboard(args):
        data.write_scoreboard(rows)
    return 1 if stale else 0


if __name__ == "__main__":
    sys.exit(main())
