#!/usr/bin/env python3
"""Runs the css-parsing-tests suites, and records where this engine answers differently on purpose.

servo/rust-cssparser carries implementation-independent tests for the parser entry points. Each file pairs an input
with the expected result. The tests reach a layer that the WPT corpus cannot. WPT goes through CSSOM, so a tokenizer
difference shows only if it survives into a stylesheet.

The suite was written against the 2013 draft. If the suite and the current spec disagree, the spec wins, and the case is
recorded in suites/css-parsing/errata.txt with the sentence that settles it. A recorded case that starts agreeing also
fails, so an entry cannot outlive its reason.

Usage: python3 -m tools.suite.css_parsing [--binary PATH] [--tests DIR] [--match GLOB] [--verbose] [--list]
"""

import json
import os
import sys

from tools.suite import corpus

SUITES = ["component_value_list", "one_component_value", "one_declaration", "declaration_list",
          "one_rule", "rule_list", "stylesheet", "An+B", "urange"]

# Cases that the suite gets wrong, by "<suite>:<index>", each with the sentence that settles it.

# The suite records a numeric token's representation in Rust's float formatting, such as `0.67` for `.67` and `1200.0`
# for `12e2`. In css-syntax-3/#consume-numeric-token, the representation is the source text that the token covers.
# Neither spelling is more correct, so the comparison leaves it out. lib/css/tests/test_css_token.cpp checks that a
# numeric token covers the bytes that it read, without depending on a foreign convention.
NUMERIC = ("number", "percentage", "dimension")


def normalized(value):
    if not isinstance(value, list):
        return value
    if len(value) >= 4 and value[0] in NUMERIC:
        return [value[0], None] + [normalized(v) for v in value[2:]]
    return [normalized(v) for v in value]


class Case:
    """One input of a suite, and the answer that the suite expects."""

    def __init__(self, suite, index, text, want):
        self.key = "%s:%d" % (suite, index)
        self.label = json.dumps(text)[:60]
        self.text = text
        self.want = want


def cases(tests, suite):
    """Returns every case in the suite's file, in order."""
    with open(os.path.join(tests, suite + ".json"), encoding="utf-8") as f:
        pairs = json.load(f)
    return [Case(suite, index, pairs[index * 2], pairs[index * 2 + 1])
            for index in range(len(pairs) // 2)]


def answer(binary, suite, batch):
    """Returns the engine's answer to each case. Each case is framed as its byte count on one line, then that many
    bytes."""
    framed = b"".join(corpus.frame(case.text.encode("utf-8")) for case in batch)
    return corpus.answers([binary, suite], framed, len(batch))


def show(case, why, got, cut):
    print("  %-26s %-42s %s" % (case.key, json.dumps(case.text)[:42], why))
    if got is not None:
        print("      ours  %s" % json.dumps(got)[:cut])
        print("      suite %s" % json.dumps(case.want)[:cut])


def main():
    args = corpus.arguments(corpus.binary("css_parsing"),
                            os.path.join(corpus.ROOT, "third_party", "cssparser", "src",
                                         "css-parsing-tests")).parse_args()

    if not os.path.isdir(args.tests):
        sys.exit("no tests under %s; run ./dev.sh fetch cssparser" % args.tests)
    files = [(suite, cases(args.tests, suite)) for suite in SUITES]
    if args.list:
        return corpus.listing(files)

    corpus.require(args.binary, "css-parsing")
    return corpus.compare(files, args, corpus.Data("css-parsing"), 26,
                          answer=lambda suite, batch: answer(args.binary, suite, batch),
                          agrees=lambda case, got: normalized(got) == normalized(case.want),
                          show=show)


if __name__ == "__main__":
    sys.exit(main())
