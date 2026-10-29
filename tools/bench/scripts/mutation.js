// Measures mutating a document and its stylesheets, the same way on every engine.
//
// Nothing here is engine specific: the DOM and the CSSOM are standard browser APIs. tools/bench/measure.js describes
// the loader, which runs this file the same way in pr_shell and in a browser. `__INPUT__` holds the stylesheets that
// the cases work on.
//
// A case is one mutation that a page makes in a loop. Each case runs on a document that the earlier cases have mutated,
// as a real page's document would be, so anything an engine keeps from what a case discarded is still there for the
// next case.

// Builds a tree of `depth` levels below `root`, with `fanout` elements per parent and text in each leaf, as a page
// builds before it inserts a component.
function build(root, depth, fanout) {
    for (let i = 0; i < fanout; ++i) {
        const child = document.createElement(depth > 1 ? "div" : "span");
        child.className = "item";
        if (depth > 1)
            build(child, depth - 1, fanout);
        else
            child.appendChild(document.createTextNode("text"));
        root.appendChild(child);
    }
    return root;
}

// Visits every node below `node`, requesting each one's object.
function walk(node) {
    let count = 1;
    for (let child = node.firstChild; child !== null; child = child.nextSibling)
        count += walk(child);
    return count;
}

// Returns the markup of a tree like the one that `build` makes, for the cases that parse markup.
function markup(depth, fanout) {
    const inner = depth > 1 ? markup(depth - 1, fanout) : "text";
    const tag = depth > 1 ? "div" : "span";
    return ('<' + tag + ' class="item">' + inner + '</' + tag + '>').repeat(fanout);
}

// Returns `text` followed by a comment that differs at every call, so that no cache holds a parse of it. The cases that
// measure the cache say so in their names.
let uniqueCount = 0;
function unique(text) {
    return text + "\n/* " + ++uniqueCount + " */";
}

// Returns the cases. Each has a name, a setup that runs once before timing, and the mutation that it times. A case's
// `nodes` or `rules` is the size of what one run touches, which shows that two engines did the same work.
function cases(input) {
    const host = document.body.appendChild(document.createElement("div"));
    const other = document.body.appendChild(document.createElement("div"));
    const html = markup(3, 10);
    const tree = walk(build(document.createElement("div"), 3, 10));
    let subtree = null;
    let detached = null;
    let sheet = null;
    let style = null;
    let rule = null;
    let flip = false;
    let index = 0;

    // Returns a style element that holds the large stylesheet. The element shares the stylesheet's data with the
    // document's cache.
    function cachedStyle() {
        const element = document.head.appendChild(document.createElement("style"));
        element.textContent = input.large;
        return element;
    }

    return [
        {name: "node build detached", nodes: tree,
         setup() {},
         run() { build(document.createElement("div"), 3, 10); }},

        {name: "node build insert remove", nodes: tree,
         setup() {},
         run() { host.appendChild(build(document.createElement("div"), 3, 10)).remove(); }},

        {name: "node move detached", nodes: tree,
         setup() {
             subtree = build(document.createElement("div"), 3, 10);
             detached = [document.createElement("div"), document.createElement("div")];
         },
         run() {
             flip = !flip;
             detached[flip ? 1 : 0].appendChild(subtree);
         }},

        {name: "node move connected", nodes: tree,
         setup() { subtree = host.appendChild(build(document.createElement("div"), 3, 10)); },
         run() {
             flip = !flip;
             (flip ? other : host).appendChild(subtree);
         }},

        {name: "node walk", nodes: tree,
         setup() { subtree = host.appendChild(build(document.createElement("div"), 3, 10)); },
         run() { walk(subtree); }},

        {name: "node clone", nodes: tree,
         setup() { subtree = build(document.createElement("div"), 3, 10); },
         run() { subtree.cloneNode(true); }},

        {name: "node innerHTML", nodes: tree - 1,
         setup() {},
         run() { host.innerHTML = html; }},

        {name: "node attributes", nodes: 1,
         setup() { subtree = host.appendChild(document.createElement("div")); },
         run() {
             subtree.setAttribute("data-x", "1");
             subtree.setAttribute("title", "t");
             subtree.removeAttribute("data-x");
             subtree.removeAttribute("title");
         }},

        {name: "node range extract", nodes: tree - 1,
         setup() { subtree = host.appendChild(build(document.createElement("div"), 3, 10)); },
         run() {
             const range = document.createRange();
             range.selectNodeContents(subtree);
             subtree.appendChild(range.extractContents());
         }},

        {name: "node shadow build", nodes: 111,
         setup() {},
         run() {
             const shadow = document.createElement("div").attachShadow({mode: "open"});
             build(shadow, 2, 10);
         }},

        {name: "node textContent", nodes: tree - 1,
         setup() { host.innerHTML = html; },
         run() {
             host.textContent = "";
             host.innerHTML = html;
         }},

        {name: "sheet replaceSync", rules: 0,
         setup() {
             sheet = new CSSStyleSheet();
             sheet.replaceSync(input.small);
             this.rules = sheet.cssRules.length;
         },
         run() { sheet.replaceSync(unique(input.small)); }},

        {name: "sheet replaceSync read", rules: 0,
         setup() {
             sheet = new CSSStyleSheet();
             sheet.replaceSync(input.small);
             this.rules = sheet.cssRules.length;
         },
         run() {
             sheet.replaceSync(unique(input.small));
             const rules = sheet.cssRules;
             for (let i = 0; i < rules.length; ++i)
                 rules[i].cssText;
         }},

        {name: "sheet insert delete", rules: 1,
         setup() {
             sheet = new CSSStyleSheet();
             sheet.replaceSync(input.large);
         },
         run() {
             sheet.insertRule(".item > span:hover { color: red; margin: 1px 2px }", 0);
             sheet.deleteRule(0);
         }},

        {name: "sheet insert delete read", rules: 1,
         setup() {
             sheet = new CSSStyleSheet();
             sheet.replaceSync(input.large);
         },
         run() {
             sheet.insertRule(".item > span:hover { color: red; margin: 1px 2px }", 0);
             sheet.cssRules[0].selectorText;
             sheet.deleteRule(0);
         }},

        {name: "sheet style text", rules: 0,
         setup() {
             style = document.head.appendChild(document.createElement("style"));
             style.textContent = input.small;
             this.rules = style.sheet.cssRules.length;
         },
         run() { style.textContent = unique(input.small); }},

        {name: "sheet style insert remove", rules: 0,
         setup() {
             style = document.createElement("style");
             style.textContent = input.small;
             document.head.appendChild(style);
             this.rules = style.sheet.cssRules.length;
             style.remove();
         },
         run() {
             document.head.appendChild(style);
             style.remove();
         }},

        // Text that the cache holds gives the element's sheet shared data again, and the first change through the
        // sheet makes a private copy. The first case measures only that, as a reference for the others.
        {name: "sheet cached text", rules: 0,
         setup() { style = cachedStyle(); },
         run() { style.textContent = input.large; }},

        {name: "sheet cached edit", rules: 1,
         setup() {
             style = cachedStyle();
             const rules = style.sheet.cssRules;
             index = rules.length >> 1;
             while (rules[index].style === undefined)
                 ++index;
         },
         run() {
             style.textContent = input.large;
             style.sheet.cssRules[index].style.color = "rgb(1, 2, 3)";
         }},

        {name: "sheet cached nested edit", rules: 1,
         setup() {
             style = cachedStyle();
             const rules = style.sheet.cssRules;
             index = 0;
             while (rules[index].media === undefined
                    || rules[index].cssRules[0].style === undefined)
                 ++index;
         },
         run() {
             style.textContent = input.large;
             style.sheet.cssRules[index].cssRules[0].style.color = "rgb(1, 2, 3)";
         }},

        {name: "sheet nested insert delete", rules: 1,
         setup() {
             sheet = new CSSStyleSheet();
             sheet.replaceSync("@media print { a { color: red } }");
             rule = sheet.cssRules[0];
         },
         run() {
             rule.insertRule(".item b { margin: 1px }", 0);
             rule.cssRules[0].style.margin;
             rule.deleteRule(0);
         }},

        {name: "rule selectorText", rules: 1,
         setup() {
             sheet = new CSSStyleSheet();
             sheet.replaceSync(".item { color: red }");
             rule = sheet.cssRules[0];
         },
         run() {
             flip = !flip;
             rule.selectorText = flip ? ".item > span:hover" : ".item";
         }},

        {name: "rule cssText", rules: 1,
         setup() {
             sheet = new CSSStyleSheet();
             sheet.replaceSync(".item { color: red }");
             rule = sheet.cssRules[0];
         },
         run() {
             flip = !flip;
             rule.style.cssText = flip ? "margin: 1px 2px; color: red" : "padding: 3px; display: block";
         }},

        {name: "rule setProperty", rules: 1,
         setup() {
             sheet = new CSSStyleSheet();
             sheet.replaceSync(".item { color: red; margin: 1px 2px; display: block }");
             rule = sheet.cssRules[0];
         },
         run() {
             flip = !flip;
             rule.style.setProperty("margin", flip ? "3px 4px" : "1px 2px");
         }},

        {name: "rule setProperty large", rules: 1,
         setup() {
             sheet = new CSSStyleSheet();
             const letters = "[a b c d e f g h i j k l m n o p q r s t u v w x y z]";
             sheet.replaceSync(".item { --payload: " + Array(4).fill(letters).join(" ")
                              + "; background: linear-gradient(red, blue), "
                              + "linear-gradient(45deg, rgb(1, 2, 3) 10%, transparent 90%), "
                              + "radial-gradient(circle at 20% 30%, green, yellow 40%, blue), "
                              + "url(tile.png) repeat-x; color: red; opacity: 1 }");
             rule = sheet.cssRules[0];
         },
         run() {
             flip = !flip;
             rule.style.setProperty("opacity", flip ? "0.5" : "1");
         }},

        {name: "inline setProperty", rules: 0,
         setup() { subtree = host.appendChild(document.createElement("div")); },
         run() {
             flip = !flip;
             subtree.style.setProperty("margin", flip ? "3px 4px" : "1px 2px");
         }},
    ];
}

function benchmark(input, only) {
    const rows = [];
    for (const c of cases(input)) {
        if (only !== undefined && only.name !== c.name)
            continue;
        c.setup();
        const row = {name: c.name, nodes: c.nodes || 0, rules: c.rules || 0};
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

globalThis.__REPORT__(benchmark(globalThis.__INPUT__, globalThis.__ONLY__));
