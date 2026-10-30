#!/usr/bin/env python3
"""Compares lib/markdown's HTML with cmark-gfm's on Markdown files, beyond what the specifications' examples cover.

Each file named, or each .md file under a directory named, is parsed with GFM's extensions and footnotes by both, and a
file whose HTML differs is listed with the start of the difference. With no paths, the files are the repository's own
Markdown and that of the checkouts under third_party. The engine and cmark-gfm differ where CommonMark 0.31.2 changed
what GFM 0.29 says, so a difference is a lead to look at, not a failure, and the run exits 0.
"""

import argparse
import difflib
import os
import sys

from tools.suite import corpus

# The extensions that both parse, as cmark-gfm names them.
EXTENSIONS = b"table,strikethrough,autolink,tagfilter,tasklist,footnotes"


def markdown_files(paths):
    """Returns the Markdown files that `paths` name: each file, and each .md file under each directory."""
    files = []
    for path in paths:
        if os.path.isdir(path):
            for directory, _, names in os.walk(path):
                files += [os.path.join(directory, name) for name in sorted(names) if name.endswith(".md")]
        elif os.path.isfile(path):
            files.append(path)
    return sorted(set(files))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("paths", nargs="*", help="Markdown files, or directories to find them in")
    ap.add_argument("--shown", type=int, default=10, help="how many differing files to show")
    ap.add_argument("--lines", type=int, default=12, help="how many lines of each difference to show")
    args = ap.parse_args()
    paths = args.paths or [os.path.join(corpus.ROOT, name) for name in ("README.md", "docs", "third_party")]
    files = markdown_files(paths)
    if not files:
        sys.exit("no Markdown files")
    engine = corpus.binary("markdown_spec")
    reference = corpus.binary("cmark_gfm_reference")
    corpus.require(engine, "markdown")
    if not os.path.isfile(reference):
        sys.exit("%s missing; run ./dev.sh build" % reference)

    sources = []
    for path in files:
        with open(path, "rb") as f:
            sources.append(f.read())
    ours = corpus.answers([engine], b"".join(corpus.frame(s, EXTENSIONS, b"whole") for s in sources), len(sources))
    theirs = corpus.answers([reference], b"".join(corpus.frame(s, EXTENSIONS) for s in sources), len(sources))

    differing = []
    for path, a, b in zip(files, ours, theirs):
        if a["html"] != b["html"]:
            differing.append((path, a["html"], b["html"]))
    for path, a, b in differing[:args.shown]:
        print(os.path.relpath(path, corpus.ROOT))
        diff = difflib.unified_diff(b.splitlines(), a.splitlines(), "cmark-gfm", "lib/markdown", lineterm="", n=1)
        for line in list(diff)[:args.lines]:
            print("  " + line)
    print("%d of %d files give the same HTML" % (len(files) - len(differing), len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
