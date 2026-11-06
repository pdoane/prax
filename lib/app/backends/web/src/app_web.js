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

    prAppWebHasFocus: function() {
        return document.hasFocus();
    },

    // Shows a notification with the Notification API. If the page has not asked for permission yet, it asks the user,
    // and shows the notification once the user allows it. A click focuses the page, closes the notification, and
    // reports its tag as two 32-bit halves. Returns 0 if the notification shows or waits for the user's answer, 1 if
    // the user refused the page's notifications, and 2 if the browser does not support notifications.
    prAppWebNotify__deps: ["$UTF8ToString", "prAppWebNotificationClicked"],
    prAppWebNotify: function(title, body, tagLow, tagHigh) {
        if (typeof Notification === "undefined")
            return 2;
        if (Notification.permission === "denied")
            return 1;
        // The strings are copied now, because the program frees them when the call returns.
        const text = {title: UTF8ToString(title), body: UTF8ToString(body)};
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
