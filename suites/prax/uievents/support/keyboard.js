// Draws a section of a keyboard as keys named by their code values, for a figure about the code that a key reports.
// A `.kb-map` names its section in data-layout and the keys that its figure is about in data-mark (a group of the
// layout, such as "writing" or "functional"); the other keys of the section are drawn without their legends. Each key
// is drawn where UI Events KeyboardEvent code Values places it, with the US legend, and its code as its title. A keydown
// that reaches the map lights the keys whose code is the event's, until the keyup; a key once pressed stays tinted, so
// that the reader sees which keys they have tried. The map takes the focus, and the page's keydowns in it are canceled,
// so that Tab, Space and the arrows stay in the map rather than move the focus or scroll.

// A key is [code, legend, x, y, width, height, group], in units of a key's width.
function row(y, x, group, keys) {
    const out = [];
    for (const [code, legend, width = 1] of keys) {
        out.push([code, legend, x, y, width, 1, group]);
        x += width;
    }
    return out;
}

const W = "writing", F = "functional";
const ALPHANUMERIC = [
    ...row(0, 0, W, [["Backquote", "`"], ["Digit1", "1"], ["Digit2", "2"], ["Digit3", "3"], ["Digit4", "4"],
        ["Digit5", "5"], ["Digit6", "6"], ["Digit7", "7"], ["Digit8", "8"], ["Digit9", "9"], ["Digit0", "0"],
        ["Minus", "-"], ["Equal", "="], ["IntlYen", "¥"]]),
    ["Backspace", "⌫", 14, 0, 1, 1, F],
    ["Tab", "⇥", 0, 1, 1.5, 1, F],
    ...row(1, 1.5, W, [["KeyQ", "Q"], ["KeyW", "W"], ["KeyE", "E"], ["KeyR", "R"], ["KeyT", "T"], ["KeyY", "Y"],
        ["KeyU", "U"], ["KeyI", "I"], ["KeyO", "O"], ["KeyP", "P"], ["BracketLeft", "["], ["BracketRight", "]"],
        ["Backslash", "\\", 1.5]]),
    ["CapsLock", "⇪", 0, 2, 1.75, 1, F],
    ...row(2, 1.75, W, [["KeyA", "A"], ["KeyS", "S"], ["KeyD", "D"], ["KeyF", "F"], ["KeyG", "G"], ["KeyH", "H"],
        ["KeyJ", "J"], ["KeyK", "K"], ["KeyL", "L"], ["Semicolon", ";"], ["Quote", "'"], ["Backslash", "#"]]),
    ["Enter", "↵", 13.75, 2, 1.25, 1, F],
    ["ShiftLeft", "⇧", 0, 3, 1.25, 1, F],
    ...row(3, 1.25, W, [["IntlBackslash", "\\"], ["KeyZ", "Z"], ["KeyX", "X"], ["KeyC", "C"], ["KeyV", "V"],
        ["KeyB", "B"], ["KeyN", "N"], ["KeyM", "M"], ["Comma", ","], ["Period", "."], ["Slash", "/"], ["IntlRo", "ろ"]]),
    ["ShiftRight", "⇧", 13.25, 3, 1.75, 1, F],
    ...row(4, 0, F, [["ControlLeft", "⌃", 1.25], ["MetaLeft", "◆", 1.25], ["AltLeft", "⌥", 1.25],
        ["NonConvert", "無"], ["Space", "", 3.25], ["Convert", "変"], ["KanaMode", "か"], ["AltRight", "⌥", 1.25],
        ["MetaRight", "◆", 1.25], ["ContextMenu", "☰", 1.25], ["ControlRight", "⌃", 1.25]]),
];

const LAYOUTS = {
    alphanumeric: ALPHANUMERIC,
    controlpad: [
        ...row(0, 0, "controlpad", [["Insert", "Ins"], ["Home", "Home"], ["PageUp", "PgUp"]]),
        ...row(1, 0, "controlpad", [["Delete", "Del"], ["End", "End"], ["PageDown", "PgDn"]]),
        ["Help", "Help", 0, 2.25, 1, 1, "controlpad"],
    ],
    arrowpad: [
        ["ArrowUp", "↑", 1, 0, 1, 1, "arrowpad"],
        ...row(1, 0, "arrowpad", [["ArrowLeft", "←"], ["ArrowDown", "↓"], ["ArrowRight", "→"]]),
    ],
    numpad: [
        ...row(0, 0, "numpad", [["NumLock", "Num"], ["NumpadDivide", "/"], ["NumpadMultiply", "*"],
            ["NumpadSubtract", "-"]]),
        ...row(1, 0, "numpad", [["Numpad7", "7"], ["Numpad8", "8"], ["Numpad9", "9"]]),
        ["NumpadAdd", "+", 3, 1, 1, 2, "numpad"],
        ...row(2, 0, "numpad", [["Numpad4", "4"], ["Numpad5", "5"], ["Numpad6", "6"]]),
        ...row(3, 0, "numpad", [["Numpad1", "1"], ["Numpad2", "2"], ["Numpad3", "3"]]),
        ["NumpadEnter", "↵", 3, 3, 1, 2, "numpad"],
        ...row(4, 0, "numpad", [["Numpad0", "0", 2], ["NumpadDecimal", "."]]),
    ],
    function: [
        ["Escape", "Esc", 0, 0, 1, 1, "function"],
        ...row(0, 1.5, "function", [["F1", "F1"], ["F2", "F2"], ["F3", "F3"], ["F4", "F4"]]),
        ...row(0, 6, "function", [["F5", "F5"], ["F6", "F6"], ["F7", "F7"], ["F8", "F8"]]),
        ...row(0, 10.5, "function", [["F9", "F9"], ["F10", "F10"], ["F11", "F11"], ["F12", "F12"]]),
        ...row(0, 15, "function", [["PrintScreen", "PrSc"], ["ScrollLock", "ScLk"], ["Pause", "Paus"]]),
    ],
};

for (const map of document.querySelectorAll(".kb-map")) {
    const unit = Number(map.dataset.unit || 34);
    const mark = map.dataset.mark || "";
    let right = 0, bottom = 0;
    for (const [code, legend, x, y, width, height, group] of LAYOUTS[map.dataset.layout]) {
        const key = document.createElement("span");
        key.className = "kb-key" + (mark && group !== mark ? " kb-other" : "");
        key.dataset.code = code;
        key.title = code;
        key.textContent = mark && group !== mark ? "" : legend;
        key.style.left = x * unit + "px";
        key.style.top = y * unit + "px";
        key.style.width = width * unit - 3 + "px";
        key.style.height = height * unit - 3 + "px";
        key.style.lineHeight = height * unit - 5 + "px";
        map.append(key);
        right = Math.max(right, (x + width) * unit);
        bottom = Math.max(bottom, (y + height) * unit);
    }
    map.style.width = right + "px";
    map.style.height = bottom + "px";
    map.tabIndex = 0;
    map.addEventListener("keydown", event => {
        event.preventDefault();
        for (const key of map.querySelectorAll(`[data-code="${CSS.escape(event.code)}"]`))
            key.classList.add("kb-down", "kb-seen");
    });
    // A Mac sends no keyup for a key released while Command is held, so releasing Command, or leaving the map, lets go
    // of every key.
    const release = selector => {
        for (const key of map.querySelectorAll(selector))
            key.classList.remove("kb-down");
    };
    map.addEventListener("keyup", event => {
        release(event.key === "Meta" ? ".kb-down" : `[data-code="${CSS.escape(event.code)}"]`);
    });
    map.addEventListener("blur", () => release(".kb-down"));
}
