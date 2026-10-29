#!/usr/bin/env python3
"""Captures real pages, whole, into third_party/pages and third_party/pages-corpus, so that they load with no network.

The benchmarks of real pages read whatever is in third_party/pages, and `roll` moves it forward from three sources that
complement each other. New pages arrive, the oldest leave, and the set stays at the size that a run can afford. The
benchmarks therefore measure real markup that drifts. Nothing can be tuned to a set that changes, and two runs on
different days are never the same measurement. A page that does not respond, responds with something that is not HTML,
or is too small to measure is skipped.

A capture is everything that the page loads. Headless Chrome loads the page, scrolls it to its end so that what loads
lazily loads, and waits for its network to fall quiet. Every response that it received is kept as its bytes arrived,
before any decoding of their text: the document, its sheets, scripts, images, fonts, frames, and what its scripts
fetched. Then a sweep reads the markup and every sheet that was kept for the URLs that they name, and fetches each one
that Chrome did not: the candidates of a srcset that Chrome did not pick, the fonts of an @font-face that Chrome never
needed, a script that only an older browser runs. A sheet that was redirected is kept at the URL that the page names,
and the engine resolves its URLs against that one, so what the sheet names is kept at those URLs too. A resource too
large to keep, or that cannot be fetched, is recorded as omitted, with the reason.

The page's own markup is saved as <page>.html, and the resources under <page>.files/, each named for its type.
<page>.json records the document's URL, which is where the page's redirects ended, and its Content-Type, and for each
resource its URL, its file and its Content-Type. `pr_shell --html <page.json>`, the browser example and the invalidation
harness serve each resource from its file with its type. The browser side of the benchmarks reads the manifest beside
the pages, pages.json, because a page fetched over HTTP cannot list a directory.

Every page captured is also written to third_party/pages-corpus under the same name, so a page captured again replaces
its older copy there. That corpus only grows, and is a larger set of pages for a scan that can afford one. It is not a
history, and nothing measured against it is reproducible.

Usage:
    python3 -m tools.pages.pages roll [--if-empty]       take the day's pages, or with --if-empty only if there are none
    python3 -m tools.pages.pages refresh [--jobs N] [word...]
                                             capture again each page of both directories, or those with a word of
                                             the name, from its URL
    python3 -m tools.pages.pages check [word...]         the URLs that each capture's markup and sheets name and that it
    lacks
"""

import argparse
import base64
import concurrent.futures
import hashlib
import html.parser
import json
import os
import queue
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

import tools
from tools.browsers import browsers
from tools.browsers import websocket

ROOT = tools.ROOT
PAGES = os.path.join(ROOT, "third_party", "pages")
CORPUS = os.path.join(ROOT, "third_party", "pages-corpus")

# Hacker News is a bubble. Its front page links mostly to pages that a static site generator wrote, which are small,
# clean and text-heavy. Tranco is a ranking of the sites that people actually load, averaged over thirty days so that a
# single day's traffic cannot move it. Those sites return the other half of the web: pages that a framework rendered,
# with inline script and JSON and a DOM of thousands of nodes. Neither alone is the web, and the two together are
# closer than either.
HN_TOP = "https://hacker-news.firebaseio.com/v0/topstories.json"
HN_ITEM = "https://hacker-news.firebaseio.com/v0/item/%d.json"
TRANCO_ID = "https://tranco-list.eu/top-1m-id"
TRANCO_LIST = "https://tranco-list.eu/download/%s/%d"

# Both of those sources are mostly English, which is mostly ASCII. Text in other scripts takes two to four bytes per
# character, which is where decoding and validating are slowest. The Tranco sites on these countries' domains show what
# a framework renders in those languages, and are fetched again each time like the rest of Tranco.
#
# Wikipedia is not a source. Its articles list each image at several sizes and link the originals, and capturing every
# one of them runs into Wikimedia's limit on the requests of one address.
REGIONAL_DOMAINS = (".jp", ".cn", ".kr", ".ru", ".ua", ".tw", ".hk", ".vn", ".th", ".ir", ".il",
                    ".gr", ".in", ".tr", ".eg", ".sa")

# The sources are of two kinds, which roll forward differently.
#
# Tranco, and the regional sites drawn from it, are stable lists of sites whose pages change. Each keeps a fixed set of
# slots and fetches each one again every time: the same sites, with the current markup. A site that falls out of the
# top of the list stops being written and ages out.
#
# Hacker News is a churning list of pages that do not change, so they accumulate. New pages are fetched and the oldest
# are dropped. That is the rolling window.
#
# About half of the top domains are infrastructure with no page, so the walk covers more domains than it fetches. Few
# of the top domains are regional, so the regional walk is longer still.
SITES = 8
NEWS = 12
REGIONAL = 6
CANDIDATES = 40
REGIONAL_CANDIDATES = 3000

# The bounds on a page's document, in bytes.
MIN_BYTES = 20 * 1024
MAX_BYTES = 8 * 1024 * 1024

# The largest resource kept, in bytes, and the most that one page's resources may hold together. A video is usually
# what passes either.
MAX_RESOURCE_BYTES = 16 * 1024 * 1024
MAX_PAGE_BYTES = 96 * 1024 * 1024

# The seconds that a fetch may wait for a byte, that it may take in all, that a page may take to fire its load event,
# that its network may take to fall quiet after that, and that it must stay quiet to be done.
TIMEOUT = 20
FETCH_DEADLINE = 60
LOAD_TIMEOUT = 30
SETTLE_TIMEOUT = 20
QUIET = 1.5

# The times that a fetch is tried again when the server answers that it gets too many requests, the fetches that a
# page's sweep makes at once, and the fetches from one host at once, across every page being captured.
RETRIES = 6
SWEEP_FETCHES = 6
PER_HOST = 2

# The window that Chrome lays the page out in, which decides what loads lazily and which srcset candidate it picks.
WINDOW = (1280, 1024)


def user_agent():
    """Returns the user agent of the Chrome installed, without the "Headless" that some sites refuse. The sweep sends
    the same one, so that a site answers it as it answered Chrome."""
    version = browsers.BROWSERS["chrome"].version().split(".")[0] or "150"
    return ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/%s.0.0.0 Safari/537.36" % version)


AGENT = None
HOSTS = {}
HOSTS_LOCK = threading.Lock()
ACCEPT = "text/html,application/xhtml+xml,text/css,image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8"


def get(url, limit=MAX_BYTES, referer=None):
    """Returns the resource at `url`: its bytes, its content type, and its URL after redirects. Raises ValueError if it
    is larger than `limit`. A server that asks for fewer requests is asked again after the wait that it asks for, a few
    times. The request accepts what a browser accepts, because some servers refuse a request that accepts anything."""
    headers = {"User-Agent": AGENT, "Accept": ACCEPT}
    if referer:
        headers["Referer"] = referer
    request = urllib.request.Request(url, headers=headers)
    with HOSTS_LOCK:
        host = HOSTS.setdefault(urllib.parse.urlsplit(url).hostname, threading.Semaphore(PER_HOST))
    for attempt in range(RETRIES + 1):
        try:
            with host, urllib.request.urlopen(request, timeout=TIMEOUT) as response:
                deadline = time.monotonic() + FETCH_DEADLINE
                body = b""
                while True:
                    chunk = response.read(1 << 16)
                    if not chunk:
                        break
                    body += chunk
                    if len(body) > limit:
                        raise ValueError("larger than %d MB" % (limit >> 20))
                    if time.monotonic() > deadline:
                        raise ValueError("slower than %ds" % FETCH_DEADLINE)
                return body, response.headers.get("Content-Type", ""), response.geturl()
        except urllib.error.HTTPError as error:
            if error.code != 429 or attempt == RETRIES:
                raise
            wait = error.headers.get("Retry-After", "")
            time.sleep(min(float(wait) if wait.isdigit() else 2 << attempt, 60))


def name_for(source, url):
    """Returns a file name built from the source, the host and the path, so that the same page is not saved twice and a
    page's origin can be read from its name."""
    text = re.sub(r"^https?://(www\.)?", "", url)
    text = re.sub(r"[^A-Za-z0-9]+", "-", text).strip("-").lower()
    return "%s-%s.html" % (source, text[:56] or "page")


# ----------------------------------------------------------------------------------------------------------------------
# Sources


def hacker_news():
    """Returns the links on the Hacker News front page, in order."""
    ids, _, _ = get(HN_TOP)
    for story in json.loads(ids)[:CANDIDATES]:
        try:
            item, _, _ = get(HN_ITEM % story)
            url = json.loads(item).get("url")
        except Exception:
            continue
        if url and url.startswith("http"):
            yield url


def tranco(count=CANDIDATES, domains=None):
    """Returns the most loaded sites, most loaded first, or only those under `domains`. The list is named by the day it
    was built."""
    listing, _, _ = get(TRANCO_ID)
    rows, _, _ = get(TRANCO_LIST % (listing.decode().strip(), count))
    for row in rows.decode().strip().splitlines():
        _, _, domain = row.partition(",")
        if domain and (domains is None or domain.endswith(domains)):
            yield "https://" + domain + "/"


def regional():
    """Returns the regional sites. The most loaded site of each domain comes before the second site of any domain, so
    that the countries whose sites rank highest do not take every slot."""
    ranked = {domain: [] for domain in REGIONAL_DOMAINS}
    for url in tranco(REGIONAL_CANDIDATES, REGIONAL_DOMAINS):
        ranked[next(d for d in REGIONAL_DOMAINS if url.rstrip("/").endswith(d))].append(url)
    for depth in range(max(len(urls) for urls in ranked.values())):
        for urls in ranked.values():
            if depth < len(urls):
                yield urls[depth]


SOURCES = (("news", hacker_news, NEWS, False),
           ("tranco", tranco, SITES, True),
           ("regional", regional, REGIONAL, True))


# ----------------------------------------------------------------------------------------------------------------------
# What a page names


# The elements' attributes that name a resource that the page loads, by element. A link names one only for the
# relations in LINK_RELATIONS.
URL_ATTRIBUTES = {
    "script": ("src",), "img": ("src",), "source": ("src",), "video": ("src", "poster"), "audio": ("src",),
    "track": ("src",), "iframe": ("src",), "frame": ("src",), "embed": ("src",), "object": ("data",),
    "input": ("src",), "image": ("href", "xlink:href"), "feimage": ("href", "xlink:href"),
    "use": ("href", "xlink:href"), "body": ("background",), "table": ("background",), "td": ("background",),
    "th": ("background",),
}
SRCSET_ELEMENTS = ("img", "source")
LINK_RELATIONS = ("stylesheet", "preload", "modulepreload", "icon")

# An @import's URL, as url(...) or a string; a url() anywhere in CSS.
IMPORT = re.compile(r"""@import\s+(?:url\(\s*['"]?([^'")\s]+)['"]?\s*\)|['"]([^'"]+)['"])""", re.I)
URL = re.compile(r"""url\(\s*(?:"([^"]*)"|'([^']*)'|([^'")\s]+))\s*\)""", re.I)
COMMENT = re.compile(r"/\*.*?\*/", re.S)


class References(html.parser.HTMLParser):
    """Collects the URLs of the resources that a document names, as they are written, and its base URL. CSS in style
    elements and attributes is kept to be read for its own URLs."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.base = None
        self.urls = []
        self.css = []
        self.in_style = False

    def handle_starttag(self, tag, attrs):
        attributes = {name: value or "" for name, value in attrs}
        if tag == "base" and self.base is None and attributes.get("href"):
            self.base = attributes["href"]
        if tag == "link":
            rel = attributes.get("rel", "").lower().split()
            if "alternate" not in rel and any(r in rel for r in LINK_RELATIONS):
                if attributes.get("href"):
                    self.urls.append(attributes["href"])
                self.urls += srcset(attributes.get("imagesrcset", ""))
        for name in URL_ATTRIBUTES.get(tag, ()):
            value = attributes.get(name, "").strip()
            if value and not (tag == "use" and value.startswith("#")):
                self.urls.append(value)
        if tag in SRCSET_ELEMENTS:
            self.urls += srcset(attributes.get("srcset", ""))
        if attributes.get("style"):
            self.css.append(attributes["style"])
        if tag == "style":
            self.in_style = True
            self.css.append("")

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        if tag == "style":
            self.in_style = False

    def handle_endtag(self, tag):
        if tag == "style":
            self.in_style = False

    def handle_data(self, data):
        if self.in_style:
            self.css[-1] += data


def srcset(value):
    """Returns the URLs of a srcset's candidates, parsed as HTML parses them (html/#parse-a-srcset-attribute): a URL
    runs to whitespace, and a comma ends a candidate only after its URL, or at the URL's own end."""
    urls = []
    at = 0
    while at < len(value):
        while at < len(value) and (value[at].isspace() or value[at] == ","):
            at += 1
        start = at
        while at < len(value) and not value[at].isspace():
            at += 1
        url = value[start:at]
        if url.endswith(","):
            url = url.rstrip(",")
        else:
            depth = 0
            while at < len(value) and (value[at] != "," or depth):
                depth += {"(": 1, ")": -1}.get(value[at], 0)
                at += 1
        if url:
            urls.append(url)
    return urls


def css_references(text):
    """Returns the URLs that CSS names, in imports and in url()s, as they are written."""
    text = COMMENT.sub("", text)
    urls = [m.group(1) or m.group(2) for m in IMPORT.finditer(text)]
    urls += [(m.group(1) or m.group(2) or m.group(3) or "").strip() for m in URL.finditer(text)]
    return urls


def html_references(body):
    """Returns the base URL that a document's markup sets, if any, the URLs that it names, and its CSS."""
    parser = References()
    try:
        parser.feed(decoded(body))
        parser.close()
    except Exception:
        pass
    return parser.base, parser.urls, parser.css


def decoded(body):
    return body.decode("utf-8", errors="replace")


def absolute(base, reference):
    """Returns `reference` resolved against `base` without its fragment, or None if it names nothing to fetch."""
    # As a URL parser does, the reference loses its tabs and newlines and the spaces at its ends, and what remains that
    # a URL cannot hold is percent-encoded.
    reference = re.sub(r"[\t\n\r]", "", reference).strip(" \x00-\x1f")
    if not reference or reference.startswith(("data:", "#", "javascript:", "about:", "blob:", "mailto:")):
        return None
    try:
        url = urllib.parse.urldefrag(urllib.parse.urljoin(base, reference))[0]
    except ValueError:
        return None
    url = urllib.parse.quote(url, safe="%/:=&?~#+!$,;'@()*[]|^{}`")
    return url if url.startswith(("http://", "https://")) else None


def is_css(kind, url):
    return kind.split(";")[0].strip().lower() == "text/css" or urllib.parse.urlparse(url).path.endswith(".css")


def is_html(kind):
    return kind.split(";")[0].strip().lower() in ("text/html", "application/xhtml+xml")


def named(resources, page_url, body):
    """Yields what the document and each document and sheet among `resources` names, as pairs: the URL that the engine
    requests, which is resolved against the URL that the engine has for the resource that names it, and the URL to
    fetch it from, which is resolved against where that resource came from. They differ only under a sheet or a frame
    that was redirected. `resources` maps each URL to its record: its bytes, its content type, and the URL it came
    from."""
    documents = [(page_url, page_url, body)]
    sheets = []
    for url, (data, kind, source) in list(resources.items()):
        if is_css(kind, url):
            sheets.append((url, source, data))
        elif is_html(kind):
            documents.append((url, source, data))
    for url, source, data in documents:
        base, urls, css = html_references(data)
        if base:
            url, source = urllib.parse.urljoin(url, base), urllib.parse.urljoin(source, base)
        for reference in urls + [r for text in css for r in css_references(text)]:
            yield absolute(url, reference), absolute(source, reference)
    for url, source, data in sheets:
        for reference in css_references(decoded(data)):
            yield absolute(url, reference), absolute(source, reference)


# ----------------------------------------------------------------------------------------------------------------------
# Chrome


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class DevTools:
    """A connection to a page of Chrome over the DevTools protocol. A thread reads the messages as they come. `call`
    waits for its answer, and meanwhile hands each event to `on_event` and each other answer to its callback, so that a
    handler may send without waiting."""

    def __init__(self, url):
        self.connection = websocket.connect(url, TIMEOUT)
        self.connection.sock.settimeout(None)
        self.messages = queue.Queue()
        self.ids = iter(range(1, 1 << 30))
        self.callbacks = {}
        self.on_event = lambda method, params: None
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        try:
            while True:
                self.messages.put(json.loads(self.connection.receive()))
        except (OSError, websocket.Error, ValueError):
            self.messages.put(None)

    def send(self, method, params=None, callback=None):
        identifier = next(self.ids)
        if callback is not None:
            self.callbacks[identifier] = callback
        self.connection.send(json.dumps({"id": identifier, "method": method, "params": params or {}}))
        return identifier

    def dispatch(self, message):
        if message is None:
            raise websocket.Error("Chrome closed the connection")
        if "method" in message:
            self.on_event(message["method"], message.get("params", {}))
        elif message.get("id") in self.callbacks:
            self.callbacks.pop(message["id"])(message.get("result"), message.get("error"))

    def pump(self, seconds):
        """Handles the messages that arrive within `seconds`."""
        deadline = time.monotonic() + seconds
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return
            try:
                message = self.messages.get(timeout=left)
            except queue.Empty:
                return
            self.dispatch(message)

    def call(self, method, params=None, timeout=TIMEOUT):
        answer = []
        self.send(method, params, lambda result, error: answer.append((result, error)))
        deadline = time.monotonic() + timeout
        while not answer:
            left = deadline - time.monotonic()
            if left <= 0:
                raise websocket.Error("%s did not answer within %ds" % (method, timeout))
            try:
                self.dispatch(self.messages.get(timeout=left))
            except queue.Empty:
                pass
        result, error = answer[0]
        if error:
            raise websocket.Error("%s: %s" % (method, error.get("message")))
        return result

    def close(self):
        self.connection.close()


class Chrome:
    """Headless Chrome in a fresh profile, with every frame of a page in one process, so that one connection sees all of
    its requests."""

    def __init__(self):
        browser = browsers.BROWSERS["chrome"]
        if not browser.installed(None):
            sys.exit("pages are captured with Chrome, which is not installed at %s" % browser.app)
        self.profile = tempfile.mkdtemp(prefix="pages-chrome-")
        self.port = free_port()
        self.process = subprocess.Popen(
            [browser.path(None), "--headless", "--remote-debugging-port=%d" % self.port,
             "--remote-allow-origins=*", "--mute-audio", "--hide-scrollbars",
             "--disable-features=IsolateOrigins,site-per-process,HttpsUpgrades",
             "--window-size=%d,%d" % WINDOW, "--user-agent=" + AGENT] + browser.arguments(self.profile)
            + ["about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def endpoint(self, path, method="GET"):
        deadline = time.monotonic() + TIMEOUT
        while True:
            try:
                request = urllib.request.Request("http://127.0.0.1:%d%s" % (self.port, path), method=method)
                with urllib.request.urlopen(request, timeout=TIMEOUT) as response:
                    return json.loads(response.read())
            except (OSError, ValueError):
                if time.monotonic() > deadline or self.process.poll() is not None:
                    raise
                time.sleep(0.2)

    def close(self):
        self.process.terminate()
        try:
            self.process.wait(10)
        except subprocess.TimeoutExpired:
            self.process.kill()
        shutil.rmtree(self.profile, ignore_errors=True)


# Scrolls the page to its end, a window at a time, so that what loads lazily as it comes into view loads, and back.
SCROLL = """new Promise(done => {
    let y = 0, steps = 0;
    const step = () => {
        const end = document.scrollingElement ? document.scrollingElement.scrollHeight : 0;
        if (y >= end || ++steps > 200) { scrollTo(0, 0); setTimeout(done, 200); return; }
        y += innerHeight * 0.8;
        scrollTo(0, y);
        setTimeout(step, 60);
    };
    step();
})"""


class Recording:
    """The responses that one page load received, as their bytes arrived. Each URL that a request was redirected from
    is an alias of the URL that it ended at."""

    def __init__(self, tools, frame):
        self.tools = tools
        self.frame = frame
        self.document = None            # (URL, bytes, content type, status) of the frame's document
        self.responses = {}             # URL to (bytes, content type)
        self.aliases = {}               # URL redirected from, to the URL redirected to
        self.omitted = {}               # URL to the reason it was not kept
        self.pending = set()            # the requests in flight
        self.bodies = 0                 # the bodies being read
        self.size = 0
        self.last = time.monotonic()    # when the network last did anything

    def event(self, method, params):
        if method == "Network.requestWillBeSent":
            self.pending.add(params["requestId"])
            self.last = time.monotonic()
        elif method in ("Network.loadingFinished", "Network.loadingFailed"):
            self.pending.discard(params["requestId"])
            self.last = time.monotonic()
        elif method == "Fetch.requestPaused":
            self.paused(params)

    def paused(self, params):
        identifier = params["requestId"]
        request = params["request"]
        url = urllib.parse.urldefrag(request["url"])[0]
        status = params.get("responseStatusCode", 0)
        headers = {h["name"].lower(): h["value"] for h in params.get("responseHeaders", [])}
        kind = headers.get("content-type", "")
        is_document = params.get("resourceType") == "Document" and params.get("frameId") == self.frame
        resume = lambda *_: self.tools.send("Fetch.continueRequest", {"requestId": identifier})  # noqa: E731
        if 300 <= status < 400 and "location" in headers:
            self.aliases[url] = urllib.parse.urldefrag(urllib.parse.urljoin(url, headers["location"]))[0]
            return resume()
        if request.get("method") != "GET" or not url.startswith("http") or not 200 <= status < 300 or status in (
                204, 206) or params.get("resourceType") == "Media":
            # A range of a video, a post, or an error: the sweep fetches what the page names whole.
            if is_document and self.document is None:
                self.document = (url, b"", kind, status)
            return resume()
        length = int(headers.get("content-length", "0") or 0)
        limit = MAX_BYTES if is_document else MAX_RESOURCE_BYTES
        if length > limit:
            self.omitted[url] = "%d MB" % (length >> 20)
            return resume()

        def received(result, error):
            self.bodies -= 1
            self.last = time.monotonic()
            if result is not None:
                data = base64.b64decode(result["body"]) if result.get("base64Encoded") else result["body"].encode()
                if is_document and self.document is None:
                    self.document = (url, data, kind, status)
                elif len(data) > limit or self.size + len(data) > MAX_PAGE_BYTES:
                    self.omitted[url] = "%d KB" % (len(data) >> 10)
                else:
                    self.responses[url] = (data, kind)
                    self.size += len(data)
            resume()

        self.bodies += 1
        self.tools.send("Fetch.getResponseBody", {"requestId": identifier}, received)

    def quiet(self):
        return not self.pending and not self.bodies and time.monotonic() - self.last > QUIET


def record(url):
    """Loads `url` in headless Chrome and returns its Recording."""
    chrome = Chrome()
    tools = None
    try:
        target = chrome.endpoint("/json/new?about:blank", "PUT")
        tools = DevTools(target["webSocketDebuggerUrl"])
        frame = tools.call("Page.getFrameTree")["frameTree"]["frame"]["id"]
        recording = Recording(tools, frame)
        loaded = []
        tools.on_event = lambda method, params: (recording.event(method, params),
                                                 method == "Page.loadEventFired" and loaded.append(True))
        tools.call("Network.enable")
        tools.call("Network.setCacheDisabled", {"cacheDisabled": True})
        tools.call("Network.setBypassServiceWorker", {"bypass": True})
        tools.call("Page.enable")
        tools.call("Fetch.enable", {"patterns": [{"urlPattern": "*", "requestStage": "Response"}]})
        navigated = tools.call("Page.navigate", {"url": url}, LOAD_TIMEOUT)
        if navigated.get("errorText"):
            raise websocket.Error(navigated["errorText"])
        deadline = time.monotonic() + LOAD_TIMEOUT
        while not loaded and time.monotonic() < deadline:
            tools.pump(0.25)
        try:
            tools.call("Runtime.evaluate", {"expression": SCROLL, "awaitPromise": True}, SETTLE_TIMEOUT)
        except websocket.Error:
            pass
        deadline = time.monotonic() + SETTLE_TIMEOUT
        while not recording.quiet() and time.monotonic() < deadline:
            tools.pump(0.25)
        # What is still on its way is not waited for: a page that streams or polls never falls quiet.
        tools.call("Fetch.disable")
        return recording
    finally:
        if tools is not None:
            tools.close()
        chrome.close()


# ----------------------------------------------------------------------------------------------------------------------
# Capturing


class Capture:
    """A page and everything it loads: the URL of its document, its markup, and each resource by the URL that the engine
    requests, as its bytes, its content type and the URL it came from. `omitted` gives the reason that each URL left
    out was left out."""

    def __init__(self, url, body, kind, resources, omitted):
        self.url = url
        self.body = body
        self.kind = kind
        self.resources = resources
        self.omitted = omitted


def capture(url, fetch=None, recorder=record):
    """Captures the page at `url`, or returns the reason it cannot be captured. The sweep fetches with `fetch`, or
    else as the page would, with the page as the referrer."""
    try:
        recording = recorder(url)
    except (OSError, websocket.Error, ValueError, KeyError) as error:
        return str(error)
    if recording.document is None:
        return "no document"
    page_url, body, kind, status = recording.document
    if not 200 <= status < 300:
        return "status %d" % status
    if not is_html(kind) and kind:
        return kind
    if not MIN_BYTES <= len(body) <= MAX_BYTES:
        return "%d bytes" % len(body)
    resources = {u: (data, kind, u) for u, (data, kind) in recording.responses.items()}
    # The URLs that were redirected serve what they were redirected to.
    for source in recording.aliases:
        target = source
        for _ in range(20):
            target = recording.aliases.get(target, target)
            if target not in recording.aliases:
                break
        if target in resources and source not in resources and source != page_url:
            resources[source] = resources[target][:2] + (target,)
    omitted = dict(recording.omitted)
    sweep(resources, omitted, page_url, body, fetch or (lambda u, limit: get(u, limit, page_url)))
    return Capture(page_url, body, kind, resources, omitted)


def sweep(resources, omitted, page_url, body, fetch=get):
    """Fetches what the document and the documents and sheets among `resources` name and do not hold, until they name
    nothing new. A URL that the engine resolves differently from where the resource came from serves what was fetched
    from where it came from."""
    size = sum(len(data) for data, _, _ in resources.values())
    fetched = {}

    def attempt(url):
        try:
            return fetch(url, MAX_RESOURCE_BYTES)
        except (urllib.error.URLError, OSError, ValueError, http_errors()) as error:
            return str(error)[:80] or type(error).__name__

    with concurrent.futures.ThreadPoolExecutor(SWEEP_FETCHES) as pool:
        while True:
            wanted = {}
            for engine_url, source_url in named(resources, page_url, body):
                if engine_url is None or engine_url in resources or engine_url in omitted or engine_url == page_url:
                    continue
                wanted.setdefault(engine_url, source_url or engine_url)
            if not wanted:
                return
            sources = sorted({u for u in wanted.values() if u not in resources and u not in fetched})
            fetched.update(zip(sources, pool.map(attempt, sources)))
            for engine_url, source_url in wanted.items():
                result = resources.get(source_url) or fetched[source_url]
                if isinstance(result, str):
                    omitted[engine_url] = result
                    continue
                data, kind, final = result
                if source_url not in resources:
                    if size + len(data) > MAX_PAGE_BYTES:
                        omitted[engine_url] = "%d KB, past the page's limit" % (len(data) >> 10)
                        continue
                    size += len(data)
                resources[engine_url] = (data, kind, final)


def http_errors():
    import http.client
    return http.client.HTTPException


# The extension of a resource's file, by its content type. The engine's hosts read a file's type from the capture, and
# the extension is for the developer who opens the directory.
EXTENSIONS = {
    "text/css": "css", "text/html": "html", "application/xhtml+xml": "xhtml", "text/javascript": "js",
    "application/javascript": "js", "application/x-javascript": "js", "application/json": "json",
    "application/manifest+json": "json", "text/plain": "txt", "text/xml": "xml", "application/xml": "xml",
    "image/svg+xml": "svg", "image/png": "png", "image/jpeg": "jpg", "image/gif": "gif", "image/webp": "webp",
    "image/avif": "avif", "image/x-icon": "ico", "image/vnd.microsoft.icon": "ico", "image/apng": "png",
    "font/woff2": "woff2", "font/woff": "woff", "font/ttf": "ttf", "font/otf": "otf", "font/collection": "ttc",
    "application/font-woff": "woff", "application/font-woff2": "woff2", "application/x-font-woff": "woff",
    "application/x-font-ttf": "ttf", "font/sfnt": "ttf", "application/font-sfnt": "ttf",
    "application/vnd.ms-opentype": "otf", "video/mp4": "mp4", "video/webm": "webm", "audio/mpeg": "mp3",
    "audio/mp4": "m4a", "application/wasm": "wasm",
}


def extension(url, kind):
    """Returns the extension for a resource's file: the extension for its content type, else its URL's extension, else
    `bin`."""
    by_type = EXTENSIONS.get(kind.split(";")[0].strip().lower())
    if by_type:
        return by_type
    last = urllib.parse.urlparse(url).path.rsplit("/", 1)[-1]
    ext = last.rsplit(".", 1)[-1].lower() if "." in last else ""
    return ext if re.fullmatch(r"[a-z0-9]{1,5}", ext) else "bin"


def save(directories, name, page):
    """Writes the page, the resources that it loads, and the record that maps their URLs to files, to the first of
    `directories`, and links each file into the others. Resources with the same bytes share a file."""
    stem = name[:-len(".html")]
    first = directories[0]
    files = {}
    resources = []
    for url in sorted(page.resources):
        data, kind, origin = page.resources[url]
        digest = hashlib.sha1(data).hexdigest()
        if digest not in files:
            files[digest] = ("%s.files/%d.%s" % (stem, len(files), extension(origin, kind)), data)
        resources.append({"url": url, "file": files[digest][0], "type": kind.strip()})
    record = {"url": page.url, "type": page.kind.strip(), "resources": resources,
              "omitted": [{"url": u, "reason": r} for u, r in sorted(page.omitted.items())]}
    written = [(name, page.body), (stem + ".json", json.dumps(record, indent=1).encode())] + list(files.values())
    for directory in directories:
        os.makedirs(directory, exist_ok=True)
        shutil.rmtree(os.path.join(directory, stem + ".files"), ignore_errors=True)
        if files:
            os.makedirs(os.path.join(directory, stem + ".files"))
        for path, data in written:
            target = os.path.join(directory, path)
            if directory == first:
                with open(target + ".partial", "wb") as f:
                    f.write(data)
                os.replace(target + ".partial", target)
            else:
                if os.path.exists(target):
                    os.remove(target)
                os.link(os.path.join(first, path), target)


def take(name, url, directories):
    """Captures the page at `url` and writes it as `name` to each of `directories`. Returns false, saying why, if it
    cannot be captured."""
    page = capture(url)
    if isinstance(page, str):
        print("  skipped %s: %s" % (url, page[:70]), flush=True)
        return False
    save(directories, name, page)
    size = sum(len(data) for data, _, _ in {id(r[0]): r for r in page.resources.values()}.values())
    print("  took %s (%d KB, %d resources, %d MB%s)" % (
        name, len(page.body) >> 10, len(page.resources), size >> 20,
        ", %d omitted" % len(page.omitted) if page.omitted else ""), flush=True)
    return True


# ----------------------------------------------------------------------------------------------------------------------
# Commands


def prune():
    """Limits each source to its own count of pages, keeping the newest, so that a source that churns cannot crowd out
    one that does not. A page from no source is left over from an older naming scheme, and is also removed."""
    prefixes = tuple(source + "-" for source, _, _, _ in SOURCES)
    for source, keep in [(source, count) for source, _, count, _ in SOURCES] + [(None, 0)]:
        entries = sorted(((os.path.getmtime(os.path.join(PAGES, name)), name)
                          for name in os.listdir(PAGES)
                          if name.endswith(".html")
                          and (name.startswith(source + "-") if source
                               else not name.startswith(prefixes))),
                         reverse=True)
        for _, name in entries[keep:]:
            print("  dropped %s" % name)
            remove(PAGES, name)


def remove(directory, name):
    stem = os.path.join(directory, name[:-len(".html")])
    os.remove(stem + ".html")
    shutil.rmtree(stem + ".files", ignore_errors=True)
    if os.path.exists(stem + ".json"):
        os.remove(stem + ".json")


def manifest():
    """Writes the manifest of the pages on disk and returns their names. The browser side fetches this list."""
    names = sorted(n for n in os.listdir(PAGES) if n.endswith(".html"))
    with open(os.path.join(PAGES, "pages.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(names, f, indent=2)
    return names


def roll(if_empty):
    os.makedirs(PAGES, exist_ok=True)
    if if_empty and manifest():
        return 0
    for source, urls, wanted, again in SOURCES:
        print("%s" % source)
        taken = 0
        try:
            for url in urls():
                if taken >= wanted:
                    break
                name = name_for(source, url)
                if os.path.exists(os.path.join(PAGES, name)) and not again:
                    continue
                taken += take(name, url, (PAGES, CORPUS))
        except Exception as error:
            print("  unreachable: %s" % str(error)[:70])
    prune()
    names = manifest()
    if not names:
        sys.exit("no pages under %s" % PAGES)
    print("%d pages" % len(names))
    return 0


def captures(words):
    """Returns each page of both directories with a word of `words` in its name, or every page, by name: the URL that it
    was captured from and the directories that hold it."""
    found = {}
    for directory in (PAGES, CORPUS):
        if not os.path.isdir(directory):
            continue
        for name in sorted(os.listdir(directory)):
            if not name.endswith(".html") or (words and not any(w in name for w in words)):
                continue
            found.setdefault(name, [None, []])[1].append(directory)
            if found[name][0] is None:
                found[name][0] = captured_url(directory, name)
    return found


class Canonical(html.parser.HTMLParser):
    """Finds the URL that a document names as its own: its canonical link, or else its og:url."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.canonical = None
        self.og = None

    def handle_starttag(self, tag, attrs):
        attributes = {name: value or "" for name, value in attrs}
        if tag == "link" and "canonical" in attributes.get("rel", "").lower().split() and self.canonical is None:
            self.canonical = attributes.get("href")
        if tag == "meta" and attributes.get("property") == "og:url" and self.og is None:
            self.og = attributes.get("content")


def captured_url(directory, name):
    """Returns the URL that the page `name` was captured from, as its record gives it, or else as its markup names
    itself, which a page saved before records were kept needs. Returns None if neither does."""
    stem = os.path.join(directory, name[:-len(".html")])
    try:
        with open(stem + ".json", encoding="utf-8") as f:
            return json.load(f)["url"]
    except (OSError, ValueError, KeyError):
        pass
    parser = Canonical()
    try:
        with open(stem + ".html", "rb") as f:
            parser.feed(decoded(f.read()))
    except Exception:
        pass
    url = parser.canonical or parser.og
    return url if url and url.startswith(("http://", "https://")) else None


def refresh(words, jobs):
    """Captures each page again from its URL. A page that can no longer be captured is removed, because a page that does
    not load whole is not one that a benchmark can read."""
    pages = captures(words)
    print("capturing %d pages again, %d at a time" % (len(pages), jobs))

    def again(item):
        name, (url, directories) = item
        if url is None:
            print("  skipped %s: no URL in a record or in its markup" % name, flush=True)
            return name, directories, False
        return name, directories, take(name, url, directories)

    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        results = list(pool.map(again, pages.items()))
    for name, directories, ok in results:
        if not ok:
            for directory in directories:
                print("  removed %s" % os.path.join(os.path.basename(directory), name))
                remove(directory, name)
    if os.path.isdir(PAGES):
        print("%d pages, %d in the corpus" % (len(manifest()), len(captures([]))))
    return 0


def check(words):
    """Prints the URLs that each capture's markup and sheets name, that it holds no file for and that it did not record
    as omitted. Returns false if any capture lacks one."""
    complete = True
    for directory in (PAGES, CORPUS):
        if not os.path.isdir(directory):
            continue
        for name in sorted(os.listdir(directory)):
            if not name.endswith(".html") or (words and not any(w in name for w in words)):
                continue
            stem = os.path.join(directory, name[:-len(".html")])
            try:
                with open(stem + ".json", encoding="utf-8") as f:
                    record = json.load(f)
                with open(stem + ".html", "rb") as f:
                    body = f.read()
            except (OSError, ValueError):
                print("%s: no capture record" % os.path.relpath(stem, ROOT))
                complete = False
                continue
            resources = {}
            for r in record.get("resources", []):
                path = os.path.join(directory, r["file"])
                if not os.path.exists(path):
                    print("%s: no file %s" % (os.path.relpath(stem, ROOT), r["file"]))
                    complete = False
                    continue
                with open(path, "rb") as f:
                    resources[r["url"]] = (f.read(), r.get("type", ""), r["url"])
            omitted = {o["url"] for o in record.get("omitted", [])}
            missing = sorted({engine_url for engine_url, _ in named(resources, record["url"], body)
                              if engine_url and engine_url not in resources and engine_url not in omitted
                              and engine_url != record["url"]})
            if missing:
                complete = False
                print("%s: %d missing" % (os.path.relpath(stem, ROOT), len(missing)))
                for url in missing[:5]:
                    print("    %s" % url)
    return complete


def main():
    global AGENT
    ap = argparse.ArgumentParser()
    commands = ap.add_subparsers(dest="command", required=True)
    rolled = commands.add_parser("roll", help="take the day's pages and drop the oldest")
    rolled.add_argument("--if-empty", action="store_true", help="take pages only if there are none, as a benchmark does")
    refreshed = commands.add_parser("refresh", help="capture each page again from its URL")
    refreshed.add_argument("--jobs", type=int, default=4)
    refreshed.add_argument("words", nargs="*")
    checked = commands.add_parser("check", help="the URLs that captures name and lack")
    checked.add_argument("words", nargs="*")
    args = ap.parse_args()

    if args.command == "check":
        return 0 if check(args.words) else 1
    AGENT = user_agent()
    if args.command == "roll":
        return roll(args.if_empty)
    return refresh(args.words, args.jobs)


if __name__ == "__main__":
    sys.exit(main())
