// XML documents built from the XML parser's calls.
//
// xml - https://www.w3.org/TR/xml/

#pragma once

#include "pr/array.h"
#include "pr/dom.h"
#include "pr/dom_xml.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The steps that the builder runs when it closes an HTML or SVG script element, if the document runs scripts. `line`
// and `column` give the start of the element's text. A failure stops the parse with its status. The steps may pause the
// parse (prXmlParserPause).
struct DomXmlScripting {
    void* PR_NULLABLE context;
    PrStatus (*PR_NULLABLE run)(void* PR_NULLABLE context, PrDomElement* script, uint32_t line, uint32_t column);
};

// The source of the entities that a document does not declare, when its doctype names an external subset that the
// parser does not read (PrXmlHandler::entity).
struct DomXmlEntities {
    void* PR_NULLABLE context;
    PrStringView (*PR_NONNULL resolve)(void* PR_NULLABLE context, PrStringView publicId,
                                       PrStringView name);
};

// A parse of an XML document's source into a document, as the source arrives. The builder holds a reference to each
// element whose end it has not seen, while a script could remove it. A builder must not move while its parse runs,
// since the parser's handler refers to it.
struct DomXmlBuilder {
    PrDomDocument* document;
    DomXmlScripting scripting;      // a null `run` means that the document runs no scripts
    const DomXmlEntities* PR_NULLABLE entities;
    PrXmlHandler handler;
    PrXmlParser* parser;            // the parse, whose input the builder's user appends and runs
    Array<PrDomElement*> open;
};

// Starts a parse into `document`, which must have no children. `entities` must outlive the builder.
void domXmlBuilderInit(DomXmlBuilder* builder, PrDomDocument* document, DomXmlScripting scripting,
                       const DomXmlEntities* PR_NULLABLE entities);

// Frees the parse, and releases the elements that it holds. The document keeps what was built.
void domXmlBuilderReset(DomXmlBuilder* builder);

// Parses the UTF-8 `source` into `document`, which must have no children, as a builder does with the whole source.
// Returns Syntax if the document is not well-formed, and `error` describes where. The document holds the content parsed
// before the error.
PrStatus domXmlParse(PrDomDocument* document, PrStringView source, DomXmlScripting scripting,
                     const DomXmlEntities* PR_NULLABLE entities, PrXmlError* PR_NULLABLE error);

// Finds the charset of an XML document's bytes, and writes it to `*out`. The charset is that of the byte order mark, or
// else `transport`, the charset of the document's Content-Type, or else the one the document declares
// (prXmlDeclaredCharset), or else UTF-8. Returns false if the bytes are not `complete` and too few have arrived to
// decide.
bool domXmlCharset(PrStringView bytes, PrCharset transport, bool complete, PrCharset* out);

// Returns the text of an XML document's bytes as UTF-8, and sets `*outCharset` to the charset that decoded them
// (domXmlCharset). The text is a view of `bytes` if they decode to themselves, and is otherwise appended to `out`.
PrStringView domXmlDecode(PrStringView bytes, PrCharset transport, PrString* out, PrCharset* outCharset);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
