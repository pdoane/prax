// Parsed rules, and declaration serialization.
//
// cssom-1 - https://drafts.csswg.org/cssom/
// css-mixins-1 - https://drafts.csswg.org/css-mixins-1/

#include "css/src/css_rule.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/bitset.h"
#include "pr/mem.h"
#include "pr/name_table.h"
#include "pr/string_ops.h"

#include "css/src/css_keyword.h"
#include "css/src/css_parser.h"
#include "css/src/css_shorthand.h"

namespace pr_impl {
namespace {

// Returns the value of the token at `index`. If the token holds an escape, the value is resolved into `scratch`.
PrStringView tokenView(PrTextBuffer* scratch, const CssTokenStream* ts, uint32_t index) {
    PrStringView text = cssTokenValueText(ts->tokens, index);
    if ((cssTokenFlags(ts->tokens, index) & CssTokenFlag_PlainText) != 0)
        return text;

    prTextBufferClear(scratch);
    cssValueAppend(scratch, text);
    return prTextBufferView(scratch);
}

// ---------------------------------------------------------------------------------------------------------------------
// Names

// Returns true if the name is a custom property (css-variables-1/#defining-variables).
bool isCustomProperty(PrStringView name) {
    return prStringStartsWith(name, "--");
}

// Resolves an author's name in a block whose rule accepts one. The name is the block's named value, or a custom
// property's <declaration-value> (css-variables-1/#syntax). Returns false if the block accepts no author's names.
bool authorName(CssAtRule rule, PrStringView name, CssDeclaredName* out) {
    CssGrammarIndex named = kCssAtRuleNamedValue[rule];
    if (named == CssGrammarRoot_None && (!isCustomProperty(name) || kCssAtRuleProperties[rule] == CssPropertiesIn_None))
        return false;
    *out = CssDeclaredName{
        PrCssProperty_Count, PrCssDescriptor_None, named, name,
        uint32_t(nameHash(name.data, name.size, kNameSeed)), false,
    };
    return true;
}

// Resolves `name` in the same way as cssNameFromText.
bool resolveName(CssAtRule rule, PrStringView name, CssDeclaredName* out) {
    if (name.data == nullptr)
        return false;

    if (authorName(rule, name, out))
        return true;

    // css-cascade-5/#legacy-name-alias: a legacy name declares the descriptor it is an alias of.
    PrCssDescriptor descriptor = cssDescriptorFromText(rule, name);
    if (descriptor != PrCssDescriptor_None) {
        *out = cssDescriptorDeclaredName(kCssDescriptorAlias[descriptor]);
        return true;
    }
    if (kCssAtRuleProperties[rule] != CssPropertiesIn_All)
        return false;

    // css-cascade-5/#legacy-name-alias: a legacy name declares the property it is an alias of.
    PrCssProperty property = cssPropertyFromText(name);
    if (property == PrCssProperty_Count)
        return false;
    *out = cssPropertyDeclaredName(kCssPropertyAlias[property]);
    return true;
}

// Returns true if two declarations have the same name. Each comes with the text that holds its author's name. Authors'
// names are compared exactly. Other names are compared by hash, which decides unless there is a collision.
bool declarationNameEquals(const CssDeclaration* a, const char* PR_NULLABLE aText, const CssDeclaration* b,
        const char* PR_NULLABLE bText) {
    if (a->hash != b->hash || a->property != b->property || a->descriptor != b->descriptor)
        return false;
    if (a->property != PrCssProperty_Count || a->descriptor != PrCssDescriptor_None)
        return true;
    return PrStringView{aText + a->name, a->nameSize} == PrStringView{bText + b->name, b->nameSize};
}

// ---------------------------------------------------------------------------------------------------------------------
// Shared declarations

// A set of properties.
struct PropertySet {
    uint64_t words[bitsetWords(PrCssProperty_Count)];
};

// Returns true if the declaration is shared. A shared declaration is a shorthand's declaration, and every longhand that
// it lists takes its single value.
bool isShared(const CssDeclaration& declaration) {
    return declaration.property != PrCssProperty_Count && cssIsShorthand(declaration.property);
}

// Returns true if the declaration is for a longhand that `shorthand` sets.
bool setBy(const CssDeclaration& declaration, PrCssProperty shorthand) {
    return declaration.property != PrCssProperty_Count && cssShorthandSets(shorthand, declaration.property);
}

// Returns `declaration` as the declaration of the longhand `property`.
CssDeclaration asLonghand(CssDeclaration declaration, PrCssProperty property) {
    declaration.property = property;
    declaration.hash = property;
    return declaration;
}

// Returns true if the shared declaration `a`, rather than the shared declaration `b`, declares a longhand that both of
// them set. The important one declares it. Otherwise the later one does, and `a` is the later one if `later` is true.
bool precedes(const CssDeclaration& a, const CssDeclaration& b, bool later) {
    return a.important != b.important ? a.important : later;
}

// Returns true if the shorthands `a` and `b` both set some longhand.
bool overlaps(PrCssProperty a, PrCssProperty b) {
    if (cssPropertyLonghands(b).count < cssPropertyLonghands(a).count)
        return overlaps(b, a);
    CssPropertyList longhands = cssPropertyLonghands(a);
    for (uint32_t i = 0; i < longhands.count; ++i) {
        if (cssShorthandSets(b, longhands.data[i]))
            return true;
    }
    return false;
}

// Returns the index of the declaration in `declarations` that declares `longhand`, or `count` if there is none.
uint32_t declarerOf(const CssDeclaration* PR_NULLABLE declarations, uint32_t count, PrCssProperty longhand) {
    uint32_t shared = count;
    for (uint32_t i = 0; i < count; ++i) {
        const CssDeclaration& declaration = declarations[i];
        if (declaration.property == longhand)
            return i;
        if (isShared(declaration) && cssShorthandSets(declaration.property, longhand) &&
                (shared == count || precedes(declaration, declarations[shared], true)))
            shared = i;
    }
    return shared;
}

// Writes to `out` the longhands of the shared declaration at `index` of `declarations` that another declaration
// declares.
void takenFrom(const CssDeclaration* declarations, uint32_t count, uint32_t index, PropertySet* out) {
    prMemZero(out, sizeof(PropertySet));
    for (uint32_t i = 0; i < count; ++i) {
        const CssDeclaration& declaration = declarations[i];
        if (i == index || declaration.property == PrCssProperty_Count)
            continue;
        if (!isShared(declaration)) {
            bitsetSet(out->words, declaration.property);
        } else if (precedes(declaration, declarations[index], i > index)) {
            CssPropertyList longhands = cssPropertyLonghands(declaration.property);
            for (uint32_t l = 0; l < longhands.count; ++l)
                bitsetSet(out->words, longhands.data[l]);
        }
    }
}

// Calls `visit` with each longhand that the declaration at `index` of `declarations` lists, in order. A declaration
// lists its own property. A shared declaration lists those of its longhands that no other declaration declares.
template <typename Visit>
void forEachListed(const CssDeclaration* declarations, uint32_t count, uint32_t index, Visit visit) {
    const CssDeclaration& declaration = declarations[index];
    if (!isShared(declaration)) {
        visit(declaration.property);
        return;
    }
    PropertySet taken;
    takenFrom(declarations, count, index, &taken);
    CssPropertyList longhands = cssPropertyLonghands(declaration.property);
    for (uint32_t i = 0; i < longhands.count; ++i) {
        if (!bitsetTest(taken.words, longhands.data[i]))
            visit(longhands.data[i]);
    }
}

// Returns the number of declarations that the declaration at `index` of `declarations` lists.
uint32_t listedBy(const CssDeclaration* declarations, uint32_t count, uint32_t index) {
    uint32_t listed = 0;
    forEachListed(declarations, count, index, [&](PrCssProperty) { ++listed; });
    return listed;
}

// Returns the total number of declarations that `declarations` lists.
uint32_t listedLength(const CssDeclaration* PR_NULLABLE declarations, uint32_t count) {
    uint32_t length = 0;
    for (uint32_t i = 0; i < count; ++i)
        length += listedBy(declarations, count, i);
    return length;
}

// ---------------------------------------------------------------------------------------------------------------------
// Staging
//
// A packed block is written whole from declarations staged in the parser's `declared`. Their values are runs of the
// parser's pool, and their authors' names are in the pool's text. A staging begins at a Stage and restores the scratch
// when it is done.

// The parser's state at the start of a staging.
struct Stage {
    uint32_t declared;
    uint32_t values;
    uint32_t text;
    uint32_t filter;     // number of the parser's filter words that precede this staging's words
};

Stage beginStage(const CssParser* parser) {
    return Stage{parser->declared.count, parser->pool.values.count, parser->pool.text.count, parser->filter.count};
}

void endStage(CssParser* parser, Stage stage) {
    parser->declared.count = stage.declared;
    parser->pool.values.count = stage.values;
    parser->pool.text.count = stage.text;
    parser->filter.count = stage.filter;
}

// A filter with one bit per name hash, and sixty-four bits for each declaration that it covers. A clear bit means that
// no staged declaration has that name. The filter's words are the parser's filter words from `first` on.
struct NameFilter {
    uint32_t first;
    uint32_t mask;     // index of the last word, used as a mask
};

NameFilter makeNameFilter(CssParser* parser, uint32_t declarations) {
    uint32_t words = 1;
    while (words < declarations)
        words *= 2;

    uint32_t first = parser->filter.count;
    parser->filter.reserve(first + words);
    prMemZero(parser->filter.data + first, size_t(words) * sizeof(uint64_t));
    parser->filter.count = first + words;
    return NameFilter{first, words - 1};
}

// Returns true if the filter already holds `hash`. In either case, the filter holds it afterwards.
bool nameSeen(CssParser* parser, NameFilter filter, uint32_t hash) {
    uint64_t* word = &parser->filter.data[filter.first + ((hash >> 6) & filter.mask)];
    uint64_t bit = uint64_t(1) << (hash & 63);
    bool seen = (*word & bit) != 0;
    *word |= bit;
    return seen;
}

// Matches `span` as a value of `name`, and adds it to the pool.
bool matchValue(const CssDeclaredName& name, CssParser* parser, const CssTokenStream* ts,
        CssTokenSpan span, CssValueSpan* out) {
    CssTokenStream value = cssSubStream(ts, span);
    if (name.property != PrCssProperty_Count)
        return cssParsePropertyValue(name.property, &value, &parser->pool, &parser->match, out);
    if (name.descriptor != PrCssDescriptor_None)
        return cssParseDescriptorValue(name.descriptor, &value, &parser->pool, &parser->match, out);
    if (name.grammar != CssGrammarRoot_None)
        return cssParseValue(name.grammar, &value, &parser->pool, &parser->match, out);
    return cssParseTokens(&value, &parser->pool, out);
}

// Adds to the parser's `incoming` the declarations that `declaration` of `shorthand` makes for `value`. For a CSS-wide
// keyword or a pending value, that is one shared declaration. Otherwise it is a declaration for each longhand that the
// value expands to (css-cascade-5/#shorthand-property). Returns false if the value expands to no longhands.
bool declareLonghands(CssParser* parser, PrCssProperty shorthand, CssValueSpan value, CssDeclaration declaration) {
    const CssValueNode* first = &parser->pool.values.data[value.start];
    bool wide = value.count == 1 && first->kind == PrCssValueKind_Keyword &&
        cssIsCssWideKeyword(cssValueKeyword(first));
    if (wide || cssShorthandPending(shorthand, cssValuePoolRun(&parser->pool, value))) {
        declaration.shorthand = wide ? PrCssProperty_Count : shorthand;
        parser->incoming.push(declaration);
        return true;
    }
    if (!cssExpandShorthand(parser, shorthand, value))
        return false;

    CssPropertyList longhands = cssPropertyLonghands(shorthand);
    for (uint32_t i = 0; i < longhands.count; ++i) {
        CssValueSpan span = parser->expanded.data[i];
        CssDeclaration longhand = asLonghand(declaration, longhands.data[i]);
        longhand.value = span.start;
        longhand.valueCount = span.count;
        parser->incoming.push(longhand);
    }
    return true;
}

// Declares `span` as the value of `name`, into the parser's `incoming`. The result is the declaration itself, or the
// declarations of a shorthand's longhands. Their values and names are in the parser's pool. Returns false if `name`
// does not accept the value. If `given` is set, the tokens are those of a given value (cssValueTokens).
bool declare(CssParser* parser, const CssDeclaredName& name, const CssTokenStream* ts,
        CssTokenSpan span, bool important, bool given = false) {
    parser->incoming.count = 0;
    CssValueSpan matched;
    if (!name.unicodeRanges || given) {
        if (!matchValue(name, parser, ts, span, &matched))
            return false;
    } else {
        // css-syntax-3/#parse-list-of-component-values reads the value again with unicode ranges allowed.
        cssTokenize(&parser->asideTokens, &parser->asideScratch, cssSpanText(ts->tokens, span),
            CssTokenizeFlag_UnicodeRanges);
        CssTokenStream ranged = cssStream(&parser->asideTokens);
        if (!matchValue(name, parser, &ranged, cssStreamTrimmed(&ranged), &matched)) {
            return false;
        }
    }

    CssDeclaration declaration = {};
    declaration.value = matched.start;
    declaration.valueCount = matched.count;
    if (name.text.data != nullptr) {
        declaration.name = cssValuePoolAddText(&parser->pool, name.text);
        declaration.nameSize = uint32_t(name.text.size);
    }
    declaration.hash = name.hash;
    declaration.property = name.property;
    declaration.shorthand = PrCssProperty_Count;
    declaration.descriptor = name.descriptor;
    declaration.important = important;
    if (name.property != PrCssProperty_Count && cssIsShorthand(name.property))
        return declareLonghands(parser, name.property, matched, declaration);
    parser->incoming.push(declaration);
    return true;
}

// Stages a declaration after those from `first` on, and drops an earlier declaration of its name or of a longhand that
// it sets. In a block's text, the last declaration of a longhand wins, at the place where it was written. The exception
// is an important declaration: a later declaration that is not important does not replace it
// (css-cascade-5/#importance). If two shared declarations set the same longhand, both stay, and the block's order
// decides which one declares it.
void stageLast(CssParser* parser, uint32_t first, NameFilter filter, const CssDeclaration& declaration) {
    const char* text = parser->pool.text.data;
    bool seen = nameSeen(parser, filter, declaration.hash);
    if (isShared(declaration)) {
        CssPropertyList longhands = cssPropertyLonghands(declaration.property);
        for (uint32_t i = 0; i < longhands.count; ++i)
            seen = nameSeen(parser, filter, longhands.data[i]) || seen;
    }
    if (seen) {
        for (uint32_t i = first; i < parser->declared.count; ++i) {
            const CssDeclaration& earlier = parser->declared.data[i];
            bool stands = earlier.important && !declaration.important;
            if (stands && (declarationNameEquals(&earlier, text, &declaration, text) ||
                    setBy(declaration, earlier.property)))
                return;
        }
        uint32_t kept = first;
        for (uint32_t i = first; i < parser->declared.count; ++i) {
            const CssDeclaration& earlier = parser->declared.data[i];
            bool stands = earlier.important && !declaration.important;
            if (stands || !(declarationNameEquals(&earlier, text, &declaration, text) ||
                    setBy(earlier, declaration.property)))
                parser->declared.data[kept++] = earlier;
        }
        parser->declared.count = kept;
    }
    parser->declared.push(declaration);
}

// Returns the block's declaration at `index` as a staged declaration, with its value and name copied to the pool.
CssDeclaration restage(CssParser* parser, const CssDeclarationBlock* block, uint32_t index) {
    const CssDeclaration* held = &cssDeclarations(block)[index];
    CssValuePool* pool = &parser->pool;
    CssDeclaration staged = *held;
    staged.value = cssValuePoolAddRun(pool, cssDeclarationValue(block, held)).start;
    staged.name = pool->text.count;
    if (held->nameSize != 0)
        pool->text.append(cssDeclarationText(block, held) + held->name, held->nameSize);
    return staged;
}

// The allocation that holds a packed block. It begins with the block's values.
CssValueNode* PR_NULLABLE packedHeld(const CssDeclarationBlock* block) {
    PR_ASSERT(block->stored == nullptr);
    if (block->declarations == nullptr)
        return nullptr;
    return reinterpret_cast<CssValueNode*>(block->declarations) - block->valueCount;
}

// Replaces the block's declarations with those staged from `first` on, packed. A shared declaration that declares no
// longhand is left out.
void write(CssDeclarationBlock* block, CssParser* parser, uint32_t first) {
    const CssValuePool* pool = &parser->pool;
    for (uint32_t i = first; i < parser->declared.count; ++i) {
        if (isShared(parser->declared.data[i]) &&
                listedBy(parser->declared.data + first, parser->declared.count - first, i - first) == 0)
            parser->declared.erase(i--);
    }
    uint32_t count = parser->declared.count - first;
    uint32_t valueCount = 0;
    uint32_t textSize = 0;
    for (uint32_t i = first; i < parser->declared.count; ++i) {
        const CssDeclaration* staged = &parser->declared.data[i];
        uint32_t start = 0;
        uint32_t size = 0;
        cssValueRunText(cssValuePoolRun(pool, {staged->value, staged->valueCount}), &start, &size);
        valueCount += staged->valueCount;
        textSize += staged->nameSize + size;
    }

    cssDeclarationBlockReset(block);
    if (count == 0)
        return;
    CssValueNode* held = static_cast<CssValueNode*>(
        prMemoryAlloc(sizeof(CssValueNode) * valueCount + sizeof(CssDeclaration) * count + textSize));
    CssDeclaration* declarations = reinterpret_cast<CssDeclaration*>(held + valueCount);
    char* text = reinterpret_cast<char*>(declarations + count);
    uint32_t values = 0;
    uint32_t at = 0;
    for (uint32_t i = 0; i < count; ++i) {
        CssDeclaration declaration = parser->declared.data[first + i];
        if (declaration.nameSize != 0)
            prMemCopy(text + at, pool->text.data + declaration.name, declaration.nameSize);
        declaration.name = at;
        at += declaration.nameSize;

        CssValueRun run = cssValuePoolRun(pool, {declaration.value, declaration.valueCount});
        at += cssValueRunCopy(run, held + values, text + at, at);
        declaration.value = values;
        values += declaration.valueCount;
        declarations[i] = declaration;
    }

    block->declarations = declarations;
    block->count = count;
    block->length = listedLength(declarations, count);
    block->valueCount = valueCount;
    block->textSize = textSize;
}

// ---------------------------------------------------------------------------------------------------------------------
// Mutable blocks

// Allocates the storage for a declaration of a mutable block that declares `run` under `name`. The storage holds the
// values, then the text that they reference, then the name. `declaration` is set to index into it. Returns null if
// there is nothing to store.
CssValueNode* PR_NULLABLE storeDeclaration(CssValueRun run, PrStringView name, CssDeclaration* declaration) {
    uint32_t start = 0;
    uint32_t size = 0;
    cssValueRunText(run, &start, &size);
    declaration->value = 0;
    declaration->valueCount = run.count;
    declaration->name = size;
    declaration->nameSize = uint32_t(name.size);
    size_t bytes = sizeof(CssValueNode) * run.count + size + name.size;
    if (bytes == 0)
        return nullptr;
    CssValueNode* held = static_cast<CssValueNode*>(prMemoryAlloc(bytes));
    char* text = reinterpret_cast<char*>(held + run.count);
    cssValueRunCopy(run, held, text, 0);
    if (name.size != 0)
        prMemCopy(text + size, name.data, name.size);
    return held;
}

// Returns the author's name of a declaration of `block`, or the null view for any other declaration.
PrStringView authorText(const CssDeclarationBlock* block, const CssDeclaration* declaration) {
    if (declaration->nameSize == 0)
        return PrStringView{};
    return PrStringView{cssDeclarationText(block, declaration) + declaration->name, declaration->nameSize};
}

// Copies the declarations of `from`, in either form, into `out` in the mutable form, with room for `capacity` of them.
void copyMutable(CssDeclarationBlock* out, const CssDeclarationBlock* from, uint32_t capacity) {
    PR_ASSERT(capacity >= from->count && capacity != 0);
    CssDeclaration* declarations = static_cast<CssDeclaration*>(prMemoryAlloc(sizeof(CssDeclaration) * capacity));
    CssValueNode** stored = static_cast<CssValueNode**>(prMemoryAlloc(sizeof(CssValueNode*) * capacity));
    for (uint32_t i = 0; i < from->count; ++i) {
        const CssDeclaration* declaration = &from->declarations[i];
        declarations[i] = *declaration;
        stored[i] = storeDeclaration(cssDeclarationValue(from, declaration),
            authorText(from, declaration), &declarations[i]);
    }
    out->declarations = declarations;
    out->stored = stored;
    out->count = from->count;
    out->capacity = capacity;
    out->valueCount = 0;
    out->textSize = 0;
    out->length = from->length;
    out->rule = from->rule;
}

// Makes a packed block mutable.
void makeMutable(CssDeclarationBlock* block) {
    if (block->stored != nullptr)
        return;
    CssDeclarationBlock copy = {};
    copyMutable(&copy, block, block->count > 4 ? block->count : 4);
    cssDeclarationBlockReset(block);
    *block = copy;
}

// Replaces `removed` declarations of a mutable block from `index` on with room for `added` declarations, whose storage
// is null. The caller owns the storage of the removed declarations.
void splice(CssDeclarationBlock* block, uint32_t index, uint32_t removed, uint32_t added) {
    uint32_t count = block->count - removed + added;
    if (count > block->capacity) {
        while (block->capacity < count)
            block->capacity *= 2;
        block->declarations = static_cast<CssDeclaration*>(
            prMemoryRealloc(block->declarations, sizeof(CssDeclaration) * block->capacity));
        block->stored = static_cast<CssValueNode**>(
            prMemoryRealloc(block->stored, sizeof(CssValueNode*) * block->capacity));
    }
    uint32_t tail = block->count - index - removed;
    prMemMove(block->declarations + index + added, block->declarations + index + removed, sizeof(CssDeclaration) * tail);
    prMemMove(block->stored + index + added, block->stored + index + removed, sizeof(CssValueNode*) * tail);
    for (uint32_t i = 0; i < added; ++i)
        block->stored[index + i] = nullptr;
    block->count = count;
}

// Removes the declaration at `index` of a mutable block.
void eraseDeclaration(CssDeclarationBlock* block, uint32_t index) {
    prMemoryFree(block->stored[index]);
    splice(block, index, 1, 0);
}

// Sets the declaration at `index` of a mutable block to `declaration` declaring `run` under `name`.
void storeAt(CssDeclarationBlock* block, uint32_t index, CssValueRun run, PrStringView name, CssDeclaration declaration) {
    CssValueNode* held = storeDeclaration(run, name, &declaration);
    prMemoryFree(block->stored[index]);
    block->stored[index] = held;
    block->declarations[index] = declaration;
}

// Replaces the shared declaration at `index` of a mutable block with declarations of the longhands that it lists, in
// the same place.
void unshare(CssDeclarationBlock* block, uint32_t index) {
    PropertySet taken;
    takenFrom(block->declarations, block->count, index, &taken);
    CssDeclaration shared = block->declarations[index];
    CssValueNode* held = block->stored[index];
    CssValueRun run = cssDeclarationValue(block, &block->declarations[index]);
    CssPropertyList longhands = cssPropertyLonghands(shared.property);
    uint32_t listed = 0;
    for (uint32_t i = 0; i < longhands.count; ++i)
        listed += bitsetTest(taken.words, longhands.data[i]) ? 0 : 1;
    splice(block, index, 1, listed);
    for (uint32_t i = 0; i < longhands.count; ++i) {
        if (!bitsetTest(taken.words, longhands.data[i]))
            storeAt(block, index++, run, PrStringView{}, asLonghand(shared, longhands.data[i]));
    }
    prMemoryFree(held);
}

// Returns the index of the block's declaration with the same name as `staged`, or the block's count if there is none.
// `text` holds the name of `staged`.
uint32_t indexOfName(const CssDeclarationBlock* block, const CssDeclaration& staged, const char* text) {
    uint32_t index = 0;
    while (index < block->count && !declarationNameEquals(&block->declarations[index],
            cssDeclarationText(block, &block->declarations[index]), &staged, text)) {
        ++index;
    }
    return index;
}

// Sets a declaration of a mutable block to `staged`, whose value and name are in the parser's pool. If the block holds
// a declaration of the same name, that declaration is set. Otherwise a new declaration is appended after the rest
// (cssom-1/#set-a-css-declaration). A longhand is set at the position where it is listed, so a shared declaration that
// lists it is first rewritten as its longhands. A shared declaration takes over the longhands that it sets. Their own
// declarations take its value, and another shared declaration that sets one of them is first rewritten as its
// longhands.
void setDeclaration(CssDeclarationBlock* block, const CssParser* parser, const CssDeclaration& staged) {
    const CssValuePool* pool = &parser->pool;
    CssValueRun run = cssValuePoolRun(pool, {staged.value, staged.valueCount});
    PrStringView name = {pool->text.data + staged.name, staged.nameSize};
    if (isShared(staged)) {
        for (uint32_t i = 0; i < block->count; ++i) {
            const CssDeclaration& held = block->declarations[i];
            if (isShared(held) && held.property != staged.property && overlaps(held.property, staged.property))
                unshare(block, i--);
            else if (setBy(held, staged.property))
                storeAt(block, i, run, name, asLonghand(staged, held.property));
        }
    } else if (staged.property != PrCssProperty_Count) {
        uint32_t declarer = declarerOf(block->declarations, block->count, staged.property);
        if (declarer != block->count && isShared(block->declarations[declarer]))
            unshare(block, declarer);
    }

    uint32_t index = indexOfName(block, staged, pool->text.data);
    if (index == block->count)
        splice(block, index, 0, 1);
    storeAt(block, index, run, name, staged);
    if (isShared(staged) && listedBy(block->declarations, block->count, index) == 0)
        eraseDeclaration(block, index);
}

// Removes the block's declaration of `name`, or of each longhand of a shorthand. A shared declaration that sets only
// removed longhands is removed too. A shared declaration that also sets other longhands is rewritten as its longhands,
// because it would otherwise take the place of the removed ones.
void removeDeclarations(CssDeclarationBlock* block, const CssDeclaredName& name) {
    makeMutable(block);
    if (name.property == PrCssProperty_Count) {
        eraseDeclaration(block, cssDeclarationBlockFind(block, name));
        --block->length;
        return;
    }

    CssPropertyList longhands;
    if (cssIsShorthand(name.property))
        longhands = cssPropertyLonghands(name.property);
    else
        longhands = CssPropertyList{&name.property, 1};
    PropertySet removed = {};
    for (uint32_t i = 0; i < longhands.count; ++i)
        bitsetSet(removed.words, longhands.data[i]);
    for (uint32_t i = 0; i < block->count; ++i) {
        PrCssProperty held = block->declarations[i].property;
        if (held == PrCssProperty_Count)
            continue;
        CssPropertyList sets = cssIsShorthand(held) ? cssPropertyLonghands(held) : CssPropertyList{&held, 1};
        uint32_t count = 0;
        for (uint32_t l = 0; l < sets.count; ++l)
            count += bitsetTest(removed.words, sets.data[l]) ? 1 : 0;
        if (count == sets.count)
            eraseDeclaration(block, i--);
        else if (count != 0)
            unshare(block, i--);
    }
    block->length = listedLength(block->declarations, block->count);
}

// Returns true if the block's declaration at `index` has the value, the importance and the pending shorthand of
// `staged`. The value of `staged` is in `pool`.
bool holds(const CssDeclarationBlock* block, uint32_t index, const CssDeclaration& staged, const CssValuePool* pool) {
    if (index == block->count)
        return false;
    const CssDeclaration* held = &cssDeclarations(block)[index];
    return held->important == staged.important && held->shorthand == staged.shorthand &&
        cssValueRunEquals(cssDeclarationValue(block, held), cssValuePoolRun(pool, {staged.value, staged.valueCount}));
}

// Returns true if setting the parser's `incoming` declarations in the block changes the block. It does if a declaration
// that they set, or the declaration of a longhand of a shared one, does not already have that value and importance
// (cssom-1/#set-a-css-declaration).
bool incomingChanges(const CssDeclarationBlock* block, const CssParser* parser) {
    const CssValuePool* pool = &parser->pool;
    for (uint32_t d = 0; d < parser->incoming.count; ++d) {
        const CssDeclaration& staged = parser->incoming.data[d];
        if (staged.property == PrCssProperty_Count) {
            if (!holds(block, indexOfName(block, staged, pool->text.data), staged, pool))
                return true;
            continue;
        }
        CssPropertyList longhands;
        if (isShared(staged))
            longhands = cssPropertyLonghands(staged.property);
        else
            longhands = CssPropertyList{&staged.property, 1};
        for (uint32_t i = 0; i < longhands.count; ++i) {
            if (!holds(block, declarerOf(cssDeclarations(block), block->count, longhands.data[i]), staged, pool))
                return true;
        }
    }
    return false;
}

// Sets each declaration that the parser's `incoming` holds.
void setIncoming(CssDeclarationBlock* block, const CssParser* parser) {
    makeMutable(block);
    for (uint32_t d = 0; d < parser->incoming.count; ++d)
        setDeclaration(block, parser, parser->incoming.data[d]);
    block->length = listedLength(block->declarations, block->count);
}

// Returns true if a run is one value kept as tokens.
bool isTokens(CssValueRun run) {
    return run.count == 1 && run.values[0].kind == PrCssValueKind_Tokens;
}

// Adds the declarations of `from` to those of `into`. The last declaration of each name wins, at the place where it was
// written.
void mergeDeclarations(CssParser* parser, CssDeclarationBlock* into, const CssDeclarationBlock* from) {
    Stage stage = beginStage(parser);
    NameFilter filter = makeNameFilter(parser, into->count + from->count);
    for (uint32_t i = 0; i < into->count; ++i)
        stageLast(parser, stage.declared, filter, restage(parser, into, i));
    for (uint32_t i = 0; i < from->count; ++i)
        stageLast(parser, stage.declared, filter, restage(parser, from, i));
    write(into, parser, stage.declared);
    endStage(parser, stage);
}

// Stages the declarations among a parsed block's items in [first, end), for a declaration block of `rule`. A
// declaration is staged only if the block supports its name and accepts its value.
void stageDeclarations(CssParser* parser, CssAtRule rule, const CssTokenStream* ts, uint32_t first,
        uint32_t end, uint32_t runFirst, NameFilter filter) {
    bool important = bitsetTest(kCssAtRuleImportant, rule);
    for (uint32_t i = first; i < end; ++i) {
        CssItem item = parser->items.data[i];
        if (item.type != CssItemType_Declaration)
            continue;

        CssSyntaxDeclaration syntax = parser->declarations.data[item.index];
        if (syntax.important && !important)
            continue;

        CssDeclaredName name = {};
        if (!resolveName(rule, tokenView(&parser->names, ts, syntax.name), &name))
            continue;

        // css-variables-1/#defining-variables: a custom property's value may be empty, and no other property's may.
        CssTokenSpan span = cssTrimWhitespace(ts->tokens, syntax.value);
        if (span.count == 0 && name.text.data == nullptr)
            continue;

        if (!declare(parser, name, ts, span, syntax.important))
            continue;
        for (uint32_t d = 0; d < parser->incoming.count; ++d)
            stageLast(parser, runFirst, filter, parser->incoming.data[d]);
    }
}

// Replaces the block's declarations with the declarations in `text`, dropping any rules among them
// (cssom-1/#parse-a-css-declaration-block).
void parseDeclarations(CssParser* parser, CssDeclarationBlock* block, PrStringView text) {
    CssTokenStream ts = cssParserTokenize(parser, text);
    CssBlock parsed = {};
    cssParseBlockContents(&ts, parser, &parsed);
    Stage stage = beginStage(parser);
    NameFilter filter = makeNameFilter(parser, parsed.declarations.count);
    stageDeclarations(parser, block->rule, &ts, parsed.items.start,
        parsed.items.start + parsed.items.count, stage.declared, filter);
    write(block, parser, stage.declared);
    endStage(parser, stage);
    cssParserDrop(parser, parsed);
}

// ---------------------------------------------------------------------------------------------------------------------
// Rule placement
//
// css-syntax-3/#block-contents: a block holds what its grammar allows. A rule is valid in a block if its spec says so.

CssAtRule resolved(const CssRuleData* rule) {
    return kCssAtRuleAlias[rule->atRule];
}

bool isAt(const CssRuleData* PR_NULLABLE rule, CssAtRule atRule) {
    return rule != nullptr && rule->kind == CssRuleDataKind_At && resolved(rule) == atRule;
}

// Returns the rule that the chain starts at, or null for the sheet.
const CssRuleData* PR_NULLABLE ruleOf(const CssRuleChain* PR_NULLABLE chain) {
    return chain != nullptr ? chain->rule : nullptr;
}

// Returns the nearest style rule, searching outward from the chain's rule and including it. `&` represents the
// selectors of that rule.
const CssRuleData* PR_NULLABLE styleAncestor(const CssRuleChain* PR_NULLABLE chain) {
    while (chain != nullptr && chain->rule->kind != CssRuleDataKind_Style)
        chain = chain->outer;
    return ruleOf(chain);
}

// Returns true if the block of the chain's rule is a nested context. A nested context is a style rule's block, or the
// block of a rule that holds both declarations and style rules, directly or through nested group rules in between
// (css-nesting-1/#nested-group-rules). The sheet is not a nested context.
bool nested(const CssRuleChain* PR_NULLABLE chain) {
    for (; chain != nullptr; chain = chain->outer) {
        const CssRuleData* rule = chain->rule;
        if (rule->kind == CssRuleDataKind_Style)
            return true;
        if (rule->kind != CssRuleDataKind_At)
            return false;
        CssAtRule atRule = resolved(rule);
        if (kCssAtRuleContents[atRule] == CssContents_BlockContents)
            return true;
        if (!bitsetTest(kCssAtRuleNests, atRule))
            return false;
    }
    return false;
}

// Returns true if the block of the chain's rule is an @function body. That is the @function's own block, or the block
// of a conditional group rule inside one (css-mixins-1/#function-rule).
bool functionBody(const CssRuleChain* PR_NULLABLE chain) {
    for (; chain != nullptr && chain->rule->kind == CssRuleDataKind_At; chain = chain->outer) {
        CssAtRule atRule = resolved(chain->rule);
        if (atRule == CssAtRule_Function)
            return true;
        if (!cssAtRuleConditional(atRule))
            return false;
    }
    return false;
}

// Returns what the block of the chain's rule may hold. A group rule in a nested context holds the same contents as a
// style rule (css-nesting-1/#nested-group-rules). A group rule in an @function body holds the same contents as the
// @function.
CssContents contentsOf(const CssRuleChain* chain) {
    const CssRuleData* rule = chain->rule;
    switch (rule->kind) {
        case CssRuleDataKind_Style:
            return CssContents_BlockContents;
        case CssRuleDataKind_NestedDeclarations:
            return CssContents_DeclarationList;
        case CssRuleDataKind_Keyframe:
            return kCssAtRuleQualifiedContents[rule->style.rule];
        default: {
            CssAtRule atRule = resolved(rule);
            if (bitsetTest(kCssAtRuleNests, atRule) && kCssAtRuleContents[atRule] != 0 && nested(chain->outer))
                return CssContents_BlockContents;
            if (functionBody(chain))
                return kCssAtRuleContents[CssAtRule_Function];
            return kCssAtRuleContents[atRule];
        }
    }
}

// Returns true if an at-rule is valid in the block of the rule that `parent` starts at, or in the sheet if `parent` is
// null.
bool atRuleFits(CssAtRule atRule, const CssRuleChain* PR_NULLABLE parent) {
    if (atRule == CssAtRule_Apply) {
        bool allowed = false;
        for (const CssRuleChain* at = parent; at != nullptr; at = at->outer)
            allowed |= at->rule->kind == CssRuleDataKind_Style || isAt(at->rule, CssAtRule_Mixin);
        if (!allowed)
            return false;
    }
    CssAtRule within = kCssAtRuleWithin[atRule];
    if (within != CssAtRule_None) {
        for (const CssRuleChain* at = parent; at != nullptr; at = at->outer) {
            if (isAt(at->rule, within))
                return true;
        }
        return false;
    }
    if (parent == nullptr)
        return true;
    if ((contentsOf(parent) & CssContents_AtRules) == 0)
        return false;
    if (nested(parent))
        return bitsetTest(kCssAtRuleNests, atRule);
    if (functionBody(parent))
        return cssAtRuleConditional(atRule);

    // css-conditional-3/#contents-of: a group rule's block holds the same contents as a style sheet, except @import and
    // @namespace.
    if ((contentsOf(parent) & CssContents_QualifiedRules) != 0)
        return atRule != CssAtRule_Import && atRule != CssAtRule_Namespace;
    return false;
}

// Returns the position of a rule of the sheet's own list in the order that css-cascade-5/#at-import and
// css-namespaces-3/#declaration require: @import rules first, then @namespace rules, then the rest. An @layer statement
// may appear among the @import rules.
enum {
    Order_Import,
    Order_Namespace,
    Order_Rest,
};
typedef uint8_t Order;

bool isLayerStatement(const CssRuleData* rule) {
    return isAt(rule, CssAtRule_Layer) && !rule->block;
}

Order orderOf(const CssRuleData* rule) {
    if (isAt(rule, CssAtRule_Import))
        return Order_Import;
    if (isAt(rule, CssAtRule_Namespace))
        return Order_Namespace;
    return Order_Rest;
}

// ---------------------------------------------------------------------------------------------------------------------
// Building

// Creates a rule with one reference, which the list that receives it holds.
CssRuleData* newRule(CssRuleDataKind kind) {
    CssRuleData* rule = allocZeroed<CssRuleData>();
    rule->references = 1;
    rule->kind = kind;
    return rule;
}

// Gives an @font-feature-values rule an empty block for each feature type. Each block is the map for its type
// (css-fonts-4/#om-fontfeaturevalues). The blocks written in the rule add to these.
void addFeatureBlocks(CssRuleData* rule) {
    for (CssAtRule type = 0; type < CssAtRule_Count; ++type) {
        if (kCssAtRuleWithin[type] != CssAtRule_FontFeatureValues)
            continue;
        CssRuleData* block = newRule(CssRuleDataKind_At);
        block->atRule = type;
        block->block = true;
        block->style.rule = type;
        rule->children.push(block);
    }
}

// Adds the declarations of `written` to `earlier`. `earlier` is the block of the same at-rule, in the block of `rule`,
// that `written` merges into (kCssAtRuleMerged). The blocks that hold declarations come first, in the order in which
// their at-rules were first written.
void mergeBlock(CssParser* parser, CssRuleData* rule, CssRuleData* earlier, const CssRuleData* written) {
    if (earlier->style.count == 0 && written->style.count != 0) {
        RawArray<CssRuleData* PR_NONNULL>* blocks = &rule->children;
        uint32_t held = 0;
        while (blocks->data[held]->style.count != 0)
            ++held;
        uint32_t at = held;
        while (blocks->data[at] != earlier)
            ++at;
        blocks->erase(at);
        blocks->insert(held, earlier);
    }
    mergeDeclarations(parser, &earlier->style, &written->style);
}

// Parses the selectors of a style rule in the block of the rule that `parent` starts at. Nested selectors are relative,
// and `&` represents the nearest style rule's selectors (css-nesting-1/#nesting). Directly inside @scope, they are
// relative to an implied :scope (css-nesting-1/#nesting-at-scope).
PrCssSelectors* PR_NULLABLE parseSelectors(CssParser* parser, const CssTokenStream* ts,
        CssTokenSpan span, const CssSheetData* PR_NULLABLE sheet, const CssRuleChain* PR_NULLABLE parent) {
    CssSelectorContext context = {};
    context.sheet = sheet;
    const CssRuleData* style = styleAncestor(parent);
    context.parent = style != nullptr ? style->selectors : nullptr;
    if (isAt(ruleOf(parent), CssAtRule_Scope))
        context.nesting = CssSelectorNesting_Scope;
    else if (nested(parent))
        context.nesting = CssSelectorNesting_Style;
    return cssParseSelectors(parser, ts, span, &context);
}

// Returns the grammar of a keyframe rule's selectors, or of an at-rule's prelude in the form in which it is written.
CssGrammarIndex preludeRoot(const CssRuleData* rule) {
    if (rule->kind == CssRuleDataKind_Keyframe)
        return kCssAtRuleQualifiedPrelude[rule->style.rule];
    return rule->block ? kCssAtRuleBlockPrelude[resolved(rule)] : kCssAtRuleStatementPrelude[resolved(rule)];
}

// Returns true if `name` is one of the counter styles that no @counter-style rule may define
// (css-counter-styles-3/#the-counter-style-rule).
bool isNonOverridable(PrCssKeyword name) {
    switch (name) {
        case PrCssKeyword_Decimal:
        case PrCssKeyword_Disc:
        case PrCssKeyword_Square:
        case PrCssKeyword_Circle:
        case PrCssKeyword_DisclosureOpen:
        case PrCssKeyword_DisclosureClosed:
            return true;
        default:
            return false;
    }
}

// Returns true if the rule accepts a matched prelude, beyond what its grammar checks. A @counter-style rule's prelude
// must name a counter style that the rule may define. A @custom-media rule's list must not hold `not all` in place of
// an entry that is not a media query (mediaqueries/at-custom-media-parsing.html).
bool preludeFits(const CssRuleData* rule, CssValueRun run) {
    if (isAt(rule, CssAtRule_CounterStyle))
        return !isNonOverridable(cssKeywordFromText(cssValueText(run, run.values)));
    if (isAt(rule, CssAtRule_CustomMedia)) {
        for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
            if (cssValueIsProduction(&run.values[i], CssValueDef_MediaQuery) && run.values[i].next == 1)
                return false;
        }
    }
    return true;
}

// Replaces the rule's prelude with a matched one. Its media queries go to the rule's media list, and the rest is the
// prelude. @media's prelude is all media queries, and @import's prelude ends in them.
void holdPrelude(CssRuleData* rule, CssValueRun run) {
    uint32_t media = run.count;      // index at which the media queries start
    uint32_t queries = run.count;    // index of the first query, past the node that holds the queries
    if (isAt(rule, CssAtRule_Media)) {
        media = 0;
        queries = 0;
    }
    if (isAt(rule, CssAtRule_CustomMedia) && run.count > 1 && !cssValueIsKeyword(&run.values[1], PrCssKeyword_True) &&
            !cssValueIsKeyword(&run.values[1], PrCssKeyword_False)) {
        media = 1;
        queries = 1;
    }
    for (uint32_t i = 0; i < run.count && isAt(rule, CssAtRule_Import); i += run.values[i].next) {
        if (cssValueIsProduction(&run.values[i], CssValueDef_MediaImportCondition)) {
            media = i;
            queries = i + 1;
        }
    }
    cssValueListReset(&rule->prelude);
    cssValueListReset(&rule->media.queries);
    rule->prelude = cssValueListCopy(CssValueRun{run.values, media, run.text});
    rule->media.queries = cssValueListCopy(CssValueRun{run.values + queries, run.count - queries, run.text});
}

// Matches `span` as the prelude that `rule` accepts, onto the parser's pool, and writes it to `out`. Returns false if
// `span` is not such a prelude. A root of CssGrammarRoot_None accepts no tokens.
bool matchPrelude(CssParser* parser, const CssTokenStream* ts, CssTokenSpan span,
        const CssRuleData* rule, const CssSheetData* PR_NULLABLE sheet, CssValueSpan* out) {
    span = cssTrimWhitespace(ts->tokens, span);
    CssGrammarIndex root = preludeRoot(rule);
    if (root == CssGrammarRoot_None) {
        *out = CssValueSpan{parser->pool.values.count, 0};
        return span.count == 0;
    }

    CssTokenStream prelude = cssSubStream(ts, span);
    CssParseContext context = {sheet};
    CssValueSpan matched;
    bool parsed = cssParsePrelude(root, &prelude, &context, &parser->pool, &parser->match, &matched) &&
        preludeFits(rule, cssValuePoolRun(&parser->pool, matched));
    if (parsed && isAt(rule, CssAtRule_CustomMedia)) {
        // The media-query alternative also accepts these as media types. Custom media instead gives a lone true/false
        // its boolean meaning (mediaqueries-5/#custom-mq).
        CssTokenStream query = cssSubStream(ts, span);
        ++query.pos; // The extension name.
        cssSkipWhitespace(&query);
        if (query.pos + 1 == query.end && cssPeek(&query) == CssTokenKind_Ident) {
            PrCssKeyword keyword = cssTokenKeyword(query.tokens, query.pos);
            if (keyword == PrCssKeyword_True || keyword == PrCssKeyword_False) {
                parser->pool.values.data[matched.start + 1] = cssKeywordValue(keyword);
                matched.count = 2;
            }
        }
    }
    *out = matched;
    return parsed;
}

bool parsePrelude(CssParser* parser, const CssTokenStream* ts, CssTokenSpan span, CssRuleData* rule,
        const CssSheetData* PR_NULLABLE sheet) {
    uint32_t values = parser->pool.values.count;
    uint32_t text = parser->pool.text.count;
    CssValueSpan matched;
    bool parsed = matchPrelude(parser, ts, span, rule, sheet, &matched);
    if (parsed) {
        holdPrelude(rule, cssValuePoolRun(&parser->pool, matched));
        rule->selectors = cssCompilePreludeSelectors(parser, matched, sheet);
    }
    parser->pool.values.count = values;
    parser->pool.text.count = text;
    return parsed;
}

void buildBlock(CssParser* parser, const CssTokenStream* ts, CssTokenSpan span, CssRuleData* rule,
    const CssRuleChain* PR_NULLABLE outer, const CssSheetData* PR_NULLABLE sheet);

// Returns true if a built at-rule's block holds what its spec requires of it. An @property rule declares its syntax and
// whether it inherits. It also declares an initial value, unless its syntax is the universal syntax definition
// (css-properties-values-api-1/#at-property-rule).
bool blockFits(const CssRuleData* rule) {
    if (!isAt(rule, CssAtRule_Property))
        return true;
    const CssDeclarationBlock* block = &rule->style;
    uint32_t syntax = cssDeclarationBlockFind(block, cssDescriptorDeclaredName(PrCssDescriptor_PropertySyntax));
    uint32_t inherits = cssDeclarationBlockFind(block, cssDescriptorDeclaredName(PrCssDescriptor_PropertyInherits));
    uint32_t initial = cssDeclarationBlockFind(block, cssDescriptorDeclaredName(PrCssDescriptor_PropertyInitialValue));
    if (syntax == block->count || inherits == block->count)
        return false;
    CssValueRun definition = cssDeclarationValue(block, &cssDeclarations(block)[syntax]);
    return initial != block->count || prStringTrim(cssValueText(definition, definition.values)) == "*";
}

// Returns the at-rule that a parsed at-rule names as written, which may be a legacy alias. Returns CssAtRule_None for a
// name that no spec defines.
CssAtRule writtenAtRule(CssParser* parser, const CssTokenStream* ts, CssSyntaxRule syntax) {
    return cssAtRuleFromText(tokenView(&parser->names, ts, syntax.name));
}

// Returns the rule in the block of `parent` that a later `child` adds its declarations to, if the child's at-rule
// merges them (kCssAtRuleMerged). Returns null otherwise.
CssRuleData* PR_NULLABLE mergedInto(CssRuleData* parent, const CssRuleData* child) {
    if (child->kind != CssRuleDataKind_At || !bitsetTest(kCssAtRuleMerged, resolved(child)))
        return nullptr;
    for (uint32_t i = 0; i < parent->children.count; ++i) {
        if (isAt(parent->children.data[i], resolved(child)))
            return parent->children.data[i];
    }
    return nullptr;
}

// Builds the rule for a parsed rule in the block of `parent`, or in the sheet if `parent` is null, and writes it to
// `out`. Returns PrStatus_Syntax if the rule is invalid anywhere (cssom-1/#parse-a-css-rule), and PrStatus_NotAllowed
// if it is valid, but not in that block.
PrStatus buildRule(CssParser* parser, const CssTokenStream* ts, CssSyntaxRule syntax,
        const CssSheetData* PR_NULLABLE sheet, const CssRuleChain* PR_NULLABLE parent, CssRuleData* PR_NULLABLE* out) {
    bool block = (syntax.flags & CssSyntaxRuleFlag_HasBlock) != 0;
    if (syntax.type == CssSyntaxRuleType_Qualified) {
        // A qualified rule is a style rule, or the rule that the block's at-rule holds instead
        // (css-syntax-3/#consume-qualified-rule). A qualified rule needs a block.
        if (!block)
            return PrStatus_Syntax;
        if (parent != nullptr && (contentsOf(parent) & CssContents_QualifiedRules) == 0)
            return PrStatus_NotAllowed;
        CssAtRule holder =
            parent != nullptr && parent->rule->kind == CssRuleDataKind_At ? resolved(parent->rule) : CssAtRule_None;
        if (holder != CssAtRule_None && kCssAtRuleQualifiedContents[holder] != 0) {
            CssRuleData* rule = newRule(CssRuleDataKind_Keyframe);
            rule->style.rule = holder;
            if (!parsePrelude(parser, ts, syntax.prelude, rule, sheet)) {
                cssRuleDataRelease(rule);
                return PrStatus_Syntax;
            }
            buildBlock(parser, ts, syntax.block, rule, parent, sheet);
            *out = rule;
            return PrStatus_Ok;
        }

        PrCssSelectors* selectors = parseSelectors(parser, ts, cssTrimWhitespace(ts->tokens, syntax.prelude), sheet, parent);
        if (selectors == nullptr)
            return PrStatus_Syntax;
        CssRuleData* rule = newRule(CssRuleDataKind_Style);
        rule->selectors = selectors;
        buildBlock(parser, ts, syntax.block, rule, parent, sheet);
        *out = rule;
        return PrStatus_Ok;
    }

    CssAtRule written = writtenAtRule(parser, ts, syntax);
    CssAtRule atRule = kCssAtRuleAlias[written];
    if (atRule == CssAtRule_None || (block ? kCssAtRuleContents[atRule] == 0 : !bitsetTest(kCssAtRuleStatement, atRule)))
        return PrStatus_Syntax;
    if (!atRuleFits(atRule, parent))
        return PrStatus_NotAllowed;

    CssRuleData* rule = newRule(CssRuleDataKind_At);
    rule->atRule = written;
    rule->block = block;
    if (!parsePrelude(parser, ts, syntax.prelude, rule, sheet)) {
        cssRuleDataRelease(rule);
        return PrStatus_Syntax;
    }
    rule->style.rule = atRule;
    if (atRule == CssAtRule_FontFeatureValues)
        addFeatureBlocks(rule);
    if (block)
        buildBlock(parser, ts, syntax.block, rule, parent, sheet);
    if (!blockFits(rule)) {
        cssRuleDataRelease(rule);
        return PrStatus_Syntax;
    }
    *out = rule;
    return PrStatus_Ok;
}

// Adds to `rule` a nested declarations rule that holds the declarations staged from `first` on, if there are any
// (css-nesting-1/#nested-declarations-rule). The names of the declarations are resolved against the block of
// `declares`.
void endRun(CssParser* parser, CssRuleData* rule, uint32_t first, CssAtRule declares) {
    if (parser->declared.count != first) {
        CssRuleData* declarations = newRule(CssRuleDataKind_NestedDeclarations);
        declarations->style.rule = declares;
        write(&declarations->style, parser, first);
        rule->children.push(declarations);
    }
    parser->declared.count = first;
}

// Builds the rules and declarations of a rule's block, as css-syntax-3/#consume-block-contents reads them. A rule ends
// the run of declarations before it, if the run holds any. The exception is a rule that is not valid in the block,
// which the block reads past, although the spec ends the run there (css-syntax/var-with-blocks.html,
// csswg-drafts#11271).
//
// In a nested context, if the first item is a run of declarations, it holds the rule's own declarations. Every other
// run is a nested declarations rule (css-nesting-1/#nested-declarations-rule). In a block that holds declarations and
// no style rules, all of the declarations are the rule's own.
//
// A child's block is parsed after this one, onto the same arrays, so this block's items are read by index rather than
// kept.
void buildBlock(CssParser* parser, const CssTokenStream* ts, CssTokenSpan span, CssRuleData* rule,
        const CssRuleChain* PR_NULLABLE outer, const CssSheetData* PR_NULLABLE sheet) {
    CssRuleChain here = {rule, outer};
    CssTokenStream contents = cssSubStream(ts, span);
    CssBlock parsed = {};
    cssParseBlockContents(&contents, parser, &parsed);

    // css-mixins-1/#function-rule: in an @function body every run of declarations is a CSSFunctionDeclarations rule.
    CssContents holds = contentsOf(&here);
    bool declares = (holds & CssContents_Declarations) != 0;
    bool body = functionBody(&here);
    bool nests = holds == CssContents_BlockContents || body ||
        isAt(rule, CssAtRule_Apply) || isAt(rule, CssAtRule_Contents);
    bool own = !nests || rule->kind == CssRuleDataKind_Style;    // true if the next run is the rule's own
    CssAtRule runs = body ? CssAtRule_Function : CssAtRule_None; // the at-rule whose declarations a nested run holds

    Stage stage = beginStage(parser);
    NameFilter filter = makeNameFilter(parser, parsed.declarations.count);
    uint32_t run = parsed.items.start;       // the first item of the run of declarations
    uint32_t written = 0;                    // number of declarations in the run
    uint32_t end = parsed.items.start + parsed.items.count;
    for (uint32_t i = run; i <= end; ++i) {
        CssItemType type = i < end ? parser->items.data[i].type : CssItemType_Rule;
        if (type == CssItemType_Declaration)
            ++written;
        if (type != CssItemType_Rule)
            continue;
        CssRuleData* child = nullptr;
        if (i < end && buildRule(parser, ts, parser->rules.data[parser->items.data[i].index], sheet, &here, &child) !=
                PrStatus_Ok) {
            continue;
        }

        // The run ends here if it holds declarations. Outside a nested context, the rule's own declarations continue
        // past the rules among them, to the end.
        if (declares && written != 0 && (nests || i == end)) {
            stageDeclarations(parser, own ? rule->style.rule : runs, ts, run, i, stage.declared, filter);
            if (own) {
                write(&rule->style, parser, stage.declared);
                parser->declared.count = stage.declared;
            } else {
                endRun(parser, rule, stage.declared, runs);
            }
            own = !nests;
            written = 0;
        }
        if (i == end)
            break;
        if (nests)
            run = i + 1;

        CssRuleData* earlier = mergedInto(rule, child);
        if (earlier != nullptr) {
            mergeBlock(parser, rule, earlier, child);
            cssRuleDataRelease(child);
            continue;
        }
        rule->children.push(child);
        own = !nests;
    }
    endStage(parser, stage);
    cssParserDrop(parser, parsed);
}

// Adds the rules of a style sheet, in order. Drops each rule that is out of the order that css-cascade-5/#at-import and
// css-namespaces-3/#declaration require.
void addSheetRules(CssSheetData* sheet, CssParser* parser, const CssTokenStream* ts, CssBlock parsed) {
    sheet->rules.reserve(sheet->rules.count + parsed.rules.count);
    Order order = Order_Import;
    for (uint32_t i = 0; i < parsed.items.count; ++i) {
        CssItem item = parser->items.data[parsed.items.start + i];
        if (item.type != CssItemType_Rule)
            continue;
        CssRuleData* rule = nullptr;
        if (buildRule(parser, ts, parser->rules.data[item.index], sheet, nullptr, &rule) != PrStatus_Ok)
            continue;

        // An @layer statement may stand among the @import rules without ending them.
        Order at = isLayerStatement(rule) ? order : orderOf(rule);
        if (at < order) {
            cssRuleDataRelease(rule);
            continue;
        }
        order = at;
        sheet->rules.push(rule);
    }
}

// Recomputes the specificity of the style rules in `rules` and in the group rules among them. In those rules, `&`
// represents `parent`.
void respecify(const RawArray<CssRuleData* PR_NONNULL>& rules, const PrCssSelectors* parent) {
    for (uint32_t i = 0; i < rules.count; ++i) {
        CssRuleData* rule = rules.data[i];
        if (rule->kind == CssRuleDataKind_Style)
            cssSelectorsSpecify(rule->selectors, parent);
        else
            respecify(rule->children, parent);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Serialization

// Returns the declaration of the longhand `property`, or null if there is none.
const CssDeclaration* PR_NULLABLE findProperty(const CssDeclarationBlock* block, PrCssProperty property) {
    uint32_t index = declarerOf(cssDeclarations(block), block->count, property);
    return index != block->count ? &cssDeclarations(block)[index] : nullptr;
}

// The number of items that a listing holds in place. Up to this many, a listing finds a longhand by reading the items
// in turn.
constexpr uint32_t kListingHeld = 64;

// The declarations that a block lists, and the position of each longhand among them.
struct Listing {
    const CssDeclarationBlock* block;
    uint32_t count;                              // number of items

    // Set if the block holds a shared declaration. Points to `held`, or to `more` past kListingHeld items.
    CssDeclarationItem* PR_NULLABLE items;

    CssDeclarationItem held[kListingHeld];
    RawArray<CssDeclarationItem> more;
    RawArray<uint32_t> positions;                   // item position by property, used past kListingHeld items
};

void listDeclarations(const CssDeclarationBlock* block, Listing* out) {
    out->block = block;
    out->count = block->count;
    out->items = nullptr;
    const CssDeclaration* declarations = cssDeclarations(block);
    uint32_t shared = 0;
    while (shared < block->count && !isShared(declarations[shared]))
        ++shared;
    if (shared == block->count)
        return;

    out->count = block->length;
    out->items = out->held;
    if (block->length > kListingHeld) {
        out->more.reserve(block->length);
        out->items = out->more.data;
    }
    uint32_t at = 0;
    for (uint32_t i = 0; i < block->count; ++i) {
        forEachListed(declarations, block->count, i, [&](PrCssProperty property) {
            out->items[at++] = CssDeclarationItem{&declarations[i], property};
        });
    }
    PR_ASSERT(at == block->length);
    if (block->length <= kListingHeld)
        return;
    out->positions.reserve(PrCssProperty_Count);
    for (uint32_t i = 0; i < PrCssProperty_Count; ++i)
        out->positions.push(out->count);
    for (uint32_t i = 0; i < out->count; ++i) {
        if (out->items[i].property != PrCssProperty_Count)
            out->positions.data[out->items[i].property] = i;
    }
}

CssDeclarationItem itemOf(const Listing* listing, uint32_t index) {
    if (listing->items != nullptr)
        return listing->items[index];
    const CssDeclaration* declaration = &cssDeclarations(listing->block)[index];
    return CssDeclarationItem{declaration, declaration->property};
}

// Returns the position of the longhand `property` among the listing's items, or the number of items if it is not
// listed.
uint32_t positionOf(const Listing* listing, PrCssProperty property) {
    if (listing->positions.count != 0)
        return listing->positions.data[property];
    uint32_t i = 0;
    while (i < listing->count && itemOf(listing, i).property != property)
        ++i;
    return i;
}

void listingReset(Listing* listing) {
    listing->more.reset();
    listing->positions.reset();
}

// The number of longhand values that LonghandValues holds in place before it spills to `more`.
constexpr uint32_t kLonghandsHeld = 32;

// The values of the longhands of a shorthand, in canonical order, when all are declared with the same importance.
struct LonghandValues {
    CssValueRun held[kLonghandsHeld];
    RawArray<CssValueRun> more;
    const CssValueRun* values;
    PrCssProperty pending;     // the shorthand that the first value is pending on, or PrCssProperty_Count if none
    bool mixed;                // true if some value is not pending on `pending`
    bool important;
    uint32_t first;          // with a listing: the position of the first longhand
    uint32_t last;           // with a listing: the position of the last longhand
};

// Gathers the values of the longhands of `shorthand`, using the block's listing if there is one. Returns false if a
// longhand is not declared, or if their importance differs.
bool gatherLonghands(const CssDeclarationBlock* block, const Listing* PR_NULLABLE listing,
        PrCssProperty shorthand, LonghandValues* out) {
    CssPropertyList longhands = cssPropertyLonghands(shorthand);
    CssValueRun* values = out->held;
    if (longhands.count > kLonghandsHeld) {
        out->more.reserve(longhands.count);
        values = out->more.data;
    }
    out->values = values;
    out->first = UINT32_MAX;
    out->last = 0;
    for (uint32_t i = 0; i < longhands.count; ++i) {
        const CssDeclaration* declaration = nullptr;
        if (listing == nullptr) {
            declaration = findProperty(block, longhands.data[i]);
        } else {
            uint32_t position = positionOf(listing, longhands.data[i]);
            if (position != listing->count) {
                declaration = itemOf(listing, position).declaration;
                out->first = position < out->first ? position : out->first;
                out->last = position > out->last ? position : out->last;
            }
        }
        if (declaration == nullptr)
            return false;
        if (i == 0) {
            out->important = declaration->important;
            out->pending = declaration->shorthand;
        }
        if (declaration->important != out->important)
            return false;
        out->mixed = out->mixed || declaration->shorthand != out->pending;
        values[i] = cssDeclarationValue(block, declaration);
    }
    return true;
}

// Serializes the value of `shorthand` from its gathered longhands (cssom-1/#serialize-a-css-value). If each longhand is
// pending on the shorthand, writes the value that the shorthand declared. If any is pending on another shorthand,
// writes nothing. Otherwise writes the shorthand's serialization of the longhands' values.
void serializeGathered(PrTextBuffer* out, PrCssProperty shorthand, const LonghandValues* gathered) {
    if (gathered->mixed)
        return;
    if (gathered->pending == shorthand)
        cssSerializeValues(out, gathered->values[0]);
    else if (gathered->pending == PrCssProperty_Count)
        cssSerializeShorthand(out, shorthand, gathered->values);
}

// Serializes a shorthand's value from its longhands' values (cssom-1/#dom-cssstyledeclaration-getpropertyvalue).
void serializeShorthand(PrTextBuffer* out, const CssDeclarationBlock* block, PrCssProperty shorthand) {
    LonghandValues gathered = {};
    if (gatherLonghands(block, nullptr, shorthand, &gathered))
        serializeGathered(out, shorthand, &gathered);
    gathered.more.reset();
}

// Writes the longhands of `shorthand` as the shorthand, as in the shorthand loop of
// cssom-1/#serialize-a-css-declaration-block. Each longhand must be declared, and none may be before the listing's item
// at `first` or already `written`. Their importance must be the same, and the shorthand must be able to write their
// values. Returns false otherwise.
bool serializeAsShorthand(PrTextBuffer* out, const CssDeclarationBlock* block, const Listing* listing,
        PrCssProperty shorthand, uint32_t first, PropertySet* written) {
    CssPropertyList longhands = cssPropertyLonghands(shorthand);
    if (longhands.count > listing->count - first)
        return false;
    LonghandValues gathered = {};
    bool writes = gatherLonghands(block, listing, shorthand, &gathered) && gathered.first >= first;
    for (uint32_t i = 0; i < longhands.count && writes; ++i)
        writes = !bitsetTest(written->words, longhands.data[i]);
    uint32_t last = writes ? gathered.last : first;
    // cssom-1/#serialize-into-a-shorthand-form: moving a longhand across an intermixed property with a different
    // mapping in its logical group would change their order.
    for (uint32_t i = first + 1; i < last && writes; ++i) {
        PrCssProperty property = itemOf(listing, i).property;
        if (property == PrCssProperty_Count || cssShorthandSets(shorthand, property))
            continue;
        uint8_t group = kCssPropertyLogicalGroup[property];
        if (group == 0)
            continue;
        bool flowRelative = bitsetTest(kCssPropertyFlowRelative, property);
        for (uint32_t j = 0; j < longhands.count; ++j) {
            PrCssProperty other = longhands.data[j];
            if (group == kCssPropertyLogicalGroup[other] && flowRelative != bitsetTest(kCssPropertyFlowRelative, other)) {
                writes = false;
                break;
            }
        }
    }

    uint32_t start = out->size;
    if (writes) {
        prTextBufferAppend(out, cssPropertyName(shorthand));
        prTextBufferAppend(out, ": ");
        uint32_t value = out->size;
        serializeGathered(out, shorthand, &gathered);
        writes = out->size != value;
    }
    if (!writes) {
        out->size = start;
    } else {
        if (gathered.important)
            prTextBufferAppend(out, " !important");
        prTextBufferAppendChar(out, ';');
        for (uint32_t i = 0; i < longhands.count; ++i)
            bitsetSet(written->words, longhands.data[i]);
    }
    gathered.more.reset();
    return writes;
}

// Returns the number of values in a range descriptor's run that serialize. That is all of them, except a last bound
// that equals the one before it (css-fonts-4/#serializing-at-rules).
uint32_t boundsWritten(CssValueRun run) {
    if (run.count < 2)
        return run.count;
    const CssValueNode* upper = &run.values[run.count - 1];
    const CssValueNode* lower = upper - 1;
    bool same = lower->next == 1 && lower->kind == upper->kind && lower->unit == upper->unit &&
        lower->aux == upper->aux && lower->number == upper->number;
    return same ? run.count - 1 : run.count;
}

// Serializes one declaration as `name: value`, with `!important` if set, and a closing semicolon
// (cssom-1/#serialize-a-css-declaration). A feature value's name is escaped as an identifier
// (css-fonts-4/#font-feature-values-syntax).
void serializeDeclaration(PrTextBuffer* out, const CssDeclarationBlock* block, CssDeclarationItem item) {
    const CssDeclaration* declaration = item.declaration;
    if (item.property != PrCssProperty_Count)
        prTextBufferAppend(out, cssPropertyName(item.property));
    else if (kCssAtRuleNamedValue[block->rule] != CssGrammarRoot_None)
        cssSerializeIdent(out, cssDeclarationName(block, declaration));
    else
        prTextBufferAppend(out, cssDeclarationName(block, declaration));
    prTextBufferAppend(out, ": ");
    cssSerializeDeclarationValue(out, block, declaration);
    if (declaration->important)
        prTextBufferAppend(out, " !important");
    prTextBufferAppendChar(out, ';');
}

// Writes `{`, the rule's declarations and child rules each on its own line, and `}`. Style rules and grouping rules
// write their blocks this way (cssom-1/#serialize-a-css-rule). A child whose serialization is empty gets no line.
void serializeBlock(PrTextBuffer* out, const CssRuleData* rule) {
    prTextBufferAppend(out, " {");
    if (rule->style.count != 0) {
        prTextBufferAppend(out, "\n  ");
        cssSerializeDeclarationBlock(out, &rule->style);
    }
    for (uint32_t i = 0; i < rule->children.count; ++i) {
        uint32_t line = out->size;
        prTextBufferAppend(out, "\n  ");
        uint32_t start = out->size;
        cssSerializeRule(out, rule->children.data[i]);
        if (out->size == start)
            out->size = line;
    }
    prTextBufferAppend(out, "\n}");
}

// Writes `{ `, the declarations, and ` }`. A rule whose block holds declarations is written this way. Any child rules
// follow the declarations on the same line. An empty feature value block writes nothing.
void serializeDeclarations(PrTextBuffer* out, const CssRuleData* rule) {
    prTextBufferAppend(out, " {");
    if (rule->style.count != 0) {
        prTextBufferAppendChar(out, ' ');
        cssSerializeDeclarationBlock(out, &rule->style);
    }
    for (uint32_t i = 0; i < rule->children.count; ++i) {
        const CssRuleData* child = rule->children.data[i];
        // A block that others merge into serializes only if it holds declarations.
        if (child->kind == CssRuleDataKind_At && bitsetTest(kCssAtRuleMerged, resolved(child)) && child->style.count == 0)
            continue;
        prTextBufferAppendChar(out, ' ');
        cssSerializeRule(out, child);
    }
    prTextBufferAppend(out, " }");
}

// Writes `{ `, the declarations of an @property rule in the order syntax, inherits and initial-value, and ` }`
// (css-properties-values-api-1/#serialize-a-csspropertyrule).
void serializePropertyDeclarations(PrTextBuffer* out, const CssRuleData* rule) {
    static constexpr PrCssDescriptor kOrder[] = {
        PrCssDescriptor_PropertySyntax,
        PrCssDescriptor_PropertyInherits,
        PrCssDescriptor_PropertyInitialValue,
    };
    prTextBufferAppend(out, " {");
    for (PrCssDescriptor descriptor : kOrder) {
        uint32_t index = cssDeclarationBlockFind(&rule->style, cssDescriptorDeclaredName(descriptor));
        if (index != rule->style.count) {
            prTextBufferAppendChar(out, ' ');
            serializeDeclaration(out, &rule->style,
                CssDeclarationItem{&cssDeclarations(&rule->style)[index], PrCssProperty_Count});
        }
    }
    prTextBufferAppend(out, " }");
}

void serializeAtRule(PrTextBuffer* out, const CssRuleData* rule) {
    CssAtRule atRule = resolved(rule);
    prTextBufferAppendChar(out, '@');
    prTextBufferAppend(out, cssAtRuleName(rule->atRule));

    // cssom-1/#serialize-a-css-rule writes the space after @media, whatever follows it.
    if (rule->prelude.count != 0 || rule->media.queries.count != 0 || atRule == CssAtRule_Media)
        prTextBufferAppendChar(out, ' ');
    cssSerializePrelude(out, rule);

    if (!rule->block) {
        prTextBufferAppendChar(out, ';');
        return;
    }
    if ((kCssAtRuleContents[atRule] & CssContents_QualifiedRules) != 0)
        serializeBlock(out, rule);
    else if (atRule == CssAtRule_Property)
        serializePropertyDeclarations(out, rule);
    else
        serializeDeclarations(out, rule);
}

// ---------------------------------------------------------------------------------------------------------------------
// Media queries

// Matches the media queries in `text` onto the end of the parser's pool (cssom-1/#parse-a-media-query-list).
CssValueSpan parseMediaQueries(CssParser* parser, PrStringView text) {
    CssTokenStream ts = cssParserTokenize(parser, text);
    CssParseContext context = {};
    CssValueSpan matched = {parser->pool.values.count, 0};
    cssParseProduction(CssValueDef_MediaQueryList, &ts, &context, &parser->pool, &parser->match, &matched);
    return matched;
}

// Returns true if a parsed media query list is one media query (cssom-1/#parse-a-media-query).
bool isOneQuery(CssValueRun run) {
    return run.count != 0 && run.values[0].next == run.count;
}

// Returns true if `query` serializes as `serialized`. Writes the serialization to `scratch`
// (cssom-1/#compare-media-queries).
bool isQuery(PrTextBuffer* scratch, CssValueRun query, PrStringView serialized) {
    prTextBufferClear(scratch);
    cssSerializeValues(scratch, query);
    return prTextBufferView(scratch) == serialized;
}

// Returns the index of the list's first query that equals `query`, or the list's length if there is none.
uint32_t findQuery(const CssMediaQueries* list, CssValueRun query) {
    PrTextBuffer wanted = {};
    PrTextBuffer held = {};
    cssSerializeValues(&wanted, query);
    uint32_t length = cssMediaListLength(list);
    uint32_t index = 0;
    while (index < length && !isQuery(&held, cssMediaQueryAt(list, index), prTextBufferView(&wanted)))
        ++index;
    prTextBufferReset(&wanted);
    prTextBufferReset(&held);
    return index;
}

// Copies the block into `out`, keeping its form.
void copyBlock(CssDeclarationBlock* out, const CssDeclarationBlock* block) {
    if (block->stored != nullptr) {
        copyMutable(out, block, block->capacity);
        return;
    }
    *out = *block;
    if (block->count == 0) {
        out->declarations = nullptr;
        return;
    }
    size_t size = sizeof(CssValueNode) * block->valueCount + sizeof(CssDeclaration) * block->count + block->textSize;
    CssValueNode* held = static_cast<CssValueNode*>(prMemoryAlloc(size));
    prMemCopy(held, packedHeld(block), size);
    out->declarations = reinterpret_cast<CssDeclaration*>(held + block->valueCount);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Declarations

const char* PR_NULLABLE cssDeclarationText(const CssDeclarationBlock* block, const CssDeclaration* declaration) {
    if (block->stored == nullptr)
        return reinterpret_cast<const char*>(block->declarations + block->count);
    const CssValueNode* held = block->stored[declaration - block->declarations];
    return reinterpret_cast<const char*>(held + declaration->valueCount);
}

PrStringView cssDeclarationName(const CssDeclarationBlock* block, const CssDeclaration* declaration) {
    if (declaration->property != PrCssProperty_Count)
        return cssPropertyName(declaration->property);
    if (declaration->descriptor != PrCssDescriptor_None)
        return cssDescriptorName(declaration->descriptor);
    return authorText(block, declaration);
}

CssValueRun cssDeclarationValue(const CssDeclarationBlock* block, const CssDeclaration* declaration) {
    const CssValueNode* values;
    if (block->stored != nullptr)
        values = block->stored[declaration - block->declarations];
    else
        values = packedHeld(block);
    return CssValueRun{values + declaration->value, declaration->valueCount, cssDeclarationText(block, declaration)};
}

uint32_t cssDeclarationBlockLength(const CssDeclarationBlock* block) {
    return block->length;
}

CssDeclarationItem cssDeclarationBlockItem(const CssDeclarationBlock* block, uint32_t index) {
    const CssDeclaration* declarations = cssDeclarations(block);
    CssDeclarationItem item = {};
    for (uint32_t i = 0; item.declaration == nullptr; ++i) {
        PR_ASSERT(i < block->count);
        forEachListed(declarations, block->count, i, [&](PrCssProperty property) {
            if (item.declaration == nullptr && index-- == 0)
                item = CssDeclarationItem{&declarations[i], property};
        });
    }
    return item;
}

// ---------------------------------------------------------------------------------------------------------------------
// Media lists

uint32_t cssMediaListLength(const CssMediaQueries* list) {
    // The queries and the ','s between them alternate.
    uint32_t values = 0;
    for (uint32_t i = 0; i < list->queries.count; i += list->queries.values[i].next)
        ++values;
    return (values + 1) / 2;
}

CssValueRun cssMediaQueryAt(const CssMediaQueries* list, uint32_t index) {
    CssValueRun run = cssValueListRun(&list->queries);
    uint32_t at = 0;
    for (uint32_t i = 0; i < index * 2; ++i)
        at += run.values[at].next;
    return cssValueSubtree(run, at);
}

// ---------------------------------------------------------------------------------------------------------------------
// Rules

PrCssRuleType cssRuleType(const CssRuleData* rule) {
    switch (rule->kind) {
        case CssRuleDataKind_Style:
            return PrCssRuleType_Style;
        case CssRuleDataKind_Keyframe:
            return PrCssRuleType_Keyframe;
        case CssRuleDataKind_At:
            return kCssAtRuleType[resolved(rule)];
        default:
            return 0;
    }
}

bool cssAtRuleConditional(CssAtRule atRule) {
    switch (atRule) {
        case CssAtRule_Media:
        case CssAtRule_Supports:
        case CssAtRule_Container:
            return true;
        default:
            return false;
    }
}

uint32_t cssFeatureBlock(const CssRuleData* rule, CssAtRule type) {
    uint32_t i = 0;
    while (resolved(rule->children.data[i]) != type)
        ++i;
    return i;
}

// ---------------------------------------------------------------------------------------------------------------------
// Lifetime

CssSheetData* cssSheetDataCreate() {
    CssSheetData* sheet = allocZeroed<CssSheetData>();
    sheet->references = 1;
    sheet->href = prRawStringNull();
    return sheet;
}

void cssSheetDataRetain(CssSheetData* sheet) {
    ++sheet->references;
}

void cssSheetDataRelease(CssSheetData* PR_NULLABLE sheet) {
    if (sheet == nullptr || --sheet->references != 0)
        return;
    for (uint32_t i = 0; i < sheet->rules.count; ++i)
        cssRuleDataRelease(sheet->rules.data[i]);
    sheet->rules.reset();
    prRawStringReset(&sheet->href);
    prMemoryFree(sheet);
}

void cssRuleDataRetain(CssRuleData* rule) {
    ++rule->references;
}

void cssRuleDataRelease(CssRuleData* PR_NULLABLE rule) {
    if (rule == nullptr || --rule->references != 0)
        return;
    for (uint32_t i = 0; i < rule->children.count; ++i)
        cssRuleDataRelease(rule->children.data[i]);
    rule->children.reset();
    cssDeclarationBlockReset(&rule->style);
    cssSelectorsDestroy(rule->selectors);
    cssValueListReset(&rule->prelude);
    cssValueListReset(&rule->media.queries);
    cssSheetDataRelease(rule->imported);
    prMemoryFree(rule);
}

CssRuleData* cssRuleDataCopy(const CssRuleData* rule) {
    PR_ASSERT(rule->imported == nullptr);
    CssRuleData* copy = newRule(rule->kind);
    copy->atRule = rule->atRule;
    copy->block = rule->block;
    copy->selectors = rule->selectors != nullptr ? cssSelectorsCopy(rule->selectors) : nullptr;
    copy->prelude = cssValueListCopy(cssValueListRun(&rule->prelude));
    copy->media.queries = cssValueListCopy(cssValueListRun(&rule->media.queries));
    copyBlock(&copy->style, &rule->style);
    copy->children.reserve(rule->children.count);
    for (uint32_t i = 0; i < rule->children.count; ++i) {
        cssRuleDataRetain(rule->children.data[i]);
        copy->children.push(rule->children.data[i]);
    }
    return copy;
}

CssSheetData* cssSheetDataCopy(const CssSheetData* sheet) {
    CssSheetData* copy = cssSheetDataCreate();
    copy->href = prRawStringClone(&sheet->href);
    copy->rules.reserve(sheet->rules.count);
    for (uint32_t i = 0; i < sheet->rules.count; ++i) {
        cssRuleDataRetain(sheet->rules.data[i]);
        copy->rules.push(sheet->rules.data[i]);
    }
    return copy;
}

bool cssSheetDataImports(const CssSheetData* sheet) {
    // The @import rules come first, among the @layer statements.
    for (uint32_t i = 0; i < sheet->rules.count; ++i) {
        const CssRuleData* rule = sheet->rules.data[i];
        if (isAt(rule, CssAtRule_Import))
            return true;
        if (!isLayerStatement(rule))
            return false;
    }
    return false;
}

void cssDeclarationBlockReset(CssDeclarationBlock* block) {
    if (block->stored != nullptr) {
        for (uint32_t i = 0; i < block->count; ++i)
            prMemoryFree(block->stored[i]);
        prMemoryFree(block->stored);
        prMemoryFree(block->declarations);
    } else {
        prMemoryFree(packedHeld(block));
    }
    block->declarations = nullptr;
    block->stored = nullptr;
    block->count = 0;
    block->length = 0;
    block->capacity = 0;
    block->valueCount = 0;
    block->textSize = 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Edits

void cssEditBegin(CssEdit* edit, CssParserSlot slot) {
    cssParserBegin(&edit->scope, slot);
    edit->kind = CssEditKind_None;
    edit->name = CssDeclaredName{};
    edit->staged = CssValueSpan{};
    edit->selectors = nullptr;
    edit->rule = nullptr;
    edit->index = 0;
    edit->values = edit->scope.parser->pool.values.count;
    edit->text = edit->scope.parser->pool.text.count;
}

void cssEditEnd(CssEdit* edit) {
    CssParser* parser = edit->scope.parser;
    if (edit->selectors != nullptr)
        cssSelectorsDestroy(edit->selectors);
    cssRuleDataRelease(edit->rule);
    parser->incoming.count = 0;
    parser->pool.values.count = edit->values;
    parser->pool.text.count = edit->text;
    cssParserEnd(&edit->scope);
}

// ---------------------------------------------------------------------------------------------------------------------
// Parsing

void cssSheetDataParse(CssSheetData* sheet, PrStringView text, CssParserSlot slot, CssOrigin origin) {
    for (uint32_t i = 0; i < sheet->rules.count; ++i)
        cssRuleDataRelease(sheet->rules.data[i]);
    sheet->rules.count = 0;

    CssParserScope scope;
    cssParserBegin(&scope, slot);
    CssParser* parser = scope.parser;
    CssTokenStream ts = cssParserTokenize(parser, text);

    CssBlock parsed = {};
    parser->match.engine = origin != CssOrigin_Author;
    cssParseStylesheetContents(&ts, parser, &parsed);
    addSheetRules(sheet, parser, &ts, parsed);
    parser->match.engine = false;
    cssParserDrop(parser, parsed);
    cssParserEnd(&scope);
}

CssEditResult cssStyleRulePrepareSelectorText(CssEdit* edit, const CssRuleChain* PR_NULLABLE outer,
        const CssSheetData* PR_NULLABLE sheet, PrStringView text) {
    CssParser* parser = edit->scope.parser;
    CssTokenStream ts = cssParserTokenize(parser, text);
    edit->selectors = parseSelectors(parser, &ts, cssStreamTrimmed(&ts), sheet, outer);
    if (edit->selectors == nullptr)
        return CssEditResult_Rejected;
    edit->kind = CssEditKind_Selectors;
    return CssEditResult_Changed;
}

CssEditResult cssRulePreparePrelude(CssEdit* edit, const CssRuleData* rule,
        const CssSheetData* PR_NULLABLE sheet, PrStringView text) {
    CssParser* parser = edit->scope.parser;
    CssTokenStream ts = cssParserTokenize(parser, text);
    if (!matchPrelude(parser, &ts, cssStreamAll(&ts), rule, sheet, &edit->staged))
        return CssEditResult_Rejected;
    edit->selectors = cssCompilePreludeSelectors(parser, edit->staged, sheet);
    edit->kind = CssEditKind_Prelude;
    return CssEditResult_Changed;
}

void cssRuleCommit(CssEdit* edit, CssRuleData* rule) {
    if (edit->kind == CssEditKind_Selectors || edit->kind == CssEditKind_Prelude) {
        cssSelectorsDestroy(rule->selectors);
        rule->selectors = edit->selectors;
        edit->selectors = nullptr;
    }
    if (edit->kind == CssEditKind_Selectors)
        respecify(rule->children, rule->selectors);
    else if (edit->kind == CssEditKind_Prelude)
        holdPrelude(rule, cssValuePoolRun(&edit->scope.parser->pool, edit->staged));
}

void cssNamespaceRuleParts(const CssRuleData* rule, PrStringView* prefix, PrStringView* url) {
    // The prelude is a <namespace-prefix> if one is written, then a <string> or a <url>. The <string> or <url> is the
    // only value in the prelude that has text.
    CssValueRun run = cssValueListRun(&rule->prelude);
    const CssValueNode* last = &run.values[0];
    *prefix = PrStringView{};
    if (cssValueIsProduction(last, CssValueDef_NamespacePrefix)) {
        const CssValueNode* name = &run.values[1];
        *prefix = cssValueText(run, name);
        last = &run.values[last->next];
    }
    while (!cssValueHasText(last->kind))
        ++last;
    *url = cssValueText(run, last);
}

bool cssSheetNamespace(const CssSheetData* sheet, PrStringView prefix, PrStringView* url) {
    // css-namespaces-3/#declaration: the @namespace rules follow the @import rules and precede every other rule. If a
    // prefix is declared twice, the last declaration wins.
    bool found = false;
    for (uint32_t i = 0; i < sheet->rules.count; ++i) {
        const CssRuleData* rule = sheet->rules.data[i];
        Order order = orderOf(rule);
        if (order == Order_Rest && !isLayerStatement(rule))
            break;
        if (order != Order_Namespace)
            continue;

        PrStringView declared;
        PrStringView declaredUrl;
        cssNamespaceRuleParts(rule, &declared, &declaredUrl);
        bool matches = prefix.data == nullptr ? declared.data == nullptr : declared.data != nullptr && declared == prefix;
        if (matches) {
            *url = declaredUrl;
            found = true;
        }
    }
    return found;
}

uint32_t cssKeyframesFind(const CssRuleData* rule, PrStringView select, CssParserSlot slot) {
    CssParserScope scope;
    cssParserBegin(&scope, slot);
    CssParser* parser = scope.parser;
    CssTokenStream ts = cssParserTokenize(parser, select);
    CssTokenStream selectors = cssSubStream(&ts, cssStreamTrimmed(&ts));
    CssParseContext context = {nullptr};
    uint32_t values = parser->pool.values.count;
    uint32_t text = parser->pool.text.count;
    CssValueSpan matched;
    uint32_t found = rule->children.count;
    if (cssParsePrelude(kCssAtRuleQualifiedPrelude[resolved(rule)], &selectors, &context, &parser->pool, &parser->match,
            &matched)) {
        // Two selector lists are equal if their serializations are.
        PrTextBuffer wanted = {};
        PrTextBuffer held = {};
        cssSerializeValues(&wanted, cssValuePoolRun(&parser->pool, matched));
        while (found != 0) {
            prTextBufferClear(&held);
            cssSerializePrelude(&held, rule->children.data[found - 1]);
            if (prTextBufferView(&held) == prTextBufferView(&wanted))
                break;
            --found;
        }
        found = found != 0 ? found - 1 : rule->children.count;
        prTextBufferReset(&wanted);
        prTextBufferReset(&held);
    }
    parser->pool.values.count = values;
    parser->pool.text.count = text;
    cssParserEnd(&scope);
    return found;
}

void cssMediaListParse(CssMediaQueries* list, PrStringView text) {
    CssEdit edit;
    cssEditBegin(&edit, nullptr);
    if (cssMediaListPrepareText(&edit, list, text) == CssEditResult_Changed)
        cssMediaListCommit(&edit, list);
    cssEditEnd(&edit);
}

CssEditResult cssMediaListPrepareText(CssEdit* edit, const CssMediaQueries* list, PrStringView text) {
    CssParser* parser = edit->scope.parser;
    edit->staged = parseMediaQueries(parser, text);
    edit->kind = CssEditKind_Media;
    bool same = cssValueRunEquals(cssValuePoolRun(&parser->pool, edit->staged), cssValueListRun(&list->queries));
    return same ? CssEditResult_Unchanged : CssEditResult_Changed;
}

CssEditResult cssMediaListPrepareAppend(CssEdit* edit, const CssMediaQueries* list, PrStringView text) {
    CssParser* parser = edit->scope.parser;
    CssValuePool* pool = &parser->pool;

    // The list's queries, then a ',' if the list holds any, then the new query.
    CssValueSpan whole = cssValuePoolAddRun(pool, cssValueListRun(&list->queries));
    if (whole.count != 0)
        cssValuePoolAdd(pool, cssLiteralValue(','));
    CssValueRun query = cssValuePoolRun(pool, parseMediaQueries(parser, text));
    if (!isOneQuery(query) || findQuery(list, query) != cssMediaListLength(list))
        return CssEditResult_Unchanged;
    whole.count = pool->values.count - whole.start;
    edit->staged = whole;
    edit->kind = CssEditKind_Media;
    return CssEditResult_Changed;
}

CssEditResult cssMediaListPrepareDelete(CssEdit* edit, const CssMediaQueries* list, PrStringView text) {
    CssParser* parser = edit->scope.parser;
    CssValuePool* pool = &parser->pool;
    CssValueSpan parsed = parseMediaQueries(parser, text);
    if (!isOneQuery(cssValuePoolRun(pool, parsed)))
        return CssEditResult_Unchanged;

    // The queries that are not the one `text` holds, joined by ','.
    PrTextBuffer wanted = {};
    PrTextBuffer held = {};
    cssSerializeValues(&wanted, cssValuePoolRun(pool, parsed));
    uint32_t length = cssMediaListLength(list);
    CssValueSpan kept = {pool->values.count, 0};
    uint32_t removed = 0;
    for (uint32_t i = 0; i < length; ++i) {
        CssValueRun at = cssMediaQueryAt(list, i);
        if (isQuery(&held, at, prTextBufferView(&wanted))) {
            ++removed;
            continue;
        }
        if (pool->values.count != kept.start)
            cssValuePoolAdd(pool, cssLiteralValue(','));
        cssValuePoolAddRun(pool, at);
    }
    prTextBufferReset(&wanted);
    prTextBufferReset(&held);
    if (removed == 0)
        return CssEditResult_Rejected;
    kept.count = pool->values.count - kept.start;
    edit->staged = kept;
    edit->kind = CssEditKind_Media;
    return CssEditResult_Changed;
}

void cssMediaListCommit(CssEdit* edit, CssMediaQueries* list) {
    cssValueListReset(&list->queries);
    list->queries = cssValueListCopy(cssValuePoolRun(&edit->scope.parser->pool, edit->staged));
}

void cssDeclarationBlockParse(CssDeclarationBlock* block, PrStringView text, CssParserSlot slot, CssOrigin origin) {
    CssParserScope scope;
    cssParserBegin(&scope, slot);
    scope.parser->match.engine = origin != CssOrigin_Author;
    parseDeclarations(scope.parser, block, text);
    scope.parser->match.engine = false;
    cssParserEnd(&scope);
}

// ---------------------------------------------------------------------------------------------------------------------
// Declarations

bool cssNameFromText(CssAtRule rule, PrStringView text, CssDeclaredName* out) {
    return resolveName(rule, text, out);
}

bool cssDeclaredNameFromName(CssAtRule rule, const PrCssDeclarationName* name, CssDeclaredName* out) {
    PrStringView author = prRawStringView(&name->author);
    if (author.data != nullptr)
        return authorName(rule, author, out);
    if (name->descriptor != PrCssDescriptor_None) {
        // The block does not declare a descriptor of another rule.
        if (name->descriptor >= PrCssDescriptor_Count ||
                cssDescriptorFromText(rule, cssDescriptorName(name->descriptor)) != name->descriptor)
            return false;
        *out = cssDescriptorDeclaredName(kCssDescriptorAlias[name->descriptor]);
        return true;
    }
    if (name->property >= PrCssProperty_Count || kCssAtRuleProperties[rule] != CssPropertiesIn_All)
        return false;
    *out = cssPropertyDeclaredName(kCssPropertyAlias[name->property]);
    return true;
}

CssDeclaredName cssPropertyDeclaredName(PrCssProperty property) {
    return CssDeclaredName{property, PrCssDescriptor_None, CssGrammarRoot_None, PrStringView{}, property, false};
}

CssDeclaredName cssDescriptorDeclaredName(PrCssDescriptor descriptor) {
    return CssDeclaredName{
        PrCssProperty_Count,
        descriptor,
        CssGrammarRoot_None,
        PrStringView{},
        uint32_t(PrCssProperty_Count + descriptor),
        bitsetTest(kCssDescriptorUnicodeRanges, descriptor),
    };
}

uint32_t cssDeclarationBlockFind(const CssDeclarationBlock* block, const CssDeclaredName& name) {
    if (name.property != PrCssProperty_Count && cssIsShorthand(name.property))
        return block->count;
    if (name.property != PrCssProperty_Count)
        return declarerOf(cssDeclarations(block), block->count, name.property);
    CssDeclaration key = {};
    key.hash = name.hash;
    key.property = name.property;
    key.descriptor = name.descriptor;
    key.nameSize = uint32_t(name.text.size);
    const CssDeclaration* declarations = cssDeclarations(block);
    for (uint32_t i = 0; i < block->count; ++i) {
        if (declarationNameEquals(&declarations[i], cssDeclarationText(block, &declarations[i]), &key, name.text.data))
            return i;
    }
    return block->count;
}

bool cssDeclarationSupported(const CssDeclaredName& name, PrStringView value) {
    CssParserScope scope;
    cssParserBegin(&scope, nullptr);
    CssParser* parser = scope.parser;
    CssTokenStream ts = cssParserTokenize(parser, value);
    CssTokenSpan span = cssStreamTrimmed(&ts);

    // css-variables-1/#defining-variables: a custom property's value may be empty, and no other property's may.
    bool supported = (span.count != 0 || name.text.data != nullptr) && declare(parser, name, &ts, span, false);
    cssParserEnd(&scope);
    return supported;
}

void cssSerializePropertyValue(PrTextBuffer* out, const CssDeclarationBlock* block, const CssDeclaredName& name) {
    if (name.property != PrCssProperty_Count && cssIsShorthand(name.property)) {
        serializeShorthand(out, block, name.property);
        return;
    }
    uint32_t index = cssDeclarationBlockFind(block, name);
    if (index != block->count)
        cssSerializeDeclarationValue(out, block, &cssDeclarations(block)[index]);
}

bool cssDeclarationBlockImportant(const CssDeclarationBlock* block, const CssDeclaredName& name) {
    if (name.property == PrCssProperty_Count || !cssIsShorthand(name.property)) {
        uint32_t index = cssDeclarationBlockFind(block, name);
        return index != block->count && cssDeclarations(block)[index].important;
    }
    CssPropertyList longhands = cssPropertyLonghands(name.property);
    for (uint32_t i = 0; i < longhands.count; ++i) {
        const CssDeclaration* declaration = findProperty(block, longhands.data[i]);
        if (declaration == nullptr || !declaration->important)
            return false;
    }
    return true;
}

CssEditResult cssDeclarationBlockPrepareSet(CssEdit* edit, const CssDeclarationBlock* block,
        const CssDeclaredName& name, PrStringView value, bool important) {
    if (important && !bitsetTest(kCssAtRuleImportant, block->rule))
        return CssEditResult_Rejected;

    CssParser* parser = edit->scope.parser;
    CssTokenStream ts = cssParserTokenize(parser, value);
    CssTokenSpan span = cssStreamTrimmed(&ts);

    // cssom-1/#dom-cssstyledeclaration-setproperty: an empty value removes the declaration.
    if (span.count == 0)
        return cssDeclarationBlockPrepareRemove(edit, block, name, nullptr);

    // The value is matched whole before the block changes, so a shorthand sets every longhand or none.
    if (!declare(parser, name, &ts, span, important))
        return CssEditResult_Rejected;
    edit->kind = CssEditKind_Set;
    return incomingChanges(block, parser) ? CssEditResult_Changed : CssEditResult_Unchanged;
}

CssEditResult cssDeclarationBlockPrepareSetValue(CssEdit* edit, const CssDeclarationBlock* block,
        const CssDeclaredName& name, CssValueRun run, bool important, bool append) {
    if (run.count == 0 || (important && !bitsetTest(kCssAtRuleImportant, block->rule)))
        return CssEditResult_Rejected;

    // An appended list holds its existing values, then the given ones.
    CssValuePool joined = {};
    if (append) {
        uint32_t index = cssDeclarationBlockFind(block, name);
        if (index < block->count) {
            const CssDeclaration* held = &cssDeclarations(block)[index];
            CssValueRun had = cssDeclarationValue(block, held);
            if (held->shorthand != PrCssProperty_Count || isTokens(had))
                return CssEditResult_Rejected;
            cssValuePoolAddRun(&joined, had);
            cssValuePoolAdd(&joined, cssLiteralValue(','));
        }
        cssValuePoolAddRun(&joined, run);
        run = cssValuePoolRun(&joined, CssValueSpan{0, joined.values.count});
    }

    CssParser* parser = edit->scope.parser;
    bool takes;
    PrTextBuffer text = {};
    if (name.text.data != nullptr || isTokens(run)) {
        // A custom property's value, or a value kept as tokens, is the tokens that it is written as
        // (css-typed-om-1/#create-an-internal-representation).
        if (isTokens(run))
            prTextBufferAppend(&text, cssValueText(run, run.values));
        else
            cssSerializeValues(&text, run);
        CssTokenStream ts = cssParserTokenize(parser, prTextBufferView(&text));
        CssTokenSpan span = cssStreamTrimmed(&ts);
        takes = (span.count != 0 || name.text.data != nullptr) && declare(parser, name, &ts, span, important);
    } else {
        // A number that its position does not accept as is, because it is out of range or not an integer, is wrapped in
        // a calculation if the value matches no other way (css-typed-om-1/#create-an-internal-representation).
        cssValueTokens(&parser->givenTokens, &parser->givenSource, run);
        CssTokenStream ts = cssStream(&parser->givenTokens);
        takes = declare(parser, name, &ts, cssStreamAll(&ts), important, true);
        if (!takes) {
            parser->match.calculate = true;
            takes = declare(parser, name, &ts, cssStreamAll(&ts), important, true);
            parser->match.calculate = false;
        }
    }
    prTextBufferReset(&text);
    cssValuePoolReset(&joined);
    if (!takes)
        return CssEditResult_Rejected;
    edit->kind = CssEditKind_Set;
    return incomingChanges(block, parser) ? CssEditResult_Changed : CssEditResult_Unchanged;
}

CssEditResult cssDeclarationBlockPrepareRemove(CssEdit* edit, const CssDeclarationBlock* block,
        const CssDeclaredName& name, PrRawString* PR_NULLABLE out) {
    if (out != nullptr) {
        PrTextBuffer text = {};
        cssSerializePropertyValue(&text, block, name);
        *out = prTextBufferTake(&text);
    }
    bool held;
    if (name.property == PrCssProperty_Count || !cssIsShorthand(name.property)) {
        held = cssDeclarationBlockFind(block, name) != block->count;
    } else {
        CssPropertyList longhands = cssPropertyLonghands(name.property);
        held = false;
        for (uint32_t i = 0; i < longhands.count && !held; ++i)
            held = findProperty(block, longhands.data[i]) != nullptr;
    }
    if (!held)
        return CssEditResult_Unchanged;
    edit->kind = CssEditKind_Remove;
    edit->name = name;
    return CssEditResult_Changed;
}

void cssDeclarationBlockCommit(CssEdit* edit, CssDeclarationBlock* block) {
    if (edit->kind == CssEditKind_Set)
        setIncoming(block, edit->scope.parser);
    else if (edit->kind == CssEditKind_Remove)
        removeDeclarations(block, edit->name);
}

CssEditResult cssDeclarationBlockSet(CssDeclarationBlock* block, const CssDeclaredName& name,
        PrStringView value, bool important, CssParserSlot slot) {
    CssEdit edit;
    cssEditBegin(&edit, slot);
    CssEditResult result = cssDeclarationBlockPrepareSet(&edit, block, name, value, important);
    if (result == CssEditResult_Changed)
        cssDeclarationBlockCommit(&edit, block);
    cssEditEnd(&edit);
    return result;
}

CssEditResult cssDeclarationBlockSetValue(CssDeclarationBlock* block, const CssDeclaredName& name, CssValueRun run,
        bool important, bool append, CssParserSlot slot) {
    CssEdit edit;
    cssEditBegin(&edit, slot);
    CssEditResult result = cssDeclarationBlockPrepareSetValue(&edit, block, name, run, important, append);
    if (result == CssEditResult_Changed)
        cssDeclarationBlockCommit(&edit, block);
    cssEditEnd(&edit);
    return result;
}

CssEditResult cssDeclarationBlockRemove(CssDeclarationBlock* block, const CssDeclaredName& name,
        PrRawString* PR_NULLABLE out) {
    CssEdit edit;
    cssEditBegin(&edit, nullptr);
    CssEditResult result = cssDeclarationBlockPrepareRemove(&edit, block, name, out);
    if (result == CssEditResult_Changed)
        cssDeclarationBlockCommit(&edit, block);
    cssEditEnd(&edit);
    return result;
}

void cssDeclarationBlockValue(const CssDeclarationBlock* block, const CssDeclaredName& name,
        CssValuePool* out, CssParserSlot slot) {
    if (name.property == PrCssProperty_Count || !cssIsShorthand(name.property)) {
        uint32_t index = cssDeclarationBlockFind(block, name);
        // A longhand pending on its shorthand's substitution has no value of its own.
        if (index < block->count && cssDeclarations(block)[index].shorthand == PrCssProperty_Count)
            cssValuePoolAddRun(out, cssDeclarationValue(block, &cssDeclarations(block)[index]));
        return;
    }

    // A shorthand's value is the serialization of its longhands. A shorthand that is pending on its substitution has
    // the tokens it was given as its value (cssom-1/#dom-cssstyledeclaration-getpropertyvalue).
    PrTextBuffer text = {};
    serializeShorthand(&text, block, name.property);
    CssPropertyList longhands = cssPropertyLonghands(name.property);
    const CssDeclaration* first = findProperty(block, longhands.data[0]);
    bool pending = first != nullptr && first->shorthand == name.property;
    if (text.size != 0) {
        CssParserScope scope;
        cssParserBegin(&scope, slot);
        CssParser* parser = scope.parser;
        CssTokenStream ts = cssParserTokenize(parser, prTextBufferView(&text));
        CssTokenStream value = cssSubStream(&ts, cssStreamTrimmed(&ts));
        uint32_t values = parser->pool.values.count;
        uint32_t size = parser->pool.text.count;
        CssValueSpan matched;
        CssTokenSpan span = {value.pos, value.end - value.pos};
        bool parsed = pending ? cssParseTokens(&value, &parser->pool, &matched) : matchValue(name, parser, &ts, span, &matched);
        if (parsed)
            cssValuePoolAddRun(out, cssValuePoolRun(&parser->pool, matched));
        parser->pool.values.count = values;
        parser->pool.text.count = size;
        cssParserEnd(&scope);
    }
    prTextBufferReset(&text);
}

bool cssParseDeclaredValue(const CssDeclaredName& name, PrStringView text, CssValuePool* out, CssParserSlot slot) {
    uint32_t mark = out->values.count;
    // A shorthand's value is spread over its longhands. A block of those longhands gathers it back.
    if (name.property != PrCssProperty_Count && cssIsShorthand(name.property)) {
        CssDeclarationBlock block = {};
        if (cssDeclarationBlockSet(&block, name, text, false, slot) == CssEditResult_Changed)
            cssDeclarationBlockValue(&block, name, out, slot);
        cssDeclarationBlockReset(&block);
        return out->values.count != mark;
    }

    CssParserScope scope;
    cssParserBegin(&scope, slot);
    CssParser* parser = scope.parser;
    CssTokenStream ts = cssParserTokenize(parser, text);
    CssTokenSpan span = cssStreamTrimmed(&ts);
    uint32_t values = parser->pool.values.count;
    uint32_t size = parser->pool.text.count;
    CssValueSpan matched;
    // css-variables-1/#defining-variables: a custom property's value may be empty, and no other property's may.
    bool parsed = (span.count != 0 || name.text.data != nullptr) && matchValue(name, parser, &ts, span, &matched);
    if (parsed)
        cssValuePoolAddRun(out, cssValuePoolRun(&parser->pool, matched));
    parser->pool.values.count = values;
    parser->pool.text.count = size;
    cssParserEnd(&scope);
    return parsed;
}

// ---------------------------------------------------------------------------------------------------------------------
// Rule lists

PrStatus cssRuleListPrepareInsert(CssEdit* edit, const CssSheetData* PR_NULLABLE sheet,
        const CssRuleChain* PR_NULLABLE parent, PrStringView text, uint32_t index) {
    const RawArray<CssRuleData* PR_NONNULL>* list = parent != nullptr ? &parent->rule->children : &sheet->rules;
    if (index > list->count)
        return PrStatus_OutOfRange;

    CssParser* parser = edit->scope.parser;
    CssTokenStream ts = cssParserTokenize(parser, text);

    // cssom-1/#parse-a-css-rule: the text must be one rule, valid at the insertion point. A rule that may not go there
    // is a HierarchyRequestError, and any other failure is a SyntaxError.
    CssSyntaxRule syntax = {};
    CssRuleData* rule = nullptr;
    PrStatus status = PrStatus_Syntax;
    if (cssParseRule(&ts, &syntax)) {
        cssSkipWhitespace(&ts);
        if (cssPeek(&ts) == CssTokenKind_Eof)
            status = buildRule(parser, &ts, syntax, sheet, parent, &rule);
    }

    // cssom-1/#insert-a-css-rule: in a nested context, text that is not a rule may be declarations.
    if (status == PrStatus_Syntax && nested(parent)) {
        rule = newRule(CssRuleDataKind_NestedDeclarations);
        parseDeclarations(parser, &rule->style, text);
        if (rule->style.count != 0) {
            status = PrStatus_Ok;
        } else {
            cssRuleDataRelease(rule);
            rule = nullptr;
        }
    }
    if (status != PrStatus_Ok)
        return status;

    // The sheet's own rules keep their order: no @import after another rule, and no @namespace after any rule other
    // than an @import. Layer statements may go anywhere. The list is already in order, so the rule fits if it fits
    // between the nearest other rules on each side.
    if (parent == nullptr) {
        Order at = orderOf(rule);
        bool fits = true;
        if (!isLayerStatement(rule)) {
            for (uint32_t i = index; i-- != 0;) {
                if (!isLayerStatement(list->data[i])) {
                    fits = orderOf(list->data[i]) <= at;
                    break;
                }
            }
            for (uint32_t i = index; i < list->count && fits; ++i) {
                if (!isLayerStatement(list->data[i])) {
                    fits = orderOf(list->data[i]) >= at;
                    break;
                }
            }
        }
        if (!fits) {
            cssRuleDataRelease(rule);
            return PrStatus_NotAllowed;
        }
        if (at == Order_Namespace) {
            for (uint32_t i = 0; i < list->count; ++i) {
                if (orderOf(list->data[i]) == Order_Rest) {
                    cssRuleDataRelease(rule);
                    return PrStatus_InvalidState;
                }
            }
        }
    }

    edit->kind = CssEditKind_Insert;
    edit->rule = rule;
    edit->index = index;
    return PrStatus_Ok;
}

void cssRuleListCommitInsert(CssEdit* edit, CssSheetData* PR_NULLABLE sheet, CssRuleData* PR_NULLABLE parent) {
    RawArray<CssRuleData* PR_NONNULL>* list = parent != nullptr ? &parent->children : &sheet->rules;
    list->insert(edit->index, edit->rule);
    edit->rule = nullptr;
}

PrStatus cssRuleListInsert(CssSheetData* PR_NULLABLE sheet, CssRuleData* PR_NULLABLE parent,
        const CssRuleChain* PR_NULLABLE outer, PrStringView text, uint32_t index, CssParserSlot slot) {
    CssRuleChain chain = {parent, outer};
    CssEdit edit;
    cssEditBegin(&edit, slot);
    PrStatus status = cssRuleListPrepareInsert(&edit, sheet, parent != nullptr ? &chain : nullptr, text, index);
    if (status == PrStatus_Ok)
        cssRuleListCommitInsert(&edit, sheet, parent);
    cssEditEnd(&edit);
    return status;
}

PrStatus cssRuleListRemovable(const CssSheetData* PR_NULLABLE sheet, const CssRuleData* PR_NULLABLE parent, uint32_t index) {
    const RawArray<CssRuleData* PR_NONNULL>& list = parent != nullptr ? parent->children : sheet->rules;
    if (index >= list.count)
        return PrStatus_OutOfRange;

    // cssom-1/#remove-a-css-rule: an @namespace rule stays while the list holds other rules.
    if (isAt(list.data[index], CssAtRule_Namespace)) {
        for (uint32_t i = 0; i < list.count; ++i) {
            if (orderOf(list.data[i]) == Order_Rest)
                return PrStatus_InvalidState;
        }
    }
    return PrStatus_Ok;
}

void cssRuleListRemove(CssSheetData* PR_NULLABLE sheet, CssRuleData* PR_NULLABLE parent, uint32_t index) {
    RawArray<CssRuleData* PR_NONNULL>* list = parent != nullptr ? &parent->children : &sheet->rules;
    CssRuleData* rule = list->data[index];
    list->erase(index);
    cssRuleDataRelease(rule);
}

void cssRuleListReplace(RawArray<CssRuleData* PR_NONNULL>* list, uint32_t index, CssRuleData* rule) {
    CssRuleData* held = list->data[index];
    list->data[index] = rule;
    cssRuleDataRelease(held);
}

// ---------------------------------------------------------------------------------------------------------------------
// Serialization

void cssSerializeDeclarationBlock(PrTextBuffer* out, const CssDeclarationBlock* block) {
    Listing listing;
    listDeclarations(block, &listing);
    PropertySet written = {};        // the longhands written as a shorthand
    bool allUnwritten = false;       // true if `all`, which sets every longhand, cannot be written
    uint32_t start = out->size;
    for (uint32_t i = 0; i < listing.count; ++i) {
        CssDeclarationItem item = itemOf(&listing, i);
        if (item.property != PrCssProperty_Count && bitsetTest(written.words, item.property))
            continue;
        if (out->size != start)
            prTextBufferAppendChar(out, ' ');
        CssPropertyList shorthands;
        if (item.property != PrCssProperty_Count)
            shorthands = cssPropertyShorthands(item.property);
        else
            shorthands = CssPropertyList{nullptr, 0};
        bool writes = false;
        for (uint32_t s = 0; s < shorthands.count && !writes; ++s) {
            PrCssProperty shorthand = shorthands.data[s];
            bool all = kCssPropertyShorthand[shorthand] == CssShorthand_All;
            if (!bitsetTest(kCssPropertyUnwritten, shorthand) && !(all && allUnwritten))
                writes = serializeAsShorthand(out, block, &listing, shorthand, i, &written);
            allUnwritten = allUnwritten || (all && !writes);
        }
        if (!writes)
            serializeDeclaration(out, block, item);
    }
    listingReset(&listing);
}

void cssSerializeDeclarationValue(PrTextBuffer* out, const CssDeclarationBlock* block,
        const CssDeclaration* declaration) {
    // css-values-5/#pending-substitution-value: a value pending on a shorthand serializes as nothing.
    if (declaration->shorthand != PrCssProperty_Count)
        return;

    CssValueRun run = cssDeclarationValue(block, declaration);
    if (declaration->descriptor != PrCssDescriptor_None && bitsetTest(kCssDescriptorBounded, declaration->descriptor))
        run.count = boundsWritten(run);

    // css-variables-1/#serializing-custom-props: an empty custom property is a single space.
    uint32_t start = out->size;
    cssSerializeValues(out, run);
    if (out->size == start && declaration->nameSize != 0)
        prTextBufferAppendChar(out, ' ');
}

void cssSerializeSelectorText(PrTextBuffer* out, const CssRuleData* rule) {
    cssSerializeSelectors(out, rule->selectors);
}

void cssSerializePrelude(PrTextBuffer* out, const CssRuleData* rule) {
    CssValueRun prelude = cssValueListRun(&rule->prelude);
    // An empty parameter/argument list is omitted (css-mixins-1/#cssom).
    if ((isAt(rule, CssAtRule_Mixin) || isAt(rule, CssAtRule_Apply)) && prelude.count == 3 &&
            cssValueIsLiteral(&prelude.values[1], '(') && cssValueIsLiteral(&prelude.values[2], ')'))
        prelude.count = 1;
    // cssom-1/#serialize-a-css-rule writes the <string> of @import and @namespace as a URL.
    bool urls = isAt(rule, CssAtRule_Import) || isAt(rule, CssAtRule_Namespace);
    cssSerializePreludeValues(out, prelude, urls, rule->selectors, 0);
    if (rule->prelude.count != 0 && rule->media.queries.count != 0)
        prTextBufferAppendChar(out, ' ');
    cssSerializeMediaList(out, &rule->media);
}

void cssSerializeMediaList(PrTextBuffer* out, const CssMediaQueries* list) {
    cssSerializeValues(out, cssValueListRun(&list->queries));
}

void cssSerializeRule(PrTextBuffer* out, const CssRuleData* rule) {
    switch (rule->kind) {
        case CssRuleDataKind_Style:
            cssSerializeSelectorText(out, rule);
            if (rule->children.count == 0) {
                serializeDeclarations(out, rule);
                return;
            }
            serializeBlock(out, rule);
            return;
        case CssRuleDataKind_NestedDeclarations:
            // css-nesting-1/#nested-declarations-rule: serialized as its block alone.
            cssSerializeDeclarationBlock(out, &rule->style);
            return;
        case CssRuleDataKind_Keyframe:
            cssSerializePrelude(out, rule);
            serializeDeclarations(out, rule);
            return;
        default:
            serializeAtRule(out, rule);
            return;
    }
}

void cssSerializeRuleList(PrTextBuffer* out, const RawArray<CssRuleData* PR_NONNULL>& rules) {
    for (uint32_t i = 0; i < rules.count; ++i) {
        if (i != 0)
            prTextBufferAppendChar(out, '\n');
        cssSerializeRule(out, rules.data[i]);
    }
}

} // namespace pr_impl
