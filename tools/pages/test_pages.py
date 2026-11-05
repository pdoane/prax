#!/usr/bin/env python3
"""Tests what a capture holds: what Chrome loaded, kept at every URL that redirected to it, and what the sweep fetched
of what the markup and the sheets name, at the URLs that the engine requests.

Usage: python3 -m tools.pages.test_pages
"""

import json
import os
import tempfile
import types
import unittest

from tools.pages import pages


class Web:
    """Answers fetches from `resources`, by URL. Each resource is its bytes, its content type, and the URL it redirects
    to, if any. Records the URLs fetched."""

    def __init__(self, resources):
        self.resources = resources
        self.fetched = []

    def __call__(self, url, limit):
        self.fetched.append(url)
        if url not in self.resources:
            raise OSError("not found")
        body, kind, final = self.resources[url]
        if len(body) > limit:
            raise ValueError("too large")
        return body, kind, final or url


PAGE_URL = "https://site.test/docs/index.html"

PAGE = b"""<!doctype html>
<base href="https://site.test/docs/">
<link rel=stylesheet href="old.css">
<link rel=preload as=image href="hero.avif" imagesrcset="hero-1x.avif 1x, hero-2x.avif 2x">
<link rel=alternate stylesheet href="alternate.css">
<a href="elsewhere.html">a link is not loaded</a>
<script src="app.js"></script>
<script nomodule src="legacy.js"></script>
<img src="small.png" srcset="small.png 400w, large.png 800w" style="background: url('inline-bg.png')">
<img src="data:image/png;base64,AAAA">
<svg><use href="#local"/><image href="figure.svg"/></svg>
<video poster="poster.jpg"></video>
<iframe src="frame.html"></iframe>
<style>
@import url(imported.css);
/* url(commented.png) */
@font-face { font-family: Inline; src: url(fonts/inline.ttf) format("truetype"); }
@font-face { font-family: Data; src: url(data:font/ttf;base64,AAAA); }
</style>
"""

# What Chrome loaded: the linked sheet, which redirected to another directory, the script that it ran, and the srcset
# candidate that it picked.
RECORDED = {
    "https://site.test/css/v2/sheet.css": (
        b"@import 'child.css'; .a { background: url(../img/bg.png) }"
        b"@font-face { font-family: Linked; src: url('../fonts/linked.woff2'), url(\"shared.woff\"); }",
        "text/css; charset=utf-8"),
    "https://site.test/docs/app.js": (b"run()", "text/javascript"),
    "https://site.test/docs/small.png": (b"small", "image/png"),
}
ALIASES = {"https://site.test/docs/old.css": "https://site.test/css/v2/sheet.css"}

# What the sweep may fetch.
WEB = {
    "https://site.test/css/v2/child.css": (b"@font-face { font-family: Child; src: url(child.ttf); }", "text/css",
                                           None),
    "https://site.test/css/v2/child.ttf": (b"child", "font/ttf", None),
    "https://site.test/css/img/bg.png": (b"bg", "image/png", None),
    "https://site.test/css/fonts/linked.woff2": (b"linked", "font/woff2", None),
    "https://site.test/css/v2/shared.woff": (b"shared", "font/woff", None),
    "https://site.test/docs/imported.css": (b".b { background: url(imported-bg.png) }", "text/css", None),
    "https://site.test/docs/imported-bg.png": (b"imported-bg", "image/png", None),
    "https://site.test/docs/fonts/inline.ttf": (b"inline", "application/octet-stream", None),
    "https://site.test/docs/hero.avif": (b"hero", "image/avif", None),
    "https://site.test/docs/hero-1x.avif": (b"hero-1x", "image/avif", None),
    "https://site.test/docs/hero-2x.avif": (b"hero-2x", "image/avif", None),
    "https://site.test/docs/large.png": (b"large", "image/png", None),
    "https://site.test/docs/inline-bg.png": (b"inline-bg", "image/png", None),
    "https://site.test/docs/figure.svg": (b"<svg/>", "image/svg+xml", None),
    "https://site.test/docs/poster.jpg": (b"poster", "image/jpeg", None),
    "https://site.test/docs/frame.html": (b"<img src=framed.gif>", "text/html", "https://other.test/frame/"),
    "https://other.test/frame/framed.gif": (b"framed", "image/gif", None),
    "https://site.test/docs/legacy.js": (b"x" * (pages.MAX_RESOURCE_BYTES + 1), "text/javascript", None),
}


def recorder(url):
    return types.SimpleNamespace(document=(url, PAGE, "text/html; charset=utf-8", 200), responses=dict(RECORDED),
                                 aliases=dict(ALIASES), omitted={})


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.web = Web(WEB)
        self.page = pages.capture(PAGE_URL, self.web, recorder)
        self.by_url = {url: data for url, (data, _, _) in self.page.resources.items()}

    def test_what_chrome_loaded_is_kept_and_served_at_the_url_it_was_redirected_from(self):
        self.assertEqual(self.by_url["https://site.test/docs/app.js"], b"run()")
        self.assertEqual(self.by_url["https://site.test/docs/old.css"],
                         self.by_url["https://site.test/css/v2/sheet.css"])
        self.assertNotIn("https://site.test/docs/app.js", self.web.fetched)

    def test_what_the_markup_names_and_chrome_did_not_load_is_fetched(self):
        for name in ("hero.avif", "hero-1x.avif", "hero-2x.avif", "large.png", "inline-bg.png", "figure.svg",
                     "poster.jpg", "frame.html", "imported.css", "imported-bg.png", "fonts/inline.ttf"):
            self.assertIn("https://site.test/docs/" + name, self.by_url, name)

    def test_links_that_load_nothing_and_data_urls_are_not_fetched(self):
        for url in self.web.fetched:
            self.assertFalse(url.endswith(("elsewhere.html", "alternate.css", "commented.png")), url)
            self.assertFalse(url.startswith("data:"))

    def test_a_redirected_sheets_urls_are_fetched_from_where_it_went_and_served_where_the_engine_resolves_them(self):
        # The engine resolves the sheet's URLs against the URL that the page names, which is where the capture serves
        # the sheet. Chrome resolved them against where the sheet went.
        self.assertEqual(self.by_url["https://site.test/fonts/linked.woff2"], b"linked")
        self.assertEqual(self.by_url["https://site.test/css/fonts/linked.woff2"], b"linked")
        self.assertEqual(self.by_url["https://site.test/img/bg.png"], b"bg")
        self.assertEqual(self.by_url["https://site.test/docs/child.css"], WEB["https://site.test/css/v2/child.css"][0])
        self.assertEqual(self.by_url["https://site.test/docs/child.ttf"], b"child")
        self.assertEqual(self.web.fetched.count("https://site.test/css/fonts/linked.woff2"), 1)

    def test_a_redirected_frames_urls_are_fetched_from_where_it_went(self):
        self.assertEqual(self.by_url["https://site.test/docs/framed.gif"], b"framed")
        self.assertIn("https://other.test/frame/framed.gif", self.web.fetched)

    def test_what_cannot_be_kept_is_omitted_with_its_reason(self):
        self.assertIn("https://site.test/docs/legacy.js", self.page.omitted)
        self.assertNotIn("https://site.test/docs/legacy.js", self.by_url)

    def test_a_saved_capture_is_complete_and_linked_into_the_corpus(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        first, second = os.path.join(directory.name, "pages"), os.path.join(directory.name, "corpus")
        pages.save((first, second), "news-site.html", self.page)
        with open(os.path.join(first, "news-site.json"), encoding="utf-8") as f:
            record = json.load(f)
        self.assertEqual(record["url"], PAGE_URL)
        self.assertEqual(record["type"], "text/html; charset=utf-8")
        files = {r["url"]: r for r in record["resources"]}
        self.assertTrue(files["https://site.test/docs/old.css"]["file"].endswith(".css"))
        self.assertEqual(files["https://site.test/docs/old.css"]["type"], "text/css; charset=utf-8")
        self.assertTrue(files["https://site.test/docs/hero.avif"]["file"].endswith(".avif"))
        # A resource kept at two URLs is one file.
        self.assertEqual(files["https://site.test/docs/old.css"]["file"],
                         files["https://site.test/css/v2/sheet.css"]["file"])
        for r in record["resources"]:
            self.assertEqual(os.stat(os.path.join(first, r["file"])).st_ino,
                             os.stat(os.path.join(second, r["file"])).st_ino)
        self.assertEqual(os.stat(os.path.join(first, "news-site.html")).st_ino,
                         os.stat(os.path.join(second, "news-site.html")).st_ino)

    def test_a_page_that_is_a_file_keeps_only_what_other_origins_serve(self):
        served = pages.Capture(PAGE_URL, PAGE, "text/html", {
            "https://site.test/docs/app.js": (b"run()", "text/javascript", "https://site.test/docs/app.js"),
            "https://fonts.test/face.woff2": (b"face", "font/woff2", "https://fonts.test/face.woff2"),
        }, {"https://site.test/docs/missing.png": "not found", "https://fonts.test/refused.ttf": "HTTP Error 403"})
        page = pages.localized(served, "https://site.test/", "file:///checkout/docs/index.html")
        self.assertEqual(page.url, "file:///checkout/docs/index.html")
        self.assertEqual(sorted(page.resources), ["https://fonts.test/face.woff2"])
        self.assertEqual(sorted(page.omitted), ["https://fonts.test/refused.ttf"])

    def test_a_page_that_is_not_html_or_failed_is_not_captured(self):
        def failed(url):
            return types.SimpleNamespace(document=(url, b"", "text/html", 404), responses={}, aliases={}, omitted={})

        self.assertEqual(pages.capture(PAGE_URL, self.web, failed), "status 404")


class SrcsetTests(unittest.TestCase):
    def test_candidates_are_split_on_commas_between_them_and_not_in_urls(self):
        self.assertEqual(pages.srcset("a.png 1x, b,c.png 2x,d.png 3x"), ["a.png", "b,c.png", "d.png"])
        self.assertEqual(pages.srcset(" one.webp "), ["one.webp"])


class AbsoluteTests(unittest.TestCase):
    def test_a_reference_is_resolved_as_a_url_parser_resolves_it(self):
        self.assertEqual(pages.absolute("https://a.test/x/", " im g.png\n?q=1 2#top"), "https://a.test/x/im%20g.png?q=1%202")
        self.assertEqual(pages.absolute("https://a.test/", "/wiki/\u6771"), "https://a.test/wiki/%E6%9D%B1")
        self.assertIsNone(pages.absolute("https://a.test/", "data:image/png;base64,AAAA"))
        self.assertIsNone(pages.absolute("https://a.test/", "#local"))


if __name__ == "__main__":
    unittest.main()
