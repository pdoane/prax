// The page's side of the web application backend, which Emscripten links into the program (--js-library). Strings
// arrive as UTF-8 that ends with NUL.

addToLibrary({
    prAppWebSetTitle__deps: ["$UTF8ToString"],
    prAppWebSetTitle: function(title) {
        document.title = UTF8ToString(title);
    },

    prAppWebSetCursor__deps: ["$UTF8ToString"],
    prAppWebSetCursor: function(selector, cursor) {
        const canvas = document.querySelector(UTF8ToString(selector));
        if (canvas !== null)
            canvas.style.cursor = UTF8ToString(cursor);
    },

    // Writes the canvas's top left on the page's viewport, in CSS pixels. Mouse events' client positions are relative
    // to that viewport.
    prAppWebCanvasOrigin__deps: ["$UTF8ToString"],
    prAppWebCanvasOrigin: function(selector, x, y) {
        const canvas = document.querySelector(UTF8ToString(selector));
        const rect = canvas !== null ? canvas.getBoundingClientRect() : {left: 0, top: 0};
        HEAPF64[x >> 3] = rect.left;
        HEAPF64[y >> 3] = rect.top;
    },

    prAppWebOpenUrl__deps: ["$UTF8ToString"],
    prAppWebOpenUrl: function(url) {
        window.open(UTF8ToString(url), "_blank", "noopener");
    },

    prAppWebWriteClipboard__deps: ["$UTF8ToString"],
    prAppWebWriteClipboard: function(text) {
        if (navigator.clipboard)
            navigator.clipboard.writeText(UTF8ToString(text)).catch(() => {});
    },
});
