// Fonts: the faces of font files, read by the readers of font/src: sfnt.h for their tables, glyf.h and cff.h for their
// outlines, glyph_metrics.h for their advances and colr.h for their color glyphs.
//
// The readers only read the font's bytes, so any number of threads load from a font at once. The mutex of the font
// guards the caches of outlines that it fills behind const. Outlines are cached in the ems of the design. Every value
// that the font returns is scaled by its size-adjust on the way out. A font hints through its hinter, which the readers
// leave alone.

#include "pr/font.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/bit_cast.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/font_hinter.h"
#include "pr/mem.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"
#include "pr/vec.h"

#include "font/src/cff.h"
#include "font/src/colr.h"
#include "font/src/glyf.h"
#include "font/src/glyph_metrics.h"
#include "font/src/outline.h"
#include "font/src/sfnt.h"
#include "font/src/woff.h"

namespace pr_impl {

namespace {

constexpr PrStringView kHintingNames[PrFontHinting_Count] = {"none", "light", "full"};

constexpr uint32_t tagOf(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) << 24 | uint32_t(uint8_t(b)) << 16 | uint32_t(uint8_t(c)) << 8 | uint32_t(uint8_t(d));
}

// The bytes of a font file, shared by the fonts derived from it: either a copy that follows this structure in the same
// allocation, or a mapped file.
struct Font;

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
    bool truetype;                      // true if glyf holds the outlines, and false for CFF or none
    bool outlined;                      // false for a face of bitmap strikes only
    bool strikes;                       // true if the font has color bitmap strikes
    uint32_t unitsPerEm;
    uint32_t glyphCount;
    SfntCmap cmap;

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
    RawArray<float> axisKey;               // for each axis, the value that the derivation asked for; equal fonts share
                                           // it

    // The instance of the font, and its measurements at that instance.
    RawArray<PrFontAxis> axes;
    RawArray<float> axisValues;
    RawArray<float> normalized;            // the axis values normalized, avar applied; empty at the default instance
    PrFontMetrics metrics;
    PrFontInfo info;

    // The outlines, by glyph, each created on first use with its bounds. The mutex guards them.
    mutable PrMutex mutex;
    mutable PrPath* PR_NULLABLE* PR_NULLABLE outlines;
    mutable PrRect* PR_NULLABLE outlineBounds;

    // By glyph, whether the bounds of its outline are filled in. A reader reads the bounds without the mutex once the
    // flag is set. The bounds are set before the flag.
    Atomic<uint8_t>* boundsFilled;

    // By glyph, the bits of its advance in font units, or kAdvanceUnknown until it is found. A font at an instance
    // other than its default finds each advance once. Null at the default instance, at which hmtx answers.
    Atomic<uint32_t>* PR_NULLABLE advances;
};

// The bits of a NaN, which no advance can have.
constexpr uint32_t kAdvanceUnknown = 0xFFFFFFFFu;

Atomic<uint64_t> nextFontId;

const uint8_t* bytesOf(const FontData* data) {
    return data->bytes;
}

// Starts the single reference of the data and its list of fonts.
void initData(FontData* data) {
    data->references.storeRelaxed(1);
    prMutexInit(&data->lock);
}

void releaseData(FontData* data) {
    if (data->references.fetchSub(1) != 1)
        return;
    data->fonts.reset();
    prMutexReset(&data->lock);
    prFileUnmap(&data->mapping);
    prMemoryFree(data->owned);
    prMemoryFree(data);
}

// Takes a reference to a font in the list of the data, unless its last reference is gone, as it is while the font is
// being freed. Called with the lock of the data held.
bool retainLive(const Font* font) {
    uint32_t references = font->references.load();
    while (references != 0 && !font->references.cas(references, references + 1)) {
    }
    return references != 0;
}

SfntCoords coordsOf(const Font* font) {
    return SfntCoords{font->normalized.data, font->normalized.count};
}

// ---------------------------------------------------------------------------------------------------------------------
// Faces

// Opens the readers of the face. Returns false if the face has neither outlines nor bitmap strikes.
bool openReaders(Font* font) {
    const Sfnt& sfnt = font->sfnt;
    font->unitsPerEm = sfntTable(sfnt, tagOf('h', 'e', 'a', 'd')).uint16At(18);
    if (font->unitsPerEm == 0)
        return false;
    font->glyphCount = sfntTable(sfnt, tagOf('m', 'a', 'x', 'p')).uint16At(4);
    fontGlyphMetricsOpen(sfnt, &font->metricTables);
    font->truetype = glyfOpen(sfnt, &font->metricTables, font->glyphCount, &font->glyf);
    font->outlined = font->truetype || cffOpen(sfnt, &font->cff);
    if (!font->truetype && font->outlined && cffGlyphCount(font->cff) < font->glyphCount)
        font->glyphCount = cffGlyphCount(font->cff);
    font->strikes =
        sfntTable(sfnt, tagOf('s', 'b', 'i', 'x')).size != 0 || sfntTable(sfnt, tagOf('C', 'B', 'L', 'C')).size != 0;
    if ((!font->outlined && !font->strikes) || font->glyphCount == 0)
        return false;
    font->boundsFilled = static_cast<Atomic<uint8_t>*>(prMemoryAllocZeroed(sizeof(Atomic<uint8_t>) * font->glyphCount));
    sfntCmapUnicode(sfnt, &font->cmap);
    colrOpen(sfnt, &font->colr);
    return true;
}

void closeReaders(Font* font) {
    cffReset(font->cff);
}

// Reads the axes of the face and its default instance.
void readAxes(Font* font) {
    uint32_t count = sfntAxisCount(font->sfnt);
    for (uint32_t i = 0; i < count; ++i) {
        PrFontAxis axis = sfntAxis(font->sfnt, i);
        font->axes.push(axis);
        font->axisValues.push(axis.defaultValue);
    }
}

// Sets the axis values of the instance, and the normalized coordinates that the readers vary by. The default instance
// has none.
void setInstance(Font* font, const float* values) {
    font->normalized.clear();
    bool varied = false;
    for (uint32_t i = 0; i < font->axes.count; ++i) {
        font->axisValues.data[i] = values[i];
        varied = varied || values[i] != font->axes.data[i].defaultValue;
    }
    if (!varied)
        return;
    for (uint32_t i = 0; i < font->axes.count; ++i)
        font->normalized.push(0);
    sfntNormalize(font->sfnt, font->axisValues.data, font->axes.count, font->normalized.data);
    bool moved = false;
    for (uint32_t i = 0; i < font->normalized.count; ++i)
        moved = moved || font->normalized.data[i] != 0;
    if (!moved)
        font->normalized.clear();
}

// Loads the outline of the glyph in font units with y up. Gets its phantom points, which a TrueType outline carries.
// Returns false if the face has no outline for it.
bool loadDesign(const Font* font, uint32_t glyph, FontGlyphOutline* out, GlyfPhantom* phantom) {
    *phantom = GlyfPhantom{};
    if (!font->outlined || glyph >= font->glyphCount)
        return false;
    if (font->truetype)
        return glyfLoad(font->glyf, coordsOf(font), glyph, out, phantom);
    return cffLoad(font->cff, coordsOf(font), glyph, out);
}

// Gets the box of the glyph of a code point in font units with y up, for a metric that the font lacks. Returns false if
// the font has no glyph for the code point or the glyph has no outline.
bool boxOf(Font* font, uint32_t codePoint, float* bottom, float* top) {
    uint32_t glyph = font->cmap.subtable.data != nullptr ? sfntCmapGlyph(font->cmap, codePoint) : 0;
    FontGlyphOutline outline{};
    GlyfPhantom phantom{};
    bool found = glyph != 0 && loadDesign(font, glyph, &outline, &phantom) && outline.points.count != 0;
    if (found) {
        PrRect box = fontOutlineControlBox(outline);
        *bottom = box.top;
        *top = box.bottom;
    }
    fontOutlineReset(outline);
    return found;
}

// Reads the metrics of the font in design units with y up, from its tables and their variations. Then synthesizes the
// metrics that it lacks, and converts all of them to ems with y down, scaled by the size-adjust.
void readMetrics(Font* font) {
    const Sfnt& sfnt = font->sfnt;
    SfntCoords coords = coordsOf(font);
    auto delta = [&](const char* tag) {
        return sfntMetricDelta(sfnt, tagOf(tag[0], tag[1], tag[2], tag[3]), coords);
    };
    SfntTable hhea = sfntTable(sfnt, tagOf('h', 'h', 'e', 'a'));
    SfntTable os2 = sfntTable(sfnt, tagOf('O', 'S', '/', '2'));
    SfntTable post = sfntTable(sfnt, tagOf('p', 'o', 's', 't'));
    // A font at an instance other than its default finds each advance once: from HVAR, or by loading its glyph if its
    // phantom points vary the advance.
    if (coords.count != 0) {
        size_t bytes = sizeof(Atomic<uint32_t>) * font->glyphCount;
        font->advances = static_cast<Atomic<uint32_t>*>(prMemoryAlloc(bytes));
        prMemFill(font->advances, 0xFF, bytes);
    }
    uint16_t os2Version = os2.uint16At(0);
    bool hasOs2 = os2.size >= 78;
    float em = float(font->unitsPerEm);
    PrFontMetrics m{};
    m.unitsPerEm = font->unitsPerEm;
    if (sfntTable(sfnt, tagOf('v', 'h', 'e', 'a')).size != 0 && sfntTable(sfnt, tagOf('v', 'm', 't', 'x')).size != 0)
        m.fromFont |= PrFontMetricFlag_Vertical;

    // The USE_TYPO_METRICS flag of OS/2 requests its typographic metrics instead of those of hhea.
    float ascent = float(hhea.int16At(4));
    float descent = -float(hhea.int16At(6));
    float gap = float(hhea.int16At(8));
    if (hasOs2 && (os2.uint16At(62) & (1u << 7)) != 0) {
        ascent = float(os2.int16At(68)) + delta("hasc");
        descent = -(float(os2.int16At(70)) + delta("hdsc"));
        gap = float(os2.int16At(72)) + delta("hlgn");
    }
    if (ascent == 0 && descent == 0) {
        ascent = em * 0.8f;
        descent = em * 0.2f;
    }
    float capHeight = hasOs2 && os2Version >= 2 ? float(os2.int16At(88)) + delta("cpht") : 0;
    float xHeight = hasOs2 && os2Version >= 2 ? float(os2.int16At(86)) + delta("xhgt") : 0;
    float bottom = 0;
    float top = 0;
    if (capHeight > 0)
        m.fromFont |= PrFontMetricFlag_CapHeight;
    else if (boxOf(font, 'H', &bottom, &top))
        capHeight = top;
    else
        capHeight = em * 0.66f;
    if (xHeight > 0)
        m.fromFont |= PrFontMetricFlag_XHeight;
    else if (boxOf(font, 'x', &bottom, &top))
        xHeight = top;
    else
        xHeight = em * 0.5f;

    // A font without OS/2's average gives half an em, as the cascade's ch does.
    float averageAdvance = hasOs2 ? float(os2.int16At(2)) : 0;
    if (averageAdvance > 0)
        m.fromFont |= PrFontMetricFlag_AverageAdvance;
    else
        averageAdvance = em * 0.5f;
    float maxAdvance = float(hhea.uint16At(10));
    if (!(maxAdvance > 0))
        maxAdvance = averageAdvance;

    float underlineThickness = float(post.int16At(10)) + delta("unds");
    float underlinePosition = float(post.int16At(8)) + delta("undo");
    if (!(underlineThickness > 0))
        underlineThickness = em / 14;
    // post gives the top of the underline. Its center is half its thickness below.
    underlinePosition = underlinePosition != 0 ? underlinePosition - underlineThickness / 2 : -underlineThickness * 1.5f;
    float strikeThickness = hasOs2 ? float(os2.int16At(26)) + delta("strs") : 0;
    float strikePosition = hasOs2 ? float(os2.int16At(28)) + delta("stro") : 0;
    if (strikeThickness > 0) {
        strikePosition -= strikeThickness / 2;
    } else {
        strikeThickness = underlineThickness;
        strikePosition = xHeight / 2;
    }

    // Subscripts and superscripts: OS/2 gives their em size and how far their baseline moves.
    float subSize = hasOs2 ? float(os2.int16At(12)) + delta("sbys") : 0;
    float subOffset = hasOs2 ? float(os2.int16At(16)) + delta("sbyo") : 0;
    float superSize = hasOs2 ? float(os2.int16At(20)) + delta("spys") : 0;
    float superOffset = hasOs2 ? float(os2.int16At(24)) + delta("spyo") : 0;
    if (subSize > 0 && subOffset != 0)
        m.fromFont |= PrFontMetricFlag_Subscript;
    else {
        subSize = em * 0.65f;
        subOffset = em * 0.15f;
    }
    if (superSize > 0 && superOffset != 0)
        m.fromFont |= PrFontMetricFlag_Superscript;
    else {
        superSize = em * 0.65f;
        superOffset = em * 0.45f;
    }

    // Baselines: the values of BASE, and otherwise each synthesized from the metrics that it relates to.
    auto baseline = [&](char a, char b, char c, char d, float* out) {
        return sfntBaseline(sfnt, tagOf(a, b, c, d), 0, coords, out);
    };
    float alphabetic = 0;
    baseline('r', 'o', 'm', 'n', &alphabetic);
    float hanging = 0;
    if (baseline('h', 'a', 'n', 'g', &hanging))
        m.fromFont |= PrFontMetricFlag_Hanging;
    else if (boxOf(font, 0x0915, &bottom, &top))
        hanging = top;
    else
        hanging = em * 0.6f;
    float under = 0;
    float over = 0;
    bool hasUnder = baseline('i', 'd', 'e', 'o', &under);
    bool hasOver = baseline('i', 'd', 't', 'p', &over);
    if (hasUnder || hasOver)
        m.fromFont |= PrFontMetricFlag_Ideographic;
    if (hasUnder && !hasOver) {
        over = under + em;
    } else if (hasOver && !hasUnder) {
        under = over - em;
    } else if (!hasUnder && !hasOver) {
        // The ascent and descent, which match the em box of a CJK font, and center the text of any other font on its
        // central baseline.
        under = -descent;
        over = ascent;
    }
    float inkUnder = 0;
    float inkOver = 0;
    if (baseline('i', 'c', 'f', 'b', &inkUnder) && baseline('i', 'c', 'f', 't', &inkOver))
        m.fromFont |= PrFontMetricFlag_IdeographicInk;
    else if (boxOf(font, 0x6c38, &bottom, &top)) {
        inkUnder = bottom;
        inkOver = top;
    } else {
        inkUnder = under;
        inkOver = over;
    }
    float math = 0;
    if (baseline('m', 'a', 't', 'h', &math))
        m.fromFont |= PrFontMetricFlag_Math;
    else if (boxOf(font, 0x2212, &bottom, &top) || boxOf(font, '+', &bottom, &top))
        math = (bottom + top) / 2;
    else
        math = xHeight / 2;

    // Converts to ems, with y down, scaled. Overrides are already in ems.
    float k = font->sizeAdjust / em;
    m.ascent = (font->overrides & PrFontOverrideFlag_Ascent) != 0 ? font->ascentOverride * font->sizeAdjust : ascent * k;
    m.descent = (font->overrides & PrFontOverrideFlag_Descent) != 0 ? font->descentOverride * font->sizeAdjust : descent * k;
    if ((font->overrides & PrFontOverrideFlag_LineGap) != 0)
        m.lineGap = font->lineGapOverride * font->sizeAdjust;
    else
        m.lineGap = prF32Max(gap, 0) * k;
    m.capHeight = capHeight * k;
    m.xHeight = xHeight * k;
    m.underlinePosition = -underlinePosition * k;
    m.underlineThickness = underlineThickness * k;
    m.strikeoutPosition = -strikePosition * k;
    m.strikeoutThickness = strikeThickness * k;
    m.subscriptPosition = subOffset * k;
    m.subscriptSize = subSize * k;
    m.superscriptPosition = -superOffset * k;
    m.superscriptSize = superSize * k;
    m.alphabeticBaseline = -alphabetic * k;
    m.hangingBaseline = -hanging * k;
    m.ideographicUnder = -under * k;
    m.ideographicOver = -over * k;
    m.ideographicInkUnder = -inkUnder * k;
    m.ideographicInkOver = -inkOver * k;
    m.centralBaseline = -(under + over) / 2 * k;
    m.mathBaseline = -math * k;
    m.averageAdvance = averageAdvance * k;
    m.maxAdvance = maxAdvance * k;
    font->metrics = m;
}

// Opens the zeroed `font` over the face at `index` of the data. Returns false if the face does not parse. An open font
// holds a reference to the data. The caller sets what a derivation adds, and then reads the metrics.
bool openFont(Font* font, FontData* data, uint32_t index) {
    data->references.fetchAdd(1);
    font->data = data;
    font->index = index;
    font->sizeAdjust = 1;
    if (!sfntOpen(bytesOf(data), data->size, index, &font->sfnt) || !openReaders(font)) {
        closeReaders(font);
        prMemoryFree(font->boundsFilled);
        releaseData(data);
        return false;
    }
    font->references.storeRelaxed(1);
    font->id = nextFontId.fetchAdd(1) + 1;
    prMutexInit(&font->mutex);
    readAxes(font);
    sfntDescribe(font->sfnt, &font->info);
    // Ahem, the test font whose glyphs are boxes of the em, draws aliased, so that a box whose edge falls inside a
    // pixel covers it whole, as the reftests built on it expect. This is decided here, where every font opens, because
    // it is a property of the font file. Every copy draws alike, whether a host installs it, the @font-face of a page
    // loads it from bytes or WOFF, or a producer derives an instance of it, which takes the setting of its base. If a
    // user of fonts set it instead, the user would have to do so on each path that a font takes into the engine. A path
    // it missed would draw the page's own copy of Ahem antialiased beside the aliased copy of the host. A derivation
    // can still set it either way (PrFontOverrideFlag_Aliased).
    PrRawString family = sfntFamily(font->sfnt);
    font->aliased = prAsciiEqualsFold(prRawStringView(&family), "Ahem");
    prRawStringReset(&family);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Outlines

// Appends to `out` the outline of the glyph in the ems of the design, emboldened by `strength` font units. The readers
// alone answer, so no lock is needed.
void appendDesign(const Font* font, uint32_t glyph, float strength, PrPath* out) {
    FontGlyphOutline outline{};
    GlyfPhantom phantom{};
    if (loadDesign(font, glyph, &outline, &phantom) && outline.ends.count != 0) {
        if (strength != 0) {
            // Emboldening grows the outline right and up by the whole strength, so half of it is moved back.
            fontOutlineEmbolden(outline, strength, strength);
            fontOutlineTransform(outline, 0, PrAffine2{1, 0, 0, 1, -strength / 2, -strength / 2});
        }
        fontOutlineAppendPath(outline, 1 / float(font->unitsPerEm), out);
    }
    fontOutlineReset(outline);
}

// Returns the outline of the glyph, loaded once and cached. The mutex is held.
const PrPath* cachedOutline(const Font* font, uint32_t glyph) {
    if (font->outlines == nullptr) {
        font->outlines = static_cast<PrPath**>(prMemoryAllocZeroed(sizeof(PrPath*) * font->glyphCount));
        font->outlineBounds = static_cast<PrRect*>(prMemoryAllocZeroed(sizeof(PrRect) * font->glyphCount));
    }
    PrPath*& outline = font->outlines[glyph];
    if (outline == nullptr) {
        outline = prPathCreate();
        appendDesign(font, glyph, 0, outline);
        font->outlineBounds[glyph] = prPathBounds(outline);
        font->boundsFilled[glyph].store(1);
    }
    return outline;
}

// ---------------------------------------------------------------------------------------------------------------------
// Color glyphs

void replay(const ColrWalk& w, const PrFontPainter* painter) {
    void* PR_NULLABLE context = painter->context;
    for (uint32_t i = 0; i < w.steps.count; ++i) {
        const ColrStep& step = w.steps.data[i];
        switch (step.op) {
            case ColrOp_PushTransform:
                painter->pushTransform(context, step.transform);
                break;
            case ColrOp_PopTransform:
                painter->popTransform(context);
                break;
            case ColrOp_PushClipGlyph:
                painter->pushClipGlyph(context, step.glyph);
                break;
            case ColrOp_PopClip:
                painter->popClip(context);
                break;
            case ColrOp_PushLayer:
                painter->pushLayer(context, step.composite);
                break;
            case ColrOp_PopLayer:
                painter->popLayer(context);
                break;
            case ColrOp_Fill: {
                PrFontPaint paint = step.paint;
                paint.stops = paint.stopCount != 0 ? w.stops.data + step.stopStart : nullptr;
                painter->fill(context, &paint);
                break;
            }
            default:
                break;
        }
    }
}

bool validDerivation(const PrFontDerivation* PR_NULLABLE d) {
    return d == nullptr || (d->sizeAdjust > 0 && prF32IsFinite(d->sizeAdjust) && prF32IsFinite(d->embolden) &&
        prF32IsFinite(d->oblique));
}

// What a derivation makes of a font: the values that it sets each axis to, and what it scales, synthesizes and
// overrides. Two fonts of one face with equal states are equal.
struct Derived {
    RawArray<float> axes;                  // for each axis
    float sizeAdjust;
    float embolden;
    float oblique;
    PrFontHinting hinting;
    PrFontHinter* PR_NULLABLE hinter;
    bool aliased;
    PrFontOverrideFlags overrides;
    float ascentOverride;
    float descentOverride;
    float lineGapOverride;
    RawArray<PrFontFeature> features;
};

// Computes the state of a font opened in the same way as `font`, derived from the base if there is one and then as `d`
// specifies. The state takes the axis values of the base with the axes that `d` names changed, the scale of the base
// times that of `d`, the synthesis of the base plus that of `d`, the overrides of `d` over those of the base, and the
// features of the base followed by those of `d`.
void derivedOf(const Font* font, const Font* PR_NULLABLE base, const PrFontDerivation* PR_NULLABLE d, Derived* out) {
    const Font* from = base != nullptr ? base : font;
    for (uint32_t i = 0; i < font->axes.count; ++i) {
        const PrFontAxis& axis = font->axes.data[i];
        float value = base != nullptr ? base->axisValues.data[i] : axis.defaultValue;
        for (uint32_t k = 0; d != nullptr && k < d->variationCount; ++k) {
            if (d->variations[k].tag == axis.tag)
                value = prF32Clamp(d->variations[k].value, axis.min, axis.max);
        }
        out->axes.push(value);
    }
    out->sizeAdjust = from->sizeAdjust;
    out->embolden = from->embolden;
    out->oblique = from->oblique;
    out->hinting = from->hinting;
    out->hinter = from->hinter;
    out->aliased = from->aliased;
    out->overrides = from->overrides;
    out->ascentOverride = from->ascentOverride;
    out->descentOverride = from->descentOverride;
    out->lineGapOverride = from->lineGapOverride;
    out->features.append(from->features.span());
    if (d == nullptr)
        return;
    out->sizeAdjust *= d->sizeAdjust;
    out->embolden += d->embolden;
    out->oblique += d->oblique;
    if ((d->overrides & PrFontOverrideFlag_Hinting) != 0) {
        out->hinting = d->hinting;
        out->hinter = d->hinter;
    }
    if ((d->overrides & PrFontOverrideFlag_Aliased) != 0)
        out->aliased = d->aliased;
    if ((d->overrides & PrFontOverrideFlag_Ascent) != 0)
        out->ascentOverride = d->ascent;
    if ((d->overrides & PrFontOverrideFlag_Descent) != 0)
        out->descentOverride = d->descent;
    if ((d->overrides & PrFontOverrideFlag_LineGap) != 0)
        out->lineGapOverride = d->lineGap;
    out->overrides |= d->overrides;
    if (d->featureCount != 0)
        out->features.append(d->features, d->featureCount);
}

void derivedReset(Derived* derived) {
    derived->axes.reset();
    derived->features.reset();
}

// Returns true if the font is of the state `derived`.
bool isDerived(const Font* font, const Derived& derived) {
    return font->axisKey.count == derived.axes.count &&
        prMemEquals(font->axisKey.data, derived.axes.data, derived.axes.count * sizeof(float)) &&
        font->sizeAdjust == derived.sizeAdjust && font->embolden == derived.embolden &&
        font->oblique == derived.oblique && font->hinting == derived.hinting && font->hinter == derived.hinter &&
        font->aliased == derived.aliased && font->overrides == derived.overrides &&
        font->ascentOverride == derived.ascentOverride && font->descentOverride == derived.descentOverride &&
        font->lineGapOverride == derived.lineGapOverride && font->features.count == derived.features.count &&
        prMemEquals(font->features.data, derived.features.data, derived.features.count * sizeof(PrFontFeature));
}

// Makes a font, just opened at its default instance, match the base if there is one, and then applies the derivation.
// Reads its metrics.
void derive(Font* font, const Font* PR_NULLABLE base, const PrFontDerivation* PR_NULLABLE d) {
    Derived derived{};
    derivedOf(font, base, d, &derived);
    setInstance(font, derived.axes.data);
    font->sizeAdjust = derived.sizeAdjust;
    font->embolden = derived.embolden;
    font->oblique = derived.oblique;
    font->hinting = derived.hinting;
    font->hinter = derived.hinter;
    font->aliased = derived.aliased;
    font->overrides = derived.overrides;
    font->ascentOverride = derived.ascentOverride;
    font->descentOverride = derived.descentOverride;
    font->lineGapOverride = derived.lineGapOverride;
    font->features.clear();
    font->features.append(derived.features.span());
    font->axisKey.clear();
    font->axisKey.append(derived.axes.span());
    derivedReset(&derived);
    readMetrics(font);
}

// Opens the zeroed `font` over the face at `index` of the data, derived from the base if there is one and then as the
// derivation specifies. Returns false and frees `font` if the data has no such face. Releases the reference of the
// caller to the data.
bool openDerived(Font* font, FontData* data, uint32_t index, const Font* PR_NULLABLE base,
        const PrFontDerivation* PR_NULLABLE derivation) {
    bool opened = openFont(font, data, index);
    releaseData(data);
    if (!opened) {
        prMemoryFree(font);
        return false;
    }
    derive(font, base, derivation);
    return true;
}

// Returns true if the two fonts are of one face of one data, in one state.
bool sameFont(const Font* a, const Font* b) {
    return a->data == b->data && a->index == b->index && a->axisKey.count == b->axisKey.count &&
        prMemEquals(a->axisKey.data, b->axisKey.data, a->axisKey.count * sizeof(float)) &&
        a->sizeAdjust == b->sizeAdjust && a->embolden == b->embolden && a->oblique == b->oblique &&
        a->hinting == b->hinting && a->hinter == b->hinter && a->aliased == b->aliased &&
        a->overrides == b->overrides && a->ascentOverride == b->ascentOverride &&
        a->descentOverride == b->descentOverride && a->lineGapOverride == b->lineGapOverride &&
        a->features.count == b->features.count &&
        prMemEquals(a->features.data, b->features.data, a->features.count * sizeof(PrFontFeature));
}

// Releases a reference to the font, and frees it with the last (prFontRelease).
void releaseFont(const Font* PR_NULLABLE font) {
    if (font == nullptr || font->references.fetchSub(1) != 1)
        return;
    Font* data = const_cast<Font*>(font);
    // No reference is left to take, because a lookup in the list takes one only from a font that still has one.
    FontData* shared = data->data;
    prMutexLock(&shared->lock);
    for (uint32_t i = 0; i < shared->fonts.count; ++i) {
        if (shared->fonts.data[i] == data) {
            shared->fonts.data[i] = shared->fonts.last();
            shared->fonts.pop();
            break;
        }
    }
    prMutexUnlock(&shared->lock);
    if (data->hinter != nullptr)
        prFontHinterForget(data->hinter, data->id);
    if (data->outlines != nullptr) {
        for (uint32_t i = 0; i < data->glyphCount; ++i)
            prPathDestroy(data->outlines[i]);
        prMemoryFree(data->outlines);
        prMemoryFree(data->outlineBounds);
    }
    prMemoryFree(data->boundsFilled);
    prMemoryFree(data->advances);
    data->axes.reset();
    data->axisValues.reset();
    data->normalized.reset();
    data->features.reset();
    data->axisKey.reset();
    closeReaders(data);
    prMutexReset(&data->mutex);
    releaseData(data->data);
    prMemoryFree(data);
}

// Adds the font, just opened, to the list of its data, and returns it. If the list holds an equal font that is not
// being freed, frees the new font instead and returns a reference to that one.
const Font* intern(Font* font) {
    FontData* data = font->data;
    const Font* found = nullptr;
    prMutexLock(&data->lock);
    for (uint32_t i = 0; i < data->fonts.count && found == nullptr; ++i) {
        if (sameFont(data->fonts.data[i], font) && retainLive(data->fonts.data[i]))
            found = data->fonts.data[i];
    }
    if (found == nullptr)
        data->fonts.push(font);
    prMutexUnlock(&data->lock);
    if (found == nullptr)
        return font;
    releaseFont(font);
    return found;
}

// Returns the advance of the glyph of the design, in font units: from hmtx and HVAR, or from the phantom points of a
// TrueType outline that its variation moves.
float designAdvance(const Font* font, uint32_t glyph) {
    if (font->advances == nullptr) {
        // At the default instance, hmtx alone answers, and the advance of its last long metric repeats past them.
        const FontGlyphMetricTables& tables = font->metricTables;
        uint32_t count = tables.horizontalCount;
        if (count == 0)
            return 0;
        return float(tables.hmtx.uint16At((glyph < count ? glyph : count - 1) * 4));
    }
    uint32_t bits = font->advances[glyph].load();
    if (bits != kAdvanceUnknown)
        return bitCast<float>(bits);
    float units = 0;
    if (font->truetype && !fontGlyphMetricsVariesHorizontal(font->metricTables) && font->glyf.gvar.size != 0) {
        FontGlyphOutline outline{};
        GlyfPhantom phantom{};
        if (loadDesign(font, glyph, &outline, &phantom))
            units = phantom.horizontalAdvance.x - phantom.horizontalOrigin.x;
        fontOutlineReset(outline);
    } else {
        int32_t advance = 0;
        int32_t lsb = 0;
        fontGlyphMetricsHorizontal(font->metricTables, coordsOf(font), glyph, &advance, &lsb);
        units = float(advance);
    }
    font->advances[glyph].store(bitCast<uint32_t>(units));
    return units;
}

// Returns the hinting that the font's outlines take: none without a hinter.
PrFontHinting hintingOf(const Font* font) {
    return font->hinter != nullptr ? font->hinting : PrFontHinting_None;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

uint32_t prFontCount(PrByteView bytes) {
    return sfntFaceCount(bytes.data, bytes.size);
}

PrFontHinting prFontHintingFromName(PrStringView name) {
    for (PrFontHinting hinting = 0; hinting < PrFontHinting_Count; ++hinting) {
        if (prAsciiEqualsFold(kHintingNames[hinting], name))
            return hinting;
    }
    return PrFontHinting_Count;
}

PrStringView prFontHintingName(PrFontHinting hinting) {
    return hinting < PrFontHinting_Count ? kHintingNames[hinting] : PrStringView{};
}

// C opaque types
struct PrFont : Font {};

PrFontDerivation prFontDerivationDefault(void) {
    PrFontDerivation d{};
    d.sizeAdjust = 1;
    return d;
}

PrStatus prFontCreate(PrByteView bytes, uint32_t index, const PrFontDerivation* PR_NULLABLE derivation,
        const PrFont* PR_NULLABLE* PR_NONNULL out) {
    PR_ASSERT(validDerivation(derivation));
    FontData* copy = static_cast<FontData*>(prMemoryAllocZeroed(sizeof(FontData) + bytes.size));
    initData(copy);
    copy->bytes = reinterpret_cast<const uint8_t*>(copy + 1);
    copy->size = bytes.size;
    prMemCopy(copy + 1, bytes.data, bytes.size);
    PrFont* font = allocZeroed<PrFont>();
    *out = openDerived(font, copy, index, nullptr, derivation) ? static_cast<const PrFont*>(intern(font)) : nullptr;
    return *out != nullptr ? PrStatus_Ok : PrStatus_InvalidData;
}

PrStatus prFontCreateWoff(PrByteView bytes, uint32_t index, const PrFontDerivation* PR_NULLABLE derivation,
        PrCompressCodec* codec, const PrFont* PR_NULLABLE* PR_NONNULL out) {
    PR_ASSERT(validDerivation(derivation));
    *out = nullptr;
    RawArray<uint8_t> decoded;
    const PrStatus status = woffDecode(bytes, codec, &decoded);
    if (status != PrStatus_Ok) {
        decoded.reset();
        return status;
    }
    FontData* data = allocZeroed<FontData>();
    initData(data);
    data->owned = decoded.data;
    data->bytes = decoded.data;
    data->size = decoded.count;
    PrFont* font = allocZeroed<PrFont>();
    *out = openDerived(font, data, index, nullptr, derivation) ? static_cast<const PrFont*>(intern(font)) : nullptr;
    return *out != nullptr ? PrStatus_Ok : PrStatus_InvalidData;
}

PrStatus prFontCreateFromFile(PrStringView path, uint32_t index, const PrFontDerivation* PR_NULLABLE derivation,
        const PrFont* PR_NULLABLE* PR_NONNULL out) {
    PR_ASSERT(validDerivation(derivation));
    *out = nullptr;
    PrByteView mapping{};
    if (!prFileMap(path, &mapping))
        return PrStatus_Io;
    FontData* data = allocZeroed<FontData>();
    initData(data);
    data->bytes = mapping.data;
    data->size = mapping.size;
    data->mapping = mapping;
    PrFont* font = allocZeroed<PrFont>();
    *out = openDerived(font, data, index, nullptr, derivation) ? static_cast<const PrFont*>(intern(font)) : nullptr;
    return *out != nullptr ? PrStatus_Ok : PrStatus_InvalidData;
}

const PrFont* prFontCreateDerived(const PrFont* font, const PrFontDerivation* derivation) {
    PR_ASSERT(validDerivation(derivation));
    const Font* base = font;
    FontData* data = base->data;
    // If a font of this state is already open, it is the result, and no face is opened.
    Derived wanted{};
    derivedOf(base, base, derivation, &wanted);
    const Font* found = nullptr;
    prMutexLock(&data->lock);
    for (uint32_t i = 0; i < data->fonts.count && found == nullptr; ++i) {
        const Font* at = data->fonts.data[i];
        if (at->index == base->index && isDerived(at, wanted) && retainLive(at))
            found = at;
    }
    prMutexUnlock(&data->lock);
    derivedReset(&wanted);
    if (found != nullptr)
        return static_cast<const PrFont*>(found);
    data->references.fetchAdd(1);
    PrFont* derived = allocZeroed<PrFont>();
    bool opened = openDerived(derived, data, base->index, base, derivation);
    PR_ASSERT(opened);
    return static_cast<const PrFont*>(intern(derived));
}

void prFontRetain(const PrFont* font) {
    font->references.fetchAdd(1);
}

void prFontRelease(const PrFont* PR_NULLABLE font) {
    releaseFont(font);
}

uint64_t prFontId(const PrFont* font) {
    return font->id;
}

PrByteView prFontData(const PrFont* font) {
    const FontData* data = font->data;
    return PrByteView{bytesOf(data), data->size};
}

uint32_t prFontIndex(const PrFont* font) {
    return font->index;
}

PrRawString prFontFamilyName(const PrFont* font) {
    return sfntFamily(font->sfnt);
}

PrRawString prFontStyleName(const PrFont* font) {
    PrRawString name = sfntName(font->sfnt, 17);
    if (prRawStringIsNull(&name))
        name = sfntName(font->sfnt, 2);
    return name;
}

float prFontSizeAdjust(const PrFont* font) {
    return font->sizeAdjust;
}

PrFontHinting prFontHinting(const PrFont* font) {
    return hintingOf(font);
}

PrFontHinter* PR_NULLABLE prFontHinter(const PrFont* font) {
    return font->hinter;
}

bool prFontIsAliased(const PrFont* font) {
    return font->aliased;
}

float prFontEmbolden(const PrFont* font) {
    return font->embolden;
}

float prFontOblique(const PrFont* font) {
    return font->oblique;
}

uint32_t prFontFeatureCount(const PrFont* font) {
    return font->features.count;
}

const PrFontFeature* PR_NULLABLE prFontFeatures(const PrFont* font) {
    return font->features.data;
}

uint32_t prFontAxisCount(const PrFont* font) {
    return font->axes.count;
}

PrFontAxis prFontAxis(const PrFont* font, uint32_t index) {
    const Font* data = font;
    return index < data->axes.count ? data->axes.data[index] : PrFontAxis{0, 0, 0, 0};
}

float prFontAxisValue(const PrFont* font, uint32_t index) {
    const Font* data = font;
    return index < data->axisValues.count ? data->axisValues.data[index] : 0;
}

float prFontAxisCoordinate(const PrFont* font, uint32_t index) {
    const Font* data = font;
    return index < data->normalized.count ? data->normalized.data[index] : 0;
}

PrFontInfo prFontInfo(const PrFont* font) {
    return font->info;
}

PrFontMetrics prFontMetrics(const PrFont* font) {
    return font->metrics;
}

uint32_t prFontScriptTags(const PrFont* font, uint32_t* PR_NULLABLE out, uint32_t capacity) {
    return sfntScriptTags(font->sfnt, out, capacity);
}

uint32_t prFontGlyphCount(const PrFont* font) {
    return font->glyphCount;
}

uint16_t prFontCodePointGlyph(const PrFont* font, uint32_t codePoint) {
    const Font* data = font;
    uint32_t glyph = data->cmap.subtable.data != nullptr ? sfntCmapGlyph(data->cmap, codePoint) : 0;
    return glyph < data->glyphCount ? uint16_t(glyph) : 0;
}

float prFontGlyphVerticalAdvance(const PrFont* font, uint16_t glyph) {
    const Font* data = font;
    if (glyph >= data->glyphCount)
        return 0;
    const FontGlyphMetricTables& tables = data->metricTables;
    float units = 0;
    // A TrueType font whose variation moves its phantom points, without VVAR, finds the advance by loading the glyph.
    if (data->truetype && data->normalized.count != 0 && !fontGlyphMetricsVariesVertical(tables) &&
            tables.verticalCount != 0 && data->glyf.gvar.size != 0) {
        FontGlyphOutline outline{};
        GlyfPhantom phantom{};
        if (loadDesign(data, glyph, &outline, &phantom))
            units = prF32Max(phantom.verticalOrigin.y - phantom.verticalAdvance.y, 0);
        fontOutlineReset(outline);
    } else {
        int32_t advance = 0;
        int32_t tsb = 0;
        fontGlyphMetricsVertical(tables, coordsOf(data), glyph, 0, &advance, &tsb);
        units = float(advance);
    }
    return units / float(data->unitsPerEm) * data->sizeAdjust;
}

PrVec2 prFontGlyphVerticalOrigin(const PrFont* font, uint16_t glyph) {
    const Font* data = font;
    if (glyph >= data->glyphCount)
        return PrVec2{0, 0};
    FontGlyphOutline outline{};
    GlyfPhantom phantom{};
    PrVec2 origin{0, 0};
    if (loadDesign(data, glyph, &outline, &phantom)) {
        // The vertical origin lies half the horizontal advance across, and the top side bearing above the glyph's top.
        PrRect box = fontOutlineControlBox(outline);
        float advance = data->truetype ? phantom.horizontalAdvance.x - phantom.horizontalOrigin.x : designAdvance(data, glyph);
        const FontGlyphMetricTables& tables = data->metricTables;
        float top = 0;
        // The bearing and the height round to whole units, as FreeType divides them out of its scaled coordinates.
        if (tables.verticalCount != 0 && data->truetype) {
            top = prF32Round(phantom.verticalOrigin.y - box.bottom);
        } else if (tables.verticalCount != 0) {
            int32_t height = 0;
            int32_t tsb = 0;
            fontGlyphMetricsVertical(tables, coordsOf(data), glyph, 0, &height, &tsb);
            top = float(tsb);
        } else {
            int32_t extent = int32_t(tables.ascender) - tables.descender;
            top = float((extent - int32_t(prF32Round(box.bottom - box.top))) / 2);
        }
        float k = data->sizeAdjust / float(data->unitsPerEm);
        origin = PrVec2{advance / 2 * k, -(box.bottom + top) * k};
    }
    fontOutlineReset(outline);
    return origin;
}

PrRect prFontGlyphBounds(const PrFont* font, uint16_t glyph) {
    const Font* data = font;
    if (glyph >= data->glyphCount)
        return PrRect{0, 0, 0, 0};
    PrRect bounds{};
    if (data->boundsFilled[glyph].load() != 0) {
        bounds = data->outlineBounds[glyph];
    } else {
        prMutexLock(&data->mutex);
        cachedOutline(data, glyph);
        bounds = data->outlineBounds[glyph];
        prMutexUnlock(&data->mutex);
    }
    if (prRectIsEmpty(bounds))
        return PrRect{0, 0, 0, 0};
    float k = data->sizeAdjust;
    float e = data->embolden;
    return PrRect{bounds.left * k - e, bounds.top * k - e, bounds.right * k + e, bounds.bottom * k + e};
}

bool prFontGlyphOutline(const PrFont* font, uint16_t glyph, float pixelsPerEm, PrAffine2 transform, PrPath* out) {
    const Font* data = font;
    if (glyph >= data->glyphCount)
        return false;
    float embolden = data->embolden;
    PrFontHinting hinting = pixelsPerEm > 0 ? hintingOf(data) : PrFontHinting_None;
    // The ems of the design scale by the size-adjust. A hinted outline fits the size at which its em is drawn.
    float k = data->sizeAdjust;
    PrAffine2 scaled = prAffine2Concat(prAffine2Scale(PrVec2{k, k}), transform);
    PrPath* outline = prPathCreate();
    bool hinted = hinting != PrFontHinting_None &&
        prFontHinterOutline(data->hinter, font, glyph, pixelsPerEm * k, hinting, embolden / k, outline);
    if (!hinted && embolden == 0) {
        prMutexLock(&data->mutex);
        const PrPath* cached = cachedOutline(data, glyph);
        if (prPathVerbCount(cached) != 0)
            prPathAddPath(out, cached, scaled);
        bool any = prPathVerbCount(cached) != 0;
        prMutexUnlock(&data->mutex);
        prPathDestroy(outline);
        return any;
    }
    if (!hinted)
        appendDesign(data, glyph, 2 * embolden / k * float(data->unitsPerEm), outline);
    bool any = prPathVerbCount(outline) != 0;
    if (any)
        prPathAddPath(out, outline, scaled);
    prPathDestroy(outline);
    return any;
}

float prFontGlyphAdvance(const PrFont* font, uint16_t glyph, float pixelsPerEm) {
    const Font* data = font;
    if (glyph >= data->glyphCount)
        return 0;
    float k = data->sizeAdjust;
    float design = designAdvance(data, glyph) / float(data->unitsPerEm) * k;
    if (hintingOf(data) != PrFontHinting_Full || !(pixelsPerEm > 0))
        return design;
    // The hinted size is the size of the em of the design, which the size-adjust scales to the em of the font.
    float pixels = 0;
    if (!prFontHinterAdvance(data->hinter, font, glyph, pixelsPerEm * k, PrFontHinting_Full, &pixels))
        return design;
    return pixels / pixelsPerEm;
}

uint32_t prFontPaletteCount(const PrFont* font) {
    return colrPaletteCount(font->colr);
}

PrFontPaletteFlags prFontPaletteFlags(const PrFont* font, uint32_t palette) {
    // In CPAL version 1, the color indices of the palettes are followed by an offset to their types.
    SfntTable cpal = sfntTable(font->sfnt, tagOf('C', 'P', 'A', 'L'));
    uint16_t count = cpal.uint16At(4);
    if (cpal.uint16At(0) < 1 || palette >= count)
        return 0;
    uint32_t types = cpal.uint32At(12 + uint32_t(count) * 2);
    if (types == 0)
        return 0;
    uint32_t type = cpal.uint32At(types + palette * 4);
    return PrFontPaletteFlags(((type & 1u) != 0 ? PrFontPaletteFlag_Light : 0) |
        ((type & 2u) != 0 ? PrFontPaletteFlag_Dark : 0));
}

uint32_t prFontPaletteSelect(const PrFont* font, PrFontPaletteFlags flags) {
    uint32_t count = prFontPaletteCount(font);
    for (uint32_t i = 0; i < count; ++i) {
        if ((prFontPaletteFlags(font, i) & flags) != 0)
            return i;
    }
    return 0;
}

uint32_t prFontPaletteEntryCount(const PrFont* font) {
    return colrPaletteEntryCount(font->colr);
}

PrColor prFontPaletteEntry(const PrFont* font, const PrFontPalette* PR_NULLABLE palette, uint32_t index) {
    PR_ASSERT(index < prFontPaletteEntryCount(font));
    for (uint32_t i = palette != nullptr ? palette->colorCount : 0; i > 0; --i) {
        if (palette->colors[i - 1].index == index)
            return palette->colors[i - 1].color;
    }
    return colrPaletteEntry(font->colr, palette != nullptr ? palette->index : 0, index);
}

bool prFontGlyphHasColor(const PrFont* font, uint16_t glyph) {
    return glyph < font->glyphCount && colrHasGlyph(font->colr, glyph);
}

bool prFontGlyphPaint(const PrFont* font, uint16_t glyph, const PrFontPalette* PR_NULLABLE palette,
        PrColor foreground, const PrFontPainter* painter) {
    const Font* data = font;
    if (glyph >= data->glyphCount)
        return false;
    ColrWalk w{};
    w.palette = palette != nullptr ? palette->index : 0;
    w.foreground = foreground;
    w.unitsPerEm = float(data->unitsPerEm) / data->sizeAdjust;
    if (palette != nullptr) {
        w.overrides = palette->colors;
        w.overrideCount = palette->colorCount;
    }
    bool painted = colrWalk(data->colr, coordsOf(data), glyph, &w);
    if (painted)
        replay(w, painter);
    w.steps.reset();
    w.stops.reset();
    return painted;
}

bool prFontGlyphImage(const PrFont* font, uint16_t glyph, float pixelsPerEm, PrFontGlyphImage* out) {
    const Font* data = font;
    // The pixels of the strike are placed on the device in the same way as the ems of the design, which the size-adjust
    // scales.
    float k = data->sizeAdjust;
    SfntImage image{};
    if (!data->strikes || glyph >= data->glyphCount || !sfntGlyphImage(data->sfnt, glyph, pixelsPerEm / k, &image))
        return false;
    float unit = k / image.pixelsPerEm;
    out->bytes = PrByteView{image.data, image.size};
    out->type = image.type == SfntImageType_Png    ? "image/png" : image.type == SfntImageType_Jpeg ? "image/jpeg" : "image/tiff";
    out->extent.x = image.width;
    out->extent.y = image.height;
    out->pixelsPerEm = image.pixelsPerEm;
    out->bounds = PrRect{
        image.left * unit, image.top * unit,
        (image.left + float(image.width)) * unit,
        (image.top + float(image.height)) * unit,
    };
    return true;
}

bool prFontGlyphSvg(const PrFont* font, uint16_t glyph, PrFontGlyphSvg* out) {
    const Font* data = font;
    SfntTable document{};
    uint32_t index = 0;
    if (!sfntGlyphSvg(data->sfnt, glyph, &document, &index))
        return false;
    out->document = PrByteView{document.data, document.size};
    out->index = index;
    out->scale = data->sizeAdjust / float(data->unitsPerEm);
    return true;
}
