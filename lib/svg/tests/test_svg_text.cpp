// SVG fonts, and text set in them and in the host's fonts.

#include "pr/css.h"
#include "pr/dom_xml.h"
#include "pr/draw_commands.h"
#include "pr/string_ops.h"
#include "pr/svg.h"
#include "pr/svg_render.h"
#include "pr/text_kb.h"

#include "dom/src/dom_node.h"
#include "svg/src/svg_font.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

PrDomDocument* parse(const char* source) {
    PrDomDocument* document = prDomParseXml(source, "image/svg+xml", nullptr);
    prCssEnable(document);
    prSvgEnable(document);
    return document;
}

PrSvgElement* rootOf(PrDomDocument* document) {
    return prSvgElementQuery(prDomElementAsEventTarget(prDomDocumentElement(document)));
}

// A font with 1000 units per em whose glyphs are 100-unit squares: `a`, then the ligature `ab` that its order hides,
// `b`, a French `c` before an English one, and kerning between `a` and `b`.
constexpr const char* kFont = "<font horiz-adv-x='500'><font-face font-family='Squares' units-per-em='1000'/>"
    "<missing-glyph horiz-adv-x='700' d='M0 0h700v700z'/><glyph unicode=' ' horiz-adv-x='250'/>"
    "<glyph unicode='a' glyph-name='first' d='M0 0h100v100z'/><glyph unicode='ab' d='M0 0h100v100z'/>"
    "<glyph unicode='b' glyph-name='second' horiz-adv-x='300' d='M0 0h100v100z'/>"
    "<glyph unicode='c' lang='fr' d='M0 0h100v100z'/><glyph unicode='c' lang='en' d='M0 0h100v100z'/>"
    "<glyph unicode='d' horiz-adv-x='300'><rect width='100' height='200' fill='blue'/></glyph>"
    "<hkern g1='first' u2='U+0062' k='100'/></font>";

std::string svgOf(const char* text) {
    return std::string("<svg xmlns='http://www.w3.org/2000/svg'><defs>") + kFont + "</defs>" + text + "</svg>";
}

// Renders the document and returns the bounds of the paths it draws, and their count.
PrRect renderedPath(const std::string& source, uint32_t* paths) {
    PrDomDocument* document = parse(source.c_str());
    PrDrawList* list = prDrawListCreate();
    prSvgElementDraw(rootOf(document), PrVec2{100, 100}, nullptr, list);
    PrRect bounds{};
    *paths = 0;
    const PrDrawCommand* commands = prDrawListCommands(list);
    for (uint32_t i = 0; i < prDrawListCommandCount(list); ++i) {
        if (commands[i].kind != PrDrawCommandKind_Path)
            continue;
        PrRect path = prPathTightBounds(commands[i].path);
        bounds = *paths == 0 ? path : prRectUnion(bounds, path);
        ++*paths;
    }
    prDrawListRelease(list);
    prDomDocumentRelease(document);
    return bounds;
}

// Returns the SVG element with the ID `id` of `document`.
PrSvgElement* byId(PrDomDocument* document, const char* id) {
    PrDomElement* element = nullptr;
    std::string selector = std::string("#") + id;
    REQUIRE(prCssNodeQuerySelector(prDomDocumentAsNode(document), selector.c_str(), &element) == PrStatus_Ok);
    REQUIRE(element != nullptr);
    return prSvgElementQuery(prDomElementAsEventTarget(element));
}

bool near(PrRect a, PrRect b) {
    auto close = [](float x, float y) { return x - y < 1e-3f && y - x < 1e-3f; };
    return close(a.left, b.left) && close(a.top, b.top) && close(a.right, b.right) && close(a.bottom, b.bottom);
}

// The text with the ID `t` of a document of `body`, set in Squares at a font size of ten.
struct SquaresText {
    PrDomDocument* document;
    PrSvgElement* text;

    explicit SquaresText(const std::string& body) {
        document = parse(svgOf(body.c_str()).c_str());
        text = byId(document, "t");
    }

    ~SquaresText() { prDomDocumentRelease(document); }

    uint32_t count() const { return prSvgTextContentElementGetNumberOfChars(text); }

    // Returns where character `index` starts.
    PrVec2 at(uint32_t index) const {
        PrDomPoint point{};
        REQUIRE(prSvgTextContentElementGetStartPositionOfChar(text, index, &point) == PrStatus_Ok);
        return PrVec2{float(point.x), float(point.y)};
    }
};

// Returns the rotation of character `index` of the text.
float rotationOf(const SquaresText& text, uint32_t index) {
    float rotation = 0;
    REQUIRE(prSvgTextContentElementGetRotationOfChar(text.text, index, &rotation) == PrStatus_Ok);
    return rotation;
}

// Returns true if character `index` of the text is drawn, which a point inside its cell finds.
bool drawn(const SquaresText& text, uint32_t index) {
    PrDomRect extent{};
    REQUIRE(prSvgTextContentElementGetExtentOfChar(text.text, index, &extent) == PrStatus_Ok);
    PrDomPoint middle{extent.x + extent.width / 2, extent.y + extent.height / 2, 0, 1};
    return prSvgTextContentElementGetCharNumAtPosition(text.text, middle) == int32_t(index);
}

// A path that a drawing paints: its bounds, and whether its color is red.
struct PaintedPath {
    PrRect bounds;
    bool red;
};

// Renders the document of `source` and returns the paths that it paints, in order.
std::vector<PaintedPath> paintedPaths(const std::string& source) {
    PrDomDocument* document = parse(source.c_str());
    PrDrawList* list = prDrawListCreate();
    prSvgElementDraw(rootOf(document), PrVec2{200, 100}, nullptr, list);
    std::vector<PaintedPath> paths;
    const PrDrawCommand* commands = prDrawListCommands(list);
    for (uint32_t i = 0; i < prDrawListCommandCount(list); ++i) {
        if (commands[i].kind != PrDrawCommandKind_Path)
            continue;
        bool red = prFloat16ToFloat(commands[i].color.r) > 0.5f && prFloat16ToFloat(commands[i].color.g) < 0.5f;
        paths.push_back(PaintedPath{prPathTightBounds(commands[i].path), red});
    }
    prDrawListRelease(list);
    prDomDocumentRelease(document);
    return paths;
}

// Returns the document of `text`, with the Squares font and the style sheet `css`.
std::string styledSvg(const char* css, const char* text) {
    return std::string("<svg xmlns='http://www.w3.org/2000/svg'><style>") + css + "</style><defs>" + kFont +
        "</defs>" + text + "</svg>";
}

// Returns the union of the paths' bounds.
PrRect unionOf(const std::vector<PaintedPath>& paths) {
    PrRect all = paths.front().bounds;
    for (const PaintedPath& path : paths)
        all = prRectUnion(all, path.bounds);
    return all;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a unicode-range reads ranges, wildcards and a U+ on both ends") {
    Array<PrFontCodePointRange> ranges;
    REQUIRE(svgParseUnicodeRange("U+0-7F, u+4??,U+0064-U+0072", &ranges));
    REQUIRE(ranges.count == 3);
    CHECK(ranges.data[0].first == 0);
    CHECK(ranges.data[0].last == 0x7f);
    CHECK(ranges.data[1].first == 0x400);
    CHECK(ranges.data[1].last == 0x4ff);
    CHECK(ranges.data[2].first == 0x64);
    CHECK(ranges.data[2].last == 0x72);
    CHECK_FALSE(svgParseUnicodeRange("U+7F-0", &ranges));
    CHECK_FALSE(svgParseUnicodeRange("0-7F", &ranges));
    ranges.reset();
}

TEST_CASE("an SVG font matches the first glyph in document order, by language, and kerns") {
    std::string source = svgOf("");
    PrDomDocument* document = parse(source.c_str());
    const PrDomElement* font = nullptr;
    PrDomNode* root = reinterpret_cast<PrDomNode*>(document);
    for (PrDomNode* node = domNodeNext(root, root); node != nullptr; node = domNodeNext(root, node))
        if (const PrDomElement* e = domElementFromNode(node); e != nullptr && e->tag == PrDomTag_Font)
            font = e;
    REQUIRE(font != nullptr);
    SvgFont svg{};
    svgFontLoad(font, &svg);
    CHECK(svg.unitsPerEm == 1000.0f);
    size_t length = 0;
    auto match = [&](const char* text, size_t pos, const char* language) {
        PrStringView lang = language != nullptr ? language : PrStringView{};
        return svgFontMatch(svg, text, pos, lang, SvgArabicForm_Any, &length);
    };
    // `a` comes before `ab`, which it hides.
    uint32_t a = match("ab", 0, nullptr);
    CHECK(length == 1);
    uint32_t b = match("ab", 1, nullptr);
    CHECK(svg.glyphs.data[b].advance == 300.0f);
    CHECK(svgFontKerning(svg, a, b) == 100.0f);
    CHECK(svgFontKerning(svg, b, a) == 0.0f);
    uint32_t english = match("c", 0, "en-US");
    uint32_t french = match("c", 0, "fr");
    CHECK(english == french + 1);
    CHECK(match("c", 0, "de") == kSvgMissingGlyph);
    CHECK(match("\xc3\xa9", 0, nullptr) == kSvgMissingGlyph);
    CHECK(length == 2);
    CHECK(svg.glyphs.data[kSvgMissingGlyph].advance == 700.0f);
    svgFontReset(&svg);
    prDomDocumentRelease(document);
}

TEST_CASE("text in an SVG font draws its glyphs kerned, anchored and y up from the baseline") {
    uint32_t paths = 0;
    // `a` is at 5, and `b` is a half-em advance later, minus the tenth of an em that kerning removes.
    PrRect start = renderedPath(svgOf("<text font-family='Squares' font-size='10' x='5' y='20'>ab</text>"), &paths);
    CHECK(paths == 1);
    CHECK(near(start, PrRect{5, 19, 10, 20}));
    // The advance, 5 - 1 + 3, ends at x.
    PrRect end = renderedPath(
        svgOf("<text font-family='Squares' font-size='10' x='5' y='20' text-anchor='end'>ab</text>"), &paths);
    CHECK(near(end, PrRect{-2, 19, 3, 20}));
}

TEST_CASE("text collapses white space, and positions characters as its elements' lists say") {
    uint32_t paths = 0;
    // The space between `a` and `b` is the second character, so `b` is at the third x.
    PrRect bounds = renderedPath(
        svgOf("<text font-family='Squares' font-size='10' x='0 20 40' y='20'>\n  a \t b  </text>"), &paths);
    CHECK(near(bounds, PrRect{0, 19, 41, 20}));
    // A tspan's dy moves its characters and those after them. Its x starts a chunk.
    bounds = renderedPath(svgOf("<text font-family='Squares' font-size='10' y='20'>a<tspan dy='5'>b</tspan>"
        "<tspan x='50'>a</tspan></text>"), &paths);
    CHECK(near(bounds, PrRect{0, 19, 51, 25}));
    // Kerning applies across elements: `b` is a half-em after `a`, minus a tenth.
    bounds = renderedPath(svgOf("<text font-family='Squares' font-size='10' y='20'>a<tspan fill='blue'>b</tspan>"
        "</text>"), &paths);
    CHECK(near(bounds, PrRect{0, 19, 5, 20}));
}

TEST_CASE("right-to-left text is set in visual order and anchored at its start on the right") {
    uint32_t paths = 0;
    // With the direction overridden to right to left, `b` is left of `a`, and the text ends at x. `a` is half an em
    // wide and `b` three tenths, and there is no kerning for `b` then `a`.
    PrRect bounds = renderedPath(svgOf("<text font-family='Squares' font-size='10' x='20' y='20' direction='rtl'"
        " unicode-bidi='bidi-override'>ab</text>"), &paths);
    CHECK(near(bounds, PrRect{12, 19, 16, 20}));
}

TEST_CASE("a textArea breaks its text into lines a font size apart that fit its width") {
    uint32_t paths = 0;
    // `a` is half an em wide and a space a quarter, so at a font size of ten no two words fit in a width of ten.
    PrRect wrapped = renderedPath(svgOf("<textArea font-family='Squares' font-size='10' x='0' y='0' width='10'>a a a"
        "</textArea>"), &paths);
    CHECK(near(wrapped, PrRect{0, 9, 1, 30}));
    // A tbreak ends a line, and two make an empty line between them.
    PrRect broken = renderedPath(svgOf("<textArea font-family='Squares' font-size='10' x='0' y='0'>a<tbreak/><tbreak/>a"
        "</textArea>"), &paths);
    CHECK(near(broken, PrRect{0, 9, 1, 30}));
    // A line that ends below the height is not drawn.
    PrRect clipped = renderedPath(svgOf("<textArea font-family='Squares' font-size='10' x='0' y='0' width='10'"
        " height='25'>a a a</textArea>"), &paths);
    CHECK(near(clipped, PrRect{0, 9, 1, 20}));
}

TEST_CASE("a glyph's child elements draw it in font units with y up") {
    std::string source = svgOf("<text font-family='Squares' font-size='10' x='5' y='20'>ad</text>");
    PrDomDocument* document = parse(source.c_str());
    PrDrawList* list = prDrawListCreate();
    prSvgElementDraw(rootOf(document), PrVec2{100, 100}, nullptr, list);
    const PrDrawCommand* commands = prDrawListCommands(list);
    PrAffine2 transform{};
    PrRect rect{};
    uint32_t rects = 0;
    for (uint32_t i = 0; i < prDrawListCommandCount(list); ++i) {
        if (commands[i].kind == PrDrawCommandKind_PushTransformAffine2)
            transform = prAffine2FromDAffine2(commands[i].affine);
        if (commands[i].kind == PrDrawCommandKind_Rect) {
            rect = prAffine2MapRect(transform, commands[i].rect);
            ++rects;
        }
    }
    // The `d` is a half-em after `a`. Its rectangle is 100 units across and 200 up from the baseline.
    CHECK(rects == 1);
    CHECK(near(rect, PrRect{10, 18, 11, 20}));
    prDrawListRelease(list);
    prDomDocumentRelease(document);
}

TEST_CASE("a baseline of the font aligns with the pen, and baseline-shift moves an element's") {
    uint32_t paths = 0;
    // The font's em is its ascent and it has no descent, so its central baseline is half an em up. The glyphs move down
    // half an em to put that baseline on the pen.
    PrRect central = renderedPath(
        svgOf("<text font-family='Squares' font-size='10' y='20' dominant-baseline='central'>a</text>"), &paths);
    CHECK(near(central, PrRect{0, 24, 1, 25}));
    // An element shifted down by a percentage of its font size, inside an element shifted up by a length.
    PrRect shifted = renderedPath(svgOf("<text font-family='Squares' font-size='10' y='20'><tspan baseline-shift='5'>"
        "<tspan baseline-shift='-20%'>a</tspan></tspan></text>"), &paths);
    CHECK(near(shifted, PrRect{0, 16, 1, 17}));
}

TEST_CASE("text in the host's fonts is shaped, and a generic family is the set's alias") {
    PrFontSet* set = prFontSetCreate(nullptr);
    PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
    const PrFont* font = nullptr;
    REQUIRE(prFontCreateFromFile(PR_FONTS_DIR "/NotoSans.ttf", 0, nullptr, &font) == PrStatus_Ok);
    prFontSetAdd(set, &descriptor, font);
    prFontRelease(font);
    prFontSetSetGeneric(set, PrFontGeneric_SansSerif, "Noto Sans");
    PrSvgDrawOptions options{};
    options.fonts = set;
    options.shaper = prTextKbCreate();
    PrDomDocument* document = parse("<svg xmlns='http://www.w3.org/2000/svg'><text font-family='Missing, sans-serif'"
        " font-size='20' x='10' y='30'>Hello</text></svg>");
    PrDrawList* list = prDrawListCreate();
    prSvgElementDraw(rootOf(document), PrVec2{100, 100}, &options, list);
    const PrFontGlyphRun* run = nullptr;
    const PrDrawCommand* commands = prDrawListCommands(list);
    for (uint32_t i = 0; i < prDrawListCommandCount(list); ++i)
        if (commands[i].kind == PrDrawCommandKind_Glyphs)
            run = commands[i].glyphs;
    REQUIRE(run != nullptr);
    CHECK(run->glyphCount == 5);
    CHECK(run->size == 20.0f);
    CHECK(run->glyphs[0].position.x == 10.0f);
    CHECK(run->glyphs[0].position.y == 30.0f);
    CHECK(run->glyphs[4].position.x > run->glyphs[0].position.x);
    prDrawListRelease(list);
    prDomDocumentRelease(document);
    prTextShaperDestroy(options.shaper);
    prFontSetDestroy(set);
}

// ---------------------------------------------------------------------------------------------------------------------
// SVGTextContentElement

TEST_CASE("a text content element counts its addressable characters and their advances") {
    // `a` is half an em, a space a quarter and `b` three tenths, at a font size of ten. Kerning moves `b` but is not
    // part of an advance. Collapsed white space is not addressable.
    PrDomDocument* document = parse(svgOf("<text id='t' font-family='Squares' font-size='10' x='5' y='20'>  a  "
        "<tspan id='s'>b</tspan>ab</text>") .c_str());
    PrSvgElement* text = byId(document, "t");
    PrSvgElement* tspan = byId(document, "s");
    CHECK(prSvgElementInterface(text) == PrSvgInterface_TextContent);
    CHECK(prSvgTextContentElementGetNumberOfChars(text) == 5);
    CHECK(prSvgTextContentElementGetComputedTextLength(text) == doctest::Approx(5 + 2.5 + 3 + 5 + 3));
    CHECK(prSvgTextContentElementGetNumberOfChars(tspan) == 1);
    CHECK(prSvgTextContentElementGetComputedTextLength(tspan) == doctest::Approx(3));
    float length = 0;
    REQUIRE(prSvgTextContentElementGetSubStringLength(text, 1, 2, &length) == PrStatus_Ok);
    CHECK(length == doctest::Approx(2.5 + 3));
    CHECK(prSvgTextContentElementGetSubStringLength(text, 5, 1, &length) == PrStatus_OutOfRange);
    prDomDocumentRelease(document);
}

TEST_CASE("a character's positions, extent and rotation are its glyph's") {
    // The font's em is its ascent and it has no descent.
    PrDomDocument* document = parse(svgOf("<text id='t' font-family='Squares' font-size='10' x='5 20' y='20' rotate='0 90'>ab</text>")
        .c_str());
    PrSvgElement* text = byId(document, "t");
    PrDomPoint start{};
    PrDomPoint end{};
    REQUIRE(prSvgTextContentElementGetStartPositionOfChar(text, 0, &start) == PrStatus_Ok);
    REQUIRE(prSvgTextContentElementGetEndPositionOfChar(text, 0, &end) == PrStatus_Ok);
    CHECK(start.x == doctest::Approx(5));
    CHECK(start.y == doctest::Approx(20));
    CHECK(end.x == doctest::Approx(10));
    PrDomRect extent{};
    REQUIRE(prSvgTextContentElementGetExtentOfChar(text, 0, &extent) == PrStatus_Ok);
    CHECK(extent.x == doctest::Approx(5));
    CHECK(extent.y == doctest::Approx(10));
    CHECK(extent.width == doctest::Approx(5));
    CHECK(extent.height == doctest::Approx(10));
    // `b`, turned 90 degrees, runs down from 20, 20, and its cell lies to the right of its baseline.
    float rotation = 0;
    REQUIRE(prSvgTextContentElementGetRotationOfChar(text, 1, &rotation) == PrStatus_Ok);
    CHECK(rotation == doctest::Approx(90));
    REQUIRE(prSvgTextContentElementGetEndPositionOfChar(text, 1, &end) == PrStatus_Ok);
    CHECK(end.x == doctest::Approx(20));
    CHECK(end.y == doctest::Approx(23));
    REQUIRE(prSvgTextContentElementGetExtentOfChar(text, 1, &extent) == PrStatus_Ok);
    CHECK(extent.x == doctest::Approx(20));
    CHECK(extent.y == doctest::Approx(20));
    CHECK(extent.width == doctest::Approx(10));
    CHECK(extent.height == doctest::Approx(3));
    CHECK(prSvgTextContentElementGetCharNumAtPosition(text, PrDomPoint{7, 15, 0, 1}) == 0);
    CHECK(prSvgTextContentElementGetCharNumAtPosition(text, PrDomPoint{25, 21, 0, 1}) == 1);
    CHECK(prSvgTextContentElementGetCharNumAtPosition(text, PrDomPoint{50, 50, 0, 1}) == -1);
    CHECK(prSvgTextContentElementGetRotationOfChar(text, 2, &rotation) == PrStatus_OutOfRange);
    prDomDocumentRelease(document);
}

TEST_CASE("a character outside the basic multilingual plane is two code units") {
    // The missing glyph is 700 units wide.
    PrDomDocument* document =
        parse(svgOf("<text id='t' font-family='Squares' font-size='10'>\xf0\x9f\x98\x80" "a</text>").c_str());
    PrSvgElement* text = byId(document, "t");
    CHECK(prSvgTextContentElementGetNumberOfChars(text) == 3);
    PrDomPoint start{};
    REQUIRE(prSvgTextContentElementGetStartPositionOfChar(text, 2, &start) == PrStatus_Ok);
    CHECK(start.x == doctest::Approx(7));
    float length = 0;
    REQUIRE(prSvgTextContentElementGetSubStringLength(text, 0, 2, &length) == PrStatus_Ok);
    CHECK(length == doctest::Approx(7));
    prDomDocumentRelease(document);
}

// ---------------------------------------------------------------------------------------------------------------------
// White space and lines

TEST_CASE("collapsible white space is one space, and none at the start or end of a line") {
    // A segment break is a space among the spaces around it (css-text-4/#white-space-phase-1).
    SquaresText text("<text id='t' font-family='Squares' font-size='10'>\n  a \n\t b  \n</text>");
    CHECK(text.count() == 3);
    CHECK(text.at(2).x == doctest::Approx(7.5));
    // Of spaces at the end of one element and the start of the next, the first is kept.
    SquaresText across("<text id='t' font-family='Squares' font-size='10'>a <tspan id='s'> b</tspan></text>");
    CHECK(across.count() == 3);
    CHECK(prSvgTextContentElementGetNumberOfChars(byId(across.document, "s")) == 1);
}

TEST_CASE("xml:space preserve keeps every space, and makes tabs and segment breaks spaces") {
    SquaresText text("<text id='t' font-family='Squares' font-size='10' xml:space='preserve'> a\n\tb</text>");
    CHECK(text.count() == 5);
    CHECK(text.at(4).x == doctest::Approx(2.5 + 5 + 2.5 + 2.5));
    // white-space on the element overrides xml:space (SVG2/text.html#DuplicateWhiteSpace).
    SquaresText overridden("<text id='t' font-family='Squares' font-size='10' xml:space='preserve' "
        "style='white-space: normal'> a  b</text>");
    CHECK(overridden.count() == 3);
}

TEST_CASE("white-space pre and pre-line break lines at segment breaks, line-height apart") {
    SquaresText pre("<text id='t' font-family='Squares' font-size='10' x='5' y='20' "
        "style='white-space: pre; line-height: 20px'>a\n  b</text>");
    CHECK(pre.count() == 5);
    CHECK(pre.at(4).x == doctest::Approx(10));
    CHECK(pre.at(4).y == doctest::Approx(40));
    // pre-line collapses the spaces, which do not start the line.
    SquaresText preLine("<text id='t' font-family='Squares' font-size='10' x='5' y='20' "
        "style='white-space: pre-line; line-height: 20px'>a  \n  b</text>");
    CHECK(preLine.count() == 3);
    CHECK(preLine.at(2).x == doctest::Approx(5));
    CHECK(preLine.at(2).y == doctest::Approx(40));
}

TEST_CASE("a line's height is the tallest box of its characters and the text's strut") {
    // The font has an ascent of an em and no descent. A line-height of 20 puts 5 above and below the em, and the
    // tspan's 40 puts 15 below it, so the next baseline is 15 + 15 below the first.
    SquaresText text("<text id='t' font-family='Squares' font-size='10' y='20' "
        "style='white-space: pre-line; line-height: 20px'>a<tspan style='line-height: 40px'>a</tspan>\nb</text>");
    CHECK(text.at(3).y == doctest::Approx(50));
}

TEST_CASE("textLength does not apply to an element with a forced line break") {
    SquaresText text("<text id='t' font-family='Squares' font-size='10' textLength='100' "
        "style='white-space: pre; line-height: 20px'>a\na</text>");
    CHECK(text.at(2).x == doctest::Approx(0));
    CHECK(text.at(2).y == doctest::Approx(20));
}

TEST_CASE("inline-size wraps lines at soft wrap opportunities, and nowrap does not") {
    // `a ` is 7.5 and `a a` 12.5, so in 10 each `a` is a line of its own.
    SquaresText text("<text id='t' font-family='Squares' font-size='10' x='5' y='20' "
        "style='inline-size: 10px; line-height: 20px'>a a a</text>");
    CHECK(text.at(2).x == doctest::Approx(5));
    CHECK(text.at(2).y == doctest::Approx(40));
    CHECK(text.at(4).y == doctest::Approx(60));
    SquaresText nowrap("<text id='t' font-family='Squares' font-size='10' x='5' y='20' "
        "style='inline-size: 10px; white-space: nowrap'>a a a</text>");
    CHECK(nowrap.at(4).x == doctest::Approx(20));
    CHECK(nowrap.at(4).y == doctest::Approx(20));
}

TEST_CASE("wrapped text ignores every position but the first, and each line is anchored") {
    SquaresText text("<text id='t' font-family='Squares' font-size='10' x='50' y='20' text-anchor='middle' "
        "style='inline-size: 10px; line-height: 20px'>a <tspan x='0' dx='30' rotate='45'>bb</tspan></text>");
    // `a` is centered on 50, and `bb`, 6 long, on the next line.
    CHECK(text.at(0).x == doctest::Approx(47.5));
    CHECK(text.at(2).x == doctest::Approx(47));
    CHECK(text.at(2).y == doctest::Approx(40));
    float rotation = -1;
    REQUIRE(prSvgTextContentElementGetRotationOfChar(text.text, 2, &rotation) == PrStatus_Ok);
    CHECK(rotation == 0.0f);
}

TEST_CASE("a soft hyphen and a bidi control are not addressable") {
    SquaresText text("<text id='t' font-family='Squares' font-size='10' x='0 20'>a\xc2\xad\xe2\x80\x8e" "b</text>");
    CHECK(text.count() == 2);
    CHECK(text.at(1).x == doctest::Approx(20));
}

// ---------------------------------------------------------------------------------------------------------------------
// Text on a path

TEST_CASE("a textPath follows a path, the path attribute before href") {
    SquaresText text("<path id='p' d='M 0 80 H 100'/><text id='t' font-family='Squares' font-size='10'>"
        "<textPath href='#p' path='M 0 50 H 100' startOffset='10'>aa</textPath></text>");
    CHECK(text.at(0).x == doctest::Approx(10));
    CHECK(text.at(0).y == doctest::Approx(50));
    CHECK(text.at(1).x == doctest::Approx(15));
}

TEST_CASE("a textPath follows a basic shape's equivalent path, which a rect starts at its top left") {
    // The rect's path runs right along the top, 40, down the right side, 20, left along the bottom and up the left.
    SquaresText text("<rect id='r' x='10' y='10' width='40' height='20'/><text id='t' font-family='Squares' "
        "font-size='10'><textPath href='#r'>aaaaaaaaaa</textPath></text>");
    CHECK(text.at(0).x == doctest::Approx(10));
    CHECK(text.at(0).y == doctest::Approx(10));
    CHECK(rotationOf(text, 0) == doctest::Approx(0));
    // The ninth `a` starts 40 along and its middle is 42.5 along, down the right side.
    CHECK(text.at(8).x == doctest::Approx(50));
    CHECK(rotationOf(text, 8) == doctest::Approx(90));
}

TEST_CASE("a textPath on a closed path goes round it once, from startOffset") {
    // The square is 80 round, and 20 `a`s are 100 long: those whose middles are past one circuit are not drawn.
    SquaresText text("<text id='t' font-family='Squares' font-size='10'><textPath path='M 0 0 H 20 V 20 H 0 Z' "
        "startOffset='20'>aaaaaaaaaaaaaaaaaaaa</textPath></text>");
    CHECK(text.at(0).x == doctest::Approx(20));
    CHECK(text.at(0).y == doctest::Approx(0));
    // The fifth `a` starts 40 along, at the bottom right corner, and the thirteenth 80 along, back at the path's start.
    CHECK(text.at(4).x == doctest::Approx(20));
    CHECK(text.at(4).y == doctest::Approx(20));
    CHECK(text.at(12).x == doctest::Approx(0));
    CHECK(text.at(12).y == doctest::Approx(0));
    CHECK(drawn(text, 15));
    CHECK_FALSE(drawn(text, 16));
}

TEST_CASE("side right reverses the path, and the text hangs from it") {
    SquaresText text("<text id='t' font-family='Squares' font-size='10'><textPath path='M 0 50 H 100' "
        "side='right'>a</textPath></text>");
    CHECK(text.at(0).x == doctest::Approx(100));
    CHECK(text.at(0).y == doctest::Approx(50));
    CHECK(std::abs(rotationOf(text, 0)) == doctest::Approx(180));
}

TEST_CASE("in a textPath, x is a distance along the path, and y is ignored") {
    SquaresText text("<text id='t' font-family='Squares' font-size='10'><textPath path='M 0 50 H 100'>a"
        "<tspan x='60' y='10'>b</tspan></textPath></text>");
    CHECK(text.at(1).x == doctest::Approx(60));
    CHECK(text.at(1).y == doctest::Approx(50));
}

TEST_CASE("text after a textPath starts at the end of its path") {
    SquaresText text("<text id='t' font-family='Squares' font-size='10'><textPath path='M 0 50 L 30 10'>a"
        "</textPath>b</text>");
    CHECK(text.at(1).x == doctest::Approx(30));
    CHECK(text.at(1).y == doctest::Approx(10));
    CHECK(rotationOf(text, 1) == doctest::Approx(0));
}

TEST_CASE("a textPath whose href names no path or shape draws nothing") {
    SquaresText text("<g id='g'/><text id='t' font-family='Squares' font-size='10'>a<textPath href='#g'>b</textPath>"
        "</text>");
    CHECK(drawn(text, 0));
    CHECK_FALSE(drawn(text, 1));
}

TEST_CASE("startOffset is in the units of the target's pathLength, and a percentage is of the path") {
    SquaresText length("<path id='p' d='M 0 50 H 100' pathLength='10'/><text id='t' font-family='Squares' "
        "font-size='10'><textPath href='#p' startOffset='2'>a</textPath></text>");
    CHECK(length.at(0).x == doctest::Approx(20));
    SquaresText percent("<text id='t' font-family='Squares' font-size='10'><textPath path='M 0 50 H 100' "
        "startOffset='30%'>a</textPath></text>");
    CHECK(percent.at(0).x == doctest::Approx(30));
}

TEST_CASE("method stretch bends a glyph's outline onto the path") {
    // A square glyph on a quarter circle: its outline's points stay on the circle's normals, so the outline's bounds
    // are not a turned square's.
    uint32_t paths = 0;
    PrRect bent = renderedPath(svgOf("<text font-family='Squares' font-size='100'><textPath "
        "path='M 0 100 A 100 100 0 0 1 100 0' method='stretch'>a</textPath></text>"), &paths);
    PrRect turned = renderedPath(svgOf("<text font-family='Squares' font-size='100'><textPath "
        "path='M 0 100 A 100 100 0 0 1 100 0'>a</textPath></text>"), &paths);
    CHECK(paths == 1);
    CHECK_FALSE(near(bent, turned));
}

// ---------------------------------------------------------------------------------------------------------------------
// Vertical text

TEST_CASE("vertical text runs down from its position, Latin on its side and upright when text-orientation says") {
    // `a` is set on its side, advancing half an em down; upright, it advances the font's em. Either way, it starts on
    // the column's line.
    SquaresText sideways("<text id='t' font-family='Squares' font-size='10' x='50' y='10' "
        "writing-mode='vertical-rl'>aa</text>");
    CHECK(sideways.at(1).x == doctest::Approx(50));
    CHECK(sideways.at(1).y == doctest::Approx(15));
    CHECK(rotationOf(sideways, 0) == doctest::Approx(90));
    SquaresText upright("<text id='t' font-family='Squares' font-size='10' x='50' y='10' "
        "style='writing-mode: vertical-rl; text-orientation: upright'>aa</text>");
    CHECK(upright.at(1).x == doctest::Approx(50));
    CHECK(upright.at(1).y == doctest::Approx(20));
    CHECK(rotationOf(upright, 0) == doctest::Approx(0));
    // glyph-orientation-vertical is a shorthand of text-orientation.
    SquaresText legacy("<text id='t' font-family='Squares' font-size='10' x='50' y='10' writing-mode='tb' "
        "glyph-orientation-vertical='0'>aa</text>");
    CHECK(legacy.at(1).y == doctest::Approx(20));
}

TEST_CASE("vertical text anchors along its column, and its lines stack across") {
    SquaresText anchored("<text id='t' font-family='Squares' font-size='10' x='50' y='50' text-anchor='middle' "
        "writing-mode='vertical-rl'>aa</text>");
    CHECK(anchored.at(0).y == doctest::Approx(45));
    // vertical-rl stacks lines to the left, and vertical-lr to the right, a line-height apart.
    SquaresText rl("<text id='t' font-family='Squares' font-size='10' x='50' y='10' "
        "style='writing-mode: vertical-rl; white-space: pre; line-height: 20px'>a\na</text>");
    CHECK(rl.at(2).x == doctest::Approx(30));
    CHECK(rl.at(2).y == doctest::Approx(10));
    SquaresText lr("<text id='t' font-family='Squares' font-size='10' x='50' y='10' "
        "style='writing-mode: vertical-lr; white-space: pre; line-height: 20px'>a\na</text>");
    CHECK(lr.at(2).x == doctest::Approx(70));
}

TEST_CASE("vertical text wraps at an inline-size down the column") {
    SquaresText text("<text id='t' font-family='Squares' font-size='10' x='50' y='10' "
        "style='writing-mode: vertical-rl; inline-size: 10px; line-height: 20px'>a a a</text>");
    CHECK(text.at(2).x == doctest::Approx(30));
    CHECK(text.at(2).y == doctest::Approx(10));
}

TEST_CASE("an element outside text, or a text that does not render, has no characters") {
    PrDomDocument* document = parse(svgOf("<rect id='r' width='10' height='10'/><g style='display: none'>"
        "<text id='t' font-family='Squares'>ab</text></g>") .c_str());
    CHECK(prSvgElementInterface(byId(document, "r")) == PrSvgInterface_Rect);
    CHECK(prSvgTextContentElementGetNumberOfChars(byId(document, "r")) == 0);
    CHECK(prSvgTextContentElementGetNumberOfChars(byId(document, "t")) == 0);
    prDomDocumentRelease(document);
}

TEST_CASE("::first-line styles the text on a text element's first line") {
    std::vector<PaintedPath> one = paintedPaths(styledSvg(
        "text::first-line { fill: red }", "<text font-family='Squares' font-size='10' y='20'>aa</text>"));
    REQUIRE(one.size() == 1);
    CHECK(one[0].red);
    // Only the first line of wrapped text is styled, and a tspan on that line inherits from it.
    std::vector<PaintedPath> wrapped = paintedPaths(styledSvg("text::first-line { fill: red }",
        "<text font-family='Squares' font-size='10' y='20' style='inline-size: 12px'><tspan>a</tspan> a</text>"));
    REQUIRE(wrapped.size() == 2);
    CHECK(wrapped[0].red);
    CHECK(!wrapped[1].red);
    CHECK(wrapped[1].bounds.top > wrapped[0].bounds.top);
}

TEST_CASE("::first-letter styles the first letter and the punctuation around it") {
    std::vector<PaintedPath> paths = paintedPaths(styledSvg(
        "text::first-letter { fill: red }", "<text font-family='Squares' font-size='10' y='20'>ab</text>"));
    REQUIRE(paths.size() == 2);
    CHECK(paths[0].red);
    CHECK(!paths[1].red);
    CHECK(paths[0].bounds.right <= paths[1].bounds.left);
    // The first letter inherits from ::first-line. A tspan's own ::first-letter styles nothing, since only a text
    // element is a block container.
    std::vector<PaintedPath> lined = paintedPaths(styledSvg("text::first-line { fill: red } text::first-letter { stroke: blue }",
        "<text font-family='Squares' font-size='10' y='20'>ab</text>"));
    REQUIRE(lined.size() >= 2);
    CHECK(lined[0].red);
    std::vector<PaintedPath> inner = paintedPaths(styledSvg("tspan::first-letter { fill: red }",
        "<text font-family='Squares' font-size='10' y='20'><tspan>ab</tspan></text>"));
    REQUIRE(inner.size() == 1);
    CHECK(!inner[0].red);
}

TEST_CASE("::before and ::after generate nothing in SVG text") {
    std::vector<PaintedPath> paths = paintedPaths(styledSvg("text::before, tspan::after { content: 'a' }",
        "<text font-family='Squares' font-size='10' y='20'><tspan>a</tspan></text>"));
    REQUIRE(paths.size() == 1);
    CHECK(paths[0].bounds.right - paths[0].bounds.left == doctest::Approx(1));
}

TEST_CASE("overflow-wrap and word-break break a word that fits no line") {
    // "aaaa" is 20 long in lines of 12: it overflows unless a style lets it break.
    SquaresText normal("<text id='t' font-family='Squares' font-size='10' x='0' y='10' "
        "style='inline-size: 12px'>aaaa</text>");
    CHECK(normal.at(3).y == doctest::Approx(10));
    for (const char* style : {"overflow-wrap: anywhere", "overflow-wrap: break-word", "word-break: break-word"}) {
        CAPTURE(style);
        SquaresText broken(std::string("<text id='t' font-family='Squares' font-size='10' x='0' y='10' "
            "style='inline-size: 12px; line-height: 20px; ") + style + "'>aaaa</text>");
        CHECK(broken.at(2).x == doctest::Approx(0));
        CHECK(broken.at(2).y == doctest::Approx(30));
    }
    // break-all breaks between letters, where the word would otherwise move whole to the next line.
    SquaresText all("<text id='t' font-family='Squares' font-size='10' x='0' y='10' "
        "style='inline-size: 17px; line-height: 20px; word-break: break-all'>a aaa</text>");
    CHECK(all.at(2).x == doctest::Approx(7.5));
    CHECK(all.at(2).y == doctest::Approx(10));
    CHECK(all.at(4).y == doctest::Approx(30));
}

TEST_CASE("text-overflow clips or ends with an ellipsis the lines that overflow the wrapping area") {
    // "aaaa" is 20 long in a wrapping area of 12 from x 0, and white-space: pre keeps it on one line.
    std::vector<PaintedPath> plain = paintedPaths(svgOf(
        "<text font-family='Squares' font-size='10' y='20' style='inline-size: 12px; white-space: pre'>aaaa</text>"));
    REQUIRE(plain.size() == 1);
    CHECK(plain[0].bounds.right > 15);
    // The ellipsis is the missing glyph, 7 wide, which leaves room for one a.
    std::vector<PaintedPath> ellipsis = paintedPaths(svgOf("<text font-family='Squares' font-size='10' y='20' "
        "style='inline-size: 12px; white-space: pre; text-overflow: ellipsis'>aaaa</text>"));
    REQUIRE(!ellipsis.empty());
    CHECK(unionOf(ellipsis).right == doctest::Approx(12));
    // The characters measure as if nothing were hidden.
    SquaresText measured("<text id='t' font-family='Squares' font-size='10' y='20' "
        "style='inline-size: 12px; white-space: pre; text-overflow: ellipsis'>aaaa</text>");
    CHECK(measured.count() == 4);
    CHECK(measured.at(3).x == doctest::Approx(15));
}
