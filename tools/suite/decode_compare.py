"""What the suites of image corpora share: decoding each file with lib/image beside the PNGs that hold its picture.

A suite of this kind is a corpus of files, some with PNGs of their pictures. The decode_compare harness decodes a file
and its references through the set of every codec (tools/suite/decode_compare/decode_compare.cpp). A case is one file.
It passes if it decodes to one of its references exactly, or if it has none and decodes. A suite may mark a case as
one that passes whatever the decoder says of it, as long as the harness answers, as a file that is bad on purpose. A
case that differs on purpose, or that the decoders say is NotSupported, is recorded in the suite's errata with why.

The run fails on a case that fails without an entry in the errata, a recorded case that passes, or an entry that names
no case.
"""

import json
import subprocess
import sys

from tools.suite import corpus

# The number of cases that one run of the harness answers.
BATCH = 16


class Case:
    """A file of a corpus: its key, its path, the paths of the PNGs of its picture, and whether it passes whatever the
    decoder says of it."""

    def __init__(self, key, path, references=(), any_answer=False):
        self.key = key
        self.path = path
        self.references = list(references)
        self.any_answer = any_answer


def answers(binary, cases):
    """Returns the harness's answer for each case. A case that ends the harness is answered with the status Crash."""
    specs = [case.path + ("=" + ",".join(case.references) if case.references else "") for case in cases]
    results = []
    for start in range(0, len(specs), BATCH):
        batch = specs[start:start + BATCH]
        done = subprocess.run([binary, *batch], stdout=subprocess.PIPE)
        lines = done.stdout.decode("utf-8", "replace").splitlines()
        if done.returncode == 0 and len(lines) == len(batch):
            results += [json.loads(line) for line in lines]
            continue
        for spec in batch:
            done = subprocess.run([binary, spec], stdout=subprocess.PIPE)
            lines = done.stdout.decode("utf-8", "replace").splitlines()
            if done.returncode == 0 and len(lines) == 1:
                results.append(json.loads(lines[0]))
            else:
                results.append({"status": "Crash", "width": 0, "height": 0, "references": []})
    return results


def passes(case, result):
    """Returns true if the decoder's answer of a case is what the suite expects of it."""
    if result["status"] == "Crash":
        return False
    if case.any_answer:
        return True
    if result["status"] != "":
        return False
    return not case.references or 0 in result["references"]


def run(name, parts, fetch):
    """Runs the suite `name` over `parts`, a list of (part, cases). `fetch` names the checkout of its corpus."""
    ap = corpus.arguments(corpus.binary("decode_compare"), None)
    args = ap.parse_args()
    suite = corpus.Data(name)
    chosen = [(part, [case for case in cases if suite.runs(case.key, args.match)]) for part, cases in parts]
    if not any(cases for _, cases in chosen):
        sys.exit("%s corpus missing; run ./dev.sh fetch %s" % (name, fetch))
    if args.list:
        return corpus.listing([(part, cases) for part, cases in chosen])
    corpus.require(args.binary, name)

    results = corpus.Results(args, suite, keys={case.key for _, cases in parts for case in cases})
    refused = 0
    compared = 0
    for part, cases in chosen:
        for case, result in zip(cases, answers(args.binary, cases)):
            refused += 1 if result["status"] not in ("", "Truncated") else 0
            compared += 1 if case.references and result["status"] == "" else 0
            crashed = result["status"] == "Crash"
            results.case(case.key, int(passes(case, result)), status="CRASH" if crashed else corpus.OK,
                         detail=json.dumps(result), part=part)
    total = len(results.passed)
    return results.finish(notes=["Of the %d files, %d decode beside a reference and %d are refused."
                                 % (total, compared, refused)])
