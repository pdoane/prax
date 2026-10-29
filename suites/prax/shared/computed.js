// Fills each `.pr-computed` element with a value read back from the element whose id is `data-of`, so a page shows
// what a declaration computes to or does:
//   data-property: the computed value of that CSS property, of the element or, with data-pseudo (`::before`,
//                  `::marker`...), of its pseudo-element.
//   data-dom:      a property of the element itself, such as `scrollWidth` or `offsetHeight`.
// It reads once at once, and again once the page's fonts have loaded, since a size that comes from text changes then.
function prReadBack() {
    for (const out of document.querySelectorAll(".pr-computed")) {
        const element = document.getElementById(out.dataset.of);
        if (!element)
            out.textContent = "?";
        else if (out.dataset.dom)
            out.textContent = String(element[out.dataset.dom]);
        else
            out.textContent = getComputedStyle(element, out.dataset.pseudo || null).getPropertyValue(out.dataset.property);
    }
}

prReadBack();
if (document.fonts)
    document.fonts.ready.then(prReadBack);
