// Fonts: the state of a font, which the sources of the module's public headers read.

#pragma once

#include "pr/atomic.h"
#include "pr/base.h"
#include "pr/file.h"
#include "pr/font.h"
#include "pr/mutex.h"
#include "pr/path.h"
#include "pr/raw_array.h"

#include "font/src/cff.h"
#include "font/src/colr.h"
#include "font/src/font_math.h"
#include "font/src/glyf.h"
#include "font/src/glyph_metrics.h"
#include "font/src/sfnt.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct Font;

// The bytes of a font file, shared by the fonts derived from it: either a copy that follows this structure in the same
// allocation, or a mapped file.
struct FontData {
    Atomic<uint32_t> references;
    const uint8_t* bytes;
    size_t size;
    PrByteView mapping;
    uint8_t* PR_NULLABLE owned;     // bytes allocated apart from the data, which the data frees

    // The fonts open over the data, among which prFontCreateDerived looks for one equal to the font that it would make.
    // Each font removes itself as it is freed. The lock guards the list.
    PrMutex lock;
    RawArray<Font*> fonts;
};

// Which of the bounds of a glyph's outline are filled in (Font::boundsFilled).
enum {
    FontBounds_Outline = 1u << 0,   // outlineBounds
    FontBounds_Ink = 1u << 1,       // inkBounds
};
typedef uint8_t FontBounds;

// A face of the data, at the instance and with the derivation that it was opened with. Fonts are shared between
// threads.
struct Font {
    mutable Atomic<uint32_t> references;
    uint64_t id;
    FontData* data;
    uint32_t index;
    Sfnt sfnt;

    // The readers of the face.
    FontGlyphMetricTables metricTables;
    GlyfFace glyf;
    CffFace cff;
    ColrFace colr;
    FontMathFace math;
    bool truetype;                      // true if glyf holds the outlines, and false for CFF or none
    bool outlined;                      // false for a face of bitmap strikes only
    bool strikes;                       // true if the font has color bitmap strikes
    bool svg;                           // true if the font has SVG documents of glyphs
    uint32_t unitsPerEm;
    uint32_t glyphCount;
    SfntCmap cmap;
    SfntCmapSequences sequences;

    // The derivation that the font was opened with.
    float sizeAdjust;
    PrFontHinting hinting;
    PrFontHinter* PR_NULLABLE hinter;
    bool aliased;
    float embolden;
    float oblique;
    PrFontOverrideFlags overrides;
    float ascentOverride;
    float descentOverride;
    float lineGapOverride;
    RawArray<PrFontFeature> features;
    RawArray<float> axisKey;               // the value that the derivation asked for, by axis, which equal fonts share

    // The instance of the font, and its measurements at that instance.
    RawArray<PrFontAxis> axes;
    RawArray<float> axisValues;
    RawArray<float> normalized;            // the normalized axis values with avar applied, or empty at the default
                                           // instance
    PrFontMetrics metrics;
    PrFontInfo info;

    // The outlines, by glyph, each created on first use with its bounds, and the tight bounds of its ink, found on
    // first use. The mutex guards them.
    mutable PrMutex mutex;
    mutable PrPath* PR_NULLABLE* PR_NULLABLE outlines;
    mutable PrRect* PR_NULLABLE outlineBounds;
    mutable PrRect* PR_NULLABLE inkBounds;

    // By glyph, which of the bounds of its outline are filled in (FontBounds). A reader reads the bounds without the
    // mutex once their flag is set. The bounds are set before the flag.
    Atomic<uint8_t>* boundsFilled;

    // The substitutions of GSUB's features (prFontFeatureGlyph), built on first use under the mutex. A reader reads
    // them without the mutex once the flag is set, which is set after they are built.
    mutable SfntFeatureSubstitutions substitutions;
    mutable Atomic<uint8_t> substitutionsBuilt;

    // By glyph, the bits of its advance in font units, or kAdvanceUnknown until it is found. A font at an instance
    // other than its default finds each advance once. Null at the default instance, whose advances hmtx gives.
    Atomic<uint32_t>* PR_NULLABLE advances;
};

} // namespace pr_impl

// C opaque types
struct PrFont : pr_impl::Font {};

PR_ASSUME_NONNULL_END
