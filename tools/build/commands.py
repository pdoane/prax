"""`./dev.sh build`, `test`, `example` and `clean`: the builds, the unit tests, and the examples."""

import os
import shutil

from tools import dev
from tools.build import build as builds
from tools.fetch import checkouts
from tools.lint import commands as lint

USAGE = {
    "build": """\
./dev.sh build [debug|release|asan|tsan] [--keep-going] [--target NAME]...
    configure and build. The default is optimized, with debug symbols and without link-time optimization. `debug`
    builds without the optimizer and with the assertions. `release` adds link-time optimization. `asan` is Debug with
    the address and undefined behavior sanitizers, and `tsan` is Debug with the thread sanitizer. --keep-going builds
    past a failure and reports every error. --target builds only the targets named, and what they need""",
    "test": """\
./dev.sh test [debug|asan|tsan] [module] [args...]
    the unit tests, with lint beside them, or one module's doctest binary with args, as in ./dev.sh test dom
    -ts="*keyword*" --success, from that build, which builds only that binary. `asan` also runs the tools' tests
    against its harnesses""",
    "example": """\
./dev.sh example <name> [args...]
    build an example under examples, and only what it needs, and run it with the arguments""",
    "clean": """\
./dev.sh clean
    delete the build directories""",
}

# The modules whose tests connect to the servers of a peer that runs them (tools/harness/net_peer.py).
NETWORK_MODULES = ("net", "tls", "http", "fetch", "websocket", "alchemy_comfy")

# The seconds that a unit test may run before it fails as stalled, for a test that sets no limit of its own. A unit
# test takes well under a second, so a stall fails at once rather than holding the run.
TEST_TIMEOUT = 60


def build(argv):
    dev.usage(argv, USAGE["build"])
    mode = None
    targets = []
    keep_going = False
    arguments = iter(argv)
    for argument in arguments:
        if argument == "--keep-going":
            keep_going = True
        elif argument == "--target":
            targets.append(next(arguments, None) or dev.fail("--target names a target"))
        elif argument in builds.MODES:
            mode = argument
        else:
            dev.usage(argv, USAGE["build"], 1)
    builds.build(mode, targets, keep_going)


def unit_tests(mode=None):
    """Builds and runs every unit test of the build that `mode` names, and with asan, the tools' tests against its
    harnesses."""
    chosen = builds.build(mode)
    dev.run(["ctest", "--test-dir", chosen.directory, "--output-on-failure", "--parallel", os.cpu_count() or 4,
             "--timeout", TEST_TIMEOUT])
    if chosen.name == "asan":
        lint.tools_tests(chosen.environment())


def test(argv):
    dev.usage(argv, USAGE["test"])
    checkouts.fetch("fonts", "unicode", "html5lib")
    mode = argv.pop(0) if argv[:1] in (["debug"], ["asan"], ["tsan"]) else None
    if not argv:
        # The whole run lints beside the build.
        lint.beside(lambda: unit_tests(mode))
        return
    # One module's tests build only their binary, <module>_tests.
    module, args = argv[0], argv[1:]
    chosen = builds.build(mode, [module + "_tests"])
    binary = dev.executable(chosen.directory, module + "_tests")
    if not os.path.exists(binary):
        names = sorted(name[:-len("_tests")] for name in os.listdir(chosen.directory) if name.endswith("_tests"))
        dev.fail("no unit tests named %s; one of: %s" % (module, " ".join(names)))
    if module in NETWORK_MODULES:
        dev.python("tools.harness.net_peer", binary, *args)
    else:
        dev.run([binary, *args])


def example(argv):
    dev.usage(argv, USAGE["example"], None if argv else 1)
    # An example's target is its directory's name.
    examples = os.path.join(dev.ROOT, "examples")
    names = sorted(name for name in os.listdir(examples) if os.path.isdir(os.path.join(examples, name)))
    if argv[0] not in names:
        dev.fail("no example named %s; one of: %s" % (argv[0], " ".join(names)))
    chosen = builds.build(targets=[argv[0]])
    dev.run([dev.executable(chosen.directory, argv[0]), *argv[1:]])


def clean(argv):
    dev.usage(argv, USAGE["clean"])
    for directory in (builds.DEFAULT, builds.DEBUG, builds.RELEASE, builds.ASAN, builds.TSAN, builds.AB,
                      builds.PACKAGE):
        shutil.rmtree(directory, ignore_errors=True)
