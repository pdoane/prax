// Measures parsing and serializing stylesheets, over the sheets that `__INPUT__` holds by name.
//
// Nothing here is engine specific: `CSSStyleSheet` and `cssText` are standard browser APIs. tools/bench/measure.js
// describes the loader, which runs this file the same way in pr_shell and in a browser.
//
// The amount of each stylesheet that an engine kept is reported next to how long it took, because an engine that holds
// less of a stylesheet has less work to do.

// Counts the rules and declarations that a sheet parsed to, through the whole tree.
function contents(rules, out) {
    for (let i = 0; i < rules.length; ++i) {
        const rule = rules[i];
        ++out.rules;
        if (rule.style)
            out.declarations += rule.style.length;
        if (rule.cssRules)
            contents(rule.cssRules, out);
    }
    return out;
}

// Returns the number of bytes that a sheet serializes to. Two engines agree on what this number means, unlike the rule
// and declaration counts. An engine that expands a shorthand into its longhands holds more declarations for the same
// stylesheet.
function serializeAll(rules, sink) {
    for (let i = 0; i < rules.length; ++i) {
        sink.length += rules[i].cssText.length;
        if (rules[i].cssRules)
            serializeAll(rules[i].cssRules, sink);
    }
    return sink.length;
}

// Returns `text` followed by a comment that differs at every call.
let uniqueCount = 0;
function unique(text) {
    return text + "\n/* " + ++uniqueCount + " */";
}

function benchmark(sheets) {
    const rows = [];
    for (const [name, text] of Object.entries(sheets)) {
        const parsed = new CSSStyleSheet();
        parsed.replaceSync(text);

        const counted = contents(parsed.cssRules, { rules: 0, declarations: 0 });
        const out = serializeAll(parsed.cssRules, { length: 0 });

        // Measures the cost of replaceSync on its own, and the cost of having every declaration. An engine that defers
        // parsing a rule's body until something reads it pays the first cost and not the second, so two engines can be
        // compared on `ready`. Each call parses text that no earlier call has parsed, so that an engine that caches
        // parsed sheets by their text does the same work as the others.
        const parse = measure(function () {
            const target = new CSSStyleSheet();
            target.replaceSync(unique(text));
        });
        const ready = measure(function () {
            const target = new CSSStyleSheet();
            target.replaceSync(unique(text));
            contents(target.cssRules, { rules: 0, declarations: 0 });
        });
        const write = measure(function () {
            serializeAll(parsed.cssRules, { length: 0 });
        });

        rows.push(Object.assign({ name: name, bytes: text.length, out: out, rules: counted.rules,
                                  declarations: counted.declarations },
                                measuredFields({ parse: parse, ready: ready, write: write })));
    }

    return rows;
}

globalThis.__REPORT__(benchmark(globalThis.__INPUT__));
