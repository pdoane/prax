"""`./dev.sh build`, `test`, `ci`, `example`, `app` and `clean`: the builds, the unit tests, the examples and the
apps."""

import os
import shutil
import sys
import time

from tools import dev
from tools.build import build as builds
from tools.build import emsdk
from tools.fetch import checkouts
from tools.lint import commands as lint

USAGE = {
    "build": """\
./dev.sh build [debug|release|asan|tsan|wasm|ios] [--lean] [--configure | --keep-going] [--target NAME]...
    configure and build. The default is optimized, with debug symbols and without link-time optimization. `debug`
    builds without the optimizer and with the assertions. `release` adds link-time optimization. `asan` is Debug with
    the address and undefined behavior sanitizers, and `tsan` is Debug with the thread sanitizer. `wasm` is the default
    compiled to WebAssembly with Emscripten, which it fetches into third_party/emsdk. `ios` is the default compiled
    for iOS devices with Xcode's iOS SDK, on macOS. --lean builds the lean form, for a machine that only tests: no
    tools, comparisons or examples, one font bundle, and ccache where it is installed (tools/build/build.py). --configure
    configures and builds nothing, which fetches everything that the build and its tests read. --keep-going builds
    past a failure and reports every error. --target builds only the targets named, and what they need""",
    "test": """\
./dev.sh test [debug|asan|tsan|wasm] [module] [args...]
    the unit tests and the tools' tests against that build's harnesses, with lint beside them, or one module's doctest
    binary with args, as in ./dev.sh test dom -ts="*keyword*" --success, from that build, which builds only that
    binary. `wasm` runs the unit tests under Node.js, then the page tests (pr_page_tests) in headless Chrome, and not
    the tools' tests, whose harnesses are the host's. A module's page tests are named <module>_page, as in
    ./dev.sh test wasm draw_page""",
    "ci": """\
./dev.sh ci [--configure | --key] [asan] [host|debug|wasm|ios]...
    whether the tree is in a good state on this machine: lint beside every target that the machine builds, each run
    past the failures of the others, and then a table of their outcomes. The targets are the host's build and its
    debug build, each with its unit tests and the tools' tests, and on macOS the WebAssembly build with its unit tests
    and page tests, and the iOS build, whose tests are built and not run. Each is the build's lean form, which fetches
    only what its tests read (./dev.sh build --lean). The targets named narrow the run to them. `asan` checks the
    host's sanitizer build in place of the targets. --configure configures each target's build and fetches what lint
    reads, and builds and runs nothing: everything that the run fetches, which CI caches before it runs
    (.github/workflows/ci.yml). It reports what third_party then holds, and in CI, where that is what the
    run fetched, fails past a budget (FETCH_BUDGET). --key prints a hash of what decides what the run fetches, which
    changes when that does, as CI's cache of third_party is keyed""",
    "example": """\
./dev.sh example [wasm] <name> [args...]
./dev.sh example wasm <name> --screenshot <png> [--seconds S] [args...]
    build an example under examples, and only what it needs, and run it with the arguments. `wasm` builds it for the
    web, serves the checkout, and opens its page in the system's browser, with the arguments in the page's query, until
    interrupted. --screenshot instead captures the page in headless Chrome, at 960x720, after it has run for S seconds
    (4 by default), and prints what the page logged""",
    "app": """\
./dev.sh app <name> [args...]
    build an app under apps, and only what it needs, and run it with the arguments""",
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
    lean = False
    configure_only = False
    arguments = iter(argv)
    for argument in arguments:
        if argument == "--keep-going":
            keep_going = True
        elif argument == "--lean":
            lean = True
        elif argument == "--configure":
            configure_only = True
        elif argument == "--target":
            targets.append(next(arguments, None) or dev.fail("--target names a target"))
        elif argument in builds.MODES:
            mode = argument
        else:
            dev.usage(argv, USAGE["build"], 1)
    if configure_only and (keep_going or targets):
        dev.usage(argv, USAGE["build"], 1)
    builds.build(mode, targets, keep_going, lean, configure_only)


def unit_tests(mode=None, lean=False):
    """Builds and runs every unit test of the build that `mode` names, or with `lean` its lean form, then the tools'
    tests against its harnesses."""
    chosen = builds.build(mode, lean=lean)
    dev.run(["ctest", "--test-dir", chosen.directory, "--output-on-failure", "--parallel", os.cpu_count() or 4,
             "--timeout", TEST_TIMEOUT])
    if chosen.wasm:
        for name in sorted(os.listdir(chosen.directory)):
            if name.endswith("_page_tests.html"):
                run_page_tests(os.path.join(chosen.directory, name), [])
    else:
        lint.tools_tests(chosen.environment())


def test(argv):
    dev.usage(argv, USAGE["test"])
    mode = argv.pop(0) if argv[:1] in (["debug"], ["asan"], ["tsan"], ["wasm"]) else None
    if not argv:
        # The whole run lints beside the build.
        lint.beside(lambda: unit_tests(mode))
        return
    # One module's tests build only their binary, <module>_tests.
    module, args = argv[0], argv[1:]
    chosen = builds.build(mode, [module + "_tests"])
    page = os.path.join(chosen.directory, module + "_tests.html")
    if chosen.wasm and os.path.exists(page):
        run_page_tests(page, args)
        return
    binary = dev.executable(chosen.directory, module + "_tests", chosen.wasm)
    if not os.path.exists(binary):
        suffix = ".js" if chosen.wasm else ""
        endings = ["_tests" + suffix] + (["_tests.html"] if chosen.wasm else [])
        names = sorted(name[:-len(ending)] for name in os.listdir(chosen.directory) for ending in endings
                       if name.endswith(ending))
        dev.fail("no unit tests named %s; one of: %s" % (module, " ".join(names)))
    if chosen.wasm:
        dev.run([emsdk.node(), binary, *args])
    elif module in NETWORK_MODULES:
        dev.python("tools.harness.net_peer", binary, *args)
    else:
        dev.run([binary, *args])


# The targets of ./dev.sh ci, by the name that narrows a run to them.
CI_TARGETS = ("host", "debug", "wasm", "ios")


def ci(argv):
    dev.usage(argv, USAGE["ci"])
    configure_only = argv[:1] == ["--configure"]
    key = argv[:1] == ["--key"]
    if configure_only or key:
        argv = argv[1:]
    mode = argv.pop(0) if argv[:1] == ["asan"] else None
    if any(argument not in CI_TARGETS for argument in argv) or (mode and argv):
        dev.usage(argv, USAGE["ci"], 1)
    chosen = argv or CI_TARGETS
    host = ("macOS" if dev.darwin() else "Windows" if dev.WINDOWS else sys.platform) + (" " + mode if mode else "")
    # Each step's name, its build, and its work.
    steps = []
    if "host" in chosen:
        steps.append((host, mode, lambda: unit_tests(mode, lean=True)))
    if mode is None:
        # The debug build checks the assertions, which the default build compiles out.
        if "debug" in chosen:
            steps.append((host + " debug", "debug", lambda: unit_tests("debug", lean=True)))
        # Emscripten's SDK is two gigabytes, which a check of the web's build on one platform is enough to fetch.
        if "wasm" in chosen and dev.darwin():
            steps.append(("wasm", "wasm", lambda: unit_tests("wasm", lean=True)))
        if "ios" in chosen and dev.darwin():
            steps.append(("iOS (build)", "ios", lambda: builds.build("ios", lean=True)))
    if key:
        print(fetch_key(any(built == "wasm" for _, built, _ in steps)))
        return
    if configure_only:
        before = sizes()
        lint.sources()
        for _, built, _ in steps:
            builds.build(built, lean=True, configure_only=True)
        report_fetched(before, sizes())
        return
    linting = lint.Background()
    outcomes = []
    for name, _, work in steps:
        print("\nci: %s" % name, flush=True)
        start = time.monotonic()
        status = lint.attempt(work)
        outcomes.append((name, status, "%.0fs" % (time.monotonic() - start)))
    outcomes.append(("lint", linting.finish(), ""))
    print("\nci:")
    width = max(len(name) for name, _, _ in outcomes)
    for name, status, took in outcomes:
        print("    %s  %s  %s" % (name.ljust(width), "ok    " if status == 0 else "FAILED", took))
    sys.exit(1 if any(status for _, status, _ in outcomes) else 0)


# The most that third_party may hold in CI, in megabytes, where it holds what ./dev.sh ci fetched and nothing else: what
# its lean builds, their tests and lint read. A run that holds more fails, so that what a change adds to every run is a
# deliberate act: the budget moves with it. A developer's third_party holds what every command fetched, and the report
# of it does not fail.
FETCH_BUDGET = 400


# The files that pin what a lean build fetches and choose which of it it fetches.
FETCH_FILES = ("cmake/dependencies.cmake", "cmake/dependencies_tree_sitter.cmake", "cmake/test_data.cmake",
               "cmake/lean.cmake", "lib/font_bundle/font_bundle.cmake")


def fetch_key(wasm):
    """Returns a hash of what decides what ./dev.sh ci fetches: the files that pin and choose what its lean builds
    fetch, the pins of lint's checkouts, and with `wasm`, the version of Emscripten's SDK."""
    import hashlib

    digest = hashlib.sha256()
    for name in FETCH_FILES:
        with open(os.path.join(dev.ROOT, name), "rb") as f:
            digest.update(name.encode() + b"\0" + f.read() + b"\0")
    for name in lint.SOURCES:
        for part in checkouts.CHECKOUTS[name]():
            digest.update(repr(sorted(vars(part).items())).encode() + b"\0")
    if wasm:
        digest.update(emsdk.VERSION.encode())
    return digest.hexdigest()[:24]


def size(path):
    """Returns the bytes of the files under `path`, without following links."""
    return sum(os.path.getsize(os.path.join(directory, name)) for directory, _, names in os.walk(path)
               for name in names if not os.path.islink(os.path.join(directory, name)))


def sizes():
    """Returns what third_party holds, as {name: bytes}: each download of the builds (downloads/<name>), and each
    checkout."""
    found = {}
    if not os.path.isdir(checkouts.THIRD_PARTY):
        return found
    for entry in sorted(os.listdir(checkouts.THIRD_PARTY)):
        path = os.path.join(checkouts.THIRD_PARTY, entry)
        if entry == "downloads" and os.path.isdir(path):
            for name in sorted(os.listdir(path)):
                found["downloads/" + name] = size(os.path.join(path, name))
        elif os.path.isdir(path):
            found[entry] = size(path)
    return found


def report_fetched(before, after):
    """Prints what third_party holds (`after`), the largest first, with what the run fetched, which `before` lacked or
    held at another size, marked. Under GitHub Actions, the report is also the step's summary, and the run fails if
    third_party holds more than FETCH_BUDGET."""
    megabytes = lambda size: size / 1e6
    total = sum(after.values())
    new = {name for name, size in after.items() if size != before.get(name)}
    lines = ["%d entries, %.0f MB of a budget of %d MB; this run fetched %d, %.0f MB"
             % (len(after), megabytes(total), FETCH_BUDGET, len(new), megabytes(sum(after[n] for n in new)))]
    for name, size in sorted(after.items(), key=lambda entry: -entry[1]):
        lines.append("    %7.1f MB  %s%s" % (megabytes(size), name, "  (fetched)" if name in new else ""))
    print("\nfetched: " + "\n".join(lines), flush=True)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write("### What ./dev.sh ci fetches\n\n%s\n\n| | MB | |\n| --- | ---: | --- |\n" % lines[0])
            for name, size in sorted(after.items(), key=lambda entry: -entry[1]):
                f.write("| %s | %.1f | %s |\n" % (name, megabytes(size), "fetched" if name in new else ""))
    if os.environ.get("GITHUB_ACTIONS") == "true" and megabytes(total) > FETCH_BUDGET:
        dev.fail("third_party holds %.0f MB, past the budget of %d MB (FETCH_BUDGET in tools/build/commands.py)"
                 % (megabytes(total), FETCH_BUDGET))


def application_names(folder):
    """The applications under `folder`, examples or apps, each a directory whose name is its target's."""
    root = os.path.join(dev.ROOT, folder)
    return sorted(name for name in os.listdir(root) if os.path.isdir(os.path.join(root, name)))


def app(argv):
    dev.usage(argv, USAGE["app"], None if argv else 1)
    names = application_names("apps")
    if argv[0] not in names:
        dev.fail("no app named %s; one of: %s" % (argv[0], " ".join(names)))
    chosen = builds.build(targets=[argv[0]])
    dev.run([dev.executable(chosen.directory, argv[0]), *argv[1:]])


def example(argv):
    dev.usage(argv, USAGE["example"], None if argv else 1)
    wasm = argv[0] == "wasm"
    if wasm:
        argv = argv[1:]
        dev.usage(argv, USAGE["example"], None if argv else 1)
    names = application_names("examples")
    if argv[0] not in names:
        dev.fail("no example named %s; one of: %s" % (argv[0], " ".join(names)))
    if argv[0] in builds.EXAMPLE_CHECKOUTS:
        checkouts.fetch(*builds.EXAMPLE_CHECKOUTS[argv[0]])
    if not wasm:
        chosen = builds.build(targets=[argv[0]])
        dev.run([dev.executable(chosen.directory, argv[0]), *argv[1:]])
        return
    name, args = argv[0], argv[1:]
    screenshot = None
    seconds = 4.0
    while args[:1] in (["--screenshot"], ["--seconds"]):
        if len(args) < 2:
            dev.usage([], USAGE["example"], 1)
        if args[0] == "--screenshot":
            screenshot = os.path.abspath(args[1])
        else:
            seconds = float(args[1])
        args = args[2:]
    chosen = builds.build("wasm", targets=[name])
    page = os.path.join(chosen.directory, name + ".html")
    if screenshot:
        capture_page(page, args, screenshot, seconds)
    else:
        serve_page(page, args)


def page_url(server, page, args):
    """The URL of `page` on the server, whose query passes `args` to the program (examples/shell.html)."""
    from urllib.parse import urlencode

    from tools.browsers import browsers

    url = "http://127.0.0.1:%d%s" % (server.server_address[1], browsers.url_of(page))
    return url + ("?" + urlencode([("arg", a) for a in args]) if args else "")


def serve_page(page, args):
    """Serves the checkout, which holds the build, and opens `page` in the system's browser until interrupted."""
    import time
    import webbrowser

    from tools.browsers import browsers

    server = browsers.Server()
    url = page_url(server, page, args)
    print("serving %s until interrupted" % url, flush=True)
    webbrowser.open(url)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        server.shutdown()


class ChromePage:
    """A page of headless Chrome at 960x720, driven through the DevTools protocol, so that the page's animation frames
    and media run as they do in a window. It keeps the messages that the page logs. Use it in a `with` statement, which
    serves the checkout and stops Chrome at the end."""

    def __enter__(self):
        import json
        import subprocess
        import tempfile
        import time
        import urllib.request

        from tools.browsers import browsers, probe, websocket

        chrome = browsers.BROWSERS["chrome"]
        if not chrome.installed(None):
            dev.fail("this needs Chrome, which is not installed")
        self.server = browsers.Server()
        self.logged = []
        self.serial = 0
        self.profile = tempfile.TemporaryDirectory()
        port = probe.free_port()
        self.process = subprocess.Popen([chrome.path(None), "--headless=new", "--window-size=960,720",
                                         "--hide-scrollbars", "--autoplay-policy=no-user-gesture-required",
                                         "--remote-debugging-port=%d" % port] +
                                        browsers.chrome_arguments(self.profile.name) + ["about:blank"],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            target = None
            deadline = time.monotonic() + 30
            while target is None:
                try:
                    with urllib.request.urlopen("http://127.0.0.1:%d/json/list" % port, timeout=1) as response:
                        pages = [t for t in json.loads(response.read()) if t["type"] == "page"]
                        target = pages[0]["webSocketDebuggerUrl"] if pages else None
                except OSError:
                    pass
                if target is None:
                    if time.monotonic() > deadline:
                        dev.fail("Chrome offered no page")
                    time.sleep(0.1)
            self.connection = websocket.connect(target, 1)
            self.call("Runtime.enable")
            self.call("Page.enable")
            self.call("Emulation.setDeviceMetricsOverride",
                      {"width": 960, "height": 720, "deviceScaleFactor": 1, "mobile": False})
        except BaseException:
            self.__exit__(None, None, None)
            raise
        return self

    def __exit__(self, *_):
        connection = getattr(self, "connection", None)
        if connection is not None:
            connection.close()
        self.process.terminate()
        self.process.wait()
        self.profile.cleanup()
        self.server.shutdown()

    def message(self):
        """Returns the next message from Chrome, or None if none arrives within a second. A message of the console is
        kept in `logged`."""
        import json
        import socket

        try:
            answer = json.loads(self.connection.receive())
        except (socket.timeout, TimeoutError):
            return None
        if answer.get("method") == "Runtime.consoleAPICalled":
            self.logged.append(" ".join(str(argument.get("value", argument.get("description", "")))
                                        for argument in answer["params"]["args"]))
        return answer

    def call(self, method, parameters=None):
        """Calls a method and returns its result."""
        import json

        self.serial += 1
        self.connection.send(json.dumps({"id": self.serial, "method": method, "params": parameters or {}}))
        while True:
            answer = self.message()
            if answer is not None and answer.get("id") == self.serial:
                if "error" in answer:
                    dev.fail("%s: %s" % (method, answer["error"]))
                return answer["result"]

    def open(self, page, args):
        """Navigates to `page`, built under the checkout, with `args` in its query."""
        self.call("Page.navigate", {"url": page_url(self.server, page, args)})


def capture_page(page, args, png, seconds):
    """Captures `page` in headless Chrome into `png` once it has run for `seconds`, in real time, and prints the
    messages that the page logged."""
    import base64
    import time

    with ChromePage() as chrome:
        chrome.open(page, args)
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            chrome.message()
        shot = chrome.call("Page.captureScreenshot", {"format": "png"})
        with open(png, "wb") as f:
            f.write(base64.b64decode(shot["data"]))
    for line in chrome.logged:
        print(line)
    print("wrote %s" % png)


# The seconds that a page's tests may run before they fail as stalled.
PAGE_TEST_TIMEOUT = 300


def run_page_tests(page, args):
    """Runs the page tests of `page` (pr_page_tests) in headless Chrome, prints what they logged, and fails if they
    fail."""
    import time

    print("page tests: %s" % os.path.basename(page), flush=True)
    status = None
    with ChromePage() as chrome:
        chrome.open(page, args)
        deadline = time.monotonic() + PAGE_TEST_TIMEOUT
        printed = 0
        while status is None and time.monotonic() < deadline:
            chrome.message()
            for line in chrome.logged[printed:]:
                if line.startswith("pr-page-tests-exit "):
                    status = line.split()[1]
                else:
                    print(line, flush=True)
            printed = len(chrome.logged)
    if status is None:
        dev.fail("%s stalled for %d seconds" % (os.path.basename(page), PAGE_TEST_TIMEOUT))
    if status != "0":
        dev.fail("%s failed with status %s" % (os.path.basename(page), status))


def clean(argv):
    dev.usage(argv, USAGE["clean"])
    for chosen in builds.MODES.values():
        shutil.rmtree(chosen.directory, ignore_errors=True)
        shutil.rmtree(chosen.lean().directory, ignore_errors=True)
    for directory in (builds.AB, builds.PACKAGE):
        shutil.rmtree(directory, ignore_errors=True)
