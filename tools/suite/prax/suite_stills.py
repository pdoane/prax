#!/usr/bin/env python3
"""Captures stills of our pages in the browsers and the engine, and a sheet of each page's stills side by side, for
review.

Each page of the named directories, in their index's order, is captured at the suite's viewport of 600 by 800, as
`./dev.sh probe <page> --screenshot` captures it (tools/browsers/probe.py): in Chrome at a pixel ratio of one, WebKit in
the build's host, or Safari itself with --safari, Firefox, and the engine's pr_shell. What is captured is a copy of the
page whose moving figures, the animations of its `.pr-motion` stages, are paused, so that each shows the frame that its
caption describes in every column. A page's stills go to <out>/<directory>/<page>/<column>.png. Its sheet, which shows
the four stills at the same scale under their names, goes to <out>/<directory>/<page>.png. The browsers run several
pages at once.

A figure that shows printed output has a stage `<iframe class=pr-paged srcdoc="...">` that holds a whole document with
its own `@page` rules. A screen shows only the document's continuous layout, so each browser also prints the document:
headless Chrome to a PDF, WebKit in the build's host (webkit_host --pdf), even with --safari, since Safari's
WebDriver does not print, and Firefox over WebDriver BiDi
(navigable.print, on paper of the size that the document's first `@page` rule gives). The document is printed
from a copy under build/ whose `<base href>` is the page's directory on the server, so its relative URLs load as the
page's URLs do. pdftoppm renders each page of a PDF at 96 dots per inch, one dot per CSS px, to
<out>/<directory>/<page>/print-<figure>/<column>-<page>.png. The sheet adds a row for each such figure below the
screen columns, showing each browser's pages left to right at their size. The engine has no paged output yet, and its
cell says so. Without pdftoppm, the printed figures are skipped.

Usage: python3 -m tools.suite.prax.suite_stills --build <directory> --out <directory> <directory-of-the-suite>...
                                     [--file <page>]... [--page NAME]... [--browser C1|S|F]... [--print-only]
                                     [--jobs N] [--scheme light|dark]

Every column is captured for a user who prefers a dark color scheme, which the suite's pages follow, unless --scheme
light asks for the other.
"""

import argparse
import base64
import concurrent.futures
import glob
import html
import html.parser
import json
import os
import pathlib
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

from tools.bench import common
from tools.browsers import browsers
from tools.browsers import probe
from tools.browsers import websocket
from tools.suite.prax import suite

VIEWPORT = "600x800"
COLUMNS = [("C1", "chrome", ["--force-device-scale-factor=1"]), ("S", "webkit", []), ("F", "firefox", [])]
NAMES = {"C1": "Chrome", "S": "Safari", "F": "Firefox", "E": "Prax"}

# The directory that the documents of printed figures are written to for serving, under the checkout's ignored build
# directory.
PRINTS = os.path.join(common.ROOT, "build", "suite-prints")

# The directory that the frozen copies of the pages are written to for serving (`frozen`).
SCREENS = os.path.join(common.ROOT, "build", "suite-screens")

# The style sheet that holds a page's moving figures still for a capture (`frozen`). It comes first in the head, and
# wins over the page's own sheets by its importance.
FREEZE = "<style>.pr-motion, .pr-motion * { animation-play-state: paused !important; }</style>"

# The paper size that a document prints on if no `@page` rule gives a size: US Letter, in CSS px.
LETTER = (816, 1056)

# Matches the size that the first `@page` rule without a selector gives, in px, as in `@page { size: 240px 160px }`.
PAGE_SIZE = re.compile(r"@page\s*\{[^}]*?\bsize\s*:\s*([\d.]+)px\s+([\d.]+)px", re.S)


# ----------------------------------------------------------------------------------------------------------------------
# The screen


def frozen(page, base):
    """Returns the page with its moving figures held still: a `<base href>` of `base`, the URL of the page's directory,
    so that its relative URLs load as the page's own do, and a style sheet that pauses the animations of every
    `.pr-motion` stage. A page's figures move for its readers; a still shows the frame that the declared style or a
    negative animation-delay picks, which its caption describes, the same in every browser and the engine."""
    with open(page, encoding="utf-8") as f:
        document = f.read()
    return with_base(document, base, FREEZE)


def capture(server, build, columns, engine, page, out, timeout, scheme):
    """Writes the page's stills to `out` in the browsers' `columns`, and in the engine if `engine` is set, for a user
    who prefers the color `scheme`, light or dark, and returns the columns that were written. Each column captures a
    frozen copy of the page (`frozen`), written under build/: the browsers' is served, and its base is the page's
    directory on the server, and the engine's loads at its file: URL, and its base is the directory's file: URL."""
    os.makedirs(out, exist_ok=True)
    os.makedirs(SCREENS, exist_ok=True)
    work = tempfile.mkdtemp(prefix="screen-", dir=SCREENS)
    try:
        source = os.path.join(work, os.path.basename(page))
        with open(source, "w", encoding="utf-8", newline="\n") as f:
            f.write(frozen(page, browsers.url_of(os.path.dirname(page)) + "/"))
        url = "http://127.0.0.1:%d%s" % (server.server_address[1], browsers.url_of(source))
        written = []
        for column, name, extra in columns:
            path = os.path.join(out, column + ".png")
            if probe.screenshot(column, name, extra, url, path, timeout, build, VIEWPORT, None, scheme):
                written.append(column)
        path = os.path.join(out, "E.png")
        if engine:
            if os.path.exists(path):
                os.remove(path)
            local = os.path.join(work, "engine-" + os.path.basename(page))
            with open(local, "w", encoding="utf-8", newline="\n") as f:
                f.write(frozen(page, pathlib.Path(os.path.dirname(os.path.abspath(page))).as_uri() + "/"))
            dark = ["--dark"] if scheme == "dark" else []
            subprocess.run([common.binary(build, "pr_shell"), "--viewport", VIEWPORT] + dark
                           + ["--screenshot", path, "--html", local], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
            if os.path.exists(path):
                written.append("E")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return written


# ----------------------------------------------------------------------------------------------------------------------
# Printed output


def paged_documents(page):
    """Returns the documents of the page's `iframe.pr-paged` figures, in order, with their srcdoc unescaped."""
    found = []

    class Parser(html.parser.HTMLParser):
        def handle_starttag(self, tag, attrs):
            a = dict(attrs)
            if tag == "iframe" and "pr-paged" in (a.get("class") or "").split() and a.get("srcdoc") is not None:
                found.append(a["srcdoc"])

    # The parser unescapes attribute values itself.
    with open(page, encoding="utf-8") as f:
        Parser().feed(f.read())
    return found


def page_size(document):
    """Returns the document's page size in CSS px, from its `@page` rule, or US Letter."""
    match = PAGE_SIZE.search(document)
    return (float(match.group(1)), float(match.group(2))) if match else LETTER


def with_base(document, base, extra=""):
    """Returns the document with a `<base href>` of `base` first in its head, so that relative URLs resolve as they do
    in the page, followed by `extra`."""
    element = '<base href="%s">' % html.escape(base) + extra
    for pattern in (r"<head\b[^>]*>", r"<html\b[^>]*>", r"<!doctype[^>]*>"):
        match = re.search(pattern, document, re.I)
        if match:
            return document[:match.end()] + element + document[match.end():]
    return element + document


def print_chrome(build, url, pdf, size, timeout):
    """Prints `url` to `pdf` in headless Chrome, which sizes its paper from the document's `@page` rule."""
    browser = browsers.BROWSERS["chrome"]
    profile = tempfile.mkdtemp(prefix="print-chrome-")
    try:
        subprocess.run([browser.path(build), "--headless", "--user-data-dir=" + profile, "--no-pdf-header-footer",
                        "--print-to-pdf=" + pdf, url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=timeout)
    except subprocess.TimeoutExpired:
        pass
    finally:
        shutil.rmtree(profile, ignore_errors=True)


def print_webkit(build, url, pdf, size, timeout):
    """Prints `url` to `pdf` in the build's WebKit host, which sizes its paper from the document's `@page` rule."""
    try:
        subprocess.run([browsers.BROWSERS["webkit"].path(build), "--pdf", pdf, url], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=timeout)
    except subprocess.TimeoutExpired:
        pass


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def print_firefox(build, url, pdf, size, timeout):
    """Prints `url` to `pdf` in headless Firefox over WebDriver BiDi, on paper of `size` in CSS px, with backgrounds and
    without the paper's own margins. BiDi's print command takes the paper's size in centimeters."""
    browser = browsers.BROWSERS["firefox"]
    profile = tempfile.mkdtemp(prefix="print-firefox-")
    browser.profile(profile)
    port = free_port()
    process = subprocess.Popen([browser.path(build), "--headless", "-no-remote", "-profile", profile,
                                "--remote-debugging-port", str(port)], env=dict(os.environ, **browser.environment),
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    connection = None
    try:
        # Firefox listens only once it has started.
        deadline = time.monotonic() + timeout
        while connection is None:
            try:
                connection = websocket.connect("ws://127.0.0.1:%d/session" % port, timeout)
            except (OSError, websocket.Error):
                if time.monotonic() > deadline or process.poll() is not None:
                    raise
                time.sleep(0.25)
        calls = iter(range(1, 1 << 30))

        def call(method, params):
            identifier = next(calls)
            connection.send(json.dumps({"id": identifier, "method": method, "params": params}))
            while True:
                message = json.loads(connection.receive())
                if message.get("id") != identifier:
                    continue
                if message.get("type") == "error":
                    raise websocket.Error("%s: %s" % (method, message.get("message") or message.get("error")))
                return message["result"]

        call("session.new", {"capabilities": {}})
        context = call("navigable.getTree", {})["contexts"][0]["context"]
        call("navigable.navigate", {"context": context, "url": url, "wait": "complete"})
        call("script.evaluate", {"expression": "document.fonts.ready.then(() => new Promise(r => "
                                               "requestAnimationFrame(() => requestAnimationFrame(r))))",
                                 "target": {"context": context}, "awaitPromise": True})
        # A CSS px is 2.54 / 96 cm.
        result = call("navigable.print", {
            "context": context, "background": True, "shrinkToFit": False,
            "margin": {"top": 0, "bottom": 0, "left": 0, "right": 0},
            "page": {"width": size[0] * 2.54 / 96, "height": size[1] * 2.54 / 96}})
        with open(pdf, "wb") as f:
            f.write(base64.b64decode(result["data"]))
        call("session.end", {})
    except (OSError, websocket.Error, KeyError, ValueError) as error:
        print("  Firefox did not print %s: %s" % (url, error), file=sys.stderr)
    finally:
        if connection is not None:
            connection.close()
        process.terminate()
        try:
            process.wait(10)
        except subprocess.TimeoutExpired:
            process.kill()
        shutil.rmtree(profile, ignore_errors=True)


PRINTERS = {"chrome": print_chrome, "webkit": print_webkit, "safari": print_webkit, "firefox": print_firefox}


def capture_prints(server, build, columns, page, out, timeout):
    """Prints each `iframe.pr-paged` figure of the page in each browser, and renders the pages to PNGs under `out`.
    Returns, for each figure, its page size in px and the pages that each column printed, as {column: [path, ...]}."""
    documents = paged_documents(page)
    if not documents:
        return []
    base = "http://127.0.0.1:%d%s/" % (server.server_address[1], browsers.url_of(os.path.dirname(page)))
    os.makedirs(PRINTS, exist_ok=True)
    work = tempfile.mkdtemp(prefix="print-", dir=PRINTS)
    figures = []
    try:
        for index, document in enumerate(documents, 1):
            source = os.path.join(work, "figure-%d.html" % index)
            with open(source, "w", encoding="utf-8", newline="\n") as f:
                f.write(with_base(document, base))
            url = "http://127.0.0.1:%d%s" % (server.server_address[1], browsers.url_of(source))
            size = page_size(document)
            directory = os.path.join(out, "print-%d" % index)
            shutil.rmtree(directory, ignore_errors=True)
            os.makedirs(directory)
            pages = {}
            for column, name, _ in columns:
                pdf = os.path.join(work, "figure-%d-%s.pdf" % (index, column))
                PRINTERS[name](build, url, pdf, size, timeout)
                if not os.path.exists(pdf):
                    print("  %s printed nothing of figure %d of %s" % (NAMES[column], index, page), file=sys.stderr)
                    continue
                subprocess.run(["pdftoppm", "-r", "96", "-png", pdf, os.path.join(directory, column)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                # pdftoppm numbers the pages with as many digits as the last page's number needs.
                found = glob.glob(os.path.join(glob.escape(directory), column + "-*.png"))
                pages[column] = sorted(found, key=lambda p: int(p[p.rindex("-") + 1:-4]))
            figures.append((size, pages))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return figures


# ----------------------------------------------------------------------------------------------------------------------
# The sheet


def sheet(out, columns, figures, path):
    """Writes the sheet: the stills side by side at 600 by 800 each, under their names, and below them a row for each
    printed figure. Each row shows each browser's pages left to right at their own size, each under the browser's name
    and the page number."""
    from PIL import Image, ImageDraw
    width, height, label, gap, note = 600, 800, 24, 16, 240
    rows = []
    for size, pages in figures:
        images = {column: [Image.open(p).convert("RGB") for p in paths] for column, paths in pages.items()}
        tallest = max([i.height for each in images.values() for i in each] + [int(size[1])])
        across = gap + sum(sum(i.width + gap for i in each) + gap for each in images.values()) + note
        rows.append((images, label + tallest + 2 * label, across))
    across = max([width * len(columns)] + [r[2] for r in rows])
    screen = height + label if columns else 0
    image = Image.new("RGB", (across, screen + sum(r[1] for r in rows)), "white")
    draw = ImageDraw.Draw(image)
    for i, column in enumerate(columns):
        still = Image.open(os.path.join(out, column + ".png")).convert("RGB").resize((width, height))
        image.paste(still, (i * width, label))
        draw.text((i * width + 8, 6), NAMES[column], fill="black")
    y = screen
    for index, (images, row, _) in enumerate(rows, 1):
        draw.line([(0, y), (across, y)], fill="#888888")
        draw.text((8, y + 6), "Printed figure %d" % index, fill="black")
        x = gap
        for column, each in images.items():
            for number, page in enumerate(each, 1):
                draw.text((x, y + label + 6), "%s %d (%dx%d)" % (NAMES[column], number, page.width, page.height),
                          fill="black")
                image.paste(page, (x, y + 2 * label))
                draw.rectangle([x - 1, y + 2 * label - 1, x + page.width, y + 2 * label + page.height],
                               outline="#888888")
                x += page.width + gap
            x += gap
        draw.text((x, y + label + 6), "Prax: no paged output yet", fill="black")
        y += row
    image.save(path)


def main():
    parser = argparse.ArgumentParser(description="Stills of our pages in the browsers and the engine.")
    parser.add_argument("--build", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("directories", nargs="*")
    parser.add_argument("--file", action="append", default=[],
                        help="a page of the checkout outside the suite, captured as a page of it is")
    parser.add_argument("--page", action="append", help="only the pages of this name, as `float-rules.html`")
    parser.add_argument("--browser", action="append", choices=["C1", "S", "F", "E"],
                        help="a column to capture: C1, S, F or E for the engine; every one by default. The sheet keeps "
                             "the stills of the columns not captured from an earlier run")
    parser.add_argument("--print-only", action="store_true",
                        help="only the printed figures; the sheet keeps the screen stills of an earlier run")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--safari", action="store_true",
                        help="capture the S column in Safari itself, through safaridriver, in place of the build's host")
    parser.add_argument("--scheme", choices=["light", "dark"], default="dark",
                        help="the color scheme that the user prefers, which every column shows the pages for")
    parser.add_argument("--timeout", type=int, default=60)
    options = parser.parse_args()

    columns = [c for c in COLUMNS if (not options.browser or c[0] in options.browser)
               and probe.installed(c[1], options.build)]
    if options.safari:
        columns = [("S", "safari", []) if c[0] == "S" else c for c in columns]
    engine = not options.browser or "E" in options.browser
    pages = [(d, os.path.join(suite.SUITE, d, p)) for d, p in suite.pages_of(options.directories)
             if not options.page or p in options.page]
    pages += [("", os.path.abspath(f)) for f in options.file]
    if not pages:
        sys.exit("no pages")
    printing = shutil.which("pdftoppm") is not None
    if not printing:
        print("pdftoppm is missing (brew install poppler): the printed figures are skipped", file=sys.stderr)
    server = browsers.Server()

    def one(item):
        directory, page = item
        out = os.path.join(os.path.abspath(options.out), directory, os.path.basename(page)[:-5])
        os.makedirs(out, exist_ok=True)
        order = [c[0] for c in COLUMNS] + ["E"]
        if not options.print_only:
            capture(server, options.build, columns, engine, page, out, options.timeout, options.scheme)
        written = [c for c in order if os.path.exists(os.path.join(out, c + ".png"))]
        figures = capture_prints(server, options.build, columns, page, out, options.timeout) \
            if printing and columns else []
        sheet(out, written, figures, out + ".png")
        return out + ".png"

    with concurrent.futures.ThreadPoolExecutor(options.jobs) as pool:
        for path in pool.map(one, pages):
            print(path, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
