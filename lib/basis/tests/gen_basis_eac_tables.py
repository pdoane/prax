#!/usr/bin/env python3
"""Prints the tables of basis_etc1s_eac.h: the EAC block that best represents one channel of an ETC1S block.

A channel of an ETC1S block has a base value of five bits, one of the eight ETC1 modifier tables, and the selectors that
its texels use. A run is the range of selectors that a block uses, from the lowest of the four selectors to the highest.
For each base, table and run, the script tries every base, multiplier and table of EAC. Each selector takes the nearest
value of the EAC block. The script keeps the block that leaves the least squared error for the values of the run's
selectors. If several blocks leave the least error, it keeps the first in the order base, multiplier, table. This search
repeats the search that made the tables of Basis Universal's transcoder, so a transcoded block equals the block that the
reference transcoder writes.

An entry is printed as one word. It holds the base in its low byte. The next byte holds the multiplier in its high four
bits and the table in its low four bits, as the second byte of an EAC block does. From bit 16 the word holds the EAC
index of each selector, three bits each.

Run with no argument. It needs numpy.
"""

import numpy

ETC1_MODIFIERS = ((2, 8), (5, 17), (9, 29), (13, 42), (18, 60), (24, 80), (33, 106), (47, 183))
EAC_MODIFIERS = numpy.array((
    (-3, -6, -9, -15, 2, 5, 8, 14), (-3, -7, -10, -13, 2, 6, 9, 12), (-2, -5, -8, -13, 1, 4, 7, 12),
    (-2, -4, -6, -13, 1, 3, 5, 12), (-3, -6, -8, -12, 2, 5, 7, 11), (-3, -7, -9, -11, 2, 6, 8, 10),
    (-4, -7, -8, -11, 3, 6, 7, 10), (-3, -5, -8, -11, 2, 4, 7, 10), (-2, -6, -8, -10, 1, 5, 7, 9),
    (-2, -5, -8, -10, 1, 4, 7, 9), (-2, -4, -8, -10, 1, 3, 7, 9), (-2, -5, -7, -10, 1, 4, 6, 9),
    (-3, -4, -7, -10, 2, 3, 6, 9), (-1, -2, -3, -10, 0, 1, 2, 9), (-4, -6, -8, -9, 3, 5, 7, 8),
    (-3, -5, -7, -9, 2, 4, 6, 8)), dtype=numpy.int64)

# The runs of selectors that have an entry. A block with any other run takes the first entry.
RUNS = ((0, 3), (1, 3), (0, 2), (1, 2))


def candidates(eleven):
    """Returns the values of every EAC block, indexed by base, multiplier, table and index, and the lowest multiplier
    that the search tries."""
    bases = numpy.arange(256, dtype=numpy.int64).reshape(256, 1, 1, 1)
    first = 0 if eleven else 1
    multipliers = numpy.arange(first, 16, dtype=numpy.int64).reshape(1, 16 - first, 1, 1)
    modifiers = EAC_MODIFIERS.reshape(1, 1, 16, 8)
    if eleven:
        values = numpy.where(multipliers != 0, multipliers * 8, 1) * modifiers + bases * 8 + 4
        return numpy.clip(values, 0, 2047), first
    return numpy.clip(multipliers * modifiers + bases, 0, 255), first


def table(eleven):
    """Returns the words of one table, ordered by ETC1 modifier table, base and run."""
    values, first = candidates(eleven)
    words = []
    for modifiers in ETC1_MODIFIERS:
        deltas = (-modifiers[1], -modifiers[0], modifiers[0], modifiers[1])
        for base in range(32):
            colors = [min(255, max(0, (base << 3 | base >> 2) + delta)) for delta in deltas]
            for low, high in RUNS:
                total = numpy.zeros(values.shape[:3], dtype=numpy.int64)
                picks = []
                for selector in range(low, high + 1):
                    wanted = (colors[selector] * 2047 + 128) // 255 if eleven else colors[selector]
                    errors = numpy.abs(values - wanted)
                    picks.append(errors.argmin(axis=3))
                    total += errors.min(axis=3) ** 2
                best = numpy.unravel_index(total.argmin(), total.shape)
                word = int(best[0]) | (int(best[1]) + first) << 12 | int(best[2]) << 8
                for selector in range(low, high + 1):
                    word |= int(picks[selector - low][best]) << (16 + 3 * selector)
                words.append(word)
    return words


def main():
    for name, eleven in (("kBasisEacAlpha", False), ("kBasisEacEleven", True)):
        words = table(eleven)
        print("inline constexpr uint32_t %s[8][32][4] = {" % name)
        for at in range(0, len(words), 128):
            print("    {")
            for row in range(at, at + 128, 8):
                entries = ["{%s}" % ", ".join("0x%07x" % word for word in words[start:start + 4])
                           for start in (row, row + 4)]
                print("        %s," % ", ".join(entries))
            print("    },")
        print("};\n")


if __name__ == "__main__":
    main()
