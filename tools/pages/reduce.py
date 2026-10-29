#!/usr/bin/env python3
"""Cuts a captured page down to what an invalidation failure needs, so that it can be made into a unit test.

The invalidation harness (tools/pages/invalidation.cpp) changes elements spread through a page and checks the
presentation after each change. If a check fails, this script cuts the page down while the same check still fails at
the same element. It removes, in order: the elements that do not contain that element; the rules of the page's style
sheets, with each sheet kept in its own file so that its URLs resolve as they did; each remaining rule's declarations;
the elements again; and last, every attribute except those that styles read most. Each step tries removing halves of
what it can remove, then quarters, down to one at a time, as delta debugging does. What is left is usually a few
elements and a few rules.

The element is the one that `--element N` chooses in the harness, which the harness marks in a copy of the page as it
loaded (`--mark`), or the first element that `--match TEXT` names. The check is the first failure that the harness
reports at that element, up to the first comma: what failed, without the values, which cutting the page may change.
--want names a different check.

The cut page, its capture and its sheets are written under OUT. `invalidation --match <match> OUT/capture.json
OUT/page.html` fails as the page did.

Usage: python3 -m tools.pages.reduce [--binary PATH] (--element N | --match TEXT) [--want TEXT] <capture.json>
<page.html> OUT"""

import argparse
import html
import html.parser
import json
import os
import subprocess
import sys

from tools.suite import corpus

# The elements that have no end tag.
VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"}

# The attribute that the harness marks the element chosen by --element with.
MARK = "data-invalidation-target"


class Element:
    """An element of the page's markup: its name, attributes, and the span of its source, including its end tag."""

    def __init__(self, tag, attrs, start, parent):
        self.tag = tag
        self.attrs = attrs
        self.start = start
        self.open_end = None            # the offset just past its start tag
        self.end = None
        self.parent = parent
        self.children = []


def parse(text):
    """Returns the elements of the markup, as a tree under a root that spans all of it. The harness's copy of a page is
    serialized from its document, so every element that has an end tag has one."""
    starts = [0]
    for i, c in enumerate(text):
        if c == "\n":
            starts.append(i + 1)
    root = Element("#root", {}, 0, None)
    root.end = len(text)

    class Parser(html.parser.HTMLParser):
        def __init__(self):
            super().__init__(convert_charrefs=False)
            self.open = root

        def position(self):
            line, column = self.getpos()
            return starts[line - 1] + column

        def handle_starttag(self, tag, attrs):
            at = self.position()
            element = Element(tag, dict(attrs), at, self.open)
            element.open_end = at + len(self.get_starttag_text())
            self.open.children.append(element)
            if tag in VOID:
                element.end = at + len(self.get_starttag_text())
            else:
                self.open = element

        def handle_startendtag(self, tag, attrs):
            at = self.position()
            element = Element(tag, dict(attrs), at, self.open)
            element.open_end = element.end = at + len(self.get_starttag_text())
            self.open.children.append(element)

        def handle_endtag(self, tag):
            at = self.position()
            element = self.open
            while element is not root and element.tag != tag:
                element = element.parent
            if element is root:
                return
            # Elements left open inside this element end where it ends.
            inner = self.open
            while inner is not element:
                inner.end = at
                inner = inner.parent
            element.end = text.find(">", at) + 1
            self.open = element.parent

    parser = Parser()
    parser.feed(text)
    unclosed = parser.open
    while unclosed is not root:
        unclosed.end = len(text)
        unclosed = unclosed.parent
    return root


def names(element, match):
    """Returns true if `match` names the element, as the harness's --match interprets it."""
    return match in (element.attrs.get("class") or "") or element.tag == match or match in element.attrs


def holds(element, match):
    return names(element, match) or any(holds(child, match) for child in element.children)


# The attributes that styles and selectors most often read, which cutting attributes keeps, and the elements whose
# attributes load the page's style, which cutting attributes leaves whole.
KEPT_ATTRIBUTES = ("class", "style", "id", MARK)
WHOLE = ("link", "style", "meta", "base")


def without_attributes(text):
    """Returns the markup with every attribute except KEPT_ATTRIBUTES removed from every start tag."""
    tags = []

    def visit(element):
        if element.open_end is not None:
            tags.append(element)
        for child in element.children:
            visit(child)

    visit(parse(text))
    for element in sorted(tags, key=lambda e: -e.start):
        if element.tag in WHOLE:
            continue
        kept = "".join(" %s" % name if value is None else ' %s="%s"' % (name, html.escape(value))
                       for name, value in element.attrs.items() if name in KEPT_ATTRIBUTES)
        text = text[:element.start] + "<" + element.tag + kept + ">" + text[element.open_end:]
    return text


def declarations(rule):
    """Returns a style rule's selector and its declarations, split at the semicolons outside parentheses and strings.
    Returns None if the rule holds rules instead of declarations."""
    head, body = rule.split("{", 1)
    body = body[:-1]
    if "{" in body:
        return None
    out = []
    depth = 0
    quote = None
    start = 0
    for i, c in enumerate(body):
        if quote:
            if c == quote:
                quote = None
        elif c in "'\"":
            quote = c
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif c == ";" and depth == 0:
            out.append(body[start:i].strip())
            start = i + 1
    if body[start:].strip():
        out.append(body[start:].strip())
    return head, [d for d in out if d]


def rules(text):
    """Returns a style sheet's top-level rules. A block at-rule that holds rules (@layer, @media, @supports) is split
    into one copy for each rule that it holds, so that each rule can be removed on its own."""
    out = []
    depth = 0
    start = 0
    for i, c in enumerate(text):
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                rule = text[start:i + 1].strip()
                start = i + 1
                head = rule.split("{", 1)[0].strip()
                if head.startswith(("@layer", "@media", "@supports")):
                    inner = rules(rule[len(head) + 1:-1])
                    out += [head + "{" + r + "}" for r in inner]
                else:
                    out.append(rule)
    return out


class Reduction:
    """A page that is being cut down while the harness still fails at the element that `match` names, with `want` in
    its report."""

    def __init__(self, binary, capture, match, want, out):
        self.binary = binary
        self.match = match
        self.want = want
        self.out = out
        self.capture = capture
        self.tries = 0
        # Each sheet's rules, by the index of its resource in the capture.
        self.sheets = {}
        for index, resource in enumerate(capture["resources"]):
            if resource["file"].endswith(".css"):
                with open(resource["file"], encoding="utf-8", errors="replace") as f:
                    self.sheets[index] = rules(f.read())

    def write(self, text, sheets, prefix):
        """Writes the page, its sheets and a capture that names them under OUT, and returns the paths of the page and
        the capture."""
        capture = json.loads(json.dumps(self.capture))
        for index, kept in sheets.items():
            path = os.path.join(self.out, "%s-sheet-%d.css" % (prefix, index))
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(kept))
            capture["resources"][index]["file"] = path
        page = os.path.join(self.out, prefix + ".html")
        with open(page, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        manifest = os.path.join(self.out, prefix + ".json")
        with open(manifest, "w", encoding="utf-8", newline="\n") as f:
            json.dump(capture, f)
        return page, manifest

    def report(self, page, manifest):
        """Returns the harness's first failure at the element on the page, or None if it found none."""
        done = subprocess.run([self.binary, "--match", self.match, manifest, page], capture_output=True, text=True,
                              timeout=600 * corpus.SLOWDOWN)
        for line in done.stdout.splitlines():
            if line.startswith("{"):
                report = json.loads(line)
                return report["firstFailure"] if report["failures"] else None
        return None

    def fails(self, text, sheets):
        self.tries += 1
        failure = self.report(*self.write(text, sheets, "try"))
        return failure is not None and self.want in failure

    def cut_elements(self, text, sheets):
        """Removes elements that do not contain the target, as long as the failure remains. At each element, it tries
        removing halves of its children, then quarters, down to one at a time. A cut moves the spans after it, so the
        page is parsed again after each cut and its elements are tried again."""
        while True:
            cut = self.cut_one(text, sheets)
            if cut is None:
                return text
            text = cut
            print("  %d bytes" % len(text), flush=True)

    def cut_one(self, text, sheets):
        """Returns the page with the first group of elements removed whose removal keeps the failure, or None."""
        queue = [parse(text)]
        while queue:
            element = queue.pop(0)
            removable = [c for c in element.children
                         if not holds(c, self.match) and c.tag not in ("html", "head", "body")]
            chunk = max(1, len(removable) // 2)
            while removable and chunk >= 1:
                for i in range(0, len(removable), chunk):
                    candidate = text
                    for c in sorted(removable[i:i + chunk], key=lambda c: -c.start):
                        candidate = candidate[:c.start] + candidate[c.end:]
                    if self.fails(candidate, sheets):
                        return candidate
                chunk //= 2
            queue.extend(element.children)
        return None

    def cut_rules(self, text, sheets):
        """Removes the sheets' rules as long as the failure remains: halves of them, then quarters, down to one at a
        time."""
        units = [(index, rule) for index, kept in sheets.items() for rule in kept]

        def grouped(kept):
            out = {index: [] for index in sheets}
            for index, rule in kept:
                out[index].append(rule)
            return out

        chunk = len(units) // 2
        while chunk >= 1:
            cut = False
            i = 0
            while i < len(units):
                candidate = units[:i] + units[i + chunk:]
                if self.fails(text, grouped(candidate)):
                    units = candidate
                    cut = True
                    print("  %d rules" % len(units), flush=True)
                else:
                    i += chunk
            if not cut:
                chunk //= 2
        return grouped(units)

    def cut_declarations(self, text, sheets):
        """Removes each remaining rule's declarations as long as the failure remains, the same way cut_rules removes
        rules."""
        sheets = {index: list(kept) for index, kept in sheets.items()}
        for index, kept in sheets.items():
            for at, rule in enumerate(kept):
                # An at-rule's copy of one rule keeps its prelude, and only that rule's declarations are cut.
                prefix, inner = "", rule
                if rule.startswith("@"):
                    prefix = rule.split("{", 1)[0] + "{"
                    inner = rule[len(prefix):-1]
                split = declarations(inner)
                if split is None:
                    continue
                head, decls = split
                build = lambda ds: prefix + head + "{" + ";".join(ds) + "}" + ("}" if prefix else "")
                chunk = len(decls) // 2
                while chunk >= 1:
                    cut = False
                    i = 0
                    while i < len(decls):
                        candidate = decls[:i] + decls[i + chunk:]
                        kept[at] = build(candidate)
                        if self.fails(text, sheets):
                            decls = candidate
                            cut = True
                        else:
                            i += chunk
                    if not cut:
                        chunk //= 2
                kept[at] = build(decls)
        return sheets


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--binary", default=corpus.binary("invalidation"), help="the invalidation harness")
    which = ap.add_mutually_exclusive_group(required=True)
    which.add_argument("--element", type=int, help="the element the harness chooses as this one, from zero")
    which.add_argument("--match", help="the first element whose class holds this, whose name it is, or which has it")
    ap.add_argument("--want", help="what the failure must report; what failed first, by default")
    ap.add_argument("capture")
    ap.add_argument("page")
    ap.add_argument("out")
    args = ap.parse_args()
    corpus.require(args.binary, "invalidation")
    os.makedirs(args.out, exist_ok=True)

    with open(args.capture, encoding="utf-8") as f:
        capture = json.load(f)
    for resource in capture["resources"]:
        resource["file"] = os.path.join(os.path.dirname(os.path.abspath(args.capture)), resource["file"])
    page = os.path.abspath(args.page)
    match = args.match
    if args.element is not None:
        page = os.path.join(args.out, "marked.html")
        done = subprocess.run([args.binary, "--element", str(args.element), "--mark", page, args.capture, args.page],
                              capture_output=True, text=True)
        if done.returncode != 0:
            sys.exit("reduce: the harness did not mark element %d" % args.element)
        match = MARK

    reduction = Reduction(args.binary, capture, match, args.want or "", args.out)
    with open(page, encoding="utf-8", errors="replace") as f:
        text = f.read()
    failure = reduction.report(*reduction.write(text, reduction.sheets, "try"))
    if failure is None:
        sys.exit("reduce: the harness finds no failure at the element")
    if args.want is None:
        # What failed, without the element's path before it or the values after it.
        reduction.want = failure.split(": ", 2)[-1].split(",")[0]
    print("keeping: %s" % reduction.want)
    print("elements")
    text = reduction.cut_elements(text, reduction.sheets)
    print("rules")
    sheets = reduction.cut_rules(text, reduction.sheets)
    print("declarations")
    sheets = reduction.cut_declarations(text, sheets)
    print("elements, again")
    text = reduction.cut_elements(text, sheets)
    stripped = without_attributes(text)
    if reduction.fails(stripped, sheets):
        text = stripped
        print("attributes: kept only %s" % ", ".join(KEPT_ATTRIBUTES))
    page, manifest = reduction.write(text, sheets, "page")
    os.replace(manifest, os.path.join(args.out, "capture.json"))
    print("%d bytes, %d rules, after %d tries: invalidation --match %s %s %s" % (
        len(text), sum(len(kept) for kept in sheets.values()), reduction.tries, match,
        os.path.join(args.out, "capture.json"), page))
    return 0


if __name__ == "__main__":
    sys.exit(main())
