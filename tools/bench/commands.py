"""`./dev.sh bench` and `profile`: the benchmarks (registry.py), their A/B comparisons, and where a harness spends its
time."""

import os
import re
import subprocess
import sys

from tools import dev
from tools.bench import browser
from tools.bench import perf
from tools.bench import registry
from tools.build import ab
from tools.build import build as builds
from tools.fetch import checkouts

USAGE = {
    "bench": """\
./dev.sh bench list
    what each benchmark measures, and whether browsers run it
./dev.sh bench ab [name...] [--base REF | --base-dir DIR] [--runs N] [--all] [--threshold PERCENT] [args...]
    what a change does: the benchmarks named, or every one, on the harnesses of the base and of the working tree,
    compared on instructions. The base is REF, which is HEAD with changes in the tree and HEAD^ without, or the sources
    in DIR: a worktree, or a copy of the tree made before the change, for a base that is not a commit. Prints the
    measures that changed past their runs' spread, two runs a side by default (--runs), and the measures whose runs
    disagree; --all prints every measure
./dev.sh bench [name...] [args...]
    the benchmarks named, or every one, run once on the working tree's harnesses, and their measures, the other
    engines' beside the engine's where a benchmark runs them. Nothing is recorded. Other arguments go to the
    benchmarks' runs, as in --renderer cpu for draw
./dev.sh bench browser [name...] [--browser NAME]... [--timeout S] [--serve]
    the benchmarks that browsers run, or those named, timed in pr_shell and in every browser installed, or those
    named, side by side, and each browser's time as a multiple of the engine's. Nothing is recorded. --serve serves one
    benchmark's page to open by hand
./dev.sh bench kept [name...] [--count N]
    what N runs of each case of those benchmarks keep, which must stay flat as N grows""",
    "profile": """\
./dev.sh profile [--base REF] [--seconds N] [--under NAME] [--rows N] <harness> [args...]
./dev.sh profile [--base REF] [--seconds N] [--under NAME] [--rows N] bench <name> <case>
    where a harness of the default build spends its time, by function and by the innermost inlined line
    (tools/bench/profile.py, macOS), and with --base, the same harness of REF beside it, as shares of each side's
    samples. A frame_bench case or the invalidation harness's --element narrows it. `bench` runs one case of a scripted
    benchmark, as in ./dev.sh profile bench layout "cards style", over and over in pr_shell""",
}


def split(args):
    """Returns the benchmarks that `args` name, and the rest of the arguments."""
    names = [arg for arg in args if arg in registry.BY_NAME]
    return names, [arg for arg in args if arg not in registry.BY_NAME]


def fetch(names):
    """Fetches what the benchmarks read. The real pages are taken only if there are none, so that a run stays quick and
    needs no network."""
    for checkout in sorted({name for bench in registry.named(names) for name in bench.fetch}):
        checkouts.fetch(checkout, pages_args=("--if-empty",))


def harnesses(names):
    """The build's targets that the benchmarks run."""
    return sorted({target for bench in registry.named(names) for target in bench.harnesses})


def build(names, extra=()):
    """Builds the release build's harnesses of the benchmarks, and the `extra` targets."""
    builds.configure(dev.ROOT, builds.RELEASE, "Release")
    targets = harnesses(names) + list(extra)
    present = builds.build_targets(builds.RELEASE)
    missing = [target for target in targets if target not in present]
    if missing:
        dev.fail("the build has no %s, which tools/bench/CMakeLists.txt builds only where its tools are installed"
                 % ", ".join(missing))
    dev.run(["cmake", "--build", builds.RELEASE] + [part for target in targets for part in ("--target", target)])


def power():
    """Says if the machine runs on battery, which caps the clocks. That moves the times and cycles that a run reports,
    not its instructions."""
    if not dev.darwin():
        return
    listing = subprocess.run(["pmset", "-g", "ps"], capture_output=True, text=True).stdout
    source = re.search(r"Now drawing from '(.*)'", listing)
    if source and source.group(1) != "AC Power":
        print("power: %s -- the clocks are capped, which moves times and cycles, not instructions" % source.group(1))


def run(args):
    """Runs the benchmarks once on the release build's harnesses and prints their measures. Arguments that are not a
    benchmark's name go to the benchmarks' runners."""
    names, rest = split(args)
    # The checkouts first, because the build has a benchmark's harness only where they are there.
    fetch(names)
    build(names)
    power()
    sys.exit(0 if perf.run(names, builds.RELEASE, rest) else 1)


def compare(args):
    """A/B: runs the benchmarks on the base's harnesses and on the working tree's, and reports what changed."""
    ref = directory = None
    runs = 2
    threshold = perf.THRESHOLD
    everything = False
    rest = []
    arguments = iter(args)
    for argument in arguments:
        if argument in ("--base", "--base-dir", "--runs", "--threshold"):
            value = next(arguments, None) or dev.fail(argument + " takes a value")
            if argument == "--base":
                ref = value
            elif argument == "--base-dir":
                directory = value
            elif argument == "--runs":
                runs = max(1, int(value))
            else:
                threshold = float(value)
        elif argument == "--all":
            everything = True
        else:
            rest.append(argument)
    names, rest = split(rest)
    fetch(names)
    build(names)
    base = ab.Base(ref, directory)
    base_bin, absent = base.harnesses(harnesses(names))
    # A benchmark whose harness the base's build does not have, such as one that a later commit added, has nothing to
    # compare with.
    compared = []
    for bench in registry.named(names):
        lacking = [target for target in bench.harnesses if target in absent]
        if lacking:
            print("%s: not compared, because the base's build has no %s" % (bench.name, ", ".join(lacking)), flush=True)
        else:
            compared.append(bench.name)
    if not compared:
        dev.fail("no benchmark has a harness in both builds")
    power()
    perf.ab(compared, base_bin, base.label, builds.RELEASE, runs, threshold, everything, rest)


def scripted(command, args):
    """The scripted benchmarks in pr_shell and in browsers (browser), or what their cases keep (kept), on the release
    build."""
    names, rest = split(args)
    benches = perf.scripted_named(names)
    names = [bench.name for bench in benches]
    # The system's WebKit runs in a window of the build's own.
    build(names, ["webkit_host"] if command == "browser" and dev.darwin() else [])
    power()
    fetch(names)
    options = {"--browser": [], "--timeout": 300, "--serve": False, "--count": 100}
    arguments = iter(rest)
    for argument in arguments:
        if argument == "--serve":
            options[argument] = True
        elif argument == "--browser":
            options[argument].append(next(arguments, None) or dev.fail("--browser names a browser"))
        elif argument in ("--timeout", "--count"):
            options[argument] = int(next(arguments, None) or dev.fail(argument + " takes a number"))
        else:
            dev.usage([], USAGE["bench"], 1)
    if command == "browser":
        sys.exit(browser.main(benches, options["--browser"], builds.RELEASE, options["--timeout"],
                               options["--serve"]))
    perf.kept(benches, builds.RELEASE, options["--count"])


def bench(argv):
    dev.usage(argv, USAGE["bench"])
    if argv[:1] == ["list"]:
        print(registry.table())
    elif argv[:1] == ["ab"]:
        compare(argv[1:])
    elif argv[:1] in (["browser"], ["kept"]):
        scripted(argv[0], argv[1:])
    else:
        run(argv)


def profile(argv):
    dev.usage(argv, USAGE["profile"])
    ref = None
    options = []
    while argv[:1] in (["--base"], ["--seconds"], ["--under"], ["--rows"]):
        if len(argv) < 2:
            dev.usage(argv, USAGE["profile"], 1)
        if argv[0] == "--base":
            ref = argv[1]
        else:
            options += argv[:2]
        argv = argv[2:]
    if not argv:
        dev.usage(argv, USAGE["profile"], 1)
    harness, args = argv[0], argv[1:]
    # The default build's debug information names the lines that a harness spends its time in.
    builds.configure(dev.ROOT, builds.DEFAULT, "RelWithDebInfo")
    if harness == "bench":
        if len(args) != 2:
            dev.usage(argv, USAGE["profile"], 1)
        fetch([args[0]])
        args = perf.case_args(args[0], args[1], builds.DEFAULT)
        harness = "pr_shell"
    dev.run(["cmake", "--build", builds.DEFAULT, "--target", harness])
    base_binary = []
    if ref:
        base = ab.Base(ref)
        directory = os.path.join(builds.AB, "profile")
        base.compile(directory, "RelWithDebInfo", [harness], log=os.path.join(builds.AB, "profile.log"))
        base_binary = ["--base-binary", dev.executable(directory, harness)]
    dev.python("tools.bench.profile", "--binary", dev.executable(builds.DEFAULT, harness), *base_binary, *options,
               "--", *args)
