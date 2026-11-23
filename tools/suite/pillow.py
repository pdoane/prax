#!/usr/bin/env python3
"""Decodes Pillow's Targa and DDS test images with lib/image, beside the PNGs of their pictures.

The corpus is the `pillow_images` checkout, Pillow's Tests/images. A file has the PNG of the same name as its
reference, or for the Targa files of tga/common, which are named for their size, mode, origin and encoding, the PNG of
their size and mode, if there is one. A decode passes if it is its reference exactly. The comparison is
tools/suite/decode_compare.py's.
"""

import os
import re
import sys

from tools.suite import corpus
from tools.suite import decode_compare

ROOT = os.path.join(corpus.ROOT, "third_party", "pillow", "Tests", "images")


def cases(extension):
    """Returns the cases of the files of `extension`."""
    found = []
    for walk, _, names in os.walk(ROOT):
        for name in names:
            if not name.lower().endswith(extension):
                continue
            path = os.path.join(walk, name)
            stem = name[:-len(extension)]
            reference = os.path.join(walk, stem + ".png")
            common = re.match(r"^(\d+x\d+_[a-z]+)_", stem)
            if os.path.basename(walk) == "common" and common:
                reference = os.path.join(walk, common.group(1) + ".png")
            key = os.path.relpath(path, ROOT).replace(os.sep, "/")
            found.append(decode_compare.Case(key, path, [reference] if os.path.exists(reference) else []))
    return sorted(found, key=lambda case: case.key)


def parts():
    """Returns the suite's parts, its Targa and DDS files, with their cases."""
    if not os.path.isdir(ROOT):
        return []
    return [("tga", cases(".tga")), ("dds", cases(".dds"))]


if __name__ == "__main__":
    sys.exit(decode_compare.run("pillow", parts(), "pillow_images"))
