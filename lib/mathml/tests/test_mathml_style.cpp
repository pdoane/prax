// MathML's styles: the user agent's sheet, the hints of MathML's attributes and the CSS properties of math, as the
// computed style shows them.

#include "pr/css.h"
#include "pr/css_presentation.h"
#include "pr/font.h"
#include "pr/font_set.h"
#include "pr/html.h"
#include "pr/html_page.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/layout_value.h"
#include "pr/text_font.h"
#include "pr/text_harfbuzz.h"
#include "pr/text_transform.h"

#include "tools/harness/test.h"

#include <cmath>
#include <cstdlib>
#include <string>

namespace pr_impl {
namespace {

// Loads the font in the file `name` of the test fonts.
const PrFont* load(const char* name) {
    std::string path = std::string(PR_FONTS_DIR) + "/" + name;
    const PrFont* font = nullptr;
    REQUIRE(prFontCreateFromFile(path, 0, nullptr, &font) == PrStatus_Ok);
    return font;
}

// A document presented over Tinos, with Noto Sans Math as the math family, or with Tinos for math too without
// `mathFont`.
struct Page {
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextHarfBuzzCreate();
    PrTextFont* defaultFont = nullptr;
    PrDomAgent* agent = nullptr;
    PrDomDocument* document = nullptr;
    PrCssPresentation* presentation = nullptr;

    explicit Page(const char* html, bool mathFont = true) {
        const char* files[] = {"Tinos.ttf", "NotoSansMath.ttf"};
        for (const char* file : files) {
            const PrFont* font = load(file);
            PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
            prFontSetAdd(fonts, &descriptor, font);
            prFontRelease(font);
        }
        prFontSetSetGeneric(fonts, PrFontGeneric_Math, mathFont ? "Noto Sans Math" : "Tinos");
        prFontSetSetDefaultFamily(fonts, prFontFamilyFromName("Tinos"));
        PrTextFontDesc defaultFontDesc{.set = fonts, .style = prFontStyleDefault()};
        defaultFont = prTextFontCreate(&defaultFontDesc);
        document = prHtmlDomParserParseFromString(html, PrHtmlDomParserSupportedType_TextHtml);
        PrDomAgentOptions agentOptions = prDomAgentOptionsDefault();
        agent = prDomAgentCreate(&agentOptions);
        uint32_t count = 0;
        const PrCssUserAgentSheet* sheets = prHtmlUserAgentSheets(&count);
        PrCssPresentationDesc desc{
            .document = document,
            .font = defaultFont,
            .shaper = shaper,
            .agent = agent,
            .userAgentSheets = sheets,
            .userAgentSheetCount = count,
        };
        presentation = prCssPresentationCreate(&desc);
        prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    }

    ~Page() {
        prCssPresentationDestroy(presentation);
        prDomDocumentRelease(document);
        prDomAgentDestroy(agent);
        prTextFontRelease(defaultFont);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
    }

    // Returns the resolved value of `property` of the element with `id`, as a script reads it.
    std::string operator()(const char* id, PrCssProperty property) {
        PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(document), id);
        REQUIRE(element != nullptr);
        PrCssDeclarationName name = prCssDeclarationNameFromProperty(property);
        PrCssStyleProperties* computed = prCssElementComputedStyle(element, "");
        PrRawString read = prCssStyleDeclarationGetPropertyValue(prCssStylePropertiesAsCssStyleDeclaration(computed), &name);
        std::string value(prRawStringView(&read).data, prRawStringView(&read).size);
        prRawStringReset(&read);
        return value;
    }

    // Returns the element with `id`.
    PrDomElement* element(const char* id) {
        PrDomElement* found = prDomNodeGetElementById(prDomDocumentAsNode(document), id);
        REQUIRE(found != nullptr);
        return found;
    }

    // Returns the font features that the box of the element with `id` sets its text with.
    PrLayoutFontStyle font(const char* id) {
        PrLayoutTree* tree = prCssPresentationTree(presentation);
        PrLayoutNode node = prCssPresentationLayoutNode(presentation, prDomElementAsNode(element(id)));
        REQUIRE(node != kPrLayoutNodeNone);
        PrLayoutStyle style = prLayoutStyleRecord(tree, prLayoutNodeStyle(tree, node));
        return prLayoutFontStyleRecord(tree, style.groups[PrLayoutGroup_Font]);
    }

    // Returns the text transform that the box of the element with `id` sets its text in.
    PrTextTransform textTransform(const char* id) {
        PrLayoutTree* tree = prCssPresentationTree(presentation);
        PrLayoutNode node = prCssPresentationLayoutNode(presentation, prDomElementAsNode(element(id)));
        REQUIRE(node != kPrLayoutNodeNone);
        PrLayoutStyle style = prLayoutStyleRecord(tree, prLayoutNodeStyle(tree, node));
        return prLayoutFontStyleRecord(tree, style.groups[PrLayoutGroup_Font]).textTransform;
    }

    // Updates the presentation, and checks it against one computed from scratch.
    void update() {
        prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
        PrRawString report{};
        bool same = prCssPresentationVerify(presentation, &report);
        if (!same)
            MESSAGE(std::string(prRawStringView(&report).data, prRawStringView(&report).size));
        CHECK(same);
        prRawStringReset(&report);
    }

    // Returns the computed font size of the element with `id`, in px.
    double fontSize(const char* id) {
        return std::strtod((*this)(id, PrCssProperty_FontSize).c_str(), nullptr);
    }
};

} // namespace
} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the user agent's sheet gives MathML elements their display, math-style, math-depth and math-shift") {
    Page page("<math id=m><mfrac id=f><mi id=n>a</mi><mi id=d>b</mi></mfrac><mtable id=t><mtr id=r><mtd id=c>"
        "</mtd></mtr></mtable><msqrt id=s><mi>x</mi></msqrt></math><math id=b display=BLOCK></math>"
        "<div id=h style='display: block math'></div><span id=i style='display: math'></span>");
    CHECK(page("m", PrCssProperty_Display) == "inline math");
    CHECK(page("b", PrCssProperty_Display) == "block math");
    CHECK(page("f", PrCssProperty_Display) == "block math");
    CHECK(page("t", PrCssProperty_Display) == "inline-table");
    CHECK(page("r", PrCssProperty_Display) == "table-row");
    CHECK(page("c", PrCssProperty_Display) == "table-cell");
    CHECK(page("h", PrCssProperty_Display) == "block");
    CHECK(page("i", PrCssProperty_Display) == "inline");
    CHECK(page("m", PrCssProperty_MathStyle) == "compact");
    CHECK(page("b", PrCssProperty_MathStyle) == "normal");
    CHECK(page("n", PrCssProperty_MathDepth) == "1");
    CHECK(page("d", PrCssProperty_MathShift) == "compact");
    CHECK(page("n", PrCssProperty_MathShift) == "normal");
    CHECK(page("s", PrCssProperty_MathShift) == "compact");
    CHECK(page("m", PrCssProperty_WritingMode) == "horizontal-tb");
    CHECK(page("n", PrCssProperty_TextTransform) == "math-auto");
}

TEST_CASE("math-depth computes from its inherited value and math-style") {
    Page page("<math><mrow id=a style='math-depth: 3'><mrow id=b style='math-depth: add(-1)'><mrow id=c "
        "style='math-depth: auto-add'></mrow></mrow><mrow id=d style='math-style: compact'><mrow id=e "
        "style='math-depth: auto-add'></mrow></mrow></mrow><mrow id=f style='math-style: normal'><mrow id=g "
        "style='math-depth: auto-add'></mrow></mrow></math>");
    CHECK(page("a", PrCssProperty_MathDepth) == "3");
    CHECK(page("b", PrCssProperty_MathDepth) == "2");
    CHECK(page("c", PrCssProperty_MathDepth) == "3");
    CHECK(page("e", PrCssProperty_MathDepth) == "4");
    CHECK(page("g", PrCssProperty_MathDepth) == "0");
}

TEST_CASE("a blockified math element is block math, and another element's math display is block") {
    Page page("<math id=f style='float: left'></math><math id=a style='position: absolute'></math><div "
        "style='display: flex'><math id=x></math></div><div style='display: grid'><math id=g style='display: inline "
        "math'></math></div><math id=b display=block style='float: left'></math><div id=d style='display: math; "
        "float: left'></div><span id=s style='display: inline math; position: absolute'></span>");
    CHECK(page("f", PrCssProperty_Display) == "block math");
    CHECK(page("a", PrCssProperty_Display) == "block math");
    CHECK(page("x", PrCssProperty_Display) == "block math");
    CHECK(page("g", PrCssProperty_Display) == "block math");
    CHECK(page("b", PrCssProperty_Display) == "block math");
    CHECK(page("d", PrCssProperty_Display) == "block");
    CHECK(page("s", PrCssProperty_Display) == "block");
}

TEST_CASE("math-depth stays within its range however add() and scriptlevel nest") {
    Page page("<math style='font-size: 100px'><mrow style='math-depth: add(1000000000)'><mrow "
        "style='math-depth: add(1000000000)'><mrow id=up style='math-depth: add(1000000000)'><mrow id=auto "
        "style='math-style: compact'><mrow id=more style='math-depth: auto-add'></mrow></mrow></mrow></mrow></mrow>"
        "<mstyle scriptlevel='-1000000000'><mstyle scriptlevel='-1000000000'><mstyle id=down "
        "scriptlevel='-1000000000'></mstyle></mstyle></mstyle></math>");
    CHECK(page("up", PrCssProperty_MathDepth) == "1000000000");
    CHECK(page("more", PrCssProperty_MathDepth) == "1000000000");
    CHECK(page("down", PrCssProperty_MathDepth) == "-1000000000");
    CHECK(std::isfinite(page.fontSize("up")));
    CHECK(std::isfinite(page.fontSize("down")));
}

TEST_CASE("font-size: math scales by the MATH table's script scales, or by 0.71 a level without one") {
    // Noto Sans Math scales a script to 60% and a script of a script to 50%.
    Page page("<math id=m style='font-size: 100px'><msub><mi>x</mi><mi id=s1>i</mi></msub><msub><mi>x</mi><msub>"
        "<mi>y</mi><mi id=s2>j</mi></msub></msub><mrow id=d5 style='math-depth: 5'></mrow><mrow style='math-depth: 2'>"
        "<mrow id=back style='math-depth: 0'></mrow></mrow></math>");
    CHECK(std::fabs(page.fontSize("s1") - 60) < 0.01);
    CHECK(std::fabs(page.fontSize("s2") - 50) < 0.01);
    CHECK(std::fabs(page.fontSize("d5") - 50 * 0.71 * 0.71 * 0.71) < 0.01);
    CHECK(std::fabs(page.fontSize("back") - 100) < 0.01);
    Page plain("<math style='font-size: 100px'><msub><mi>x</mi><mi id=s1>i</mi></msub><mrow id=d2 "
        "style='math-depth: 2'></mrow></math>", false);
    CHECK(std::fabs(plain.fontSize("s1") - 71) < 0.01);
    CHECK(std::fabs(plain.fontSize("d2") - 50.41) < 0.01);
}

TEST_CASE("MathML's attributes are presentational hints") {
    Page page("<math><mrow id=a dir=RTL mathcolor=red mathbackground='rgb(0, 0, 255)' mathsize='20px' "
        "displaystyle=true scriptlevel='+2'></mrow><mrow id=b dir=up mathcolor='red; color: blue' displaystyle=no "
        "scriptlevel='+-1'></mrow><mrow id=c scriptlevel=3 displaystyle=FALSE mathsize='small'></mrow>"
        "<mi id=n mathvariant=NORMAL>x</mi><mi id=v mathvariant=bold>x</mi><mspace id=s width='2em' height='10px' "
        "depth='5px'/><mspace id=t width='50%' height='10%' depth='3px'/><mpadded id=p width='thinmathspace'>"
        "</mpadded></math>");
    CHECK(page("a", PrCssProperty_Direction) == "rtl");
    CHECK(page("a", PrCssProperty_Color) == "rgb(255, 0, 0)");
    CHECK(page("a", PrCssProperty_BackgroundColor) == "rgb(0, 0, 255)");
    CHECK(page("a", PrCssProperty_FontSize) == "20px");
    CHECK(page("a", PrCssProperty_MathStyle) == "normal");
    CHECK(page("a", PrCssProperty_MathDepth) == "2");
    CHECK(page("b", PrCssProperty_Direction) == "ltr");
    CHECK(page("b", PrCssProperty_Color) == "rgb(0, 0, 0)");
    CHECK(page("b", PrCssProperty_MathStyle) == "compact");
    CHECK(page("b", PrCssProperty_MathDepth) == "0");
    CHECK(page("c", PrCssProperty_MathDepth) == "3");
    CHECK(page("c", PrCssProperty_MathStyle) == "compact");
    CHECK(page("n", PrCssProperty_TextTransform) == "none");
    CHECK(page("v", PrCssProperty_TextTransform) == "-pr-math-bold");
    CHECK(page("s", PrCssProperty_Width) == "32px");
    CHECK(page("s", PrCssProperty_Height) == "15px");
    CHECK(page("t", PrCssProperty_Height) == "3px");
    CHECK(page("t", PrCssProperty_Width) != "50%");
}

TEST_CASE("a table's attributes give its cells their alignment, padding and lines") {
    Page page("<math><mtable id=t frame=solid columnalign='left right' rowspacing='2ex' columnlines='solid' "
        "rowlines='dashed'><mtr><mtd id=a></mtd><mtd id=b columnalign=center></mtd></mtr><mtr rowalign=top>"
        "<mtd id=c columnspan=2></mtd></mtr></mtable></math>");
    CHECK(page("a", PrCssProperty_TextAlign) == "left");
    CHECK(page("b", PrCssProperty_TextAlign) == "center");
    CHECK(page("c", PrCssProperty_TextAlign) == "left");
    CHECK(page("c", PrCssProperty_VerticalAlign) == "top");
    CHECK(page("a", PrCssProperty_BorderRightStyle) == "solid");
    CHECK(page("b", PrCssProperty_BorderRightStyle) == "none");
    CHECK(page("a", PrCssProperty_BorderBottomStyle) == "dashed");
    CHECK(page("c", PrCssProperty_BorderBottomStyle) == "none");
    CHECK(page("t", PrCssProperty_BorderTopStyle) == "solid");
}

TEST_CASE("a change to a MathML attribute restyles what its hints reach") {
    Page page("<math style='font-size: 100px'><mrow id=r><mi id=x>x</mi></mrow><mtable id=t columnalign=left><mtr>"
        "<mtd id=c></mtd></mtr></mtable></math>");
    page.update();
    prDomElementSetAttribute(page.element("r"), "scriptlevel", "2");
    page.update();
    CHECK(page("x", PrCssProperty_MathDepth) == "2");
    CHECK(std::fabs(page.fontSize("x") - 50) < 0.01);
    prDomElementSetAttribute(page.element("t"), "columnalign", "right");
    page.update();
    CHECK(page("c", PrCssProperty_TextAlign) == "right");
}

TEST_CASE("a change to an attribute that an mstyle or a math element gives restyles the elements that read it") {
    Page page("<math id=m><mstyle id=s><mover><mi id=x>x</mi><mo>+</mo></mover><mtable><mtr><mtd id=c></mtd></mtr>"
        "</mtable></mstyle><mover><mi id=y>y</mi><mo id=p>+</mo></mover></math>");
    page.update();
    CHECK(page("x", PrCssProperty_MathShift) == "normal");
    CHECK(page("c", PrCssProperty_TextAlign) == "center");
    prDomElementSetAttribute(page.element("s"), "accent", "true");
    page.update();
    CHECK(page("x", PrCssProperty_MathShift) == "compact");
    prDomElementRemoveAttribute(page.element("s"), "accent");
    page.update();
    CHECK(page("x", PrCssProperty_MathShift) == "normal");
    prDomElementSetAttribute(page.element("s"), "columnalign", "left");
    page.update();
    CHECK(page("c", PrCssProperty_TextAlign) == "left");
    // An mo's accent, which an ancestor gives too, decides whether its mover's base is an accent's.
    CHECK(page("y", PrCssProperty_MathShift) == "normal");
    prDomElementSetAttribute(page.element("m"), "accent", "true");
    page.update();
    CHECK(page("y", PrCssProperty_MathShift) == "compact");
    prDomElementSetAttribute(page.element("s"), "mathvariant", "bold");
    page.update();
    CHECK(page.textTransform("x") == PrTextTransform_MathBold);
}

TEST_CASE("a change to the children of a table, a row or a script element rebuilds the hints that read them") {
    Page page("<math><mtable id=t columnalign='left right' rowlines=solid><mtr id=r1><mtd id=a></mtd></mtr>"
        "<mtr id=r2><mtd id=b></mtd></mtr></mtable><mover id=v><mi id=x>x</mi><mo>&#x2192;</mo></mover>"
        "<munder id=u><mi>y</mi></munder><mrow><mo id=o>&#x2192;</mo></mrow></math>");
    page.update();
    CHECK(page("a", PrCssProperty_TextAlign) == "left");
    CHECK(page("a", PrCssProperty_BorderBottomStyle) == "solid");
    CHECK(page("b", PrCssProperty_BorderBottomStyle) == "none");
    // A cell before #a moves it to the second column.
    PrDomElement* cell = nullptr;
    REQUIRE(prDomElementCreateNs(page.document, "http://www.w3.org/1998/Math/MathML", "mtd", nullptr, &cell) ==
        PrStatus_Ok);
    REQUIRE(prDomNodeInsertBefore(prDomElementAsNode(page.element("r1")), prDomElementAsNode(cell),
        prDomElementAsNode(page.element("a"))) == PrStatus_Ok);
    page.update();
    CHECK(page("a", PrCssProperty_TextAlign) == "right");
    // A row after #r2 makes it no longer the last, so a line follows it.
    PrDomElement* row = nullptr;
    REQUIRE(prDomElementCreateNs(page.document, "http://www.w3.org/1998/Math/MathML", "mtr", nullptr, &row) ==
        PrStatus_Ok);
    REQUIRE(prDomNodeAppendChild(prDomElementAsNode(page.element("t")), prDomElementAsNode(row)) == PrStatus_Ok);
    page.update();
    CHECK(page("b", PrCssProperty_BorderBottomStyle) == "solid");
    // A labeled row makes the table the containing block of its label.
    PrDomElement* labeled = nullptr;
    REQUIRE(prDomElementCreateNs(page.document, "http://www.w3.org/1998/Math/MathML", "mlabeledtr", nullptr,
        &labeled) == PrStatus_Ok);
    REQUIRE(prDomNodeAppendChild(prDomElementAsNode(page.element("t")), prDomElementAsNode(labeled)) == PrStatus_Ok);
    page.update();
    CHECK(page("t", PrCssProperty_Position) == "relative");
    prDomNodeRemove(prDomElementAsNode(labeled));
    page.update();
    CHECK(page("t", PrCssProperty_Position) == "static");
    // A base before the base of an accent makes it the script, which is not an accent.
    CHECK(page("x", PrCssProperty_MathShift) == "compact");
    PrDomElement* base = nullptr;
    REQUIRE(prDomElementCreateNs(page.document, "http://www.w3.org/1998/Math/MathML", "mi", nullptr, &base) ==
        PrStatus_Ok);
    REQUIRE(prDomNodeInsertBefore(prDomElementAsNode(page.element("v")), prDomElementAsNode(base),
        prDomElementAsNode(page.element("x"))) == PrStatus_Ok);
    page.update();
    CHECK(page("x", PrCssProperty_MathShift) == "normal");
    // An accent moved under a base keeps the base's size.
    REQUIRE(prDomNodeAppendChild(prDomElementAsNode(page.element("u")), prDomElementAsNode(page.element("o"))) ==
        PrStatus_Ok);
    page.update();
    CHECK(page("o", PrCssProperty_FontSize) == page("u", PrCssProperty_FontSize));
}

TEST_CASE("a change to the core operator of a script rebuilds the hints of its parts") {
    Page page("<math><mover><mi id=x>x</mi><mrow><mo id=o>&#x2192;</mo></mrow></mover><mover><mi id=y>y</mi>"
        "<mo id=p>+</mo></mover></math>");
    page.update();
    CHECK(page("x", PrCssProperty_MathShift) == "compact");
    CHECK(page("y", PrCssProperty_MathShift) == "normal");
    PrDomNode* text = prDomNodeFirstChild(prDomElementAsNode(page.element("o")));
    REQUIRE(text != nullptr);
    prDomCharacterDataSetData(prDomCharacterDataQuery(prDomNodeAsEventTarget(text)), "+");
    page.update();
    CHECK(page("x", PrCssProperty_MathShift) == "normal");
    prDomElementSetAttribute(page.element("p"), "accent", "true");
    page.update();
    CHECK(page("y", PrCssProperty_MathShift) == "compact");
}

TEST_CASE("a token's mathvariant sets its text in the Mathematical Alphanumeric Symbols") {
    Page page("<math><mi id=a>x</mi><mi id=b mathvariant=fraktur>x</mi><mn id=c mathvariant='double-struck'>1</mn>"
        "<mi id=d mathvariant=normal>x</mi></math>");
    page.update();
    CHECK(page.textTransform("a") == PrTextTransform_MathAuto);
    CHECK(page.textTransform("b") == PrTextTransform_MathFraktur);
    CHECK(page.textTransform("c") == PrTextTransform_MathDoubleStruck);
    CHECK(page.textTransform("d") == PrTextTransform_None);
    prDomElementSetAttribute(page.element("b"), "mathvariant", "bold");
    page.update();
    CHECK(page.textTransform("b") == PrTextTransform_MathBold);
}

TEST_CASE("mathvariant is a presentational hint, which author styles override") {
    Page page("<style>.plain { text-transform: none } .upper { text-transform: uppercase }</style><math>"
        "<mi id=a mathvariant=bold class=plain>x</mi><mi id=b mathvariant=fraktur style='text-transform: uppercase'>x"
        "</mi><mrow class=upper><mi id=c mathvariant=script>x</mi></mrow><mi id=d mathvariant=bold>x</mi></math>"
        "<p id=p style='text-transform: -pr-math-bold'></p>");
    page.update();
    CHECK(page.textTransform("a") == PrTextTransform_None);
    CHECK(page.textTransform("b") == PrTextTransform_Uppercase);
    CHECK(page.textTransform("c") == PrTextTransform_MathScript);
    CHECK(page.textTransform("d") == PrTextTransform_MathBold);
    // Authors may not name the engine's private values.
    CHECK(page("p", PrCssProperty_TextTransform) == "none");
}

TEST_CASE("a script's text carries its math depth, which sets it in the font's script style") {
    Page page("<math><msub><mi id=b>x</mi><mi id=s>i</mi></msub><msub><mi>x</mi><msub><mi>y</mi><mi id=ss>j</mi>"
        "</msub></msub></math>");
    page.update();
    CHECK(page.font("b").mathDepth == 0);
    CHECK(page.font("s").mathDepth == 1);
    CHECK(page.font("ss").mathDepth == 2);
}
