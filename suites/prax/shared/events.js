// Logs the events that reach an element, for a figure that asks the reader to act. A `.pr-log` lists, newest last, the
// events whose types its data-log names as they reach the element that its data-on selects in its figure, the document
// for data-on="document", or else the figure's stage. Each line is the event's type and the fields that data-fields
// names: a true boolean by its name, an element by its id, and any other value as name=value. A log keeps the last
// lines that data-lines gives, 6 by default.
//
// Its listeners capture and are passive, so they change neither which element sees an event first nor whether it can
// be canceled. Each field is read as the event reaches the log, except defaultPrevented, which is read once the
// dispatch has ended, after the page's own listeners. A task reads it, because an event that the browser fires runs
// microtasks between its listeners.
function describe(event, field) {
    const value = event[field];
    if (value === undefined || value === false || value === "")
        return "";
    if (value === true)
        return field;
    if (value instanceof Element)
        return field + "=" + (value.id || value.localName);
    if (typeof value === "string")
        return field + "=" + JSON.stringify(value);
    if (typeof value === "number")
        return field + "=" + Math.round(value * 100) / 100;
    return field + "=" + value;
}

for (const log of document.querySelectorAll(".pr-log")) {
    const figure = log.closest("figure");
    const on = log.dataset.on;
    const target = on === "document" ? document : on ? figure.querySelector(on) : figure.querySelector(".pr-stage");
    const fields = (log.dataset.fields || "").split(/\s+/).filter(Boolean);
    const lines = Number(log.dataset.lines || 6);
    const prevented = fields.indexOf("defaultPrevented");
    log.style.setProperty("--pr-log-lines", lines);
    for (const type of log.dataset.log.split(/\s+/)) {
        target.addEventListener(type, event => {
            const item = document.createElement("li");
            const parts = [type, ...fields.map(field => describe(event, field))];
            item.textContent = parts.filter(Boolean).join(" ");
            log.append(item);
            while (log.children.length > lines)
                log.firstElementChild.remove();
            if (prevented >= 0) {
                setTimeout(() => {
                    parts[prevented + 1] = describe(event, "defaultPrevented");
                    item.textContent = parts.filter(Boolean).join(" ");
                });
            }
        }, {capture: true, passive: true});
    }
}
