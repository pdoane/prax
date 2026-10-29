#!/usr/bin/env python3
"""Orders includes as lint's include-order check requires: within each run of includes, the file's own header, public
headers, internal headers, third-party headers and system headers, one blank line apart and each sorted, with a
duplicate dropped and one blank line after the run. A directive between two includes, such as a conditional, ends a
run, and so does text other than blank lines. Generated files are left alone: gen_common orders theirs as it writes
them.

Usage: python3 -m tools.lint.includes [path...]
"""

import argparse
import os
import sys

from tools.lint import lint
from tools.lint import rewrap


def runs(lines):
    """Yields the runs of includes as (first, last) line indices, each run bounded by includes and holding only
    includes and blank lines."""
    start = None
    last = None
    for index, line in enumerate(lines):
        if lint.INCLUDE.match(line):
            if start is None:
                start = index
            last = index
        elif line.strip() and start is not None:
            yield start, last
            start = None
    if start is not None:
        yield start, last


def ordered(path, lines, first, last, own):
    """Returns the lines of the run from `first` to `last`, grouped and sorted."""
    sections = [[] for _ in lint.INCLUDE_SECTIONS]
    seen = set()
    for line in lines[first:last + 1]:
        match = lint.INCLUDE.match(line)
        if not match:
            continue
        bracket, name = match.group(1), match.group(2).strip()
        if (bracket, name) in seen:
            continue
        seen.add((bracket, name))
        section = lint.include_section(name, bracket, own)
        sections[section].append((bracket, name, line.rstrip()))
    result = []
    for number, section in enumerate(sections):
        if not section:
            continue
        if number == 0:
            section.sort(key=lambda entry: own.index(entry[1]))
        else:
            section.sort(key=lambda entry: lint.include_sort_key(entry[1], entry[0]))
        if result:
            result.append("")
        result.extend(entry[2] for entry in section)
    return result


def ordered_text(path, text):
    """Returns `text`, the contents of `path`, with its includes ordered."""
    lines = text.split("\n")
    names = [m.group(2).strip() for m in (lint.INCLUDE.match(line) for line in lines) if m]
    own = lint.own_headers(path, names)
    for first, last in reversed(list(runs(lines))):
        run = ordered(path, lines, first, last, own)
        lines[first:last + 1] = run
        # A run that lost its last section leaves the blank line before it beside the one that follows the run.
        end = first + len(run)
        while end + 1 < len(lines) and not lines[end].strip() and not lines[end + 1].strip():
            del lines[end]
    return "\n".join(lines)


def fix(path):
    """Orders the includes of `path`, and returns true if the file changed. A generated file is left to its generator,
    which orders its includes as it writes it."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    if lint.is_generated(text):
        return False
    result = ordered_text(path, text)
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
