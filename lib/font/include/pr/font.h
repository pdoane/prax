// Fonts: the fonts of font files, and the data that drawing needs from them.
//
// Shaping text into glyphs belongs to lib/text. Every length is in ems, with y down from the glyph's origin on the
// baseline, whatever the font's units. A font is immutable, and any thread may read it.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/color.h"
#include "pr/compress.h"
#include "pr/geom.h"
#include "pr/path.h"
#include "pr/raw_string.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// One font of a font file, which may be a collection of several fonts, and the data that drawing needs from it. A font
// is immutable and reference counted.
typedef struct PrFont PrFont;

// A backend that fits outlines to the pixel grid (pr/font_hinter.h).
typedef struct PrFontHinter PrFontHinter;

// An OpenType tag: four ASCII characters, the first in the high byte.
#define PR_FONT_TAG(a, b, c, d)                                                                   \
    (((a) & 0xffu) << 24 | ((b) & 0xffu) << 16 | ((c) & 0xffu) << 8 | ((d) & 0xffu))

// Returns the number of fonts in the font file in `bytes`: one for a single font, the number of fonts in a collection,
// and zero for bytes that are not a font.
uint32_t prFontCount(PrByteView bytes);

// A value for a variation axis.
typedef struct PrFontVariation {
    uint32_t tag;
    float value;                // in the user coordinates of the axis, such as 700 on wght
} PrFontVariation;

// A value for an OpenType feature. Zero is off, one is on, and a larger value selects an alternate.
typedef struct PrFontFeature {
    uint32_t tag;
    uint32_t value;
} PrFontFeature;

// How the outlines of a font are fitted to the pixel grid at the size at which they are drawn, by the font's hinter.
enum {
    PrFontHinting_None,         // the design outline, the same at every size
    PrFontHinting_Light,        // fitted vertically only; advances and horizontal positions stay as designed
    PrFontHinting_Full,         // fitted in both directions, advances included
    PrFontHinting_Count,
};
typedef uint8_t PrFontHinting;

// Returns the hinting that `name` names, as prFontHintingName spells it, ignoring case, or Count if it names none.
PrFontHinting prFontHintingFromName(PrStringView name);

// Returns the name of a hinting in lowercase, as a command line spells it.
PrStringView prFontHintingName(PrFontHinting hinting);

// The fields of a PrFontDerivation that replace the values of the base font.
enum {
    PrFontOverrideFlag_Ascent = 1u << 0,
    PrFontOverrideFlag_Descent = 1u << 1,
    PrFontOverrideFlag_LineGap = 1u << 2,
    PrFontOverrideFlag_Hinting = 1u << 3,
    PrFontOverrideFlag_Aliased = 1u << 4,
};
typedef uint8_t PrFontOverrideFlags;

// How a derived font differs from its base font.
typedef struct PrFontDerivation {
    // Values for the axes that they name, clamped to the range of each axis. Other axes keep the values of the base
    // font.
    const PrFontVariation* PR_NULLABLE variations;
    uint32_t variationCount;

    // Features that follow the base font's own features. A later feature wins over an earlier one.
    const PrFontFeature* PR_NULLABLE features;
    uint32_t featureCount;

    float sizeAdjust;           // multiplies the size-adjust of the base font

    // Values that replace those of the base font if `overrides` names them. The metrics are in ems before the
    // size-adjust.
    float ascent;
    float descent;
    float lineGap;
    bool aliased;
    PrFontOverrideFlags overrides;

    // The hinting and the hinter that fits outlines by it, which replace the base font's together if `overrides` names
    // PrFontOverrideFlag_Hinting. The hinter must outlive the fonts that name it.
    PrFontHinting hinting;
    PrFontHinter* PR_NULLABLE hinter;

    // Synthetic bold and oblique, added to those of the base font (prFontEmbolden, prFontOblique).
    float embolden;
    float oblique;
} PrFontDerivation;

// Returns the derivation with no variation, feature, override or synthesis, and a size-adjust of one.
PrFontDerivation prFontDerivationDefault(void);

// Creates the font at `index` of the file in `bytes`, derived as `derivation` specifies (prFontCreateDerived), and
// copies the bytes. A null `derivation` gives the default instance. Returns InvalidData if the bytes are not a font or
// the file has no font `index`. On failure, `*out` is null.
PrStatus prFontCreate(PrByteView bytes, uint32_t index, const PrFontDerivation* PR_NULLABLE derivation,
    const PrFont* PR_NULLABLE* PR_NONNULL out);

// Creates a font in the same way as prFontCreate, from a WOFF or WOFF2 file in `bytes`, whose tables `codec`
// decompresses: zlib for WOFF and Brotli for WOFF2. A WOFF2 file of a collection decodes to the collection, and `index`
// picks its font. Returns NotSupported if `codec` does not decode the compression of the file. Returns InvalidData in
// the same cases as prFontCreate, and for a file that the rules of the format reject.
PrStatus prFontCreateWoff(PrByteView bytes, uint32_t index, const PrFontDerivation* PR_NULLABLE derivation,
    PrCompressCodec* codec, const PrFont* PR_NULLABLE* PR_NONNULL out);

// Creates a font in the same way as prFontCreate, for the file at the UTF-8 `path`. The file is mapped instead of
// copied. Returns Io if the file cannot be read, and InvalidData in the same cases as prFontCreate. On failure, `*out`
// is null.
PrStatus prFontCreateFromFile(PrStringView path, uint32_t index, const PrFontDerivation* PR_NULLABLE derivation,
    const PrFont* PR_NULLABLE* PR_NONNULL out);

// Creates a font of the same file and index as `font`, derived as `derivation` specifies. The two fonts share their
// bytes. The size-adjust is finite and above zero, and the synthesis is finite. A variation whose tag names no axis of
// the font, or that the font cannot take, is ignored.
//
// One state is one font. If a font of the same bytes and index is open in the state that this derivation gives, the
// call returns a reference to that font instead of a new font. If the derivation changes nothing, that font is `font`
// itself. Fonts made from the same file or bytes by separate calls to create are separate, whatever their state.
const PrFont* prFontCreateDerived(const PrFont* font, const PrFontDerivation* derivation);

// Adds or releases a reference. The last release frees the font.
void prFontRetain(const PrFont* font);
void prFontRelease(const PrFont* PR_NULLABLE font);

// Returns a number unique to the font for the life of the process, which serves as a key for data derived from it. A
// derived font has its own id.
uint64_t prFontId(const PrFont* font);

// Returns the bytes of the font file, and the index of the font within them. Every font derived from the file shares
// the bytes, and they stay valid while the font lives.
PrByteView prFontData(const PrFont* font);
uint32_t prFontIndex(const PrFont* font);

// Returns the family and style names from the name table of the font. The typographic names are preferred if the table
// has them.
PrRawString prFontFamilyName(const PrFont* font);
PrRawString prFontStyleName(const PrFont* font);

// Returns the factor that scales the outlines and metrics of the font. It is one for a font made from a file.
float prFontSizeAdjust(const PrFont* font);

// Returns how the outlines of the font are fitted to the pixel grid, and the hinter that fits them. The hinting is None
// for a font made from a file, and for a font without a hinter.
PrFontHinting prFontHinting(const PrFont* font);
PrFontHinter* PR_NULLABLE prFontHinter(const PrFont* font);

// Returns true if the glyphs of the font draw without antialiasing, each pixel covered or not by its center, as its
// derivation says (PrFontOverrideFlag_Aliased). A font made from a file is antialiased, except for one whose family is
// Ahem, the test font whose glyphs are boxes of the em.
bool prFontIsAliased(const PrFont* font);

// Returns the synthetic bold: how far outlines grow outward, in ems. A negative value shrinks them. Advances stay as
// designed.
float prFontEmbolden(const PrFont* font);

// Returns the synthetic oblique: the x shear per unit of height, about the origin of each glyph. It applies to outlines
// and color glyphs alike. prFontGlyphOutline does not apply it. The code that places the glyphs does.
float prFontOblique(const PrFont* font);

// Returns the features that a shaper applies before the features of a run. There are none for a font made from a file.
uint32_t prFontFeatureCount(const PrFont* font);
const PrFontFeature* PR_NULLABLE prFontFeatures(const PrFont* font);

// ---------------------------------------------------------------------------------------------------------------------
// Variations

// A variation axis and its range, in the user coordinates of the axis.
typedef struct PrFontAxis {
    uint32_t tag;
    float min;
    float defaultValue;
    float max;
} PrFontAxis;

// Returns the axes of a variable font, in the order in which the font lists them. There are none for any other font. An
// `index` past the axes gives zeros.
uint32_t prFontAxisCount(const PrFont* font);
PrFontAxis prFontAxis(const PrFont* font, uint32_t index);

// Returns the value of the font on axis `index`, or zero for an `index` past the axes.
float prFontAxisValue(const PrFont* font, uint32_t index);

// Returns the normalized coordinate of the font on axis `index`, which variation data is indexed by: in [-1, 1], with
// the axis's default at zero and avar applied. Returns zero for an `index` past the axes.
float prFontAxisCoordinate(const PrFont* font, uint32_t index);

// ---------------------------------------------------------------------------------------------------------------------
// Info

// The slope of a font's glyphs.
enum {
    PrFontSlope_Normal,
    PrFontSlope_Italic,
    PrFontSlope_Oblique,
};
typedef uint8_t PrFontSlope;

// The normal and bold weights, and the normal width in percent.
static const float kPrFontWeightNormal = 400;
static const float kPrFontWeightBold = 700;
static const float kPrFontWidthNormal = 100;

// The properties of a font that matching reads. A property that a variation axis varies is a range.
typedef struct PrFontInfo {
    float weightMin;            // on the scale of kPrFontWeightNormal and kPrFontWeightBold
    float weightMax;
    float widthMin;             // percent
    float widthMax;
    float obliqueMin;           // degrees clockwise, from the italic angle or a slnt axis
    float obliqueMax;
    float opticalSizeMin;       // zero unless an opsz axis varies it
    float opticalSizeMax;
    PrFontSlope slope;          // at the default instance
    bool italicAxis;            // true if an ital axis selects italic
} PrFontInfo;

// Returns the info of the font.
PrFontInfo prFontInfo(const PrFont* font);

// ---------------------------------------------------------------------------------------------------------------------
// Metrics

// The metrics that the font provides, as opposed to those synthesized from its glyphs or fallbacks.
enum {
    PrFontMetricFlag_CapHeight = 1u << 0,
    PrFontMetricFlag_XHeight = 1u << 1,
    PrFontMetricFlag_Subscript = 1u << 2,
    PrFontMetricFlag_Superscript = 1u << 3,
    PrFontMetricFlag_Hanging = 1u << 4,
    PrFontMetricFlag_Ideographic = 1u << 5,
    PrFontMetricFlag_IdeographicInk = 1u << 6,
    PrFontMetricFlag_Math = 1u << 7,
    PrFontMetricFlag_Vertical = 1u << 8,      // vertical metrics (vhea and vmtx), which vertical advances read
    PrFontMetricFlag_AverageAdvance = 1u << 9,
};
typedef uint16_t PrFontMetricFlags;

// The metrics of a font in ems, with size-adjust applied. Positions along y are down from the alphabetic baseline, so a
// position above it is negative. Lengths are positive. A metric the font lacks is synthesized from related metrics,
// then from glyphs, then from fallbacks.
typedef struct PrFontMetrics {
    uint32_t unitsPerEm;            // the design grid of the font, for code that reads its tables
    float ascent;                   // above the baseline, positive
    float descent;                  // below the baseline, positive
    float lineGap;
    float capHeight;
    float xHeight;
    float underlinePosition;        // the center of the underline
    float underlineThickness;
    float strikeoutPosition;        // the center of the strikeout
    float strikeoutThickness;
    float subscriptPosition;        // the baseline of a subscript
    float subscriptSize;            // the em size of a subscript
    float superscriptPosition;
    float superscriptSize;
    float alphabeticBaseline;       // relative to the origin of the glyph
    float hangingBaseline;
    float ideographicUnder;         // the bottom of the ideographic em box
    float ideographicOver;          // the top of the ideographic em box
    float ideographicInkUnder;      // the bottom of the ideographic character face
    float ideographicInkOver;       // the top of the ideographic character face
    float centralBaseline;          // halfway between the ideographic over and under
    float mathBaseline;

    // The average advance of the font's characters, which OS/2 gives, and the largest advance of its glyphs, which hhea
    // gives. Text controls are sized in characters by them.
    float averageAdvance;
    float maxAdvance;

    PrFontMetricFlags fromFont;     // the metrics the font provides
} PrFontMetrics;

// Returns the metrics of the font.
PrFontMetrics prFontMetrics(const PrFont* font);

// Returns the OpenType script tags that the layout tables of the font (GSUB and GPOS) declare, each once, except DFLT.
// Writes up to `capacity` tags to `out` and returns the total.
uint32_t prFontScriptTags(const PrFont* font, uint32_t* PR_NULLABLE out, uint32_t capacity);

// Returns the number of glyphs in the font.
uint32_t prFontGlyphCount(const PrFont* font);

// Returns the glyph that the character map of the font assigns to `codePoint`, or zero if there is none. The lookup
// does no shaping.
uint16_t prFontCodePointGlyph(const PrFont* font, uint32_t codePoint);

// Returns the advance of the glyph down a vertical line, and the position of its vertical origin relative to its
// horizontal origin. Both come from the vertical metrics of the font, or are synthesized from its ascent and descent.
float prFontGlyphVerticalAdvance(const PrFont* font, uint16_t glyph);
PrVec2 prFontGlyphVerticalOrigin(const PrFont* font, uint16_t glyph);

// Returns the bounds of the unhinted, emboldened outline of the glyph, including control points. Returns an empty
// rectangle for a glyph with no outline.
PrRect prFontGlyphBounds(const PrFont* font, uint16_t glyph);

// Appends the emboldened outline of the glyph in ems to `out`, mapped through `transform`. The outline fills with the
// nonzero rule. Returns false for a glyph with no outline, such as a space. A hinted font fits the outline to
// `pixelsPerEm` pixels per em. It lands on the pixel grid if `transform` scales an em to that size and puts the origin
// of the glyph on a pixel. A `pixelsPerEm` of zero gives the unhinted outline.
bool prFontGlyphOutline(const PrFont* font, uint16_t glyph, float pixelsPerEm, PrAffine2 transform, PrPath* out);

// Returns the horizontal advance of the glyph in ems, drawn at `pixelsPerEm` pixels per em. With Full hinting, it is a
// whole number of pixels. Other hinting, and a `pixelsPerEm` of zero, give the design advance. Returns zero for a glyph
// past the last glyph of the font.
float prFontGlyphAdvance(const PrFont* font, uint16_t glyph, float pixelsPerEm);

// ---------------------------------------------------------------------------------------------------------------------
// Color glyphs

// How far a gradient continues past its ends.
enum {
    PrFontExtend_Pad,
    PrFontExtend_Repeat,
    PrFontExtend_Reflect,
};
typedef uint8_t PrFontExtend;

// The kinds of paint: a solid color or a gradient.
enum {
    PrFontPaintKind_Solid,
    PrFontPaintKind_Linear,
    PrFontPaintKind_Radial,
    PrFontPaintKind_Conic,          // the sweep gradient of COLR
};
typedef uint8_t PrFontPaintKind;

// How a layer combines with what lies beneath it, as the composite modes of COLR define it. A blend mode composites
// source-over.
enum {
    PrFontComposite_Clear,
    PrFontComposite_Source,
    PrFontComposite_Destination,
    PrFontComposite_SourceOver,
    PrFontComposite_DestinationOver,
    PrFontComposite_SourceIn,
    PrFontComposite_DestinationIn,
    PrFontComposite_SourceOut,
    PrFontComposite_DestinationOut,
    PrFontComposite_SourceAtop,
    PrFontComposite_DestinationAtop,
    PrFontComposite_Xor,
    PrFontComposite_Plus,
    PrFontComposite_Screen,
    PrFontComposite_Overlay,
    PrFontComposite_Darken,
    PrFontComposite_Lighten,
    PrFontComposite_ColorDodge,
    PrFontComposite_ColorBurn,
    PrFontComposite_HardLight,
    PrFontComposite_SoftLight,
    PrFontComposite_Difference,
    PrFontComposite_Exclusion,
    PrFontComposite_Multiply,
    PrFontComposite_Hue,
    PrFontComposite_Saturation,
    PrFontComposite_Color,
    PrFontComposite_Luminosity,
};
typedef uint8_t PrFontComposite;

// The color of a gradient at `offset`.
typedef struct PrFontColorStop {
    float offset;
    PrColor color;              // sRGB, straight alpha
} PrFontColorStop;

// A paint. Its kind decides which member of the union it uses. A gradient interpolates its stops in sRGB with
// premultiplied alpha.
typedef struct PrFontPaint {
    PrFontPaintKind kind;
    PrFontExtend extend;                        // gradients
    PrColor color;                              // Solid: sRGB, straight alpha
    const PrFontColorStop* PR_NULLABLE stops;   // gradients: offsets nondecreasing
    uint32_t stopCount;
    union {
        struct {
            PrVec2 start;               // where the parameter is 0
            PrVec2 end;                 // where the parameter is 1, before `rotationPoint` rotates the gradient
            PrVec2 rotationPoint;       // the lines of the gradient run parallel to the line from `start` to here
        } linear;                       // Linear
        struct {
            PrVec2 start;               // the circle where the parameter is 0
            float startRadius;
            PrVec2 end;                 // the circle where the parameter is 1
            float endRadius;
        } radial;                       // Radial: two-point conical
        struct {
            PrVec2 center;
            float startAngle;           // radians clockwise from +x, where the parameter is 0
            float endAngle;             // where the parameter is 1
        } conic;                        // Conic
    };
} PrFontPaint;

// Receives a color glyph (OpenType COLR) as fills nested in pushes and pops, in the space of the outline of the glyph.
// Each push is closed by its pop, in order. Each callback is passed `context`.
typedef struct PrFontPainter {
    void* PR_NULLABLE context;

    // A transform applied before the current transform, until its pop.
    void (*pushTransform)(void* PR_NULLABLE context, PrAffine2 transform);
    void (*popTransform)(void* PR_NULLABLE context);

    // A clip to the outline of `glyph` (prFontGlyphOutline), until its pop.
    void (*pushClipGlyph)(void* PR_NULLABLE context, uint16_t glyph);
    void (*popClip)(void* PR_NULLABLE context);

    // A layer that starts transparent. At its pop, it combines with what lies beneath by `composite`.
    void (*pushLayer)(void* PR_NULLABLE context, PrFontComposite composite);
    void (*popLayer)(void* PR_NULLABLE context);

    // Paints `paint` over everything that the current clips let through.
    void (*fill)(void* PR_NULLABLE context, const PrFontPaint* paint);
} PrFontPainter;

// Returns the number of palettes that the colors of the font come in.
uint32_t prFontPaletteCount(const PrFont* font);

// The backgrounds that a palette is marked as suitable for.
enum {
    PrFontPaletteFlag_Light = 1u << 0,
    PrFontPaletteFlag_Dark = 1u << 1,
};
typedef uint8_t PrFontPaletteFlags;

// Returns the flags of palette `palette`, or none if the font does not mark its palettes or has no such palette.
PrFontPaletteFlags prFontPaletteFlags(const PrFont* font, uint32_t palette);

// Returns the first palette marked with any of `flags`, or the first palette if none is.
uint32_t prFontPaletteSelect(const PrFont* font, PrFontPaletteFlags flags);

// A color that replaces the color of the font for palette entry `index`.
PR_NO_PADDING_BEGIN
typedef struct PrFontPaletteColor {
    uint32_t index;
    PrColor color;              // sRGB, straight alpha
} PrFontPaletteColor;
PR_NO_PADDING_END

// The palette that colors a color glyph: one of the palettes of the font, with some of its entries replaced.
typedef struct PrFontPalette {
    uint32_t index;                               // the first palette if the font has no such palette
    const PrFontPaletteColor* PR_NULLABLE colors; // a later color wins over an earlier one
    uint32_t colorCount;
} PrFontPalette;

// Returns the number of entries in each palette of the font.
uint32_t prFontPaletteEntryCount(const PrFont* font);

// Returns the color of entry `index` in `palette`, in sRGB with straight alpha. `index` is below
// prFontPaletteEntryCount. A color in `palette` replaces the color of the font. A null `palette` means the first
// palette of the font.
PrColor prFontPaletteEntry(const PrFont* font, const PrFontPalette* PR_NULLABLE palette, uint32_t index);

// Returns true if the glyph has color layers (OpenType COLR).
bool prFontGlyphHasColor(const PrFont* font, uint16_t glyph);

// Walks the color layers of the glyph through `painter`. Colors come from `palette`, or from the first palette if it is
// null. `foreground` is the color used if the font asks for the text color. Returns false and paints nothing for a
// glyph with no color layers. The font holds none of its locks while it calls the painter, so the painter may use the
// font.
bool prFontGlyphPaint(const PrFont* font, uint16_t glyph, const PrFontPalette* PR_NULLABLE palette,
    PrColor foreground, const PrFontPainter* painter);

// ---------------------------------------------------------------------------------------------------------------------
// Bitmap glyphs

// A color glyph as an encoded image, from one of the strikes of fixed sizes of the font (sbix, CBDT). The size-adjust
// scales `bounds`. Synthetic bold does not affect the image.
typedef struct PrFontGlyphImage {
    PrByteView bytes;           // the encoded image, valid while the font lives
    PrStringView type;          // the essence of its MIME type, as an image codec names it
    PrUVec2 extent;             // in pixels
    float pixelsPerEm;          // the pixels per em of the strike that the image belongs to
    PrRect bounds;              // the position of the image, in ems with y down
} PrFontGlyphImage;

// Writes to `out` the image of the glyph from the strike for drawing at `pixelsPerEm` pixels per em. That is the
// smallest strike at least that large, or else the largest. Returns false for a glyph with no image.
bool prFontGlyphImage(const PrFont* font, uint16_t glyph, float pixelsPerEm, PrFontGlyphImage* out);

// ---------------------------------------------------------------------------------------------------------------------
// SVG glyphs

// The SVG document that holds a color glyph (OpenType SVG). A document may hold several glyphs. The glyph is the
// element whose id is "glyph" followed by the glyph id, drawn in place in the user space of the document. That space
// has y down from the origin of the glyph on the baseline. The custom properties --color0, --color1 and so on are the
// entries of the palette, and the initial color is the text color.
typedef struct PrFontGlyphSvg {
    PrByteView document;        // UTF-8, or gzip-compressed if it starts with 1f 8b; valid while the font lives
    uint32_t index;             // the index of the document in the font, which serves as a key for a parse of it
    float scale;                // ems per unit of the user space of the document, with the size-adjust applied
} PrFontGlyphSvg;

// Writes to `out` the SVG document that holds the glyph. Returns false for a glyph with no document.
bool prFontGlyphSvg(const PrFont* font, uint16_t glyph, PrFontGlyphSvg* out);

// ---------------------------------------------------------------------------------------------------------------------
// Glyph runs

// A glyph of a run, and its position.
typedef struct PrFontGlyph {
    uint16_t id;
    PrVec2 position;                   // the origin of the glyph, on its baseline
} PrFontGlyph;

// Placed glyphs of one font at one size, which text sets and drawing records. The outline of a glyph is in ems with y
// down. It is emboldened, sheared by the oblique of the font, scaled by `size`, transformed, and moved to its position,
// in that order. It is drawn with the rendering options of the font.
typedef struct PrFontGlyphRun {
    const PrFont* font;
    float size;                                 // the em size
    const PrFontGlyph* glyphs;
    uint32_t glyphCount;
    const PrAffine2* PR_NULLABLE transforms;    // one per glyph about its origin, or null
    PrFontPalette palette;                      // the palette and color overrides of a color font
} PrFontGlyphRun;

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
