#!/usr/bin/env python3
"""Compares lib/zstd with Zstandard's reference library: lib/zstd's decoder over the frames that the reference encoder
writes, and the reference decoder over the frames that lib/zstd's encoder writes.

The corpus is data of each kind that an encoder codes differently, at sizes from nothing to several blocks, which the
zstd_compare harness makes and lists (tools/suite/zstd/zstd_compare.cpp). A case's key is its part and its data, as
decode/words.70000.19, the reference's frame of 70,000 bytes of words at level 19. A part of the suite is the first
word of a key.

The run fails on a case that fails without an entry in the errata, a recorded case that passes, or an entry that names
no case.
"""

import subprocess
import sys

from tools.suite import corpus


def main():
    args = corpus.arguments(corpus.binary("zstd_compare"), None).parse_args()
    data = corpus.Data("zstd")
    corpus.require(args.binary, "zstd")
    listed = subprocess.run([args.binary, "--list"], stdout=subprocess.PIPE, check=True).stdout.decode("utf-8").split()
    parts = {}
    for key in listed:
        parts.setdefault(key.split("/", 1)[0], []).append(corpus.Check(key, [key]))
    parts = sorted(parts.items())
    if args.list:
        return corpus.listing(parts)
    return corpus.check_cases(args, data, parts, batch=8)


if __name__ == "__main__":
    sys.exit(main())
