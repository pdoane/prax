// Grid templates: the track lists, template areas and names that a tree interns for its grid groups.

#include "layout/src/layout_grid_template.h"

#include "pr/alloc.h"
#include "pr/hash.h"
#include "pr/mem.h"
#include "pr/string_ops.h"

namespace pr_impl {

namespace {

// The seed that the keys of track lists and areas are hashed with.
constexpr uint64_t kGridTemplateSeed = 0x6a09e667bb67ae85ull;

// Appends `size` bytes from `bytes` to `key`.
void appendKey(Array<uint8_t>& key, const void* PR_NULLABLE bytes, size_t size) {
    if (size == 0)
        return;
    uint32_t at = key.count;
    key.resize(at + uint32_t(size));
    memCopy(key.data + at, bytes, size);
}

// Appends a count to `key`, so that lists of different lengths have different keys.
void appendCount(Array<uint8_t>& key, uint32_t count) {
    appendKey(key, &count, sizeof count);
}

// Returns a copy of `count` items from `items`, which the caller frees, or null if there are none.
template <typename T>
T* PR_NULLABLE copyOf(const T* PR_NULLABLE items, uint32_t count) {
    if (count == 0)
        return nullptr;
    T* copy = static_cast<T*>(prMemoryAlloc(sizeof(T) * count));
    memCopy(copy, items, sizeof(T) * count);
    return copy;
}

// Returns the index of the entry of `table` whose key is `key`, or adds an entry for it, by `make`, and returns the
// entry's index. A new entry takes the key and goes in the table's index and candidates. Frees `key` if an entry holds
// it already.
template <typename Entry, typename Make>
uint32_t intern(LayoutGridTable<Entry>& table, Array<uint8_t>& key, Make make) {
    uint32_t hash = uint32_t(hashBytes(key.data, key.count, kGridTemplateSeed));
    uint32_t found = table.index.find(hash, [&](uint32_t i) {
        const Entry& e = table.entries.data[i];
        return e.key.count == key.count && memEquals(e.key.data, key.data, key.count);
    });
    if (found != kHashMissing) {
        key.reset();
        return found;
    }
    uint32_t at = 0;
    if (table.free.count != 0) {
        at = table.free.last();
        table.free.pop();
    } else {
        at = table.entries.count;
        table.entries.push(Entry{});
    }
    Entry& entry = table.entries.data[at];
    make(entry);
    entry.key = key;
    entry.hash = hash;
    entry.references = 0;
    table.index.insert(hash, at);
    table.candidates.push(at);
    return at;
}

// Adds `delta` to the references of the entry at `index` of `table`, unless it is the empty entry.
template <typename Entry>
void count(LayoutGridTable<Entry>& table, uint32_t index, uint32_t delta) {
    if (index == 0)
        return;
    Entry& entry = table.entries.data[index];
    entry.references += delta;
    if (entry.references == 0)
        table.candidates.push(index);
}

// Adds `delta` to the references of the compounds that the breadths and limits of `count` tracks name.
void countTracks(LayoutCompounds* compounds, const PrLayoutGridTrack* PR_NULLABLE tracks, uint32_t count, uint32_t delta) {
    for (uint32_t i = 0; i < count; ++i) {
        layoutCompoundsCount(compounds, tracks[i].min.bits, delta);
        layoutCompoundsCount(compounds, tracks[i].max.bits, delta);
        layoutCompoundsCount(compounds, tracks[i].limit.bits, delta);
    }
}

// Releases what a track list holds, and marks it free.
void freeTracks(LayoutGridTracksEntry& entry) {
    prMemoryFree(entry.tracks);
    prMemoryFree(entry.names);
    prMemoryFree(entry.repeatNames);
    entry.key.reset();
    entry = LayoutGridTracksEntry{};
}

// Releases what a template's areas hold, and marks them free.
void freeAreas(LayoutGridAreasEntry& entry) {
    prMemoryFree(entry.areas);
    entry.key.reset();
    entry = LayoutGridAreasEntry{};
}

// Returns true if a breadth is of a kind that PrLayoutTrackBreadth accepts.
bool isBreadth(PrLayoutTrackBreadth breadth) {
    switch (prLayoutWordKind(breadth.bits)) {
        case PrLayoutWordKind_Px:
        case PrLayoutWordKind_Percent:
        case PrLayoutWordKind_Number:
        case PrLayoutWordKind_Compound: return true;
        case PrLayoutWordKind_Keyword: {
            PrLayoutKeyword keyword = prLayoutWordKeywordValue(breadth.bits);
            return keyword == PrLayoutKeyword_Auto || keyword == PrLayoutKeyword_MinContent ||
                keyword == PrLayoutKeyword_MaxContent || keyword == PrLayoutKeyword_FitContent;
        }
        default: return false;
    }
}

// Returns true if a value of a grid-placement property is in its range.
bool isLine(const PrLayoutGridLine& line) {
    switch (line.kind) {
        case PrLayoutGridLineKind_Auto: return true;
        case PrLayoutGridLineKind_Line:
            return line.integer >= -kPrLayoutGridMaxLine && line.integer <= kPrLayoutGridMaxLine &&
                (line.integer != 0 || line.name != kPrLayoutGridNameNone);
        case PrLayoutGridLineKind_Span: return line.integer >= 1 && line.integer <= kPrLayoutGridMaxLine;
        default: return false;
    }
}

} // namespace

void layoutGridTemplatesInit(LayoutGridTemplates* templates) {
    templates->tracks.entries.push(LayoutGridTracksEntry{});
    templates->areas.entries.push(LayoutGridAreasEntry{});
}

PrLayoutGridTracksIndex layoutGridTemplatesInternTracks(LayoutGridTemplates* templates, LayoutCompounds* compounds,
        const PrLayoutGridTrackList& list) {
    if (list.trackCount == 0)
        return kPrLayoutGridTracksNone;
    PR_ASSERT(list.repeat == PrLayoutGridRepeat_None ||
        (list.repeatCount != 0 && list.repeatStart + list.repeatCount <= list.trackCount));
    for (uint32_t i = 0; i < list.trackCount; ++i)
        PR_ASSERT(isBreadth(list.tracks[i].min) && isBreadth(list.tracks[i].max));
    PrLayoutGridRepeat repeat = list.repeat;
    uint32_t repeatStart = repeat != PrLayoutGridRepeat_None ? list.repeatStart : 0;
    uint32_t repeatCount = repeat != PrLayoutGridRepeat_None ? list.repeatCount : 0;
    uint32_t repeatNameCount = repeat != PrLayoutGridRepeat_None ? list.repeatNameCount : 0;
    Array<uint8_t> key;
    appendCount(key, list.trackCount);
    appendKey(key, list.tracks, sizeof(PrLayoutGridTrack) * list.trackCount);
    appendCount(key, list.nameCount);
    appendKey(key, list.names, sizeof(PrLayoutGridLineName) * list.nameCount);
    appendKey(key, &repeat, sizeof repeat);
    appendCount(key, repeatStart);
    appendCount(key, repeatCount);
    appendCount(key, repeatNameCount);
    appendKey(key, list.repeatNames, sizeof(PrLayoutGridLineName) * repeatNameCount);
    return intern(templates->tracks, key, [&](LayoutGridTracksEntry& entry) {
        entry.tracks = copyOf(list.tracks, list.trackCount);
        entry.trackCount = list.trackCount;
        entry.names = copyOf(list.names, list.nameCount);
        entry.nameCount = list.nameCount;
        entry.repeat = repeat;
        entry.repeatStart = repeatStart;
        entry.repeatCount = repeatCount;
        entry.repeatNames = copyOf(list.repeatNames, repeatNameCount);
        entry.repeatNameCount = repeatNameCount;
        countTracks(compounds, entry.tracks, entry.trackCount, 1);
    });
}

PrLayoutGridAreasIndex layoutGridTemplatesInternAreas(LayoutGridTemplates* templates, const PrLayoutGridAreas& areas) {
    if (areas.rowCount == 0 || areas.columnCount == 0)
        return kPrLayoutGridAreasNone;
    PR_ASSERT(areas.rowCount <= kPrLayoutGridMaxLine && areas.columnCount <= kPrLayoutGridMaxLine);
    Array<uint8_t> key;
    appendCount(key, areas.rowCount);
    appendCount(key, areas.columnCount);
    appendCount(key, areas.areaCount);
    appendKey(key, areas.areas, sizeof(PrLayoutGridArea) * areas.areaCount);
    return intern(templates->areas, key, [&](LayoutGridAreasEntry& entry) {
        entry.areas = copyOf(areas.areas, areas.areaCount);
        entry.areaCount = areas.areaCount;
        entry.rowCount = areas.rowCount;
        entry.columnCount = areas.columnCount;
        entry.live = true;
    });
}

PrLayoutGridName layoutGridTemplatesInternName(LayoutGridTemplates* templates, PrStringView name) {
    Atom atom = atomTableAdd(&templates->names, name);
    while (templates->forms.count <= atom)
        templates->forms.push(LayoutGridNameForms{});
    if (templates->forms.data[atom].start == kPrLayoutGridNameNone) {
        // The forms are interned as names of their own, but are given no forms of their own unless they are interned as
        // themselves.
        Array<char> text;
        text.resize(uint32_t(name.size) + 6);
        memCopy(text.data, name.data, name.size);
        memCopy(text.data + name.size, "-start", 6);
        Atom start = atomTableAdd(&templates->names, PrStringView{text.data, name.size + 6});
        memCopy(text.data + name.size, "-end", 4);
        Atom end = atomTableAdd(&templates->names, PrStringView{text.data, name.size + 4});
        text.reset();
        while (templates->forms.count <= (start > end ? start : end))
            templates->forms.push(LayoutGridNameForms{});
        templates->forms.data[atom] = LayoutGridNameForms{start + 1, end + 1};
    }
    return atom + 1;
}

void layoutGridTemplatesCount(LayoutGridTemplates* templates, const PrLayoutGridStyle& grid, uint32_t delta) {
    PR_ASSERT(isLine(grid.columnStart) && isLine(grid.columnEnd) && isLine(grid.rowStart) && isLine(grid.rowEnd));
    PR_ASSERT((grid.autoFlow & ~(PrLayoutGridAutoFlowFlag_Column | PrLayoutGridAutoFlowFlag_Dense)) == 0);
    count(templates->tracks, grid.templateColumns, delta);
    count(templates->tracks, grid.templateRows, delta);
    count(templates->tracks, grid.autoColumns, delta);
    count(templates->tracks, grid.autoRows, delta);
    count(templates->areas, grid.templateAreas, delta);
}

void layoutGridTemplatesCollect(LayoutGridTemplates* templates, LayoutCompounds* compounds) {
    LayoutGridTable<LayoutGridTracksEntry>& tracks = templates->tracks;
    for (uint32_t k = 0; k < tracks.candidates.count; ++k) {
        uint32_t i = tracks.candidates.data[k];
        LayoutGridTracksEntry& entry = tracks.entries.data[i];
        if (entry.tracks == nullptr || entry.references != 0)
            continue;
        tracks.index.erase(entry.hash, i);
        countTracks(compounds, entry.tracks, entry.trackCount, 0xffffffffu);
        freeTracks(entry);
        tracks.free.push(i);
    }
    tracks.candidates.clear();
    LayoutGridTable<LayoutGridAreasEntry>& areas = templates->areas;
    for (uint32_t k = 0; k < areas.candidates.count; ++k) {
        uint32_t i = areas.candidates.data[k];
        LayoutGridAreasEntry& entry = areas.entries.data[i];
        if (!entry.live || entry.references != 0)
            continue;
        areas.index.erase(entry.hash, i);
        freeAreas(entry);
        areas.free.push(i);
    }
    areas.candidates.clear();
}

bool layoutGridTemplatesVerify(const LayoutGridTemplates* templates) {
    const LayoutGridTable<LayoutGridTracksEntry>& tracks = templates->tracks;
    const LayoutGridTable<LayoutGridAreasEntry>& areas = templates->areas;
    return layoutCandidatesCover(tracks.candidates, tracks.entries.count, [&](uint32_t i) {
        const LayoutGridTracksEntry& entry = tracks.entries.data[i];
        return entry.tracks != nullptr && entry.references == 0;
    }) && layoutCandidatesCover(areas.candidates, areas.entries.count, [&](uint32_t i) {
        const LayoutGridAreasEntry& entry = areas.entries.data[i];
        return entry.live && entry.references == 0;
    });
}

void layoutGridTemplatesReset(LayoutGridTemplates* templates) {
    for (uint32_t i = 0; i < templates->tracks.entries.count; ++i)
        freeTracks(templates->tracks.entries.data[i]);
    for (uint32_t i = 0; i < templates->areas.entries.count; ++i)
        freeAreas(templates->areas.entries.data[i]);
    LayoutGridTable<LayoutGridTracksEntry>& tracks = templates->tracks;
    LayoutGridTable<LayoutGridAreasEntry>& areas = templates->areas;
    tracks.entries.reset();
    tracks.free.reset();
    tracks.index.reset();
    tracks.candidates.reset();
    areas.entries.reset();
    areas.free.reset();
    areas.index.reset();
    areas.candidates.reset();
    atomTableReset(&templates->names);
    templates->forms.reset();
}

} // namespace pr_impl
