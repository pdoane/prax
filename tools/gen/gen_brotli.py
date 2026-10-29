#!/usr/bin/env python3
"""Generates the Brotli decoder's tables from RFC 7932: the static dictionary (appendix A), its word counts, the word
transformations (appendix B) and the literal context lookup tables (section 7.1). Each table is checked against the
length and CRC-32 that the RFC gives for it."""

import ast
import os
import re
import zlib

from tools.gen.gen_common import ROOT, banner, namespaced, table, write

RFC = os.path.join(ROOT, "third_party", "rfc", "rfc7932.txt")
RFC_URL = "https://www.rfc-editor.org/rfc/rfc7932.txt"
OUT = os.path.join(ROOT, "lib", "compress", "backends", "builtin", "src")

DICTIONARY_SIZE = 122784
DICTIONARY_CRC = 0x5136CB04
TRANSFORMS_SIZE = 648
TRANSFORMS_CRC = 0x3D965F81
LUT_CRC = {"Lut0": 0x8E91EFB7, "Lut1": 0xD01A32F4, "Lut2": 0x0DD7A0D6}

# The elementary transforms, in the order whose indices appendix B's check value serializes.
ELEMENTARY = (["Identity", "FermentFirst", "FermentAll"] + ["OmitFirst%d" % k for k in range(1, 10)]
              + ["OmitLast%d" % k for k in range(1, 10)])

# Matches a line of page furniture: a footer, a running header or a form feed.
FURNITURE = re.compile(r"^(Alakuijala & Szabadka .*|RFC 7932 .*|\f)$")


def body(lines, start, end):
    """Returns the lines from the line that starts with `start` up to the line that starts with `end`, without page
    furniture."""
    first = next(i for i, line in enumerate(lines) if line.startswith(start))
    last = next(i for i in range(first + 1, len(lines)) if lines[i].strip().startswith(end))
    return [line for line in lines[first + 1:last] if not FURNITURE.match(line.strip())]


def check(name, data, size, crc):
    if len(data) != size or zlib.crc32(data) != crc:
        raise SystemExit("%s: %d bytes with CRC-32 %08x, where rfc 7932 gives %d and %08x"
                         % (name, len(data), zlib.crc32(data), size, crc))


def dictionary(lines):
    rows = body(lines, "Appendix A.", "The number of words for each length")
    data = bytes.fromhex("".join(line.strip() for line in rows if re.fullmatch(r"\s+[0-9a-f]+", line)))
    check("the static dictionary", data, DICTIONARY_SIZE, DICTIONARY_CRC)
    first = next(i for i, line in enumerate(lines) if line.startswith("      NDBITS :="))
    bits = [int(n) for n in re.findall(r"\d+", " ".join(lines[first:first + 3]).split(":=")[1])]
    if len(bits) != 25:
        raise SystemExit("NDBITS has %d values, not 25" % len(bits))
    offsets = [0]
    for length in range(24):
        offsets.append(offsets[-1] + (length * (1 << bits[length]) if length >= 4 else 0))
    if offsets[24] + 24 * (1 << bits[24]) != DICTIONARY_SIZE:
        raise SystemExit("NDBITS does not describe the dictionary's size")
    return data, bits, offsets


def transforms(lines):
    rows = body(lines, "Appendix B.", "Appendix C.")
    found = []
    for line in rows:
        match = re.fullmatch(r"\s+(\d+)\s+(\".*?\")\s+(\w+)\s+(\".*\")\s*", line)
        if match:
            prefix = ast.literal_eval("b" + match.group(2))
            suffix = ast.literal_eval("b" + match.group(4))
            found.append((int(match.group(1)), prefix, ELEMENTARY.index(match.group(3)), suffix))
    if [row[0] for row in found] != list(range(121)):
        raise SystemExit("appendix B does not list transforms 0 to 120")
    serialized = b"".join(prefix + b"\0" + bytes([kind]) + suffix + b"\0" for _, prefix, kind, suffix in found)
    check("the word transformations", serialized, TRANSFORMS_SIZE, TRANSFORMS_CRC)
    return [row[1:] for row in found]


def luts(lines):
    rows = body(lines, "7.1.  Context Modes", "7.2.  Context ID")
    text = "\n".join(rows)
    found = {}
    for name in LUT_CRC:
        after = text.split(name + " :=")[1]
        values = [int(n) for n in re.findall(r"\d+", after)[:256]]
        check(name, bytes(values), 256, LUT_CRC[name])
        found[name] = values
    return found


def c_bytes(data):
    """Returns a C string literal of `data`. Bytes outside printable ASCII, and `?`, which could begin a trigraph,
    become three-digit octal escapes, which a following digit cannot extend."""
    out = []
    for byte in data:
        if byte in (0x22, 0x5C):
            out.append("\\" + chr(byte))
        elif 0x20 <= byte < 0x7F and byte != 0x3F:
            out.append(chr(byte))
        else:
            out.append("\\%03o" % byte)
    return '"' + "".join(out) + '"'


def main():
    with open(RFC, encoding="utf-8") as f:
        lines = f.read().split("\n")
    data, bits, offsets = dictionary(lines)
    words = transforms(lines)
    lut = luts(lines)

    # The context lookup of each mode, indexed by mode and then by a previous byte. The first 256 entries are indexed by
    # the last byte, and the next 256 by the byte before it. A context ID is the bitwise or of the two entries.
    context = ([p & 0x3F for p in range(256)] + [0] * 256 + [p >> 2 for p in range(256)] + [0] * 256
               + lut["Lut0"] + lut["Lut1"] + [v << 3 for v in lut["Lut2"]] + lut["Lut2"])

    generated = banner(__file__, "rfc 7932", RFC_URL)
    header = namespaced([generated, "\n// The tables of Brotli decoding (rfc 7932).\n",
                         '\n#pragma once\n\n#include "pr/base.h"\n'], [
        "\n// The static dictionary's bytes (rfc 7932 appendix A).\n",
        "constexpr uint32_t kBrotliDictionarySize = %d;\n" % DICTIONARY_SIZE,
        "extern const uint8_t kBrotliDictionary[kBrotliDictionarySize];\n",
        "\n// The shortest and longest dictionary words.\n",
        "constexpr uint32_t kBrotliWordMinLength = 4;\n",
        "constexpr uint32_t kBrotliWordMaxLength = 24;\n",
        "\n// The base-2 log of the number of dictionary words of each length (NDBITS), by length.\n",
        "extern const uint8_t kBrotliDictionaryBits[kBrotliWordMaxLength + 1];\n",
        "\n// The offset in the dictionary of the first word of each length (DOFFSET), by length.\n",
        "extern const uint32_t kBrotliDictionaryOffsets[kBrotliWordMaxLength + 1];\n",
        "\n// The elementary transforms of a dictionary word (rfc 7932 8). OmitFirst<k> is BrotliElementary_OmitFirst1",
        " plus k - 1, and OmitLast<k> is BrotliElementary_OmitLast1 plus k - 1.\n",
        "enum {\n    BrotliElementary_Identity,\n    BrotliElementary_FermentFirst,\n",
        "    BrotliElementary_FermentAll,\n    BrotliElementary_OmitFirst1,\n",
        "    BrotliElementary_OmitLast1 = BrotliElementary_OmitFirst1 + 9,\n};\n",
        "typedef uint8_t BrotliElementary;\n",
        "\n// A word transformation: a prefix, an elementary transform and a suffix.\n",
        "struct BrotliTransform {\n    const char* prefix;\n    const char* suffix;\n",
        "    uint8_t prefixSize;\n    uint8_t suffixSize;\n    BrotliElementary elementary;\n};\n",
        "\n// The word transformations (rfc 7932 appendix B), by transform ID.\n",
        "constexpr uint32_t kBrotliTransformCount = %d;\n" % len(words),
        "extern const BrotliTransform kBrotliTransforms[kBrotliTransformCount];\n",
        "\n// The literal context lookup of each context mode (rfc 7932 7.1), by mode and then by byte: 256 entries",
        " indexed by the last byte, then 256 by the byte before it. A context ID is the two entries combined with",
        " or.\n",
        "extern const uint8_t kBrotliContextLookup[4 * 512];\n",
    ])
    write(os.path.join(OUT, "brotli_tables.h"), header)

    rows = ['    {%s, %s, %d, %d, %s},\n' % (c_bytes(prefix), c_bytes(suffix), len(prefix), len(suffix),
                                            elementary_name(kind))
            for prefix, kind, suffix in words]
    source = [generated, '\n#include "compress/backends/builtin/src/brotli_tables.h"\n',
              "\nnamespace pr_impl {\n",
              "\nconst uint8_t kBrotliDictionary[kBrotliDictionarySize] = {\n",
              "".join(dictionary_word_bytes(data)), "};\n",
              table("const uint8_t kBrotliDictionaryBits[kBrotliWordMaxLength + 1]", bits, per_line=25),
              table("const uint32_t kBrotliDictionaryOffsets[kBrotliWordMaxLength + 1]", offsets, per_line=10),
              "\nconst BrotliTransform kBrotliTransforms[kBrotliTransformCount] = {\n", "".join(rows), "};\n",
              table("const uint8_t kBrotliContextLookup[4 * 512]", context, per_line=16),
              "\n} // namespace pr_impl\n"]
    write(os.path.join(OUT, "brotli_tables.cpp"), source)


def elementary_name(kind):
    if kind == 0:
        return "BrotliElementary_Identity"
    if kind == 1:
        return "BrotliElementary_FermentFirst"
    if kind == 2:
        return "BrotliElementary_FermentAll"
    if kind < 12:
        return "BrotliElementary_OmitFirst1 + %d" % (kind - 3) if kind > 3 else "BrotliElementary_OmitFirst1"
    return "BrotliElementary_OmitLast1 + %d" % (kind - 12) if kind > 12 else "BrotliElementary_OmitLast1"


def dictionary_word_bytes(data):
    """Returns the dictionary as rows of byte values, 22 to a row."""
    return ["    " + ", ".join(str(b) for b in data[at:at + 22]) + ",\n" for at in range(0, len(data), 22)]


if __name__ == "__main__":
    main()
