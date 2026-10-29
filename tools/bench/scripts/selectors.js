// Measures matching selectors against a document, the same way on every engine.
//
// Nothing here is engine specific. tools/bench/measure.js describes the loader, which runs this file the same way in
// pr_shell and in a browser.
//
// A case is one query that a script makes, over a document that holds a long list of siblings and a tree of nested
// groups. The structural and relational cases are expensive for an engine that forgets what it learned about one
// element while it matches the next.

// The number of siblings in the list, the number of levels in the tree, and the number of elements per parent in the
// tree.
const kSiblings = 1000;
const kDepth = 3;
const kFanout = 10;

// The number of levels in the chain, and the number of spans that each level holds.
const kChain = 100;
const kChainSpans = 5;

// The list: every fourth sibling is a p among li elements. Every third sibling is of class a, and the rest are of class
// b.
function listMarkup() {
    let out = '<ul id="list">';
    for (let i = 0; i < kSiblings; ++i) {
        const tag = i % 4 === 3 ? "p" : "li";
        out += "<" + tag + ' class="' + (i % 3 === 0 ? "a" : "b") + '"></' + tag + ">";
    }
    return out + "</ul>";
}

// The tree: groups `kDepth` levels deep. Each leaf is a span of class item, every seventh leaf is also of class x, and
// the leaf at the middle has the ID leaf.
function treeMarkup() {
    let leaves = 0;
    function level(depth) {
        let out = "";
        for (let i = 0; i < kFanout; ++i) {
            if (depth > 1) {
                out += '<div class="group">' + level(depth - 1) + "</div>";
                continue;
            }
            const leaf = leaves++;
            const id = leaf === 500 ? ' id="leaf"' : "";
            out += "<span" + id + ' class="item' + (leaf % 7 === 0 ? " x" : "") + '">text</span>';
        }
        return out;
    }
    return '<div id="tree">' + level(kDepth) + '</div><p id="target"></p>';
}

// The chain: `kChain` divs of class level, each inside the last. Each holds `kChainSpans` spans of class leaf before
// the next div, and the innermost span has the ID bottom.
function chainMarkup() {
    let out = '<div id="deep">';
    for (let i = 0; i < kChain; ++i) {
        out += '<div class="level">';
        for (let j = 0; j < kChainSpans; ++j)
            out += '<span class="leaf"></span>';
    }
    out += '<span id="bottom"></span>';
    for (let i = 0; i < kChain; ++i)
        out += "</div>";
    return out + "</div>";
}

// The cases. Each has a name, the element or document that it queries, the query and the selector. A case's `nodes` is
// the number of elements that it returns, which shows that two engines did the same work.
const kCases = [
    ["querySelector id", "document", "first", "#target"],
    ["querySelector class", "tree", "first", ".x"],
    ["querySelectorAll class", "tree", "all", ".item"],
    ["querySelectorAll descendant", "tree", "all", "div.group > div span.x"],
    ["querySelectorAll tag", "tree", "all", "span"],
    ["querySelectorAll attribute", "list", "all", "[class=a]"],
    ["querySelectorAll id descendant", "document", "all", "#tree .x"],
    ["querySelectorAll list", "tree", "all", "p, .x, #leaf"],
    ["querySelectorAll not", "list", "all", "li:not(.a)"],
    ["querySelectorAll is", "tree", "all", ":is(div, section) > .x"],
    ["querySelectorAll deep descendant", "deep", "all", "section span"],
    ["querySelectorAll deep child", "deep", "all", ".level > .level > span.leaf"],
    ["querySelectorAll nth-child", "list", "all", ":nth-child(2n+1)"],
    ["querySelectorAll nth-last-child", "list", "all", "li:nth-last-child(3n)"],
    ["querySelectorAll nth-of-type", "list", "all", "li:nth-of-type(2n)"],
    ["querySelectorAll nth-child of", "list", "all", ":nth-child(odd of .a)"],
    ["querySelectorAll has child", "tree", "all", "div:has(> .x) span"],
    ["querySelectorAll has descendant", "tree", "all", ".group:has(.x) .item"],
    ["querySelectorAll has next sibling", "list", "all", "li:has(+ p)"],
    ["querySelectorAll has later sibling", "list", "all", "li:has(~ .a.b)"],
    ["querySelectorAll has absent class", "list", "all", "li:has(+ .absent)"],
    ["querySelectorAll has after class", "tree", "all", ":has(~ * .x).item"],
    ["querySelectorAll has sibling pair", "list", "all", "li:has(~ p + p)"],
    ["querySelectorAll has deep", "deep", "all", "div:has(p)"],
    ["querySelectorAll has deep child", "deep", "all", "div:has(> div > p)"],
    ["matches", "leaf", "matches", ".group > .group .item:nth-child(10n+1)"],
    ["closest", "leaf", "closest", "#tree"],
    ["closest deep", "bottom", "closest", "#deep"],
];

function cases() {
    const host = document.body.appendChild(document.createElement("div"));
    host.innerHTML = listMarkup() + treeMarkup() + chainMarkup();

    return kCases.map(([name, on, ask, selector]) => {
        const target = on === "document" ? document : document.getElementById(on);
        let run;
        switch (ask) {
        case "first": run = () => target.querySelector(selector); break;
        case "all": run = () => target.querySelectorAll(selector); break;
        case "matches": run = () => target.matches(selector); break;
        case "closest": run = () => target.closest(selector); break;
        }
        const answer = run();
        const nodes = ask === "all" ? answer.length : answer ? 1 : 0;
        return {name, nodes, run};
    });
}

function benchmark(only) {
    const rows = [];
    for (const c of cases()) {
        if (only !== undefined && only.name !== c.name)
            continue;
        const row = {name: c.name, nodes: c.nodes};
        if (only !== undefined) {
            row.time = only.runs !== 0 ? elapsed(c.run, only.runs) / only.runs : 0;
            row.runs = only.runs;
        } else {
            Object.assign(row, measuredFields({time: measure(c.run)}));
        }
        rows.push(row);
    }

    return rows;
}

globalThis.__REPORT__(benchmark(globalThis.__ONLY__));
