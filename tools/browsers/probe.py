#!/usr/bin/env python3
"""Runs a probe page in the browsers and the engine, and prints what each measured for each case, side by side.

A probe page holds cases, each a figure with data-case, data-what and data-measure, and measures them once its fonts
have loaded. It writes its results as one JSON object, {"dpr": ..., "cases": {case: [value, ...]}}, posts it to the
URL that its ?post= parameter names, and logs it to the console.

Each browser loads the page from a server for the checkout, as the browser benchmarks do (tools/browsers/browsers.py):
Chrome at a pixel ratio of two and of one, WebKit in the build's own host, and Firefox. WebKit and Firefox run at the
display's pixel ratio. The engine runs the page in pr_shell. The report is a table of every case, in which a
browser's column says `same` if the browser measured what the first column measured. Screenshots are for a user who
prefers a dark color scheme, or a light one with --scheme light.

With --safari, the S column is Safari itself, through safaridriver, in place of WebKit in the build's host, for a
capture that must be what Safari shows. Safari needs its "Allow remote automation" setting. It draws at the display's
ratio and takes the system's appearance, neither of which WebDriver can set, so it skips a run at another ratio or
for the other scheme.

With --screenshot <directory>, each column instead writes a PNG of the page to the directory, named for the column.
The screenshots show how each browser paints what the measures cannot show. The engine's screenshot comes from
pr_shell.

With --scan, the columns' screenshots are read along the scanlines that the page's figures name, and the report gives,
for each scanline, the pixels that each column painted along it, a line for each column, `same` where it is the
first column's. A figure names its scanlines in data-scan="h <y> <x0> <x1>; v <x> <y0> <y1>", in CSS px of the page: a
horizontal line at y from x0 to x1, or a vertical one. Each reads the row or column of the screenshot's pixels that
holds the line, and gives the runs of pixels that are not white, as `<first>-<last>:<color>` in pixels from the line's
start, with the color in hex. Scanlines show how each browser snaps and covers what it paints, where the measures and a
glance at the screenshots cannot. The screenshots go to a temporary directory unless --screenshot names one.

With --pixel-ratio <pixels per px>, every browser and the engine run at that many pixels per CSS pixel, in place of the
display's, so that a fractional ratio such as 1.5 can be compared. Chrome then runs once, at the ratio, and each
screenshot has the ratio's pixels per CSS pixel.

With --keys, a browser presses the keys that the page asks for, as trusted input, for what only input can show, such as
a caret's motion. The page defines probeKey(), which returns the next key as {key, modifiers}, or null once it has set
window.probeReport to its results. A key is named as KeyboardEvent.key names it, and a modifier is Alt, Control, Meta or
Shift. Chrome runs through the DevTools protocol, with the editing commands that macOS's key bindings give it, Firefox
through Marionette, Safari through safaridriver with --safari, and the engine through the shell's sendInput. The build's
WebKit host takes no input, so its column is left out. --no-key-commands leaves macOS's commands out, which shows what
Chrome's own key map does, as on other systems.

Usage: python3 -m tools.browsers.probe --build <directory> <page> [--browser NAME]... [--timeout S] [--json]
                              [--screenshot <directory>] [--scan] [--viewport WxH] [--pixel-ratio <pixels per px>]
"""

import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

from tools.bench import common
from tools.browsers import browsers
from tools.browsers import websocket

# The columns that a run measures: a name, the browser that measures it, and the arguments that it adds to the
# browser's own.
COLUMNS = [
    ("C", "chrome", []),
    ("C1", "chrome", ["--force-device-scale-factor=1"]),
    ("S", "webkit", []),
    ("F", "firefox", []),
]

# The default viewport that every column runs the page in, matching pr_shell's default.
VIEWPORT = "800x600"


def columns_at(pixel_ratio):
    """Returns the columns of a run at `pixel_ratio` pixels per CSS pixel: Chrome once, WebKit and Firefox, each with
    the arguments that set the ratio. Firefox takes it from its profile (preferences_of)."""
    return [
        ("C", "chrome", ["--force-device-scale-factor=%g" % pixel_ratio]),
        ("S", "webkit", ["--pixel-ratio", "%g" % pixel_ratio]),
        ("F", "firefox", []),
    ]


def preferences_of(name, pixel_ratio):
    """Returns the preferences that browser `name`'s profile adds for `pixel_ratio`: Firefox takes its pixels per CSS
    pixel from one."""
    return {"layout.css.devPixelsPerPx": "%g" % pixel_ratio} if name == "firefox" and pixel_ratio is not None else {}


# Safari's app, which safaridriver drives.
SAFARI = "/Applications/Safari.app"


def installed(name, directory):
    """Returns true if browser `name` can run."""
    if name == "safari":
        return os.path.exists(SAFARI) and shutil.which("safaridriver") is not None
    return browsers.BROWSERS[name].installed(directory)


def version(name):
    if name == "safari":
        return subprocess.run(["plutil", "-extract", "CFBundleShortVersionString", "raw",
                               os.path.join(SAFARI, "Contents", "Info.plist")],
                              capture_output=True, text=True).stdout.strip()
    return browsers.BROWSERS[name].version()


def run_browser(server, name, extra, url, timeout, directory, pixel_ratio=None):
    """Returns what browser `name`, launched with `extra` arguments, reported for the page at `url`, or None."""
    if name == "safari":
        return run_safari(server, url, timeout)
    browser = browsers.BROWSERS[name]
    profile = tempfile.mkdtemp(prefix="probe-%s-" % name)
    process = None
    server.report = None
    server.reported.clear()
    try:
        process = browser.launch(url, profile, directory, extra, preferences_of(name, pixel_ratio))
        if not server.reported.wait(timeout):
            print("  %s did not report within %ds" % (name, timeout), file=sys.stderr)
            return None
        return server.report
    finally:
        if process is not None:
            browsers.stop(process)
        shutil.rmtree(profile, ignore_errors=True)


def run_safari(server, url, timeout):
    """Returns what Safari reported for the page at `url`, or None."""
    with SAFARI_LOCK:
        return safari_report(server, url, timeout)


def safari_report(server, url, timeout):
    server.report = None
    server.reported.clear()
    driver = None
    try:
        driver = SafariDriver(timeout)
        driver.call("POST", "/url", {"url": url})
        if not server.reported.wait(timeout):
            print("  safari did not report within %ds" % timeout, file=sys.stderr)
            return None
        return server.report
    except (OSError, RuntimeError) as error:
        print("  safari did not run: %s" % error, file=sys.stderr)
        return None
    finally:
        if driver is not None:
            driver.close()


def shell_pixel_ratio(pixel_ratio):
    return ["--pixel-ratio", "%g" % pixel_ratio] if pixel_ratio is not None else []


# A prelude that presses the keys that a page asks for through the shell's input, and logs what the page reports. The
# modifiers are the flags of PrAppEvent.
ENGINE_KEYS = """
addEventListener("load", async () => {
    const flags = {Shift: 1, Control: 2, Alt: 4, Meta: 8};
    for (let step = probeKey(); step !== null; step = probeKey()) {
        const modifiers = (step.modifiers || []).reduce((all, name) => all | flags[name], 0);
        const event = {key: step.key, code: step.key, modifiers};
        await sendInput([{type: "keyDown", ...event}, {type: "keyUp", ...event}]);
    }
    console.log(JSON.stringify(probeReport));
    stopRun();
});
"""


def run_engine(page, directory, viewport, pixel_ratio=None, keys=False, extra=()):
    """Returns what pr_shell logged when it ran the page, a path or a URL, at `viewport`, or None. The console prints
    on stderr. With `keys`, the shell presses the keys that the page asks for. `extra` are more of the shell's
    arguments."""
    shell = common.binary(directory, "pr_shell")
    prelude = []
    script = None
    if keys:
        script = tempfile.NamedTemporaryFile("w", suffix=".js", delete=False)
        script.write(ENGINE_KEYS)
        script.close()
        prelude = ["--prelude", script.name]
    try:
        done = subprocess.run([shell, "--viewport", viewport] + shell_pixel_ratio(pixel_ratio) + prelude + list(extra)
                              + ["--html", page], capture_output=True, text=True)
    finally:
        if script is not None:
            os.remove(script.name)
    for line in reversed((done.stdout + done.stderr).splitlines()):
        line = line.strip()
        if line.startswith("{") and '"cases"' in line:
            return json.loads(line)
    print("  the engine reported nothing:\n%s" % done.stderr.strip()[-2000:], file=sys.stderr)
    return None


def screenshot(column, name, extra, url, path, timeout, directory, viewport, pixel_ratio, scheme):
    """Writes browser `name`'s rendering of `url` at `viewport` to `path`, for a user who prefers the color `scheme`,
    light or dark. Returns true if it wrote the file."""
    dark = scheme == "dark"
    if name == "safari":
        width, height = viewport.split("x")
        return safari_screenshot(column, url, path, timeout, int(width), int(height), pixel_ratio, dark)
    browser = browsers.BROWSERS[name]
    width, height = viewport.split("x")
    # The run ends when the file stops growing, so a file left by an earlier run would end it immediately.
    if os.path.exists(path):
        os.remove(path)
    profile = tempfile.mkdtemp(prefix="probe-%s-" % name)
    try:
        if name == "chrome":
            command = [browser.path(directory), "--headless", "--user-data-dir=" + profile, "--hide-scrollbars",
                       "--window-size=%s,%s" % (width, height), "--screenshot=" + path,
                       # The screenshot waits, for up to 5s of virtual time, until the page's network is idle and its
                       # web fonts have loaded, instead of firing at the load event, which may come earlier.
                       "--virtual-time-budget=5000",
                       # Blink's preferred scheme: 0 is dark and 1 light.
                       "--blink-settings=preferredColorScheme=%d" % (0 if dark else 1)] + extra + [url]
        elif name == "firefox":
            return firefox_screenshot(column, browser, url, path, timeout, directory, profile, int(width), int(height),
                                      pixel_ratio, dark)
        else:
            command = [browser.path(directory), "--screenshot", path, "--size", viewport,
                       "--appearance", scheme] + extra + [url]
        # A headless browser may stay open after it writes its screenshot, so the run ends once the file has been
        # written and has stopped growing.
        process = browsers.start(command, browser.environment)
        deadline = time.monotonic() + timeout
        size = -1
        while process.poll() is None and time.monotonic() < deadline:
            time.sleep(0.5)
            if os.path.exists(path) and os.path.getsize(path) == size:
                break
            size = os.path.getsize(path) if os.path.exists(path) else -1
        browsers.stop(process)
        if not os.path.exists(path):
            print("  %s did not finish its screenshot within %ds" % (column, timeout), file=sys.stderr)
        return os.path.exists(path)
    finally:
        shutil.rmtree(profile, ignore_errors=True)


class Marionette:
    """A client of Firefox's remote protocol (Marionette): each message is its length in decimal, a colon, and JSON. A
    command is [0, id, name, parameters], and its answer is [1, id, error, result]."""

    def __init__(self, port, timeout):
        import socket
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.socket = socket.create_connection(("127.0.0.1", port), timeout=timeout)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.2)
        self.buffer = b""
        self.id = 0
        self.read()                 # the server's greeting

    def read(self):
        while b":" not in self.buffer:
            self.buffer += self.socket.recv(65536)
        length, _, rest = self.buffer.partition(b":")
        size = int(length)
        while len(rest) < size:
            rest += self.socket.recv(65536)
        self.buffer = rest[size:]
        return json.loads(rest[:size])

    def call(self, name, parameters=None):
        self.id += 1
        body = json.dumps([0, self.id, name, parameters or {}]).encode("utf-8")
        self.socket.sendall(b"%d:%s" % (len(body), body))
        while True:
            answer = self.read()
            if answer[0] == 1 and answer[1] == self.id:
                if answer[2] is not None:
                    raise RuntimeError("%s: %s" % (name, answer[2]))
                return answer[3]


def free_port():
    import socket
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


# A script that waits for the page's fonts, and for two frames after them, before a screenshot.
READY = ("const done = arguments[arguments.length - 1];"
         "document.fonts.ready.then(() => requestAnimationFrame(() => requestAnimationFrame(() => done(true))));")


def shot_ratio(client):
    """Returns the page's devicePixelRatio: its pixels per CSS px."""
    return client.call("WebDriver:ExecuteScript", {"script": "return devicePixelRatio;", "args": []})["value"]


def firefox_screenshot(column, browser, url, path, timeout, directory, profile, width, height, pixel_ratio, dark):
    """Writes Firefox's rendering of `url` in a viewport of `width` by `height` CSS px to `path`, through Marionette,
    whose screenshot has the window's pixels. Firefox's own --screenshot draws one pixel per CSS px, whatever the
    ratio. Returns true if it wrote the file."""
    port = free_port()
    # The content's preferred scheme: 0 is dark and 1 light.
    preferences = dict(preferences_of("firefox", pixel_ratio), **{"marionette.port": port,
                                                           "layout.css.prefers-color-scheme.content-override":
                                                               0 if dark else 1})
    browser.profile(profile, preferences)
    process = browsers.start([browser.path(directory), "--headless", "--marionette", "-no-remote", "-profile", profile],
                             browser.environment)
    try:
        client = Marionette(port, timeout)
        client.call("WebDriver:NewSession", {"capabilities": {}})
        client.call("WebDriver:Navigate", {"url": url})
        # The window's chrome takes room around the viewport, so the window is sized for the viewport it leaves.
        inner = client.call("WebDriver:ExecuteScript",
                            {"script": "return [innerWidth, innerHeight, outerWidth, outerHeight];", "args": []})
        inner = inner["value"]
        client.call("WebDriver:SetWindowRect",
                    {"width": width + inner[2] - inner[0], "height": height + inner[3] - inner[1]})
        client.call("WebDriver:ExecuteAsyncScript", {"script": READY, "args": []})
        shot = client.call("WebDriver:TakeScreenshot", {"full": False})
        import base64
        with open(path, "wb") as f:
            f.write(base64.b64decode(shot["value"]))
        # Firefox sizes its window in whole CSS px around a viewport that may come out a fraction of a px larger at a
        # fractional ratio, so the image is cut to the viewport's pixels, as the other columns give them.
        from PIL import Image
        ratio = pixel_ratio if pixel_ratio is not None else shot_ratio(client)
        with Image.open(path) as image:
            cut = image.crop((0, 0, min(image.width, math.ceil(width * ratio)),
                              min(image.height, math.ceil(height * ratio))))
            cut.load()
        cut.save(path)
        client.call("WebDriver:DeleteSession")
        return True
    except (OSError, RuntimeError, ValueError) as error:
        print("  %s did not take its screenshot: %s" % (column, error), file=sys.stderr)
        return False
    finally:
        browsers.stop(process)


class SafariDriver:
    """A client of safaridriver, Safari's WebDriver server, which runs pages in Safari's automation windows, apart from
    the user's browsing. Safari takes WebDriver sessions only once its "Allow remote automation" setting is on."""

    def __init__(self, timeout):
        import urllib.request
        self.port = free_port()
        self.timeout = timeout
        self.process = subprocess.Popen(["safaridriver", "-p", str(self.port)], stdout=subprocess.DEVNULL,
                                        stderr=subprocess.DEVNULL)
        self.session = None
        deadline = time.monotonic() + timeout
        while True:
            try:
                urllib.request.urlopen("http://127.0.0.1:%d/status" % self.port, timeout=1)
                break
            except OSError:
                if time.monotonic() > deadline:
                    self.close()
                    raise
                time.sleep(0.1)
        self.session = self.call("POST", "/session", {"capabilities": {"alwaysMatch": {"browserName": "safari"}}},
                                 session=False)["sessionId"]

    def call(self, method, path, body=None, session=True):
        import urllib.error
        import urllib.request
        prefix = "/session/%s" % self.session if session else ""
        data = json.dumps(body).encode("utf-8") if body is not None else None
        request = urllib.request.Request("http://127.0.0.1:%d%s%s" % (self.port, prefix, path), data=data,
                                         method=method, headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                return json.loads(response.read())["value"]
        except urllib.error.HTTPError as error:
            answer = json.loads(error.read() or b"{}").get("value", {})
            raise RuntimeError("%s %s: %s" % (method, path, answer.get("message", error)))

    def script(self, source):
        return self.call("POST", "/execute/sync", {"script": source, "args": []})

    def close(self):
        if self.session is not None:
            try:
                self.call("DELETE", "")
            except (OSError, RuntimeError):
                pass
        self.process.terminate()
        try:
            self.process.wait(10)
        except subprocess.TimeoutExpired:
            self.process.kill()


# The WebDriver code points of the keys that a page may ask for, and of the modifiers.
WEBDRIVER_KEYS = {
    "ArrowLeft": "\ue012",
    "ArrowUp": "\ue013",
    "ArrowRight": "\ue014",
    "ArrowDown": "\ue015",
    "Home": "\ue011",
    "End": "\ue010",
    "Backspace": "\ue003",
    "Delete": "\ue017",
    "Alt": "\ue00a",
    "Control": "\ue009",
    "Meta": "\ue03d",
    "Shift": "\ue008",
}

# The virtual key codes of those keys in the DevTools protocol, and the bits of its modifiers.
CHROME_KEY_CODES = {"ArrowLeft": 37, "ArrowUp": 38, "ArrowRight": 39, "ArrowDown": 40, "Home": 36, "End": 35,
                    "Backspace": 8, "Delete": 46}
CHROME_MODIFIERS = {"Alt": 1, "Control": 2, "Meta": 4, "Shift": 8}

# The editing commands that macOS's standard key bindings give each key, by its modifier, which a window's text input
# sends with a key press and the DevTools protocol does not.
MAC_COMMANDS = {
    ("ArrowLeft", ""): "moveLeft",
    ("ArrowRight", ""): "moveRight",
    ("ArrowUp", ""): "moveUp",
    ("ArrowDown", ""): "moveDown",
    ("ArrowLeft", "Alt"): "moveWordLeft",
    ("ArrowRight", "Alt"): "moveWordRight",
    ("ArrowLeft", "Meta"): "moveToLeftEndOfLine",
    ("ArrowRight", "Meta"): "moveToRightEndOfLine",
    ("ArrowUp", "Meta"): "moveToBeginningOfDocument",
    ("ArrowDown", "Meta"): "moveToEndOfDocument",
    ("Backspace", ""): "deleteBackward",
    ("Delete", ""): "deleteForward",
    ("Home", ""): "scrollToBeginningOfDocument",
    ("End", ""): "scrollToEndOfDocument",
}


def webdriver_actions(key, modifiers):
    """Returns the WebDriver actions that press `key` with `modifiers` held."""
    held = [WEBDRIVER_KEYS[m] for m in modifiers]
    code = WEBDRIVER_KEYS[key]
    events = [{"type": "keyDown", "value": v} for v in held + [code]]
    events += [{"type": "keyUp", "value": v} for v in [code] + held[::-1]]
    return [{"type": "key", "id": "keyboard", "actions": events}]


class ChromeKeys:
    """Chrome with a page at `url`, driven through the DevTools protocol."""

    def __init__(self, browser, url, directory, profile, timeout, commands):
        import urllib.request
        self.commands = commands
        port = free_port()
        self.process = browsers.start([browser.path(directory), "--headless", "--user-data-dir=" + profile,
                                       "--remote-debugging-port=%d" % port, url], browser.environment)
        deadline = time.monotonic() + timeout
        target = None
        while target is None:
            try:
                with urllib.request.urlopen("http://127.0.0.1:%d/json/list" % port, timeout=1) as response:
                    pages = [t for t in json.loads(response.read()) if t["type"] == "page"]
                    target = pages[0]["webSocketDebuggerUrl"] if pages else None
            except OSError:
                pass
            if target is None:
                if time.monotonic() > deadline:
                    raise RuntimeError("Chrome offered no page")
                time.sleep(0.1)
        self.connection = websocket.connect(target, timeout)
        self.id = 0
        self.call("Emulation.setFocusEmulationEnabled", {"enabled": True})

    def call(self, method, parameters=None):
        self.id += 1
        self.connection.send(json.dumps({"id": self.id, "method": method, "params": parameters or {}}))
        while True:
            answer = json.loads(self.connection.receive())
            if answer.get("id") == self.id:
                if "error" in answer:
                    raise RuntimeError("%s: %s" % (method, answer["error"]))
                return answer["result"]

    def evaluate(self, source):
        return self.call("Runtime.evaluate", {"expression": source, "returnByValue": True})["result"].get("value")

    def press(self, key, modifiers):
        bits = sum(CHROME_MODIFIERS[m] for m in modifiers)
        code = CHROME_KEY_CODES[key]
        down = {"type": "rawKeyDown", "key": key, "code": key, "windowsVirtualKeyCode": code, "modifiers": bits}
        command = MAC_COMMANDS.get((key, "+".join(modifiers)))
        if self.commands and sys.platform == "darwin" and command is not None:
            down["commands"] = [command]
        self.call("Input.dispatchKeyEvent", down)
        self.call("Input.dispatchKeyEvent", {"type": "keyUp", "key": key, "code": key, "windowsVirtualKeyCode": code,
                                             "modifiers": bits})

    def close(self):
        self.connection.close()
        browsers.stop(self.process)


class FirefoxKeys:
    """Firefox with a page at `url`, driven through Marionette."""

    def __init__(self, browser, url, directory, profile, timeout):
        port = free_port()
        browser.profile(profile, dict(preferences_of("firefox", None), **{"marionette.port": port}))
        self.process = browsers.start([browser.path(directory), "--headless", "--marionette", "-no-remote", "-profile",
                                       profile], browser.environment)
        self.client = Marionette(port, timeout)
        self.client.call("WebDriver:NewSession", {"capabilities": {}})
        self.client.call("WebDriver:Navigate", {"url": url})

    def evaluate(self, source):
        return self.client.call("WebDriver:ExecuteScript", {"script": "return (%s);" % source, "args": []})["value"]

    def press(self, key, modifiers):
        self.client.call("WebDriver:PerformActions", {"actions": webdriver_actions(key, modifiers)})
        self.client.call("WebDriver:ReleaseActions")

    def close(self):
        browsers.stop(self.process)


class SafariKeys:
    """Safari with a page at `url`, driven through safaridriver."""

    def __init__(self, url, timeout):
        self.driver = SafariDriver(timeout)
        self.driver.call("POST", "/url", {"url": url})

    def evaluate(self, source):
        return self.driver.script("return (%s);" % source)

    def press(self, key, modifiers):
        self.driver.call("POST", "/actions", {"actions": webdriver_actions(key, modifiers)})
        self.driver.call("DELETE", "/actions")

    def close(self):
        self.driver.close()


def run_keys(name, url, timeout, directory, commands):
    """Returns what browser `name` reported after pressing the keys that the page at `url` asked for, or None."""
    profile = tempfile.mkdtemp(prefix="probe-%s-" % name)
    driver = None
    try:
        if name == "chrome":
            driver = ChromeKeys(browsers.BROWSERS[name], url, directory, profile, timeout, commands)
        elif name == "firefox":
            driver = FirefoxKeys(browsers.BROWSERS[name], url, directory, profile, timeout)
        else:
            driver = SafariKeys(url, timeout)
        deadline = time.monotonic() + timeout
        while driver.evaluate("typeof window.probeKey") != "function":
            if time.monotonic() > deadline:
                raise RuntimeError("the page defined no probeKey")
            time.sleep(0.1)
        while True:
            step = driver.evaluate("window.probeKey()")
            if step is None:
                return json.loads(driver.evaluate("JSON.stringify(window.probeReport)"))
            driver.press(step["key"], step.get("modifiers", []))
    except (OSError, RuntimeError, ValueError, websocket.Error) as error:
        print("  %s did not run: %s" % (name, error), file=sys.stderr)
        return None
    finally:
        if driver is not None:
            driver.close()
        shutil.rmtree(profile, ignore_errors=True)


# safaridriver runs one session at a time, so the threads that capture pages take turns with Safari.
SAFARI_LOCK = threading.Lock()

# How much taller than the page's viewport Safari's is made. The bottom corners of Safari's window are rounded over the
# viewport, so the screenshot is cut from the top of a taller one.
SAFARI_EXTRA_HEIGHT = 64


def safari_screenshot(column, url, path, timeout, width, height, pixel_ratio, dark):
    """Writes Safari's rendering of `url` in a viewport of `width` by `height` CSS px to `path`. Safari draws at the
    display's ratio and takes the system's color scheme, which WebDriver cannot set, so a run that asks for another
    ratio or scheme writes nothing. Returns true if it wrote the file."""
    with SAFARI_LOCK:
        return safari_capture(column, url, path, timeout, width, height, pixel_ratio, dark)


def safari_capture(column, url, path, timeout, width, height, pixel_ratio, dark):
    driver = None
    try:
        driver = SafariDriver(timeout)
        driver.call("POST", "/url", {"url": url})
        if driver.script("return matchMedia('(prefers-color-scheme: dark)').matches;") != dark:
            raise RuntimeError("Safari takes the system's color scheme, which is not %s" % ("dark" if dark else "light"))
        ratio = driver.script("return devicePixelRatio;")
        if pixel_ratio is not None and ratio != pixel_ratio:
            raise RuntimeError("Safari draws at the display's %g pixels per px, not %g" % (ratio, pixel_ratio))
        # The window's chrome takes room around the viewport, so the window is sized for the viewport it leaves.
        tall = height + SAFARI_EXTRA_HEIGHT
        for _ in range(3):
            inner = driver.script("return [innerWidth, innerHeight, outerWidth, outerHeight];")
            if inner[0] == width and inner[1] == tall:
                break
            driver.call("POST", "/window/rect", {"width": width + inner[2] - inner[0],
                                                 "height": tall + inner[3] - inner[1]})
        else:
            raise RuntimeError("the viewport is %dx%d, not %dx%d; the screen may be too small" % (inner[0], inner[1],
                                                                                                  width, tall))
        driver.call("POST", "/execute/async", {"script": READY, "args": []})
        import base64
        with open(path, "wb") as f:
            f.write(base64.b64decode(driver.call("GET", "/screenshot")))
        from PIL import Image
        with Image.open(path) as image:
            cut = image.convert("RGB").crop((0, 0, math.ceil(width * ratio), math.ceil(height * ratio)))
        cut.save(path)
        return True
    except (OSError, RuntimeError, ValueError) as error:
        print("  %s did not take its screenshot: %s" % (column, error), file=sys.stderr)
        if os.path.exists(path):
            os.remove(path)
        return False
    finally:
        if driver is not None:
            driver.close()


def cases_of(page):
    """Returns the page's cases in order, each with what it tests and what it measures."""
    import html.parser

    found = []

    class Parser(html.parser.HTMLParser):
        def handle_starttag(self, tag, attrs):
            a = dict(attrs)
            if tag == "figure" and "data-case" in a:
                found.append((a["data-case"], a.get("data-what", ""), a.get("data-measure", "")))

    with open(page, encoding="utf-8") as f:
        Parser().feed(f.read())
    return found


def scans_of(page):
    """Returns the page's scanlines in order, each as (case, axis, at, start, end) in CSS px."""
    import html.parser

    found = []

    class Parser(html.parser.HTMLParser):
        def handle_starttag(self, tag, attrs):
            a = dict(attrs)
            if tag != "figure" or "data-case" not in a or "data-scan" not in a:
                return
            for scan in a["data-scan"].split(";"):
                parts = scan.split()
                if len(parts) != 4 or parts[0] not in ("h", "v"):
                    sys.exit("%s: a scanline is `h <y> <x0> <x1>` or `v <x> <y0> <y1>`, not %r" % (a["data-case"],
                                                                                              scan))
                found.append((a["data-case"], parts[0], float(parts[1]), float(parts[2]), float(parts[3])))

    with open(page, encoding="utf-8") as f:
        Parser().feed(f.read())
    return found


def read_scan(image, ratio, axis, at, start, end):
    """Returns the runs of pixels that are not white along a scanline of `image`, which has `ratio` pixels per CSS px,
    as `<first>-<last>:<color>`, counted from the line's first pixel."""
    fixed = math.floor(at * ratio)
    first = math.floor(start * ratio)
    last = min(math.ceil(end * ratio), image.width if axis == "h" else image.height)
    runs = []
    for i in range(first, last):
        pixel = image.getpixel((i, fixed) if axis == "h" else (fixed, i))[:3]
        color = "%02x%02x%02x" % pixel
        index = i - first
        if color == "ffffff":
            continue
        if runs and runs[-1][1] == index - 1 and runs[-1][2] == color:
            runs[-1][1] = index
        else:
            runs.append([index, index, color])
    return " ".join("%d:%s" % (a, c) if a == b else "%d-%d:%s" % (a, b, c) for a, b, c in runs) or "white"


def print_scans(page, directory, names, ratio):
    """Prints each scanline of the page as each column's screenshot in `directory` paints it."""
    from PIL import Image

    images = {}
    for name in names:
        path = os.path.join(directory, name + ".png")
        if os.path.exists(path):
            images[name] = Image.open(path).convert("RGB")
    columns = [name for name in names if name in images]
    for case, axis, at, start, end in scans_of(page):
        print("%s  %s %g %g-%g" % (case, axis, at, start, end))
        first = None
        for name in columns:
            text = read_scan(images[name], ratio, axis, at, start, end)
            print("  %-2s %s" % (name, text if first is None or text != first else "same"))
            first = text if first is None else first


def cell(values):
    return ", ".join(str(v) for v in values) if values is not None else "-"


def main():
    parser = argparse.ArgumentParser(description="A probe page in the browsers and the engine.")
    parser.add_argument("page")
    parser.add_argument("--build", required=True, help="the build whose pr_shell and webkit_host run")
    parser.add_argument("--browser", action="append", help="a column to run: C, C1, S or F; every one by default")
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--json", action="store_true", help="print every column's results as JSON")
    parser.add_argument("--screenshot", metavar="DIRECTORY", help="write each column's rendering there instead")
    parser.add_argument("--safari", action="store_true",
                        help="run the S column in Safari itself, through safaridriver, in place of the build's host")
    parser.add_argument("--scheme", choices=["light", "dark"], default="dark",
                        help="the color scheme that the user prefers, which a screenshot shows the page for")
    parser.add_argument("--viewport", default=VIEWPORT, metavar="WxH",
                        help="the viewport the page is laid out in; suites/prax's pages are 600x800")
    parser.add_argument("--scan", action="store_true",
                        help="read the screenshots along the scanlines that the page's figures name")
    parser.add_argument("--keys", action="store_true", help="press the keys that the page asks for, as trusted input")
    parser.add_argument("--no-key-commands", action="store_true", help="with --keys, send Chrome no macOS commands")
    parser.add_argument("--pixel-ratio", type=float, metavar="PIXELS",
                        help="pixels per CSS pixel in every column, in place of the display's")
    parser.add_argument("--root", metavar="DIRECTORY",
                        help="the directory served at /, for a page that names its resources from the root; the "
                             "engine then loads the page from the server too, as it does with --query")
    parser.add_argument("--query", default="", metavar="QUERY",
                        help="the query of the page's URL, as fontFamily=NewComputerModern")
    options = parser.parse_args()
    page = os.path.abspath(options.page)
    if not os.path.exists(page):
        sys.exit("no page %s" % options.page)

    if options.pixel_ratio is not None and options.pixel_ratio <= 0:
        sys.exit("--pixel-ratio takes pixels per CSS pixel above 0")
    every = COLUMNS if options.pixel_ratio is None else columns_at(options.pixel_ratio)
    columns = [c for c in every if not options.browser or c[0] in options.browser]
    if options.safari:
        columns = [("S", "safari", []) if c[0] == "S" else c for c in columns]
    root = os.path.abspath(options.root) if options.root else common.ROOT
    server = browsers.Server(root)
    # A page under the root is served, so that its resources load as they do for every column, and one outside it is
    # opened as a file. The engine loads the page from the server, over sockets, if the root or the query is the
    # caller's, and otherwise opens its file, as the shell takes no query on a path.
    inside = not os.path.relpath(page, root).startswith("..")
    served = inside and bool(options.root or options.query)

    def page_url(*parameters):
        """Returns the page's URL, with the caller's query and `parameters`."""
        joined = "&".join(p for p in (options.query,) + parameters if p)
        query = "?" + joined if joined else ""
        if inside:
            return "http://127.0.0.1:%d%s%s" % (server.server_address[1], browsers.url_of(page, root), query)
        return "file://" + page + query

    engine_page = page_url() if served else page
    engine_network = ["--network"] if served else []
    if options.scan and options.pixel_ratio is None:
        sys.exit("--scan needs --pixel-ratio, so that every column has the same pixels per CSS px")
    if options.scan and not options.screenshot:
        options.screenshot = tempfile.mkdtemp(prefix="probe-scan-")
    if options.screenshot:
        os.makedirs(options.screenshot, exist_ok=True)
        url = page_url()
        for column, name, extra in columns:
            if not installed(name, options.build):
                continue
            path = os.path.join(os.path.abspath(options.screenshot), column + ".png")
            if screenshot(column, name, extra, url, path, options.timeout, options.build, options.viewport,
                          options.pixel_ratio, options.scheme) and not options.scan:
                print(path)
        path = os.path.join(os.path.abspath(options.screenshot), "P.png")
        shell = common.binary(options.build, "pr_shell")
        subprocess.run([shell, "--viewport", options.viewport] + shell_pixel_ratio(options.pixel_ratio)
                       + (["--dark"] if options.scheme == "dark" else []) + engine_network
                       + ["--screenshot", path, "--html", engine_page],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if options.scan:
            print_scans(page, options.screenshot, [c[0] for c in columns] + ["P"], options.pixel_ratio)
        elif os.path.exists(path):
            print(path)
        return 0
    if options.keys:
        # One Chrome column is enough for input, and the build's WebKit host takes none.
        columns = [c for c in columns if c[0] != "C1" and c[1] != "webkit"]
    # A page that asks for keys reports through its driver, so it needs no server, and may stand outside the root. Any
    # other posts its report to the server.
    if not options.keys and not inside:
        sys.exit("a page that reports its measures must be under the root that is served, %s" % root)
    url = page_url() if options.keys else page_url("post=/report")
    reports = {}
    versions = []
    for column, name, extra in columns:
        if not installed(name, options.build):
            print("  %s is not installed" % name, file=sys.stderr)
            continue
        print("  %s" % column, file=sys.stderr, flush=True)
        if options.keys:
            report = run_keys(name, url, options.timeout, options.build, not options.no_key_commands)
        else:
            report = run_browser(server, name, extra, url, options.timeout, options.build, options.pixel_ratio)
        if report is not None:
            reports[column] = report
            versions.append("%s %s at %s" % (name, version(name), report.get("dpr")))
    print("  P", file=sys.stderr, flush=True)
    engine = run_engine(engine_page, options.build, options.viewport, options.pixel_ratio, options.keys,
                        engine_network)
    if engine is not None:
        reports["P"] = engine

    if options.json:
        print(json.dumps(reports, indent=1))
        return 0
    names = [c[0] for c in columns if c[0] in reports] + (["P"] if "P" in reports else [])
    print("; ".join(versions))
    lines = []
    for case, what, measure in cases_of(page):
        first = None
        row = [case, measure]
        for name in names:
            values = reports[name]["cases"].get(case)
            text = cell(values)
            if first is None:
                first = text
                row.append(text)
            else:
                row.append("same" if text == first else text)
        lines.append(row)
    print(common.text_table(["case", "measures"] + names, lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
