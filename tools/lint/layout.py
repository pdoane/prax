#!/usr/bin/env python3
"""Lays out C, C++ and Objective-C code as lint's layout checks require (lint.layout and lint.layout_problems): four
spaces a level, each line that continues another one level past it, and lines that reach toward the column limit before
they break. Generated files are left alone: their generators lay them out.

Usage: python3 -m tools.lint.layout [path...]
"""

import argparse
import os
import sys

from tools.lint import lint
from tools.lint import rewrap

# The files whose code lint lays out.
CODE_EXTENSIONS = (".c", ".cpp", ".h", ".mm", ".m")

# The checks of lint.layout_problems, in the order that they are fixed: a line that runs too long breaks, an operator
# moves before the lines around it join, and an initializer list collapses or opens before its items join.
CHECKS = ("long", "operator", "initializer", "wrap")


def indented_text(text):
    """Returns `text` with each line that the layout decides at the indentation that it gives."""
    lines = text.split("\n")
    for number, own, expected, _ in lint.layout(text)[0]:
        if expected is not None and own != expected:
            lines[number - 1] = " " * expected + lines[number - 1].lstrip(" ")
    return "\n".join(lines)


def edited_text(text, edits):
    """Returns `text` with each edit, (start, end, replacement), applied, and those that overlap one already applied
    left for the next pass."""
    limit = len(text) + 1
    for start, end, replacement in sorted(edits, key=lambda edit: (edit[0], edit[1]), reverse=True):
        if end > limit or (end == limit and start < end):
            continue
        text = text[:start] + replacement + text[end:]
        limit = start
    return text


def laid_out_text(text):
    """Returns `text` laid out, fixing each check in turn until nothing changes."""
    while True:
        before = text
        text = indented_text(text)
        for check in CHECKS:
            edits = [edit for _, found, _, fixes in lint.layout_problems(text) if found == check for edit in fixes]
            text = indented_text(edited_text(text, edits))
        if text == before:
            return text


def fix(path):
    """Lays out `path`, and returns true if the file changed."""
    if not path.endswith(CODE_EXTENSIONS):
        return False
    with open(path, encoding="utf-8") as f:
        text = f.read()
    if lint.is_generated(text):
        return False
    result = laid_out_text(text)
    if result == text:
        return False
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(result)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("paths", nargs="*", help="files to fix, or every source that lint reads")
    args = parser.parse_args()
    paths = rewrap.sources()
    if args.paths:
        named = [os.path.abspath(p) for p in args.paths]
        paths = [p for p in paths if any(p == n or p.startswith(n + os.sep) for n in named)]
    for path in paths:
        if fix(path):
            print(os.path.relpath(path, lint.ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
