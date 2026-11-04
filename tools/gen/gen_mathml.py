#!/usr/bin/env python3
"""Generates MathML's operator dictionary and the tables that stretch operators from MathML Core's tables, at the commit
that tools/fetch/checkouts.py pins (MATHML_CORE_COMMIT):

  lib/mathml/src/mathml_dictionary.h, lib/mathml/src/mathml_dictionary.cpp
  lib/tex/src/tex_dictionary.h, lib/tex/src/tex_dictionary.cpp
  lib/font/src/font_math_unicode.h, lib/font/src/font_math_unicode.cpp

lib/tex has its own copy of the dictionary, without the tables that only layout reads, so that it depends on core alone.
The dictionary is the compact one of mathml-core/#operator-dictionary: each (content, form) key's category, the values
of each category, the fences and the separators. The other tables are the operators that stretch along the inline axis
(mathml-core/#stretchy-operator-axis), the combining characters that stand for spacing accents
(mathml-core/#combining-character-equivalences), and the glyph assemblies that Unicode's characters build
(mathml-core/#unicode-based-glyph-assemblies), which lib/font stretches a glyph by in a font without a MATH table.
The operators that are accents come from MathML 3's dictionary (MathML3/appendixc.html), the last to list the accent
property, which MathML 4 still reads as the default of munder's and mover's accent attributes (mathml4/#presm_munder).

Usage: python3 -m tools.gen.gen_mathml [--tables DIR] [--out LIBDIR]
"""

import argparse
import html
import os
import re

from tools.fetch import checkouts
from tools.gen import gen_common
from tools.gen.gen_common import namespaced, table, write

SOURCE = "MathML Core's tables"
URL = "https://github.com/w3c/mathml-core/tree/%s/tables" % checkouts.MATHML_CORE_COMMIT
BANNER = gen_common.banner(__file__, SOURCE, URL)

# The forms of an operator, in the order of the spec's encoding of a category: the two least significant bits of the
# encoding are the form (mathml-core/#operator-dictionary-categories-values).
FORMS = ["infix", "prefix", "postfix"]

# The categories that have no encoding, which the lookup returns before it reads the encoded table.
UNENCODED = ["L", "M"]

# The properties of an operator that a category may list, each with its flag.
PROPERTIES = {
    "stretchy": "@TOperatorFlag_Stretchy",
    "symmetric": "@TOperatorFlag_Symmetric",
    "largeop": "@TOperatorFlag_LargeOp",
    "movablelimits": "@TOperatorFlag_MovableLimits",
}


def code_points(text):
    """Returns the code points of the `{U+XXXX}`, `[U+XXXX–U+YYYY]` and bare `U+XXXX` entries in `text`, in order."""
    out = []
    for first, last in re.findall(r"U\+([0-9A-F]+)(?:–U\+([0-9A-F]+))?", text):
        out += range(int(first, 16), int(last or first, 16) + 1)
    return out


def figure(text, ident):
    """Returns the HTML of the figure whose id is `ident`."""
    start = text.index('<figure id="%s"' % ident)
    return text[start:text.index("</figure>", start)]


def read_dictionary(path):
    """Returns the dictionary: the strings of two ASCII characters, the fences, the separators, each category's
    (form, encoding, lspace, rspace, properties), and each (key, form) entry's category."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    special = figure(text, "operator-dictionary-compact-special-tables")
    pairs = html.unescape(re.search(r"Operators_2_ascii_chars</code></td><td>[^:]*:\s*<code>(.*?)</code>",
                                    special, re.S).group(1))
    ascii_pairs = re.findall(r"'(..)'", pairs)
    fences = code_points(re.search(r"Operators_fence</code></td><td>(.*?)</td>", special, re.S).group(1))
    separators = code_points(re.search(r"Operators_separator</code></td><td>(.*?)</td>", special, re.S).group(1))

    values = {}
    for row in re.findall(r"<tr><td>(\w+)</td><td>([\w/]+)</td><td>([\w/]+)</td><td><code>([^<]*)</code></td>"
                          r"<td><code>([^<]*)</code></td><td>([^<]*)</td></tr>",
                          figure(text, "operator-dictionary-categories-values")):
        name, form, encoding, lspace, rspace, properties = row
        listed = [] if properties.strip() == "N/A" else properties.split()
        unknown = [p for p in listed if p not in PROPERTIES]
        if unknown:
            raise SystemExit("gen_mathml: category %s lists properties that PROPERTIES lacks: %s" % (name, unknown))
        values[name] = (form, None if encoding == "N/A" else int(encoding, 16), em(lspace), em(rspace), listed)

    entries = {}
    for body, category in re.findall(r"<tr><td>(.*?)</td><td>([A-Z])</td></tr>",
                                     figure(text, "operator-dictionary-category-table"), re.S):
        form = re.search(r"<strong>(\w+)</strong>", body).group(1)
        for key in code_points(re.search(r"<code>(.*)</code>", body, re.S).group(1)):
            entries[(key, form)] = category
    check_encoded(text, values, entries)
    return ascii_pairs, fences, separators, values, entries


def em(text):
    """Returns a length of the categories table, a number of ems or 0, in eighteenths of an em. Every length of the
    table is a whole number of eighteenths, the unit of MathML's named spaces (mathml4/#type_length)."""
    text = text.strip()
    eighteenths = (float(text[:-2]) if text.endswith("em") else float(text)) * 18
    if abs(eighteenths - round(eighteenths)) > 1e-6:
        raise SystemExit("gen_mathml: %s is not a whole number of eighteenths of an em" % text)
    return int(round(eighteenths))


def check_encoded(text, values, entries):
    """Exits unless the entries of the encoded categories are those of the spec's table of encoded entries
    (mathml-core/#operator-dictionary-categories-hexa-table), so that the two presentations of the dictionary agree.
    An encoded entry is the category's encoding shifted left 12 bits above the key's index, which is the code point,
    or the code point less 0x1C00 from U+2000. The encoding's two low bits are the form."""
    listed = re.search(r"<code>(.*?)</code>", figure(text, "operator-dictionary-categories-hexa-table"), re.S).group(1)
    spec = set(code_points(listed.replace("0x", "U+")))
    ours = set()
    for (key, _), category in entries.items():
        encoding = values[category][1]
        if encoding is not None:
            ours.add(encoding << 12 | (key if key < 0x400 else key - 0x1C00))
    if spec != ours:
        raise SystemExit("gen_mathml: the category table and the encoded table disagree on %d entries"
                         % len(spec ^ ours))


def read_inline_axis(path):
    """Returns the code points whose intrinsic stretch axis is inline."""
    with open(path, encoding="utf-8") as f:
        return code_points(f.read())


def read_combining(path):
    """Returns (spacing, combining, above) for each row of the combining characters' table, in order."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    rows = re.findall(r"<tr><td>(above|below)</td><td>U\+([0-9A-F]+)</td><td>[^<]*</td><td>U\+([0-9A-F]+)</td>", text)
    return [(int(spacing, 16), int(combining, 16), style == "above") for style, spacing, combining in rows]


def read_assemblies(path):
    """Returns (base, horizontal, extender, start, middle, end) for each glyph assembly, where start is the bottom or
    left character, end the top or right one, and a missing part is 0."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    out = []
    for row in re.findall(r"<tr>(.*?)</tr>", text, re.S):
        cells = [re.sub(r"<[^>]+>", "", c).strip() for c in re.findall(r"<td>(.*?)</td>", row, re.S)]
        if len(cells) != 6:
            continue

        def part(cell):
            found = re.match(r"U\+([0-9A-F]+)", cell)
            return int(found.group(1), 16) if found else 0
        out.append((part(cells[0]), cells[1] == "Horizontal", part(cells[2]), part(cells[3]), part(cells[4]),
                    part(cells[5])))
    return out


def read_accents(path):
    """Returns the code points that MathML 3's dictionary gives the accent property, in any form."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    out = set()
    for row in re.findall(r"<tr>(.*?)</tr>", text, re.S):
        cells = [html.unescape(re.sub(r"<[^>]+>", "", c)).strip() for c in re.findall(r"<t[hd][^>]*>(.*?)</t[hd]>",
                                                                                         row, re.S)]
        if len(cells) < 8 or "accent" not in [p.strip() for p in cells[-1].split(",")]:
            continue
        character = cells[0]
        found = re.fullmatch(r"&#x([0-9A-Fa-f]+);", character)
        if found:
            out.add(int(found.group(1), 16))
        elif len(character) == 1:
            out.add(ord(character))
        else:
            raise SystemExit("gen_mathml: MathML 3's dictionary lists an accent that is not one character: %r"
                             % character)
    return sorted(out)


def ranges(keys):
    """Returns the runs of consecutive keys as (first, length), each at most 256 long."""
    out = []
    for key in sorted(keys):
        if out and out[-1][0] + out[-1][1] == key and out[-1][1] < 256:
            out[-1] = (out[-1][0], out[-1][1] + 1)
        else:
            out.append((key, 1))
    return out


class Names:
    """The spelling of the generated names in one library: `types` begins a type or an enum value, and `functions` a
    function. A full dictionary also has the fences, the separators and the combining accents, which only a layout
    reads."""

    def __init__(self, types, functions, full):
        self.types = types
        self.functions = functions
        self.full = full

    def __call__(self, text):
        """Returns `text` with @T spelled as the type prefix and @f as the function prefix."""
        return text.replace("@T", self.types).replace("@f", self.functions)


# lib/mathml lays out operators, and lib/tex writes the attributes in which TeX's spacing differs from the dictionary.
# lib/tex depends on nothing but core, so it has a dictionary of its own, generated from the same tables.
MATHML = Names("MathMl", "mathMl", True)
TEX = Names("Tex", "tex", False)


def category_ident(n, name):
    return n("@TOperatorCategory_") + name


def emit_header(path, values, n, title):
    categories = ["Default", "ForceDefault"] + sorted(name for name in values if name not in ("Default", "ForceDefault"))
    head = [BANNER, "\n// %s\n//\n// mathml-core - https://w3c.github.io/mathml-core/\n" % title, "\n#pragma once\n",
            '\n#include "pr/base.h"\n']
    body = [n("\n// The form of an operator: its position among its operands (mathml-core/#dfn-form).\nenum {\n"
              "    @TForm_Infix,\n    @TForm_Prefix,\n    @TForm_Postfix,\n};\n"
              "typedef uint8_t @TForm;\n"),
            n("\n// The properties of an operator that are true or false (mathml-core/#dictionary-based-attributes).\n"
              "enum {\n"
              "    @TOperatorFlag_Stretchy = 1u << 0,\n"
              "    @TOperatorFlag_Symmetric = 1u << 1,\n"
              "    @TOperatorFlag_LargeOp = 1u << 2,\n"
              "    @TOperatorFlag_MovableLimits = 1u << 3,\n"
              "    @TOperatorFlag_Fence = 1u << 4,\n"
              "    @TOperatorFlag_Separator = 1u << 5,\n"
              "};\ntypedef uint8_t @TOperatorFlags;\n"),
            "\n// The categories of the operator dictionary (mathml-core/#operator-dictionary-categories-values). An "
            "operator that the\n// dictionary does not list is Default.\nenum {\n"]
    body += ["    %s,\n" % category_ident(n, name) for name in categories]
    body.append(n("};\ntypedef uint8_t @TOperatorCategory;\n"))
    body.append(n("\n// The values that a category gives an operator: its leading and trailing space in eighteenths of "
                  "an em, and its\n// properties.\nstruct @TCategoryValues {\n    uint8_t lspace;\n    uint8_t rspace;\n"
                  "    @TOperatorFlags flags;\n};\n"))
    body.append(n("\n// Returns the code point that stands for the string of ASCII characters `first` and `second` if "
                  "the dictionary\n// lists it (Operators_2_ascii_chars), or 0. The code point is U+0320 plus the "
                  "index of the string in the list.\nuint32_t @fDictionaryAsciiPair(char first, char second);\n"))
    body.append(n("\n// Returns the category of the operator whose content is `key` in `form`, from the dictionary's "
                  "tables. `key` is a\n// code point, or the code point that stands for a pair of ASCII characters. An "
                  "entry that the tables lack is\n// Default. This is the table lookup of "
                  "mathml-core/#operator-dictionary, without its special cases of content and\n// form.\n"
                  "@TOperatorCategory @fDictionaryCategory(uint32_t key, @TForm form);\n"))
    body.append(n("\n// The values of `category`.\n@TCategoryValues @fCategoryValues(@TOperatorCategory "
                  "category);\n"))
    if n.full:
        body.append(n("\n// Returns true if the dictionary makes `codePoint` a fence (Operators_fence) or a separator "
                      "(Operators_separator).\nbool @fDictionaryIsFence(uint32_t codePoint);\n"
                      "bool @fDictionaryIsSeparator(uint32_t codePoint);\n"))
    body.append(n("\n// Returns true if the intrinsic stretch axis of `codePoint` is inline "
                  "(mathml-core/#stretchy-operator-axis).\nbool @fIsInlineAxis(uint32_t codePoint);\n"))
    if n.full:
        body.append(n("\n// A spacing character and a combining character that stand for the same accent in an "
                      "underscript or overscript\n// (mathml-core/#combining-character-equivalences).\n"
                      "struct @TCombiningAccent {\n    uint32_t spacing;\n    uint32_t combining;\n"
                      "    bool above;                 // the combining character sits above its base, not below\n};\n"))
        body.append(n("\n// The pairs of spacing and combining accents, ordered by spacing character.\n"
                      "extern const @TCombiningAccent k@TCombiningAccents[];\n"
                      "extern const uint32_t k@TCombiningAccentCount;\n"))
        body.append(n("\n// Returns true if MathML 3's dictionary makes `codePoint` an accent in any form, which is the "
                      "default of the accent\n// attribute of an mo (mathml4/#presm_mo).\n"
                      "bool @fDictionaryIsAccent(uint32_t codePoint);\n"))
    write(path, namespaced(head, body))
    return categories


def emit_source(path, n, categories, ascii_pairs, fences, separators, values, entries, inline_axis, combining,
                accents):
    head = [BANNER, '\n#include "%s"\n' % include_path(path, ".h"), '\n#include "pr/count_of.h"\n'
            '#include "pr/search.h"\n']
    body = ["\nnamespace {\n"]
    body.append("\n// The strings of two ASCII characters that the dictionary lists, in its order.\n")
    body.append(table("constexpr char kAsciiPairs[][2]", ["{%s}" % ", ".join(gen_common.c_string(c)[1:-1].join("''")
                                                                        for c in pair) for pair in ascii_pairs], 6))
    # Each entry is a run of keys of one category: (form << 18 | first key) << 8 | length - 1, and its category.
    keyed = {}
    for (key, form), category in entries.items():
        keyed.setdefault(category, []).append(FORMS.index(form) << 18 | key)
    runs = []
    for category, keys in keyed.items():
        for first, length in ranges(keys):
            runs.append((first, length, category))
    runs.sort()
    body.append("\n// The first key of each run of keys that share a category, with the form shifted left 18 bits "
                "above it, in order.\n")
    body.append(table("constexpr uint32_t kRunFirst[]", ["0x%05x" % first for first, _, _ in runs], 8))
    body.append("\n// The length of each run less one.\n")
    body.append(table("constexpr uint8_t kRunLength[]", ["%d" % (length - 1) for _, length, _ in runs], 16))
    body.append("\n// The category of each run.\n")
    body.append(table(n("constexpr @TOperatorCategory kRunCategory[]"),
                      [category_ident(n, category) for _, _, category in runs], 4))
    body.append(n("\n// The values of each category, by @TOperatorCategory.\n"))
    rows = []
    for name in categories:
        _, _, lspace, rspace, listed = values[name]
        flags = " | ".join(n(PROPERTIES[p]) for p in listed) or "0"
        rows.append("{%d, %d, %s}" % (lspace, rspace, flags))
    body.append(table(n("constexpr @TCategoryValues kCategoryValues[]"), rows, 1))
    if n.full:
        body.append("\n// The runs of fences: the first code point of each, and its length less one.\n")
        fence_runs = ranges(fences)
        body.append(table("constexpr uint32_t kFenceFirst[]", ["0x%04x" % f for f, _ in fence_runs], 8))
        body.append(table("constexpr uint8_t kFenceLength[]", ["%d" % (length - 1) for _, length in fence_runs], 16))
        body.append("\n// The separators, in order.\n")
        body.append(table("constexpr uint32_t kSeparators[]", ["0x%04x" % s for s in sorted(separators)], 8))
        body.append("\n// The accents, in order.\n")
        body.append(table("constexpr uint32_t kAccents[]", ["0x%04x" % a for a in accents], 8))
    body.append("\n// The code points whose intrinsic stretch axis is inline, in order.\n")
    body.append(table("constexpr uint32_t kInlineAxis[]", ["0x%05x" % c for c in sorted(inline_axis)], 8))
    body.append("\n} // namespace\n")
    if n.full:
        body.append(table(n("const @TCombiningAccent k@TCombiningAccents[]"),
                          ["{0x%04x, 0x%04x, %s}" % (spacing, comb, "true" if above else "false")
                           for spacing, comb, above in sorted(combining)], 3))
        body.append(n("\nconst uint32_t k@TCombiningAccentCount = uint32_t(countOf(k@TCombiningAccents));\n"))
    body.append(n("\nuint32_t @fDictionaryAsciiPair(char first, char second) {\n"
                  "    for (uint32_t i = 0; i < countOf(kAsciiPairs); ++i) {\n"
                  "        if (kAsciiPairs[i][0] == first && kAsciiPairs[i][1] == second)\n"
                  "            return 0x320 + i;\n    }\n    return 0;\n}\n"))
    body.append(n("\n@TOperatorCategory @fDictionaryCategory(uint32_t key, @TForm form) {\n"
                  "    uint32_t wanted = uint32_t(form) << 18 | key;\n"
                  "    uint32_t at = upperBound(Span(kRunFirst), wanted);\n"
                  "    if (at == 0 || wanted - kRunFirst[at - 1] > kRunLength[at - 1])\n"
                  "        return @TOperatorCategory_Default;\n"
                  "    return kRunCategory[at - 1];\n}\n"))
    body.append(n("\n@TCategoryValues @fCategoryValues(@TOperatorCategory category) {\n"
                  "    return kCategoryValues[category];\n}\n"))
    if n.full:
        body.append(n("\nbool @fDictionaryIsFence(uint32_t codePoint) {\n"
                      "    uint32_t at = upperBound(Span(kFenceFirst), codePoint);\n"
                      "    return at != 0 && codePoint - kFenceFirst[at - 1] <= kFenceLength[at - 1];\n}\n"))
        body.append(n("\nbool @fDictionaryIsSeparator(uint32_t codePoint) {\n"
                      "    uint32_t at = lowerBound(Span(kSeparators), codePoint);\n"
                      "    return at != countOf(kSeparators) && kSeparators[at] == codePoint;\n}\n"))
    body.append(n("\nbool @fIsInlineAxis(uint32_t codePoint) {\n"
                  "    uint32_t at = lowerBound(Span(kInlineAxis), codePoint);\n"
                  "    return at != countOf(kInlineAxis) && kInlineAxis[at] == codePoint;\n}\n"))
    if n.full:
        body.append(n("\nbool @fDictionaryIsAccent(uint32_t codePoint) {\n"
                      "    uint32_t at = lowerBound(Span(kAccents), codePoint);\n"
                      "    return at != countOf(kAccents) && kAccents[at] == codePoint;\n}\n"))
    write(path, namespaced(head, body))


def emit_assemblies(base, assemblies):
    """Writes the glyph assemblies that Unicode's characters build to lib/font, which stretches by them."""
    # lib/font names no spec, so the table is described in its own terms.
    head = [BANNER, "\n// The glyph assemblies that Unicode's characters build, as MathML Core lists them\n",
            "\n#pragma once\n", '\n#include "pr/base.h"\n']
    body = ["\n// An assembly of the Unicode characters for the parts of stretched operators, which stretches an "
            "operator in a\n// font without a MATH table. A missing part is 0.\n"
            "struct FontMathUnicodeAssembly {\n    uint32_t base;\n"
            "    uint32_t extender;\n    uint32_t start;             // the bottom or left part\n"
            "    uint32_t middle;\n    uint32_t end;               // the top or right part\n"
            "    bool horizontal;            // the assembly stretches along the inline axis\n};\n",
            "\n// Returns the assembly of `codePoint`, or null if Unicode's characters build none.\n"
            "const FontMathUnicodeAssembly* PR_NULLABLE fontMathUnicodeAssembly(uint32_t codePoint);\n"]
    write(base + ".h", namespaced(head, body))
    head = [BANNER, '\n#include "%s"\n' % include_path(base + ".cpp", ".h"), '\n#include "pr/count_of.h"\n'
            '#include "pr/search.h"\n']
    rows = ["{0x%04x, 0x%04x, 0x%04x, 0x%04x, 0x%04x, %s}" % (first, extender, start, middle, end,
                                                             "true" if horizontal else "false")
            for first, horizontal, extender, start, middle, end in sorted(assemblies)]
    body = ["\nnamespace {\n", "\n// The glyph assemblies, ordered by base character.\n",
            table("constexpr FontMathUnicodeAssembly kAssemblies[]", rows, 2),
            table("constexpr uint32_t kAssemblyBases[]", ["0x%04x" % a[0] for a in sorted(assemblies)], 8),
            "\n} // namespace\n",
            "\nconst FontMathUnicodeAssembly* PR_NULLABLE fontMathUnicodeAssembly(uint32_t codePoint) {\n"
            "    uint32_t at = lowerBound(Span(kAssemblyBases), codePoint);\n"
            "    return at != countOf(kAssemblyBases) && kAssemblyBases[at] == codePoint ? &kAssemblies[at] : "
            "nullptr;\n}\n"]
    write(base + ".cpp", namespaced(head, body))


def include_path(path, extension):
    """Returns the include path of the file beside `path` with `extension`, as <module>/src/<name><extension>."""
    parts = os.path.normpath(path).split(os.sep)
    return "/".join(parts[-3:-1] + [os.path.splitext(parts[-1])[0] + extension])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tables", default=os.path.join(gen_common.ROOT, "third_party", "mathml"))
    ap.add_argument("--out", default=os.path.join(gen_common.ROOT, "lib"))
    args = ap.parse_args()

    ascii_pairs, fences, separators, values, entries = read_dictionary(
        os.path.join(args.tables, "operator-dictionary-compact.html"))
    inline_axis = read_inline_axis(os.path.join(args.tables, "inline-axis-operators.txt"))
    combining = read_combining(os.path.join(args.tables, "combining-operators.html"))
    assemblies = read_assemblies(os.path.join(args.tables, "unicode-assembly.html"))
    accents = read_accents(os.path.join(args.tables, "mathml3-operator-dictionary.html"))
    outputs = ((MATHML, "mathml/src/mathml_dictionary", "MathML's operator dictionary and the tables that stretch "
                "operators"),
               (TEX, "tex/src/tex_dictionary", "MathML's operator dictionary, against which TeX's spacing is "
                "compared"))
    for names, stem, title in outputs:
        base = os.path.join(args.out, *stem.split("/"))
        categories = emit_header(base + ".h", values, names, title)
        emit_source(base + ".cpp", names, categories, ascii_pairs, fences, separators, values, entries, inline_axis,
                    combining, accents)
    emit_assemblies(os.path.join(args.out, "font", "src", "font_math_unicode"), assemblies)
    print("%d entries, %d fences, %d separators, %d inline-axis, %d combining accents, %d accents, %d assemblies"
          % (len(entries), len(fences), len(separators), len(inline_axis), len(combining), len(accents),
             len(assemblies)))


if __name__ == "__main__":
    main()
