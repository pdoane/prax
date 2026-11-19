#!/usr/bin/env python3
"""Tests that a program carries only the formats that it names. The terminal names Deflate and PNG, for kitty's graphics
protocol and the emoji font's glyphs, so it carries no other format library, and loads no system library of a
platform codec. A library that chose formats for its program would bring the rest along.

Usage: python3 -m tools.build.test_formats
"""

import os
import re
import shutil
import subprocess
import unittest

from tools.suite import corpus

# The format libraries, and the codecs of the platform, by the prefix of their public functions.
FORMATS = (
    "prBrotli", "prCompressApple", "prDeflate", "prEmjc", "prExr", "prGif", "prHdr", "prIco", "prImageImageIO",
    "prJpeg", "prLz4", "prLzfse", "prPng", "prTiff", "prWebp", "prZip", "prZstd",
)

# The formats that the terminal names (apps/terminal/main.cpp).
TERMINAL = {"prDeflate", "prPng"}

# The system libraries of the platform's codecs, which a program loads only if it creates one of them.
PLATFORM_CODECS = ("libcompression", "ImageIO.framework")


def carried(path):
    """Returns the format prefixes that the binary at `path` has functions of, or None if it has no symbols to read."""
    nm = shutil.which("nm")
    if nm is None or not os.path.exists(path):
        return None
    symbols = subprocess.run([nm, path], capture_output=True, text=True, check=True).stdout
    # A stripped binary names none of Prax's functions.
    if not re.search(r"\b_?pr[A-Z]", symbols):
        return None
    return {prefix for prefix in FORMATS if re.search(r"\b_?%s[A-Z]" % prefix, symbols)}


class FormatsTest(unittest.TestCase):
    def test_terminal_carries_only_its_formats(self):
        formats = carried(corpus.binary("terminal"))
        if formats is None:
            self.skipTest("no terminal with symbols in %s" % corpus.BUILD)
        self.assertEqual(formats, TERMINAL)

    def test_terminal_loads_no_platform_codec(self):
        path = corpus.binary("terminal")
        otool = shutil.which("otool")
        if otool is None or not os.path.exists(path):
            self.skipTest("no terminal, or no otool to read its libraries")
        libraries = subprocess.run([otool, "-L", path], capture_output=True, text=True, check=True).stdout
        self.assertEqual([name for name in PLATFORM_CODECS if name in libraries], [])


if __name__ == "__main__":
    unittest.main()
