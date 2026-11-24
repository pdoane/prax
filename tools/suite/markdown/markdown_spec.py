#!/usr/bin/env python3
"""Compares lib/markdown's HTML with the examples of the CommonMark and GFM specifications.

`commonmark` runs the examples of CommonMark's spec.txt with no extension. `gfm` runs the examples of GFM's spec.txt,
each with the extensions that its example line names, the examples of cmark-gfm's extensions.txt and regression.txt,
the examples of its smart_punct.txt with smart punctuation, and the pathological inputs. A case's key is the file and
the example's number, as spec:12. A case passes if its HTML is the example's, byte for byte, and if a parse that takes
the source a byte at a time gives the same HTML. An example whose HTML is <IGNORE> passes if the parse finishes, and a
pathological input passes if its HTML matches its pattern within PATHOLOGICAL_MILLIS. The parts of the scoreboard are
the sections that the examples are in.
"""

import collections
import os
import re
import sys

from tools.suite import corpus

# The example files of each suite, with the extensions that every example of the file parses.
SUITES = {
    "commonmark": [("spec", os.path.join("commonmark-spec", "spec.txt"), [])],
    "gfm": [("spec", os.path.join("cmark-gfm", "test", "spec.txt"), []),
            ("extensions", os.path.join("cmark-gfm", "test", "extensions.txt"),
             ["table", "strikethrough", "autolink", "tagfilter", "footnotes", "tasklist"]),
            ("regression", os.path.join("cmark-gfm", "test", "regression.txt"), []),
            ("smart", os.path.join("cmark-gfm", "test", "smart_punct.txt"), ["smart"])],
}

# The inputs that cmark-gfm's pathological_tests.py parses with its table extension, each with a pattern that its HTML
# matches. Each must parse within PATHOLOGICAL_MILLIS, which keeps the parser from growing quadratic. They are part of
# the gfm suite, as pathological:<index>, and parse whole. cmark-gfm's case of colliding hashes is left out, since it
# collides cmark's hash, not the engine's.
PATHOLOGICAL = [
    ("nested strong emph", ("*a **a " * 65000) + "b" + (" a** a*" * 65000),
     "(<em>a <strong>a ){65000}b( a</strong> a</em>){65000}"),
    ("many emph closers with no openers", "a_ " * 65000, "(a[_] ){64999}a_"),
    ("many emph openers with no closers", "_a " * 65000, "(_a ){64999}_a"),
    ("many link closers with no openers", "a]" * 65000, r"(a\]){65000}"),
    ("many link openers with no closers", "[a" * 65000, r"(\[a){65000}"),
    ("mismatched openers and closers", "*a_ " * 50000, "([*]a[_] ){49999}[*]a_"),
    ("openers and closers multiple of 3", "a**b" + ("c* " * 50000), "a[*][*]b(c[*] ){49999}c[*]"),
    ("link openers and emph closers", "[ a_" * 50000, r"(\[ a_){50000}"),
    ("pattern [ (]( repeated", "[ (](" * 80000, r"(\[ \(\]\(){80000}"),
    ("pattern ![[]() repeated", "![[]()" * 160000, r'(!\[<a href=""></a>){160000}'),
    ("hard link/emph case", "**x [a*b**c*](d)", r'\*\*x <a href="d">a<em>b\*\*c</em></a>'),
    ("nested brackets", ("[" * 50000) + "a" + ("]" * 50000), r"\[{50000}a\]{50000}"),
    ("nested block quotes", ("> " * 50000) + "a", "(<blockquote>\n){50000}"),
    ("deeply nested lists", "".join("  " * x + "* a\n" for x in range(1000)),
     "<ul>\n(<li>a\n<ul>\n){999}<li>a</li>\n</ul>\n(</li>\n</ul>\n){999}"),
    ("U+0000 in input", "abc\u0000de\u0000", "abc�?de�?"),
    ("backticks", "".join("e" + "`" * x for x in range(1, 5000)), "^<p>[e`]*</p>\n$"),
    ("unclosed links A", "[a](<b" * 30000, r"(\[a\]\(&lt;b){30000}"),
    ("unclosed links B", "[a](b" * 30000, r"(\[a\]\(b){30000}"),
    ("unclosed <!--", "</" + "<!--" * 300000, r"\&lt;\/(\&lt;!--){300000}"),
    ("tables", "aaa\rbbb\n-\v\n" * 30000,
     "^<p>aaa</p>\n<table>\n<thead>\n<tr>\n<th>bbb</th>\n</tr>\n</thead>\n<tbody>\n(<tr>\n<td>aaa</td>\n</tr>\n"
     "<tr>\n<td>bbb</td>\n</tr>\n<tr>\n<td>-\x0b</td>\n</tr>\n){29999}</tbody>\n</table>\n$"),
]
PATHOLOGICAL_MILLIS = 1000

FENCE = "`" * 32

# A case: its key, the part of the scoreboard that it counts in, the extensions that it parses, and its source, with
# either the HTML of an example or, for a pathological case, its title and the pattern that its HTML matches.
Case = collections.namedtuple("Case", "key part names markdown html title pattern", defaults=(None, None))

# The HTML of an example that checks only that a parse finishes.
IGNORE = "<IGNORE>\n"


def examples(path):
    """Returns each example of the spec file at `path`, as (number, section, extensions, markdown, html). Examples are
    numbered from 1, and a disabled example keeps its number."""
    found = []
    section = ""
    number = 0
    state = None
    markdown, html, names = [], [], []
    with open(path, encoding="utf-8") as f:
        for line in f:
            if state is None:
                if line.startswith(FENCE + " example"):
                    names = line[len(FENCE + " example"):].split()
                    state, markdown, html = "markdown", [], []
                elif line.startswith("#"):
                    section = line.lstrip("#").strip()
                continue
            if line.rstrip("\n") == FENCE:
                number += 1
                if "disabled" not in names:
                    found.append((number, section, names, "".join(markdown).replace("→", "\t"),
                                  "".join(html).replace("→", "\t")))
                state = None
            elif state == "markdown" and line.rstrip("\n") == ".":
                state = "html"
            else:
                (markdown if state == "markdown" else html).append(line)
    return found


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in SUITES:
        sys.exit("usage: markdown_spec.py commonmark|gfm [args...]")
    name = sys.argv.pop(1)
    third_party = os.path.join(corpus.ROOT, "third_party")
    args = corpus.arguments(corpus.binary("markdown_spec"), third_party).parse_args()
    suite = corpus.Data(name)

    cases = []
    keys = set()
    for file, relative, extensions in SUITES[name]:
        path = os.path.join(args.tests, relative)
        if not os.path.isfile(path):
            sys.exit("%s missing; run ./dev.sh fetch %s" % (path, "commonmark" if name == "commonmark" else "cmark_gfm"))
        for number, section, names, markdown, html in examples(path):
            key = "%s:%d" % (file, number)
            keys.add(key)
            if suite.runs(key, args.match):
                cases.append(Case(key, "%s: %s" % (file, section), sorted(set(names + extensions)), markdown, html))
    if name == "gfm":
        for index, (title, markdown, pattern) in enumerate(PATHOLOGICAL):
            key = "pathological:%d" % index
            keys.add(key)
            if suite.runs(key, args.match):
                cases.append(Case(key, "pathological", ["table"], markdown, None, title, re.compile(pattern)))
    if not cases:
        sys.exit("no Markdown cases selected")
    if args.list:
        print("%d Markdown cases" % len(cases))
        return 0
    corpus.require(args.binary, name)

    frames = b"".join(corpus.frame(case.markdown.encode("utf-8"), (",".join(case.names) or "commonmark").encode(),
                                   *([b"whole"] if case.pattern else []))
                      for case in cases)
    results = corpus.answers([args.binary], frames, len(cases))

    outcome = corpus.Results(args, suite, keys=keys)
    for case, result in zip(cases, results):
        streamed = result.get("streamed", True)
        if case.pattern:
            slow = result["millis"] > PATHOLOGICAL_MILLIS * corpus.SLOWDOWN
            agrees = case.pattern.search(result["html"]) is not None and not slow
            matched = "matches" if case.pattern.search(result["html"]) else "does not match"
            detail = "%s took %d ms, and its HTML %s the pattern" % (case.title, result["millis"], matched)
            lines = ()
        else:
            # An example whose HTML is <IGNORE> tests only that the parse finishes.
            agrees = (case.html == IGNORE or result["html"] == case.html) and streamed
            detail = case.part + ("" if streamed else ", which streams differently")
            lines = ["markdown %r" % case.markdown, "expected %r" % case.html, "got      %r" % result["html"]]
        outcome.case(case.key, int(agrees), detail=detail, lines=lines, part=case.part)
    return outcome.finish()


if __name__ == "__main__":
    sys.exit(main())
