#!/usr/bin/env python3
"""Checks a prose pass: a change that rewrites comments and leaves the code alone.

For each file, compares the code with its comments removed against the same file at a git revision, token by token,
so that rewrapped or rewritten comments and moved whitespace do not count as changes but any change to the code does.
Then refills the files' comments (rewrap.py) and reports what lint finds in them.

C, C++, Objective-C and JavaScript sources compare without their `//` and `/* */` comments, and Python sources without
their `#` comments and docstrings. Other files, such as Markdown, are prose throughout and are only reported as changed.

Usage: python3 -m tools.lint.prose [--base REV | --staged] [path...]

With no paths, checks every file that differs from REV in the working tree. REV is HEAD by default. With --staged,
compares the working tree with the index, and with no paths checks the files that are staged.
"""

import argparse
import ast
import io
import os
import re
import subprocess
import sys
import tokenize

from tools.lint import lint

C_FAMILY = (".c", ".h", ".cpp", ".mm", ".js")

# A C-family token: a string or character literal, a comment, or one character of anything else that is not white
# space. Literals are matched whole so that `//` inside a string is not taken for a comment.
C_TOKEN = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|//[^\n]*|/\*.*?\*/|\S', re.S)


def c_code(text):
    """Returns the tokens of C-family source `text`, without its comments."""
    return [t for t in C_TOKEN.findall(text) if not t.startswith(("//", "/*"))]


def python_code(text):
    """Returns the tokens of Python source `text`, without its comments and docstrings."""
    docstrings = set()
    try:
        for node in ast.walk(ast.parse(text)):
            if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)) and node.body:
                first = node.body[0]
                if isinstance(first, ast.Expr) and isinstance(first.value, ast.Constant) \
                        and isinstance(first.value.value, str):
                    docstrings.add((first.lineno, first.col_offset))
    except SyntaxError:
        return None
    skip = (tokenize.COMMENT, tokenize.NL, tokenize.NEWLINE, tokenize.INDENT, tokenize.DEDENT, tokenize.ENCODING)
    tokens = []
    for token in tokenize.generate_tokens(io.StringIO(text).readline):
        if token.type in skip or (token.type == tokenize.STRING and token.start in docstrings):
            continue
        tokens.append(token.string)
    return tokens


def code_of(path, text):
    """Returns the tokens of `text` without its comments, or None for a file that is prose throughout."""
    if path.endswith(C_FAMILY):
        return c_code(text)
    if path.endswith(".py"):
        return python_code(text)
    return None


def git(*args):
    return subprocess.run(["git", *args], cwd=lint.ROOT, capture_output=True, text=True)


def changed_files(base, staged):
    """Returns the files that differ from `base` in the working tree, and the files that git does not track. With
    `staged`, returns the files that the index changes from HEAD instead."""
    if staged:
        return sorted(set(git("diff", "--name-only", "--cached").stdout.split()))
    out = git("diff", "--name-only", base).stdout.split()
    out += git("ls-files", "--others", "--exclude-standard").stdout.split()
    return sorted(set(out))


def first_difference(old, new):
    """Returns the index of the first token at which `old` and `new` differ."""
    for i, (a, b) in enumerate(zip(old, new)):
        if a != b:
            return i
    return min(len(old), len(new))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base", default="HEAD", help="the revision to compare with")
    parser.add_argument("--staged", action="store_true", help="compare with the index instead of a revision")
    parser.add_argument("paths", nargs="*")
    args = parser.parse_args()
    base = "" if args.staged else args.base  # git spells the index as an empty revision

    paths = [os.path.relpath(os.path.abspath(p), lint.ROOT) for p in args.paths]
    paths = paths or changed_files(args.base, args.staged)
    files = []
    for path in paths:
        full = os.path.join(lint.ROOT, path)
        if os.path.isdir(full):
            files += sorted(os.path.relpath(os.path.join(d, f), lint.ROOT)
                            for d, _, names in os.walk(full) for f in names)
        elif os.path.isfile(full):
            files.append(path)

    problems = 0
    sources = []
    for path in files:
        with open(os.path.join(lint.ROOT, path), encoding="utf-8", errors="replace") as f:
            text = f.read()
        if lint.GENERATED in text[:400]:
            continue
        new = code_of(path, text)
        if new is None:
            continue
        sources.append(os.path.join(lint.ROOT, path))
        shown = git("show", f"{base}:{path}")
        if shown.returncode != 0:
            continue
        old = code_of(path, shown.stdout)
        if old is None or new is None:
            print(f"{path}: does not parse")
            problems += 1
        elif old != new:
            at = first_difference(old, new)
            print(f"{path}: the code changed, at {' '.join(new[max(0, at - 6):at + 6])!r}")
            problems += 1

    c_sources = [p for p in sources if p.endswith((".c", ".h", ".cpp", ".mm"))]
    if c_sources:
        subprocess.run([sys.executable, "-m", "tools.lint.rewrap", *c_sources], cwd=lint.ROOT,
                       capture_output=True)

    # Lint reads the whole tree, so report only what it finds in these files.
    report = subprocess.run([sys.executable, "-m", "tools.lint.lint"], cwd=lint.ROOT,
                            capture_output=True, text=True)
    named = {os.path.relpath(p, lint.ROOT) for p in sources} | {p for p in files if p.endswith(".md")}
    for line in (report.stdout + report.stderr).splitlines():
        if line.split(":", 1)[0] in named:
            print(line)
            problems += 1

    print(f"{len(files)} files checked, {problems} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
