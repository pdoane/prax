// A Lottie animation's text: documents laid out into letters once, and drawn letter by letter under their animators.
//
// lottie-spec - https://lottie.github.io/lottie-spec/
//
// Each paragraph is shaped as a whole, so that letters take the forms and kerning that their neighbors give them. Each
// letter is the glyphs of its grapheme cluster, which animators move independently. A ligature belongs to the letter
// that it starts in. A document whose glyphs the animation draws uses the animation's glyph shapes. Any other document
// is set in the host's fonts.

#include "lottie/src/lottie_text.h"

#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/locale_common.h"
#include "pr/search.h"
#include "pr/sort.h"
#include "pr/string_ops.h"
#include "pr/text.h"
#include "pr/text_font.h"
#include "pr/text_itemize.h"
#include "pr/unicode_grapheme.h"
#include "pr/vec.h"

#include "lottie/src/lottie_eval.h"
#include "lottie/src/lottie_render.h"
#include "lottie/src/lottie_shape.h"

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Layout

// Reads the weight and slope that a font's style name specifies, from the words in it.
void styleOf(PrStringView style, float* weight, PrFontSlope* slope) {
    *weight = 400;
    *slope = PrFontSlope_Normal;
    size_t at = 0;
    while (at < style.size) {
        size_t end = at;
        while (end < style.size && style.data[end] != ' ')
            ++end;
        PrStringView word = prStringSlice(style, at, end - at);
        if (prAsciiEqualsFold(word, "italic"))
            *slope = PrFontSlope_Italic;
        else if (prAsciiEqualsFold(word, "bold"))
            *weight = 700;
        else if (prAsciiEqualsFold(word, "black"))
            *weight = 900;
        else if (prAsciiEqualsFold(word, "medium"))
            *weight = 500;
        else if (prAsciiEqualsFold(word, "regular") || prAsciiEqualsFold(word, "normal"))
            *weight = 400;
        else if (prAsciiEqualsFold(word, "light") || prAsciiEqualsFold(word, "thin"))
            *weight = 200;
        at = end + 1;
    }
}

// Returns the host's text font for the font's family and style, falling back to the default family (pr/text_font.h).
// Returns null if the host gives no fonts.
PrTextFont* PR_NULLABLE textFontOf(LottieAnimation& a, const LottieFont& font, const PrLottieOptions& options) {
    if (options.fonts == nullptr)
        return nullptr;
    PrFontStyle style = prFontStyleDefault();
    styleOf(lottieString(a, font.style), &style.weight, &style.slope);
    PrFontFamily family = prFontFamilyFromName(lottieString(a, font.family));
    PrTextFontDesc desc{
        .set = options.fonts,
        .families = &family,
        .familyCount = family.name.size != 0 ? 1u : 0u,
        .style = style,
    };
    return prTextFontCreate(&desc);
}

// A glyph shape's character and its position among the animation's glyph shapes, which sort by character and then by
// position.
struct CharKey {
    PrStringView character;
    uint32_t shape;

    bool operator<(PrStringView other) const { return prStringCompare(character, other) < 0; }
};

// A measured character: its advance at the document's size, and what draws it.
struct Measured {
    float width;
    float origin;                       // the pen position in the paragraph at the first glyph, in ems
    uint32_t glyphs;                    // in Layout::measuredGlyphs, relative to the origin
    uint32_t glyphCount;
    bool scaled;                        // true if the shaper synthesized the glyph at another size
    uint32_t font;                      // in the animation's fonts
    uint32_t shape;                     // a glyph that the animation draws itself; kLottieNone for none
};

// A face of a text font that a run is set in, and the face's font in the animation's fonts.
struct FaceFont {
    uint32_t textFont;
    uint32_t face;
    uint32_t font;
};

struct Layout {
    LottieAnimation* a;
    const PrLottieOptions* options;
    PrTextGlyphBuffer* glyphs;
    Array<PrTextFont* PR_NULLABLE> textFonts; // for each of the animation's fonts, or null
    Array<FaceFont> faceFonts;                // the faces of the text fonts that runs are set in, and their fonts
    PrTextItems* PR_NULLABLE items;           // a paragraph's runs
    Array<CharKey> shapes;                    // the glyph shapes, sorted
    Array<Measured> measured;                 // the document's characters
    Array<PrFontGlyph> measuredGlyphs;
    Array<PrAffine2> measuredTransforms;     // one per measured glyph, about its origin
};

// Returns the glyph shape that the animation provides for the character in the font, taking the first one listed. If
// the font has none, a shape from any font is used.
const LottieCharShape* PR_NULLABLE charShapeFor(const Layout& t, PrStringView character, const LottieFont* PR_NULLABLE font) {
    const LottieAnimation& a = *t.a;
    for (uint32_t at = lowerBound(t.shapes.data, t.shapes.count, character);
            at < t.shapes.count && t.shapes.data[at].character == character; ++at) {
        const LottieCharShape& c = a.charShapes.data[t.shapes.data[at].shape];
        if (font == nullptr || (lottieString(a, c.style) == lottieString(a, font->style) &&
                lottieString(a, c.family) == lottieString(a, font->family)))
            return &c;
    }
    return nullptr;
}

bool isNewline(PrStringView character) {
    return character.size == 1 && (character.data[0] == '\r' || character.data[0] == 3);
}

// Returns the index in the animation's fonts of face `face` of text font `textFont`. The animation holds a reference to
// that font.
uint32_t fontOfFace(Layout& t, uint32_t textFont, uint32_t face) {
    for (uint32_t i = 0; i < t.faceFonts.count; ++i) {
        const FaceFont& known = t.faceFonts.data[i];
        if (known.textFont == textFont && known.face == face)
            return known.font;
    }
    const PrFont* font = prTextFontMatchedFont(t.textFonts.data[textFont], face);
    PR_ASSERT(font != nullptr);
    prFontRetain(font);
    t.a->fonts.push(font);
    t.faceFonts.push(FaceFont{textFont, face, t.a->fonts.count - 1});
    return t.a->fonts.count - 1;
}

// Measures the document's characters into `t.measured`. Each paragraph between line breaks is shaped as one run, and
// each glyph goes to the character that its cluster is in.
void measureCharacters(Layout& t, const LottieTextDocument& d, PrStringView text, const Array<LottieString>& chars) {
    LottieAnimation& a = *t.a;
    const LottieFont* font = d.font != kLottieNone ? &a.textFonts.data[d.font] : nullptr;
    t.measured.clear();
    t.measuredGlyphs.clear();
    t.measuredTransforms.clear();
    for (uint32_t i = 0; i < chars.count; ++i)
        t.measured.push(Measured{0, 0, 0, 0, false, 0, kLottieNone});
    auto charAt = [&](uint32_t i) { return prStringSlice(text, chars.data[i].offset, chars.data[i].size); };
    if (a.charShapes.count > 0) {
        for (uint32_t i = 0; i < chars.count; ++i) {
            if (isNewline(charAt(i)))
                continue;
            const LottieCharShape* shape = charShapeFor(t, charAt(i), font);
            t.measured.data[i].shape = shape != nullptr ? uint32_t(shape - a.charShapes.data) : kLottieNone;
            t.measured.data[i].width = shape != nullptr ? shape->width * d.size / 100 : 0;
        }
        return;
    }
    PrTextFont* PR_NULLABLE textFont = font != nullptr ? t.textFonts.data[d.font] : nullptr;
    if (textFont == nullptr || t.options->shaper == nullptr)
        return;
    for (uint32_t first = 0; first < chars.count;) {
        if (isNewline(charAt(first))) {
            ++first;
            continue;
        }
        uint32_t end = first;
        while (end < chars.count && !isNewline(charAt(end)))
            ++end;
        uint32_t start = chars.data[first].offset;
        PrStringView paragraph = prStringSlice(text, start, chars.data[end - 1].offset + chars.data[end - 1].size - start);
        // The paragraph's runs (pr/text_itemize.h), set left to right, which is how the animation places its
        // characters.
        PrTextPiece piece{};
        piece.kind = PrTextPieceKind_Text;
        piece.end = uint32_t(paragraph.size);
        piece.font = textFont;
        PrTextItemizeDesc desc{};
        desc.text = paragraph;
        desc.pieces = &piece;
        desc.pieceCount = 1;
        if (t.items == nullptr)
            t.items = prTextItemsCreate();
        prTextItemsItemize(t.items, &desc);
        // Shapes each range in the font of its face, left to right, with the surrounding paragraph as context.
        uint32_t letter = first;
        float pen = 0;
        const PrTextItemRun* ranges = prTextItemsRuns(t.items);
        for (uint32_t r = 0; r < prTextItemsRunCount(t.items); ++r) {
            const PrTextItemRun& range = ranges[r];
            if (range.face == kPrTextMatchNone)
                continue;
            uint32_t faceFont = fontOfFace(t, d.font, range.face);
            PrTextRun run{};
            run.font = t.a->fonts.data[faceFont];
            run.text = PrStringView{paragraph.data + range.start, range.end - range.start};
            run.before = PrStringView{paragraph.data + range.contextStart, range.start - range.contextStart};
            run.after = PrStringView{paragraph.data + range.end, range.contextEnd - range.end};
            run.direction = PrTextDirection_Ltr;
            run.script = range.script;
            if (prTextShaperShape(t.options->shaper, &run, t.glyphs) != PrStatus_Ok)
                continue;
            uint32_t count = prTextGlyphBufferGlyphCount(t.glyphs);
            const PrTextGlyph* glyphs = prTextGlyphBufferGlyphs(t.glyphs);
            for (uint32_t g = 0; g < count; ++g) {
                // Clusters ascend in a run shaped left to right, so a glyph's letter is at or after the previous
                // glyph's letter.
                uint32_t cluster = start + range.start + glyphs[g].cluster;
                while (letter + 1 < end && chars.data[letter + 1].offset <= cluster)
                    ++letter;
                Measured& m = t.measured.data[letter];
                if (m.glyphCount == 0) {
                    m.origin = pen;
                    m.glyphs = t.measuredGlyphs.count;
                    m.font = faceFont;
                }
                t.measuredGlyphs.push(PrFontGlyph{
                    glyphs[g].id, PrVec2{(pen - m.origin + glyphs[g].offset.x) * d.size, glyphs[g].offset.y * d.size},
                });
                t.measuredTransforms.push(prAffine2Scale(PrVec2{glyphs[g].scale, glyphs[g].scale}));
                m.scaled = m.scaled || glyphs[g].scale != 1;
                ++m.glyphCount;
                m.width += glyphs[g].advance.x * d.size;
                pen += glyphs[g].advance.x;
            }
        }
        first = end;
    }
}

// Splits the document's text into the characters that animators move: grapheme clusters, with each line break as a
// character of its own. Appends them to `out` as runs of `text`.
void characters(PrStringView text, Array<LottieString>& out) {
    size_t at = 0;
    while (at < text.size) {
        size_t next = prUnicodeGraphemeNext(text, at);
        if (next <= at)
            next = at + 1;
        // Here, a carriage return before a line feed is its own cluster.
        if (text.data[at] == '\r' && next > at + 1)
            next = at + 1;
        out.push(LottieString{uint32_t(at), uint32_t(next - at)});
        at = next;
    }
}

void layoutDocument(Layout& t, LottieTextDocument& d, const LottieLayer& layer) {
    LottieAnimation& a = *t.a;
    PrStringView text = lottieString(a, d.text);
    Array<LottieString> chars;
    characters(text, chars);
    auto charAt = [&](uint32_t i) { return prStringSlice(text, chars.data[i].offset, chars.data[i].size); };
    measureCharacters(t, d, text, chars);
    float tracking = d.tracking / 1000 * d.size;
    const LottieFont* font = d.font != kLottieNone ? &a.textFonts.data[d.font] : nullptr;
    float lineHeight = d.lineHeight;

    // Paragraph text breaks where a letter would exceed the box's width: at the last space, or before the letter if the
    // line has no space. Each break is an empty character of its own.
    Array<bool> breaks;
    for (uint32_t i = 0; i < chars.count; ++i)
        breaks.push(false);
    if (d.box) {
        float lineWidth = 0;
        uint32_t lastSpace = kLottieNone;
        for (uint32_t i = 0; i < chars.count; ++i) {
            PrStringView c = charAt(i);
            bool space = c.size == 1 && c.data[0] == ' ';
            if (space)
                lastSpace = i;
            else if (isNewline(c) || breaks.data[i])
                lineWidth = 0;
            float length = t.measured.data[i].width;
            if (lineWidth + length > d.boxSize.x && !space) {
                if (lastSpace == kLottieNone) {
                    // Inserts a break before this letter, which starts the next line.
                    chars.insert(i, LottieString{0, 0});
                    breaks.insert(i, true);
                    t.measured.insert(i, Measured{0, 0, 0, 0, false, 0, kLottieNone});
                    lineWidth = length + tracking;
                    ++i;
                } else {
                    // The space becomes the break.
                    breaks.data[lastSpace] = true;
                    lineWidth = 0;
                    i = lastSpace;
                }
                lastSpace = kLottieNone;
            } else {
                lineWidth += length + tracking;
            }
        }
    }

    d.letters = a.letters.count;
    float lineWidth = -tracking;
    float maxLineWidth = 0;
    float spaces = 0;
    uint32_t line = 0;
    d.lines = a.numbers.count;
    uint32_t group = 0;
    float groupSize = 0;
    uint32_t groupStart = a.letters.count;
    for (uint32_t i = 0; i < chars.count; ++i) {
        PrStringView c = breaks.data[i] ? "\r" : charAt(i);
        LottieLetter letter{};
        letter.shape = kLottieNone;
        letter.transforms = kLottieNone;
        letter.newline = isNewline(c);
        letter.space = c.size == 1 && c.data[0] == ' ';
        // A space that became a break measures nothing.
        if (!breaks.data[i]) {
            const Measured& m = t.measured.data[i];
            letter.width = m.width;
            letter.shape = m.shape;
            letter.font = m.font;
            letter.glyphs = a.textGlyphs.count;
            if (m.glyphCount != 0)
                a.textGlyphs.append(t.measuredGlyphs.data + m.glyphs, m.glyphCount);
            letter.glyphCount = m.glyphCount;
            if (m.scaled) {
                letter.transforms = a.textTransforms.count;
                a.textTransforms.append(t.measuredTransforms.data + m.glyphs, m.glyphCount);
            }
        }
        if (letter.newline) {
            spaces = 0;
            a.numbers.push(lineWidth);
            maxLineWidth = prF32Max(maxLineWidth, lineWidth);
            lineWidth = -2 * tracking;
            ++line;
        }
        if (letter.space) {
            spaces += letter.width + tracking;
        } else {
            lineWidth += letter.width + tracking + spaces;
            spaces = 0;
        }
        letter.anchorWidth = letter.width;
        letter.add = groupSize;
        letter.line = line;
        uint32_t index = a.letters.count;
        a.letters.push(letter);
        // If letters are grouped by word or line, a letter is anchored by its group's width.
        bool empty = letter.newline;
        if (layer.grouping == 2 || layer.grouping == 3) {
            groupSize += letter.width;
            bool ends = layer.grouping == 2 ? empty || letter.space : empty;
            if (ends || i == chars.count - 1) {
                if (ends)
                    groupSize -= letter.width;
                for (uint32_t k = groupStart; k <= index; ++k) {
                    a.letters.data[k].anchorWidth = groupSize;
                    a.letters.data[k].group = group;
                }
                groupStart = index + 1;
                ++group;
                groupSize = 0;
            }
        } else {
            a.letters.data[index].group = group++;
        }
    }
    d.letterCount = a.letters.count - d.letters;
    maxLineWidth = prF32Max(maxLineWidth, lineWidth);
    a.numbers.push(lineWidth);
    d.lineCount = a.numbers.count - d.lines;
    if (d.box) {
        d.boxWidth = d.boxSize.x;
        d.justifyOffset = 0;
    } else {
        d.boxWidth = maxLineWidth;
        d.justifyOffset = d.justify == 1 ? -d.boxWidth : d.justify == 2 ? -d.boxWidth / 2 : 0;
    }
    d.yOffset = lineHeight != 0 ? lineHeight : d.size * 1.2f;
    d.ascent = (font != nullptr ? font->ascent : 75) * d.size / 100;

    // Each animator counts the units that it selects over: letters, letters excluding spaces, words or lines.
    d.indexes = a.letterIndexes.count;
    for (uint32_t k = 0; k < layer.animatorCount * d.letterCount; ++k)
        a.letterIndexes.push(0);
    d.totals = a.letterIndexes.count;
    for (uint32_t j = 0; j < layer.animatorCount; ++j) {
        const LottieTextAnimator& animator = a.animators.data[layer.animators + j];
        uint32_t index = 0;
        for (uint32_t i = 0; i < d.letterCount; ++i) {
            const LottieLetter& letter = a.letters.data[d.letters + i];
            a.letterIndexes.data[d.indexes + j * d.letterCount + i] = index;
            bool counts;
            if (animator.basedOn == 2)
                counts = !letter.newline && !letter.space;
            else if (animator.basedOn == 3)
                counts = letter.newline || letter.space || i == d.letterCount - 1;
            else if (animator.basedOn == 4)
                counts = letter.newline || i == d.letterCount - 1;
            else
                counts = !letter.newline;
            if (counts)
                ++index;
        }
        a.letterIndexes.push(index);
    }
    chars.reset();
    breaks.reset();
}

// ---------------------------------------------------------------------------------------------------------------------
// Drawing

// Returns the share of its animator that a letter receives from the range selector, according to the selector's shape,
// ease and smoothness.
float selectorMult(const LottieTextAnimator& t, float s, float e, float easeLow, float easeHigh, float smoothness,
        float amount, float index) {
    double x1 = 0;
    double y1 = 0;
    double x2 = 1;
    double y2 = 1;
    if (easeLow > 0)
        x1 = double(easeLow) / 100;
    else
        y1 = -double(easeLow) / 100;
    if (easeHigh > 0)
        x2 = 1 - double(easeHigh) / 100;
    else
        y2 = 1 + double(easeHigh) / 100;
    auto ease = [&](float m) { return float(lottieEase(x1, y1, x2, y2, double(m))); };
    float mult = 0;
    auto ramp = [&] { return prF32Max(0, prF32Min(0.5f / (e - s) + (index - s) / (e - s), 1)); };
    switch (t.shape) {
        case 2:
            mult = ease(e == s ? (index >= e ? 1 : 0) : ramp());
            break;
        case 3:
            mult = ease(e == s ? (index >= e ? 0 : 1) : 1 - ramp());
            break;
        case 4:
            if (e != s) {
                mult = ramp();
                mult = mult < 0.5f ? mult * 2 : 1 - 2 * (mult - 0.5f);
            }
            mult = ease(mult);
            break;
        case 5:
            if (e != s) {
                float total = e - s;
                float i = prF32Min(prF32Max(0, index + 0.5f - s), total);
                float x = -total / 2 + i;
                float half = total / 2;
                mult = prF32Sqrt(prF32Max(0, 1 - (x * x) / (half * half)));
            }
            mult = ease(mult);
            break;
        case 6:
            if (e != s) {
                float i = prF32Min(prF32Max(0, index + 0.5f - s), e - s);
                mult = (1 + prF32Cos(kPrF32Pi + kPrF32Pi * 2 * i / (e - s))) / 2;
            }
            mult = ease(mult);
            break;
        default:
            if (index >= prF32Floor(s)) {
                if (index - s < 0)
                    mult = prF32Max(0, prF32Min(prF32Min(e, 1) - (s - index), 1));
                else
                    mult = prF32Max(0, prF32Min(e - index, 1));
            }
            mult = ease(mult);
            break;
    }
    if (smoothness != 100) {
        float smooth = smoothness * 0.01f;
        if (smooth == 0)
            smooth = 0.00000001f;
        float threshold = 0.5f - smooth * 0.5f;
        mult = mult < threshold ? 0 : prF32Min((mult - threshold) / smooth, 1);
    }
    return mult * amount;
}

// An animator's values at a frame.
struct Animated {
    float s;
    float e;
    float easeLow;
    float easeHigh;
    float smoothness;
    float amount;
    float total;
};

// Conversions from RGB with 0-1 channels to HSV, and back.
void rgbToHsv(const float* c, float* hsv) {
    float r = c[0];
    float g = c[1];
    float b = c[2];
    float max = prF32Max(r, prF32Max(g, b));
    float min = prF32Min(r, prF32Min(g, b));
    float d = max - min;
    float h = 0;
    float s = max == 0 ? 0 : d / max;
    if (d != 0) {
        if (max == r)
            h = (g - b) + d * (g < b ? 6 : 0);
        else if (max == g)
            h = (b - r) + d * 2;
        else
            h = (r - g) + d * 4;
        h /= 6 * d;
    }
    hsv[0] = h;
    hsv[1] = s;
    hsv[2] = max;
}

void hsvToRgb(const float* hsv, float* c) {
    float h = hsv[0];
    float s = hsv[1];
    float v = hsv[2];
    float i = prF32Floor(h * 6);
    float f = h * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);
    switch (int32_t(i) % 6) {
        case 0: c[0] = v; c[1] = t; c[2] = p; break;
        case 1: c[0] = q; c[1] = v; c[2] = p; break;
        case 2: c[0] = p; c[1] = v; c[2] = t; break;
        case 3: c[0] = p; c[1] = q; c[2] = v; break;
        case 4: c[0] = t; c[1] = p; c[2] = v; break;
        default: c[0] = v; c[1] = p; c[2] = q; break;
    }
}

void adjustHsv(float* c, uint32_t channel, float by) {
    float hsv[3];
    rgbToHsv(c, hsv);
    hsv[channel] += channel == 0 ? by / 360 : by;
    if (channel == 0)
        hsv[0] = hsv[0] > 1 ? hsv[0] - 1 : hsv[0] < 0 ? hsv[0] + 1 : hsv[0];
    else
        hsv[channel] = prF32Clamp(hsv[channel], 0, 1);
    hsvToRgb(hsv, c);
}

float textChannel(float value) {
    return prF32Clamp(value, 0, 1);
}

PrAffine2 skewFromAxis(float skew, float axis) {
    PrAffine2 shear{1, 0, -prF32Tan(skew), 1, 0, 0};
    PrAffine2 m = prAffine2Concat(prAffine2Rotate(axis), shear);
    return prAffine2Concat(m, prAffine2Rotate(-axis));
}

} // namespace

void lottieLayoutText(LottieAnimation& a, const PrLottieOptions& options) {
    Layout t{.a = &a, .options = &options, .glyphs = prTextGlyphBufferCreate()};
    for (uint32_t i = 0; i < a.textFonts.count; ++i)
        t.textFonts.push(textFontOf(a, a.textFonts.data[i], options));
    for (uint32_t i = 0; i < a.charShapes.count; ++i)
        t.shapes.push(CharKey{lottieString(a, a.charShapes.data[i].character), i});
    sortItems(t.shapes.data, t.shapes.count, [](const CharKey& x, const CharKey& y) {
        int order = prStringCompare(x.character, y.character);
        return order < 0 || (order == 0 && x.shape < y.shape);
    });
    for (uint32_t l = 0; l < a.layers.count; ++l) {
        const LottieLayer& layer = a.layers.data[l];
        if (layer.kind != LottieLayerKind_Text)
            continue;
        for (uint32_t d = 0; d < layer.count; ++d)
            layoutDocument(t, a.documents.data[layer.first + d], layer);
    }
    prTextGlyphBufferDestroy(t.glyphs);
    for (uint32_t i = 0; i < t.textFonts.count; ++i)
        prTextFontRelease(t.textFonts.data[i]);
    t.textFonts.reset();
    t.faceFonts.reset();
    prTextItemsDestroy(t.items);
    t.shapes.reset();
    t.measured.reset();
    t.measuredGlyphs.reset();
    t.measuredTransforms.reset();
}

void lottieDrawText(LottieRender& r, const LottieLayer& layer, double frame) {
    const LottieAnimation& a = *r.a;
    if (layer.count == 0)
        return;
    // The document of the last keyframe at or before the frame.
    uint32_t which = 0;
    while (which + 1 < layer.count && a.documents.data[layer.first + which + 1].time <= frame)
        ++which;
    const LottieTextDocument& d = a.documents.data[layer.first + which];
    uint32_t n = layer.animatorCount;
    Array<Animated> animated;
    for (uint32_t j = 0; j < n; ++j) {
        const LottieTextAnimator& t = a.animators.data[layer.animators + j];
        Animated v{};
        v.total = float(a.letterIndexes.data[d.totals + j]);
        float divisor = t.units == 2 ? 1 : (v.total > 0 ? 100 / v.total : 1);
        float offset = lottieScalar(a, t.offset, frame, 0) / divisor;
        float end = 100;
        if (t.end != kLottieNone)
            end = lottieScalar(a, t.end, frame, 100);
        else if (t.units == 2)
            end = float(d.letterCount);
        v.s = lottieScalar(a, t.start, frame, 0) / divisor + offset;
        v.e = end / divisor + offset;
        if (v.s > v.e) {
            float swap = v.s;
            v.s = v.e;
            v.e = swap;
        }
        v.easeLow = lottieScalar(a, t.easeLow, frame, 0);
        v.easeHigh = lottieScalar(a, t.easeHigh, frame, 0);
        v.smoothness = lottieScalar(a, t.smoothness, frame, 100);
        v.amount = lottieScalar(a, t.amount, frame, 100) / 100;
        animated.push(v);
    }
    auto mult = [&](uint32_t j, uint32_t letter) {
        const Animated& v = animated.data[j];
        float index = float(a.letterIndexes.data[d.indexes + j * d.letterCount + letter]);
        return selectorMult(a.animators.data[layer.animators + j], v.s, v.e, v.easeLow, v.easeHigh, v.smoothness,
            v.amount, index);
    };
    PrVec2 alignment = lottiePoint(a, layer.alignment, frame, PrVec2{0, 0});
    float tracking = d.tracking * 0.001f * d.size;
    float yOff = d.size * 1.2f * 0.714f;

    // The amount by which tracking animators move each letter of a justified line.
    Array<float> justified;
    for (uint32_t i = 0; i < d.letterCount; ++i)
        justified.push(0);
    if (d.justify == 1 || d.justify == 2) {
        float offset = 0;
        float first = 0;
        float factor = d.justify == 2 ? -0.5f : -1;
        uint32_t last = 0;
        bool newLine = true;
        for (uint32_t i = 0; i < d.letterCount; ++i) {
            const LottieLetter& letter = a.letters.data[d.letters + i];
            if (letter.newline) {
                if (offset != 0)
                    offset += first;
                for (; last < i; ++last)
                    justified.data[last] = offset;
                offset = 0;
                newLine = true;
                continue;
            }
            for (uint32_t j = 0; j < n; ++j) {
                const LottieTextAnimator& t = a.animators.data[layer.animators + j];
                if (t.tracking == kLottieNone)
                    continue;
                float value = lottieScalar(a, t.tracking, frame, 0);
                if (newLine && d.justify == 2)
                    first += value * factor;
                offset += value * mult(j, i) * factor;
            }
            newLine = false;
        }
        if (offset != 0)
            offset += first;
        for (; last < d.letterCount; ++last)
            justified.data[last] = offset;
    }

    float xPos = 0;
    float yPos = 0;
    bool firstLine = true;
    PrPath* path = prPathCreate();
    for (uint32_t i = 0; i < d.letterCount; ++i) {
        const LottieLetter& letter = a.letters.data[d.letters + i];
        if (letter.newline) {
            xPos = 0;
            yPos += d.yOffset + (firstLine ? 1 : 0);
            firstLine = false;
            continue;
        }
        float offf = letter.anchorWidth / 2 - letter.add;
        PrAffine2 m = prAffine2Translate(PrVec2{-offf, 0});
        PrVec2 aligned{alignment.x * letter.anchorWidth * 0.005f, alignment.y * yOff * 0.01f};
        m = prAffine2Concat(m, prAffine2Translate(-aligned));
        for (uint32_t j = 0; j < n; ++j) {
            const LottieTextAnimator& t = a.animators.data[layer.animators + j];
            if (t.tracking != kLottieNone && (xPos != 0 || d.justify != 0))
                xPos += lottieScalar(a, t.tracking, frame, 0) * mult(j, i);
        }
        float strokeWidth = d.strokeWidth;
        float stroke[3] = {d.stroke.r, d.stroke.g, d.stroke.b};
        float fill[3] = {d.fill.r, d.fill.g, d.fill.b};
        float opacity = 1;
        for (uint32_t j = 0; j < n; ++j) {
            const LottieTextAnimator& t = a.animators.data[layer.animators + j];
            if (t.anchor == kLottieNone)
                continue;
            PrVec2 p = lottiePoint(a, t.anchor, frame, PrVec2{0, 0});
            float k = mult(j, i);
            m = prAffine2Concat(m, prAffine2Translate(-p * k));
        }
        for (uint32_t j = 0; j < n; ++j) {
            const LottieTextAnimator& t = a.animators.data[layer.animators + j];
            if (t.scale == kLottieNone)
                continue;
            PrVec2 s = lottiePoint(a, t.scale, frame, PrVec2{100, 100});
            float k = mult(j, i);
            m = prAffine2Concat(m, prAffine2Scale(PrVec2{1 + (s.x / 100 - 1) * k, 1 + (s.y / 100 - 1) * k}));
        }
        for (uint32_t j = 0; j < n; ++j) {
            const LottieTextAnimator& t = a.animators.data[layer.animators + j];
            float k = mult(j, i);
            if (t.skew != kLottieNone) {
                float skew = lottieScalar(a, t.skew, frame, 0) * kPrF32Pi / 180;
                float axis = lottieScalar(a, t.skewAxis, frame, 0) * kPrF32Pi / 180;
                m = prAffine2Concat(m, skewFromAxis(skew * k, axis * k));
            }
            if (t.rotation != kLottieNone)
                m = prAffine2Concat(m, prAffine2Rotate(lottieScalar(a, t.rotation, frame, 0) * kPrF32Pi / 180 * k));
            if (t.opacity != kLottieNone)
                opacity += (lottieScalar(a, t.opacity, frame, 100) / 100 * k - opacity) * k;
            if (t.strokeWidth != kLottieNone)
                strokeWidth += lottieScalar(a, t.strokeWidth, frame, 0) * k;
            if (t.strokeColor != kLottieNone) {
                float c[4] = {0, 0, 0, 1};
                if (lottieDims(a, t.strokeColor) >= 3 && lottieDims(a, t.strokeColor) <= 4)
                    lottieEvaluate(a, t.strokeColor, frame, c);
                for (uint32_t ch = 0; ch < 3; ++ch)
                    stroke[ch] += (c[ch] - stroke[ch]) * k;
            }
            if (d.hasFill) {
                if (t.fillColor != kLottieNone) {
                    float c[4] = {0, 0, 0, 1};
                    if (lottieDims(a, t.fillColor) >= 3 && lottieDims(a, t.fillColor) <= 4)
                        lottieEvaluate(a, t.fillColor, frame, c);
                    for (uint32_t ch = 0; ch < 3; ++ch)
                        fill[ch] += (c[ch] - fill[ch]) * k;
                }
                if (t.fillHue != kLottieNone)
                    adjustHsv(fill, 0, lottieScalar(a, t.fillHue, frame, 0) * k);
                if (t.fillSaturation != kLottieNone)
                    adjustHsv(fill, 1, lottieScalar(a, t.fillSaturation, frame, 0) / 100 * k);
                if (t.fillBrightness != kLottieNone)
                    adjustHsv(fill, 2, lottieScalar(a, t.fillBrightness, frame, 0) / 100 * k);
            }
        }
        for (uint32_t j = 0; j < n; ++j) {
            const LottieTextAnimator& t = a.animators.data[layer.animators + j];
            if (t.position == kLottieNone)
                continue;
            PrVec2 p = lottiePoint(a, t.position, frame, PrVec2{0, 0});
            float k = mult(j, i);
            m = prAffine2Concat(m, prAffine2Translate(p * k));
        }
        m = prAffine2Concat(m, prAffine2Translate(PrVec2{xPos, yPos}));
        if (d.box)
            m = prAffine2Concat(m, prAffine2Translate(PrVec2{d.boxPosition.x, d.boxPosition.y + d.ascent}));
        float lineWidth = letter.line < d.lineCount ? a.numbers.data[d.lines + letter.line] : 0;
        float slack = d.justify == 1 ? d.boxWidth - lineWidth : d.justify == 2 ? (d.boxWidth - lineWidth) / 2 : 0;
        if (d.justify == 1 || d.justify == 2)
            m = prAffine2Concat(m, prAffine2Translate(PrVec2{justified.data[i] + d.justifyOffset + slack, 0}));
        m = prAffine2Concat(m, prAffine2Translate(PrVec2{0, -d.baselineShift}));
        m = prAffine2Concat(m, prAffine2Translate(PrVec2{offf, 0}));
        m = prAffine2Concat(m, prAffine2Translate(aligned));
        xPos += letter.width + tracking;

        if (!(opacity > 0) || (letter.glyphCount == 0 && letter.shape == kLottieNone))
            continue;
        prDrawListPushTransformAffine2(r.list, m);
        bool grouped = opacity < 1;
        if (grouped) {
            PrDrawGroup group = prDrawGroupDefault();
            group.opacity = opacity;
            prDrawListPushGroup(r.list, &group);
        }
        PrDrawStroke outline = prDrawStrokeDefault();
        outline.shape.width = prF32Max(strokeWidth, 0);
        outline.shape.join = PrPathJoin_Round;
        PrColor fillTint{textChannel(fill[0]), textChannel(fill[1]), textChannel(fill[2]), 1};
        PrColor strokeTint{textChannel(stroke[0]), textChannel(stroke[1]), textChannel(stroke[2]), 1};
        bool stroked = d.hasStroke && outline.shape.width > 0;
        if (letter.shape != kLottieNone) {
            const LottieCharShape& shape = a.charShapes.data[letter.shape];
            prPathClear(path);
            float scale = d.size / 100;
            lottieItemPaths(r, shape.first, shape.count, frame, prAffine2Scale(PrVec2{scale, scale}), path);
            if (d.hasFill)
                prDrawListPath(r.list, path, PrPathFillRule_NonZero, {.color = fillTint});
            if (stroked)
                prDrawListPath(r.list, path, PrPathFillRule_NonZero, {.color = strokeTint, .stroke = &outline});
        } else {
            PrFontGlyphRun run{};
            run.font = a.fonts.data[letter.font];
            run.size = d.size;
            run.glyphs = a.textGlyphs.data + letter.glyphs;
            run.glyphCount = letter.glyphCount;
            run.transforms = letter.transforms != kLottieNone ? a.textTransforms.data + letter.transforms : nullptr;
            if (d.hasFill)
                prDrawListGlyphs(r.list, &run, PrVec2{}, {.color = fillTint});
            if (stroked)
                prDrawListGlyphs(r.list, &run, PrVec2{}, {.color = strokeTint, .stroke = &outline});
        }
        if (grouped)
            prDrawListPopGroup(r.list);
        prDrawListPopTransform(r.list);
    }
    prPathDestroy(path);
    animated.reset();
    justified.reset();
}

} // namespace pr_impl
