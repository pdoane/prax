// Markdown: CommonMark and its extensions, parsed into a document that a client walks.

#include "pr/markdown.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/hash.h"
#include "pr/hash_index.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/raw_array.h"
#include "pr/sort.h"
#include "pr/string_ops.h"
#include "pr/string_trim.h"

#include "markdown/src/markdown_block.h"
#include "markdown/src/markdown_inline.h"
#include "markdown/src/markdown_scan.h"
#include "markdown/src/markdown_tree.h"

namespace pr_impl {

namespace {

// The position of no block, and no change.
constexpr uint32_t kBlockNone = 0xffffffffu;

// A footnote definition or reference of a top-level block.
struct MarkdownFootnoteMark {
    uint32_t node;                  // by position in the block's nodes
    uint32_t hash;
    MarkdownRange label;            // the normalized label, as a range of the block's footnote labels
    bool definition;
};

// A top-level block: its nodes in document order, and the text that their views range over.
struct MarkdownTopBlock {
    uint32_t sourceStart;           // the offset in the source of the block's first line
    uint32_t sourceEnd;             // the offset in the source of the line after the block's last line
    RawArray<MarkdownNode> nodes;
    RawArray<char> text;
    bool resolved;                  // true if the block was built again for a late reference since the last change

    // The block's footnote definitions and references in document order, and where its references' resolutions start in
    // the document's footnote uses. A reference's `start` value is its position among the block's references.
    RawArray<MarkdownFootnoteMark> footnoteMarks;
    RawArray<char> footnoteLabels;
    uint32_t firstUse;

    // The block's headings, whose `start` values are their positions among them, and where their ids start in the
    // document's ids.
    uint32_t headingCount;
    uint32_t firstHeading;
};

// A slug that the document's headings use, with how many headings after the first took it with a number.
struct MarkdownSlug {
    uint32_t hash;
    MarkdownRange text;             // a range of the document's slug text
    uint32_t count;
};

// What a footnote reference resolves to: the definition that has its label, and which reference to it this is. The
// number is 0 if no definition has the label.
struct MarkdownFootnoteUse {
    uint32_t number;
    uint32_t reference;
    uint32_t block;
    uint32_t node;
};

// A footnote that the document references: its definition, and how many references it has.
struct MarkdownFootnote {
    uint32_t block;
    uint32_t node;
    uint32_t referenceCount;
};

// The first definition of a footnote label, found by a pass over the document.
struct MarkdownFootnoteDefinition {
    uint32_t hash;
    uint32_t block;
    uint32_t mark;                  // by position in the block's footnote marks
    uint32_t number;
    uint32_t referenceCount;
};

// A link reference definition, whose views are ranges of the text of the table that holds it.
struct MarkdownDefinitionEntry {
    uint32_t hash;
    MarkdownRange label;
    MarkdownRange destination;
    MarkdownRange title;
};

// Link reference definitions in document order. A label's first definition is the one that matches it.
struct MarkdownDefinitions {
    RawArray<MarkdownDefinitionEntry> entries;
    RawArray<char> text;
    HashIndex index;                // by hash, over entries
};

// A reference that a block looked up and that no final definition matched. A definition that comes later can match it,
// and the block is then built again.
struct MarkdownLookup {
    uint32_t hash;
    uint32_t block;
};

struct Markdown {
    PrMarkdownOptions options;
    RawArray<char> source;
    bool ended;

    // The blocks of the document are the first `blockCount` of `blocks`. The slots after them keep their storage for
    // the blocks that a later parse makes.
    RawArray<MarkdownTopBlock> blocks;
    uint32_t blockCount;
    uint32_t finalCount;
    uint32_t tailStart;             // the offset in the source where the tentative blocks start

    // The definitions in final blocks, and those in tentative blocks, which are found again by each parse.
    MarkdownDefinitions definitions;
    MarkdownDefinitions tailDefinitions;
    RawArray<MarkdownLookup> lookups;
    uint32_t building;              // the block whose inlines are being parsed, which a lookup is recorded for

    // The changes since the client last took them: the blocks from `changedFrom` on were replaced, and the blocks in
    // `resolvedBlocks` before it were built again. `reportedCount` is how many blocks the client has. The queue holds
    // changes that a take made and has not yet written.
    uint32_t reportedCount;
    uint32_t changedFrom;
    RawArray<uint32_t> resolvedBlocks;
    RawArray<PrMarkdownChange> queue;
    uint32_t queueHead;

    // The resolution of each footnote reference, by position in document order, and the footnotes by number less 1,
    // from the last pass over the document and the pass before it. The definitions are the pass's index of labels.
    RawArray<MarkdownFootnoteUse> uses;
    RawArray<MarkdownFootnoteUse> previousUses;
    RawArray<MarkdownFootnote> footnotes;
    RawArray<MarkdownFootnote> previousFootnotes;
    RawArray<MarkdownFootnoteDefinition> footnoteDefinitions;
    HashIndex footnoteIndex;        // by hash, over footnoteDefinitions
    bool footnotesChanged;

    // The id of each heading, by position in document order, as ranges of the id text, from the last pass over the
    // document and the pass before it. The slugs are the pass's index of the ids that headings took.
    RawArray<MarkdownRange> ids;
    RawArray<MarkdownRange> previousIds;
    RawArray<char> idText;
    RawArray<char> previousIdText;
    RawArray<MarkdownSlug> slugs;
    RawArray<char> slugText;
    HashIndex slugIndex;            // by hash, over slugs

    MarkdownBlockParser blockParser;
    MarkdownInlineParser inlineParser;
    MarkdownTree tree;
    RawArray<uint32_t> changedHashes;
};

uint32_t hashOf(PrStringView label) {
    return uint32_t(prHashBytes(label.data, label.size, 0));
}

PrStringView textOf(const MarkdownDefinitions& table, MarkdownRange range) {
    return PrStringView{table.text.data + range.start, range.size};
}

void definitionsClear(MarkdownDefinitions* table) {
    table->entries.clear();
    table->text.clear();
    table->index.clear();
}

void definitionsReset(MarkdownDefinitions* table) {
    table->entries.reset();
    table->text.reset();
    table->index.reset();
}

// Returns the first definition of `label` in `table`, or null if there is none.
const MarkdownDefinitionEntry* definitionsFind(const MarkdownDefinitions& table, PrStringView label, uint32_t hash) {
    uint32_t at = table.index.find(hash, [&](uint32_t entry) {
        return textOf(table, table.entries.data[entry].label) == label;
    });
    return at == kHashMissing ? nullptr : &table.entries.data[at];
}

// Adds a definition of `label` to `table` unless the table defines it already. Returns true if it was added.
bool definitionsAdd(MarkdownDefinitions* table, PrStringView label, PrStringView destination, PrStringView title) {
    uint32_t hash = hashOf(label);
    if (definitionsFind(*table, label, hash) != nullptr)
        return false;
    auto add = [&](PrStringView text) {
        MarkdownRange range{table->text.count, uint32_t(text.size)};
        if (text.size != 0)
            table->text.append(text.data, uint32_t(text.size));
        return range;
    };
    MarkdownDefinitionEntry entry{};
    entry.hash = hash;
    entry.label = add(label);
    entry.destination = add(destination);
    entry.title = add(title);
    table->index.insert(hash, table->entries.count);
    table->entries.push(entry);
    return true;
}

bool resolve(void* PR_NULLABLE context, PrStringView label, MarkdownDefinition* out) {
    Markdown* document = static_cast<Markdown*>(context);
    uint32_t hash = hashOf(label);
    const MarkdownDefinitions* table = &document->definitions;
    const MarkdownDefinitionEntry* entry = definitionsFind(*table, label, hash);
    if (entry == nullptr) {
        document->lookups.push(MarkdownLookup{hash, document->building});
        table = &document->tailDefinitions;
        entry = definitionsFind(*table, label, hash);
    }
    if (entry == nullptr)
        return false;
    out->destination = textOf(*table, entry->destination);
    out->title = textOf(*table, entry->title);
    return true;
}

PrStringView parserText(const MarkdownBlockParser& parser, MarkdownRange range) {
    return PrStringView{parser.text.data + range.start, range.size};
}

PrStringView viewOf(const MarkdownTopBlock& block, MarkdownRange range) {
    return PrStringView{block.text.data + range.start, range.size};
}

// Returns the slot for a new block at the end of the document, emptied.
MarkdownTopBlock& addBlock(Markdown* document) {
    if (document->blockCount == document->blocks.count) {
        MarkdownTopBlock& added = document->blocks.emplace();
        prMemZero(&added, sizeof(added));
    }
    MarkdownTopBlock& block = document->blocks.data[document->blockCount++];
    block.nodes.clear();
    block.text.clear();
    block.resolved = false;
    block.footnoteMarks.clear();
    block.footnoteLabels.clear();
    block.firstUse = 0;
    block.headingCount = 0;
    block.firstHeading = 0;
    return block;
}

void swapText(RawArray<char>* a, RawArray<char>* b) {
    RawArray<char> held = *a;
    *a = *b;
    *b = held;
}

// Builds the child `child` of the root of the block parser's last parse into the block at `index`.
void build(Markdown* document, MarkdownBlockIndex child, uint32_t index) {
    MarkdownTopBlock& block = document->blocks.data[index];
    document->building = index;
    markdownTreeClear(&document->tree);
    markdownBuildBlock(&document->blockParser, document->options, child, &document->tree, &document->inlineParser,
        resolve, document);
    block.nodes.clear();
    markdownTreeFlatten(&document->tree, &block.nodes);
    swapText(&block.text, &document->tree.text);

    block.footnoteMarks.clear();
    block.footnoteLabels.clear();
    uint32_t references = 0;
    for (uint32_t i = 0; i < block.nodes.count; ++i) {
        MarkdownValues& values = block.nodes.data[i].values;
        bool definition = values.kind == PrMarkdownNodeKind_FootnoteDefinition;
        if (!definition && values.kind != PrMarkdownNodeKind_FootnoteReference)
            continue;
        if (!definition)
            values.start = references++;
        uint32_t mark = block.footnoteLabels.count;
        markdownAppendNormalizedLabel(PrStringView{block.text.data + values.info.start, values.info.size},
            &block.footnoteLabels);
        MarkdownRange label{mark, block.footnoteLabels.count - mark};
        uint32_t hash = hashOf(PrStringView{block.footnoteLabels.data + mark, label.size});
        block.footnoteMarks.push(MarkdownFootnoteMark{i, hash, label, definition});
    }

    block.headingCount = 0;
    if ((document->options.extensions & PrMarkdownExtensionFlag_HeadingIds) != 0) {
        for (uint32_t i = 0; i < block.nodes.count; ++i) {
            if (block.nodes.data[i].values.kind == PrMarkdownNodeKind_Heading)
                block.nodes.data[i].values.start = block.headingCount++;
        }
    }
}

// Builds a front matter block of `content` into the block at `index` (PrMarkdownExtensionFlag_FrontMatter).
void buildFrontMatter(Markdown* document, PrStringView content, uint32_t index) {
    MarkdownTopBlock& block = document->blocks.data[index];
    markdownTreeClear(&document->tree);
    MarkdownNodeIndex node = markdownTreeAppend(&document->tree, 0, PrMarkdownNodeKind_FrontMatter);
    if (content.size != 0) {
        MarkdownRange range = markdownTreeAddText(&document->tree, content);
        MarkdownNodeIndex text = markdownTreeAppend(&document->tree, node, PrMarkdownNodeKind_Text);
        document->tree.nodes.data[text].values.text = range;
    }
    block.nodes.clear();
    markdownTreeFlatten(&document->tree, &block.nodes);
    swapText(&block.text, &document->tree.text);
}

// What the start of a source holds, as front matter.
enum {
    FrontMatter_None,               // no front matter
    FrontMatter_Found,
    FrontMatter_Pending,            // a source that is open may yet start with front matter
};
typedef uint8_t FrontMatter;

// Returns true if `line` is a fence of front matter: three hyphens, or three periods if `closing`, and spaces or tabs.
bool isFrontMatterFence(PrStringView line, bool closing) {
    PrStringView fence = stringTrimBy(line, [](char c) { return c == ' ' || c == '\t' || c == '\r'; });
    return fence == "---" || (closing && fence == "...");
}

// Finds the front matter at the start of `source`. If it is found, writes to `*outContent` the lines between its
// fences, and to `*outEnd` the offset after its closing fence.
FrontMatter findFrontMatter(PrStringView source, bool ended, PrStringView* outContent, size_t* outEnd) {
    size_t first = prStringFindByte(source, '\n');
    if (first == source.size) {
        // A first line that is not complete may yet be a fence.
        PrStringView line = stringTrimBy(source, [](char c) { return c == ' ' || c == '\t' || c == '\r'; });
        bool prefix = line.size <= 3 && prStringStartsWith("---", line);
        return !ended && prefix ? FrontMatter_Pending : FrontMatter_None;
    }
    if (!isFrontMatterFence(prStringSlice(source, 0, first), false))
        return FrontMatter_None;
    size_t start = first + 1;
    size_t line = start;
    while (line < source.size) {
        size_t end = line + prStringFindByte(prStringFrom(source, line), '\n');
        bool complete = end < source.size;
        if ((complete || ended) && isFrontMatterFence(prStringSlice(source, line, end - line), true)) {
            *outContent = prStringSlice(source, start, line - start);
            *outEnd = complete ? end + 1 : end;
            return FrontMatter_Found;
        }
        line = complete ? end + 1 : source.size;
    }
    return ended ? FrontMatter_None : FrontMatter_Pending;
}

void markChangedFrom(Markdown* document, uint32_t block) {
    if (document->changedFrom == kBlockNone || block < document->changedFrom)
        document->changedFrom = block;
}

void markResolved(Markdown* document, uint32_t index) {
    MarkdownTopBlock& block = document->blocks.data[index];
    if (block.resolved || (document->changedFrom != kBlockNone && index >= document->changedFrom))
        return;
    block.resolved = true;
    document->resolvedBlocks.push(index);
}

// Removes the lookups of the blocks for which `removes` returns true.
template <typename Removes>
void removeLookups(Markdown* document, Removes removes) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < document->lookups.count; ++i) {
        if (!removes(document->lookups.data[i]))
            document->lookups.data[kept++] = document->lookups.data[i];
    }
    document->lookups.resize(kept);
}

bool hasHash(const RawArray<uint32_t>& hashes, uint32_t hash) {
    for (uint32_t i = 0; i < hashes.count; ++i) {
        if (hashes.data[i] == hash)
            return true;
    }
    return false;
}

// Builds again each final block before `before` that looked up a label whose definition may have changed: a label whose
// hash is among the document's changed hashes.
void resolveLate(Markdown* document, uint32_t before) {
    if (document->changedHashes.count == 0)
        return;
    RawArray<uint32_t> rebuilt;
    for (uint32_t i = 0; i < document->lookups.count; ++i) {
        const MarkdownLookup& lookup = document->lookups.data[i];
        if (lookup.block < before && hasHash(document->changedHashes, lookup.hash) &&
                rebuilt.find(lookup.block) == kArrayMissing)
            rebuilt.push(lookup.block);
    }
    for (uint32_t i = 0; i < rebuilt.count; ++i) {
        uint32_t index = rebuilt.data[i];
        removeLookups(document, [&](const MarkdownLookup& lookup) { return lookup.block == index; });
        // A final block parses the same alone as it did among the blocks around it, since it started a child of the
        // root and its lines end where the next one starts.
        const MarkdownTopBlock& block = document->blocks.data[index];
        PrStringView source{document->source.data, size_t(block.sourceEnd)};
        markdownParseBlocks(&document->blockParser, document->options, source, block.sourceStart, true);
        MarkdownBlockIndex child = document->blockParser.blocks.data[0].first;
        PR_ASSERT(child != kMarkdownBlockNone);
        build(document, child, index);
        markResolved(document, index);
    }
    rebuilt.reset();
}

PrStringView footnoteLabel(const MarkdownTopBlock& block, const MarkdownFootnoteMark& mark) {
    return PrStringView{block.footnoteLabels.data + mark.label.start, mark.label.size};
}

template <typename T>
void swapArrays(RawArray<T>* a, RawArray<T>* b) {
    RawArray<T> held = *a;
    *a = *b;
    *b = held;
}

// Resolves the footnote references of the document and numbers its footnotes in the order of their first references. A
// block before `rebuiltFrom` that was not built again, and whose references resolve differently than before, is
// reported as changed.
void resolveFootnotes(Markdown* document, uint32_t rebuiltFrom) {
    bool marked = false;
    for (uint32_t b = 0; b < document->blockCount && !marked; ++b)
        marked = document->blocks.data[b].footnoteMarks.count != 0;
    if (!marked && document->uses.count == 0 && document->footnotes.count == 0)
        return;

    swapArrays(&document->uses, &document->previousUses);
    swapArrays(&document->footnotes, &document->previousFootnotes);
    document->uses.clear();
    document->footnotes.clear();
    document->footnoteDefinitions.clear();
    document->footnoteIndex.clear();
    auto find = [&](const MarkdownTopBlock& block, const MarkdownFootnoteMark& mark) {
        return document->footnoteIndex.find(mark.hash, [&](uint32_t at) {
            const MarkdownFootnoteDefinition& entry = document->footnoteDefinitions.data[at];
            const MarkdownTopBlock& owner = document->blocks.data[entry.block];
            return footnoteLabel(owner, owner.footnoteMarks.data[entry.mark]) == footnoteLabel(block, mark);
        });
    };
    for (uint32_t b = 0; b < document->blockCount; ++b) {
        const MarkdownTopBlock& block = document->blocks.data[b];
        for (uint32_t m = 0; m < block.footnoteMarks.count; ++m) {
            const MarkdownFootnoteMark& mark = block.footnoteMarks.data[m];
            if (!mark.definition || find(block, mark) != kHashMissing)
                continue;
            document->footnoteIndex.insert(mark.hash, document->footnoteDefinitions.count);
            document->footnoteDefinitions.push(MarkdownFootnoteDefinition{mark.hash, b, m, 0, 0});
        }
    }

    for (uint32_t b = 0; b < document->blockCount; ++b) {
        MarkdownTopBlock& block = document->blocks.data[b];
        uint32_t previousFirst = block.firstUse;
        block.firstUse = document->uses.count;
        for (uint32_t m = 0; m < block.footnoteMarks.count; ++m) {
            const MarkdownFootnoteMark& mark = block.footnoteMarks.data[m];
            if (mark.definition)
                continue;
            uint32_t at = find(block, mark);
            MarkdownFootnoteUse use{};
            if (at != kHashMissing) {
                MarkdownFootnoteDefinition& definition = document->footnoteDefinitions.data[at];
                const MarkdownTopBlock& owner = document->blocks.data[definition.block];
                if (definition.number == 0) {
                    document->footnotes.push(
                        MarkdownFootnote{definition.block, owner.footnoteMarks.data[definition.mark].node, 0});
                    definition.number = document->footnotes.count;
                }
                use = MarkdownFootnoteUse{
                    definition.number, ++definition.referenceCount, definition.block,
                    owner.footnoteMarks.data[definition.mark].node,
                };
                document->footnotes.data[definition.number - 1].referenceCount = definition.referenceCount;
            }
            document->uses.push(use);
        }
        if (b >= rebuiltFrom || block.resolved)
            continue;
        uint32_t count = document->uses.count - block.firstUse;
        bool same = previousFirst + count <= document->previousUses.count;
        for (uint32_t i = 0; same && i < count; ++i)
            same = prMemEquals(&document->previousUses.data[previousFirst + i], &document->uses.data[block.firstUse + i],
                sizeof(MarkdownFootnoteUse));
        if (!same)
            markResolved(document, b);
    }

    // The footnotes change if they are other footnotes, or if a definition that they show was built again.
    bool changed = document->footnotes.count != document->previousFootnotes.count;
    for (uint32_t i = 0; !changed && i < document->footnotes.count; ++i) {
        const MarkdownFootnote& footnote = document->footnotes.data[i];
        changed = !prMemEquals(&footnote, &document->previousFootnotes.data[i], sizeof(MarkdownFootnote)) ||
            footnote.block >= rebuiltFrom || document->blocks.data[footnote.block].resolved;
    }
    document->footnotesChanged = document->footnotesChanged || changed;
}

// Gives each heading its id: its slug, or the slug and a number if an earlier heading took the slug. A block before
// `rebuiltFrom` that was not built again, and whose ids changed, is reported as changed.
void resolveHeadingIds(Markdown* document, uint32_t rebuiltFrom) {
    if ((document->options.extensions & PrMarkdownExtensionFlag_HeadingIds) == 0)
        return;
    swapArrays(&document->ids, &document->previousIds);
    swapArrays(&document->idText, &document->previousIdText);
    document->ids.clear();
    document->idText.clear();
    document->slugs.clear();
    document->slugText.clear();
    document->slugIndex.clear();
    auto find = [&](PrStringView slug, uint32_t hash) {
        return document->slugIndex.find(hash, [&](uint32_t at) {
            const MarkdownRange& range = document->slugs.data[at].text;
            return PrStringView{document->slugText.data + range.start, range.size} == slug;
        });
    };
    auto take = [&](PrStringView slug, uint32_t hash) {
        MarkdownRange range{document->slugText.count, uint32_t(slug.size)};
        document->slugText.append(slug.data, uint32_t(slug.size));
        document->slugIndex.insert(hash, document->slugs.count);
        document->slugs.push(MarkdownSlug{hash, range, 0});
    };
    RawArray<char> numbered;
    for (uint32_t b = 0; b < document->blockCount; ++b) {
        MarkdownTopBlock& block = document->blocks.data[b];
        uint32_t previousFirst = block.firstHeading;
        block.firstHeading = document->ids.count;
        for (uint32_t i = 0; i < block.nodes.count; ++i) {
            const MarkdownValues& values = block.nodes.data[i].values;
            if (values.kind != PrMarkdownNodeKind_Heading)
                continue;
            PrStringView base = viewOf(block, values.info);
            uint32_t baseHash = hashOf(base);
            PrStringView id = base;
            uint32_t hash = baseHash;
            uint32_t at = find(base, baseHash);
            while (find(id, hash) != kHashMissing) {
                uint32_t count = ++document->slugs.data[at].count;
                numbered.clear();
                numbered.append(base.data, uint32_t(base.size));
                numbered.push('-');
                char digits[16];
                size_t size = 0;
                do {
                    digits[size++] = char('0' + count % 10);
                    count /= 10;
                } while (count != 0);
                while (size != 0)
                    numbered.push(digits[--size]);
                id = PrStringView{numbered.data, numbered.count};
                hash = hashOf(id);
            }
            take(id, hash);
            MarkdownRange range{document->idText.count, uint32_t(id.size)};
            document->idText.append(id.data, uint32_t(id.size));
            document->ids.push(range);
        }
        if (b >= rebuiltFrom || block.resolved)
            continue;
        bool same = previousFirst + block.headingCount <= document->previousIds.count;
        for (uint32_t i = 0; same && i < block.headingCount; ++i) {
            const MarkdownRange& before = document->previousIds.data[previousFirst + i];
            const MarkdownRange& now = document->ids.data[block.firstHeading + i];
            PrStringView was{document->previousIdText.data + before.start, before.size};
            same = was == PrStringView{document->idText.data + now.start, now.size};
        }
        if (!same)
            markResolved(document, b);
    }
    numbered.reset();
}

// Parses the tentative blocks again from the source as it now is.
void reparse(Markdown* document) {
    uint32_t oldFinal = document->finalCount;
    document->blockCount = oldFinal;
    markChangedFrom(document, oldFinal);
    removeLookups(document, [&](const MarkdownLookup& lookup) { return lookup.block >= oldFinal; });

    // A definition may change the blocks that look up its label: one that becomes final, and any in tentative blocks
    // before or after this parse.
    document->changedHashes.clear();
    for (uint32_t i = 0; i < document->tailDefinitions.entries.count; ++i)
        document->changedHashes.push(document->tailDefinitions.entries.data[i].hash);

    MarkdownBlockParser* parser = &document->blockParser;
    PrStringView source{document->source.data, document->source.count};
    uint32_t start = document->tailStart;
    uint32_t frontFinal = 0;
    FrontMatter front = FrontMatter_None;
    if ((document->options.extensions & PrMarkdownExtensionFlag_FrontMatter) != 0 && start == 0) {
        PrStringView content{};
        size_t end = 0;
        front = findFrontMatter(source, document->ended, &content, &end);
        if (front == FrontMatter_Found) {
            MarkdownTopBlock& block = addBlock(document);
            block.sourceStart = 0;
            block.sourceEnd = uint32_t(end);
            buildFrontMatter(document, content, document->blockCount - 1);
            start = uint32_t(end);
            frontFinal = 1;
        }
    }
    markdownParseBlocks(parser, document->options, source, start, document->ended);

    definitionsClear(&document->tailDefinitions);
    for (uint32_t i = 0; i < parser->definitions.count; ++i) {
        const MarkdownParsedDefinition& parsed = parser->definitions.data[i];
        PrStringView label = parserText(*parser, parsed.label);
        PrStringView destination = parserText(*parser, parsed.destination);
        PrStringView title = parserText(*parser, parsed.title);
        bool final = parsed.sourceStart < parser->finalEnd;
        if (final) {
            if (definitionsAdd(&document->definitions, label, destination, title))
                document->changedHashes.push(hashOf(label));
        } else if (definitionsAdd(&document->tailDefinitions, label, destination, title)) {
            document->changedHashes.push(hashOf(label));
        }
    }

    uint32_t index = 0;
    for (MarkdownBlockIndex child = parser->blocks.data[0].first; child != kMarkdownBlockNone;
            child = parser->blocks.data[child].next, ++index) {
        MarkdownTopBlock& block = addBlock(document);
        block.sourceStart = parser->blocks.data[child].sourceStart;
        MarkdownBlockIndex next = parser->blocks.data[child].next;
        if (next != kMarkdownBlockNone)
            block.sourceEnd = parser->blocks.data[next].sourceStart;
        else
            block.sourceEnd = index < parser->finalCount ? parser->finalEnd : document->source.count;
        build(document, child, document->blockCount - 1);
    }
    document->finalCount = oldFinal + frontFinal + parser->finalCount;
    document->tailStart = parser->finalEnd;
    if (front == FrontMatter_Pending) {
        // Front matter that has no closing fence yet may turn the blocks that follow its opening fence into its lines.
        document->finalCount = 0;
        document->tailStart = 0;
    }

    resolveLate(document, oldFinal);
    resolveFootnotes(document, oldFinal);
    resolveHeadingIds(document, oldFinal);
}

void documentClear(Markdown* document) {
    document->source.clear();
    document->ended = false;
    document->blockCount = 0;
    document->finalCount = 0;
    document->tailStart = 0;
    definitionsClear(&document->definitions);
    definitionsClear(&document->tailDefinitions);
    document->lookups.clear();
    document->reportedCount = 0;
    document->changedFrom = kBlockNone;
    for (uint32_t i = 0; i < document->resolvedBlocks.count; ++i)
        document->blocks.data[document->resolvedBlocks.data[i]].resolved = false;
    document->resolvedBlocks.clear();
    document->queue.clear();
    document->queueHead = 0;
    document->uses.clear();
    document->previousUses.clear();
    document->footnotes.clear();
    document->previousFootnotes.clear();
    document->footnotesChanged = false;
    document->ids.clear();
    document->previousIds.clear();
    document->idText.clear();
    document->previousIdText.clear();
}

// Fills `step` with the values of `node` of `block`.
void fillStep(const Markdown* document, const MarkdownTopBlock& block, const MarkdownNode& node,
        PrMarkdownStepKind kind, PrMarkdownStep* step) {
    auto view = [&](MarkdownRange range) { return viewOf(block, range); };
    const MarkdownValues& values = node.values;
    *step = PrMarkdownStep{};
    step->kind = kind;
    step->node = values.kind;
    switch (values.kind) {
        case PrMarkdownNodeKind_Text:
            step->textKind = values.textKind;
            step->text = view(values.text);
            break;
        case PrMarkdownNodeKind_Heading:
            step->level = values.level;
            if ((document->options.extensions & PrMarkdownExtensionFlag_HeadingIds) != 0) {
                const MarkdownRange& id = document->ids.data[block.firstHeading + values.start];
                step->id = PrStringView{document->idText.data + id.start, id.size};
            }
            break;
        case PrMarkdownNodeKind_Alert:
            step->alert = values.alert;
            break;
        case PrMarkdownNodeKind_Math:
            step->display = values.display;
            break;
        case PrMarkdownNodeKind_MathBlock:
            step->display = true;
            break;
        case PrMarkdownNodeKind_List:
            step->list = values.list;
            step->tight = values.tight;
            step->start = values.start;
            break;
        case PrMarkdownNodeKind_Item:
            step->task = values.task;
            break;
        case PrMarkdownNodeKind_TableCell:
            step->alignment = values.alignment;
            step->header = values.header;
            break;
        case PrMarkdownNodeKind_CodeBlock:
            step->info = view(values.info);
            break;
        case PrMarkdownNodeKind_Link:
        case PrMarkdownNodeKind_Image:
            step->destination = view(values.destination);
            step->title = view(values.title);
            break;
        case PrMarkdownNodeKind_FootnoteReference: {
            const MarkdownFootnoteUse& use = document->uses.data[block.firstUse + values.start];
            if (use.number == 0) {
                step->kind = PrMarkdownStepKind_Text;
                step->node = PrMarkdownNodeKind_Text;
                step->text = view(values.text);
                break;
            }
            const MarkdownTopBlock& owner = document->blocks.data[use.block];
            step->label = viewOf(owner, owner.nodes.data[use.node].values.info);
            step->number = use.number;
            step->reference = use.reference;
            break;
        }
        case PrMarkdownNodeKind_FootnoteDefinition:
            step->label = view(values.info);
            break;
        default:
            break;
    }
}

// Walks the nodes of `block` from `first` up to `end`, and leaves out the footnote definitions among them.
void walkNodes(const Markdown* document, const MarkdownTopBlock& block, uint32_t first, uint32_t end,
        PrMarkdownStepFn fn, void* PR_NULLABLE context) {
    RawArray<uint32_t> open;
    PrMarkdownStep step;
    uint32_t i = first;
    while (true) {
        while (open.count != 0 && block.nodes.data[open.last()].end == i) {
            fillStep(document, block, block.nodes.data[open.last()], PrMarkdownStepKind_Leave, &step);
            fn(context, &step);
            open.pop();
        }
        if (i >= end)
            break;
        const MarkdownNode& node = block.nodes.data[i];
        if (node.values.kind == PrMarkdownNodeKind_FootnoteDefinition) {
            i = node.end;
            continue;
        }
        if (node.values.kind == PrMarkdownNodeKind_Text) {
            fillStep(document, block, node, PrMarkdownStepKind_Text, &step);
            fn(context, &step);
            ++i;
            continue;
        }
        fillStep(document, block, node, PrMarkdownStepKind_Enter, &step);
        fn(context, &step);
        if (step.kind == PrMarkdownStepKind_Enter && node.end == i + 1) {
            step.kind = PrMarkdownStepKind_Leave;
            fn(context, &step);
        } else if (step.kind == PrMarkdownStepKind_Enter) {
            open.push(i);
        }
        ++i;
    }
    open.reset();
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrMarkdown : Markdown {};

PrMarkdownOptions prMarkdownOptionsDefault(void) {
    PrMarkdownOptions options{};
    options.extensions = PrMarkdownExtensionFlag_GitHub;
    options.rawHtml = PrMarkdownRawHtml_Pass;
    options.destinations = PrMarkdownDestinations_All;
    return options;
}

PrMarkdown* prMarkdownCreate(const PrMarkdownOptions* PR_NULLABLE options) {
    PrMarkdown* document = allocZeroed<PrMarkdown>();
    document->options = options != nullptr ? *options : prMarkdownOptionsDefault();
    document->changedFrom = kBlockNone;
    return document;
}

void prMarkdownDestroy(PrMarkdown* PR_NULLABLE document) {
    if (document == nullptr)
        return;
    document->source.reset();
    for (uint32_t i = 0; i < document->blocks.count; ++i) {
        document->blocks.data[i].nodes.reset();
        document->blocks.data[i].text.reset();
        document->blocks.data[i].footnoteMarks.reset();
        document->blocks.data[i].footnoteLabels.reset();
    }
    document->blocks.reset();
    definitionsReset(&document->definitions);
    definitionsReset(&document->tailDefinitions);
    document->lookups.reset();
    document->resolvedBlocks.reset();
    document->queue.reset();
    markdownBlockParserReset(&document->blockParser);
    markdownInlineParserReset(&document->inlineParser);
    document->tree.nodes.reset();
    document->tree.text.reset();
    document->changedHashes.reset();
    document->uses.reset();
    document->previousUses.reset();
    document->footnotes.reset();
    document->previousFootnotes.reset();
    document->footnoteDefinitions.reset();
    document->footnoteIndex.reset();
    document->ids.reset();
    document->previousIds.reset();
    document->idText.reset();
    document->previousIdText.reset();
    document->slugs.reset();
    document->slugText.reset();
    document->slugIndex.reset();
    prMemoryFree(document);
}

void prMarkdownClear(PrMarkdown* document) {
    documentClear(document);
}

void prMarkdownAppend(PrMarkdown* document, PrStringView text) {
    PR_ASSERT(!document->ended);
    if (text.size == 0)
        return;
    document->source.append(text.data, uint32_t(text.size));
    reparse(document);
}

void prMarkdownEnd(PrMarkdown* document) {
    PR_ASSERT(!document->ended);
    document->ended = true;
    reparse(document);
}

void prMarkdownParse(PrMarkdown* document, PrStringView text) {
    documentClear(document);
    if (text.size != 0)
        document->source.append(text.data, uint32_t(text.size));
    document->ended = true;
    reparse(document);
}

bool prMarkdownIsEnded(const PrMarkdown* document) {
    return document->ended;
}

uint32_t prMarkdownBlockCount(const PrMarkdown* document) {
    return document->blockCount;
}

uint32_t prMarkdownFinalCount(const PrMarkdown* document) {
    return document->finalCount;
}

uint32_t prMarkdownTakeChanges(PrMarkdown* document, PrMarkdownChange* PR_NULLABLE out, uint32_t capacity) {
    if (document->queueHead == document->queue.count) {
        document->queue.clear();
        document->queueHead = 0;
        sortItems(document->resolvedBlocks.span(), [](uint32_t a, uint32_t b) { return a < b; });
        for (uint32_t i = 0; i < document->resolvedBlocks.count; ++i) {
            uint32_t index = document->resolvedBlocks.data[i];
            if (index < document->blockCount)
                document->blocks.data[index].resolved = false;
            if (document->changedFrom == kBlockNone || index < document->changedFrom)
                document->queue.push(PrMarkdownChange{index, 1, 1});
        }
        document->resolvedBlocks.clear();
        if (document->changedFrom != kBlockNone) {
            uint32_t first = document->changedFrom;
            PrMarkdownChange change{first, document->reportedCount - first, document->blockCount - first};
            if (change.removedCount != 0 || change.insertedCount != 0)
                document->queue.push(change);
            document->reportedCount = document->blockCount;
            document->changedFrom = kBlockNone;
        }
    }
    uint32_t written = 0;
    while (written < capacity && document->queueHead < document->queue.count)
        out[written++] = document->queue.data[document->queueHead++];
    return written;
}

bool prMarkdownTakeFootnotesChanged(PrMarkdown* document) {
    bool changed = document->footnotesChanged;
    document->footnotesChanged = false;
    return changed;
}

void prMarkdownWalk(const PrMarkdown* document, uint32_t first, uint32_t count, PrMarkdownStepFn fn, void* PR_NULLABLE context) {
    PR_ASSERT(first <= document->blockCount && count <= document->blockCount - first);
    for (uint32_t b = first; b < first + count; ++b) {
        const MarkdownTopBlock& block = document->blocks.data[b];
        walkNodes(document, block, 0, block.nodes.count, fn, context);
    }
}

void prMarkdownWalkFootnotes(const PrMarkdown* document, PrMarkdownStepFn fn, void* PR_NULLABLE context) {
    for (uint32_t i = 0; i < document->footnotes.count; ++i) {
        const MarkdownFootnote& footnote = document->footnotes.data[i];
        const MarkdownTopBlock& block = document->blocks.data[footnote.block];
        const MarkdownNode& node = block.nodes.data[footnote.node];
        PrMarkdownStep step;
        fillStep(document, block, node, PrMarkdownStepKind_Enter, &step);
        step.number = i + 1;
        step.referenceCount = footnote.referenceCount;
        fn(context, &step);
        walkNodes(document, block, footnote.node + 1, node.end, fn, context);
        step.kind = PrMarkdownStepKind_Leave;
        fn(context, &step);
    }
}
