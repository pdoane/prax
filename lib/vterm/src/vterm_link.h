// The hyperlinks that a program puts on cells (OSC 8). Cells hold links by index into a table, and a sweep frees the
// links that no cell holds.

#pragma once

#include "pr/base.h"
#include "pr/hash_index.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"
#include "pr/vterm.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct Vterm;

// An entry of the link table. A free entry has the null URI.
struct VtermLinkEntry {
    PrRawString uri;
    PrRawString id;             // the program's id, or the null string for a link opened without one
    uint32_t hash;              // the hash of the id and the URI, the key of a link with an id in `ids`
    bool marked;                // a cell or the open link holds the link, during a sweep
};

// A terminal's links. Cells hold links by index, and the table frees a link once no cell holds it. A sweep marks the
// links that each line holds, and frees the rest. Each line records its links as of its revision, so a sweep reads the
// cells of only the lines that changed since the last sweep.
struct VtermLinks {
    RawArray<VtermLinkEntry> entries;       // by PrVtermLinkIndex, from 1
    RawArray<PrVtermLinkIndex> free;        // the indices of the free entries
    HashIndex ids;                          // the entries of the links with ids, by `hash`
    uint32_t live;                          // the number of entries that are not free

    // A sweep waits until `live` reaches `sweepAt` and enough links were opened since the last sweep. This keeps sweeps
    // of a long scrollback rare, so that their cost for each link stays small.
    uint32_t sweepAt;
    uint32_t opened;
};

// Frees the links' storage, and leaves the table empty.
void vtermLinksReset(VtermLinks* links);

// The link at `index`, which is not free.
PrVtermLink vtermLinksAt(const VtermLinks* links, PrVtermLinkIndex index);

// Runs OSC 8 with `rest`, the text after its number: the params, and the URI that opens a link or the empty URI that
// ends it.
void vtermLinkCommand(Vterm* vterm, PrStringView rest);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
