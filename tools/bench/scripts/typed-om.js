// Measures the CSS Typed OM, the same way on every engine.
//
// Nothing here is engine specific. tools/bench/measure.js describes the loader, which runs this file the same way in
// pr_shell and in a browser.
//
// A case is one operation that a script performs on typed values. Most cases use values of the size that a page
// writes. A few use values nested `kDeep` levels, which are expensive for an engine that walks a value recursively or
// rebuilds it at every call.

const kDeep = 200;

// Returns a math value that wraps one length in `depth` nested negations.
function nested(depth) {
    let value = CSS.px(1);
    for (let i = 0; i < depth; ++i)
        value = new CSSMathNegate(value);
    return value;
}

// Returns the cases. Each has a name, a setup that runs once before timing, and the operation that it times. A case's
// `nodes` is the number of values that one run touches, which shows that two engines did the same work.
function cases() {
    const element = document.body.appendChild(document.createElement("div"));
    const map = element.attributeStyleMap;
    const sum = new CSSMathSum(CSS.px(1), CSS.percent(50), new CSSMathProduct(CSS.em(2), 3));
    const transform = new CSSTransformValue([
        new CSSTranslate(CSS.px(10), CSS.px(20)), new CSSRotate(CSS.deg(45)), new CSSScale(2, 3),
    ]);
    const deep = nested(kDeep);
    let flip = false;

    return [
        {name: "unit factory", nodes: 1,
         run() { CSS.px(1); }},

        {name: "math construct", nodes: 6,
         run() { new CSSMathSum(CSS.px(1), CSS.percent(50), new CSSMathProduct(CSS.em(2), 3)); }},

        {name: "math toString", nodes: 6,
         run() { sum.toString(); }},

        {name: "math type", nodes: 6,
         run() { sum.type(); }},

        {name: "numeric add", nodes: 4,
         run() { CSS.px(1).add(CSS.em(2), CSS.px(3)); }},

        {name: "numeric to", nodes: 1,
         run() { CSS.in(1).to("px"); }},

        {name: "numeric toSum", nodes: 6,
         run() { sum.toSum("px", "percent", "em"); }},

        {name: "numeric equals", nodes: 12,
         run() { sum.equals(sum); }},

        {name: "numeric parse", nodes: 6,
         run() { CSSNumericValue.parse("calc(1px + 2em * 3 - 50%)"); }},

        {name: "style parse transform", nodes: 10,
         run() { CSSStyleValue.parse("transform", "translate(10px, 20px) rotate(45deg) scale(2, 3)"); }},

        {name: "transform construct", nodes: 11,
         run() {
             new CSSTransformValue([
                 new CSSTranslate(CSS.px(10), CSS.px(20)), new CSSRotate(CSS.deg(45)),
                 new CSSScale(2, 3),
             ]);
         }},

        {name: "transform toString", nodes: 11,
         run() { transform.toString(); }},

        {name: "transform toMatrix", nodes: 11,
         run() { transform.toMatrix(); }},

        {name: "unparsed construct", nodes: 4,
         run() { new CSSUnparsedValue(["1px ", new CSSVariableReferenceValue("--a"), " 2px"]); }},

        {name: "map set", nodes: 1,
         run() {
             flip = !flip;
             map.set("width", flip ? CSS.px(10) : CSS.percent(50));
         }},

        {name: "map set transform", nodes: 11,
         run() { map.set("transform", transform); }},

        {name: "map get", nodes: 1,
         setup() { map.set("margin-left", CSS.px(4)); },
         run() { map.get("margin-left"); }},

        {name: "map get transform", nodes: 11,
         setup() { map.set("transform", transform); },
         run() { map.get("transform"); }},

        {name: "deep construct", nodes: kDeep + 1,
         run() { nested(kDeep); }},

        {name: "deep toString", nodes: kDeep + 1,
         run() { deep.toString(); }},

        {name: "deep to", nodes: kDeep + 1,
         run() { deep.to("px"); }},
    ];
}

function benchmark(only) {
    const rows = [];
    for (const c of cases()) {
        if (only !== undefined && only.name !== c.name)
            continue;
        if (c.setup)
            c.setup();
        const row = {name: c.name, nodes: c.nodes};
        if (only !== undefined) {
            row.time = only.runs !== 0 ? elapsed(() => c.run(), only.runs) / only.runs : 0;
            row.runs = only.runs;
        } else {
            Object.assign(row, measuredFields({time: measure(() => c.run())}));
        }
        rows.push(row);
    }

    return rows;
}

globalThis.__REPORT__(benchmark(globalThis.__ONLY__));
