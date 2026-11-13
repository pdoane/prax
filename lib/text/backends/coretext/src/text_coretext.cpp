// The CoreText shaper, over a CTFont made from the font's bytes.
//
// A face is the graphics font of a face of a font file, which PrTextShaper keeps. Each font keeps a CTFont over a copy
// of that graphics font at the design point of the font, at its units per em, so the positions that CoreText gives are
// in font units. A run is laid out as a line whose writing direction is forced to the direction of the run. The glyphs
// of the line are read back run by run in visual order.

#include "pr/text_coretext.h"

#include "pr/alloc.h"
#include "pr/apple_string.h"
#include "pr/raw_array.h"
#include "pr/sort.h"
#include "pr/utf8.h"
#include "pr/vec.h"

#include "text/backends/shared/src/text_shared_font_cache.h"

#include <CoreText/CoreText.h>

namespace pr_impl {

namespace {

CFNumberRef numberOf(uint32_t value) {
    int64_t wide = value;
    return CFNumberCreate(nullptr, kCFNumberSInt64Type, &wide);
}

struct CtFace {
    CGFontRef graphics;
};

struct CtFont {
    CTFontRef ct;
    CFStringRef name;                   // the PostScript name, which a run in another face does not share
    float unitsPerEm;
};

bool createFace(PrTextShaper*, const PrFont* font, void* state) {
    CtFace* out = static_cast<CtFace*>(state);
    PrByteView bytes = prFontData(font);
    CFDataRef data = CFDataCreateWithBytesNoCopy(nullptr, bytes.data, CFIndex(bytes.size), kCFAllocatorNull);
    CFArrayRef descriptors = CTFontManagerCreateFontDescriptorsFromData(data);
    CFRelease(data);
    if (descriptors == nullptr)
        return false;
    if (CFIndex(prFontIndex(font)) < CFArrayGetCount(descriptors)) {
        CTFontDescriptorRef descriptor = static_cast<CTFontDescriptorRef>(
            CFArrayGetValueAtIndex(descriptors, CFIndex(prFontIndex(font))));
        CTFontRef face = CTFontCreateWithFontDescriptor(descriptor, 0, nullptr);
        out->graphics = CTFontCopyGraphicsFont(face, nullptr);
        CFRelease(face);
    }
    CFRelease(descriptors);
    return out->graphics != nullptr;
}

void destroyFace(PrTextShaper*, void* state) {
    CFRelease(static_cast<CtFace*>(state)->graphics);
}

// Creates the font at its design point. Through the variation of a font descriptor, CoreText makes no usable font of a
// design point between the named instances of a variable font, and it starts a font made from bytes at a design point
// other than the default. A copy of the graphics font takes the value of every axis, by axis name, in the order in
// which the font lists the axes.
void createFont(PrTextShaper*, void* face, const PrFont* font, void* state) {
    CtFont* out = static_cast<CtFont*>(state);
    out->unitsPerEm = float(prFontMetrics(font).unitsPerEm);
    CGFontRef graphics = static_cast<CtFace*>(face)->graphics;
    CFRetain(graphics);
    if (prFontAxisCount(font) != 0) {
        CFArrayRef axes = CGFontCopyVariationAxes(graphics);
        CFIndex axisCount = axes != nullptr ? CFArrayGetCount(axes) : 0;
        CFMutableDictionaryRef values = CFDictionaryCreateMutable(nullptr, axisCount, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        for (CFIndex i = 0; i < axisCount && i < CFIndex(prFontAxisCount(font)); ++i) {
            CFDictionaryRef axis = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(axes, i));
            double value = double(prFontAxisValue(font, uint32_t(i)));
            CFNumberRef number = CFNumberCreate(nullptr, kCFNumberDoubleType, &value);
            CFDictionarySetValue(values, CFDictionaryGetValue(axis, kCGFontVariationAxisName), number);
            CFRelease(number);
        }
        CGFontRef varied = CGFontCreateCopyWithVariations(graphics, values);
        CFRelease(values);
        if (axes != nullptr)
            CFRelease(axes);
        if (varied != nullptr) {
            CFRelease(graphics);
            graphics = varied;
        }
    }
    out->ct = CTFontCreateWithGraphicsFont(graphics, CGFloat(out->unitsPerEm), nullptr, nullptr);
    CFRelease(graphics);
    out->name = CTFontCopyPostScriptName(out->ct);
}

void destroyFont(PrTextShaper*, void* state) {
    CtFont* font = static_cast<CtFont*>(state);
    CFRelease(font->name);
    CFRelease(font->ct);
}

// A copy of the font of the run, with the features of the run in `mask` set.
struct FeaturedFont {
    uint64_t mask;
    CTFontRef ct;
};

struct CtBackend : PrTextShaper {
    TextSharedFontCache fonts;
    RawArray<UniChar> utf16;
    RawArray<uint32_t> byteOf;             // for each UTF-16 unit, its byte in the text of the run
    RawArray<CGGlyph> glyphs;
    RawArray<CGPoint> positions;
    RawArray<CFIndex> indices;
    RawArray<CFIndex> breaks;              // the offsets at which the set of features over the text changes
    RawArray<FeaturedFont> featured;       // the fonts of the run with features, emptied after the run
};

// Converts the text of the run to UTF-16, recording the byte offset of each unit.
void toUtf16(CtBackend* c, PrStringView text) {
    c->utf16.clear();
    c->byteOf.clear();
    size_t at = 0;
    while (at < text.size) {
        uint32_t codePoint = 0;
        uint32_t length = prUtf8Decode(text, at, &codePoint);
        if (codePoint >= 0x10000) {
            codePoint -= 0x10000;
            c->utf16.push(UniChar(0xd800 + (codePoint >> 10)));
            c->utf16.push(UniChar(0xdc00 + (codePoint & 0x3ff)));
            c->byteOf.push(uint32_t(at));
        } else {
            c->utf16.push(UniChar(codePoint));
        }
        c->byteOf.push(uint32_t(at));
        at += length;
    }
}

// Returns the UTF-16 range of the bytes [start, end) of the text.
CFRange rangeOf(const CtBackend* c, uint32_t start, uint32_t end) {
    CFIndex from = CFIndex(c->byteOf.count);
    CFIndex to = CFIndex(c->byteOf.count);
    for (uint32_t i = 0; i < c->byteOf.count; ++i) {
        if (from == CFIndex(c->byteOf.count) && c->byteOf.data[i] >= start)
            from = CFIndex(i);
        if (c->byteOf.data[i] >= end) {
            to = CFIndex(i);
            break;
        }
    }
    return CFRangeMake(from, to > from ? to - from : 0);
}

// Returns the font with the features of the run in `mask`, made once for each set of features that the run uses.
CTFontRef featuredFont(CtBackend* c, const PrTextRun* run, CTFontRef font, uint64_t mask) {
    for (uint32_t i = 0; i < c->featured.count; ++i) {
        if (c->featured.data[i].mask == mask)
            return c->featured.data[i].ct;
    }
    CFMutableArrayRef settings = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    for (uint32_t i = 0; i < 64; ++i) {
        if ((mask & (uint64_t(1) << i)) == 0)
            continue;
        const PrTextFeature& f = run->features[i];
        char tag[4] = {char(f.tag >> 24), char(f.tag >> 16), char(f.tag >> 8), char(f.tag)};
        CFStringRef name = prAppleStringCreate(PrStringView{tag, 4});
        CFNumberRef value = numberOf(f.value);
        const void* keys[] = {kCTFontOpenTypeFeatureTag, kCTFontOpenTypeFeatureValue};
        const void* values[] = {name, value};
        CFDictionaryRef setting = CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CFArrayAppendValue(settings, setting);
        CFRelease(setting);
        CFRelease(value);
        CFRelease(name);
    }
    const void* attributeKeys[] = {kCTFontFeatureSettingsAttribute};
    const void* attributeValues[] = {settings};
    CFDictionaryRef attributes = CFDictionaryCreate(nullptr, attributeKeys, attributeValues, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CTFontDescriptorRef descriptor = CTFontDescriptorCreateWithAttributes(attributes);
    CTFontRef featured = CTFontCreateCopyWithAttributes(font, 0, nullptr, descriptor);
    CFRelease(descriptor);
    CFRelease(attributes);
    CFRelease(settings);
    c->featured.push(FeaturedFont{mask, featured});
    return featured;
}

CFAttributedStringRef createText(CtBackend* c, const PrTextRun* run, CTFontRef font) {
    CFStringRef string = CFStringCreateWithCharactersNoCopy(nullptr, c->utf16.data, CFIndex(c->utf16.count),
        kCFAllocatorNull);
    CFMutableAttributedStringRef text = CFAttributedStringCreateMutable(nullptr, 0);
    CFAttributedStringReplaceString(text, CFRangeMake(0, 0), string);
    CFRelease(string);
    CFRange all = CFRangeMake(0, CFIndex(c->utf16.count));
    CFAttributedStringSetAttribute(text, all, kCTFontAttributeName, font);
    // The run already has one direction, so this overrides what the bidi algorithm would resolve.
    bool rtl = run->direction == PrTextDirection_Rtl;
    int32_t level = int32_t(rtl ? kCTWritingDirectionRightToLeft : kCTWritingDirectionLeftToRight) |
        int32_t(kCTWritingDirectionOverride);
    CFNumberRef number = CFNumberCreate(nullptr, kCFNumberSInt32Type, &level);
    CFArrayRef levels = CFArrayCreate(nullptr, reinterpret_cast<const void**>(&number), 1, &kCFTypeArrayCallBacks);
    CFAttributedStringSetAttribute(text, all, kCTWritingDirectionAttributeName, levels);
    // A run down the line takes vertical forms and vertical metrics, which CoreText applies in its vertical layout.
    if (run->axis == PrTextAxis_Vertical)
        CFAttributedStringSetAttribute(text, all, kCTVerticalFormsAttributeName, kCFBooleanTrue);
    CFRelease(levels);
    CFRelease(number);
    if (run->locale != 0) {
        char tag[kPrLocaleMaxTag];
        uint32_t size = prLocaleTag(run->locale, tag);
        CFStringRef language = prAppleStringCreate(PrStringView{tag, size});
        if (language != nullptr) {
            CFAttributedStringSetAttribute(text, all, kCTLanguageAttributeName, language);
            CFRelease(language);
        }
    }
    // Each stretch of text between the ends of features gets a font with every feature that covers it.
    uint32_t featureCount = run->featureCount < 64 ? run->featureCount : 64;
    c->breaks.clear();
    for (uint32_t i = 0; i < featureCount; ++i) {
        CFRange range = rangeOf(c, run->features[i].start, run->features[i].end);
        c->breaks.push(range.location);
        c->breaks.push(range.location + range.length);
    }
    sortItems(c->breaks.span(), [](CFIndex a, CFIndex b) { return a < b; });
    for (uint32_t b = 0; b + 1 < c->breaks.count; ++b) {
        CFIndex from = c->breaks.data[b];
        CFIndex to = c->breaks.data[b + 1];
        if (from == to)
            continue;
        uint64_t mask = 0;
        for (uint32_t i = 0; i < featureCount; ++i) {
            CFRange range = rangeOf(c, run->features[i].start, run->features[i].end);
            if (range.location <= from && from < range.location + range.length)
                mask |= uint64_t(1) << i;
        }
        if (mask != 0)
            CFAttributedStringSetAttribute(text, CFRangeMake(from, to - from), kCTFontAttributeName,
                featuredFont(c, run, font, mask));
    }
    return text;
}

void destroyBackend(PrTextShaper* shaper) {
    CtBackend* c = static_cast<CtBackend*>(shaper);
    textSharedFontCacheReset(shaper, &c->fonts);
    c->utf16.reset();
    c->byteOf.reset();
    c->glyphs.reset();
    c->positions.reset();
    c->indices.reset();
    c->breaks.reset();
    c->featured.reset();
    prMemoryFree(c);
}

PrStatus shape(PrTextShaper* shaper, void*, void* fontState, const PrTextRun* run, PrTextGlyphBuffer* out) {
    CtBackend* c = static_cast<CtBackend*>(shaper);
    CtFont* font = static_cast<CtFont*>(fontState);
    bool vertical = run->axis == PrTextAxis_Vertical;
    toUtf16(c, run->text);
    CFAttributedStringRef text = createText(c, run, font->ct);
    CTLineRef line = CTLineCreateWithAttributedString(text);
    CFRelease(text);
    CFArrayRef runs = CTLineGetGlyphRuns(line);
    CFIndex runCount = CFArrayGetCount(runs);
    float scale = 1 / font->unitsPerEm;
    // The runs are in visual order along the line, and so are the glyphs of each run. CoreText places each glyph from
    // the origin of the line. Here, each advance runs to the x of the next glyph, and the advance of the last glyph
    // runs to the end of its run. The offset keeps the remainder, on the baseline. A line down places its glyphs from
    // `down`, the distance of its pen from its start.
    double runStart = 0;
    double down = 0;
    for (CFIndex r = 0; r < runCount; ++r) {
        CTRunRef glyphRun = static_cast<CTRunRef>(CFArrayGetValueAtIndex(runs, r));
        CFIndex count = CTRunGetGlyphCount(glyphRun);
        c->glyphs.clear();
        c->glyphs.reserve(uint32_t(count));
        c->positions.clear();
        c->positions.reserve(uint32_t(count));
        c->indices.clear();
        c->indices.reserve(uint32_t(count));
        CTRunGetGlyphs(glyphRun, CFRangeMake(0, 0), c->glyphs.data);
        CTRunGetPositions(glyphRun, CFRangeMake(0, 0), c->positions.data);
        CTRunGetStringIndices(glyphRun, CFRangeMake(0, 0), c->indices.data);
        // A run in a face other than the face of the font is a CoreText fallback, whose glyphs the font does not have.
        // The font of the run is the font itself, a copy with features, or a font that CoreText made for the same face.
        CTFontRef runFont = static_cast<CTFontRef>(
            CFDictionaryGetValue(CTRunGetAttributes(glyphRun), kCTFontAttributeName));
        bool fallback = runFont == nullptr;
        if (runFont != nullptr && runFont != font->ct) {
            CFStringRef name = CTFontCopyPostScriptName(runFont);
            fallback = !CFEqual(name, font->name);
            CFRelease(name);
        }
        double runEnd = runStart + CTRunGetTypographicBounds(glyphRun, CFRangeMake(0, 0), nullptr, nullptr, nullptr);
        PrTextGlyph* glyphs = prTextGlyphBufferAppend(out, uint32_t(count));
        double pen = runStart;
        for (CFIndex i = 0; i < count; ++i) {
            CGPoint p = c->positions.data[i];
            double next = i + 1 < count ? c->positions.data[i + 1].x : runEnd;
            PrTextGlyph& g = glyphs[i];
            g.id = fallback ? 0 : c->glyphs.data[i];
            CFIndex index = c->indices.data[i];
            g.cluster = index < CFIndex(c->byteOf.count) ? c->byteOf.data[index] : 0;
            if (vertical) {
                // The vertical layout of CoreText places the horizontal origin of each glyph from the start of the
                // line, on its center with y up, and gives the advance down as the width of the advance.
                CGSize advance{};
                CTRunGetAdvances(glyphRun, CFRangeMake(i, 1), &advance);
                g.offset = PrVec2{float(p.x) * scale, float(-p.y - down) * scale};
                g.advance = PrVec2{0, float(advance.width) * scale};
                down += advance.width;
            } else {
                g.offset = PrVec2{float(p.x - pen) * scale, -float(p.y) * scale};
                g.advance = PrVec2{float(next - pen) * scale, 0};
            }
            pen = next;
        }
        runStart = runEnd;
    }
    CFRelease(line);
    for (uint32_t i = 0; i < c->featured.count; ++i)
        CFRelease(c->featured.data[i].ct);
    c->featured.clear();
    return PrStatus_Ok;
}

constexpr TextSharedFontStates kStates = {
    .faceSize = sizeof(CtFace),
    .fontSize = sizeof(CtFont),
    .createFace = createFace,
    .destroyFace = destroyFace,
    .createFont = createFont,
    .destroyFont = destroyFont,
    .shape = shape,
};

PrStatus shapeRun(PrTextShaper* shaper, const PrTextRun* run, PrTextGlyphBuffer* out) {
    return textSharedFontCacheShape(shaper, &static_cast<CtBackend*>(shaper)->fonts, run, out);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrTextShaper* prTextCoreTextCreate(void) {
    CtBackend* c = allocZeroed<CtBackend>();
    c->destroyFn = destroyBackend;
    c->shapeFn = shapeRun;
    c->fonts.states = &kStates;
    return c;
}
