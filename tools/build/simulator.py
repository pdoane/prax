"""Runs programs built for the simulator of a UIKit platform: ios, tvos or visionos.

As a simulator build's CMAKE_CROSSCOMPILING_EMULATOR, it runs a program: the build's tests run each test binary through
it (pr_write_tests). It boots a simulator device of the platform if none is booted, runs the program there with the
arguments, passes its output through, and exits with its status.

    python3 simulator.py --platform <platform> <program> [args...]

`./dev.sh example` and `./dev.sh app` with a simulator build, as ios-sim, launch an application bundle through
launch(): they install it on the device and launch it with the arguments, showing the device (show), and stream the
application's output until it exits.

The device is the one that PR_SIMULATOR_DEVICE names, by its name or its UDID, or else the platform's booted device, or
else its first available device. The program's environment reaches it through simctl's SIMCTL_CHILD_ prefix, so a
variable that the tests read, such as PR_GPU_BACKEND, is passed as it is set here.

It is a script of its own, without the tools package, since the tests run it from any directory."""

import json
import os
import plistlib
import subprocess
import sys
import time

# The variables of the host's environment that the program reads.
PASSED = ("PR_", "MTL_")

# Each platform's simulator runtimes, by the start of their identifiers, and the name of its downloads in Xcode.
RUNTIMES = {
    "ios": ("com.apple.CoreSimulator.SimRuntime.iOS-", "iOS"),
    "tvos": ("com.apple.CoreSimulator.SimRuntime.tvOS-", "tvOS"),
    "visionos": ("com.apple.CoreSimulator.SimRuntime.xrOS-", "visionOS"),
}


def devices():
    """Returns the simulator devices that are available, by runtime."""
    listed = subprocess.run(["xcrun", "simctl", "list", "devices", "available", "--json"], check=True,
                            capture_output=True, text=True).stdout
    return json.loads(listed)["devices"]


def device(platform):
    """Returns the UDID of the device of `platform` to run on, booting it if it is not booted."""
    wanted = os.environ.get("PR_SIMULATOR_DEVICE")
    prefix, download = RUNTIMES[platform]
    found = [d for runtime, listed in devices().items() if runtime.startswith(prefix) for d in listed]
    if wanted:
        chosen = next((d for d in found if wanted in (d["name"], d["udid"])), None)
        if chosen is None:
            sys.exit("simulator: no available %s device named %s" % (download, wanted))
    else:
        chosen = next((d for d in found if d["state"] == "Booted"), None) or next(iter(found), None)
        if chosen is None:
            sys.exit("simulator: no %s simulator is available (xcodebuild -downloadPlatform %s)" % (download, download))
    # The tests run many programs at once, which may all find the device shut down. bootstatus boots it, or waits
    # for the boot that another started, until the device is ready.
    if chosen["state"] != "Booted":
        subprocess.run(["xcrun", "simctl", "bootstatus", chosen["udid"], "-b"], check=True, stdout=subprocess.DEVNULL)
    return chosen["udid"]


def environment():
    """The host's environment, with the variables that the program reads passed to it through simctl."""
    passed = dict(os.environ)
    for name, value in os.environ.items():
        if name.startswith(PASSED):
            passed["SIMCTL_CHILD_" + name] = value
    return passed


def show(udid):
    """Shows the device's screen in Xcode's viewer of devices: Simulator, which opens on the device, or in Xcode 27 and
    later DeviceHub, which shows the booted devices."""
    developer = subprocess.run(["xcode-select", "-p"], capture_output=True, text=True).stdout.strip()
    viewers = (
        (os.path.join(developer, "Applications", "Simulator.app"), ["--args", "-CurrentDeviceUDID", udid]),
        (os.path.join(os.path.dirname(developer), "Applications", "DeviceHub.app"), []),
    )
    for viewer, arguments in viewers:
        if os.path.isdir(viewer):
            subprocess.run(["open", "-a", viewer, *arguments], check=False)
            return
    print("simulator: Xcode has no viewer of devices to show the device in", file=sys.stderr)


def launch(platform, bundle, args, screenshot=None, seconds=4.0):
    """Installs the application bundle `bundle` on a device of `platform` and launches it with `args`, and returns its
    exit status. The application's output streams here until it exits, or until an interrupt, which terminates it. With
    `screenshot`, the path of a PNG, the device is not shown: the application runs for `seconds`, the device's screen is
    captured, and the application is terminated."""
    with open(os.path.join(bundle, "Info.plist"), "rb") as f:
        identifier = plistlib.load(f)["CFBundleIdentifier"]
    udid = device(platform)
    if screenshot is None:
        show(udid)
    subprocess.run(["xcrun", "simctl", "install", udid, bundle], check=True)
    # A terminal gets the output as the application writes it, through a pseudo-terminal. A pipe gets it as the
    # application's C library flushes it.
    console = "--console-pty" if sys.stdout.isatty() and screenshot is None else "--console"
    command = ["xcrun", "simctl", "launch", console, "--terminate-running-process", udid, identifier, *args]
    running = subprocess.Popen(command, env=environment())
    try:
        if screenshot is None:
            return running.wait()
        deadline = time.monotonic() + seconds
        while running.poll() is None and time.monotonic() < deadline:
            time.sleep(0.1)
        if running.poll() is not None:
            print("simulator: %s exited before the screenshot" % identifier, file=sys.stderr)
            return running.returncode or 1
        subprocess.run(["xcrun", "simctl", "io", udid, "screenshot", screenshot], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print("wrote %s" % screenshot)
        return 0
    except KeyboardInterrupt:
        return 130
    finally:
        if running.poll() is None:
            subprocess.run(["xcrun", "simctl", "terminate", udid, identifier], check=False,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                running.wait(timeout=5)
            except subprocess.TimeoutExpired:
                running.kill()


def main():
    if len(sys.argv) < 4 or sys.argv[1] != "--platform" or sys.argv[2] not in RUNTIMES:
        sys.exit(__doc__)
    run = subprocess.run(["xcrun", "simctl", "spawn", device(sys.argv[2]), *sys.argv[3:]], env=environment(),
                         check=False)
    sys.exit(run.returncode)


if __name__ == "__main__":
    main()
