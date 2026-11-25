// The page's side of test_web_string.cpp: the calls that it makes of web_string.js's helpers.

addToLibrary({
    // Reads `size` bytes of text at `data` as a string with prWebString, and returns its text from prWebNewString,
    // whose size it writes to `*outSize`.
    prCoreWebTestRoundTrip__deps: ["$prWebString", "$prWebStringSize", "$prWebNewString"],
    prCoreWebTestRoundTrip: function(data, size, outSize) {
        const text = prWebString(data, size);
        HEAPU32[outSize >> 2] = prWebStringSize(text);
        return prWebNewString(text);
    },

    // Returns the UTF-16 units, in hexadecimal and joined by commas, of the string that `size` bytes of text at `data`
    // read as.
    prCoreWebTestUnits__deps: ["$prWebString", "$prWebNewString"],
    prCoreWebTestUnits: function(data, size) {
        const text = prWebString(data, size);
        const units = [];
        for (let i = 0; i < text.length; ++i)
            units.push(text.charCodeAt(i).toString(16));
        return prWebNewString(units.join(","));
    },
});
