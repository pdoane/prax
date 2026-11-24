#!/usr/bin/env python3
"""Runs lib/unicode against the conformance files of the Unicode Character Database and of UTS #46.

Each part of the suite is one file of the `unicode` checkout, at the version that lib/unicode's tables are generated
from: BidiTest, BidiCharacterTest, NormalizationTest, LineBreakTest, GraphemeBreakTest, WordBreakTest and IdnaTestV2.
The unicode_conformance harness runs every case of a file and answers with the cases that disagree, so a case's key,
<file>:<index>, counts the cases in the harness's order. A case is a line of the file, except that a BidiTest line is a
case in each paragraph direction that it lists, and NormalizationTest goes on past its lines with a case for each code
point that its Part 1 does not list, which must be in every form. The run fails on a disagreement that the errata do
not record, a recorded case that agrees, or an entry that names no case.
"""

import fnmatch
import json
import os
import subprocess
import sys

from tools.suite import corpus

PARTS = ("BidiTest", "BidiCharacterTest", "NormalizationTest", "LineBreakTest", "GraphemeBreakTest", "WordBreakTest",
         "IdnaTestV2")


def run_part(binary, part, path):
    """Returns the number of cases of a part, and the text of each that disagrees, by its index."""
    done = subprocess.run([binary, part, path], stdout=subprocess.PIPE)
    if done.returncode != 0:
        sys.exit("%s %s exited %d" % (binary, part, done.returncode))
    disagreeing = {}
    cases = None
    for line in done.stdout.decode("utf-8", "replace").splitlines():
        answer = json.loads(line)
        if "cases" in answer:
            cases = answer["cases"]
        else:
            disagreeing[answer["case"]] = answer["text"]
    if cases is None:
        sys.exit("%s %s answered no count of cases" % (binary, part))
    return cases, disagreeing


def indices(suite, part):
    """Returns the indices of a part that the errata, and the lists of cases that a plain run leaves out, name."""
    named = {}
    for name, entries in (("errata", suite.errata), ("cut", suite.cut), ("deferred", suite.deferred),
                          ("skip", suite.skip)):
        for key in entries:
            if corpus.file_of(key) == part and ":" in key:
                named.setdefault(name, set()).add(int(key.rsplit(":", 1)[1]))
    return named


def main():
    args = corpus.arguments(corpus.binary("unicode_conformance"),
                            os.path.join(corpus.ROOT, "third_party", "unicode")).parse_args()
    suite = corpus.Data("unicode")
    parts = [part for part in PARTS if corpus.holds(part, args.match)]
    if not parts:
        sys.exit("no part of the Unicode suite is selected")
    if args.list:
        for part in parts:
            print("%s  %s" % (part.ljust(18), os.path.join(args.tests, part + ".txt")))
        print("\nThe harness counts the cases of each part as it runs them.")
        return 0
    corpus.require(args.binary, "unicode")
    narrowed = args.match is not None and ":" in args.match
    outcome = corpus.Results(args, suite)
    keys = set()
    for part in parts:
        path = os.path.join(args.tests, part + ".txt")
        if not os.path.isfile(path):
            sys.exit("%s missing; run ./dev.sh fetch unicode" % path)
        # A plain run leaves out a part that a list names whole.
        keys.add(part)
        if args.match is None and suite.left_out(part) is not None:
            continue
        cases, disagreeing = run_part(args.binary, part, path)
        named = indices(suite, part)
        left = set() if args.match else set().union(*(named.get(n, set()) for n in ("cut", "deferred", "skip")))
        for index in range(cases):
            key = "%s:%d" % (part, index)
            keys.add(key)
            if narrowed and not fnmatch.fnmatch(key, args.match):
                continue
            if index in left:
                outcome.left += 1
                continue
            outcome.case(key, 0 if index in disagreeing else 1, detail=disagreeing.get(index, ""), part=part)
    # The entries of the parts that the run took name cases among those parts' indices.
    outcome.keys = keys if args.match is None else None
    return outcome.finish()

if __name__ == "__main__":
    sys.exit(main())
