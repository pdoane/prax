// Languages over tree-sitter grammars: the queries and their predicates, the retained tree that edits update, the
// ranges that injections hand to other languages, and the flattening of nested captures into spans.

#include "pr/syntax_treesitter.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/memory.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/regex.h"
#include "pr/sort.h"
#include "pr/time.h"

#include "tree_sitter/api.h"

namespace pr_impl {

namespace {

// A text predicate of a pattern, which tree-sitter leaves to its host.
enum {
    TreeSitterTestKind_Eq,
    TreeSitterTestKind_Match,
    TreeSitterTestKind_AnyOf,
};
typedef uint8_t TreeSitterTestKind;

// One predicate: whether the text of a capture equals a string, equals another capture's text, matches an expression,
// or is one of several strings, or the negation of one of these.
struct TreeSitterTest {
    TreeSitterTestKind kind;
    bool negated;
    uint32_t capture;               // by the query's capture index
    uint32_t otherCapture;          // for Eq of two captures, or UINT32_MAX
    RawArray<PrRawString> strings;  // the string of Eq, or the strings of AnyOf
    PrRegex* PR_NULLABLE regex;     // of Match, or null for a pattern that does not compile, which matches nothing
};

// What a query keeps for one pattern. Of an injections query, a pattern names its language with #set! or with the text
// of a capture, and its content leaves out the ranges of the content node's children unless it includes them. A
// combined pattern's matches are one document of its language.
struct TreeSitterPattern {
    RawArray<TreeSitterTest> tests;
    PrRawString injectionLanguage;  // #set! injection.language, or null
    bool combined;                  // #set! injection.combined
    bool includeChildren;           // #set! injection.include-children
};

// A compiled query, and the predicates of each of its patterns.
struct TreeSitterQuery {
    TSQuery* PR_NULLABLE query;
    RawArray<TreeSitterPattern> patterns;   // by index of the query's patterns
    uint32_t contentCapture;                // @injection.content, or UINT32_MAX
    uint32_t languageCapture;               // @injection.language, or UINT32_MAX
};

// A language over a grammar.
struct TreeSitterLanguage : PrSyntaxLanguage {
    PrRawString ownName;
    PrRawString ownNamePattern;
    RawArray<PrRawString> ownExtensions;
    RawArray<PrRawString> ownFileNames;
    RawArray<PrStringView> extensionViews;
    RawArray<PrStringView> fileNameViews;
    const TSLanguage* grammar;
    TreeSitterQuery highlights;
    TreeSitterQuery injections;
};

// The category of a capture of one match, before categories that nest are flattened.
struct TreeSitterItem {
    uint32_t start;
    uint32_t end;
    PrSyntaxCategory category;
    uint32_t pattern;
    uint8_t layer;                  // zero for the language's own categories, and the depth of an injection for its
};

// The tree of an injection that the last query of spans parsed, which the edits since then have moved. A query reuses
// it for the same ranges, and parses it again incrementally for ranges that an edit changed.
struct TreeSitterLayer {
    const TreeSitterLanguage* language;
    uint8_t depth;
    bool edited;                    // an edit since the parse touched the ranges
    bool used;                      // by the query that runs
    RawArray<TSRange> ranges;       // their bytes moved by the edits since the parse, and their points as parsed
    TSTree* tree;
};

// The syntax tree of one buffer.
struct TreeSitterTree : PrSyntaxTree {
    const TreeSitterLanguage* language;
    PrSyntaxRegistry* registry;
    PrTextEditBuffer* buffer;
    PrTextEditStream* stream;
    TSParser* parser;
    TSTree* PR_NULLABLE parsed;     // the last parse that finished, which edits since then have moved
    TSQueryCursor* cursor;

    // The parser holds a parse that ran out of time, which the next update resumes unless the text changes first.
    bool halted;
    double deadline;                // of the parse that runs, on the clock of prTimeMonotonicMilliseconds
    uint32_t reports;               // of the running parse's progress, since its last look at the clock

    // By the capture index of a language's highlights query: the registry's category. A tree keeps one map for its own
    // language and one for each language that it has injected.
    RawArray<const TreeSitterLanguage*> mappedLanguages;
    RawArray<RawArray<PrSyntaxCategory>> categoryMaps;

    // A parser for each language that the tree has injected, which parses each range afresh.
    RawArray<const TreeSitterLanguage*> injectedLanguages;
    RawArray<TSParser*> injectedParsers;
    RawArray<TreeSitterLayer> layers;

    RawArray<TreeSitterItem> items;
    RawArray<uint32_t> active;         // by index of `items`
    RawArray<uint32_t> bounds;
    RawArray<char> text;
};

void* allocate(size_t size) {
    return prMemoryAlloc(size);
}

void* allocateZeroed(size_t count, size_t size) {
    return prMemoryAllocZeroed(count * size);
}

void* reallocate(void* PR_NULLABLE memory, size_t size) {
    return prMemoryRealloc(memory, size);
}

void release(void* PR_NULLABLE memory) {
    prMemoryFree(memory);
}

// Routes tree-sitter's allocations through the platform, once.
void useAllocator() {
    static bool set = false;
    if (!set) {
        ts_set_allocator(allocate, allocateZeroed, reallocate, release);
        set = true;
    }
}

// Reads the predicates of each pattern of a compiled query.
void readPredicates(TreeSitterQuery& q) {
    uint32_t patternCount = ts_query_pattern_count(q.query);
    q.patterns.resize(patternCount);
    for (uint32_t p = 0; p < patternCount; ++p)
        q.patterns.data[p] = TreeSitterPattern{};
    for (uint32_t p = 0; p < patternCount; ++p) {
        uint32_t stepCount = 0;
        const TSQueryPredicateStep* steps = ts_query_predicates_for_pattern(q.query, p, &stepCount);
        uint32_t i = 0;
        while (i < stepCount) {
            uint32_t end = i;
            while (end < stepCount && steps[end].type != TSQueryPredicateStepTypeDone)
                ++end;
            if (end > i && steps[i].type == TSQueryPredicateStepTypeString) {
                uint32_t length = 0;
                const char* chars = ts_query_string_value_for_id(q.query, steps[i].value_id, &length);
                PrStringView name = PrStringView(chars, length);
                auto stringAt = [&](uint32_t at) {
                    uint32_t size = 0;
                    const char* value = ts_query_string_value_for_id(q.query, steps[at].value_id, &size);
                    return PrStringView(value, size);
                };
                bool hasCapture = end > i + 1 && steps[i + 1].type == TSQueryPredicateStepTypeCapture;
                TreeSitterTest test{};
                test.otherCapture = UINT32_MAX;
                bool kept = false;
                if ((name == "eq?" || name == "not-eq?") && hasCapture && end == i + 3) {
                    test.kind = TreeSitterTestKind_Eq;
                    test.negated = name == "not-eq?";
                    test.capture = steps[i + 1].value_id;
                    if (steps[i + 2].type == TSQueryPredicateStepTypeCapture)
                        test.otherCapture = steps[i + 2].value_id;
                    else
                        test.strings.push(prRawStringCopy(stringAt(i + 2)));
                    kept = true;
                } else if ((name == "match?" || name == "not-match?") && hasCapture && end == i + 3 &&
                        steps[i + 2].type == TSQueryPredicateStepTypeString) {
                    test.kind = TreeSitterTestKind_Match;
                    test.negated = name == "not-match?";
                    test.capture = steps[i + 1].value_id;
                    prRegexCreate(stringAt(i + 2), &test.regex);
                    kept = true;
                } else if ((name == "any-of?" || name == "not-any-of?") && hasCapture) {
                    test.kind = TreeSitterTestKind_AnyOf;
                    test.negated = name == "not-any-of?";
                    test.capture = steps[i + 1].value_id;
                    for (uint32_t k = i + 2; k < end; ++k)
                        test.strings.push(prRawStringCopy(stringAt(k)));
                    kept = true;
                } else if (name == "set!" && end == i + 3 && steps[i + 1].type == TSQueryPredicateStepTypeString &&
                        stringAt(i + 1) == "injection.language") {
                    q.patterns.data[p].injectionLanguage = prRawStringCopy(stringAt(i + 2));
                } else if (name == "set!" && end == i + 2 && steps[i + 1].type == TSQueryPredicateStepTypeString) {
                    if (stringAt(i + 1) == "injection.combined")
                        q.patterns.data[p].combined = true;
                    else if (stringAt(i + 1) == "injection.include-children")
                        q.patterns.data[p].includeChildren = true;
                }
                // Other predicates, such as those of locals or of directives that move a range, hold for every match.
                if (kept)
                    q.patterns.data[p].tests.push(test);
            }
            i = end + 1;
        }
    }
}

void compileQuery(TreeSitterQuery& q, const TSLanguage* grammar, PrStringView source) {
    q.query = nullptr;
    q.contentCapture = UINT32_MAX;
    q.languageCapture = UINT32_MAX;
    if (source.size == 0)
        return;
    uint32_t errorOffset = 0;
    TSQueryError error = TSQueryErrorNone;
    q.query = ts_query_new(grammar, source.data, uint32_t(source.size), &errorOffset, &error);
    if (q.query == nullptr)
        return;
    readPredicates(q);
    for (uint32_t i = 0; i < ts_query_capture_count(q.query); ++i) {
        uint32_t length = 0;
        const char* chars = ts_query_capture_name_for_id(q.query, i, &length);
        PrStringView name = PrStringView(chars, length);
        if (name == "injection.content")
            q.contentCapture = i;
        else if (name == "injection.language")
            q.languageCapture = i;
    }
}

void resetQuery(TreeSitterQuery& q) {
    for (uint32_t p = 0; p < q.patterns.count; ++p) {
        TreeSitterPattern& pattern = q.patterns.data[p];
        for (uint32_t t = 0; t < pattern.tests.count; ++t) {
            TreeSitterTest& test = pattern.tests.data[t];
            for (uint32_t s = 0; s < test.strings.count; ++s)
                prRawStringReset(&test.strings.data[s]);
            test.strings.reset();
            prRegexDestroy(test.regex);
        }
        pattern.tests.reset();
        prRawStringReset(&pattern.injectionLanguage);
    }
    q.patterns.reset();
    if (q.query != nullptr)
        ts_query_delete(q.query);
    q.query = nullptr;
}

// Reads the buffer for tree-sitter, a chunk at a time.
const char* readBuffer(void* payload, uint32_t byte, TSPoint, uint32_t* bytesRead) {
    const PrTextEditBuffer* buffer = static_cast<const PrTextEditBuffer*>(payload);
    if (byte >= prTextEditBufferSize(buffer)) {
        *bytesRead = 0;
        return "";
    }
    PrStringView chunk = prTextEditBufferChunk(buffer, byte);
    *bytesRead = uint32_t(chunk.size);
    return chunk.data;
}

TSInput inputOf(const PrTextEditBuffer* buffer) {
    TSInput input{};
    input.payload = const_cast<PrTextEditBuffer*>(buffer);
    input.read = readBuffer;
    input.encoding = TSInputEncodingUTF8;
    return input;
}

TSPoint pointOf(PrTextEditPoint point) {
    return TSPoint{point.line, point.column};
}

// Returns the text of a node, copied into the tree's scratch, which stays valid until the next copy.
PrStringView textOf(TreeSitterTree* h, TSNode node) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    h->text.resize(end - start);
    prTextEditBufferCopy(h->buffer, start, end, h->text.data);
    return PrStringView{h->text.data, h->text.count};
}

// Returns the node of the first capture of `capture` in `match`, or a null node.
TSNode captured(const TSQueryMatch& match, uint32_t capture) {
    for (uint16_t i = 0; i < match.capture_count; ++i) {
        if (match.captures[i].index == capture)
            return match.captures[i].node;
    }
    return TSNode{};
}

// Returns true if every text predicate of the match's pattern holds.
bool passes(TreeSitterTree* h, const TreeSitterQuery& q, const TSQueryMatch& match) {
    const TreeSitterPattern& pattern = q.patterns.data[match.pattern_index];
    for (uint32_t t = 0; t < pattern.tests.count; ++t) {
        const TreeSitterTest& test = pattern.tests.data[t];
        TSNode node = captured(match, test.capture);
        if (ts_node_is_null(node))
            continue;
        bool holds = false;
        if (test.kind == TreeSitterTestKind_Eq && test.otherCapture != UINT32_MAX) {
            TSNode other = captured(match, test.otherCapture);
            PrRawString first = prRawStringCopy(textOf(h, node));
            holds = !ts_node_is_null(other) && prRawStringView(&first) == textOf(h, other);
            prRawStringReset(&first);
        } else {
            PrStringView text = textOf(h, node);
            if (test.kind == TreeSitterTestKind_Match) {
                holds = test.regex != nullptr && prRegexSearch(test.regex, text, nullptr, 0);
            } else {
                for (uint32_t s = 0; s < test.strings.count && !holds; ++s)
                    holds = prRawStringView(&test.strings.data[s]) == text;
            }
        }
        if (holds == test.negated)
            return false;
    }
    return true;
}

// Returns the map from the capture indices of `language`'s highlights query to the registry's categories.
const RawArray<PrSyntaxCategory>& categoryMapOf(TreeSitterTree* h, const TreeSitterLanguage* language) {
    for (uint32_t i = 0; i < h->mappedLanguages.count; ++i) {
        if (h->mappedLanguages.data[i] == language)
            return h->categoryMaps.data[i];
    }
    RawArray<PrSyntaxCategory> map;
    if (language->highlights.query != nullptr) {
        uint32_t count = ts_query_capture_count(language->highlights.query);
        map.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t length = 0;
            const char* name = ts_query_capture_name_for_id(language->highlights.query, i, &length);
            map.data[i] = prSyntaxRegistryCategoryIntern(h->registry, PrStringView(name, length));
        }
    }
    h->mappedLanguages.push(language);
    h->categoryMaps.push(map);
    return h->categoryMaps.last();
}

// Adds the captures of `language`'s highlights query in `root` that intersect the bytes from `start` to `end`.
void addCaptures(TreeSitterTree* h, const TreeSitterLanguage* language, TSNode root, uint32_t start,
        uint32_t end, uint8_t layer) {
    const TreeSitterQuery& q = language->highlights;
    if (q.query == nullptr)
        return;
    const RawArray<PrSyntaxCategory>& map = categoryMapOf(h, language);
    ts_query_cursor_set_byte_range(h->cursor, start, end);
    ts_query_cursor_exec(h->cursor, q.query, root);
    TSQueryMatch match{};
    while (ts_query_cursor_next_match(h->cursor, &match)) {
        if (!passes(h, q, match))
            continue;
        for (uint16_t i = 0; i < match.capture_count; ++i) {
            TSNode node = match.captures[i].node;
            uint32_t from = ts_node_start_byte(node);
            uint32_t to = ts_node_end_byte(node);
            PrStringView name{};
            uint32_t length = 0;
            const char* chars = ts_query_capture_name_for_id(q.query, match.captures[i].index, &length);
            name = PrStringView(chars, length);
            // A capture whose name starts with an underscore only serves a predicate. A match that intersects the range
            // can hold captures outside it.
            if (from >= to || (name.size != 0 && name.data[0] == '_') || to <= start || from >= end)
                continue;
            h->items.push(TreeSitterItem{from, to, map.data[match.captures[i].index], match.pattern_index, layer});
        }
    }
}

bool isTreeSitter(const PrSyntaxLanguage* language);

// The deepest that injections nest, as a script in an HTML block in Markdown's inline text in a Markdown block.
constexpr uint8_t kMaxInjectionDepth = 4;

// The ranges of the text that an injection hands to a language, which parses them as one document.
struct TreeSitterInjection {
    const TreeSitterLanguage* language;
    uint32_t pattern;               // of the host's injections query
    bool combined;
    RawArray<TSRange> ranges;
};

// Returns the tree's parser of an injected language.
TSParser* parserFor(TreeSitterTree* h, const TreeSitterLanguage* language) {
    for (uint32_t i = 0; i < h->injectedLanguages.count; ++i) {
        if (h->injectedLanguages.data[i] == language)
            return h->injectedParsers.data[i];
    }
    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, language->grammar);
    h->injectedLanguages.push(language);
    h->injectedParsers.push(parser);
    return parser;
}

// Returns true if two sets of ranges cover the same bytes.
bool sameBytes(const RawArray<TSRange>& a, const RawArray<TSRange>& b) {
    if (a.count != b.count)
        return false;
    for (uint32_t i = 0; i < a.count; ++i) {
        if (a.data[i].start_byte != b.data[i].start_byte || a.data[i].end_byte != b.data[i].end_byte)
            return false;
    }
    return true;
}

// Returns the tree of `language` over `ranges` at `depth`: the layer of the last query over the same bytes, a layer
// over ranges that overlap them, parsed again incrementally, or a new parse. Returns null if the parse fails.
TSTree* PR_NULLABLE layerTree(TreeSitterTree* h, const TreeSitterLanguage* language, uint8_t depth,
        const RawArray<TSRange>& ranges) {
    uint32_t first = ranges.data[0].start_byte;
    uint32_t last = ranges.last().end_byte;
    TreeSitterLayer* layer = nullptr;
    for (uint32_t i = 0; i < h->layers.count && layer == nullptr; ++i) {
        TreeSitterLayer& candidate = h->layers.data[i];
        if (candidate.used || candidate.language != language || candidate.depth != depth)
            continue;
        if (candidate.ranges.data[0].start_byte < last && candidate.ranges.last().end_byte > first)
            layer = &candidate;
    }
    if (layer != nullptr && !layer->edited && sameBytes(layer->ranges, ranges)) {
        layer->used = true;
        return layer->tree;
    }
    TSParser* parser = parserFor(h, language);
    if (!ts_parser_set_included_ranges(parser, ranges.data, ranges.count))
        return nullptr;
    TSTree* tree = ts_parser_parse(parser, layer != nullptr ? layer->tree : nullptr, inputOf(h->buffer));
    if (tree == nullptr)
        return nullptr;
    if (layer == nullptr) {
        h->layers.push(TreeSitterLayer{.language = language, .depth = depth});
        layer = &h->layers.last();
    } else {
        ts_tree_delete(layer->tree);
    }
    layer->tree = tree;
    layer->edited = false;
    layer->used = true;
    layer->ranges.clear();
    layer->ranges.append(ranges.span());
    return tree;
}

// Moves the layers by an edit of the text, and marks those whose ranges it touches.
void editLayers(TreeSitterTree* h, const TSInputEdit& edit) {
    int64_t change = int64_t(edit.new_end_byte) - int64_t(edit.old_end_byte);
    for (uint32_t i = 0; i < h->layers.count; ++i) {
        TreeSitterLayer& layer = h->layers.data[i];
        ts_tree_edit(layer.tree, &edit);
        for (uint32_t r = 0; r < layer.ranges.count; ++r) {
            TSRange& range = layer.ranges.data[r];
            if (range.end_byte < edit.start_byte)
                continue;
            if (range.start_byte > edit.old_end_byte) {
                range.start_byte = uint32_t(int64_t(range.start_byte) + change);
                range.end_byte = uint32_t(int64_t(range.end_byte) + change);
                continue;
            }
            layer.edited = true;
            if (range.end_byte >= edit.old_end_byte)
                range.end_byte = uint32_t(int64_t(range.end_byte) + change);
            else
                range.end_byte = edit.new_end_byte;
        }
    }
}

// Frees the layers that the last query did not use, as those that left the window, and readies the rest for the next.
void dropUnusedLayers(TreeSitterTree* h) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < h->layers.count; ++i) {
        TreeSitterLayer& layer = h->layers.data[i];
        if (!layer.used) {
            ts_tree_delete(layer.tree);
            layer.ranges.reset();
            continue;
        }
        layer.used = false;
        h->layers.data[kept++] = layer;
    }
    h->layers.count = kept;
}

// Adds the ranges of an injection's content node: the whole node, or the stretches between its named children. A
// grammar's anonymous children are its punctuation, as Markdown's are, which belongs to the injected text.
void addContent(RawArray<TSRange>& ranges, TSNode content, bool includeChildren) {
    TSRange whole{
        ts_node_start_point(content), ts_node_end_point(content), ts_node_start_byte(content),
        ts_node_end_byte(content),
    };
    if (includeChildren) {
        ranges.push(whole);
        return;
    }
    TSRange stretch = whole;
    for (uint32_t i = 0; i < ts_node_named_child_count(content); ++i) {
        TSNode child = ts_node_named_child(content, i);
        if (ts_node_start_byte(child) > stretch.start_byte) {
            stretch.end_byte = ts_node_start_byte(child);
            stretch.end_point = ts_node_start_point(child);
            ranges.push(stretch);
        }
        stretch.start_byte = ts_node_end_byte(child);
        stretch.start_point = ts_node_end_point(child);
    }
    if (whole.end_byte > stretch.start_byte) {
        stretch.end_byte = whole.end_byte;
        stretch.end_point = whole.end_point;
        ranges.push(stretch);
    }
}

// Returns the language of an injection's match: the one that its pattern names, or that the text of its language
// capture names, or null if the registry has no tree-sitter language of that name.
const TreeSitterLanguage* PR_NULLABLE injectedLanguageOf(TreeSitterTree* h, const TreeSitterQuery& q,
        const TSQueryMatch& match) {
    PrStringView name = prRawStringView(&q.patterns.data[match.pattern_index].injectionLanguage);
    PrRawString fromText{};
    if (name.size == 0 && q.languageCapture != UINT32_MAX) {
        TSNode named = captured(match, q.languageCapture);
        if (!ts_node_is_null(named)) {
            fromText = prRawStringCopy(textOf(h, named));
            name = prRawStringView(&fromText);
        }
    }
    const PrSyntaxLanguage* language = name.size != 0 ? prSyntaxRegistryFind(h->registry, name) : nullptr;
    prRawStringReset(&fromText);
    if (language == nullptr || !isTreeSitter(language))
        return nullptr;
    return static_cast<const TreeSitterLanguage*>(language);
}

// Parses afresh each document that the injections of `host` in `root` hand to another tree-sitter language of the
// registry within the bytes from `start` to `end`, adds its categories at `depth`, and injects into it in turn.
void inject(TreeSitterTree* h, const TreeSitterLanguage* host, TSNode root, uint32_t start, uint32_t end, uint8_t depth) {
    const TreeSitterQuery& q = host->injections;
    if (q.query == nullptr || q.contentCapture == UINT32_MAX || depth > kMaxInjectionDepth)
        return;
    // The matches are read first, since parsing an injection and adding its categories use cursors of their own.
    RawArray<TreeSitterInjection> found;
    TSQueryCursor* cursor = ts_query_cursor_new();
    ts_query_cursor_set_byte_range(cursor, start, end);
    ts_query_cursor_exec(cursor, q.query, root);
    TSQueryMatch match{};
    while (ts_query_cursor_next_match(cursor, &match)) {
        const TreeSitterLanguage* language = injectedLanguageOf(h, q, match);
        if (language == nullptr)
            continue;
        const TreeSitterPattern& pattern = q.patterns.data[match.pattern_index];
        TreeSitterInjection* into = nullptr;
        for (uint32_t i = 0; pattern.combined && i < found.count && into == nullptr; ++i) {
            TreeSitterInjection& kept = found.data[i];
            if (kept.combined && kept.pattern == match.pattern_index && kept.language == language)
                into = &kept;
        }
        if (into == nullptr) {
            found.push(TreeSitterInjection{language, match.pattern_index, pattern.combined, {}});
            into = &found.last();
        }
        for (uint16_t i = 0; i < match.capture_count; ++i) {
            if (match.captures[i].index == q.contentCapture)
                addContent(into->ranges, match.captures[i].node, pattern.includeChildren);
        }
    }
    ts_query_cursor_delete(cursor);
    for (uint32_t i = 0; i < found.count; ++i) {
        TreeSitterInjection& injection = found.data[i];
        // A parse takes its ranges in order, without overlap.
        RawArray<TSRange>& ranges = injection.ranges;
        sortItems(ranges.span(), [](const TSRange& a, const TSRange& b) { return a.start_byte < b.start_byte; });
        uint32_t kept = 0;
        for (uint32_t r = 0; r < ranges.count; ++r) {
            if (kept == 0 || ranges.data[r].start_byte >= ranges.data[kept - 1].end_byte)
                ranges.data[kept++] = ranges.data[r];
        }
        ranges.count = kept;
        TSTree* tree = kept != 0 ? layerTree(h, injection.language, depth, ranges) : nullptr;
        if (tree != nullptr) {
            TSNode injected = ts_tree_root_node(tree);
            addCaptures(h, injection.language, injected, start, end, depth);
            inject(h, injection.language, injected, start, end, uint8_t(depth + 1));
        }
        ranges.reset();
    }
    found.reset();
}

// Returns true if `a` names its bytes over `b`: an injection's categories over the language's own, then the inner
// category, then the one that the grammar lists last, as tree-sitter's own highlighter chooses, so that a query lists
// its general patterns first.
bool outranks(const TreeSitterItem& a, const TreeSitterItem& b) {
    if (a.layer != b.layer)
        return a.layer > b.layer;
    uint32_t aSize = a.end - a.start;
    uint32_t bSize = b.end - b.start;
    if (aSize != bSize)
        return aSize < bSize;
    return a.pattern > b.pattern;
}

// Writes the items as spans that do not overlap, each piece of the text named by the item that outranks the others over
// it.
uint32_t flatten(TreeSitterTree* h, PrSyntaxSpan* PR_NULLABLE out, uint32_t capacity) {
    RawArray<TreeSitterItem>& items = h->items;
    // By start, so that a sweep adds each item where it begins.
    sortItems(items.span(), [](const TreeSitterItem& a, const TreeSitterItem& b) { return a.start < b.start; });
    h->bounds.clear();
    for (uint32_t i = 0; i < items.count; ++i) {
        h->bounds.push(items.data[i].start);
        h->bounds.push(items.data[i].end);
    }
    sortItems(h->bounds.span(), [](uint32_t a, uint32_t b) { return a < b; });
    // The last span is written once the next cannot extend it, so that every capacity gives the same count.
    uint32_t count = 0;
    PrSyntaxSpan last{};
    bool hasLast = false;
    auto emit = [&]() {
        if (count < capacity)
            out[count] = last;
        count += 1;
    };
    uint32_t next = 0;
    h->active.clear();
    for (uint32_t b = 0; b + 1 < h->bounds.count; ++b) {
        uint32_t from = h->bounds.data[b];
        uint32_t to = h->bounds.data[b + 1];
        if (from == to)
            continue;
        while (next < items.count && items.data[next].start <= from)
            h->active.push(next++);
        uint32_t best = UINT32_MAX;
        for (uint32_t a = 0; a < h->active.count;) {
            const TreeSitterItem& item = items.data[h->active.data[a]];
            if (item.end <= from) {
                h->active.data[a] = h->active.last();
                h->active.count -= 1;
                continue;
            }
            if (best == UINT32_MAX || outranks(item, items.data[best]))
                best = h->active.data[a];
            ++a;
        }
        if (best == UINT32_MAX)
            continue;
        PrSyntaxCategory category = items.data[best].category;
        if (hasLast && last.end == from && last.category == category) {
            last.end = to;
            continue;
        }
        if (hasLast)
            emit();
        last = PrSyntaxSpan{from, to, category};
        hasLast = true;
    }
    if (hasLast)
        emit();
    return count;
}

void treeDestroy(PrSyntaxTree* tree) {
    TreeSitterTree* h = static_cast<TreeSitterTree*>(tree);
    if (h->parsed != nullptr)
        ts_tree_delete(h->parsed);
    ts_parser_delete(h->parser);
    ts_query_cursor_delete(h->cursor);
    prTextEditStreamDestroy(h->stream);
    for (uint32_t i = 0; i < h->categoryMaps.count; ++i)
        h->categoryMaps.data[i].reset();
    h->categoryMaps.reset();
    h->mappedLanguages.reset();
    for (uint32_t i = 0; i < h->injectedParsers.count; ++i)
        ts_parser_delete(h->injectedParsers.data[i]);
    h->injectedParsers.reset();
    h->injectedLanguages.reset();
    for (uint32_t i = 0; i < h->layers.count; ++i) {
        ts_tree_delete(h->layers.data[i].tree);
        h->layers.data[i].ranges.reset();
    }
    h->layers.reset();
    h->items.reset();
    h->active.reset();
    h->bounds.reset();
    h->text.reset();
    prMemoryFree(h);
}

// How many of a parse's reports of its progress pass between looks at the clock.
constexpr uint32_t kProgressStride = 64;

// Returns true if the parse has run out of time, which halts it.
bool outOfTime(TSParseState* state) {
    TreeSitterTree* h = static_cast<TreeSitterTree*>(state->payload);
    if (++h->reports % kProgressStride != 0)
        return false;
    return prTimeMonotonicMilliseconds() > h->deadline;
}

bool treeUpdate(PrSyntaxTree* tree, double milliseconds) {
    TreeSitterTree* h = static_cast<TreeSitterTree*>(tree);
    PrTextEditRecord records[64];
    bool edited = false;
    for (;;) {
        uint32_t count = prTextEditStreamTake(h->stream, records, 64);
        if (count == 0)
            break;
        edited = true;
        // An edit before the first parse is in the text that the parse reads.
        if (h->parsed == nullptr)
            continue;
        for (uint32_t i = 0; i < count; ++i) {
            const PrTextEditRecord& r = records[i];
            TSInputEdit edit{
                r.start, r.oldEnd, r.newEnd,
                pointOf(r.startPoint), pointOf(r.oldEndPoint), pointOf(r.newEndPoint),
            };
            ts_tree_edit(h->parsed, &edit);
            editLayers(h, edit);
        }
    }
    // A halted parse read the text as it stood, so an edit starts it again from the moved tree.
    if (edited && h->halted) {
        ts_parser_reset(h->parser);
        h->halted = false;
    }
    if (h->parsed != nullptr && !edited && !h->halted)
        return false;
    h->deadline = prTimeMonotonicMilliseconds() + milliseconds;
    TSParseOptions options{};
    options.payload = h;
    options.progress_callback = milliseconds < kPrF64Infinity ? outOfTime : nullptr;
    TSTree* parsed = ts_parser_parse_with_options(h->parser, h->parsed, inputOf(h->buffer), options);
    if (parsed == nullptr) {
        h->halted = true;
        return false;
    }
    h->halted = false;
    if (h->parsed != nullptr)
        ts_tree_delete(h->parsed);
    h->parsed = parsed;
    return true;
}

bool treePending(const PrSyntaxTree* tree) {
    return static_cast<const TreeSitterTree*>(tree)->halted;
}

uint32_t treeSpans(PrSyntaxTree* tree, uint32_t start, uint32_t end, PrSyntaxSpan* PR_NULLABLE out, uint32_t capacity) {
    TreeSitterTree* h = static_cast<TreeSitterTree*>(tree);
    if (h->parsed == nullptr)
        return 0;
    h->items.clear();
    TSNode root = ts_tree_root_node(h->parsed);
    addCaptures(h, h->language, root, start, end, 0);
    inject(h, h->language, root, start, end, 1);
    dropUnusedLayers(h);
    return flatten(h, out, capacity);
}

PrSyntaxTree* treeCreate(const PrSyntaxLanguage* language, PrSyntaxRegistry* registry, PrTextEditBuffer* buffer) {
    TreeSitterTree* h = allocZeroed<TreeSitterTree>();
    h->destroy = treeDestroy;
    h->update = treeUpdate;
    h->spans = treeSpans;
    h->pending = treePending;
    h->language = static_cast<const TreeSitterLanguage*>(language);
    h->registry = registry;
    h->buffer = buffer;
    h->stream = prTextEditStreamCreate(buffer);
    h->parser = ts_parser_new();
    ts_parser_set_language(h->parser, h->language->grammar);
    h->cursor = ts_query_cursor_new();
    return h;
}

bool isTreeSitter(const PrSyntaxLanguage* language) {
    return language->create == treeCreate;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterLanguageCreate(const PrSyntaxTreeSitterDesc* desc) {
    useAllocator();
    TreeSitterLanguage* l = allocZeroed<TreeSitterLanguage>();
    l->ownName = prRawStringCopy(desc->name);
    l->name = prRawStringView(&l->ownName);
    l->ownNamePattern = prRawStringCopy(desc->namePattern);
    l->namePattern = prRawStringView(&l->ownNamePattern);
    for (uint32_t i = 0; i < desc->extensionCount; ++i)
        l->ownExtensions.push(prRawStringCopy(desc->extensions[i]));
    for (uint32_t i = 0; i < desc->fileNameCount; ++i)
        l->ownFileNames.push(prRawStringCopy(desc->fileNames[i]));
    for (uint32_t i = 0; i < l->ownExtensions.count; ++i)
        l->extensionViews.push(prRawStringView(&l->ownExtensions.data[i]));
    for (uint32_t i = 0; i < l->ownFileNames.count; ++i)
        l->fileNameViews.push(prRawStringView(&l->ownFileNames.data[i]));
    l->extensions = l->extensionViews.data;
    l->extensionCount = l->extensionViews.count;
    l->fileNames = l->fileNameViews.data;
    l->fileNameCount = l->fileNameViews.count;
    l->create = treeCreate;
    l->grammar = static_cast<const TSLanguage*>(desc->grammar);
    compileQuery(l->highlights, l->grammar, desc->highlights);
    compileQuery(l->injections, l->grammar, desc->injections);
    return l;
}

void prSyntaxTreeSitterLanguageDestroy(const PrSyntaxLanguage* PR_NULLABLE language) {
    if (language == nullptr)
        return;
    TreeSitterLanguage* l = const_cast<TreeSitterLanguage*>(static_cast<const TreeSitterLanguage*>(language));
    resetQuery(l->highlights);
    resetQuery(l->injections);
    prRawStringReset(&l->ownName);
    prRawStringReset(&l->ownNamePattern);
    for (uint32_t i = 0; i < l->ownExtensions.count; ++i)
        prRawStringReset(&l->ownExtensions.data[i]);
    for (uint32_t i = 0; i < l->ownFileNames.count; ++i)
        prRawStringReset(&l->ownFileNames.data[i]);
    l->ownExtensions.reset();
    l->ownFileNames.reset();
    l->extensionViews.reset();
    l->fileNameViews.reset();
    prMemoryFree(l);
}
