// OpenType tables
//
// A reader over the bytes of a font file: the table directory of the face, and the tables that lib/font reads itself
// instead of through FreeType. Every read is bounds checked. A read past the end of a table returns zero.

#pragma once

#include "pr/base.h"
#include "pr/font.h"
#include "pr/raw_string.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The bytes of a table. Empty if the face has no such table.
struct SfntTable {
    const uint8_t* PR_NULLABLE data;
    uint32_t size;

    // Returns the big-endian value at byte `at`, or zero past the end.
    uint8_t uint8At(uint32_t at) const;
    uint16_t uint16At(uint32_t at) const;
    int16_t int16At(uint32_t at) const;
    uint32_t uint32At(uint32_t at) const;

    // Returns a Fixed (16.16) or F2DOT14 as a float.
    float fixed(uint32_t at) const;
    float f2dot14(uint32_t at) const;

    // Returns the part from `at` on, or an empty table if `at` is past the end.
    SfntTable from(uint32_t at) const;
};

// A face of a font file: the file, and the position of the table directory of the face.
struct Sfnt {
    const uint8_t* file;
    size_t size;
    uint32_t directory;
};

// Returns the number of faces in the file: the number of faces in a collection, one for a single font, and zero for
// bytes that are neither.
uint32_t sfntFaceCount(const uint8_t* file, size_t size);

// Opens the face `index` of the file, a collection or a single font. Returns false for bytes that are neither.
bool sfntOpen(const uint8_t* file, size_t size, uint32_t index, Sfnt* out);

// Returns the table `tag` of the face, or an empty table if the face has none.
SfntTable sfntTable(const Sfnt& sfnt, uint32_t tag);

// The normalized coordinates of a variation, with avar applied, one per font axis. A `count` of zero means the default
// instance.
struct SfntCoords {
    const float* PR_NULLABLE values;
    uint32_t count;
};

// Returns the delta that an ItemVariationStore gives for an outer and inner index at the coordinates.
float sfntVariationDelta(SfntTable store, uint16_t outer, uint16_t inner, SfntCoords coords);

// Gets the outer and inner indices that a DeltaSetIndexMap gives `index`. An index past the end of the map takes its
// last entry. Without a map (an empty table), the outer index is 0 and the inner index is `index`.
void sfntDeltaSetIndex(SfntTable map, uint32_t index, uint16_t* outer, uint16_t* inner);

// Writes to `out` the scalar at the coordinates of each region that the ItemVariationData `outer` of the store lists,
// in its order, and returns how many regions it lists. Writes no more than `capacity`.
uint32_t sfntVariationScalars(SfntTable store, uint16_t outer, SfntCoords coords, float* PR_NULLABLE out, uint32_t capacity);

// Returns the scalar of one axis of a region at a coordinate: 1 at its peak, falling to 0 at its start and end. An axis
// whose peak is 0, or whose region is invalid, takes no part and gives 1.
float sfntRegionAxisScalar(float start, float peak, float end, float coord);

// Returns the number of the face's variation axes (fvar), or one of them, in design units.
uint32_t sfntAxisCount(const Sfnt& sfnt);
PrFontAxis sfntAxis(const Sfnt& sfnt, uint32_t index);

// Normalizes the design value of each axis, one per axis, to a coordinate in [-1, 1] with the default at 0, and maps it
// through avar. Each coordinate is rounded to a multiple of 1/16384, before avar and after, as F2DOT14 holds it.
void sfntNormalize(const Sfnt& sfnt, const float* design, uint32_t count, float* out);

// Returns the MVAR delta of a metric tag, in font units, or zero if the table has none.
float sfntMetricDelta(const Sfnt& sfnt, uint32_t tag, SfntCoords coords);

// Returns a name from the name table by id, as UTF-8, preferring a Windows Unicode English record. Returns the null
// string if there is none.
PrRawString sfntName(const Sfnt& sfnt, uint16_t id);

// Gets a baseline of the horizontal axis of the BASE table, in font units above the origin, at the coordinates. It is
// the value of the script if the script has one, and otherwise the value of the first script that does. Returns false
// if no script has the tag.
bool sfntBaseline(const Sfnt& sfnt, uint32_t baselineTag, uint32_t scriptTag, SfntCoords coords, float* out);

// Writes up to `capacity` tags to `out` and returns the total, each tag once: the script tags that the GSUB and GPOS
// tables declare, except DFLT. If `out` holds fewer than the total, only the count can be wrong, because a tag past
// `capacity` is not compared with those before it.
uint32_t sfntScriptTags(const Sfnt& sfnt, uint32_t* PR_NULLABLE out, uint32_t capacity);

// Reads the weight, width and slope of the face from OS/2 and post, and the ranges that its fvar axes give them.
void sfntDescribe(const Sfnt& sfnt, PrFontInfo* out);

// Returns the family name that matching reads: the typographic family if the name table has one.
PrRawString sfntFamily(const Sfnt& sfnt);

// A subtable of the cmap table, which maps code points to glyphs. Reading one changes nothing, so any number of threads
// may read it at once.
struct SfntCmap {
    SfntTable subtable;             // empty for none, which maps nothing
    uint16_t format;
};

// Opens the subtable that maps Unicode code points, as FreeType chooses it: the last subtable of a 32-bit Unicode
// encoding, or else the last of any Unicode encoding, among those of a format that maps characters. Returns false and
// opens none if there is no such subtable, or if the subtable chosen is not of a format that sfntCmapGlyph reads.
bool sfntCmapUnicode(const Sfnt& sfnt, SfntCmap* out);

// Returns the glyph that the subtable maps the code point to, or 0 for none.
uint32_t sfntCmapGlyph(const SfntCmap& cmap, uint32_t codePoint);

// The encodings that a color bitmap strike holds.
enum {
    SfntImageType_Png,
    SfntImageType_Jpeg,
    SfntImageType_Tiff,
};
typedef uint8_t SfntImageType;

// The image of a glyph from a color bitmap strike (sbix, or CBDT and CBLC).
struct SfntImage {
    const uint8_t* data;            // the encoded image, within the file
    uint32_t size;
    uint32_t width;                 // in pixels
    uint32_t height;
    float pixelsPerEm;              // the pixels per em of the strike

    // The corner of the top left pixel, in pixels from the origin of the glyph, with y down.
    float left;
    float top;

    // How `data` is encoded.
    SfntImageType type;
};

// Gets the image of the glyph from the best strike for `pixelsPerEm`, among the strikes with an image of the glyph. The
// best is the smallest strike at least that large, or else the largest. Returns false if no strike has an image.
bool sfntGlyphImage(const Sfnt& sfnt, uint32_t glyph, float pixelsPerEm, SfntImage* out);

// Gets the SVG document that holds the glyph (OpenType SVG), and its index among the documents of the font. Returns
// false if no document holds it.
bool sfntGlyphSvg(const Sfnt& sfnt, uint32_t glyph, SfntTable* document, uint32_t* index);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
