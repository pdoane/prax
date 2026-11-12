// MathML's boxes: the math groups that the presentation projects MathML elements onto, and the layouts they give, in
// Ahem, whose glyphs are squares of one em, so that positions follow from the markup alone.

#include "pr/css_presentation.h"
#include "pr/float.h"
#include "pr/font.h"
#include "pr/font_set.h"
#include "pr/html.h"
#include "pr/html_page.h"
#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/text_font.h"
#include "pr/text_harfbuzz.h"

#include "tools/harness/test.h"

namespace pr_impl {
namespace {

// A document presented over Ahem, with New Computer Modern Math as the math family.
struct MathPage {
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextHarfBuzzCreate();
    PrTextFont* defaultFont = nullptr;
    PrDomAgent* agent = nullptr;
    PrDomDocument* document = nullptr;
    PrCssPresentation* presentation = nullptr;

    explicit MathPage(const char* html) {
        const PrStringView paths[] = {PR_FONTS_DIR "/Ahem.ttf", PR_FONTS_DIR "/NewCMMath-Regular.otf"};
        for (PrStringView path : paths) {
            const PrFont* font = nullptr;
            REQUIRE(prFontCreateFromFile(path, 0, nullptr, &font) == PrStatus_Ok);
            PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
            prFontSetAdd(fonts, &descriptor, font);
            prFontRelease(font);
        }
        prFontSetSetGeneric(fonts, PrFontGeneric_Math, "NewComputerModernMath");
        prFontSetSetDefaultFamily(fonts, prFontFamilyFromName("Ahem"));
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
        update();
    }

    ~MathPage() {
        prCssPresentationDestroy(presentation);
        prDomDocumentRelease(document);
        prDomAgentDestroy(agent);
        prTextFontRelease(defaultFont);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
    }

    // Updates the presentation and lays it out, and checks both against a recomputation.
    void update() {
        prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
        PrRawString report{};
        bool same = prCssPresentationVerify(presentation, &report);
        if (!same)
            MESSAGE(report);
        CHECK(same);
        prRawStringReset(&report);
    }

    PrDomElement* element(const char* id) {
        PrDomElement* found = prDomNodeGetElementById(prDomDocumentAsNode(document), id);
        REQUIRE(found != nullptr);
        return found;
    }

    // Returns the border box of the element with `id`, in px.
    PrRect bounds(const char* id) {
        PrLayoutTree* tree = prCssPresentationTree(presentation);
        PrLayoutNode node = prCssPresentationLayoutNode(presentation, prDomElementAsNode(element(id)));
        REQUIRE(node != kPrLayoutNodeNone);
        return prLayoutRectToPx(prLayoutNodeBounds(tree, node, PrLayoutCoordinates_Viewport));
    }
};

} // namespace
} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an embellished mrow takes its operator's spaces around it, and none inside it") {
    MathPage page("<style>math, math * { font: 25px/1 Ahem; }</style><p><math><mn id=a>X</mn><mrow id=row>"
        "<mtext id=t>X</mtext><mo lspace=1em rspace=0em>X</mo></mrow><mn id=b>X</mn></math>");
    CHECK(page.bounds("t").left - page.bounds("a").right == doctest::Approx(25));
    CHECK(page.bounds("b").left - page.bounds("t").right == doctest::Approx(25));
}

TEST_CASE("a change to the font of an embellished operator's core respaces the operator around it") {
    MathPage page("<style>math, math * { font: 25px/1 Ahem; } .big #o { font-size: 50px; }</style><p id=p><math><mn id=a>X</mn>"
        "<munder><mo id=o lspace=1em rspace=0em>X</mo><mn>X</mn></munder><mn>X</mn></math>");
    CHECK(page.bounds("o").left - page.bounds("a").right == doctest::Approx(25));
    prDomElementSetAttribute(page.element("p"), "class", "big");
    page.update();
    CHECK(page.bounds("o").left - page.bounds("a").right == doctest::Approx(50));
}

TEST_CASE("white space and comments between children leave an embellished mrow embellished") {
    MathPage page("<style>math, math * { font: 25px/1 Ahem; }</style><p>\n<math>\n  <mn id=a>X</mn>\n  <mrow>\n"
        "    <!-- mtext is space-like. -->\n    <mtext id=t>X</mtext>\n    <mo lspace=\"1em\" rspace=\"0em\">X</mo>\n"
        "  </mrow>\n  <mn id=b>X</mn>\n</math>");
    CHECK(page.bounds("t").left - page.bounds("a").right == doctest::Approx(25));
    CHECK(page.bounds("b").left - page.bounds("t").right == doctest::Approx(25));
}

TEST_CASE("an integral in display style takes a larger size than in text style") {
    MathPage page("<p><math display=block><msubsup><mo id=d lspace=0em>&#x222B;</mo><mn>0</mn><mn>1</mn></msubsup>"
        "</math><math><msubsup><mo id=t lspace=0em>&#x222B;</mo><mn>0</mn><mn>1</mn></msubsup></math>"
        "<math><mtable displaystyle=true><mtr><mtd><msubsup><mo id=c lspace=0em>&#x222B;</mo><mn>0</mn><mn>1</mn>"
        "</msubsup></mtd></mtr></mtable></math>");
    PrRect d = page.bounds("d");
    PrRect t = page.bounds("t");
    PrRect c = page.bounds("c");
    CHECK(d.bottom - d.top > t.bottom - t.top);
    CHECK(c.bottom - c.top == doctest::Approx(d.bottom - d.top));
}

TEST_CASE("a stretchy accent covers its base") {
    MathPage page("<p><math><mover accent=true><mrow id=b><mi>x</mi><mi>y</mi><mi>z</mi></mrow><mo id=h>^</mo>"
        "</mover></math>");
    PrRect b = page.bounds("b");
    PrRect h = page.bounds("h");
    CHECK(h.right - h.left >= (b.right - b.left) * 0.9f);
}

TEST_CASE("an mpadded of no width with a negative lspace laps its content to the left") {
    MathPage page("<p><math><mi id=a>a</mi><mpadded width=0 lspace=-1width><mi id=b>b</mi></mpadded>"
        "<mi id=c>c</mi></math>");
    PrRect a = page.bounds("a");
    PrRect b = page.bounds("b");
    PrRect c = page.bounds("c");
    CHECK(c.left == doctest::Approx(a.right));
    CHECK(prF32Abs(b.right - a.right) < 0.5f);
}

TEST_CASE("a table of full width in display math fills the line, and its labels go at its right edge") {
    MathPage page("<body style='margin:0'><math display=block><mtable id=t width='100%'><mlabeledtr><mtd id=l>"
        "<mtext>(1)</mtext></mtd><mtd><mi>a</mi></mtd></mlabeledtr></mtable></math>");
    PrRect t = page.bounds("t");
    PrRect l = page.bounds("l");
    CHECK(t.right - t.left == doctest::Approx(800));
    CHECK(l.right == doctest::Approx(t.right));
}

TEST_CASE("fit columns share the width that a table's other columns leave, and center them between") {
    MathPage page("<style>math, math * { font: 20px/1 Ahem; }</style><body style='margin:0'><math display=block>"
        "<mtable width='100%' columnwidth='fit auto auto fit' columnspacing='0'><mtr><mtd id=l></mtd>"
        "<mtd id=a><mn>XX</mn></mtd>\n<mtd id=b><mn>X</mn></mtd><mtd id=r></mtd></mtr></mtable></math>");
    PrRect l = page.bounds("l");
    PrRect a = page.bounds("a");
    PrRect b = page.bounds("b");
    PrRect r = page.bounds("r");
    CHECK(b.left - a.left == doctest::Approx(40));
    CHECK(r.left - b.left == doctest::Approx(20));
    CHECK(l.right - l.left == doctest::Approx(r.right - r.left));
    CHECK(r.right == doctest::Approx(800));
}

TEST_CASE("a child that leaves or enters its parent's flow by its style changes its siblings' operator forms") {
    MathPage page("<style>math, math * { font: 25px/1 Ahem; } .gone { display: none; } .out { position: absolute; }"
        "</style><p><math id=m><mi id=h>X</mi><mo id=o>&#x2212;</mo><mi>X</mi></math>");
    // A minus after an element is infix, with medium spaces, and first in its row a prefix with none.
    CHECK(page.bounds("o").left - page.bounds("h").right == doctest::Approx(25 * 4 / 18.0f).epsilon(0.02));
    prDomElementSetAttribute(page.element("h"), "class", "gone");
    page.update();
    CHECK(page.bounds("o").left == doctest::Approx(page.bounds("m").left));
    prDomElementSetAttribute(page.element("h"), "class", "");
    page.update();
    CHECK(page.bounds("o").left - page.bounds("h").right == doctest::Approx(25 * 4 / 18.0f).epsilon(0.02));
    prDomElementSetAttribute(page.element("h"), "class", "out");
    page.update();
    CHECK(page.bounds("o").left == doctest::Approx(page.bounds("m").left));
}

TEST_CASE("a child inserted in a table cell changes the forms of the operators beside it") {
    MathPage page("<style>math, math * { font: 25px/1 Ahem; }</style><p><math><mtable><mtr><mtd id=c><mo id=o>&#x2212;"
        "</mo><mi>X</mi></mtd></mtr></mtable></math><math><mi id=x>X</mi></math>");
    // A minus first in its row is a prefix with no spaces, and after an element infix, with medium spaces.
    float prefix = page.bounds("o").left;
    prDomNodeInsertBefore(prDomElementAsNode(page.element("c")), prDomElementAsNode(page.element("x")),
        prDomElementAsNode(page.element("o")));
    page.update();
    CHECK(page.bounds("x").left == doctest::Approx(prefix));
    CHECK(page.bounds("o").left - page.bounds("x").right == doctest::Approx(25 * 4 / 18.0f).epsilon(0.02));
}

TEST_CASE("a change to a table's or a row's attributes restyles its cells, and an accent attribute its parts") {
    MathPage page("<style>math, math * { font: 20px/1 Ahem; }</style><p><math><mtable id=t><mtr id=r><mtd>"
        "<mi id=a>X</mi></mtd><mtd><mi id=b>X</mi></mtd></mtr><mtr><mtd><mi>XXX</mi></mtd><mtd><mi>X</mi></mtd></mtr>"
        "</mtable><mover id=o><mi>X</mi><mo>~</mo></mover></math>");
    auto gap = [&] { return page.bounds("b").left - page.bounds("a").right; };
    prDomElementSetAttribute(page.element("t"), "columnspacing", "10px");
    page.update();
    float narrow = gap();
    prDomElementSetAttribute(page.element("t"), "columnspacing", "30px");
    page.update();
    CHECK(gap() == doctest::Approx(narrow + 20).epsilon(0.01));
    // The first column is three glyphs wide, and its cell's glyph moves from its center to its left.
    float centered = page.bounds("a").left;
    prDomElementSetAttribute(page.element("r"), "columnalign", "left");
    page.update();
    CHECK(page.bounds("a").left == doctest::Approx(centered - 20).epsilon(0.01));
    prDomElementSetAttribute(page.element("t"), "columnalign", "right");
    prDomElementRemoveAttribute(page.element("r"), "columnalign");
    page.update();
    CHECK(page.bounds("a").left == doctest::Approx(centered + 20).epsilon(0.01));
    prDomElementSetAttribute(page.element("o"), "accent", "false");
    page.update();
    prDomElementRemoveAttribute(page.element("o"), "accent");
    page.update();
}

TEST_CASE("a change to the text of an accent's operator changes whether its base takes its dotless form") {
    MathPage page("<p><math><mover><mi id=i>i</mi><mo id=o>^</mo></mover></math>");
    PrDomNode* text = prDomNodeFirstChild(prDomElementAsNode(page.element("o")));
    REQUIRE(text != nullptr);
    prDomCharacterDataSetData(prDomCharacterDataQuery(prDomNodeAsEventTarget(text)), "x");
    page.update();
    prDomCharacterDataSetData(prDomCharacterDataQuery(prDomNodeAsEventTarget(text)), "^");
    page.update();
}

TEST_CASE("an operator in a script drops the dictionary's medium and thick spaces, as TeX's script styles do") {
    MathPage page("<style>math, math * { font: 25px/1 Ahem; }</style><p><math><mi id=a>X</mi><mo id=p>+</mo>"
        "<msup><mi>X</mi><mrow><mi id=b>X</mi><mo id=q>+</mo><mi>X</mi></mrow></msup></math>");
    CHECK(page.bounds("p").left - page.bounds("a").right == doctest::Approx(25 * 4 / 18.0f).epsilon(0.02));
    CHECK(page.bounds("q").left == doctest::Approx(page.bounds("b").right));
}

TEST_CASE("a display formula that scrolls along the inline axis has nothing to scroll along the block axis") {
    MathPage page("<style>math { font-size: 20px; overflow-x: auto; }</style><body style='margin:0'>"
        "<math id=m display=block><munderover><mo>&#x2211;</mo><mrow><mi>n</mi><mo>=</mo><mn>1</mn></mrow>"
        "<mi>&#x221E;</mi></munderover><mfrac><mn>1</mn><msup><mi>n</mi><mn>2</mn></msup></mfrac><mo>=</mo><mfrac>"
        "<msup><mi>&#x3C0;</mi><mn>2</mn></msup><mn>6</mn></mfrac><msqrt><mi>x</mi></msqrt><mover><mi>a</mi>"
        "<mo>^</mo></mover><mover><mi>abc</mi><mo>&#x23DE;</mo></mover><mrow><mo>(</mo><mtable><mtr><mtd><mn>1</mn>"
        "</mtd></mtr><mtr><mtd><mn>2</mn></mtd></mtr></mtable><mo>)</mo></mrow></math>");
    PrLayoutTree* tree = prCssPresentationTree(page.presentation);
    PrLayoutNode node = prCssPresentationLayoutNode(page.presentation, prDomElementAsNode(page.element("m")));
    PrLayoutScrollInfo info{};
    REQUIRE(prLayoutNodeScrollInfo(tree, node, &info));
    CHECK(info.scrollContainer);
    CHECK(info.range.top == info.range.bottom);
    CHECK(info.range.left == info.range.right);
}

TEST_CASE("a fence in a font without a MATH table stretches by a fallback face's parts") {
    // Ahem has no MATH table and none of the parts of a parenthesis, which New Computer Modern Math, next in the list,
    // has.
    MathPage page("<style>math { font: 20px Ahem, NewComputerModernMath; }</style><p><math><mrow><mo id=p>(</mo><mspace id=s width='1px' "
        "height='60px' depth='20px'/><mo>)</mo></mrow></math>");
    PrRect p = page.bounds("p");
    PrRect s = page.bounds("s");
    CHECK(p.bottom - p.top >= 0.9f * (s.bottom - s.top));
}

TEST_CASE("an absent accent is the dictionary's accent property of the script's operator, and a present one stands") {
    // The circumflex is an accent in MathML 3's dictionary, which MathML 4 takes as the default of mover's accent. As
    // an accent, it keeps its base's size and sits as an explicit accent does.
    MathPage page("<style>math { font-size: 100px; }</style><p><math><mover><mi>a</mi><mo id=implied>^</mo></mover>"
        "<mover accent=true><mi>a</mi><mo id=explicit>^</mo></mover><mover accent=false><mi>a</mi><mo id=off>^</mo>"
        "</mover><mover><mi>a</mi><mrow><mo id=core>^</mo></mrow></mover></math>");
    PrRect implied = page.bounds("implied");
    PrRect explicitAccent = page.bounds("explicit");
    CHECK(implied.bottom - implied.top == doctest::Approx(explicitAccent.bottom - explicitAccent.top));
    CHECK(implied.top == doctest::Approx(explicitAccent.top));
    // Without the accent, the circumflex is a script, a size smaller.
    PrRect off = page.bounds("off");
    CHECK(off.bottom - off.top < 0.9f * (explicitAccent.bottom - explicitAccent.top));
    // An embellished operator takes its core operator's accent, and keeps its base's size.
    PrRect core = page.bounds("core");
    CHECK(core.bottom - core.top > 0.95f * (explicitAccent.bottom - explicitAccent.top));
}

TEST_CASE("a brace stretched over its base clears it by a stretch stack's gap, and an overline by an overbar's") {
    MathPage page("<style>math { font-size: 100px; }</style><p><math><mover><mi id=a>a</mi><mo id=brace>&#x23DE;</mo>"
        "</mover><mover accent=false><mi id=b>a</mi><mo id=bar>&#x203E;</mo></mover></math>");
    // New Computer Modern Math's StretchStackGapBelowMin is 0.167em, and its OverbarVerticalGap 0.12em.
    CHECK(page.bounds("a").top - page.bounds("brace").bottom == doctest::Approx(16.7).epsilon(0.03));
    CHECK(page.bounds("b").top - page.bounds("bar").bottom == doctest::Approx(12).epsilon(0.03));
}

TEST_CASE("a cell of vertical-align middle centers its math in a row that a taller cell makes") {
    MathPage page("<style>math, math * { font: 20px/1 Ahem; } mtd { padding: 0; vertical-align: middle; }</style>"
        "<body style='margin:0'><math display=block><mtable><mtr><mtd id=tall><mspace height='100px' width='10px'/>"
        "</mtd><mtd id=c><mtext id=x>X</mtext></mtd></mtr></mtable></math>");
    PrRect tall = page.bounds("tall");
    PrRect x = page.bounds("x");
    CHECK((x.top + x.bottom) / 2 == doctest::Approx((tall.top + tall.bottom) / 2));
}

TEST_CASE("a row's label sits on the row's baseline, beside a tall cell as beside a short one") {
    MathPage page("<style>math, math * { font: 20px/1 Ahem; }</style><body style='margin:0'><math display=block>"
        "<mtable width='100%'><mlabeledtr><mtd><mtext id=l>X</mtext></mtd><mtd><mtext id=x>X</mtext><mspace "
        "height='60px' depth='20px' width='10px'/></mtd></mlabeledtr></mtable></math>");
    CHECK(page.bounds("l").bottom == doctest::Approx(page.bounds("x").bottom));
    CHECK(page.bounds("l").left > page.bounds("x").right);
}

TEST_CASE("a change to an operator's attribute lays it out again") {
    MathPage page("<style>math, math * { font: 25px/1 Ahem; }</style><p><math><mn id=a>X</mn>"
        "<mo id=o lspace=0em rspace=0em>X</mo></math>");
    CHECK(page.bounds("o").left - page.bounds("a").right == doctest::Approx(0));
    prDomElementSetAttribute(page.element("o"), "lspace", "1em");
    page.update();
    CHECK(page.bounds("o").left - page.bounds("a").right == doctest::Approx(25));
}

TEST_CASE("an mfenced lays out as its expanded form, its fences stretched over its arguments") {
    MathPage page("<p><math><mfenced id=f><mi>x</mi><mi>y</mi></mfenced><mrow id=r><mo>(</mo><mrow><mi>x</mi>"
        "<mo>,</mo><mi>y</mi></mrow><mo>)</mo></mrow><mfenced id=tf><mfrac><mn>1</mn><mn>2</mn></mfrac></mfenced>"
        "<mrow id=tr><mo>(</mo><mfrac><mn>1</mn><mn>2</mn></mfrac><mo>)</mo></mrow>"
        "<mfenced id=s open='[' close='}' separators=';|'><mi>a</mi><mi>b</mi><mi>c</mi><mi>d</mi></mfenced>"
        "<mrow id=sr><mo>[</mo><mrow><mi>a</mi><mo>;</mo><mi>b</mi><mo>|</mo><mi>c</mi><mo>|</mo><mi>d</mi></mrow>"
        "<mo>}</mo></mrow><mfenced id=n separators=''><mi>a</mi><mi>b</mi></mfenced><mrow id=nr><mo>(</mo>"
        "<mrow><mi>a</mi><mi>b</mi></mrow><mo>)</mo></mrow></math>");
    struct Pair {
        const char* fenced;
        const char* expanded;
    };
    const Pair pairs[] = {{"f", "r"}, {"tf", "tr"}, {"s", "sr"}, {"n", "nr"}};
    for (const Pair& pair : pairs) {
        CAPTURE(pair.fenced);
        PrRect fenced = page.bounds(pair.fenced);
        PrRect expanded = page.bounds(pair.expanded);
        CHECK(fenced.right - fenced.left == doctest::Approx(expanded.right - expanded.left).epsilon(0.001));
        CHECK(fenced.top == doctest::Approx(expanded.top).epsilon(0.001));
        CHECK(fenced.bottom == doctest::Approx(expanded.bottom).epsilon(0.001));
    }
    // The parentheses around the fraction are taller than those around x and y.
    PrRect f = page.bounds("f");
    PrRect tf = page.bounds("tf");
    CHECK(tf.bottom - tf.top > f.bottom - f.top + 2);
}

TEST_CASE("an mstack aligns the digits of its rows in columns, its rows' children passed through to it") {
    MathPage page("<p><math><mstack><mn id=a>424</mn><msrow><mo id=p>+</mo><mn id=b>33</mn></msrow><msline id=l />"
        "<msgroup><msrow position=1><mn id=c>45</mn></msrow></msgroup></mstack>"
        "<mlongdiv><mn id=d>3</mn><mn id=r>41</mn><mn id=n>123</mn></mlongdiv></math>");
    PrRect a = page.bounds("a");
    PrRect b = page.bounds("b");
    PrRect p = page.bounds("p");
    PrRect l = page.bounds("l");
    PrRect c = page.bounds("c");
    float digit = (a.right - a.left) / 3;
    CHECK(b.right == doctest::Approx(a.right).epsilon(0.001));
    // The + takes the hundreds column, over which it is wider than the 4.
    CHECK(p.right <= a.left + digit + 0.5f);
    CHECK(p.left < a.left);
    CHECK(l.top > b.bottom);
    CHECK(c.top > l.top);
    // position=1 moves 45 a column to the left.
    CHECK(c.right == doctest::Approx(a.right - digit).epsilon(0.01));
    // lefttop: the divisor at the left, the result over the dividend's last columns.
    PrRect d = page.bounds("d");
    PrRect r = page.bounds("r");
    PrRect n = page.bounds("n");
    CHECK(d.right < n.left);
    CHECK(r.bottom < n.top);
    CHECK(r.right == doctest::Approx(n.right).epsilon(0.001));
}

TEST_CASE("a change to an msgroup's position or an mstyle's attribute lays out the elements in it again") {
    MathPage page("<p><math><mstack><mn id=a>424</mn><msgroup id=g><msrow><mn id=c>45</mn></msrow></msgroup></mstack>"
        "<mstyle id=s linethickness=1px><mfrac id=f><mn>1</mn><mn>2</mn></mfrac></mstyle></math>");
    PrRect a = page.bounds("a");
    float digit = (a.right - a.left) / 3;
    CHECK(page.bounds("c").right == doctest::Approx(a.right).epsilon(0.001));
    prDomElementSetAttribute(page.element("g"), "position", "1");
    page.update();
    CHECK(page.bounds("c").right == doctest::Approx(a.right - digit).epsilon(0.01));
    float thin = page.bounds("f").bottom - page.bounds("f").top;
    prDomElementSetAttribute(page.element("s"), "linethickness", "20px");
    page.update();
    CHECK(page.bounds("f").bottom - page.bounds("f").top > thin + 10);
}

TEST_CASE("a change to a math element's attribute lays out the elements below it that inherit it") {
    MathPage page("<p><math id=m linethickness=1px><mrow><mrow><mfrac id=f><mn>1</mn><mn>2</mn></mfrac></mrow></mrow>"
        "</math>");
    float thin = page.bounds("f").bottom - page.bounds("f").top;
    prDomElementSetAttribute(page.element("m"), "linethickness", "20px");
    page.update();
    CHECK(page.bounds("f").bottom - page.bounds("f").top > thin + 10);
}

TEST_CASE("an mstack in a right-to-left formula keeps its columns left to right") {
    MathPage page("<p><math dir=rtl><mstack><mn id=a>424</mn><msrow><mo id=p>+</mo><mn id=b>33</mn></msrow></mstack>"
        "</math>");
    PrRect a = page.bounds("a");
    PrRect b = page.bounds("b");
    PrRect p = page.bounds("p");
    // The numbers align at their right edges, the units column, and the operator stands at the left.
    CHECK(b.right == doctest::Approx(a.right).epsilon(0.001));
    CHECK(p.left < a.left);
}

TEST_CASE("an mglyph in a token is set on its line at its size, its bottom valign above the baseline") {
    MathPage page("<style>math, math * { font: 25px/1 Ahem; }</style><p><math><mi id=x>X</mi><mi>"
        "<mglyph id=g alt=flag width=40px height=20px valign=-10px /></mi><mi><mglyph id=h alt=flag height=20px />"
        "</mi></math>");
    PrRect x = page.bounds("x");
    PrRect g = page.bounds("g");
    PrRect h = page.bounds("h");
    CHECK(g.right - g.left == doctest::Approx(40));
    CHECK(g.bottom - g.top == doctest::Approx(20));
    // The mi's X is italic, which New Computer Modern Math sets on the baseline.
    CHECK(g.bottom - x.bottom == doctest::Approx(10));
    CHECK(h.bottom - x.bottom == doctest::Approx(0));
    CHECK(h.right - h.left == doctest::Approx(0));
}
