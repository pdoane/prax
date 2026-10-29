"""Perfect hash tables over a fixed set of names.

A generator passes its names to `NameTable`, in the order that its enum numbers them, and gets back the tables and the
lines of the lookup. The runtime side is lib/pr/name_table.h.

The hash is a 64-bit mix over the name, read eight bytes at a time. Its high half picks a bucket and its low half a
slot. Every name in a bucket moves by the displacement that the bucket holds. No two names end on the same slot, so a
lookup reads one displacement, one slot and one name.

A slot that no name claims holds 0. A lookup that lands on such a slot compares the text against the name of entry 0
and fails. The caller chooses entry 0. An enum that reserves 0 for no name gives it the empty name, and an enum that
numbers its names from 0 gives it the first name.
"""

import sys

from tools.gen.gen_common import c_string, table

MASK = (1 << 64) - 1
SEED = 0x9E3779B97F4A7C15


def mix(hash):
    hash &= MASK
    hash ^= hash >> 33
    hash = (hash * 0xFF51AFD7ED558CCD) & MASK
    hash ^= hash >> 29
    hash = (hash * 0xC4CEB9FE1A85EC53) & MASK
    hash ^= hash >> 32
    return hash


def fold(word):
    """nameFold: returns a word with every ASCII uppercase byte folded to lowercase."""
    out = 0
    for i in range(8):
        byte = (word >> (8 * i)) & 0xFF
        out |= (byte | 0x20 if 0x41 <= byte <= 0x5A else byte) << (8 * i)
    return out


def hash_of(name, lower, seed, salt=None):
    """nameHash and nameHashLower, which the tables must agree with byte for byte.

    A salt is mixed in after the name. That lets one table hold a set keyed by more than a name, such as descriptors,
    each of which is an at-rule and a name."""
    text = name.encode("utf-8")
    size = len(text)
    word = lambda at: int.from_bytes(text[at:at + 8], "little")
    if lower:
        word = lambda at, w=word: fold(w(at))

    hash = seed ^ size
    if size < 8:
        return _salted(mix(hash ^ word(0)), salt)
    at = 0
    while at < size - 8:
        hash = mix(hash ^ word(at))
        at += 8
    return _salted(mix(hash ^ word(size - 8)), salt)


def _salted(hash, salt):
    return hash if salt is None else mix(hash ^ salt)


def _slot_of(hash, amount, slots):
    """Returns the slot that a name lands on. Each name has its own stride, which moves the names of a bucket by
    different amounts. Adding the same amount to all of them would leave two names with the same low bits on the same
    slot, whatever the amount."""
    return ((hash & 0xFFFFFFFF) + amount * ((hash >> 32) | 1)) & (slots - 1)


def _place(hashes, buckets, slots):
    """Returns a displacement for each bucket that gives every name its own slot, or None.

    Placing the crowded buckets first leaves gaps for the rest to fall into.
    """
    members = {}
    for index, hash in enumerate(hashes):
        members.setdefault((hash >> 32) & (buckets - 1), []).append(index)

    table = [0] * buckets
    slot = [0] * slots
    taken = [False] * slots
    for bucket, group in sorted(members.items(), key=lambda pair: -len(pair[1])):
        for amount in range(1 << 16):
            at = [_slot_of(hashes[i], amount, slots) for i in group]
            if len(set(at)) == len(at) and not any(taken[i] for i in at):
                table[bucket] = amount
                for i, index in zip(at, group):
                    taken[i] = True
                    slot[i] = index
                break
        else:
            return None
    return table, slot


def displace(names, lower, salts):
    """Returns a seed, a displacement table and the slots, using the fewest buckets that can hold the names.

    The bucket count starts at four names per bucket and doubles until the displacements succeed.
    """
    count = len(names)
    slots = 1 << (count - 1).bit_length()

    for trial in range(256):
        seed = mix(SEED + trial * 0x9E3779B97F4A7C15)
        hashes = [hash_of(name, lower, seed, salt)
                  for name, salt in zip(names, salts or [None] * count)]
        if len(set(hashes)) != count:
            continue
        buckets = 1 << max(1, (count // 4).bit_length())
        while buckets <= slots:
            placed = _place(hashes, buckets, slots)
            if placed is not None:
                return (seed, buckets) + placed
            buckets *= 2

    sys.exit("name_table: no perfect hash over %d names" % count)


def width(values):
    largest = max(values) if values else 0
    return "uint8_t" if largest < 256 else "uint16_t" if largest < 65536 else "uint32_t"


class Blob:
    """Texts stored end to end, and the offset that each starts at.

    The offsets run one past the texts, and a length is the step from one offset to the next.
    Eight bytes of padding follow the last text, so a word read near the end stays in bounds.
    """

    def __init__(self, prefix, texts, per_line=90):
        self.prefix = prefix
        self.offsets = []
        at = 0
        for text in texts:
            self.offsets.append(at)
            at += len(text.encode("utf-8"))
        self.offsets.append(at)

        out = ["\nconstexpr char %sBlob[] =\n" % prefix]
        line = ""
        for text in texts:
            if text == "":
                continue
            piece = c_string(text)
            if len(line) + len(piece) > per_line:
                out.append("    %s\n" % line)
                line = ""
            line += piece if line == "" else " " + piece
        if line:
            out.append("    %s\n" % line)
        out.append('    "\\0\\0\\0\\0\\0\\0\\0\\0";\n')
        self.source = "".join(out)

    def tables(self):
        return self.source + table("constexpr %s %sAt[]" % (width(self.offsets), self.prefix),
                                   self.offsets, 12)

    def view(self, index):
        """Returns the expression for the text at `index`."""
        return "nameAt(%(p)sBlob, %(p)sAt, %(i)s)" % {"p": self.prefix, "i": index}


class NameTable:
    """The tables for one set of names, and the expressions that a lookup is written with.

    `names` is indexed the way the caller's enum is.
    """

    def __init__(self, prefix, names, lower, salts=None):
        if lower and any(name != name.lower() for name in names):
            sys.exit("%s: a folded table takes lowercase names" % prefix)
        keys = list(names) if salts is None else list(zip(names, salts))
        if len(set(keys)) != len(keys):
            sys.exit("%s: a name is in the set twice" % prefix)

        self.prefix = prefix
        self.lower = lower
        self.count = len(names)
        self.seed, self.buckets, self.table, self.slot = displace(names, lower, salts)
        self.names = Blob(prefix, names)
        self.slots = len(self.slot)

    def tables(self):
        """Returns every table that the lookup reads, as the source declares them."""
        return "".join([
            self.names.tables(),
            table("constexpr %s %sDisplace[]" % (width(self.table), self.prefix), self.table, 12),
            table("constexpr %s %sSlot[]" % (width(self.slot), self.prefix), self.slot, 12),
        ])

    def name(self, index):
        """Returns the expression for the name of entry `index`."""
        return self.names.view(index)

    def lookup(self, text, index, salt=None):
        """Returns the lines that map `text` to the entry that it names, stored in `index`, and leave the confirmation
        to the caller. `index` is 0 if no name matches."""
        out = ("    uint64_t word;\n"
               "    uint64_t hash = %(h)s(%(t)s.data, %(t)s.size, 0x%(s)016xull, &word);\n"
               % {"h": "nameHashLower" if self.lower else "nameHash",
                  "t": text, "s": self.seed})
        if salt is not None:
            out += "    hash = nameMix(hash ^ %s);\n" % salt
        return out + ("    uint32_t %(i)s = %(p)sSlot[(uint32_t(hash)\n"
                      "        + %(p)sDisplace[(hash >> 32) & %(b)du] * (uint32_t(hash >> 32) | "
                      "1))\n        & %(m)du];\n" % {"i": index, "p": self.prefix,
                         "b": self.buckets - 1, "m": self.slots - 1})

    def confirm(self, text, name):
        """Returns the expression that compares `name`, the name that the lookup reached, with `text`."""
        equals = "nameEqualsLower" if self.lower else "nameEquals"
        return "%s(%s, %s, word)" % (equals, name, text)
