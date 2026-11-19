// Atom tables: interned strings, compared as numbers.

#pragma once

#include "pr/arena.h"
#include "pr/base.h"
#include "pr/raw_array.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// A string in a table, numbered from zero in the order the strings were added.
typedef uint32_t Atom;

// A value that is never an atom.
constexpr Atom kAtomNone = 0xffffffffu;

// The text of an atom, and its hash.
struct AtomEntry {
    const char* PR_NULLABLE text;
    uint32_t size;
    uint32_t hash;
};

// A slot of the hash index over the atoms.
struct AtomSlot {
    uint32_t hash;
    Atom atom;                  // kAtomNone for an empty slot
};

// A table of atoms. An atom stays valid as long as the table holds it. The table only adds atoms, and clearing it
// forgets every atom at once. The text of an atom never moves.
struct AtomTable {
    RawArray<AtomEntry> atoms;          // by atom
    AtomSlot* PR_NULLABLE slots;
    uint32_t mask;                    // the number of slots minus one, or zero if there are no slots
    Arena text;                       // the text of the atoms
};

// Returns the atom of `text`, and adds `text` if the table does not hold it.
Atom atomTableAdd(AtomTable* table, PrStringView text);

// Returns the atom of `text`, or kAtomNone if the table does not hold it.
Atom atomTableFind(const AtomTable* table, PrStringView text);

// Returns the text of `atom`. The table must hold `atom`.
inline PrStringView atomTableText(const AtomTable* table, Atom atom) {
    const AtomEntry& entry = table->atoms.data[atom];
    return PrStringView{entry.text, entry.size};
}

// Returns the number of bytes that the text of the atoms occupies.
uint32_t atomTableBytes(const AtomTable* table);

// Forgets every atom, and keeps the pages and slots for new atoms.
void atomTableClear(AtomTable* table);

// Frees the storage of the table and leaves it empty.
void atomTableReset(AtomTable* table);

} // namespace pr

PR_ASSUME_NONNULL_END
