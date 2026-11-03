#include "pr/xml.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/raw_string.h"
#include "pr/string_ops.h"
#include "pr/utf8.h"

#include "xml/src/xml_parser.h"

namespace pr_impl {
namespace {

constexpr PrStringView kXmlNamespace = "http://www.w3.org/XML/1998/namespace";
constexpr PrStringView kXmlnsNamespace = "http://www.w3.org/2000/xmlns/";
constexpr PrStringView kByteOrderMark = "\xEF\xBB\xBF";

// The maximum nesting depth of entity expansion. Beyond it, a reference is treated as recursive.
constexpr uint32_t kEntityDepth = 16;

using Parser = PrXmlParser;

// An attribute as its start tag writes it, before namespaces are processed.
struct Attribute {
    PrStringView name;
    PrRawString value;
};

bool fail(Parser* p, PrStringView message) {
    if (p->status == PrStatus_Ok) {
        p->status = PrStatus_Syntax;
        p->message = message;
        p->errorAt = p->depth != 0 ? p->reference : p->at;
    }
    return false;
}

// Takes the status that a handler's call returned. Returns false if it stops the parse.
bool handled(Parser* p, PrStatus status) {
    if (status != PrStatus_Ok && p->status == PrStatus_Ok)
        p->status = status;
    return p->status == PrStatus_Ok;
}

bool more(const Parser* p) {
    return p->status == PrStatus_Ok && p->at < p->input.size;
}

PrStringView rest(const Parser* p) {
    return prStringSlice(p->input, p->at, p->input.size - p->at);
}

bool startsWith(const Parser* p, PrStringView literal) {
    return prStringStartsWith(rest(p), literal);
}

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n';
}

size_t skipSpaces(Parser* p) {
    size_t start = p->at;
    while (p->at < p->input.size && isSpace(p->input.data[p->at]))
        ++p->at;
    return p->at - start;
}

bool expect(Parser* p, PrStringView literal, PrStringView message) {
    if (!startsWith(p, literal))
        return fail(p, message);
    p->at += literal.size;
    return true;
}

// Returns true if `c` matches XML's Char production.
bool isChar(uint32_t c) {
    return c == 0x9 || c == 0xa || c == 0xd || (c >= 0x20 && c <= 0xd7ff) ||
        (c >= 0xe000 && c <= 0xfffd) || (c >= 0x10000 && c <= 0x10ffff);
}

// Returns the code point at the parser's position, and writes its size in bytes to `*size`.
uint32_t peek(const Parser* p, uint32_t* size) {
    uint32_t c = 0;
    *size = prUtf8DecodeScalar(p->input, p->at, &c);
    return c;
}

// Reads a Name and moves the parser past it.
bool parseName(Parser* p, PrStringView* out) {
    size_t start = p->at;
    uint32_t size = 0;
    if (p->at >= p->input.size || !prXmlIsNameStartChar(peek(p, &size)))
        return fail(p, "expected a name");
    p->at += size;
    while (p->at < p->input.size && prXmlIsNameChar(peek(p, &size)))
        p->at += size;
    *out = prStringSlice(p->input, start, p->at - start);
    return true;
}

// Reads a quoted literal, without its quotes, and moves the parser past it.
bool parseQuoted(Parser* p, PrStringView* out) {
    if (p->at >= p->input.size || (p->input.data[p->at] != '"' && p->input.data[p->at] != '\''))
        return fail(p, "expected a quoted value");
    char quote = p->input.data[p->at++];
    size_t start = p->at;
    while (p->at < p->input.size && p->input.data[p->at] != quote)
        ++p->at;
    if (p->at == p->input.size)
        return fail(p, "a quoted value is not closed");
    *out = prStringSlice(p->input, start, p->at - start);
    ++p->at;
    return true;
}

const XmlEntity* PR_NULLABLE findEntity(const Parser* p, PrStringView name) {
    for (uint32_t i = 0; i < p->entities.count; ++i) {
        if (prRawStringView(&p->entities.data[i].name) == name)
            return &p->entities.data[i];
    }
    return nullptr;
}

// Appends to `out` the text of a character reference, or of a reference to a predefined entity or to an entity of the
// external subset that the handler resolves. The parser is past the '&', and moves past the ';'. Returns false, without
// failing, for any other entity, and does not move past it.
bool characterReference(Parser* p, PrTextBuffer* out, bool* handled) {
    *handled = true;
    size_t start = p->at;
    if (p->at < p->input.size && p->input.data[p->at] == '#') {
        ++p->at;
        bool hex = p->at < p->input.size && p->input.data[p->at] == 'x';
        p->at += hex;
        uint64_t value = 0;
        size_t digits = 0;
        while (p->at < p->input.size &&
                (hex ? prAsciiIsHexDigit(p->input.data[p->at]) : prAsciiIsDigit(p->input.data[p->at]))) {
            value = value * (hex ? 16 : 10) + prAsciiHexValue(p->input.data[p->at++]);
            value = value > 0x10ffff ? 0x110000 : value;
            ++digits;
        }
        if (digits == 0 || !expect(p, ";", "a character reference is not closed"))
            return fail(p, "a character reference has no digits");
        if (!isChar(uint32_t(value)))
            return fail(p, "a character reference names no character XML allows");
        prTextBufferAppendCodePoint(out, uint32_t(value));
        return true;
    }
    PrStringView name;
    uint32_t size = 0;
    if (p->at >= p->input.size || !prXmlIsNameStartChar(peek(p, &size)))
        return fail(p, "a '&' does not start a reference; write it as &amp;");
    if (!parseName(p, &name) || !expect(p, ";", "an entity reference is not closed"))
        return false;
    struct PredefinedEntity {
        PrStringView name;
        char value;
    };
    constexpr PredefinedEntity kPredefined[] = {{"lt", '<'}, {"gt", '>'}, {"amp", '&'}, {"apos", '\''}, {"quot", '"'}};
    for (const PredefinedEntity& entity : kPredefined) {
        if (name == entity.name) {
            prTextBufferAppendChar(out, entity.value);
            return true;
        }
    }
    if (findEntity(p, name) == nullptr && p->handler->entity != nullptr && !prRawStringIsEmpty(&p->publicId)) {
        PrStringView expansion = p->handler->entity(p->handler->context, prRawStringView(&p->publicId), name);
        if (expansion.data != nullptr) {
            prTextBufferAppend(out, expansion);
            return true;
        }
    }
    p->at = start;
    *handled = false;
    return true;
}

// Runs `read` over the replacement text of the entity whose name is at the parser's position, then moves past its ';'.
template <typename Read>
bool expandEntity(Parser* p, Read read) {
    size_t start = p->at;
    PrStringView name;
    parseName(p, &name);
    ++p->at;
    const XmlEntity* entity = findEntity(p, name);
    if (entity == nullptr) {
        p->at = start - 1;
        return fail(p, "an entity is referred to that is not declared");
    }
    // An external entity is not fetched, and expands to nothing.
    if (prRawStringIsNull(&entity->value))
        return true;
    if (p->depth == kEntityDepth)
        return fail(p, "an entity refers to itself");
    PrStringView input = p->input;
    size_t at = p->at;
    if (p->depth++ == 0)
        p->reference = start - 1;
    p->input = prRawStringView(&entity->value);
    p->at = 0;
    bool ok = read();
    p->input = input;
    p->at = at;
    --p->depth;
    return ok && p->status == PrStatus_Ok;
}

// Reads an attribute's value, with its references replaced and its whitespace normalized to spaces, and moves the
// parser past it and its closing quote. A `quote` of '\0' reads an entity's replacement text in one pass, to its end.
bool attributeValue(Parser* p, PrTextBuffer* out, char quote) {
    while (more(p)) {
        char c = p->input.data[p->at];
        if (c == quote) {
            ++p->at;
            return true;
        }
        if (c == '<')
            return fail(p, "an attribute value holds a '<'");
        if (c == '&') {
            ++p->at;
            bool handled = false;
            if (!characterReference(p, out, &handled))
                return false;
            if (!handled && !expandEntity(p, [&] { return attributeValue(p, out, '\0'); }))
                return false;
            continue;
        }
        prTextBufferAppendChar(out, isSpace(c) ? ' ' : c);
        ++p->at;
    }
    return quote == '\0' ? p->status == PrStatus_Ok : fail(p, "an attribute value is not closed");
}

// Reports the pending character data. Outside the document element it must be whitespace, which is not reported.
bool flushText(Parser* p) {
    PrStringView data = prTextBufferView(&p->text);
    if (data.size == 0)
        return true;
    if (p->open.count == 0) {
        for (size_t i = 0; i < data.size; ++i) {
            if (!isSpace(data.data[i])) {
                p->at = p->textAt;
                return fail(p, "text is outside the document element");
            }
        }
    } else if (p->handler->text != nullptr && !handled(p, p->handler->text(p->handler->context, p, data))) {
        return false;
    }
    prTextBufferClear(&p->text);
    return true;
}

// Looks up the URL that `prefix` is bound to. The empty prefix is the default namespace. Returns false if the prefix is
// not bound.
bool resolve(const Parser* p, PrStringView prefix, PrStringView* url) {
    if (prefix == "xml") {
        *url = kXmlNamespace;
        return true;
    }
    for (uint32_t i = p->bindings.count; i-- > 0;) {
        if (prRawStringView(&p->bindings.data[i].prefix) == prefix) {
            *url = prRawStringView(&p->bindings.data[i].url);
            return true;
        }
    }
    *url = PrStringView{};
    return prefix.size == 0;
}

// Splits a qualified name (the QName of xml-names) into its prefix and local name. The prefix is the null view if there
// is none.
bool splitName(Parser* p, PrStringView name, PrStringView* prefix, PrStringView* local) {
    size_t colon = prStringFindByte(name, ':');
    if (colon >= name.size) {
        *prefix = PrStringView{};
        *local = name;
        return true;
    }
    *prefix = PrStringView{name.data, colon};
    *local = prStringSlice(name, colon + 1, name.size - colon - 1);
    if (prefix->size == 0 || local->size == 0 || prStringFindByte(*local, ':') < local->size)
        return fail(p, "a name is not a qualified name");
    return true;
}

// Resolves an element's qualified name against the bindings in scope.
bool elementName(Parser* p, PrStringView qualified, PrXmlName* out) {
    if (!splitName(p, qualified, &out->prefix, &out->local))
        return false;
    if (!resolve(p, out->prefix.data != nullptr ? out->prefix : "", &out->space))
        return fail(p, "an element's prefix is not bound to a namespace");
    if (out->space.data == nullptr)
        out->space = "";
    return true;
}

// Writes the position of `offset` in the source as a line and a column in code points, each starting at 1.
void position(PrStringView source, size_t offset, uint32_t* line, uint32_t* column) {
    *line = 1;
    size_t start = 0;
    for (size_t i = 0; i < offset && i < source.size; ++i) {
        if (source.data[i] == '\n') {
            ++*line;
            start = i + 1;
        }
    }
    *column = 1;
    for (size_t i = start; i < offset; ++i)
        *column += (uint8_t(source.data[i]) & 0xc0) != 0x80;
}

// Reports the end of the element whose qualified name is `qualified`, while its bindings are in scope.
bool endElement(Parser* p, PrStringView qualified, size_t contentAt) {
    if (p->handler->endElement == nullptr)
        return true;
    PrXmlName name = {};
    if (!elementName(p, qualified, &name))
        return false;
    p->contentAt = contentAt;
    return handled(p, p->handler->endElement(p->handler->context, p, &name));
}

// Removes the bindings after the first `count` from scope.
void popBindings(Parser* p, uint32_t count) {
    for (uint32_t i = count; i < p->bindings.count; ++i) {
        prRawStringReset(&p->bindings.data[i].prefix);
        prRawStringReset(&p->bindings.data[i].url);
    }
    p->bindings.count = count;
}

void resetAttributes(RawArray<Attribute>* attributes) {
    for (uint32_t i = 0; i < attributes->count; ++i)
        prRawStringReset(&attributes->data[i].value);
    attributes->reset();
}

// Resolves the start tag's attributes into `out`, and checks that no two have one expanded name.
bool resolveAttributes(Parser* p, const RawArray<Attribute>& attributes, RawArray<PrXmlAttribute>* out) {
    for (uint32_t i = 0; i < attributes.count; ++i) {
        PrStringView attribute = attributes.data[i].name;
        PrXmlAttribute resolved = {};
        resolved.value = prRawStringView(&attributes.data[i].value);
        resolved.name.space = "";
        if (attribute == "xmlns") {
            resolved.name.local = attribute;
            resolved.name.space = kXmlnsNamespace;
        } else {
            if (!splitName(p, attribute, &resolved.name.prefix, &resolved.name.local))
                return false;
            if (resolved.name.prefix == "xmlns")
                resolved.name.space = kXmlnsNamespace;
            else if (resolved.name.prefix.data != nullptr && !resolve(p, resolved.name.prefix, &resolved.name.space))
                return fail(p, "an attribute's prefix is not bound to a namespace");
        }
        for (uint32_t j = 0; j < out->count; ++j) {
            if (out->data[j].name.space == resolved.name.space && out->data[j].name.local == resolved.name.local)
                return fail(p, "an attribute is given twice in one namespace");
        }
        out->push(resolved);
    }
    return true;
}

// Reads a start tag, with the parser past its '<', and processes namespaces (xml-names).
bool startTag(Parser* p) {
    if (p->open.count == 0 && p->root)
        return fail(p, "an element follows the document element");
    PrStringView name;
    if (!parseName(p, &name) || !flushText(p))
        return false;
    RawArray<Attribute> attributes;
    bool empty = false;
    for (;;) {
        size_t spaces = skipSpaces(p);
        if (startsWith(p, "/>")) {
            p->at += 2;
            empty = true;
            break;
        }
        if (startsWith(p, ">")) {
            ++p->at;
            break;
        }
        PrStringView attribute;
        if (spaces == 0 || !parseName(p, &attribute)) {
            resetAttributes(&attributes);
            return fail(p, "a start tag is not well-formed");
        }
        skipSpaces(p);
        if (!expect(p, "=", "an attribute has no value")) {
            resetAttributes(&attributes);
            return false;
        }
        skipSpaces(p);
        char quote = p->at < p->input.size ? p->input.data[p->at] : '\0';
        PrTextBuffer value = {};
        bool ok = quote == '"' || quote == '\'';
        if (ok) {
            ++p->at;
            ok = attributeValue(p, &value, quote);
        } else {
            fail(p, "an attribute value is not quoted");
        }
        for (uint32_t i = 0; ok && i < attributes.count; ++i) {
            if (attributes.data[i].name == attribute)
                ok = fail(p, "an attribute is given twice");
        }
        if (!ok) {
            prTextBufferReset(&value);
            resetAttributes(&attributes);
            return false;
        }
        attributes.push(Attribute{attribute, prTextBufferTake(&value)});
    }

    // The namespaces that the start tag declares are in scope for its name and its attributes.
    uint32_t bindings = p->bindings.count;
    bool ok = true;
    for (uint32_t i = 0; ok && i < attributes.count; ++i) {
        PrStringView attribute = attributes.data[i].name;
        PrStringView url = prRawStringView(&attributes.data[i].value);
        bool declares = attribute == "xmlns" || prStringStartsWith(attribute, "xmlns:");
        if (!declares)
            continue;
        PrStringView prefix = attribute.size > 5 ? prStringSlice(attribute, 6, attribute.size - 6) : "";
        if (prefix == "xmlns" || url == kXmlnsNamespace || ((prefix == "xml") != (url == kXmlNamespace)) ||
                (prefix.size != 0 && url.size == 0))
            ok = fail(p, "a namespace declaration binds a reserved prefix or name, or undeclares a prefix");
        else
            p->bindings.push(XmlBinding{prRawStringCopy(prefix), prRawStringCopy(url)});
    }
    PrXmlName element = {};
    RawArray<PrXmlAttribute> resolved;
    ok = ok && elementName(p, name, &element) && resolveAttributes(p, attributes, &resolved);
    if (ok && p->handler->startElement != nullptr)
        ok = handled(p, p->handler->startElement(p->handler->context, p, &element, resolved.data, resolved.count));
    resolved.reset();
    resetAttributes(&attributes);
    if (!ok)
        return false;

    p->root = true;
    if (empty) {
        ok = endElement(p, name, p->at);
        popBindings(p, bindings);
        return ok;
    }
    p->open.push(XmlOpen{prRawStringCopy(name), bindings, p->at});
    return true;
}

bool endTag(Parser* p) {
    PrStringView name;
    if (!parseName(p, &name))
        return false;
    skipSpaces(p);
    if (!expect(p, ">", "an end tag is not closed") || !flushText(p))
        return false;
    if (p->open.count == 0 || prRawStringView(&p->open.last().name) != name)
        return fail(p, "an end tag does not match the element it closes");
    XmlOpen closed = p->open.data[--p->open.count];
    bool done = endElement(p, prRawStringView(&closed.name), closed.contentAt);
    prRawStringReset(&closed.name);
    popBindings(p, closed.bindings);
    return done;
}

// Reads a comment, with the parser past its "<!--".
bool comment(Parser* p) {
    size_t end = prStringFind(rest(p), "--");
    if (end == rest(p).size)
        return fail(p, "a comment is not closed");
    PrStringView data = prStringSlice(p->input, p->at, end);
    p->at += end;
    if (!startsWith(p, "-->"))
        return fail(p, "a comment holds \"--\"");
    p->at += 3;
    if (!flushText(p))
        return false;
    return p->handler->comment == nullptr || handled(p, p->handler->comment(p->handler->context, p, data));
}

// Reads a processing instruction, with the parser past its "<?".
bool processingInstruction(Parser* p) {
    PrStringView target;
    if (!parseName(p, &target))
        return false;
    if (prAsciiEqualsFold(target, "xml"))
        return fail(p, "an XML declaration is not at the start of the document");
    PrStringView data = "";
    if (!startsWith(p, "?>")) {
        if (skipSpaces(p) == 0)
            return fail(p, "a processing instruction's target is not followed by a space");
        size_t end = prStringFind(rest(p), "?>");
        if (end == rest(p).size)
            return fail(p, "a processing instruction is not closed");
        data = prStringSlice(p->input, p->at, end);
        p->at += end;
    }
    p->at += 2;
    if (!flushText(p))
        return false;
    return p->handler->processingInstruction == nullptr ||
        handled(p, p->handler->processingInstruction(p->handler->context, p, target, data));
}

// Reads a CDATA section, with the parser past its "<![CDATA[".
bool cdataSection(Parser* p) {
    if (p->open.count == 0)
        return fail(p, "a CDATA section is outside the document element");
    size_t end = prStringFind(rest(p), "]]>");
    if (end == rest(p).size)
        return fail(p, "a CDATA section is not closed");
    PrStringView data = prStringSlice(p->input, p->at, end);
    p->at += end + 3;
    if (!flushText(p))
        return false;
    return p->handler->cdataSection == nullptr || handled(p, p->handler->cdataSection(p->handler->context, p, data));
}

// Skips a declaration of the internal subset that the parser does not read, up to its '>' outside quotes.
bool skipDeclaration(Parser* p) {
    while (more(p)) {
        char c = p->input.data[p->at];
        if (c == '"' || c == '\'') {
            PrStringView quoted;
            if (!parseQuoted(p, &quoted))
                return false;
            continue;
        }
        ++p->at;
        if (c == '>')
            return true;
    }
    return fail(p, "a declaration is not closed");
}

// Reads the declaration of a general entity, with the parser past "<!ENTITY". The replacement text has its character
// references expanded and its entity references kept as written. The declaration of a parameter entity is skipped.
bool entityDeclaration(Parser* p) {
    if (skipSpaces(p) == 0)
        return fail(p, "an entity declaration is not well-formed");
    if (startsWith(p, "%"))
        return skipDeclaration(p);
    PrStringView name;
    if (!parseName(p, &name) || skipSpaces(p) == 0)
        return fail(p, "an entity declaration is not well-formed");
    PrRawString value = prRawStringNull();
    if (startsWith(p, "SYSTEM") || startsWith(p, "PUBLIC")) {
        if (!skipDeclaration(p))
            return false;
    } else {
        PrStringView literal;
        if (!parseQuoted(p, &literal))
            return false;
        PrTextBuffer text = {};
        for (size_t i = 0; i < literal.size; ++i) {
            if (literal.data[i] == '&' && i + 1 < literal.size && literal.data[i + 1] == '#') {
                size_t end = prStringFindByte(prStringSlice(literal, i, literal.size - i), ';');
                PrStringView input = p->input;
                size_t at = p->at;
                p->input = prStringSlice(literal, i + 1, end < literal.size - i ? end : literal.size - i - 1);
                p->at = 0;
                bool handled = false;
                bool ok = characterReference(p, &text, &handled);
                p->input = input;
                p->at = at;
                if (!ok) {
                    prTextBufferReset(&text);
                    return false;
                }
                i += end;
                continue;
            }
            prTextBufferAppendChar(&text, literal.data[i]);
        }
        value = prTextBufferTake(&text);
        skipSpaces(p);
        if (!expect(p, ">", "an entity declaration is not closed")) {
            prRawStringReset(&value);
            return false;
        }
    }
    // The first declaration of an entity binds it.
    if (findEntity(p, name) == nullptr)
        p->entities.push(XmlEntity{prRawStringCopy(name), value});
    else
        prRawStringReset(&value);
    return true;
}

// Reads the internal subset, with the parser past its '['.
bool internalSubset(Parser* p) {
    for (;;) {
        skipSpaces(p);
        if (!more(p))
            return fail(p, "the internal subset is not closed");
        if (startsWith(p, "]")) {
            ++p->at;
            return true;
        }
        bool ok = true;
        if (startsWith(p, "<!--") || startsWith(p, "<?")) {
            PrStringView close = startsWith(p, "<?") ? "?>" : "-->";
            size_t end = prStringFind(rest(p), close);
            if (end == rest(p).size)
                return fail(p, "a comment or a processing instruction is not closed");
            p->at += end + close.size;
        } else if (startsWith(p, "<!ENTITY")) {
            p->at += 8;
            ok = entityDeclaration(p);
        } else if (startsWith(p, "<!")) {
            ok = skipDeclaration(p);
        } else if (startsWith(p, "%")) {
            // A parameter entity reference. The parser does not read its declarations.
            size_t end = prStringFindByte(rest(p), ';');
            if (end >= p->input.size - p->at)
                return fail(p, "a parameter entity reference is not closed");
            p->at += end + 1;
        } else {
            return fail(p, "the internal subset holds something that is not a declaration");
        }
        if (!ok)
            return false;
    }
}

// Reads the document type declaration, with the parser past "<!DOCTYPE".
bool doctypeDeclaration(Parser* p) {
    if (p->doctype || p->root)
        return fail(p, "a document type declaration is misplaced");
    p->doctype = true;
    PrStringView name;
    if (skipSpaces(p) == 0 || !parseName(p, &name))
        return fail(p, "a document type declaration has no name");
    PrStringView publicId = "";
    PrStringView systemId = "";
    bool spaced = skipSpaces(p) != 0;
    if (spaced && startsWith(p, "PUBLIC")) {
        p->at += 6;
        if (skipSpaces(p) == 0 || !parseQuoted(p, &publicId) || skipSpaces(p) == 0 || !parseQuoted(p, &systemId))
            return fail(p, "a public identifier is not well-formed");
        skipSpaces(p);
    } else if (spaced && startsWith(p, "SYSTEM")) {
        p->at += 6;
        if (skipSpaces(p) == 0 || !parseQuoted(p, &systemId))
            return fail(p, "a system identifier is not well-formed");
        skipSpaces(p);
    }
    if (startsWith(p, "[")) {
        ++p->at;
        if (!internalSubset(p))
            return false;
        skipSpaces(p);
    }
    if (!expect(p, ">", "a document type declaration is not closed"))
        return false;
    p->publicId = prRawStringCopy(publicId);
    return p->handler->doctype == nullptr ||
        handled(p, p->handler->doctype(p->handler->context, p, name, publicId, systemId));
}

// Returns true if more input may follow the text being read: the parser reads the source, and its end has not arrived.
bool waits(const Parser* p) {
    return p->depth == 0 && !p->inputEnded;
}

// Returns true if `text` is shorter than `literal` and is its start, so that the text that follows decides whether
// `literal` is there.
bool startsPart(PrStringView text, PrStringView literal) {
    return text.size < literal.size && prStringStartsWith(literal, text);
}

// Reads character data up to the next markup or reference. "]]>" is not allowed in it, and neither is a character that
// XML does not allow. Character data stops before a ']' that could start "]]>" with text that has not arrived.
bool characterData(Parser* p) {
    size_t start = p->at;
    while (p->at < p->input.size) {
        char c = p->input.data[p->at];
        if (c == '<' || c == '&')
            break;
        if (c == ']' && waits(p) && startsPart(rest(p), "]]>"))
            break;
        if (c == ']' && startsWith(p, "]]>"))
            return fail(p, "text holds \"]]>\"");
        if (uint8_t(c) < 0x20 && c != '\t' && c != '\n')
            return fail(p, "text holds a character XML does not allow");
        ++p->at;
    }
    if (p->text.size == 0)
        p->textAt = p->depth != 0 ? p->reference : start;
    prTextBufferAppend(&p->text, prStringSlice(p->input, start, p->at - start));
    return true;
}

bool content(Parser* p);

// Reads one piece of content: markup, a reference, or character data up to the next of those. `depth` is the number of
// open elements where the text being read starts.
bool contentPiece(Parser* p, uint32_t depth) {
    if (startsWith(p, "</")) {
        if (p->open.count == depth && p->depth != 0)
            return fail(p, "an entity closes an element it did not open");
        p->at += 2;
        return endTag(p);
    }
    if (startsWith(p, "<!--")) {
        p->at += 4;
        return comment(p);
    }
    if (startsWith(p, "<![CDATA[")) {
        p->at += 9;
        return cdataSection(p);
    }
    if (startsWith(p, "<!DOCTYPE")) {
        p->at += 9;
        return doctypeDeclaration(p);
    }
    if (startsWith(p, "<?")) {
        p->at += 2;
        return processingInstruction(p);
    }
    if (startsWith(p, "<")) {
        ++p->at;
        return startTag(p);
    }
    if (startsWith(p, "&")) {
        if (p->open.count == 0)
            return fail(p, "a reference is outside the document element");
        ++p->at;
        bool handled = false;
        if (!characterReference(p, &p->text, &handled))
            return false;
        return handled || expandEntity(p, [&] { return content(p); });
    }
    return characterData(p);
}

// Reads the content of an entity's replacement text, to its end.
bool content(Parser* p) {
    uint32_t depth = p->open.count;
    while (more(p)) {
        if (!contentPiece(p, depth))
            return false;
    }
    if (p->open.count != depth)
        return fail(p, "an entity opens an element it does not close");
    return p->status == PrStatus_Ok;
}

// Returns true if `literal` has arrived at or after `start`, followed by `after` more bytes. A look that fails records
// where the next look resumes: at the last bytes that could start `literal`.
bool foundFrom(Parser* p, size_t start, PrStringView literal, size_t after) {
    size_t from = p->scanned > start ? p->scanned : start;
    PrStringView tail = prStringSlice(p->input, from, p->input.size - from);
    size_t found = prStringFind(tail, literal);
    if (found < tail.size && from + found + literal.size + after <= p->input.size)
        return true;
    size_t kept = literal.size + after - 1;
    p->scanned = p->input.size > start + kept ? p->input.size - kept : start;
    return false;
}

// Returns true if the '>' that ends the tag at `start` has arrived. A '>' in a quoted attribute value does not end it.
bool tagArrived(Parser* p, size_t start) {
    size_t at = p->scanned > start ? p->scanned : start + 1;
    char quote = p->scanQuote;
    for (; at < p->input.size; ++at) {
        char c = p->input.data[at];
        if (quote != '\0')
            quote = c == quote ? '\0' : quote;
        else if (c == '"' || c == '\'')
            quote = c;
        else if (c == '>')
            return true;
    }
    p->scanned = at;
    p->scanQuote = quote;
    return false;
}

// Returns true if a byte has arrived past the bytes of the reference at `start` that could belong to its name or its
// digits, so that the parser reads the reference to its ';' or fails where it does.
bool referenceArrived(const Parser* p, size_t start) {
    for (size_t at = start + 1; at < p->input.size; ++at) {
        char c = p->input.data[at];
        bool named = prAsciiIsLetter(c) || prAsciiIsDigit(c) || uint8_t(c) >= 0x80 || prStringFindByte("#:_-.", c) < 5;
        if (!named)
            return true;
    }
    return false;
}

// Returns the offset just past the quoted literal at `at`, or the input's size if its closing quote has not arrived.
size_t skipQuoted(PrStringView input, size_t at) {
    PrStringView tail = prStringSlice(input, at + 1, input.size - at - 1);
    size_t close = prStringFindByte(tail, input.data[at]);
    return close < tail.size ? at + close + 2 : input.size;
}

// Returns true if the '>' that ends the document type declaration at `start` has arrived. The look follows the grammar
// that doctypeDeclaration and internalSubset read, so that a quote, a bracket or a comment ends where they end it. A
// declaration is short, so each look starts again from its start.
bool doctypeArrived(const Parser* p, size_t start) {
    PrStringView input = p->input;
    bool subset = false;
    size_t at = start + 9;
    while (at < input.size) {
        char c = input.data[at];
        PrStringView tail = prStringSlice(input, at, input.size - at);
        if (c == '"' || c == '\'') {
            at = skipQuoted(input, at);
            continue;
        }
        if (!subset) {
            if (c == '>')
                return true;
            subset = c == '[';
            ++at;
            continue;
        }
        // Inside the internal subset, between its declarations.
        if (isSpace(c)) {
            ++at;
            continue;
        }
        if (c == ']') {
            subset = false;
            ++at;
            continue;
        }
        if (startsPart(tail, "<!--") || startsPart(tail, "<?"))
            return false;
        PrStringView close;
        if (prStringStartsWith(tail, "<!--"))
            close = "-->";
        else if (prStringStartsWith(tail, "<?"))
            close = "?>";
        else if (c == '%')
            close = ";";
        else
            close = PrStringView{};
        if (close.data != nullptr) {
            size_t end = prStringFind(tail, close);
            if (end == tail.size)
                return false;
            at += end + close.size;
            continue;
        }
        // The parser fails at anything else that is not a declaration.
        if (!prStringStartsWith(tail, "<!"))
            return true;
        // A declaration ends at its first '>' outside quotes.
        for (at += 2; at < input.size && input.data[at] != '>';)
            at = input.data[at] == '"' || input.data[at] == '\'' ? skipQuoted(input, at) : at + 1;
        ++at;
    }
    return false;
}

// Returns true if the whole of the piece of content at the parser's position has arrived, or if no more input follows.
// The parser reads a piece only once it has arrived, so that it reads the same pieces, and finds the same errors,
// however its input is divided. A look errs toward waiting: it may wait for bytes past the piece's end, but it never
// lets the parser reach the end of the input inside a piece.
bool arrived(Parser* p) {
    if (p->inputEnded)
        return true;
    PrStringView tail = rest(p);
    constexpr PrStringView kOpeners[] = {"</", "<!--", "<![CDATA[", "<!DOCTYPE", "<?"};
    for (PrStringView opener : kOpeners) {
        if (startsPart(tail, opener))
            return false;
    }
    if (prStringStartsWith(tail, "</"))
        return foundFrom(p, p->at, ">", 0);
    if (prStringStartsWith(tail, "<!--"))
        return foundFrom(p, p->at + 4, "--", 1);
    if (prStringStartsWith(tail, "<![CDATA["))
        return foundFrom(p, p->at + 9, "]]>", 0);
    if (prStringStartsWith(tail, "<!DOCTYPE"))
        return doctypeArrived(p, p->at);
    if (prStringStartsWith(tail, "<?"))
        return foundFrom(p, p->at + 2, "?>", 0);
    if (prStringStartsWith(tail, "<"))
        return tagArrived(p, p->at);
    if (prStringStartsWith(tail, "&"))
        return referenceArrived(p, p->at);
    return !startsPart(tail, "]]>");
}

// Returns true if the input decides how the document starts: whether with a byte order mark, and whether with an XML
// declaration, and if so, the whole of that declaration.
bool startArrived(const Parser* p) {
    if (p->inputEnded)
        return true;
    PrStringView source = rest(p);
    if (startsPart(source, kByteOrderMark))
        return false;
    if (prStringStartsWith(source, kByteOrderMark))
        source = prStringSlice(source, 3, source.size - 3);
    if (source.size < 6)
        return !prStringStartsWith("<?xml", prStringSlice(source, 0, source.size < 5 ? source.size : 5));
    if (!prStringStartsWith(source, "<?xml") || !isSpace(source.data[5]))
        return true;
    return prStringContains(source, "?>");
}

// Reads the XML declaration, if the document starts with one: its version, encoding and standalone pseudo-attributes.
bool xmlDeclaration(Parser* p) {
    if (!startsWith(p, "<?xml") || p->input.size < 6 || !isSpace(p->input.data[5]))
        return true;
    p->at += 5;
    size_t end = prStringFind(rest(p), "?>");
    if (end == rest(p).size)
        return fail(p, "the XML declaration is not closed");
    PrStringView pseudo = prStringSlice(p->input, p->at, end);
    if (!prStringContains(pseudo, "version"))
        return fail(p, "the XML declaration has no version");
    p->at += end + 2;
    return true;
}

// Reports the character data that is still to be reported at the end of the input, and checks that the document is
// complete.
void finish(Parser* p) {
    if (!flushText(p))
        return;
    if (p->open.count != 0)
        fail(p, "an element is not closed");
    else if (!p->root)
        fail(p, "the document has no document element");
    p->done = true;
}

// Parses as much of the input as has arrived, until the handler pauses the parse or the parse ends.
void run(Parser* p) {
    if (!p->started) {
        if (!startArrived(p))
            return;
        // The byte order mark is not part of the source, whose positions count from after it.
        if (startsWith(p, kByteOrderMark)) {
            prMemMove(p->source.data, p->source.data + kByteOrderMark.size, p->source.size - kByteOrderMark.size);
            p->source.size -= uint32_t(kByteOrderMark.size);
            p->input = prTextBufferView(&p->source);
        }
        p->started = true;
        if (!xmlDeclaration(p))
            return;
    }
    while (p->status == PrStatus_Ok && !p->paused && p->at < p->input.size) {
        if (!arrived(p))
            return;
        p->scanned = 0;
        p->scanQuote = '\0';
        if (!contentPiece(p, p->open.count))
            return;
    }
    if (p->status == PrStatus_Ok && !p->paused && p->inputEnded)
        finish(p);
}

// Returns the charset that a document's bytes give it (prXmlDeclaredCharset), from as many of them as have arrived.
PrCharset declaredCharset(PrStringView bytes) {
    if (prStringStartsWith(bytes, PrStringView{"<\0?\0", 4}))
        return PrCharset_Utf16Le;
    if (prStringStartsWith(bytes, PrStringView{"\0<\0?", 4}))
        return PrCharset_Utf16Be;
    if (!prStringStartsWith(bytes, "<?xml") || bytes.size < 6 || !isSpace(bytes.data[5]))
        return PrCharset_None;
    PrStringView declaration = prStringSlice(bytes, 5, prStringFind(bytes, "?>") - 5);
    size_t at = prStringFind(declaration, "encoding");
    if (at == declaration.size)
        return PrCharset_None;
    at += 8;
    while (at < declaration.size && isSpace(declaration.data[at]))
        ++at;
    if (at == declaration.size || declaration.data[at] != '=')
        return PrCharset_None;
    ++at;
    while (at < declaration.size && isSpace(declaration.data[at]))
        ++at;
    if (at == declaration.size || (declaration.data[at] != '"' && declaration.data[at] != '\''))
        return PrCharset_None;
    char quote = declaration.data[at++];
    PrStringView rest = prStringSlice(declaration, at, declaration.size - at);
    size_t close = prStringFindByte(rest, quote);
    if (close == rest.size)
        return PrCharset_None;
    PrCharset charset = prCharsetFromLabel(prStringSlice(rest, 0, close));
    return charset == PrCharset_Utf16Be || charset == PrCharset_Utf16Le ? PrCharset_Utf8 : charset;
}

} // namespace
} // namespace pr_impl

using namespace pr_impl;

PrStatus prXmlParse(PrStringView source, const PrXmlHandler* handler, PrXmlError* PR_NULLABLE error) {
    PrXmlParser* parser = prXmlParserCreate(handler);
    prXmlParserAppend(parser, source);
    prXmlParserEnd(parser);
    PrStatus status = prXmlParserRun(parser);
    while (status == PrStatus_Ok && !prXmlParserIsEnded(parser))
        status = prXmlParserRun(parser);
    if (error != nullptr)
        *error = prXmlParserError(parser);
    prXmlParserDestroy(parser);
    return status;
}

PrXmlParser* prXmlParserCreate(const PrXmlHandler* handler) {
    PrXmlParser* parser = allocZeroed<PrXmlParser>();
    parser->handler = handler;
    parser->input = prTextBufferView(&parser->source);
    parser->status = PrStatus_Ok;
    return parser;
}

void prXmlParserDestroy(PrXmlParser* PR_NULLABLE parser) {
    if (parser == nullptr)
        return;
    for (uint32_t i = 0; i < parser->open.count; ++i)
        prRawStringReset(&parser->open.data[i].name);
    parser->open.reset();
    popBindings(parser, 0);
    parser->bindings.reset();
    for (uint32_t i = 0; i < parser->entities.count; ++i) {
        prRawStringReset(&parser->entities.data[i].name);
        prRawStringReset(&parser->entities.data[i].value);
    }
    parser->entities.reset();
    prRawStringReset(&parser->publicId);
    prTextBufferReset(&parser->text);
    prTextBufferReset(&parser->source);
    prMemoryFree(parser);
}

void prXmlParserAppend(PrXmlParser* parser, PrStringView text) {
    PR_ASSERT(!parser->running && !parser->inputEnded);
    if (text.size == 0)
        return;
    // A carriage return is a line break, and so is a carriage return and the line feed after it, whichever pieces of
    // input they arrive in.
    size_t start = parser->carriageReturn && text.data[0] == '\n' ? 1 : 0;
    for (size_t i = start; i < text.size; ++i) {
        if (text.data[i] != '\r')
            continue;
        prTextBufferAppend(&parser->source, prStringSlice(text, start, i - start));
        prTextBufferAppendChar(&parser->source, '\n');
        i += i + 1 < text.size && text.data[i + 1] == '\n';
        start = i + 1;
    }
    prTextBufferAppend(&parser->source, prStringSlice(text, start, text.size - start));
    parser->carriageReturn = text.data[text.size - 1] == '\r';
    parser->input = prTextBufferView(&parser->source);
}

void prXmlParserEnd(PrXmlParser* parser) {
    PR_ASSERT(!parser->running && !parser->inputEnded);
    parser->inputEnded = true;
}

PrStatus prXmlParserRun(PrXmlParser* parser) {
    PR_ASSERT(!parser->running);
    parser->paused = false;
    parser->running = true;
    run(parser);
    parser->running = false;
    return parser->status;
}

void prXmlParserPause(PrXmlParser* parser) {
    parser->paused = true;
}

bool prXmlParserIsPaused(const PrXmlParser* parser) {
    return parser->paused;
}

bool prXmlParserIsEnded(const PrXmlParser* parser) {
    return parser->done || parser->status != PrStatus_Ok;
}

PrXmlError prXmlParserError(const PrXmlParser* parser) {
    PrXmlError error = {};
    if (parser->status == PrStatus_Syntax) {
        error.message = parser->message;
        position(prTextBufferView(&parser->source), parser->errorAt, &error.line, &error.column);
    }
    return error;
}

PrStatus prXmlParserFail(PrXmlParser* parser, PrStringView message) {
    fail(parser, message);
    return PrStatus_Syntax;
}

PrXmlPosition prXmlParserContentStart(const PrXmlParser* parser) {
    PrXmlPosition at = {};
    position(prTextBufferView(&parser->source), parser->contentAt, &at.line, &at.column);
    return at;
}

bool prXmlParserIsInEntity(const PrXmlParser* parser) {
    return parser->depth != 0;
}

bool prXmlIsNameStartChar(uint32_t c) {
    return c == ':' || c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= 0xc0 && c <= 0xd6) || (c >= 0xd8 && c <= 0xf6) || (c >= 0xf8 && c <= 0x2ff) ||
        (c >= 0x370 && c <= 0x37d) || (c >= 0x37f && c <= 0x1fff) || (c >= 0x200c && c <= 0x200d) ||
        (c >= 0x2070 && c <= 0x218f) || (c >= 0x2c00 && c <= 0x2fef) || (c >= 0x3001 && c <= 0xd7ff) ||
        (c >= 0xf900 && c <= 0xfdcf) || (c >= 0xfdf0 && c <= 0xfffd) || (c >= 0x10000 && c <= 0xeffff);
}

bool prXmlIsNameChar(uint32_t c) {
    return prXmlIsNameStartChar(c) || c == '-' || c == '.' || (c >= '0' && c <= '9') || c == 0xb7 ||
        (c >= 0x300 && c <= 0x36f) || (c >= 0x203f && c <= 0x2040);
}

bool prXmlIsName(PrStringView name) {
    if (name.size == 0)
        return false;
    for (size_t offset = 0; offset < name.size;) {
        uint32_t c = 0;
        uint32_t size = prUtf8DecodeScalar(name, offset, &c);
        if (!(offset == 0 ? prXmlIsNameStartChar(c) : prXmlIsNameChar(c)))
            return false;
        offset += size;
    }
    return true;
}

bool prXmlDeclaredCharset(PrStringView bytes, bool complete, PrCharset* out) {
    constexpr PrStringView kUtf16Le = {"<\0?\0", 4};
    constexpr PrStringView kUtf16Be = {"\0<\0?", 4};
    if (!complete && (startsPart(bytes, kUtf16Le) || startsPart(bytes, kUtf16Be)))
        return false;
    // The byte after "<?xml" decides whether the bytes start with a declaration, and its "?>" ends it.
    PrStringView head = prStringSlice(bytes, 0, bytes.size < 5 ? bytes.size : 5);
    if (!complete && bytes.size < 6 && prStringStartsWith("<?xml", head))
        return false;
    size_t end = prStringFind(bytes, "?>");
    bool declares = prStringStartsWith(bytes, "<?xml") && bytes.size >= 6 && isSpace(bytes.data[5]);
    if (!complete && declares && end == bytes.size)
        return false;
    *out = declaredCharset(bytes);
    return true;
}
