// Grid templates: the track lists, template areas and names that a tree interns for its grid groups.
//
// css-grid-1 - https://drafts.csswg.org/css-grid-1/
//
// A track list and a template's areas are interned as one entry each, as background layers are (layout_background.h),
// so a grid group that names them compares as bits, equal lists are one entry, and a layout frees an entry that no
// group names. Names are interned for the tree's life, together with the `-start` and `-end` forms that a named area
// gives its lines (css-grid-1/#implicit-named-lines) and that a placement by a name alone reads.

#pragma once

#include "pr/array.h"
#include "pr/assert.h"
#include "pr/atom_table.h"
#include "pr/hash_index.h"
#include "pr/layout_style.h"
#include "pr/string_view.h"

#include "layout/src/layout_value.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// One interned track list (PrLayoutGridTrackList), whose arrays the entry owns.
struct LayoutGridTracksEntry {
    PrLayoutGridTrack* PR_NULLABLE tracks;      // null while the entry is free, and for the empty list at index 0
    PrLayoutGridLineName* PR_NULLABLE names;
    PrLayoutGridLineName* PR_NULLABLE repeatNames;
    uint32_t trackCount;
    uint32_t nameCount;
    uint32_t repeatNameCount;
    uint32_t repeatStart;
    uint32_t repeatCount;
    PrLayoutGridRepeat repeat;
    Array<uint8_t> key;                         // the bytes that equal lists share
    uint32_t hash;
    uint32_t references;                        // number of grid groups that name it
};

// One interned template of areas (PrLayoutGridAreas), whose array the entry owns.
struct LayoutGridAreasEntry {
    PrLayoutGridArea* PR_NULLABLE areas;        // null while the entry is free, and for no areas at index 0
    uint32_t areaCount;
    uint32_t rowCount;
    uint32_t columnCount;
    Array<uint8_t> key;
    uint32_t hash;
    uint32_t references;
    bool live;                                  // true unless the entry is free
};

// The names that a name gives the lines at the edges of the area that it names (css-grid-1/#implicit-named-lines).
struct LayoutGridNameForms {
    PrLayoutGridName start;         // the name with `-start` after it
    PrLayoutGridName end;           // the name with `-end` after it
};

// A table of interned entries. Index 0 is the empty entry, which is never freed. An index names its entry until the
// entry is freed.
template <typename Entry>
struct LayoutGridTable {
    Array<Entry> entries;
    Array<uint32_t> free;           // freed indices, which interning reuses
    HashIndex index;
    Array<uint32_t> candidates;     // same as LayoutTable::candidates
};

// The track lists, template areas and names of a tree's styles.
struct LayoutGridTemplates {
    LayoutGridTable<LayoutGridTracksEntry> tracks;
    LayoutGridTable<LayoutGridAreasEntry> areas;

    // A name is its atom plus one, so that zero is no name. The forms of a name that was interned as itself are kept by
    // atom, and a name that is only another name's form has none.
    AtomTable names;
    Array<LayoutGridNameForms> forms;
};

// Prepares the tables with the empty entries at index 0.
void layoutGridTemplatesInit(LayoutGridTemplates* templates);

// Interns a track list as prLayoutGridTracksIntern defines. An entry that it adds holds references to the compounds
// that its breadths and limits name.
PrLayoutGridTracksIndex layoutGridTemplatesInternTracks(LayoutGridTemplates* templates, LayoutCompounds* compounds,
                                                        const PrLayoutGridTrackList& list);

// Interns a template's areas as prLayoutGridAreasIntern defines.
PrLayoutGridAreasIndex layoutGridTemplatesInternAreas(LayoutGridTemplates* templates, const PrLayoutGridAreas& areas);

// Interns a name as prLayoutGridNameIntern defines, together with its forms.
PrLayoutGridName layoutGridTemplatesInternName(LayoutGridTemplates* templates, PrStringView name);

// Returns the track list at `index`.
inline const LayoutGridTracksEntry& layoutGridTracksAt(const LayoutGridTemplates* templates,
                                                       PrLayoutGridTracksIndex index) {
    PR_ASSERT(index < templates->tracks.entries.count);
    return templates->tracks.entries.data[index];
}

// Returns the template areas at `index`.
inline const LayoutGridAreasEntry& layoutGridAreasAt(const LayoutGridTemplates* templates,
                                                     PrLayoutGridAreasIndex index) {
    PR_ASSERT(index < templates->areas.entries.count);
    return templates->areas.entries.data[index];
}

// Returns the forms of `name`, which are none if `name` is none or was only interned as another name's form.
inline LayoutGridNameForms layoutGridNameForms(const LayoutGridTemplates* templates, PrLayoutGridName name) {
    if (name == kPrLayoutGridNameNone || name > templates->forms.count)
        return LayoutGridNameForms{};
    return templates->forms.data[name - 1];
}

// Adds `delta` to the references of the track lists and areas that a grid group names, and checks in debug builds that
// its values are in their ranges.
void layoutGridTemplatesCount(LayoutGridTemplates* templates, const PrLayoutGridStyle& grid, uint32_t delta);

// Frees the entries that no group names, of those listed as candidates, and releases what they hold.
void layoutGridTemplatesCollect(LayoutGridTemplates* templates, LayoutCompounds* compounds);

// Returns true if every live entry that no group names is listed as a candidate.
bool layoutGridTemplatesVerify(const LayoutGridTemplates* templates);

// Frees every entry and name, and the tables' memory.
void layoutGridTemplatesReset(LayoutGridTemplates* templates);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
