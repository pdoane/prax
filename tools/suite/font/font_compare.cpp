// The harness of the font-freetype suite (tools/suite/font_freetype.py): lib/font beside FreeType, over a font file.
//
// `font_compare --checks <path>` writes the checks that apply to the font at the path, one to a line. `font_compare
// <check> <path> [<check> <path>]...` makes each check of the font at each path and writes a line of JSON for each,
// {"ok": <bool>, "detail": <text>}, whose detail says what differs when the check fails:
//
//   cmap        the glyph of every code point
//   sequences   the glyph of each variation sequence that FreeType lists, and of the code points beside them
//   advances    the unhinted design advance of every glyph
//   outlines    the unhinted outline of each glyph (an even sample of a large font) at the default instance, and at the
//               ends of each axis of a variable font, segment by segment
//
// lib/font is reached through its public API alone, as an application reaches it.

#include "pr/array.h"
#include "pr/blob.h"
#include "pr/blob_ref.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/font.h"
#include "pr/path.h"
#include "pr/string.h"
#include "pr/text_buffer.h"

#include "tools/harness/harness.h"

#include "freetype/freetype.h"
#include "freetype/ftadvanc.h"
#include "freetype/ftmm.h"
#include "freetype/ftoutln.h"

using namespace harness;
using namespace pr;

namespace {

// How far a point may lie from FreeType's, in font units. FreeType keeps a variation's points to a 64th of a unit.
constexpr float kTolerance = 0.05f;

// The most glyphs of one font that an outline comparison loads at each instance. A larger font is sampled evenly.
constexpr uint32_t kMaxGlyphs = 6000;

// A font's bytes, FreeType's face of them, and lib/font's font of them.
struct Faces {
    Array<uint8_t> bytes;
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    const PrFont* font = nullptr;
    String error;

    explicit Faces(PrStringView path) {
        BlobRef blob = BlobRef::adopt(prFileOpen(path));
        if (blob.get() == nullptr) {
            error = "the font cannot be read";
            return;
        }
        PrByteView read = prBlobBytes(blob.get());
        bytes.append(read.data, uint32_t(read.size));
        if (FT_Init_FreeType(&library) != 0 ||
                FT_New_Memory_Face(library, bytes.data(), FT_Long(bytes.count()), 0, &face) != 0) {
            error = "FreeType does not open the font";
        } else if (prFontCreate(blob.get(), 0, nullptr, &font) != PrStatus_Ok) {
            error = "lib/font does not open the font";
        }
    }

    ~Faces() {
        prFontRelease(font);
        if (face != nullptr)
            FT_Done_Face(face);
        if (library != nullptr)
            FT_Done_FreeType(library);
    }
};

String cmap(PrStringView path) {
    Faces f(path);
    if (!f.error.isEmpty())
        return f.error;
    uint32_t differ = 0;
    uint32_t first = 0;
    for (uint32_t c = 0; c <= 0x10ffff; ++c) {
        if (FT_Get_Char_Index(f.face, c) != prFontCodePointGlyph(f.font, c) && differ++ == 0)
            first = c;
    }
    if (differ == 0)
        return {};
    return format("{} code points map otherwise, the first U+{:04X}: glyph {} against FreeType's {}", differ, first,
        prFontCodePointGlyph(f.font, first), FT_Get_Char_Index(f.face, first));
}

String sequences(PrStringView path) {
    Faces f(path);
    if (!f.error.isEmpty())
        return f.error;
    uint32_t differ = 0;
    uint32_t firstBase = 0;
    uint32_t firstSelector = 0;
    auto check = [&](uint32_t base, uint32_t selector) {
        int isDefault = FT_Face_GetCharVariantIsDefault(f.face, base, selector);
        FT_UInt glyph = FT_Face_GetCharVariantIndex(f.face, base, selector);
        PrFontSequenceMapping expected = PrFontSequenceMapping_None;
        if (isDefault == 1 && glyph != 0)
            expected = PrFontSequenceMapping_Default;
        else if (isDefault == 0 && glyph != 0)
            expected = PrFontSequenceMapping_Variant;
        uint16_t expectedGlyph = expected != PrFontSequenceMapping_None ? uint16_t(glyph) : 0;
        PrFontSequenceGlyph found = prFontVariationSequenceGlyph(f.font, base, selector);
        if ((found.mapping != expected || found.glyph != expectedGlyph) && differ++ == 0) {
            firstBase = base;
            firstSelector = selector;
        }
    };
    // FreeType returns each list in a buffer of the face's, which its next list reuses, so the selectors are copied
    // before their code points are asked for.
    Array<FT_UInt32> selectors;
    for (FT_UInt32* at = FT_Face_GetVariantSelectors(f.face); at != nullptr && *at != 0; ++at)
        selectors.push(*at);
    for (FT_UInt32 selector : selectors) {
        FT_UInt32* bases = FT_Face_GetCharsOfVariant(f.face, selector);
        for (uint32_t b = 0; bases != nullptr && bases[b] != 0; ++b) {
            // The code points beside a listed one, which the font may not list.
            check(bases[b], selector);
            check(bases[b] + 1, selector);
            check(bases[b] - 1, selector);
        }
        check('A', selector);
    }
    check('A', 0xFE00);
    if (differ == 0)
        return {};
    return format("{} sequences map otherwise, the first U+{:04X} U+{:04X}", differ, firstBase, firstSelector);
}

String advances(PrStringView path) {
    Faces f(path);
    if (!f.error.isEmpty())
        return f.error;
    if (FT_Set_Char_Size(f.face, 0, FT_F26Dot6(f.face->units_per_EM) << 6, 72, 72) != 0)
        return "FreeType does not size the font";
    uint32_t differ = 0;
    uint32_t first = 0;
    for (FT_Long g = 0; g < f.face->num_glyphs; ++g) {
        FT_Fixed advance = 0;
        FT_Get_Advance(f.face, FT_UInt(g), FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP | FT_LOAD_NO_AUTOHINT, &advance);
        float expected = float(advance) / 65536.0f / float(f.face->units_per_EM);
        if (prFontGlyphAdvance(f.font, uint16_t(g), 0) != expected && differ++ == 0)
            first = uint32_t(g);
    }
    return differ == 0 ? String() : format("{} glyphs advance otherwise, the first glyph {}", differ, first);
}

// One segment of an outline: the verb of a path, and its points in font units with y up.
struct Segment {
    PrPathVerb verb;
    PrVec2 points[3];
};

// The segments of a glyph's outline. A contour ends at its start, without a line back to it of no length, so that the
// two outlines' contours close alike.
struct Segments {
    Array<Segment> all;
    PrVec2 start{};

    void move(PrVec2 p) {
        end();
        all.push(Segment{PrPathVerb_Move, {p}});
        start = p;
    }

    void add(PrPathVerb verb, PrVec2 a, PrVec2 b = {}, PrVec2 c = {}) { all.push(Segment{verb, {a, b, c}}); }

    // Ends the contour: a last line to its start is dropped.
    void end() {
        if (!all.isEmpty() && all.last().verb == PrPathVerb_Line && all.last().points[0].x == start.x &&
                all.last().points[0].y == start.y)
            all.pop();
    }
};

PrVec2 fromFreeType(const FT_Vector* v) {
    return PrVec2{float(v->x) / 64, float(v->y) / 64};
}

// FreeType's outline of the glyph that its face last loaded.
Segments freeTypeSegments(FT_Face face) {
    Segments out;
    FT_Outline_Funcs funcs{};
    funcs.move_to = [](const FT_Vector* to, void* user) {
        static_cast<Segments*>(user)->move(fromFreeType(to));
        return 0;
    };
    funcs.line_to = [](const FT_Vector* to, void* user) {
        static_cast<Segments*>(user)->add(PrPathVerb_Line, fromFreeType(to));
        return 0;
    };
    funcs.conic_to = [](const FT_Vector* control, const FT_Vector* to, void* user) {
        static_cast<Segments*>(user)->add(PrPathVerb_Quad, fromFreeType(control), fromFreeType(to));
        return 0;
    };
    funcs.cubic_to = [](const FT_Vector* a, const FT_Vector* b, const FT_Vector* to, void* user) {
        static_cast<Segments*>(user)->add(PrPathVerb_Cubic, fromFreeType(a), fromFreeType(b), fromFreeType(to));
        return 0;
    };
    FT_Outline_Decompose(&face->glyph->outline, &funcs, &out);
    out.end();
    return out;
}

// lib/font's outline of the glyph, in font units with y up.
Segments praxSegments(const PrFont* font, uint16_t glyph, float unitsPerEm, PrPath* path) {
    Segments out;
    prPathClear(path);
    prFontGlyphOutline(font, glyph, 0, prAffine2Scale(PrVec2{unitsPerEm, -unitsPerEm}), path);
    const PrVec2* points = prPathPoints(path);
    uint32_t at = 0;
    for (uint32_t i = 0; i < prPathVerbCount(path); ++i) {
        PrPathVerb verb = prPathVerbs(path)[i];
        switch (verb) {
            case PrPathVerb_Move:
                out.move(points[at++]);
                break;
            case PrPathVerb_Line:
                out.add(verb, points[at++]);
                break;
            case PrPathVerb_Quad:
                out.add(verb, points[at], points[at + 1]);
                at += 2;
                break;
            case PrPathVerb_Cubic:
                out.add(verb, points[at], points[at + 1], points[at + 2]);
                at += 3;
                break;
            default:
                break;
        }
    }
    out.end();
    return out;
}

uint32_t pointsOf(PrPathVerb verb) {
    return verb == PrPathVerb_Cubic ? 3 : verb == PrPathVerb_Quad ? 2 : 1;
}

// Returns the index of the first segment that differs, or the count of segments if none does.
uint32_t firstDifference(const Segments& a, const Segments& b) {
    uint32_t count = a.all.count() < b.all.count() ? a.all.count() : b.all.count();
    for (uint32_t i = 0; i < count; ++i) {
        const Segment& x = a.all[i];
        const Segment& y = b.all[i];
        if (x.verb != y.verb)
            return i;
        for (uint32_t p = 0; p < pointsOf(x.verb); ++p) {
            if (prF32Abs(x.points[p].x - y.points[p].x) > kTolerance || prF32Abs(x.points[p].y - y.points[p].y) >
                    kTolerance)
                return i;
        }
    }
    return count;
}

// Returns what differs from FreeType at the face's and the font's instance, or an empty string if nothing does.
String compareInstance(FT_Face face, const PrFont* font) {
    float unitsPerEm = float(face->units_per_EM);
    uint32_t glyphs = uint32_t(face->num_glyphs);
    uint32_t step = glyphs > kMaxGlyphs ? glyphs / kMaxGlyphs + 1 : 1;
    PrPath* path = prPathCreate();
    uint32_t differing = 0;
    String first;
    for (uint32_t glyph = 0; glyph < glyphs; glyph += step) {
        if (FT_Load_Glyph(face, glyph, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0)
            continue;
        Segments theirs = freeTypeSegments(face);
        Segments ours = praxSegments(font, uint16_t(glyph), unitsPerEm, path);
        float advance = prFontGlyphAdvance(font, uint16_t(glyph), 0) * unitsPerEm;
        // The linear advance is the one that HVAR varies, where a font has it. The advance of the metrics comes from
        // the phantom points, which a font's gvar may vary otherwise.
        float ftAdvance = float(face->glyph->linearHoriAdvance) / 65536;
        uint32_t at = firstDifference(ours, theirs);
        bool same = at == ours.all.count() && ours.all.count() == theirs.all.count() &&
            prF32Abs(advance - ftAdvance) <= kTolerance;
        if (!same && differing++ == 0) {
            first = format("glyph {}: {} segments against {}, advance {} against {}", glyph, ours.all.count(),
                theirs.all.count(), advance, ftAdvance);
            if (at < ours.all.count() && at < theirs.all.count())
                formatTo(first, ", first differing segment {}", at);
        }
    }
    prPathDestroy(path);
    return differing == 0 ? String() : format("{} glyphs differ, the first {}", differing, first);
}

String outlines(PrStringView path) {
    Faces f(path);
    if (!f.error.isEmpty())
        return f.error;
    // One pixel per unit, so that FreeType's 26.6 coordinates are 64ths of a unit.
    if (FT_Set_Char_Size(f.face, 0, FT_F26Dot6(f.face->units_per_EM) << 6, 72, 72) != 0)
        return "FreeType does not size the font";
    String differs = compareInstance(f.face, f.font);
    if (!differs.isEmpty())
        return format("at the default instance, {}", differs);
    // Each axis at its minimum and its maximum, with the others at their defaults.
    uint32_t axes = prFontAxisCount(f.font);
    for (uint32_t i = 0; i < axes; ++i) {
        PrFontAxis axis = prFontAxis(f.font, i);
        const float ends[] = {axis.min, axis.max};
        for (float value : ends) {
            if (value == axis.defaultValue)
                continue;
            Array<FT_Fixed> design;
            for (uint32_t k = 0; k < axes; ++k)
                design.push(FT_Fixed(prF32Round((k == i ? value : prFontAxis(f.font, k).defaultValue) * 65536)));
            if (FT_Set_Var_Design_Coordinates(f.face, FT_UInt(design.count()), design.data()) != 0)
                return "FreeType does not set an instance";
            PrFontVariation variation{axis.tag, value};
            PrFontDerivation derivation = prFontDerivationDefault();
            derivation.variations = &variation;
            derivation.variationCount = 1;
            const PrFont* instance = prFontCreateDerived(f.font, &derivation);
            differs = compareInstance(f.face, instance);
            prFontRelease(instance);
            if (!differs.isEmpty()) {
                char tag[5] = {char(axis.tag >> 24), char(axis.tag >> 16), char(axis.tag >> 8), char(axis.tag), 0};
                return format("at {} {}, {}", PrStringView(tag), value, differs);
            }
        }
    }
    return {};
}

// Returns the checks that apply to the font at `path`: those of what FreeType and lib/font both find in it.
Array<PrStringView> checksOf(PrStringView path) {
    Array<PrStringView> out;
    Faces f(path);
    if (!f.error.isEmpty())
        return out;
    if (f.face->charmap != nullptr)
        out.push("cmap");
    if (FT_Face_GetVariantSelectors(f.face) != nullptr)
        out.push("sequences");
    if (FT_IS_SCALABLE(f.face)) {
        out.push("advances");
        out.push("outlines");
    }
    return out;
}

String check(PrStringView name, PrStringView path) {
    if (name == "cmap")
        return cmap(path);
    if (name == "sequences")
        return sequences(path);
    if (name == "advances")
        return advances(path);
    if (name == "outlines")
        return outlines(path);
    return format("no check is named {}", name);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && PrStringView(argv[1]) == "--checks") {
        for (PrStringView name : checksOf(argv[2]))
            harnessWriteLine(name);
        return 0;
    }
    for (int i = 1; i + 1 < argc; i += 2) {
        String detail = check(argv[i], argv[i + 1]);
        PrTextBuffer line{};
        prTextBufferAppend(&line, detail.isEmpty() ? "{\"ok\": true, \"detail\": " : "{\"ok\": false, \"detail\": ");
        harnessWriteString(&line, detail);
        prTextBufferAppend(&line, "}");
        harnessWriteLine(prTextBufferView(&line));
        prTextBufferReset(&line);
    }
    return 0;
}
