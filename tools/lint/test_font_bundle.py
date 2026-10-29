#!/usr/bin/env python3
"""Checks that the font bundles (pr/font_bundle.h) cover the text of every language that lib/core names in
pr/locale_common.h: those with 50 million users or more, and the most used language of each script that UAX #31's
Table 5 recommends. Each of a language's main exemplar characters, as CLDR gives them, must have a glyph in some face
of the bundles other than Last Resort, whose glyphs mark what is missing. The fonts are those that the build fetches
into font_bundle in its directory, which is build by default or $PR_FONT_BUNDLE_DIR. Without them, or without CLDR in
third_party/cldr, the check is skipped.

Usage: python3 -m tools.lint.test_font_bundle
"""

import os
import re
import struct
import unittest

import tools

ROOT = tools.ROOT
BUNDLE = os.environ.get("PR_FONT_BUNDLE_DIR", os.path.join(ROOT, "build", "font_bundle"))
CLDR = os.path.join(ROOT, "third_party", "cldr", "common", "main")
LANGUAGES = os.path.join(ROOT, "lib", "locale", "include", "pr", "locale_common.h")

def code_points(path):
    """Returns the code points that the cmap of the font file at `path` maps, from its format 4 and 12 subtables."""
    with open(path, "rb") as f:
        data = f.read()
    tables = {}
    for i in range(struct.unpack(">H", data[4:6])[0]):
        tag, _, offset, _ = struct.unpack(">4sIII", data[12 + 16 * i:28 + 16 * i])
        tables[tag] = offset
    cmap = tables[b"cmap"]
    found = set()
    for i in range(struct.unpack(">H", data[cmap + 2:cmap + 4])[0]):
        _, _, offset = struct.unpack(">HHI", data[cmap + 4 + 8 * i:cmap + 12 + 8 * i])
        table = cmap + offset
        kind = struct.unpack(">H", data[table:table + 2])[0]
        if kind == 12:
            for g in range(struct.unpack(">I", data[table + 12:table + 16])[0]):
                first, last, _ = struct.unpack(">III", data[table + 16 + 12 * g:table + 28 + 12 * g])
                found.update(range(first, last + 1))
        elif kind == 4:
            count = struct.unpack(">H", data[table + 6:table + 8])[0] // 2
            ends = struct.unpack(">%dH" % count, data[table + 14:table + 14 + 2 * count])
            starts = struct.unpack(">%dH" % count, data[table + 16 + 2 * count:table + 16 + 4 * count])
            for first, last in zip(starts, ends):
                if first != 0xFFFF:
                    found.update(range(first, last + 1))
    return found


def languages():
    """Returns the subtags of the languages that pr/locale_common.h names."""
    with open(LANGUAGES, encoding="utf-8") as f:
        return [name.lower() for name in re.findall(r"^\s*PrLanguage_(\w+) = \d+,", f.read(), re.M)]


def exemplars(language):
    """Returns the code points of the main exemplar characters of `language`'s CLDR locale, or None if CLDR has
    none."""
    path = os.path.join(CLDR, language + ".xml")
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as f:
        match = re.search(r"<exemplarCharacters>\[(.*?)\]</exemplarCharacters>", f.read())
    if match is None:
        return None
    text = re.sub(r"\\u([0-9A-Fa-f]{4})", lambda m: chr(int(m.group(1), 16)), match.group(1))
    found = set()
    at = 0
    while at < len(text):
        c = text[at]
        if c == "\\":
            found.add(ord(text[at + 1]))
            at += 2
        elif c == "-" and at + 1 < len(text) and found:
            # A range from the code point before the dash to the one after it.
            last = max(found)
            found.update(range(last, ord(text[at + 1]) + 1))
            at += 2
        elif c not in " {}":
            found.add(ord(c))
            at += 1
        else:
            at += 1
    # Joiners and other default ignorable code points need no glyph.
    return {c for c in found if not (0x200B <= c <= 0x200F or 0xFE00 <= c <= 0xFE0F)}


class FontBundleTests(unittest.TestCase):
    def test_every_language_has_its_exemplar_characters_served(self):
        if not os.path.isdir(BUNDLE):
            self.skipTest("no font bundles; the build fetches them")
        if not os.path.isdir(CLDR):
            self.skipTest("no CLDR; ./dev.sh fetch cldr fetches it")
        served = set()
        for name in sorted(os.listdir(BUNDLE)):
            if name.endswith((".ttf", ".otf")) and not name.startswith("LastResort"):
                served |= code_points(os.path.join(BUNDLE, name))
        checked = 0
        for language in languages():
            characters = exemplars(language)
            if characters is None:
                continue
            checked += 1
            missing = sorted(c for c in characters if c not in served)
            with self.subTest(language=language):
                self.assertEqual([], ["U+%04X" % c for c in missing])
        self.assertGreater(checked, 40)


if __name__ == "__main__":
    unittest.main()
