#!/usr/bin/env python3
"""Runs web-platform-tests in pr_shell, with WPT's harness and a vendor result reporter.

A run covers one suite: the directories of the checkout that SUITES names for it, without those of NOT_RUN. Its tests
are found as WPT's manifest finds them. They are every page except those in a support or resources directory and the
references that a reftest compares against. They also include every .any.js and .window.js file, which runs in a page
made for it, as WPT's server makes one. A testharness page's cases are its subtests, keyed "<file>:<index>" in
registration order, where <file> is the page's path in the checkout. A crashtest is one case, which passes if the page
loads and becomes ready, as wptrunner waits for it (its root without the class test-wait), without the shell failing.

A page runs on a virtual clock (pr_shell --virtual-time) under the shell's own deadline, and a shell that takes more
memory than SHELL_MEMORY_LIMIT is killed (MemoryGuard), so that a page fails on its own rather than slow or starve the
run's other pages.

A reftest is one case. It is rendered if the test and each of its references can be rendered at WPT's 800 by 600: an
HTML, XHTML or SVG document loads in the shell, as a browser loads it, with its scripts, style sheets and fonts. Where
the shell serves, one run of it renders the test and its references once they are ready, and compares them
(--reference). Otherwise the shell captures each page as a PNG (--screenshot), and the reftest harness
(tools/suite/reftest.cpp) compares them. A print reftest, which WPT's manifest finds by its name (is_print), is
rendered if each of its documents is HTML or XHTML. The shell prints the test and its references, from
the page box of WPT's print reftests, and renders each page that the test's reftest-pages metas select as an image of
its own (pr_shell --print). Two documents then render the same if they have as many pages and each page renders the
same as the page in the same place, each within the fuzzy ranges (web-platform-tests docs, "Print Reftests"). A
reftest is judged as wptrunner judges it: it passes if it renders the same as one of its `match` references, within
the ranges that a fuzzy meta gives, and that reference and the test then differ from the `mismatch` references along
wptrunner's chain of them (reftest_outcome). Renders are compared as wptrunner compares them, where a pixel differs
only if the luma of its channels' differences is nonzero. suites/wpt/fuzzy.txt overrides a test's fuzzy ranges, as a
browser's metadata does, each with its reason. --write keeps each reftest's render, its reference and their
difference, as tools/suite/reftest.py does, and those of a print reftest's later page with `-page<n>` after the stem.
A match in which the engine draws nothing for both the test and its reference does not pass, because two renders of
nothing agree without testing anything. It counts as a file that completed and a test that failed, so that what the
engine draws does not change what is counted. A reftest that is not rendered is listed and not counted. Results are one
row per module: a directory's first two levels, with CSS2 split by its subdirectories.

Every page runs against the engine as it is. Nothing stands in for what the engine lacks: a page that uses an entry
point that the engine does not define fails as it would in an engine without it. The shell gives every window WPT's
vendor files (tools/suite/wpt): the reporter, a testdriver that drives clicks, keys and action sequences through the
page's view as a host's input, and a fetch() of the checkout's resources.

A plain run takes every test of the suite except those that a person runs, which wptrunner leaves out too
(is_manual). --module takes the tests of a plain run that are in the modules that it names, and --all takes every
test. --match takes the tests that it names,
and names each of their assertions that does not pass, as --failures does. A plain run and a run of modules leave out
each page that tests a feature that tools/suite/wpt/web_features.txt does not rule as built by the engine. The
WEB_FEATURES.yml files of the checkout map pages to features. --left-out lists every page that a plain run of any suite
leaves out, with what it needs, for the viewer to mark.

A file is complete if its harness reports OK and the shell exits cleanly. A test counts if it passes or the errata
record it. A recorded case is one in which the engine follows the spec and the corpus does not. A file with no
subtests counts as one test unless the errata excuse it. --raw counts as WPT does: a test counts only if it passes.

--verify checks the engine's incremental rendering as the pages run. The shell compares each page's render with a full
recomputation after every rendering update and once the run ends (pr_shell --verify). The pages whose render
failed a check are listed after the table, each with its first difference. They are counted separately from the
results, which the checks do not change, so a plain run with --verify writes the same scoreboard as a run without it.

--pixel-ratio renders and compares every page at that many target pixels per CSS px, 1 by default, where a reftest's
test and references snap alike. Pixel snapping shows at a ratio such as 1.5 or 2 more than at 1, where most fractional
positions fall on halves and quarters of a pixel. A run at another ratio leaves the scoreboard alone.

--renderer <name> renders every page with that renderer of lib/draw, on the platform's device for a renderer that
renders on one, instead of the cpu renderer, where a reftest's test and references are drawn by the same renderer. A run
on it leaves the scoreboard alone.

A plain run of the default build's shell writes its suite's table in suites/wpt/scoreboard.md, and a run of modules
writes those modules' rows of it, whose selection is a plain run's. The table gives each module's files that are
complete or excused, its tests passed and its pass rate, and the total, which is the sum of the rows. The scoreboard is
committed with the change that alters it, so its diff shows what a change did: a rate that fell, and whether the
passing tests or the counted tests changed it.

Usage: python3 -m tools.suite.wpt.wpt [SUITE] [--binary PATH] [--tests DIR] [--match GLOB] [--verbose]
                      [--list] [--failures] [--first-errors] [--raw] [--json FILE] [--jobs N]
                      [--verify] [--pixel-ratio PIXELS] [--renderer NAME] [--module GLOB | --all | --left-out]
"""

import argparse
import atexit
import collections
import concurrent.futures
import json
import fnmatch
import os
import pathlib
import posixpath
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

from tools.suite import corpus
from tools.suite import reftest
from tools.suite.wpt import web_features
from tools.suite.wpt import wpt_serve

# The real path of the checkout. The base runner of an A/B reaches third_party through a link, and the paths of the
# tests through that link can exceed Windows' limit of 260 characters.
WPT = os.path.realpath(os.path.join(corpus.ROOT, "third_party", "wpt"))
CSS = os.path.join(WPT, "css")
# The suites, each with the directories of the checkout that it runs. A directory belongs to the suite of the standard
# that it tests, so a suite is a family of specs, not a directory of WPT's.
SUITES = {
    "css": ("css", "web-animations", "compat", "resize-observer"),
    "dom": ("dom", "domparsing", "selection", "shadow-dom", "uievents", "pointerevents", "input-events", "webidl",
            "intersection-observer"),
    "encoding": ("encoding",),
    "html": ("html", "custom-elements", "focus", "inert", "quirks", "html-ruby-extensions", "png", "apng", "gif",
             "webstorage", "workers", "webmessaging", "requestidlecallback"),
    "infrastructure": ("infrastructure",),
    "mathml": ("mathml",),
    "media-source": ("media-source",),
    "networking": ("fetch", "beacon", "referrer-policy", "cors", "FileAPI", "streams", "websockets"),
    "performance": ("hr-time", "performance-timeline", "user-timing", "navigation-timing", "resource-timing",
                    "server-timing"),
    "svg": ("svg",),
    "url": ("url",),
    "webaudio": ("webaudio",),
    "xhr": ("xhr",),
}
# The directories inside a suite's that no suite runs.
NOT_RUN = {
    "css/css-device-adapt", "css/css-exclusions", "css/css-layout-api", "css/css-link-params", "css/css-navigation",
    "html/capability-delegation", "html/cross-origin-embedder-policy", "html/cross-origin-opener-policy",
    "html/document-isolation-policy", "html/syntax/speculative-parsing",
}

SHELL = corpus.binary("pr_shell")
SHELL_EXIT_TIMEOUT = 60  # seconds that a shell has to exit once its input closes
# The vendor's files, which serve the checkout as WPT's server serves it to a vendor's runner.
SUPPORT = os.path.join(corpus.ROOT, "tools", "suite", "wpt")
# The data of every WPT suite (corpus.Data).
DATA = corpus.Data("wpt")
TIMEOUT = 10 * corpus.SLOWDOWN  # seconds; the runner enforces the deadline that report.js leaves to it
LONG_TIMEOUT = 60 * corpus.SLOWDOWN  # seconds, for a test that marks itself long (wptrunner's long_timeout)
# The most resident memory that a shell may hold, in megabytes, before the runner kills it (MemoryGuard). A page that
# passes it fails, as one that passes its deadline does.
SHELL_MEMORY_LIMIT = 2048

# Matches a test's mark that it takes WPT's long timeout: a page's <meta name=timeout content=long>, or a script's
# `// META: timeout=long` comment (WPT's manifest, tools/manifest/sourcefile.py).
LONG_MARK = re.compile(r"<meta\s+name=[\"']?timeout[\"']?\s+content=[\"']?long[\"']?|^//\s*META:\s*timeout=long\s*$",
                       re.M | re.I)


# The shell's options for every run beyond those that reach WPT's server, which the command line adds to (setup()).
SETUP = []

# The hosts that WPT's tests name, which its server answers on the loopback (WPT's config.json "browser_host" and
# "alternate_hosts", with their subdomains).
HOSTS = ("web-platform.test", "*.web-platform.test", "not-web-platform.test", "*.not-web-platform.test")

# wptrunner's wait for a reftest's screenshot (test-wait.js), its class name filled in as its executor fills it in.
TEST_WAIT = os.path.join(WPT, "tools", "wptrunner", "wptrunner", "executors", "test-wait.js")

# The URL path at which the server serves the run's scratch directory, where the runner's own tests write pages.
SCRATCH_PATH = "/_pr/"

# WPT's server for the run (tools/suite/wpt/wpt_serve.py), the scratch directory that it serves, and the directory of
# the files that the shell reads (capture_wait), once a shell needs them.
SERVER = None
SCRATCH_DIRECTORY = None
SHELL_DIRECTORY = None
server_lock = threading.Lock()


def server():
    """Returns WPT's server for the run, which starts the first time it is asked for and stops when the run exits. It
    serves the checkout, and the scratch directory at SCRATCH_PATH."""
    global SERVER, SCRATCH_DIRECTORY, SHELL_DIRECTORY
    with server_lock:
        if SERVER is None:
            SCRATCH_DIRECTORY = tempfile.mkdtemp(prefix="wpt-scratch-")
            SHELL_DIRECTORY = tempfile.mkdtemp(prefix="wpt-shell-")
            SERVER = wpt_serve.Server(WPT, {SCRATCH_PATH: SCRATCH_DIRECTORY}, clocked=True)
            atexit.register(stop_server)
        return SERVER


def stop_server():
    if SERVER is not None:
        SERVER.stop()
        shutil.rmtree(SCRATCH_DIRECTORY, ignore_errors=True)
        shutil.rmtree(SHELL_DIRECTORY, ignore_errors=True)


def capture_wait(classname="reftest-wait"):
    """Returns the script that the shell runs in a page that it waits for (pr_shell --capture-wait): wptrunner's wait,
    with its class name filled in, reftest-wait for a reftest's screenshot and test-wait for a crashtest."""
    server()
    path = os.path.join(SHELL_DIRECTORY, classname + ".js")
    with server_lock:
        if not os.path.exists(path):
            with open(TEST_WAIT, encoding="utf-8") as f:
                source = f.read().replace("%(classname)s", classname)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(source)
    return path


def network_options():
    """Returns the shell's options that reach WPT's server as a browser under wptrunner reaches it: WPT's hosts resolve
    to the loopback, and its CA is trusted."""
    options = ["--ca", os.path.join(WPT, "tools", "certs", "cacert.pem")]
    for host in HOSTS:
        options += ["--resolve", host, "127.0.0.1"]
    return options


def scratch_directory():
    """Returns the directory that the server serves at SCRATCH_PATH, for pages that the runner writes."""
    server()
    return SCRATCH_DIRECTORY


# The page that the server makes for a .any.js test in each of its globals (WPT's manifest), in the order in which a
# run prefers them, and the globals that a test runs in if it names none. "worker" names every kind of worker.
GLOBAL_PAGES = (("window", ".any.html"), ("dedicatedworker", ".any.worker.html"),
                ("sharedworker", ".any.sharedworker.html"), ("serviceworker", ".any.serviceworker.html"))
DEFAULT_GLOBALS = {"window", "dedicatedworker"}
SCRIPT_GLOBAL = re.compile(r"^//\s*META:\s*global=(.*)$", re.M)


def script_suffix(path):
    """Returns the suffix that replaces .js in the URL of the page that the server makes for the script test at `path`:
    its window page, if the test runs in a window, or else the page of the first of its globals. A test that runs in no
    global that the server makes a page for runs in its window page, which the server answers with an error."""
    if path.endswith(".window.js"):
        return ".html"
    with open(path, encoding="utf-8") as f:
        named = SCRIPT_GLOBAL.findall(f.read())
    globals_ = {name.strip() for line in named for name in line.split(",")} if named else DEFAULT_GLOBALS
    if "worker" in globals_:
        globals_ |= {"dedicatedworker", "sharedworker", "serviceworker"}
    for name, page in GLOBAL_PAGES:
        if name in globals_:
            return page[len(".any"):]
    return ".html"


def url_of(path, variant=""):
    """Returns the URL at which the server serves the page at `path`, in the checkout or the scratch directory, with the
    variant that it runs as. A .any.js or .window.js test runs in the page that the server makes for it (script_suffix).
    A page whose name has `.https.` is served over HTTPS. A page that the server does not serve loads at its file:
    URL."""
    ports = server().ports
    scratch = os.path.relpath(path, SCRATCH_DIRECTORY)
    checkout = os.path.relpath(path, WPT)
    if not scratch.startswith(".."):
        resource = SCRATCH_PATH + scratch.replace(os.sep, "/")
    elif not checkout.startswith(".."):
        resource = "/" + checkout.replace(os.sep, "/")
    else:
        return pathlib.Path(os.path.abspath(path)).as_uri() + variant
    if resource.endswith(SCRIPTS):
        resource = resource[:-len(".js")] + script_suffix(path)
    if ".https." in os.path.basename(path):
        return "https://web-platform.test:%d%s%s" % (ports["https"][0], resource, variant)
    return "http://web-platform.test:%d%s%s" % (ports["http"][0], resource, variant)

# The options that hand the shell and the reftest harness the run's text backends, or none for their defaults
# (corpus.text_backends).
TEXT = []
# The directory that each reftest's render, reference and difference are written to, or None (--write).
WRITE = None
# The fuzzy overrides of suites/wpt/fuzzy.txt (read_fuzzy_overrides), by test.
FUZZY_OVERRIDES = {}

# A shell that accepts a deadline stops a page at TIMEOUT itself and exits with TIMED_OUT, keeping what the page wrote
# and its own process. The runner's deadline is then a backstop for a page whose script never returns to the shell.
TIMED_OUT = 124
BACKSTOP = 5 * corpus.SLOWDOWN  # seconds past TIMEOUT
# What a run that the runner stopped at its backstop says of the system's time deciding it (corpus.TIMING).
BACKSTOPPED = "the runner stopped the shell at its backstop"

# The status of a run whose shell the runner killed for its memory (MemoryGuard), and Shells.ask's answers for a served
# run that passed the backstop or its memory limit.
OVER_MEMORY = 137
TIMED_OUT_ANSWER = "timed out"
OVER_MEMORY_ANSWER = "over memory"


def over_memory():
    """Returns what a run that the runner killed for its memory says of it."""
    return "pr_shell passed its memory limit of %d MB and was killed" % SHELL_MEMORY_LIMIT

# Matches what a shell writes on stderr if a page's render fails a check under --verify.
VERIFY_FAILURE = re.compile(r"^(.*): render verify: (.*)$", re.M)

# Under --verify, the first failure of each page whose render failed a check, by the page's URL.
verify_failures = {}
verify_lock = threading.Lock()


def resident_memory(pids):
    """Returns the resident memory of each process of `pids` that is still running, in bytes, by pid."""
    if not pids:
        return {}
    if sys.platform == "win32":
        import ctypes
        from ctypes import wintypes

        class Counters(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("faults", wintypes.DWORD)] + [
                (name, ctypes.c_size_t) for name in ("peak_working_set", "working_set", "peak_paged_pool",
                                                     "paged_pool", "peak_nonpaged_pool", "nonpaged_pool",
                                                     "pagefile", "peak_pagefile")]
        sizes = {}
        for pid in pids:
            handle = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
            if not handle:
                continue
            counters = Counters(cb=ctypes.sizeof(Counters))
            if ctypes.windll.psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                sizes[pid] = counters.working_set
            ctypes.windll.kernel32.CloseHandle(handle)
        return sizes
    listed = subprocess.run(["ps", "-o", "pid=,rss=", "-p", ",".join(str(pid) for pid in pids)],
                            capture_output=True, text=True).stdout
    sizes = {}
    for line in listed.splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[0].isdigit() and fields[1].isdigit():
            sizes[int(fields[0])] = int(fields[1]) * 1024
    return sizes


class MemoryGuard:
    """Kills a shell whose resident memory passes SHELL_MEMORY_LIMIT, so that a page that takes memory without end fails
    on its own instead of exhausting the machine that every other job of the run shares. The limit is the same on every
    machine, so that a page's result does not depend on the machine that runs it."""

    INTERVAL = 0.25  # seconds between looks at the shells' memory

    def __init__(self):
        self.lock = threading.Lock()
        self.processes = {}  # the processes watched, by pid
        self.killed = set()  # the pids of the processes killed for their memory
        self.thread = None

    def start(self):
        if self.thread is None:
            self.thread = threading.Thread(target=self.watch_all, daemon=True)
            self.thread.start()

    def watch(self, process):
        with self.lock:
            self.processes[process.pid] = process

    def forget(self, process):
        """Stops watching `process`, and returns true if the guard killed it."""
        with self.lock:
            self.processes.pop(process.pid, None)
            return process.pid in self.killed

    def watch_all(self):
        while True:
            time.sleep(self.INTERVAL)
            with self.lock:
                watched = dict(self.processes)
            limit = SHELL_MEMORY_LIMIT * 1024 * 1024
            for pid, size in resident_memory(list(watched)).items():
                if size > limit and watched[pid].poll() is None:
                    with self.lock:
                        self.killed.add(pid)
                    watched[pid].kill()


MEMORY = MemoryGuard()


class Shells:
    """The shells that run pages, one for each thread of the pool. Each shell serves one run of pages after another in
    one process (pr_shell --serve), which saves a process start on every page. A shell that predates serving runs each
    page in its own process. A served run that crashes is run again in its own process, as without serving, and its
    shell is replaced, so that the run's result is the same as in its own process. A served run that passes the
    runner's deadline times out, and one whose shell passes its memory limit (MemoryGuard) fails, each without a run
    again, which would only take as long again or as much memory again."""

    def __init__(self):
        self.local = threading.local()
        self.lock = threading.Lock()
        self.serving = {}           # by binary: true if the shell supports serving
        self.started = []
        self.stuck = False

    def serves(self, binary):
        with self.lock:
            if binary not in self.serving:
                probe = subprocess.run([binary, "--serve"], input=b"", capture_output=True)
                self.serving[binary] = probe.returncode == 0
            return self.serving[binary]

    def setup(self, binary):
        """Returns the options for every run of the shell: WPT's server reached over the network (network_options), the
        fonts that WPT requires a browser to have installed, wptrunner's wait for a screenshot, a virtual clock that the
        server's sleeps move (wpt_serve.Control), and the deadline if the shell accepts one, which a shell that serves
        does."""
        return (network_options() + ["--font", os.path.join(WPT, "fonts", "Ahem.ttf"), "--capture-wait", capture_wait(),
                                     "--virtual-time", "--server-control", str(server().control)] + SETUP
                + (["--timeout", "%d" % (TIMEOUT * 1000)] if self.serves(binary) else []))

    def run(self, binary, args, timeout):
        """Returns the result of running the shell with its setup and `args`, as subprocess.run returns it with text
        output. Raises subprocess.TimeoutExpired as subprocess.run does. A shell killed for its memory exits with
        OVER_MEMORY, and says so on the first line of its stderr."""
        command = [binary] + self.setup(binary) + args
        if not self.serves(binary):
            return self.own_process(command, timeout)
        timeout += BACKSTOP
        answer = self.ask(binary, args, timeout)
        if answer is TIMED_OUT_ANSWER:
            raise subprocess.TimeoutExpired(command, timeout, output="", stderr="")
        if answer is OVER_MEMORY_ANSWER:
            return subprocess.CompletedProcess(command, OVER_MEMORY, "", over_memory())
        if answer is None:
            return self.own_process(command, timeout)
        status, out, err = answer
        return subprocess.CompletedProcess(command, status, out, err)

    @staticmethod
    def own_process(command, timeout):
        """Returns the result of running `command` in a process of its own, as Shells.run returns it."""
        # The shell writes UTF-8, whatever the platform's default encoding is.
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding="utf-8",
                                   errors="replace")
        MEMORY.watch(process)
        try:
            out, err = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            out, err = process.communicate()
            MEMORY.forget(process)
            raise subprocess.TimeoutExpired(command, timeout, output=out, stderr=err) from None
        if MEMORY.forget(process):
            return subprocess.CompletedProcess(command, OVER_MEMORY, out, over_memory() + "\n" + err)
        return subprocess.CompletedProcess(command, process.returncode, out, err)

    def ask(self, binary, args, timeout):
        """Returns the status, stdout and stderr of a served run, or TIMED_OUT_ANSWER if it passed the deadline,
        OVER_MEMORY_ANSWER if its shell passed its memory limit, or None if its shell crashed. In each of those cases
        the shell is replaced."""
        shells = self.local.__dict__.setdefault("shells", {})
        shell = shells.get(binary)
        if shell is None or shell.poll() is not None:
            shell = subprocess.Popen([binary] + self.setup(binary) + ["--serve"], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            shell.chunks = queue.SimpleQueue()
            shell.pending = bytearray()
            shell.pump = threading.Thread(target=self.pump, args=(shell,), daemon=True)
            shell.pump.start()
            shells[binary] = shell
            MEMORY.watch(shell)
            with self.lock:
                self.started.append(shell)
        data = "\n".join(args).encode()
        deadline = time.monotonic() + timeout
        try:
            shell.stdin.write(corpus.frame(data))
            shell.stdin.flush()
            head = self.read_line(shell, deadline)
            status, written, failed = (int(field) for field in head.split())
            out = self.read(shell, written, deadline)
            err = self.read(shell, failed, deadline)
        except (OSError, ValueError, TimeoutError) as error:
            shell.kill()
            shell.wait()
            shell.pump.join()
            shell.stdin.close()
            shell.stdout.close()
            shells.pop(binary, None)
            with self.lock:
                self.started.remove(shell)
            if MEMORY.forget(shell):
                return OVER_MEMORY_ANSWER
            return TIMED_OUT_ANSWER if isinstance(error, TimeoutError) else None
        return (status, out.decode("utf-8", errors="replace"),
                err.decode("utf-8", errors="replace"))

    @staticmethod
    def pump(shell):
        """Passes what the shell writes to its queue, ending with an empty chunk. A thread reads it because a pipe can
        only be waited on with a deadline this way on every platform."""
        fd = shell.stdout.fileno()
        while True:
            chunk = os.read(fd, 65536)
            shell.chunks.put(chunk)
            if not chunk:
                return

    @staticmethod
    def read(shell, count, deadline):
        data = shell.pending
        while len(data) < count:
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError()
            try:
                chunk = shell.chunks.get(timeout=left)
            except queue.Empty:
                raise TimeoutError() from None
            if not chunk:
                raise OSError("the shell ended")
            data += chunk
        result = bytes(data[:count])
        del data[:count]
        return result

    @classmethod
    def read_line(cls, shell, deadline):
        line = bytearray()
        while not line.endswith(b"\n"):
            line += cls.read(shell, 1, deadline)
        return bytes(line)

    def close(self):
        for shell in self.started:
            MEMORY.forget(shell)
            if shell.poll() is None:
                shell.stdin.close()
                try:
                    shell.wait(timeout=SHELL_EXIT_TIMEOUT)
                except subprocess.TimeoutExpired:
                    print("pr_shell %d did not exit in %d seconds, and was killed" % (shell.pid, SHELL_EXIT_TIMEOUT),
                          file=sys.stderr)
                    self.stuck = True
                    shell.kill()
                    shell.wait()


SHELLS = Shells()

# How long each test took when it last ran, in seconds, by name. The file is kept beside the harness, and is read so
# that a run starts its slowest tests first and does not end waiting on a slow test that started last.
DURATIONS_FILE = corpus.binary("wpt-durations.json")


def read_durations():
    try:
        with open(DURATIONS_FILE, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def write_durations(durations):
    try:
        with open(DURATIONS_FILE, "w", encoding="utf-8", newline="\n") as f:
            json.dump(durations, f, sort_keys=True)
    except OSError:
        pass


def timed(durations, name, function, *args):
    """Returns what `function(*args)` returns, and records how long it took as the duration of `name`."""
    started = time.monotonic()
    try:
        return function(*args)
    finally:
        durations[name] = round(time.monotonic() - started, 3)


def note_verify(error):
    """Records the render checks that failed in a shell's stderr."""
    for match in VERIFY_FAILURE.finditer(error or ""):
        with verify_lock:
            verify_failures.setdefault(match.group(1), match.group(2))

FUZZY_FILE = DATA.path("fuzzy.txt")
DEFERRED_FILE = DATA.path("deferred.txt")
CUT_FILE = DATA.path("cut.txt")
SKIP_FILE = DATA.path("skip.txt")
SCOREBOARD_FILE = DATA.path(corpus.SCOREBOARD)


# A row of the scoreboard: the files complete or excused and the tests passed, each as
# "done/count", and the pass rate.
Score = collections.namedtuple("Score", "files tests rate")


def score(counts):
    """Returns the Score of `counts`."""
    return Score("%d/%d" % (counts["completed"] + counts["excused"], counts["files"]),
                 "%d/%d" % (counts["passed"], counts["tests"]), corpus.pass_rate(counts["passed"], counts["tests"]))


def parse_scoreboard(text):
    """Returns the Scores of a scoreboard's tables, by suite, then by module and TOTAL."""
    boards, suite = {}, None
    for line in text.splitlines():
        if line.startswith("## "):
            suite = boards.setdefault(line[3:].strip(), {})
        elif suite is not None and line.startswith("|"):
            cells = [cell.strip().strip("*") for cell in line.strip().strip("|").split("|")]
            if len(cells) == 4 and cells[3].endswith("%"):
                suite[cells[0]] = Score(cells[1], cells[2], float(cells[3][:-1]))
    return boards


def format_scoreboard(boards):
    """Returns a scoreboard of `boards`: one table per suite, with its total last."""
    text = ["# WPT scoreboard", "",
            "What a plain run of each suite of tools/suite/wpt/wpt.py passes: of each module, the files "
            "complete",
            "or excused, the tests passed, and the rate. A plain run writes its suite's table, and a run of "
            "modules", "their rows; the diff of a change to it is what the change did.", ""]
    for suite in sorted(boards):
        rows = [(module, boards[suite][module]) for module in sorted(boards[suite])
                if module != corpus.TOTAL]
        if corpus.TOTAL in boards[suite]:
            rows.append(("**%s**" % corpus.TOTAL, boards[suite][corpus.TOTAL]))
        cells = [(module, row.files, row.tests, "%.1f%%" % row.rate) for module, row in rows]
        text += ["## " + suite, ""] + corpus.table(("Module", "Files", "Tests", "Passing"), cells) + [""]
    return "\n".join(text)


def in_modules(module, patterns):
    """Returns true if a run of the modules `patterns` (--module) selects the module `module`."""
    return any(fnmatch.fnmatchcase(module, pattern) or fnmatch.fnmatchcase(module.split("/")[0], pattern)
               for pattern in patterns)


def total_of(rows):
    """Returns the Score that sums the Scores `rows`."""
    sums = [0, 0, 0, 0]
    for row in rows:
        for i, cell in enumerate((row.files, row.tests)):
            done, count = cell.split("/")
            sums[2 * i] += int(done)
            sums[2 * i + 1] += int(count)
    return Score("%d/%d" % (sums[0], sums[1]), "%d/%d" % (sums[2], sums[3]), corpus.pass_rate(sums[2], sums[3]))


def record_scoreboard(suite, table, modules=()):
    """Writes `table`, by module, as the suite's table of the scoreboard, with its total. A run of the modules
    `modules` replaces the rows of those modules and keeps the others."""
    boards = {}
    if os.path.exists(SCOREBOARD_FILE):
        with open(SCOREBOARD_FILE, encoding="utf-8") as f:
            boards = parse_scoreboard(f.read())
    rows = {module: row for module, row in table.items() if module != corpus.TOTAL}
    if modules:
        kept = {module: row for module, row in boards.get(suite, {}).items()
                if module != corpus.TOTAL and not in_modules(module, modules)}
        rows = {**kept, **rows}
    rows[corpus.TOTAL] = total_of(rows.values())
    boards[suite] = rows
    with open(SCOREBOARD_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write(format_scoreboard(boards))


ERRATA = DATA.errata


# The pages left out until the engine has what they need, by name, with the need and the reason. The file uses the
# errata's form, with whole files only.
DEFERRED = DATA.deferred
# The pages left out because they test what the engine has decided not to build, by name, with what is cut and the
# reason. The file uses the errata's form, with whole files only.
CUT = DATA.cut
# The pages that we choose not to run, by name, with the reason. The file uses the errata's form, with whole files only.
SKIP = DATA.skip


# The extensions of a page, the extensions of the scripts that WPT makes a window page for, and the directories that
# hold no tests (WPT's manifest rules).
PAGES = (".html", ".htm", ".xht", ".xhtml", ".svg")
SCRIPTS = (".any.js", ".window.js")
NOT_TESTS = {"support", "resources", "tools", "reference"}

# The extensions of the pages that the shell captures, and the viewport that WPT renders a reftest in, which is the one
# that the shell captures.
CAPTURED = (".html", ".htm", ".xht", ".xhtml", ".svg")
PRINTED = (".html", ".htm", ".xht", ".xhtml")
REFTEST_SIZE = "800x600"

# The page box that WPT prints a print reftest's documents on before their @page rules apply, in CSS pixels at 96 per
# inch: 5 by 3 inches, with margins of half an inch (web-platform-tests docs, "Print Reftests").
PRINT_PAGE_SIZE = "480x288"
PRINT_PAGE_MARGIN = "48"

REFTEST_LINK = re.compile(r"<(?:\w+:)?link\b[^>]*>", re.I | re.S)
LINK_ATTRIBUTE = re.compile(r"""\b(rel|href)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'=<>`]+))""", re.I)

# <meta name=fuzzy content="[url:]maxDifference=a-b;totalPixels=c-d">, which may omit the names
# (web-platform-tests docs, "Fuzzy Matching").
FUZZY = re.compile(r"""<meta\b[^>]*\bname\s*=\s*["']?fuzzy["']?[^>]*\bcontent\s*=\s*["']([^"']*)""",
                   re.I)

TESTHARNESS = "testharness"
REFTEST = "reftest"
CRASHTEST = "crashtest"


# Matches a variant that a testharness test runs as (WPT's manifest): a page's <meta name=variant> or a script's
# `// META: variant=` comment, each a query or a fragment added to its URL.
META_TAG = re.compile(r"<meta\b[^>]*>", re.I)
META_ATTRIBUTE = re.compile(r"""\b(name|content)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]*))""", re.I)
SCRIPT_VARIANT = re.compile(r"^//\s*META:\s*variant=(.*)$", re.M)


def variants(path):
    """Returns the variants of the testharness test at `path`, in the order that it names them, or the test once as
    itself if it names none."""
    if path.endswith(SCRIPTS):
        with open(path, encoding="utf-8") as f:
            found = [v.strip() for v in SCRIPT_VARIANT.findall(f.read())]
    else:
        found = []
        for tag in META_TAG.findall(page_text(path)):
            attributes = {m.group(1).lower(): m.group(2) or m.group(3) or m.group(4) or ""
                          for m in META_ATTRIBUTE.finditer(tag)}
            if attributes.get("name", "").lower() == "variant":
                found.append(attributes.get("content", ""))
    return found or [""]


def page_of(name):
    """Returns the file that a test's name refers to, without the variant that it runs as."""
    for mark in "?#":
        name = name.split(mark, 1)[0]
    return name




def page_text(path):
    """Returns the page's text, decoded by its byte order mark, or as Latin-1 without one. Every marker that discovery
    looks for is ASCII."""
    with open(path, "rb") as f:
        data = f.read()
    for bom, encoding in ((b"\xef\xbb\xbf", "utf-8"), (b"\xff\xfe", "utf-16-le"),
                          (b"\xfe\xff", "utf-16-be")):
        if data.startswith(bom):
            return data[len(bom):].decode(encoding, errors="replace")
    return data.decode("latin-1")


# The CSS flags that make a test one that a person runs (WPT's manifest, content_is_css_manual).
CSS_MANUAL_FLAGS = {"animated", "font", "history", "interact", "paged", "speech", "userstyle"}

# Matches a <meta name=flags> element, whose content lists a CSS test's flags.
FLAGS_META = re.compile(r"<meta\b[^>]*\bname\s*=\s*[\"']?flags\b[^>]*>", re.I)
CONTENT_ATTRIBUTE = re.compile(r"\bcontent\s*=\s*(?:\"([^\"]*)\"|'([^']*)'|([^\s>]+))", re.I)


def css_flags(text):
    """Returns the flags that a CSS test's <meta name=flags> elements list."""
    flags = set()
    for tag in FLAGS_META.findall(text):
        match = CONTENT_ATTRIBUTE.search(tag)
        if match:
            flags.update(next(g for g in match.groups() if g is not None).split())
    return flags


def type_flag(path):
    """Returns the type that the name of the file at `path` gives it, the word after its last hyphen and before any
    dot, or None, as WPT's manifest reads it (tools/manifest/sourcefile.py)."""
    stem = os.path.splitext(os.path.basename(path))[0]
    return stem.rsplit("-", 1)[1].split(".")[0] if "-" in stem else None


def is_print(path):
    """Returns true if the page at `path` is a print reftest's by its name, as WPT's manifest finds them
    (name_is_print_reftest): its type is print, or a directory of its path in the checkout is named print."""
    directory = os.path.dirname(os.path.relpath(path, WPT))
    return type_flag(path) == "print" or "print" in directory.split(os.sep)


def is_manual(path):
    """Returns true if the test at `path` is one that a person runs, which wptrunner leaves out, as WPT's manifest finds
    them: a name whose type is manual or visual, or a CSS test whose flags call for a person. A print reftest is never
    one by its flags, since the manifest finds it first."""
    if type_flag(path) in ("manual", "visual"):
        return True
    if path.endswith(SCRIPTS) or (is_print(path) and references(path)):
        return False
    return bool(css_flags(page_text(path)) & CSS_MANUAL_FLAGS)


def kind_of(path, relative):
    """Returns the kind of test that the file at `path` is, or None if it is not a test."""
    if path.endswith(SCRIPTS):
        return TESTHARNESS
    stem = os.path.splitext(os.path.basename(path))[0]
    if stem.endswith(("-ref", "-notref")) or stem.startswith(("ref-", "notref-")):
        return None
    if "crashtests" in relative.split("/")[:-1] or stem.endswith("-crash"):
        return CRASHTEST
    text = page_text(path)
    if "/resources/testharness.js" in text:
        return TESTHARNESS
    # An SVG reftest names its references with a link in HTML's namespace, under a prefix such as html:link.
    if re.search(r"""<(?:\w+:)?link[^>]*\brel\s*=\s*["']?(?:match|mismatch)\b""", text, re.I):
        return REFTEST
    return None


def references(path):
    """Returns the reftest's references, as (rel, path): each link whose rel is match or mismatch."""
    found = []
    for tag in REFTEST_LINK.findall(page_text(path)):
        attributes = {m.group(1).lower(): next(g for g in m.groups()[1:] if g is not None)
                      for m in LINK_ATTRIBUTE.finditer(tag)}
        rel = attributes.get("rel", "").lower()
        if rel in ("match", "mismatch") and attributes.get("href"):
            href = attributes["href"].split("?")[0].split("#")[0]
            base = WPT if href.startswith("/") else os.path.dirname(path)
            found.append((rel, os.path.normpath(os.path.join(base, href.lstrip("/")))))
    return found


def fuzzy(path, reference):
    """Returns the ranges of the largest channel difference and of the differing pixel count that the test allows
    against `reference`, as wptrunner finds them: from the test's override in FUZZY_OVERRIDES if it has one, as a
    browser's metadata overrides a test, and otherwise from the test's own fuzzy metas, as WPT's manifest reads them.
    In each, a value keyed by a reference's URL, relative to the test, applies to that reference, and an unkeyed value
    to any other. Each range is (0, 0) if neither gives one."""
    name = os.path.relpath(path, WPT).replace(os.sep, "/")
    sources = [FUZZY_OVERRIDES.get(name, []), FUZZY.findall(page_text(path))]
    for contents in sources:
        keyed = fuzzy_ranges(path, contents)
        for key in (reference, None):
            if key in keyed:
                return keyed[key]
    return ((0, 0), (0, 0))


def fuzzy_ranges(path, contents):
    """Returns the ranges that fuzzy metas' `contents` give the test at `path`, by the reference that each keys, or by
    None for an unkeyed one."""
    keyed = {}
    for content in contents:
        parts = content.rsplit(":", 1)
        key = None
        if len(parts) == 2:
            href = parts[0].strip().split("?")[0].split("#")[0]
            base = WPT if href.startswith("/") else os.path.dirname(path)
            key = os.path.normpath(os.path.join(base, href.lstrip("/")))
        keyed[key] = corpus.fuzzy_content_ranges(parts[-1])
    return keyed


def drawable(path, printed=False):
    """Returns true if a reftest's document can be rendered: a page that the shell captures, and for a print reftest,
    `printed`, a page that the shell prints."""
    return path.endswith(PRINTED if printed else CAPTURED)


def renderable(path):
    """Returns true if the test and each of its references can be rendered."""
    refs = references(path)
    printed = is_print(path)
    return (bool(refs) and drawable(path, printed)
            and all(os.path.exists(r) and drawable(r, printed) for _, r in refs))


def page_ranges(path):
    """Returns the pages of each document that the print reftest at `path` compares, by the document's path, as the
    test's reftest-pages metas give them, in the form that pr_shell --pages takes. A meta keyed by a reference's URL,
    relative to the test, selects that reference's pages, and an unkeyed one selects the test's own. A document that
    no meta names compares every page. WPT's manifest reads only the test's metas, so a reference's own do not apply.
    The shell rejects ranges that do not parse, which fails the test, as the manifest rejects them."""
    ranges = {}
    for tag in META_TAG.findall(page_text(path)):
        attributes = {m.group(1).lower(): m.group(2) or m.group(3) or m.group(4) or ""
                      for m in META_ATTRIBUTE.finditer(tag)}
        if attributes.get("name", "").lower() != "reftest-pages":
            continue
        parts = attributes.get("content", "").rsplit(":", 1)
        document = path
        if len(parts) == 2:
            href = parts[0].strip().split("?")[0].split("#")[0]
            base = WPT if href.startswith("/") else os.path.dirname(path)
            document = os.path.normpath(os.path.join(base, href.lstrip("/")))
        ranges[document] = "".join(parts[-1].split())
    return ranges


# A line in which core reports where a harness failed, as an assertion or a fatal error does (pr/assert.h): the
# source's path and line, then the message. A backtrace follows it.
FAILURE_SITE = re.compile(r"^\S+:\d+: ")


def failure_of(stderr, returncode, last=True):
    """Returns what a harness that failed printed about its failure: the line where core reports where it failed, or
    else its last line of output, or its first if `last` is false, or its exit status if it printed nothing."""
    diagnostics = stderr.strip().splitlines()
    for line in diagnostics:
        if FAILURE_SITE.match(line):
            return line
    if diagnostics:
        return diagnostics[-1] if last else diagnostics[0]
    return "process exited %d" % returncode


def capture(shell, path, png, timing):
    """Captures the page at `path` in the shell, as a PNG at `png`. Returns the failure, or the empty string, and adds
    to `timing` what says that timing decided the capture (corpus.timing_of)."""
    try:
        process = SHELLS.run(shell, ["--screenshot", png, "--html", url_of(path)], TIMEOUT)
    except subprocess.TimeoutExpired:
        timing.append(BACKSTOPPED)
        return "TIMEOUT"
    note_verify(process.stderr)
    timing += corpus.timing_of(process.stderr)
    if process.returncode == TIMED_OUT:
        return "TIMEOUT"
    if process.returncode != 0 or not os.path.exists(png):
        return "ERROR: " + failure_of(process.stderr, process.returncode)
    return ""


def comparisons(path):
    """Returns the pairs of documents, (left, right), that wptrunner compares for the reftest at `path`: the test with
    each of its references, then, if it has mismatch references, each match reference with the first of them. The
    mismatch references form a chain after each match reference (wptrunner's ReftestTest.from_manifest)."""
    refs = references(path)
    pairs = [(path, ref) for _, ref in refs]
    mismatches = [ref for rel, ref in refs if rel == "mismatch"]
    if mismatches:
        pairs += [(ref, mismatches[0]) for rel, ref in refs if rel == "match"]
    return pairs


def compare_reftest(shell, path):
    """Returns the answer for each of the reftest's comparisons, in the form that the reftest harness gives, or for a
    print reftest in the form that pr_shell --print gives. A shell that serves renders and compares the documents in one
    run, as it always does for a print reftest. Otherwise the shell captures each document as a PNG, and the reftest
    harness compares them."""
    pairs = comparisons(path)
    if is_print(path):
        return compare_pages(shell, pairs, page_ranges(path))
    if SHELLS.serves(shell):
        return compare_pages(shell, pairs)
    return compare_captures(shell, pairs)


def compare_pages(shell, pairs, printed=None):
    """Returns compare_reftest's answers for the comparisons `pairs`, each of two pages. If `printed` is given, the
    pages are printed, and it holds the page ranges of the documents that it names (page_ranges)."""
    args = []
    if printed is not None:
        args += ["--page-size", PRINT_PAGE_SIZE, "--page-margin", PRINT_PAGE_MARGIN]

    def pages(document):
        return ["--pages", printed[document]] if printed and document in printed else []

    for left in dict.fromkeys(left for left, _ in pairs):
        args += [arg for l, right in pairs if l == left for arg in pages(right) + ["--reference", url_of(right)]]
        if WRITE:
            stem = reftest.write_stem(WRITE, os.path.relpath(left, WPT))
            os.makedirs(os.path.dirname(stem), exist_ok=True)
            args += ["--write", stem]
        args += (["--print"] if printed is not None else []) + pages(left) + ["--html", url_of(left)]
    try:
        process = SHELLS.run(shell, args, TIMEOUT)
    except subprocess.TimeoutExpired:
        return [{"status": "TIMEOUT", "timing": [BACKSTOPPED]} for _ in pairs]
    note_verify(process.stderr)
    timing = corpus.timing_of(process.stderr)
    if process.returncode == TIMED_OUT:
        return [{"status": "TIMEOUT", "timing": timing} for _ in pairs]
    found = [json.loads(line[len("REFTEST "):]) for line in process.stdout.splitlines()
             if line.startswith("REFTEST ")]
    if process.returncode != 0 or len(found) != len(pairs):
        failure = "ERROR: " + failure_of(process.stderr, process.returncode)
        return [{"status": failure, "timing": timing} for _ in pairs]
    # The shell answers each page's references in turn, in the order of the pages.
    order = [pair for left in dict.fromkeys(left for left, _ in pairs) for pair in pairs if pair[0] == left]
    answers = dict(zip(order, found))
    return [dict(answers[pair], status="", timing=timing) for pair in pairs]


def compare_captures(shell, pairs):
    """Returns compare_reftest's answers for the comparisons `pairs`, from the shell's capture of each document."""
    with tempfile.TemporaryDirectory(prefix="wpt-reftest-") as directory:
        captured = {}
        timing = []
        for index, document in enumerate(dict.fromkeys(d for pair in pairs for d in pair)):
            png = os.path.join(directory, "%d.png" % index)
            captured[document] = (png, capture(shell, document, png, timing))
        slots = []
        cases = []
        for left, right in pairs:
            failure = next((captured[d][1] for d in (left, right) if captured[d][1]), "")
            slots.append({"status": failure} if failure else None)
            if not failure:
                source, reference = (captured[d][0] for d in (left, right))
                cases.append(reftest.Case(os.path.relpath(left, WPT), source, reference, REFTEST_SIZE))
        given = iter(reftest.run(reftest.HARNESS, cases, WRITE, 1, text=TEXT))
        return [dict(slot if slot is not None else next(given), timing=timing) for slot in slots]


def same(allowed, answer):
    """Returns true if a comparison's answer says that its two documents render the same within the fuzzy ranges
    `allowed`. A printed document renders the same as another if they have as many pages and each page is within the
    ranges of the page in the same place (wptrunner's check_pass)."""
    if "pageCounts" not in answer:
        return corpus.within_fuzzy(allowed, answer["maxDifference"], answer["differingPixels"])
    count, other = answer["pageCounts"]
    return count == other and all(corpus.within_fuzzy(allowed, page["maxDifference"], page["differingPixels"])
                                  for page in answer["pages"])


def reftest_outcome(path, answers):
    """Returns whether the reftest passes or fails, from the harness's answer for each of its comparisons, as wptrunner
    walks them. The test passes if it matches one of its match references, within its fuzzy ranges, and that reference
    then differs from the first mismatch reference, which differs from the test, as each later mismatch reference does.
    Of these, only a comparison of the test with its own reference uses the test's fuzzy ranges. With no match
    reference, the test must differ from every mismatch reference."""
    if any(a["status"] != "" or not ("maxDifference" in a or "pageCounts" in a) for a in answers):
        return "fail"
    refs = references(path)
    own = answers[:len(refs)]
    chained = iter(answers[len(refs):])

    def differs(answer):
        return not same(((0, 0), (0, 0)), answer)

    mismatches = [(ref, a) for (rel, ref), a in zip(refs, own) if rel == "mismatch"]
    later = all(differs(a) for _, a in mismatches[1:])
    matches = [(ref, a) for (rel, ref), a in zip(refs, own) if rel == "match"]
    if not matches:
        first, answer = mismatches[0]
        apart = not same(fuzzy(path, first), answer)
        return "pass" if apart and later else "fail"
    passing = []
    for ref, answer in matches:
        tail = not mismatches or (differs(next(chained)) and differs(mismatches[0][1]) and later)
        if same(fuzzy(path, ref), answer) and tail:
            passing.append(answer)
    return "pass" if passing else "fail"


def discover(root, directories):
    """Returns (path, name, kind) for each test in `directories` of the checkout at `root`, in path order. The name is
    the path in the checkout."""
    tests = []
    for top in directories:
        tests += walk(root, os.path.join(root, top))
    return tests


def walk(root, top):
    tests = []
    for directory, subdirectories, files in os.walk(top):
        parent = os.path.relpath(directory, root).replace(os.sep, "/")
        subdirectories[:] = sorted(d for d in subdirectories if d not in NOT_TESTS and not d.startswith(".")
                                   and posixpath.join(parent, d) not in NOT_RUN)
        for file in sorted(files):
            if not file.endswith(PAGES + SCRIPTS):
                continue
            path = os.path.join(directory, file)
            relative = os.path.relpath(path, root).replace(os.sep, "/")
            kind = kind_of(path, relative)
            if kind == TESTHARNESS:
                tests += [(path, relative + variant, kind) for variant in variants(path)]
            elif kind is not None:
                tests.append((path, relative, kind))
    return tests


def module_of(name):
    """Returns a directory's first two levels, or CSS2's first three, because CSS2 is a collection of modules."""
    parts = page_of(name).split("/")[:-1]
    depth = 3 if parts[:2] == ["css", "CSS2"] else 2
    return "/".join(parts[:depth]) or name


def tested_features(root, features):
    """Returns the features that the suites test, by the manifests of the checkout at `root`."""
    directories = {directory for suite in SUITES.values() for directory in suite}
    return web_features.Pages(root, features).named(directories, NOT_RUN)


def check_rulings(root):
    """Exits unless tools/suite/wpt/web_features.txt rules on exactly the features that the suites test, by the
    manifests of the checkout at `root`, since a page's ruling decides whether a run selects it."""
    features = web_features.data()
    if features is None:
        sys.exit("no web-features data; run ./dev.sh fetch webfeatures")
    problems = web_features.check(features, tested_features(root, features))
    if problems:
        sys.exit("%s does not match the suites:\n%s" % (os.path.relpath(web_features.RULINGS_FILE, corpus.ROOT),
                                                        "\n".join("  " + problem for _, problem in problems)))


def unbuilt(pages, ruled, name):
    """Returns the first feature that the page `name` tests and the engine does not build, as its ruling and name, or
    None if the engine builds each feature."""
    for feature in pages.of(page_of(name)):
        ruling = ruled.get(feature, "open")
        if ruling not in web_features.BUILT:
            return "%s: %s" % (ruling, feature)
    return None


def select(root, directories, modules=(), all_tests=False, match=None, needs=None):
    """Returns the selected (path, name, kind) entries and the counts of pages excluded by dependency. A run of the
    default selection or of modules leaves out a page that tests a feature that tools/suite/wpt/web_features.txt does
    not rule as built by the engine, a page that deferred.txt defers, a page that cut.txt cuts, and a page that skip.txt
    skips. If `needs` is given, each page left out is also written to it, by name, with what it needs."""
    default = not (modules or all_tests or match)
    pruned = default or (modules and not match)
    features = web_features.data() if pruned else None
    if pruned and features is None:
        sys.exit("no web-features data; run ./dev.sh fetch webfeatures")
    pages = web_features.Pages(root, features) if pruned else None
    ruled = web_features.rulings()[0] if pruned else {}
    tests = []
    left = collections.Counter()
    for path, name, kind in discover(root, directories):
        if modules and not any(fnmatch.fnmatchcase(module_of(name), pattern)
                               or fnmatch.fnmatchcase(name.split("/")[0], pattern)
                               for pattern in modules):
            continue
        if not corpus.holds(name, match) and not corpus.holds(page_of(name), match):
            continue
        need = None
        if pruned and is_manual(path):
            need = "manual"
        if need is None and pruned:
            need = unbuilt(pages, ruled, name)
        deferred = DEFERRED.get(name, DEFERRED.get(page_of(name)))
        if need is None and pruned and deferred is not None:
            need = "deferred: " + deferred.split(":", 1)[0]
        cut = CUT.get(name, CUT.get(page_of(name)))
        if need is None and pruned and cut is not None:
            need = "cut: " + cut.split(":", 1)[0]
        skip = SKIP.get(name, SKIP.get(page_of(name)))
        if need is None and pruned and skip is not None:
            need = "skip: " + skip.split(":", 1)[0]
        if need is None:
            tests.append((path, name, kind))
        else:
            left[need] += 1
            if needs is not None:
                needs[name] = need
    return tests, left


def timeout_of(path):
    """Returns the deadline of the test at `path`, in seconds: LONG_TIMEOUT if it marks itself long, and TIMEOUT
    otherwise."""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return LONG_TIMEOUT if LONG_MARK.search(f.read()) else TIMEOUT
    except OSError:
        return TIMEOUT


class Test:
    def __init__(self, path, variant=""):
        self.path = path
        self.variant = variant    # the query or fragment that it runs as, or empty for none
        self.timeout = timeout_of(path)  # seconds
        self.results = []      # (status, name, message)
        self.result_indices = []  # registration order, independent of completion order
        self.harness = None    # harness or host failure, separate from subtest failures
        self.complete = False
        self.timing = []       # what says that the system's time decided the run (corpus.timing_of)


def run(shell, test):
    return run_page(shell, test, url_of(test.path, test.variant))


def deadline_options(test):
    """Returns the shell's options for the test's own deadline, which a long test needs in place of the shell's."""
    return ["--timeout", "%d" % (test.timeout * 1000)] if test.timeout != TIMEOUT else []


def text_of(captured):
    """Returns the text of what a process that timed out wrote, which subprocess leaves as bytes on POSIX and as text
    on Windows."""
    if isinstance(captured, bytes):
        return captured.decode("utf-8", errors="replace")
    return captured or ""


def run_page(shell, test, url):
    returncode = None
    reported = False
    try:
        process = SHELLS.run(shell, deadline_options(test) + ["--html", url], test.timeout)
        output, error = process.stdout, process.stderr
        returncode = process.returncode
        note_verify(error)
        test.timing = corpus.timing_of(error)
        if returncode == TIMED_OUT:
            test.harness = "TIMEOUT"
            returncode = None
    except subprocess.TimeoutExpired as expired:
        test.harness = "TIMEOUT"
        output = text_of(expired.stdout)
        error = text_of(expired.stderr)
        test.timing = [BACKSTOPPED]

    results = {}
    # A report is one line. JSON leaves U+2028 and the other separators that splitlines() breaks at unescaped in its
    # strings, so lines end only at line feeds.
    for line in output.split("\n"):
        kind, _, payload = line.partition(" ")
        if kind not in ("RESULT", "HARNESS"):
            continue
        try:
            record = json.loads(payload)
        except json.JSONDecodeError:
            # A killed process may leave its last report incomplete.
            test.harness = test.harness or "ERROR: invalid reporter output"
            continue
        if kind == "RESULT":
            results[record["index"]] = (record["status"], record["name"], record["message"])
        else:
            reported = True
            if test.harness is None and record["status"] != "OK":
                test.harness = record["status"] + (": " + record["message"] if record["message"]
                                                   else "")

    test.result_indices = sorted(results)
    test.results = [results[index] for index in test.result_indices]
    if test.harness is None:
        diagnostics = error.strip().splitlines()
        if returncode:
            test.harness = "ERROR: " + failure_of(error, returncode, last=False)
        elif not reported:
            test.harness = "INCOMPLETE: " + (diagnostics[0] if diagnostics
                                             else "harness did not complete")
    test.complete = reported and returncode == 0 and test.harness is None
    return test


# What the shell says of a document that is not well-formed (htmlShowXmlError).
XML_ERROR = ": XML Parsing Error: "


def crash(shell, test):
    """Runs a crashtest, which passes if the page loads and the shell exits without failing. The page is done once it
    has loaded and rendered and its root no longer has the class test-wait, as wptrunner waits for a crashtest, whatever
    timers or animations it leaves running. A document that is not well-formed fails a run of the shell, but it loads as
    its parse error, which is all that a crashtest asks of it."""
    try:
        # A crashtest may load testharness.js, which reports through the vendor reporter as a testharness page does,
        # instead of waiting out its own timeout.
        process = SHELLS.run(shell, deadline_options(test) + ["--capture-wait", capture_wait("test-wait"), "--wait",
                                                              "--html", url_of(test.path)], test.timeout)
        note_verify(process.stderr)
        test.timing = corpus.timing_of(process.stderr)
        diagnostics = process.stderr.strip().splitlines()
        if process.returncode == TIMED_OUT:
            test.harness = "TIMEOUT"
        elif process.returncode != 0 and not any(XML_ERROR in line for line in diagnostics):
            test.harness = "CRASH: " + failure_of(process.stderr, process.returncode, last=False)
    except subprocess.TimeoutExpired:
        test.harness = "TIMEOUT"
        test.timing = [BACKSTOPPED]
    test.complete = test.harness is None
    test.result_indices = [0]
    test.results = [("PASS" if test.complete else "FAIL", "loads without crashing", test.harness)]
    return test


def summary(test, name, raw=False):
    """Returns the harness failure or the first failing subtest, honoring the errata unless `raw` is set."""
    if test.harness:
        return " ".join(test.harness.splitlines())
    for index, (status, subtest, message) in zip(test.result_indices, test.results):
        if status != "PASS" and (raw or "%s:%d" % (name, index) not in ERRATA):
            return status + ": " + " ".join((message or subtest).splitlines())
    return ""


class Progress:
    """Shows a run's progress on stderr. In a terminal, one line is redrawn as each page finishes. Otherwise a line is
    printed at each tenth of the way, so that a log shows the run's progress. The run's own output goes through write(),
    which clears the progress line first."""

    def __init__(self, total):
        self.total = total
        self.done = 0
        self.terminal = sys.stderr.isatty()
        self.shown = False
        self.lock = threading.Lock()

    def finished(self, name):
        with self.lock:
            self.done += 1
            if self.terminal:
                line = "[%d/%d] %s" % (self.done, self.total, name)
                width = shutil.get_terminal_size().columns - 1
                sys.stderr.write("\r\033[K" + line[:width])
                self.shown = True
            elif self.total >= 10 and self.done % (self.total // 10) == 0:
                sys.stderr.write("%d/%d pages run\n" % (self.done, self.total))
            sys.stderr.flush()

    def clear(self):
        if self.shown:
            sys.stderr.write("\r\033[K")
            sys.stderr.flush()
            self.shown = False

    def write(self, text):
        with self.lock:
            self.clear()
            print(text)
            sys.stdout.flush()


def passed_tests(record):
    """Returns the number of tests that a file's record passed."""
    if record.get("reftest") is not None:
        return int(record["reftest"] == "pass")
    return sum(1 for status, _, _ in record.get("tests") or [] if status == "PASS")


def message_of(failure):
    """Returns what a failure says without the status that leads it, as "TIMEOUT" or "FAIL: message" has, since a
    case's line gives the status (corpus.case_line)."""
    word, colon, message = failure.partition(": ")
    if word.isupper() and (colon or word == failure):
        return message
    return failure


def status_of(record):
    """Returns how a file ended: OK if it completed, and otherwise the status that its harness or its render gave, as
    TIMEOUT or ERROR, without the message."""
    if record["complete"]:
        return "OK"
    return (record["harness"] or "INCOMPLETE").split(":")[0]


# How many times a run that says timing decided it runs again (corpus.timing_of).
TIMING_TRIES = 2


def listed_timing(name):
    """Returns true if timing.txt lists the page, or its file: a page that we expect to pass but whose result depends on
    timing."""
    return DATA.timing.get(name, DATA.timing.get(page_of(name))) is not None


def timing_of_result(result):
    """Returns what a test's result says of timing deciding it: a Test's, or a reftest's answers'."""
    if isinstance(result, Test):
        return result.timing
    return [line for answer in result for line in answer.get("timing", ())]


def timed_detail(detail, result):
    """Returns a case's detail, with what says that timing decided its first run (corpus.timing_of)."""
    timing = list(dict.fromkeys(timing_of_result(result)))
    if not timing:
        return detail
    return "%s%stiming: %s" % (detail, "; " if detail else "", "; ".join(timing))


def narrow(test, name, match):
    """Returns `test` with only the subtests that `match` selects (corpus.selected)."""
    chosen = [at for at, index in enumerate(test.result_indices)
              if corpus.selected("%s:%d" % (name, index), match)]
    test.result_indices = [test.result_indices[at] for at in chosen]
    test.results = [test.results[at] for at in chosen]
    return test


def test_try(test, recorded):
    """Returns a testharness or crash test's run as an outcome that Results compare: (passed, count, status), as the
    run's line counts them. A file that registers nothing counts as one test, unless the errata excuse it (`recorded`)
    and it did not finish."""
    finished = test.complete and test.harness is None
    count = len(test.results) or int(finished or not recorded)
    passed = sum(status == "PASS" for status, _, _ in test.results)
    return passed, count, "OK" if finished else (test.harness or "INCOMPLETE").split(":")[0]


def reftest_try(path, answers):
    """Returns a reftest's run as an outcome that Results compare: (passed, count, status)."""
    failure = next((a["status"] for a in answers if a["status"]), "")
    return int(reftest_outcome(path, answers) == "pass"), 1, failure.split(":")[0] if failure else "OK"


def rate(counts):
    """Returns the percentage of the tests in `counts` that passed."""
    return 100.0 * counts["passed"] / counts["tests"] if counts["tests"] else 100.0


def main():
    global TEXT, WRITE, FUZZY_OVERRIDES
    ap = corpus.arguments(SHELL, WPT)
    ap.add_argument("suite", nargs="?", default="css", choices=sorted(SUITES),
                    help="the suite to run (default: css)")
    ap.add_argument("--first-errors", action="store_true",
                    help="print a separate table with the first file or test failure per module")
    ap.add_argument("--write", metavar="DIR",
                    help="write each reftest's render, its reference and their difference here, as reftest.py does")
    ap.add_argument("--raw", action="store_true",
                    help="count a test when it passes and nothing else, as wpt.fyi does")
    ap.add_argument("--jobs", type=int, default=os.cpu_count(), help="how many pages run at once")
    ap.add_argument("--verify", action="store_true",
                    help="check each page's incremental rendering against a full recomputation")
    ap.add_argument("--pixel-ratio", type=float, metavar="PIXELS",
                    help="target pixels per CSS px that pages render and compare at, 1 unless given; a run at "
                         "another ratio leaves the scoreboard alone")
    ap.add_argument("--renderer", default="cpu", metavar="NAME",
                    help="the renderer of lib/draw that renders pages, cpu unless given; a run on another renderer "
                         "leaves the scoreboard alone")
    selection = ap.add_mutually_exclusive_group()
    selection.add_argument("--module", action="append", default=[], metavar="GLOB",
                           help="run a whole module, as css/css-syntax, repeatable; css/CSS2/* "
                                "selects its subdivisions")
    selection.add_argument("--all", action="store_true", help="run every discovered test")
    selection.add_argument("--left-out", action="store_true",
                           help="print each page that a plain run of any suite leaves out, as its "
                                "name and what it needs, a tab apart")
    args = ap.parse_args()
    if args.verify:
        SETUP.append("--verify")
    TEXT = corpus.text_backends(args)
    WRITE = args.write
    SETUP.extend(TEXT)
    if args.pixel_ratio is not None:
        if not args.pixel_ratio > 0:
            sys.exit("--pixel-ratio takes target pixels per CSS px above 0")
        SETUP.extend(["--pixel-ratio", "%g" % args.pixel_ratio])
    if args.renderer != "cpu":
        SETUP.extend(["--renderer", args.renderer])
    FUZZY_OVERRIDES = corpus.read_fuzzy_overrides(FUZZY_FILE)

    for path, entries in ((DEFERRED_FILE, DEFERRED), (CUT_FILE, CUT), (SKIP_FILE, SKIP)):
        missing = [name for name in sorted(entries) if not os.path.exists(os.path.join(args.tests, name))]
        if missing:
            sys.exit("%s: %s no longer exists" % (os.path.relpath(path, corpus.ROOT), missing[0]))
    check_rulings(args.tests)
    if args.left_out:
        needs = {}
        for directories in SUITES.values():
            select(args.tests, directories, needs=needs)
        for name in sorted(needs):
            print("%s\t%s" % (name, needs[name]))
        return 0
    directories = SUITES[args.suite]
    found, left = select(args.tests, directories, args.module, args.all, args.match)
    if not found:
        sys.exit("no tests selected under %s; check the selection or run ./dev.sh fetch wpt"
                 % args.tests)
    width = max(44, max(len(name) for _, name, _ in found))
    if args.list:
        for _, name, kind in found:
            print("%s  %s" % (name.ljust(width), kind))
        print("\n%d files" % len(found))
        if left:
            print("\n%d pages left out, by what they need:" % sum(left.values()))
            for need, count in left.most_common():
                print("  %-44s %5d" % (need, count))
        return 0

    corpus.require(args.binary, "%s-wpt" % args.suite)
    MEMORY.start()
    records = {}
    modules = collections.defaultdict(collections.Counter)
    first_failure = {}
    reftests = collections.Counter()
    seen = set()
    # The reftests that are rendered. They run in the same pool as every other test.
    rendered = {name for path, name, kind in found if kind == REFTEST and renderable(path)}
    if rendered:
        corpus.require(reftest.HARNESS, "%s-wpt" % args.suite)
    progress = Progress(sum(kind != REFTEST or name in rendered for _, name, kind in found))
    out = progress.write
    # A file is a case, whose tests are counted, and which ended as its harness says. The errata record where WPT is
    # wrong, and the failures that they do not record are counted in the scoreboard.
    results = corpus.Results(args, DATA, requires_errata=False, width=width, out=out)
    durations = read_durations()
    took = {}
    # The slowest tests run first, by their last run. A test that has not run is assumed to take the typical time.
    typical = sorted(durations.values())[len(durations) // 2] if durations else 0
    order = sorted(found, key=lambda entry: -durations.get(entry[1], typical))
    def attempt(path, name, kind):
        if kind == REFTEST:
            return compare_reftest(args.binary, path)
        return (run if kind == TESTHARNESS else crash)(args.binary, Test(path, name[len(page_of(name)):]))

    run_order = [(path, name, kind) for path, name, kind in order if kind != REFTEST or name in rendered]
    first = {}
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        running = {}
        for path, name, kind in run_order:
            running[name] = pool.submit(timed, took, name, attempt, path, name, kind)
            running[name].add_done_callback(lambda _, name=name: progress.finished(name))
        first = {name: future.result() for name, future in running.items()}
    # A page that we expect to pass but whose result depends on timing (timing.txt), and whose run says that timing
    # decided it (corpus.timing_of), runs again, with fewer at once so that load decides less, until a run says nothing
    # of timing or the tries run out, and counts its best try (corpus.Results.case). Any other page's result is the
    # result of its one run: the clock that it runs on decides nothing by load, so a page that reaches the deadline on
    # the system's clock waits for what never comes, and another try would only take as long again.
    tries = collections.defaultdict(list)
    timed_out = [entry for entry in run_order
                 if listed_timing(entry[1]) and timing_of_result(first[entry[1]])]
    jobs = args.jobs
    for _ in range(TIMING_TRIES):
        if not timed_out:
            break
        jobs = max(1, jobs // 4)
        with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
            again = {entry[1]: pool.submit(attempt, *entry) for entry in timed_out}
            for name, future in again.items():
                tries[name].append(future.result())
        timed_out = [entry for entry in timed_out if timing_of_result(tries[entry[1]][-1])]
    # A case that we expect to pass but whose result depends on timing is recorded as its best try.
    kinds = {name: (path, kind) for path, name, kind in run_order}
    for name in tries:
        path, kind = kinds[name]

        def outcome(result):
            if kind == REFTEST:
                passed, count, status = reftest_try(path, result)
            else:
                passed, count, status = test_try(narrow(result, name, args.match), False)
            return status == "OK", passed / count if count else 1.0

        first[name] = max([first[name]] + tries[name], key=outcome)
    for path, name, kind in found:
        module = module_of(name)
        answers = first.get(name) if kind == REFTEST else None
        if answers is not None:
            seen.add(name)
            outcome = reftest_outcome(path, answers)
            passed = outcome == "pass"
            recorded = name in ERRATA
            if passed and recorded:
                results.look("%s agrees now; drop the entry" % name)
            ok = int(passed or (recorded and not args.raw))
            failure = next((a["status"] for a in answers if a["status"]), "")
            modules[module].update(files=1, completed=not failure, excused=0, tests=1, passed=ok)
            records[name] = {"complete": not failure, "reftest": outcome, "harness": failure or None}
            if not passed:
                first_failure.setdefault(module, name + ": " + (failure or "reftest does not match"))
            results.case(name, int(passed), status=status_of(records[name]), part=module, excused=ok - int(passed),
                         detail=timed_detail(message_of(failure) if failure else "" if passed else
                                             "reftest does not match", answers),
                         lines=["recorded: " + ERRATA[name]] if recorded and args.verbose else [],
                         tries=[reftest_try(path, tried) for tried in tries[name]])
            continue
        if kind == REFTEST:
            reftests[module] += 1
            seen.add(name)
            if args.verbose:
                out("%s   -/-   NOTRUN: reftest not rendered" % name.ljust(width))
            continue
        test = narrow(first[name], name, args.match)

        keys = ["%s:%d" % (name, i) for i in test.result_indices]
        seen.update([name] + keys)
        ok = 0
        for key, (status, _, _) in zip(keys, test.results):
            recorded = key in ERRATA
            if status == "PASS" and recorded:
                results.look("%s agrees now; drop the entry" % key)
            ok += status == "PASS" or (recorded and not args.raw)
        finished = test.complete and test.harness is None
        excused = not finished and name in ERRATA and not args.raw
        if finished and name in ERRATA:
            results.look("%s completes now; drop the entry" % name)
        # A file that registered nothing counts as one test, which passes if it finished.
        count = len(test.results)
        if count == 0 and not excused:
            count = 1
            ok = int(finished)
        modules[module].update(files=1, completed=finished, excused=excused, tests=count,
                               passed=ok)
        records[name] = {"complete": finished, "excused": excused, "harness": test.harness,
                         "tests": test.results}

        failure = summary(test, name, args.raw)
        if failure:
            first_failure.setdefault(module, name + ": " + failure)

        # Below the file's line, each subtest that did not pass, and with --verbose each entry that excuses one.
        lines = []
        for index, (status, subtest, message) in zip(test.result_indices, test.results):
            if status != "PASS":
                lines.append("%-7s %-5s %s" % (status, ":%d" % index, subtest))
                if message:
                    lines.append("              %s" % message)
        if args.verbose:
            lines += ["recorded: %s: %s" % (key, ERRATA[key]) for key in [name] + keys if key in ERRATA]
        raw = passed_tests(records[name])
        results.case(name, raw, count, status=status_of(records[name]), part=module, excused=ok - raw,
                     detail=timed_detail(message_of(failure), test), lines=lines,
                     tries=[test_try(narrow(tried, name, args.match), name in ERRATA and not args.raw)
                            for tried in tries[name]])

    SHELLS.close()
    if SHELLS.stuck:
        sys.exit("a shell did not exit; the run's results are not recorded")
    durations.update(took)
    write_durations(durations)
    progress.clear()
    rows = []
    total = sum(modules.values(), collections.Counter())
    for module in sorted(modules.keys() | reftests.keys()):
        notes = "%d reftests not rendered" % reftests[module] if reftests[module] else ""
        if modules[module]["excused"]:
            notes = ("%d file(s) excused by ERRATA" % modules[module]["excused"]
                     + ("; " + notes if notes else ""))
        rows.append((module, modules[module], notes))
    rows.append(("total", total, "%d file(s) excused by ERRATA" % total["excused"]
                 if total["excused"] else ""))
    column = max(len(name) for name, _, _ in rows)
    print("\n%s  %-11s  %s" % ("".ljust(column), "Files", "Tests"))
    for name, counts, note in rows:
        print(("%s  %-11s  %-13s %5.1f%%   %s"
               % (name.ljust(column),
                  "%d/%d" % (counts["completed"] + counts["excused"], counts["files"]),
                  "%d/%d" % (counts["passed"], counts["tests"]), rate(counts), note)).rstrip())
    # Why the files that did not complete stopped, by the status that their harness or reftest gave, with one file of
    # each. A run whose counts fall with no change to the engine shows here what its pages met.
    reasons = collections.Counter()
    examples = {}
    for name, record in sorted(records.items()):
        if record["complete"] or record.get("excused"):
            continue
        reason = (record["harness"] or "INCOMPLETE").split(":")[0]
        reasons[reason] += 1
        examples.setdefault(reason, name)
    if reasons:
        print("\nIncomplete files")
        for reason, count in reasons.most_common():
            print("%6d  %s, as %s" % (count, reason, examples[reason]))
    if args.first_errors and first_failure:
        print("\n%s  First error" % "Module".ljust(column))
        for module in sorted(first_failure):
            print("%s  %s" % (module.ljust(column), first_failure[module]))
    # A key that names no case cannot be told apart from a key whose case this run left out, so it is reported only by a
    # run that took every test of its directories.
    whole = args.all or not (args.match or args.module or left)
    if whole:
        for key in ERRATA:
            if key not in seen and any(corpus.names(key.split(":")[0], top) for top in directories):
                results.look("%s names no case" % key)
    status = results.finish(table=False)
    if args.verify:
        print("\n%d page(s) whose render failed a check" % len(verify_failures))
        for page in sorted(verify_failures):
            print("  %s\n      %s" % (page, verify_failures[page]))
    # Only a plain run of the default build, or of some of its modules, writes the scoreboard, because the scoreboard
    # measures that run's selection under that build's deadlines.
    plain = not (args.match or args.all or args.raw or TEXT or args.renderer != "cpu"
                 or (args.pixel_ratio is not None and args.pixel_ratio != 1))
    if plain and os.path.abspath(args.binary) == corpus.binary("pr_shell", corpus.DEFAULT):
        table = {module: score(counts) for module, counts in modules.items() if counts["files"]}
        record_scoreboard(args.suite, table, args.module)
        print("\nThe scoreboard is written to %s." % os.path.relpath(SCOREBOARD_FILE, corpus.ROOT))
    return status

if __name__ == "__main__":
    # A failure's message quotes what a page wrote, which the console's own encoding may not hold.
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    sys.exit(main())
