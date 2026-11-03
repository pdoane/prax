// Atom tables.

#include "pr/atom_table.h"

#include "pr/hash.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/string_ops.h"

namespace pr {
namespace {

constexpr uint64_t kAtomSeed = 0x6a09e667f3bcc909ull;

uint32_t atomHash(PrStringView text) {
    return uint32_t(prHashBytes(text.data, text.size, kAtomSeed));
}

// Returns the slot that holds `text`, or the empty slot where it would go.
uint32_t slotOf(const AtomTable* table, PrStringView text, uint32_t hash) {
    for (uint32_t slot = hash & table->mask;; slot = (slot + 1) & table->mask) {
        const AtomSlot& at = table->slots[slot];
        if (at.atom == kAtomNone)
            return slot;
        const AtomEntry& entry = table->atoms.data[at.atom];
        if (at.hash == hash && entry.size == text.size &&
                (text.size == 0 || prMemEquals(entry.text, text.data, text.size)))
            return slot;
    }
}

void clearSlots(AtomSlot* slots, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i)
        slots[i] = AtomSlot{0, kAtomNone};
}

// Doubles the slots and reinserts every atom.
void grow(AtomTable* table) {
    uint32_t size = table->slots == nullptr ? 64 : (table->mask + 1) * 2;
    prMemoryFree(table->slots);
    table->slots = static_cast<AtomSlot*>(prMemoryAlloc(sizeof(AtomSlot) * size));
    table->mask = size - 1;
    clearSlots(table->slots, size);
    for (Atom atom = 0; atom < table->atoms.count; ++atom) {
        uint32_t slot = table->atoms.data[atom].hash & table->mask;
        while (table->slots[slot].atom != kAtomNone)
            slot = (slot + 1) & table->mask;
        table->slots[slot] = AtomSlot{table->atoms.data[atom].hash, atom};
    }
}

// Returns storage for `size` bytes of text: in the page being filled, in the next page, or in a separate allocation.
char* store(AtomTable* table, uint32_t size) {
    if (size > kAtomPageSize) {
        char* text = static_cast<char*>(prMemoryAlloc(size));
        table->large.push(text);
        table->largeBytes += size;
        return text;
    }
    if (table->page == table->pages.count || table->used + size > kAtomPageSize) {
        if (table->page != table->pages.count)
            ++table->page;
        if (table->page == table->pages.count)
            table->pages.push(static_cast<char*>(prMemoryAlloc(kAtomPageSize)));
        table->used = 0;
    }
    char* text = table->pages.data[table->page] + table->used;
    table->used += size;
    return text;
}

} // namespace

Atom atomTableAdd(AtomTable* table, PrStringView text) {
    // The slots are at most half full.
    if (table->slots == nullptr || (table->atoms.count + 1) * 2 > table->mask + 1)
        grow(table);
    uint32_t hash = atomHash(text);
    uint32_t slot = slotOf(table, text, hash);
    if (table->slots[slot].atom != kAtomNone)
        return table->slots[slot].atom;

    char* stored = nullptr;
    if (text.size != 0) {
        stored = store(table, uint32_t(text.size));
        prMemCopy(stored, text.data, text.size);
    }
    Atom atom = table->atoms.count;
    table->atoms.push(AtomEntry{stored, uint32_t(text.size), hash});
    table->slots[slot] = AtomSlot{hash, atom};
    return atom;
}

Atom atomTableFind(const AtomTable* table, PrStringView text) {
    if (table->slots == nullptr)
        return kAtomNone;
    return table->slots[slotOf(table, text, atomHash(text))].atom;
}

uint32_t atomTableBytes(const AtomTable* table) {
    uint32_t paged = table->page < table->pages.count ? table->page * kAtomPageSize + table->used : 0;
    return paged + table->largeBytes;
}

void atomTableClear(AtomTable* table) {
    table->atoms.count = 0;
    if (table->slots != nullptr)
        clearSlots(table->slots, table->mask + 1);
    for (uint32_t i = 0; i < table->large.count; ++i)
        prMemoryFree(table->large.data[i]);
    table->large.count = 0;
    table->largeBytes = 0;
    table->page = 0;
    table->used = 0;
}

void atomTableReset(AtomTable* table) {
    atomTableClear(table);
    for (uint32_t i = 0; i < table->pages.count; ++i)
        prMemoryFree(table->pages.data[i]);
    table->pages.reset();
    table->large.reset();
    table->atoms.reset();
    prMemoryFree(table->slots);
    table->slots = nullptr;
    table->mask = 0;
}

} // namespace pr
