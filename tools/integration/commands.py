"""`./dev.sh integration`: the integration tests, applications that check how the libraries serve a host
(tools/integration/CMakeLists.txt). Each prints `integration: passed` or `integration: failed`, with why, and ends."""

import os
import sys

from tools import dev
from tools.build import build as builds

USAGE = {
    "integration": """\
./dev.sh integration [name...]
    build the integration tests, or those named, each for the simulator of every platform that it runs on, and run
    each on a simulator device of the platform, chosen as ./dev.sh test chooses it. A test passes once it prints that
    it passed, and fails if it prints that it failed, ends without saying, or runs past its time""",
}

# The integration tests, each with the UIKit platforms whose simulator builds run it.
TESTS = {
    "swift_host": ("visionos",),
    "app_host": ("visionos",),
}

# How long a test may run once it is installed, in seconds.
TIMEOUT = 120


def outcome(status, output):
    """Returns whether a test passed, and its outcome as it reported it, from its exit status and output."""
    reported = [line for line in output.splitlines() if line.startswith("integration: ")]
    if status is None:
        return False, "timed out"
    if not reported:
        return False, "ended with status %d without an outcome" % status
    return status == 0 and reported[-1].startswith("integration: passed"), reported[-1]


def integration(argv):
    dev.usage(argv, USAGE["integration"])
    names = argv or list(TESTS)
    for name in names:
        if name not in TESTS:
            dev.fail("no integration test named %s; one of: %s" % (name, " ".join(TESTS)))
    if not dev.darwin():
        dev.fail("the integration tests run on the simulators of UIKit's platforms, on macOS")
    from tools.build import simulator

    failed = []
    for name in names:
        for platform in TESTS[name]:
            chosen = builds.build(platform + "-sim", targets=[name])
            status, output = simulator.run(platform, os.path.join(chosen.directory, name + ".app"), TIMEOUT)
            passed, reported = outcome(status, output)
            label = "%s on %s-sim" % (name, platform)
            print("%s: %s" % (label, reported))
            if not passed:
                print(output, file=sys.stderr)
                failed.append(label)
    if failed:
        dev.fail("integration tests failed: " + ", ".join(failed))
