// The runner's response for /resources/testdriver-vendor.js. The shell drives a page's input through the page's view,
// as a host's input reaches it (sendInput, tools/shell/page_host.cpp): testdriver's clicks, keys and action sequences
// become the events of PrAppEvent, dispatched as tasks. Every other action fails, as testdriver.js fails an action that
// its vendor does not implement. Marking the run as automated stops testdriver.js from waiting for a person to act
// instead. The file is ASCII, as WPT's resources are, so that it reads the same in a page of any charset.
(function () {
    const internal = window.test_driver_internal;
    internal.in_automation = true;

    // The modifier flags of PrAppEvent.
    const SHIFT = 1, CONTROL = 2, ALT = 4, META = 8;

    // WebDriver's keys in the private use area (webdriver2/#keyboard-actions): the key value of each, and its code.
    const SPECIAL = {
        "\uE000": ["Unidentified", ""], "\uE001": ["Cancel", ""], "\uE002": ["Help", "Help"],
        "\uE003": ["Backspace", "Backspace"], "\uE004": ["Tab", "Tab"], "\uE005": ["Clear", "NumLock"],
        "\uE006": ["Enter", "Enter"], "\uE007": ["Enter", "NumpadEnter"], "\uE008": ["Shift", "ShiftLeft"],
        "\uE009": ["Control", "ControlLeft"], "\uE00A": ["Alt", "AltLeft"], "\uE00B": ["Pause", "Pause"],
        "\uE00C": ["Escape", "Escape"], "\uE00D": [" ", "Space"], "\uE00E": ["PageUp", "PageUp"],
        "\uE00F": ["PageDown", "PageDown"], "\uE010": ["End", "End"], "\uE011": ["Home", "Home"],
        "\uE012": ["ArrowLeft", "ArrowLeft"], "\uE013": ["ArrowUp", "ArrowUp"], "\uE014": ["ArrowRight", "ArrowRight"],
        "\uE015": ["ArrowDown", "ArrowDown"], "\uE016": ["Insert", "Insert"], "\uE017": ["Delete", "Delete"],
        "\uE018": [";", "Semicolon"], "\uE019": ["=", "Equal"], "\uE01A": ["0", "Numpad0"], "\uE01B": ["1", "Numpad1"],
        "\uE01C": ["2", "Numpad2"], "\uE01D": ["3", "Numpad3"], "\uE01E": ["4", "Numpad4"], "\uE01F": ["5", "Numpad5"],
        "\uE020": ["6", "Numpad6"], "\uE021": ["7", "Numpad7"], "\uE022": ["8", "Numpad8"], "\uE023": ["9", "Numpad9"],
        "\uE024": ["*", "NumpadMultiply"], "\uE025": ["+", "NumpadAdd"], "\uE026": [",", "NumpadComma"],
        "\uE027": ["-", "NumpadSubtract"], "\uE028": [".", "NumpadDecimal"], "\uE029": ["/", "NumpadDivide"],
        "\uE031": ["F1", "F1"], "\uE032": ["F2", "F2"], "\uE033": ["F3", "F3"], "\uE034": ["F4", "F4"],
        "\uE035": ["F5", "F5"], "\uE036": ["F6", "F6"], "\uE037": ["F7", "F7"], "\uE038": ["F8", "F8"],
        "\uE039": ["F9", "F9"], "\uE03A": ["F10", "F10"], "\uE03B": ["F11", "F11"], "\uE03C": ["F12", "F12"],
        "\uE03D": ["Meta", "MetaLeft"], "\uE050": ["Shift", "ShiftRight"], "\uE051": ["Control", "ControlRight"],
        "\uE052": ["Alt", "AltRight"], "\uE053": ["Meta", "MetaRight"], "\uE054": ["PageUp", "Numpad9"],
        "\uE055": ["PageDown", "Numpad3"], "\uE056": ["End", "Numpad1"], "\uE057": ["Home", "Numpad7"],
        "\uE058": ["ArrowLeft", "Numpad4"], "\uE059": ["ArrowUp", "Numpad8"], "\uE05A": ["ArrowRight", "Numpad6"],
        "\uE05B": ["ArrowDown", "Numpad2"], "\uE05C": ["Insert", "Numpad0"], "\uE05D": ["Delete", "NumpadDecimal"],
    };

    // The code of each printable character on a US layout, and the characters that Shift types.
    const CODES = {
        "`": "Backquote", "-": "Minus", "=": "Equal", "[": "BracketLeft", "]": "BracketRight", "\\": "Backslash",
        ";": "Semicolon", "'": "Quote", ",": "Comma", ".": "Period", "/": "Slash", " ": "Space",
    };
    const SHIFTED = {
        "~": "`", "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9", ")": "0",
        "_": "-", "+": "=", "{": "[", "}": "]", "|": "\\", ":": ";", "\"": "'", "<": ",", ">": ".", "?": "/",
    };
    const MODIFIERS = {Shift: SHIFT, Control: CONTROL, Alt: ALT, Meta: META};

    // Returns what a code point of WebDriver's key values presses: its key, its code, the text it types, and whether
    // it needs Shift.
    function keyOf(value) {
        if (value in SPECIAL) {
            const [key, code] = SPECIAL[value];
            return {key, code, text: key.length === 1 ? key : "", shifted: false};
        }
        let base = value;
        let shifted = value in SHIFTED || (value >= "A" && value <= "Z");
        if (value in SHIFTED)
            base = SHIFTED[value];
        else if (shifted)
            base = value.toLowerCase();
        let code = "";
        if (base >= "a" && base <= "z")
            code = "Key" + base.toUpperCase();
        else if (base >= "0" && base <= "9")
            code = "Digit" + base;
        else if (base in CODES)
            code = CODES[base];
        return {key: value, code, text: value, shifted};
    }

    // Appends the events of a key going down, then of its text, if it types any.
    function keyDown(events, pressed, modifiers) {
        events.push({type: "keyDown", key: pressed.key, code: pressed.code, text: pressed.text, modifiers});
        if (pressed.text && (modifiers & (CONTROL | META)) === 0)
            events.push({type: "text", text: pressed.text});
    }

    function keyUp(events, pressed, modifiers) {
        events.push({type: "keyUp", key: pressed.key, code: pressed.code, text: pressed.text, modifiers});
    }

    // An app numbers the secondary button 1 and the auxiliary button 2, and WebDriver the other way around.
    function buttonOf(button) {
        return button === 1 ? 2 : button === 2 ? 1 : button;
    }

    // The center of the element's first box in the viewport (webdriver2/#dfn-center-point).
    function centerOf(element) {
        const rect = element.getClientRects()[0] || element.getBoundingClientRect();
        const left = Math.max(0, Math.min(rect.left, rect.right));
        const right = Math.min(innerWidth, Math.max(rect.left, rect.right));
        const top = Math.max(0, Math.min(rect.top, rect.bottom));
        const bottom = Math.min(innerHeight, Math.max(rect.top, rect.bottom));
        return [Math.floor((left + right) / 2), Math.floor((top + bottom) / 2)];
    }

    // Returns the point that a pointer or wheel action moves to, from its origin.
    function pointOf(action, pointer) {
        const x = action.x || 0, y = action.y || 0;
        if (action.origin === "pointer")
            return [pointer.x + x, pointer.y + y];
        if (action.origin instanceof Element) {
            const [cx, cy] = centerOf(action.origin);
            return [cx + x, cy + y];
        }
        return [x, y];
    }

    function inThisWindow(context) {
        if (context !== null && context !== undefined && context !== window)
            throw new Error("an action in another browsing context is not implemented by testdriver-vendor.js");
    }

    internal.click = function (element, coords) {
        const x = coords.x, y = coords.y;
        return sendInput([{type: "pointerMove", x, y}, {type: "pointerDown", x, y, button: 0},
                          {type: "pointerUp", x, y, button: 0}]);
    };

    // Element Send Keys (webdriver2/#element-send-keys): focuses the element, then types each code point, holding
    // Shift for a character that needs it.
    internal.send_keys = function (element, keys) {
        if (document.activeElement !== element)
            element.focus();
        const events = [];
        let modifiers = 0;
        for (const value of keys) {
            const pressed = keyOf(value);
            if (pressed.key in MODIFIERS) {
                modifiers ^= MODIFIERS[pressed.key];
                ((modifiers & MODIFIERS[pressed.key]) ? keyDown : keyUp)(events, pressed, modifiers);
                continue;
            }
            const shift = pressed.shifted && (modifiers & SHIFT) === 0;
            if (shift)
                keyDown(events, keyOf("\uE008"), modifiers | SHIFT);
            keyDown(events, pressed, modifiers | (shift ? SHIFT : 0));
            keyUp(events, pressed, modifiers | (shift ? SHIFT : 0));
            if (shift)
                keyUp(events, keyOf("\uE008"), modifiers);
        }
        return sendInput(events);
    };

    // Perform Actions (webdriver2/#perform-actions): the sources' actions run tick by tick, each tick's events
    // dispatched together, and a tick lasts as long as its longest pause or duration.
    internal.action_sequence = async function (sources, context) {
        inThisWindow(context);
        const pointers = {};
        let modifiers = 0;
        const ticks = Math.max(0, ...sources.map(source => source.actions.length));
        for (let tick = 0; tick < ticks; ++tick) {
            const events = [];
            let duration = 0;
            for (const source of sources) {
                const action = source.actions[tick];
                if (!action)
                    continue;
                const pointer = pointers[source.id] || (pointers[source.id] = {x: 0, y: 0});
                duration = Math.max(duration, action.duration || 0);
                switch (action.type) {
                    case "keyDown": {
                        const pressed = keyOf(action.value);
                        if (pressed.key in MODIFIERS)
                            modifiers |= MODIFIERS[pressed.key];
                        keyDown(events, pressed, modifiers);
                        break;
                    }
                    case "keyUp": {
                        const pressed = keyOf(action.value);
                        if (pressed.key in MODIFIERS)
                            modifiers &= ~MODIFIERS[pressed.key];
                        keyUp(events, pressed, modifiers);
                        break;
                    }
                    case "pointerMove": {
                        [pointer.x, pointer.y] = pointOf(action, pointer);
                        events.push({type: "pointerMove", x: pointer.x, y: pointer.y, modifiers});
                        break;
                    }
                    case "pointerDown":
                    case "pointerUp":
                        events.push({type: action.type, x: pointer.x, y: pointer.y, button: buttonOf(action.button),
                                     modifiers});
                        break;
                    case "scroll": {
                        const [x, y] = pointOf(action, pointer);
                        events.push({type: "scroll", x, y, deltaX: action.deltaX || 0, deltaY: action.deltaY || 0,
                                     modifiers});
                        break;
                    }
                    case "pause":
                        break;
                    default:
                        throw new Error(action.type + " is not implemented by testdriver-vendor.js");
                }
            }
            if (events.length !== 0)
                await sendInput(events);
            if (duration > 0)
                await new Promise(resolve => setTimeout(resolve, duration));
        }
    };
})();
