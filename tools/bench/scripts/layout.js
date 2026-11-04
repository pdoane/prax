// Measures laying out a document, from scratch and after the kinds of changes that a page makes, the same way on every
// engine.
//
// Nothing here is engine specific. tools/bench/measure.js describes the loader, which runs this file the same way in
// pr_shell and in a browser.
//
// The documents are fair to compare. Each uses only features that the engine lays out (blocks, inline boxes,
// inline-blocks, floats, margins, relative and absolute positioning, flex containers, aspect-ratio and MathML) in Arimo
// and New Computer Modern Math, fonts that both engines have. They are generated from a fixed seed, so every engine and
// every run lays out the same text.
// A case is a change followed by a geometry read, which brings style and layout up to date. Painting is not measured,
// because a browser does not expose it.

// Returns a generator of numbers in [0, 1) from a fixed seed, so that every run makes the same documents.
function random(seed) {
    let state = seed >>> 0;
    return function () {
        state = (state + 0x6d2b79f5) >>> 0;
        let t = state;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

const kWords = ("layout block inline float margin padding border line box text width height offset context " +
                "paragraph sentence document engine browser render style cascade selector element node tree " +
                "flow formatting baseline glyph font advance break wrap clear side edge content area").split(" ");

// Returns text of `count` words. If `inline` is set, some of the words are emphasized or linked.
function words(next, count, inline) {
    const out = [];
    for (let i = 0; i < count; ++i) {
        const word = kWords[Math.floor(next() * kWords.length)];
        const pick = next();
        if (inline && pick < 0.04)
            out.push("<em>" + word + "</em>");
        else if (inline && pick < 0.08)
            out.push('<a href="#">' + word + "</a>");
        else if (inline && pick < 0.10)
            out.push('<span class="tag">' + word + "</span>");
        else
            out.push(word);
    }
    return out.join(" ");
}

// The documents. Each has its rules, its markup, and a selector for the elements that its cases change. The cases
// change the middle element that the selector matches.
const kDocuments = [
    {name: "article", target: "p",
     style: "h2 { font-size: 22px; margin: 24px 0 8px } p { margin: 0 0 12px }",
     markup(next) {
         let html = "";
         for (let i = 0; i < 300; ++i) {
             if (i % 10 === 0)
                 html += "<h2>" + words(next, 4, false) + "</h2>";
             html += "<p>" + words(next, 40 + Math.floor(next() * 80), true) + "</p>";
         }
         return html;
     }},

    {name: "cards", target: ".card",
     style: ".card { margin: 4px; padding: 4px 8px; border: 1px solid #ccc; max-width: 600px }" +
            ".card h3 { font-size: 15px; margin: 0 0 4px } .card p { margin: 2px 0 }",
     markup(next) {
         let html = "";
         for (let i = 0; i < 1000; ++i)
             html += '<div class="card"><h3>' + words(next, 3, false) + "</h3><p>" + words(next, 24, true) +
                     "</p></div>";
         return html;
     }},

    {name: "floats", target: "p",
     style: "p { margin: 0 0 12px } h2 { clear: both; font-size: 22px; margin: 24px 0 8px }" +
            ".figure { margin: 4px 12px; background: #eee; border: 1px solid #999 }" +
            ".left { float: left } .right { float: right }" +
            ".sidebar { float: right; width: 200px; height: 2400px; margin-left: 16px; background: #f4f4f4 }",
     markup(next) {
         let html = '<div class="sidebar">' + words(next, 60, false) + "</div>";
         for (let i = 0; i < 200; ++i) {
             if (i % 12 === 0)
                 html += "<h2>" + words(next, 4, false) + "</h2>";
             if (i % 3 === 0) {
                 const width = 100 + Math.floor(next() * 100);
                 const height = 60 + Math.floor(next() * 90);
                 html += '<div class="figure ' + (i % 6 === 0 ? "left" : "right") + '" style="width: ' + width +
                         "px; height: " + height + 'px"></div>';
             }
             html += "<p>" + words(next, 40 + Math.floor(next() * 60), true) + "</p>";
         }
         return html;
     }},

    {name: "lists", target: "li",
     style: "ul { margin: 4px 0; padding-left: 24px } li { margin: 2px 0 }",
     markup(next) {
         function list(depth) {
             let html = "<ul>";
             const count = depth === 0 ? 30 : 7;
             for (let i = 0; i < count; ++i)
                 html += "<li>" + words(next, 6 + Math.floor(next() * 10), true) + (depth < 2 ? list(depth + 1) : "") +
                         "</li>";
             return html + "</ul>";
         }
         return list(0);
     }},

    {name: "deep", target: ".leaf",
     style: ".level { margin: 1px 0 1px 2px; padding: 1px 0 }",
     markup(next) {
         let html = "";
         for (let tree = 0; tree < 60; ++tree) {
             let open = "";
             let close = "";
             for (let depth = 0; depth < 30; ++depth) {
                 open += '<div class="level">';
                 close += "</div>";
             }
             html += open + '<div class="leaf">' + words(next, 12, true) + "</div>" + close;
         }
         return html;
     }},

    // Deep chains of static blocks under a positioned stage. Each leaf holds absolutely positioned boxes, both
    // inline-level and block-level, some at their static positions and some at insets. The boxes' placeholders are far
    // below their containing block.
    {name: "positioned", target: ".leaf",
     style: "#stage { position: relative } .level { margin: 1px 0 1px 2px; padding: 1px 0 }" +
            ".pin { position: absolute; width: 8px; height: 8px; background: #c60 }",
     markup(next) {
         let html = "";
         for (let tree = 0; tree < 60; ++tree) {
             let open = "";
             let close = "";
             for (let depth = 0; depth < 30; ++depth) {
                 open += '<div class="level">';
                 close += "</div>";
             }
             let leaf = "";
             for (let i = 0; i < 4; ++i)
                 leaf += words(next, 3, true) + '<span class="pin"></span>';
             leaf += '<div class="pin" style="top: ' + tree * 10 + 'px; right: 0"></div>';
             html += open + '<div class="leaf">' + leaf + "</div>" + close;
         }
         return html;
     }},

    // Blocks, each followed by a long stretch of absolutely positioned siblings, among block-level children. Measures
    // the cost of deciding whether a stretch of out-of-flow boxes starts a run of inline content.
    {name: "positioned-wide", target: ".row",
     style: "#stage { position: relative } .row { margin: 0 }" +
            ".pin { position: absolute; width: 4px; height: 4px; background: #c60 }",
     markup(next) {
         let html = "";
         for (let row = 0; row < 20; ++row) {
             html += '<div class="row">' + words(next, 6, false) + "</div>";
             for (let i = 0; i < 250; ++i)
                 html += '<div class="pin" style="top: ' + row * 4 + "px; left: " + i * 3 + 'px"></div>';
         }
         return html;
     }},

    // Positioned cards at z-index auto, each with an absolutely positioned icon. A few cards are raised by a z-index,
    // and some are deep below the stage. Faded sections are static stacking contexts that hold badges whose containing
    // block is the stage. Measures the cost of finding the stage's stack levels and painting its stacked passes if most
    // positioned boxes are at z-index auto.
    {name: "stacked", target: ".card",
     style: "#stage { position: relative; z-index: 0 } .card { position: relative; margin: 4px; padding: 4px 8px }" +
            ".icon { position: absolute; right: 0; top: 0; width: 8px; height: 8px; background: #c60 }" +
            ".raised { z-index: 1 } .fade { opacity: 0.9 } .level { margin: 1px 0 1px 2px }" +
            ".badge { position: absolute; width: 6px; height: 6px; background: #06c }",
     markup(next) {
         let html = "";
         for (let i = 0; i < 300; ++i) {
             const raised = i % 50 === 0 ? " raised" : "";
             let card = '<div class="card' + raised + '">' + words(next, 16, true) + '<span class="icon"></span></div>';
             if (i % 30 === 0) {
                 let open = "";
                 let close = "";
                 for (let depth = 0; depth < 20; ++depth) {
                     open += '<div class="level">';
                     close += "</div>";
                 }
                 card = open + card + close;
             }
             if (i % 10 === 0)
                 card = '<div class="fade">' + card + '<span class="badge" style="z-index: ' + (i % 20 === 0 ? 2 : -1) +
                        '"></span></div>';
             html += card;
         }
         return html;
     }},

    // Absolutely positioned siblings, each at its own z-index level. Measures the cost of sorting a stacking context's
    // list.
    {name: "levels", target: ".at",
     style: "#stage { position: relative; z-index: 0; height: 400px } .at { position: absolute; width: 20px;" +
            " height: 20px }",
     markup(next) {
         let html = "";
         for (let i = 0; i < 1000; ++i)
             html += '<div class="at" style="left: ' + (i % 40) * 10 + "px; top: " + Math.floor(i / 40) * 10 +
                     "px; z-index: " + Math.floor(next() * 1000) + '">' + words(next, 1, false) + "</div>";
         return html;
     }},

    // An application shell: a header of links, and a sidebar column beside a main column of cards. Each card is a row
    // of a fixed square avatar, a column of text that takes the remaining space, and a button that its margin pushes
    // to the end.
    {name: "flex-app", target: ".button",
     style: ".app { display: flex; flex-direction: column } .bar { display: flex; gap: 12px; padding: 8px }" +
            ".body { display: flex; align-items: flex-start } .side { display: flex; flex-direction: column;" +
            " flex: 0 0 160px; gap: 4px } .main { display: flex; flex-direction: column; flex: 1; min-width: 0;" +
            " gap: 6px } .card { display: flex; align-items: center; gap: 8px; padding: 6px; border: 1px solid #ccc }" +
            ".avatar { flex: none; width: 40px; height: 40px; background: #9ab } .text { flex: 1; min-width: 0 }" +
            ".text h3 { font-size: 15px; margin: 0 } .text p { margin: 2px 0 0 } .button { margin-left: auto;" +
            " flex: none; padding: 2px 8px; border: 1px solid #999 }",
     markup(next) {
         let html = '<div class="app"><div class="bar">';
         for (let i = 0; i < 8; ++i)
             html += "<span>" + words(next, 1, false) + "</span>";
         html += '</div><div class="body"><div class="side">';
         for (let i = 0; i < 30; ++i)
             html += "<div>" + words(next, 2, false) + "</div>";
         html += '</div><div class="main">';
         for (let i = 0; i < 400; ++i)
             html += '<div class="card"><div class="avatar"></div><div class="text"><h3>' + words(next, 3, false) +
                     "</h3><p>" + words(next, 10 + Math.floor(next() * 20), true) + '</p></div><span class="button">' +
                     words(next, 1, false) + "</span></div>";
         return html + "</div></div></div>";
     }},

    // A gallery: tiles of varied widths in wrapping rows, with gaps between tiles and between lines. Each tile is a
    // column of a thumbnail, sized by its aspect ratio, and a caption.
    {name: "flex-wrap", target: ".tile p",
     style: ".gallery { display: flex; flex-wrap: wrap; gap: 8px 6px; align-content: flex-start }" +
            ".tile { display: flex; flex-direction: column; flex: 1 1 auto }" +
            ".thumb { aspect-ratio: 4 / 3; background: #bca } .tile p { margin: 2px 0; font-size: 13px }",
     markup(next) {
         let html = '<div class="gallery">';
         for (let i = 0; i < 600; ++i)
             html += '<div class="tile" style="width: ' + (80 + Math.floor(next() * 120)) + 'px"><div class="thumb">' +
                     '</div><p>' + words(next, 4 + Math.floor(next() * 6), false) + "</p></div>";
         return html + "</div>";
     }},

    // Rows and columns that alternate twelve levels deep. Each level holds two growing flex containers, and the leaves
    // are text. Measures the cost of measuring an item in each axis if every level's size depends on the level below.
    {name: "flex-nested", target: ".leaf",
     style: ".row { display: flex; gap: 2px } .column { display: flex; flex-direction: column; gap: 2px }" +
            ".row > *, .column > * { flex: 1 1 auto } .leaf { padding: 1px }",
     markup(next) {
         function level(depth) {
             if (depth === 12)
                 return '<div class="leaf">' + words(next, 2 + Math.floor(next() * 4), false) + "</div>";
             const kind = depth % 2 === 0 ? "row" : "column";
             return '<div class="' + kind + '">' + level(depth + 1) + '<div class="leaf">' +
                    words(next, 3, false) + "</div>" + (depth > 8 ? level(depth + 1) : "") + "</div>";
         }
         let html = "";
         for (let tree = 0; tree < 40; ++tree)
             html += level(0);
         return html;
     }},

    // Rows of items in several font sizes, aligned by their baselines and spaced with space-between. One item in each
    // row is pushed away by an auto margin, each row is a centered line, and items hold positioned badges. Measures
    // alignment, baselines and static positions.
    {name: "flex-aligned", target: ".far",
     style: ".row { display: flex; align-items: baseline; justify-content: space-between; padding: 2px 4px;" +
            " border-bottom: 1px solid #ddd } .row > span { position: relative } .big { font-size: 24px }" +
            ".small { font-size: 12px } .far { margin-left: auto } .line { display: flex; align-items: center;" +
            " min-height: 48px } .badge { position: absolute; top: -4px; right: -6px; width: 8px; height: 8px;" +
            " background: #c33 } .pinned { position: absolute; width: 6px; height: 6px; background: #36c }",
     markup(next) {
         let html = "";
         for (let i = 0; i < 400; ++i) {
             html += '<div class="line"><div class="row" style="flex: 1">' +
                     '<span class="big">' + words(next, 2, false) + '<i class="badge"></i></span>' +
                     "<span>" + words(next, 4, true) + "</span>" +
                     '<span class="small">' + words(next, 3, false) + "</span>" +
                     '<span class="far">' + words(next, 1, false) + '</span><i class="pinned"></i></div></div>';
         }
         return html;
     }},

    // A formula of one long table, a row of a sum, a fraction and a power each. Measures the cost of a change in one
    // cell, which must not grow with the table's rows, and of a change to an attribute of the table that its cells
    // read.
    {name: "math-table", target: "mn",
     style: "math { font-family: NewComputerModernMath }",
     markup(next) {
         let html = '<math display="block"><mtable>';
         for (let i = 0; i < 1000; ++i)
             html += "<mtr><mtd><mi>" + words(next, 1, false) + "</mi><mo>+</mo><mn>" + i + "</mn></mtd><mtd><mfrac>" +
                     "<mn>1</mn><mi>x</mi></mfrac></mtd><mtd><msup><mi>y</mi><mn>2</mn></msup></mtd></mtr>";
         return html + "</mtable></math>";
     },
     cases(stage, read) {
         const table = stage.querySelector("mtable");
         let flip = false;
         return [
             {name: "columnalign", run() {
                 flip = !flip;
                 table.setAttribute("columnalign", flip ? "left" : "center");
                 read();
             }},
         ];
     }},

    {name: "wide", target: "div",
     style: "div { margin: 0 }",
     markup(next) {
         let html = "";
         for (let i = 0; i < 5000; ++i)
             html += "<div>" + words(next, 4, false) + "</div>";
         return html;
     }},

    {name: "paragraph", target: "span",
     style: "p { margin: 0 }",
     markup(next) {
         return "<p>" + words(next, 6000, true) + "</p>";
     }},
];

// The widths of the stage that each document is laid out in: its normal width, and the width that a resize alternates
// with.
const kWidth = "800px";
const kNarrowWidth = "640px";

// Returns the cases for a document: the changes that every document takes, and those of its own. `stage` holds the
// document, and a geometry read of `marker` brings layout up to date.
function cases(doc, stage, marker, target) {
    const read = () => marker.getBoundingClientRect().top;
    const changed = target.firstChild && target.firstChild.nodeType === 3 ? target.firstChild : null;
    const original = changed ? changed.data : "";
    const other = original.split("").reverse().join("");
    const inserted = document.createElement("div");
    inserted.textContent = "An inserted block, with a sentence of text in it that wraps as the paragraphs do.";
    const floated = document.createElement("div");
    floated.style.cssText = "float: left; width: 100px; height: 40px";
    let flip = false;
    return [
        {name: "rebuild", run() {
            stage.style.display = "none";
            read();
            stage.style.display = "";
            read();
        }},
        {name: "resize", run() {
            flip = !flip;
            stage.style.width = flip ? kNarrowWidth : kWidth;
            read();
        }},
        {name: "text", run() {
            flip = !flip;
            if (changed)
                changed.data = flip ? other : original;
            read();
        }},
        {name: "style", run() {
            flip = !flip;
            target.style.paddingLeft = flip ? "1px" : "";
            read();
        }},
        {name: "insert", run() {
            if (inserted.parentNode)
                inserted.remove();
            else
                target.parentNode.insertBefore(inserted, target);
            read();
        }},
        {name: "float", run() {
            if (floated.parentNode)
                floated.remove();
            else
                target.insertBefore(floated, target.firstChild);
            read();
        }},
    ].concat(doc.cases ? doc.cases(stage, read) : []);
}

function benchmark(only) {
    const rows = [];
    for (const doc of kDocuments) {
        const style = document.head.appendChild(document.createElement("style"));
        // The page holds nothing else while the document is on it, so the document's rules need no scope.
        style.textContent = "#stage { font: 16px/1.4 Arimo, sans-serif; width: " + kWidth + " }" +
                            " .tag { display: inline-block; padding: 0 2px } " + doc.style;
        const stage = document.body.appendChild(document.createElement("div"));
        stage.id = "stage";
        stage.innerHTML = doc.markup(random(1));
        const marker = document.body.appendChild(document.createElement("div"));
        const targets = stage.querySelectorAll(doc.target);
        const target = targets[targets.length >> 1];
        const nodes = stage.getElementsByTagName("*").length;
        // The height of the laid-out document, which shows that two engines laid out the same lines.
        const height = Math.round(stage.getBoundingClientRect().height);
        for (const c of cases(doc, stage, marker, target)) {
            const name = doc.name + " " + c.name;
            if (only !== undefined && only.name !== name)
                continue;
            const row = {name: name, nodes: nodes, height: height};
            if (only !== undefined) {
                row.time = only.runs !== 0 ? elapsed(() => c.run(), only.runs) / only.runs : 0;
                row.runs = only.runs;
            } else {
                Object.assign(row, measuredFields({time: measure(() => c.run())}));
            }
            rows.push(row);
        }
        stage.remove();
        marker.remove();
        style.remove();
    }

    return rows;
}

globalThis.__REPORT__(benchmark(globalThis.__ONLY__));
