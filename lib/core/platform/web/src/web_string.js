// The page's side of core's helper of the web platform, which Emscripten links into every program that links core
// (--js-library): the page's strings of the program's text. A JavaScript library of another module takes text as a
// view, its pointer and its size, and reads it with prWebString, and gives the program a string's text with
// prWebNewString or prWebWriteString.
//
// The program's text is WTF-8 (pr/utf8.h), which holds any string: a lone surrogate is its three bytes.

addToLibrary({
    // Returns the `size` bytes of text at `data` as a string, U+0000 included, and a lone surrogate as itself. An
    // ill-formed sequence is U+FFFD. The bytes are copied first, because a decoder does not read memory that threads
    // share.
    $prWebString: function(data, size) {
        if (size === 0)
            return "";
        const bytes = HEAPU8.slice(data, data + size);
        try {
            return new TextDecoder("utf-8", {fatal: true}).decode(bytes);
        } catch (error) {
            // A lone surrogate, which UTF-8 does not hold, or an ill-formed sequence.
        }
        const units = [];
        for (let at = 0; at < size;) {
            const lead = bytes[at];
            const length = lead < 0x80 ? 1 : lead >= 0xc2 && lead <= 0xdf ? 2 : lead >= 0xe0 && lead <= 0xef ? 3
                : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
            let codePoint = length === 1 ? lead : length === 2 ? lead & 0x1f : length === 3 ? lead & 0x0f : lead & 0x07;
            let valid = length !== 0 && at + length <= size;
            for (let i = 1; valid && i < length; ++i) {
                valid = (bytes[at + i] & 0xc0) === 0x80;
                codePoint = codePoint << 6 | (bytes[at + i] & 0x3f);
            }
            // Overlong forms and values past U+10FFFF are ill-formed; surrogates are not.
            const lowest = [0, 0, 0x80, 0x800, 0x10000][length];
            if (!valid || codePoint < lowest || codePoint > 0x10ffff) {
                units.push(0xfffd);
                at += 1;
                continue;
            }
            if (codePoint >= 0x10000)
                units.push(0xd800 + ((codePoint - 0x10000) >> 10), 0xdc00 + ((codePoint - 0x10000) & 0x3ff));
            else
                units.push(codePoint);
            at += length;
        }
        let text = "";
        for (let at = 0; at < units.length; at += 0x8000)
            text += String.fromCharCode.apply(null, units.slice(at, at + 0x8000));
        return text;
    },

    // Returns the number of bytes of `string`'s text: a surrogate pair as its code point's four, and a lone surrogate
    // as its three.
    $prWebStringSize: function(string) {
        let size = 0;
        for (let i = 0; i < string.length; ++i) {
            const unit = string.charCodeAt(i);
            if (unit < 0x80) {
                size += 1;
            } else if (unit < 0x800) {
                size += 2;
            } else if (unit >= 0xd800 && unit <= 0xdbff && i + 1 < string.length &&
                    (string.charCodeAt(i + 1) & 0xfc00) === 0xdc00) {
                size += 4;
                ++i;
            } else {
                size += 3;
            }
        }
        return size;
    },

    // Writes `string`'s text to `out`, as much of it as `capacity` holds whole, and a NUL after it if room remains.
    // Returns the number of bytes of text written.
    $prWebWriteString: function(string, out, capacity) {
        let at = out;
        const end = out + capacity;
        const put = (bytes) => {
            if (at + bytes.length > end)
                return false;
            for (const byte of bytes)
                HEAPU8[at++] = byte;
            return true;
        };
        for (let i = 0; i < string.length; ++i) {
            let codePoint = string.charCodeAt(i);
            if (codePoint >= 0xd800 && codePoint <= 0xdbff && i + 1 < string.length &&
                    (string.charCodeAt(i + 1) & 0xfc00) === 0xdc00)
                codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + (string.charCodeAt(++i) - 0xdc00);
            let bytes;
            if (codePoint < 0x80)
                bytes = [codePoint];
            else if (codePoint < 0x800)
                bytes = [0xc0 | codePoint >> 6, 0x80 | (codePoint & 0x3f)];
            else if (codePoint < 0x10000)
                bytes = [0xe0 | codePoint >> 12, 0x80 | (codePoint >> 6 & 0x3f), 0x80 | (codePoint & 0x3f)];
            else
                bytes = [0xf0 | codePoint >> 18, 0x80 | (codePoint >> 12 & 0x3f), 0x80 | (codePoint >> 6 & 0x3f),
                    0x80 | (codePoint & 0x3f)];
            if (!put(bytes))
                break;
        }
        const written = at - out;
        if (at < end)
            HEAPU8[at] = 0;
        return written;
    },

    // Returns `string`'s text, NUL-terminated, in memory that the caller frees with free().
    $prWebNewString__deps: ["$prWebStringSize", "$prWebWriteString", "malloc"],
    $prWebNewString: function(string) {
        const size = prWebStringSize(string);
        const out = _malloc(size + 1);
        prWebWriteString(string, out, size + 1);
        return out;
    },
});
