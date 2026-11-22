#!/usr/bin/env python3
"""Prints basis_etc1s_bc.h: the tables of the BC1 and BC4 block ends that best represent an ETC1S block.

A channel of an ETC1S block has a base value of five bits, one of the eight ETC1 modifier tables, and the selectors that
its texels use. The base and the table give the channel four values, one for each selector.

The BC1 search repeats the search that made the tables of Basis Universal's transcoder, so a transcoded block equals the
block that the reference transcoder writes. A run is the range of selectors that a block uses, from the lowest to the
highest. A placing assigns each of the four selectors to one of the four BC1 colors. There are ten placings. For each
base, table, run and placing, the search tries every pair of ends of 5 or 6 bits. It keeps the pair that leaves the
least squared error between the values of the run's selectors and the BC1 colors that the placing assigns to them. The
two BC1 colors between the ends are rounded down. If several pairs leave the least error, the search keeps the pair with
the lowest high end, then the lowest low end. For a block's run, the transcoder takes the placing with the least sum of
the errors of the three channels.

Two more BC1 tables give the ends for a single value of 8 bits. One table holds the pair of ends that best puts the
value a third of the way from the high end to the low end. The other table holds the one end nearest the value.

The BC4 search covers each set of three or four selectors that a block uses. It tries every pair of ends of 8 bits in
both orders, because the order of the ends selects one of the two BC4 modes. Each selector takes the nearest of the
eight BC4 values. The search keeps the pair that leaves the least squared error for the values of the set's selectors.
It uses the exact values between the ends, as a GPU interpolates them.

An entry of a BC1 table is one word. It holds the low end in its low byte, the high end in the next byte, and the
squared error from bit 16. An entry of the BC4 table holds the first end in its low byte and the second end in the next
byte. From bit 16 it holds the BC4 index of each selector, three bits each. An entry of a single-value table holds the
high end in its low byte and the low end in the next byte.

Run with no argument. It needs numpy.
"""

import numpy

ETC1_MODIFIERS = ((2, 8), (5, 17), (9, 29), (13, 42), (18, 60), (24, 80), (33, 106), (47, 183))

# The runs of selectors that the BC1 tables have an entry for. Each run is its lowest selector and its highest.
BC1_RUNS = ((0, 3), (1, 3), (0, 2), (1, 2), (2, 3), (0, 1))

# The BC1 color that each placing gives to each of the four selectors. The four BC1 colors are numbered from the low
# end to the high end.
BC1_PLACINGS = ((0, 0, 1, 1), (0, 0, 1, 2), (0, 0, 1, 3), (0, 0, 2, 3), (0, 1, 1, 1), (0, 1, 2, 2), (0, 1, 2, 3),
                (0, 2, 3, 3), (1, 2, 2, 2), (1, 2, 3, 3))

# The sets of selectors that the BC4 table has an entry for, as masks. They are the sets of three or four selectors.
BC4_SETS = (0b0111, 0b1011, 0b1101, 0b1110, 0b1111)


# The last column that a line of the header may reach.
LIMIT = 120


def colors(table, base):
    """Returns the four values of a channel of an ETC1S block, indexed by selector."""
    small, large = ETC1_MODIFIERS[table]
    expanded = base << 3 | base >> 2
    return [min(255, max(0, expanded + delta)) for delta in (-large, -small, small, large)]


def bc1_table(bits):
    """Returns the words of the BC1 table for ends of `bits` bits, ordered by table, base, run and placing."""
    levels = 1 << bits
    ends = numpy.arange(levels, dtype=numpy.int64)
    expanded = ends << (8 - bits) | ends >> (2 * bits - 8)
    # The four colors, from the low end to the high end. The array is indexed by high end, then by low end.
    low = expanded.reshape(1, levels)
    high = expanded.reshape(levels, 1)
    palette = numpy.stack([numpy.broadcast_to(low, (levels, levels)), (low * 2 + high) // 3, (high * 2 + low) // 3,
                           numpy.broadcast_to(high, (levels, levels))])
    words = []
    for table in range(8):
        for base in range(32):
            values = colors(table, base)
            for lowest, highest in BC1_RUNS:
                for placing in BC1_PLACINGS:
                    total = numpy.zeros((levels, levels), dtype=numpy.int64)
                    for selector in range(lowest, highest + 1):
                        total += (values[selector] - palette[placing[selector]]) ** 2
                    best = int(total.argmin())
                    error = int(total.flat[best])
                    assert error <= 0xFFFF
                    words.append(best % levels | (best // levels) << 8 | error << 16)
    return words


def bc1_one_color(bits, third):
    """Returns the words of a single-value table for ends of `bits` bits, one word for each value of 8 bits. If `third`
    is set, a word holds the high end and the low end that best put the value a third of the way from the high end to
    the low end, with a small penalty for ends that are far apart. Otherwise a word holds the one end nearest the
    value."""
    levels = 1 << bits
    ends = numpy.arange(levels, dtype=numpy.int64)
    expanded = ends << (8 - bits) | ends >> (2 * bits - 8)
    low = expanded.reshape(levels, 1)
    high = expanded.reshape(1, levels)
    words = []
    for value in range(256):
        if third:
            error = numpy.abs((high * 2 + low) // 3 - value) + numpy.abs(high - low) * 3 // 100
        else:
            error = numpy.broadcast_to(numpy.abs(high - value), (1, levels))
        best = int(error.argmin())
        words.append(best % levels | (best // levels if third else 0) << 8)
    return words


def bc4_table():
    """Returns the words of the BC4 table, ordered by table, base and set of selectors."""
    first = numpy.arange(256, dtype=numpy.float64).reshape(256, 1, 1)
    second = numpy.arange(256, dtype=numpy.float64).reshape(1, 256, 1)
    eight = numpy.arange(8, dtype=numpy.float64).reshape(1, 1, 8)
    six = numpy.minimum(eight, 5)
    # The eight BC4 values, by index. The two ends come first. If the first end is the greater, six values between the
    # ends follow. Otherwise four values between the ends follow, then zero and 255.
    between8 = (first * (7 - (eight - 1)) + second * (eight - 1)) / 7
    between6 = (first * (5 - (six - 1)) + second * (six - 1)) / 5
    order = numpy.array([0, 1, 2, 3, 4, 5, 6, 7]).reshape(1, 1, 8)
    values8 = numpy.where(order == 0, first, numpy.where(order == 1, second, between8))
    values6 = numpy.where(order == 0, first, numpy.where(order == 1, second,
                                                         numpy.where(order == 6, 0.0, numpy.where(order == 7, 255.0,
                                                                                                  between6))))
    values = numpy.where(first > second, values8, values6)
    words = []
    for table in range(8):
        for base in range(32):
            wanted = colors(table, base)
            errors = []
            picks = []
            for value in wanted:
                distance = (values - value) ** 2
                picks.append(distance.argmin(axis=2))
                errors.append(distance.min(axis=2))
            for mask in BC4_SETS:
                total = sum(errors[s] for s in range(4) if mask >> s & 1)
                best = numpy.unravel_index(total.argmin(), total.shape)
                word = int(best[0]) | int(best[1]) << 8
                for s in range(4):
                    if mask >> s & 1:
                        word |= int(picks[s][best]) << (16 + 3 * s)
                words.append(word)
    return words


def emit(comment, name, shape, words):
    """Prints `words` as a constexpr array of `shape` under `comment`. An innermost list that fits on one line is
    printed on one line. A longer list is printed with as many entries on each line as fit."""
    dims = "".join("[%d]" % d for d in shape)
    print("// %s" % comment)
    print("inline constexpr uint%d_t %s%s = {" % (32 if max(words) > 0xFFFF else 16, name, dims))

    def nested(at, depth, indent):
        size = 1
        for d in shape[depth + 1:]:
            size *= d
        if depth == len(shape) - 1:
            entries = ["0x%x" % word for word in words[at:at + shape[depth]]]
            one = "%s{%s}," % (indent, ", ".join(entries))
            if len(one) <= LIMIT:
                return [one]
            lines = []
            line = indent + "    "
            for entry in entries:
                piece = entry + ","
                if len(line) + 1 + len(piece) > LIMIT and line.strip():
                    lines.append(line)
                    line = indent + "    "
                line += (" " if line.strip() else "") + piece
            lines.append(line)
            return ["%s{" % indent] + lines + ["%s}," % indent]
        out = ["%s{" % indent]
        for i in range(shape[depth]):
            out += nested(at + i * size, depth + 1, indent + "    ")
        return out + ["%s}," % indent]

    lines = nested(0, 0, "")
    print("\n".join(lines[1:-1]))
    print("};\n")


HEADER = """\
// Tables of the BC1 and BC4 block ends that best represent an ETC1S block. The transcoders of ETC1S to BC1, BC3, BC4
// and BC5 read them. tests/gen_basis_bc_tables.py prints this file. Its BC1 search repeats the search that made the
// tables of Basis Universal's transcoder, so a transcoded block equals the block that the reference transcoder writes.
//
// kBasisBc1Five, kBasisBc1Six and kBasisBc4 are indexed by the ETC1 modifier table, then by the channel's base value of
// five bits. kBasisBc1Five and kBasisBc1Six have an entry for each run of selectors in kBasisBc1Runs and each placing
// in kBasisBc1Placings. An entry holds the low end of a channel in its low byte, the high end in the next byte, and
// from bit 16 the squared error that the two ends leave. kBasisBc4 has an entry for each set of selectors in
// kBasisBc4Sets. An entry holds the first end in its low byte, the second end in the next byte, and from bit 16 the BC4
// index of each selector, three bits each.
//
// kBasisBc1OneThird and kBasisBc1OneEnd are indexed by the width of the ends, 5 bits then 6 bits, and then by a value
// of 8 bits. An entry of kBasisBc1OneThird holds the high end in its low byte and the low end in the next byte. Those
// two ends put the value a third of the way from the high end to the low end. An entry of kBasisBc1OneEnd holds the one
// end nearest the value.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The runs of selectors that the BC1 tables have an entry for. Each run is its lowest selector and its highest
// selector.
inline constexpr uint8_t kBasisBc1Runs[6][2] = {{0, 3}, {1, 3}, {0, 2}, {1, 2}, {2, 3}, {0, 1}};

// The BC1 color that each placing assigns to each of the four selectors. The four BC1 colors are numbered from the low
// end to the high end.
inline constexpr uint8_t kBasisBc1Placings[10][4] = {
    {0, 0, 1, 1}, {0, 0, 1, 2}, {0, 0, 1, 3}, {0, 0, 2, 3}, {0, 1, 1, 1},
    {0, 1, 2, 2}, {0, 1, 2, 3}, {0, 2, 3, 3}, {1, 2, 2, 2}, {1, 2, 3, 3},
};

// The sets of selectors that the BC4 table has an entry for. Each set is a mask of the selectors that a block uses.
inline constexpr uint8_t kBasisBc4Sets[5] = {0x7, 0xB, 0xD, 0xE, 0xF};
"""

FOOTER = """\
} // namespace pr_impl

PR_ASSUME_NONNULL_END"""


def main():
    print(HEADER)
    emit("The ends of 5 bits for red and blue, indexed by table, base, run and placing.", "kBasisBc1Five",
         (8, 32, 6, 10), bc1_table(5))
    emit("The ends of 6 bits for green, indexed by table, base, run and placing.", "kBasisBc1Six", (8, 32, 6, 10),
         bc1_table(6))
    emit("The ends that put a value of 8 bits a third of the way from the high end, indexed by end width, then by "
         "value.", "kBasisBc1OneThird", (2, 256), bc1_one_color(5, True) + bc1_one_color(6, True))
    emit("The end nearest a value of 8 bits, indexed by end width, then by value.", "kBasisBc1OneEnd", (2, 256),
         bc1_one_color(5, False) + bc1_one_color(6, False))
    emit("The BC4 ends and the BC4 index of each selector, indexed by table, base and set of selectors.", "kBasisBc4",
         (8, 32, 5), bc4_table())
    print(FOOTER)


if __name__ == "__main__":
    main()
