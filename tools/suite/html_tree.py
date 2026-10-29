#!/usr/bin/env python3
"""Runs the tree construction corpus, and records where this engine answers differently on purpose.

web-platform-tests carries the html5lib tree construction tests as .dat files. Each pairs an input with the tree that
it parses to, printed one node per line. The .html files beside them are wrappers that feed each case through a
browser. The .dat files need no DOM API, so this runner reads them directly and compares the printed tree as text.
The cases in the scripted_ files run scripts while they parse, and this harness has no script engine. The shell runs
those files, as it runs every file, through the wrappers of the html-wpt suite.

If the corpus and the current spec disagree, the spec wins, and the case is recorded in suites/html-tree/errata.txt with
the sentence that settles it. A recorded case that starts agreeing also fails, so an entry cannot outlive its reason.

Usage: python3 -m tools.suite.html_tree [--binary PATH] [--tests DIR] [--match GLOB] [--verbose] [--list]
                            [--errors]
"""

import glob
import json
import os
import sys

from tools.suite import corpus

# The prefix of the files whose cases run scripts while they parse. The html-wpt suite runs them.
SCRIPTED = "scripted_"

# The sections that a test is written in. A line that starts with '#' may name one of them.
SECTIONS = ("data", "errors", "new-errors", "document-fragment", "document",
            "script-on", "script-off")

# Cases that the corpus gets wrong, by "<file>:<index>", each with the sentence that settles it.

class Case:
    """One run: a test, with the scripting flag that it asks for."""

    def __init__(self, path, index, sections, scripting):
        self.key = "%s:%d" % (os.path.basename(path)[:-4], index)
        self.label = None
        self.data = unterminated(sections["data"])
        self.document = trimmed(sections["document"])
        self.context = trimmed(sections["document-fragment"]) or None
        self.scripting = scripting
        self.errors = (len(nonempty(sections["errors"]))
                       + len(nonempty(sections["new-errors"])))

    def context_frame(self):
        """Returns the context element as a namespace and a local name. A name with no namespace before it is an HTML
        element. No context at all means a document parse."""
        if self.context is None:
            return (b"-", b"-")
        space, _, name = self.context.partition(" ")
        return (space.encode(), name.encode()) if name else (b"html", space.encode())


def trimmed(lines):
    """Returns a section's text, without the blank line that separates it from the next test. A section that the test
    does not have has no lines."""
    return "".join(lines or ()).rstrip("\n")


def unterminated(lines):
    """Returns a section's text, for a section whose text is the input instead of a list of lines. The format ends the
    section with one newline, and any newlines before that one belong to the input."""
    text = "".join(lines or ())
    return text[:-1] if text.endswith("\n") else text


def nonempty(lines):
    return [line for line in lines or () if line.strip()]


def read(path):
    """Returns every test in the file, as the lines of each section that it has. A section that it does not have is
    None, which distinguishes a `#script-on` with nothing under it from no such line at all."""
    tests = []
    sections = None
    current = None
    # newline="\n" makes a carriage return in a case survive the read. The default universal newlines would rewrite it.
    with open(path, encoding="utf-8", newline="\n") as f:
        for line in f:
            name = line[1:].rstrip("\n") if line.startswith("#") else None
            if name in SECTIONS:
                if name == "data":
                    if sections is not None:
                        tests.append(sections)
                    sections = {section: None for section in SECTIONS}
                if sections[name] is None:
                    sections[name] = []
                current = sections[name]
                continue
            if current is not None:
                current.append(line)
    if sections is not None:
        tests.append(sections)
    return tests


def cases(path):
    """Returns every run that the file asks for. A test that names neither scripting flag runs with both, because naming
    neither means that its two answers are the same."""
    out = []
    for index, sections in enumerate(read(path)):
        on = sections["script-on"] is not None
        off = sections["script-off"] is not None
        for scripting in (False, True) if not on and not off else (on,):
            out.append(Case(path, index, sections, scripting))
    return out


def answer(binary, batch):
    """Returns the engine's answer to each case. Each case is framed as its byte count, its scripting flag and its
    context element on one line, then that many bytes."""
    framed = b"".join(corpus.frame(case.data.encode("utf-8", "surrogatepass"),
                                   b"on" if case.scripting else b"off", *case.context_frame())
                      for case in batch)
    return corpus.answers([binary], framed, len(batch))


def show(case, why, got, cut):
    print("  %-24s %s   scripting %s" % (case.key, why, case.scripting))
    print("      input    %s" % json.dumps(case.data)[:cut])
    if case.context is not None:
        print("      context  %s" % case.context)
    if got is not None:
        print("      ours   %s" % json.dumps(got["document"])[:cut])
        print("      corpus %s" % json.dumps(case.document)[:cut])


def main():
    ap = corpus.arguments(corpus.binary("html_tree"),
                          os.path.join(corpus.ROOT, "third_party", "wpt", "html", "syntax",
                                       "parsing", "resources"))
    ap.add_argument("--errors", action="store_true",
                    help="compare how many parse errors the case reported")
    args = ap.parse_args()

    paths = sorted(path for path in glob.glob(os.path.join(args.tests, "*.dat"))
                   if not os.path.basename(path).startswith(SCRIPTED))
    if not paths:
        sys.exit("no tests under %s; run ./dev.sh fetch wpt" % args.tests)

    files = [(os.path.basename(path)[:-4], cases(path)) for path in paths]
    if args.list:
        return corpus.listing(files,
                              lambda batch: "%3d fragments"
                              % sum(1 for case in batch if case.context is not None))

    def agrees(case, got):
        return (got["document"].rstrip("\n") == case.document
                and (not args.errors or got["errors"] == case.errors))

    corpus.require(args.binary, "html-tree")
    return corpus.compare(files, args, corpus.Data("html-tree"), 24,
                          answer=lambda name, batch: answer(args.binary, batch),
                          agrees=agrees, show=show)


if __name__ == "__main__":
    sys.exit(main())
