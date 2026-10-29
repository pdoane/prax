#!/usr/bin/env python3
"""Generates the charsets of the Encoding Standard: their enum, their labels and names, and the indexes that decode
and encode them.

    pr/charset_names.h          PrCharset, one member for each encoding
    charset/src/charset_tables  the lookup of a label, each charset's name, and the indexes
"""

import json
import os
import re

from tools.gen.gen_common import ROOT, banner, bitset, enum_members, table, write
from tools.gen.name_table import Blob, NameTable

SOURCE = os.path.join(ROOT, "third_party", "encoding")
SPEC = "https://encoding.spec.whatwg.org/"

# The indexes that the multi-byte decoders read, each by the name the generated table takes.
INDEXES = [
    ("jis0208", "Jis0208"),
    ("jis0212", "Jis0212"),
    ("euc-kr", "EucKr"),
    ("gb18030", "Gb18030"),
]


# The pointers of index jis0208 that the Shift_JIS encoder skips (encoding/#index-shift_jis-pointer).
SHIFT_JIS_SKIPPED = range(8272, 8836)

# The pointers of index Big5 below this one, Hong Kong extensions, are never encoded (encoding/#index-big5-pointer).
BIG5_FIRST_ENCODED = (0xA1 - 0x81) * 157

# The code points that the Big5 encoder encodes with their last pointer instead of their first.
BIG5_LAST = {0x2550, 0x255E, 0x2561, 0x256A, 0x5341, 0x5345}


def reverse(index, skip=(), last=()):
    """Returns the code points of an index in increasing order, each with the pointer that encodes it: its first
    pointer (encoding/#index-pointer), or its last if it is in `last`. A pointer in `skip` encodes nothing."""
    chosen = {}
    for pointer, code_point in enumerate(index):
        if code_point is None or pointer in skip:
            continue
        if code_point not in chosen or code_point in last:
            chosen[code_point] = pointer
    return sorted(chosen.items())


def ident(name):
    """Returns the enum member for an encoding's name: UTF-16BE is Utf16Be, and x-mac-cyrillic is XMacCyrillic."""
    return "".join(part.capitalize() for part in re.findall(r"[A-Za-z]+|[0-9]+", name))


def single_byte(index):
    """Returns the UTF-8 of each code point of a single-byte index, packed for the decoder: the bytes in order from
    the low byte, and the length in the high byte. A pointer with no code point packs as zero."""
    out = []
    for code_point in index:
        if code_point is None:
            out.append("0")
            continue
        utf8 = chr(code_point).encode("utf-8")
        out.append("0x%08x" % (len(utf8) << 24 | int.from_bytes(utf8, "little")))
    return out


def main():
    with open(os.path.join(SOURCE, "encodings.json"), encoding="utf-8") as f:
        groups = json.load(f)
    with open(os.path.join(SOURCE, "indexes.json"), encoding="utf-8") as f:
        indexes = json.load(f)

    encodings = [encoding for group in groups for encoding in group["encodings"]]
    names = [encoding["name"] for encoding in encodings]
    singles = [encoding["name"] for encoding in groups[1]["encodings"]]
    if groups[1]["heading"] != "Legacy single-byte encodings":
        raise SystemExit("gen_charset: the second group of encodings.json is not the single-byte encodings")
    if names.index(singles[-1]) - names.index(singles[0]) != len(singles) - 1:
        raise SystemExit("gen_charset: the single-byte encodings are not consecutive")

    labels = []
    charsets = []
    for number, encoding in enumerate(encodings, 1):
        for label in encoding["labels"]:
            labels.append(label)
            charsets.append(number)
    lookup = NameTable("kCharsetLabel", [""] + labels, lower=True)
    blob = Blob("kCharsetName", [""] + names)

    jis0208 = reverse(indexes["jis0208"])
    shift_jis = dict(reverse(indexes["jis0208"], skip=SHIFT_JIS_SKIPPED))
    encoders = [
        ("Jis0208", jis0208),
        ("ShiftJis", [(code_point, shift_jis.get(code_point, 0xFFFF)) for code_point, pointer in jis0208
                      if shift_jis.get(code_point) != pointer]),
        ("EucKr", reverse(indexes["euc-kr"])),
        ("Gb18030", reverse(indexes["gb18030"])),
        ("Big5", reverse(indexes["big5"], skip=range(BIG5_FIRST_ENCODED), last=BIG5_LAST)),
    ]

    generated = banner(__file__, "the Encoding Standard's encodings and indexes",
                       "https://github.com/whatwg/encoding/blob/main/encodings.json",
                       "https://github.com/whatwg/encoding/blob/main/indexes.json")

    members = [("PrCharset_None", "")] + [("PrCharset_" + ident(name), name) for name in names]
    public = [generated, "\n// Charsets\n//\n// encoding - %s\n" % SPEC,
              '\n#pragma once\n\n#include "pr/base.h"\n\nPR_ASSUME_NONNULL_BEGIN\n',
              "\n// The encodings of the Encoding Standard (encoding/#names-and-labels), each beside its name, and "
              "PrCharset_None for none.\nenum {\n"]
    public += enum_members(members)
    public += ["\n    PrCharset_Count,\n};\ntypedef uint8_t PrCharset;\n", "\nPR_ASSUME_NONNULL_END\n"]
    write(os.path.join(ROOT, "lib/charset/include/pr/charset_names.h"), public)

    source = os.path.join(ROOT, "lib/charset/src")
    first = "PrCharset_" + ident(singles[0])
    header = [generated, '\n#pragma once\n\n#include "pr/base.h"\n#include "pr/charset_names.h"\n',
              "\nPR_ASSUME_NONNULL_BEGIN\n\nnamespace pr_impl {\n",
              "\n// The single-byte charsets, which are consecutive members of PrCharset.\n",
              "constexpr PrCharset kCharsetSingleByteFirst = %s;\n" % first,
              "constexpr PrCharset kCharsetSingleByteLast = PrCharset_%s;\n" % ident(singles[-1]),
              "constexpr uint32_t kCharsetSingleByteCount = %d;\n" % len(singles),
              "\n// Returns the charset that `label` names, or PrCharset_None. `label` has no surrounding whitespace, "
              "and its ASCII\n// letters may be in either case.\n",
              "PrCharset charsetOfLabel(PrStringView label);\n",
              "\n// Returns the name of `charset`, or the empty view for PrCharset_None.\n",
              "PrStringView charsetName(PrCharset charset);\n",
              "\n// The code points of the bytes 0x80 to 0xff in each single-byte charset, by charset from "
              "kCharsetSingleByteFirst,\n// as UTF-8 packed into a word: the bytes in order from the low byte, and "
              "their count in the high byte. A\n// byte that has no code point is zero.\n",
              "extern const uint32_t kCharsetSingleByte[kCharsetSingleByteCount][128];\n",
              "\n// The indexes of the multi-byte charsets (encoding/#indexes), each a code point by pointer, with zero "
              "for a pointer\n// that has none.\n"]
    for name, suffix in INDEXES:
        header.append("extern const uint16_t kCharsetIndex%s[%d];\n" % (suffix, len(indexes[name])))
    header += ["\n// The index of Big5, by pointer. A code point above U+FFFF is in plane 2, and its pointer is in "
               "kCharsetIndexBig5Plane2,\n// a bitset by pointer, which pr/bitset.h reads. The table holds the low 16 "
               "bits of every code point.\n",
               "extern const uint16_t kCharsetIndexBig5[%d];\n" % len(indexes["big5"]),
               "extern const uint64_t kCharsetIndexBig5Plane2[%d];\n" % ((len(indexes["big5"]) + 63) // 64),
               "\n// The code points of index ISO-2022-JP katakana, by pointer, for the ISO-2022-JP encoder.\n",
               "extern const uint16_t kCharsetIndexIso2022JpKatakana[%d];\n" % len(indexes["iso-2022-jp-katakana"]),
               "\n// The code points that the multi-byte encoders encode, in increasing order, and the pointer that "
               "encodes each\n// (encoding/#index-pointer). The Shift_JIS encoder reads jis0208's, but skips pointers "
               "8272 to 8835: it encodes\n// each code point of kCharsetEncodeShiftJisCodePoints with the pointer "
               "beside it in place, or with none if that\n// is 0xffff. Big5's pointers are those of "
               "encoding/#index-big5-pointer.\n"]
    for suffix, pairs in encoders:
        width = "uint32_t" if pairs[-1][0] > 0xffff else "uint16_t"
        header.append("extern const %s kCharsetEncode%sCodePoints[%d];\n" % (width, suffix, len(pairs)))
        header.append("extern const uint16_t kCharsetEncode%sPointers[%d];\n" % (suffix, len(pairs)))
    header += ["\n// A range of index gb18030 ranges: the first pointer of the range and its code point.\n",
               "struct CharsetGb18030Range {\n    uint32_t pointer;\n    uint32_t codePoint;\n};\n",
               "\n// The ranges of index gb18030 ranges, in increasing order of pointer.\n",
               "extern const CharsetGb18030Range kCharsetGb18030Ranges[%d];\n" % len(indexes["gb18030-ranges"]),
               "\n} // namespace pr_impl\n\nPR_ASSUME_NONNULL_END\n"]
    write(os.path.join(source, "charset_tables.h"), header)

    body = [generated, '\n#include "charset/src/charset_tables.h"\n', '\n#include "pr/name_table.h"\n',
            "\nnamespace pr_impl {\nnamespace {\n", lookup.tables(),
            table("constexpr PrCharset kCharsetOfLabel[]", ["PrCharset_None"]
                  + ["PrCharset_" + ident(names[number - 1]) for number in charsets], 4),
            blob.tables(), "\n} // namespace\n",
            "\nPrCharset charsetOfLabel(PrStringView label) {\n", lookup.lookup("label", "index"),
            "    return %s ? kCharsetOfLabel[index] : PrCharset_None;\n}\n"
            % lookup.confirm("label", lookup.name("index")),
            "\nPrStringView charsetName(PrCharset charset) {\n    return %s;\n}\n" % blob.view("charset"),
            "\nextern const uint32_t kCharsetSingleByte[kCharsetSingleByteCount][128] = {\n"]
    for name in singles:
        # ISO-8859-8-I decodes with the index of ISO-8859-8 (encoding/#legacy-single-byte-encodings).
        values = single_byte(indexes[name.lower().removesuffix("-i")])
        body.append("    // %s\n    {\n" % name)
        for at in range(0, 128, 8):
            body.append("        " + ", ".join(values[at:at + 8]) + ",\n")
        body.append("    },\n")
    body.append("};\n")
    for name, suffix in INDEXES:
        values = ["0x%04x" % (code_point or 0) for code_point in indexes[name]]
        body.append(table("extern const uint16_t kCharsetIndex%s[%d]" % (suffix, len(values)), values, 12))
    big5 = indexes["big5"]
    body.append(table("extern const uint16_t kCharsetIndexBig5[%d]" % len(big5),
                      ["0x%04x" % ((code_point or 0) & 0xffff) for code_point in big5], 12))
    body.append(bitset("extern const uint64_t kCharsetIndexBig5Plane2[%d]" % ((len(big5) + 63) // 64),
                       [code_point is not None and code_point > 0xffff for code_point in big5]))
    katakana = indexes["iso-2022-jp-katakana"]
    body.append(table("extern const uint16_t kCharsetIndexIso2022JpKatakana[%d]" % len(katakana),
                      ["0x%04x" % code_point for code_point in katakana], 12))
    for suffix, pairs in encoders:
        width = "uint32_t" if pairs[-1][0] > 0xffff else "uint16_t"
        body.append(table("extern const %s kCharsetEncode%sCodePoints[%d]" % (width, suffix, len(pairs)),
                          ["0x%04x" % code_point for code_point, _ in pairs], 12))
        body.append(table("extern const uint16_t kCharsetEncode%sPointers[%d]" % (suffix, len(pairs)),
                          [str(pointer) for _, pointer in pairs], 12))
    ranges = indexes["gb18030-ranges"]
    body.append(table("extern const CharsetGb18030Range kCharsetGb18030Ranges[%d]" % len(ranges),
                      ["{%d, 0x%04x}" % (pointer, code_point) for pointer, code_point in ranges], 6))
    body.append("\n} // namespace pr_impl\n")
    write(os.path.join(source, "charset_tables.cpp"), body)


if __name__ == '__main__':
    main()
