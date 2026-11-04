#!/usr/bin/env python3
"""Searches a specification's source verbatim. The editor's draft's Bikeshed source is fetched once into
third_party/specs and searched there.

A plan cites the drafts in force, and reads them instead of a summary of them. A name is the short name of a CSS
Working Group draft (css-break-4, css2, cssom-view-1), whose source is the Overview.bs in its directory of
w3c/csswg-drafts; of an FXTF draft (css-masking-1, filter-effects-1, compositing-2), whose source is the same in
w3c/fxtf-drafts; of a WHATWG standard (html, dom), whose source is in its own repository; or svg2-<chapter> for a
chapter of SVG 2 (svg2-styling, svg2-painting), whose source is that chapter's file in w3c/svgwg; or of a W3C
specification in a repository of its own (uievents, uievents-key, pointerevents, mathml-core, mathml4), whose source
is its Bikeshed file, or the HTML that ReSpec publishes from, with its includes inlined, where the repository keeps no
Bikeshed source. The source is text as its editors write it, so a match is shown with its section's heading and its
markup.

Usage: python3 -m tools.spec.spec <name> [pattern] [--context N] [--refresh]
  With a pattern, a case-insensitive regular expression, prints each match with N lines around it, by line number in
  the source. Without a pattern, prints the path of the source.
"""

import argparse
import os
import re
import sys
import urllib.request

import tools

ROOT = tools.ROOT
SPECS = os.path.join(ROOT, "third_party", "specs")

# WHATWG standards, each in its own repository, with the file of its source if that is not <name>.bs.
WHATWG = {"html": "source", "dom": None, "url": None, "fetch": None, "encoding": None, "infra": None, "streams": "index.bs",
          "console": None, "webidl": "index.bs", "storage": None, "compat": "compatibility.bs", "xhr": None}


# W3C specifications in repositories of their own, each with the path of its source in its repository.
W3C = {"uievents": "main/index.bs", "uievents-key": "gh-pages/index.html", "uievents-code": "gh-pages/index.html",
       "input-events": "gh-pages/index.html", "pointerevents": "gh-pages/index.html",
       "intersection-observer": "main/index.bs", "hr-time": "main/index.bs", "performance-timeline": "main/index.bs",
       "user-timing": "main/index.bs", "navigation-timing": "main/index.bs", "resource-timing": "main/index.bs",
       "server-timing": "main/index.bs", "file-api": "main/index.bs", "media-source-2": "main/media-source-respec.html",
       "mse-byte-stream-format-isobmff": "gh-pages/index.html", "mathml-core": "main/spec.html",
       "mathml4": "main/spec.html"}

# The repositories of the W3C specifications whose repository is not named for the specification.
W3C_REPOSITORIES = {"intersection-observer": "IntersectionObserver", "file-api": "FileAPI",
                    "media-source-2": "media-source", "mathml4": "mathml"}

# The drafts of the FXTF, which w3c/fxtf-drafts holds rather than w3c/csswg-drafts.
FXTF = {"compositing-1", "compositing-2", "css-masking-1", "css-masking-2", "filter-effects-1", "filter-effects-2",
        "geometry-1", "motion-1"}

# The prefix of an SVG 2 chapter's name: svg2-styling is master/styling.bs, or master/styling.html for a chapter that
# is not yet in Bikeshed.
SVG2 = "svg2-"

RAW = "https://raw.githubusercontent.com/"


def urls_of(name):
    """Returns the URLs that the source of `name` may be at, in the order to try them."""
    if name in WHATWG:
        return [RAW + "whatwg/%s/main/%s" % (name, WHATWG[name] or name + ".bs")]
    if name in W3C:
        return [RAW + "w3c/%s/%s" % (W3C_REPOSITORIES.get(name, name), W3C[name])]
    if name.startswith(SVG2):
        chapter = name[len(SVG2):]
        return [RAW + "w3c/svgwg/main/master/%s.%s" % (chapter, kind) for kind in ("bs", "html")]
    group = "fxtf-drafts" if name in FXTF else "csswg-drafts"
    return [RAW + "w3c/%s/main/%s/Overview.bs" % (group, name)]


def fetch(name, refresh):
    """Returns the path of the draft's source, fetching it if it is not already stored or if `refresh` is set."""
    if not re.fullmatch(r"[a-z0-9-]+", name):
        sys.exit("%s is not a draft's short name" % name)
    os.makedirs(SPECS, exist_ok=True)
    html = name == "html" or W3C.get(name, "").endswith(".html")
    path = os.path.join(SPECS, name + (".html" if html else ".bs"))
    if refresh or not os.path.exists(path):
        errors = []
        body = None
        for url in urls_of(name):
            try:
                with urllib.request.urlopen(url, timeout=60) as response:
                    body = response.read()
                break
            except OSError as error:
                errors.append("%s: %s" % (url, error))
        if body is None:
            sys.exit("no source for %s at %s" % (name, "; ".join(errors)))
        with open(path, "wb") as f:
            f.write(inline_respec_includes(body, url) if html else inline_includes(body, url))
    return path


# A Bikeshed include: a block that stands for the file at its path, relative to the source.
INCLUDE = re.compile(rb'<pre class="?include"?>\s*path:\s*(\S+)\s*</pre>')


def inline_includes(body, url, root=None):
    """Returns the source `body`, fetched from `url`, with each include replaced by the file that it names, so that a
    source split into files reads as one. A path resolves against the including file, or else against the root source.
    A generated .include is read from the .txt that a repository builds it from. An include that is found at neither
    stays as it is."""
    bases = [url.rsplit("/", 1)[0] + "/"]
    if root is not None and root not in bases:
        bases.append(root)

    def included(match):
        path = match.group(1).decode()
        paths = [path] + ([path[:-len(".include")] + ".txt"] if path.endswith(".include") else [])
        for target in [base + p for p in paths for base in bases]:
            try:
                with urllib.request.urlopen(target, timeout=60) as response:
                    return inline_includes(response.read(), target, root or bases[0])
            except OSError:
                continue
        return match.group(0)

    return INCLUDE.sub(included, body)


# A ReSpec include: an element whose content is the file at its path, relative to the source.
RESPEC_INCLUDE = re.compile(rb'(<(\w+)\b[^>]*\bdata-include="([^"]+)"[^>]*>)')


def inline_respec_includes(body, url):
    """Returns the published HTML `body`, fetched from `url`, with the file that each ReSpec include names placed inside
    the element that names it, so that tables and style sheets kept in files of their own are searched with the rest.
    An include that is not found stays empty."""
    base = url.rsplit("/", 1)[0] + "/"
    cache = {}

    def included(match):
        path = match.group(3).decode()
        if path not in cache:
            try:
                with urllib.request.urlopen(base + path, timeout=60) as response:
                    cache[path] = response.read()
            except OSError:
                cache[path] = b""
        return match.group(1) + cache[path]

    return RESPEC_INCLUDE.sub(included, body)


def main():
    parser = argparse.ArgumentParser(description="A specification's source, verbatim.")
    parser.add_argument("name")
    parser.add_argument("pattern", nargs="?")
    parser.add_argument("--context", type=int, default=15)
    parser.add_argument("--refresh", action="store_true", help="fetch the source again")
    options = parser.parse_args()
    path = fetch(options.name, options.refresh)
    if options.pattern is None:
        print(path)
        return 0
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    pattern = re.compile(options.pattern, re.IGNORECASE)
    hits = {i for i, line in enumerate(lines) if pattern.search(line)}
    if not hits:
        print("no match for %r in %s" % (options.pattern, path), file=sys.stderr)
        return 1
    # Matches whose contexts overlap print as one passage.
    passages = []
    for i in sorted(hits):
        lo, hi = max(0, i - options.context), min(len(lines), i + options.context + 1)
        if passages and lo <= passages[-1][1]:
            passages[-1][1] = hi
        else:
            passages.append([lo, hi])
    for n, (lo, hi) in enumerate(passages):
        if n:
            print("--")
        for i in range(lo, hi):
            print("%6d%s %s" % (i + 1, ":" if i in hits else " ", lines[i]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
