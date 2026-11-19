#!/usr/bin/env python3
"""Generates the WebP decoder's tables from their RFCs: VP8's dequantization tables, default coefficient
probabilities, coefficient update probabilities and key frame subblock mode probabilities from RFC 6386 (sections
14.1, 13.5, 13.4 and 11.5), and the lossless format's distance map from RFC 9649 (section 3.6.2.2.1). Each table is
checked against the number of values that the RFC declares for it."""

import os
import re

from tools.gen.gen_common import ROOT, banner, namespaced, table, write

VP8 = os.path.join(ROOT, "third_party", "rfc", "rfc6386.txt")
VP8_URL = "https://www.rfc-editor.org/rfc/rfc6386.txt"
WEBP = os.path.join(ROOT, "third_party", "rfc", "rfc9649.txt")
WEBP_URL = "https://www.rfc-editor.org/rfc/rfc9649.txt"
OUT = os.path.join(ROOT, "lib", "webp", "src")

# Matches a line of page furniture: a footer, a running header or a form feed.
FURNITURE = re.compile(r"^(Bankoski, et al\..*|RFC 6386 .*|Zern, et al\..*|RFC 9649 .*|\f)$")


def text_of(path):
    with open(path, encoding="utf-8") as f:
        return "\n".join(line for line in f.read().split("\n") if not FURNITURE.match(line.strip()))


def values(text, declaration, count):
    """Returns the integers of the C array that `declaration` begins, between its `=` and its closing `};`, without
    its comments, and checks that there are `count`."""
    start = text.index(declaration)
    body = text[text.index("=", start) + 1:text.index("};", start)]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    found = [int(n) for n in re.findall(r"-?\d+", body)]
    if len(found) != count:
        raise SystemExit("%s: %d values, not %d" % (declaration, len(found), count))
    return found


def distance_map(text):
    """Returns the offsets (xi, yi) of the distance codes 1 to 120 (RFC 9649 figure 20)."""
    start = text.index("The mapping between distance code distance_code and the neighboring")
    end = text.index("Figure 20: Distance Code to Neighboring Pixel Offset Mapping")
    pairs = [(int(x), int(y)) for x, y in re.findall(r"\((-?\d+),\s*(-?\d+)\)", text[start:end])]
    if len(pairs) != 120:
        raise SystemExit("the distance map has %d entries, not 120" % len(pairs))
    return pairs


def main():
    vp8 = text_of(VP8)
    webp = text_of(WEBP)
    dc = values(vp8, "static const int dc_qlookup[QINDEX_RANGE]", 128)
    ac = values(vp8, "static const int ac_qlookup[QINDEX_RANGE]", 128)
    coefficients = values(vp8, "const Prob default_coeff_probs [4] [8] [3] [num_dct_tokens - 1]", 4 * 8 * 3 * 11)
    updates = values(vp8, "const Prob coeff_update_probs [4] [8] [3] [num_dct_tokens-1]", 4 * 8 * 3 * 11)
    # Section 11.5 repeats the declaration that section 11.4 introduces, and only 11.5's has the values.
    modes_at = vp8.index("11.5.  Subblock Mode Probability Table")
    modes = values(vp8[modes_at:], "const Prob kf_bmode_prob [num_intra_bmodes] [num_intra_bmodes]", 10 * 10 * 9)
    distances = distance_map(webp)

    generated = banner(__file__, "rfc 6386 and rfc 9649", VP8_URL, WEBP_URL)
    header = namespaced([generated, "\n// The tables of VP8 key frame decoding (rfc 6386), and of WebP lossless",
                         " decoding (rfc 9649 3).\n", '\n#pragma once\n\n#include "pr/base.h"\n'], [
        "\n// The step of a quantizer index's DC and AC coefficients (rfc 6386 14.1), by index.\n",
        "extern const uint16_t kVp8DcQuant[128];\n",
        "extern const uint16_t kVp8AcQuant[128];\n",
        "\n// The probabilities of the coefficient token tree at the start of a frame (rfc 6386 13.5), by block type,",
        " coefficient band, context and node.\n",
        "extern const uint8_t kVp8CoefficientProbabilities[4][8][3][11];\n",
        "\n// The probability that a frame updates each probability of the coefficient token tree (rfc 6386 13.4),",
        " in the same order.\n",
        "extern const uint8_t kVp8CoefficientUpdateProbabilities[4][8][3][11];\n",
        "\n// The probabilities of the subblock mode tree in a key frame (rfc 6386 11.5), by the mode of the",
        " subblock above, the mode of the subblock to the left, and node.\n",
        "extern const uint8_t kVp8SubblockModeProbabilities[10][10][9];\n",
        "\n// The offsets (x, y) to the left and up of the pixel that each of the lossless format's distance codes 1",
        " to 120 names (rfc 9649 3.6.2.2.1), as x then y.\n",
        "extern const int8_t kVp8lDistanceMap[120][2];\n",
    ])
    write(os.path.join(OUT, "vp8_tables.h"), header)

    source = [generated, '\n#include "webp/src/vp8_tables.h"\n', "\nnamespace pr_impl {\n",
              table("const uint16_t kVp8DcQuant[128]", dc, per_line=16),
              table("const uint16_t kVp8AcQuant[128]", ac, per_line=16),
              nested("const uint8_t kVp8CoefficientProbabilities[4][8][3][11]", coefficients, (4, 8, 3, 11)),
              nested("const uint8_t kVp8CoefficientUpdateProbabilities[4][8][3][11]", updates, (4, 8, 3, 11)),
              nested("const uint8_t kVp8SubblockModeProbabilities[10][10][9]", modes, (10, 10, 9)),
              "\nconst int8_t kVp8lDistanceMap[120][2] = {\n",
              "".join("    %s,\n" % ", ".join("{%d, %d}" % pair for pair in distances[at:at + 8])
                      for at in range(0, 120, 8)),
              "};\n", "\n} // namespace pr_impl\n"]
    write(os.path.join(OUT, "vp8_tables.cpp"), source)


def nested(declaration, flat, shape):
    """Returns the definition of a multidimensional array whose innermost rows are each on a line."""
    out = ["\n%s = {\n" % declaration]

    def emit(values, dims, indent):
        if len(dims) == 1:
            out.append("%s{%s},\n" % (indent, ", ".join(str(v) for v in values)))
            return
        out.append("%s{\n" % indent)
        step = len(values) // dims[0]
        for i in range(dims[0]):
            emit(values[i * step:(i + 1) * step], dims[1:], indent + "    ")
        out.append("%s},\n" % indent)

    step = len(flat) // shape[0]
    for i in range(shape[0]):
        emit(flat[i * step:(i + 1) * step], shape[1:], "    ")
    out.append("};\n")
    return "".join(out)


if __name__ == "__main__":
    main()
