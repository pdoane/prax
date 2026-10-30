// XML
//
// xml - https://www.w3.org/TR/xml/
// xml-names - https://www.w3.org/TR/xml-names/

#pragma once

#include "pr/base.h"
#include "pr/charset.h"
#include "pr/status.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A position in a document's source. Columns count code points.
typedef struct PrXmlPosition {
    uint32_t line;     // from 1
    uint32_t column;   // from 1
} PrXmlPosition;

// The position where a document is not well-formed, and what was found there. The message is the parser's own text, or
// the handler's (prXmlParserFail), and outlives the parse. The message is the null view for a well-formed document.
typedef struct PrXmlError {
    PrStringView message;
    uint32_t line;     // from 1
    uint32_t column;   // in code points, from 1
} PrXmlError;

// A name with its namespace resolved (xml-names). `space` is the namespace's URL, or the empty view for no namespace.
// `prefix` is the null view if the name has none.
typedef struct PrXmlName {
    PrStringView space;
    PrStringView prefix;
    PrStringView local;
} PrXmlName;

// An attribute of a start tag, with its value's references replaced and its whitespace normalized. Namespace
// declarations are attributes too, in the xmlns namespace.
typedef struct PrXmlAttribute {
    PrXmlName name;
    PrStringView value;
} PrXmlAttribute;

// A parse of a document whose source arrives in pieces. The parser reports each piece of content once the whole of it
// has arrived. A handler may pause the parse, and a later run continues it. A browsing context pauses at a script that
// blocks the parser, for example.
typedef struct PrXmlParser PrXmlParser;

// What a parse reports, in document order. Each view is valid for the duration of the call. A call that returns
// anything other than PrStatus_Ok stops the parse with that status. A null member ignores what it would report.
typedef struct PrXmlHandler {
    void* PR_NULLABLE context;

    // An element's start tag, whose attributes are in source order. An empty-element tag is a start and an end.
    PrStatus (*PR_NULLABLE startElement)(void* PR_NULLABLE context, PrXmlParser* parser,
        const PrXmlName* name, const PrXmlAttribute* PR_NULLABLE attributes, uint32_t attributeCount);

    // The end of the element that the latest unended start began (prXmlParserContentStart).
    PrStatus (*PR_NULLABLE endElement)(void* PR_NULLABLE context, PrXmlParser* parser, const PrXmlName* name);

    // The character data between two pieces of markup, with its references replaced. Character data outside the
    // document element is whitespace, and is not reported.
    PrStatus (*PR_NULLABLE text)(void* PR_NULLABLE context, PrXmlParser* parser, PrStringView data);

    PrStatus (*PR_NULLABLE cdataSection)(void* PR_NULLABLE context, PrXmlParser* parser, PrStringView data);
    PrStatus (*PR_NULLABLE comment)(void* PR_NULLABLE context, PrXmlParser* parser, PrStringView data);
    PrStatus (*PR_NULLABLE processingInstruction)(void* PR_NULLABLE context, PrXmlParser* parser,
        PrStringView target, PrStringView data);

    // The document type declaration. Its identifiers are the empty view where it has none.
    PrStatus (*PR_NULLABLE doctype)(void* PR_NULLABLE context, PrXmlParser* parser, PrStringView name,
        PrStringView publicId, PrStringView systemId);

    // Returns the replacement text of the entity `name` in the external subset that `publicId` identifies, which the
    // parser does not read, or the null view if it knows no such entity. A reference to an entity that neither the
    // internal subset declares nor this resolves is an error.
    PrStringView (*PR_NULLABLE entity)(void* PR_NULLABLE context, PrStringView publicId, PrStringView name);
} PrXmlHandler;

// Parses the UTF-8 `source`, reporting it to `handler`. The parser processes namespaces, and expands the general
// entities of the internal subset. Returns Syntax if the document is not well-formed, and a non-null `error` describes
// where; the handler has been told of what came before the error. A handler's pause has no effect: the parse continues
// at once.
PrStatus prXmlParse(PrStringView source, const PrXmlHandler* handler, PrXmlError* PR_NULLABLE error);

// Creates a parse that reports to `handler`, as prXmlParse does, with no input yet. The handler must outlive the parse.
PrXmlParser* prXmlParserCreate(const PrXmlHandler* handler);

// Frees the parse.
void prXmlParserDestroy(PrXmlParser* PR_NULLABLE parser);

// Appends UTF-8 `text` to the parse's input, or ends the input. Neither parses: prXmlParserRun does. Neither may be
// called during a handler's call, nor once the input has ended.
void prXmlParserAppend(PrXmlParser* parser, PrStringView text);
void prXmlParserEnd(PrXmlParser* parser);

// Parses as much of the input as it can. A run stops once it has parsed the input that has arrived, once the handler
// pauses it, or once the parse has ended. Returns the parse's status: Ok while the parse goes on or once it has ended
// with a well-formed document, Syntax if the document is not well-formed (prXmlParserError), or a handler's status. A
// parse that has failed stays failed.
PrStatus prXmlParserRun(PrXmlParser* parser);

// Pauses the parse once the handler's call returns. The next prXmlParserRun continues it. A pause during the expansion
// of an entity reference takes effect once the expansion ends, since a run continues only from the source.
void prXmlParserPause(PrXmlParser* parser);

// Returns true if the handler paused the parse, and no run has continued it since.
bool prXmlParserIsPaused(const PrXmlParser* parser);

// Returns true once the parse has ended: it has parsed its whole input, or has stopped with a status other than Ok.
bool prXmlParserIsEnded(const PrXmlParser* parser);

// The position where the document is not well-formed, and what was found there. The message is the null view unless the
// parse's status is Syntax.
PrXmlError prXmlParserError(const PrXmlParser* parser);

// Records that the document is not well-formed at the parser's position, for a reason the handler found, and returns
// Syntax for the handler's call to return. `message` must outlive the parse.
PrStatus prXmlParserFail(PrXmlParser* parser, PrStringView message);

// Returns where the content of the element being ended starts. Called from endElement.
PrXmlPosition prXmlParserContentStart(const PrXmlParser* parser);

// Returns true while the parser reads the replacement text of an entity reference, rather than the source itself.
bool prXmlParserIsInEntity(const PrXmlParser* parser);

// Returns true if the code point can start a Name, or can appear in one after its start (XML's NameStartChar and
// NameChar).
bool prXmlIsNameStartChar(uint32_t c);
bool prXmlIsNameChar(uint32_t c);

// Returns true if `name` matches XML's Name production.
bool prXmlIsName(PrStringView name);

// Finds the charset that the start of a document's bytes gives it, apart from a byte order mark, as Appendix F of XML
// detects it, and writes it to `*out`. That is UTF-16LE or UTF-16BE for a document that starts with `<?` in that
// charset, and otherwise the charset that its encoding declaration names (XML's EncodingDecl). A declaration written in
// ASCII cannot be in UTF-16, so one that names UTF-16 gives UTF-8. The charset is PrCharset_None if the bytes give
// none, or name one that is not a label.
//
// Returns false, and writes nothing, if the bytes are not `complete` and end before they decide: within the first four
// bytes, or within an XML declaration.
bool prXmlDeclaredCharset(PrStringView bytes, bool complete, PrCharset* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
