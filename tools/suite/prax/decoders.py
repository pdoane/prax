#!/usr/bin/env python3
"""Runs the decoders over the damaged inputs of suites/prax-decoders.

Each file under a format's directory is two cases: `flips/<file>`, every byte flipped in turn, and `cut/<file>`, every
proper prefix. Each font that fonts.txt names, from the build's test fonts, is a case for each table that it names,
`<table>/fonts/<font>`. The decoder_check harness checks each case (tools/suite/prax/decoder_check.cpp). A part of the
suite is a directory, and `fonts`.

The run fails on a case that fails without an entry in the errata, a recorded case that passes, or an entry that names
no case.
"""

import os
import sys

from tools.suite import corpus

SUITE = os.path.join(corpus.ROOT, "suites", "prax-decoders")
FONTS = os.path.join(corpus.BUILD, "test_data", "fonts")
# The formats' directories, which hold their files.
FORMATS = ("brotli", "lzfse", "webp", "zstd")

def cases():
    """Returns each part's cases, in the order of their keys."""
    parts = []
    for name in FORMATS:
        directory = os.path.join(SUITE, name)
        found = []
        for file in sorted(os.listdir(directory)):
            path = os.path.join(directory, file)
            found += [corpus.Check("%s/%s/%s" % (check, name, file), (check, path)) for check in ("flips", "cut")]
        parts.append((name, found))
    fonts = []
    with open(os.path.join(SUITE, "fonts.txt"), encoding="utf-8") as f:
        for line in f:
            words = line.split("#", 1)[0].split()
            if words:
                fonts += [corpus.Check("%s/fonts/%s" % (tag, words[0]), ("table:" + tag, os.path.join(FONTS, words[0])))
                          for tag in words[1:]]
    parts.append(("fonts", fonts))
    return parts


def main():
    args = corpus.arguments(corpus.binary("decoder_check"), SUITE).parse_args()
    data = corpus.Data("prax-decoders")
    parts = cases()
    if args.list:
        return corpus.listing(parts)
    corpus.require(args.binary, "prax-decoders")
    return corpus.check_cases(args, data, parts)

if __name__ == "__main__":
    sys.exit(main())
