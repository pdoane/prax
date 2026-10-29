// Measures parsing a page with DOMParser, over the pages that `__INPUT__` holds by name.
//
// Nothing here is engine specific. tools/bench/measure.js describes the loader, which runs this file the same way in
// pr_shell and in a browser. lib/html/tests/html_bench.cpp parses the same pages through the C API.

// Returns the number of nodes in the tree, which shows that the two sides built the same tree. It is counted outside
// the timed region, because only the parse is being measured.
function countNodes(node) {
    let count = 1;
    for (let child = node.firstChild; child !== null; child = child.nextSibling)
        count += countNodes(child);
    return count;
}

function row(name, text) {
    const parser = new DOMParser();
    const parse = measure(() => parser.parseFromString(text, "text/html"));
    return Object.assign({name, nodes: countNodes(parser.parseFromString(text, "text/html"))}, measuredFields({parse}));
}

function benchmark(pages) {
    const rows = [];
    for (const [name, text] of Object.entries(pages))
        rows.push(row(name, text));

    return rows;
}

globalThis.__REPORT__(benchmark(globalThis.__INPUT__));
