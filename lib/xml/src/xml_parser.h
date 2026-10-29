// The state of a parse (PrXmlParser).
//
// xml - https://www.w3.org/TR/xml/

#pragma once

#include "pr/array.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"
#include "pr/xml.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A namespace prefix in scope. The empty prefix is the default namespace, and the empty URL means no namespace.
struct XmlBinding {
    PrString prefix;
    PrString url;
};

// An element whose end tag has not been read: its qualified name as written in its start tag, the number of bindings
// that were in scope before it, and where its content starts in the source.
struct XmlOpen {
    PrString name;
    uint32_t bindings;
    size_t contentAt;
};

// A general entity that the internal subset declares, and its replacement text. The text is null for an external
// entity, which the parser does not fetch.
struct XmlEntity {
    PrString name;
    PrString value;
};

// A parse: what it reads, where it is, and what it holds while it reads.
struct XmlParser {
    const PrXmlHandler* handler;

    // The source that has arrived, its line breaks normalized. The parse keeps all of it, since an error's position and
    // an element's content start are counted from its start.
    TextBuffer source;
    bool carriageReturn;        // true if the last text appended ended in a carriage return
    bool inputEnded;

    PrStringView input;         // the text being read: the source, or an entity's replacement
    size_t at;
    size_t reference;           // the source offset of the outermost reference being expanded
    uint32_t depth;             // how many entities are expanding
    size_t contentAt;           // where the content of the element being ended starts
    Array<XmlOpen> open;
    Array<XmlBinding> bindings;
    Array<XmlEntity> entities;
    TextBuffer text;            // the character data not yet reported
    size_t textAt;              // where in the source it starts
    PrString publicId;          // the doctype's public identifier, which identifies its external subset
    bool started;               // true once the byte order mark and the XML declaration are read
    bool doctype;
    bool root;                  // the document element has started
    bool done;                  // true once the parse has read the end of its input
    bool paused;
    bool running;               // true during prXmlParserRun, whose handler's calls must not append input
    PrStatus status;
    PrStringView message;
    size_t errorAt;

    // How far the parser has looked for the end of the piece of content at `at`, and the quote that it was in there, so
    // that the next look as more input arrives resumes there. Zero while no look has failed.
    size_t scanned;
    char scanQuote;
};

} // namespace pr_impl

// The handle of a parse.
struct PrXmlParser : pr_impl::XmlParser {};

PR_ASSUME_NONNULL_END
