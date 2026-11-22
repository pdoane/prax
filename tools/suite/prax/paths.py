#!/usr/bin/env python3
"""Runs the path library's fills and combinations over the shapes of suites/prax-paths.

Each file is a case, `<directory>/<file>`, and its directory names what the path_check harness checks of it
(tools/suite/prax/path_check.cpp): `polygons` the tessellator's triangles, `curves` the sparse rasterizer's spans, and
`pairs` the combinations of two shapes. A part of the suite is a directory.

The run fails on a case that fails without an entry in the errata, a recorded case that passes, or an entry that names
no case.
"""

import os
import sys

from tools.suite import corpus

SUITE = os.path.join(corpus.ROOT, "suites", "prax-paths")
PARTS = ("polygons", "curves", "pairs")

def main():
    args = corpus.arguments(corpus.binary("path_check"), SUITE).parse_args()
    data = corpus.Data("prax-paths")
    parts = [(part, [corpus.Check("%s/%s" % (part, file), (os.path.join(SUITE, part, file),))
                     for file in sorted(os.listdir(os.path.join(SUITE, part)))]) for part in PARTS]
    if args.list:
        return corpus.listing(parts)
    corpus.require(args.binary, "prax-paths")
    return corpus.check_cases(args, data, parts, batch=8)

if __name__ == "__main__":
    sys.exit(main())
