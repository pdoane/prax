#include "pr/array.h"
#include "pr/css.h"
#include "pr/string.h"
#include "pr/string_ops.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace pr;

namespace {

// A property's name, for a call that takes a pointer to it.
struct Prop {
    PrCssDeclarationName name;

    explicit Prop(PrCssProperty property) : name(prCssDeclarationNameFromProperty(property)) {}

    using Pointer = const PrCssDeclarationName* PR_NONNULL;
    operator Pointer() const { return &name; }
};

// A value built node by node. Each node is opened, then closed once its children are added.
struct Value {
    PrCssValue* value = prCssValueCreate();

    Value() = default;
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;
    ~Value() { prCssValueDestroy(value); }

    operator PrCssValue*() const { return value; }

    PrStatus open(PrCssValueKind kind) {
        PrCssValueFields fields = {};
        fields.kind = kind;
        return prCssValueOpen(value, &fields);
    }

    PrStatus openCalc(PrCssCalc calc) {
        PrCssValueFields fields = {};
        fields.kind = PrCssValueKind_Calc;
        fields.calc = calc;
        return prCssValueOpen(value, &fields);
    }

    PrStatus openFunction(PrCssKeyword name) {
        PrCssValueFields fields = {};
        fields.kind = PrCssValueKind_Function;
        fields.keyword = name;
        return prCssValueOpen(value, &fields);
    }

    PrStatus close() { return prCssValueClose(value); }

    Value& number(double number, PrCssUnit unit) {
        PrCssValueFields fields = {};
        if (unit == PrCssUnit_Number)
            fields.kind = PrCssValueKind_Number;
        else if (unit == PrCssUnit_Percent)
            fields.kind = PrCssValueKind_Percentage;
        else
            fields.kind = PrCssValueKind_Dimension;
        fields.number.value = number;
        fields.number.unit = unit;
        REQUIRE(prCssValueAdd(value, &fields) == PrStatus_Ok);
        return *this;
    }

    Value& keyword(PrCssKeyword keyword) {
        PrCssValueFields fields = {};
        fields.kind = PrCssValueKind_Keyword;
        fields.keyword = keyword;
        REQUIRE(prCssValueAdd(value, &fields) == PrStatus_Ok);
        return *this;
    }

    Value& literal(char character) {
        PrCssValueFields fields = {};
        fields.kind = PrCssValueKind_Literal;
        fields.literal = uint8_t(character);
        REQUIRE(prCssValueAdd(value, &fields) == PrStatus_Ok);
        return *this;
    }

    Value& ident(const char* text) {
        PrCssValueFields fields = {};
        fields.kind = PrCssValueKind_Ident;
        fields.text = text;
        REQUIRE(prCssValueAdd(value, &fields) == PrStatus_Ok);
        return *this;
    }
};

String take(PrRawString text) {
    PrStringView view = prRawStringView(&text);
    String result(view.size != 0 ? view : PrStringView(""));
    prRawStringReset(&text);
    return result;
}

String serialize(const PrCssValue* value) {
    return take(prCssValueSerialize(value, 0, prCssValueSize(value)));
}

// Returns the serialization of each tree of a value. Clears the value.
Array<String> serializeAll(PrCssValue* value) {
    Array<String> result;
    for (uint32_t tree = 0; tree < prCssValueSize(value); tree = prCssValueNext(value, tree))
        result.push(take(prCssValueSerialize(value, tree, prCssValueNext(value, tree))));
    prCssValueClear(value);
    return result;
}

PrCssValueKind kindAt(const PrCssValue* value, uint32_t node) {
    return prCssValueFields(value, node).kind;
}

// Returns a style rule's block, from a sheet that the caller releases.
PrCssStyleDeclaration* newBlock(PrCssStyleSheet** sheet) {
    *sheet = prCssStyleSheetCreate(nullptr);
    REQUIRE(prCssStyleSheetReplaceSync(*sheet, "a {}") == PrStatus_Ok);
    PrCssStyleRule* rule = prCssStyleRuleQuery(prCssRuleListRuleAt(prCssStyleSheetCssRules(*sheet), 0));
    return prCssStylePropertiesAsCssStyleDeclaration(prCssStyleRuleStyle(rule));
}

String property(PrCssStyleDeclaration* block, PrCssProperty property) {
    return take(prCssStyleDeclarationGetPropertyValue(block, Prop(property)));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("units and keywords are named as values write them") {
    PrCssUnit unit;
    REQUIRE(prCssUnitFromName("KHZ", &unit));
    CHECK(unit == PrCssUnit_KHz);
    CHECK(prCssUnitName(unit) == "khz");
    REQUIRE(prCssUnitFromName("percent", &unit));
    CHECK(unit == PrCssUnit_Percent);
    CHECK_FALSE(prCssUnitFromName("lemon", &unit));

    CHECK(prCssKeywordFromText("AUTO") == PrCssKeyword_Auto);
    CHECK(prCssKeywordFromText("lemon") == PrCssKeyword_Unknown);
    CHECK(PrStringView(prCssKeywordName(PrCssKeyword_Auto).data, 4) == "auto");
    PrColor8 color;
    REQUIRE(prCssColorFromKeyword(PrCssKeyword_Red, &color));
    CHECK((color.r == 255 && color.g == 0 && color.a == 255));
    REQUIRE(prCssColorFromKeyword(PrCssKeyword_Transparent, &color));
    CHECK(color.a == 0);
    CHECK_FALSE(prCssColorFromKeyword(PrCssKeyword_Auto, &color));

    CHECK(prCssPropertyListValued(PrCssProperty_TransitionDuration));
    CHECK_FALSE(prCssPropertyListValued(PrCssProperty_Width));
    CHECK(prCssPropertyLonghands(PrCssProperty_Width, nullptr, 0) == 0);
    PrCssProperty longhands[4];
    REQUIRE(prCssPropertyLonghands(PrCssProperty_Margin, longhands, 4) == 4);
    CHECK(longhands[0] == PrCssProperty_MarginTop);
}

TEST_CASE("a value is built a node at a time, each checked as it closes") {
    Value value;
    REQUIRE(value.open(PrCssValueKind_Math) == PrStatus_Ok);
    REQUIRE(value.openCalc(PrCssCalc_Sum) == PrStatus_Ok);
    value.number(1, PrCssUnit_Px).number(2, PrCssUnit_Em);
    REQUIRE(value.close() == PrStatus_Ok);
    REQUIRE(value.close() == PrStatus_Ok);
    value.keyword(PrCssKeyword_Auto);
    CHECK(prCssValueSize(value) == 5);
    CHECK(prCssValueNext(value, 0) == 4);
    CHECK(prCssValueNext(value, 1) == 4);
    CHECK(prCssValueNext(value, 2) == 3);
    PrCssValueFields fields = prCssValueFields(value, 3);
    CHECK(fields.kind == PrCssValueKind_Dimension);
    CHECK(fields.number.value == 2);
    CHECK(fields.number.unit == PrCssUnit_Em);
    CHECK(prCssValueFields(value, 4).keyword == PrCssKeyword_Auto);
    // A calculation that a caller builds is written as it was built.
    CHECK(serialize(value) == "calc(2em + 1px) auto");

    // A node whose kind does not accept its children is removed when it closes.
    REQUIRE(value.open(PrCssValueKind_Math) == PrStatus_Ok);
    REQUIRE(value.openCalc(PrCssCalc_Negate) == PrStatus_Ok);
    value.number(1, PrCssUnit_Px).number(2, PrCssUnit_Px);
    CHECK(value.close() == PrStatus_InvalidData);
    CHECK(value.close() == PrStatus_InvalidData);
    REQUIRE(value.open(PrCssValueKind_Color) == PrStatus_Ok);
    value.number(1, PrCssUnit_Number);
    CHECK(value.close() == PrStatus_InvalidData);
    CHECK(prCssValueSize(value) == 5);

    // A calculation has a type. A length and a time do not add, and a length times a length has a type that no place
    // accepts.
    REQUIRE(value.open(PrCssValueKind_Math) == PrStatus_Ok);
    REQUIRE(value.openCalc(PrCssCalc_Product) == PrStatus_Ok);
    value.number(1, PrCssUnit_Px).number(2, PrCssUnit_Px);
    REQUIRE(value.close() == PrStatus_Ok);
    REQUIRE(value.close() == PrStatus_Ok);
    CHECK(prCssValueSize(value) == 9);
    prCssValueClear(value);
    REQUIRE(value.open(PrCssValueKind_Math) == PrStatus_Ok);
    REQUIRE(value.openCalc(PrCssCalc_Sum) == PrStatus_Ok);
    value.number(1, PrCssUnit_Px).number(2, PrCssUnit_S);
    REQUIRE(value.close() == PrStatus_Ok);
    CHECK(value.close() == PrStatus_InvalidData);
    CHECK(prCssValueSize(value) == 0);
    value.keyword(PrCssKeyword_Auto);

    // A calc-operator node is only valid inside a calculation.
    CHECK(value.openCalc(PrCssCalc_Sum) == PrStatus_InvalidData);
    // A kind, unit, keyword or space that no member names is not a node.
    PrCssValueFields invalid = {};
    invalid.kind = PrCssValueKind_Count;
    CHECK(prCssValueAdd(value, &invalid) == PrStatus_InvalidData);
    invalid.kind = PrCssValueKind_Keyword;
    invalid.keyword = PrCssKeyword_Count;
    CHECK(prCssValueAdd(value, &invalid) == PrStatus_InvalidData);
    invalid = {};
    invalid.kind = PrCssValueKind_Color;
    invalid.color.space = PrColorSpace_Lab;
    invalid.color.legacy = true;
    CHECK(prCssValueAdd(value, &invalid) == PrStatus_InvalidData);
    CHECK(prCssValueSize(value) == 1);
    value.number(1, PrCssUnit_Px);

    // A tree of one value added to another.
    Value copy;
    REQUIRE(prCssValueAddTree(copy, value, 0) == PrStatus_Ok);
    CHECK(prCssValueEquals(copy, 0, value, 0));
    CHECK_FALSE(prCssValueEquals(copy, 0, value, 1));
}

TEST_CASE("a value is no deeper than its limit") {
    Value value;
    REQUIRE(value.open(PrCssValueKind_Math) == PrStatus_Ok);
    for (uint32_t i = 1; i < kPrCssValueMaxDepth; ++i)
        REQUIRE(value.openCalc(PrCssCalc_Negate) == PrStatus_Ok);
    CHECK(value.openCalc(PrCssCalc_Negate) == PrStatus_OutOfRange);
    Value inner;
    inner.number(1, PrCssUnit_Px);
    CHECK(prCssValueAddTree(value, inner, 0) == PrStatus_OutOfRange);
}

TEST_CASE("a numeric value's type adds with a percent hint where percentages meet lengths") {
    Value sum;
    sum.open(PrCssValueKind_Math);
    sum.openCalc(PrCssCalc_Sum);
    sum.number(1, PrCssUnit_Px).number(2, PrCssUnit_Percent);
    sum.close();
    REQUIRE(sum.close() == PrStatus_Ok);
    PrCssNumericType type = {};
    REQUIRE(prCssValueNumericType(sum, 0, &type) == PrStatus_Ok);
    CHECK(type.exponents[PrCssNumericBaseType_Length] == 1);
    CHECK(type.exponents[PrCssNumericBaseType_Percent] == 0);
    CHECK(type.hasPercentHint);
    CHECK(type.percentHint == PrCssNumericBaseType_Length);

    PrCssNumericType length;
    PrCssNumericType time;
    REQUIRE(prCssNumericTypeFromUnit(PrCssUnit_Px, &length));
    REQUIRE(prCssNumericTypeFromUnit(PrCssUnit_S, &time));
    CHECK_FALSE(prCssNumericTypeFromUnit(PrCssUnit_DB, &time));
    PrCssNumericType product;
    CHECK_FALSE(prCssNumericTypeAdd(&length, &time, &product));
    REQUIRE(prCssNumericTypeMultiply(&length, &length, &product));
    CHECK(product.exponents[PrCssNumericBaseType_Length] == 2);
    CHECK(prCssNumericTypeInvert(&length).exponents[PrCssNumericBaseType_Length] == -1);
}

TEST_CASE("a numeric value converts to compatible units and writes itself as a sum") {
    Value sum;
    sum.open(PrCssValueKind_Math);
    sum.openCalc(PrCssCalc_Sum);
    sum.number(1, PrCssUnit_In).number(4, PrCssUnit_Px).number(2, PrCssUnit_Em);
    sum.close();
    REQUIRE(sum.close() == PrStatus_Ok);
    double number = 0;
    CHECK(prCssValueTo(sum, 0, PrCssUnit_Px, &number) == PrStatus_InvalidData);

    Value result;
    REQUIRE(prCssValueToSum(sum, 0, nullptr, 0, result) == PrStatus_Ok);
    CHECK(kindAt(result, 0) == PrCssValueKind_Math);
    CHECK(kindAt(result, 1) == PrCssValueKind_Calc);
    CHECK(prCssValueFields(result, 2).number.unit == PrCssUnit_Em);
    CHECK(prCssValueFields(result, 3).number.value == 100);
    prCssValueClear(result);
    const PrCssUnit units[] = {PrCssUnit_Px};
    CHECK(prCssValueToSum(sum, 0, units, 1, result) == PrStatus_InvalidData);
    CHECK(prCssValueSize(result) == 0);

    Value inch;
    inch.number(1, PrCssUnit_In);
    REQUIRE(prCssValueTo(inch, 0, PrCssUnit_Px, &number) == PrStatus_Ok);
    CHECK(number == 96);
}

TEST_CASE("a transform function gives its matrix, its lengths absolute") {
    Value translate;
    translate.openFunction(PrCssKeyword_Translate);
    translate.number(10, PrCssUnit_Px).literal(',').number(20, PrCssUnit_Px);
    REQUIRE(translate.close() == PrStatus_Ok);
    PrDomMatrix matrix;
    REQUIRE(prCssValueTransformMatrix(translate, 0, &matrix) == PrStatus_Ok);
    CHECK(matrix.m[12] == 10);
    CHECK(matrix.m[13] == 20);
    CHECK(matrix.is2D);

    // A calculation is accepted if it is one number of pixels.
    Value calculated;
    calculated.openFunction(PrCssKeyword_Translatex);
    calculated.open(PrCssValueKind_Math);
    calculated.openCalc(PrCssCalc_Sum);
    calculated.number(1, PrCssUnit_In).number(4, PrCssUnit_Px);
    calculated.close();
    calculated.close();
    REQUIRE(calculated.close() == PrStatus_Ok);
    REQUIRE(prCssValueTransformMatrix(calculated, 0, &matrix) == PrStatus_Ok);
    CHECK(matrix.m[12] == 100);

    Value relative;
    relative.openFunction(PrCssKeyword_Translatex);
    relative.number(1, PrCssUnit_Em);
    REQUIRE(relative.close() == PrStatus_Ok);
    CHECK(prCssValueTransformMatrix(relative, 0, &matrix) == PrStatus_InvalidData);
}

TEST_CASE("a path's data is its commands, each a letter followed by its numbers, and writes in one form") {
    Value values;
    REQUIRE(prCssValueParse(Prop(PrCssProperty_D), "path('m 10.0 20 30 40 z')", values) == PrStatus_Ok);
    CHECK(kindAt(values, 0) == PrCssValueKind_Function);
    // The moveto, the lineto that its second pair is, and the closepath.
    REQUIRE(prCssValueSize(values) == 1 + 3 + 3 + 1);
    CHECK(prCssValueFields(values, 1).literal == 'm');
    CHECK(prCssValueFields(values, 3).number.value == 20);
    CHECK(prCssValueFields(values, 4).literal == 'l');
    CHECK(serialize(values) == "path(\"m 10 20 l 30 40 Z\")");

    // A fill rule comes first, and data in error holds the commands before the error.
    prCssValueClear(values);
    REQUIRE(prCssValueParse(Prop(PrCssProperty_ClipPath), "path(evenodd, 'M 0 0 L 10')", values) == PrStatus_Ok);
    CHECK(serialize(values) == "path(evenodd, \"M 0 0\")");
}

TEST_CASE("parsing gives the value a declaration of the text holds") {
    Value values;
    REQUIRE(prCssValueParse(Prop(PrCssProperty_Width), "calc(1px + 2em)", values) == PrStatus_Ok);
    CHECK(kindAt(values, 0) == PrCssValueKind_Math);
    CHECK(serializeAll(values) == Array<String>({"calc(2em + 1px)"}));

    REQUIRE(prCssValueParse(Prop(PrCssProperty_TransitionDuration), "1s, 2s", values) == PrStatus_Ok);
    CHECK(serializeAll(values) == Array<String>({"1s", ",", "2s"}));

    // A named color stays the keyword it was written as, while a color parse gives its channels.
    REQUIRE(prCssValueParse(Prop(PrCssProperty_Color), "red", values) == PrStatus_Ok);
    CHECK(kindAt(values, 0) == PrCssValueKind_Keyword);
    prCssValueClear(values);
    REQUIRE(prCssValueParseColor(" red ", values) == PrStatus_Ok);
    CHECK(kindAt(values, 0) == PrCssValueKind_Color);
    PrCssColorChannel channels[4];
    prCssValueColorChannels(values, 0, channels);
    CHECK(prCssValueFields(values, channels[0].node).number.value == 255);
    CHECK(channels[0].number == 1.0 / 255.0);
    CHECK(serializeAll(values) == Array<String>({"rgb(255, 0, 0)"}));
    CHECK(prCssValueParseColor("1px", values) == PrStatus_Syntax);

    // A custom property's value is its tokens, and so is a value that a var() reaches.
    const PrCssDeclarationName custom = {PrCssProperty_Count, PrCssDescriptor_None, prRawStringExternal("--x")};
    REQUIRE(prCssValueParse(&custom, " a var(--y, b)", values) == PrStatus_Ok);
    CHECK(kindAt(values, 0) == PrCssValueKind_Tokens);
    prCssValueClear(values);
    REQUIRE(prCssValueParse(Prop(PrCssProperty_Width), "var(--y)", values) == PrStatus_Ok);
    CHECK(kindAt(values, 0) == PrCssValueKind_Tokens);
    prCssValueClear(values);

    // A shorthand's value is what its longhands serialize as.
    REQUIRE(prCssValueParse(Prop(PrCssProperty_Margin), "1px 2px 1px 2px", values) == PrStatus_Ok);
    CHECK(serialize(values) == "1px 2px");
    prCssValueClear(values);

    // Parsing appends, and leaves the value unchanged if it fails.
    REQUIRE(prCssValueParse(Prop(PrCssProperty_Width), "1px", values) == PrStatus_Ok);
    CHECK(prCssValueParse(Prop(PrCssProperty_Width), "red", values) == PrStatus_Syntax);
    CHECK(serializeAll(values) == Array<String>({"1px"}));

    // A descriptor is not a property, and these values are for properties.
    const PrCssDeclarationName src = {PrCssProperty_Count, PrCssDescriptor_FontFaceSrc, prRawStringNull()};
    CHECK(prCssValueParse(&src, "1px", values) == PrStatus_InvalidData);
    CHECK(prCssValueParseNumeric("auto", values) == PrStatus_Syntax);
    REQUIRE(prCssValueParseNumeric(" 1px ", values) == PrStatus_Ok);
    CHECK(serializeAll(values) == Array<String>({"1px"}));
}

TEST_CASE("the var() references of tokens are found at their top level") {
    const PrStringView text = "a var(--b, var(--c)) d var( --e )";
    PrCssVariableReference references[4];
    REQUIRE(prCssVariableReferences(text, references, 4) == 2);
    CHECK(references[0].start == 2);
    CHECK(references[0].end == 20);
    CHECK(references[0].name == "--b");
    REQUIRE(references[0].hasFallback);
    CHECK(prStringSlice(text, references[0].fallbackStart, references[0].fallbackEnd - references[0].fallbackStart) ==
        " var(--c)");
    CHECK(references[1].name == "--e");
    CHECK_FALSE(references[1].hasFallback);
    CHECK(prCssVariableReferences("var(x)", nullptr, 0) == 0);
}

TEST_CASE("a value declared is matched by its type, not by the text it would be written as") {
    PrCssStyleSheet* sheet = nullptr;
    PrCssStyleDeclaration* block = newBlock(&sheet);

    // A number is not a length, though a zero written without a unit is.
    Value zero;
    zero.number(0, PrCssUnit_Number);
    CHECK(prCssStyleDeclarationSetValue(block, Prop(PrCssProperty_Width), zero, false) == PrStatus_InvalidData);
    CHECK(property(block, PrCssProperty_Width).isEmpty());
    REQUIRE(prCssStyleDeclarationSetValue(block, Prop(PrCssProperty_LineHeight), zero, false) == PrStatus_Ok);
    CHECK(property(block, PrCssProperty_LineHeight) == "0");

    // A number outside the range that its place accepts is wrapped in a calculation, and so is a non-integer if its
    // place requires an integer.
    Value negative;
    negative.number(-1, PrCssUnit_In);
    REQUIRE(prCssStyleDeclarationSetValue(block, Prop(PrCssProperty_Width), negative, false) == PrStatus_Ok);
    CHECK(property(block, PrCssProperty_Width) == "calc(-96px)");
    Value fraction;
    fraction.number(1.5, PrCssUnit_Number);
    REQUIRE(prCssStyleDeclarationSetValue(block, Prop(PrCssProperty_ZIndex), fraction, false) == PrStatus_Ok);
    CHECK(property(block, PrCssProperty_ZIndex) == "calc(1.5)");

    // A calculation is simplified as it is declared.
    Value sum;
    sum.open(PrCssValueKind_Math);
    sum.openCalc(PrCssCalc_Sum);
    sum.number(1, PrCssUnit_Px).number(2, PrCssUnit_Px);
    sum.close();
    sum.close();
    REQUIRE(prCssStyleDeclarationSetValue(block, Prop(PrCssProperty_Width), sum, false) == PrStatus_Ok);
    CHECK(property(block, PrCssProperty_Width) == "calc(3px)");

    // An ident is a keyword only for the grammar that has it.
    Value lemon;
    lemon.ident("lemon");
    CHECK(prCssStyleDeclarationSetValue(block, Prop(PrCssProperty_Width), lemon, false) == PrStatus_InvalidData);
    REQUIRE(prCssStyleDeclarationSetValue(block, Prop(PrCssProperty_AnimationName), lemon, false) == PrStatus_Ok);
    CHECK(property(block, PrCssProperty_AnimationName) == "lemon");

    // A shorthand accepts a value that its grammar accepts, and sets its longhands.
    Value length;
    length.number(4, PrCssUnit_Px);
    REQUIRE(prCssStyleDeclarationSetValue(block, Prop(PrCssProperty_Margin), length, false) == PrStatus_Ok);
    CHECK(property(block, PrCssProperty_MarginLeft) == "4px");
    Value got;
    REQUIRE(prCssStyleDeclarationValue(block, Prop(PrCssProperty_Margin), got) == PrStatus_Ok);
    CHECK(serialize(got) == "4px");
    prCssValueClear(got);

    // A list is its items and the commas between them. Appending adds to it.
    Value times;
    times.number(1, PrCssUnit_S).literal(',').number(2, PrCssUnit_S);
    CHECK(prCssStyleDeclarationSetValue(block, Prop(PrCssProperty_Width), times, false) == PrStatus_InvalidData);
    REQUIRE(prCssStyleDeclarationSetValue(block, Prop(PrCssProperty_TransitionDuration), times, false) == PrStatus_Ok);
    REQUIRE(prCssStyleDeclarationAppendValue(block, Prop(PrCssProperty_TransitionDuration), times) == PrStatus_Ok);
    CHECK(property(block, PrCssProperty_TransitionDuration) == "1s, 2s, 1s, 2s");
    CHECK(prCssStyleDeclarationAppendValue(block, Prop(PrCssProperty_Width), length) == PrStatus_InvalidData);

    // A color given by its channels becomes the color function that it folds from.
    Value color;
    PrCssValueFields rgb = {};
    rgb.kind = PrCssValueKind_Color;
    rgb.color.space = PrColorSpace_Srgb;
    rgb.color.legacy = true;
    REQUIRE(prCssValueOpen(color, &rgb) == PrStatus_Ok);
    color.number(255, PrCssUnit_Number) .number(0, PrCssUnit_Number) .number(0, PrCssUnit_Number)
        .number(50, PrCssUnit_Percent);
    REQUIRE(color.close() == PrStatus_Ok);
    REQUIRE(prCssStyleDeclarationSetValue(block, Prop(PrCssProperty_Color), color, false) == PrStatus_Ok);
    CHECK(property(block, PrCssProperty_Color) == "rgba(255, 0, 0, 0.5)");

    // A custom property's value is the tokens that the value is written as.
    const PrCssDeclarationName custom = {PrCssProperty_Count, PrCssDescriptor_None, prRawStringExternal("--x")};
    REQUIRE(prCssStyleDeclarationSetValue(block, &custom, sum, false) == PrStatus_Ok);
    REQUIRE(prCssStyleDeclarationValue(block, &custom, got) == PrStatus_Ok);
    CHECK(kindAt(got, 0) == PrCssValueKind_Tokens);
    CHECK(serialize(got) == "calc(1px + 2px)");
    prCssValueClear(got);

    const PrCssDeclarationName src = {PrCssProperty_Count, PrCssDescriptor_FontFaceSrc, prRawStringNull()};
    CHECK(prCssStyleDeclarationValue(block, &src, got) == PrStatus_InvalidData);
    prCssStyleSheetRelease(sheet);
}
