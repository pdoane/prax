#!/usr/bin/env python3
"""Runs the html5lib tokenizer suite, and records where this engine answers differently on purpose.

html5lib/html5lib-tests carries implementation-independent tests for the tokenizer. Each file pairs an input with the
tokens that it produces and the parse errors that it reports. The tests reach a layer that the tree construction
corpus cannot, because a tokenizer difference shows there only if it survives into the tree.

If the suite and the current spec disagree, the spec wins, and the case is recorded in suites/html-tokens/errata.txt
with the sentence that settles it. A recorded case that starts agreeing also fails, so an entry cannot outlive its
reason.

Usage: python3 -m tools.suite.html_tokens [--binary PATH] [--tests DIR] [--match GLOB] [--verbose] [--list]
"""

import glob
import json
import os
import re
import sys

from tools.suite import corpus

# The states that html5lib names, each mapped to the engine's name for it.
STATES = {
    "Data state": "data",
    "PLAINTEXT state": "plaintext",
    "RCDATA state": "rcdata",
    "RAWTEXT state": "rawtext",
    "Script data state": "script-data",
    "CDATA section state": "cdata-section",
}

# Cases that the corpus gets wrong, by "<file>:<index>", each with the sentence that settles it.

ESCAPE = re.compile(r"\\u([0-9A-Fa-f]{4})")


def unescape(value):
    """Returns a doubleEscaped string with its \\uHHHH sequences resolved. A surrogate pair written as two sequences
    becomes one code point, as the JSON import would have made it. A lone surrogate stays lone, because the engine is
    asked about exactly what the suite wrote."""
    if isinstance(value, str):
        text = ESCAPE.sub(lambda m: chr(int(m.group(1), 16)), value)
        return text.encode("utf-16", "surrogatepass").decode("utf-16", "surrogatepass")
    if isinstance(value, list):
        return [unescape(v) for v in value]
    if isinstance(value, dict):
        return {unescape(k): unescape(v) for k, v in value.items()}
    return value


class Case:
    """One run: a test, in one of the states that it asks to be run in."""

    def __init__(self, path, index, test, state):
        self.key = "%s:%d" % (os.path.basename(path)[:-5], index)
        self.label = test["description"]
        self.state = state
        self.input = test["input"]
        self.last_start_tag = test.get("lastStartTag")
        self.tokens = coalesced(test["output"])
        self.errors = test.get("errors", [])


def coalesced(tokens):
    """Returns the tokens with adjacent Character tokens merged into one, the form in which the suite writes them."""
    out = []
    for token in tokens:
        if token[0] == "Character" and out and out[-1][0] == "Character":
            out[-1] = ["Character", out[-1][1] + token[1]]
        else:
            out.append(list(token))
    return out


def cases(path):
    """Returns every run that the file asks for, in order."""
    with open(path, encoding="utf-8") as f:
        suite = json.load(f)

    out = []
    for index, test in enumerate(suite["tests"]):
        if test.get("doubleEscaped"):
            test = dict(test, input=unescape(test["input"]),
                        output=unescape(test["output"]))
        for state in test.get("initialStates", ["Data state"]):
            out.append(Case(path, index, test, state))
    return out


def answer(binary, batch):
    """Returns the engine's answer to each case. Each case is framed as its byte count, its state and its last start
    tag on one line, then that many bytes. The suite writes a case as code points, so the bytes encode those code
    points, including surrogates. Deciding an encoding is the step before the tokenizer, and the suite starts after
    that step."""
    framed = b"".join(corpus.frame(case.input.encode("utf-8", "surrogatepass"),
                                   STATES[case.state].encode(),
                                   (case.last_start_tag or "-").encode())
                      for case in batch)
    return corpus.answers([binary], framed, len(batch))


def found(got):
    """Returns the tokens and errors that an answer holds, in the form in which the suite writes them."""
    return (coalesced(got["tokens"]), got["errors"])


def show(case, why, got, cut):
    print("  %-22s %-42s %s" % (case.key, case.label[:42], why))
    print("      state %s   input %s" % (case.state, json.dumps(case.input)[:cut]))
    if got is not None:
        print("      ours  %s" % json.dumps(found(got))[:cut])
        print("      suite %s" % json.dumps((case.tokens, case.errors))[:cut])


def main():
    args = corpus.arguments(corpus.binary("html_tokens"),
                            os.path.join(corpus.ROOT, "third_party", "html5lib",
                                         "tokenizer")).parse_args()

    paths = sorted(glob.glob(os.path.join(args.tests, "*.test")))
    if not paths:
        sys.exit("no tests under %s; run ./dev.sh fetch html5lib" % args.tests)

    # xmlViolation.test expects the changes that html/#coercing-an-html-dom-into-an-infoset makes, which apply to a
    # tree serialized as XML, not to a tokenizer.
    files = [(os.path.basename(path)[:-5], cases(path)) for path in paths
             if os.path.basename(path) != "xmlViolation.test"]
    if args.list:
        return corpus.listing(files)

    corpus.require(args.binary, "html-tokens")
    return corpus.compare(files, args, corpus.Data("html-tokens"), 22,
                          answer=lambda name, batch: answer(args.binary, batch),
                          agrees=lambda case, got: found(got) == (case.tokens, case.errors),
                          show=show)


if __name__ == "__main__":
    sys.exit(main())
