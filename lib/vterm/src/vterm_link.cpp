// The hyperlinks that a program puts on cells: OSC 8, the table of links, and the sweep that frees unused links.

#include "vterm/src/vterm_link.h"

#include "pr/assert.h"
#include "pr/hash.h"
#include "pr/string_ops.h"

#include "vterm/src/vterm_screen.h"

namespace pr_impl {

namespace {

// The fewest live links at which a sweep runs.
constexpr uint32_t kSweepMinLinks = 256;

// The number of lines that a sweep may visit for each link opened since the last sweep. This bounds the cost of
// sweeping a long scrollback for each link.
constexpr uint64_t kSweepLinesPerLink = 1024;

// Returns true if `text` holds only printable ASCII. If `spaces` is false, it may not hold a space either.
bool isPrintable(PrStringView text, bool spaces) {
    for (size_t i = 0; i < text.size; ++i) {
        uint8_t byte = uint8_t(text.data[i]);
        if (byte < (spaces ? 0x20 : 0x21) || byte > 0x7e)
            return false;
    }
    return true;
}

uint32_t hashOf(PrStringView id, PrStringView uri) {
    uint64_t hash = prHashBytes(id.data, id.size, 0);
    return uint32_t(prHashBytes(uri.data, uri.size, hash));
}

void linkRelease(VtermLinks* links, PrVtermLinkIndex index) {
    VtermLinkEntry& link = links->entries.data[index];
    if (!prRawStringIsNull(&link.id))
        links->ids.erase(link.hash, index);
    prRawStringReset(&link.uri);
    prRawStringReset(&link.id);
    links->free.push(index);
    --links->live;
}

void mark(VtermLinks* links, PrVtermLinkIndex index) {
    if (index != kPrVtermLinkNone)
        links->entries.data[index].marked = true;
}

// Records the links that the cells of `line` hold, as of the line's revision.
void lineCollect(VtermStoredLine* line) {
    line->links.clear();
    PrVtermLinkIndex last = kPrVtermLinkNone;
    for (uint32_t i = 0; i < line->cells.count; ++i) {
        PrVtermLinkIndex link = line->cells.data[i].link;
        if (link != kPrVtermLinkNone && link != last)
            line->links.push(link);
        last = link;
    }
    line->linksRevision = line->revision;
}

// Frees the links that neither a cell nor the open link holds.
void sweep(Vterm* vterm) {
    VtermLinks* links = &vterm->links;
    for (uint32_t i = 1; i < links->entries.count; ++i)
        links->entries.data[i].marked = false;
    mark(links, vterm->link);
    for (VtermScreen& screen : vterm->screens) {
        for (uint32_t i = 0; i < screen.rows.slots.count; ++i) {
            VtermStoredLine& line = screen.rows.slots.data[i];
            if (line.linksRevision != line.revision)
                lineCollect(&line);
            for (PrVtermLinkIndex link : line.links.span())
                mark(links, link);
        }
    }
    // A packed line lists its links as runs. Only a line with links has them.
    const VtermScrollback* scrollback = &vterm->scrollback;
    for (uint32_t i = 0; i < scrollback->count; ++i) {
        const VtermPackedLine& line = vtermScrollbackAt(scrollback, i);
        if ((line.packed & VtermPackedFlag_Links) == 0)
            continue;
        VtermPackedView view = vtermPackedView(scrollback, line);
        const uint8_t* at = view.links;
        while (at < view.links + view.linkBytes) {
            vtermPackedReadCount(&at);
            vtermPackedReadCount(&at);
            mark(links, PrVtermLinkIndex(vtermPackedReadCount(&at)));
        }
    }
    for (uint32_t i = 1; i < links->entries.count; ++i) {
        VtermLinkEntry& link = links->entries.data[i];
        if (!prRawStringIsNull(&link.uri) && !link.marked)
            linkRelease(links, PrVtermLinkIndex(i));
    }
    uint32_t next = 2 * links->live;
    if (next < kSweepMinLinks)
        next = kSweepMinLinks;
    links->sweepAt = next < kPrVtermMaxLinks ? next : uint32_t(kPrVtermMaxLinks);
    links->opened = 0;
}

// Returns the link to `uri` with `id`. If `id` is not empty and a link has both, returns that link. Otherwise returns a
// new link, or kPrVtermLinkNone if the table is full.
PrVtermLinkIndex linkOpen(Vterm* vterm, PrStringView id, PrStringView uri) {
    VtermLinks* links = &vterm->links;
    uint32_t hash = 0;
    if (id.size != 0) {
        hash = hashOf(id, uri);
        uint32_t found = links->ids.find(hash, [&](uint32_t at) {
            const VtermLinkEntry& link = links->entries.data[at];
            return prRawStringView(&link.id) == id && prRawStringView(&link.uri) == uri;
        });
        if (found != kHashMissing)
            return PrVtermLinkIndex(found);
    }
    ++links->opened;
    uint64_t lines = uint64_t(vtermLineCount(vterm)) + vterm->rows;
    if (links->live >= links->sweepAt && uint64_t(links->opened) * kSweepLinesPerLink >= lines)
        sweep(vterm);
    if (links->entries.count == 0)
        links->entries.push(VtermLinkEntry{});
    PrVtermLinkIndex index = kPrVtermLinkNone;
    if (links->free.count != 0) {
        index = links->free.last();
        links->free.pop();
    } else if (links->entries.count <= kPrVtermMaxLinks) {
        index = PrVtermLinkIndex(links->entries.count);
        links->entries.push(VtermLinkEntry{});
    } else {
        return kPrVtermLinkNone;
    }
    VtermLinkEntry& link = links->entries.data[index];
    link.uri = prRawStringCopy(uri);
    link.id = id.size != 0 ? prRawStringCopy(id) : prRawStringNull();
    link.hash = hash;
    if (id.size != 0)
        links->ids.insert(hash, index);
    ++links->live;
    return index;
}

} // namespace

void vtermLinksReset(VtermLinks* links) {
    for (VtermLinkEntry& link : links->entries.span()) {
        prRawStringReset(&link.uri);
        prRawStringReset(&link.id);
    }
    links->entries.reset();
    links->free.reset();
    links->ids.reset();
    links->live = 0;
    links->sweepAt = kSweepMinLinks;
    links->opened = 0;
}

PrVtermLink vtermLinksAt(const VtermLinks* links, PrVtermLinkIndex index) {
    PR_ASSERT(index != kPrVtermLinkNone && index < links->entries.count);
    const VtermLinkEntry& link = links->entries.data[index];
    PR_ASSERT(!prRawStringIsNull(&link.uri));
    PrVtermLink out{.uri = prRawStringView(&link.uri), .id = ""};
    if (!prRawStringIsNull(&link.id))
        out.id = prRawStringView(&link.id);
    return out;
}

void vtermLinkCommand(Vterm* vterm, PrStringView rest) {
    size_t separator = prStringFindByte(rest, ';');
    if (separator == rest.size)
        return;
    PrStringView params = prStringSlice(rest, 0, separator);
    PrStringView uri = prStringSlice(rest, separator + 1, rest.size - separator - 1);
    vterm->link = kPrVtermLinkNone;
    if (uri.size == 0 || uri.size > kPrVtermLinkMaxUriBytes || !isPrintable(uri, true))
        return;
    PrStringView id = prStringSlice(params, 0, 0);
    while (params.size != 0) {
        size_t colon = prStringFindByte(params, ':');
        PrStringView param = prStringSlice(params, 0, colon);
        size_t next = colon < params.size ? colon + 1 : params.size;
        params = prStringSlice(params, next, params.size - next);
        if (prStringStartsWith(param, "id="))
            id = prStringSlice(param, 3, param.size - 3);
    }
    if (id.size > kPrVtermLinkMaxIdBytes || !isPrintable(id, false))
        return;
    vterm->link = linkOpen(vterm, id, uri);
}

} // namespace pr_impl
