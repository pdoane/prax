#!/usr/bin/env python3
"""Generates the table of powers of five that lib/core's conversions between doubles and decimal text scale by
(lib/core/src/float_convert.cpp). Each entry is 5^q normalized into [2^127, 2^128) and truncated, computed exactly with
Python's integers. Decimal to binary (Eisel-Lemire) reads the truncated entries. Binary to decimal (Dragonbox) and the
rounded digits read them rounded up, which an entry is exactly where it is inexact plus one."""

import os

from tools.gen.gen_common import ROOT, banner, namespaced, table, write

OUT = os.path.join(ROOT, "lib", "core", "src", "float_convert_tables.h")

# The range of q: every power of ten by which a conversion scales a double. Parsing reads 10^-342 through 10^308, past
# which a decimal is zero or infinite, and the shortest form of the least subnormal scales by 10^326.
MIN_EXP10 = -342
MAX_EXP10 = 326


def entry(q):
    """Returns 5^q normalized into [2^127, 2^128), truncated."""
    num, den = (5 ** q, 1) if q >= 0 else (1, 5 ** -q)
    # 2^127 <= num * 2^shift / den < 2^128, where the bit lengths place the quotient within one bit.
    shift = 127 - (num.bit_length() - den.bit_length())
    value = (num << shift) // den if shift >= 0 else num // (den << -shift)
    if value >= 1 << 128:
        value >>= 1
    elif value < 1 << 127:
        shift += 1
        value = (num << shift) // den if shift >= 0 else num // (den << -shift)
    assert 1 << 127 <= value < 1 << 128, q
    return value


def main():
    rows = []
    for q in range(MIN_EXP10, MAX_EXP10 + 1):
        value = entry(q)
        rows.append("{.hi = 0x%016Xull, .lo = 0x%016Xull}" % (value >> 64, value & ((1 << 64) - 1)))
    header = namespaced([banner(__file__), "\n// The table of powers of five for the conversions in float_convert.cpp.\n",
                         '\n#pragma once\n\n#include "pr/base.h"\n'], [
        "\n// A 128-bit fixed-point table entry. The conversion code recovers its implied exponent from its index.\n",
        "struct Pow5 {\n    uint64_t hi;\n    uint64_t lo;\n};\n",
        "\n// The powers of five that kPow5 covers: those of every power of ten that a conversion scales a double by.\n",
        "constexpr int32_t kPow5MinExp10 = %d;\n" % MIN_EXP10,
        "constexpr int32_t kPow5MaxExp10 = %d;\n" % MAX_EXP10,
        "\n// 5^q normalized into [2^127, 2^128) and truncated to 128 bits, indexed by q - kPow5MinExp10. Those of q",
        " from 0 through 55 are exact.",
        table("constexpr Pow5 kPow5[%d]" % len(rows), rows),
    ])
    write(OUT, header)


if __name__ == "__main__":
    main()
