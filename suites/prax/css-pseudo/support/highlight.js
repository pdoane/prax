// Sets the highlights that a page's figures draw, from its markup, once it has loaded. An element whose
// data-highlight attribute names one or more custom highlights adds its contents to each of them. Two empty markers,
// `<span data-from="name"></span>` and `<span data-to="name"></span>`, add the range between them, which may cross
// element boundaries. The name "selection" makes the selection instead: the document has a single one. A
// data-priority attribute on the first element of a custom highlight sets that highlight's priority.
(function () {
    const ranges = new Map();
    function add(name, range, element) {
        if (!ranges.has(name))
            ranges.set(name, { ranges: [], priority: Number(element.dataset.priority || 0) });
        ranges.get(name).ranges.push(range);
    }
    for (const element of document.querySelectorAll("[data-highlight]")) {
        for (const name of element.dataset.highlight.split(/\s+/)) {
            const range = document.createRange();
            range.selectNodeContents(element);
            add(name, range, element);
        }
    }
    for (const from of document.querySelectorAll("[data-from]")) {
        const name = from.dataset.from;
        const to = document.querySelector(`[data-to="${name}"]`);
        const range = document.createRange();
        range.setStartAfter(from);
        range.setEndBefore(to);
        add(name, range, from);
    }
    for (const [name, entry] of ranges) {
        if (name === "selection") {
            const selection = getSelection();
            selection.removeAllRanges();
            selection.addRange(entry.ranges[0]);
        } else if (window.CSS && CSS.highlights && window.Highlight) {
            const highlight = new Highlight(...entry.ranges);
            highlight.priority = entry.priority;
            CSS.highlights.set(name, highlight);
        }
    }
})();
