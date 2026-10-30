// Grid layout: a grid container's items, placed in its grid and sized with its tracks.

#include "layout/src/layout_grid.h"

#include "pr/alloc.h"
#include "pr/float.h"
#include "pr/search.h"

#include "layout/src/layout_block.h"
#include "layout/src/layout_box.h"
#include "layout/src/layout_intrinsic.h"
#include "layout/src/layout_position.h"
#include "layout/src/layout_sizing.h"

namespace pr_impl {

// Where an item sits in its grid area along one axis once its alignment is resolved (css-align-3/#self-alignment):
// flush with the area's start or end edge, centered, stretched over it, or placed with its first or last baseline on
// the shared baseline of its baseline-sharing group.
enum {
    LayoutGridPlace_Start,
    LayoutGridPlace_End,
    LayoutGridPlace_Center,
    LayoutGridPlace_Stretch,
    LayoutGridPlace_Baseline,
    LayoutGridPlace_LastBaseline,
};
typedef uint8_t LayoutGridPlace;

// The contributions of an item to the sizes of the tracks it spans, in one axis (css-grid-1/#algo-content).
enum {
    LayoutGridContribution_Minimum = 1u << 0,
    LayoutGridContribution_MinContent = 1u << 1,
    LayoutGridContribution_MaxContent = 1u << 2,
};
typedef uint8_t LayoutGridContributions;

// An item of a grid container being laid out: what its style gives, where it is placed, and what the algorithm finds.
// Sizes are outer sizes, margin box included, in units that may be fractional.
struct LayoutGridItem {
    LayoutNodeIndex node;           // for an anonymous item, the first node of its run, which keeps its caches
    const PrLayoutBoxStyle* box;
    const PrLayoutFlexStyle* flex;
    const PrLayoutGridStyle* grid;
    int32_t order;
    bool orthogonal;                // an orthogonal flow root, whose inline axis is the container's block axis
    bool replaced;
    bool anonymous;                 // an anonymous item, which wraps a run of text and has no record of its own

    // The lines that bound its grid area in each axis. While it is placed, they are counted from 0 at the start of the
    // explicit grid, and an axis whose position is automatic holds the span in `end`. Once it is placed, they are
    // indices of the implicit grid's lines, from 0 at its start.
    int32_t start[PrLayoutGridAxis_Count];
    int32_t end[PrLayoutGridAxis_Count];
    bool definite[PrLayoutGridAxis_Count];

    LayoutGridPlace place[PrLayoutGridAxis_Count];
    bool safe[PrLayoutGridAxis_Count];    // true if an alignment falls back to start where the item overflows its area
    bool autoMargins[PrLayoutGridAxis_Count];
    bool spansFlexible[PrLayoutGridAxis_Count];

    // Its contributions in the axis being sized, which are found on first use in each sizing of the axis, and the shim
    // that baseline alignment adds to them on each side (css-grid-1/#algo-baseline-shims).
    LayoutGridContributions known;
    double minimum;
    double minContent;
    double maxContent;
    double shimStart;
    double shimEnd;

    // Its baselines, from the block-start edge of its border box, at the inline size of its last measurement or layout.
    LayoutBaseline first;
    LayoutBaseline last;

    // Its used border-box sizes and margins in the container's axes, by PrLayoutGridAxis and LayoutSide, and the offset
    // of its border box in the content box.
    PrLayoutUnit size[PrLayoutGridAxis_Count];
    PrLayoutUnit margin[LayoutSide_Count];
    PrLayoutUnit offset[PrLayoutGridAxis_Count];
};

// What a sizing function of a track is (css-grid-1/#algo-grid-sizing).
enum {
    LayoutGridSizing_Fixed,         // a length, or a percentage that resolves
    LayoutGridSizing_Auto,
    LayoutGridSizing_MinContent,
    LayoutGridSizing_MaxContent,
    LayoutGridSizing_FitContent,    // a maximum only: max-content, capped by the track's limit
    LayoutGridSizing_Flex,          // a maximum only: a flex factor
};
typedef uint8_t LayoutGridSizing;

// A track of a grid container being laid out, in one axis: its sizing functions, and what the track sizing algorithm
// finds (css-grid-1/#algo-track-sizing). Sizes are in units that may be fractional.
struct GridTrack {
    LayoutGridSizing min;
    LayoutGridSizing max;
    bool collapsed;                 // an empty track of an auto-fit repetition (css-grid-1/#collapsed-grid-track)
    bool infinitelyGrowable;        // its growth limit became finite in the step that accommodates intrinsic maximums
    bool frozen;                    // full, in the current share of space
    bool touched;                   // spanned by an item of the current distribution of space
    double minFixed;                // the size of a fixed minimum
    double maxFixed;                // the size of a fixed maximum, the limit of fit-content, or the flex factor
    double base;                    // its base size
    double limit;                   // its growth limit, which may be infinite
    double planned;                 // the increase that the items of the current step plan for it
    double incurred;                // the increase that the current item asks of it
    double offset;                  // the offset of its start edge in the content box, once aligned
};

// A name of a line of an axis of a grid: the line, counted from 0 at the start of the explicit grid, and its name.
struct GridLineName {
    PrLayoutGridName name;
    int32_t line;
};

// What a grid container's lines are, kept from its layout until the boxes out of flow that it holds are placed and its
// fragment is made (LayoutGridLines). The positions of its lines are in the scratch positions from `positions`, the
// sizes of its tracks in the scratch sizes from `sizes`, and the names of its lines from `names`, columns first.
struct LayoutGridKept {
    LayoutMode mode;
    LayoutLogicalOffset origin;     // the start corner of its content box in its border box
    uint32_t positions;
    uint32_t sizes;
    uint32_t names;
    uint32_t repetitions[PrLayoutGridAxis_Count];
    uint32_t nameCount[PrLayoutGridAxis_Count];
    uint32_t trackCount[PrLayoutGridAxis_Count];
    int32_t explicitStart[PrLayoutGridAxis_Count];    // the index of the explicit grid's first line among its lines
    int32_t explicitCount[PrLayoutGridAxis_Count];    // the number of tracks of the explicit grid
};

// Scratch memory that grid layout keeps between layouts: the items, tracks and names of the containers being laid out,
// innermost last, the bits of auto-placement, and the lines kept for the boxes out of flow.
struct LayoutGridScratch {
    Array<LayoutGridItem> items;
    Array<GridTrack> tracks;
    Array<GridLineName> names;
    Array<uint64_t> occupied;
    Array<uint32_t> order;
    Array<double> sizes;
    Array<LayoutGridKept> kept;
    Array<PrLayoutUnit> keptPositions;
    Array<PrLayoutUnit> keptSizes;
    Array<GridLineName> keptNames;
};

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Containers

// Returns the unit nearest to `raw` units.
PrLayoutUnit unitOf(double raw) {
    return prLayoutUnitClamped(int64_t(roundF64(raw)));
}

// Returns the units of `unit`, as the algorithm counts them.
double rawOf(PrLayoutUnit unit) {
    return double(unit.raw);
}

// The sizing constraint that an axis is sized under (css-grid-1/#algo-terms): a definite available grid space, or a
// min-content or max-content constraint, under which the space is indefinite.
enum {
    LayoutGridSpace_Definite,
    LayoutGridSpace_MinContent,
    LayoutGridSpace_MaxContent,
};
typedef uint8_t LayoutGridSpace;

// One axis of the grid of a container being laid out.
struct GridAxis {
    uint32_t trackMark;             // the index of its first track in the scratch tracks
    uint32_t trackCount;            // the tracks of its implicit grid
    int32_t explicitStart;          // the index of the explicit grid's first line among its lines
    int32_t explicitCount;          // the tracks of the explicit grid
    uint32_t nameMark;              // the index of its first line name in the scratch names
    uint32_t nameCount;
    PrLayoutUnit gap;               // the gap between two tracks (css-align-3/#gaps)
    bool gapReadsSize;              // true if the gap is a percentage of the content box's size in this axis
    PrLayoutAlignment content;      // justify-content or align-content
    bool collapses;                 // true if a track of an auto-fit repetition may be collapsed

    // The template that sizes the explicit grid's first tracks, its automatic repetition expanded, and the sizes of the
    // tracks that it leaves unsized, which repeat.
    const LayoutGridTracksEntry* tracks;
    uint32_t templateCount;
    uint32_t repetitions;           // of its automatic repetition, or 0 if it has none
    const LayoutGridTracksEntry* autos;
};

// What one layout of a grid container reads: the container, its axes, and its content box.
struct Grid {
    LayoutState& state;
    LayoutNodeIndex node;
    const PrLayoutGridStyle& style;
    const PrLayoutFlexStyle& flex;
    const LayoutSpace& content;                   // the space its items are laid out in, apart from their requests and
                                                  // areas
    PrLayoutUnit size[PrLayoutGridAxis_Count];    // its content box, or negative where its content decides it
    PrLayoutUnit minSize[PrLayoutGridAxis_Count]; // the limits of a size that its content decides; zero if none
    PrLayoutUnit maxSize[PrLayoutGridAxis_Count]; // negative if none
    GridAxis axes[PrLayoutGridAxis_Count];
    bool placed[PrLayoutGridAxis_Count]; // true once the tracks of the axis are sized and placed
    uint32_t mark;                       // the index of its first item in the scratch items
    uint32_t count;                      // the number of its items

    LayoutGridScratch& scratch() const { return *state.scratch->grid; }
    LayoutGridItem& item(uint32_t i) const { return scratch().items.data[mark + i]; }
    GridTrack& track(PrLayoutGridAxis axis, uint32_t t) const {
        return scratch().tracks.data[axes[axis].trackMark + t];
    }
    LayoutMode mode() const { return content.mode; }
};

// ---------------------------------------------------------------------------------------------------------------------
// The explicit grid

// Returns the size that a track of `track` is treated as when the repetitions of an automatic repetition are counted
// (css-grid-1/#auto-repeat): its maximum if that is fixed, floored by its minimum if that is fixed too, or else its
// minimum if that is fixed. Returns a negative size if neither is fixed. Percentages resolve against `base`.
double repeatedSize(const LayoutStyles* styles, const PrLayoutGridTrack& track, PrLayoutUnit base) {
    auto fixed = [&](PrLayoutTrackBreadth breadth) {
        PrLayoutLength length{breadth.bits};
        PrLayoutWordKind kind = prLayoutWordKind(breadth.bits);
        if (kind == PrLayoutWordKind_Number || !layoutLengthIsFixed(length) || !layoutResolves(length, base))
            return -1.0;
        return maxF64(0, rawOf(layoutResolve(styles, length, base)));
    };
    double min = fixed(track.min);
    double max = fixed(track.max);
    if (max >= 0)
        return min >= 0 ? maxF64(max, min) : max;
    return min;
}

// Returns the number of repetitions of the automatic repetition of `list` in an axis whose content box is `size`, or
// whose limits are `minSize` and `maxSize` if its size is negative, with gaps of `gap` (css-grid-1/#auto-repeat). The
// count is the most that do not overflow a definite size or maximum, the fewest that fill a definite minimum, or else
// one. A track that is not repeated counts at its repeated size, or as nothing if it has none. The count keeps the
// tracks within kPrLayoutGridMaxLine.
uint32_t repetitionsOf(const LayoutStyles* styles, const LayoutGridTracksEntry& list, PrLayoutUnit size,
        PrLayoutUnit minSize, PrLayoutUnit maxSize, PrLayoutUnit gap) {
    PrLayoutUnit space = layoutIsDefinite(size) ? size : maxSize;
    bool fills = !layoutIsDefinite(space);
    if (fills)
        space = minSize;
    if (!layoutIsDefinite(space) || (fills && space == kLayoutUnitZero))
        return 1;
    PrLayoutUnit base = space;
    double others = 0;
    double repeated = 0;
    for (uint32_t i = 0; i < list.trackCount; ++i) {
        double one = repeatedSize(styles, list.tracks[i], base);
        bool inRepeat = i >= list.repeatStart && i < list.repeatStart + list.repeatCount;
        if (!inRepeat) {
            others += maxF64(0, one);
            continue;
        }
        // A track that has no definite size makes one repetition, and a size is floored at 1px so that the count stays
        // finite.
        if (one < 0)
            return 1;
        repeated += maxF64(one, double(kPrLayoutUnitsPerPx));
    }
    double g = rawOf(gap);
    uint32_t othersCount = list.trackCount - list.repeatCount;
    auto total = [&](double n) {
        double tracks = double(othersCount) + n * double(list.repeatCount);
        return others + n * repeated + g * maxF64(0, tracks - 1);
    };
    uint32_t most = 1;
    if (othersCount < uint32_t(kPrLayoutGridMaxLine))
        most = (uint32_t(kPrLayoutGridMaxLine) - othersCount) / list.repeatCount;
    if (most < 1)
        most = 1;
    double avail = rawOf(space);
    // The size of n repetitions grows by `repeated` plus the gaps of one repetition with each n.
    double step = repeated + g * double(list.repeatCount);
    double first = total(1);
    if (!fills) {
        if (first > avail)
            return 1;
        double n = 1 + floorF64((avail - first) / step);
        return uint32_t(minF64(n, double(most)));
    }
    if (first >= avail)
        return 1;
    double n = 1 + ceilF64((avail - first) / step);
    return uint32_t(minF64(n, double(most)));
}

// Returns the track of the template of `axis` at `index` of its expanded tracks, its automatic repetition repeated.
const PrLayoutGridTrack& templateTrack(const GridAxis& axis, uint32_t index) {
    const LayoutGridTracksEntry& list = *axis.tracks;
    if (axis.repetitions == 0 || index < list.repeatStart)
        return list.tracks[index];
    uint32_t repeated = list.repeatCount * axis.repetitions;
    if (index < list.repeatStart + repeated)
        return list.tracks[list.repeatStart + (index - list.repeatStart) % list.repeatCount];
    return list.tracks[index - repeated + list.repeatCount];
}

// The implicit track size, an auto track, of an empty list of implicit track sizes.
constexpr PrLayoutGridTrack kAutoTrack{
    .min = {prLayoutWordKeyword(PrLayoutKeyword_Auto)},
    .max = {prLayoutWordKeyword(PrLayoutKeyword_Auto)},
    .limit = {prLayoutWordPx(0)},
};

// Returns the sizing functions of the track `index` of `axis`, counted from 0 at the start of the explicit grid
// (css-grid-1/#auto-tracks). A track of the template takes the template's. Any other track takes the implicit track
// sizes, the first after the template's tracks taking the first, and the last before the explicit grid taking the last.
const PrLayoutGridTrack& trackSizes(const GridAxis& axis, int32_t index) {
    if (index >= 0 && uint32_t(index) < axis.templateCount)
        return templateTrack(axis, uint32_t(index));
    const LayoutGridTracksEntry& autos = *axis.autos;
    if (autos.trackCount == 0)
        return kAutoTrack;
    int32_t n = int32_t(autos.trackCount);
    int32_t from = index >= 0 ? index - int32_t(axis.templateCount) : index;
    int32_t at = from % n;
    if (at < 0)
        at += n;
    return autos.tracks[at];
}

// Adds the names of the lines of the explicit grid of `axis` to the scratch names, sorted by name and then by line: the
// names of its template, with its automatic repetition repeated, and the names that the template's areas give the lines
// at their edges (css-grid-1/#implicit-named-lines). `rows` says which edges of an area are the axis's.
void collectNames(Grid& g, GridAxis& axis, const LayoutGridAreasEntry& areas, bool rows) {
    Array<GridLineName>& names = g.scratch().names;
    axis.nameMark = names.count;
    const LayoutGridTracksEntry& list = *axis.tracks;
    uint32_t shift = axis.repetitions != 0 ? list.repeatCount * (axis.repetitions - 1) : 0;
    for (uint32_t i = 0; i < list.nameCount; ++i) {
        const PrLayoutGridLineName& n = list.names[i];
        bool after = axis.repetitions != 0 && n.line > list.repeatStart;
        names.push(GridLineName{n.name, int32_t(n.line + (after ? shift : 0))});
    }
    for (uint32_t r = 0; r < axis.repetitions; ++r) {
        for (uint32_t i = 0; i < list.repeatNameCount; ++i) {
            const PrLayoutGridLineName& n = list.repeatNames[i];
            names.push(GridLineName{n.name, int32_t(list.repeatStart + r * list.repeatCount + n.line)});
        }
    }
    const LayoutGridTemplates* templates = &g.state.styles->grids;
    for (uint32_t i = 0; i < areas.areaCount; ++i) {
        const PrLayoutGridArea& a = areas.areas[i];
        LayoutGridNameForms forms = layoutGridNameForms(templates, a.name);
        if (forms.start == kPrLayoutGridNameNone)
            continue;
        names.push(GridLineName{forms.start, int32_t(rows ? a.rowStart : a.columnStart)});
        names.push(GridLineName{forms.end, int32_t(rows ? a.rowEnd : a.columnEnd)});
    }
    axis.nameCount = names.count - axis.nameMark;
    GridLineName* first = names.data + axis.nameMark;
    // An insertion sort, since most templates name few lines, in order.
    for (uint32_t i = 1; i < axis.nameCount; ++i) {
        for (uint32_t k = i; k > 0; --k) {
            GridLineName& a = first[k - 1];
            GridLineName& b = first[k];
            if (a.name < b.name || (a.name == b.name && a.line <= b.line))
                break;
            GridLineName moved = b;
            b = a;
            a = moved;
        }
    }
}

// The lines of an axis that have one name, ascending, among the names of the axis.
struct NamedLines {
    const GridLineName* PR_NULLABLE first;
    uint32_t count;
};

// Returns the lines of `names`, `count` of them sorted by name, that have `name`.
NamedLines namedLines(const GridLineName* PR_NULLABLE names, uint32_t count, PrLayoutGridName name) {
    uint32_t lo = partitionPoint(0, count, [&](uint32_t i) { return names[i].name < name; });
    uint32_t end = lo;
    while (end < count && names[end].name == name)
        ++end;
    return NamedLines{names + lo, end - lo};
}

// ---------------------------------------------------------------------------------------------------------------------
// Line placement

// The lines of an axis that placement resolves against: the names of the explicit grid's lines, and its number of
// tracks.
struct AxisLines {
    const GridLineName* PR_NULLABLE names;
    uint32_t nameCount;
    int32_t explicitCount;
};

// Returns the `n`th line named `name`, counting from the start of the explicit grid if `n` is positive and from its end
// if negative (css-grid-1/#grid-placement-int). If the explicit grid has too few such lines, every implicit line counts
// as one, past the explicit grid's end or before its start.
int32_t nthNamed(const AxisLines& lines, PrLayoutGridName name, int32_t n) {
    NamedLines named = namedLines(lines.names, lines.nameCount, name);
    int32_t k = int32_t(named.count);
    if (n > 0)
        return n <= k ? named.first[n - 1].line : lines.explicitCount + (n - k);
    int32_t back = -n;
    return back <= k ? named.first[k - back].line : -(back - k);
}

// Returns the line `n` lines named `name` from `from`, forward if `forward` is true and backward otherwise, or `n`
// lines from it if `name` is none (css-grid-1/#grid-placement-span-int). If the explicit grid has too few such lines
// past `from`, every implicit line on the side of the explicit grid that the search goes toward counts as one.
int32_t spanFrom(const AxisLines& lines, int32_t from, bool forward, PrLayoutGridName name, int32_t n) {
    if (name == kPrLayoutGridNameNone)
        return forward ? from + n : from - n;
    NamedLines named = namedLines(lines.names, lines.nameCount, name);
    int32_t found = 0;
    if (forward) {
        for (uint32_t i = 0; i < named.count; ++i) {
            if (named.first[i].line > from && ++found == n)
                return named.first[i].line;
        }
        int32_t past = from > lines.explicitCount ? from : lines.explicitCount;
        return past + (n - found);
    }
    for (uint32_t i = named.count; i > 0; --i) {
        if (named.first[i - 1].line < from && ++found == n)
            return named.first[i - 1].line;
    }
    int32_t before = from < 0 ? from : 0;
    return before - (n - found);
}

// Returns the line that a grid-placement property's line `line` names, as a start edge if `start` is true and an end
// edge otherwise (css-grid-1/#line-placement). A name alone names first the edge of an area of that name, through the
// lines named with its `-start` or `-end` form, and otherwise the first line of that name.
int32_t lineOf(const LayoutGridTemplates* templates, const AxisLines& lines, const PrLayoutGridLine& line, bool start) {
    if (line.name == kPrLayoutGridNameNone)
        return line.integer > 0 ? line.integer - 1 : lines.explicitCount + 1 + line.integer;
    if (line.integer != 0)
        return nthNamed(lines, line.name, line.integer);
    LayoutGridNameForms forms = layoutGridNameForms(templates, line.name);
    PrLayoutGridName edge = start ? forms.start : forms.end;
    if (edge != kPrLayoutGridNameNone) {
        NamedLines named = namedLines(lines.names, lines.nameCount, edge);
        if (named.count != 0)
            return named.first[0].line;
    }
    return nthNamed(lines, line.name, 1);
}

// A placement in one axis (css-grid-1/#placement): two definite lines, or a span to place automatically.
struct AxisPlacement {
    bool definite;
    int32_t start;
    int32_t end;                    // if not definite, the span
};

// Clamps a line to the limits of the implicit grid (css-grid-1/#overlarge-grids).
int32_t clampLine(int32_t line) {
    return line < -kPrLayoutGridMaxLine ? -kPrLayoutGridMaxLine : line > kPrLayoutGridMaxLine ? kPrLayoutGridMaxLine : line;
}

// Resolves the placement that the grid-placement properties `startLine` and `endLine` give in an axis
// (css-grid-1/#line-placement), handling their conflicts (css-grid-1/#grid-placement-errors). Two lines in the wrong
// order swap, and equal lines make a span of one. Of two spans, the start's counts, and a span of a name alone counts
// as a span of one. An area past the implicit grid's limits is clamped into them (css-grid-1/#overlarge-grids).
AxisPlacement placementOf(const LayoutGridTemplates* templates, const AxisLines& lines,
        const PrLayoutGridLine& startLine, const PrLayoutGridLine& endLine) {
    bool startIsLine = startLine.kind == PrLayoutGridLineKind_Line;
    bool endIsLine = endLine.kind == PrLayoutGridLineKind_Line;
    AxisPlacement p{};
    if (startIsLine && endIsLine) {
        p.start = lineOf(templates, lines, startLine, true);
        p.end = lineOf(templates, lines, endLine, false);
        if (p.start > p.end) {
            int32_t swap = p.start;
            p.start = p.end;
            p.end = swap;
        }
        if (p.start == p.end)
            p.end = p.start + 1;
        p.definite = true;
    } else if (startIsLine) {
        p.start = lineOf(templates, lines, startLine, true);
        if (endLine.kind == PrLayoutGridLineKind_Span)
            p.end = spanFrom(lines, p.start, true, endLine.name, endLine.integer);
        else
            p.end = p.start + 1;
        p.definite = true;
    } else if (endIsLine) {
        p.end = lineOf(templates, lines, endLine, false);
        if (startLine.kind == PrLayoutGridLineKind_Span)
            p.start = spanFrom(lines, p.end, false, startLine.name, startLine.integer);
        else
            p.start = p.end - 1;
        p.definite = true;
    } else {
        const PrLayoutGridLine& span = startLine.kind == PrLayoutGridLineKind_Span ? startLine : endLine;
        p.end = span.kind == PrLayoutGridLineKind_Span && span.name == kPrLayoutGridNameNone ? span.integer : 1;
        p.end = p.end < 2 * kPrLayoutGridMaxLine ? p.end : 2 * kPrLayoutGridMaxLine;
        return p;
    }
    // A span that leaves the limits is cut at them, and an area wholly outside them takes the last track inside.
    if (p.end <= -kPrLayoutGridMaxLine) {
        p.start = -kPrLayoutGridMaxLine;
        p.end = p.start + 1;
    } else if (p.start >= kPrLayoutGridMaxLine) {
        p.end = kPrLayoutGridMaxLine;
        p.start = p.end - 1;
    } else {
        p.start = clampLine(p.start);
        p.end = clampLine(p.end);
    }
    return p;
}

// ---------------------------------------------------------------------------------------------------------------------
// Auto-placement

// The cells of a grid that its items occupy, which auto-placement reads (css-grid-1/#auto-placement-algo): a row of
// bits for each track of the axis that the flow advances through, with a bit for each track of the other axis. Rows are
// added as the flow reaches them.
struct Occupancy {
    Array<uint64_t>& bits;
    uint32_t mark;                  // the index of the first row's first word in `bits`
    uint32_t words;                 // the words of a row
    int32_t majorBase;              // the line of the first row
    int32_t minorBase;              // the line of each row's first bit
    uint32_t rows;
};

// Adds the rows that reach the line `majorEnd`.
void growRows(Occupancy& o, int32_t majorEnd) {
    uint32_t needed = uint32_t(majorEnd - o.majorBase);
    if (needed <= o.rows)
        return;
    uint32_t at = o.bits.count;
    o.bits.resize(o.mark + needed * o.words);
    memZero(o.bits.data + at, (o.bits.count - at) * sizeof(uint64_t));
    o.rows = needed;
}

// Returns true if any cell of the area from `majorStart` to `majorEnd` and from `minorStart` to `minorEnd` is occupied.
bool occupied(Occupancy& o, int32_t majorStart, int32_t majorEnd, int32_t minorStart, int32_t minorEnd) {
    uint32_t lo = uint32_t(minorStart - o.minorBase);
    uint32_t hi = uint32_t(minorEnd - o.minorBase);
    uint32_t rowEnd = uint32_t(majorEnd - o.majorBase) < o.rows ? uint32_t(majorEnd - o.majorBase) : o.rows;
    for (uint32_t r = uint32_t(majorStart - o.majorBase); r < rowEnd; ++r) {
        const uint64_t* row = o.bits.data + o.mark + r * o.words;
        for (uint32_t bit = lo; bit < hi;) {
            uint32_t word = bit / 64;
            uint32_t from = bit % 64;
            uint32_t to = hi - word * 64 < 64 ? hi - word * 64 : 64;
            uint64_t mask = (to == 64 ? ~uint64_t(0) : (uint64_t(1) << to) - 1) & ~((uint64_t(1) << from) - 1);
            if ((row[word] & mask) != 0)
                return true;
            bit = (word + 1) * 64;
        }
    }
    return false;
}

// Marks the cells of an area as occupied.
void occupy(Occupancy& o, int32_t majorStart, int32_t majorEnd, int32_t minorStart, int32_t minorEnd) {
    growRows(o, majorEnd);
    for (uint32_t r = uint32_t(majorStart - o.majorBase); r < uint32_t(majorEnd - o.majorBase); ++r) {
        uint64_t* row = o.bits.data + o.mark + r * o.words;
        for (int32_t m = minorStart; m < minorEnd; ++m) {
            uint32_t bit = uint32_t(m - o.minorBase);
            row[bit / 64] |= uint64_t(1) << (bit % 64);
        }
    }
}

// Returns the lines that placement resolves against in `axis`.
AxisLines linesOf(const Grid& g, PrLayoutGridAxis axis) {
    const GridAxis& a = g.axes[axis];
    return AxisLines{g.scratch().names.data + a.nameMark, a.nameCount, a.explicitCount};
}

// Places the items of the grid (css-grid-1/#auto-placement-algo). Resolves each item's definite lines, places the items
// locked to a track of the flow's axis, finds the tracks of the other axis, and then places the remaining items with
// the auto-placement cursor, in order-modified document order. Lines are counted from the start of the explicit grid
// until the implicit grid is known, and then from its start.
void placeItems(Grid& g) {
    const LayoutGridTemplates* templates = &g.state.styles->grids;
    bool column = (g.style.autoFlow & PrLayoutGridAutoFlowFlag_Column) != 0;
    bool dense = (g.style.autoFlow & PrLayoutGridAutoFlowFlag_Dense) != 0;
    PrLayoutGridAxis major = column ? PrLayoutGridAxis_Columns : PrLayoutGridAxis_Rows;
    PrLayoutGridAxis minor = column ? PrLayoutGridAxis_Rows : PrLayoutGridAxis_Columns;
    AxisLines lines[PrLayoutGridAxis_Count] = {linesOf(g, PrLayoutGridAxis_Columns), linesOf(g, PrLayoutGridAxis_Rows)};
    const PrLayoutGridStyle initial = prLayoutGridStyleDefault();

    // The definite lines of each item, and the extents that bound the cells that auto-placement can reach.
    int32_t majorMin = 0;
    int32_t minorMin = 0;
    int32_t minorMax = g.axes[minor].explicitCount;
    int32_t lockedSpans = 0;
    int32_t widestAuto = 0;
    for (uint32_t i = 0; i < g.count; ++i) {
        LayoutGridItem& it = g.item(i);
        const PrLayoutGridStyle& s = it.grid != nullptr ? *it.grid : initial;
        AxisPlacement columns = placementOf(templates, lines[PrLayoutGridAxis_Columns], s.columnStart, s.columnEnd);
        AxisPlacement rows = placementOf(templates, lines[PrLayoutGridAxis_Rows], s.rowStart, s.rowEnd);
        const AxisPlacement byAxis[PrLayoutGridAxis_Count] = {columns, rows};
        for (PrLayoutGridAxis axis = 0; axis < PrLayoutGridAxis_Count; ++axis) {
            it.definite[axis] = byAxis[axis].definite;
            it.start[axis] = byAxis[axis].start;
            it.end[axis] = byAxis[axis].end;
        }
        if (it.definite[major])
            majorMin = it.start[major] < majorMin ? it.start[major] : majorMin;
        if (it.definite[minor]) {
            minorMin = it.start[minor] < minorMin ? it.start[minor] : minorMin;
            minorMax = it.end[minor] > minorMax ? it.end[minor] : minorMax;
        } else {
            widestAuto = it.end[minor] > widestAuto ? it.end[minor] : widestAuto;
            if (it.definite[major])
                lockedSpans += it.end[minor];
        }
    }

    // The bits reach every line that the items locked to a track can be placed at, past the definite extents.
    LayoutGridScratch& s = g.scratch();
    int32_t width = minorMax - minorMin + lockedSpans + widestAuto;
    Occupancy o{
        .bits = s.occupied,
        .mark = s.occupied.count,
        .words = uint32_t(width + 63) / 64,
        .majorBase = majorMin,
        .minorBase = minorMin,
        .rows = 0,
    };
    for (uint32_t i = 0; i < g.count; ++i) {
        const LayoutGridItem& it = g.item(i);
        if (it.definite[major] && it.definite[minor])
            occupy(o, it.start[major], it.end[major], it.start[minor], it.end[minor]);
    }

    // Each item locked to a track of the flow's axis takes the first position along the other axis, from the start of
    // the explicit grid, where it fits. A sparse flow also starts past the items that this step placed in that track.
    Array<int32_t> cursors;
    for (uint32_t i = 0; i < g.count; ++i) {
        LayoutGridItem& it = g.item(i);
        if (!it.definite[major] || it.definite[minor])
            continue;
        int32_t span = it.end[minor];
        int32_t row = it.start[major] - majorMin;
        while (cursors.count <= uint32_t(row))
            cursors.push(0);
        int32_t at = dense ? 0 : cursors.data[row];
        while (occupied(o, it.start[major], it.end[major], at, at + span))
            ++at;
        it.start[minor] = at;
        it.end[minor] = at + span;
        it.definite[minor] = true;
        occupy(o, it.start[major], it.end[major], at, at + span);
        cursors.data[row] = at + span;
        minorMax = it.end[minor] > minorMax ? it.end[minor] : minorMax;
    }
    cursors.reset();
    if (widestAuto > minorMax - minorMin)
        minorMax = minorMin + widestAuto;

    // The cursor places the remaining items. An item with a definite position along the other axis moves only through
    // the flow's tracks.
    int32_t cursorMajor = majorMin;
    int32_t cursorMinor = minorMin;
    for (uint32_t i = 0; i < g.count; ++i) {
        LayoutGridItem& it = g.item(i);
        if (it.definite[major])
            continue;
        int32_t span = it.end[major];
        if (it.definite[minor]) {
            if (dense) {
                cursorMajor = majorMin;
            } else if (it.start[minor] < cursorMinor) {
                ++cursorMajor;
            }
            cursorMinor = it.start[minor];
            while (occupied(o, cursorMajor, cursorMajor + span, it.start[minor], it.end[minor]))
                ++cursorMajor;
        } else {
            int32_t minorSpan = it.end[minor];
            if (dense) {
                cursorMajor = majorMin;
                cursorMinor = minorMin;
            }
            for (;;) {
                bool found = false;
                for (; cursorMinor + minorSpan <= minorMax; ++cursorMinor) {
                    if (!occupied(o, cursorMajor, cursorMajor + span, cursorMinor, cursorMinor + minorSpan)) {
                        found = true;
                        break;
                    }
                }
                if (found)
                    break;
                ++cursorMajor;
                cursorMinor = minorMin;
            }
            it.start[minor] = cursorMinor;
            it.end[minor] = cursorMinor + minorSpan;
            it.definite[minor] = true;
        }
        it.start[major] = cursorMajor;
        it.end[major] = cursorMajor + span;
        it.definite[major] = true;
        occupy(o, it.start[major], it.end[major], it.start[minor], it.end[minor]);
    }
    s.occupied.count = o.mark;
}

// Finds the implicit grid of each axis from the explicit grid and the placed items, and counts the items' lines from
// its start (css-grid-1/#implicit-grids).
void findImplicitGrid(Grid& g) {
    for (PrLayoutGridAxis axis = 0; axis < PrLayoutGridAxis_Count; ++axis) {
        GridAxis& a = g.axes[axis];
        int32_t lo = 0;
        int32_t hi = a.explicitCount;
        for (uint32_t i = 0; i < g.count; ++i) {
            const LayoutGridItem& it = g.item(i);
            lo = it.start[axis] < lo ? it.start[axis] : lo;
            hi = it.end[axis] > hi ? it.end[axis] : hi;
        }
        a.explicitStart = -lo;
        a.trackCount = uint32_t(hi - lo);
        for (uint32_t i = 0; i < g.count; ++i) {
            LayoutGridItem& it = g.item(i);
            it.start[axis] += a.explicitStart;
            it.end[axis] += a.explicitStart;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Tracks

// Returns the kind of a sizing function of `breadth`, and sets `*size` to the size of a fixed function, or to the flex
// factor of a flexible one. A percentage of an indefinite `base` is auto (css-grid-1/#track-sizes). A flex factor is
// auto as a minimum (css-grid-1/#algo-terms).
LayoutGridSizing sizingOf(const LayoutStyles* styles, PrLayoutTrackBreadth breadth, PrLayoutUnit base, bool minimum,
        double* size) {
    *size = 0;
    switch (prLayoutWordKind(breadth.bits)) {
        case PrLayoutWordKind_Number:
            if (minimum)
                return LayoutGridSizing_Auto;
            *size = maxF64(0, double(prLayoutWordNumberValue(breadth.bits)));
            return LayoutGridSizing_Flex;
        case PrLayoutWordKind_Keyword:
            switch (prLayoutWordKeywordValue(breadth.bits)) {
                case PrLayoutKeyword_MinContent: return LayoutGridSizing_MinContent;
                case PrLayoutKeyword_MaxContent: return LayoutGridSizing_MaxContent;
                case PrLayoutKeyword_FitContent:
                    return minimum ? LayoutGridSizing_Auto : LayoutGridSizing_FitContent;
                default: return LayoutGridSizing_Auto;
            }
        default: {
            PrLayoutLength length{breadth.bits};
            if (!layoutResolves(length, base))
                return LayoutGridSizing_Auto;
            *size = maxF64(0, rawOf(layoutResolve(styles, length, base)));
            return LayoutGridSizing_Fixed;
        }
    }
}

// Returns true if a sizing function is intrinsic: auto, min-content, max-content or fit-content.
bool isIntrinsic(LayoutGridSizing sizing) {
    return sizing == LayoutGridSizing_Auto || sizing == LayoutGridSizing_MinContent ||
        sizing == LayoutGridSizing_MaxContent || sizing == LayoutGridSizing_FitContent;
}

// Sets up the tracks of `axis` from their sizing functions, with percentages of `base`, and collapses the empty tracks
// of an auto-fit repetition (css-grid-1/#auto-repeat). A collapsed track has a fixed size of zero.
void setUpTracks(Grid& g, PrLayoutGridAxis axis, PrLayoutUnit base) {
    GridAxis& a = g.axes[axis];
    Array<GridTrack>& tracks = g.scratch().tracks;
    a.trackMark = tracks.count;
    tracks.resize(tracks.count + a.trackCount);
    for (uint32_t t = 0; t < a.trackCount; ++t) {
        const PrLayoutGridTrack& sizes = trackSizes(a, int32_t(t) - a.explicitStart);
        GridTrack track{};
        track.min = sizingOf(g.state.styles, sizes.min, base, true, &track.minFixed);
        track.max = sizingOf(g.state.styles, sizes.max, base, false, &track.maxFixed);
        if (track.max == LayoutGridSizing_FitContent) {
            PrLayoutLength limit = sizes.limit;
            track.maxFixed = kInfinityF64;
            if (layoutResolves(limit, base))
                track.maxFixed = maxF64(0, rawOf(layoutResolve(g.state.styles, limit, base)));
        }
        tracks.data[a.trackMark + t] = track;
    }
    if (!a.collapses)
        return;
    const LayoutGridTracksEntry& list = *a.tracks;
    uint32_t first = uint32_t(a.explicitStart) + list.repeatStart;
    uint32_t end = first + list.repeatCount * a.repetitions;
    for (uint32_t t = first; t < end; ++t)
        g.track(axis, t).collapsed = true;
    for (uint32_t i = 0; i < g.count; ++i) {
        const LayoutGridItem& it = g.item(i);
        for (int32_t t = it.start[axis]; t < it.end[axis]; ++t)
            g.track(axis, uint32_t(t)).collapsed = false;
    }
    for (uint32_t t = first; t < end; ++t) {
        GridTrack& track = g.track(axis, t);
        if (track.collapsed) {
            track.min = LayoutGridSizing_Fixed;
            track.max = LayoutGridSizing_Fixed;
            track.minFixed = 0;
            track.maxFixed = 0;
        }
    }
}

// Returns the gap that follows the track `t` of `axis`: the axis's gap, unless no track that is not collapsed precedes
// it or follows it, or the next such track is past a collapsed track, whose gaps coincide with the gap before it
// (css-grid-1/#collapsed-gutter).
double gapAfter(const Grid& g, PrLayoutGridAxis axis, uint32_t t) {
    const GridAxis& a = g.axes[axis];
    if (t + 1 >= a.trackCount || a.gap == kLayoutUnitZero)
        return 0;
    if (!a.collapses)
        return rawOf(a.gap);
    // The gap after a collapsed track coincides with the one before it.
    if (g.track(axis, t).collapsed)
        return 0;
    for (uint32_t u = t + 1; u < a.trackCount; ++u) {
        if (!g.track(axis, u).collapsed)
            return rawOf(a.gap);
    }
    return 0;
}

// Returns the gaps between the tracks from `start` to `end` of `axis`, which an item spanning them crosses.
double gapsWithin(const Grid& g, PrLayoutGridAxis axis, int32_t start, int32_t end) {
    double sum = 0;
    for (int32_t t = start; t + 1 < end; ++t)
        sum += gapAfter(g, axis, uint32_t(t));
    return sum;
}

// ---------------------------------------------------------------------------------------------------------------------
// Items

// Returns the axis of the container that the item's inline axis runs along.
PrLayoutGridAxis inlineAxisOf(const LayoutGridItem& it) {
    return it.orthogonal ? PrLayoutGridAxis_Rows : PrLayoutGridAxis_Columns;
}

// The sides of the container that bound an axis.
LayoutSide startSideOf(PrLayoutGridAxis axis) {
    return axis == PrLayoutGridAxis_Columns ? LayoutSide_InlineStart : LayoutSide_BlockStart;
}
LayoutSide endSideOf(PrLayoutGridAxis axis) {
    return axis == PrLayoutGridAxis_Columns ? LayoutSide_InlineEnd : LayoutSide_BlockEnd;
}

// The sizing properties of an item along one axis of its container.
struct AxisSizes {
    PrLayoutLength size;
    PrLayoutLength min;
    PrLayoutLength max;
};

// Returns the sizing properties of `logical`, an item's style in its container's mode, along `axis`.
AxisSizes axisSizesOf(const LayoutLogicalBox& logical, PrLayoutGridAxis axis) {
    if (axis == PrLayoutGridAxis_Columns)
        return AxisSizes{logical.inlineSize, logical.minInlineSize, logical.maxInlineSize};
    return AxisSizes{logical.blockSize, logical.minBlockSize, logical.maxBlockSize};
}

// Returns the padding and borders of `e` across `axis`.
PrLayoutUnit edgesAcross(const LayoutEdges& e, PrLayoutGridAxis axis) {
    return axis == PrLayoutGridAxis_Columns ? layoutEdgesInline(e) : layoutEdgesBlock(e);
}

// Returns the margins of `e` across `axis`, with auto margins as zero.
PrLayoutUnit marginsAcross(const LayoutEdges& e, PrLayoutGridAxis axis) {
    return e.margin[startSideOf(axis)] + e.margin[endSideOf(axis)];
}

// Returns the edge of the start of the track `t` of `axis`, and of its end, in the content box.
double trackStart(const Grid& g, PrLayoutGridAxis axis, int32_t t) {
    return g.track(axis, uint32_t(t)).offset;
}
double trackEnd(const Grid& g, PrLayoutGridAxis axis, int32_t t) {
    const GridTrack& track = g.track(axis, uint32_t(t));
    return track.offset + track.base;
}

// Returns the size of the item's grid area along `axis`, once that axis's tracks are placed.
PrLayoutUnit areaOf(const Grid& g, const LayoutGridItem& it, PrLayoutGridAxis axis) {
    return unitOf(trackEnd(g, axis, it.end[axis] - 1)) - unitOf(trackStart(g, axis, it.start[axis]));
}

// Returns the sum of the sizes of the tracks that the item spans along `axis`, and of the gaps between them, if each
// has a fixed maximum, or a negative size otherwise. Such a sum stands for the area's size along the axis before the
// axis is sized (css-grid-1/#algo-grid-sizing).
double fixedSpanOf(const Grid& g, const LayoutGridItem& it, PrLayoutGridAxis axis) {
    double sum = gapsWithin(g, axis, it.start[axis], it.end[axis]);
    for (int32_t t = it.start[axis]; t < it.end[axis]; ++t) {
        const GridTrack& track = g.track(axis, uint32_t(t));
        if (track.max != LayoutGridSizing_Fixed)
            return -1;
        sum += track.maxFixed;
    }
    return sum;
}

// Returns the space that an item is measured or laid out in: an area of `inlineSize` and `blockSize` in the container's
// mode, the latter negative if indefinite.
LayoutSpace areaSpace(const Grid& g, PrLayoutUnit inlineSize, PrLayoutUnit blockSize) {
    return LayoutSpace{
        .mode = g.mode(),
        .available = inlineSize,
        .percentInline = inlineSize,
        .percentBlock = blockSize,
        .containing = LayoutLogicalSize{inlineSize, blockSize},
        .blockLimit = layoutIsDefinite(blockSize) ? layoutUnit(-1) : g.content.blockLimit,
        .fallback = g.content.fallback,
    };
}

// Returns the intrinsic sizes of the item in its own inline axis, with the edges that it resolves in an area whose
// inline size is `containingInline`, and with `percentBlock` as the base of its block-axis percentages.
LayoutIntrinsicSizes intrinsicOf(const Grid& g, const LayoutGridItem& it, PrLayoutUnit containingInline,
        PrLayoutUnit percentBlock) {
    LayoutIntrinsicInput input = layoutIntrinsicInputFromSpace(g.content);
    input.percentBlock = percentBlock;
    if (it.orthogonal)
        input.mode = layoutOwnMode(g.state.tree, it.node);
    LayoutLogicalBox logical = layoutLogicalBox(*it.box, input.mode);
    LayoutEdges measured = layoutEdgesFromBox(g.state.styles, logical, layoutUnit(-1));
    LayoutEdges used = layoutEdgesFromBox(g.state.styles, logical, containingInline);
    return layoutWithEdges(layoutIntrinsicSizes(g.state, it.node, input), layoutEdgesInline(measured),
        layoutEdgesInline(used));
}

// Returns true if the item, whose automatic minimum size along `axis` is in question, takes its content-based minimum
// size there (css-grid-1/#min-size-auto): it is not a scroll container, it spans a track whose minimum is auto, and it
// spans no flexible track if it spans more than one track.
bool takesContentMinimum(const Grid& g, const LayoutGridItem& it, PrLayoutGridAxis axis) {
    if (layoutIsScrollContainer(*it.box))
        return false;
    bool autoMinimum = false;
    for (int32_t t = it.start[axis]; t < it.end[axis]; ++t) {
        const GridTrack& track = g.track(axis, uint32_t(t));
        autoMinimum = autoMinimum || track.min == LayoutGridSizing_Auto;
        if (track.max == LayoutGridSizing_Flex && it.end[axis] - it.start[axis] > 1)
            return false;
    }
    return autoMinimum;
}

// Returns the border-box size of the item along its own inline axis, in an area of `area` along that axis, with its
// edges `e` in the container's mode (css-grid-1/#grid-item-sizing). A stretched item fills the area less its margins,
// floored by its automatic minimum size. An item with an auto size that is not stretched fits its content. A preferred
// size resolves against the area. The limits clamp each.
PrLayoutUnit inlineSizeOf(const Grid& g, const LayoutGridItem& it, PrLayoutUnit area, const LayoutEdges& e,
        PrLayoutUnit percentBlock) {
    PrLayoutGridAxis axis = inlineAxisOf(it);
    LayoutLogicalBox logical = layoutLogicalBox(*it.box, g.mode());
    AxisSizes sizes = axisSizesOf(logical, axis);
    PrLayoutUnit edges = edgesAcross(e, axis);
    PrLayoutUnit stretch = layoutStretchFit(area, e.margin[startSideOf(axis)], e.margin[endSideOf(axis)]);
    bool measured = false;
    LayoutIntrinsicSizes intrinsic{};
    auto keyword = [&](PrLayoutLength length) {
        if (!measured)
            intrinsic = intrinsicOf(g, it, area, percentBlock);
        measured = true;
        return layoutKeywordSize(length, intrinsic, stretch);
    };
    PrLayoutUnit size = kLayoutUnitZero;
    bool stretched = it.place[axis] == LayoutGridPlace_Stretch;
    if (stretched)
        size = layoutUnitMax(stretch, edges);
    else if (layoutLengthIsStretch(sizes.size))
        size = layoutUnitMax(stretch, edges);
    else if (layoutLengthIsFixed(sizes.size))
        size = layoutBorderSize(*it.box, layoutResolve(g.state.styles, sizes.size, area), edges);
    else
        size = keyword(layoutLengthIsContentBased(sizes.size) ? sizes.size : prLayoutLengthFitContent());
    PrLayoutUnit clamped = layoutClampSize(g.state.styles, *it.box, size, sizes.min, sizes.max, area, edges, stretch, keyword);
    // A stretched item does not shrink below its content-based minimum size, which a fixed area caps.
    if (stretched && layoutLengthIsAuto(sizes.min) && takesContentMinimum(g, it, axis)) {
        PrLayoutUnit least = keyword(prLayoutLengthMinContent());
        double cap = fixedSpanOf(g, it, axis);
        if (cap >= 0)
            least = layoutUnitMin(least, layoutUnitMax(unitOf(cap) - marginsAcross(e, axis), edges));
        if (layoutResolves(sizes.max, area))
            least = layoutUnitMin(least, layoutBorderSize(*it.box, layoutResolve(g.state.styles, sizes.max, area), edges));
        clamped = layoutUnitMax(clamped, least);
    }
    return clamped;
}

// Returns what the container asks of the item's size along its own block axis, in an area of `area` along that axis,
// which is negative if indefinite. A stretched item with an auto size is fixed to the area less its margins, clamped by
// its limits. Any other item sizes itself.
LayoutRequest blockRequestOf(const Grid& g, const LayoutGridItem& it, PrLayoutUnit area, const LayoutEdges& e) {
    PrLayoutGridAxis axis =
        inlineAxisOf(it) == PrLayoutGridAxis_Columns ? PrLayoutGridAxis_Rows : PrLayoutGridAxis_Columns;
    if (it.place[axis] != LayoutGridPlace_Stretch || !layoutIsDefinite(area))
        return LayoutRequest{};
    LayoutLogicalBox logical = layoutLogicalBox(*it.box, g.mode());
    AxisSizes sizes = axisSizesOf(logical, axis);
    PrLayoutUnit edges = edgesAcross(e, axis);
    PrLayoutUnit stretch = layoutStretchFit(area, e.margin[startSideOf(axis)], e.margin[endSideOf(axis)]);
    PrLayoutUnit size = layoutClampSize(g.state.styles, *it.box, layoutUnitMax(stretch, edges), sizes.min, sizes.max,
        area, edges, stretch, layoutNoKeyword);
    return LayoutRequest{.size = size, .kind = LayoutRequestKind_Fixed, .definite = true};
}

// Returns the requests of the item in an area of `columns` by `rows`, the latter negative if indefinite, with its edges
// `e`.
LayoutRequests requestsOf(const Grid& g, const LayoutGridItem& it, PrLayoutUnit columns, PrLayoutUnit rows,
        const LayoutEdges& e) {
    bool orthogonal = it.orthogonal;
    PrLayoutUnit inlineArea = orthogonal ? rows : columns;
    PrLayoutUnit blockArea = orthogonal ? columns : rows;
    PrLayoutUnit percentBlock = orthogonal ? columns : rows;
    PrLayoutUnit inlineSize = inlineSizeOf(g, it, inlineArea, e, percentBlock);
    return LayoutRequests{
        .inlineAxis = LayoutRequest{.size = inlineSize, .kind = LayoutRequestKind_Fixed, .definite = true},
        .blockAxis = blockRequestOf(g, it, blockArea, e),
    };
}

// Measures the item's block size, and its baselines if `kind` asks for them, in its column area once the columns are
// placed, with rows not yet sized. Returns the measurement.
LayoutMeasurement measureInColumns(Grid& g, const LayoutGridItem& it, LayoutMeasureKind kind) {
    PrLayoutUnit columns = areaOf(g, it, PrLayoutGridAxis_Columns);
    LayoutEdges e = layoutEdgesFromBox(g.state.styles, layoutLogicalBox(*it.box, g.mode()), columns);
    LayoutRequests requests = requestsOf(g, it, columns, layoutUnit(-1), e);
    return layoutMeasure(g.state, it.node, areaSpace(g, columns, layoutUnit(-1)), requests, kind);
}

// Finds the min-content and max-content contributions of the item `i` to the tracks of `axis` that it spans
// (css-sizing-3/#contributions), if they are not yet known in this sizing of the axis. Sizes of the other axis's area
// that are not yet known are indefinite, except that the rows of an item sized for the columns are taken at their fixed
// maximums if each has one (css-grid-1/#algo-grid-sizing).
void findContentContributions(Grid& g, uint32_t i, PrLayoutGridAxis axis) {
    LayoutGridItem copy = g.item(i);
    if ((copy.known & LayoutGridContribution_MinContent) != 0)
        return;
    double minContent = 0;
    double maxContent = 0;
    if (axis == PrLayoutGridAxis_Columns) {
        double rows;
        if (g.placed[PrLayoutGridAxis_Rows])
            rows = rawOf(areaOf(g, copy, PrLayoutGridAxis_Rows));
        else
            rows = fixedSpanOf(g, copy, PrLayoutGridAxis_Rows);
        LayoutIntrinsicInput input = layoutIntrinsicInputFromSpace(g.content);
        input.percentBlock = rows >= 0 ? unitOf(rows) : layoutUnit(-1);
        // The contribution of an anonymous item is its intrinsic sizes, because it has no sizes or edges of its own.
        LayoutIntrinsicSizes c;
        if (copy.anonymous)
            c = layoutIntrinsicSizes(g.state, copy.node, input);
        else
            c = layoutContribution(g.state, copy.node, input);
        minContent = rawOf(c.minContent);
        maxContent = rawOf(c.maxContent);
    } else if (copy.orthogonal) {
        PrLayoutUnit columns = areaOf(g, copy, PrLayoutGridAxis_Columns);
        LayoutIntrinsicInput input = layoutIntrinsicInputFromSpace(g.content);
        input.mode = layoutOwnMode(g.state.tree, copy.node);
        input.percentBlock = columns;
        LayoutIntrinsicSizes c = layoutContribution(g.state, copy.node, input);
        minContent = rawOf(c.minContent);
        maxContent = rawOf(c.maxContent);
    } else {
        bool baselines =
            copy.place[axis] == LayoutGridPlace_Baseline || copy.place[axis] == LayoutGridPlace_LastBaseline;
        LayoutMeasurement m =
            measureInColumns(g, copy, baselines ? LayoutMeasureKind_Baselines : LayoutMeasureKind_Size);
        PrLayoutUnit columns = areaOf(g, copy, PrLayoutGridAxis_Columns);
        LayoutEdges e = layoutEdgesFromBox(g.state.styles, layoutLogicalBox(*copy.box, g.mode()), columns);
        minContent = rawOf(m.blockSize + marginsAcross(e, axis));
        maxContent = minContent;
        copy.first = m.first;
        copy.last = m.last;
    }
    LayoutGridItem& it = g.item(i);
    it.first = copy.first;
    it.last = copy.last;
    it.minContent = minContent;
    it.maxContent = maxF64(minContent, maxContent);
    it.known |= LayoutGridContribution_MinContent | LayoutGridContribution_MaxContent;
}

// Returns the min-content or max-content contribution of the item `i` to `axis`, with its baseline shims.
double minContentOf(Grid& g, uint32_t i, PrLayoutGridAxis axis) {
    findContentContributions(g, i, axis);
    const LayoutGridItem& it = g.item(i);
    return it.minContent + it.shimStart + it.shimEnd;
}
double maxContentOf(Grid& g, uint32_t i, PrLayoutGridAxis axis) {
    findContentContributions(g, i, axis);
    const LayoutGridItem& it = g.item(i);
    return it.maxContent + it.shimStart + it.shimEnd;
}

// Returns the minimum contribution of the item `i` to `axis` (css-grid-1/#min-size-contribution): the outer size that
// its used minimum size gives if its preferred size behaves as auto or depends on its containing block, and otherwise
// its min-content contribution. An automatic minimum is its content-based minimum size where it applies
// (css-grid-1/#min-size-auto), and zero otherwise. Percentages of the area resolve against zero, as cyclic percentages
// do in a contribution (css-sizing-3/#cyclic-percentage-contribution).
double minimumOf(Grid& g, uint32_t i, PrLayoutGridAxis axis) {
    LayoutGridItem copy = g.item(i);
    if ((copy.known & LayoutGridContribution_Minimum) != 0)
        return copy.minimum + copy.shimStart + copy.shimEnd;
    LayoutLogicalBox logical = layoutLogicalBox(*copy.box, g.mode());
    AxisSizes sizes = axisSizesOf(logical, axis);
    PrLayoutUnit containingInline =
        axis == PrLayoutGridAxis_Rows ? areaOf(g, copy, PrLayoutGridAxis_Columns) : layoutUnit(-1);
    LayoutEdges e = layoutEdgesFromBox(g.state.styles, logical, containingInline);
    PrLayoutUnit edges = edgesAcross(e, axis);
    PrLayoutUnit margins = marginsAcross(e, axis);
    bool behavesAuto = layoutLengthIsAuto(sizes.size) || layoutLengthIsStretch(sizes.size) ||
        (layoutLengthIsFixed(sizes.size) && layoutLengthReadsBase(sizes.size));
    double minimum = 0;
    if (!behavesAuto) {
        minimum = minContentOf(g, i, axis) - g.item(i).shimStart - g.item(i).shimEnd;
    } else if (layoutLengthIsAuto(sizes.min)) {
        PrLayoutUnit least = edges;
        if (takesContentMinimum(g, copy, axis)) {
            // The content size suggestion: the min-content size along the axis, which for the item's block axis is the
            // size of its content at its inline size.
            PrLayoutUnit content = kLayoutUnitZero;
            if (axis == inlineAxisOf(copy)) {
                PrLayoutUnit percentBlock = copy.orthogonal ? containingInline : layoutUnit(-1);
                content = intrinsicOf(g, copy, containingInline, percentBlock).minContent;
            } else if (axis == PrLayoutGridAxis_Rows) {
                // Laid out at its column area with its preferred block size and limits unread.
                PrLayoutUnit columns = areaOf(g, copy, PrLayoutGridAxis_Columns);
                LayoutRequests requests = requestsOf(g, copy, columns, layoutUnit(-1), e);
                requests.blockAxis = LayoutRequest{.kind = LayoutRequestKind_Content};
                content = layoutMeasure(g.state, copy.node, areaSpace(g, columns, layoutUnit(-1)), requests,
                    LayoutMeasureKind_Size) .blockSize;
            } else {
                content = unitOf(minContentOf(g, i, axis) - g.item(i).shimStart - g.item(i).shimEnd) - margins;
            }
            double cap = fixedSpanOf(g, copy, axis);
            if (cap >= 0)
                content = layoutUnitMin(content, layoutUnitMax(unitOf(cap) - margins, edges));
            // The maximum size caps the suggestion, and the preferred size too for a compressible replaced item, whose
            // percentages of the area resolve against zero for this.
            PrLayoutNodeKind kind = layoutTreeNode(g.state.tree, copy.node).kind;
            bool compressible = kind == PrLayoutNodeKind_Replaced || kind == PrLayoutNodeKind_SemiReplaced;
            auto capBy = [&](PrLayoutLength length) {
                PrLayoutUnit size = layoutResolve(g.state.styles, length, kLayoutUnitZero);
                content = layoutUnitMin(content, layoutBorderSize(*copy.box, size, edges));
            };
            if (compressible && layoutLengthIsFixed(sizes.size))
                capBy(sizes.size);
            if (layoutLengthIsFixed(sizes.max) && (compressible || !layoutLengthReadsBase(sizes.max)))
                capBy(sizes.max);
            least = layoutUnitMax(least, content);
        }
        minimum = rawOf(least + margins);
    } else if (layoutLengthIsContentBased(sizes.min)) {
        minimum = minContentOf(g, i, axis) - g.item(i).shimStart - g.item(i).shimEnd;
    } else {
        PrLayoutUnit least = edges;
        if (layoutLengthIsFixed(sizes.min))
            least = layoutBorderSize(*copy.box, layoutResolve(g.state.styles, sizes.min, kLayoutUnitZero), edges);
        minimum = rawOf(layoutUnitMax(least, edges) + margins);
    }
    LayoutGridItem& it = g.item(i);
    it.minimum = minimum;
    it.known |= LayoutGridContribution_Minimum;
    return minimum + it.shimStart + it.shimEnd;
}

// Returns the sum of the fixed maximums of the tracks that the item `i` spans along `axis`, with fit-content limits
// counting as fixed, and the gaps between them, or infinity if any of them has another maximum. A limited contribution
// is capped by that sum (css-grid-1/#limited-contribution).
double limitOf(const Grid& g, uint32_t i, PrLayoutGridAxis axis) {
    const LayoutGridItem& it = g.item(i);
    double sum = gapsWithin(g, axis, it.start[axis], it.end[axis]);
    for (int32_t t = it.start[axis]; t < it.end[axis]; ++t) {
        const GridTrack& track = g.track(axis, uint32_t(t));
        if (track.max != LayoutGridSizing_Fixed && track.max != LayoutGridSizing_FitContent)
            return kInfinityF64;
        sum += track.maxFixed;
    }
    return sum;
}

// Returns the limited min-content or max-content contribution of the item `i` to `axis`: its contribution capped by the
// fixed maximums of the tracks it spans, and floored by its minimum contribution (css-grid-1/#limited-contribution).
double limitedMinContentOf(Grid& g, uint32_t i, PrLayoutGridAxis axis) {
    return maxF64(minF64(minContentOf(g, i, axis), limitOf(g, i, axis)), minimumOf(g, i, axis));
}
double limitedMaxContentOf(Grid& g, uint32_t i, PrLayoutGridAxis axis) {
    return maxF64(minF64(maxContentOf(g, i, axis), limitOf(g, i, axis)), minimumOf(g, i, axis));
}

// ---------------------------------------------------------------------------------------------------------------------
// Track sizing

// Returns true if the item `i` spans a flexible track of `axis`.
bool spansFlexibleTrack(const Grid& g, const LayoutGridItem& it, PrLayoutGridAxis axis) {
    for (int32_t t = it.start[axis]; t < it.end[axis]; ++t) {
        if (g.track(axis, uint32_t(t)).max == LayoutGridSizing_Flex)
            return true;
    }
    return false;
}

// Which tracks receive space beyond their limits when an item's contribution remains after they reach them
// (css-grid-1/#distribute-extra-space): those with an intrinsic maximum, or else every affected track, when base sizes
// accommodate minimum or min-content contributions; those with a max-content maximum, or else every affected track,
// when base sizes accommodate max-content contributions; and those with an intrinsic maximum when growth limits
// accommodate any contribution.
enum {
    LayoutGridBeyond_Intrinsic,
    LayoutGridBeyond_MaxContent,
    LayoutGridBeyond_Limits,
};
typedef uint8_t LayoutGridBeyond;

// Returns true if a track's maximum is max-content for the distribution of space, as auto and fit-content are
// (css-grid-1/#algo-terms).
bool maxIsMaxContent(LayoutGridSizing max) {
    return max == LayoutGridSizing_MaxContent || max == LayoutGridSizing_Auto || max == LayoutGridSizing_FitContent;
}

// Shares `space` among the tracks of `axis` at `tracks`, `count` of them, by `weight`, each up to the room that `room`
// gives it, and adds each share to the track's incurred increase. Returns the space that is left once every track is
// full.
template <typename Room, typename Weight>
double share(Grid& g, PrLayoutGridAxis axis, const uint32_t* tracks, uint32_t count, double space, Room room, Weight weight) {
    for (uint32_t k = 0; k < count; ++k)
        g.track(axis, tracks[k]).frozen = false;
    while (space > 0) {
        double total = 0;
        for (uint32_t k = 0; k < count; ++k) {
            const GridTrack& track = g.track(axis, tracks[k]);
            if (!track.frozen)
                total += weight(track);
        }
        if (total <= 0)
            break;
        // A track whose share would pass its room takes its room and is frozen, and the rest share again.
        bool froze = false;
        for (uint32_t k = 0; k < count; ++k) {
            GridTrack& track = g.track(axis, tracks[k]);
            if (track.frozen)
                continue;
            double r = maxF64(0, room(track));
            if (space * weight(track) / total >= r) {
                track.incurred += r;
                space -= r;
                track.frozen = true;
                froze = true;
            }
        }
        if (froze)
            continue;
        double left = space;
        for (uint32_t k = 0; k < count; ++k) {
            GridTrack& track = g.track(axis, tracks[k]);
            if (!track.frozen)
                track.incurred += left * weight(track) / total;
        }
        space = 0;
    }
    return space;
}

// Distributes extra space to the base sizes of the tracks of `axis` that `affected` selects, or to their growth limits
// if `limits` is true, to accommodate the contributions that `contribution` gives of the items at `items`, `count` of
// them (css-grid-1/#extra-space). `beyond` says which tracks take space past their limits. Flexible tracks share space
// by their flex factors if `byFlex` is true, and every other distribution is equal. A track whose growth limit becomes
// finite here is infinitely growable if `growable` is true.
template <typename Affected, typename Contribution>
void distribute(Grid& g, PrLayoutGridAxis axis, const uint32_t* items, uint32_t count, bool limits, Affected affected,
        Contribution contribution, LayoutGridBeyond beyond, bool byFlex, bool growable) {
    uint32_t trackCount = g.axes[axis].trackCount;
    for (uint32_t t = 0; t < trackCount; ++t) {
        GridTrack& track = g.track(axis, t);
        track.planned = 0;
        track.frozen = false;
    }
    Array<uint32_t>& set = g.scratch().order;
    for (uint32_t t = 0; t < trackCount; ++t)
        g.track(axis, t).touched = false;
    auto sizeOf = [&](const GridTrack& track) {
        return limits ? (track.limit == kInfinityF64 ? track.base : track.limit) : track.base;
    };
    // The most a track's affected size reaches before it is full: a base size its growth limit, capped by its
    // fit-content limit, and a growth limit itself unless it is infinite or infinitely growable.
    auto limitOfTrack = [&](const GridTrack& track) {
        double fit = track.max == LayoutGridSizing_FitContent ? track.maxFixed : kInfinityF64;
        if (!limits)
            return minF64(track.limit, fit);
        if (track.limit != kInfinityF64 && !track.infinitelyGrowable)
            return track.limit;
        return fit;
    };
    auto room = [&](const GridTrack& track) { return limitOfTrack(track) - sizeOf(track) - track.incurred; };
    for (uint32_t k = 0; k < count; ++k) {
        uint32_t i = items[k];
        double wanted = contribution(i);
        const LayoutGridItem& it = g.item(i);
        int32_t first = it.start[axis];
        int32_t end = it.end[axis];
        double sum = gapsWithin(g, axis, first, end);
        for (int32_t t = first; t < end; ++t)
            sum += sizeOf(g.track(axis, uint32_t(t)));
        double space = maxF64(0, wanted - sum);
        uint32_t mark = set.count;
        double factors = 0;
        uint32_t flexible = 0;
        for (int32_t t = first; t < end; ++t) {
            GridTrack& track = g.track(axis, uint32_t(t));
            track.incurred = 0;
            if (affected(track)) {
                set.push(uint32_t(t));
                track.touched = true;
                if (track.max == LayoutGridSizing_Flex) {
                    factors += track.maxFixed;
                    ++flexible;
                }
            }
        }
        uint32_t affectedCount = set.count - mark;
        if (affectedCount == 0)
            continue;
        // Flexible tracks share by their factors if those reach one, and otherwise share that part by their factors and
        // the rest equally.
        auto weight = [&](const GridTrack& track) {
            if (!byFlex || flexible == 0)
                return 1.0;
            if (factors >= 1)
                return track.maxFixed / factors;
            return track.maxFixed + (1 - factors) / double(flexible);
        };
        if (space > 0)
            space = share(g, axis, set.data + mark, affectedCount, space, room, weight);
        // Space that remains goes past the limits, to the tracks that `beyond` selects. A fit-content track takes it as
        // a max-content track until it reaches its limit.
        if (space > 0) {
            uint32_t chosen = set.count;
            for (uint32_t s = mark; s < chosen; ++s) {
                uint32_t t = set.data[s];
                const GridTrack& track = g.track(axis, t);
                bool takes;
                if (beyond == LayoutGridBeyond_MaxContent)
                    takes = maxIsMaxContent(track.max);
                else
                    takes = isIntrinsic(track.max);
                if (takes)
                    set.push(t);
            }
            auto pastLimit = [&](const GridTrack& track) {
                if (track.max != LayoutGridSizing_FitContent)
                    return kInfinityF64;
                return track.maxFixed - sizeOf(track) - track.incurred;
            };
            if (set.count > chosen)
                space = share(g, axis, set.data + chosen, set.count - chosen, space, pastLimit, weight);
            if (space > 0 && beyond != LayoutGridBeyond_Limits)
                share(g, axis, set.data + mark, affectedCount, space, [](const GridTrack&) { return kInfinityF64; }, weight);
            set.count = chosen;
        }
        for (uint32_t s = mark; s < mark + affectedCount; ++s) {
            GridTrack& track = g.track(axis, set.data[s]);
            track.planned = maxF64(track.planned, track.incurred);
        }
        set.count = mark;
    }
    // Each affected track that an item spans takes its planned increase. An infinite growth limit becomes the base size
    // with the increase.
    for (uint32_t t = 0; t < trackCount; ++t) {
        GridTrack& track = g.track(axis, t);
        if (!track.touched)
            continue;
        if (!limits) {
            track.base += track.planned;
        } else if (track.limit == kInfinityF64) {
            track.limit = track.base + track.planned;
            track.infinitelyGrowable = growable;
        } else {
            track.limit += track.planned;
        }
    }
}

// Raises each growth limit of `axis` that is below its track's base size to the base size.
void raiseLimits(Grid& g, PrLayoutGridAxis axis) {
    for (uint32_t t = 0; t < g.axes[axis].trackCount; ++t) {
        GridTrack& track = g.track(axis, t);
        if (track.limit < track.base)
            track.limit = track.base;
    }
}

// Adds a shim to the contributions of each item of `axis` that is aligned by its baseline there, so that the items of
// each baseline-sharing group align when they are aligned at the start or the end together
// (css-grid-1/#algo-baseline-shims). Items aligned by their first baseline share by the track where their span starts,
// and those aligned by their last baseline by the track where it ends.
void shimBaselines(Grid& g, PrLayoutGridAxis axis) {
    for (uint32_t i = 0; i < g.count; ++i) {
        LayoutGridItem& it = g.item(i);
        it.shimStart = 0;
        it.shimEnd = 0;
    }
    if (axis != PrLayoutGridAxis_Rows)
        return;
    uint32_t trackCount = g.axes[axis].trackCount;
    Array<double>& above = g.scratch().sizes;
    uint32_t mark = above.count;
    above.resize(mark + 2 * trackCount);
    for (uint32_t k = mark; k < above.count; ++k)
        above.data[k] = 0;
    bool any = false;
    // The extent above the baseline of each item in its group, measured from its margin box's start, and below it.
    auto extents = [&](uint32_t i, double* before, double* after) {
        findContentContributions(g, i, axis);
        const LayoutGridItem& it = g.item(i);
        PrLayoutUnit columns = areaOf(g, it, PrLayoutGridAxis_Columns);
        LayoutEdges e = layoutEdgesFromBox(g.state.styles, layoutLogicalBox(*it.box, g.mode()), columns);
        bool last = it.place[axis] == LayoutGridPlace_LastBaseline;
        const LayoutBaseline& b = last ? it.last : it.first;
        double outer = it.minContent;
        double size = outer - rawOf(marginsAcross(e, axis));
        double baseline = b.present ? rawOf(b.offset) : size;
        *before = rawOf(e.margin[LayoutSide_BlockStart]) + baseline;
        *after = outer - *before;
    };
    for (uint32_t i = 0; i < g.count; ++i) {
        const LayoutGridItem& it = g.item(i);
        bool first = it.place[axis] == LayoutGridPlace_Baseline;
        if (!first && it.place[axis] != LayoutGridPlace_LastBaseline)
            continue;
        any = true;
        double before = 0;
        double after = 0;
        extents(i, &before, &after);
        uint32_t slot = first ? uint32_t(it.start[axis]) : trackCount + uint32_t(it.end[axis] - 1);
        double extent = first ? before : after;
        above.data[mark + slot] = maxF64(above.data[mark + slot], extent);
    }
    if (any) {
        for (uint32_t i = 0; i < g.count; ++i) {
            bool first = g.item(i).place[axis] == LayoutGridPlace_Baseline;
            if (!first && g.item(i).place[axis] != LayoutGridPlace_LastBaseline)
                continue;
            double before = 0;
            double after = 0;
            extents(i, &before, &after);
            LayoutGridItem& it = g.item(i);
            if (first)
                it.shimStart = above.data[mark + uint32_t(it.start[axis])] - before;
            else
                it.shimEnd = above.data[mark + trackCount + uint32_t(it.end[axis] - 1)] - after;
        }
    }
    above.count = mark;
}

// Resolves the intrinsic sizes of the tracks of `axis` sized under `space` (css-grid-1/#algo-content): from the items
// that span one track, then from those that span more, by increasing span, and then from those that span flexible
// tracks. A growth limit that is still infinite becomes its track's base size.
void resolveIntrinsicSizes(Grid& g, PrLayoutGridAxis axis, LayoutGridSpace space) {
    shimBaselines(g, axis);
    bool constrained = space != LayoutGridSpace_Definite;
    uint32_t widest = 1;
    for (uint32_t i = 0; i < g.count; ++i) {
        LayoutGridItem& it = g.item(i);
        it.spansFlexible[axis] = spansFlexibleTrack(g, it, axis);
        uint32_t span = uint32_t(it.end[axis] - it.start[axis]);
        if (!it.spansFlexible[axis] && span > widest)
            widest = span;
    }

    // The items that span one track that is not flexible set its intrinsic sizes directly.
    for (uint32_t i = 0; i < g.count; ++i) {
        const LayoutGridItem& it = g.item(i);
        if (it.end[axis] - it.start[axis] != 1 || it.spansFlexible[axis])
            continue;
        uint32_t t = uint32_t(it.start[axis]);
        LayoutGridSizing min = g.track(axis, t).min;
        LayoutGridSizing max = g.track(axis, t).max;
        if (!isIntrinsic(min) && !isIntrinsic(max))
            continue;
        double base = 0;
        if (min == LayoutGridSizing_MinContent)
            base = minContentOf(g, i, axis);
        else if (min == LayoutGridSizing_MaxContent)
            base = maxContentOf(g, i, axis);
        else if (min == LayoutGridSizing_Auto)
            base = constrained ? limitedMinContentOf(g, i, axis) : minimumOf(g, i, axis);
        double limit = -1;
        if (max == LayoutGridSizing_MinContent)
            limit = minContentOf(g, i, axis);
        else if (maxIsMaxContent(max))
            limit = maxContentOf(g, i, axis);
        if (max == LayoutGridSizing_FitContent)
            limit = minF64(limit, g.track(axis, t).maxFixed);
        GridTrack& track = g.track(axis, t);
        track.base = maxF64(track.base, base);
        if (limit >= 0)
            track.limit = track.limit == kInfinityF64 ? limit : maxF64(track.limit, limit);
    }
    raiseLimits(g, axis);

    // The items that span more tracks, none flexible, by increasing span, and then every item that spans a flexible
    // track, whose contributions go only to flexible tracks.
    Array<uint32_t> group;
    for (uint32_t span = 2; span <= widest + 1; ++span) {
        bool flexible = span == widest + 1;
        group.clear();
        for (uint32_t i = 0; i < g.count; ++i) {
            const LayoutGridItem& it = g.item(i);
            bool spans = uint32_t(it.end[axis] - it.start[axis]) == span;
            if (flexible ? it.spansFlexible[axis] : !it.spansFlexible[axis] && spans)
                group.push(i);
        }
        if (group.count == 0)
            continue;
        auto only = [&](bool selected, const GridTrack& track) {
            return selected && (!flexible || track.max == LayoutGridSizing_Flex);
        };
        auto minimum = [&](uint32_t i) {
            return constrained ? limitedMinContentOf(g, i, axis) : minimumOf(g, i, axis);
        };
        auto minContent = [&](uint32_t i) { return minContentOf(g, i, axis); };
        auto maxContent = [&](uint32_t i) { return maxContentOf(g, i, axis); };
        auto limitedMaxContent = [&](uint32_t i) { return limitedMaxContentOf(g, i, axis); };
        distribute(g, axis, group.data, group.count, false,
            [&](const GridTrack& t) { return only(isIntrinsic(t.min), t); }, minimum,
            LayoutGridBeyond_Intrinsic, flexible, false);
        distribute(g, axis, group.data, group.count, false, [&](const GridTrack& t) {
            return only(t.min == LayoutGridSizing_MinContent || t.min == LayoutGridSizing_MaxContent, t);
        }, minContent, LayoutGridBeyond_Intrinsic, flexible, false);
        if (space == LayoutGridSpace_MaxContent)
            distribute(g, axis, group.data, group.count, false, [&](const GridTrack& t) {
                return only(t.min == LayoutGridSizing_Auto || t.min == LayoutGridSizing_MaxContent, t);
            }, limitedMaxContent, LayoutGridBeyond_MaxContent, flexible, false);
        distribute(g, axis, group.data, group.count, false,
            [&](const GridTrack& t) { return only(t.min == LayoutGridSizing_MaxContent, t); }, maxContent,
            LayoutGridBeyond_MaxContent, flexible, false);
        raiseLimits(g, axis);
        if (flexible)
            continue;
        for (uint32_t t = 0; t < g.axes[axis].trackCount; ++t)
            g.track(axis, t).infinitelyGrowable = false;
        distribute(g, axis, group.data, group.count, true, [](const GridTrack& t) { return isIntrinsic(t.max); },
            minContent, LayoutGridBeyond_Limits, false, true);
        distribute(g, axis, group.data, group.count, true, [](const GridTrack& t) { return maxIsMaxContent(t.max); },
            maxContent, LayoutGridBeyond_Limits, false, false);
    }
    group.reset();
    for (uint32_t t = 0; t < g.axes[axis].trackCount; ++t) {
        GridTrack& track = g.track(axis, t);
        track.infinitelyGrowable = false;
        if (track.limit == kInfinityF64)
            track.limit = track.base;
    }
}

// Returns the sum of the base sizes of the tracks of `axis` and of the gaps between them.
double totalOf(const Grid& g, PrLayoutGridAxis axis) {
    uint32_t count = g.axes[axis].trackCount;
    double sum = 0;
    for (uint32_t t = 0; t < count; ++t)
        sum += g.track(axis, t).base + gapAfter(g, axis, t);
    return sum;
}

// Returns the size of an fr of the tracks of `axis` from `first` to `end`, which fill `space`
// (css-grid-1/#algo-find-fr-size). A flexible track whose share would fall below its base size is treated as
// inflexible, and the size is found again.
double frSizeOf(Grid& g, PrLayoutGridAxis axis, int32_t first, int32_t end, double space) {
    for (int32_t t = first; t < end; ++t)
        g.track(axis, uint32_t(t)).frozen = g.track(axis, uint32_t(t)).max != LayoutGridSizing_Flex;
    for (;;) {
        double leftover = space - gapsWithin(g, axis, first, end);
        double factors = 0;
        for (int32_t t = first; t < end; ++t) {
            const GridTrack& track = g.track(axis, uint32_t(t));
            if (track.frozen)
                leftover -= track.base;
            else
                factors += track.maxFixed;
        }
        double fr = leftover / maxF64(factors, 1);
        bool again = false;
        for (int32_t t = first; t < end; ++t) {
            GridTrack& track = g.track(axis, uint32_t(t));
            if (!track.frozen && fr * track.maxFixed < track.base) {
                track.frozen = true;
                again = true;
            }
        }
        if (!again)
            return maxF64(0, fr);
    }
}

// Expands the flexible tracks of `axis` (css-grid-1/#algo-flex-tracks). The used flex fraction is zero under a
// min-content constraint or with no free space, fills a definite space, and is otherwise the largest that the flexible
// tracks' base sizes and the items that cross them ask for, refound against the container's limits if it passes them.
void expandFlexibleTracks(Grid& g, PrLayoutGridAxis axis, LayoutGridSpace space, double available) {
    uint32_t count = g.axes[axis].trackCount;
    bool any = false;
    for (uint32_t t = 0; t < count && !any; ++t)
        any = g.track(axis, t).max == LayoutGridSizing_Flex;
    if (!any || space == LayoutGridSpace_MinContent)
        return;
    double fr = 0;
    if (space == LayoutGridSpace_Definite) {
        if (available - totalOf(g, axis) <= 0)
            return;
        fr = frSizeOf(g, axis, 0, int32_t(count), available);
    } else {
        for (uint32_t t = 0; t < count; ++t) {
            const GridTrack& track = g.track(axis, t);
            if (track.max == LayoutGridSizing_Flex)
                fr = maxF64(fr, track.maxFixed > 1 ? track.base / track.maxFixed : track.base);
        }
        for (uint32_t i = 0; i < g.count; ++i) {
            const LayoutGridItem& it = g.item(i);
            if (!spansFlexibleTrack(g, it, axis))
                continue;
            int32_t first = it.start[axis];
            int32_t end = it.end[axis];
            fr = maxF64(fr, frSizeOf(g, axis, first, end, maxContentOf(g, i, axis)));
        }
        // A grid that the fraction makes smaller than the container's minimum or larger than its maximum is sized with
        // the fraction that fills that limit.
        double total = 0;
        for (uint32_t t = 0; t < count; ++t) {
            const GridTrack& track = g.track(axis, t);
            double flexed = track.max == LayoutGridSizing_Flex ? maxF64(track.base, fr * track.maxFixed) : track.base;
            total += flexed + gapAfter(g, axis, t);
        }
        double least = rawOf(g.minSize[axis]);
        if (total < least) {
            fr = frSizeOf(g, axis, 0, int32_t(count), least);
        } else if (layoutIsDefinite(g.maxSize[axis]) && total > rawOf(g.maxSize[axis])) {
            fr = frSizeOf(g, axis, 0, int32_t(count), rawOf(g.maxSize[axis]));
        }
    }
    for (uint32_t t = 0; t < count; ++t) {
        GridTrack& track = g.track(axis, t);
        if (track.max == LayoutGridSizing_Flex && fr * track.maxFixed > track.base)
            track.base = fr * track.maxFixed;
    }
}

// Sizes the tracks of `axis` under `space`, filling `available` if the space is definite, by the track sizing algorithm
// (css-grid-1/#algo-track-sizing): it initializes them, resolves their intrinsic sizes, maximizes them, expands the
// flexible ones and stretches the auto ones.
void sizeTracks(Grid& g, PrLayoutGridAxis axis, LayoutGridSpace space, double available) {
    uint32_t count = g.axes[axis].trackCount;
    for (uint32_t t = 0; t < count; ++t) {
        GridTrack& track = g.track(axis, t);
        track.base = track.min == LayoutGridSizing_Fixed ? track.minFixed : 0;
        track.limit = track.max == LayoutGridSizing_Fixed ? track.maxFixed : kInfinityF64;
        if (track.limit < track.base)
            track.limit = track.base;
        track.infinitelyGrowable = false;
    }
    for (uint32_t i = 0; i < g.count; ++i)
        g.item(i).known = 0;
    resolveIntrinsicSizes(g, axis, space);

    // Maximizes the tracks: positive free space grows every base size equally up to its growth limit. Under a
    // max-content constraint the free space is infinite, unless the container's maximum limits the grid.
    double free = 0;
    if (space == LayoutGridSpace_Definite)
        free = available - totalOf(g, axis);
    else if (space == LayoutGridSpace_MaxContent)
        free = layoutIsDefinite(g.maxSize[axis]) ? rawOf(g.maxSize[axis]) - totalOf(g, axis) : kInfinityF64;
    if (free > 0) {
        Array<uint32_t>& all = g.scratch().order;
        uint32_t mark = all.count;
        for (uint32_t t = 0; t < count; ++t)
            all.push(t);
        for (uint32_t t = 0; t < count; ++t)
            g.track(axis, t).incurred = 0;
        if (free == kInfinityF64) {
            for (uint32_t t = 0; t < count; ++t)
                g.track(axis, t).base = g.track(axis, t).limit;
        } else {
            share(g, axis, all.data + mark, count, free,
                [](const GridTrack& track) { return track.limit - track.base - track.incurred; },
                [](const GridTrack&) { return 1.0; });
            for (uint32_t t = 0; t < count; ++t)
                g.track(axis, t).base += g.track(axis, t).incurred;
        }
        all.count = mark;
    }
    expandFlexibleTracks(g, axis, space, available);

    // Stretches the auto tracks over the free space that is left, or over what the container's minimum leaves if the
    // space is indefinite (css-grid-1/#algo-stretch).
    PrLayoutAlignment content = PrLayoutAlignment(g.axes[axis].content & kPrLayoutAlignmentValueMask);
    if (content != PrLayoutAlignment_Normal && content != PrLayoutAlignment_Stretch)
        return;
    double stretchSpace = space == LayoutGridSpace_Definite ? available : rawOf(g.minSize[axis]);
    if (space == LayoutGridSpace_MinContent)
        return;
    double left = stretchSpace - totalOf(g, axis);
    if (left <= 0)
        return;
    uint32_t autos = 0;
    for (uint32_t t = 0; t < count; ++t)
        autos += g.track(axis, t).max == LayoutGridSizing_Auto && !g.track(axis, t).collapsed ? 1 : 0;
    for (uint32_t t = 0; t < count && autos != 0; ++t) {
        GridTrack& track = g.track(axis, t);
        if (track.max == LayoutGridSizing_Auto && !track.collapsed)
            track.base += left / double(autos);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Alignment

// Returns true if a track of `axis` other than a collapsed one follows the track `t`, with none of the tracks between
// them collapsed, so that the space that distributed alignment adds goes between them (css-grid-1/#collapsed-gutter).
bool separates(const Grid& g, PrLayoutGridAxis axis, uint32_t t) {
    const GridAxis& a = g.axes[axis];
    if (t + 1 >= a.trackCount || g.track(axis, t).collapsed)
        return false;
    for (uint32_t u = t + 1; u < a.trackCount; ++u) {
        if (!g.track(axis, u).collapsed)
            return true;
    }
    return false;
}

// Places the tracks of `axis` in a content box of `size` along it, by justify-content or align-content
// (css-grid-1/#grid-align, css-align-3/#distribution-values). Sets each track's offset. Distributed values share the
// free space between the tracks that are not collapsed, and fall back to start or safe center where they cannot. Normal
// and stretch, whose auto tracks took the free space, act as start. `left` and `right` are the line-left and line-right
// sides along the inline axis, and start along the block axis. A safe alignment of tracks that overflow is start.
void alignTracks(Grid& g, PrLayoutGridAxis axis, double size) {
    uint32_t count = g.axes[axis].trackCount;
    double free = size - totalOf(g, axis);
    uint32_t visible = 0;
    for (uint32_t t = 0; t < count; ++t)
        visible += g.track(axis, t).collapsed ? 0 : 1;
    PrLayoutAlignment align = g.axes[axis].content;
    PrLayoutAlignment value = PrLayoutAlignment(align & kPrLayoutAlignmentValueMask);
    bool safe = (align & PrLayoutAlignmentFlag_Safe) != 0;
    if (value == PrLayoutAlignment_Left || value == PrLayoutAlignment_Right) {
        LayoutSide side;
        if (axis != PrLayoutGridAxis_Columns)
            side = LayoutSide_InlineStart;
        else if (value == PrLayoutAlignment_Left)
            side = layoutLineLeft(g.mode());
        else
            side = layoutLineRight(g.mode());
        value = side == LayoutSide_InlineStart ? PrLayoutAlignment_Start : PrLayoutAlignment_End;
    }
    double leading = 0;
    double between = 0;
    switch (value) {
        case PrLayoutAlignment_SpaceBetween:
            if (free > 0 && visible > 1)
                between = free / double(visible - 1);
            break;
        case PrLayoutAlignment_SpaceAround:
            if (free > 0 && visible > 0) {
                between = free / double(visible);
                leading = between / 2;
            } else {
                leading = maxF64(0, free / 2);
            }
            break;
        case PrLayoutAlignment_SpaceEvenly:
            if (free > 0 && visible > 0) {
                between = free / double(visible + 1);
                leading = between;
            } else {
                leading = maxF64(0, free / 2);
            }
            break;
        case PrLayoutAlignment_Center: leading = safe && free < 0 ? 0 : free / 2; break;
        case PrLayoutAlignment_End:
        case PrLayoutAlignment_FlexEnd: leading = safe && free < 0 ? 0 : free; break;
        default: break;
    }
    double cursor = leading;
    for (uint32_t t = 0; t < count; ++t) {
        GridTrack& track = g.track(axis, t);
        track.offset = cursor;
        cursor += track.base + gapAfter(g, axis, t) + (separates(g, axis, t) ? between : 0);
    }
    g.placed[axis] = true;
}

// Returns where an item aligned by `self`, or by the container's `items` if `self` is auto, sits along `axis` of its
// area, and sets `*safe` to whether the alignment is safe (css-align-3/#self-alignment). `stretches` says whether the
// item can be stretched along the axis, which its auto size and margins decide. Normal is stretch for an item that can
// be stretched and has no preferred aspect ratio, and start otherwise. `self-start` and `self-end` are the item's own
// sides along the axis, and `left` and `right` are the line-left and line-right sides along the inline axis and start
// along the block axis. Baseline alignment needs the item's block axis to run along `axis`, and otherwise falls back to
// safe self-start or safe self-end (css-align-3/#baseline-values).
LayoutGridPlace placeOf(const Grid& g, const LayoutGridItem& it, PrLayoutGridAxis axis, PrLayoutAlignment self,
        PrLayoutAlignment items, bool stretches, bool ratio, bool* safe) {
    PrLayoutAlignment align = (self & kPrLayoutAlignmentValueMask) == PrLayoutAlignment_Auto ? items : self;
    *safe = (align & PrLayoutAlignmentFlag_Safe) != 0;
    PrLayoutAlignment value = PrLayoutAlignment(align & kPrLayoutAlignmentValueMask);
    LayoutMode own = layoutOwnMode(g.state.tree, it.node);
    LayoutSide start = startSideOf(axis);
    // The item's own start side along the axis.
    LayoutSide ownStart =
        (axis == PrLayoutGridAxis_Columns) != it.orthogonal ? LayoutSide_InlineStart : LayoutSide_BlockStart;
    bool sameStart = layoutPhysicalSide(own, ownStart) == layoutPhysicalSide(g.mode(), start);
    bool baselines = (axis == PrLayoutGridAxis_Rows) != it.orthogonal;
    switch (value) {
        case PrLayoutAlignment_Auto:
        case PrLayoutAlignment_Normal:
            return stretches && !ratio ? LayoutGridPlace_Stretch : LayoutGridPlace_Start;
        case PrLayoutAlignment_Stretch: return stretches ? LayoutGridPlace_Stretch : LayoutGridPlace_Start;
        case PrLayoutAlignment_Center: return LayoutGridPlace_Center;
        case PrLayoutAlignment_End:
        case PrLayoutAlignment_FlexEnd: return LayoutGridPlace_End;
        case PrLayoutAlignment_SelfStart: return sameStart ? LayoutGridPlace_Start : LayoutGridPlace_End;
        case PrLayoutAlignment_SelfEnd: return sameStart ? LayoutGridPlace_End : LayoutGridPlace_Start;
        case PrLayoutAlignment_Left:
        case PrLayoutAlignment_Right: {
            if (axis != PrLayoutGridAxis_Columns)
                return LayoutGridPlace_Start;
            LayoutSide side = value == PrLayoutAlignment_Left ? layoutLineLeft(g.mode()) : layoutLineRight(g.mode());
            return side == LayoutSide_InlineStart ? LayoutGridPlace_Start : LayoutGridPlace_End;
        }
        case PrLayoutAlignment_Baseline:
            if (baselines)
                return LayoutGridPlace_Baseline;
            *safe = true;
            return sameStart ? LayoutGridPlace_Start : LayoutGridPlace_End;
        case PrLayoutAlignment_LastBaseline:
            if (baselines)
                return LayoutGridPlace_LastBaseline;
            *safe = true;
            return sameStart ? LayoutGridPlace_End : LayoutGridPlace_Start;
        default: return LayoutGridPlace_Start;
    }
}

// Returns true if the item's size along `axis` depends on the size of an intrinsically sized track it spans, so that it
// takes no part in baseline alignment there (css-grid-1/#row-align). A flexible track counts as intrinsically sized if
// the container's size along the axis is indefinite.
bool baselineIsCyclic(const Grid& g, const LayoutGridItem& it, PrLayoutGridAxis axis) {
    AxisSizes sizes = axisSizesOf(layoutLogicalBox(*it.box, g.mode()), axis);
    auto reads = [](PrLayoutLength length) {
        return layoutLengthIsStretch(length) || (layoutLengthIsFixed(length) && layoutLengthReadsBase(length));
    };
    if (!reads(sizes.size) && !reads(sizes.min) && !reads(sizes.max))
        return false;
    for (int32_t t = it.start[axis]; t < it.end[axis]; ++t) {
        const GridTrack& track = g.track(axis, uint32_t(t));
        if (isIntrinsic(track.min) || isIntrinsic(track.max) ||
                (track.max == LayoutGridSizing_Flex && !layoutIsDefinite(g.size[axis])))
            return true;
    }
    return false;
}

// Resolves where each item sits along each axis of its area (placeOf), once the item is placed and its spanned tracks
// are known. An auto margin along an axis places the item there instead of its alignment.
void resolvePlaces(Grid& g) {
    for (uint32_t i = 0; i < g.count; ++i) {
        LayoutGridItem& it = g.item(i);
        LayoutLogicalBox logical = layoutLogicalBox(*it.box, g.mode());
        bool replaced = it.replaced;
        bool ratio = replaced || it.box->aspectRatio > 0;
        for (PrLayoutGridAxis axis = 0; axis < PrLayoutGridAxis_Count; ++axis) {
            AxisSizes sizes = axisSizesOf(logical, axis);
            bool autoMargins = layoutLengthIsAuto(logical.margin[startSideOf(axis)]) ||
                layoutLengthIsAuto(logical.margin[endSideOf(axis)]);
            it.autoMargins[axis] = autoMargins;
            bool stretches = layoutLengthIsAuto(sizes.size) && !autoMargins;
            PrLayoutAlignment self = axis == PrLayoutGridAxis_Columns ? it.flex->justifySelf : it.flex->alignSelf;
            PrLayoutAlignment items = axis == PrLayoutGridAxis_Columns ? g.flex.justifyItems : g.flex.alignItems;
            it.safe[axis] = false;
            if (autoMargins)
                it.place[axis] = LayoutGridPlace_Start;
            else
                it.place[axis] = placeOf(g, it, axis, self, items, stretches, ratio, &it.safe[axis]);
            if ((it.place[axis] == LayoutGridPlace_Baseline || it.place[axis] == LayoutGridPlace_LastBaseline) &&
                    baselineIsCyclic(g, it, axis)) {
                bool last = it.place[axis] == LayoutGridPlace_LastBaseline;
                it.place[axis] = last ? LayoutGridPlace_End : LayoutGridPlace_Start;
                it.safe[axis] = true;
            }
        }
    }
}

// Returns the offset of an item's border box from the start of its area along an axis: `free` is the area's size less
// the item's border box and margins `start` and `end`. Positive free space goes to auto margins, which `autoStart` and
// `autoEnd` say there are. Otherwise the item's place decides, and a safe alignment of an item that overflows its area
// is start (css-grid-1/#auto-margins).
PrLayoutUnit offsetInArea(LayoutGridPlace place, bool safe, PrLayoutUnit free, PrLayoutUnit start, bool autoStart, bool autoEnd) {
    if (autoStart || autoEnd) {
        if (free <= kLayoutUnitZero)
            return start;
        if (autoStart && autoEnd)
            return start + free / 2;
        return autoStart ? start + free : start;
    }
    if (safe && free < kLayoutUnitZero)
        return start;
    switch (place) {
        case LayoutGridPlace_End: return start + free;
        case LayoutGridPlace_Center: return start + free / 2;
        default: return start;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Layout

// The scratch memory of grid layout, made the first time a layout reaches a grid container.
LayoutGridScratch& scratchOf(LayoutState& state) {
    if (state.scratch->grid == nullptr)
        state.scratch->grid = layoutGridScratchCreate();
    return *state.scratch->grid;
}

// Collects the items of the grid container `node` in order-modified document order
// (css-display-4/#order-modified-document-order), and counts its children out of flow.
uint32_t collectItems(Grid& g) {
    LayoutTree* tree = g.state.tree;
    Array<LayoutGridItem>& items = g.scratch().items;
    g.mark = items.count;
    uint32_t outOfFlow = 0;
    auto add = [&](LayoutNodeIndex c, const PrLayoutBoxStyle& box, const PrLayoutFlexStyle& flex,
            const PrLayoutGridStyle* PR_NULLABLE grid) {
        LayoutGridItem it{};
        it.node = c;
        it.box = &box;
        it.flex = &flex;
        it.grid = grid;
        it.order = flex.order;
        it.orthogonal = layoutIsOrthogonalRoot(tree, c, g.mode());
        it.replaced = layoutTreeNode(tree, c).kind == PrLayoutNodeKind_Replaced;
        it.anonymous = grid == nullptr;
        items.push(it);
        ++g.count;
    };
    layoutEachItem(tree, g.node, [&](uint32_t c, const PrLayoutBoxStyle& box) {
        if (layoutIsOutOfFlow(tree, c)) {
            ++outOfFlow;
            return;
        }
        PrLayoutStyleIndex style = layoutTreeNode(tree, c).style;
        add(c, box, layoutFlexStyle(g.state.styles, style), &layoutGridStyle(g.state.styles, style));
    },
        // A run of text is an anonymous item (css-grid-1/#grid-items), which is always placed automatically.
        [&](uint32_t first, uint32_t) { add(first, layoutAnonymousBoxStyle(), layoutAnonymousFlexStyle(), nullptr); });
    // A stable sort by order, which leaves the items of most containers unchanged.
    for (uint32_t i = 1; i < g.count; ++i) {
        for (uint32_t k = i; k > 0 && g.item(k - 1).order > g.item(k).order; --k) {
            LayoutGridItem moved = g.item(k);
            g.item(k) = g.item(k - 1);
            g.item(k - 1) = moved;
        }
    }
    return outOfFlow;
}

// Sets up the grid of the container `node` with its content box in `content` and its sizes from `input`: its explicit
// grid, its items, placed, and its implicit grid with its tracks. Returns the number of its children out of flow.
uint32_t setUpGrid(Grid& g) {
    LayoutStyles* styles = g.state.styles;
    const LayoutGridTemplates* templates = &styles->grids;
    const LayoutGridAreasEntry& areas = layoutGridAreasAt(templates, g.style.templateAreas);
    const PrLayoutGridTracksIndex lists[PrLayoutGridAxis_Count] = {g.style.templateColumns, g.style.templateRows};
    const PrLayoutGridTracksIndex autos[PrLayoutGridAxis_Count] = {g.style.autoColumns, g.style.autoRows};
    const PrLayoutLength gaps[PrLayoutGridAxis_Count] = {g.flex.columnGap, g.flex.rowGap};
    const PrLayoutAlignment contents[PrLayoutGridAxis_Count] = {g.flex.justifyContent, g.flex.alignContent};
    for (PrLayoutGridAxis axis = 0; axis < PrLayoutGridAxis_Count; ++axis) {
        GridAxis& a = g.axes[axis];
        a = GridAxis{};
        a.tracks = &layoutGridTracksAt(templates, lists[axis]);
        a.autos = &layoutGridTracksAt(templates, autos[axis]);
        a.gap = layoutGapResolve(styles, gaps[axis], g.size[axis]);
        a.gapReadsSize = layoutLengthIsFixed(gaps[axis]) && layoutLengthReadsBase(gaps[axis]);
        a.content = contents[axis];
        if (a.tracks->repeat != PrLayoutGridRepeat_None) {
            a.repetitions = repetitionsOf(styles, *a.tracks, g.size[axis], g.minSize[axis], g.maxSize[axis], a.gap);
            a.collapses = a.tracks->repeat == PrLayoutGridRepeat_AutoFit;
        }
        a.templateCount = a.tracks->trackCount + (a.repetitions != 0 ? a.tracks->repeatCount * (a.repetitions - 1) : 0);
        uint32_t fromAreas = axis == PrLayoutGridAxis_Columns ? areas.columnCount : areas.rowCount;
        a.explicitCount = int32_t(a.templateCount > fromAreas ? a.templateCount : fromAreas);
        if (a.explicitCount > kPrLayoutGridMaxLine)
            a.explicitCount = kPrLayoutGridMaxLine;
        collectNames(g, a, areas, axis == PrLayoutGridAxis_Rows);
    }
    uint32_t outOfFlow = collectItems(g);
    placeItems(g);
    findImplicitGrid(g);
    for (PrLayoutGridAxis axis = 0; axis < PrLayoutGridAxis_Count; ++axis)
        setUpTracks(g, axis, g.size[axis]);
    resolvePlaces(g);
    return outOfFlow;
}

// Frees the scratch memory that the grid set up.
void tearDownGrid(Grid& g) {
    LayoutGridScratch& s = g.scratch();
    s.items.count = g.mark;
    s.tracks.count = g.axes[PrLayoutGridAxis_Columns].trackMark;
    s.names.count = g.axes[PrLayoutGridAxis_Columns].nameMark;
}

// Returns the grid of `node` for a layout with `content` as its content box.
Grid gridOf(LayoutState& state, LayoutNodeIndex node, const LayoutSpace& content) {
    PrLayoutStyleIndex style = layoutTreeNode(state.tree, node).style;
    scratchOf(state);
    return Grid{
        .state = state,
        .node = node,
        .style = layoutGridStyle(state.styles, style),
        .flex = layoutFlexStyle(state.styles, style),
        .content = content,
        .size = {content.containing.inlineSize, content.percentBlock},
        .minSize = {kLayoutUnitZero, kLayoutUnitZero},
        .maxSize = {layoutUnit(-1), layoutUnit(-1)},
        .axes = {},
        .placed = {false, false},
        .mark = 0,
        .count = 0,
    };
}

// Sizes the rows of the grid, whose columns are sized and placed: under the container's definite block size, or
// otherwise under a max-content constraint, and then again with that size if a percentage of the rows or of the row gap
// reads it (css-grid-1/#track-sizes) or a row is flexible. Returns the size of the content box along the block axis.
PrLayoutUnit sizeRows(Grid& g) {
    PrLayoutGridAxis rows = PrLayoutGridAxis_Rows;
    if (layoutIsDefinite(g.size[rows])) {
        sizeTracks(g, rows, LayoutGridSpace_Definite, rawOf(g.size[rows]));
        return g.size[rows];
    }
    sizeTracks(g, rows, LayoutGridSpace_MaxContent, 0);
    PrLayoutUnit size = layoutUnitMax(unitOf(totalOf(g, rows)), g.minSize[rows]);
    if (layoutIsDefinite(g.maxSize[rows]))
        size = layoutUnitMax(layoutUnitMin(size, g.maxSize[rows]), g.minSize[rows]);
    // A flexible row takes its share of the free space that the size leaves, which can differ from its share of the
    // max-content constraint: a fraction of less than 1 in all leaves part of that space empty.
    bool again = g.axes[rows].gapReadsSize;
    const GridAxis& a = g.axes[rows];
    for (uint32_t t = 0; t < a.trackCount && !again; ++t) {
        const PrLayoutGridTrack& sizes = trackSizes(a, int32_t(t) - a.explicitStart);
        again = layoutLengthReadsBase(PrLayoutLength{sizes.min.bits}) ||
            layoutLengthReadsBase(PrLayoutLength{sizes.max.bits}) || g.track(rows, t).max == LayoutGridSizing_Flex;
    }
    if (again) {
        g.size[rows] = size;
        g.axes[rows].gap = layoutGapResolve(g.state.styles, g.flex.rowGap, size);
        Array<GridTrack>& tracks = g.scratch().tracks;
        // The tracks are set up again with their percentages resolved.
        uint32_t columnsEnd = a.trackMark;
        uint32_t saved = tracks.count;
        setUpTracks(g, rows, size);
        // setUpTracks appended the rows anew past the old ones, which move down over them.
        for (uint32_t t = 0; t < a.trackCount; ++t)
            tracks.data[columnsEnd + t] = tracks.data[saved + t];
        tracks.count = saved;
        g.axes[rows].trackMark = columnsEnd;
        sizeTracks(g, rows, LayoutGridSpace_Definite, rawOf(size));
        g.size[rows] = layoutUnit(-1);
    }
    return size;
}

// Returns the baseline of the item `i` along the block axis, from the block-start edge of its border box: its first or
// last baseline if it has one in the container's block axis, or one synthesized from its border box's block-end edge
// (css-align-3/#synthesize-baseline).
LayoutBaseline itemBaseline(const LayoutGridItem& it, LayoutBaselineEdge edge) {
    const LayoutBaseline& b = edge == LayoutBaselineEdge_First ? it.first : it.last;
    if (b.present && !it.orthogonal)
        return b;
    return layoutBaselineAt(it.size[PrLayoutGridAxis_Rows], kLayoutBaselineSynthesized);
}

// Returns the index of the item whose baseline is the container's first or last baseline (css-grid-1/#grid-baselines),
// or `g.count` if it has none. The baseline comes from the first or last row that holds an item: from its items aligned
// by baseline, or else from its first item in grid order.
uint32_t baselineItemOf(const Grid& g, LayoutBaselineEdge edge) {
    bool first = edge == LayoutBaselineEdge_First;
    uint32_t rows = g.axes[PrLayoutGridAxis_Rows].trackCount;
    // The row: the first that an item starts in, or the last that one ends in.
    int32_t row = first ? int32_t(rows) : -1;
    for (uint32_t i = 0; i < g.count; ++i) {
        const LayoutGridItem& it = g.item(i);
        if (first)
            row = (it.start[PrLayoutGridAxis_Rows] < row ? it.start[PrLayoutGridAxis_Rows] : row);
        else
            row = (it.end[PrLayoutGridAxis_Rows] - 1 > row ? it.end[PrLayoutGridAxis_Rows] - 1 : row);
    }
    if (g.count == 0)
        return g.count;
    auto inRow = [&](const LayoutGridItem& it, bool fromStart) {
        return fromStart ? it.start[PrLayoutGridAxis_Rows] == row : it.end[PrLayoutGridAxis_Rows] - 1 == row;
    };
    LayoutGridPlace same = first ? LayoutGridPlace_Baseline : LayoutGridPlace_LastBaseline;
    LayoutGridPlace other = first ? LayoutGridPlace_LastBaseline : LayoutGridPlace_Baseline;
    for (uint32_t i = 0; i < g.count; ++i) {
        if (inRow(g.item(i), first) && g.item(i).place[PrLayoutGridAxis_Rows] == same)
            return i;
    }
    for (uint32_t i = 0; i < g.count; ++i) {
        if (inRow(g.item(i), !first) && g.item(i).place[PrLayoutGridAxis_Rows] == other)
            return i;
    }
    // The first item in grid order: by row, then by column, then in order-modified document order. The last baseline
    // takes the last.
    uint32_t chosen = g.count;
    for (uint32_t i = 0; i < g.count; ++i) {
        const LayoutGridItem& it = g.item(i);
        if (!inRow(it, first))
            continue;
        if (chosen == g.count) {
            chosen = i;
            continue;
        }
        const LayoutGridItem& was = g.item(chosen);
        bool before = it.start[PrLayoutGridAxis_Columns] < was.start[PrLayoutGridAxis_Columns];
        bool after = it.start[PrLayoutGridAxis_Columns] > was.start[PrLayoutGridAxis_Columns];
        if (first ? before : (after || !before))
            chosen = i;
    }
    return chosen;
}

// Finds the edges along an axis of the grid area of a box out of flow whose containing block is the grid container,
// from the box's grid-placement properties along it, `startLine` and `endLine` (css-grid-1/#abspos-items). `lines`
// names the grid's lines, `explicitStart` is the index of the explicit grid's first line among them, and `position`
// gives a line's offset by its index. An auto line, a placement that is only a span, and a line that the implicit grid
// does not have are the padding edges, `*start` and `*end` as given.
template <typename Position>
void absoluteEdges(const LayoutGridTemplates* templates, const AxisLines& lines, int32_t explicitStart,
        uint32_t trackCount, const PrLayoutGridLine& startLine, const PrLayoutGridLine& endLine,
        Position position, PrLayoutUnit* start, PrLayoutUnit* end) {
    bool startIsLine = startLine.kind == PrLayoutGridLineKind_Line;
    bool endIsLine = endLine.kind == PrLayoutGridLineKind_Line;
    if (!startIsLine && !endIsLine)
        return;
    AxisPlacement p = placementOf(templates, lines, startLine, endLine);
    int32_t first = p.start + explicitStart;
    int32_t last = p.end + explicitStart;
    int32_t lineCount = int32_t(trackCount) + 1;
    bool startAuto = startLine.kind == PrLayoutGridLineKind_Auto;
    bool endAuto = endLine.kind == PrLayoutGridLineKind_Auto;
    if (!startAuto && first >= 0 && first < lineCount)
        *start = position(uint32_t(first));
    if (!endAuto && last >= 0 && last < lineCount)
        *end = position(uint32_t(last));
}

// Appends a placeholder for each child out of flow of the container, at its static position: where the container's sole
// item would be in its static-position rectangle, aligned there by its own justify-self and align-self, with stretch
// and baseline alignment acting as start (css-grid-1/#static-position). The rectangle is the content box, or the grid
// area of a child whose containing block is the container (css-position-3/#staticpos-rect). The content box is
// `inlineSize` by `blockSize` from `origin`, and the container's padding is `padding`, by LayoutSide.
void placeOutOfFlow(Grid& g, LayoutLogicalOffset origin, PrLayoutUnit inlineSize, PrLayoutUnit blockSize,
        const PrLayoutUnit* padding) {
    LayoutTree* tree = g.state.tree;
    const LayoutGridTemplates* templates = &g.state.styles->grids;
    layoutEachItem(tree, g.node, [&](uint32_t c, const PrLayoutBoxStyle& box) {
        if (!layoutIsOutOfFlow(tree, c))
            return;
            // The rectangle, from the start corner of the content box.
        PrLayoutUnit start[PrLayoutGridAxis_Count] = {kLayoutUnitZero, kLayoutUnitZero};
        PrLayoutUnit end[PrLayoutGridAxis_Count] = {inlineSize, blockSize};
        if (layoutHoldsPosition(tree, g.node, box.position)) {
            const PrLayoutGridStyle& placement = layoutGridStyle(g.state.styles, layoutTreeNode(tree, c).style);
            const PrLayoutGridLine starts[PrLayoutGridAxis_Count] = {placement.columnStart, placement.rowStart};
            const PrLayoutGridLine ends[PrLayoutGridAxis_Count] = {placement.columnEnd, placement.rowEnd};
            for (PrLayoutGridAxis axis = 0; axis < PrLayoutGridAxis_Count; ++axis) {
                const GridAxis& a = g.axes[axis];
                start[axis] = kLayoutUnitZero - padding[startSideOf(axis)];
                end[axis] = end[axis] + padding[endSideOf(axis)];
                auto position = [&](uint32_t line) {
                    if (line < a.trackCount)
                        return unitOf(trackStart(g, axis, int32_t(line)));
                    if (a.trackCount != 0)
                        return unitOf(trackEnd(g, axis, int32_t(a.trackCount) - 1));
                    return kLayoutUnitZero;
                };
                absoluteEdges(templates, linesOf(g, axis), a.explicitStart, a.trackCount, starts[axis],
                    ends[axis], position, &start[axis], &end[axis]);
            }
        }
        LayoutGridItem it{};
        it.node = c;
        it.box = &box;
        it.orthogonal = layoutIsOrthogonalRoot(tree, c, g.mode());
        const PrLayoutFlexStyle& flex = layoutFlexStyle(g.state.styles, layoutTreeNode(tree, c).style);
        LayoutStaticAlign aligns[PrLayoutGridAxis_Count] = {};
        bool safe[PrLayoutGridAxis_Count] = {};
        for (PrLayoutGridAxis axis = 0; axis < PrLayoutGridAxis_Count; ++axis) {
            PrLayoutAlignment self = axis == PrLayoutGridAxis_Columns ? flex.justifySelf : flex.alignSelf;
            PrLayoutAlignment items = axis == PrLayoutGridAxis_Columns ? g.flex.justifyItems : g.flex.alignItems;
            LayoutGridPlace place = placeOf(g, it, axis, self, items, false, false, &safe[axis]);
            if (place == LayoutGridPlace_Center)
                aligns[axis] = LayoutStaticAlign_Center;
            else if (place == LayoutGridPlace_End || place == LayoutGridPlace_LastBaseline)
                aligns[axis] = LayoutStaticAlign_End;
            else
                aligns[axis] = LayoutStaticAlign_Start;
        }
        auto at = [](LayoutStaticAlign align, PrLayoutUnit size) {
            if (align == LayoutStaticAlign_Start)
                return kLayoutUnitZero;
            if (align == LayoutStaticAlign_End)
                return size;
            return layoutUnit(size.raw / 2);
        };
        PrLayoutUnit sizes[PrLayoutGridAxis_Count];
        for (PrLayoutGridAxis axis = 0; axis < PrLayoutGridAxis_Count; ++axis)
            sizes[axis] = layoutUnitMax(kLayoutUnitZero, end[axis] - start[axis]);
        LayoutLogicalOffset offset{
            start[PrLayoutGridAxis_Columns] + at(aligns[PrLayoutGridAxis_Columns], sizes[PrLayoutGridAxis_Columns]),
            start[PrLayoutGridAxis_Rows] + at(aligns[PrLayoutGridAxis_Rows], sizes[PrLayoutGridAxis_Rows]),
        };
        LayoutStaticEdges edges =
            layoutStaticEdgesAligned(g.mode(), aligns[PrLayoutGridAxis_Columns], aligns[PrLayoutGridAxis_Rows],
                safe[PrLayoutGridAxis_Columns], safe[PrLayoutGridAxis_Rows]);
        LayoutFragment* placeholder = layoutFragmentCreatePlaceholder(c, edges, layoutIsFixed(tree, c));
        g.state.scratch->children.push(LayoutLogicalChild{placeholder, origin + offset});
    }, [](uint32_t, uint32_t) {});
}

// Keeps the lines of the grid for the boxes out of flow that the container holds, and sets `lines` to them.
void keepLines(Grid& g, LayoutLogicalOffset origin, LayoutGridLines* lines) {
    LayoutGridScratch& s = g.scratch();
    LayoutGridKept kept{};
    kept.mode = g.mode();
    kept.origin = origin;
    kept.positions = s.keptPositions.count;
    kept.sizes = s.keptSizes.count;
    kept.names = s.keptNames.count;
    for (PrLayoutGridAxis axis = 0; axis < PrLayoutGridAxis_Count; ++axis) {
        const GridAxis& a = g.axes[axis];
        kept.trackCount[axis] = a.trackCount;
        kept.explicitStart[axis] = a.explicitStart;
        kept.explicitCount[axis] = a.explicitCount;
        kept.repetitions[axis] = a.repetitions;
        for (uint32_t t = 0; t < a.trackCount; ++t) {
            s.keptPositions.push(unitOf(trackStart(g, axis, int32_t(t))));
            s.keptSizes.push(unitOf(trackEnd(g, axis, int32_t(t))) - unitOf(trackStart(g, axis, int32_t(t))));
        }
        if (a.trackCount != 0)
            s.keptPositions.push(unitOf(trackEnd(g, axis, int32_t(a.trackCount) - 1)));
        else
            s.keptPositions.push(kLayoutUnitZero);
        kept.nameCount[axis] = a.nameCount;
        s.keptNames.append(s.names.data + a.nameMark, a.nameCount);
    }
    *lines = LayoutGridLines{g.node, s.kept.count};
    s.kept.push(kept);
}

} // namespace

LayoutGridScratch* layoutGridScratchCreate(void) {
    return allocZeroed<LayoutGridScratch>();
}

void layoutGridScratchDestroy(LayoutGridScratch* PR_NULLABLE scratch) {
    if (scratch == nullptr)
        return;
    scratch->items.reset();
    scratch->tracks.reset();
    scratch->names.reset();
    scratch->occupied.reset();
    scratch->order.reset();
    scratch->sizes.reset();
    scratch->kept.reset();
    scratch->keptPositions.reset();
    scratch->keptSizes.reset();
    scratch->keptNames.reset();
    prMemoryFree(scratch);
}

LayoutContentOutcome layoutGridContent(LayoutState& state, LayoutNodeIndex node, const LayoutContentInput& input,
        LayoutGridLines* PR_NULLABLE lines) {
    bool builds = input.goal == LayoutContentGoal_Build;
    Grid g = gridOf(state, node, *input.content);
    g.size[PrLayoutGridAxis_Rows] = input.blockSize;
    g.minSize[PrLayoutGridAxis_Rows] = input.minBlock;
    g.maxSize[PrLayoutGridAxis_Rows] = input.maxBlock;
    uint32_t outOfFlow = setUpGrid(g);
    PrLayoutUnit inlineSize = g.size[PrLayoutGridAxis_Columns];

    // The grid sizing algorithm (css-grid-1/#algo-grid-sizing): the columns, then the rows at the columns' sizes, with
    // the columns placed by justify-content in the content box's definite inline size.
    sizeTracks(g, PrLayoutGridAxis_Columns, LayoutGridSpace_Definite, rawOf(inlineSize));
    alignTracks(g, PrLayoutGridAxis_Columns, rawOf(inlineSize));
    PrLayoutUnit blockSize = sizeRows(g);
    alignTracks(g, PrLayoutGridAxis_Rows, rawOf(blockSize));
    // Orthogonal items contribute to the columns through the rows' sizes, so the columns are sized again with the rows
    // known, and then the rows once more with the new columns (css-grid-1/#algo-grid-sizing).
    bool orthogonal = false;
    for (uint32_t i = 0; i < g.count && !orthogonal; ++i)
        orthogonal = g.item(i).orthogonal;
    if (orthogonal) {
        sizeTracks(g, PrLayoutGridAxis_Columns, LayoutGridSpace_Definite, rawOf(inlineSize));
        alignTracks(g, PrLayoutGridAxis_Columns, rawOf(inlineSize));
        blockSize = sizeRows(g);
        alignTracks(g, PrLayoutGridAxis_Rows, rawOf(blockSize));
    }

    // Lays out each item in its area at its final request, or measures it for a solve, and aligns it there.
    Array<LayoutLogicalChild>& children = state.scratch->children;
    LayoutContentOutcome outcome{};
    outcome.contentBlock = layoutIsDefinite(input.blockSize) ? input.blockSize : blockSize;
    uint32_t built = 0;
    for (uint32_t i = 0; i < g.count; ++i) {
        LayoutGridItem copy = g.item(i);
        PrLayoutUnit columns = areaOf(g, copy, PrLayoutGridAxis_Columns);
        PrLayoutUnit rows = areaOf(g, copy, PrLayoutGridAxis_Rows);
        LayoutLogicalBox logical = layoutLogicalBox(*copy.box, g.mode());
        LayoutEdges e = layoutEdgesFromBox(state.styles, logical, columns);
        LayoutRequests requests = requestsOf(g, copy, columns, rows, e);
        LayoutSpace space = areaSpace(g, columns, rows);
        const LayoutFragment* fragment = nullptr;
        if (builds) {
            LayoutResult result = layoutFinal(state, copy.node, space, requests);
            fragment = result.fragment;
            LayoutLogicalSize size = layoutLogicalSize(g.mode(), fragment->extent);
            copy.size[PrLayoutGridAxis_Columns] = size.inlineSize;
            copy.size[PrLayoutGridAxis_Rows] = size.blockSize;
            copy.first = layoutFragmentBaseline(fragment, LayoutBaselineEdge_First);
            copy.last = layoutFragmentBaseline(fragment, LayoutBaselineEdge_Last);
        } else {
            LayoutMeasurement m = layoutMeasure(state, copy.node, space, requests,
                input.goal == LayoutContentGoal_Baselines ? LayoutMeasureKind_Baselines : LayoutMeasureKind_Size);
            bool inlineIsColumns = !copy.orthogonal;
            copy.size[inlineIsColumns ? PrLayoutGridAxis_Columns : PrLayoutGridAxis_Rows] = requests.inlineAxis.size;
            copy.size[inlineIsColumns ? PrLayoutGridAxis_Rows : PrLayoutGridAxis_Columns] = m.blockSize;
            copy.first = m.first;
            copy.last = m.last;
        }
        for (LayoutSide side = 0; side < LayoutSide_Count; ++side)
            copy.margin[side] = e.margin[side];
        for (PrLayoutGridAxis axis = 0; axis < PrLayoutGridAxis_Count; ++axis) {
            PrLayoutUnit area = axis == PrLayoutGridAxis_Columns ? columns : rows;
            PrLayoutUnit start = copy.margin[startSideOf(axis)];
            PrLayoutUnit free = area - copy.size[axis] - start - copy.margin[endSideOf(axis)];
            bool autoStart = copy.autoMargins[axis] && layoutLengthIsAuto(logical.margin[startSideOf(axis)]);
            bool autoEnd = copy.autoMargins[axis] && layoutLengthIsAuto(logical.margin[endSideOf(axis)]);
            copy.offset[axis] = unitOf(trackStart(g, axis, copy.start[axis])) +
                offsetInArea(copy.place[axis], copy.safe[axis], free, start, autoStart, autoEnd);
        }
        g.item(i) = copy;
        if (fragment != nullptr) {
            // The fragment is pushed once the item's baseline position is known, below.
            children.push(LayoutLogicalChild{fragment, LayoutLogicalOffset{}});
        }
    }

    // Aligns the items aligned by their baseline in each row's baseline-sharing groups: those aligned by their first
    // baseline at the row where their span starts, and those aligned by their last baseline at the row where it ends.
    uint32_t rowCount = g.axes[PrLayoutGridAxis_Rows].trackCount;
    Array<double>& extents = g.scratch().sizes;
    uint32_t extentMark = extents.count;
    extents.resize(extentMark + 2 * rowCount);
    for (uint32_t k = extentMark; k < extents.count; ++k)
        extents.data[k] = 0;
    bool anyBaseline = false;
    for (uint32_t i = 0; i < g.count; ++i) {
        const LayoutGridItem& it = g.item(i);
        LayoutGridPlace place = it.place[PrLayoutGridAxis_Rows];
        if (place != LayoutGridPlace_Baseline && place != LayoutGridPlace_LastBaseline)
            continue;
        anyBaseline = true;
        bool first = place == LayoutGridPlace_Baseline;
        PrLayoutUnit baseline = itemBaseline(it, first ? LayoutBaselineEdge_First : LayoutBaselineEdge_Last).offset;
        double before = rawOf(it.margin[LayoutSide_BlockStart] + baseline);
        double after = rawOf(it.size[PrLayoutGridAxis_Rows] - baseline + it.margin[LayoutSide_BlockEnd]);
        uint32_t slot;
        if (first)
            slot = uint32_t(it.start[PrLayoutGridAxis_Rows]);
        else
            slot = rowCount + uint32_t(it.end[PrLayoutGridAxis_Rows] - 1);
        extents.data[extentMark + slot] = maxF64(extents.data[extentMark + slot], first ? before : after);
    }
    if (anyBaseline) {
        for (uint32_t i = 0; i < g.count; ++i) {
            LayoutGridItem& it = g.item(i);
            LayoutGridPlace place = it.place[PrLayoutGridAxis_Rows];
            if (place != LayoutGridPlace_Baseline && place != LayoutGridPlace_LastBaseline)
                continue;
            bool first = place == LayoutGridPlace_Baseline;
            PrLayoutUnit baseline = itemBaseline(it, first ? LayoutBaselineEdge_First : LayoutBaselineEdge_Last).offset;
            if (first) {
                double shared = extents.data[extentMark + uint32_t(it.start[PrLayoutGridAxis_Rows])];
                double start = trackStart(g, PrLayoutGridAxis_Rows, it.start[PrLayoutGridAxis_Rows]);
                it.offset[PrLayoutGridAxis_Rows] = unitOf(start + shared - rawOf(baseline));
            } else {
                double shared = extents.data[extentMark + rowCount + uint32_t(it.end[PrLayoutGridAxis_Rows] - 1)];
                double end = trackEnd(g, PrLayoutGridAxis_Rows, it.end[PrLayoutGridAxis_Rows] - 1);
                PrLayoutUnit size = it.size[PrLayoutGridAxis_Rows];
                it.offset[PrLayoutGridAxis_Rows] = unitOf(end - shared + rawOf(size - baseline)) - size;
            }
        }
    }
    extents.count = extentMark;

    // Places the fragments, each moved by its relative position, and gathers the bounds of the items' margin boxes and
    // of the grid, which a scroll container's scrollable overflow holds (css-grid-1/#overflow).
    LayoutLogicalOffset origin = input.origin;
    uint32_t childMark = children.count - (builds ? g.count : 0);
    for (uint32_t i = 0; i < g.count; ++i) {
        const LayoutGridItem& it = g.item(i);
        LayoutLogicalOffset at{it.offset[PrLayoutGridAxis_Columns], it.offset[PrLayoutGridAxis_Rows]};
        if (builds) {
            LayoutSpace space =
                areaSpace(g, areaOf(g, it, PrLayoutGridAxis_Columns), areaOf(g, it, PrLayoutGridAxis_Rows));
            LayoutLogicalOffset relative = layoutRelativeOffset(state.styles, *it.box, space);
            children.data[childMark + i].offset = origin + at + relative;
        }
        if (input.scrolls && builds) {
            LayoutLogicalOffset start{
                at.inlineOffset - it.margin[LayoutSide_InlineStart],
                at.blockOffset - it.margin[LayoutSide_BlockStart],
            };
            LayoutLogicalOffset end{
                at.inlineOffset + it.size[PrLayoutGridAxis_Columns] + it.margin[LayoutSide_InlineEnd],
                at.blockOffset + it.size[PrLayoutGridAxis_Rows] + it.margin[LayoutSide_BlockEnd],
            };
            if (built++ != 0) {
                start = {
                    layoutUnitMin(start.inlineOffset, outcome.itemsStart.inlineOffset),
                    layoutUnitMin(start.blockOffset, outcome.itemsStart.blockOffset),
                };
                end = {
                    layoutUnitMax(end.inlineOffset, outcome.itemsEnd.inlineOffset),
                    layoutUnitMax(end.blockOffset, outcome.itemsEnd.blockOffset),
                };
            }
            outcome.itemsStart = start;
            outcome.itemsEnd = end;
        }
    }
    if (input.scrolls && builds) {
        LayoutLogicalOffset gridStart{};
        if (g.axes[PrLayoutGridAxis_Columns].trackCount != 0)
            gridStart.inlineOffset = unitOf(trackStart(g, PrLayoutGridAxis_Columns, 0));
        if (g.axes[PrLayoutGridAxis_Rows].trackCount != 0)
            gridStart.blockOffset = unitOf(trackStart(g, PrLayoutGridAxis_Rows, 0));
        LayoutLogicalOffset gridEnd{
            gridStart.inlineOffset + unitOf(totalOf(g, PrLayoutGridAxis_Columns)),
            gridStart.blockOffset + unitOf(totalOf(g, PrLayoutGridAxis_Rows)),
        };
        if (built == 0) {
            outcome.itemsStart = gridStart;
            outcome.itemsEnd = gridEnd;
        } else {
            outcome.itemsStart = {
                layoutUnitMin(gridStart.inlineOffset, outcome.itemsStart.inlineOffset),
                layoutUnitMin(gridStart.blockOffset, outcome.itemsStart.blockOffset),
            };
            outcome.itemsEnd = {
                layoutUnitMax(gridEnd.inlineOffset, outcome.itemsEnd.inlineOffset),
                layoutUnitMax(gridEnd.blockOffset, outcome.itemsEnd.blockOffset),
            };
        }
    }

    // The container's baselines (css-grid-1/#grid-baselines), from the block-start edge of its content box.
    if (input.goal != LayoutContentGoal_Size) {
        uint32_t first = baselineItemOf(g, LayoutBaselineEdge_First);
        uint32_t last = baselineItemOf(g, LayoutBaselineEdge_Last);
        if (first != g.count)
            outcome.first = layoutBaselineMoved(itemBaseline(g.item(first), LayoutBaselineEdge_First),
                g.item(first).offset[PrLayoutGridAxis_Rows]);
        if (last != g.count)
            outcome.last = layoutBaselineMoved(itemBaseline(g.item(last), LayoutBaselineEdge_Last),
                g.item(last).offset[PrLayoutGridAxis_Rows]);
    }
    if (builds && outOfFlow != 0)
        placeOutOfFlow(g, origin, inlineSize, outcome.contentBlock, input.padding);
    if (builds && lines != nullptr)
        keepLines(g, origin, lines);
    tearDownGrid(g);
    ++(builds ? state.stats->gridsLaidOut : state.stats->gridsSolved);
    return outcome;
}

PrLayoutRect layoutGridAbsoluteArea(LayoutState& state, const LayoutGridLines& lines, LayoutNodeIndex box,
        PrLayoutRect padding, PrLayoutVec extent) {
    const LayoutGridScratch& s = *state.scratch->grid;
    const LayoutGridKept& kept = s.kept.data[lines.mark];
    const PrLayoutGridStyle& style = layoutGridStyle(state.styles, layoutTreeNode(state.tree, box).style);
    const LayoutGridTemplates* templates = &state.styles->grids;
    LayoutMode mode = kept.mode;
    LayoutLogicalSize size = layoutLogicalSize(mode, extent);
    // The padding box's edges along each axis, from the border box's start corner.
    PrLayoutUnit paddingStart[PrLayoutGridAxis_Count];
    PrLayoutUnit paddingEnd[PrLayoutGridAxis_Count];
    for (PrLayoutGridAxis axis = 0; axis < PrLayoutGridAxis_Count; ++axis) {
        LayoutSide sides[2] = {startSideOf(axis), endSideOf(axis)};
        PrLayoutUnit edges[2];
        for (uint32_t k = 0; k < 2; ++k) {
            switch (layoutPhysicalSide(mode, sides[k])) {
                case PrRectSide_Left: edges[k] = padding.left; break;
                case PrRectSide_Top: edges[k] = padding.top; break;
                case PrRectSide_Right: edges[k] = extent.x - padding.right; break;
                default: edges[k] = extent.y - padding.bottom; break;
            }
        }
        PrLayoutUnit axisSize = axis == PrLayoutGridAxis_Columns ? size.inlineSize : size.blockSize;
        paddingStart[axis] = edges[0];
        paddingEnd[axis] = axisSize - edges[1];
    }
    uint32_t positions = kept.positions;
    uint32_t names = kept.names;
    PrLayoutUnit start[PrLayoutGridAxis_Count];
    PrLayoutUnit end[PrLayoutGridAxis_Count];
    const PrLayoutGridLine starts[PrLayoutGridAxis_Count] = {style.columnStart, style.rowStart};
    const PrLayoutGridLine ends[PrLayoutGridAxis_Count] = {style.columnEnd, style.rowEnd};
    for (PrLayoutGridAxis axis = 0; axis < PrLayoutGridAxis_Count; ++axis) {
        AxisLines axisLines{s.keptNames.data + names, kept.nameCount[axis], kept.explicitCount[axis]};
        PrLayoutUnit origin = axis == PrLayoutGridAxis_Columns ? kept.origin.inlineOffset : kept.origin.blockOffset;
        start[axis] = paddingStart[axis];
        end[axis] = paddingEnd[axis];
        auto position = [&](uint32_t line) { return origin + s.keptPositions.data[positions + line]; };
        absoluteEdges(templates, axisLines, kept.explicitStart[axis], kept.trackCount[axis], starts[axis], ends[axis],
            position, &start[axis], &end[axis]);
        positions += kept.trackCount[axis] + 1;
        names += kept.nameCount[axis];
    }
    LayoutLogicalOffset at{start[PrLayoutGridAxis_Columns], start[PrLayoutGridAxis_Rows]};
    LayoutLogicalSize area{
        layoutUnitMax(kLayoutUnitZero, end[PrLayoutGridAxis_Columns] - start[PrLayoutGridAxis_Columns]),
        layoutUnitMax(kLayoutUnitZero, end[PrLayoutGridAxis_Rows] - start[PrLayoutGridAxis_Rows]),
    };
    return layoutRectAt(layoutPhysicalOffset(mode, at, area, size), layoutPhysicalSize(mode, area));
}

LayoutFragment* layoutGridFinish(LayoutState& state, const LayoutGridLines& lines, LayoutFragment* built) {
    LayoutGridScratch& s = *state.scratch->grid;
    PR_ASSERT(lines.mark + 1 == s.kept.count);
    const LayoutGridKept& kept = s.kept.data[lines.mark];
    LayoutGridAxisTracks axes[PrLayoutGridAxis_Count];
    uint32_t sizes = kept.sizes;
    for (PrLayoutGridAxis axis = 0; axis < PrLayoutGridAxis_Count; ++axis) {
        axes[axis] = LayoutGridAxisTracks{
            kept.trackCount[axis], kept.explicitStart[axis], kept.repetitions[axis],
            s.keptSizes.data + sizes,
        };
        sizes += kept.trackCount[axis];
    }
    LayoutFragment* fragment = layoutFragmentGrid(built, axes);
    s.keptPositions.count = kept.positions;
    s.keptSizes.count = kept.sizes;
    s.keptNames.count = kept.names;
    s.kept.count = lines.mark;
    return fragment;
}

LayoutIntrinsicSizes layoutGridIntrinsic(LayoutState& state, LayoutNodeIndex node, const LayoutIntrinsicInput& input) {
    LayoutSpace content{
        .mode = input.mode,
        .available = layoutUnit(-1),
        .percentInline = layoutUnit(-1),
        .percentBlock = input.percentBlock,
        .containing = LayoutLogicalSize{layoutUnit(-1), input.percentBlock},
        .blockLimit = input.blockLimit,
        .fallback = input.fallback,
    };
    Grid g = gridOf(state, node, content);
    // The container's limits, if they are fixed, set the repetitions of an automatic repetition of its columns
    // (css-grid-1/#auto-repeat). Its preferred size is the size that its intrinsic sizes stand in for, so it does not:
    // a grid of a fixed width and no limits has the intrinsic sizes of one repetition.
    const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, layoutTreeNode(state.tree, node).style);
    LayoutLogicalBox logical = layoutLogicalBox(box, input.mode);
    PrLayoutUnit edges = layoutEdgesInline(layoutEdgesFromBox(state.styles, logical, layoutUnit(-1)));
    PrLayoutUnit most = layoutFixedInner(state.styles, box, logical.maxInlineSize, layoutUnit(-1), edges);
    PrLayoutUnit least = layoutFixedInner(state.styles, box, logical.minInlineSize, layoutUnit(-1), edges);
    g.maxSize[PrLayoutGridAxis_Columns] = most;
    g.minSize[PrLayoutGridAxis_Columns] = layoutIsDefinite(least) ? least : kLayoutUnitZero;
    setUpGrid(g);
    LayoutIntrinsicSizes sizes{};
    sizeTracks(g, PrLayoutGridAxis_Columns, LayoutGridSpace_MinContent, 0);
    sizes.minContent = unitOf(totalOf(g, PrLayoutGridAxis_Columns));
    sizeTracks(g, PrLayoutGridAxis_Columns, LayoutGridSpace_MaxContent, 0);
    sizes.maxContent = layoutUnitMax(unitOf(totalOf(g, PrLayoutGridAxis_Columns)), sizes.minContent);
    sizes.readsPercentBlock = layoutIsDefinite(input.percentBlock);
    tearDownGrid(g);
    return sizes;
}

} // namespace pr_impl
