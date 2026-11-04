// MathML's element model, attributes and operator dictionary.

#include "pr/css.h"
#include "pr/html.h"
#include "pr/mathml.h"

#include "mathml/src/mathml_attribute.h"
#include "mathml/src/mathml_element.h"
#include "mathml/src/mathml_operator.h"

#include "tools/harness/test.h"

#include <cmath>

extern "C" int prTestMathMlCApi(void);

namespace pr_impl {
namespace {

// An HTML document parsed from markup, released at the end of the scope.
struct Document {
    PrDomDocument* document;

    explicit Document(PrStringView html) {
        document = prHtmlDomParserParseFromString(html, PrHtmlDomParserSupportedType_TextHtml);
    }
    ~Document() {
        prDomDocumentRelease(document);
    }

    // The element that `selector` selects.
    const PrDomElement* operator[](const char* selector) const {
        PrDomElement* element = nullptr;
        REQUIRE(prCssNodeQuerySelector(prDomDocumentAsNode(document), selector, &element) == PrStatus_Ok);
        REQUIRE(element != nullptr);
        return element;
    }
};

// The children that display: none hides, as a presentation would classify them, from their style attribute.
MathMlChild classifyByStyle(const void*, const PrDomElement* child) {
    PrStringView style = domAttributeValue(child, "style");
    if (style == "display: none")
        return MathMlChild_OutOfFlow;
    return style == "display: contents" ? MathMlChild_Contents : MathMlChild_InFlow;
}

constexpr MathMlFlow kAll = {};
constexpr MathMlFlow kStyled = {.classify = classifyByStyle};

// Returns the category of the operator whose content is `content` in `form`.
MathMlOperatorCategory category(PrStringView content, MathMlForm form = MathMlForm_Infix) {
    return mathMlOperatorCategory(content, form);
}

// Returns the length that `text` parses to in `syntax`.
MathMlLength length(PrStringView text, MathMlLengthSyntax syntax = MathMlLengthSyntax_Normal) {
    MathMlLength out{};
    mathMlParseLength(text, syntax, &out);
    return out;
}

// Returns true if `length` is the single term `value` of `unit`.
bool isTerm(const MathMlLength& length, float value, MathMlUnit unit) {
    return length.count == 1 && length.units[0] == unit && std::fabs(length.values[0] - value) < 1e-4f;
}

} // namespace
} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestMathMlCApi() == 0);
}

TEST_CASE("the dictionary gives each category its entries") {
    CHECK(category("→") == MathMlOperatorCategory_A);                     // rightwards arrow
    CHECK(category("+") == MathMlOperatorCategory_B);
    CHECK(category("×") == MathMlOperatorCategory_C);                     // multiplication sign
    CHECK(category("-", MathMlForm_Prefix) == MathMlOperatorCategory_D);
    CHECK(category("!", MathMlForm_Postfix) == MathMlOperatorCategory_E);
    CHECK(category("(", MathMlForm_Prefix) == MathMlOperatorCategory_F);
    CHECK(category(")", MathMlForm_Postfix) == MathMlOperatorCategory_G);
    CHECK(category("∫", MathMlForm_Prefix) == MathMlOperatorCategory_H);  // integral
    CHECK(category("^", MathMlForm_Postfix) == MathMlOperatorCategory_I);
    CHECK(category("∑", MathMlForm_Prefix) == MathMlOperatorCategory_J);  // summation
    CHECK(category("/") == MathMlOperatorCategory_K);
    CHECK(category("∂", MathMlForm_Prefix) == MathMlOperatorCategory_L);  // partial differential
    CHECK(category(",") == MathMlOperatorCategory_M);
    CHECK(category("x") == MathMlOperatorCategory_Default);
    CHECK(category("(") == MathMlOperatorCategory_Default);
    MathMlCategoryValues j = mathMlCategoryValues(MathMlOperatorCategory_J);
    CHECK(j.lspace == 3);
    CHECK(j.rspace == 3);
    CHECK(j.flags == (MathMlOperatorFlag_Symmetric | MathMlOperatorFlag_LargeOp | MathMlOperatorFlag_MovableLimits));
    CHECK(mathMlCategoryValues(MathMlOperatorCategory_L).rspace == 0);
    CHECK(mathMlCategoryValues(MathMlOperatorCategory_Default).lspace == 5);
}

TEST_CASE("the dictionary's special cases of content and form") {
    // Content of more than two UTF-16 code units is not looked up, and a single character in U+0320-U+03FF is not.
    CHECK(category("sin") == MathMlOperatorCategory_Default);
    CHECK(category("̢") == MathMlOperatorCategory_Default);
    // Pairs of ASCII characters stand for the code points from U+0320.
    CHECK(category("+=") == MathMlOperatorCategory_Default);
    CHECK(category("&&") == MathMlOperatorCategory_B);
    CHECK(category("**") == MathMlOperatorCategory_C);
    CHECK(category("||", MathMlForm_Prefix) == MathMlOperatorCategory_D);
    CHECK(category("++", MathMlForm_Postfix) == MathMlOperatorCategory_E);
    CHECK(category("ab") == MathMlOperatorCategory_Default);
    // A combining solidus or vertical line overlay negates the operator before it, which keeps its category.
    CHECK(category("≠") == category("="));
    CHECK(category("→⃒") == MathMlOperatorCategory_A);
    // The vertical line and the tilde operator are Default when infix, so that no other form is tried.
    CHECK(category("|") == MathMlOperatorCategory_ForceDefault);
    CHECK(category("∼") == MathMlOperatorCategory_ForceDefault);
    CHECK(category("|", MathMlForm_Prefix) == MathMlOperatorCategory_F);
    // Two Arabic operators of the SMP are postfix and stretch.
    CHECK(category("\U0001EEF0", MathMlForm_Postfix) == MathMlOperatorCategory_I);
    CHECK(category("\U0001EEF1", MathMlForm_Infix) == MathMlOperatorCategory_Default);
    CHECK(category("\U0001D465") == MathMlOperatorCategory_Default);
}

TEST_CASE("the dictionary's fences, separators, stretch axes and accents") {
    CHECK(mathMlDictionaryIsFence('('));
    CHECK(mathMlDictionaryIsFence(0x27E8));
    CHECK_FALSE(mathMlDictionaryIsFence('+'));
    CHECK(mathMlDictionaryIsSeparator(','));
    CHECK(mathMlDictionaryIsSeparator(0x2063));
    CHECK_FALSE(mathMlDictionaryIsSeparator('.'));
    CHECK(mathMlIsInlineAxis(0x2192));
    CHECK(mathMlIsInlineAxis(0x1EEF0));
    CHECK_FALSE(mathMlIsInlineAxis('('));
    CHECK(kMathMlCombiningAccentCount > 30);
    CHECK(kMathMlCombiningAccents[0].spacing == '+');
}

TEST_CASE("an element's in-flow children skip what layout leaves out and pass display: contents through") {
    Document d("<math><mrow id=r><mi>a</mi><mi style='display: none'>b</mi><mrow style='display: contents'><mi>c</mi>"
        "<mi>d</mi></mrow><mi>e</mi></mrow></math>");
    const PrDomElement* row = d["#r"];
    CHECK(mathMlInFlowCount(row, kAll) == 4);
    CHECK(mathMlInFlowCount(row, kStyled) == 4);
    const PrDomElement* second = mathMlInFlowAt(row, 1, kStyled);
    REQUIRE(second != nullptr);
    CHECK(mathMlTokenCodePoint(second) == 'c');
    CHECK(mathMlInFlowAt(row, 4, kStyled) == nullptr);
}

TEST_CASE("an element whose in-flow children make it invalid lays out as an mrow") {
    Document d("<math><mfrac id=f1><mi>a</mi><mi>b</mi></mfrac><mfrac id=f2><mi>a</mi></mfrac>"
        "<mfrac id=f3><mi>a</mi><mi>b</mi><mi style='display: none'>c</mi></mfrac>"
        "<msubsup id=s><mi>a</mi><mi>b</mi><mi>c</mi></msubsup><mroot id=r><mi>a</mi></mroot>"
        "<mmultiscripts id=m1><mi>a</mi><mi>b</mi><mi>c</mi><mprescripts/><mi>d</mi><mi>e</mi></mmultiscripts>"
        "<mmultiscripts id=m2><mi>a</mi><mi>b</mi></mmultiscripts>"
        "<mmultiscripts id=m3><mprescripts/></mmultiscripts><mspace id='sp'/><foo id=u></foo><mtd id=td></mtd></math>");
    CHECK(mathMlLayout(d["#f1"], kAll) == MathMlLayout_Fraction);
    CHECK(mathMlLayout(d["#f2"], kAll) == MathMlLayout_Row);
    CHECK(mathMlLayout(d["#f3"], kAll) == MathMlLayout_Row);
    CHECK(mathMlLayout(d["#f3"], kStyled) == MathMlLayout_Fraction);
    CHECK(mathMlLayout(d["#s"], kAll) == MathMlLayout_SubSuperscript);
    CHECK(mathMlLayout(d["#r"], kAll) == MathMlLayout_Row);
    CHECK(mathMlLayout(d["#m1"], kAll) == MathMlLayout_Multiscripts);
    MathMlMultiscripts parts{};
    REQUIRE(mathMlMultiscripts(d["#m1"], kAll, &parts));
    CHECK(parts.postscriptPairs == 1);
    CHECK(parts.prescriptPairs == 1);
    CHECK(mathMlLayout(d["#m2"], kAll) == MathMlLayout_Row);
    CHECK(mathMlLayout(d["#m3"], kAll) == MathMlLayout_Row);
    CHECK(mathMlLayout(d["#sp"], kAll) == MathMlLayout_Space);
    CHECK(mathMlIsGroupingElement(d["#u"]));
    CHECK_FALSE(mathMlIsKnownElement(d["#u"]));
    CHECK(mathMlLayout(d["#td"], kAll) == MathMlLayout_Table);
    CHECK(mathMlWrapsInRow(d["#td"]));
}

TEST_CASE("embellished operators and their core operators") {
    Document d("<math><msub id=a><mo id=o1>∑</mo><mi>i</mi></msub><mrow id=b><mspace/><mo id=o2>+</mo>"
        "<mtext> </mtext></mrow><mrow id=c><mo>+</mo><mi>x</mi></mrow><mfrac id=e><mo id=o3>-</mo><mn>1</mn></mfrac>"
        "<mphantom id=f><mo id=o4>=</mo></mphantom><mtable id=g><mtr><mtd><mo>=</mo></mtd></mtr></mtable>"
        "<mrow id=s><mspace/><mpadded><mtext/></mpadded></mrow></math>");
    CHECK(mathMlCoreOperator(d["#a"], kAll) == d["#o1"]);
    CHECK(mathMlCoreOperator(d["#b"], kAll) == d["#o2"]);
    CHECK(mathMlCoreOperator(d["#c"], kAll) == nullptr);
    CHECK(mathMlCoreOperator(d["#e"], kAll) == d["#o3"]);
    CHECK(mathMlCoreOperator(d["#f"], kAll) == d["#o4"]);
    CHECK(mathMlCoreOperator(d["#g"], kAll) == nullptr);
    CHECK(mathMlIsSpaceLike(d["#s"], kAll));
    CHECK_FALSE(mathMlIsSpaceLike(d["#b"], kAll));
}

TEST_CASE("an operator's form follows its position among its parent's in-flow children") {
    Document d("<math><mrow><mo id=p>(</mo><mi>x</mi><mo id=i>+</mo><mi>y</mi><mo id=q>)</mo></mrow>"
        "<mrow><mspace/><mo id=sp>-</mo><mi>x</mi><mspace/></mrow><mrow><mo id=alone>-</mo></mrow>"
        "<msup><mi>x</mi><mo id=s>′</mo></msup><msqrt><mo id=r>-</mo><mn>2</mn></msqrt>"
        "<mrow><mo id=f form='postfix'>(</mo><mi>x</mi></mrow><mrow><mi>x</mi><mo id=h style='display: none'>!</mo>"
        "<mo id=last>!</mo></mrow><mrow><msub id=emb><mo>∑</mo><mi>i</mi></msub><mi>x</mi></mrow></math>");
    bool explicitForm = false;
    CHECK(mathMlOperatorForm(d["#p"], kAll, nullptr) == MathMlForm_Prefix);
    CHECK(mathMlOperatorForm(d["#i"], kAll, nullptr) == MathMlForm_Infix);
    CHECK(mathMlOperatorForm(d["#q"], kAll, nullptr) == MathMlForm_Postfix);
    CHECK(mathMlOperatorForm(d["#sp"], kAll, nullptr) == MathMlForm_Prefix);
    CHECK(mathMlOperatorForm(d["#alone"], kAll, nullptr) == MathMlForm_Infix);
    CHECK(mathMlOperatorForm(d["#s"], kAll, nullptr) == MathMlForm_Postfix);
    CHECK(mathMlOperatorForm(d["#r"], kAll, nullptr) == MathMlForm_Prefix);
    CHECK(mathMlOperatorForm(d["#f"], kAll, &explicitForm) == MathMlForm_Postfix);
    CHECK(explicitForm);
    CHECK(mathMlOperatorForm(d["#last"], kStyled, nullptr) == MathMlForm_Postfix);
    CHECK(mathMlOperatorForm(d["#emb"], kAll, nullptr) == MathMlForm_Prefix);
}

TEST_CASE("an operator's properties come from its attributes, then from the dictionary") {
    Document d("<math><mrow><mo id=open>(</mo><mi>x</mi><mo id=plus>+</mo><mi>y</mi>"
        "<mo id=bar>|</mo><mo id=set lspace='1em' rspace='50%' stretchy='FALSE' largeop='true' maxsize='infinity' "
        "minsize='2'>(</mo><mo id=sum>∑</mo><mo id=arrow>→</mo><mo id=fallback>)</mo><mo id=text>"
        "<mi>x</mi></mo></mrow><mstyle stretchy='false' form='prefix'><mo id=styled>)</mo></mstyle></math>");
    MathMlOperator op{};
    REQUIRE(mathMlOperator(d["#open"], kAll, &op));
    CHECK(op.form == MathMlForm_Prefix);
    CHECK(op.category == MathMlOperatorCategory_F);
    CHECK((op.flags & MathMlOperatorFlag_Stretchy) != 0);
    CHECK((op.flags & MathMlOperatorFlag_Fence) != 0);
    CHECK(op.lspace == 0);
    CHECK(op.axis == MathMlStretchAxis_Block);
    REQUIRE(mathMlOperator(d["#plus"], kAll, &op));
    CHECK(op.category == MathMlOperatorCategory_B);
    CHECK(op.lspace == 4);
    CHECK(op.rspace == 4);
    REQUIRE(mathMlOperator(d["#bar"], kAll, &op));
    CHECK(op.category == MathMlOperatorCategory_ForceDefault);
    CHECK(op.flags == MathMlOperatorFlag_Fence);
    REQUIRE(mathMlOperator(d["#set"], kAll, &op));
    CHECK((op.flags & MathMlOperatorFlag_Stretchy) == 0);
    CHECK((op.flags & MathMlOperatorFlag_LargeOp) != 0);
    CHECK((op.flags & MathMlOperatorFlag_Symmetric) != 0);
    CHECK(isTerm(op.lspaceAttribute, 1, MathMlUnit_Em));
    CHECK(isTerm(op.rspaceAttribute, 50, MathMlUnit_Percent));
    CHECK((op.maxsize.flags & MathMlLengthFlag_Infinite) != 0);
    CHECK(isTerm(op.minsize, 200, MathMlUnit_Percent));
    REQUIRE(mathMlOperator(d["#sum"], kAll, &op));
    CHECK(op.category == MathMlOperatorCategory_J);
    REQUIRE(mathMlOperator(d["#arrow"], kAll, &op));
    CHECK(op.axis == MathMlStretchAxis_Inline);
    // A closing parenthesis that is infix by position takes its postfix entry.
    REQUIRE(mathMlOperator(d["#fallback"], kAll, &op));
    CHECK(op.form == MathMlForm_Infix);
    REQUIRE(mathMlOperator(d["#fallback"], kAll, &op));
    CHECK(op.category == MathMlOperatorCategory_G);
    REQUIRE(mathMlOperator(d["#text"], kAll, &op));
    CHECK(op.category == MathMlOperatorCategory_Default);
    // An enclosing mstyle gives the defaults of an mo's attributes (mathml4/#presm_mstyle).
    REQUIRE(mathMlOperator(d["#styled"], kAll, &op));
    CHECK(op.explicitForm);
    CHECK(op.form == MathMlForm_Prefix);
    CHECK((op.flags & MathMlOperatorFlag_Stretchy) == 0);
}

TEST_CASE("a token's text has its white space trimmed and collapsed") {
    Document d("<math><mo id=a>  (  </mo><mi id=b> a \n b </mi><mi id=c><mi>x</mi></mi><mi id=e></mi></math>");
    PrRawString text{};
    REQUIRE(mathMlTokenText(d["#a"], &text));
    CHECK(prRawStringView(&text) == "(");
    prRawStringReset(&text);
    REQUIRE(mathMlTokenText(d["#b"], &text));
    CHECK(prRawStringView(&text) == "a b");
    prRawStringReset(&text);
    CHECK_FALSE(mathMlTokenText(d["#c"], &text));
    CHECK(mathMlTokenCodePoint(d["#a"]) == '(');
    CHECK(mathMlTokenCodePoint(d["#b"]) == 0);
    CHECK(mathMlTokenCodePoint(d["#e"]) == 0);
}

TEST_CASE("lengths parse as <length-percentage>, named spaces and MathML 3's forms") {
    CHECK(isTerm(length("2em"), 2, MathMlUnit_Em));
    CHECK(isTerm(length(" 1in "), 96, MathMlUnit_Px));
    CHECK(isTerm(length("12pt"), 16, MathMlUnit_Px));
    CHECK(isTerm(length("-3.5EX"), -3.5f, MathMlUnit_Ex));
    CHECK(isTerm(length("50%"), 50, MathMlUnit_Percent));
    CHECK(isTerm(length("0"), 0, MathMlUnit_Px));
    CHECK(isTerm(length("thickmathspace"), 5.0f / 18, MathMlUnit_Em));
    CHECK(isTerm(length("NegativeVeryThinMathSpace"), -2.0f / 18, MathMlUnit_Em));
    CHECK(length("2").count == 0);
    CHECK(isTerm(length("2", MathMlLengthSyntaxFlag_Unitless), 200, MathMlUnit_Percent));
    CHECK(length("+2em").count == 0);
    CHECK(length("2 em").count == 0);
    CHECK(length("2em3").count == 0);
    CHECK(length("").count == 0);
    CHECK(length("infinity").count == 0);
    CHECK((length("infinity", MathMlLengthSyntaxFlag_Infinity).flags & MathMlLengthFlag_Infinite) != 0);
    MathMlLength calc = length("calc(1em + 2px - 25%)");
    REQUIRE(calc.count == 3);
    CHECK(mathMlLengthHasPercent(calc));
    CHECK(isTerm(length("calc(2 * 3px)"), 6, MathMlUnit_Px));
    CHECK(length("calc(1em+2px)").count == 0);
    MathMlLength pseudo = length("+0.5height", MathMlLengthSyntaxFlag_Pseudo);
    CHECK(isTerm(pseudo, 0.5f, MathMlUnit_Height));
    CHECK(pseudo.flags == MathMlLengthFlag_Increment);
    CHECK(isTerm(length("-50%width", MathMlLengthSyntaxFlag_Pseudo), 0.5f, MathMlUnit_Width));
    CHECK(length("1height").count == 0);
}

TEST_CASE("booleans, integers and script levels parse by MathML's grammars") {
    CHECK(mathMlParseBoolean("TrUe") == MathMlBoolean_True);
    CHECK(mathMlParseBoolean("false") == MathMlBoolean_False);
    CHECK(mathMlParseBoolean(" true") == MathMlBoolean_None);
    uint32_t value = 0;
    CHECK(mathMlParseUnsigned("12", &value));
    CHECK(value == 12);
    CHECK_FALSE(mathMlParseUnsigned("+1", &value));
    CHECK_FALSE(mathMlParseUnsigned("", &value));
    int32_t level = 0;
    bool relative = false;
    CHECK(mathMlParseScriptLevel("-2", &level, &relative));
    CHECK(level == -2);
    CHECK(relative);
    CHECK(mathMlParseScriptLevel("3", &level, &relative));
    CHECK_FALSE(relative);
    CHECK_FALSE(mathMlParseScriptLevel("+-1", &level, &relative));
    CHECK_FALSE(mathMlParseScriptLevel("1.5", &level, &relative));
    CHECK(mathMlListItem("left  right", 1) == "right");
    CHECK(mathMlListItem("left right", 5) == "right");
    CHECK(mathMlListItem("  ", 0).data == nullptr);
}

TEST_CASE("the attributes of fractions, spaces, scripts, tables and enclosures") {
    Document d("<math><mfrac id=f linethickness='thick' numalign='LEFT' bevelled='true'><mi>a</mi><mi>b</mi></mfrac>"
        "<mfrac id=g linethickness='bad'><mi>a</mi><mi>b</mi></mfrac><mstyle linethickness='0'><mfrac id=h>"
        "<mi>a</mi><mi>b</mi></mfrac></mstyle><mspace id=s width='1em' height='2ex' depth='bad'/>"
        "<munderover id=u accent='true' accentunder='nope'><mi>a</mi><mi>b</mi><mi>c</mi></munderover>"
        "<mtable id=t columnalign='left right' rowspacing='1ex 2ex' rowlines='solid dashed' frame='dashed' "
        "align='top 2'><mtr columnalign='center'><mtd id=c1 columnspan='3' rowspan='0'></mtd><mtd id=c2></mtd></mtr>"
        "<mtr><mtd id=c3 columnalign='right' columnspan='0'></mtd><mtd id=c4></mtd></mtr></mtable>"
        "<menclose id=e notation='box  updiagonalstrike Circle bogus'></menclose>"
        "<mfenced id=fe open=' [ ' separators=' ; , '></mfenced><mi id=v mathvariant='double-struck'>R</mi>"
        "<mi id=n mathvariant='normal'>x</mi></math>");
    MathMlFraction f = mathMlFraction(d["#f"]);
    CHECK(isTerm(f.lineThickness, 200, MathMlUnit_Percent));
    CHECK(f.numeratorAlign == MathMlAlign_Left);
    CHECK(f.denominatorAlign == MathMlAlign_None);
    CHECK(f.bevelled);
    CHECK_FALSE(mathMlLengthIsPresent(mathMlFraction(d["#g"]).lineThickness));
    CHECK(isTerm(mathMlFraction(d["#h"]).lineThickness, 0, MathMlUnit_Percent));
    MathMlSpace s = mathMlSpace(d["#s"]);
    CHECK(isTerm(s.width, 1, MathMlUnit_Em));
    CHECK(isTerm(s.height, 2, MathMlUnit_Ex));
    CHECK_FALSE(mathMlLengthIsPresent(s.depth));
    MathMlUnderOver u = mathMlUnderOver(d["#u"]);
    CHECK(u.accent == MathMlBoolean_True);
    CHECK(u.accentUnder == MathMlBoolean_None);
    MathMlTableAlign align = mathMlTableAlign(d["#t"]);
    CHECK(align.align == MathMlVerticalAlign_Top);
    CHECK(align.row == 2);
    CHECK(mathMlCellColumnAlign(d["#c1"], 0) == MathMlAlign_Center);
    CHECK(mathMlCellColumnAlign(d["#c3"], 0) == MathMlAlign_Right);
    CHECK(mathMlCellColumnAlign(d["#c4"], 1) == MathMlAlign_Right);
    CHECK(mathMlCellColumnAlign(d["#c4"], 7) == MathMlAlign_Right);
    CHECK(isTerm(mathMlTableRowSpacing(d["#t"], 1), 2, MathMlUnit_Ex));
    CHECK(isTerm(mathMlTableRowSpacing(d["#t"], 9), 2, MathMlUnit_Ex));
    CHECK(mathMlTableRowLine(d["#t"], 0) == MathMlLine_Solid);
    CHECK(mathMlTableRowLine(d["#t"], 4) == MathMlLine_Dashed);
    CHECK(mathMlTableColumnLine(d["#t"], 0) == MathMlLine_None);
    CHECK(mathMlTable(d["#t"]).frame == MathMlLine_Dashed);
    uint32_t columns = 0;
    uint32_t rows = 0;
    mathMlCellSpans(d["#c1"], &columns, &rows);
    CHECK(columns == 3);
    CHECK(rows == 0);
    mathMlCellSpans(d["#c3"], &columns, &rows);
    CHECK(columns == 1);
    CHECK(rows == 1);
    CHECK(mathMlNotations(d["#e"]) ==
        (MathMlNotationFlag_Box | MathMlNotationFlag_UpDiagonalStrike | MathMlNotationFlag_Circle));
    CHECK(mathMlFencedOpen(d["#fe"]) == "[");
    CHECK(mathMlFencedClose(d["#fe"]) == ")");
    CHECK(mathMlFencedSeparator(d["#fe"], 0) == ';');
    CHECK(mathMlFencedSeparator(d["#fe"], 1) == ',');
    CHECK(mathMlFencedSeparator(d["#fe"], 4) == ',');
    PrUnicodeMathVariant variant = 0;
    CHECK(mathMlTokenVariant(d["#v"], &variant));
    CHECK(variant == PrUnicodeMathVariant_DoubleStruck);
    CHECK_FALSE(mathMlTokenVariant(d["#n"], &variant));
}

TEST_CASE("the attributes of elementary math") {
    Document d("<math><mstack id=s stackalign='left' charspacing='loose'><msgroup id=g position='-2' shift='1'>"
        "<msrow></msrow></msgroup><mscarries id=cs location='nw' crossout='updiagonalstrike verticalstrike'>"
        "<mscarry id=c crossout='none'></mscarry><mscarry id=c2 location='s'></mscarry></mscarries>"
        "<msline id=l length='3' mslinethickness='thin' leftoverhang='1em'/></mstack>"
        "<mlongdiv id=d longdivstyle='left)(right'></mlongdiv></math>");
    MathMlStack stack = mathMlStack(d["#s"]);
    CHECK(stack.stackAlign == MathMlAlign_Left);
    CHECK(stack.charAlign == MathMlAlign_Right);
    CHECK(isTerm(stack.charSpacing, 125, MathMlUnit_Percent));
    CHECK(stack.decimalCharacter == '.');
    CHECK(mathMlStackPosition(d["#g"]) == -2);
    CHECK(mathMlStackShift(d["#g"]) == 1);
    MathMlCarry carry = mathMlCarry(d["#c"]);
    CHECK(carry.location == MathMlCarryLocation_Nw);
    CHECK(carry.crossout == 0);
    carry = mathMlCarry(d["#c2"]);
    CHECK(carry.location == MathMlCarryLocation_S);
    CHECK(carry.crossout == (MathMlCrossoutFlag_UpDiagonalStrike | MathMlCrossoutFlag_VerticalStrike));
    MathMlStackLine line = mathMlStackLine(d["#l"]);
    CHECK(line.length == 3);
    CHECK(isTerm(line.thickness, 50, MathMlUnit_Percent));
    CHECK(isTerm(line.leftOverhang, 1, MathMlUnit_Em));
    CHECK(mathMlLongDivStyle(d["#d"]) == MathMlLongDivStyle_LeftParenRight);
}

TEST_CASE("an mpadded's sizes are its own, and take MathML 3's pseudo-units") {
    Document d("<math><mstyle lspace='3em'><mpadded id=p width='+2width' voffset='-0.5em'><mo id=o>+</mo></mpadded>"
        "</mstyle></math>");
    MathMlPadded padded = mathMlPadded(d["#p"]);
    CHECK(isTerm(padded.width, 2, MathMlUnit_Width));
    CHECK(padded.width.flags == MathMlLengthFlag_Increment);
    CHECK(isTerm(padded.voffset, -0.5f, MathMlUnit_Em));
    CHECK_FALSE(mathMlLengthIsPresent(padded.lspace));
    CHECK(isTerm(mathMlOperatorAttributes(d["#o"]).lspace, 3, MathMlUnit_Em));
}
