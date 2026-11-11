"""Runs a program built for the iOS simulator, as the simulator build's CMAKE_CROSSCOMPILING_EMULATOR: the build's
tests run each test binary through it (pr_write_tests). It boots a simulator device if none is booted, runs the program
there with the arguments, passes its output through, and exits with its status.

    python3 simulator.py <program> [args...]

The device is the one that PR_SIMULATOR_DEVICE names, by its name or its UDID, or the booted one, or else the first
available iPhone. The program's environment reaches it through simctl's SIMCTL_CHILD_ prefix, so a variable that the
tests read, such as PR_GPU_BACKEND, is passed as it is set here.

It is a script of its own, without the tools package, since the tests run it from any directory."""

import json
import os
import subprocess
import sys

# The variables of the host's environment that the program reads.
PASSED = ("PR_", "MTL_")


def devices():
    """Returns the simulator devices that are available, by runtime."""
    listed = subprocess.run(["xcrun", "simctl", "list", "devices", "available", "--json"], check=True,
                            capture_output=True, text=True).stdout
    return json.loads(listed)["devices"]


def device():
    """Returns the UDID of the device to run on, booting it if it is not booted."""
    wanted = os.environ.get("PR_SIMULATOR_DEVICE")
    found = [d for runtime in devices().values() for d in runtime]
    if wanted:
        chosen = next((d for d in found if wanted in (d["name"], d["udid"])), None)
        if chosen is None:
            sys.exit("simulator: no available device named %s" % wanted)
    else:
        chosen = next((d for d in found if d["state"] == "Booted"), None) or \
            next((d for d in found if d["name"].startswith("iPhone")), None)
        if chosen is None:
            sys.exit("simulator: no iPhone simulator is available (xcodebuild -downloadPlatform iOS)")
    if chosen["state"] != "Booted":
        subprocess.run(["xcrun", "simctl", "boot", chosen["udid"]], check=True)
    return chosen["udid"]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    environment = dict(os.environ)
    for name, value in os.environ.items():
        if name.startswith(PASSED):
            environment["SIMCTL_CHILD_" + name] = value
    run = subprocess.run(["xcrun", "simctl", "spawn", device(), *sys.argv[1:]], env=environment, check=False)
    sys.exit(run.returncode)


if __name__ == "__main__":
    main()
