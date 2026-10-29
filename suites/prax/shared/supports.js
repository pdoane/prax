// Marks each declaration of a supports page with whether the engine parses it: `pr-yes` where CSS.supports accepts the
// property and value, as a style sheet's declaration would be kept.
for (const decl of document.querySelectorAll(".pr-decl")) {
    const text = decl.textContent;
    const colon = text.indexOf(":");
    if (CSS.supports(text.slice(0, colon).trim(), text.slice(colon + 1).trim()))
        decl.classList.add("pr-yes");
}

// Marks each selector of a supports page with whether the engine parses it: `pr-yes` where CSS.supports accepts it as a
// selector, as a style rule's selector would be kept.
for (const selector of document.querySelectorAll(".pr-selector")) {
    if (CSS.supports("selector(" + selector.textContent.trim() + ")"))
        selector.classList.add("pr-yes");
}

// Marks each interface or member of a supports page with whether the engine exposes it: `pr-yes` where the global
// object has the interface, and the interface's prototype has the member, as `KeyboardEvent.prototype.code` names it.
for (const api of document.querySelectorAll(".pr-api")) {
    const [name, , member] = api.textContent.trim().split(".");
    const type = window[name];
    if (type !== undefined && (member === undefined || member in type.prototype))
        api.classList.add("pr-yes");
}
