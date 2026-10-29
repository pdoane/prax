#!/usr/bin/env python3
"""Rewraps comment paragraphs so that lines break where they reach the line limit, as lint's comment-wrap check
requires, and aligns the comments beside runs of code, as its comment-align check requires.

A paragraph is a run of whole-line `//` comments at one column. A blank comment line, a list item, a line indented
past the comment's text, a row of aligned columns, a spec link and a section's title each stand on their own. If a
comment beside code moves to its run's column, its continuation lines move with it, and are refilled if they would pass
the limit. Generated files are left alone, and so is a file that is not C, C++, Objective-C or Slang, whose `//` may
be a URL's.

Usage: python3 -m tools.lint.rewrap [path...]
"""

import argparse
import glob
import os
import re
import sys

from tools.lint import lint


# The extensions of the files whose `//` comments the script rewraps.
EXTENSIONS = (".c", ".h", ".cpp", ".mm", ".slang")


def sources():
    """Returns every hand-written C, C++, Objective-C and Slang file that lint reads."""
    patterns = ["lib/**/*.[ch]", "lib/**/*.cpp", "lib/**/*.mm", "lib/**/*.slang", "examples/**/*.[ch]",
                "examples/**/*.cpp", "examples/**/*.mm", "examples/**/*.slang", "tools/**/*.[ch]", "tools/**/*.cpp",
                "tools/**/*.mm"]
    return sorted({p for pattern in patterns for p in glob.glob(os.path.join(lint.ROOT, pattern), recursive=True)})


def stands_alone(lines, number, column):
    """Returns true if the comment line at `column` is a paragraph on its own."""
    text, indented = lint.comment_words(lines[number], column)
    return (not text or indented or "  " in text or lint.LIST_ITEM.match(text) is not None
            or lint.SPEC_LINE.match(text) is not None or lint.SECTION_RULE.match(lines[number]) is not None
            or lint.SUBSECTION.match(lines[number]) is not None)


def code_comment(line):
    """Returns the column of the `//` comment beside code on `line` and the end of that code, or None."""
    code = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', lambda m: " " * len(m.group(0)), line)
    own = len(line) - len(line.lstrip())
    at = code.find("//")
    return (at, len(code[:at].rstrip())) if at > own else None


def move_comment(lines, number, column, shared):
    """Moves the comment beside code on line `number`, at `column`, and its continuation lines to `shared`. Returns
    the replacement lines and the number of lines replaced."""
    end = number + 1
    while end < len(lines) and lines[end].startswith(" " * column + "//") and lines[end][:column].strip() == "":
        end += 1
    code = lines[number][:column].rstrip()
    moved = [code.ljust(shared) + lines[number][column:]]
    moved += [" " * shared + line[column:] for line in lines[number + 1:end]]
    if all(len(line) <= lint.LINE_LIMIT for line in moved):
        return moved, end - number
    words = " ".join(line[column + 2:].strip() for line in lines[number:end]).split()
    out, line = [], code.ljust(shared) + "// " + words[0]
    for word in words[1:]:
        if len(line) + 1 + len(word) <= lint.LINE_LIMIT:
            line += " " + word
        else:
            out.append(line)
            line = " " * shared + "// " + word
    out.append(line)
    return out, end - number


def align(text):
    """Returns `text` with the comments beside each run of code lines moved to the run's column
    (lint.comment_run_column)."""
    lines = text.split("\n")
    comments = lint.line_comments(text)
    runs, run, indent = [], [], 0
    for number, line in enumerate(lines):
        stripped = line.lstrip()
        own = len(line) - len(stripped)
        found = comments.get(number)
        if stripped.startswith("//"):
            if own <= indent and run:
                runs.append(run)
                run = []
            continue
        beside = code_comment(line) if found is not None and found[1] else None
        if beside is not None and (not run or own == indent):
            run.append((number, beside[1], beside[0]))
        else:
            if run:
                runs.append(run)
            run = [(number, beside[1], beside[0])] if beside is not None else []
        indent = own if beside is not None else 0
    if run:
        runs.append(run)
    for run in reversed(runs):
        shared = lint.comment_run_column(run)
        for number, _, column in reversed(run):
            if column != shared:
                moved, count = move_comment(lines, number, column, shared)
                lines[number:number + count] = moved
    return "\n".join(lines)


def rewrap(text):
    """Returns `text` with each comment paragraph refilled to the line limit."""
    lines = text.split("\n")
    comments = lint.line_comments(text)
    out, number = [], 0
    while number < len(lines):
        found = comments.get(number)
        if found is None or found[1] or stands_alone(lines, number, found[0]):
            out.append(lines[number])
            number += 1
            continue
        column = found[0]
        end = number + 1
        while end < len(lines) and comments.get(end) == (column, False) and not stands_alone(lines, end, column):
            end += 1
        words = " ".join(lint.comment_words(lines[k], column)[0] for k in range(number, end)).split(" ")
        prefix = lines[number][:column + 2] + " "
        line = prefix + words[0]
        for word in words[1:]:
            if len(line) + 1 + len(word) <= lint.LINE_LIMIT:
                line += " " + word
            else:
                out.append(line)
                line = prefix + word
        out.append(line)
        number = end
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="*", help="files to rewrap; every source lint reads by default")
    paths = [os.path.abspath(p) for p in parser.parse_args().paths] or sources()
    for path in paths:
        if not path.endswith(EXTENSIONS):
            print("%s: not C, C++, Objective-C or Slang, left alone" % os.path.relpath(path, lint.ROOT),
                  file=sys.stderr)
            continue
        with open(path, encoding="utf-8") as f:
            text = f.read()
        if lint.is_generated(text):
            continue
        wrapped = rewrap(align(text))
        if wrapped != text:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(wrapped)
            print(os.path.relpath(path, lint.ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
