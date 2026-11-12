// Shaped text: what a host measures and breaks without a layout tree.

#include "pr/font_set.h"
#include "pr/text_harfbuzz.h"
#include "pr/text_shaping.h"

#include "tools/harness/test.h"

#include <initializer_list>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

// A shaping in Noto Sans and Noto Sans Arabic, with what it needs.
struct Shaped {
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextHarfBuzzCreate();
    PrTextFont* font = nullptr;
    PrTextShaping* shaping = prTextShapingCreate();
    PrTextShapingScratch* scratch = prTextShapingScratchCreate();
    std::string text;
    float tabSize = 0;
    float tabMinimum = 0;
    PrTextObjectBreak objectBreak = PrTextObjectBreak_Unicode;

    Shaped() {
        for (const char* name : {"NotoSans[wdth,wght].ttf", "NotoSansArabic[wdth,wght].ttf"}) {
            std::string path = std::string(PR_FONTS_DIR) + "/" + name;
            const PrFont* PR_NULLABLE face = nullptr;
            REQUIRE(prFontCreateFromFile(path, 0, nullptr, &face) == PrStatus_Ok);
            PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
            prFontSetAdd(fonts, &descriptor, face);
            prFontRelease(face);
        }
        PrFontFamily families[] = {prFontFamilyFromName("Noto Sans"), prFontFamilyFromName("Noto Sans Arabic")};
        PrTextFontDesc desc{.set = fonts, .families = families, .familyCount = 2, .style = prFontStyleDefault()};
        font = prTextFontCreate(&desc);
    }

    ~Shaped() {
        prTextShapingDestroy(shaping);
        prTextShapingScratchDestroy(scratch);
        prTextFontRelease(font);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
    }

    // Returns a piece of the text from `start` to `end` in the font.
    PrTextPiece piece(uint32_t start, uint32_t end, PrTextBreakStyle style = {}) {
        return PrTextPiece{.kind = PrTextPieceKind_Text, .start = start, .end = end, .font = font, .breakStyle = style};
    }

    void set(std::string value, std::vector<PrTextPiece> pieces, PrTextDirection direction = PrTextDirection_Ltr,
            const PrTextSpaceFlags* spaces = nullptr) {
        text = std::move(value);
        PrTextShapingDesc desc{
            .text = text,
            .pieces = pieces.data(),
            .pieceCount = uint32_t(pieces.size()),
            .direction = direction,
            .shaper = shaper,
            .spaces = spaces,
            .tabSize = tabSize,
            .tabMinimum = tabMinimum,
            .objectBreak = objectBreak,
        };
        prTextShapingSet(shaping, &desc, scratch);
    }

    void set(std::string value, PrTextBreakStyle style = {}) {
        uint32_t size = uint32_t(value.size());
        set(std::move(value), std::vector<PrTextPiece>{piece(0, size, style)});
    }

    float width(uint32_t start, uint32_t end, const float* positions = nullptr) {
        PrTextShapingLine line{.start = start, .end = end, .positions = positions};
        return prTextShapingLineWidth(shaping, &line);
    }

    // Returns the end of each line in `width`, or in the width that `widths` gives each line.
    std::vector<uint32_t> ends(std::vector<float> widths, const float* positions = nullptr, PrTextWordBreaks* words = nullptr) {
        std::vector<uint32_t> found;
        uint32_t size = uint32_t(text.size());
        for (uint32_t start = 0; start < size;) {
            float width = widths[found.size() < widths.size() ? found.size() : widths.size() - 1];
            PrTextShapingLine line{.start = start, .end = size, .positions = positions};
            start = prTextShapingBreakLine(shaping, text, &line, width, words);
            found.push_back(start);
        }
        return found;
    }
};

using Ends = std::vector<uint32_t>;

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a shaping breaks at any width without shaping again, each line in the width its host gives it") {
    Shaped s;
    s.set("one two three");
    uint32_t glyphs = prTextShapingInfo(s.shaping).glyphCount;
    CHECK(glyphs == 13);
    float one = s.width(0, 3);
    float three = s.width(8, 13);
    CHECK(s.ends({1000}) == Ends{13});
    CHECK(s.ends({three + 1}) == Ends{4, 8, 13});
    // A host beside a float narrows the lines it reaches.
    CHECK(s.ends({one + 1, 1000}) == Ends{4, 13});
    CHECK(prTextShapingInfo(s.shaping).glyphCount == glyphs);
}

TEST_CASE("the white space that a line ends in hangs, and a line feed forces a break") {
    Shaped s;
    s.set("ab   cd");
    float ab = s.width(0, 2);
    CHECK(s.width(0, 5) == doctest::Approx(ab));
    // The spaces hang past the width, and the line takes them.
    CHECK(s.ends({ab + 0.5f}) == Ends{5, 7});
    s.set("ab\ncd");
    CHECK(s.ends({1000}) == Ends{3, 5});
}

TEST_CASE("a shaping's intrinsic sizes break at every opportunity and at forced breaks") {
    Shaped s;
    s.set("one two\nthree");
    PrTextIntrinsic sizes = prTextShapingIntrinsic(s.shaping, 0, 13, nullptr, nullptr);
    CHECK(sizes.minContent == doctest::Approx(s.width(8, 13)));
    CHECK(sizes.maxContent == doctest::Approx(s.width(0, 7)));
}

TEST_CASE("an empty shaping has one segment and nothing to measure") {
    Shaped s;
    s.set("");
    CHECK(prTextShapingInfo(s.shaping).segmentCount == 1);
    CHECK(prTextShapingInfo(s.shaping).glyphCount == 0);
    PrTextIntrinsic sizes = prTextShapingIntrinsic(s.shaping, 0, 0, nullptr, nullptr);
    CHECK(sizes.minContent == 0);
    CHECK(sizes.maxContent == 0);
    PrTextShapingLine line{};
    CHECK(prTextShapingBreakLine(s.shaping, s.text, &line, 100, nullptr) == 0);
}

TEST_CASE("a host gives an object its advance, and a line may break beside it") {
    Shaped s;
    std::string text = "ab\xef\xbf\xbc" "cd";
    PrTextPiece object{.kind = PrTextPieceKind_Object, .start = 2, .end = 5};
    s.objectBreak = PrTextObjectBreak_Around;
    s.set(text, std::vector<PrTextPiece>{s.piece(0, 2), object, s.piece(5, 7)});
    const PrUnicodeLineBreak* breaks = prTextShapingInfo(s.shaping).breaks;
    CHECK(breaks[2] == PrUnicodeLineBreak_Allowed);
    CHECK(breaks[5] == PrUnicodeLineBreak_Allowed);

    float advances[] = {0, 100, 0};
    float positions[8];
    prTextShapingPositions(s.shaping, prTextShapingInfo(s.shaping).segments, advances, positions);
    float plain = s.width(0, 7);
    CHECK(s.width(0, 7, positions) == doctest::Approx(plain + 100));
    float ab = s.width(0, 2, positions);
    CHECK(s.ends({ab + 110}, positions) == Ends{5, 7});
    CHECK(s.ends({ab + 50}, positions) == Ends{2, 5, 7});
    PrTextIntrinsic sizes = prTextShapingIntrinsic(s.shaping, 0, 7, positions, positions);
    CHECK(sizes.minContent >= 100);
    CHECK(sizes.maxContent == doctest::Approx(plain + 100));
}

TEST_CASE("a Break piece with no bytes starts a new segment, with an inline box's edges in the positions") {
    Shaped s;
    PrTextPiece open{.kind = PrTextPieceKind_Open, .start = 0, .end = 0};
    PrTextPiece close{.kind = PrTextPieceKind_Close, .start = 2, .end = 2};
    PrTextPiece block{.kind = PrTextPieceKind_Break, .start = 2, .end = 2};
    s.set("abcd", std::vector<PrTextPiece>{open, s.piece(0, 2), close, block, s.piece(2, 4)});
    REQUIRE(prTextShapingInfo(s.shaping).segmentCount == 2);
    const PrTextShapingSegment* segments = prTextShapingInfo(s.shaping).segments;
    CHECK(segments[0].byteEnd == 2);
    CHECK(segments[0].pieceEnd == 3);
    CHECK(segments[1].byteStart == 2);
    CHECK(segments[1].pieceStart == 4);
    // A segment ends in a mandatory break, so a line stops there.
    CHECK(prTextShapingInfo(s.shaping).breaks[2] == PrUnicodeLineBreak_Mandatory);

    float advances[] = {7, 0, 5, 0, 0};
    float positions[5];
    prTextShapingPositions(s.shaping, &segments[0], advances, positions);
    CHECK(positions[0] == 0);
    CHECK(s.width(0, 2, positions) == doctest::Approx(s.width(0, 2) + 12));
}

TEST_CASE("a host says where lines do not wrap, by the piece around an opportunity and by each space") {
    Shaped s;
    PrTextPiece piece = s.piece(0, 3);
    piece.noWrap = true;
    s.set("a-b", std::vector<PrTextPiece>{piece});
    CHECK(prTextShapingInfo(s.shaping).breaks[2] == PrUnicodeLineBreak_Prohibited);

    // The opportunity between two pieces follows the nearest Open piece that holds both of them.
    PrTextPiece open{.kind = PrTextPieceKind_Open, .noWrap = true};
    PrTextPiece close{.kind = PrTextPieceKind_Close, .start = 3, .end = 3};
    s.set("a-b", std::vector<PrTextPiece>{open, s.piece(0, 2), s.piece(2, 3), close});
    CHECK(prTextShapingInfo(s.shaping).breaks[2] == PrUnicodeLineBreak_Prohibited);
    s.set("a-b", std::vector<PrTextPiece>{s.piece(0, 2), s.piece(2, 3)});
    CHECK(prTextShapingInfo(s.shaping).breaks[2] == PrUnicodeLineBreak_Allowed);

    // An opportunity after white space belongs to the white space, whatever holds it.
    PrTextSpaceFlags wraps[] = {0, PrTextSpaceFlag_Hangs | PrTextSpaceFlag_Wraps, 0};
    s.set("a b", std::vector<PrTextPiece>{piece}, PrTextDirection_Ltr, wraps);
    CHECK(prTextShapingInfo(s.shaping).breaks[2] == PrUnicodeLineBreak_Allowed);
    PrTextSpaceFlags stays[] = {0, PrTextSpaceFlag_Hangs, 0};
    s.set("a b", std::vector<PrTextPiece>{s.piece(0, 3)}, PrTextDirection_Ltr, stays);
    CHECK(prTextShapingInfo(s.shaping).breaks[2] == PrUnicodeLineBreak_Prohibited);

    PrTextPiece spaces = s.piece(0, 4);
    spaces.breakSpaces = true;
    s.set("a  b", std::vector<PrTextPiece>{spaces});
    CHECK(prTextShapingInfo(s.shaping).breaks[2] == PrUnicodeLineBreak_Allowed);
}

TEST_CASE("a preserved tab shapes into nothing and advances to the stops that its host gives, from where the line starts") {
    Shaped s;
    s.tabSize = 40;
    s.tabMinimum = 5;
    PrTextSpaceFlags spaces[] = {0, PrTextSpaceFlag_Tab | PrTextSpaceFlag_Hangs, 0};
    s.set("a\tb", std::vector<PrTextPiece>{s.piece(0, 3)}, PrTextDirection_Ltr, spaces);
    CHECK(prTextShapingInfo(s.shaping).glyphCount == 2);
    float a = s.width(0, 1);
    float b = s.width(2, 3);
    CHECK(s.width(0, 3) == doctest::Approx(40 + b));
    // From this origin, the next stop is 1 px past the a, which is nearer than the least advance of a tab.
    PrTextShapingLine shifted{.end = 3, .origin = 39 - a};
    CHECK(prTextShapingLineWidth(s.shaping, &shifted) == doctest::Approx(a + 41 + b));
}

TEST_CASE("a host chooses where a line may break beside an object") {
    Shaped s;
    // A no-break space before the object, and a word joiner after it.
    std::string text = "a\xc2\xa0\xef\xbf\xbc\xe2\x81\xa0" "b";
    PrTextPiece object{.kind = PrTextPieceKind_Object, .start = 3, .end = 6};
    std::vector<PrTextPiece> pieces{s.piece(0, 3), object, s.piece(6, 10)};
    s.set(text, pieces);
    CHECK(prTextShapingInfo(s.shaping).breaks[3] == PrUnicodeLineBreak_Prohibited);
    CHECK(prTextShapingInfo(s.shaping).breaks[6] == PrUnicodeLineBreak_Prohibited);
    s.objectBreak = PrTextObjectBreak_Around;
    s.set(text, pieces);
    CHECK(prTextShapingInfo(s.shaping).breaks[3] == PrUnicodeLineBreak_Allowed);
    CHECK(prTextShapingInfo(s.shaping).breaks[6] == PrUnicodeLineBreak_Prohibited);
}

TEST_CASE("overflow-wrap breaks a word that fits on no line, and only anywhere counts for the narrowest width") {
    Shaped s;
    PrTextBreakStyle breakWord{.overflowWrap = PrTextOverflowWrap_BreakWord};
    s.set("abcdef", breakWord);
    CHECK(prTextShapingInfo(s.shaping).emergency == nullptr);
    float abc = s.width(0, 3);
    PrTextWordBreaks* words = prTextWordBreaksCreate();
    CHECK(s.ends({abc + 0.5f}, nullptr, words) == Ends{3, 6});
    prTextWordBreaksClear(words);
    prTextWordBreaksDestroy(words);
    // Without memory for the word, each line finds its opportunities again and gets the same answer.
    CHECK(s.ends({abc + 0.5f}) == Ends{3, 6});
    CHECK(prTextShapingIntrinsic(s.shaping, 0, 6, nullptr, nullptr).minContent == doctest::Approx(s.width(0, 6)));

    s.set("abcdef", PrTextBreakStyle{.overflowWrap = PrTextOverflowWrap_Anywhere});
    CHECK(prTextShapingInfo(s.shaping).emergency != nullptr);
    CHECK(s.ends({abc + 0.5f}) == Ends{3, 6});
    CHECK(prTextShapingIntrinsic(s.shaping, 0, 6, nullptr, nullptr).minContent < s.width(0, 6));

    s.set("abcdef");
    CHECK(s.ends({abc + 0.5f}) == Ends{6});
}

TEST_CASE("a line's runs are in visual order, with the white space that ends it at its paragraph's level") {
    Shaped s;
    std::string text = "abc \xd8\xb3\xd9\x84\xd8\xa7\xd9\x85";
    uint32_t size = uint32_t(text.size());
    s.set(text, std::vector<PrTextPiece>{s.piece(0, size)}, PrTextDirection_Rtl);
    CHECK(prTextShapingParagraphLevel(s.shaping, 0) == 1);
    REQUIRE(prTextShapingInfo(s.shaping).levels != nullptr);
    uint32_t count = prTextShapingLineRuns(s.shaping, 0, size, size, nullptr, 0);
    REQUIRE(count >= 2);
    std::vector<PrTextLineRun> parts(count);
    CHECK(prTextShapingLineRuns(s.shaping, 0, size, size, parts.data(), count) == count);
    // The Arabic goes left of the Latin.
    CHECK(parts.front().start >= 4);
    CHECK(parts.back().start == 0);

    // The glyphs of a right-to-left run are in the order of the text, as every run's are.
    const PrTextShapedGlyph* glyphs = prTextShapingInfo(s.shaping).glyphs;
    for (uint32_t g = 1; g < prTextShapingInfo(s.shaping).glyphCount; ++g)
        CHECK(glyphs[g - 1].cluster <= glyphs[g].cluster);

    // A line that ends in white space gives it the paragraph's level.
    s.set("abc  ", std::vector<PrTextPiece>{s.piece(0, 5)}, PrTextDirection_Rtl);
    count = prTextShapingLineRuns(s.shaping, 0, 5, 3, nullptr, 0);
    REQUIRE(count == 2);
    PrTextLineRun two[2];
    prTextShapingLineRuns(s.shaping, 0, 5, 3, two, 2);
    CHECK(two[0].start == 3);
    CHECK(two[0].level == 1);
    CHECK(two[1].start == 0);
}

TEST_CASE("a piece's letter and word spacing widen its advances by their px, and Arabic takes no letter spacing") {
    Shaped s;
    // An e with a combining acute is one typographic character unit.
    std::string text = "ab e\xcc\x81";
    uint32_t size = uint32_t(text.size());
    s.set(text);
    float plain = s.width(0, size);
    PrTextPiece spaced = s.piece(0, size);
    spaced.letterSpacing = 2;
    spaced.wordSpacing = 5;
    s.set(text, std::vector<PrTextPiece>{spaced});
    CHECK(s.width(0, size) == doctest::Approx(plain + 4 * 2 + 5));
    // The space after a character is part of its advance.
    CHECK(s.width(0, 1) == doctest::Approx(prTextShapingInfo(s.shaping).advances[0]));

    // Arabic letters, which join, take no letter spacing.
    std::string arabic = "\xd8\xb3\xd9\x84";
    s.set(arabic, std::vector<PrTextPiece>{s.piece(0, 4)}, PrTextDirection_Rtl);
    float joined = s.width(0, 4);
    PrTextPiece rtl = s.piece(0, 4);
    rtl.letterSpacing = 3;
    s.set(arabic, std::vector<PrTextPiece>{rtl}, PrTextDirection_Rtl);
    CHECK(s.width(0, 4) == doctest::Approx(joined));
}
