// The Typed OM benchmark's cases through the C API, with no shell around them.
//
// ./dev.sh bench typed-om runs this and prints its rows beside the rows that pr_shell reports. Each case is the case
// that tools/bench/scripts/typed-om.js names, and does through C what the binding asks of C for the page. It builds the
// values that the page's objects represent and calls C on them, measured the same way. The difference between the two
// rows is therefore the cost of the binding and QuickJS. Here a style map is a style rule's block, while the page uses
// an element's block.
//
// Usage: typed_bench [case]
//
// If a case is named, only that case runs, measured repeatedly for five minutes (harnessRepeat), so that a profiler can
// be attached.

#include "pr/assert.h"
#include "pr/css.h"
#include "pr/string_ops.h"

#include "tools/harness/harness.h"

#include <stdio.h>

using namespace harness;

using namespace pr_impl;

namespace {

constexpr uint32_t kDeep = 200;

// The data that the cases work on, as the `cases` of bench.js holds it.
struct Work {
    PrCssValue* scratch;     // value that a case builds, cleared before each build
    PrCssValue* result;      // value that C writes for the case
    PrCssValue* sum;
    PrCssValue* transform;
    PrCssValue* deep;
    PrCssStyleSheet* sheet;
    PrCssStyleDeclaration* map;
    bool flip;
};

typedef void (*BodyFn)(Work* work);

void open(PrCssValue* value, PrCssValueKind kind, PrCssKeyword name = PrCssKeyword_Unknown) {
    PrCssValueFields fields = {};
    fields.kind = kind;
    fields.keyword = name;
    prCssValueOpen(value, &fields);
}

void calc(PrCssValue* value, PrCssCalc calc) {
    PrCssValueFields fields = {};
    fields.kind = PrCssValueKind_Calc;
    fields.calc = calc;
    prCssValueOpen(value, &fields);
}

void unit(PrCssValue* value, double number, PrCssUnit unit) {
    PrCssValueFields fields = {};
    if (unit == PrCssUnit_Number)
        fields.kind = PrCssValueKind_Number;
    else if (unit == PrCssUnit_Percent)
        fields.kind = PrCssValueKind_Percentage;
    else
        fields.kind = PrCssValueKind_Dimension;
    fields.number.value = number;
    fields.number.unit = unit;
    prCssValueAdd(value, &fields);
}

void comma(PrCssValue* value) {
    PrCssValueFields fields = {};
    fields.kind = PrCssValueKind_Literal;
    fields.literal = ',';
    prCssValueAdd(value, &fields);
}

// calc(1px + 50% + 2em * 3)
void buildSum(PrCssValue* value) {
    open(value, PrCssValueKind_Math);
    calc(value, PrCssCalc_Sum);
    unit(value, 1, PrCssUnit_Px);
    unit(value, 50, PrCssUnit_Percent);
    calc(value, PrCssCalc_Product);
    unit(value, 2, PrCssUnit_Em);
    unit(value, 3, PrCssUnit_Number);
    prCssValueClose(value);
    prCssValueClose(value);
    prCssValueClose(value);
}

// translate(10px, 20px) rotate(45deg) scale(2, 3)
void buildTransform(PrCssValue* value) {
    open(value, PrCssValueKind_Function, PrCssKeyword_Translate);
    unit(value, 10, PrCssUnit_Px);
    comma(value);
    unit(value, 20, PrCssUnit_Px);
    prCssValueClose(value);
    open(value, PrCssValueKind_Function, PrCssKeyword_Rotate);
    unit(value, 45, PrCssUnit_Deg);
    prCssValueClose(value);
    open(value, PrCssValueKind_Function, PrCssKeyword_Scale);
    unit(value, 2, PrCssUnit_Number);
    comma(value);
    unit(value, 3, PrCssUnit_Number);
    prCssValueClose(value);
}

void buildDeep(PrCssValue* value) {
    open(value, PrCssValueKind_Math);
    for (uint32_t i = 0; i < kDeep; ++i)
        calc(value, PrCssCalc_Negate);
    unit(value, 1, PrCssUnit_Px);
    for (uint32_t i = 0; i <= kDeep; ++i)
        prCssValueClose(value);
}

// Serializes a value and releases the result at once.
void serialize(const PrCssValue* value) {
    PrString written = prCssValueSerialize(value, 0, prCssValueSize(value));
    prStringReset(&written);
}

void setValue(Work* w, PrCssProperty property, const PrCssValue* value) {
    PrCssDeclarationName name = prCssDeclarationNameFromProperty(property);
    prCssStyleDeclarationSetValue(w->map, &name, value, false);
}

void getValue(Work* w, PrCssProperty property) {
    PrCssDeclarationName name = prCssDeclarationNameFromProperty(property);
    prCssValueClear(w->result);
    prCssStyleDeclarationValue(w->map, &name, w->result);
}

// A case, as bench.js names it: what it does once before it is timed, and the call that it times.
struct Case {
    const char* name;
    BodyFn setup;
    BodyFn run;
};

void none(Work*) {}

constexpr Case kCases[] = {
    {
        "unit factory",
        none,
        [](Work* w) {
            prCssValueClear(w->scratch);
            unit(w->scratch, 1, PrCssUnit_Px);
        },
    },
    {
        "math construct",
        none,
        [](Work* w) {
            prCssValueClear(w->scratch);
            buildSum(w->scratch);
        },
    },
    {"math toString", none, [](Work* w) { serialize(w->sum); }},
    {
        "math type",
        none,
        [](Work* w) {
            PrCssNumericType type;
            prCssValueNumericType(w->sum, 0, &type);
        },
    },
    {
        "numeric add",
        none,
        [](Work*) {
            PrCssNumericType px;
            PrCssNumericType em;
            PrCssNumericType sum;
            prCssNumericTypeFromUnit(PrCssUnit_Px, &px);
            prCssNumericTypeFromUnit(PrCssUnit_Em, &em);
            prCssNumericTypeAdd(&px, &em, &sum);
            prCssNumericTypeAdd(&sum, &px, &sum);
        },
    },
    {
        "numeric to",
        none,
        [](Work* w) {
            prCssValueClear(w->scratch);
            unit(w->scratch, 1, PrCssUnit_In);
            double number;
            prCssValueTo(w->scratch, 0, PrCssUnit_Px, &number);
        },
    },
    {
        "numeric toSum",
        none,
        [](Work* w) {
            const PrCssUnit units[] = {PrCssUnit_Px, PrCssUnit_Percent, PrCssUnit_Em};
            prCssValueClear(w->result);
            prCssValueToSum(w->sum, 0, units, 3, w->result);
        },
    },
    {
        "numeric equals",
        none,
        [](Work* w) {
            prCssValueClear(w->scratch);
            buildSum(w->scratch);
            buildSum(w->scratch);
            prCssValueEquals(w->scratch, 0, w->scratch, prCssValueNext(w->scratch, 0));
        },
    },
    {
        "numeric parse",
        none,
        [](Work* w) {
            prCssValueClear(w->result);
            prCssValueParseNumeric("calc(1px + 2em * 3 - 50%)", w->result);
        },
    },
    {
        "style parse transform",
        none,
        [](Work* w) {
            PrCssDeclarationName name = prCssDeclarationNameFromProperty(PrCssProperty_Transform);
            prCssValueClear(w->result);
            prCssValueParse(&name, "translate(10px, 20px) rotate(45deg) scale(2, 3)", w->result);
        },
    },
    {
        "transform construct",
        none,
        [](Work* w) {
            prCssValueClear(w->scratch);
            buildTransform(w->scratch);
        },
    },
    {"transform toString", none, [](Work* w) { serialize(w->transform); }},
    {
        "transform toMatrix",
        none,
        [](Work* w) {
            PrDomMatrix matrix = prDomMatrixIdentity();
            for (uint32_t node = 0; node < prCssValueSize(w->transform); node = prCssValueNext(w->transform, node)) {
                PrDomMatrix next;
                prCssValueTransformMatrix(w->transform, node, &next);
                prDomMatrixMultiply(&matrix, &next);
            }
        },
    },
    {
        "unparsed construct",
        none,
        [](Work* w) {
            PrCssValueFields fields = {};
            fields.kind = PrCssValueKind_Tokens;
            fields.text = "1px var(--a) 2px";
            prCssValueClear(w->scratch);
            prCssValueAdd(w->scratch, &fields);
        },
    },
    {
        "map set",
        none,
        [](Work* w) {
            w->flip = !w->flip;
            prCssValueClear(w->scratch);
            unit(w->scratch, w->flip ? 10 : 50, w->flip ? PrCssUnit_Px : PrCssUnit_Percent);
            setValue(w, PrCssProperty_Width, w->scratch);
        },
    },
    {
        "map set transform",
        none,
        [](Work* w) { setValue(w, PrCssProperty_Transform, w->transform); },
    },
    {
        "map get",
        [](Work* w) {
            prCssValueClear(w->scratch);
            unit(w->scratch, 4, PrCssUnit_Px);
            setValue(w, PrCssProperty_MarginLeft, w->scratch);
        },
        [](Work* w) { getValue(w, PrCssProperty_MarginLeft); },
    },
    {
        "map get transform",
        [](Work* w) { setValue(w, PrCssProperty_Transform, w->transform); },
        [](Work* w) { getValue(w, PrCssProperty_Transform); },
    },
    {
        "deep construct",
        none,
        [](Work* w) {
            prCssValueClear(w->scratch);
            buildDeep(w->scratch);
        },
    },
    {"deep toString", none, [](Work* w) { serialize(w->deep); }},
    {
        "deep to",
        none,
        [](Work* w) {
            double number;
            prCssValueTo(w->deep, 0, PrCssUnit_Px, &number);
        },
    },
};

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    if (argc > 2) {
        fprintf(stderr, "usage: typed_bench [case]\n");
        return 2;
    }
    Work work = {};
    work.scratch = prCssValueCreate();
    work.result = prCssValueCreate();
    work.sum = prCssValueCreate();
    work.transform = prCssValueCreate();
    work.deep = prCssValueCreate();
    buildSum(work.sum);
    buildTransform(work.transform);
    buildDeep(work.deep);
    work.sheet = prCssStyleSheetCreate(nullptr);
    prCssStyleSheetReplaceSync(work.sheet, "div {}");
    PrCssStyleRule* rule = prCssStyleRuleQuery(prCssRuleListRuleAt(prCssStyleSheetCssRules(work.sheet), 0));
    work.map = prCssStylePropertiesAsCssStyleDeclaration(prCssStyleRuleStyle(rule));

    PrStringView only = argc == 2 ? argv[1] : PrStringView{};
    printf("[");
    bool first = true;
    for (const Case& c : kCases) {
        if (only.data != nullptr) {
            if (only != c.name)
                continue;
            c.setup(&work);
            harnessRepeat([&] { c.run(&work); });
        }
        c.setup(&work);
        harnessWriteCase(first, c.name, harnessMeasure([&] { c.run(&work); }));
        first = false;
    }
    printf("\n]\n");

    prCssStyleSheetRelease(work.sheet);
    prCssValueDestroy(work.scratch);
    prCssValueDestroy(work.result);
    prCssValueDestroy(work.sum);
    prCssValueDestroy(work.transform);
    prCssValueDestroy(work.deep);
    return 0;
}
