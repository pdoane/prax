"""The browsers that the engine is compared with, launched with fresh profiles, the server of the checkout that their
pages load from, and the time on cores that a browser's processes spend.

Chrome and Firefox each run in a fresh profile, without extensions and without the throttling that applies to a
background window, and are closed afterward. Firefox's profile also turns off the rounding of its clock. WebKit, the
engine that Safari uses, runs in tools/browsers/webkit_host.cpp instead of the developer's Safari. The host is a browser
of lib/app's Cocoa backend, over a data store that keeps nothing. A browser loads its page from a server of the
checkout, which sends the headers that isolate the page across origins, so that the page's clock is as fine as the
browser allows.
"""


import ctypes
import http.server
import json
import os
import subprocess
import sys
import threading

import tools

class Browser:
    """A browser to compare with. It is either an application, or `host`, a harness in the build that the runner
    launches with a URL. `arguments(profile)` returns the arguments that precede the URL, `profile(directory,
    preferences)` writes a fresh profile into the directory, with `preferences` beside its own, and `environment` holds
    variables added to the browser's environment."""

    def __init__(self, app=None, executable=None, arguments=None, profile=None, host=None, environment=None):
        self.app = app
        self.executable = executable
        self.arguments = arguments or (lambda profile: [])
        self.profile = profile
        self.host = host
        self.environment = environment or {}

    def path(self, directory):
        """Returns the program to launch, given that the harnesses are in `directory`."""
        return os.path.join(directory, self.host) if self.host else os.path.join(self.app, "Contents", "MacOS",
                                                                                 self.executable)

    def installed(self, directory):
        return os.path.exists(self.path(directory))

    def version(self):
        if self.host:
            plist = "/System/Library/Frameworks/WebKit.framework/Resources/Info.plist"
            key = "CFBundleVersion"
        else:
            plist = os.path.join(self.app, "Contents", "Info.plist")
            key = "CFBundleShortVersionString"
        return subprocess.run(["plutil", "-extract", key, "raw", plist], capture_output=True, text=True).stdout.strip()

    def launch(self, url, profile, directory, extra=(), preferences=None):
        """Opens `url`, with `extra` arguments after the browser's own and `preferences` in its profile, and returns the
        process to stop once the page has reported. A host prints the names of the other processes that it runs the page
        in (tools/browsers/webkit_host.cpp)."""
        if self.profile:
            self.profile(profile, preferences or {})
        return subprocess.Popen([self.path(directory)] + self.arguments(profile) + list(extra) + [url], text=True,
                                env=dict(os.environ, **self.environment),
                                stdout=subprocess.PIPE if self.host else subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)


def chrome_arguments(profile):
    return ["--user-data-dir=" + profile, "--no-first-run", "--no-default-browser-check", "--disable-extensions",
            "--use-mock-keychain", "--password-store=basic", "--disable-background-timer-throttling",
            "--disable-renderer-backgrounding", "--disable-backgrounding-occluded-windows", "--new-window"]


# Preferences for a fresh Firefox profile: no first-run pages, no update checks, and a clock that is not rounded.
FIREFOX_PREFERENCES = {
    "app.update.disabledForTesting": True,
    "browser.shell.checkDefaultBrowser": False,
    "browser.aboutwelcome.enabled": False,
    "browser.startup.homepage_override.mstone": "ignore",
    "datareporting.policy.dataSubmissionEnabled": False,
    "toolkit.telemetry.reportingpolicy.firstRun": False,
    "browser.sessionstore.resume_from_crash": False,
    "privacy.reduceTimerPrecision": False,
    # A page on the loopback server is local, so no local network access prompt is shown.
    "network.lna.enabled": False,
    # Every permission that a page might request is denied without a prompt.
    "permissions.default.desktop-notification": 2,
    "permissions.default.geo": 2,
    "permissions.default.camera": 2,
    "permissions.default.microphone": 2,
    "permissions.default.xr": 2,
    "dom.storage_access.prompt.testing": True,
    "dom.storage_access.prompt.testing.allow": False,
}


def firefox_profile(profile, preferences=None):
    with open(os.path.join(profile, "user.js"), "w", encoding="utf-8", newline="\n") as f:
        for key, value in dict(FIREFOX_PREFERENCES, **(preferences or {})).items():
            f.write("user_pref(%s, %s);\n" % (json.dumps(key), json.dumps(value)))


BROWSERS = {
    "chrome": Browser("/Applications/Google Chrome.app", "Google Chrome", chrome_arguments),
    # The system's WebKit, which Safari uses, in a window of its own instead of in the developer's Safari.
    "webkit": Browser(host="webkit_host"),
    # Firefox installs a staged update the next time it launches, and macOS asks the terminal that launched it for
    # permission. A run leaves the update for the next time the developer launches Firefox.
    "firefox": Browser("/Applications/Firefox.app", "firefox",
                       lambda profile: ["-no-remote", "-profile", profile, "-new-window"], firefox_profile,
                       environment={"MOZ_DISABLE_UPDATE_PROCESSING": "1"}),
}


# ----------------------------------------------------------------------------------------------------------------------
# Serving


def url_of(path):
    relative = os.path.relpath(path, tools.ROOT)
    if relative.startswith(".."):
        sys.exit("%s is not under %s, which is what a browser is served" % (path, tools.ROOT))
    return "/" + relative


class Handler(http.server.SimpleHTTPRequestHandler):
    """Serves the checkout and /config with cross-origin isolation, and receives the results that the page posts to
    /results."""

    server_version = "bench"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=tools.ROOT, **kwargs)

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/cpu":
            used = browser_time(self.server.browser, self.server.others)
            if used is None:
                self.send_error(404)
                return
            body = ("%.6f" % used).encode("utf-8")
        elif path == "/config":
            body = json.dumps(self.server.config).encode("utf-8")
        else:
            return super().do_GET()
        self.send_response(200)
        self.send_header("Content-Type", "application/json" if path == "/config" else "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        size = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(size).decode("utf-8")
        self.send_response(204)
        self.end_headers()
        self.server.report = json.loads(body)
        self.server.reported.set()

    def log_message(self, *args):
        pass


class Server(http.server.ThreadingHTTPServer):
    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.config = None
        self.report = None
        self.reported = threading.Event()
        self.browser = None         # the browser's main process; /cpu reports the time on cores of its processes
        self.others = set()         # processes that the browser named but did not launch itself
        threading.Thread(target=self.serve_forever, daemon=True).start()

    def wait(self, timeout=None):
        """Returns what the page reported, or None if it reported nothing in time. If the page posted an error, the run
        stops."""
        if not self.reported.wait(timeout):
            return None
        if "error" in self.report:
            print("  stopped: %s" % self.report["error"], file=sys.stderr)
            return None
        return self.report


# The maximum number of processes counted for a browser, and the library that reports the time each process spent.
MAX_PROCESSES = 1024
LIBPROC = ctypes.CDLL("/usr/lib/libproc.dylib") if sys.platform == "darwin" else None


class Timebase(ctypes.Structure):
    _fields_ = [("numer", ctypes.c_uint32), ("denom", ctypes.c_uint32)]


class Usage(ctypes.Structure):
    """rusage_info_v0: the user and system times, in units of the absolute clock, followed by the remaining fields."""
    _fields_ = [("uuid", ctypes.c_uint8 * 16), ("user", ctypes.c_uint64), ("system", ctypes.c_uint64)] \
        + [("rest%d" % i, ctypes.c_uint64) for i in range(8)]


def browser_time(root, others=()):
    """Returns the milliseconds that a browser has run on cores, summed over its threads, or None if there is no
    process. The browser's processes are its main process `root`, the descendants of `root`, and `others`, the
    processes that it named and something else launched."""
    if root is None or LIBPROC is None:
        return None
    counted = {root} | set(others)
    pending = [root]
    children = (ctypes.c_int * MAX_PROCESSES)()
    while pending:
        found = LIBPROC.proc_listchildpids(pending.pop(), children, ctypes.sizeof(children))
        for i in range(max(0, min(found, MAX_PROCESSES))):
            if children[i] not in counted:
                counted.add(children[i])
                pending.append(children[i])
    timebase = Timebase()
    ctypes.CDLL(None).mach_timebase_info(ctypes.byref(timebase))
    total = 0
    for pid in counted:
        usage = Usage()
        if LIBPROC.proc_pid_rusage(pid, 0, ctypes.byref(usage)) == 0:
            total += usage.user + usage.system
    return total * timebase.numer / timebase.denom / 1e6


def read_processes(process, into):
    """Adds each process that a host names on its output (`<role> <pid>`) to `into`, until the host exits."""
    for line in process.stdout:
        parts = line.split()
        if len(parts) == 2 and parts[1].isdigit():
            into.add(int(parts[1]))


def chosen(names, directory):
    if not names:
        return [name for name, browser in BROWSERS.items() if browser.installed(directory)]
    for name in names:
        if name not in BROWSERS:
            sys.exit("no browser named %s; one of: %s" % (name, ", ".join(BROWSERS)))
        if not BROWSERS[name].installed(directory):
            sys.exit("%s is not installed" % name)
    return names
