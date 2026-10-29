// Logs the drag-and-drop events that reach a figure's stage, as shared/events.js does, for a `.pr-log` with the class
// `drag-log` instead of data-log: its data-log names the event types, and its data-fields may name a field of the
// event's DataTransfer as `dataTransfer.dropEffect`. Its listeners capture and are passive.
function dragField(event, field) {
    const value = field.split(".").reduce((object, key) => object == null ? undefined : object[key], event);
    const name = field.replace(/^dataTransfer\./, "");
    if (value === undefined || value === null || value === false || value === "")
        return "";
    if (value === true)
        return name;
    if (value instanceof Element)
        return name + "=" + (value.id || value.localName);
    if (value instanceof Node)
        return name + "=" + value.nodeName;
    return name + "=" + value;
}

for (const log of document.querySelectorAll(".drag-log")) {
    const figure = log.closest("figure");
    const on = log.dataset.on;
    const target = on === "document" ? document : on ? figure.querySelector(on) : figure.querySelector(".pr-stage");
    const fields = (log.dataset.fields || "").split(/\s+/).filter(Boolean);
    const lines = Number(log.dataset.lines || 6);
    log.style.setProperty("--pr-log-lines", lines);
    let last = "";
    for (const type of log.dataset.log.split(/\s+/)) {
        target.addEventListener(type, event => {
            const text = [type, ...fields.map(field => dragField(event, field))].filter(Boolean).join(" ");
            // drag and dragover repeat while the pointer moves: a repeat of the last line is not logged again.
            if (text === last && (type === "drag" || type === "dragover"))
                return;
            last = text;
            const item = document.createElement("li");
            item.textContent = text;
            log.append(item);
            while (log.children.length > lines)
                log.firstElementChild.remove();
        }, {capture: true, passive: true});
    }
}
