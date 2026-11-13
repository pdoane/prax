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

    // Writes the canvas's top left on the page's viewport, in CSS pixels. Mouse events' client positions are relative
    // to that viewport.
    prAppWebCanvasOrigin__deps: ["$prWebString"],
    prAppWebCanvasOrigin: function(selector, selectorSize, x, y) {
        const canvas = document.querySelector(prWebString(selector, selectorSize));
        const rect = canvas !== null ? canvas.getBoundingClientRect() : {left: 0, top: 0};
        HEAPF64[x >> 3] = rect.left;
        HEAPF64[y >> 3] = rect.top;
    },

    prAppWebOpenUrl__deps: ["$prWebString"],
    prAppWebOpenUrl: function(url, urlSize) {
        window.open(prWebString(url, urlSize), "_blank", "noopener");
    },

    prAppWebWriteClipboard__deps: ["$prWebString"],
    prAppWebWriteClipboard: function(text, textSize) {
        if (navigator.clipboard)
            navigator.clipboard.writeText(prWebString(text, textSize)).catch(() => {});
    },

    prAppWebHasFocus: function() {
        return document.hasFocus();
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
