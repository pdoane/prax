#!/usr/bin/env python3
"""Code generator for the Unicode character properties that the engine reads.

Reads the Unicode Character Database and emits:

    unicode_bidi_data.cpp
                        the properties that unicode_bidi.h declares: the bidirectional class of every code point and the
                        paired brackets
    unicode_line_break_data.cpp
                        the properties that unicode_line_break.h declares: the line breaking class of every code point
                        and the other properties that the rules read
    unicode_staged.h    the lookup that reads a property's value for a code point from its three stages
    unicode_data.cpp    the properties that unicode.h declares: grapheme breaks, emoji, case and mark properties,
                        scripts, joining types, vertical orientations, general categories, combining classes,
                        uppercase and lowercase mappings and case foldings
    unicode_normalization_data.cpp
                        the canonical decompositions and primary composites that normalization reads
    unicode_idna_data.cpp
                        the IDNA Mapping Table of UTS #46

Usage: python3 -m tools.gen.gen_unicode [--ucd DIR] [--out DIR]
"""

import argparse
import os
import re

from tools.gen import gen_common
from tools.gen import gen_locale
from tools.gen.gen_common import namespaced, table, write

UCD_URL = "https://www.unicode.org/Public/17.0.0/ucd/"
BANNER = gen_common.banner(__file__, UCD_URL + "extracted/DerivedBidiClass.txt",
                           UCD_URL + "BidiBrackets.txt", UCD_URL + "UnicodeData.txt")

LINE_BREAK_BANNER = gen_common.banner(__file__, UCD_URL + "LineBreak.txt",
                                      UCD_URL + "EastAsianWidth.txt",
                                      UCD_URL + "extracted/DerivedGeneralCategory.txt",
                                      UCD_URL + "emoji/emoji-data.txt")

# The Line_Break values, in PrUnicodeLineBreakClass's order.
LINE_BREAK_CLASSES = ["BK", "CR", "LF", "NL", "SP", "ZW", "ZWJ", "CM", "WJ", "GL", "BA", "BB",
                      "B2", "HY", "HH", "CB", "CL", "CP", "EX", "IN", "NS", "OP", "QU", "IS", "NU",
                      "PO", "PR", "SY", "AI", "AK", "AL", "AP", "AS", "CJ", "EB", "EM", "H2", "H3",
                      "HL", "ID", "JL", "JV", "JT", "RI", "SA", "SG", "VF", "VI", "XX"]

# The flags stored beside a line breaking class, named as in PrUnicodeLineBreakFlags, mapped to their bit positions.
LINE_BREAK_FLAGS = {"InitialQuote": 0, "FinalQuote": 1, "EastAsian": 2, "Mark": 3,
                    "UnassignedPictographic": 4}

DATA_BANNER = gen_common.banner(__file__, UCD_URL + "auxiliary/GraphemeBreakProperty.txt",
                                UCD_URL + "emoji/emoji-data.txt",
                                UCD_URL + "DerivedCoreProperties.txt",
                                UCD_URL + "Scripts.txt", UCD_URL + "PropertyValueAliases.txt",
                                UCD_URL + "extracted/DerivedJoiningType.txt",
                                UCD_URL + "UnicodeData.txt", UCD_URL + "SpecialCasing.txt",
                                UCD_URL + "VerticalOrientation.txt",
                                UCD_URL + "extracted/DerivedGeneralCategory.txt",
                                UCD_URL + "CaseFolding.txt", UCD_URL + "PropList.txt")

NORMALIZATION_BANNER = gen_common.banner(__file__, UCD_URL + "UnicodeData.txt", UCD_URL + "CompositionExclusions.txt")

IDNA_URL = "https://www.unicode.org/Public/17.0.0/idna/"
IDNA_BANNER = gen_common.banner(__file__, IDNA_URL + "IdnaMappingTable.txt")

# The Status values of the IDNA Mapping Table, in IdnaStatus's order.
IDNA_STATUS = ["valid", "ignored", "mapped", "deviation", "disallowed"]

GRAPHEME = ["Other", "CR", "LF", "Control", "Extend", "ZWJ", "Regional_Indicator", "Prepend",
            "SpacingMark", "L", "V", "T", "LV", "LVT"]
GRAPHEME_MEMBER = {name: "PrUnicodeGraphemeBreak_" + name.replace("_", "") for name in GRAPHEME}

# The Word_Break values, in PrUnicodeWordBreak's order.
WORD = ["Other", "CR", "LF", "Newline", "Extend", "ZWJ", "Format", "Katakana", "Hebrew_Letter", "ALetter", "Single_Quote",
        "Double_Quote", "MidNumLet", "MidLetter", "MidNum", "Numeric", "ExtendNumLet", "Regional_Indicator", "WSegSpace"]
WORD_MEMBER = {name: "PrUnicodeWordBreak_" + name.replace("_", "") for name in WORD}

INCB = {"None": "PrUnicodeIndicConjunctBreak_None", "Linker": "PrUnicodeIndicConjunctBreak_Linker",
        "Consonant": "PrUnicodeIndicConjunctBreak_Consonant", "Extend": "PrUnicodeIndicConjunctBreak_Extend"}

EMOJI_FLAGS = {"Emoji": "PrUnicodeFlag_Emoji", "Emoji_Presentation": "PrUnicodeFlag_EmojiPresentation",
               "Emoji_Modifier": "PrUnicodeFlag_EmojiModifier",
               "Emoji_Modifier_Base": "PrUnicodeFlag_EmojiModifierBase",
               "Emoji_Component": "PrUnicodeFlag_EmojiComponent",
               "Extended_Pictographic": "PrUnicodeFlag_ExtendedPictographic"}
FLAG_BITS = {"Emoji": 0, "Emoji_Presentation": 1, "Emoji_Modifier": 2, "Emoji_Modifier_Base": 3,
             "Emoji_Component": 4, "Extended_Pictographic": 5, "Lowercase": 6, "Uppercase": 7,
             "Default_Ignorable_Code_Point": 8, "Mark": 9, "Cased": 10, "Case_Ignorable": 11, "Soft_Dotted": 12,
             "Variation_Selector": 13}

MAX_CODE_POINT = 0x10FFFF

# The Bidi_Class values, by the short names that DerivedBidiClass.txt uses, in PrUnicodeBidiClass's order.
BIDI_CLASSES = ["L", "R", "AL", "EN", "ES", "ET", "AN", "CS", "NSM", "BN", "B", "S", "WS", "ON",
                "LRE", "LRO", "RLE", "RLO", "PDF", "LRI", "RLI", "FSI", "PDI"]

# The short names of the Bidi_Class values, keyed by the long names that @missing lines use.
BIDI_LONG = {"Left_To_Right": "L", "Right_To_Left": "R", "Arabic_Letter": "AL",
             "European_Number": "EN", "European_Terminator": "ET", "Arabic_Number": "AN",
             "Boundary_Neutral": "BN", "Other_Neutral": "ON", "Nonspacing_Mark": "NSM"}

# The Vertical_Orientation values (UAX50) as PrUnicodeVerticalOrientation members, in the enum's order.
VERTICAL = {"U": "PrUnicodeVerticalOrientation_Upright", "R": "PrUnicodeVerticalOrientation_Rotated",
            "Tu": "PrUnicodeVerticalOrientation_TransformedUpright",
            "Tr": "PrUnicodeVerticalOrientation_TransformedRotated"}

# The Joining_Type values as PrUnicodeJoiningType members, keyed by the short names that the data lines of
# DerivedJoiningType.txt use and the long name that its @missing line uses.
JOINING = {"U": "PrUnicodeJoiningType_NonJoining", "Non_Joining": "PrUnicodeJoiningType_NonJoining",
           "R": "PrUnicodeJoiningType_Right", "L": "PrUnicodeJoiningType_Left", "D": "PrUnicodeJoiningType_Dual",
           "C": "PrUnicodeJoiningType_JoinCausing", "T": "PrUnicodeJoiningType_Transparent"}

# The General_Category values, by their short names, in PrUnicodeGeneralCategory's order, each with the name of its
# member.
GENERAL_CATEGORIES = [
    ("Lu", "UppercaseLetter"), ("Ll", "LowercaseLetter"), ("Lt", "TitlecaseLetter"), ("Lm", "ModifierLetter"),
    ("Lo", "OtherLetter"), ("Mn", "NonspacingMark"), ("Mc", "SpacingMark"), ("Me", "EnclosingMark"),
    ("Nd", "DecimalNumber"), ("Nl", "LetterNumber"), ("No", "OtherNumber"), ("Pc", "ConnectorPunctuation"),
    ("Pd", "DashPunctuation"), ("Ps", "OpenPunctuation"), ("Pe", "ClosePunctuation"),
    ("Pi", "InitialPunctuation"), ("Pf", "FinalPunctuation"), ("Po", "OtherPunctuation"), ("Sm", "MathSymbol"),
    ("Sc", "CurrencySymbol"), ("Sk", "ModifierSymbol"), ("So", "OtherSymbol"), ("Zs", "SpaceSeparator"),
    ("Zl", "LineSeparator"), ("Zp", "ParagraphSeparator"), ("Cc", "Control"), ("Cf", "Format"),
    ("Cs", "Surrogate"), ("Co", "PrivateUse"), ("Cn", "Unassigned")]

MISSING = re.compile(r"#\s*@missing:\s*([0-9A-F]+)\.\.([0-9A-F]+);\s*(\w+)")


def read(path):
    """Returns the Bidi_Class of every code point. The @missing defaults are applied first, in the order that the file
    gives them, from the widest to the narrowest, and then every listed range is applied over them."""
    classes = [None] * (MAX_CODE_POINT + 1)
    listed = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            missing = MISSING.match(line)
            if missing:
                first, last = int(missing.group(1), 16), int(missing.group(2), 16)
                value = BIDI_LONG.get(missing.group(3), missing.group(3))
                classes[first:last + 1] = [value] * (last - first + 1)
                continue
            data = line.split("#", 1)[0].strip()
            if not data:
                continue
            points, value = (part.strip() for part in data.split(";"))
            first, _, last = points.partition("..")
            listed.append((int(first, 16), int(last or first, 16), value))
    for first, last, value in listed:
        classes[first:last + 1] = [value] * (last - first + 1)
    if None in classes:
        raise SystemExit("gen_unicode: %s leaves U+%04X without a Bidi_Class"
                         % (path, classes.index(None)))
    return classes


def singletons(unicode_data):
    """Returns, for each code point that has a singleton canonical decomposition, the code point that it decomposes
    to."""
    out = {}
    with open(unicode_data, encoding="utf-8") as f:
        for line in f:
            fields = line.split(";")
            parts = fields[5].split()
            if len(parts) == 1 and not parts[0].startswith("<"):
                out[int(fields[0], 16)] = int(parts[0], 16)
    return out


def brackets(path, canonical):
    """Returns each paired bracket, in order: its code point, whether it opens, and the key that pairs it. The key is an
    opening bracket's closing bracket, or a closing bracket itself, canonically decomposed so that canonically
    equivalent brackets pair (UAX #9 BD16)."""
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            data = line.split("#", 1)[0].strip()
            if not data:
                continue
            point, pair, kind = (part.strip() for part in data.split(";"))
            code_point, pair = int(point, 16), int(pair, 16)
            opens = kind == "o"
            key = pair if opens else code_point
            out.append((code_point, opens, canonical.get(key, key)))
    return out


def emit_source(path, classes, bracket_list):
    index = {name: i for i, name in enumerate(BIDI_CLASSES)}
    head = [BANNER, '\n#include "pr/unicode_bidi.h"\n\n#include "pr/count_of.h"\n'
            '#include "pr/search.h"\n\n#include "unicode/src/unicode_staged.h"\n']
    body = ["\nnamespace {\n",
            "\n" + enum_order(["PrUnicodeBidiClass_" + name for name in BIDI_CLASSES]),
            "\n// The class of every code point.\n",
            staged("BidiClass", [index[c] for c in classes], "uint8_t", "%d", 32),
            "\n// The paired brackets, in order, each with the key that pairs it above its "
            "code point.\n",
            table("constexpr uint32_t kBidiBrackets[]",
                  ["0x%06x" % cp for cp, _, _ in bracket_list], 8),
            table("constexpr uint32_t kBidiBracketKeys[]",
                  ["0x%06x << 1 | %d" % (key, 1 if opens else 0)
                   for _, opens, key in bracket_list], 5),
            "\n} // namespace\n",
            "\nPrUnicodeBidiClass prUnicodeBidiClass(uint32_t codePoint) {\n"
            "    return PrUnicodeBidiClass(unicodeStagedValue(kBidiClassTop, kBidiClassMiddle, kBidiClassLeaves, codePoint));\n}\n",
            "\nPrUnicodeBidiBracket prUnicodeBidiBracket(uint32_t codePoint, uint32_t* key) {\n"
            "    uint32_t at = lowerBound(kBidiBrackets, uint32_t(countOf(kBidiBrackets)), "
            "codePoint);\n"
            "    if (at == countOf(kBidiBrackets) || kBidiBrackets[at] != codePoint)\n"
            "        return PrUnicodeBidiBracket_None;\n"
            "    *key = kBidiBracketKeys[at] >> 1;\n"
            "    return (kBidiBracketKeys[at] & 1) != 0 ? PrUnicodeBidiBracket_Open : PrUnicodeBidiBracket_Close;\n"
            "}\n"]
    write(path, namespaced(head, body, public=True))


def ranges(path, field=None):
    """Returns (first, last, value) for each data line of a UCD file. The value is the second field. If the file holds
    several properties and `field` names one of them, the value is the third field of the lines that name that
    property."""
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            data = line.split("#", 1)[0].strip()
            if not data:
                continue
            parts = [part.strip() for part in data.split(";")]
            if field is not None:
                if parts[1] != field:
                    continue
                value = parts[2] if len(parts) > 2 else True
            else:
                value = parts[1]
            first, _, last = parts[0].partition("..")
            out.append((int(first, 16), int(last or first, 16), value))
    return out


def fill(default, spans):
    """Returns the value of every code point: `default`, overwritten by each span in turn."""
    values = [default] * (MAX_CODE_POINT + 1)
    for first, last, value in spans:
        values[first:last + 1] = [value] * (last - first + 1)
    return values


# A property's values for all code points are stored in three stages, so that looking up any code point takes three
# loads. The top stage, indexed by the code point's bits above the other two stages, names a middle block. The middle
# block, indexed by the next STAGE_MIDDLE_BITS, names a leaf block. The leaf block holds the values of
# 1 << STAGE_LEAF_BITS code points. Equal blocks are stored once.
STAGE_LEAF_BITS = 4
STAGE_MIDDLE_BITS = 5


def index_type(count):
    """Returns the narrowest unsigned type that can index `count` blocks."""
    return "uint8_t" if count <= 0x100 else "uint16_t" if count <= 0x10000 else "uint32_t"


def staged(prefix, values, value_type, value_format, per_line):
    """Returns the definitions of k<prefix>Top, k<prefix>Middle and k<prefix>Leaves, which hold `values`, one per code
    point, in the form that unicodeStagedValue reads."""
    leaf_size = 1 << STAGE_LEAF_BITS
    middle_size = 1 << STAGE_MIDDLE_BITS
    leaves = {}
    leaf_of = []
    for at in range(0, len(values), leaf_size):
        block = tuple(values[at:at + leaf_size])
        block += (block[-1],) * (leaf_size - len(block))
        leaf_of.append(leaves.setdefault(block, len(leaves)))
    middles = {}
    top = []
    for at in range(0, len(leaf_of), middle_size):
        block = tuple(leaf_of[at:at + middle_size])
        block += (block[-1],) * (middle_size - len(block))
        top.append(middles.setdefault(block, len(middles)))
    return "".join([
        table("constexpr %s k%sTop[]" % (index_type(len(middles)), prefix), top, 16),
        table("constexpr %s k%sMiddle[]" % (index_type(len(leaves)), prefix),
              [i for block in middles for i in block], 16),
        table("constexpr %s k%sLeaves[]" % (value_type, prefix),
              [value_format % v for block in leaves for v in block], per_line)])


# The header that the tables share, which reads a value from a property's stages.
STAGED_HEADER = (
    "\n#pragma once\n\n#include \"pr/base.h\"\n\nPR_ASSUME_NONNULL_BEGIN\n\nnamespace pr_impl {\n"
    "\n// Returns the value of the code point in a property's stages. A value past the last code point is the last "
    "code point's.\n"
    "template <typename Top, typename Middle, typename Leaf, size_t T, size_t M, size_t L>\n"
    "uint32_t unicodeStagedValue(const Top (&top)[T], const Middle (&middle)[M], const Leaf (&leaves)[L], "
    "uint32_t codePoint) {\n"
    "    uint32_t c = codePoint <= 0x%x ? codePoint : 0x%x;\n"
    "    uint32_t leaf = middle[uint32_t(top[c >> %d]) << %d | (c >> %d & 0x%x)];\n"
    "    return leaves[leaf << %d | (c & 0x%x)];\n}\n"
    % (MAX_CODE_POINT, MAX_CODE_POINT, STAGE_LEAF_BITS + STAGE_MIDDLE_BITS, STAGE_MIDDLE_BITS, STAGE_LEAF_BITS,
       (1 << STAGE_MIDDLE_BITS) - 1, STAGE_LEAF_BITS, (1 << STAGE_LEAF_BITS) - 1)
    + "\n} // namespace pr_impl\n\nPR_ASSUME_NONNULL_END\n")


def enum_order(members):
    """Returns static assertions that the members of a hand-written enum have the values that the tables use."""
    return "".join("static_assert(%s == %d);\n" % (member, i) for i, member in enumerate(members))


def script_codes(path):
    """Returns the ISO 15924 code for the long name of each Script value."""
    codes = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = [part.strip() for part in line.split("#", 1)[0].split(";")]
            if len(parts) >= 3 and parts[0] == "sc":
                codes[parts[2]] = parts[1]
    return codes


# The case mappings, as their fields of UnicodeData.txt and SpecialCasing.txt. An empty simple titlecase mapping is
# the simple uppercase mapping.
UPPERCASE = (12, 3)
LOWERCASE = (13, 1)
TITLECASE = (14, 2)


def case_mapping(unicode_data, special_casing, fields):
    """Returns the mapping to one case, UPPERCASE, LOWERCASE or TITLECASE, of every code point whose mapping differs
    from itself: its simple mapping, or the unconditional full mapping if SpecialCasing gives one."""
    simple, full = fields
    mappings = {}
    with open(unicode_data, encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").split(";")
            mapped = parts[simple] or (parts[UPPERCASE[0]] if fields == TITLECASE else "")
            if mapped:
                mappings[int(parts[0], 16)] = [int(mapped, 16)]
    with open(special_casing, encoding="utf-8") as f:
        for line in f:
            parts = [part.strip() for part in line.split("#", 1)[0].split(";")]
            if len(parts) < 5 or parts[4]:
                continue
            code_point = int(parts[0], 16)
            mapped = [int(x, 16) for x in parts[full].split()]
            if mapped != [code_point]:
                mappings[code_point] = mapped
            else:
                mappings.pop(code_point, None)
    return sorted(mappings.items())


def compatibility_forms(unicode_data):
    """Returns the full-width form of every code point that has one, as text-transform: full-width maps them
    (css-text-4/#valdef-text-transform-full-width): the code point whose <wide> decomposition is the code point, or
    the <narrow> decomposition of a half-width form."""
    forms = {}
    with open(unicode_data, encoding="utf-8") as f:
        for line in f:
            fields = line.split(";")
            parts = fields[5].split()
            if len(parts) != 2 or parts[0] not in ("<wide>", "<narrow>"):
                continue
            code_point, base = int(fields[0], 16), int(parts[1], 16)
            source, target = (base, code_point) if parts[0] == "<wide>" else (code_point, base)
            if forms.get(source, target) != target:
                raise SystemExit("gen_unicode: U+%04X has two full-width forms" % source)
            forms[source] = target
    return sorted(forms.items())


def small_kana(unicode_data):
    """Returns the full-size form of every small kana (css-text-4/#small-kana): the code point named as the small one
    is, without SMALL."""
    names = {}
    with open(unicode_data, encoding="utf-8") as f:
        for line in f:
            fields = line.split(";")
            names[fields[1]] = int(fields[0], 16)
    pattern = re.compile(r"^((?:HALFWIDTH )?(?:HIRAGANA|KATAKANA) LETTER )SMALL (.*)$")
    forms = {}
    for name, code_point in names.items():
        match = pattern.match(name)
        if match is None:
            continue
        target = names.get(match.group(1) + match.group(2))
        if target is None:
            raise SystemExit("gen_unicode: %s has no full-size form" % name)
        forms[code_point] = target
    return sorted(forms.items())


def math_italics(unicode_data):
    """Returns the Mathematical Italic form of every code point that has one: the code point whose <font>
    decomposition it is, among those named MATHEMATICAL ITALIC, and U+210E PLANCK CONSTANT, which stands in the hole
    for italic h. These are MathML Core's italic mappings (mathml-core/#italic-mappings)."""
    forms = {}
    with open(unicode_data, encoding="utf-8") as f:
        for line in f:
            fields = line.split(";")
            parts = fields[5].split()
            code_point = int(fields[0], 16)
            if len(parts) == 2 and parts[0] == "<font>" and (fields[1].startswith("MATHEMATICAL ITALIC ")
                                                              or code_point == 0x210E):
                forms[int(parts[1], 16)] = code_point
    return sorted(forms.items())


def case_folding(path):
    """Returns the full case folding of every code point that does not fold to itself: the mappings of status C, which
    the simple and full foldings share, and those of status F, which only the full folding has."""
    mappings = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = [part.strip() for part in line.split("#", 1)[0].split(";")]
            if len(parts) < 3 or parts[1] not in ("C", "F"):
                continue
            mappings[int(parts[0], 16)] = [int(x, 16) for x in parts[2].split()]
    if max(len(m) for m in mappings.values()) > 3:
        raise SystemExit("gen_unicode: %s folds a code point to more than three" % path)
    return sorted(mappings.items())


def mapping_lookup(function, prefix):
    """Returns the definition of `function`, which writes the mapping of a code point from the tables <prefix>From and
    <prefix>To, or the code point itself if they do not hold it."""
    return ("\nuint32_t %s(uint32_t codePoint, uint32_t out[3]) {\n" % function
            + "    uint32_t at = lowerBound(%sFrom, uint32_t(countOf(%sFrom)), codePoint);\n" % (prefix, prefix)
            + "    if (at == countOf(%sFrom) || %sFrom[at] != codePoint) {\n" % (prefix, prefix)
            + "        out[0] = codePoint;\n        return 1;\n    }\n    uint32_t count = 0;\n"
            + "    while (count < 3 && %sTo[at][count] != 0) {\n" % prefix
            + "        out[count] = %sTo[at][count];\n        ++count;\n    }\n" % prefix
            + "    return count;\n}\n")




def single_lookup(function, prefix):
    """Returns the definition of `function`, which returns the mapping of a code point from the tables <prefix>From
    and <prefix>To, or the code point itself if they do not hold it."""
    return ("\nuint32_t %s(uint32_t codePoint) {\n" % function
            + "    uint32_t at = lowerBound(%sFrom, uint32_t(countOf(%sFrom)), codePoint);\n" % (prefix, prefix)
            + "    return at != countOf(%sFrom) && %sFrom[at] == codePoint ? %sTo[at] : codePoint;\n}\n"
            % (prefix, prefix, prefix))


def canonical(unicode_data):
    """Returns the canonical decomposition mapping of each code point that has one, one level deep, and the
    Canonical_Combining_Class of each code point whose class is not zero."""
    decompositions = {}
    classes = {}
    with open(unicode_data, encoding="utf-8") as f:
        for line in f:
            fields = line.split(";")
            code_point = int(fields[0], 16)
            if fields[3] != "0":
                classes[code_point] = int(fields[3])
            parts = fields[5].split()
            if parts and not parts[0].startswith("<"):
                decompositions[code_point] = [int(part, 16) for part in parts]
    return decompositions, classes


def emit_normalization(path, ucd):
    decompositions, classes = canonical(os.path.join(ucd, "UnicodeData.txt"))

    def full(code_point):
        parts = decompositions.get(code_point)
        return [code_point] if parts is None else [c for part in parts for c in full(part)]

    excluded = set()
    with open(os.path.join(ucd, "CompositionExclusions.txt"), encoding="utf-8") as f:
        for line in f:
            data = line.split("#", 1)[0].strip()
            if data:
                first, _, last = data.partition("..")
                excluded.update(range(int(first, 16), int(last or first, 16) + 1))
    # A primary composite has a canonical decomposition of two code points, the first a starter, and is not in
    # Full_Composition_Exclusion: an exclusion that the file lists, a singleton, or a non-starter decomposition.
    composites = sorted((parts[0] << 21 | parts[1], code_point) for code_point, parts in decompositions.items()
                        if len(parts) == 2 and code_point not in excluded and code_point not in classes
                        and parts[0] not in classes)
    decomposed = sorted(decompositions)
    starts = [0]
    pool = []
    for code_point in decomposed:
        pool += full(code_point)
        starts.append(len(pool))
    head = [NORMALIZATION_BANNER, '\n#include "unicode/src/unicode_normalization.h"\n\n#include "pr/count_of.h"\n'
            '#include "pr/search.h"\n']
    body = ["\nnamespace {\n",
            "\n// The code points that have a canonical decomposition, in order, other than the Hangul syllables.\n",
            table("constexpr uint32_t kDecomposed[]", ["0x%05x" % cp for cp in decomposed], 8),
            "\n// Where the full canonical decomposition of each decomposed code point starts in kDecompositions. The "
            "next\n// start ends it.\n",
            table("constexpr uint16_t kDecompositionStarts[]", starts, 16),
            table("constexpr uint32_t kDecompositions[]", ["0x%05x" % cp for cp in pool], 8),
            "\n// The pairs that compose to a primary composite, as the first code point << 21 | the second, in order, "
            "and\n// the composite of each pair.\n",
            table("constexpr uint64_t kCompositionPairs[]", ["0x%011x" % key for key, _ in composites], 6),
            table("constexpr uint32_t kComposites[]", ["0x%05x" % cp for _, cp in composites], 8),
            "\n} // namespace\n",
            "\nuint32_t unicodeDecomposition(uint32_t codePoint, const uint32_t* PR_NULLABLE* PR_NONNULL out) {\n"
            "    uint32_t at = lowerBound(kDecomposed, uint32_t(countOf(kDecomposed)), codePoint);\n"
            "    if (at == countOf(kDecomposed) || kDecomposed[at] != codePoint)\n"
            "        return 0;\n"
            "    *out = kDecompositions + kDecompositionStarts[at];\n"
            "    return uint32_t(kDecompositionStarts[at + 1] - kDecompositionStarts[at]);\n}\n",
            "\nbool unicodeComposition(uint32_t first, uint32_t second, uint32_t* composite) {\n"
            "    uint64_t key = uint64_t(first) << 21 | second;\n"
            "    uint32_t at = lowerBound(kCompositionPairs, uint32_t(countOf(kCompositionPairs)), key);\n"
            "    if (at == countOf(kCompositionPairs) || kCompositionPairs[at] != key)\n"
            "        return false;\n"
            "    *composite = kComposites[at];\n"
            "    return true;\n}\n"]
    write(path, namespaced(head, body))


def emit_idna(path, ucd):
    spans = []
    with open(os.path.join(ucd, "IdnaMappingTable.txt"), encoding="utf-8") as f:
        for line in f:
            data = line.split("#", 1)[0].strip()
            if not data:
                continue
            parts = [part.strip() for part in data.split(";")]
            first, _, last = parts[0].partition("..")
            status = parts[1]
            if status not in IDNA_STATUS:
                raise SystemExit("gen_unicode: an IDNA status that IdnaStatus lacks: %s" % status)
            mapping = tuple(int(c, 16) for c in parts[2].split()) if len(parts) > 2 else ()
            spans.append((int(first, 16), int(last or first, 16), status, mapping))
    # Adjacent spans with the same status and mapping are one range.
    merged = []
    for first, last, status, mapping in spans:
        if merged and merged[-1][1] + 1 == first and merged[-1][2:] == (status, mapping):
            merged[-1] = (merged[-1][0], last, status, mapping)
            continue
        if (merged[-1][1] + 1 if merged else 0) != first:
            raise SystemExit("gen_unicode: the IDNA Mapping Table skips U+%04X" % (merged[-1][1] + 1))
        merged.append((first, last, status, mapping))
    if merged[-1][1] != MAX_CODE_POINT:
        raise SystemExit("gen_unicode: the IDNA Mapping Table ends before U+10FFFF")
    offsets = {}
    pool = []
    entries = []
    for _, _, status, mapping in merged:
        if len(mapping) >= 32:
            raise SystemExit("gen_unicode: an IDNA mapping of %d code points" % len(mapping))
        if mapping not in offsets:
            offsets[mapping] = len(pool)
            pool += mapping
        entries.append(offsets[mapping] << 8 | len(mapping) << 3 | IDNA_STATUS.index(status))
    head = [IDNA_BANNER, '\n#include "unicode/src/unicode_idna.h"\n\n#include "pr/count_of.h"\n#include "pr/search.h"\n']
    body = ["\nnamespace {\n",
            "\n" + enum_order(["IdnaStatus_" + status.capitalize() for status in IDNA_STATUS]),
            "\n// The first code point of each range of the table, in order.\n",
            table("constexpr uint32_t kIdnaFirsts[]", ["0x%05x" % first for first, _, _, _ in merged], 8),
            "\n// Each range's entry: where its mapping starts in kIdnaMappings << 8 | the mapping's length << 3 | the "
            "status.\n",
            table("constexpr uint32_t kIdnaEntries[]", ["0x%07x" % entry for entry in entries], 8),
            table("constexpr uint32_t kIdnaMappings[]", ["0x%05x" % cp for cp in pool], 8),
            "\n} // namespace\n",
            "\nUnicodeIdnaMapping unicodeIdnaMapping(uint32_t codePoint) {\n"
            "    uint32_t at = upperBound(kIdnaFirsts, uint32_t(countOf(kIdnaFirsts)), codePoint) - 1;\n"
            "    uint32_t entry = kIdnaEntries[at];\n"
            "    UnicodeIdnaMapping mapping{};\n"
            "    mapping.status = IdnaStatus(entry & 7);\n"
            "    mapping.count = entry >> 3 & 31;\n"
            "    mapping.codePoints = kIdnaMappings + (entry >> 8);\n"
            "    return mapping;\n}\n"]
    write(path, namespaced(head, body))


def emit_line_break(path, ucd):
    classes = fill("XX", [])
    with open(os.path.join(ucd, "LineBreak.txt"), encoding="utf-8") as f:
        for line in f:
            missing = MISSING.match(line)
            if missing:
                first, last = int(missing.group(1), 16), int(missing.group(2), 16)
                classes[first:last + 1] = [missing.group(3)] * (last - first + 1)
    for first, last, value in ranges(os.path.join(ucd, "LineBreak.txt")):
        classes[first:last + 1] = [value] * (last - first + 1)
    unknown = set(classes) - set(LINE_BREAK_CLASSES)
    if unknown:
        raise SystemExit("gen_unicode: Line_Break values PrUnicodeLineBreakClass lacks: %s"
                         % ", ".join(sorted(unknown)))
    flags = [0] * (MAX_CODE_POINT + 1)
    categories = fill("Cn", ranges(os.path.join(ucd, "DerivedGeneralCategory.txt")))
    for code_point, category in enumerate(categories):
        if category == "Pi":
            flags[code_point] |= 1 << LINE_BREAK_FLAGS["InitialQuote"]
        elif category == "Pf":
            flags[code_point] |= 1 << LINE_BREAK_FLAGS["FinalQuote"]
        elif category in ("Mn", "Mc"):
            flags[code_point] |= 1 << LINE_BREAK_FLAGS["Mark"]
    for first, last, value in ranges(os.path.join(ucd, "EastAsianWidth.txt")):
        if value in ("F", "W", "H"):
            for code_point in range(first, last + 1):
                flags[code_point] |= 1 << LINE_BREAK_FLAGS["EastAsian"]
    emoji = os.path.join(ucd, "emoji", "emoji-data.txt")
    if not os.path.exists(emoji):
        emoji = os.path.join(ucd, "emoji-data.txt")
    for first, last, value in ranges(emoji):
        if value != "Extended_Pictographic":
            continue
        for code_point in range(first, last + 1):
            if categories[code_point] == "Cn":
                flags[code_point] |= 1 << LINE_BREAK_FLAGS["UnassignedPictographic"]
    index = {name: i for i, name in enumerate(LINE_BREAK_CLASSES)}
    head = [LINE_BREAK_BANNER, '\n#include "pr/unicode_line_break.h"\n\n#include "unicode/src/unicode_staged.h"\n']
    body = ["\nnamespace {\n",
            "\n" + enum_order(["PrUnicodeLineBreakClass_" + name for name in LINE_BREAK_CLASSES]),
            "\n// The class and flags of every code point, the flags above the six bits of the class.\n",
            staged("LineBreak", [index[c] | f << 6 for c, f in zip(classes, flags)], "uint16_t", "0x%03x", 16),
            "\n} // namespace\n",
            "\nPrUnicodeLineBreakClass prUnicodeLineBreakClass(uint32_t codePoint, PrUnicodeLineBreakFlags* flags) {\n"
            "    uint32_t value = unicodeStagedValue(kLineBreakTop, kLineBreakMiddle, kLineBreakLeaves, codePoint);\n"
            "    *flags = PrUnicodeLineBreakFlags(value >> 6);\n"
            "    return PrUnicodeLineBreakClass(value & 0x3f);\n}\n"]
    write(path, namespaced(head, body, public=True))


def emit_data(path, ucd):
    grapheme = fill("Other", ranges(os.path.join(ucd, "GraphemeBreakProperty.txt")))
    word = fill("Other", ranges(os.path.join(ucd, "WordBreakProperty.txt")))
    incb = fill("None", ranges(os.path.join(ucd, "DerivedCoreProperties.txt"), "InCB"))
    flags = [0] * (MAX_CODE_POINT + 1)
    for first, last, value in ranges(os.path.join(ucd, "emoji", "emoji-data.txt")
                                     if os.path.isdir(os.path.join(ucd, "emoji"))
                                     else os.path.join(ucd, "emoji-data.txt")):
        for code_point in range(first, last + 1):
            flags[code_point] |= 1 << FLAG_BITS[value]
    for case in ("Lowercase", "Uppercase", "Default_Ignorable_Code_Point", "Cased", "Case_Ignorable"):
        for first, last, _ in ranges(os.path.join(ucd, "DerivedCoreProperties.txt"), case):
            for code_point in range(first, last + 1):
                flags[code_point] |= 1 << FLAG_BITS[case]
    for prop in ("Soft_Dotted", "Variation_Selector"):
        for first, last, _ in ranges(os.path.join(ucd, "PropList.txt"), prop):
            for code_point in range(first, last + 1):
                flags[code_point] |= 1 << FLAG_BITS[prop]
    for first, last, category in ranges(os.path.join(ucd, "DerivedGeneralCategory.txt")):
        if category in ("Mn", "Mc", "Me"):
            for code_point in range(first, last + 1):
                flags[code_point] |= 1 << FLAG_BITS["Mark"]
    codes = script_codes(os.path.join(ucd, "PropertyValueAliases.txt"))
    scripts = fill("Unknown", ranges(os.path.join(ucd, "Scripts.txt")))
    # The PrScript of each Script value: the position of its ISO 15924 code in the language generator's order.
    script_index = gen_locale.registry().script_index
    missing = sorted({codes[name] for name in set(scripts)} - set(script_index))
    if missing:
        raise SystemExit("gen_unicode: scripts that PrScript does not name: %s" % missing)
    joining = fill("Non_Joining", ranges(os.path.join(ucd, "DerivedJoiningType.txt")))
    vertical = fill("R", ranges(os.path.join(ucd, "VerticalOrientation.txt")))
    upper = case_mapping(os.path.join(ucd, "UnicodeData.txt"), os.path.join(ucd, "SpecialCasing.txt"), UPPERCASE)
    lower = case_mapping(os.path.join(ucd, "UnicodeData.txt"), os.path.join(ucd, "SpecialCasing.txt"), LOWERCASE)
    title = case_mapping(os.path.join(ucd, "UnicodeData.txt"), os.path.join(ucd, "SpecialCasing.txt"), TITLECASE)
    wide = compatibility_forms(os.path.join(ucd, "UnicodeData.txt"))
    kana = small_kana(os.path.join(ucd, "UnicodeData.txt"))
    italic = math_italics(os.path.join(ucd, "UnicodeData.txt"))
    folded = case_folding(os.path.join(ucd, "CaseFolding.txt"))
    category_index = {short: i for i, (short, _) in enumerate(GENERAL_CATEGORIES)}
    categories = fill("Cn", ranges(os.path.join(ucd, "DerivedGeneralCategory.txt")))
    unknown = set(categories) - set(category_index)
    if unknown:
        raise SystemExit("gen_unicode: General_Category values PrUnicodeGeneralCategory lacks: %s"
                         % ", ".join(sorted(unknown)))
    _, combining = canonical(os.path.join(ucd, "UnicodeData.txt"))
    classes = [0] * (MAX_CODE_POINT + 1)
    for code_point, value in combining.items():
        classes[code_point] = value

    grapheme_index = {name: i for i, name in enumerate(GRAPHEME)}
    word_index = {name: i for i, name in enumerate(WORD)}
    incb_index = {name: i for i, name in enumerate(INCB)}
    joining_members = list(dict.fromkeys(JOINING.values()))
    head = [DATA_BANNER, '\n#include "pr/unicode.h"\n\n#include "pr/count_of.h"\n'
            '#include "pr/search.h"\n\n#include "unicode/src/unicode_staged.h"\n']
    body = ["\nnamespace {\n",
            "\n" + enum_order([GRAPHEME_MEMBER[name] for name in GRAPHEME])
            + enum_order([WORD_MEMBER[name] for name in WORD])
            + enum_order(list(INCB.values())) + enum_order(joining_members) + enum_order(list(VERTICAL.values()))
            + enum_order(["PrUnicodeGeneralCategory_" + name for _, name in GENERAL_CATEGORIES]),
            "\n// Each property's value for every code point.\n",
            staged("Grapheme", [grapheme_index[v] for v in grapheme], "uint8_t", "%d", 32),
            staged("Word", [word_index[v] for v in word], "uint8_t", "%d", 32),
            staged("IndicConjunct", [incb_index[v] for v in incb], "uint8_t", "%d", 32),
            staged("Flag", flags, "uint16_t", "0x%04x", 12),
            staged("Script", [script_index[codes[v]] for v in scripts], "uint8_t", "%d", 24),
            staged("Joining", [joining_members.index(JOINING[v]) for v in joining], "uint8_t", "%d", 32),
            staged("Vertical", [list(VERTICAL).index(v) for v in vertical], "uint8_t", "%d", 32),
            staged("Combining", classes, "uint8_t", "%d", 32),
            staged("Category", [category_index[v] for v in categories], "uint8_t", "%d", 32),
            "\n// Code points whose uppercase differs from themselves, in order, and what they map to: up to "
            "three code\n// points, padded with zeros.\n",
            table("constexpr uint32_t kUpperFrom[]", ["0x%06x" % cp for cp, _ in upper], 8),
            table("constexpr uint32_t kUpperTo[][3]",
                  ["{%s}" % ", ".join("0x%06x" % c for c in (m + [0, 0])[:3]) for _, m in upper],
                  3),
            "\n// Code points whose lowercase differs from themselves, in order, and what they map to: up to "
            "three code\n// points, padded with zeros.\n",
            table("constexpr uint32_t kLowerFrom[]", ["0x%06x" % cp for cp, _ in lower], 8),
            table("constexpr uint32_t kLowerTo[][3]",
                  ["{%s}" % ", ".join("0x%06x" % c for c in (m + [0, 0])[:3]) for _, m in lower],
                  3),
            "\n// Code points whose titlecase differs from themselves, in order, and what they map to: up to "
            "three code\n// points, padded with zeros.\n",
            table("constexpr uint32_t kTitleFrom[]", ["0x%06x" % cp for cp, _ in title], 8),
            table("constexpr uint32_t kTitleTo[][3]",
                  ["{%s}" % ", ".join("0x%06x" % c for c in (m + [0, 0])[:3]) for _, m in title],
                  3),
            "\n// Code points that have a full-width form, in order, and that form.\n",
            table("constexpr uint32_t kWideFrom[]", ["0x%06x" % cp for cp, _ in wide], 8),
            table("constexpr uint32_t kWideTo[]", ["0x%06x" % m for _, m in wide], 8),
            "\n// Small kana, in order, and their full-size forms.\n",
            table("constexpr uint32_t kKanaFrom[]", ["0x%06x" % cp for cp, _ in kana], 8),
            table("constexpr uint32_t kKanaTo[]", ["0x%06x" % m for _, m in kana], 8),
            "\n// Code points that have a Mathematical Italic form, in order, and that form.\n",
            table("constexpr uint32_t kItalicFrom[]", ["0x%06x" % cp for cp, _ in italic], 8),
            table("constexpr uint32_t kItalicTo[]", ["0x%06x" % m for _, m in italic], 8),
            "\n// Code points that do not fold to themselves, in order, and their full case foldings: up to three code "
            "points,\n// padded with zeros.\n",
            table("constexpr uint32_t kFoldFrom[]", ["0x%06x" % cp for cp, _ in folded], 8),
            table("constexpr uint32_t kFoldTo[][3]",
                  ["{%s}" % ", ".join("0x%06x" % c for c in (m + [0, 0])[:3]) for _, m in folded],
                  3),
            "\n} // namespace\n",
            "\nPrUnicodeGraphemeBreak prUnicodeGraphemeBreak(uint32_t codePoint) {\n"
            "    return PrUnicodeGraphemeBreak(unicodeStagedValue(kGraphemeTop, kGraphemeMiddle, kGraphemeLeaves, codePoint));\n}\n",
            "\nPrUnicodeWordBreak prUnicodeWordBreak(uint32_t codePoint) {\n"
            "    return PrUnicodeWordBreak(unicodeStagedValue(kWordTop, kWordMiddle, kWordLeaves, codePoint));\n}\n",
            "\nPrUnicodeIndicConjunctBreak prUnicodeIndicConjunctBreak(uint32_t codePoint) {\n"
            "    return PrUnicodeIndicConjunctBreak(unicodeStagedValue(kIndicConjunctTop, kIndicConjunctMiddle, kIndicConjunctLeaves,\n"
            "                                          codePoint));\n}\n",
            "\nPrUnicodeFlags prUnicodeFlags(uint32_t codePoint) {\n"
            "    return PrUnicodeFlags(unicodeStagedValue(kFlagTop, kFlagMiddle, kFlagLeaves, codePoint));\n}\n",
            "\nPrScript prUnicodeScript(uint32_t codePoint) {\n"
            "    return PrScript(unicodeStagedValue(kScriptTop, kScriptMiddle, kScriptLeaves, codePoint));\n}\n",
            "\nPrUnicodeJoiningType prUnicodeJoiningType(uint32_t codePoint) {\n"
            "    return PrUnicodeJoiningType(unicodeStagedValue(kJoiningTop, kJoiningMiddle, kJoiningLeaves, codePoint));\n}\n",
            "\nPrUnicodeVerticalOrientation prUnicodeVerticalOrientation(uint32_t codePoint) {\n"
            "    return PrUnicodeVerticalOrientation(unicodeStagedValue(kVerticalTop, kVerticalMiddle, kVerticalLeaves, codePoint));\n"
            "}\n",
            "\nuint8_t prUnicodeCombiningClass(uint32_t codePoint) {\n"
            "    return uint8_t(unicodeStagedValue(kCombiningTop, kCombiningMiddle, kCombiningLeaves, codePoint));\n}\n",
            "\nPrUnicodeGeneralCategory prUnicodeGeneralCategory(uint32_t codePoint) {\n"
            "    return PrUnicodeGeneralCategory(unicodeStagedValue(kCategoryTop, kCategoryMiddle, kCategoryLeaves, codePoint));\n}\n",
            mapping_lookup("prUnicodeUppercase", "kUpper"),
            mapping_lookup("prUnicodeLowercase", "kLower"),
            mapping_lookup("prUnicodeTitlecase", "kTitle"),
            mapping_lookup("prUnicodeFold", "kFold"),
            single_lookup("prUnicodeFullWidth", "kWide"),
            single_lookup("prUnicodeFullSizeKana", "kKana"),
            single_lookup("prUnicodeMathItalic", "kItalic")]
    write(path, namespaced(head, body, public=True))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ucd", default=os.path.join(gen_common.ROOT, "third_party", "unicode"))
    ap.add_argument("--out", default=os.path.join(gen_common.ROOT, "lib", "unicode", "src"))
    args = ap.parse_args()

    classes = read(os.path.join(args.ucd, "DerivedBidiClass.txt"))
    unknown = set(classes) - set(BIDI_CLASSES)
    if unknown:
        raise SystemExit("gen_unicode: Bidi_Class values PrUnicodeBidiClass lacks: %s"
                         % ", ".join(sorted(unknown)))
    canonical = singletons(os.path.join(args.ucd, "UnicodeData.txt"))
    gen_common.write(os.path.join(args.out, "unicode_staged.h"), [gen_common.banner(__file__), STAGED_HEADER])
    emit_source(os.path.join(args.out, "unicode_bidi_data.cpp"), classes,
                brackets(os.path.join(args.ucd, "BidiBrackets.txt"), canonical))
    emit_line_break(os.path.join(args.out, "unicode_line_break_data.cpp"), args.ucd)
    emit_data(os.path.join(args.out, "unicode_data.cpp"), args.ucd)
    emit_normalization(os.path.join(args.out, "unicode_normalization_data.cpp"), args.ucd)
    emit_idna(os.path.join(args.out, "unicode_idna_data.cpp"), args.ucd)


if __name__ == "__main__":
    main()
