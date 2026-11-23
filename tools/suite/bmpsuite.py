#!/usr/bin/env python3
"""Decodes Jason Summers's BMP Suite with lib/image, beside the PNGs of its pictures.

The corpus is the `bmpsuite` checkout, the release archive of the BMP Suite 2.8. Its good and questionable files each
have one or more PNGs of their picture, which its page names beside the file, and a decode passes if it is one of
them exactly. Its bad files pass whatever the decoder says of them, as long as it answers. The comparison is
tools/suite/decode_compare.py's.
"""

import html.parser
import os
import re
import sys

from tools.suite import corpus
from tools.suite import decode_compare

ROOT = os.path.join(corpus.ROOT, "third_party", "bmpsuite", "bmpsuite-2.8")


class Rows(html.parser.HTMLParser):
    """Reads the table of the suite's page: each file, and the PNGs of its picture."""

    def __init__(self):
        super().__init__()
        self.rows = {}
        self.cells = []
        self.images = []

    def handle_starttag(self, tag, attributes):
        if tag == "tr":
            self.cells = []
            self.images = []
        elif tag == "img":
            source = dict(attributes).get("src", "")
            if not source.startswith(".."):
                self.images.append(source)

    def handle_endtag(self, tag):
        if tag == "tr" and self.cells:
            name = self.cells[0].strip()
            if re.match(r"^[gqb]/.*\.bmp$", name):
                self.rows[name] = list(self.images)

    def handle_data(self, data):
        if data.strip():
            self.cells.append(data)


def parts():
    """Returns the suite's parts, its good, questionable and bad directories, with their cases."""
    if not os.path.isdir(ROOT):
        return []
    page = Rows()
    with open(os.path.join(ROOT, "html", "bmpsuite.html"), encoding="utf-8") as f:
        page.feed(f.read())
    found = []
    for directory in ("g", "q", "b"):
        cases = []
        for name in sorted(os.listdir(os.path.join(ROOT, directory))):
            if not name.endswith(".bmp"):
                continue
            key = directory + "/" + name
            references = [os.path.join(ROOT, "html", image) for image in page.rows.get(key, [])]
            cases.append(decode_compare.Case(key, os.path.join(ROOT, directory, name),
                                             [path for path in references if os.path.exists(path)], directory == "b"))
        found.append((directory, cases))
    return found


if __name__ == "__main__":
    sys.exit(decode_compare.run("bmpsuite", parts(), "bmpsuite"))
