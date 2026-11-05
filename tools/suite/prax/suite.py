#!/usr/bin/env python3
"""Checks our pages (suites/prax) against the specifications that they walk through.

The pages are ordered by index files, not by their names. `suites/prax/index.txt` lists the directories in the
order of their names, so that a reader finds a module where its name puts it, CSS 2's and SVG 2's chapters among
them. Each directory is listed with every specification that it walks through, starting with its base level, which
names the directory's supports pages, and, for a chapter of CSS 2, the chapter's heading, or for a chapter of SVG 2,
the chapter's page:

    css2-10-visual-formatting-model-details CSS2/#visudet
    css-sizing css-sizing-3 css-sizing-4
    svg2-10-basic-shapes SVG2/shapes.html

A directory that walks no specification, such as `workloads`, is listed by its name alone. Its index lists its pages
and no sections, and its pages have no eyebrow, since they are real pages and not the specifications' figures. Its
pages may be files of any kind, such as `syntax`'s sources, which an editor opens; the tools that open pages in a
browser open only those that are HTML.

A directory's `index.txt` lists its sections in the specification's order, each by its anchor with the number and the
title that the specification gives it, and under each, indented, the pages that walk through it. Pages before the
first section, such as `00-supports-1.html`, belong to the directory as a whole:

    00-supports-1.html
    CSS2/#abs-non-replaced-width 10.3.7 Absolutely positioned, non-replaced elements
        absolute-static-position.html

A directory may hold sections of more than one specification, as a module's delta levels do, so a section names its
specification. A specification published as several pages, as SVG 2 is, repeats anchors from page to page, so a section
of one names its page too: `SVG2/shapes.html#Introduction`. A page is named by its subject alone, and keeps its name
when a draft renumbers its sections or the suite moves to a later level. The numbers that change then are written in the
index, in each page's eyebrow, and in each citation of a section, which is a link to the section's anchor with text that
the anchor determines. `--fix` rewrites the numbers in all three from webref's headings
(third_party/webref/ed/headings), and the titles in the index.

The checks:
  - every page is listed once, and every listed page exists; every directory is listed once, in the order of the
    names, and exists;
  - a directory's index names exactly the specifications that the top-level index lists for it, either in a section
    or in a comment for a section without a page;
  - each section's anchor is a heading of its specification, with that number and title, and a specification's
    sections come in the specification's order;
  - a page's eyebrow is "<specification> · <number> <title>" for its section (`eyebrow_problem` says how freely it may
    give the title), or "<specification> · Support" for a supports page, with the specification named as webref's
    index titles it (`spec_name`);
  - a citation, which is a link to a heading of a specification, says "§<number>" with the heading's number, after the
    specification's name if it gives one;
  - a supports page lists each property that its directory's pages declare, except those of shared/page.css.

Usage: python3 -m tools.suite.prax.suite [--fix]
"""

import argparse
import html
import json
import os
import re
import sys

import tools

ROOT = tools.ROOT
SUITE = os.path.join(ROOT, "suites", "prax")
WEBREF = os.path.join(ROOT, "third_party", "webref", "ed")
SUPPORTS = "00-supports-1.html"

# Matches a section of an index, `<spec>/#<anchor> <number> <title>` or `<spec>/<page>#<anchor> <number> <title>`, and
# a section without a page, which a comment names.
SECTION = re.compile(r"^([A-Za-z][A-Za-z0-9-]*)/((?:[\w-]+\.html)?#\S+)(?: (\S+) (.*))?$")
COMMENTED = re.compile(r"^#\s*([A-Za-z][A-Za-z0-9-]*)/(?:[\w-]+\.html)?#\S+")

# Matches a link to a heading of an editor's draft, and its text: a CSS specification's, a page of SVG 2's or of the
# HTML Standard's, or a W3C specification's published from its own repository (UI Events, Pointer Events).
CITATION = re.compile(r'<a href="?(https://drafts\.csswg\.org/[a-z0-9-]+/|https://svgwg\.org/svg2-draft/'
                      r'|https://html\.spec\.whatwg\.org/multipage/|https://w3c\.github\.io/[a-z0-9-]+/)'
                      r'((?:[\w-]+\.html)?#[^" >]+)"?>(.*?)</a>', re.S)

EYEBROW = re.compile(r"(<div class=pr-eyebrow>)(.*?)(</div>)", re.S)


# ----------------------------------------------------------------------------------------------------------------------
# Specifications

_index = None
_headings = {}


def specs():
    """Returns webref's index of specifications, by short name."""
    global _index
    if _index is None:
        path = os.path.join(WEBREF, "index.json")
        if not os.path.exists(path):
            sys.exit("no webref index at %s; run ./dev.sh fetch webref" % path)
        with open(path, encoding="utf-8") as f:
            _index = {r["shortname"]: r for r in json.load(f)["results"]}
    return _index


def short_of_draft(url):
    """Returns the short name of the specification whose editor's draft is at the URL, or None. Where the levels of a
    series share the URL, as Pointer Events 3 and 4 do, it is the series' current level."""
    found = []
    for short, record in specs().items():
        nightly = record.get("nightly", {})
        if nightly.get("url") == url or url in nightly.get("alternateUrls", []):
            found.append(short)
    for short in found:
        if specs()[short].get("series", {}).get("currentSpecification") == short:
            return short
    return found[0] if found else None


def anchor_of(reference):
    """Returns the anchor that a reference to a heading names: `visudet` for `#visudet`, and `shapes.html#Introduction`
    for a heading on a page of a specification published as several pages."""
    return reference[1:] if reference.startswith("#") else reference


def reference_of(spec, anchor):
    """Returns the reference to a heading that an index gives: `CSS2/#visudet`, or `SVG2/shapes.html#Introduction`."""
    return "%s/%s%s" % (spec, "" if "#" in anchor else "#", anchor)


def spec_name(short):
    """Returns the name that a page uses for the specification: its title without "Module", "Level" or "Standard", as
    in "CSS Fonts 4" and "HTML", or its abbreviation and version, as in "MathML 4"."""
    if short in ("CSS2", "SVG2"):
        return short[:-1] + " 2"
    title = specs()[short]["title"]
    # "Mathematical Markup Language (MathML) Version 4.0" is MathML 4.
    title = re.sub(r"^.* \((\w+)\) Version (\d+)(?:\.0)?$", r"\1 \2", title)
    title = re.sub(r" \(.*\)$", "", title)
    return re.sub(r" Module| Level| Standard$", "", title)


def headings(short):
    """Returns the specification's numbered headings, in order, as (anchor, number, title).

    Webref leaves some headings unnumbered even though their title or their parent gives a number: CSS 2's chapters
    from 10 on are titled "10. Visual formatting model details", and the sections of its appendix E have no numbers. A
    heading with no number of its own takes the next number under its parent, if the parent is numbered.

    The anchor of a heading on a page of a specification published as several pages, as SVG 2 is, names the page:
    `shapes.html#Introduction`, since each of SVG 2's chapters has an `Introduction`."""
    if short in _headings:
        return _headings[short]
    path = os.path.join(WEBREF, "headings", short + ".json")
    if not os.path.exists(path):
        _headings[short] = None
        return None
    with open(path, encoding="utf-8") as f:
        raw = json.load(f)["headings"]
    pages = set(specs().get(short, {}).get("nightly", {}).get("pages", []))
    result = []
    stack = []          # (level, number, number of children numbered so far) for each open heading
    for h in raw:
        level, title, number = h["level"], " ".join(h["title"].split()), h.get("number")
        while stack and stack[-1][0] >= level:
            stack.pop()
        if number is None:
            prefix = re.match(r"^([0-9]+|[A-Z])\. (.*)$", title)
            if prefix:
                number, title = prefix.group(1), prefix.group(2)
            elif stack and stack[-1][1] is not None and stack[-1][0] == level - 1:
                number = "%s.%d" % (stack[-1][1], stack[-1][2] + 1)
        if stack and number is not None:
            stack[-1][2] += 1
        stack.append([level, number, 0])
        if number is not None:
            page = h["href"].partition("#")[0]
            anchor = "%s#%s" % (page.rsplit("/", 1)[1], h["id"]) if page in pages else h["id"]
            result.append((anchor, number, title))
    _headings[short] = result
    return result


def heading(short, anchor):
    """Returns the (number, title) of the heading and its position in the specification, or None."""
    for i, (a, number, title) in enumerate(headings(short) or []):
        if a == anchor:
            return number, title, i
    return None


def chapter(short, page):
    """Returns the number of the chapter on a page of a specification published as several pages, which its headings'
    numbers share, or None. Webref has no heading for the chapter itself."""
    numbers = {n.split(".")[0] for a, n, _ in headings(short) or [] if a.startswith(page + "#")}
    return numbers.pop() if len(numbers) == 1 else None


def by_number(short, number):
    """Returns the anchor of the heading with that number, or None."""
    for a, n, _ in headings(short) or []:
        if n == number:
            return a
    return None


# ----------------------------------------------------------------------------------------------------------------------
# Indexes

class Section:
    def __init__(self, spec, anchor, number, title, line):
        self.spec, self.anchor, self.number, self.title, self.line = spec, anchor, number, title, line
        self.pages = []

    def text(self):
        return reference_of(self.spec, self.anchor) + " %s %s" % (self.number, self.title)


class Directory:
    def __init__(self, name, specs, anchor, line):
        self.name, self.specs, self.anchor, self.line = name, specs, anchor, line
        self.pages = []                 # pages of the directory as a whole, before its first section
        self.sections = []
        self.commented = set()          # the specifications whose sections its comments name

    @property
    def spec(self):
        """Returns the directory's base level, which names its supports pages."""
        return self.specs[0] if self.specs else None

    def path(self):
        return os.path.join(SUITE, self.name)

    def index(self):
        return os.path.join(self.path(), "index.txt")

    def all_pages(self):
        return [(None, p) for p in self.pages] + [(s, p) for s in self.sections for p in s.pages]


def content_lines(path):
    """Returns the lines of an index that are not blank or comments, as (number, text, indented)."""
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f.read().split("\n"), 1):
            stripped = line.strip()
            if stripped and not stripped.startswith("#"):
                yield n, stripped, line != line.lstrip()


def parse_directory(text):
    """Returns a line of the top-level index as (name, specifications, anchor), or None. The line holds the directory's
    name, then each specification that it walks through, the first of which may name a heading, or a page of a
    specification published as several pages. A directory that walks no specification has none."""
    parts = text.split()
    if not parts or "/" in parts[0] or any("/" in p for p in parts[2:]):
        return None
    if len(parts) == 1:
        return parts[0], [], None
    base, _, reference = parts[1].partition("/")
    return parts[0], [base] + parts[2:], anchor_of(reference) or None


def read_suite(problems):
    """Returns the directories in order, each read from its index. A problem is (path, line, message)."""
    top = os.path.join(SUITE, "index.txt")
    directories = []
    for n, text, _ in content_lines(top):
        parsed = parse_directory(text)
        if parsed is None:
            problems.append((top, n, "a directory is its name and its specifications: %r" % text))
            continue
        directories.append(Directory(parsed[0], parsed[1], parsed[2], n))
    for d in directories:
        if not os.path.exists(d.index()):
            problems.append((top, d.line, "%s has no index.txt" % d.name))
            continue
        with open(d.index(), encoding="utf-8") as f:
            for line in f.read().split("\n"):
                match = COMMENTED.match(line.strip())
                if match:
                    d.commented.add(match.group(1))
        for n, text, indented in content_lines(d.index()):
            if text.endswith(".html") or not d.specs:
                if not d.specs and (indented or d.sections):
                    problems.append((d.index(), n, "%s: a directory that walks no specification has no sections"
                                     % text))
                elif indented and d.sections:
                    d.sections[-1].pages.append(text)
                elif not indented and not d.sections:
                    d.pages.append(text)
                else:
                    problems.append((d.index(), n, "%s: a section's pages are indented under it, and only the "
                                     "directory's own come before the first section" % text))
                continue
            match = SECTION.match(text)
            if not match or indented or not d.specs:
                problems.append((d.index(), n, "not a page or a section: %r" % text))
                continue
            d.sections.append(Section(match.group(1), anchor_of(match.group(2)), match.group(3), match.group(4), n))
    return directories


def pages_of(names):
    """Returns the HTML pages of the named directories, as (directory, page), in the index's order. The pages of a
    directory that the top-level index does not list yet, such as one being written, come in its own index's order, or
    else in the order of its files."""
    listed = {d.name: d for d in read_suite([])}
    pages = []
    for name in names:
        if name in listed:
            pages += [(name, p) for _, p in listed[name].all_pages() if p.endswith(".html")]
            continue
        d = Directory(name, [], None, 0)
        if os.path.exists(d.index()):
            for _, text, _ in content_lines(d.index()):
                if text.endswith(".html"):
                    pages.append((name, text))
        elif os.path.isdir(d.path()):
            pages += [(name, f) for f in sorted(os.listdir(d.path())) if f.endswith(".html")]
    return pages


def write_index(d):
    """Rewrites each section's line from its fields, and keeps every other line: pages, and the comments of sections
    without a page."""
    with open(d.index(), encoding="utf-8") as f:
        lines = f.read().split("\n")
    for s in d.sections:
        lines[s.line - 1] = s.text()
    with open(d.index(), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


# ----------------------------------------------------------------------------------------------------------------------
# Checks

def eyebrow_of(d, section):
    """Returns the specification, number and title that the eyebrow of a page in that section gives, and what follows
    them: " · Support" for a supports page."""
    if section is None:
        found = heading(d.spec, d.anchor) if d.anchor else None
        if found:
            return spec_name(d.spec), found[0], found[1], " · Support"
        number = chapter(d.spec, d.anchor) if d.anchor else None
        if number:
            return spec_name(d.spec), number, None, " · Support"
        return spec_name(d.spec), None, None, " · Support"
    return spec_name(section.spec), section.number, section.title, ""


def plain(text):
    return " ".join(html.unescape(re.sub(r"<[^>]*>", "", text)).split())


def folded(title):
    """Returns a title in the form used to compare eyebrows, which may give a title in any case, with or without
    quotes around a property's name."""
    return " ".join(re.sub(r"['‘’\"“”]", "", title).casefold().split())


def eyebrow_problem(given, expected):
    """Returns what is wrong with an eyebrow, and the eyebrow with its specification and number fixed, or None.

    An eyebrow names the specification and gives the section's number exactly. Its title is the specification's title,
    in any case and quoting. It may stop at the title's colon, or continue after the title to say which part of the
    section the page walks through ("5.2 Matching font styles, step 4"). A chapter that webref has no heading for, as
    each of SVG 2's is, has a number and no title, and the eyebrow gives the chapter's title freely."""
    name, number, title, tail = expected
    if number is None:
        right = "%s%s" % (name, tail)
        return None if given == right else ("the eyebrow is %r" % right, right)
    match = re.match(r"^(.*?) · (\S+) (.*?)(%s)?$" % re.escape(tail) if tail else r"^(.*?) · (\S+) (.*)()$", given)
    if match is None or (tail and not match.group(4)):
        if title is None:
            return "the eyebrow is %r" % ("%s · %s <title>%s" % (name, number, tail)), None
        right = "%s · %s %s%s" % (name, number, title, tail)
        return "the eyebrow is %r" % right, right
    page_title = match.group(3)
    if title is not None:
        full, short = folded(title), folded(title.split(": ")[0])
        page = folded(page_title)
        if not (page.startswith(full) or page == short or page.startswith(short + ",") or page.startswith(short + ":")):
            return "the eyebrow's title is not %r" % title, None
    right = "%s · %s %s%s" % (name, number, page_title, tail)
    if right != given:
        return "the eyebrow is %r" % right, right
    return None


def check_sections(d, problems, fix):
    changed = False
    last = {}
    for s in d.sections:
        if headings(s.spec) is None:
            problems.append((d.index(), s.line, "%s is not a specification webref extracts" % s.spec))
            continue
        found = heading(s.spec, s.anchor)
        if found is None:
            problems.append((d.index(), s.line, "%s has no heading #%s" % (s.spec, s.anchor)))
            continue
        number, title, position = found
        if (s.number, s.title) != (number, title):
            if fix:
                s.number, s.title, changed = number, title, True
            else:
                problems.append((d.index(), s.line, "#%s is %s %s" % (s.anchor, number, title), True))
        if position < last.get(s.spec, -1):
            problems.append((d.index(), s.line, "#%s comes before the section above it in %s" % (s.anchor, s.spec)))
        last[s.spec] = position
    if changed:
        write_index(d)


def check_levels(d, problems):
    """Checks that the specifications that a directory's index names are those that the top-level index lists for
    it."""
    named = {s.spec for s in d.sections} | d.commented
    for spec in sorted(named - set(d.specs)):
        problems.append((d.index(), 1, "names %s, which index.txt does not list for %s" % (spec, d.name)))
    for spec in d.specs:
        if spec not in named:
            problems.append((os.path.join(SUITE, "index.txt"), d.line, "%s lists %s, which its index does not name"
                             % (d.name, spec)))


def is_page(d, name):
    """Returns true if the file `name` of a directory is one of its pages: an HTML file, or in a directory that walks no
    specification, any file but its index."""
    if name.endswith(".html"):
        return True
    return (not d.specs and name != "index.txt" and not name.startswith(".")
            and os.path.isfile(os.path.join(d.path(), name)))


def check_pages(d, problems, fix):
    listed = [p for _, p in d.all_pages()]
    for p in sorted(set(listed)):
        if listed.count(p) > 1:
            problems.append((d.index(), 1, "%s is listed %d times" % (p, listed.count(p))))
    on_disk = sorted(f for f in os.listdir(d.path()) if is_page(d, f)) if os.path.isdir(d.path()) else []
    for p in on_disk:
        if p not in listed:
            problems.append((os.path.join(d.path(), p), 1, "not in %s/index.txt" % d.name))
    for section, p in d.all_pages():
        path = os.path.join(d.path(), p)
        if not os.path.exists(path):
            problems.append((d.index(), 1, "%s does not exist" % p))
            continue
        with open(path, encoding="utf-8") as f:
            text = f.read()
        new = text
        if not d.specs:
            check_sources(path, text, problems)
            continue
        match = EYEBROW.search(text)
        wrong = match and eyebrow_problem(plain(match.group(2)), eyebrow_of(d, section))
        if match is None:
            problems.append((path, 1, "no eyebrow"))
        elif wrong:
            message, right = wrong
            if fix and right:
                new = new[:match.start(2)] + html.escape(right, quote=False) + new[match.end(2):]
            else:
                problems.append((path, text.count("\n", 0, match.start()) + 1, message, right is not None))
        new = check_citations(path, new, problems, fix)
        check_sources(path, text, problems)
        # A link to another page, in this directory or another of the suite: a relative path, with no scheme.
        for target in re.findall(r'<a href="?((?:\.\./[\w.-]+/)?[^":/ >]+\.html)"?', text):
            if not os.path.exists(os.path.normpath(os.path.join(d.path(), target))):
                problems.append((path, 0, "links to %s, which does not exist" % target))
        if new != text:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(new)


def check_citations(path, text, problems, fix):
    """Checks that each link to a heading gives the heading's number. Returns the text, with the numbers rewritten if
    `fix` is set."""
    out, at = [], 0
    for match in CITATION.finditer(text):
        short, anchor, label = short_of_draft(match.group(1)), anchor_of(match.group(2)), match.group(3)
        line = text.count("\n", 0, match.start()) + 1
        if short is None or headings(short) is None:
            problems.append((path, line, "%s is not a draft webref extracts" % match.group(1)))
            continue
        found = heading(short, anchor)
        if found is None:
            problems.append((path, line, "%s has no heading #%s" % (short, anchor)))
            continue
        cited = re.match(r"^(?:(.*?) )?§(\S+)$", " ".join(label.split()))
        if cited is None:
            problems.append((path, line, "a citation's text is \"[<specification> ]§<number>\": %r" % label))
            continue
        name, number = cited.group(1), cited.group(2)
        if name is not None and name != spec_name(short):
            problems.append((path, line, "%s is named %r" % (short, spec_name(short))))
        if number != found[0]:
            if fix:
                out.append(text[at:match.start(3)])
                out.append(("%s §%s" % (name, found[0])) if name else "§" + found[0])
                at = match.end(3)
            else:
                problems.append((path, line, "#%s is §%s" % (anchor, found[0]), True))
    out.append(text[at:])
    check_issues(path, text, problems)
    # The numbers in a quotation belong to the quoted text.
    linked = re.sub(r"<q>.*?</q>", "", CITATION.sub("", text), flags=re.S)
    for match in re.finditer(r"§", linked):
        problems.append((path, 0, "a section's number is cited by a link to its anchor: %r"
                         % " ".join(linked[match.start():match.start() + 30].split())))
    return "".join(out)


# Matches a link to an issue in a standard's repository on GitHub, and its text, which names the issue as
# <repository>#<number>.
ISSUE = re.compile(r'<a href="?https://github\.com/([\w.-]+)/([\w.-]+)/issues/(\d+)"?>(.*?)</a>', re.S)


def check_issues(path, text, problems):
    """Checks that an issue is cited by a link whose text names it: `csswg-drafts#5742` for issue 5742 of
    w3c/csswg-drafts, and the repository's name for an issue of another repository. An issue named without a link is
    reported."""
    for match in ISSUE.finditer(text):
        expected = "%s#%s" % (match.group(2), match.group(3))
        if " ".join(match.group(4).split()) != expected:
            problems.append((path, text.count("\n", 0, match.start()) + 1, "an issue's link says %r" % expected))
    bare = re.sub(r"<q>.*?</q>", "", ISSUE.sub("", text), flags=re.S)
    for match in re.finditer(r"[\w.-]+#\d+\b|\b[Ii]ssues? #?\d{3,}\b", bare):
        problems.append((path, bare.count("\n", 0, match.start()) + 1,
                         "an issue is cited by a link to it: %r" % match.group(0)))


def check_sources(path, text, problems):
    """Checks that a page cites no test suite: its content stands on the specifications, their issues and the
    browsers. A fixture loaded from a checkout (`third_party/wpt/...`) is a path, not a citation."""
    for match in re.finditer(r"\bWPT\b|web-platform-tests", text):
        problems.append((path, text.count("\n", 0, match.start()) + 1,
                         "a page argues from the specifications and the browsers, not WPT"))


DECLARATION = re.compile(r"(?:^|[;{\s\"'])(-?[a-z][a-z-]*)\s*:(?!:)")


def properties(declarations):
    """Returns the properties that declarations name, ignoring the URLs and strings in their values."""
    declarations = re.sub(r"url\([^)]*\)|\"[^\"]*\"|'[^']*'", "", declarations)
    return set(DECLARATION.findall(declarations))


def declared(text):
    """Returns the properties that a page's style sheets and style attributes declare. A frame's document (`srcdoc`)
    also counts as the page's. The descriptors of @font-face and similar rules are not properties."""
    text = text.replace("&quot;", '"').replace("&lt;", "<").replace("&gt;", ">")
    found = set()
    for sheet in re.findall(r"<style[^>]*>(.*?)</style>", text, re.S):
        sheet = without_descriptors(re.sub(r"/\*.*?\*/", "", sheet, flags=re.S))
        for body in re.findall(r"\{([^{}]*)\}", sheet):
            found |= properties(body)
    for attribute in re.findall(r"\sstyle=(?:\"([^\"]*)\"|'([^']*)'|([^\s>]+))", text):
        found |= properties("".join(attribute))
    return found & known_properties()


DESCRIPTORS = re.compile(r"@(?:font-face|font-palette-values|font-feature-values|counter-style|property)[^{]*\{")


def without_descriptors(sheet):
    """Returns the style sheet without its rules of descriptors, which may nest blocks (@font-feature-values)."""
    while True:
        match = DESCRIPTORS.search(sheet)
        if match is None:
            return sheet
        depth, i = 1, match.end()
        while i < len(sheet) and depth:
            depth += {"{": 1, "}": -1}.get(sheet[i], 0)
            i += 1
        sheet = sheet[:match.start()] + sheet[i:]


_properties = None


def known_properties():
    """Returns every property that a specification extracted by webref defines. A page may declare something that is
    not a property on purpose, to show that it is dropped."""
    global _properties
    if _properties is None:
        _properties = set()
        folder = os.path.join(WEBREF, "css")
        for file in os.listdir(folder):
            with open(os.path.join(folder, file), encoding="utf-8") as f:
                _properties.update(p["name"] for p in json.load(f).get("properties", []))
    return _properties


def chrome_properties():
    """Returns the properties that shared/page.css declares, with the sides of its margins, padding and borders. They
    style the page itself, and a supports page leaves them out."""
    with open(os.path.join(SUITE, "shared", "page.css"), encoding="utf-8") as f:
        chrome = declared("<style>%s</style>" % f.read())
    sides = ("top", "right", "bottom", "left")
    for side in sides:
        chrome |= {"margin-" + side, "padding-" + side, "border-" + side}
        chrome |= {"border-%s-%s" % (side, part) for part in ("width", "style", "color")}
    return chrome | {"border-width", "border-style", "border-color"}


def is_supports(page):
    return re.fullmatch(r"00-supports-[0-9]+\.html", page) is not None


def check_supports(d, problems):
    path = os.path.join(d.path(), SUPPORTS)
    if SUPPORTS not in d.pages or not os.path.exists(path):
        problems.append((d.index(), 1, "%s has no %s" % (d.name, SUPPORTS)))
        return
    chrome = chrome_properties()
    # A directory whose declarations do not fit on one page continues them on 00-supports-2.html and later pages.
    listed = set()
    for p in d.pages:
        if is_supports(p) and os.path.exists(os.path.join(d.path(), p)):
            with open(os.path.join(d.path(), p), encoding="utf-8") as f:
                listed |= {html.unescape(decl).split(":")[0].strip()
                           for decl in re.findall(r"<code class=\"?pr-decl\"?>(.*?)</code>", f.read(), re.S)}
    used = {}
    for _, p in d.all_pages():
        page = os.path.join(d.path(), p)
        if not is_supports(p) and os.path.exists(page):
            with open(page, encoding="utf-8") as f:
                for prop in declared(f.read()):
                    used.setdefault(prop, p)
    for prop in sorted(set(used) - listed - chrome):
        if not prop.startswith("--"):
            problems.append((path, 1, "lists no declaration of %s, which %s uses" % (prop, used[prop])))


def check(fix=False):
    """Returns the problems of the suite, as (path, line, message, fixable), where `fix` would fix those that are
    fixable. If `fix` is set, the numbers and titles are fixed first."""
    problems = []
    directories = read_suite(problems)
    names = [d.name for d in directories]
    for name in sorted(set(names)):
        if names.count(name) > 1:
            problems.append((os.path.join(SUITE, "index.txt"), 1, "%s is listed %d times" % (name, names.count(name))))
    for before, d in zip(directories, directories[1:]):
        if d.name < before.name:
            problems.append((os.path.join(SUITE, "index.txt"), d.line, "%s comes before %s, in the order of their names"
                             % (d.name, before.name)))
    for entry in sorted(os.listdir(SUITE)):
        if os.path.isdir(os.path.join(SUITE, entry)) and entry != "shared" and entry not in names:
            problems.append((os.path.join(SUITE, "index.txt"), 1, "%s is not listed" % entry))
    for d in directories:
        if not os.path.isdir(d.path()):
            problems.append((os.path.join(SUITE, "index.txt"), d.line, "%s does not exist" % d.name))
            continue
        if not os.path.exists(d.index()):
            continue
        if not d.specs:
            check_pages(d, problems, fix)
            continue
        unknown = [spec for spec in d.specs if headings(spec) is None]
        for spec in unknown:
            problems.append((os.path.join(SUITE, "index.txt"), d.line, "%s is not a specification webref extracts"
                             % spec))
        if unknown:
            continue
        check_levels(d, problems)
        check_sections(d, problems, fix)
        check_pages(d, problems, fix)
        check_supports(d, problems)
    return [problem if len(problem) == 4 else problem + (False,) for problem in problems]


def main():
    parser = argparse.ArgumentParser(description="Our pages, checked against the specifications they walk.")
    parser.add_argument("--fix", action="store_true",
                        help="rewrite sections' numbers and titles, eyebrows and citations from webref first")
    options = parser.parse_args()
    problems = check(options.fix)
    for path, line, message, _ in sorted(problems):
        print("%s:%d: %s" % (os.path.relpath(path, ROOT), line, message))
    print("\n%d problems" % len(problems))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
