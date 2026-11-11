"""Runs application bundles built for iOS devices.

`./dev.sh example ios` and `./dev.sh app ios` launch an application bundle through launch(): it is signed for the
device, installed on it and launched with the arguments, and the application's output streams back until it exits.

The device is the one that PR_IOS_DEVICE names, by its name or its UDID, or else the first connected iPhone or iPad.
The application's environment reaches it through devicectl's DEVICECTL_CHILD_ prefix, so a variable that it reads, such
as PR_GPU_BACKEND, is passed as it is set here.

The build signs a bundle ad hoc, which a device refuses. A device runs an application signed by a development
certificate whose provisioning profile lists the device and covers the bundle's identifier, so the bundle is signed
again here, with a profile that Xcode keeps for the developer's team (profiles) and an identity of the keychain that
the profile names."""

import datetime
import fnmatch
import hashlib
import json
import os
import plistlib
import signal
import subprocess
import sys
import tempfile
import time

from tools import dev

# The variables of the host's environment that the application reads.
PASSED = ("PR_", "MTL_")

# Where Xcode keeps the provisioning profiles that it downloads for the developer's teams: Xcode 16 and later, and
# earlier.
PROFILE_DIRECTORIES = (
    os.path.expanduser("~/Library/Developer/Xcode/UserData/Provisioning Profiles"),
    os.path.expanduser("~/Library/MobileDevice/Provisioning Profiles"),
)

# How to make a device ready for development, which a failure to find one or to sign for one points at.
SETUP = "docs/testing.md, Running on an iOS device"


def devicectl(*args):
    """Runs devicectl's `args` and returns the result of its JSON output."""
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "result.json")
        run = subprocess.run(["xcrun", "devicectl", *args, "--quiet", "--json-output", path], capture_output=True,
                             text=True)
        if run.returncode != 0:
            dev.fail("devicectl %s failed:\n%s" % (" ".join(args), (run.stderr or run.stdout).strip()))
        with open(path) as f:
            return json.load(f)["result"]


def device():
    """Returns the UDID and the name of the device to run on: an iPhone or an iPad that is paired with this Mac."""
    found = [d for d in devicectl("list", "devices")["devices"]
             if d.get("visibilityClass") != "simulators" and d["hardwareProperties"].get("platform") == "iOS"]
    wanted = os.environ.get("PR_IOS_DEVICE")
    if wanted:
        found = [d for d in found if wanted in (d["deviceProperties"].get("name"), d["hardwareProperties"]["udid"])]
        if not found:
            dev.fail("no iOS device named %s is paired with this Mac" % wanted)
    connected = [d for d in found if d["connectionProperties"].get("tunnelState") != "unavailable"]
    if not connected:
        dev.fail("no iPhone or iPad is connected and paired with this Mac (%s)" % SETUP)
    chosen = connected[0]
    return chosen["hardwareProperties"]["udid"], chosen["deviceProperties"].get("name")


def identities():
    """Returns the SHA-1 hashes of the keychain's valid code signing identities, by name."""
    listed = dev.output(["security", "find-identity", "-v", "-p", "codesigning"])
    found = {}
    for line in listed.splitlines():
        words = line.split(None, 2)
        if len(words) == 3 and words[0].endswith(")"):
            found[words[1]] = words[2].strip().strip('"')
    return found


def profiles():
    """Returns the provisioning profiles that Xcode keeps, decoded, with the path of each as `Path`."""
    found = []
    for directory in PROFILE_DIRECTORIES:
        if not os.path.isdir(directory):
            continue
        for name in sorted(os.listdir(directory)):
            if not name.endswith(".mobileprovision"):
                continue
            path = os.path.join(directory, name)
            decoded = subprocess.run(["security", "cms", "-D", "-i", path], capture_output=True).stdout
            if decoded:
                found.append(dict(plistlib.loads(decoded), Path=path))
    return found


def signing(identifier, udid, name):
    """Returns the provisioning profile, and the SHA-1 hash of the keychain's identity, that sign the bundle
    `identifier` for the device `udid`. Of the profiles that cover it, the one whose application identifier is the
    most specific is chosen, and the newest of those."""
    keychain = identities()
    now = datetime.datetime.now(datetime.timezone.utc)
    candidates = []
    for profile in profiles():
        if "iOS" not in profile.get("Platform", []):
            continue
        if profile["ExpirationDate"].replace(tzinfo=datetime.timezone.utc) <= now:
            continue
        if not profile.get("ProvisionsAllDevices") and udid not in profile.get("ProvisionedDevices", []):
            continue
        entitlements = profile.get("Entitlements", {})
        team = profile["TeamIdentifier"][0]
        pattern = entitlements.get("application-identifier", "")
        if not fnmatch.fnmatchcase("%s.%s" % (team, identifier), pattern):
            continue
        hashes = [hashlib.sha1(certificate).hexdigest().upper() for certificate in profile["DeveloperCertificates"]]
        identity = next((h for h in hashes if h in keychain), None)
        if identity is None:
            continue
        candidates.append((pattern.count("*") == 0, profile["CreationDate"], profile, identity))
    if not candidates:
        dev.fail("no provisioning profile of this Mac's teams lists %s (%s) and covers %s with a signing identity in "
                 "the keychain: register the device with the team (%s)" % (name, udid, identifier, SETUP))
    _, _, profile, identity = max(candidates, key=lambda c: (c[0], c[1]))
    return profile, identity


def sign(bundle, identifier, udid, name):
    """Signs the application bundle `bundle`, of the bundle identifier `identifier`, to run on the device `udid`: it
    embeds the profile, and is signed with the entitlements that the profile grants the identifier."""
    profile, identity = signing(identifier, udid, name)
    team = profile["TeamIdentifier"][0]
    granted = profile.get("Entitlements", {})
    entitlements = {
        "application-identifier": "%s.%s" % (team, identifier),
        "com.apple.developer.team-identifier": team,
        "get-task-allow": granted.get("get-task-allow", False),
    }
    with open(profile["Path"], "rb") as source, open(os.path.join(bundle, "embedded.mobileprovision"), "wb") as copy:
        copy.write(source.read())
    with tempfile.NamedTemporaryFile("wb", suffix=".entitlements", delete=False) as f:
        plistlib.dump(entitlements, f)
    try:
        dev.run(["codesign", "--force", "--timestamp=none", "--sign", identity, "--entitlements", f.name, bundle],
                stdout=subprocess.DEVNULL)
    finally:
        os.unlink(f.name)
    print("signed %s for %s with %s" % (identifier, name, profile["Name"]))


def environment():
    """The host's environment, with the variables that the application reads passed to it through devicectl."""
    passed = dict(os.environ)
    for variable, value in os.environ.items():
        if variable.startswith(PASSED):
            passed["DEVICECTL_CHILD_" + variable] = value
    return passed


def launch(bundle, args, screenshot=None, seconds=4.0):
    """Signs the application bundle `bundle` for the device, installs it there and launches it with `args`, and
    returns its exit status. The application's output streams here until it exits, or until an interrupt, which
    terminates it. With `screenshot`, the path of a PNG, the application runs for `seconds`, the device's screen is
    captured, and the application is terminated."""
    with open(os.path.join(bundle, "Info.plist"), "rb") as f:
        identifier = plistlib.load(f)["CFBundleIdentifier"]
    udid, name = device()
    sign(bundle, identifier, udid, name)
    devicectl("device", "install", "app", "--device", udid, bundle)
    # devicectl forwards a catchable signal to the application that it attached to the console.
    command = ["xcrun", "devicectl", "device", "process", "launch", "--console", "--terminate-existing", "--device",
               udid, identifier, *args]
    running = subprocess.Popen(command, env=environment())
    try:
        if screenshot is None:
            return running.wait()
        deadline = time.monotonic() + seconds
        while running.poll() is None and time.monotonic() < deadline:
            time.sleep(0.1)
        if running.poll() is not None:
            print("device: %s exited before the screenshot" % identifier, file=sys.stderr)
            return running.returncode or 1
        devicectl("device", "capture", "screenshot", "--device", udid, "--destination", screenshot)
        print("wrote %s" % screenshot)
        return 0
    except KeyboardInterrupt:
        return 130
    finally:
        if running.poll() is None:
            running.send_signal(signal.SIGTERM)
            try:
                running.wait(timeout=5)
            except subprocess.TimeoutExpired:
                running.kill()
