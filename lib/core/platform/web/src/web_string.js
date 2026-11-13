// The page's side of core's helper of the web platform, which Emscripten links into every program that links core
// (--js-library): the page's strings of the program's text. A JavaScript library of another module takes text as a
// view, its pointer and its size, and reads it with prWebString.

addToLibrary({
    // Returns the `size` bytes of UTF-8 at `data` as a string, U+0000 included. The bytes are copied first, because a
    // decoder does not read memory that threads share.
    $prWebString: function(data, size) {
        return size === 0 ? "" : new TextDecoder().decode(HEAPU8.slice(data, data + size));
    },
});
