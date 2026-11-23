#!/usr/bin/env python3
"""Decodes the QOI format's reference images with lib/image, each beside its PNG.

The corpus is the `qoi_test_images` checkout, qoiformat.org's archive of each image as QOI and as PNG. A decode passes
if it is its PNG exactly. The comparison is tools/suite/decode_compare.py's.
"""

import os
import sys

from tools.suite import corpus
from tools.suite import decode_compare

ROOT = os.path.join(corpus.ROOT, "third_party", "qoi-test-images", "qoi_test_images")


def parts():
    """Returns the suite's one part, with its cases."""
    if not os.path.isdir(ROOT):
        return []
    return [("qoi", [decode_compare.Case(name, os.path.join(ROOT, name), [os.path.join(ROOT, name[:-4] + ".png")])
                     for name in sorted(os.listdir(ROOT)) if name.endswith(".qoi")])]


if __name__ == "__main__":
    sys.exit(decode_compare.run("qoi", parts(), "qoi_test_images"))
