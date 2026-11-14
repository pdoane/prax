// The page's side of the web application backend, which Emscripten links into the program (--js-library). Text arrives
// as a view, its pointer and its size (web_string.js).

addToLibrary({
    prAppWebSetTitle__deps: ["$prWebString"],
    prAppWebSetTitle: function(title, titleSize) {
        document.title = prWebString(title, titleSize);
    },

    prAppWebSetCursor__deps: ["$prWebString"],
    prAppWebSetCursor: function(selector, selectorSize, cursor, cursorSize) {
        const canvas = document.querySelector(prWebString(selector, selectorSize));
        if (canvas !== null)
            canvas.style.cursor = prWebString(cursor, cursorSize);
    },

    prAppWebOpenUrl__deps: ["$prWebString"],
    prAppWebOpenUrl: function(url, urlSize) {
        window.open(prWebString(url, urlSize), "_blank", "noopener");
    },

    prAppWebClipboardWrite__deps: ["$prWebString"],
    prAppWebClipboardWrite: function(text, textSize) {
        if (navigator.clipboard)
            navigator.clipboard.writeText(prWebString(text, textSize)).catch(() => {});
    },

    // Writes `width` by `height` straight-alpha sRGB Rgba8 pixels to the clipboard as a PNG, and returns 0, or 2 if the
    // page cannot write images. The pixels are copied before the call returns, and the write completes later.
    prAppWebClipboardWriteImage: function(pixels, width, height) {
        const clipboard = navigator.clipboard;
        if (!clipboard || !clipboard.write || typeof ClipboardItem === "undefined" ||
                typeof OffscreenCanvas === "undefined")
            return 2;
        const rows = new Uint8ClampedArray(HEAPU8.subarray(pixels, pixels + width * height * 4));
        const canvas = new OffscreenCanvas(width, height);
        canvas.getContext("2d").putImageData(new ImageData(rows, width, height), 0, 0);
        const item = new ClipboardItem({"image/png": canvas.convertToBlob({type: "image/png"})});
        clipboard.write([item]).catch(() => {});
        return 0;
    },

    // Reads the clipboard's text, or its first image, and answers the read `id` from the page's event loop. A refusal
    // answers 1, and a page without the API, or a failure to read, answers 2.
    prAppWebClipboardRead__deps: ["$stringToNewUTF8", "malloc", "free", "prAppWebClipboardTextRead",
        "prAppWebClipboardImageRead"],
    prAppWebClipboardRead: function(id, image) {
        const refusal = (error) => error && error.name === "NotAllowedError" ? 1 : 2;
        const clipboard = navigator.clipboard;
        if (!image) {
            const answer = (result, text) => {
                const bytes = text !== null ? stringToNewUTF8(text) : 0;
                _prAppWebClipboardTextRead(id, result, bytes);
                if (bytes !== 0)
                    _free(bytes);
            };
            if (!clipboard || !clipboard.readText) {
                setTimeout(() => answer(2, null), 0);
                return;
            }
            clipboard.readText().then((text) => answer(0, text), (error) => answer(refusal(error), null));
            return;
        }
        const answer = (result, pixels) => {
            if (pixels === null) {
                _prAppWebClipboardImageRead(id, result, 0, 0, 0);
                return;
            }
            const bytes = _malloc(pixels.data.length);
            HEAPU8.set(pixels.data, bytes);
            _prAppWebClipboardImageRead(id, result, bytes, pixels.width, pixels.height);
            _free(bytes);
        };
        if (!clipboard || !clipboard.read || typeof createImageBitmap === "undefined") {
            setTimeout(() => answer(2, null), 0);
            return;
        }
        // A 2D canvas returns straight-alpha sRGB pixels from getImageData.
        const decode = async () => {
            for (const item of await clipboard.read()) {
                const type = item.types.find((name) => name.startsWith("image/"));
                if (type === undefined)
                    continue;
                const bitmap = await createImageBitmap(await item.getType(type));
                const canvas = new OffscreenCanvas(bitmap.width, bitmap.height);
                const context = canvas.getContext("2d");
                context.drawImage(bitmap, 0, 0);
                bitmap.close();
                return context.getImageData(0, 0, canvas.width, canvas.height);
            }
            return null;
        };
        decode().then((pixels) => answer(0, pixels), (error) => answer(refusal(error), null));
    },

    // The windows that the backend made, by id: each one's frame, canvas, title bar, resize borders and the layer that
    // blocks the windows below a modal dialog.
    $prAppWebWindows: {},

    // The height of the title bar that the page draws, in CSS px, which matches kTitleBarHeight in app_web.cpp.
    $prAppWebTitleBarHeight: 30,

    // Adds the page's rules for the windows' frames and title bars, once. The title bar follows the user's color
    // scheme.
    $prAppWebAddStyles: function() {
        if (document.getElementById("prax-window-styles") !== null)
            return;
        const style = document.createElement("style");
        style.id = "prax-window-styles";
        style.textContent = [
            ".prax-frame { position: absolute; display: none; flex-direction: column; box-sizing: border-box;",
            "  box-shadow: 0 8px 28px rgba(0, 0, 0, 0.35); }",
            ".prax-frame.prax-fill { left: 0 !important; top: 0 !important; width: 100% !important;",
            "  height: 100% !important; box-shadow: none; }",
            ".prax-canvas { display: block; flex: 1 1 auto; min-height: 0; width: 100%; outline: none;",
            "  touch-action: none; }",
            ".prax-title-bar { flex: 0 0 30px; display: flex; align-items: center; padding-left: 12px;",
            "  background: #e6e6e6; color: #222; font: 13px system-ui, sans-serif; user-select: none; }",
            "@media (prefers-color-scheme: dark) { .prax-title-bar { background: #2d2d2d; color: #ddd; } }",
            ".prax-title { flex: 1; overflow: hidden; white-space: nowrap; text-overflow: ellipsis; }",
            ".prax-button { width: 46px; height: 30px; border: 0; background: transparent; color: inherit;",
            "  font: inherit; }",
            ".prax-button:hover { background: rgba(128, 128, 128, 0.25); }",
            ".prax-button.prax-close:hover { background: #c42b1c; color: #fff; }",
            ".prax-border { position: absolute; }",
            ".prax-frame.prax-sheet { border-radius: 10px; overflow: hidden;",
            "  box-shadow: 0 12px 40px rgba(0, 0, 0, 0.45); }",
            ".prax-shelf { position: absolute; left: 8px; bottom: 8px; display: flex; gap: 6px; z-index: 2147483647; }",
            ".prax-chip { max-width: 180px; padding: 6px 12px; border: 0; border-radius: 6px; background: #e6e6e6;",
            "  color: #222; font: 13px system-ui, sans-serif; overflow: hidden; white-space: nowrap;",
            "  text-overflow: ellipsis; box-shadow: 0 2px 8px rgba(0, 0, 0, 0.35); }",
            "@media (prefers-color-scheme: dark) { .prax-chip { background: #2d2d2d; color: #ddd; } }",
            ".prax-blocker { position: absolute; left: 0; top: 0; width: 100%; height: 100%; display: none;",
            "  background: rgba(0, 0, 0, 0.2); z-index: 1; }",
        ].join("\n");
        document.head.appendChild(style);
    },

    // Starts or stops watching the host: a change of its extent calls prAppWebDisplaysChanged, a press outside every
    // window calls prAppWebPressOutside, and a change of the canvas that locks the pointer calls
    // prAppWebPointerLockChanged. Returns false if the page has no element that `selector` names. A host that the page
    // does not position becomes the frames' containing block.
    $prAppWebHostListeners: null,
    prAppWebListenForHost__deps: ["$prWebString", "$prAppWebHostListeners", "$prAppWebAddStyles",
        "prAppWebDisplaysChanged", "prAppWebPressOutside", "prAppWebPointerLockChanged"],
    prAppWebListenForHost: function(selector, selectorSize, listen) {
        const listeners = prAppWebHostListeners;
        if (listeners !== null) {
            listeners.observer.disconnect();
            document.removeEventListener("pointerdown", listeners.press, true);
            document.removeEventListener("pointerlockchange", listeners.lock);
        }
        prAppWebHostListeners = null;
        const host = document.querySelector(prWebString(selector, selectorSize));
        if (host === null)
            return false;
        if (!listen)
            return true;
        prAppWebAddStyles();
        if (host !== document.body && getComputedStyle(host).position === "static")
            host.style.position = "relative";
        const observer = new ResizeObserver(() => _prAppWebDisplaysChanged());
        observer.observe(host);
        const press = (event) => {
            if (!(event.target instanceof Element) || event.target.closest(".prax-frame") === null)
                _prAppWebPressOutside();
        };
        const lock = () => {
            const element = document.pointerLockElement;
            const locked = element !== null && element.dataset.praxWindow ? Number(element.dataset.praxWindow) : 0;
            _prAppWebPointerLockChanged(locked);
        };
        document.addEventListener("pointerdown", press, true);
        document.addEventListener("pointerlockchange", lock);
        prAppWebHostListeners = {observer, press, lock};
        return true;
    },

    // Writes the host's extent in CSS px to `out`, as two doubles. The body as the host is the viewport.
    prAppWebHostExtent__deps: ["$prWebString"],
    prAppWebHostExtent: function(selector, selectorSize, out) {
        const host = document.querySelector(prWebString(selector, selectorSize));
        let width = 0;
        let height = 0;
        if (host === document.body) {
            width = document.documentElement.clientWidth;
            height = document.documentElement.clientHeight;
        } else if (host !== null) {
            width = host.clientWidth;
            height = host.clientHeight;
        }
        HEAPF64[out >> 3] = width;
        HEAPF64[(out >> 3) + 1] = height;
    },

    // Makes the frame of the window `id` in the host, with its canvas, whose id is prax-window-<id>, and a title bar of
    // `title` if `systemTitleBar`. A window with a `sheetParent` is modal over that window, which a layer in its frame
    // dims and blocks while the window shows, and has a sheet's rounded frame if `sheetFramed`. The frame shows once
    // the backend places it. Returns false if the page has no host.
    //
    // The canvas delivers its pointer events, its wheel, the drags over it, Safari's gestures and its focus. A press on
    // the canvas captures its pointer, so that the pointer's moves and its release outside the canvas arrive, and
    // cancels the page's own response, such as selecting text or making mouse events of a touch. The canvas takes
    // touches whole, so that they do not scroll or zoom the page. A button that the mouse presses or releases while
    // another is down comes as a move whose button is set (pointerevents3/#chorded-button-interactions), and is
    // delivered as a press or a release. Each pointer event carries the pointer's movement (pointerlock-2), which goes
    // on while the canvas locks the pointer.
    //
    // A drag over the canvas asks the program for its answer, which the page gives the browser as the drop effect. A
    // drop delivers its text immediately, and keeps its files as a set for the program to read. Safari's gesture events
    // each deliver a magnify and a rotate gesture, with the change since the last, as the events carry the scale and
    // the rotation since the gesture began.
    prAppWebWindowCreate__deps: ["$prWebString", "$stringToNewUTF8", "free", "$prAppWebWindows",
        "$prAppWebTitleBarHeight", "$prAppWebAddStyles", "$prAppWebFileSetAdd", "prAppWebPointer", "prAppWebWheel",
        "prAppWebWindowFocused", "prAppWebFrameDrag", "prAppWebFrameButton", "prAppWebDrag", "prAppWebDropAnswer",
        "prAppWebDrop", "prAppWebGesture", "prAppWebWindowRestore"],
    prAppWebWindowCreate: function(selector, selectorSize, id, title, titleSize, systemTitleBar, sheetParent,
                                   sheetFramed) {
        const host = document.querySelector(prWebString(selector, selectorSize));
        if (host === null)
            return false;
        prAppWebAddStyles();
        const frame = document.createElement("div");
        frame.className = "prax-frame" + (sheetParent !== 0 && sheetFramed ? " prax-sheet" : "");
        const canvas = document.createElement("canvas");
        canvas.id = "prax-window-" + id;
        canvas.className = "prax-canvas";
        canvas.tabIndex = 0;
        canvas.dataset.praxWindow = id;
        const modifiersOf = (event) =>
            (event.shiftKey ? 1 : 0) | (event.ctrlKey ? 2 : 0) | (event.altKey ? 4 : 0) | (event.metaKey ? 8 : 0);
        const onHost = (event) => {
            const rect = host.getBoundingClientRect();
            return [event.clientX - rect.left, event.clientY - rect.top];
        };
        // A press on the frame moves or resizes the window, by the edges that it moves, until its release.
        const dragging = (element, edges) => {
            element.addEventListener("pointerdown", (event) => {
                if (event.button !== 0)
                    return;
                event.preventDefault();
                element.setPointerCapture(event.pointerId);
                _prAppWebFrameDrag(id, edges, 0, ...onHost(event));
            });
            element.addEventListener("pointermove", (event) => {
                if (element.hasPointerCapture(event.pointerId))
                    _prAppWebFrameDrag(id, edges, 1, ...onHost(event));
            });
            element.addEventListener("pointerup", (event) => {
                if (element.hasPointerCapture(event.pointerId))
                    _prAppWebFrameDrag(id, edges, 2, ...onHost(event));
            });
        };
        let bar = null;
        let label = null;
        if (systemTitleBar) {
            bar = document.createElement("div");
            bar.className = "prax-title-bar";
            label = document.createElement("span");
            label.className = "prax-title";
            label.textContent = prWebString(title, titleSize);
            bar.appendChild(label);
            // The buttons are PrAppTitleBarRegionKind's Minimize, Maximize and Close.
            for (const [kind, text, name] of [[2, "–", "Minimize"], [3, "□", "Maximize"], [4, "✕", "Close"]]) {
                const button = document.createElement("button");
                button.className = "prax-button" + (kind === 4 ? " prax-close" : "");
                button.textContent = text;
                button.title = name;
                button.tabIndex = -1;
                button.addEventListener("pointerdown", (event) => event.stopPropagation());
                button.addEventListener("click", () => _prAppWebFrameButton(id, kind));
                bar.appendChild(button);
            }
            dragging(bar, 16);
            bar.addEventListener("dblclick", (event) => {
                if (event.target === bar || event.target === label)
                    _prAppWebFrameButton(id, 3);
            });
            frame.appendChild(bar);
        }
        frame.appendChild(canvas);
        // The borders reach a little outside the frame, on each side and at each corner: left 1, top 2, right 4 and
        // bottom 8.
        const borders = [];
        const sides = [
            [1, "left: -4px; top: 0; width: 8px; height: 100%; cursor: ew-resize;"],
            [4, "right: -4px; top: 0; width: 8px; height: 100%; cursor: ew-resize;"],
            [2, "left: 0; top: -4px; width: 100%; height: 8px; cursor: ns-resize;"],
            [8, "left: 0; bottom: -4px; width: 100%; height: 8px; cursor: ns-resize;"],
            [3, "left: -6px; top: -6px; width: 14px; height: 14px; cursor: nwse-resize;"],
            [12, "right: -6px; bottom: -6px; width: 14px; height: 14px; cursor: nwse-resize;"],
            [6, "right: -6px; top: -6px; width: 14px; height: 14px; cursor: nesw-resize;"],
            [9, "left: -6px; bottom: -6px; width: 14px; height: 14px; cursor: nesw-resize;"],
        ];
        for (const [edges, place] of sides) {
            const border = document.createElement("div");
            border.className = "prax-border";
            border.style.cssText = place;
            dragging(border, edges);
            frame.appendChild(border);
            borders.push(border);
        }
        // A press on the blocked parent gives the modal window the keyboard.
        let blocker = null;
        const parent = prAppWebWindows[sheetParent];
        if (parent !== undefined) {
            blocker = document.createElement("div");
            blocker.className = "prax-blocker";
            blocker.addEventListener("pointerdown", (event) => {
                event.preventDefault();
                canvas.focus({preventScroll: true});
            });
            parent.frame.appendChild(blocker);
        }
        host.appendChild(frame);

        // The bit of each button in a PointerEvent's buttons, by its button.
        const bits = [1, 4, 2, 8, 16, 32];
        const pointer = (kind) => (event) => {
            if (kind === 1) {
                event.preventDefault();
                canvas.setPointerCapture(event.pointerId);
                canvas.focus({preventScroll: true});
            } else if (kind === 0 && event.button >= 0 && event.button < bits.length) {
                kind = (event.buttons & bits[event.button]) !== 0 ? 1 : 2;
            }
            const type = event.pointerType === "pen" ? 1 : event.pointerType === "touch" ? 2 : 0;
            const rect = canvas.getBoundingClientRect();
            _prAppWebPointer(id, kind, type, event.pointerId, event.isPrimary, event.clientX - rect.left,
                event.clientY - rect.top, event.button, event.pressure, event.tiltX, event.tiltY, event.twist,
                event.width, event.height, modifiersOf(event), event.movementX || 0, event.movementY || 0);
        };
        const listeners = [
            ["pointerdown", pointer(1)],
            ["pointermove", pointer(0)],
            ["pointerup", pointer(2)],
            ["pointercancel", pointer(3)],
            ["pointerenter", pointer(4)],
            ["pointerleave", pointer(5)],
            ["focus", () => _prAppWebWindowFocused(id, true)],
            ["blur", () => _prAppWebWindowFocused(id, false)],
        ];
        for (const [type, handler] of listeners)
            canvas.addEventListener(type, handler);
        canvas.addEventListener("wheel", (event) => {
            event.preventDefault();
            const rect = canvas.getBoundingClientRect();
            _prAppWebWheel(id, event.clientX - rect.left, event.clientY - rect.top, event.deltaX, event.deltaY,
                event.deltaMode, modifiersOf(event));
        }, {passive: false});

        // The drop effect of each PrAppDragOperation, and the operations that each value of effectAllowed names.
        const effects = ["none", "copy", "move", "link"];
        const allowances = {
            none: 0, copy: 2, move: 4, link: 8, copyMove: 6, copyLink: 10, linkMove: 12, all: 14, uninitialized: 14,
        };
        const describe = (event) => {
            const transfer = event.dataTransfer;
            const types = transfer !== null ? Array.from(transfer.types) : [];
            const text = types.includes("text/plain") || types.includes("text/uri-list");
            const rect = canvas.getBoundingClientRect();
            return {
                content: (text ? 1 : 0) | (types.includes("Files") ? 2 : 0),
                operations: transfer !== null && transfer.effectAllowed in allowances ?
                    allowances[transfer.effectAllowed] : 14,
                x: event.clientX - rect.left,
                y: event.clientY - rect.top,
                modifiers: modifiersOf(event),
            };
        };
        // The page accepts the drag by canceling the event, for any answer other than none.
        const answer = (kind) => (event) => {
            const drag = describe(event);
            const operation = _prAppWebDrag(id, kind, drag.x, drag.y, drag.content, drag.operations, drag.modifiers);
            if (kind !== 2 && operation !== 0) {
                event.preventDefault();
                event.dataTransfer.dropEffect = effects[operation];
            }
        };
        canvas.addEventListener("dragenter", answer(0));
        canvas.addEventListener("dragover", answer(1));
        canvas.addEventListener("dragleave", answer(2));
        canvas.addEventListener("drop", (event) => {
            const drag = describe(event);
            const operation = _prAppWebDropAnswer(id, drag.operations);
            if (operation === 0) {
                _prAppWebDrag(id, 2, drag.x, drag.y, drag.content, drag.operations, drag.modifiers);
                return;
            }
            event.preventDefault();
            const transfer = event.dataTransfer;
            // A link's list holds one URL a line, and comments that begin with #.
            const links = transfer.getData("text/uri-list").split(/\r?\n/);
            const text = transfer.getData("text/plain") || links.find((line) => line !== "" && line[0] !== "#") || "";
            const textBytes = stringToNewUTF8(text);
            prAppWebFileSetAdd(Array.from(transfer.files), (key, names, nameBytes, sizes, count) =>
                _prAppWebDrop(id, drag.x, drag.y, operation, drag.modifiers, textBytes, key, names, nameBytes, sizes,
                    count));
            _free(textBytes);
        });

        if (typeof GestureEvent !== "undefined") {
            let scale = 1;
            let rotation = 0;
            const gesture = (phase) => (event) => {
                event.preventDefault();
                const rect = canvas.getBoundingClientRect();
                const x = event.clientX - rect.left;
                const y = event.clientY - rect.top;
                if (phase === 0) {
                    scale = 1;
                    rotation = 0;
                }
                _prAppWebGesture(id, 0, phase, x, y, event.scale / scale - 1, 0);
                _prAppWebGesture(id, 1, phase, x, y, 0, event.rotation - rotation);
                scale = event.scale;
                rotation = event.rotation;
            };
            canvas.addEventListener("gesturestart", gesture(0));
            canvas.addEventListener("gesturechange", gesture(1));
            canvas.addEventListener("gestureend", gesture(2));
        }
        // The button that restores the window from the shelf while it is minimized. The shelf is the host's, made with
        // its first button.
        let shelf = host.querySelector(":scope > .prax-shelf");
        if (shelf === null) {
            shelf = document.createElement("div");
            shelf.className = "prax-shelf";
            host.appendChild(shelf);
        }
        const chip = document.createElement("button");
        chip.className = "prax-chip";
        chip.textContent = prWebString(title, titleSize);
        chip.style.display = "none";
        chip.addEventListener("click", () => _prAppWebWindowRestore(id));
        shelf.appendChild(chip);
        prAppWebWindows[id] = {frame, canvas, bar, label, borders, blocker, chip};
        return true;
    },

    // Removes the frame of the window `id`, and releases the pointer that its canvas locks.
    prAppWebWindowDestroy__deps: ["$prAppWebWindows"],
    prAppWebWindowDestroy: function(id) {
        const entry = prAppWebWindows[id];
        if (entry === undefined)
            return;
        if (document.pointerLockElement === entry.canvas)
            document.exitPointerLock();
        entry.frame.remove();
        entry.chip.remove();
        if (entry.blocker !== null)
            entry.blocker.remove();
        delete prAppWebWindows[id];
    },

    // Shows the frame of the window `id` with its canvas at `x` and `y` in the host, `width` by `height` CSS px, under
    // its title bar, or hides it, as the PlaceFlag set of app_web.cpp in `flags` has it, at the place `z` of the page's
    // order and at `opacity`.
    prAppWebWindowPlace__deps: ["$prAppWebWindows", "$prAppWebTitleBarHeight"],
    prAppWebWindowPlace: function(id, x, y, width, height, z, flags, opacity) {
        const entry = prAppWebWindows[id];
        if (entry === undefined)
            return;
        const visible = (flags & 1) !== 0;
        const style = entry.frame.style;
        const titled = entry.bar !== null && (flags & 64) === 0;
        if (entry.bar !== null)
            entry.bar.style.display = titled ? "" : "none";
        const bar = titled ? prAppWebTitleBarHeight : 0;
        style.display = visible ? "flex" : "none";
        entry.frame.classList.toggle("prax-fill", (flags & 2) !== 0);
        style.left = x + "px";
        style.top = (y - bar) + "px";
        style.width = width + "px";
        style.height = (height + bar) + "px";
        style.zIndex = String(z);
        style.opacity = String(opacity);
        style.pointerEvents = (flags & 4) !== 0 ? "none" : "";
        const blur = (flags & 8) !== 0 ? "blur(24px)" : "";
        style.backdropFilter = blur;
        style.webkitBackdropFilter = blur;
        for (const border of entry.borders)
            border.style.display = (flags & 16) !== 0 ? "" : "none";
        if (entry.blocker !== null)
            entry.blocker.style.display = visible && (flags & 32) !== 0 ? "block" : "none";
        entry.chip.style.display = (flags & 128) !== 0 ? "" : "none";
    },

    prAppWebWindowSetTitle__deps: ["$prWebString", "$prAppWebWindows"],
    prAppWebWindowSetTitle: function(id, title, titleSize) {
        const entry = prAppWebWindows[id];
        if (entry === undefined)
            return;
        const text = prWebString(title, titleSize);
        if (entry.label !== null)
            entry.label.textContent = text;
        entry.chip.textContent = text;
    },

    // Writes the canvas of the window `id` in the host to `out`, as four doubles in CSS px: its left, top, width and
    // height.
    prAppWebWindowBounds__deps: ["$prAppWebWindows"],
    prAppWebWindowBounds: function(id, out) {
        const entry = prAppWebWindows[id];
        let values = [0, 0, 0, 0];
        if (entry !== undefined) {
            const host = entry.frame.parentElement.getBoundingClientRect();
            const rect = entry.canvas.getBoundingClientRect();
            values = [rect.left - host.left, rect.top - host.top, rect.width, rect.height];
        }
        for (let i = 0; i < 4; ++i)
            HEAPF64[(out >> 3) + i] = values[i];
    },

    // Gives the canvas of the window `id` the page's focus.
    prAppWebWindowFocus__deps: ["$prAppWebWindows"],
    prAppWebWindowFocus: function(id) {
        const entry = prAppWebWindows[id];
        if (entry !== undefined)
            entry.canvas.focus({preventScroll: true});
    },

    // Writes the user's preferences, as the page's media queries report them, to `out` as eight doubles: 1 for a dark
    // color scheme, a PrAppContrast, 1 for reduced motion, 1 for reduced transparency, and the accent color's sRGB
    // components and alpha, which is 0 where the browser reports no accent color (css-color-4/#css-system-colors).
    prAppWebPreferences: function(out) {
        const matches = (query) => matchMedia(query).matches ? 1 : 0;
        let contrast = 0;
        if (matches("(prefers-contrast: more)"))
            contrast = 1;
        else if (matches("(prefers-contrast: less)"))
            contrast = 2;
        else if (matches("(prefers-contrast: custom)"))
            contrast = 3;
        let accent = [0, 0, 0, 0];
        if (CSS.supports("color", "AccentColor") && document.body !== null) {
            const probe = document.createElement("span");
            probe.style.color = "AccentColor";
            document.body.appendChild(probe);
            const parts = getComputedStyle(probe).color.match(/[\d.]+/g);
            probe.remove();
            if (parts !== null && parts.length >= 3)
                accent = [parts[0] / 255, parts[1] / 255, parts[2] / 255, 1];
        }
        const values = [
            matches("(prefers-color-scheme: dark)"), contrast, matches("(prefers-reduced-motion: reduce)"),
            matches("(prefers-reduced-transparency: reduce)"), ...accent,
        ];
        for (let i = 0; i < values.length; ++i)
            HEAPF64[(out >> 3) + i] = values[i];
    },

    // Starts or stops calling prAppWebPreferencesChanged when a media query of the user's preferences changes.
    $prAppWebPreferenceQueries: null,
    prAppWebListenForPreferences__deps: ["$prAppWebPreferenceQueries", "prAppWebPreferencesChanged"],
    prAppWebListenForPreferences: function(listen) {
        if (prAppWebPreferenceQueries !== null) {
            for (const [query, listener] of prAppWebPreferenceQueries)
                query.removeEventListener("change", listener);
        }
        prAppWebPreferenceQueries = null;
        if (!listen)
            return;
        const listener = () => _prAppWebPreferencesChanged();
        prAppWebPreferenceQueries = [
            "(prefers-color-scheme: dark)", "(prefers-contrast: more)", "(prefers-contrast: less)",
            "(prefers-contrast: custom)", "(prefers-reduced-motion: reduce)", "(prefers-reduced-transparency: reduce)",
        ].map((text) => [matchMedia(text), listener]);
        for (const [query, handler] of prAppWebPreferenceQueries)
            query.addEventListener("change", handler);
    },

    // Locks the pointer to the canvas, or releases it. A raw lock asks for the mouse's movement without the system's
    // acceleration, where the browser offers it. The browser locks only while the page handles the user's input, and
    // reports the change with pointerlockchange.
    prAppWebSetPointerLock__deps: ["$prWebString"],
    prAppWebSetPointerLock: function(selector, selectorSize, locked, raw) {
        const canvas = document.querySelector(prWebString(selector, selectorSize));
        if (canvas === null)
            return;
        if (!locked) {
            if (document.pointerLockElement === canvas)
                document.exitPointerLock();
            return;
        }
        const request = canvas.requestPointerLock(raw ? {unadjustedMovement: true} : undefined);
        if (request instanceof Promise)
            request.catch(() => {});
    },

    // The sets of files that the page keeps for the program's sources, by key. A source's native handle holds its key.
    // The browser's files stay readable after the event that handed them over, so the page reads their bytes only when
    // the program asks.
    $prAppWebFileSets: {},
    $prAppWebFileSetLast: 0,

    // Keeps `files` as a set of its own, and passes its key, its files' names, each ended by a NUL, and their sizes to
    // `pass`, as (key, names, nameBytes, sizes, count).
    $prAppWebFileSetAdd__deps: ["$prAppWebFileSets", "$prAppWebFileSetLast", "$stringToNewUTF8", "$lengthBytesUTF8",
        "malloc", "free"],
    $prAppWebFileSetAdd: function(files, pass) {
        let key = 0;
        if (files.length !== 0) {
            key = ++prAppWebFileSetLast;
            prAppWebFileSets[key] = files;
        }
        const joined = files.map((file) => file.name + "\0").join("");
        const names = stringToNewUTF8(joined);
        const sizes = _malloc(Math.max(files.length, 1) * 8);
        for (let i = 0; i < files.length; ++i)
            HEAPF64[(sizes >> 3) + i] = files[i].size;
        pass(key, names, lengthBytesUTF8(joined), sizes, files.length);
        _free(names);
        _free(sizes);
    },

    // Lets the files of the set `key` go.
    prAppWebFileSetRelease__deps: ["$prAppWebFileSets"],
    prAppWebFileSetRelease: function(key) {
        delete prAppWebFileSets[key];
    },

    // Reads up to `size` bytes at `offset` of the file at `index` in the set `key`, and answers the read `id` through
    // prAppWebFileRead. The status is 0 for a successful read, 1 if the set does not hold the file, and 2 if the file
    // cannot be read.
    prAppWebReadFile__deps: ["$prAppWebFileSets", "malloc", "free", "prAppWebFileRead"],
    prAppWebReadFile: function(id, key, index, offset, size) {
        const files = prAppWebFileSets[key];
        if (files === undefined || index >= files.length) {
            _prAppWebFileRead(id, 1, 0, 0);
            return;
        }
        files[index].slice(offset, offset + size).arrayBuffer().then((buffer) => {
            const bytes = new Uint8Array(buffer);
            const at = _malloc(Math.max(bytes.length, 1));
            HEAPU8.set(bytes, at);
            _prAppWebFileRead(id, 0, at, bytes.length);
            _free(at);
        }, () => _prAppWebFileRead(id, 2, 0, 0));
    },

    // Shows the browser's dialog for the user to choose files to open, which answers the dialog `id` through
    // prAppWebFilesChosen with the set of the files chosen, or none if the user canceled. `accept` lists the extensions
    // to offer, as ".png,.jpg", or is empty.
    prAppWebOpenFiles__deps: ["$prWebString", "$prAppWebFileSetAdd", "prAppWebFilesChosen"],
    prAppWebOpenFiles: function(id, multiple, accept, acceptSize) {
        const input = document.createElement("input");
        input.type = "file";
        input.multiple = !!multiple;
        const extensions = prWebString(accept, acceptSize);
        if (extensions !== "")
            input.accept = extensions;
        let answered = false;
        const answer = (files) => {
            if (answered)
                return;
            answered = true;
            prAppWebFileSetAdd(files, (key, names, nameBytes, sizes, count) =>
                _prAppWebFilesChosen(id, key, names, nameBytes, sizes, count));
        };
        input.addEventListener("cancel", () => answer([]));
        input.addEventListener("change", () => answer(Array.from(input.files || [])));
        input.click();
    },

    // Starts or stops taking the launches of an installed page from its launch queue (Launch Handler), with the files
    // that the system opens with it (File Handling). Each launch calls prAppWebLaunched with its URL and its files,
    // kept as a set. A page that has no launch queue, or whose launches the browser does not queue, as an ordinary
    // tab's, takes none. The queue keeps its consumer, so stopping drops the launches that come after.
    $prAppWebLaunching: false,
    prAppWebListenForLaunches__deps: ["$prAppWebLaunching", "$prAppWebFileSetAdd", "$stringToNewUTF8", "free",
        "prAppWebLaunched"],
    prAppWebListenForLaunches: function(listen) {
        prAppWebLaunching = !!listen;
        if (!listen || !("launchQueue" in window))
            return;
        window.launchQueue.setConsumer((params) => {
            const handles = Array.from(params.files || []);
            Promise.all(handles.map((handle) => handle.getFile ? handle.getFile().catch(() => null) : null))
                .then((files) => {
                    if (!prAppWebLaunching)
                        return;
                    const url = stringToNewUTF8(params.targetURL || "");
                    prAppWebFileSetAdd(files.filter((file) => file !== null), (key, names, nameBytes, sizes, count) =>
                        _prAppWebLaunched(url, key, names, nameBytes, sizes, count));
                    _free(url);
                });
        });
    },

    // Starts or stops turning a paste that the user chooses from the browser's menu into a Paste event.
    $prAppWebPasteListener: null,
    prAppWebListenForPaste__deps: ["$prAppWebPasteListener", "$stringToNewUTF8", "free", "prAppWebPasted"],
    prAppWebListenForPaste: function(listen) {
        if (prAppWebPasteListener !== null)
            document.removeEventListener("paste", prAppWebPasteListener);
        prAppWebPasteListener = null;
        if (!listen)
            return;
        prAppWebPasteListener = (event) => {
            const text = event.clipboardData ? event.clipboardData.getData("text/plain") : "";
            event.preventDefault();
            const bytes = stringToNewUTF8(text);
            _prAppWebPasted(bytes);
            _free(bytes);
        };
        document.addEventListener("paste", prAppWebPasteListener);
    },

    // Starts or stops delivering the page's lifecycle and its notices: freeze and resume, a change of languages, the
    // battery's changes, and a change of the keyboard's layout. Each notice is a number that prAppWebAppEvent takes.
    $prAppWebServices: null,
    $prAppWebBattery: null,
    $prAppWebLayout: null,
    prAppWebListenForServices__deps: ["$prAppWebServices", "$prAppWebBattery", "$prAppWebLayout",
        "prAppWebLifecycleChanged", "prAppWebAppEvent"],
    prAppWebListenForServices: function(listen) {
        if (prAppWebServices !== null) {
            for (const [target, type, handler] of prAppWebServices)
                target.removeEventListener(type, handler);
        }
        prAppWebServices = null;
        if (!listen)
            return;
        const lifecycle = () => _prAppWebLifecycleChanged();
        const notice = (number) => () => _prAppWebAppEvent(number);
        prAppWebServices = [
            [document, "visibilitychange", lifecycle],
            [window, "focus", lifecycle],
            [window, "blur", lifecycle],
            [document, "freeze", notice(0)],
            [document, "resume", notice(1)],
            [window, "languagechange", notice(2)],
        ];
        if (navigator.getBattery) {
            navigator.getBattery().then((battery) => {
                prAppWebBattery = battery;
                battery.addEventListener("chargingchange", notice(3));
                battery.addEventListener("levelchange", notice(3));
                _prAppWebAppEvent(3);
            }).catch(() => {});
        }
        const keyboard = navigator.keyboard;
        if (keyboard && keyboard.getLayoutMap) {
            const load = () => keyboard.getLayoutMap().then((map) => {
                const changed = prAppWebLayout !== null;
                prAppWebLayout = map;
                if (changed)
                    _prAppWebAppEvent(4);
            }).catch(() => {});
            load();
            if (keyboard.addEventListener)
                prAppWebServices.push([keyboard, "layoutchange", load]);
        }
        for (const [target, type, handler] of prAppWebServices)
            target.addEventListener(type, handler);
    },

    // Returns the page's lifecycle as a PrAppLifecycle: Background while the page is hidden, Active while it has the
    // focus, and Inactive otherwise.
    prAppWebLifecycle: function() {
        if (document.visibilityState === "hidden")
            return 2;
        return document.hasFocus() ? 0 : 1;
    },

    // Writes the device's power to `out`, as three doubles: a PrAppPowerSource, the battery's level or -1, and 1 if it
    // charges.
    prAppWebPower__deps: ["$prAppWebBattery"],
    prAppWebPower: function(out) {
        const battery = prAppWebBattery;
        HEAPF64[out >> 3] = battery ? (battery.charging ? 1 : 2) : 0;
        HEAPF64[(out >> 3) + 1] = battery ? battery.level : -1;
        HEAPF64[(out >> 3) + 2] = battery && battery.charging ? 1 : 0;
    },

    // Keeps the display awake with a screen wake lock, or lets it sleep. The browser releases the lock while the page
    // is hidden, so the page takes it again as it shows.
    $prAppWebWake: {wanted: false, lock: null, listener: null},
    prAppWebSetAwake__deps: ["$prAppWebWake"],
    prAppWebSetAwake: function(display) {
        const wake = prAppWebWake;
        wake.wanted = !!display;
        const acquire = () => {
            if (!wake.wanted || wake.lock !== null || !navigator.wakeLock || document.visibilityState !== "visible")
                return;
            navigator.wakeLock.request("screen").then((lock) => {
                if (!wake.wanted) {
                    lock.release();
                    return;
                }
                wake.lock = lock;
                lock.addEventListener("release", () => { wake.lock = null; });
            }).catch(() => {});
        };
        if (!wake.wanted && wake.lock !== null) {
            wake.lock.release();
            wake.lock = null;
        }
        if (wake.wanted && wake.listener === null) {
            wake.listener = acquire;
            document.addEventListener("visibilitychange", acquire);
        } else if (!wake.wanted && wake.listener !== null) {
            document.removeEventListener("visibilitychange", wake.listener);
            wake.listener = null;
        }
        acquire();
    },

    // Shares `text` and `url` through the Web Share API, which answers the share `id` through prAppWebShared. Returns
    // false if the browser has no share sheet. A user who closes the sheet answers as a share.
    prAppWebShare__deps: ["$prWebString", "prAppWebShared"],
    prAppWebShare: function(id, text, textSize, url, urlSize) {
        if (!navigator.share)
            return false;
        const data = {};
        const value = prWebString(text, textSize);
        const address = prWebString(url, urlSize);
        if (value !== "")
            data.text = value;
        if (address !== "")
            data.url = address;
        const refused = (error) => error && error.name === "NotAllowedError";
        const closed = (error) => error && error.name === "AbortError";
        navigator.share(data).then(() => _prAppWebShared(id, 0),
            (error) => _prAppWebShared(id, closed(error) ? 0 : refused(error) ? 1 : 2));
        return true;
    },

    // Writes the label of the key at the UI Events code `code` in the layout map, uppercased, to `out`, and returns its
    // length in bytes. Returns 0 for a key that the map leaves out, and -1 while the map is not loaded.
    prAppWebKeyLabel__deps: ["$prAppWebLayout", "$prWebString", "$stringToUTF8"],
    prAppWebKeyLabel: function(code, codeSize, out, capacity) {
        if (prAppWebLayout === null)
            return navigator.keyboard && navigator.keyboard.getLayoutMap ? -1 : 0;
        const value = prAppWebLayout.get(prWebString(code, codeSize));
        if (typeof value !== "string" || value.trim() === "")
            return 0;
        return stringToUTF8(value.toUpperCase(), out, capacity);
    },

    // Plays a PrAppHaptic as a vibration, on a device that vibrates.
    prAppWebVibrate: function(haptic) {
        if (!navigator.vibrate)
            return;
        const patterns = [[5], [10], [20], [30], [10, 50, 10], [20, 60, 20], [30, 60, 30, 60, 30]];
        navigator.vibrate(patterns[haptic] || [10]);
    },

    // Shows the badge of an installed page through the Badging API: a number, a dot for other text, or none for empty.
    prAppWebSetBadge__deps: ["$prWebString"],
    prAppWebSetBadge: function(text, textSize) {
        if (!navigator.setAppBadge)
            return;
        const value = prWebString(text, textSize);
        if (value === "") {
            navigator.clearAppBadge().catch(() => {});
            return;
        }
        const number = Number(value);
        const shown = Number.isInteger(number) && number >= 0 ? navigator.setAppBadge(number) : navigator.setAppBadge();
        shown.catch(() => {});
    },

    // Starts or stops the readings of a PrAppSensor, at most one each `interval` seconds, and returns 0, or 1 if the
    // browser has no such sensor. Device motion gives the accelerometer and the gyroscope, whose rates it reports in
    // degrees, and device orientation gives the attitude, whose angles turn about z, then x, then y. A browser that
    // asks for permission, as Safari does, starts the readings once the user allows them.
    $prAppWebSensors: {},
    prAppWebSensor__deps: ["$prAppWebSensors", "prAppWebSensorReading"],
    prAppWebSensor: function(sensor, enable, interval) {
        const motion = sensor === 0 || sensor === 1;
        const type = motion ? "devicemotion" : sensor === 3 ? "deviceorientation" : null;
        const api = motion ? window.DeviceMotionEvent : window.DeviceOrientationEvent;
        if (type === null || api === undefined)
            return 1;
        if (prAppWebSensors[sensor]) {
            window.removeEventListener(type, prAppWebSensors[sensor]);
            delete prAppWebSensors[sensor];
        }
        if (!enable)
            return 0;
        const radians = Math.PI / 180;
        let last = 0;
        const listener = (event) => {
            const now = performance.now();
            if (now - last < interval * 1000)
                return;
            last = now;
            if (sensor === 0 && event.accelerationIncludingGravity) {
                const a = event.accelerationIncludingGravity;
                _prAppWebSensorReading(0, a.x || 0, a.y || 0, a.z || 0, 0);
            } else if (sensor === 1 && event.rotationRate) {
                const r = event.rotationRate;
                _prAppWebSensorReading(1, (r.beta || 0) * radians, (r.gamma || 0) * radians, (r.alpha || 0) * radians, 0);
            } else if (sensor === 3) {
                const z = (event.alpha || 0) * radians / 2;
                const x = (event.beta || 0) * radians / 2;
                const y = (event.gamma || 0) * radians / 2;
                const cz = Math.cos(z), sz = Math.sin(z), cx = Math.cos(x), sx = Math.sin(x);
                const cy = Math.cos(y), sy = Math.sin(y);
                _prAppWebSensorReading(3, cz * sx * cy - cx * sz * sy, cz * cx * sy + sz * sx * cy,
                    cx * sz * cy + cz * sx * sy, cz * cx * cy - sz * sx * sy);
            }
        };
        prAppWebSensors[sensor] = listener;
        const start = () => {
            if (prAppWebSensors[sensor] === listener)
                window.addEventListener(type, listener);
        };
        if (typeof api.requestPermission === "function")
            api.requestPermission().then((answer) => { if (answer === "granted") start(); }).catch(() => {});
        else
            start();
        return 0;
    },

    // Locks the screen's orientation to the portraits or the landscapes in the PrAppOrientationFlags `orientations`, or
    // unlocks it for both. A browser locks only a fullscreen page, and refuses otherwise.
    prAppWebLockOrientation: function(orientations) {
        const orientation = screen.orientation;
        if (!orientation || !orientation.lock)
            return;
        const portrait = (orientations & (2 | 8)) !== 0;
        const landscape = (orientations & (1 | 4)) !== 0;
        if (portrait === landscape) {
            if (orientation.unlock)
                orientation.unlock();
            return;
        }
        orientation.lock(portrait ? "portrait" : "landscape").catch(() => {});
    },

    // Sets the page's favicon to `width` by `height` straight-alpha Rgba8 pixels, or removes the icon that it set for
    // null pixels.
    prAppWebSetIcon: function(pixels, width, height) {
        let link = document.querySelector("link[data-prax-icon]");
        if (pixels === 0) {
            if (link !== null)
                link.remove();
            return;
        }
        const canvas = document.createElement("canvas");
        canvas.width = width;
        canvas.height = height;
        const image = new ImageData(new Uint8ClampedArray(HEAPU8.slice(pixels, pixels + width * height * 4).buffer),
            width, height);
        canvas.getContext("2d").putImageData(image, 0, 0);
        if (link === null) {
            link = document.createElement("link");
            link.rel = "icon";
            link.dataset.praxIcon = "";
            document.head.appendChild(link);
        }
        link.href = canvas.toDataURL("image/png");
    },

    prAppWebHasFocus: function() {
        return document.hasFocus();
    },

    // Writes the page's screen in CSS pixels to `out`, as six doubles: its width and height, and its work area's left,
    // top, width and height from the screen's top left. Browsers that do not say where the screen is report its work
    // area from its top left.
    prAppWebScreen: function(out) {
        const left = typeof screen.left === "number" ? screen.left : 0;
        const top = typeof screen.top === "number" ? screen.top : 0;
        const availLeft = typeof screen.availLeft === "number" ? screen.availLeft - left : 0;
        const availTop = typeof screen.availTop === "number" ? screen.availTop - top : 0;
        const values = [screen.width, screen.height, availLeft, availTop, screen.availWidth, screen.availHeight];
        for (let i = 0; i < values.length; ++i)
            HEAPF64[(out >> 3) + i] = values[i];
    },

    // Starts or stops calling prAppWebDisplaysChanged when the page's device pixel ratio changes, as when the window
    // moves to a screen of another scale or the user zooms. A media query matches only one ratio, so each change makes
    // a query for the new ratio.
    $prAppWebRatioQuery: null,
    prAppWebListenForDisplays__deps: ["$prAppWebRatioQuery", "prAppWebDisplaysChanged"],
    prAppWebListenForDisplays: function(listen) {
        if (prAppWebRatioQuery !== null)
            prAppWebRatioQuery.query.removeEventListener("change", prAppWebRatioQuery.listener);
        prAppWebRatioQuery = null;
        if (!listen)
            return;
        const query = matchMedia("(resolution: " + window.devicePixelRatio + "dppx)");
        const listener = () => {
            _prAppWebListenForDisplays(true);
            _prAppWebDisplaysChanged();
        };
        query.addEventListener("change", listener);
        prAppWebRatioQuery = {query, listener};
    },

    // Focuses the page's window, which a browser may refuse while the page is in the background.
    prAppWebFocus: function() {
        window.focus();
    },

    // Returns the element that shows the host on the machine's display: the whole page for the body.
    $prAppWebHostElement__deps: ["$prWebString"],
    $prAppWebHostElement: function(selector, selectorSize) {
        const host = document.querySelector(prWebString(selector, selectorSize));
        return host === document.body ? document.documentElement : host;
    },

    // Shows the host on the machine's display, or returns it to the page. The browser refuses to show it outside the
    // handling of the user's input, and the host's change of extent reports the change.
    prAppWebHostSetFullscreen__deps: ["$prAppWebHostElement"],
    prAppWebHostSetFullscreen: function(selector, selectorSize, fullscreen) {
        const host = prAppWebHostElement(selector, selectorSize);
        if (host === null)
            return;
        if (fullscreen && document.fullscreenElement !== host && host.requestFullscreen)
            host.requestFullscreen().catch(() => {});
        else if (!fullscreen && document.fullscreenElement === host)
            document.exitFullscreen().catch(() => {});
    },

    prAppWebHostIsFullscreen__deps: ["$prAppWebHostElement"],
    prAppWebHostIsFullscreen: function(selector, selectorSize) {
        const host = prAppWebHostElement(selector, selectorSize);
        return host !== null && document.fullscreenElement === host;
    },

    // Returns true if the canvas is the page's fullscreen element.
    prAppWebIsFullscreen__deps: ["$prWebString"],
    prAppWebIsFullscreen: function(selector, selectorSize) {
        const canvas = document.querySelector(prWebString(selector, selectorSize));
        return canvas !== null && document.fullscreenElement === canvas;
    },

    // Asks the browser to show the canvas fullscreen, or to leave fullscreen. The browser refuses to enter outside the
    // handling of the user's input, and the page's fullscreenchange reports a change.
    prAppWebSetFullscreen__deps: ["$prWebString"],
    prAppWebSetFullscreen: function(selector, selectorSize, fullscreen) {
        const canvas = document.querySelector(prWebString(selector, selectorSize));
        if (canvas === null)
            return;
        if (fullscreen && document.fullscreenElement !== canvas && canvas.requestFullscreen)
            canvas.requestFullscreen().catch(() => {});
        else if (!fullscreen && document.fullscreenElement === canvas)
            document.exitFullscreen().catch(() => {});
    },

    // Shows a notification with the Notification API. If the page has not asked for permission yet, it asks the user,
    // and shows the notification once the user allows it. A click focuses the page, closes the notification, and
    // reports its tag as two 32-bit halves. Returns 0 if the notification shows or waits for the user's answer, 1 if
    // the user refused the page's notifications, and 2 if the browser does not support notifications.
    prAppWebNotify__deps: ["$prWebString", "prAppWebNotificationClicked"],
    prAppWebNotify: function(title, titleSize, body, bodySize, tagLow, tagHigh) {
        if (typeof Notification === "undefined")
            return 2;
        if (Notification.permission === "denied")
            return 1;
        // The strings are copied now, because the program frees them when the call returns.
        const text = {title: prWebString(title, titleSize), body: prWebString(body, bodySize)};
        const show = () => {
            const notification = new Notification(text.title, {body: text.body});
            notification.onclick = () => {
                window.focus();
                notification.close();
                _prAppWebNotificationClicked(tagLow, tagHigh);
            };
        };
        if (Notification.permission === "granted") {
            show();
        } else {
            Notification.requestPermission().then((answer) => {
                if (answer === "granted")
                    show();
            }).catch(() => {});
        }
        return 0;
    },
});
