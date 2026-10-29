// Tree construction
//
// html/#tree-construction - https://html.spec.whatwg.org/multipage/parsing.html#tree-construction

#pragma once

#include "pr/array.h"
#include "pr/charset_names.h"
#include "pr/dom.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"

#include "dom/src/dom_node.h"
#include "html/src/html_tokenizer.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Insertion modes

// The insertion modes that html/#parsing-main-inhtml defines rules for. A select has no mode of its own. Its contents
// are parsed in "in body".
enum {
    HtmlInsertionMode_Initial,
    HtmlInsertionMode_BeforeHtml,
    HtmlInsertionMode_BeforeHead,
    HtmlInsertionMode_InHead,
    HtmlInsertionMode_InHeadNoscript,
    HtmlInsertionMode_AfterHead,
    HtmlInsertionMode_InBody,
    HtmlInsertionMode_Text,
    HtmlInsertionMode_InTable,
    HtmlInsertionMode_InTableText,
    HtmlInsertionMode_InCaption,
    HtmlInsertionMode_InColumnGroup,
    HtmlInsertionMode_InTableBody,
    HtmlInsertionMode_InRow,
    HtmlInsertionMode_InCell,
    HtmlInsertionMode_InTemplate,
    HtmlInsertionMode_AfterBody,
    HtmlInsertionMode_InFrameset,
    HtmlInsertionMode_AfterFrameset,
    HtmlInsertionMode_AfterAfterBody,
    HtmlInsertionMode_AfterAfterFrameset,
    HtmlInsertionMode_Count,
};
typedef uint8_t HtmlInsertionMode;

// ---------------------------------------------------------------------------------------------------------------------
// The host

// What the parser asks of the browsing context that hosts it. A null member does nothing: a parser with no `runScript`
// executes nothing.
struct HtmlParserHost {
    void* PR_NULLABLE context;

    // Called for a </script> end tag, with the element that it closed. It may pause the parser (HtmlParser::paused)
    // until the script runs.
    PrStatus (*PR_NULLABLE runScript)(void* PR_NULLABLE context, PrDomElement* script);

    // Called for a meta element that declares the charset of the document (html/#parsing-main-inhead), which changes
    // the encoding if the parse's confidence is tentative (html/#change-the-encoding). Returns true if the document is
    // to be parsed again in that charset, which abandons this parse.
    bool (*PR_NULLABLE changeEncoding)(void* PR_NULLABLE context, PrCharset charset);
};

// What the document allows the parse to do.
enum {
    HtmlParseFlag_Scripting = 1 << 0,                // scripting is enabled for the document
    HtmlParseFlag_DeclarativeShadowRoots = 1 << 1,   // dom/#document-allow-declarative-shadow-roots
};
typedef uint8_t HtmlParseFlags;

// ---------------------------------------------------------------------------------------------------------------------
// The parser

// An attribute of the start tag that a formatting element was created for, as written in the tag.
struct HtmlTokenAttribute {
    PrString name;
    PrString value;
};

// An entry of the list of active formatting elements. The entry holds a reference to its element, and keeps the
// attributes of the start tag that the element was created for. An element recreated for the entry takes these
// attributes, and the Noah's Ark clause compares them (html/#push-onto-the-list-of-active-formatting-elements). A null
// element is a marker.
struct HtmlFormatting {
    PrDomElement* PR_NULLABLE element;
    HtmlTokenAttribute* PR_NULLABLE attributes;   // an allocation of `attributeCount` attributes
    uint32_t attributeCount;
};

// The state of tree construction over one document, with the tokenizer that feeds it.
struct HtmlParser {
    HtmlTokenizer tokenizer;
    PrDomDocument* document;
    Array<HtmlError> errors;
    Array<PrDomElement* PR_NONNULL> open;       // html/#the-stack-of-open-elements
    Array<HtmlFormatting> formatting;           // html/#the-list-of-active-formatting-elements
    Array<HtmlInsertionMode> templateModes;     // html/#stack-of-template-insertion-modes

    // The characters that html/#parsing-main-intabletext holds until it knows where they go.
    TextBuffer pending;

    HtmlParserHost host;
    uint32_t scriptStart;                         // the byte offset after the opening script tag
    PrStatus status;                              // the failure of a script, which stops parsing
    PrDomElement* PR_NULLABLE head;               // the head element pointer (html/#the-element-pointers)
    PrDomElement* PR_NULLABLE form;               // the form element pointer
    PrDomElement* PR_NULLABLE context;            // the context of a fragment parse (html/#parsing-html-fragments)

    // The node that takes what a fragment parse inserts into its root, or null for the root itself. A parse into a live
    // element sets it to the element, so that the element's contents appear as they are parsed. The parser does not
    // retain it.
    PrDomNode* PR_NULLABLE rootTarget;
    HtmlInsertionMode mode;
    HtmlInsertionMode original;                 // the mode that "text" and "in table text" return to
    HtmlParseFlags flags;
    bool framesetOk;                            // the frameset-ok flag (html/#other-parsing-state-flags)
    bool fosterParenting;                       // the foster parenting flag (html/#creating-and-inserting-nodes)

    // True if a line feed that opens the text of a pre, listing or textarea is to be dropped.
    bool skipNewline;

    bool pendingText;                           // true if `pending` holds a character other than ASCII whitespace

    // True if a script stopped the parse until the script runs (html/#pending-parsing-blocking-script). Input appended
    // in the meantime waits.
    bool paused;

    // The bytes of the input that document.write() inserted last, at the read position. A meta element in them declares
    // no charset, since it is not in the bytes that a charset decodes.
    uint32_t writtenStart;
    uint32_t writtenEnd;

    bool stopped;                               // true once the parse reaches the end of its input (html/#stop-parsing)
    bool abandoned;                             // true once a change of encoding abandons the parse
};

// Initializes an empty parser over `document`.
void htmlParserInit(HtmlParser* parser, PrDomDocument* document, HtmlParseFlags flags);

// Frees everything that the parser owns, releases the elements that it holds, and leaves it usable.
void htmlParserReset(HtmlParser* parser);

// Parses `text` into the parser's document (html/#parsing).
void htmlParse(HtmlParser* parser, PrStringView text);

// Parses `text` as the contents of `context` (html/#parsing-html-fragments) into a fragment. The caller releases the
// fragment's one reference. The fragment belongs to the parser's document, which also provides the quirks mode. The
// spec instead creates a separate document and adopts the result out of it.
PrDomNode* htmlParseFragment(HtmlParser* parser, PrDomElement* context, PrStringView text);

// Prepares the parser to parse the contents of `context` (html/#parsing-html-fragments), and returns the root element
// that the contents are inserted into, which has no parent and which the stack of open elements holds. The tokenizer
// must have started, since the context sets its state.
PrDomElement* htmlParserBeginFragment(HtmlParser* parser, PrDomElement* context);

// Processes the tokenizer's current token as tree construction processes each token that the tokenizer reads. The token
// is one that the tokenizer read from its input, or one that a caller made (htmlTokenizerMakeTag,
// htmlTokenizerMakeCharacters), which is processed as if the tokenizer had read it at its read point.
void htmlParserProcess(HtmlParser* parser);

// The state of a parse at rest, apart from its DOM: no formatting element is active, no text waits for a table, and the
// tokenizer is idle (htmlTokenizerIsIdle). A parse at rest goes on in the same way whatever tokens brought it there.
struct HtmlParserRest {
    uint32_t depth;                 // the number of open elements, which are the first elements of the stack
    uint32_t templates;             // the number of template insertion modes
    HtmlInsertionMode mode;
    PrDomElement* PR_NULLABLE form; // the form element pointer, which the parser holds
    bool framesetOk;
};

// Returns the parse's state as a state of rest, whether or not the parse is at rest.
HtmlParserRest htmlParserRest(const HtmlParser* parser);

// Returns true if the parse is at rest in `rest`, whatever its frameset-ok flag.
bool htmlParserIsAtRest(const HtmlParser* parser, const HtmlParserRest& rest);

// Returns the parse to rest in `rest`, which htmlParserRest returned for a point at which the stack of open elements
// held the elements that it holds now up to `rest.depth`. Pops the open elements above that depth, clears the list of
// active formatting elements, and empties the tokenizer's input. The DOM is the caller's to restore.
void htmlParserRestore(HtmlParser* parser, const HtmlParserRest& rest);

// Inserts `text` into the input at the parse's read position, for document.write() (html/#document.write()). Valid only
// while HtmlParserHost::runScript is on the stack.
void htmlParserWrite(HtmlParser* parser, PrStringView text);

// Empties the parser's input and opens it, so that it stays open until htmlParserClose. This makes the parser a
// script-created one (html/#script-created-parser) for document.open(), and starts a parse that a caller feeds.
void htmlParserOpen(HtmlParser* parser);

// Appends `text` to the open input and parses as far as it can. This is document.write() on a script-created parser,
// outside the scripts that the parser runs, and the raw HTML of a parse that a caller feeds.
void htmlParserAppend(HtmlParser* parser, PrStringView text);

// Closes the input and parses to its end, for document.close().
void htmlParserClose(HtmlParser* parser);

// Continues a parse that a script paused, as far as its input reaches.
void htmlParserResume(HtmlParser* parser);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
