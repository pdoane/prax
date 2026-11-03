"""`./dev.sh suite`: the conformance suites (registry.py), and their A/B runs against a base."""

import hashlib
import os
import sys

from tools import dev
from tools.build import ab
from tools.build import build as builds
from tools.fetch import checkouts
from tools.suite import registry

USAGE = {
    "suite": """\
./dev.sh suite [debug|asan] [name] [args...]
    one suite, or every suite that is checked out, past any that fails. Extra arguments go to that suite's runner.
    --match <glob> narrows the run to the cases it names and prints their answers whole. `debug` and `asan` run that
    build's harnesses
./dev.sh suite <name> --ab [--base REF | --base-dir DIR] [args...]
    a suite's run, with each case that passes more or fewer tests than at the base: REF, which is HEAD with changes in
    the tree and HEAD^ without, or the sources in DIR. A commit's harnesses are built once, and its results are kept
    by commit, so a base commit runs once""",
}


def run(suite, args, env, cwd=dev.ROOT, check=True, out=None):
    """Runs the suite's runner with `args`, from `cwd`, with the harnesses of the build that `env` names."""
    command = [sys.executable, "-m", suite.runner[0], *suite.runner[1:], *args]
    return dev.run(command, cwd=cwd, env=env, check=check, stdout=out, stderr=out)


def compare(suite, args, env):
    """The working tree's run of the suite, with each case that passes more or fewer tests than the base's. The base
    runs with every harness of the suite built from the base, and with the base's runner. A base commit's results are
    kept by commit and by selection, which a run's other arguments may narrow, so the base runs once."""
    ref = directory = None
    rest = []
    arguments = iter(args)
    for argument in arguments:
        if argument == "--base":
            ref = next(arguments, None) or dev.fail("--base names a commit")
        elif argument == "--base-dir":
            directory = next(arguments, None) or dev.fail("--base-dir names a directory")
        else:
            rest.append(argument)
    base = ab.Base(ref, directory)
    selection = hashlib.sha1(" ".join(rest).encode()).hexdigest()[:12]
    results = os.path.join(builds.AB, "suites", base.sha or "directory", "%s-%s.json" % (suite.name, selection))
    if base.directory or not os.path.exists(results):
        harnesses, absent = base.harnesses(suite.harnesses)
        if absent:
            dev.fail("the base's build has no %s, which the suite runs" % ", ".join(absent))
        runner = base.runner()
        os.makedirs(os.path.dirname(results), exist_ok=True)
        print("--- %s at %s%s" % (suite.name, base.label, "" if base.directory else ", once for this commit"),
              flush=True)
        # A run fails while the base fails cases, which the comparison is for. Its output is kept beside its results,
        # so that a base whose results look wrong can be read.
        partial = results + ".partial"
        with open(results + ".log", "w") as log:
            run(suite, ["--json", partial, *rest], {"PR_BUILD": harnesses}, cwd=runner, check=False, out=log)
        if not os.path.exists(partial):
            with open(results + ".log") as log:
                print("".join(log.readlines()[-20:]), end="")
            dev.fail("the base's run of %s wrote no results" % suite.name)
        os.replace(partial, results)
    run(suite, ["--against", results, *rest], env)


def suite(argv):
    dev.usage(argv, USAGE["suite"])
    mode = argv.pop(0) if argv[:1] in (["debug"], ["asan"]) else None
    env = builds.build(mode).environment()
    if not argv:
        # Every suite runs, whichever fail, and the run fails after them if any did.
        failed = []
        for each in registry.SUITES:
            if not each.ready(env["PR_BUILD"]):
                print("--- %s: not checked out or not built; ./dev.sh suite %s fetches and runs it"
                      % (each.name, each.name), flush=True)
                continue
            print("--- " + each.name, flush=True)
            if run(each, [], env, check=False) != 0:
                failed.append(each.name)
        if failed:
            dev.fail("failed: " + " ".join(failed))
        return
    chosen = registry.named(argv[0])
    args = argv[1:]
    checkouts.fetch(*chosen.checkouts)
    if "--ab" in args:
        compare(chosen, [arg for arg in args if arg != "--ab"], env)
    else:
        run(chosen, args, env)
