// TeX math parsed into a tree of MathML elements that carries the TeX classes and styles which decide its spacing.

#include "tex/src/tex_parse.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/string_ops.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Tokens

// The kinds of token.
enum {
    TokenKind_End,
    TokenKind_Command,              // a control sequence, whose text is its name without the backslash
    TokenKind_Character,            // one code point
    TokenKind_Space,                // a run of white space
};
typedef uint8_t TokenKind;

// A token of the source or of a macro's expansion.
struct Token {
    uint32_t start;                 // the text, in the source or the parser's arena
    uint32_t size;
    uint32_t sourceStart;           // the source that the token stands for, which an error shows
    uint32_t sourceEnd;
    TokenKind kind;
    bool inArena;
};

// The maximum number of macro expansions in one formula, of bytes that they copy, and of tokens that they produce. The
// limits stop a macro that expands forever, and one whose arguments grow exponentially because each expansion copies
// them more than once.
constexpr uint32_t kMaxExpansions = 4096;
constexpr uint32_t kMaxArenaBytes = 1u << 20;
constexpr uint32_t kMaxExpansionTokens = 1u << 16;

// The maximum nesting of groups that the parser follows.
constexpr uint32_t kMaxDepth = 128;

// Reads the token of `text` at `*at`.
Token lexToken(PrStringView text, uint32_t* at, bool inArena) {
    for (;;) {
        Token token{};
        token.inArena = inArena;
        token.start = *at;
        token.sourceStart = *at;
        if (*at >= text.size) {
            token.kind = TokenKind_End;
            token.sourceEnd = *at;
            return token;
        }
        char c = text.data[*at];
        if (c == '%') {
            // A comment runs to the end of its line, and the spaces that start the next line are skipped too.
            while (*at < text.size && text.data[*at] != '\n')
                ++*at;
            if (*at < text.size)
                ++*at;
            while (*at < text.size && prAsciiIsTabOrSpace(text.data[*at]))
                ++*at;
            continue;
        }
        if (prAsciiIsSpace(c)) {
            while (*at < text.size && prAsciiIsSpace(text.data[*at]))
                ++*at;
            token.kind = TokenKind_Space;
            token.size = *at - token.start;
            token.sourceEnd = *at;
            return token;
        }
        if (c == '\\') {
            ++*at;
            token.kind = TokenKind_Command;
            token.start = *at;
            if (*at < text.size && prAsciiIsLetter(text.data[*at])) {
                while (*at < text.size && prAsciiIsLetter(text.data[*at]))
                    ++*at;
                token.size = *at - token.start;
                token.sourceEnd = *at;
                // A control word swallows the spaces after it.
                while (*at < text.size && prAsciiIsSpace(text.data[*at]))
                    ++*at;
                return token;
            }
            if (*at < text.size) {
                uint32_t codePoint = 0;
                *at += prUtf8Decode(text, *at, &codePoint);
            }
            token.size = *at - token.start;
            token.sourceEnd = *at;
            return token;
        }
        uint32_t codePoint = 0;
        *at += prUtf8Decode(text, *at, &codePoint);
        token.kind = TokenKind_Character;
        token.size = *at - token.start;
        token.sourceEnd = *at;
        return token;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The parser

// The tokens that end a list.
enum : uint32_t {
    StopFlag_CloseBrace = 1u << 0,
    StopFlag_CloseBracket = 1u << 1,
    StopFlag_Right = 1u << 2,
    StopFlag_Middle = 1u << 3,
    StopFlag_EndEnvironment = 1u << 4,
    StopFlag_Align = 1u << 5,
    StopFlag_Newline = 1u << 6,
    StopFlag_MathShift = 1u << 7,
    StopFlag_At = 1u << 8,          // amscd's @, which starts an arrow
    StopFlag_Character = 1u << 9,   // the parser's stopCharacter, which ends an arrow's label
};
typedef uint32_t StopFlags;

// What ended a list.
enum {
    Stop_End,
    Stop_CloseBrace,
    Stop_CloseBracket,
    Stop_Right,
    Stop_Middle,
    Stop_EndEnvironment,
    Stop_Align,
    Stop_Newline,
    Stop_MathShift,
    Stop_At,
    Stop_Character,
};
typedef uint8_t Stop;

// The state that a group scopes.
struct Context {
    TexStyle style;
    TexFont font;
    bool operatorName;              // inside \operatorname, whose letters make one name
};

// A row of a table being parsed.
struct TableRow {
    uint32_t firstCell;             // a position in the table's cells
    uint32_t cellCount;
    float gap;                      // extra space below the row, from \\[length], in em
    uint8_t lineAbove;              // 0 for none, 1 for \hline and 2 for \hdashline
    uint8_t lineBelow;
    bool tagStar;                   // \tag*, whose tag has no parentheses
    bool noNumber;                  // \notag or \nonumber, so the row takes no number
    TexNodeIndex tag;               // the tag's contents, without parentheses, or kTexNodeNone
};

// A \label in a row, which names the row's tag once the row is numbered.
struct PendingLabel {
    uint32_t row;
    PrRawString name;
};

// The rows and cells of a table being parsed, or of an equation, which is a table of one cell.
struct TableState {
    RawArray<TexNodeIndex> cells;
    RawArray<TableRow> rows;
    RawArray<PendingLabel> labels;
    TableState* PR_NULLABLE outer;  // the table or equation that this one is in
    bool tagged;                    // true for an environment that numbers equations, whose rows take \tag, \notag and
                                    // \label
};

// A \ref or \eqref, whose text is written once the formula's labels are known.
struct PendingRef {
    TexNodeIndex node;
    PrRawString name;
    bool equation;                  // \eqref, which writes the tag in parentheses
};

struct Parser {
    TexTree* tree;
    PrStringView source;
    uint32_t position;
    PrTextBuffer arena;             // the text of macro expansions
    RawArray<Token> pending;        // tokens that come before the source's next, the first one last
    TexMacroTable local;            // the formula's own definitions
    TexDocument* document;          // the shared document, or `own` for a formula that is a document of its own
    TexDocument own;
    RawArray<PendingRef> refs;
    uint32_t expansions;
    uint32_t expansionTokens;       // the tokens that the expansions produced, which kMaxExpansionTokens limits
    uint32_t depth;
    uint32_t lastEnd;               // the end of the source of the token read last, and of the one before it
    uint32_t previousEnd;
    PrTexDisplay display;
    TableState* PR_NULLABLE table;  // the innermost table being parsed
    TableState top;                 // the formula outside any table, whose one row takes a \tag
    char stopCharacter;             // the character that StopFlag_Character stops at
};

PrStringView tokenText(const Parser* p, const Token& token) {
    const char* base = token.inArena ? p->arena.data : p->source.data;
    if (base == nullptr || token.size == 0)
        return "";
    return PrStringView{base + token.start, token.size};
}

bool isCharacter(const Parser* p, const Token& token, char c) {
    return token.kind == TokenKind_Character && token.size == 1 && tokenText(p, token).data[0] == c;
}

bool isCommand(const Parser* p, const Token& token, PrStringView name) {
    return token.kind == TokenKind_Command && tokenText(p, token) == name;
}

uint32_t tokenCodePoint(const Parser* p, const Token& token) {
    uint32_t codePoint = 0;
    PrStringView text = tokenText(p, token);
    if (text.size != 0)
        prUtf8Decode(text, 0, &codePoint);
    return codePoint;
}

Token nextRaw(Parser* p) {
    Token token{};
    if (p->pending.count != 0) {
        token = p->pending.last();
        p->pending.pop();
    } else {
        token = lexToken(p->source, &p->position, false);
    }
    p->previousEnd = p->lastEnd;
    if (token.kind != TokenKind_End)
        p->lastEnd = token.sourceEnd;
    return token;
}

void pushBack(Parser* p, const Token& token) {
    p->pending.push(token);
    p->lastEnd = p->previousEnd;
}

// Reads an argument's tokens, the contents of a group or one token, without expanding them. Returns false at the end of
// the source.
bool readArgumentTokens(Parser* p, RawArray<Token>* out) {
    Token token = nextRaw(p);
    while (token.kind == TokenKind_Space)
        token = nextRaw(p);
    if (token.kind == TokenKind_End)
        return false;
    if (!isCharacter(p, token, '{')) {
        out->push(token);
        return true;
    }
    uint32_t depth = 1;
    for (;;) {
        token = nextRaw(p);
        if (token.kind == TokenKind_End)
            return true;
        if (isCharacter(p, token, '{'))
            ++depth;
        if (isCharacter(p, token, '}') && --depth == 0)
            return true;
        out->push(token);
    }
}

// Reads the tokens of an optional argument in brackets, without expanding them. Returns false if no bracket follows.
bool readOptionalTokens(Parser* p, RawArray<Token>* out) {
    Token token = nextRaw(p);
    while (token.kind == TokenKind_Space)
        token = nextRaw(p);
    if (!isCharacter(p, token, '[')) {
        if (token.kind != TokenKind_End)
            pushBack(p, token);
        return false;
    }
    uint32_t depth = 0;
    for (;;) {
        token = nextRaw(p);
        if (token.kind == TokenKind_End)
            return true;
        if (isCharacter(p, token, '{'))
            ++depth;
        if (isCharacter(p, token, '}') && depth != 0)
            --depth;
        if (isCharacter(p, token, ']') && depth == 0)
            return true;
        out->push(token);
    }
}

// Appends the text that `tokens` were written as.
void appendTokensText(const Parser* p, const RawArray<Token>& tokens, PrTextBuffer* out) {
    for (uint32_t i = 0; i < tokens.count; ++i) {
        const Token& token = tokens.data[i];
        switch (token.kind) {
            case TokenKind_Command: {
                prTextBufferAppendChar(out, '\\');
                PrStringView name = tokenText(p, token);
                prTextBufferAppend(out, name);
                // A control word keeps its separation from a letter after it.
                bool word = name.size != 0 && prAsciiIsLetter(name.data[0]);
                if (word && i + 1 < tokens.count && tokens.data[i + 1].kind == TokenKind_Character &&
                        prAsciiIsLetter(tokenText(p, tokens.data[i + 1]).data[0]))
                    prTextBufferAppendChar(out, ' ');
                break;
            }
            case TokenKind_Character:
                prTextBufferAppend(out, tokenText(p, token));
                break;
            case TokenKind_Space:
                prTextBufferAppendChar(out, ' ');
                break;
            case TokenKind_End:
                break;
        }
    }
}

// Reads an argument's text, as it is written and trimmed. Returns false at the end of the source.
bool readTextArgument(Parser* p, PrTextBuffer* out) {
    RawArray<Token> tokens{};
    bool found = readArgumentTokens(p, &tokens);
    PrTextBuffer text{};
    appendTokensText(p, tokens, &text);
    prTextBufferAppend(out, prStringTrim(prTextBufferView(&text)));
    prTextBufferReset(&text);
    tokens.reset();
    return found;
}

// Reads an optional argument's text. Returns false if no bracket follows.
bool readOptionalText(Parser* p, PrTextBuffer* out) {
    RawArray<Token> tokens{};
    bool found = readOptionalTokens(p, &tokens);
    PrTextBuffer text{};
    appendTokensText(p, tokens, &text);
    prTextBufferAppend(out, prStringTrim(prTextBufferView(&text)));
    prTextBufferReset(&text);
    tokens.reset();
    return found;
}

// Returns true if two tokens are the same character or control sequence, or are both spaces.
bool tokensMatch(const Parser* p, const Token& a, const Token& b) {
    if (a.kind != b.kind)
        return false;
    return a.kind == TokenKind_Space || tokenText(p, a) == tokenText(p, b);
}

// Returns true if the tokens at `i` are a parameter of a parameter text, a # followed by a digit.
bool isParameter(const Parser* p, const RawArray<Token>& tokens, uint32_t i) {
    return i + 1 < tokens.count && isCharacter(p, tokens.data[i], '#') && tokens.data[i + 1].kind == TokenKind_Character &&
        prAsciiIsDigit(tokenText(p, tokens.data[i + 1]).data[0]);
}

// The range of an argument in a list of tokens.
struct ArgumentRange {
    uint32_t start;
    uint32_t count;
};

// Reads the arguments of a macro whose parameter text is `pattern`, as TeX matches a \def's delimited parameters (The
// TeXbook, chapter 20). The tokens before the first parameter must come next. A parameter that a delimiter follows
// takes the tokens up to the delimiter's first occurrence outside braces, less the braces of an argument that is one
// group. A parameter that another follows directly takes one argument. Returns false, putting back what it read, if the
// tokens after the name do not match the text before the first parameter.
bool readDelimitedArguments(Parser* p, PrStringView pattern, ArgumentRange* ranges, RawArray<Token>* arguments) {
    uint32_t at = p->arena.size;
    prTextBufferAppend(&p->arena, pattern);
    PrStringView arena = prTextBufferView(&p->arena);
    RawArray<Token> tokens{};
    for (;;) {
        Token part = lexToken(arena, &at, true);
        if (part.kind == TokenKind_End)
            break;
        tokens.push(part);
    }
    uint32_t i = 0;
    RawArray<Token> prefix{};
    bool matched = true;
    while (i < tokens.count && !isParameter(p, tokens, i)) {
        Token next = nextRaw(p);
        if (next.kind != TokenKind_End)
            prefix.push(next);
        if (!tokensMatch(p, next, tokens.data[i])) {
            matched = false;
            break;
        }
        ++i;
    }
    if (!matched) {
        for (uint32_t k = prefix.count; k > 0; --k)
            pushBack(p, prefix.data[k - 1]);
        prefix.reset();
        tokens.reset();
        return false;
    }
    prefix.reset();
    uint32_t parameter = 0;
    while (i < tokens.count && parameter < 9) {
        i += 2;
        uint32_t delimiter = i;
        while (i < tokens.count && !isParameter(p, tokens, i))
            ++i;
        uint32_t delimiterCount = i - delimiter;
        ArgumentRange& range = ranges[parameter++];
        range.start = arguments->count;
        if (delimiterCount == 0) {
            readArgumentTokens(p, arguments);
            range.count = arguments->count - range.start;
            continue;
        }
        uint32_t depth = 0;
        for (;;) {
            Token token = nextRaw(p);
            if (token.kind == TokenKind_End)
                break;
            arguments->push(token);
            if (isCharacter(p, token, '{'))
                ++depth;
            else if (isCharacter(p, token, '}') && depth != 0)
                --depth;
            uint32_t count = arguments->count - range.start;
            if (depth != 0 || count < delimiterCount)
                continue;
            bool found = true;
            for (uint32_t k = 0; k < delimiterCount && found; ++k) {
                const Token& written = arguments->data[arguments->count - delimiterCount + k];
                found = tokensMatch(p, written, tokens.data[delimiter + k]);
            }
            if (found) {
                arguments->count -= delimiterCount;
                break;
            }
        }
        range.count = arguments->count - range.start;
        // An argument that is one group loses its braces.
        if (range.count >= 2 && isCharacter(p, arguments->data[range.start], '{') &&
                isCharacter(p, arguments->data[range.start + range.count - 1], '}')) {
            uint32_t level = 0;
            bool one = true;
            for (uint32_t k = 0; k + 1 < range.count && one; ++k) {
                const Token& token = arguments->data[range.start + k];
                if (isCharacter(p, token, '{'))
                    ++level;
                else if (isCharacter(p, token, '}'))
                    one = --level != 0;
            }
            if (one) {
                ++range.start;
                range.count -= 2;
            }
        }
    }
    tokens.reset();
    return true;
}

const TexMacro* PR_NULLABLE findMacro(const Parser* p, PrStringView name) {
    const TexMacro* macro = texMacroTableFind(&p->local, name);
    if (macro == nullptr)
        macro = texMacroTableFind(&p->document->macros, name);
    return macro;
}

// Expands the control sequence `token` if a macro defines it. The expansion reads the macro's arguments and puts the
// tokens of its replacement before the rest of the input. Returns false if no macro defines it.
bool expand(Parser* p, const Token& token) {
    PrStringView name = tokenText(p, token);
    PrStringView replacement{};
    uint32_t parameterCount = 0;
    PrStringView defaultArgument{};
    PrStringView parameterText{};
    bool hasDefault = false;
    const TexMacro* macro = findMacro(p, name);
    if (macro != nullptr) {
        replacement = prRawStringView(&macro->replacement);
        parameterCount = macro->parameterCount;
        hasDefault = macro->hasDefault;
        defaultArgument = prRawStringView(&macro->defaultArgument);
        parameterText = prRawStringView(&macro->parameterText);
    } else {
        const TexEntry* entry = texEntryFromName(name);
        if (entry == nullptr || entry->kind != TexEntryKind_Macro)
            return false;
        replacement = entry->text != nullptr ? PrStringView(entry->text) : PrStringView{""};
        parameterCount = uint32_t(entry->arg);
    }
    if (p->expansions >= kMaxExpansions || p->arena.size > kMaxArenaBytes || p->expansionTokens >= kMaxExpansionTokens)
        return false;
    ++p->expansions;

    // The arguments, as ranges of one list of tokens.
    ArgumentRange ranges[9] = {};
    RawArray<Token> arguments{};
    if (parameterText.size != 0 && !readDelimitedArguments(p, parameterText, ranges, &arguments)) {
        // TeX's "Use of \x doesn't match its definition".
        arguments.reset();
        return false;
    }
    for (uint32_t i = 0; i < parameterCount && i < 9 && parameterText.size == 0; ++i) {
        ranges[i].start = arguments.count;
        if (i == 0 && hasDefault) {
            if (!readOptionalTokens(p, &arguments)) {
                uint32_t at = p->arena.size;
                prTextBufferAppend(&p->arena, defaultArgument);
                PrStringView arena = prTextBufferView(&p->arena);
                for (;;) {
                    Token part = lexToken(arena, &at, true);
                    if (part.kind == TokenKind_End)
                        break;
                    part.sourceStart = token.sourceStart;
                    part.sourceEnd = token.sourceEnd;
                    arguments.push(part);
                }
            }
        } else {
            readArgumentTokens(p, &arguments);
        }
        ranges[i].count = arguments.count - ranges[i].start;
    }
    uint32_t invocationEnd = p->lastEnd;

    // The replacement's tokens, with each parameter replaced by its argument.
    uint32_t at = p->arena.size;
    prTextBufferAppend(&p->arena, replacement);
    PrStringView arena = prTextBufferView(&p->arena);
    RawArray<Token> body{};
    for (;;) {
        Token part = lexToken(arena, &at, true);
        if (part.kind == TokenKind_End)
            break;
        part.sourceStart = token.sourceStart;
        part.sourceEnd = invocationEnd;
        if (part.kind == TokenKind_Character && part.size == 1 && arena.data[part.start] == '#') {
            uint32_t mark = at;
            Token parameter = lexToken(arena, &at, true);
            if (parameter.kind == TokenKind_Character && parameter.size == 1) {
                char digit = arena.data[parameter.start];
                if (digit >= '1' && digit <= '9') {
                    uint32_t index = uint32_t(digit - '1');
                    if (index < parameterCount)
                        body.append(arguments.data + ranges[index].start, ranges[index].count);
                    if (body.count > kMaxExpansionTokens - p->expansionTokens)
                        break;
                    continue;
                }
                if (digit == '#') {
                    body.push(part);
                    continue;
                }
            }
            at = mark;
        }
        body.push(part);
    }
    arguments.reset();
    // Past the limit the expansion fails, and so does every one after it.
    if (body.count > kMaxExpansionTokens - p->expansionTokens) {
        p->expansionTokens = kMaxExpansionTokens;
        body.reset();
        return false;
    }
    p->expansionTokens += body.count;
    for (uint32_t i = body.count; i > 0; --i)
        p->pending.push(body.data[i - 1]);
    body.reset();
    return true;
}

Token next(Parser* p) {
    for (;;) {
        Token token = nextRaw(p);
        if (token.kind == TokenKind_Command && expand(p, token))
            continue;
        return token;
    }
}

Token nextNonSpace(Parser* p) {
    Token token = next(p);
    while (token.kind == TokenKind_Space)
        token = next(p);
    return token;
}

// Reads the next token if it is the character `c`, and returns true if it was.
bool acceptCharacter(Parser* p, char c) {
    Token token = nextNonSpace(p);
    if (isCharacter(p, token, c))
        return true;
    if (token.kind != TokenKind_End)
        pushBack(p, token);
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Nodes

TexNodeIndex makeNode(Parser* p, TexElement element, TexClass texClass, const Context& context) {
    return texNodeCreate(p->tree, element, texClass, context.style);
}

TexNodeIndex makeToken(Parser* p, TexElement element, PrStringView text, TexClass texClass, const Context& context) {
    TexNodeIndex node = makeNode(p, element, texClass, context);
    texNodeSetText(p->tree, node, text);
    return node;
}

TexNode& at(Parser* p, TexNodeIndex node) {
    return p->tree->nodes.data[node];
}

TexNodeIndex makeRow(Parser* p, const Context& context) {
    return makeNode(p, TexElement_Mrow, TexClass_Ord, context);
}

bool isBoldFont(TexFont font) {
    switch (font) {
        case TexFont_Bold:
        case TexFont_BoldItalic:
        case TexFont_BoldScript:
        case TexFont_BoldCalligraphic:
        case TexFont_BoldFraktur:
        case TexFont_SansSerifBold:
        case TexFont_SansSerifBoldItalic:
            return true;
        default:
            return false;
    }
}

// Makes an operator that TeX does not stretch, as a symbol written by itself is.
TexNodeIndex makeOperator(Parser* p, PrStringView text, TexClass texClass, const Context& context) {
    TexNodeIndex node = makeToken(p, TexElement_Mo, text, texClass, context);
    at(p, node).stretchy = TexTri_False;
    // Unicode has no bold operators, so a bold font sets them in a bold weight.
    if (isBoldFont(context.font))
        texNodeSetAttribute(p->tree, node, "style", "font-weight: bold");
    return node;
}

// Returns a node with the source from `start` to `end` in an <merror>.
TexNodeIndex makeError(Parser* p, uint32_t start, uint32_t end, const Context& context) {
    TexNodeIndex error = makeNode(p, TexElement_Merror, TexClass_Ord, context);
    if (end > p->source.size)
        end = uint32_t(p->source.size);
    if (start > end)
        start = end;
    TexNodeIndex text = makeToken(p, TexElement_Mtext, prStringSlice(p->source, start, end - start), TexClass_Ord, context);
    texNodeAppend(p->tree, error, text);
    return error;
}

// Returns the row's only child, detached from it, or the row if it has another number of children.
TexNodeIndex collapse(Parser* p, TexNodeIndex row) {
    TexNode& node = at(p, row);
    if (node.firstChild == kTexNodeNone || node.firstChild != node.lastChild)
        return row;
    TexNodeIndex child = node.firstChild;
    node.firstChild = kTexNodeNone;
    node.lastChild = kTexNodeNone;
    return child;
}

// Returns a group as the Ord atom that braces make it.
TexNodeIndex groupAtom(Parser* p, TexNodeIndex row) {
    TexNodeIndex atom = collapse(p, row);
    TexNode& node = at(p, atom);
    node.texClass = TexClass_Ord;
    node.flags &= TexNodeFlags(~(TexNodeFlag_Function | TexNodeFlag_DisplayLimits | TexNodeFlag_Limits |
        TexNodeFlag_Transparent));
    return atom;
}

// Appends `child` to `parent`, or its children if it is an <mrow>, as a row element takes the contents of its argument.
void appendContents(Parser* p, TexNodeIndex parent, TexNodeIndex child) {
    if (at(p, child).element == TexElement_Mrow && !(at(p, child).flags & TexNodeFlag_Transparent))
        texNodeMoveChildren(p->tree, child, parent);
    else
        texNodeAppend(p->tree, parent, child);
}

TexNodeIndex wrap(Parser* p, TexElement element, TexNodeIndex child, TexClass texClass, const Context& context) {
    TexNodeIndex node = makeNode(p, element, texClass, context);
    texNodeAppend(p->tree, node, child);
    return node;
}

// Returns the style of the parts of a fraction set in `style`.
TexStyle fractionStyle(TexStyle style) {
    switch (style) {
        case TexStyle_Display:
            return TexStyle_Text;
        case TexStyle_Text:
            return TexStyle_Script;
        default:
            return TexStyle_ScriptScript;
    }
}

TexStyle scriptStyle(TexStyle style) {
    return style <= TexStyle_Text ? TexStyle_Script : TexStyle_ScriptScript;
}

// Returns the MathML script level of a style. TeX's display and text styles are level 0.
uint32_t styleLevel(TexStyle style) {
    return style <= TexStyle_Text ? 0 : uint32_t(style) - 1;
}

// Sets `node` in `style` with an <mstyle> if `style` differs from the context's.
TexNodeIndex inStyle(Parser* p, TexNodeIndex node, TexStyle style, const Context& context) {
    if (style == context.style)
        return node;
    TexNodeIndex mstyle = wrap(p, TexElement_Mstyle, node, at(p, node).texClass, context);
    texNodeSetAttribute(p->tree, mstyle, "displaystyle", style == TexStyle_Display ? "true" : "false");
    static const char* const kLevels[] = {"0", "1", "2"};
    texNodeSetAttribute(p->tree, mstyle, "scriptlevel", kLevels[styleLevel(style)]);
    return mstyle;
}

// Moves a subtree that was parsed in style `from` into style `to`, as \over moves the list before it into a numerator.
void restyle(Parser* p, TexNodeIndex node, TexStyle from, TexStyle to) {
    TexNode& n = at(p, node);
    int level = int(styleLevel(n.style)) + int(styleLevel(to)) - int(styleLevel(from));
    if (level < 0)
        level = 0;
    if (level > 2)
        level = 2;
    if (level == 0)
        n.style = n.style == TexStyle_Display && to == TexStyle_Display ? TexStyle_Display : TexStyle_Text;
    else
        n.style = TexStyle(level + 1);
    for (TexNodeIndex child = n.firstChild; child != kTexNodeNone; child = at(p, child).next)
        restyle(p, child, from, to);
}

// ---------------------------------------------------------------------------------------------------------------------
// Lengths

// TeX's largest dimension, \maxdimen, which is 16383.99998pt, in em at a 10pt font. TeX reports a larger one as an
// error and uses \maxdimen in its place.
constexpr float kMaxDimensionEm = 1638.4f;

// Returns `em` within TeX's largest dimension either way, or 0 for NaN.
float clampDimension(float em) {
    if (em != em)
        return 0;
    return em > kMaxDimensionEm ? kMaxDimensionEm : em < -kMaxDimensionEm ? -kMaxDimensionEm : em;
}

// Formats `value`, within TeX's largest dimension, with at most four decimals and no trailing zeros, followed by
// `unit`.
void appendLength(PrTextBuffer* out, float value, PrStringView unit) {
    value = clampDimension(value);
    int64_t scaled = int64_t(double(value) * 10000.0 + (value < 0 ? -0.5 : 0.5));
    if (scaled < 0) {
        prTextBufferAppendChar(out, '-');
        scaled = -scaled;
    }
    prTextBufferAppendU64(out, uint64_t(scaled / 10000), 10);
    int64_t fraction = scaled % 10000;
    if (fraction != 0) {
        prTextBufferAppendChar(out, '.');
        char digits[4];
        for (int i = 3; i >= 0; --i) {
            digits[i] = char('0' + fraction % 10);
            fraction /= 10;
        }
        uint32_t count = 4;
        while (count > 0 && digits[count - 1] == '0')
            --count;
        prTextBufferAppend(out, PrStringView{digits, count});
    }
    prTextBufferAppend(out, unit);
}

// Parses a TeX dimension, as `1.5em` or `-3mu`, into em. TeX's units of absolute length are converted at a 10pt font,
// whose quad is 10pt and whose x-height is 4.31pt, as Computer Modern's are, and kept within TeX's largest dimension.
// Returns false if the text is not one.
bool parseDimension(PrStringView text, float* outEm) {
    text = prStringTrim(text);
    size_t i = 0;
    float sign = 1;
    while (i < text.size && (text.data[i] == '-' || text.data[i] == '+')) {
        if (text.data[i] == '-')
            sign = -sign;
        ++i;
    }
    float value = 0;
    bool digits = false;
    while (i < text.size && prAsciiIsDigit(text.data[i])) {
        value = value * 10 + float(text.data[i] - '0');
        digits = true;
        ++i;
    }
    if (i < text.size && (text.data[i] == '.' || text.data[i] == ',')) {
        ++i;
        float scale = 0.1f;
        while (i < text.size && prAsciiIsDigit(text.data[i])) {
            value += scale * float(text.data[i] - '0');
            scale /= 10;
            digits = true;
            ++i;
        }
    }
    if (!digits)
        value = 1;
    while (i < text.size && prAsciiIsSpace(text.data[i]))
        ++i;
    PrStringView unit = prStringTrim(prStringSlice(text, i, text.size - i));
    if (prStringStartsWith(unit, "true"))
        unit = prStringTrim(prStringSlice(unit, 4, unit.size - 4));
    struct Unit {
        const char* name;
        float em;
    };
    static constexpr Unit kUnits[] = {
        {"em", 1.0f}, {"ex", 0.431f}, {"mu", 1.0f / 18}, {"pt", 0.1f}, {"pc", 1.2f}, {"in", 7.227f},
        {"cm", 2.84528f}, {"mm", 0.284528f}, {"bp", 0.100375f}, {"dd", 0.107001f}, {"cc", 1.28401f},
        {"sp", 0.1f / 65536}, {"px", 0.100375f},
    };
    for (const Unit& candidate : kUnits) {
        if (unit == PrStringView(candidate.name)) {
            *outEm = clampDimension(sign * value * candidate.em);
            return true;
        }
    }
    return false;
}

// Makes an <mspace> `em` wide, which may be negative.
TexNodeIndex makeSpace(Parser* p, float em, const Context& context) {
    // A negative space pulls what follows back, as MathML 4's negative named spaces do (mathml4/#type_namedspace).
    TexNodeIndex space = makeNode(p, TexElement_Mspace, TexClass_None, context);
    PrTextBuffer value{};
    appendLength(&value, em, "em");
    texNodeSetAttribute(p->tree, space, "width", prTextBufferView(&value));
    prTextBufferReset(&value);
    return space;
}

TexNodeIndex makeMuSpace(Parser* p, int mu, const Context& context) {
    return makeSpace(p, float(mu) / 18, context);
}

// Makes a strut, an empty box that gives a row its height and depth.
TexNodeIndex makeStrut(Parser* p, float height, float depth, const Context& context) {
    TexNodeIndex strut = makeNode(p, TexElement_Mspace, TexClass_None, context);
    PrTextBuffer value{};
    appendLength(&value, height, "em");
    texNodeSetAttribute(p->tree, strut, "height", prTextBufferView(&value));
    prTextBufferClear(&value);
    appendLength(&value, depth, "em");
    texNodeSetAttribute(p->tree, strut, "depth", prTextBufferView(&value));
    prTextBufferReset(&value);
    return strut;
}

// Reads a dimension written after a primitive such as \kern, without braces: `1.5em`, `-3mu`. A group holds one too.
bool readDimension(Parser* p, float* outEm) {
    Token token = nextNonSpace(p);
    if (isCharacter(p, token, '{')) {
        pushBack(p, token);
        PrTextBuffer text{};
        readTextArgument(p, &text);
        bool parsed = parseDimension(prTextBufferView(&text), outEm);
        prTextBufferReset(&text);
        return parsed;
    }
    PrTextBuffer text{};
    // The number, then the unit, which is two letters.
    while (token.kind == TokenKind_Character) {
        char c = tokenText(p, token).data[0];
        if (!(prAsciiIsDigit(c) || c == '.' || c == ',' || c == '-' || c == '+'))
            break;
        prTextBufferAppendChar(&text, c);
        token = next(p);
    }
    while (token.kind == TokenKind_Space)
        token = next(p);
    for (int i = 0; i < 2 && token.kind == TokenKind_Character && prAsciiIsLetter(tokenText(p, token).data[0]); ++i) {
        prTextBufferAppendChar(&text, tokenText(p, token).data[0]);
        token = next(p);
    }
    if (token.kind != TokenKind_End)
        pushBack(p, token);
    bool parsed = parseDimension(prTextBufferView(&text), outEm);
    prTextBufferReset(&text);
    return parsed;
}

// ---------------------------------------------------------------------------------------------------------------------
// Colors

// Writes the CSS color of `spec` in the xcolor `model`, or returns false if it is not one. A model that is empty takes
// a color name or a #hex value.
bool parseColor(PrStringView model, PrStringView spec, PrTextBuffer* out) {
    spec = prStringTrim(spec);
    if (spec.size == 0)
        return false;
    auto isHex = [](PrStringView text) {
        for (size_t i = 0; i < text.size; ++i) {
            if (!prAsciiIsHexDigit(text.data[i]))
                return false;
        }
        return text.size == 3 || text.size == 6 || text.size == 8;
    };
    auto appendByte = [&](uint32_t value) {
        static const char kHex[] = "0123456789abcdef";
        prTextBufferAppendChar(out, kHex[(value >> 4) & 0xf]);
        prTextBufferAppendChar(out, kHex[value & 0xf]);
    };
    // The numbers of a model's components.
    auto components = [](PrStringView text, float* values, uint32_t capacity) {
        uint32_t count = 0;
        size_t i = 0;
        while (i < text.size && count < capacity) {
            while (i < text.size && (prAsciiIsSpace(text.data[i]) || text.data[i] == ','))
                ++i;
            if (i >= text.size)
                break;
            float value = 0;
            bool digits = false;
            while (i < text.size && prAsciiIsDigit(text.data[i])) {
                value = value * 10 + float(text.data[i] - '0');
                digits = true;
                ++i;
            }
            if (i < text.size && text.data[i] == '.') {
                ++i;
                float scale = 0.1f;
                while (i < text.size && prAsciiIsDigit(text.data[i])) {
                    value += scale * float(text.data[i] - '0');
                    scale /= 10;
                    digits = true;
                    ++i;
                }
            }
            if (!digits)
                return uint32_t(0);
            values[count++] = value;
        }
        return count;
    };
    if (model.size == 0) {
        if (spec.data[0] == '#' && isHex(prStringSlice(spec, 1, spec.size - 1))) {
            prTextBufferAppend(out, spec);
            return true;
        }
        for (size_t i = 0; i < spec.size; ++i) {
            if (!prAsciiIsLetter(spec.data[i]))
                return false;
        }
        prTextBufferAppend(out, spec);
        return true;
    }
    if (model == "HTML") {
        if (!isHex(spec) || spec.size != 6)
            return false;
        prTextBufferAppendChar(out, '#');
        prTextBufferAppend(out, spec);
        return true;
    }
    float values[3];
    if (model == "RGB" || model == "rgb") {
        if (components(spec, values, 3) != 3)
            return false;
        prTextBufferAppendChar(out, '#');
        for (float value : values) {
            float scaled = model == "rgb" ? value * 255 : value;
            appendByte(uint32_t(scaled < 0 ? 0 : scaled > 255 ? 255 : scaled + 0.5f));
        }
        return true;
    }
    if (model == "gray") {
        if (components(spec, values, 1) != 1)
            return false;
        float scaled = values[0] * 255;
        uint32_t level = uint32_t(scaled < 0 ? 0 : scaled > 255 ? 255 : scaled + 0.5f);
        prTextBufferAppendChar(out, '#');
        for (int i = 0; i < 3; ++i)
            appendByte(level);
        return true;
    }
    return false;
}

// Reads a color: an optional model in brackets, then the color in braces. Returns false if it is not one.
bool readColor(Parser* p, PrTextBuffer* out) {
    PrTextBuffer model{};
    readOptionalText(p, &model);
    PrTextBuffer spec{};
    readTextArgument(p, &spec);
    bool parsed = parseColor(prTextBufferView(&model), prTextBufferView(&spec), out);
    prTextBufferReset(&model);
    prTextBufferReset(&spec);
    return parsed;
}

// ---------------------------------------------------------------------------------------------------------------------
// Atoms

Stop parseList(Parser* p, Context context, StopFlags stops, TexNodeIndex target);
TexNodeIndex parseAtom(Parser* p, const Context& context, const Token& token, bool single);
TexNodeIndex parseScripts(Parser* p, const Context& context, TexNodeIndex base);
TexNodeIndex parseTextArgument(Parser* p, const Context& context, TexTextFont font);
TexNodeIndex makePrimes(Parser* p, uint32_t count, const Context& context);

// Parses an environment after \begin, or the rows of \substack's group after its opening brace.
TexNodeIndex parseEnvironment(Parser* p, const Context& context, uint32_t sourceStart, bool substack);

// Returns true if `token` ends a list that stops at `stops`, and which stop it is.
bool isStop(const Parser* p, const Token& token, StopFlags stops, Stop* out) {
    if (token.kind == TokenKind_Character && token.size == 1) {
        char c = tokenText(p, token).data[0];
        if (c == '}' && (stops & StopFlag_CloseBrace)) {
            *out = Stop_CloseBrace;
            return true;
        }
        if (c == ']' && (stops & StopFlag_CloseBracket)) {
            *out = Stop_CloseBracket;
            return true;
        }
        if (c == '&' && (stops & StopFlag_Align)) {
            *out = Stop_Align;
            return true;
        }
        if (c == '$' && (stops & StopFlag_MathShift)) {
            *out = Stop_MathShift;
            return true;
        }
        if (c == '@' && (stops & StopFlag_At)) {
            *out = Stop_At;
            return true;
        }
        if (c == p->stopCharacter && (stops & StopFlag_Character)) {
            *out = Stop_Character;
            return true;
        }
        return false;
    }
    if (token.kind != TokenKind_Command)
        return false;
    PrStringView name = tokenText(p, token);
    if (name == "right" && (stops & StopFlag_Right)) {
        *out = Stop_Right;
        return true;
    }
    if (name == "middle" && (stops & StopFlag_Middle)) {
        *out = Stop_Middle;
        return true;
    }
    if (name == "end" && (stops & StopFlag_EndEnvironment)) {
        *out = Stop_EndEnvironment;
        return true;
    }
    if ((name == "\\" || name == "cr" || name == "newline") && (stops & StopFlag_Newline)) {
        *out = Stop_Newline;
        return true;
    }
    if (name == ")" && (stops & StopFlag_MathShift)) {
        *out = Stop_MathShift;
        return true;
    }
    return false;
}

// Parses a required argument: a group, or one atom. Returns kTexNodeNone at the end of the source.
TexNodeIndex parseArgument(Parser* p, const Context& context) {
    Token token = nextNonSpace(p);
    if (token.kind == TokenKind_End)
        return kTexNodeNone;
    if (isCharacter(p, token, '{')) {
        TexNodeIndex row = makeRow(p, context);
        parseList(p, context, StopFlag_CloseBrace, row);
        return collapse(p, row);
    }
    TexNodeIndex atom = parseAtom(p, context, token, true);
    if (atom == kTexNodeNone)
        return makeRow(p, context);
    return atom;
}

// Parses an optional argument in brackets, or returns kTexNodeNone if no bracket follows.
TexNodeIndex parseOptionalArgument(Parser* p, const Context& context) {
    if (!acceptCharacter(p, '['))
        return kTexNodeNone;
    TexNodeIndex row = makeRow(p, context);
    parseList(p, context, StopFlag_CloseBracket, row);
    return collapse(p, row);
}

// Returns the argument, or an <merror> of the source from `start` if it is missing.
TexNodeIndex requireArgument(Parser* p, TexNodeIndex argument, uint32_t start, const Context& context) {
    if (argument != kTexNodeNone)
        return argument;
    return makeError(p, start, p->lastEnd, context);
}

// Makes the <mi> of a letter, or of a run of letters in one font.
TexNodeIndex makeLetters(Parser* p, const uint32_t* codePoints, uint32_t count, const Context& context) {
    PrTextBuffer text{};
    TexFont font = context.font;
    bool single = count == 1;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t codePoint = codePoints[i];
        // A single italic letter is italic already, by math-auto.
        bool autoItalic = single && texIsAutoItalic(codePoint) &&
            (font == TexFont_None || font == TexFont_Normal || font == TexFont_Italic);
        if (!autoItalic)
            codePoint = texFontMap(codePoint, font);
        prTextBufferAppendCodePoint(&text, codePoint);
        uint32_t selector = texFontVariation(codePoint, font);
        if (selector != 0)
            prTextBufferAppendCodePoint(&text, selector);
    }
    TexNodeIndex node = makeToken(p, TexElement_Mi, prTextBufferView(&text), TexClass_Ord, context);
    if (single && font == TexFont_Roman && texIsAutoItalic(codePoints[0]))
        texNodeSetAttribute(p->tree, node, "mathvariant", "normal");
    prTextBufferReset(&text);
    return node;
}

// Returns true if a font sets runs of letters as one name, as \mathrm{max} is.
bool fontJoinsLetters(TexFont font) {
    return font != TexFont_None && font != TexFont_Normal;
}

bool isLetterCodePoint(uint32_t codePoint) {
    if (codePoint < 0x80)
        return prAsciiIsLetter(char(codePoint));
    const TexEntry* entry = texEntryFromCodePoint(codePoint);
    return entry == nullptr || entry->kind == TexEntryKind_Letter || entry->kind == TexEntryKind_Upright;
}

bool isScriptToken(const Parser* p, const Token& token) {
    return isCharacter(p, token, '^') || isCharacter(p, token, '_') || isCharacter(p, token, '\'');
}

// Parses a letter and, in a font that joins letters, the run of letters after it. A last letter that takes scripts
// stays apart from the run, because scripts attach to one letter.
TexNodeIndex parseLetters(Parser* p, const Context& context, uint32_t first, bool single) {
    RawArray<uint32_t> run{};
    run.push(first);
    while (!single && fontJoinsLetters(context.font)) {
        Token token = next(p);
        uint32_t codePoint = token.kind == TokenKind_Character ? tokenCodePoint(p, token) : 0;
        if (codePoint == 0 || !isLetterCodePoint(codePoint)) {
            if (token.kind != TokenKind_End)
                pushBack(p, token);
            break;
        }
        Token after = next(p);
        bool scripted = isScriptToken(p, after);
        if (after.kind != TokenKind_End)
            p->pending.push(after);
        if (scripted) {
            p->pending.push(token);
            break;
        }
        run.push(codePoint);
    }
    TexNodeIndex node = makeLetters(p, run.data, run.count, context);
    run.reset();
    return node;
}

// Parses a number: digits with at most one decimal point between them.
TexNodeIndex parseNumber(Parser* p, const Context& context, const Token& first, bool single) {
    PrTextBuffer text{};
    auto appendDigit = [&](char c) {
        uint32_t codePoint = uint32_t(uint8_t(c));
        if (prAsciiIsDigit(c))
            codePoint = texFontMap(codePoint, context.font);
        prTextBufferAppendCodePoint(&text, codePoint);
    };
    appendDigit(tokenText(p, first).data[0]);
    bool point = tokenText(p, first).data[0] == '.';
    if (point) {
        Token digit = next(p);
        appendDigit(tokenText(p, digit).data[0]);
    }
    while (!single) {
        Token token = next(p);
        if (token.kind == TokenKind_Character && token.size == 1 && prAsciiIsDigit(tokenText(p, token).data[0])) {
            appendDigit(tokenText(p, token).data[0]);
            continue;
        }
        if (!point && isCharacter(p, token, '.')) {
            Token after = next(p);
            bool digit = after.kind == TokenKind_Character && after.size == 1 &&
                prAsciiIsDigit(tokenText(p, after).data[0]);
            if (digit) {
                point = true;
                appendDigit('.');
                appendDigit(tokenText(p, after).data[0]);
                continue;
            }
            if (after.kind != TokenKind_End)
                p->pending.push(after);
        }
        if (token.kind != TokenKind_End)
            pushBack(p, token);
        break;
    }
    TexNodeIndex node = makeToken(p, TexElement_Mn, prTextBufferView(&text), TexClass_Ord, context);
    prTextBufferReset(&text);
    return node;
}

// Makes the node of a symbol entry.
TexNodeIndex makeSymbol(Parser* p, const TexEntry& entry, const Context& context) {
    PrStringView text = entry.text != nullptr ? PrStringView(entry.text) : PrStringView{""};
    switch (entry.kind) {
        case TexEntryKind_Letter:
        case TexEntryKind_Upright: {
            uint32_t codePoint = 0;
            uint32_t size = text.size != 0 ? prUtf8Decode(text, 0, &codePoint) : 0;
            if (size != text.size || size == 0)
                return makeToken(p, TexElement_Mi, text, TexClass_Ord, context);
            TexFont font = context.font;
            // Uppercase Greek is upright in math italic, and bold upright in bold italic, as amsmath sets it.
            if (entry.kind == TexEntryKind_Upright) {
                if (font == TexFont_None)
                    font = TexFont_Roman;
                else if (font == TexFont_BoldItalic)
                    font = TexFont_Bold;
            }
            Context letterContext = context;
            letterContext.font = font;
            return makeLetters(p, &codePoint, 1, letterContext);
        }
        case TexEntryKind_Operator:
            return makeOperator(p, text, entry.texClass, context);
        case TexEntryKind_LargeOperator:
        case TexEntryKind_Integral: {
            TexNodeIndex node = makeOperator(p, text, TexClass_Op, context);
            TexNode& n = at(p, node);
            n.stretchy = TexTri_Any;
            n.largeop = PrStringView(entry.name) == "smallint" ? TexTri_False : TexTri_True;
            if (entry.kind == TexEntryKind_LargeOperator) {
                n.flags |= TexNodeFlag_DisplayLimits;
                n.movablelimits = TexTri_True;
            }
            return node;
        }
        case TexEntryKind_Function: {
            TexNodeIndex node = makeToken(p, TexElement_Mi, text, TexClass_Op, context);
            at(p, node).flags |= TexNodeFlag_Function;
            return node;
        }
        case TexEntryKind_LimitFunction: {
            TexNodeIndex node = makeToken(p, TexElement_Mo, text, TexClass_Op, context);
            TexNode& n = at(p, node);
            n.flags |= TexNodeFlag_DisplayLimits;
            n.movablelimits = TexTri_True;
            return node;
        }
        case TexEntryKind_TextSymbol:
            return makeToken(p, TexElement_Mi, text, TexClass_Ord, context);
        default:
            return kTexNodeNone;
    }
}

// Makes the atom of a character written as itself, by TeX's math codes, or by the entry whose symbol it is.
TexNodeIndex makeCharacterAtom(Parser* p, uint32_t codePoint, const Context& context) {
    char utf8[kPrUtf8MaxBytes];
    PrStringView text{utf8, prUtf8Encode(codePoint, utf8)};
    switch (codePoint) {
        case '+':
            return makeOperator(p, "+", TexClass_Bin, context);
        case '-':
            return makeOperator(p, "−", TexClass_Bin, context);
        case '*':
            return makeOperator(p, "∗", TexClass_Bin, context);
        case '=':
        case '<':
        case '>':
        case ':':
            return makeOperator(p, text, TexClass_Rel, context);
        case '(':
        case '[':
            return makeOperator(p, text, TexClass_Open, context);
        case ')':
        case ']':
        case '!':
        case '?':
            return makeOperator(p, text, TexClass_Close, context);
        case ',':
        case ';':
            return makeOperator(p, text, TexClass_Punct, context);
        case '/':
        case '.':
        case '|':
            return makeOperator(p, text, TexClass_Ord, context);
        case '@':
        case '"':
        case '`':
            return makeToken(p, TexElement_Mi, text, TexClass_Ord, context);
        default:
            break;
    }
    if (codePoint < 0x80 && prAsciiIsLetter(char(codePoint)))
        return makeLetters(p, &codePoint, 1, context);
    if (codePoint < 0x80 && prAsciiIsDigit(char(codePoint)))
        return makeToken(p, TexElement_Mn, text, TexClass_Ord, context);
    if (codePoint == 0x2212)
        return makeOperator(p, text, TexClass_Bin, context);
    const TexEntry* entry = texEntryFromCodePoint(codePoint);
    if (entry != nullptr)
        return makeSymbol(p, *entry, context);
    return makeLetters(p, &codePoint, 1, context);
}

// Reads a delimiter, as \left and \big take one, into `out`, which is empty for the null delimiter `.`. Returns false
// if the next token is not a delimiter.
bool readDelimiter(Parser* p, PrTextBuffer* out) {
    Token token = nextNonSpace(p);
    if (token.kind == TokenKind_Character) {
        uint32_t codePoint = tokenCodePoint(p, token);
        switch (codePoint) {
            case '.':
                return true;
            case '<':
                prTextBufferAppend(out, "⟨");
                return true;
            case '>':
                prTextBufferAppend(out, "⟩");
                return true;
            case '(':
            case ')':
            case '[':
            case ']':
            case '|':
            case '/':
                prTextBufferAppend(out, tokenText(p, token));
                return true;
            default: {
                const TexEntry* entry = texEntryFromCodePoint(codePoint);
                if (entry != nullptr && entry->kind == TexEntryKind_Operator) {
                    prTextBufferAppend(out, tokenText(p, token));
                    return true;
                }
                break;
            }
        }
    } else if (token.kind == TokenKind_Command) {
        const TexEntry* entry = texEntryFromName(tokenText(p, token));
        if (entry != nullptr && entry->kind == TexEntryKind_Operator && entry->text != nullptr) {
            prTextBufferAppend(out, entry->text);
            return true;
        }
    }
    if (token.kind != TokenKind_End)
        pushBack(p, token);
    return false;
}

// Makes a delimiter that stretches: one of \left, \middle and \right, or of \big's family at a fixed size.
TexNodeIndex makeFence(Parser* p, PrStringView text, TexClass texClass, const Context& context) {
    TexNodeIndex fence = makeToken(p, TexElement_Mo, text, texClass, context);
    TexNode& node = at(p, fence);
    node.stretchy = TexTri_True;
    node.symmetric = TexTri_True;
    return fence;
}

// Makes the null delimiter that \left. and \right. stand for, a space of TeX's \nulldelimiterspace, 1.2pt.
TexNodeIndex makeNullDelimiter(Parser* p, const Context& context) {
    return makeSpace(p, 0.12f, context);
}

// Sets a fence at a fixed size in em.
void setFenceSize(Parser* p, TexNodeIndex fence, PrStringView size) {
    texNodeSetAttribute(p->tree, fence, "minsize", size);
    texNodeSetAttribute(p->tree, fence, "maxsize", size);
}

// Parses \left, the list after it and its \middle delimiters, to the \right that ends it.
TexNodeIndex parseLeftRight(Parser* p, const Context& context, uint32_t start) {
    TexNodeIndex row = makeNode(p, TexElement_Mrow, TexClass_Inner, context);
    PrTextBuffer delimiter{};
    if (!readDelimiter(p, &delimiter)) {
        prTextBufferReset(&delimiter);
        return makeError(p, start, p->lastEnd, context);
    }
    if (delimiter.size != 0)
        texNodeAppend(p->tree, row, makeFence(p, prTextBufferView(&delimiter), TexClass_Open, context));
    else
        texNodeAppend(p->tree, row, makeNullDelimiter(p, context));
    for (;;) {
        Stop stop = parseList(p, context, StopFlag_Right | StopFlag_Middle, row);
        prTextBufferClear(&delimiter);
        uint32_t delimiterStart = p->lastEnd;
        if (stop == Stop_Middle) {
            if (!readDelimiter(p, &delimiter)) {
                texNodeAppend(p->tree, row, makeError(p, delimiterStart, p->lastEnd, context));
                continue;
            }
            if (delimiter.size != 0)
                texNodeAppend(p->tree, row, makeFence(p, prTextBufferView(&delimiter), TexClass_Middle, context));
            continue;
        }
        if (stop == Stop_Right) {
            if (!readDelimiter(p, &delimiter))
                texNodeAppend(p->tree, row, makeError(p, delimiterStart, p->lastEnd, context));
            else if (delimiter.size != 0)
                texNodeAppend(p->tree, row, makeFence(p, prTextBufferView(&delimiter), TexClass_Close, context));
            else
                texNodeAppend(p->tree, row, makeNullDelimiter(p, context));
        }
        break;
    }
    prTextBufferReset(&delimiter);
    return row;
}

// Makes a generalized fraction with its rule's thickness if it is not the default, its delimiters if it has any, and
// its style.
TexNodeIndex makeFraction(Parser* p, TexNodeIndex numerator, TexNodeIndex denominator, PrStringView thickness,
        PrStringView left, PrStringView right, TexStyle style, const Context& context) {
    Context fractionContext = context;
    fractionContext.style = style;
    TexNodeIndex fraction = makeNode(p, TexElement_Mfrac, TexClass_Inner, fractionContext);
    if (thickness.data != nullptr)
        texNodeSetAttribute(p->tree, fraction, "linethickness", thickness);
    texNodeAppend(p->tree, fraction, numerator);
    texNodeAppend(p->tree, fraction, denominator);
    TexNodeIndex result = fraction;
    if (left.size != 0 || right.size != 0) {
        // TeX sizes a fraction's delimiters by style, with \delim1 in display style and \delim2 otherwise. They select
        // the 2.4em and 1.2em variants of Computer Modern's delimiters.
        PrStringView size = style == TexStyle_Display ? "2.4em" : "1.2em";
        result = makeNode(p, TexElement_Mrow, TexClass_Inner, fractionContext);
        if (left.size != 0) {
            TexNodeIndex fence = makeFence(p, left, TexClass_Open, fractionContext);
            setFenceSize(p, fence, size);
            texNodeAppend(p->tree, result, fence);
        }
        texNodeAppend(p->tree, result, fraction);
        if (right.size != 0) {
            TexNodeIndex fence = makeFence(p, right, TexClass_Close, fractionContext);
            setFenceSize(p, fence, size);
            texNodeAppend(p->tree, result, fence);
        }
    }
    return inStyle(p, result, style, context);
}

// Parses the two arguments of a fraction in `style`, or returns an error if they are missing.
TexNodeIndex parseFraction(Parser* p, const Context& context, uint32_t start, TexStyle style, PrStringView thickness,
        PrStringView left, PrStringView right) {
    Context parts = context;
    parts.style = fractionStyle(style);
    TexNodeIndex numerator = parseArgument(p, parts);
    TexNodeIndex denominator = numerator != kTexNodeNone ? parseArgument(p, parts) : kTexNodeNone;
    if (denominator == kTexNodeNone)
        return makeError(p, start, p->lastEnd, context);
    return makeFraction(p, numerator, denominator, thickness, left, right, style, context);
}

// Returns the text of a delimiter written as an argument of \genfrac, which may be empty.
void delimiterText(PrStringView written, PrTextBuffer* out) {
    written = prStringTrim(written);
    if (written.size == 0 || written == ".")
        return;
    if (written == "<") {
        prTextBufferAppend(out, "⟨");
        return;
    }
    if (written == ">") {
        prTextBufferAppend(out, "⟩");
        return;
    }
    if (written.data[0] == '\\') {
        const TexEntry* entry = texEntryFromName(prStringSlice(written, 1, written.size - 1));
        if (entry != nullptr && entry->text != nullptr)
            prTextBufferAppend(out, entry->text);
        return;
    }
    prTextBufferAppend(out, written);
}

TexNodeIndex parseGenFrac(Parser* p, const Context& context, uint32_t start) {
    PrTextBuffer leftWritten{};
    PrTextBuffer rightWritten{};
    PrTextBuffer thicknessWritten{};
    PrTextBuffer styleWritten{};
    readTextArgument(p, &leftWritten);
    readTextArgument(p, &rightWritten);
    readTextArgument(p, &thicknessWritten);
    readTextArgument(p, &styleWritten);
    PrTextBuffer left{};
    PrTextBuffer right{};
    delimiterText(prTextBufferView(&leftWritten), &left);
    delimiterText(prTextBufferView(&rightWritten), &right);
    PrTextBuffer thickness{};
    PrStringView thicknessValue{};
    float em = 0;
    if (thicknessWritten.size != 0 && parseDimension(prTextBufferView(&thicknessWritten), &em)) {
        appendLength(&thickness, em, em == 0 ? "" : "em");
        thicknessValue = prTextBufferView(&thickness);
    }
    TexStyle style = context.style;
    if (styleWritten.size == 1 && styleWritten.data[0] >= '0' && styleWritten.data[0] <= '3')
        style = TexStyle(styleWritten.data[0] - '0');
    TexNodeIndex result = parseFraction(p, context, start, style, thicknessValue, prTextBufferView(&left),
        prTextBufferView(&right));
    prTextBufferReset(&leftWritten);
    prTextBufferReset(&rightWritten);
    prTextBufferReset(&thicknessWritten);
    prTextBufferReset(&styleWritten);
    prTextBufferReset(&left);
    prTextBufferReset(&right);
    prTextBufferReset(&thickness);
    return result;
}

// Builds an accent over or under `base`.
TexNodeIndex makeAccent(Parser* p, TexNodeIndex base, PrStringView text, TexAccentFlags flags, const Context& context) {
    bool under = flags & TexAccentFlag_Under;
    TexClass texClass = flags & TexAccentFlag_Limits ? TexClass_Op : TexClass_Ord;
    TexNodeIndex node = makeNode(p, under ? TexElement_Munder : TexElement_Mover, texClass, context);
    // The attribute is always written, because MathML 4 makes an operator that its dictionary calls an accent, such as
    // an arrow or a brace, an accent if the attribute is absent (mathml4/#presm_munder).
    bool clear = flags & TexAccentFlag_Clear;
    texNodeSetAttribute(p->tree, node, under ? "accentunder" : "accent", clear ? "false" : "true");
    if (flags & TexAccentFlag_Limits)
        at(p, node).flags |= TexNodeFlag_Limits;
    TexNodeIndex mark = makeToken(p, TexElement_Mo, text, TexClass_Ord, context);
    at(p, mark).stretchy = flags & TexAccentFlag_Stretchy ? TexTri_True : TexTri_False;
    // A mark set clear of the base is an over- or underscript that is not an accent, which MathML Core places by the
    // font's overbar and underbar gaps, and which keeps the base's size instead of a script's.
    if (clear)
        texNodeSetAttribute(p->tree, mark, "scriptlevel", "+0");
    texNodeAppend(p->tree, node, base);
    texNodeAppend(p->tree, node, mark);
    return node;
}

// Sets the class of an atom that \mathrel and its kin make. A single node takes the class itself, and a list is an
// <mrow> of the class.
TexNodeIndex classAtom(Parser* p, TexNodeIndex atom, TexClass texClass) {
    TexNode& node = at(p, atom);
    node.texClass = texClass;
    node.flags &= TexNodeFlags(~(TexNodeFlag_Transparent | TexNodeFlag_Function));
    if (texClass == TexClass_Op) {
        node.flags |= TexNodeFlag_DisplayLimits;
        TexNodeIndex core = texNodeIsEmbellished(p->tree, atom) ? texNodeCore(p->tree, atom) : kTexNodeNone;
        if (core != kTexNodeNone)
            at(p, core).movablelimits = TexTri_True;
    }
    return atom;
}

// Returns true if a link's destination is safe to keep: one whose scheme cannot run script or read local files.
bool isSafeUrl(PrStringView url) {
    url = prStringTrim(url);
    size_t colon = prStringFindByte(url, ':');
    if (colon == url.size)
        return true;
    size_t slash = prStringFindByte(url, '/');
    if (slash < colon)
        return true;
    char scheme[16];
    if (colon >= sizeof(scheme))
        return true;
    for (size_t i = 0; i < colon; ++i)
        scheme[i] = prAsciiLower(url.data[i]);
    PrStringView name{scheme, colon};
    return !(name == "javascript" || name == "vbscript" || name == "file" || name == "data");
}

// Returns the code point of a character code written in decimal, "hex, 'octal, `char, 0xhex or xhex, as TeX and MathJax
// write them.
uint32_t parseCharacterCode(PrStringView text) {
    text = prStringTrim(text);
    uint32_t radix = 10;
    size_t i = 0;
    if (text.size >= 2 && text.data[0] == '`') {
        uint32_t codePoint = 0;
        prUtf8Decode(text, text.data[1] == '\\' && text.size > 2 ? 2 : 1, &codePoint);
        return codePoint;
    }
    if (text.size != 0 && text.data[0] == '"') {
        radix = 16;
        i = 1;
    } else if (text.size != 0 && text.data[0] == '\'') {
        radix = 8;
        i = 1;
    } else if (text.size >= 2 && text.data[0] == '0' && (text.data[1] == 'x' || text.data[1] == 'X')) {
        radix = 16;
        i = 2;
    } else if (text.size != 0 && (text.data[0] == 'x' || text.data[0] == 'X' || text.data[0] == 'U')) {
        radix = 16;
        i = text.size >= 2 && text.data[1] == '+' ? 2 : 1;
    }
    uint32_t value = 0;
    bool digits = false;
    for (; i < text.size; ++i) {
        char c = text.data[i];
        uint32_t digit = 0;
        if (prAsciiIsDigit(c))
            digit = uint32_t(c - '0');
        else if (prAsciiIsHexDigit(c))
            digit = uint32_t((c | 0x20) - 'a' + 10);
        else
            break;
        if (digit >= radix)
            break;
        value = value * radix + digit;
        digits = true;
        if (value > 0x10FFFF)
            return 0;
    }
    if (!digits || (value >= 0xD800 && value <= 0xDFFF))
        return 0;
    return value;
}

// ---------------------------------------------------------------------------------------------------------------------
// Text mode

// The CSS that sets text in a text font.
PrStringView textFontStyle(TexTextFont font) {
    switch (font) {
        case TexTextFont_Bold:
            return "font-weight: bold";
        case TexTextFont_Italic:
            return "font-style: italic";
        case TexTextFont_SansSerif:
            return "font-family: sans-serif";
        case TexTextFont_Monospace:
            return "font-family: monospace";
        case TexTextFont_Upright:
            break;
    }
    return PrStringView{};
}

TexTextFont textFontFromMathFont(TexFont font) {
    switch (font) {
        case TexFont_Bold:
            return TexTextFont_Bold;
        case TexFont_Italic:
            return TexTextFont_Italic;
        case TexFont_SansSerif:
            return TexTextFont_SansSerif;
        case TexFont_Monospace:
            return TexTextFont_Monospace;
        default:
            return TexTextFont_Upright;
    }
}

// The text of a run of text mode being collected.
struct PendingText {
    PrTextBuffer text;
    TexTextFont font;
};

// Writes the run as an <mtext>. Spaces at its ends would collapse, so they are no-break spaces.
void flushText(Parser* p, PendingText* run, TexNodeIndex target, const Context& context) {
    if (run->text.size == 0)
        return;
    PrStringView text = prTextBufferView(&run->text);
    size_t first = 0;
    while (first < text.size && text.data[first] == ' ')
        ++first;
    size_t last = text.size;
    while (last > first && text.data[last - 1] == ' ')
        --last;
    PrTextBuffer value{};
    for (size_t i = 0; i < first; ++i)
        prTextBufferAppend(&value, " ");
    prTextBufferAppend(&value, prStringSlice(text, first, last - first));
    for (size_t i = last; i < text.size; ++i)
        prTextBufferAppend(&value, " ");
    TexNodeIndex node = makeToken(p, TexElement_Mtext, prTextBufferView(&value), TexClass_Ord, context);
    PrStringView style = textFontStyle(run->font);
    if (style.data != nullptr)
        texNodeSetAttribute(p->tree, node, "style", style);
    texNodeAppend(p->tree, target, node);
    prTextBufferReset(&value);
    prTextBufferClear(&run->text);
}

// Parses text mode to the end of its group, or one token if `single`, into `target`.
void parseText(Parser* p, const Context& context, TexTextFont font, TexNodeIndex target, bool single) {
    if (++p->depth > kMaxDepth) {
        --p->depth;
        return;
    }
    PendingText run{};
    run.font = font;
    Context mathContext = context;
    mathContext.font = TexFont_None;
    mathContext.style = TexStyle_Text;
    for (bool first = true; single ? first : true; first = false) {
        Token token = next(p);
        if (token.kind == TokenKind_End)
            break;
        if (token.kind == TokenKind_Space) {
            if (run.text.size == 0 || run.text.data[run.text.size - 1] != ' ')
                prTextBufferAppendChar(&run.text, ' ');
            continue;
        }
        if (token.kind == TokenKind_Character) {
            uint32_t codePoint = tokenCodePoint(p, token);
            switch (codePoint) {
                case '}':
                    if (single)
                        prTextBufferAppendChar(&run.text, '}');
                    flushText(p, &run, target, context);
                    prTextBufferReset(&run.text);
                    --p->depth;
                    return;
                case '{':
                    flushText(p, &run, target, context);
                    parseText(p, context, run.font, target, false);
                    continue;
                case '$': {
                    flushText(p, &run, target, context);
                    TexNodeIndex math = makeRow(p, mathContext);
                    parseList(p, mathContext, StopFlag_MathShift, math);
                    texNodeAppend(p->tree, target, collapse(p, math));
                    continue;
                }
                case '~':
                    prTextBufferAppend(&run.text, " ");
                    continue;
                case '-': {
                    // TeX's ligatures make -- an en dash and --- an em dash.
                    uint32_t count = 1;
                    for (;;) {
                        Token after = next(p);
                        if (count < 3 && isCharacter(p, after, '-')) {
                            ++count;
                            continue;
                        }
                        if (after.kind != TokenKind_End)
                            pushBack(p, after);
                        break;
                    }
                    prTextBufferAppend(&run.text, count == 1 ? "-" : count == 2 ? "–" : "—");
                    continue;
                }
                case '`':
                case '\'': {
                    Token after = next(p);
                    bool doubled = isCharacter(p, after, char(codePoint));
                    if (!doubled && after.kind != TokenKind_End)
                        pushBack(p, after);
                    if (codePoint == '`')
                        prTextBufferAppend(&run.text, doubled ? "“" : "‘");
                    else
                        prTextBufferAppend(&run.text, doubled ? "”" : "’");
                    continue;
                }
                default:
                    prTextBufferAppend(&run.text, tokenText(p, token));
                    continue;
            }
        }
        // A control sequence.
        PrStringView name = tokenText(p, token);
        if (name == "(") {
            flushText(p, &run, target, context);
            TexNodeIndex math = makeRow(p, mathContext);
            parseList(p, mathContext, StopFlag_MathShift, math);
            texNodeAppend(p->tree, target, collapse(p, math));
            continue;
        }
        const TexEntry* entry = texEntryFromName(name);
        if (entry == nullptr) {
            flushText(p, &run, target, context);
            texNodeAppend(p->tree, target, makeError(p, token.sourceStart, token.sourceEnd, context));
            continue;
        }
        switch (entry->kind) {
            case TexEntryKind_TextFont: {
                flushText(p, &run, target, context);
                Token open = nextNonSpace(p);
                bool group = isCharacter(p, open, '{');
                if (!group && open.kind != TokenKind_End)
                    pushBack(p, open);
                parseText(p, context, TexTextFont(entry->arg), target, !group);
                continue;
            }
            case TexEntryKind_FontSwitch:
                flushText(p, &run, target, context);
                run.font = textFontFromMathFont(TexFont(entry->arg));
                continue;
            case TexEntryKind_Font: {
                flushText(p, &run, target, context);
                TexNodeIndex argument = parseArgument(p, mathContext);
                if (argument != kTexNodeNone)
                    texNodeAppend(p->tree, target, argument);
                continue;
            }
            case TexEntryKind_TextAccent: {
                Token base = nextNonSpace(p);
                if (isCharacter(p, base, '{')) {
                    pushBack(p, base);
                    PrTextBuffer written{};
                    readTextArgument(p, &written);
                    PrStringView text = prTextBufferView(&written);
                    if (text.size != 0 && text.data[0] == '\\') {
                        const TexEntry* letter = texEntryFromName(prStringSlice(text, 1, text.size - 1));
                        if (letter != nullptr && letter->text != nullptr)
                            prTextBufferAppend(&run.text, letter->text);
                    } else {
                        prTextBufferAppend(&run.text, text);
                    }
                    prTextBufferReset(&written);
                } else if (base.kind == TokenKind_Command) {
                    const TexEntry* letter = texEntryFromName(tokenText(p, base));
                    if (letter != nullptr && letter->text != nullptr)
                        prTextBufferAppend(&run.text, letter->text);
                } else if (base.kind == TokenKind_Character) {
                    prTextBufferAppend(&run.text, tokenText(p, base));
                }
                prTextBufferAppend(&run.text, entry->text);
                continue;
            }
            case TexEntryKind_TextSymbol:
            case TexEntryKind_Letter:
            case TexEntryKind_Upright:
            case TexEntryKind_Operator:
            case TexEntryKind_LargeOperator:
            case TexEntryKind_Integral:
                prTextBufferAppend(&run.text, entry->text);
                continue;
            case TexEntryKind_Space:
                flushText(p, &run, target, context);
                texNodeAppend(p->tree, target, makeMuSpace(p, entry->arg, context));
                continue;
            case TexEntryKind_Command:
                break;
            default:
                flushText(p, &run, target, context);
                texNodeAppend(p->tree, target, makeError(p, token.sourceStart, token.sourceEnd, context));
                continue;
        }
        switch (TexCommand(entry->arg)) {
            case TexCommand_TextMode: {
                flushText(p, &run, target, context);
                Token open = nextNonSpace(p);
                bool group = isCharacter(p, open, '{');
                if (!group && open.kind != TokenKind_End)
                    pushBack(p, open);
                parseText(p, context, run.font, target, !group);
                continue;
            }
            case TexCommand_TextColor:
            case TexCommand_Color: {
                flushText(p, &run, target, context);
                PrTextBuffer color{};
                bool valid = readColor(p, &color);
                TexNodeIndex colored = makeNode(p, TexElement_Mstyle, TexClass_Ord, context);
                if (valid)
                    texNodeSetAttribute(p->tree, colored, "mathcolor", prTextBufferView(&color));
                prTextBufferReset(&color);
                if (TexCommand(entry->arg) == TexCommand_TextColor) {
                    Token open = nextNonSpace(p);
                    bool group = isCharacter(p, open, '{');
                    if (!group && open.kind != TokenKind_End)
                        pushBack(p, open);
                    parseText(p, context, run.font, colored, !group);
                    texNodeAppend(p->tree, target, colored);
                    continue;
                }
                // \color sets the rest of the group.
                at(p, colored).flags |= TexNodeFlag_Transparent;
                parseText(p, context, run.font, colored, false);
                texNodeAppend(p->tree, target, colored);
                prTextBufferReset(&run.text);
                --p->depth;
                return;
            }
            case TexCommand_Newline:
                prTextBufferAppendChar(&run.text, ' ');
                continue;
            case TexCommand_Relax:
            case TexCommand_Label:
            case TexCommand_NoTag:
                continue;
            case TexCommand_HSpace:
            case TexCommand_Kern:
            case TexCommand_MKern: {
                flushText(p, &run, target, context);
                if (TexCommand(entry->arg) == TexCommand_HSpace)
                    acceptCharacter(p, '*');
                float em = 0;
                if (readDimension(p, &em))
                    texNodeAppend(p->tree, target, makeSpace(p, em, context));
                continue;
            }
            case TexCommand_Unicode:
            case TexCommand_Char: {
                PrTextBuffer written{};
                if (TexCommand(entry->arg) == TexCommand_Unicode) {
                    readTextArgument(p, &written);
                } else {
                    for (Token part = nextNonSpace(p);
                            part.kind == TokenKind_Character && !prAsciiIsSpace(tokenText(p, part).data[0]); part = next(p))
                        prTextBufferAppend(&written, tokenText(p, part));
                }
                uint32_t codePoint = parseCharacterCode(prTextBufferView(&written));
                if (codePoint != 0)
                    prTextBufferAppendCodePoint(&run.text, codePoint);
                prTextBufferReset(&written);
                continue;
            }
            case TexCommand_Href:
            case TexCommand_Url: {
                flushText(p, &run, target, context);
                PrTextBuffer url{};
                readTextArgument(p, &url);
                TexNodeIndex link = makeRow(p, context);
                if (isSafeUrl(prTextBufferView(&url)))
                    texNodeSetAttribute(p->tree, link, "href", prTextBufferView(&url));
                if (TexCommand(entry->arg) == TexCommand_Href) {
                    Token open = nextNonSpace(p);
                    bool group = isCharacter(p, open, '{');
                    if (!group && open.kind != TokenKind_End)
                        pushBack(p, open);
                    parseText(p, context, run.font, link, !group);
                } else {
                    texNodeAppend(p->tree, link, makeToken(p, TexElement_Mtext, prTextBufferView(&url), TexClass_Ord, context));
                }
                prTextBufferReset(&url);
                texNodeAppend(p->tree, target, link);
                continue;
            }
            default:
                flushText(p, &run, target, context);
                texNodeAppend(p->tree, target, makeError(p, token.sourceStart, token.sourceEnd, context));
                continue;
        }
    }
    flushText(p, &run, target, context);
    prTextBufferReset(&run.text);
    --p->depth;
}

TexNodeIndex parseTextArgument(Parser* p, const Context& context, TexTextFont font) {
    TexNodeIndex row = makeRow(p, context);
    Token open = nextNonSpace(p);
    bool group = isCharacter(p, open, '{');
    if (!group && open.kind != TokenKind_End)
        pushBack(p, open);
    parseText(p, context, font, row, !group);
    if (at(p, row).firstChild == kTexNodeNone)
        return makeToken(p, TexElement_Mtext, "", TexClass_Ord, context);
    return collapse(p, row);
}

// ---------------------------------------------------------------------------------------------------------------------
// Definitions

// Reads the name of a definition, a control sequence that may be braced. Returns false if there is none.
bool readDefinedName(Parser* p, PrTextBuffer* out) {
    Token token = nextRaw(p);
    while (token.kind == TokenKind_Space)
        token = nextRaw(p);
    bool braced = isCharacter(p, token, '{');
    if (braced) {
        token = nextRaw(p);
        while (token.kind == TokenKind_Space)
            token = nextRaw(p);
    }
    if (token.kind != TokenKind_Command)
        return false;
    prTextBufferAppend(out, tokenText(p, token));
    if (braced) {
        Token close = nextRaw(p);
        while (close.kind == TokenKind_Space)
            close = nextRaw(p);
        if (!isCharacter(p, close, '}') && close.kind != TokenKind_End)
            pushBack(p, close);
    }
    return true;
}

// Returns true if the control sequence has a meaning, as a macro or in the language itself.
bool isDefined(const Parser* p, PrStringView name) {
    return findMacro(p, name) != nullptr || texEntryFromName(name) != nullptr;
}

// Defines a macro in the formula, or globally in the document and in the formula.
void define(Parser* p, PrStringView name, uint32_t parameterCount, PrStringView replacement,
        const PrStringView* PR_NULLABLE defaultArgument, PrStringView parameterText, bool global) {
    if (global && p->document != &p->own)
        texMacroTableDefine(&p->document->macros, name, parameterCount, replacement, defaultArgument, parameterText);
    texMacroTableDefine(&p->local, name, parameterCount, replacement, defaultArgument, parameterText);
}

// Parses \newcommand, \renewcommand or \providecommand after its name.
bool parseNewCommand(Parser* p, TexCommand command) {
    acceptCharacter(p, '*');
    PrTextBuffer name{};
    if (!readDefinedName(p, &name)) {
        prTextBufferReset(&name);
        return false;
    }
    PrTextBuffer count{};
    uint32_t parameterCount = 0;
    if (readOptionalText(p, &count) && count.size == 1 && count.data[0] >= '0' && count.data[0] <= '9')
        parameterCount = uint32_t(count.data[0] - '0');
    PrTextBuffer defaultArgument{};
    bool hasDefault = readOptionalText(p, &defaultArgument);
    PrTextBuffer replacement{};
    bool found = readTextArgument(p, &replacement);
    if (found && !(command == TexCommand_ProvideCommand && isDefined(p, prTextBufferView(&name)))) {
        PrStringView defaultView = prTextBufferView(&defaultArgument);
        define(p, prTextBufferView(&name), parameterCount, prTextBufferView(&replacement),
            hasDefault ? &defaultView : nullptr, PrStringView{}, false);
    }
    prTextBufferReset(&name);
    prTextBufferReset(&count);
    prTextBufferReset(&defaultArgument);
    prTextBufferReset(&replacement);
    return found;
}

// Parses what follows \def or \gdef, which is a name, parameters #1 to #9, and a replacement in braces.
bool parseDef(Parser* p, bool global) {
    Token token = nextRaw(p);
    while (token.kind == TokenKind_Space)
        token = nextRaw(p);
    if (token.kind != TokenKind_Command)
        return false;
    PrTextBuffer name{};
    prTextBufferAppend(&name, tokenText(p, token));
    // The parameter text holds parameters #1 to #9 in order, and the tokens that delimit them.
    RawArray<Token> parameters{};
    for (;;) {
        Token part = nextRaw(p);
        if (part.kind == TokenKind_End) {
            prTextBufferReset(&name);
            parameters.reset();
            return false;
        }
        if (isCharacter(p, part, '{')) {
            pushBack(p, part);
            break;
        }
        parameters.push(part);
    }
    uint32_t parameterCount = 0;
    bool delimited = false;
    for (uint32_t i = 0; i < parameters.count; ++i) {
        if (isParameter(p, parameters, i)) {
            parameterCount = uint32_t(tokenText(p, parameters.data[i + 1]).data[0] - '0');
            ++i;
        } else {
            delimited = true;
        }
    }
    PrTextBuffer parameterText{};
    if (delimited)
        appendTokensText(p, parameters, &parameterText);
    RawArray<Token> tokens{};
    readArgumentTokens(p, &tokens);
    PrTextBuffer replacement{};
    appendTokensText(p, tokens, &replacement);
    define(p, prTextBufferView(&name), parameterCount, prTextBufferView(&replacement), nullptr,
        prTextBufferView(&parameterText), global);
    tokens.reset();
    parameters.reset();
    prTextBufferReset(&name);
    prTextBufferReset(&parameterText);
    prTextBufferReset(&replacement);
    return true;
}

// Parses what follows \let, which is a name, an optional =, and the token whose meaning the name takes.
bool parseLet(Parser* p, bool global) {
    Token token = nextRaw(p);
    while (token.kind == TokenKind_Space)
        token = nextRaw(p);
    if (token.kind != TokenKind_Command)
        return false;
    PrTextBuffer name{};
    prTextBufferAppend(&name, tokenText(p, token));
    Token source = nextRaw(p);
    while (source.kind == TokenKind_Space)
        source = nextRaw(p);
    if (isCharacter(p, source, '=')) {
        source = nextRaw(p);
        while (source.kind == TokenKind_Space)
            source = nextRaw(p);
    }
    bool defined = source.kind != TokenKind_End;
    if (defined && !(source.kind == TokenKind_Command && tokenText(p, source) == prTextBufferView(&name))) {
        PrTextBuffer replacement{};
        const TexMacro* macro = source.kind == TokenKind_Command ? findMacro(p, tokenText(p, source)) : nullptr;
        if (macro != nullptr) {
            // The macro's definition is copied, so that redefining the source later leaves the name as it was.
            PrRawString copy = prRawStringCopy(prRawStringView(&macro->replacement));
            uint32_t parameterCount = macro->parameterCount;
            PrRawString parameterText = prRawStringCopy(prRawStringView(&macro->parameterText));
            define(p, prTextBufferView(&name), parameterCount, prRawStringView(&copy), nullptr,
                prRawStringView(&parameterText), global);
            prRawStringReset(&parameterText);
            prRawStringReset(&copy);
        } else {
            if (source.kind == TokenKind_Command)
                prTextBufferAppendChar(&replacement, '\\');
            prTextBufferAppend(&replacement, tokenText(p, source));
            define(p, prTextBufferView(&name), 0, prTextBufferView(&replacement), nullptr, PrStringView{}, global);
        }
        prTextBufferReset(&replacement);
    }
    prTextBufferReset(&name);
    return defined;
}

// ---------------------------------------------------------------------------------------------------------------------
// Commands

// Makes the box of \fbox, \colorbox and \boxed, which pads `content` by TeX's \fboxsep of 3pt on every side and frames
// it with a rule if `framed`.
TexNodeIndex makeFrameBox(Parser* p, TexNodeIndex content, bool framed, const Context& context) {
    TexNodeIndex padded = makeNode(p, TexElement_Mpadded, TexClass_Ord, context);
    appendContents(p, padded, content);
    texNodeSetAttribute(p->tree, padded, "lspace", "0.3em");
    texNodeSetAttribute(p->tree, padded, "width", "+0.6em");
    texNodeSetAttribute(p->tree, padded, "height", "+0.3em");
    texNodeSetAttribute(p->tree, padded, "depth", "+0.3em");
    if (!framed)
        return padded;
    TexNodeIndex box = wrap(p, TexElement_Menclose, padded, TexClass_Ord, context);
    texNodeSetAttribute(p->tree, box, "notation", "box");
    return box;
}

TexNodeIndex parsePhantom(Parser* p, const Context& context, TexCommand command, uint32_t start) {
    TexNodeIndex argument = requireArgument(p, parseArgument(p, context), start, context);
    TexNodeIndex phantom = makeNode(p, TexElement_Mphantom, TexClass_Ord, context);
    appendContents(p, phantom, argument);
    if (command == TexCommand_Phantom)
        return phantom;
    TexNodeIndex padded = wrap(p, TexElement_Mpadded, phantom, TexClass_Ord, context);
    if (command == TexCommand_HPhantom) {
        texNodeSetAttribute(p->tree, padded, "height", "0");
        texNodeSetAttribute(p->tree, padded, "depth", "0");
    } else {
        texNodeSetAttribute(p->tree, padded, "width", "0");
    }
    return padded;
}

TexNodeIndex parseLap(Parser* p, const Context& context, TexCommand command, PrStringView name, uint32_t start) {
    // \rlap and its kin take text, and \mathrlap and its kin take math.
    bool math = prStringStartsWith(name, "math");
    TexNodeIndex argument = kTexNodeNone;
    if (math)
        argument = requireArgument(p, parseArgument(p, context), start, context);
    else
        argument = parseTextArgument(p, context, TexTextFont_Upright);
    // A box of zero width, whose contents a left or center lap moves back by all or half of their own width, in the
    // pseudo-unit of mpadded (mathml4/#presm_mpadded).
    TexNodeIndex padded = makeNode(p, TexElement_Mpadded, TexClass_Ord, context);
    texNodeSetAttribute(p->tree, padded, "width", "0");
    if (command == TexCommand_LLap)
        texNodeSetAttribute(p->tree, padded, "lspace", "-1width");
    else if (command == TexCommand_CLap)
        texNodeSetAttribute(p->tree, padded, "lspace", "-0.5width");
    appendContents(p, padded, argument);
    return padded;
}

TexNodeIndex parseEnclose(Parser* p, const Context& context, PrStringView notation, uint32_t start) {
    TexNodeIndex argument = requireArgument(p, parseArgument(p, context), start, context);
    TexNodeIndex enclose = makeNode(p, TexElement_Menclose, TexClass_Ord, context);
    texNodeSetAttribute(p->tree, enclose, "notation", notation);
    appendContents(p, enclose, argument);
    return enclose;
}

// Parses \sideset{pre}{post} and the operator after it, as <mmultiscripts>.
TexNodeIndex parseSideSet(Parser* p, const Context& context, uint32_t start) {
    Context scripts = context;
    scripts.style = scriptStyle(context.style);
    // The scripts of each side, as pairs of a subscript and a superscript.
    RawArray<TexNodeIndex> sides[2] = {};
    for (RawArray<TexNodeIndex>& side : sides) {
        if (!acceptCharacter(p, '{')) {
            sides[0].reset();
            sides[1].reset();
            return makeError(p, start, p->lastEnd, context);
        }
        TexNodeIndex sub = kTexNodeNone;
        TexNodeIndex sup = kTexNodeNone;
        for (;;) {
            Token token = nextNonSpace(p);
            if (token.kind == TokenKind_End || isCharacter(p, token, '}'))
                break;
            bool isSub = isCharacter(p, token, '_');
            bool isPrime = isCharacter(p, token, '\'');
            if (!isSub && !isPrime && !isCharacter(p, token, '^'))
                continue;
            TexNodeIndex script = isPrime ? makePrimes(p, 1, scripts) : parseArgument(p, scripts);
            if (script == kTexNodeNone)
                break;
            TexNodeIndex& slot = isSub ? sub : sup;
            if (slot != kTexNodeNone) {
                side.push(sub != kTexNodeNone ? sub : makeRow(p, scripts));
                side.push(sup != kTexNodeNone ? sup : makeRow(p, scripts));
                sub = kTexNodeNone;
                sup = kTexNodeNone;
            }
            slot = script;
        }
        if (sub != kTexNodeNone || sup != kTexNodeNone) {
            side.push(sub != kTexNodeNone ? sub : makeRow(p, scripts));
            side.push(sup != kTexNodeNone ? sup : makeRow(p, scripts));
        }
    }
    Token token = nextNonSpace(p);
    TexNodeIndex base = token.kind != TokenKind_End ? parseAtom(p, context, token, true) : kTexNodeNone;
    if (base == kTexNodeNone) {
        sides[0].reset();
        sides[1].reset();
        return makeError(p, start, p->lastEnd, context);
    }
    TexNodeIndex node = makeNode(p, TexElement_Mmultiscripts, at(p, base).texClass, context);
    at(p, node).flags = at(p, base).flags & (TexNodeFlag_DisplayLimits | TexNodeFlag_Limits);
    texNodeAppend(p->tree, node, base);
    for (uint32_t i = 0; i < sides[1].count; ++i)
        texNodeAppend(p->tree, node, sides[1].data[i]);
    if (sides[0].count != 0) {
        texNodeAppend(p->tree, node, makeNode(p, TexElement_Mprescripts, TexClass_None, context));
        for (uint32_t i = 0; i < sides[0].count; ++i)
            texNodeAppend(p->tree, node, sides[0].data[i]);
    }
    sides[0].reset();
    sides[1].reset();
    return node;
}

// Parses an extensible arrow: an optional label below and a label above.
TexNodeIndex parseExtensibleArrow(Parser* p, const Context& context, PrStringView text) {
    Context labels = context;
    labels.style = scriptStyle(context.style);
    TexNodeIndex below = parseOptionalArgument(p, labels);
    TexNodeIndex above = parseArgument(p, labels);
    TexNodeIndex arrow = makeToken(p, TexElement_Mo, text, TexClass_Rel, context);
    at(p, arrow).stretchy = TexTri_True;
    // amsmath pads each label by a thick space on both sides, which sets the arrow's length.
    auto pad = [&](TexNodeIndex label) {
        TexNodeIndex padded = makeNode(p, TexElement_Mpadded, TexClass_Ord, labels);
        appendContents(p, padded, label);
        texNodeSetAttribute(p->tree, padded, "lspace", "0.2778em");
        texNodeSetAttribute(p->tree, padded, "width", "+0.5556em");
        return padded;
    };
    bool hasAbove = above != kTexNodeNone &&
        (at(p, above).element != TexElement_Mrow || at(p, above).firstChild != kTexNodeNone);
    TexElement element = TexElement_Mover;
    if (below != kTexNodeNone)
        element = hasAbove ? TexElement_Munderover : TexElement_Munder;
    TexNodeIndex node = makeNode(p, element, TexClass_Rel, context);
    texNodeAppend(p->tree, node, arrow);
    if (below != kTexNodeNone)
        texNodeAppend(p->tree, node, pad(below));
    if (hasAbove || below == kTexNodeNone)
        texNodeAppend(p->tree, node, pad(above != kTexNodeNone ? above : makeRow(p, labels)));
    return node;
}

// Parses \operatorname and \operatorname*.
TexNodeIndex parseOperatorName(Parser* p, const Context& context, bool limits, uint32_t start) {
    if (acceptCharacter(p, '*'))
        limits = true;
    Context name = context;
    name.font = TexFont_Roman;
    name.operatorName = true;
    TexNodeIndex atom = requireArgument(p, parseArgument(p, name), start, context);
    TexNode& node = at(p, atom);
    node.texClass = TexClass_Op;
    node.flags &= TexNodeFlags(~TexNodeFlag_Transparent);
    if (!limits) {
        node.flags |= TexNodeFlag_Function;
        return atom;
    }
    node.flags |= TexNodeFlag_DisplayLimits;
    // A name of one <mi> is an operator, whose limits move by style as \lim's do.
    if (node.element == TexElement_Mi) {
        node.element = TexElement_Mo;
        node.movablelimits = TexTri_True;
    }
    return atom;
}

TexNodeIndex parseMod(Parser* p, const Context& context, TexCommand command, uint32_t start) {
    bool display = p->display == PrTexDisplay_Block;
    TexNodeIndex row = makeRow(p, context);
    Context roman = context;
    roman.font = TexFont_Roman;
    const uint32_t kMod[] = {'m', 'o', 'd'};
    if (command == TexCommand_Mod) {
        // amsmath's \mod is \mkern18mu in display style and 12mu otherwise, then "mod", \,\, and the argument.
        texNodeAppend(p->tree, row, makeMuSpace(p, display ? 18 : 12, context));
        texNodeAppend(p->tree, row, makeLetters(p, kMod, 3, roman));
        texNodeAppend(p->tree, row, makeMuSpace(p, 6, context));
        TexNodeIndex argument = requireArgument(p, parseArgument(p, context), start, context);
        appendContents(p, row, argument);
        return row;
    }
    // \pod{x} is \mkern18mu in display style and 8mu otherwise, then (x). \pmod{x} is \pod{mod\mkern6mu x}.
    texNodeAppend(p->tree, row, makeMuSpace(p, display ? 18 : 8, context));
    texNodeAppend(p->tree, row, makeOperator(p, "(", TexClass_Open, context));
    if (command == TexCommand_PMod) {
        texNodeAppend(p->tree, row, makeLetters(p, kMod, 3, roman));
        texNodeAppend(p->tree, row, makeMuSpace(p, 6, context));
    }
    TexNodeIndex argument = requireArgument(p, parseArgument(p, context), start, context);
    appendContents(p, row, argument);
    texNodeAppend(p->tree, row, makeOperator(p, ")", TexClass_Close, context));
    return row;
}

// Parses \dots, which amsmath sets as \cdots before a binary operator or a relation, and as \ldots otherwise.
TexNodeIndex parseDots(Parser* p, const Context& context) {
    Token after = nextNonSpace(p);
    bool centered = false;
    if (after.kind == TokenKind_Character) {
        uint32_t codePoint = tokenCodePoint(p, after);
        centered = codePoint == '+' || codePoint == '-' || codePoint == '=' || codePoint == '<' || codePoint == '>' ||
            codePoint == '*';
    } else if (after.kind == TokenKind_Command) {
        const TexEntry* entry = texEntryFromName(tokenText(p, after));
        centered = entry != nullptr && (entry->texClass == TexClass_Bin || entry->texClass == TexClass_Rel ||
            entry->kind == TexEntryKind_Integral || entry->kind == TexEntryKind_LargeOperator);
    }
    if (after.kind != TokenKind_End)
        pushBack(p, after);
    return makeOperator(p, centered ? "⋯" : "…", TexClass_Inner, context);
}

// Returns the row that \tag and \notag apply to. It is the row being parsed of the innermost table if that table is an
// environment that numbers equations in display math, or the formula's own row outside any table. Returns null if a tag
// is not allowed.
TableRow* PR_NULLABLE taggedRow(Parser* p) {
    TableState* state = p->table != nullptr ? p->table : &p->top;
    if (!state->tagged || p->display != PrTexDisplay_Block || state->rows.count == 0)
        return nullptr;
    return &state->rows.last();
}

TexNodeIndex parseCommand(Parser* p, const Context& context, const Token& token, TexCommand command) {
    uint32_t start = token.sourceStart;
    PrStringView name = tokenText(p, token);
    switch (command) {
        case TexCommand_Frac:
            return parseFraction(p, context, start, context.style, PrStringView{}, "", "");
        case TexCommand_DisplayFrac:
            return parseFraction(p, context, start, TexStyle_Display, PrStringView{}, "", "");
        case TexCommand_TextFrac:
            return parseFraction(p, context, start, TexStyle_Text, PrStringView{}, "", "");
        case TexCommand_Binom:
            return parseFraction(p, context, start, context.style, "0", "(", ")");
        case TexCommand_DisplayBinom:
            return parseFraction(p, context, start, TexStyle_Display, "0", "(", ")");
        case TexCommand_TextBinom:
            return parseFraction(p, context, start, TexStyle_Text, "0", "(", ")");
        case TexCommand_GenFrac:
            return parseGenFrac(p, context, start);
        case TexCommand_ContinuedFrac: {
            // amsmath's \cfrac sets display style throughout, with a strut in the numerator so that the parts of a
            // continued fraction line up.
            PrTextBuffer alignment{};
            readOptionalText(p, &alignment);
            PrStringView numeratorAlign{};
            if (prTextBufferView(&alignment) == "l")
                numeratorAlign = "left";
            else if (prTextBufferView(&alignment) == "r")
                numeratorAlign = "right";
            prTextBufferReset(&alignment);
            Context parts = context;
            parts.style = TexStyle_Display;
            TexNodeIndex numerator = parseArgument(p, parts);
            TexNodeIndex denominator = numerator != kTexNodeNone ? parseArgument(p, parts) : kTexNodeNone;
            if (denominator == kTexNodeNone)
                return makeError(p, start, p->lastEnd, context);
            Context partsInFraction = parts;
            partsInFraction.style = TexStyle_Text;
            TexNodeIndex strutted = makeRow(p, parts);
            texNodeAppend(p->tree, strutted, makeStrut(p, 0.84f, 0.36f, parts));
            appendContents(p, strutted, numerator);
            TexNodeIndex fraction = makeNode(p, TexElement_Mfrac, TexClass_Inner, parts);
            // \cfrac[l] and \cfrac[r] set the numerator flush left or right (mathml4/#presm_mfrac).
            if (numeratorAlign.data != nullptr)
                texNodeSetAttribute(p->tree, fraction, "numalign", numeratorAlign);
            texNodeAppend(p->tree, fraction, inStyle(p, strutted, TexStyle_Display, partsInFraction));
            texNodeAppend(p->tree, fraction, inStyle(p, denominator, TexStyle_Display, partsInFraction));
            return inStyle(p, fraction, TexStyle_Display, context);
        }
        case TexCommand_Over:
        case TexCommand_Atop:
        case TexCommand_Choose:
        case TexCommand_Brack:
        case TexCommand_Brace:
        case TexCommand_Above:
            // A second infix fraction in one list is TeX's "Ambiguous; you need another { and }".
            return makeError(p, start, token.sourceEnd, context);
        case TexCommand_Sqrt: {
            Context index = context;
            index.style = TexStyle_ScriptScript;
            TexNodeIndex root = parseOptionalArgument(p, index);
            TexNodeIndex base = requireArgument(p, parseArgument(p, context), start, context);
            if (root == kTexNodeNone) {
                TexNodeIndex sqrt = makeNode(p, TexElement_Msqrt, TexClass_Ord, context);
                appendContents(p, sqrt, base);
                return sqrt;
            }
            TexNodeIndex node = makeNode(p, TexElement_Mroot, TexClass_Ord, context);
            texNodeAppend(p->tree, node, base);
            texNodeAppend(p->tree, node, root);
            return node;
        }
        case TexCommand_Left:
            return parseLeftRight(p, context, start);
        case TexCommand_Right:
        case TexCommand_Middle: {
            // A \right or \middle outside \left is an error, TeX's "Extra \right" or "Missing \left".
            PrTextBuffer delimiter{};
            readDelimiter(p, &delimiter);
            prTextBufferReset(&delimiter);
            return makeError(p, start, p->lastEnd, context);
        }
        case TexCommand_Limits:
        case TexCommand_NoLimits:
        case TexCommand_DisplayLimits:
            // TeX's "Limit controls must follow a math operator".
            return makeError(p, start, token.sourceEnd, context);
        case TexCommand_OverSet:
        case TexCommand_UnderSet:
        case TexCommand_StackRel: {
            Context scripts = context;
            scripts.style = scriptStyle(context.style);
            TexNodeIndex script = parseArgument(p, scripts);
            TexNodeIndex base = script != kTexNodeNone ? parseArgument(p, context) : kTexNodeNone;
            if (base == kTexNodeNone)
                return makeError(p, start, p->lastEnd, context);
            // amsmath keeps a binary operator's or a relation's class, and \stackrel makes a relation.
            TexClass baseClass = at(p, base).texClass;
            TexClass texClass = baseClass == TexClass_Bin || baseClass == TexClass_Rel ? baseClass : TexClass_Ord;
            if (command == TexCommand_StackRel)
                texClass = TexClass_Rel;
            TexNodeIndex node = makeNode(p, command == TexCommand_UnderSet ? TexElement_Munder : TexElement_Mover,
                texClass, context);
            texNodeAppend(p->tree, node, base);
            texNodeAppend(p->tree, node, script);
            return node;
        }
        case TexCommand_OperatorName:
            return parseOperatorName(p, context, false, start);
        case TexCommand_OperatorNameWithLimits:
            return parseOperatorName(p, context, true, start);
        case TexCommand_MathOrd:
        case TexCommand_MathOp:
        case TexCommand_MathBin:
        case TexCommand_MathRel:
        case TexCommand_MathOpen:
        case TexCommand_MathClose:
        case TexCommand_MathPunct:
        case TexCommand_MathInner: {
            static const TexClass kClasses[] = {
                TexClass_Ord, TexClass_Op, TexClass_Bin, TexClass_Rel, TexClass_Open, TexClass_Close,
                TexClass_Punct, TexClass_Inner,
            };
            TexNodeIndex argument = requireArgument(p, parseArgument(p, context), start, context);
            return classAtom(p, argument, kClasses[command - TexCommand_MathOrd]);
        }
        case TexCommand_HSpace:
        case TexCommand_Kern:
        case TexCommand_MKern: {
            if (command == TexCommand_HSpace)
                acceptCharacter(p, '*');
            float em = 0;
            if (!readDimension(p, &em))
                return makeError(p, start, p->lastEnd, context);
            return makeSpace(p, em, context);
        }
        case TexCommand_Color: {
            // A declaration, which parseList handles. One that is an argument by itself colors nothing.
            PrTextBuffer color{};
            readColor(p, &color);
            prTextBufferReset(&color);
            return kTexNodeNone;
        }
        case TexCommand_TextColor: {
            PrTextBuffer color{};
            bool valid = readColor(p, &color);
            TexNodeIndex argument = requireArgument(p, parseArgument(p, context), start, context);
            if (!valid) {
                prTextBufferReset(&color);
                return makeError(p, start, p->lastEnd, context);
            }
            TexNodeIndex colored = argument;
            if (texNodeHasAttribute(p->tree, argument, "mathcolor"))
                colored = wrap(p, TexElement_Mrow, argument, TexClass_Ord, context);
            texNodeSetAttribute(p->tree, colored, "mathcolor", prTextBufferView(&color));
            prTextBufferReset(&color);
            return groupAtom(p, colored);
        }
        case TexCommand_ColorBox:
        case TexCommand_FColorBox:
        case TexCommand_FBox: {
            PrTextBuffer frame{};
            PrTextBuffer background{};
            bool valid = true;
            if (command == TexCommand_FColorBox)
                valid = readColor(p, &frame);
            if (command != TexCommand_FBox)
                valid = readColor(p, &background) && valid;
            TexNodeIndex argument = parseTextArgument(p, context, TexTextFont_Upright);
            if (!valid) {
                prTextBufferReset(&frame);
                prTextBufferReset(&background);
                return makeError(p, start, p->lastEnd, context);
            }
            // MathML draws a frame in its element's color, which its contents inherit, so a frame of a color of its own
            // is a CSS border.
            bool colored = command == TexCommand_FColorBox;
            TexNodeIndex box = makeFrameBox(p, argument, command == TexCommand_FBox, context);
            if (background.size != 0)
                texNodeSetAttribute(p->tree, box, "mathbackground", prTextBufferView(&background));
            if (colored) {
                PrTextBuffer style{};
                prTextBufferAppend(&style, "border: 0.04em solid ");
                prTextBufferAppend(&style, prTextBufferView(&frame));
                texNodeSetAttribute(p->tree, box, "style", prTextBufferView(&style));
                prTextBufferReset(&style);
            }
            prTextBufferReset(&frame);
            prTextBufferReset(&background);
            return box;
        }
        case TexCommand_Boxed: {
            // amsmath's \boxed sets its argument in display style, in an \fbox.
            Context display = context;
            display.style = TexStyle_Display;
            TexNodeIndex argument = requireArgument(p, parseArgument(p, display), start, context);
            return makeFrameBox(p, inStyle(p, argument, TexStyle_Display, context), true, context);
        }
        case TexCommand_Phantom:
        case TexCommand_HPhantom:
        case TexCommand_VPhantom:
            return parsePhantom(p, context, command, start);
        case TexCommand_Smash: {
            PrTextBuffer which{};
            readOptionalText(p, &which);
            TexNodeIndex argument = requireArgument(p, parseArgument(p, context), start, context);
            TexNodeIndex padded = makeNode(p, TexElement_Mpadded, TexClass_Ord, context);
            appendContents(p, padded, argument);
            PrStringView part = prTextBufferView(&which);
            if (part.size == 0 || part == "t")
                texNodeSetAttribute(p->tree, padded, "height", "0");
            if (part.size == 0 || part == "b")
                texNodeSetAttribute(p->tree, padded, "depth", "0");
            prTextBufferReset(&which);
            return padded;
        }
        case TexCommand_RLap:
        case TexCommand_LLap:
        case TexCommand_CLap:
            return parseLap(p, context, command, name, start);
        case TexCommand_Cancel:
            return parseEnclose(p, context, "updiagonalstrike", start);
        case TexCommand_BCancel:
            return parseEnclose(p, context, "downdiagonalstrike", start);
        case TexCommand_XCancel:
            return parseEnclose(p, context, "updiagonalstrike downdiagonalstrike", start);
        case TexCommand_SOut:
            return parseEnclose(p, context, "horizontalstrike", start);
        case TexCommand_Enclose: {
            PrTextBuffer notation{};
            readTextArgument(p, &notation);
            PrTextBuffer ignored{};
            readOptionalText(p, &ignored);
            prTextBufferReset(&ignored);
            TexNodeIndex node = parseEnclose(p, context, prTextBufferView(&notation), start);
            prTextBufferReset(&notation);
            return node;
        }
        case TexCommand_Not: {
            Token next = nextNonSpace(p);
            TexNodeIndex atom = next.kind != TokenKind_End ? parseAtom(p, context, next, true) : kTexNodeNone;
            if (atom == kTexNodeNone)
                return makeError(p, start, p->lastEnd, context);
            TexNode& node = at(p, atom);
            if (node.element == TexElement_Mo || node.element == TexElement_Mi) {
                PrStringView text = texNodeText(p->tree, atom);
                uint32_t codePoint = 0;
                uint32_t size = text.size != 0 ? prUtf8Decode(text, 0, &codePoint) : 0;
                uint32_t negated = size == text.size ? texNegate(codePoint) : 0;
                if (negated != 0) {
                    char utf8[kPrUtf8MaxBytes];
                    texNodeSetText(p->tree, atom, PrStringView{utf8, prUtf8Encode(negated, utf8)});
                } else {
                    texNodeAppendText(p->tree, atom, "\xcc\xb8");
                }
                return atom;
            }
            // A construct that is not a symbol is struck through.
            TexNodeIndex enclose = makeNode(p, TexElement_Menclose, node.texClass, context);
            texNodeSetAttribute(p->tree, enclose, "notation", "updiagonalstrike");
            texNodeAppend(p->tree, enclose, atom);
            return enclose;
        }
        case TexCommand_Begin:
            return parseEnvironment(p, context, start, false);
        case TexCommand_End: {
            PrTextBuffer environment{};
            readTextArgument(p, &environment);
            prTextBufferReset(&environment);
            return makeError(p, start, p->lastEnd, context);
        }
        case TexCommand_Newline: {
            // A line break where the formula has no lines, which TeX ignores in math.
            PrTextBuffer gap{};
            readOptionalText(p, &gap);
            prTextBufferReset(&gap);
            return kTexNodeNone;
        }
        case TexCommand_HLine:
        case TexCommand_HDashLine:
            if (p->table != nullptr && p->table->rows.count != 0)
                p->table->rows.last().lineAbove = command == TexCommand_HLine ? 1 : 2;
            return kTexNodeNone;
        case TexCommand_Tag: {
            bool star = acceptCharacter(p, '*');
            TexNodeIndex tag = parseTextArgument(p, context, TexTextFont_Upright);
            TableRow* row = taggedRow(p);
            // amsmath's "\tag not allowed here", in inline math and in an environment that numbers nothing, such as
            // aligned.
            if (row == nullptr)
                return makeError(p, start, p->lastEnd, context);
            row->tag = tag;
            row->tagStar = star;
            return kTexNodeNone;
        }
        case TexCommand_NoTag: {
            TableRow* row = taggedRow(p);
            if (row != nullptr)
                row->noNumber = true;
            return kTexNodeNone;
        }
        case TexCommand_Label: {
            PrTextBuffer label{};
            readTextArgument(p, &label);
            // A label names the equation that it is in, which may be outside the environment that holds it, as an
            // aligned in an equation is.
            TableState* state = p->table;
            while (state != nullptr && !state->tagged)
                state = state->outer;
            if (state == nullptr)
                state = &p->top;
            state->labels.push(PendingLabel{state->rows.count - 1, prRawStringCopy(prTextBufferView(&label))});
            prTextBufferReset(&label);
            return kTexNodeNone;
        }
        case TexCommand_NewCommand:
        case TexCommand_RenewCommand:
        case TexCommand_ProvideCommand:
            if (!parseNewCommand(p, command))
                return makeError(p, start, p->lastEnd, context);
            return kTexNodeNone;
        case TexCommand_Def:
        case TexCommand_GDef:
            if (!parseDef(p, command == TexCommand_GDef))
                return makeError(p, start, p->lastEnd, context);
            return kTexNodeNone;
        case TexCommand_Global: {
            Token next = nextRaw(p);
            while (next.kind == TokenKind_Space)
                next = nextRaw(p);
            bool defined = false;
            if (isCommand(p, next, "def") || isCommand(p, next, "gdef") || isCommand(p, next, "edef") ||
                    isCommand(p, next, "xdef"))
                defined = parseDef(p, true);
            else if (isCommand(p, next, "let"))
                defined = parseLet(p, true);
            if (!defined)
                return makeError(p, start, p->lastEnd, context);
            return kTexNodeNone;
        }
        case TexCommand_Let:
            if (!parseLet(p, false))
                return makeError(p, start, p->lastEnd, context);
            return kTexNodeNone;
        case TexCommand_DeclareMathOperator: {
            bool star = acceptCharacter(p, '*');
            PrTextBuffer defined{};
            PrTextBuffer text{};
            if (!readDefinedName(p, &defined) || !readTextArgument(p, &text)) {
                prTextBufferReset(&defined);
                prTextBufferReset(&text);
                return makeError(p, start, p->lastEnd, context);
            }
            PrTextBuffer replacement{};
            prTextBufferAppend(&replacement, star ? "\\operatorname*{" : "\\operatorname{");
            prTextBufferAppend(&replacement, prTextBufferView(&text));
            prTextBufferAppendChar(&replacement, '}');
            define(p, prTextBufferView(&defined), 0, prTextBufferView(&replacement), nullptr, PrStringView{}, false);
            prTextBufferReset(&defined);
            prTextBufferReset(&text);
            prTextBufferReset(&replacement);
            return kTexNodeNone;
        }
        case TexCommand_Href: {
            PrTextBuffer url{};
            readTextArgument(p, &url);
            TexNodeIndex argument = requireArgument(p, parseArgument(p, context), start, context);
            TexNodeIndex link = makeRow(p, context);
            appendContents(p, link, argument);
            if (isSafeUrl(prTextBufferView(&url)))
                texNodeSetAttribute(p->tree, link, "href", prTextBufferView(&url));
            prTextBufferReset(&url);
            return link;
        }
        case TexCommand_Url: {
            PrTextBuffer url{};
            readTextArgument(p, &url);
            TexNodeIndex text = makeToken(p, TexElement_Mtext, prTextBufferView(&url), TexClass_Ord, context);
            if (isSafeUrl(prTextBufferView(&url)))
                texNodeSetAttribute(p->tree, text, "href", prTextBufferView(&url));
            texNodeSetAttribute(p->tree, text, "style", "font-family: monospace");
            prTextBufferReset(&url);
            return text;
        }
        case TexCommand_Unicode:
        case TexCommand_Char: {
            PrTextBuffer written{};
            if (command == TexCommand_Unicode) {
                readTextArgument(p, &written);
            } else {
                Token part = nextNonSpace(p);
                if (isCharacter(p, part, '`')) {
                    prTextBufferAppendChar(&written, '`');
                    part = next(p);
                    if (part.kind == TokenKind_Command)
                        prTextBufferAppendChar(&written, '\\');
                    prTextBufferAppend(&written, tokenText(p, part));
                } else {
                    while (part.kind == TokenKind_Character && (prAsciiIsHexDigit(tokenText(p, part).data[0]) ||
                            tokenText(p, part).data[0] == '"' || tokenText(p, part).data[0] == '\'')) {
                        prTextBufferAppend(&written, tokenText(p, part));
                        part = next(p);
                    }
                    if (part.kind != TokenKind_End)
                        pushBack(p, part);
                }
            }
            uint32_t codePoint = parseCharacterCode(prTextBufferView(&written));
            prTextBufferReset(&written);
            if (codePoint == 0)
                return makeError(p, start, p->lastEnd, context);
            return makeCharacterAtom(p, codePoint, context);
        }
        case TexCommand_Class:
        case TexCommand_CssId:
        case TexCommand_Style: {
            PrTextBuffer value{};
            readTextArgument(p, &value);
            TexNodeIndex argument = requireArgument(p, parseArgument(p, context), start, context);
            TexNodeIndex row = makeRow(p, context);
            appendContents(p, row, argument);
            static const char* const kNames[] = {"class", "id", "style"};
            texNodeSetAttribute(p->tree, row, kNames[command - TexCommand_Class], prTextBufferView(&value));
            prTextBufferReset(&value);
            return row;
        }
        case TexCommand_Mod:
        case TexCommand_PMod:
        case TexCommand_Pod:
            return parseMod(p, context, command, start);
        case TexCommand_BMod: {
            Context roman = context;
            roman.font = TexFont_Roman;
            TexNodeIndex mod = makeOperator(p, "mod", TexClass_Bin, roman);
            at(p, mod).flags |= TexNodeFlag_ModSpacing;
            return mod;
        }
        case TexCommand_Dots:
            return parseDots(p, context);
        case TexCommand_SubStack:
            if (!acceptCharacter(p, '{'))
                return makeError(p, start, p->lastEnd, context);
            return parseEnvironment(p, context, start, true);
        case TexCommand_MathChoice: {
            TexNodeIndex choices[4];
            for (TexNodeIndex& choice : choices) {
                choice = parseArgument(p, context);
                if (choice == kTexNodeNone)
                    return makeError(p, start, p->lastEnd, context);
            }
            return groupAtom(p, choices[context.style]);
        }
        case TexCommand_Relax:
            return kTexNodeNone;
        case TexCommand_Strut: {
            // LaTeX's \strut at a 10pt font with a 12pt baselineskip, whose height and depth are 0.7 and 0.3 of the
            // baselineskip.
            TexNodeIndex strut = makeStrut(p, 0.84f, 0.36f, context);
            at(p, strut).texClass = TexClass_Ord;
            return strut;
        }
        case TexCommand_MathStrut: {
            TexNodeIndex phantom = makeNode(p, TexElement_Mphantom, TexClass_Ord, context);
            texNodeAppend(p->tree, phantom, makeOperator(p, "(", TexClass_Open, context));
            TexNodeIndex padded = wrap(p, TexElement_Mpadded, phantom, TexClass_Ord, context);
            texNodeSetAttribute(p->tree, padded, "width", "0");
            return padded;
        }
        case TexCommand_RaiseBox: {
            PrTextBuffer raise{};
            readTextArgument(p, &raise);
            float em = 0;
            bool valid = parseDimension(prTextBufferView(&raise), &em);
            prTextBufferReset(&raise);
            TexNodeIndex argument = parseTextArgument(p, context, TexTextFont_Upright);
            if (!valid)
                return makeError(p, start, p->lastEnd, context);
            TexNodeIndex padded = makeNode(p, TexElement_Mpadded, TexClass_Ord, context);
            appendContents(p, padded, argument);
            PrTextBuffer value{};
            appendLength(&value, em, "em");
            texNodeSetAttribute(p->tree, padded, "voffset", prTextBufferView(&value));
            prTextBufferReset(&value);
            return padded;
        }
        case TexCommand_Rule: {
            PrTextBuffer raise{};
            bool raised = readOptionalText(p, &raise);
            PrTextBuffer width{};
            PrTextBuffer height{};
            readTextArgument(p, &width);
            readTextArgument(p, &height);
            float raiseEm = 0;
            float widthEm = 0;
            float heightEm = 0;
            bool valid = parseDimension(prTextBufferView(&width), &widthEm) &&
                parseDimension(prTextBufferView(&height), &heightEm) &&
                (!raised || parseDimension(prTextBufferView(&raise), &raiseEm));
            prTextBufferReset(&raise);
            prTextBufferReset(&width);
            prTextBufferReset(&height);
            if (!valid)
                return makeError(p, start, p->lastEnd, context);
            TexNodeIndex rule = makeNode(p, TexElement_Mspace, TexClass_Ord, context);
            PrTextBuffer value{};
            appendLength(&value, widthEm, "em");
            texNodeSetAttribute(p->tree, rule, "width", prTextBufferView(&value));
            prTextBufferClear(&value);
            appendLength(&value, heightEm, "em");
            texNodeSetAttribute(p->tree, rule, "height", prTextBufferView(&value));
            prTextBufferReset(&value);
            texNodeSetAttribute(p->tree, rule, "mathbackground", "currentcolor");
            if (!raised)
                return rule;
            TexNodeIndex padded = wrap(p, TexElement_Mpadded, rule, TexClass_Ord, context);
            appendLength(&value, raiseEm, "em");
            texNodeSetAttribute(p->tree, padded, "voffset", prTextBufferView(&value));
            prTextBufferReset(&value);
            return padded;
        }
        case TexCommand_SideSet:
            return parseSideSet(p, context, start);
        case TexCommand_Ensuremath: {
            TexNodeIndex argument = requireArgument(p, parseArgument(p, context), start, context);
            if (at(p, argument).element == TexElement_Mrow && at(p, argument).texClass == TexClass_Ord)
                at(p, argument).flags |= TexNodeFlag_Transparent;
            return argument;
        }
        case TexCommand_Pmb: {
            TexNodeIndex argument = requireArgument(p, parseArgument(p, context), start, context);
            TexNodeIndex bold = makeNode(p, TexElement_Mstyle, TexClass_Ord, context);
            appendContents(p, bold, argument);
            texNodeSetAttribute(p->tree, bold, "style", "font-weight: bold");
            return bold;
        }
        case TexCommand_Ref: {
            // The text is the label's tag, which a later equation of the formula may yet define.
            PrTextBuffer label{};
            readTextArgument(p, &label);
            TexNodeIndex node = makeToken(p, TexElement_Mtext, "", TexClass_Ord, context);
            p->refs.push(PendingRef{node, prRawStringCopy(prTextBufferView(&label)), name == "eqref"});
            prTextBufferReset(&label);
            return node;
        }
        case TexCommand_TextMode:
            return parseTextArgument(p, context, TexTextFont_Upright);
        case TexCommand_Count:
            break;
    }
    return makeError(p, start, token.sourceEnd, context);
}

// Parses the atom that `token` starts. Returns kTexNodeNone for a token that makes nothing, such as a definition. A
// `single` atom is one argument, as a script's is: one digit, not a number.
TexNodeIndex parseAtom(Parser* p, const Context& context, const Token& token, bool single) {
    if (token.kind == TokenKind_Character) {
        uint32_t codePoint = tokenCodePoint(p, token);
        switch (codePoint) {
            case '{': {
                TexNodeIndex row = makeRow(p, context);
                parseList(p, context, StopFlag_CloseBrace, row);
                return groupAtom(p, row);
            }
            case '}':
            case '&':
            case '#':
            case '$':
                return makeError(p, token.sourceStart, token.sourceEnd, context);
            case '^':
            case '_':
            case '\'':
                // Scripts with no base attach to an empty one.
                pushBack(p, token);
                return makeRow(p, context);
            case '~':
                return makeMuSpace(p, 6, context);
            default:
                break;
        }
        if (codePoint < 0x80 && (prAsciiIsDigit(char(codePoint)) || codePoint == '.')) {
            if (codePoint == '.') {
                Token after = next(p);
                bool digit = after.kind == TokenKind_Character && after.size == 1 &&
                    prAsciiIsDigit(tokenText(p, after).data[0]);
                if (after.kind != TokenKind_End)
                    pushBack(p, after);
                if (!digit || single)
                    return makeCharacterAtom(p, codePoint, context);
            }
            return parseNumber(p, context, token, single);
        }
        // A character that is not a symbol is a letter, as an accented letter written as itself is.
        bool letter = prAsciiIsLetter(char(codePoint));
        if (codePoint >= 0x80)
            letter = texEntryFromCodePoint(codePoint) == nullptr && codePoint != 0x2212;
        if (letter)
            return parseLetters(p, context, codePoint, single);
        return makeCharacterAtom(p, codePoint, context);
    }
    if (token.kind != TokenKind_Command)
        return kTexNodeNone;
    PrStringView name = tokenText(p, token);
    const TexEntry* entry = texEntryFromName(name);
    if (entry == nullptr)
        return makeError(p, token.sourceStart, token.sourceEnd, context);
    switch (entry->kind) {
        case TexEntryKind_Letter:
        case TexEntryKind_Upright:
        case TexEntryKind_Operator:
        case TexEntryKind_LargeOperator:
        case TexEntryKind_Integral:
        case TexEntryKind_Function:
        case TexEntryKind_LimitFunction:
        case TexEntryKind_TextSymbol:
            return makeSymbol(p, *entry, context);
        case TexEntryKind_Accent: {
            TexNodeIndex base = requireArgument(p, parseArgument(p, context), token.sourceStart, context);
            return makeAccent(p, base, entry->text, TexAccentFlags(entry->arg), context);
        }
        case TexEntryKind_TextAccent:
            return makeError(p, token.sourceStart, token.sourceEnd, context);
        case TexEntryKind_Space:
            return makeMuSpace(p, entry->arg, context);
        case TexEntryKind_Font: {
            Context fontContext = context;
            fontContext.font = TexFont(entry->arg);
            TexNodeIndex argument = requireArgument(p, parseArgument(p, fontContext), token.sourceStart, context);
            // \boldsymbol keeps its argument's class, and the other alphabets make an Ord, as braces do.
            if (entry->arg == TexFont_BoldItalic && argument != kTexNodeNone)
                return argument;
            return groupAtom(p, argument);
        }
        case TexEntryKind_FontSwitch:
        case TexEntryKind_Style:
        case TexEntryKind_Size:
            // Declarations, which parseList handles. One that is an argument by itself sets nothing.
            return kTexNodeNone;
        case TexEntryKind_TextFont:
            return parseTextArgument(p, context, TexTextFont(entry->arg));
        case TexEntryKind_BigDelimiter: {
            PrTextBuffer delimiter{};
            if (!readDelimiter(p, &delimiter)) {
                prTextBufferReset(&delimiter);
                return makeError(p, token.sourceStart, p->lastEnd, context);
            }
            if (delimiter.size == 0) {
                prTextBufferReset(&delimiter);
                return makeNullDelimiter(p, context);
            }
            TexNodeIndex fence = makeFence(p, prTextBufferView(&delimiter), TexClass(entry->arg >> 4), context);
            // The sizes of Computer Modern's delimiter variants, which \big through \Bigg select.
            static const char* const kSizes[] = {"1.2em", "1.8em", "2.4em", "3em"};
            setFenceSize(p, fence, kSizes[(entry->arg & 0xf) - 1]);
            prTextBufferReset(&delimiter);
            return fence;
        }
        case TexEntryKind_ExtensibleArrow:
            return parseExtensibleArrow(p, context, entry->text);
        case TexEntryKind_Macro:
            // A macro that did not expand, because the formula expanded too many.
            return makeError(p, token.sourceStart, token.sourceEnd, context);
        case TexEntryKind_Command:
            return parseCommand(p, context, token, TexCommand(entry->arg));
    }
    return kTexNodeNone;
}

// Makes the prime marks of `count` primes.
TexNodeIndex makePrimes(Parser* p, uint32_t count, const Context& context) {
    PrTextBuffer text{};
    static const char* const kPrimes[] = {"′", "″", "‴", "⁗"};
    while (count > 4) {
        prTextBufferAppend(&text, kPrimes[3]);
        count -= 4;
    }
    prTextBufferAppend(&text, kPrimes[count - 1]);
    TexNodeIndex node = makeToken(p, TexElement_Mo, prTextBufferView(&text), TexClass_Ord, context);
    prTextBufferReset(&text);
    return node;
}

// Parses the superscripts, subscripts, primes and limit controls after `base`.
TexNodeIndex parseScripts(Parser* p, const Context& context, TexNodeIndex base) {
    Context scripts = context;
    scripts.style = scriptStyle(context.style);
    TexNodeIndex sub = kTexNodeNone;
    TexNodeIndex sup = kTexNodeNone;
    TexNodeIndex primes = kTexNodeNone;
    // The limit control after an Op: 0 for none, 1 for \limits, 2 for \nolimits and 3 for \displaylimits.
    uint32_t control = 0;
    for (;;) {
        Token token = nextNonSpace(p);
        if (token.kind == TokenKind_End)
            break;
        bool isSup = isCharacter(p, token, '^');
        bool isSub = isCharacter(p, token, '_');
        if (isSup || isSub) {
            uint32_t start = token.sourceStart;
            TexNodeIndex script = requireArgument(p, parseArgument(p, scripts), start, scripts);
            TexNodeIndex& slot = isSup ? sup : sub;
            if (slot != kTexNodeNone) {
                // TeX's "Double superscript" or "Double subscript".
                TexNodeIndex error = makeError(p, start, p->lastEnd, scripts);
                TexNodeIndex row = makeRow(p, scripts);
                appendContents(p, row, slot);
                texNodeAppend(p->tree, row, error);
                slot = row;
                continue;
            }
            slot = script;
            continue;
        }
        if (isCharacter(p, token, '\'')) {
            uint32_t start = token.sourceStart;
            uint32_t count = 1;
            for (;;) {
                Token after = next(p);
                if (isCharacter(p, after, '\'')) {
                    ++count;
                    continue;
                }
                if (after.kind != TokenKind_End)
                    pushBack(p, after);
                break;
            }
            if (primes == kTexNodeNone && sup == kTexNodeNone) {
                primes = makePrimes(p, count, scripts);
                continue;
            }
            // Primes are a superscript, so primes after primes or a superscript are TeX's "Double superscript".
            TexNodeIndex error = makeError(p, start, p->lastEnd, scripts);
            TexNodeIndex row = makeRow(p, scripts);
            if (sup != kTexNodeNone)
                appendContents(p, row, sup);
            texNodeAppend(p->tree, row, error);
            sup = row;
            continue;
        }
        if (token.kind == TokenKind_Command && at(p, base).texClass == TexClass_Op) {
            PrStringView name = tokenText(p, token);
            if (name == "limits") {
                control = 1;
                continue;
            }
            if (name == "nolimits") {
                control = 2;
                continue;
            }
            if (name == "displaylimits") {
                control = 3;
                continue;
            }
        }
        pushBack(p, token);
        break;
    }
    // Primes come first in the superscript, as TeX makes x'^2 the superscript ′2.
    if (primes != kTexNodeNone) {
        if (sup == kTexNodeNone) {
            sup = primes;
        } else {
            TexNodeIndex row = makeRow(p, scripts);
            texNodeAppend(p->tree, row, primes);
            appendContents(p, row, sup);
            sup = row;
        }
    }
    TexNode& b = at(p, base);
    TexNodeIndex core = texNodeIsEmbellished(p->tree, base) ? texNodeCore(p->tree, base) : kTexNodeNone;
    if (control != 0 && core != kTexNodeNone) {
        TexNode& c = at(p, core);
        if (control == 1)
            c.movablelimits = TexTri_False;
        else if (control == 3)
            c.movablelimits = TexTri_True;
    }
    if (sub == kTexNodeNone && sup == kTexNodeNone)
        return base;
    bool limits = false;
    if (b.texClass == TexClass_Op) {
        bool display = (b.flags & TexNodeFlag_DisplayLimits) != 0;
        bool always = (b.flags & TexNodeFlag_Limits) != 0;
        if (control == 1)
            always = true;
        else if (control == 2)
            always = display = false;
        else if (control == 3)
            display = true;
        if (always) {
            limits = true;
            if (core != kTexNodeNone)
                at(p, core).movablelimits = TexTri_False;
        } else if (display) {
            // MathML's movablelimits moves an operator's limits to scripts outside display style during layout. Any
            // other limits are placed by the style that they are parsed in.
            limits = core != kTexNodeNone || context.style == TexStyle_Display;
            if (core != kTexNodeNone)
                at(p, core).movablelimits = TexTri_True;
        }
    }
    TexElement element = limits ? TexElement_Munderover : TexElement_Msubsup;
    if (sub == kTexNodeNone)
        element = limits ? TexElement_Mover : TexElement_Msup;
    else if (sup == kTexNodeNone)
        element = limits ? TexElement_Munder : TexElement_Msub;
    TexNodeIndex node = makeNode(p, element, at(p, base).texClass, context);
    // The scripts carry the base's function application, as \sin^2 x applies \sin^2 to x.
    TexNodeFlags carried = at(p, base).flags & TexNodeFlag_Function;
    at(p, node).flags |= carried;
    at(p, base).flags &= TexNodeFlags(~TexNodeFlag_Function);
    texNodeAppend(p->tree, node, base);
    if (sub != kTexNodeNone)
        texNodeAppend(p->tree, node, sub);
    if (sup != kTexNodeNone)
        texNodeAppend(p->tree, node, sup);
    return node;
}

// The kinds of infix fraction, which \over and its kin make of the lists on either side.
struct InfixFraction {
    TexCommand command;
    float thickness;                // for \above, in em
    uint32_t start;
};

Stop parseList(Parser* p, Context context, StopFlags stops, TexNodeIndex target) {
    if (++p->depth > kMaxDepth) {
        // A formula nested this deeply is an error from here to its end.
        texNodeAppend(p->tree, target, makeError(p, p->lastEnd, uint32_t(p->source.size), context));
        p->position = uint32_t(p->source.size);
        p->pending.clear();
        --p->depth;
        return Stop_End;
    }
    InfixFraction infix{};
    bool hasInfix = false;
    TexNodeIndex numerator = kTexNodeNone;
    Stop result = Stop_End;
    TexStyle listStyle = context.style;
    // A declaration's <mstyle> holds the rest of the list, so each atom goes into the last one's, and the declarations
    // are those <mstyle>s, outermost first.
    TexNodeIndex into = target;
    RawArray<TexNodeIndex> declarations{};
    for (;;) {
        Token token = next(p);
        if (token.kind == TokenKind_Space)
            continue;
        if (token.kind == TokenKind_End)
            break;
        Stop stop = Stop_End;
        if (isStop(p, token, stops, &stop)) {
            result = stop;
            break;
        }
        if (token.kind == TokenKind_Command) {
            const TexEntry* entry = texEntryFromName(tokenText(p, token));
            if (entry != nullptr && entry->kind == TexEntryKind_FontSwitch) {
                context.font = TexFont(entry->arg);
                continue;
            }
            if (entry != nullptr && (entry->kind == TexEntryKind_Style || entry->kind == TexEntryKind_Size ||
                    (entry->kind == TexEntryKind_Command && entry->arg == TexCommand_Color))) {
                // A declaration sets the rest of the list, whose atoms stay atoms of this list.
                TexNodeIndex mstyle = makeNode(p, TexElement_Mstyle, TexClass_Ord, context);
                at(p, mstyle).flags |= TexNodeFlag_Transparent;
                if (entry->kind == TexEntryKind_Style) {
                    context.style = TexStyle(entry->arg);
                    texNodeSetAttribute(p->tree, mstyle, "displaystyle", context.style == TexStyle_Display ? "true" : "false");
                    static const char* const kLevels[] = {"0", "1", "2"};
                    texNodeSetAttribute(p->tree, mstyle, "scriptlevel", kLevels[styleLevel(context.style)]);
                } else if (entry->kind == TexEntryKind_Size) {
                    PrTextBuffer size{};
                    appendLength(&size, float(entry->arg) / 100, "em");
                    texNodeSetAttribute(p->tree, mstyle, "mathsize", prTextBufferView(&size));
                    prTextBufferReset(&size);
                } else {
                    PrTextBuffer color{};
                    if (readColor(p, &color))
                        texNodeSetAttribute(p->tree, mstyle, "mathcolor", prTextBufferView(&color));
                    else
                        texNodeAppend(p->tree, into, makeError(p, token.sourceStart, p->lastEnd, context));
                    prTextBufferReset(&color);
                }
                at(p, mstyle).style = context.style;
                texNodeAppend(p->tree, into, mstyle);
                into = mstyle;
                declarations.push(mstyle);
                continue;
            }
            if (entry != nullptr && entry->kind == TexEntryKind_Command && !hasInfix &&
                    entry->arg >= TexCommand_Over && entry->arg <= TexCommand_Above) {
                hasInfix = true;
                infix.command = TexCommand(entry->arg);
                infix.start = token.sourceStart;
                if (infix.command == TexCommand_Above && !readDimension(p, &infix.thickness))
                    infix.thickness = 0.04f;
                // The numerator is the list so far, declarations and all. A style is the numerator's alone, as TeX sets
                // the denominator in the fraction's style, but a color or size lasts to the end of the group.
                context.style = listStyle;
                numerator = makeRow(p, context);
                texNodeMoveChildren(p->tree, target, numerator);
                into = target;
                for (uint32_t d = 0; d < declarations.count; ++d) {
                    TexNodeIndex declaration = declarations.data[d];
                    if (texNodeHasAttribute(p->tree, declaration, "displaystyle"))
                        continue;
                    TexNodeIndex mstyle = makeNode(p, TexElement_Mstyle, TexClass_Ord, context);
                    at(p, mstyle).flags |= TexNodeFlag_Transparent;
                    texNodeCopyAttributes(p->tree, declaration, mstyle);
                    texNodeAppend(p->tree, into, mstyle);
                    into = mstyle;
                }
                // A list has one infix fraction, so what follows declares nothing that another would need.
                declarations.clear();
                continue;
            }
        }
        TexNodeIndex atom = parseAtom(p, context, token, false);
        if (atom == kTexNodeNone)
            continue;
        texNodeAppend(p->tree, into, parseScripts(p, context, atom));
    }
    declarations.reset();
    context.style = listStyle;
    if (hasInfix) {
        TexNodeIndex denominator = makeRow(p, context);
        texNodeMoveChildren(p->tree, target, denominator);
        TexStyle parts = fractionStyle(context.style);
        restyle(p, numerator, context.style, parts);
        restyle(p, denominator, context.style, parts);
        PrTextBuffer thickness{};
        PrStringView thicknessValue{};
        if (infix.command == TexCommand_Above) {
            appendLength(&thickness, infix.thickness, "em");
            thicknessValue = prTextBufferView(&thickness);
        } else if (infix.command != TexCommand_Over) {
            thicknessValue = "0";
        }
        PrStringView left = "";
        PrStringView right = "";
        if (infix.command == TexCommand_Choose) {
            left = "(";
            right = ")";
        } else if (infix.command == TexCommand_Brack) {
            left = "[";
            right = "]";
        } else if (infix.command == TexCommand_Brace) {
            left = "{";
            right = "}";
        }
        TexNodeIndex fraction = makeFraction(p, collapse(p, numerator), collapse(p, denominator), thicknessValue, left,
            right, context.style, context);
        texNodeAppend(p->tree, target, fraction);
        prTextBufferReset(&thickness);
    }
    --p->depth;
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------
// Environments

// How a table's columns are set.
enum {
    ColumnScheme_Center,            // every column centered
    ColumnScheme_Alignment,         // amsmath's alignments: columns right and left in turn
    ColumnScheme_Specified,         // as a column specification says, as array's is
    ColumnScheme_Cases,             // left aligned, a quad after the first
    ColumnScheme_Multline,          // the first row left, the last right, the rest centered
    ColumnScheme_Eqnarray,          // right, center and left
};
typedef uint8_t ColumnScheme;

// A column of a column specification.
struct SpecifiedColumn {
    char align;                     // 'l', 'c' or 'r'
    uint8_t lineBefore;             // 0 for none, 1 for | and 2 for :
    bool padBefore;                 // false after @{}
    bool padAfter;
};

// How an environment numbers its rows.
enum {
    Numbering_None,
    Numbering_EachRow,              // each row that \notag does not exempt, as align numbers its rows
    Numbering_Last,                 // the last row, once, as multline numbers its equation
};
typedef uint8_t Numbering;

// How an alignment's pairs of columns share the width of the line.
enum {
    Spread_None,                    // the pairs are as close as their gap allows, and centered
    Spread_Pairs,                   // the first pair is flush left, the last flush right, as flalign sets them
    Spread_Even,                    // the same space is before, between and after the pairs, as align sets them
};
typedef uint8_t Spread;

// How an environment lays out its table.
struct TableLayout {
    ColumnScheme scheme;
    float columnPadding;            // the space on each side of a column, in em
    bool outerPadding;              // true if the first and last columns are padded on the outside too
    float pairGap;                  // for an alignment, the space between pairs of columns, in em
    float strutHeight;              // the strut of each row, in em, or 0 for none
    float strutDepth;
    float rowGap;                   // the space between rows, in em
    TexStyle cellStyle;
    bool tagged;                    // true for an environment that numbers equations, whose rows take \tag
    const char* left;               // the delimiters around the table, as \left and \right set them, or null
    const char* right;
    Numbering numbering;

    Spread spread;
};

// The most columns that a column specification makes, which bounds what *{count}{columns} repeats.
constexpr uint32_t kMaxSpecifiedColumns = 1024;

// The columns that a column specification has made, and what applies to the next one.
struct ColumnSpec {
    RawArray<SpecifiedColumn>* columns;
    uint8_t line;                   // the rule before the next column: 0 for none, 1 for | and 2 for :
    bool pad;                       // false after @{}
};

// Parses a column specification of l, c and r, p{width} and its kin as l, | and : between columns, @{} to drop the
// padding beside it, and *{count}{columns}, which makes its columns as though they were written out `count` times.
// `depth` is the nesting of *{count}{columns}.
void parseColumnSpec(PrStringView spec, ColumnSpec* state, uint32_t depth) {
    RawArray<SpecifiedColumn>* columns = state->columns;
    for (size_t i = 0; i < spec.size; ++i) {
        char c = spec.data[i];
        switch (c) {
            case 'l':
            case 'c':
            case 'r':
            case 'p':
            case 'm':
            case 'b':
            case 'X': {
                if (columns->count < kMaxSpecifiedColumns) {
                    columns->push(SpecifiedColumn{
                        .align = c == 'c' || c == 'r' ? c : 'l',
                        .lineBefore = state->line,
                        .padBefore = state->pad,
                        .padAfter = true,
                    });
                }
                state->line = 0;
                state->pad = true;
                if (c == 'p' || c == 'm' || c == 'b') {
                    // The width in braces.
                    while (i + 1 < spec.size && spec.data[i + 1] != '}')
                        ++i;
                    ++i;
                }
                break;
            }
            case '|':
                state->line = 1;
                break;
            case ':':
                state->line = 2;
                break;
            case '@': {
                // @{text} replaces the padding with the text, which is dropped.
                if (columns->count != 0)
                    columns->last().padAfter = false;
                state->pad = false;
                uint32_t braces = 0;
                for (++i; i < spec.size; ++i) {
                    if (spec.data[i] == '{')
                        ++braces;
                    else if (spec.data[i] == '}' && --braces == 0)
                        break;
                }
                break;
            }
            case '*': {
                // *{count}{columns}.
                size_t open = prStringFindByte(prStringSlice(spec, i, spec.size - i), '{') + i;
                size_t close = spec.size;
                if (open < spec.size)
                    close = prStringFindByte(prStringSlice(spec, open, spec.size - open), '}') + open;
                uint32_t count = 0;
                for (size_t d = open + 1; d < close && d < spec.size; ++d) {
                    if (prAsciiIsDigit(spec.data[d]) && count < kMaxSpecifiedColumns)
                        count = count * 10 + uint32_t(spec.data[d] - '0');
                }
                size_t bodyOpen = close + 1;
                size_t bodyClose = bodyOpen;
                uint32_t braces = 0;
                for (; bodyClose < spec.size; ++bodyClose) {
                    if (spec.data[bodyClose] == '{')
                        ++braces;
                    else if (spec.data[bodyClose] == '}' && --braces == 0)
                        break;
                }
                if (bodyOpen < spec.size && depth < kMaxDepth) {
                    PrStringView body = prStringSlice(spec, bodyOpen + 1, bodyClose - bodyOpen - 1);
                    // A repetition that makes no column changes nothing that the first did not, so the repetitions stop
                    // there, and at the most columns.
                    for (uint32_t n = 0; n < count && columns->count < kMaxSpecifiedColumns; ++n) {
                        uint32_t before = columns->count;
                        parseColumnSpec(body, state, depth + 1);
                        if (columns->count == before)
                            break;
                    }
                }
                i = bodyClose;
                break;
            }
            default:
                break;
        }
    }
}

// Parses an environment's rows, to its \end, into `table`.
void parseRows(Parser* p, const Context& cellContext, TableState* table, bool group) {
    TableState* outer = p->table;
    table->outer = outer;
    p->table = table;
    StopFlags stops = StopFlag_Align | StopFlag_Newline | (group ? StopFlag_CloseBrace : StopFlag_EndEnvironment);
    table->rows.push(TableRow{.firstCell = 0, .tag = kTexNodeNone});
    for (;;) {
        TexNodeIndex cell = makeRow(p, cellContext);
        Stop stop = parseList(p, cellContext, stops, cell);
        table->cells.push(cell);
        ++table->rows.last().cellCount;
        if (stop == Stop_Align)
            continue;
        if (stop == Stop_Newline) {
            PrTextBuffer gap{};
            float em = 0;
            if (readOptionalText(p, &gap) && parseDimension(prTextBufferView(&gap), &em))
                table->rows.last().gap = em;
            prTextBufferReset(&gap);
            table->rows.push(TableRow{.firstCell = table->cells.count, .tag = kTexNodeNone});
            continue;
        }
        if (stop == Stop_EndEnvironment) {
            PrTextBuffer name{};
            readTextArgument(p, &name);
            prTextBufferReset(&name);
        }
        break;
    }
    // A last row with one empty cell is what a final \\ leaves, and a rule above it is a rule below the row before.
    TableRow& last = table->rows.last();
    if (table->rows.count > 1 && last.cellCount == 1 && last.tag == kTexNodeNone && !last.noNumber &&
            p->tree->nodes.data[table->cells.data[last.firstCell]].firstChild == kTexNodeNone) {
        uint8_t line = last.lineAbove;
        table->rows.pop();
        table->cells.pop();
        table->rows.last().lineBelow = line;
        for (uint32_t i = 0; i < table->labels.count; ++i) {
            if (table->labels.data[i].row == table->rows.count)
                --table->labels.data[i].row;
        }
    }
    p->table = outer;
}

// The names that a table's attributes write for a line, where 0 is none, 1 is solid and 2 is dashed.
constexpr const char* kLineNames[] = {"none", "solid", "dashed"};

// A list that a table attribute holds, one item for each row, column or gap. A list whose items are all the same is
// written as one item, since MathML repeats a list's last item.
struct AttributeList {
    PrTextBuffer text;
    uint32_t firstSize;             // the size of the first item, which starts the text
    uint32_t count;
    bool same;
};

void listAdd(AttributeList* list, PrStringView item) {
    if (list->count == 0) {
        list->firstSize = uint32_t(item.size);
        list->same = true;
    } else {
        list->same = list->same && item == PrStringView{list->text.data, list->firstSize};
        prTextBufferAppendChar(&list->text, ' ');
    }
    prTextBufferAppend(&list->text, item);
    ++list->count;
}

void listAddEm(AttributeList* list, float em) {
    PrTextBuffer item{};
    appendLength(&item, em, em == 0 ? "" : "em");
    listAdd(list, prTextBufferView(&item));
    prTextBufferReset(&item);
}

// Sets the attribute `name` to the list, unless the list is empty or is only `skip`, and empties the list.
void setList(Parser* p, TexNodeIndex node, PrStringView name, AttributeList* list, PrStringView skip) {
    if (list->count != 0) {
        PrStringView value = prTextBufferView(&list->text);
        if (list->same)
            value = prStringSlice(value, 0, list->firstSize);
        if (!(list->same && value == skip))
            texNodeSetAttribute(p->tree, node, name, value);
    }
    prTextBufferClear(&list->text);
    list->count = 0;
}

// Appends the characters of the node's tokens, in order.
void appendTextContent(const TexTree* tree, TexNodeIndex node, PrTextBuffer* out) {
    const TexNode& n = tree->nodes.data[node];
    switch (n.element) {
        case TexElement_Mi:
        case TexElement_Mn:
        case TexElement_Mo:
        case TexElement_Mtext:
            prTextBufferAppend(out, texNodeText(tree, node));
            return;
        default:
            break;
    }
    for (TexNodeIndex child = n.firstChild; child != kTexNodeNone; child = tree->nodes.data[child].next)
        appendTextContent(tree, child, out);
}

// Numbers the rows of a table that numbers equations in display math. Each row takes its \tag, or the next number of
// the document's counter if `numbering` numbers it and \notag does not exempt it. Each label then names the tag of its
// row. A label in a row without a tag names nothing.
void numberRows(Parser* p, TableState* table, Numbering numbering) {
    bool numbers = table->tagged && p->display == PrTexDisplay_Block && table->rows.count != 0;
    if (numbers && numbering == Numbering_Last) {
        // multline's one number goes on its last line. A \tag on any line moves there, and a \notag on any line drops
        // the number.
        TableRow& last = table->rows.last();
        for (uint32_t r = 0; r + 1 < table->rows.count; ++r) {
            TableRow& row = table->rows.data[r];
            if (last.tag == kTexNodeNone && row.tag != kTexNodeNone) {
                last.tag = row.tag;
                last.tagStar = row.tagStar;
            }
            last.noNumber = last.noNumber || row.noNumber;
            row.tag = kTexNodeNone;
        }
        for (uint32_t i = 0; i < table->labels.count; ++i)
            table->labels.data[i].row = table->rows.count - 1;
    }
    for (uint32_t r = 0; numbers && r < table->rows.count; ++r) {
        TableRow& row = table->rows.data[r];
        bool counted = numbering == Numbering_EachRow || (numbering == Numbering_Last && r + 1 == table->rows.count);
        if (row.tag != kTexNodeNone || !counted || row.noNumber)
            continue;
        PrTextBuffer number{};
        prTextBufferAppendU64(&number, ++p->document->equationCount, 10);
        Context context{};
        context.style = p->tree->nodes.data[table->cells.data[row.firstCell]].style;
        row.tag = makeToken(p, TexElement_Mtext, prTextBufferView(&number), TexClass_Ord, context);
        prTextBufferReset(&number);
    }
    for (uint32_t i = 0; i < table->labels.count; ++i) {
        const PendingLabel& label = table->labels.data[i];
        if (numbers && label.row < table->rows.count && table->rows.data[label.row].tag != kTexNodeNone) {
            PrTextBuffer tag{};
            appendTextContent(p->tree, table->rows.data[label.row].tag, &tag);
            texDocumentSetLabel(p->document, prRawStringView(&label.name), prTextBufferView(&tag));
            prTextBufferReset(&tag);
        }
    }
}

// Frees a table's state.
void resetTable(TableState* table) {
    for (uint32_t i = 0; i < table->labels.count; ++i)
        prRawStringReset(&table->labels.data[i].name);
    table->labels.reset();
    table->cells.reset();
    table->rows.reset();
}

// A column of a table as it is written: a column of the source, or a spacer that takes a share of the line's width.
struct TableColumn {
    uint32_t source;                // a position in the rows' cells, or kTexNodeNone for a spacer
    char align;                     // 'l', 'c' or 'r'
    float padLeft;                  // in em
    float padRight;
    uint8_t lineBefore;             // the line between the column and the one before it, as a TableRow's lines
};

// Builds the <mtable> of the parsed rows. MathML 4's table attributes carry TeX's layout, which includes the alignment
// of the columns, the space between columns and rows, the rules, and the tags as row labels (mathml4/#presm_mtable).
TexNodeIndex buildTable(Parser* p, const Context& context, const TableState& table, const TableLayout& layout,
        const RawArray<SpecifiedColumn>& specified, uint8_t specifiedLineAfter) {
    TexNodeIndex mtable = makeNode(p, TexElement_Mtable, TexClass_Ord, context);
    Context cellContext = context;
    cellContext.style = layout.cellStyle;
    if (layout.cellStyle == TexStyle_Display) {
        texNodeSetAttribute(p->tree, mtable, "displaystyle", "true");
    } else if (layout.cellStyle >= TexStyle_Script) {
        texNodeSetAttribute(p->tree, mtable, "displaystyle", "false");
        texNodeSetAttribute(p->tree, mtable, "scriptlevel", layout.cellStyle == TexStyle_Script ? "1" : "2");
    }
    uint32_t columnCount = 0;
    bool hasTags = false;
    for (uint32_t r = 0; r < table.rows.count; ++r) {
        if (table.rows.data[r].cellCount > columnCount)
            columnCount = table.rows.data[r].cellCount;
        hasTags = hasTags || table.rows.data[r].tag != kTexNodeNone;
    }
    if (layout.scheme == ColumnScheme_Specified && specified.count > columnCount)
        columnCount = specified.count;
    bool tags = hasTags && layout.tagged && p->display == PrTexDisplay_Block;

    // A table of tags is as wide as the line, and its columns are centered in it between two empty columns that share
    // what they leave, as amsmath centers the equations and sets the tags at the margin. In display math, align's pairs
    // of columns share the line's width evenly, and flalign's reach both margins.
    Spread spread = p->display == PrTexDisplay_Block ? layout.spread : Spread_None;
    bool centered = (tags && spread == Spread_None && columnCount > 1) || spread == Spread_Even;
    RawArray<TableColumn> columns{};
    if (centered)
        columns.push(TableColumn{.source = kTexNodeNone, .align = 'c'});
    for (uint32_t c = 0; c < columnCount; ++c) {
        TableColumn column{
            .source = c,
            .align = 'c',
            .padLeft = layout.columnPadding,
            .padRight = layout.columnPadding,
        };
        switch (layout.scheme) {
            case ColumnScheme_Center:
            case ColumnScheme_Multline:
                break;
            case ColumnScheme_Alignment:
                column.align = c % 2 == 0 ? 'r' : 'l';
                column.padLeft = c % 2 == 0 && c != 0 ? layout.pairGap : 0;
                column.padRight = 0;
                break;
            case ColumnScheme_Specified:
                if (c < specified.count) {
                    const SpecifiedColumn& written = specified.data[c];
                    column.align = written.align;
                    column.lineBefore = written.lineBefore;
                    if (!written.padBefore)
                        column.padLeft = 0;
                    if (!written.padAfter || (c + 1 < specified.count && !specified.data[c + 1].padBefore))
                        column.padRight = 0;
                }
                break;
            case ColumnScheme_Cases:
                column.align = 'l';
                column.padLeft = 0;
                column.padRight = c == 0 ? 1.0f : 0;
                break;
            case ColumnScheme_Eqnarray:
                column.align = c == 0 ? 'r' : c == 1 ? 'c' : 'l';
                column.padLeft = c == 1 ? layout.columnPadding : 0;
                column.padRight = c == 1 ? layout.columnPadding : 0;
                break;
        }
        if (!layout.outerPadding) {
            if (c == 0)
                column.padLeft = 0;
            if (c + 1 == columnCount)
                column.padRight = 0;
        }
        columns.push(column);
        // Spread pairs of columns share the line's width between them, and flalign's one pair is flush left.
        bool between = c + 1 < columnCount || (spread == Spread_Pairs && columnCount == 2);
        if (spread != Spread_None && c % 2 == 1 && between)
            columns.push(TableColumn{.source = kTexNodeNone, .align = 'c'});
    }
    if (centered)
        columns.push(TableColumn{.source = kTexNodeNone, .align = 'c'});

    AttributeList list{};
    bool aligned = false;
    for (uint32_t i = 0; i < columns.count; ++i) {
        char align = columns.data[i].align;
        aligned = aligned || align != 'c';
        listAdd(&list, align == 'l' ? "left" : align == 'r' ? "right" : "center");
    }
    setList(p, mtable, "columnalign", &list, aligned ? "" : "center");
    for (uint32_t i = 0; i + 1 < columns.count; ++i)
        listAddEm(&list, columns.data[i].padRight + columns.data[i + 1].padLeft);
    setList(p, mtable, "columnspacing", &list, "");
    for (uint32_t r = 0; r + 1 < table.rows.count; ++r)
        listAddEm(&list, layout.rowGap + table.rows.data[r].gap);
    setList(p, mtable, "rowspacing", &list, "");
    // A table of one cell states its spacing too, so that it takes MathML 4's model of spacing, which has none outside
    // the outer cells, rather than MathML Core's padding around each cell.
    if (!texNodeHasAttribute(p->tree, mtable, "columnspacing") && !texNodeHasAttribute(p->tree, mtable, "rowspacing"))
        texNodeSetAttribute(p->tree, mtable, "columnspacing", "0");
    for (uint32_t r = 0; r + 1 < table.rows.count; ++r) {
        uint8_t line = table.rows.data[r + 1].lineAbove;
        if (table.rows.data[r].lineBelow > line)
            line = table.rows.data[r].lineBelow;
        listAdd(&list, kLineNames[line]);
    }
    setList(p, mtable, "rowlines", &list, "none");
    for (uint32_t i = 0; i + 1 < columns.count; ++i)
        listAdd(&list, kLineNames[columns.data[i + 1].lineBefore]);
    setList(p, mtable, "columnlines", &list, "none");
    if (spread != Spread_None || centered) {
        for (uint32_t i = 0; i < columns.count; ++i)
            listAdd(&list, columns.data[i].source == kTexNodeNone ? "fit" : "auto");
        setList(p, mtable, "columnwidth", &list, "");
    }
    // A tag sits at the right margin, and flalign's columns reach both margins.
    if (tags || spread != Spread_None)
        texNodeSetAttribute(p->tree, mtable, "width", "100%");

    for (uint32_t r = 0; r < table.rows.count; ++r) {
        const TableRow& row = table.rows.data[r];
        bool labeled = tags && row.tag != kTexNodeNone;
        TexNodeIndex mtr = makeNode(p, labeled ? TexElement_Mlabeledtr : TexElement_Mtr, TexClass_None, cellContext);
        if (layout.scheme == ColumnScheme_Multline && table.rows.count > 1) {
            if (r == 0)
                texNodeSetAttribute(p->tree, mtr, "columnalign", "left");
            else if (r + 1 == table.rows.count)
                texNodeSetAttribute(p->tree, mtr, "columnalign", "right");
        }
        if (labeled) {
            TexNodeIndex label = makeNode(p, TexElement_Mtd, TexClass_None, cellContext);
            if (at(p, row.tag).element == TexElement_Mtext) {
                PrRawString text = prRawStringCopy(row.tagStar ? "" : "(");
                prRawStringAppend(&text, texNodeText(p->tree, row.tag));
                prRawStringAppend(&text, row.tagStar ? "" : ")");
                texNodeSetText(p->tree, row.tag, prRawStringView(&text));
                prRawStringReset(&text);
                texNodeAppend(p->tree, label, row.tag);
            } else {
                TexNodeIndex tagRow = makeRow(p, cellContext);
                if (!row.tagStar)
                    texNodeAppend(p->tree, tagRow, makeToken(p, TexElement_Mtext, "(", TexClass_Ord, cellContext));
                appendContents(p, tagRow, row.tag);
                if (!row.tagStar)
                    texNodeAppend(p->tree, tagRow, makeToken(p, TexElement_Mtext, ")", TexClass_Ord, cellContext));
                texNodeAppend(p->tree, label, tagRow);
            }
            texNodeAppend(p->tree, mtr, label);
        }
        for (uint32_t i = 0; i < columns.count; ++i) {
            TexNodeIndex mtd = makeNode(p, TexElement_Mtd, TexClass_None, cellContext);
            uint32_t c = columns.data[i].source;
            if (c == 0 && layout.strutHeight != 0)
                texNodeAppend(p->tree, mtd, makeStrut(p, layout.strutHeight, layout.strutDepth, cellContext));
            if (c != kTexNodeNone && c < row.cellCount) {
                TexNodeIndex cell = table.cells.data[row.firstCell + c];
                // amsmath sets an alignment's even columns after an empty Ord, which spaces a relation that starts
                // them.
                bool leadingOrd = layout.scheme == ColumnScheme_Alignment && c % 2 == 1;
                if (leadingOrd)
                    at(p, cell).flags |= TexNodeFlag_LeadingOrd;
                // A cell of one node needs no row, unless the row starts after amsmath's empty Ord.
                TexNodeIndex content = cell;
                if (!leadingOrd)
                    content = collapse(p, cell);
                if (at(p, content).element != TexElement_Mrow || at(p, content).firstChild != kTexNodeNone)
                    texNodeAppend(p->tree, mtd, content);
            }
            texNodeAppend(p->tree, mtr, mtd);
        }
        texNodeAppend(p->tree, mtable, mtr);
    }

    // The rules around the table are a frame if every side has the same rule, and otherwise an menclose of the sides
    // that have one. The padding outside the first and last columns, as an array's \arraycolsep, is the frame's spacing
    // or an mpadded's.
    uint8_t top = table.rows.count != 0 ? table.rows.data[0].lineAbove : 0;
    uint8_t bottom = table.rows.count != 0 ? table.rows.last().lineBelow : 0;
    uint8_t left = columns.count != 0 ? columns.data[0].lineBefore : 0;
    uint8_t right = layout.scheme == ColumnScheme_Specified ? specifiedLineAfter : 0;
    float padLeft = columns.count != 0 ? columns.data[0].padLeft : 0;
    float padRight = columns.count != 0 ? columns.last().padRight : 0;
    TexNodeIndex result = mtable;
    if (top != 0 && top == bottom && top == left && top == right && padLeft == padRight) {
        texNodeSetAttribute(p->tree, mtable, "frame", kLineNames[top]);
        PrTextBuffer spacing{};
        appendLength(&spacing, padLeft, padLeft == 0 ? "" : "em");
        prTextBufferAppend(&spacing, " 0");
        texNodeSetAttribute(p->tree, mtable, "framespacing", prTextBufferView(&spacing));
        prTextBufferReset(&spacing);
    } else {
        if (padLeft != 0 || padRight != 0) {
            result = wrap(p, TexElement_Mpadded, result, TexClass_Ord, context);
            PrTextBuffer value{};
            appendLength(&value, padLeft, padLeft == 0 ? "" : "em");
            texNodeSetAttribute(p->tree, result, "lspace", prTextBufferView(&value));
            prTextBufferClear(&value);
            prTextBufferAppendChar(&value, '+');
            appendLength(&value, padLeft + padRight, "em");
            texNodeSetAttribute(p->tree, result, "width", prTextBufferView(&value));
            prTextBufferReset(&value);
        }
        if (top != 0 || bottom != 0 || left != 0 || right != 0) {
            PrTextBuffer notation{};
            const uint8_t sides[] = {top, bottom, left, right};
            const char* const kSides[] = {"top", "bottom", "left", "right"};
            for (uint32_t i = 0; i < 4; ++i) {
                if (sides[i] == 0)
                    continue;
                if (notation.size != 0)
                    prTextBufferAppendChar(&notation, ' ');
                prTextBufferAppend(&notation, kSides[i]);
            }
            result = wrap(p, TexElement_Menclose, result, TexClass_Ord, context);
            texNodeSetAttribute(p->tree, result, "notation", prTextBufferView(&notation));
            prTextBufferReset(&notation);
        }
    }
    columns.reset();
    prTextBufferReset(&list.text);
    if (layout.left == nullptr && layout.right == nullptr)
        return result;
    TexNodeIndex fenced = makeNode(p, TexElement_Mrow, TexClass_Inner, context);
    if (layout.left != nullptr)
        texNodeAppend(p->tree, fenced, makeFence(p, layout.left, TexClass_Open, context));
    else
        texNodeAppend(p->tree, fenced, makeNullDelimiter(p, context));
    texNodeAppend(p->tree, fenced, result);
    if (layout.right != nullptr)
        texNodeAppend(p->tree, fenced, makeFence(p, layout.right, TexClass_Close, context));
    else
        texNodeAppend(p->tree, fenced, makeNullDelimiter(p, context));
    return fenced;
}

// The layouts of the environments, from amsmath and LaTeX. An array's strut is \@arstrut, 0.7 and 0.3 of a 12pt
// baselineskip times \arraystretch. Its columns are padded by \arraycolsep, 5pt. An alignment's rows are \jot, 3pt,
// apart, and its pairs of columns 2em.
struct EnvironmentLayout {
    const char* name;
    TableLayout layout;
};

constexpr float kArrayStrutHeight = 0.84f;
constexpr float kArrayStrutDepth = 0.36f;
constexpr float kJot = 0.3f;

constexpr TableLayout matrixLayout(const char* left, const char* right) {
    return TableLayout{
        ColumnScheme_Center, 0.5f, false, 0, kArrayStrutHeight, kArrayStrutDepth, 0, TexStyle_Text,
        false, left, right, Numbering_None, Spread_None,
    };
}

constexpr TableLayout alignmentLayout(float pairGap, bool tagged, Numbering numbering, Spread spread) {
    return TableLayout{
        ColumnScheme_Alignment, 0, false, pairGap, kArrayStrutHeight, kArrayStrutDepth, kJot,
        TexStyle_Display, tagged, nullptr, nullptr, numbering, spread,
    };
}

constexpr TableLayout gatherLayout(bool tagged, Numbering numbering) {
    return TableLayout{
        ColumnScheme_Center, 0, false, 0, kArrayStrutHeight, kArrayStrutDepth, kJot, TexStyle_Display,
        tagged, nullptr, nullptr, numbering, Spread_None,
    };
}

constexpr TableLayout casesLayout(TexStyle style, const char* left, const char* right) {
    return TableLayout{
        ColumnScheme_Cases, 0, false, 0, kArrayStrutHeight * 1.2f, kArrayStrutDepth * 1.2f, 0, style,
        false, left, right, Numbering_None, Spread_None,
    };
}

constexpr TableLayout arrayLayout(TexStyle style) {
    return TableLayout{
        ColumnScheme_Specified, 0.5f, true, 0, kArrayStrutHeight, kArrayStrutDepth, 0, style, false,
        nullptr, nullptr, Numbering_None, Spread_None,
    };
}

constexpr TableLayout subarrayLayout() {
    return TableLayout{
        ColumnScheme_Specified, 0, false, 0, 0, 0, 0, TexStyle_Script, false, nullptr, nullptr, Numbering_None,
        Spread_None,
    };
}

constexpr TableLayout multlineLayout(Numbering numbering) {
    return TableLayout{
        ColumnScheme_Multline, 0, false, 0, kArrayStrutHeight, kArrayStrutDepth, kJot, TexStyle_Display, true,
        nullptr, nullptr, numbering, Spread_None,
    };
}

constexpr TableLayout eqnarrayLayout(Numbering numbering) {
    return TableLayout{
        ColumnScheme_Eqnarray, 0.5f, false, 0, kArrayStrutHeight, kArrayStrutDepth, kJot, TexStyle_Display, true,
        nullptr, nullptr, numbering, Spread_None,
    };
}

// The layout of amscd's commutative diagrams, with objects and arrows in centered columns that the arrows space.
constexpr TableLayout cdLayout() {
    return TableLayout{
        ColumnScheme_Center, 0, false, 0, kArrayStrutHeight, kArrayStrutDepth, 0, TexStyle_Display, false,
        nullptr, nullptr, Numbering_None, Spread_None,
    };
}

constexpr EnvironmentLayout kEnvironments[] = {
    {"matrix", matrixLayout(nullptr, nullptr)},
    {"pmatrix", matrixLayout("(", ")")},
    {"bmatrix", matrixLayout("[", "]")},
    {"Bmatrix", matrixLayout("{", "}")},
    {"vmatrix", matrixLayout("|", "|")},
    {"Vmatrix", matrixLayout("‖", "‖")},
    {"matrix*", matrixLayout(nullptr, nullptr)},
    {"pmatrix*", matrixLayout("(", ")")},
    {"bmatrix*", matrixLayout("[", "]")},
    {"Bmatrix*", matrixLayout("{", "}")},
    {"vmatrix*", matrixLayout("|", "|")},
    {"Vmatrix*", matrixLayout("‖", "‖")},
    {
        "smallmatrix", TableLayout{
            ColumnScheme_Center, 0.1389f, false, 0, 0.85f, 0.35f, 0, TexStyle_Script, false,
            nullptr, nullptr, Numbering_None, Spread_None,
        },
    },
    {"cases", casesLayout(TexStyle_Text, "{", nullptr)},
    {"dcases", casesLayout(TexStyle_Display, "{", nullptr)},
    {"rcases", casesLayout(TexStyle_Text, nullptr, "}")},
    {"drcases", casesLayout(TexStyle_Display, nullptr, "}")},
    {"array", arrayLayout(TexStyle_Text)},
    {"darray", arrayLayout(TexStyle_Display)},
    {"subarray", subarrayLayout()},
    {"aligned", alignmentLayout(2, false, Numbering_None, Spread_None)},
    {"align", alignmentLayout(2, true, Numbering_EachRow, Spread_Even)},
    {"align*", alignmentLayout(2, true, Numbering_None, Spread_Even)},
    {"split", alignmentLayout(2, false, Numbering_None, Spread_None)},
    {"flalign", alignmentLayout(0, true, Numbering_EachRow, Spread_Pairs)},
    {"flalign*", alignmentLayout(0, true, Numbering_None, Spread_Pairs)},
    {"alignat", alignmentLayout(0, true, Numbering_EachRow, Spread_None)},
    {"alignat*", alignmentLayout(0, true, Numbering_None, Spread_None)},
    {"alignedat", alignmentLayout(0, false, Numbering_None, Spread_None)},
    {"gather", gatherLayout(true, Numbering_EachRow)},
    {"gather*", gatherLayout(true, Numbering_None)},
    {"gathered", gatherLayout(false, Numbering_None)},
    {"multline", multlineLayout(Numbering_Last)},
    {"multline*", multlineLayout(Numbering_None)},
    {"eqnarray", eqnarrayLayout(Numbering_EachRow)},
    {"eqnarray*", eqnarrayLayout(Numbering_None)},
};

// Parses the label of an amscd arrow, which `delimiter` ends, in script style. Returns kTexNodeNone for an empty label.
TexNodeIndex parseCdLabel(Parser* p, const Context& context, char delimiter) {
    Context label = context;
    label.style = scriptStyle(context.style);
    TexNodeIndex row = makeRow(p, label);
    char outer = p->stopCharacter;
    p->stopCharacter = delimiter;
    parseList(p, label, StopFlag_Character | StopFlag_Newline | StopFlag_EndEnvironment, row);
    p->stopCharacter = outer;
    if (at(p, row).firstChild == kTexNodeNone)
        return kTexNodeNone;
    return collapse(p, row);
}

// Makes a horizontal arrow of amscd, at least \minCDarrowwidth, 2.5pc, long, with its labels above and below, each
// padded by a thick space on both sides.
TexNodeIndex makeCdHorizontalArrow(Parser* p, PrStringView text, TexNodeIndex above, TexNodeIndex below, const Context& context) {
    Context labels = context;
    labels.style = scriptStyle(context.style);
    TexNodeIndex arrow = makeToken(p, TexElement_Mo, text, TexClass_Rel, context);
    at(p, arrow).stretchy = TexTri_True;
    auto pad = [&](TexNodeIndex label) {
        TexNodeIndex padded = makeNode(p, TexElement_Mpadded, TexClass_Ord, labels);
        appendContents(p, padded, label);
        texNodeSetAttribute(p->tree, padded, "lspace", "0.2778em");
        texNodeSetAttribute(p->tree, padded, "width", "+0.5556em");
        return padded;
    };
    TexNodeIndex over = kTexNodeNone;
    if (above != kTexNodeNone) {
        over = pad(above);
    } else {
        over = makeNode(p, TexElement_Mspace, TexClass_None, labels);
        texNodeSetAttribute(p->tree, over, "width", "3em");
    }
    TexNodeIndex node = makeNode(p, below != kTexNodeNone ? TexElement_Munderover : TexElement_Mover, TexClass_Rel, context);
    texNodeAppend(p->tree, node, arrow);
    if (below != kTexNodeNone)
        texNodeAppend(p->tree, node, pad(below));
    texNodeAppend(p->tree, node, over);
    return node;
}

// Makes a vertical arrow of amscd, with its labels to the left and right. The labels are lapped, so that the arrow is
// centered in its column. A stretchy operator in a cell does not stretch to its table row, so the arrow has a size of
// its own: 2.4em, the size of Computer Modern's largest delimiter variant.
TexNodeIndex makeCdVerticalArrow(Parser* p, PrStringView text, TexNodeIndex left, TexNodeIndex right, const Context& context) {
    Context labels = context;
    labels.style = scriptStyle(context.style);
    TexNodeIndex row = makeRow(p, context);
    if (left != kTexNodeNone) {
        TexNodeIndex lapped = makeNode(p, TexElement_Mpadded, TexClass_Ord, labels);
        texNodeSetAttribute(p->tree, lapped, "width", "0");
        texNodeSetAttribute(p->tree, lapped, "lspace", "-1width");
        appendContents(p, lapped, left);
        texNodeAppend(p->tree, lapped, makeMuSpace(p, 3, labels));
        texNodeAppend(p->tree, row, lapped);
    }
    TexNodeIndex arrow = makeFence(p, text, TexClass_Ord, context);
    setFenceSize(p, arrow, "2.4em");
    texNodeAppend(p->tree, row, arrow);
    if (right != kTexNodeNone) {
        TexNodeIndex lapped = makeNode(p, TexElement_Mpadded, TexClass_Ord, labels);
        texNodeSetAttribute(p->tree, lapped, "width", "0");
        texNodeAppend(p->tree, lapped, makeMuSpace(p, 3, labels));
        appendContents(p, lapped, right);
        texNodeAppend(p->tree, row, lapped);
    }
    return row;
}

// An item of a row of a commutative diagram, which is an arrow, or @. that leaves its place empty.
struct CdItem {
    TexNodeIndex object;            // the object before a horizontal arrow
    TexNodeIndex arrow;             // kTexNodeNone for @.
    bool vertical;
};

// Parses amscd's CD environment after \begin{CD}. Rows of objects and the horizontal arrows between them, @>>>, @<<<
// and @=, alternate with rows of vertical arrows, @VVV, @AAA and @|, each below an object. @. is an arrow that is not
// drawn. Objects and arrows are the cells of a table, with horizontal arrows in the columns between objects.
TexNodeIndex parseCd(Parser* p, const Context& context) {
    Context cellContext = context;
    cellContext.style = TexStyle_Display;
    TableState table{};
    table.outer = p->table;
    p->table = &table;
    RawArray<CdItem> items{};
    for (bool more = true; more;) {
        items.clear();
        TexNodeIndex segment = kTexNodeNone;
        for (;;) {
            segment = makeRow(p, cellContext);
            Stop stop = parseList(p, cellContext, StopFlag_At | StopFlag_Newline | StopFlag_EndEnvironment, segment);
            if (stop == Stop_At) {
                Token kind = nextRaw(p);
                char c = kind.kind == TokenKind_Character && kind.size == 1 ? tokenText(p, kind).data[0] : '\0';
                CdItem item{.object = collapse(p, segment), .arrow = kTexNodeNone};
                if (c == '>' || c == '<') {
                    TexNodeIndex above = parseCdLabel(p, cellContext, c);
                    TexNodeIndex below = parseCdLabel(p, cellContext, c);
                    item.arrow = makeCdHorizontalArrow(p, c == '>' ? "→" : "←", above, below, cellContext);
                } else if (c == '=') {
                    item.arrow = makeCdHorizontalArrow(p, "=", kTexNodeNone, kTexNodeNone, cellContext);
                } else if (c == 'V' || c == 'A') {
                    TexNodeIndex left = parseCdLabel(p, cellContext, c);
                    TexNodeIndex right = parseCdLabel(p, cellContext, c);
                    item.arrow = makeCdVerticalArrow(p, c == 'V' ? "↓" : "↑", left, right, cellContext);
                    item.vertical = true;
                } else if (c == '|') {
                    item.arrow = makeCdVerticalArrow(p, "‖", kTexNodeNone, kTexNodeNone, cellContext);
                    item.vertical = true;
                } else if (c != '.') {
                    // amscd's "Illegal arrow".
                    if (kind.kind != TokenKind_End)
                        pushBack(p, kind);
                    item.arrow =
                        makeError(p, kind.sourceStart > 0 ? kind.sourceStart - 1 : 0, kind.sourceStart, cellContext);
                }
                items.push(item);
                continue;
            }
            if (stop == Stop_EndEnvironment) {
                PrTextBuffer end{};
                readTextArgument(p, &end);
                prTextBufferReset(&end);
            }
            more = stop == Stop_Newline;
            break;
        }
        bool vertical = false;
        for (uint32_t i = 0; i < items.count; ++i)
            vertical = vertical || items.data[i].vertical;
        TexNodeIndex last = collapse(p, segment);
        bool empty =
            items.count == 0 && at(p, last).element == TexElement_Mrow && at(p, last).firstChild == kTexNodeNone;
        // A final \\ leaves an empty row.
        if (empty && !more && table.rows.count != 0)
            break;
        TableRow row{.firstCell = table.cells.count, .tag = kTexNodeNone};
        auto cell = [&](TexNodeIndex content) {
            TexNodeIndex mrow = makeRow(p, cellContext);
            if (content != kTexNodeNone)
                appendContents(p, mrow, content);
            table.cells.push(mrow);
            ++row.cellCount;
        };
        for (uint32_t i = 0; i < items.count; ++i) {
            const CdItem& item = items.data[i];
            if (vertical) {
                if (i != 0)
                    cell(kTexNodeNone);
                cell(item.arrow);
            } else {
                cell(item.object);
                cell(item.arrow);
            }
        }
        if (!vertical)
            cell(last);
        table.rows.push(row);
    }
    p->table = table.outer;
    items.reset();
    RawArray<SpecifiedColumn> none{};
    TexNodeIndex result = buildTable(p, context, table, cdLayout(), none, 0);
    resetTable(&table);
    return result;
}

TexNodeIndex parseEnvironment(Parser* p, const Context& context, uint32_t sourceStart, bool substack) {
    PrTextBuffer name{};
    if (!substack)
        readTextArgument(p, &name);
    PrStringView environment = substack ? PrStringView("subarray") : prTextBufferView(&name);
    if (environment == "equation" || environment == "equation*" || environment == "displaymath") {
        Context display = context;
        display.style = TexStyle_Display;
        TexNodeIndex row = makeRow(p, display);
        // An equation is a table of one cell, whose row takes the tag.
        TableState equation{};
        equation.tagged = true;
        equation.outer = p->table;
        equation.rows.push(TableRow{.firstCell = 0, .cellCount = 1, .tag = kTexNodeNone});
        p->table = &equation;
        parseList(p, display, StopFlag_EndEnvironment, row);
        p->table = equation.outer;
        PrTextBuffer end{};
        readTextArgument(p, &end);
        prTextBufferReset(&end);
        equation.cells.push(row);
        numberRows(p, &equation, environment == "equation" ? Numbering_EachRow : Numbering_None);
        prTextBufferReset(&name);
        TexNodeIndex result = kTexNodeNone;
        if (equation.rows.data[0].tag != kTexNodeNone) {
            RawArray<SpecifiedColumn> none{};
            TableLayout layout = gatherLayout(true, Numbering_None);
            layout.strutHeight = 0;
            result = buildTable(p, context, equation, layout, none, 0);
        } else {
            result = groupAtom(p, row);
        }
        resetTable(&equation);
        return result;
    }
    if (environment == "CD") {
        prTextBufferReset(&name);
        return parseCd(p, context);
    }
    const TableLayout* layout = nullptr;
    for (const EnvironmentLayout& candidate : kEnvironments) {
        if (environment == PrStringView(candidate.name))
            layout = &candidate.layout;
    }
    RawArray<SpecifiedColumn> specified{};
    uint8_t lineAfter = 0;
    char matrixAlign = 0;
    if (layout != nullptr && layout->scheme == ColumnScheme_Specified && !substack) {
        PrTextBuffer spec{};
        readTextArgument(p, &spec);
        ColumnSpec state{.columns = &specified, .line = 0, .pad = true};
        parseColumnSpec(prTextBufferView(&spec), &state, 0);
        lineAfter = state.line;
        prTextBufferReset(&spec);
    } else if (substack) {
        specified.push(SpecifiedColumn{.align = 'c', .padBefore = false, .padAfter = false});
    } else if (prStringEndsWith(environment, "matrix*")) {
        PrTextBuffer align{};
        if (readOptionalText(p, &align) && align.size == 1)
            matrixAlign = align.data[0];
        prTextBufferReset(&align);
    } else if (environment == "alignat" || environment == "alignat*" || environment == "alignedat") {
        PrTextBuffer count{};
        readTextArgument(p, &count);
        prTextBufferReset(&count);
    }
    Context cellContext = context;
    // The script style of \substack and smallmatrix is \scriptstyle, whatever the style around them.
    cellContext.style = layout != nullptr ? layout->cellStyle : TexStyle_Text;
    TableState table{};
    table.tagged = layout != nullptr && layout->tagged;
    parseRows(p, cellContext, &table, substack);
    numberRows(p, &table, layout != nullptr ? layout->numbering : Numbering_None);
    TexNodeIndex result = kTexNodeNone;
    if (layout == nullptr) {
        // An environment that the language does not have is an error that holds its whole source.
        result = makeError(p, sourceStart, p->lastEnd, context);
    } else {
        TableLayout used = *layout;
        if (matrixAlign == 'l' || matrixAlign == 'r') {
            used.scheme = ColumnScheme_Specified;
            for (uint32_t r = 0; r < table.rows.count; ++r) {
                while (specified.count < table.rows.data[r].cellCount)
                    specified.push(SpecifiedColumn{.align = matrixAlign, .padBefore = true, .padAfter = true});
            }
        }
        result = buildTable(p, context, table, used, specified, lineAfter);
    }
    resetTable(&table);
    specified.reset();
    prTextBufferReset(&name);
    return result;
}

bool isScripted(TexElement element) {
    switch (element) {
        case TexElement_Msub:
        case TexElement_Msup:
        case TexElement_Msubsup:
        case TexElement_Munder:
        case TexElement_Mover:
        case TexElement_Munderover:
        case TexElement_Mmultiscripts:
        case TexElement_Mfrac:
            return true;
        default:
            return false;
    }
}

// Returns true for the grouping elements and <mpadded>, whose one embellished child makes them embellished.
bool isGrouping(TexElement element) {
    switch (element) {
        case TexElement_Mrow:
        case TexElement_Mstyle:
        case TexElement_Merror:
        case TexElement_Mphantom:
        case TexElement_Mpadded:
        case TexElement_Menclose:
            return true;
        default:
            return false;
    }
}

// Returns the one embellished child of a grouping element whose other children are space-like, or kTexNodeNone.
TexNodeIndex embellishedChild(const TexTree* tree, TexNodeIndex node) {
    TexNodeIndex found = kTexNodeNone;
    for (TexNodeIndex child = tree->nodes.data[node].firstChild; child != kTexNodeNone;
            child = tree->nodes.data[child].next) {
        if (texNodeIsSpaceLike(tree, child))
            continue;
        if (found != kTexNodeNone || !texNodeIsEmbellished(tree, child))
            return kTexNodeNone;
        found = child;
    }
    return found;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Trees

TexNodeIndex texNodeCreate(TexTree* tree, TexElement element, TexClass texClass, TexStyle style) {
    TexNode node{};
    node.firstChild = kTexNodeNone;
    node.lastChild = kTexNodeNone;
    node.next = kTexNodeNone;
    node.firstAttribute = kTexNodeNone;
    node.lastAttribute = kTexNodeNone;
    node.element = element;
    node.texClass = texClass;
    node.style = style;
    node.textStart = tree->text.size;
    tree->nodes.push(node);
    return tree->nodes.count - 1;
}

void texNodeAppend(TexTree* tree, TexNodeIndex parent, TexNodeIndex child) {
    texNodeInsertBefore(tree, parent, child, kTexNodeNone);
}

void texNodeInsertBefore(TexTree* tree, TexNodeIndex parent, TexNodeIndex child, TexNodeIndex before) {
    TexNode* nodes = tree->nodes.data;
    PR_ASSERT(nodes[child].next == kTexNodeNone);
    if (before == kTexNodeNone) {
        if (nodes[parent].lastChild == kTexNodeNone)
            nodes[parent].firstChild = child;
        else
            nodes[nodes[parent].lastChild].next = child;
        nodes[parent].lastChild = child;
        return;
    }
    nodes[child].next = before;
    if (nodes[parent].firstChild == before) {
        nodes[parent].firstChild = child;
        return;
    }
    TexNodeIndex previous = nodes[parent].firstChild;
    while (nodes[previous].next != before)
        previous = nodes[previous].next;
    nodes[previous].next = child;
}

void texNodeMoveChildren(TexTree* tree, TexNodeIndex from, TexNodeIndex to) {
    TexNode* nodes = tree->nodes.data;
    if (nodes[from].firstChild == kTexNodeNone)
        return;
    if (nodes[to].lastChild == kTexNodeNone)
        nodes[to].firstChild = nodes[from].firstChild;
    else
        nodes[nodes[to].lastChild].next = nodes[from].firstChild;
    nodes[to].lastChild = nodes[from].lastChild;
    nodes[from].firstChild = kTexNodeNone;
    nodes[from].lastChild = kTexNodeNone;
}

uint32_t texNodeChildCount(const TexTree* tree, TexNodeIndex node) {
    uint32_t count = 0;
    for (TexNodeIndex child = tree->nodes.data[node].firstChild; child != kTexNodeNone;
            child = tree->nodes.data[child].next)
        ++count;
    return count;
}

void texNodeSetText(TexTree* tree, TexNodeIndex node, PrStringView text) {
    TexNode& n = tree->nodes.data[node];
    n.textStart = tree->text.size;
    n.textSize = uint32_t(text.size);
    prTextBufferAppend(&tree->text, text);
}

void texNodeAppendText(TexTree* tree, TexNodeIndex node, PrStringView text) {
    PrRawString joined = prRawStringCopy(texNodeText(tree, node));
    prRawStringAppend(&joined, text);
    texNodeSetText(tree, node, prRawStringView(&joined));
    prRawStringReset(&joined);
}

PrStringView texNodeText(const TexTree* tree, TexNodeIndex node) {
    const TexNode& n = tree->nodes.data[node];
    if (n.textSize == 0)
        return "";
    return PrStringView{tree->text.data + n.textStart, n.textSize};
}

void texNodeSetAttribute(TexTree* tree, TexNodeIndex node, PrStringView name, PrStringView value) {
    uint32_t start = tree->text.size;
    prTextBufferAppend(&tree->text, value);
    for (uint32_t a = tree->nodes.data[node].firstAttribute; a != kTexNodeNone; a = tree->attributes.data[a].next) {
        if (tree->attributes.data[a].name == name) {
            tree->attributes.data[a].valueStart = start;
            tree->attributes.data[a].valueSize = uint32_t(value.size);
            return;
        }
    }
    TexAttributeRecord record{};
    record.name = name;
    record.valueStart = start;
    record.valueSize = uint32_t(value.size);
    record.next = kTexNodeNone;
    tree->attributes.push(record);
    uint32_t index = tree->attributes.count - 1;
    TexNode& n = tree->nodes.data[node];
    if (n.lastAttribute == kTexNodeNone)
        n.firstAttribute = index;
    else
        tree->attributes.data[n.lastAttribute].next = index;
    n.lastAttribute = index;
}

void texNodeCopyAttributes(TexTree* tree, TexNodeIndex from, TexNodeIndex to) {
    for (uint32_t a = tree->nodes.data[from].firstAttribute; a != kTexNodeNone; a = tree->attributes.data[a].next) {
        // The copy shares the value's text, which the tree never changes.
        TexAttributeRecord record = tree->attributes.data[a];
        record.next = kTexNodeNone;
        tree->attributes.push(record);
        uint32_t index = tree->attributes.count - 1;
        TexNode& n = tree->nodes.data[to];
        if (n.lastAttribute == kTexNodeNone)
            n.firstAttribute = index;
        else
            tree->attributes.data[n.lastAttribute].next = index;
        n.lastAttribute = index;
    }
}

bool texNodeHasAttribute(const TexTree* tree, TexNodeIndex node, PrStringView name) {
    for (uint32_t a = tree->nodes.data[node].firstAttribute; a != kTexNodeNone; a = tree->attributes.data[a].next) {
        if (tree->attributes.data[a].name == name)
            return true;
    }
    return false;
}

PrStringView texAttributeValue(const TexTree* tree, const TexAttributeRecord& attribute) {
    if (attribute.valueSize == 0)
        return "";
    return PrStringView{tree->text.data + attribute.valueStart, attribute.valueSize};
}

void texTreeReset(TexTree* tree) {
    tree->nodes.reset();
    tree->attributes.reset();
    prTextBufferReset(&tree->text);
    tree->root = kTexNodeNone;
}

bool texNodeIsEmbellished(const TexTree* tree, TexNodeIndex node) {
    const TexNode& n = tree->nodes.data[node];
    if (n.element == TexElement_Mo)
        return true;
    if (isScripted(n.element))
        return n.firstChild != kTexNodeNone && texNodeIsEmbellished(tree, n.firstChild);
    if (isGrouping(n.element))
        return embellishedChild(tree, node) != kTexNodeNone;
    return false;
}

TexNodeIndex texNodeCore(const TexTree* tree, TexNodeIndex node) {
    const TexNode& n = tree->nodes.data[node];
    if (n.element == TexElement_Mo)
        return node;
    if (isScripted(n.element))
        return texNodeCore(tree, n.firstChild);
    return texNodeCore(tree, embellishedChild(tree, node));
}

bool texNodeIsSpaceLike(const TexTree* tree, TexNodeIndex node) {
    const TexNode& n = tree->nodes.data[node];
    if (n.element == TexElement_Mtext || n.element == TexElement_Mspace)
        return true;
    if (!isGrouping(n.element))
        return false;
    for (TexNodeIndex child = n.firstChild; child != kTexNodeNone; child = tree->nodes.data[child].next) {
        if (!texNodeIsSpaceLike(tree, child))
            return false;
    }
    return true;
}

bool texElementIsRow(TexElement element) {
    switch (element) {
        case TexElement_Math:
        case TexElement_Mrow:
        case TexElement_Mstyle:
        case TexElement_Merror:
        case TexElement_Mphantom:
        case TexElement_Mpadded:
        case TexElement_Menclose:
        case TexElement_Msqrt:
        case TexElement_Mtd:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Macros

void texMacroTableDefine(TexMacroTable* table, PrStringView name, uint32_t parameterCount, PrStringView replacement,
        const PrStringView* PR_NULLABLE defaultArgument, PrStringView parameterText) {
    PR_ASSERT(parameterCount <= 9);
    TexMacro* macro = nullptr;
    for (uint32_t i = 0; i < table->macros.count; ++i) {
        if (prRawStringView(&table->macros.data[i].name) == name)
            macro = &table->macros.data[i];
    }
    if (macro == nullptr) {
        macro = &table->macros.emplace();
        *macro = TexMacro{};
        macro->name = prRawStringCopy(name);
    }
    prRawStringReplace(&macro->replacement, replacement);
    prRawStringReplace(&macro->defaultArgument, defaultArgument != nullptr ? *defaultArgument : PrStringView{""});
    prRawStringReplace(&macro->parameterText, parameterText.size != 0 ? parameterText : PrStringView{""});
    macro->parameterCount = uint8_t(parameterCount);
    macro->hasDefault = defaultArgument != nullptr;
}

const TexMacro* PR_NULLABLE texMacroTableFind(const TexMacroTable* table, PrStringView name) {
    for (uint32_t i = 0; i < table->macros.count; ++i) {
        if (prRawStringView(&table->macros.data[i].name) == name)
            return &table->macros.data[i];
    }
    return nullptr;
}

void texMacroTableClear(TexMacroTable* table) {
    for (uint32_t i = 0; i < table->macros.count; ++i) {
        prRawStringReset(&table->macros.data[i].name);
        prRawStringReset(&table->macros.data[i].replacement);
        prRawStringReset(&table->macros.data[i].defaultArgument);
        prRawStringReset(&table->macros.data[i].parameterText);
    }
    table->macros.clear();
}

void texMacroTableReset(TexMacroTable* table) {
    texMacroTableClear(table);
    table->macros.reset();
}

void texDocumentSetLabel(TexDocument* document, PrStringView name, PrStringView tag) {
    for (uint32_t i = 0; i < document->labels.count; ++i) {
        if (prRawStringView(&document->labels.data[i].name) == name) {
            prRawStringReplace(&document->labels.data[i].tag, tag);
            return;
        }
    }
    document->labels.push(TexLabel{.name = prRawStringCopy(name), .tag = prRawStringCopy(tag)});
}

PrStringView texDocumentLabel(const TexDocument* document, PrStringView name) {
    for (uint32_t i = 0; i < document->labels.count; ++i) {
        const TexLabel& label = document->labels.data[i];
        if (prRawStringView(&label.name) == name) {
            PrStringView tag = prRawStringView(&label.tag);
            return tag.data != nullptr ? tag : PrStringView{""};
        }
    }
    return PrStringView{};
}

void texDocumentClear(TexDocument* document) {
    texMacroTableClear(&document->macros);
    for (uint32_t i = 0; i < document->labels.count; ++i) {
        prRawStringReset(&document->labels.data[i].name);
        prRawStringReset(&document->labels.data[i].tag);
    }
    document->labels.clear();
    document->equationCount = 0;
}

void texDocumentReset(TexDocument* document) {
    texDocumentClear(document);
    texMacroTableReset(&document->macros);
    document->labels.reset();
}

// ---------------------------------------------------------------------------------------------------------------------
// Parsing

void texParse(TexTree* tree, PrStringView source, PrTexDisplay display, bool annotate,
        TexDocument* PR_NULLABLE document) {
    PR_ASSERT(tree->nodes.count == 0);
    Parser p{};
    p.tree = tree;
    p.source = source;
    p.document = document != nullptr ? document : &p.own;
    p.display = display;
    p.top.tagged = true;
    p.top.rows.push(TableRow{.firstCell = 0, .cellCount = 1, .tag = kTexNodeNone});
    Context context{};
    context.style = display == PrTexDisplay_Block ? TexStyle_Display : TexStyle_Text;
    TexNodeIndex root = makeNode(&p, TexElement_Math, TexClass_None, context);
    texNodeSetAttribute(tree, root, "display", display == PrTexDisplay_Block ? "block" : "inline");
    tree->root = root;

    // Lines that \\ breaks the formula into are the rows of a gather.
    TableState lines{};
    p.table = nullptr;
    TexNodeIndex body = makeRow(&p, context);
    for (;;) {
        Stop stop = parseList(&p, context, StopFlag_Newline, body);
        if (stop != Stop_Newline)
            break;
        PrTextBuffer gap{};
        readOptionalText(&p, &gap);
        prTextBufferReset(&gap);
        lines.rows.push(TableRow{.firstCell = lines.cells.count, .cellCount = 1, .tag = kTexNodeNone});
        lines.cells.push(body);
        body = makeRow(&p, context);
    }
    if (lines.rows.count != 0) {
        lines.rows.push(TableRow{.firstCell = lines.cells.count, .cellCount = 1, .tag = kTexNodeNone});
        lines.cells.push(body);
        RawArray<SpecifiedColumn> none{};
        TableLayout layout = gatherLayout(false, Numbering_None);
        layout.cellStyle = context.style;
        TexNodeIndex table = buildTable(&p, context, lines, layout, none, 0);
        body = makeRow(&p, context);
        texNodeAppend(tree, body, table);
    }
    lines.cells.reset();
    lines.rows.reset();

    // A tag outside an environment sets the formula in a row whose tag is at the right margin.
    p.top.cells.push(body);
    numberRows(&p, &p.top, Numbering_None);
    if (p.top.rows.data[0].tag != kTexNodeNone) {
        RawArray<SpecifiedColumn> none{};
        TableLayout layout = gatherLayout(true, Numbering_None);
        layout.strutHeight = 0;
        TexNodeIndex table = buildTable(&p, context, p.top, layout, none, 0);
        body = makeRow(&p, context);
        texNodeAppend(tree, body, table);
    }
    resetTable(&p.top);

    // References take the tags of the labels, which the formula's own equations have defined by now. A label that no
    // equation defines is LaTeX's "??".
    for (uint32_t i = 0; i < p.refs.count; ++i) {
        PendingRef& ref = p.refs.data[i];
        PrStringView tag = texDocumentLabel(p.document, prRawStringView(&ref.name));
        PrRawString text = prRawStringCopy(ref.equation ? "(" : "");
        prRawStringAppend(&text, tag.data != nullptr ? tag : PrStringView{"??"});
        prRawStringAppend(&text, ref.equation ? ")" : "");
        texNodeSetText(tree, ref.node, prRawStringView(&text));
        prRawStringReset(&text);
        prRawStringReset(&ref.name);
    }
    p.refs.reset();

    if (annotate) {
        TexNodeIndex semantics = makeNode(&p, TexElement_Semantics, TexClass_None, context);
        TexNodeIndex content = body;
        if (texNodeChildCount(tree, body) == 1 && !(at(&p, at(&p, body).firstChild).flags & TexNodeFlag_Transparent))
            content = collapse(&p, body);
        texNodeAppend(tree, semantics, content);
        TexNodeIndex annotation = makeToken(&p, TexElement_Annotation, source, TexClass_None, context);
        texNodeSetAttribute(tree, annotation, "encoding", "application/x-tex");
        texNodeAppend(tree, semantics, annotation);
        texNodeAppend(tree, root, semantics);
    } else {
        texNodeMoveChildren(tree, body, root);
    }
    p.pending.reset();
    prTextBufferReset(&p.arena);
    texMacroTableReset(&p.local);
    texDocumentReset(&p.own);
}

} // namespace pr_impl
