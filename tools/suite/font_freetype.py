#!/usr/bin/env python3
"""Compares lib/font's readers with FreeType over the build's test fonts.

The corpus is every TrueType and OpenType file of the test fonts that the build fetches (cmake/test_data.cmake), and
each check that applies to a font is a case, `<check>/<font>`, as the font_compare harness lists them
(tools/suite/font/font_compare.cpp): its character map, its variation sequences, its design advances and its outlines.
A part of the suite is a check, over every font that it applies to.

The run fails on a case that differs without an entry in the errata, a recorded case that agrees, or an entry that
names no case.
"""

import concurrent.futures
import os
import subprocess
import sys

from tools.suite import corpus

FONTS = os.path.join(corpus.BUILD, "test_data", "fonts")
CHECKS = ("cmap", "sequences", "advances", "outlines")


def cases(binary):
    """Returns each check's cases, over the fonts that it applies to."""
    fonts = sorted(name for name in os.listdir(FONTS) if name.endswith((".ttf", ".otf")))

    def applying(font):
        done = subprocess.run([binary, "--checks", os.path.join(FONTS, font)], stdout=subprocess.PIPE, check=True)
        return font, done.stdout.decode("utf-8").split()

    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        listed = list(pool.map(applying, fonts))
    return [(check, [corpus.Check("%s/%s" % (check, font), (check, os.path.join(FONTS, font)))
                     for font, checks in listed if check in checks]) for check in CHECKS]


def main():
    args = corpus.arguments(corpus.binary("font_compare"), FONTS).parse_args()
    data = corpus.Data("font-freetype")
    corpus.require(args.binary, "font-freetype")
    if not os.path.isdir(FONTS):
        sys.exit("no test fonts at %s; configure the build" % FONTS)
    parts = cases(args.binary)
    if args.list:
        return corpus.listing(parts)
    return corpus.check_cases(args, data, parts, batch=2)

if __name__ == "__main__":
    sys.exit(main())
