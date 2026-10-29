// Measures bringing style and layout up to date after a change, the same way on every engine.
//
// Nothing here is engine specific. Each case makes a change through the DOM or the CSSOM, then reads a resolved value
// through getComputedStyle, which every engine answers with the change applied. tools/bench/measure.js describes the
// loader, which runs this file the same way in pr_shell and in a browser. `__INPUT__` holds the large stylesheet that
// the page carries, as a real site would.
//
// The page is a list of cards under one stylesheet. Each case's change reaches the stylesheet's rules in a different
// way: a class on the root, a class on a leaf, an attribute, a custom property, a structural pseudo-class, :has(), and
// an inserted rule. One case builds the page, and two cases write and read in a loop: one reads a computed value, and
// the other a box's geometry. The last cases add a sheet of many :has() anchors, as a site's own stylesheet may hold,
// and then build the page, insert and remove an element, and change a text, each of which every anchor may reach. A
// case's `nodes` is the number of elements in the page.

// The number of cards in the page, and of items in its list.
const kCards = 100;
const kItems = 50;

// The rules that the cases' changes reach.
const kRules = `
.card { display: block; margin: 4px; padding: 4px 8px; border: 1px solid rgb(200, 200, 200); max-width: 600px }
.card h2 { font-size: 15px; margin: 0 0 4px 0 }
.card p { margin: 2px 0; color: var(--text); line-height: 1.4 }
.card span { color: rgb(0, 0, 200); padding: 0 2px }
.theme-dark .card { background-color: rgb(20, 20, 20); color: rgb(240, 240, 240) }
.card.active { background-color: rgb(255, 255, 200) }
.card[data-state=open] p { font-weight: 700 }
.card:has(.flag) { border-color: rgb(200, 0, 0) }
.list li { display: block; height: 12px }
.list li:nth-child(odd) { color: rgb(120, 120, 120) }
:root { --text: rgb(51, 51, 51) }
`;

// The number of :has() rules in the sheet of anchors, and how many of them have an argument that reaches a later
// sibling.
const kAnchors = 400;
const kSiblingAnchors = 8;

// Returns the sheet of anchors. No rule matches the page, so a change costs only the work of finding that it reaches
// none of them.
function anchorRules() {
    let css = "";
    for (let i = 0; i < kAnchors - kSiblingAnchors; ++i)
        css += ".anchor" + i + ":has(.flag" + i + ") { outline: 1px solid rgb(200, 0, 0) }\n";
    for (let i = 0; i < kSiblingAnchors; ++i)
        css += ".sibling" + i + ":has(~ .flag" + i + ") { outline: 1px solid rgb(0, 0, 200) }\n";
    return css;
}

// Returns the markup of the page's content.
function markup() {
    let html = "";
    for (let i = 0; i < kCards; ++i) {
        html += '<div class="card" id="card' + i + '"><h2>Card ' + i + '</h2>'
                + '<p>Some text with <span>an inline span</span> in a paragraph of a card.</p></div>';
    }
    html += '<ul class="list">';
    for (let i = 0; i < kItems; ++i)
        html += "<li>item " + i + "</li>";
    return html + "</ul>";
}

function read(element, property) {
    return getComputedStyle(element).getPropertyValue(property);
}

function cases(input) {
    const large = document.head.appendChild(document.createElement("style"));
    large.textContent = input.large;
    const own = document.head.appendChild(document.createElement("style"));
    own.textContent = kRules;
    const host = document.body.appendChild(document.createElement("div"));
    const html = markup();
    host.innerHTML = html;
    const nodes = host.getElementsByTagName("*").length;
    const root = document.documentElement;
    const cards = host.getElementsByClassName("card");
    const middle = cards[kCards >> 1];
    const last = cards[kCards - 1];
    const lastText = last.getElementsByTagName("p")[0];
    const list = host.getElementsByTagName("ul")[0];
    const lastItem = list.lastElementChild;
    const span = middle.getElementsByTagName("span")[0];
    const extra = document.createElement("li");
    const inserted = document.createElement("em");
    let anchors = null;
    let flip = false;
    let height = 0;

    return [
        {name: "style page build", nodes,
         setup() {},
         run() {
             host.innerHTML = html;
             read(host.lastElementChild.lastElementChild, "color");
         }},

        {name: "style class on root", nodes,
         setup() { host.innerHTML = html; },
         run() {
             root.classList.toggle("theme-dark");
             read(host.getElementsByTagName("p")[kCards - 1], "color");
         }},

        {name: "style class on leaf", nodes,
         setup() {},
         run() {
             host.getElementsByClassName("card")[kCards >> 1].classList.toggle("active");
             read(host.getElementsByClassName("card")[kCards >> 1], "background-color");
         }},

        {name: "style attribute", nodes,
         setup() {},
         run() {
             flip = !flip;
             const card = host.getElementsByClassName("card")[kCards >> 1];
             card.setAttribute("data-state", flip ? "open" : "closed");
             read(card.getElementsByTagName("p")[0], "font-weight");
         }},

        {name: "style custom property", nodes,
         setup() {},
         run() {
             flip = !flip;
             root.style.setProperty("--text", flip ? "rgb(200, 0, 0)" : "rgb(51, 51, 51)");
             read(host.getElementsByTagName("p")[kCards - 1], "color");
         }},

        {name: "style nth-child insert", nodes,
         setup() {},
         run() {
             const items = host.getElementsByTagName("ul")[0];
             if (extra.parentNode === null)
                 items.insertBefore(extra, items.firstElementChild);
             else
                 extra.remove();
             read(items.lastElementChild, "color");
         }},

        {name: "style :has", nodes,
         setup() {},
         run() {
             const card = host.getElementsByClassName("card")[kCards >> 1];
             card.getElementsByTagName("span")[0].classList.toggle("flag");
             read(card, "border-top-color");
         }},

        {name: "style insertRule", nodes,
         setup() {},
         run() {
             const sheet = own.sheet;
             sheet.insertRule(".card span { color: rgb(0, 120, 0) }", sheet.cssRules.length);
             read(host.getElementsByTagName("span")[kCards >> 1], "color");
             sheet.deleteRule(sheet.cssRules.length - 1);
         }},

        {name: "style write read loop", nodes,
         setup() {},
         run() {
             const card = host.getElementsByClassName("card")[kCards - 1];
             for (let i = 0; i < 10; ++i) {
                 card.style.width = (300 + i) + "px";
                 read(card, "width");
             }
         }},

        // Writes a width, then reads a paragraph's height through CSSOM View, which lays out the paragraph's lines
        // again.
        {name: "style geometry loop", nodes,
         setup() {},
         run() {
             const card = host.getElementsByClassName("card")[kCards - 2];
             const text = card.getElementsByTagName("p")[0];
             for (let i = 0; i < 10; ++i) {
                 card.style.width = (200 + 10 * i) + "px";
                 height += text.offsetHeight;
             }
         }},

        {name: "style :has anchors page build", nodes,
         setup() {
             anchors = document.head.appendChild(document.createElement("style"));
             anchors.textContent = anchorRules();
             read(host.lastElementChild.lastElementChild, "color");
         },
         run() {
             host.innerHTML = html;
             read(host.lastElementChild.lastElementChild, "color");
         }},

        {name: "style :has anchors insert", nodes,
         setup() {},
         run() {
             const text = host.getElementsByClassName("card")[kCards >> 1].getElementsByTagName("p")[0];
             if (inserted.parentNode === null)
                 text.appendChild(inserted);
             else
                 inserted.remove();
             read(host.getElementsByTagName("p")[kCards - 1], "color");
         }},

        {name: "style :has anchors text", nodes,
         setup() {},
         run() {
             flip = !flip;
             const span = host.getElementsByClassName("card")[kCards >> 1].getElementsByTagName("span")[0];
             span.firstChild.data = flip ? "a changed span" : "an inline span";
             read(host.getElementsByTagName("p")[kCards - 1], "color");
         }},
    ];
}

function benchmark(input, only) {
    const rows = [];
    for (const c of cases(input)) {
        if (only !== undefined && only.name !== c.name)
            continue;
        c.setup();
        const row = {name: c.name, nodes: c.nodes || 0};
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
