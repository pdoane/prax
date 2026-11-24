#!/usr/bin/env python3
"""Compares the DOM that lib/html builds from Markdown with the DOM that lib/markdown's HTML of it parses to.

The cases are the examples of the CommonMark and GFM suites (tools/suite/markdown/markdown_spec.py), keyed as there with
the suite's name before them, as commonmark:spec:12. Those suites compare lib/markdown's HTML with the examples', so
this suite compares the DOM with that HTML alone. A case passes if the HTML parser's tree for the HTML is the tree that
the Markdown builds: in a document, given whole and a byte at a time, and as an element's contents. The parts of the
scoreboard are the two suites."""

import os
import sys

from tools.suite import corpus
from tools.suite.markdown import markdown_spec


def main():
    third_party = os.path.join(corpus.ROOT, "third_party")
    args = corpus.arguments(corpus.binary("markdown_dom"), third_party).parse_args()
    suite = corpus.Data("markdown-dom")

    cases = []
    keys = set()
    for name, files in markdown_spec.SUITES.items():
        for file, relative, extensions in files:
            path = os.path.join(args.tests, relative)
            if not os.path.isfile(path):
                sys.exit("%s missing; run ./dev.sh fetch %s"
                         % (path, "commonmark" if name == "commonmark" else "cmark_gfm"))
            for number, _, names, markdown, html in markdown_spec.examples(path):
                key = "%s:%s:%d" % (name, file, number)
                keys.add(key)
                if suite.runs(key, args.match):
                    cases.append((key, name, sorted(set(names + extensions)), markdown, html))
    if not cases:
        sys.exit("no Markdown cases selected")
    if args.list:
        print("%d Markdown DOM cases" % len(cases))
        return 0
    corpus.require(args.binary, "markdown-dom")

    frames = [corpus.frame(markdown.encode("utf-8"), (",".join(names) or "commonmark").encode())
              for _, _, names, markdown, _ in cases]
    results = corpus.answers([args.binary], b"".join(frames), len(cases))

    outcome = corpus.Results(args, suite, keys=keys)
    for (key, part, _, markdown, html), result in zip(cases, results):
        ways = [way for way in ("whole", "streamed", "element") if not result[way]]
        outcome.case(key, int(not ways), detail="differs " + ", ".join(ways) if ways else "",
                     lines=["markdown %r" % markdown], part=part)
    return outcome.finish()


if __name__ == "__main__":
    sys.exit(main())
