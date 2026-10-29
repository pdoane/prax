// Measures decoding a page's bytes with TextDecoder, as UTF-8 and as windows-1252, over the pages that `__INPUT__`
// holds by name.
//
// Nothing here is engine specific. tools/bench/measure.js describes the loader, which runs this file the same way in
// pr_shell and in a browser. lib/charset/tests/charset_bench.cpp decodes the same bytes through the C API.
//
// The bytes are the page's text encoded as UTF-8, so that every side decodes the same bytes. Decoding them as
// windows-1252 costs what decoding a legacy page of the same markup does.

function row(name, text) {
    const bytes = new TextEncoder().encode(text);
    const utf8 = new TextDecoder("utf-8");
    const legacy = new TextDecoder("windows-1252");
    const measured = {
        utf8: measure(() => utf8.decode(bytes)),
        windows1252: measure(() => legacy.decode(bytes)),
    };
    return Object.assign({name, bytes: bytes.length}, measuredFields(measured));
}

function benchmark(pages) {
    const rows = [];
    for (const [name, text] of Object.entries(pages))
        rows.push(row(name, text));
    return rows;
}

globalThis.__REPORT__(benchmark(globalThis.__INPUT__));
