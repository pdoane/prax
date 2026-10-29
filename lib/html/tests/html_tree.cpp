// The tree construction suite, reported in its own JSON format.
//
// web-platform-tests carries the html5lib tree construction tests as .dat files. Each pairs an input with the tree that
// it parses to, printed one node per line. This program reads framed cases from stdin and writes one JSON result per
// line. tools/suite/html_tree.py matches the results to the cases.
//
// html/#tree-construction - https://html.spec.whatwg.org/multipage/parsing.html#tree-construction

#include "pr/assert.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

#include "html/src/html_parser.h"
#include "tools/harness/harness.h"

#include <stdio.h>

using namespace harness;

using namespace pr_impl;

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// The printed tree
//
// Each node is one line starting with "| ", indented two spaces per level, which is the corpus's own format.

void writeIndent(TextBuffer* out, uint32_t depth) {
    textAppend(out, "| ");
    for (uint32_t i = 0; i < depth; ++i)
        textAppend(out, "  ");
}

PrStringView namespacePrefix(DomNamespaceId space) {
    switch (space) {
        case PrDomNamespace_MathMl: return "math ";
        case PrDomNamespace_Svg:    return "svg ";
        case PrDomNamespace_XLink:  return "xlink ";
        case PrDomNamespace_Xml:    return "xml ";
        case PrDomNamespace_XmlNs:  return "xmlns ";
        default:                  return "";
    }
}

// Writes the attributes, in the order that the corpus prints them, which is sorted by the name that it writes.
void writeAttributes(TextBuffer* out, const PrDomElement* element, uint32_t depth) {
    Array<const PrDomAttr* PR_NONNULL> order;
    for (const PrDomAttr* attribute = element->firstAttribute; attribute != nullptr;
         attribute = static_cast<const PrDomAttr*>(attribute->next)) {
        uint32_t at = order.count;
        while (at != 0) {
            const PrDomAttr* before = order.data[at - 1];
            int side = stringCompare(prStringView(&before->name), prStringView(&attribute->name));
            if (side < 0 || (side == 0 && before->space <= attribute->space))
                break;
            --at;
        }
        order.insert(at, attribute);
    }

    for (uint32_t i = 0; i < order.count; ++i) {
        const PrDomAttr* attribute = order.data[i];
        writeIndent(out, depth);
        textAppend(out, namespacePrefix(attribute->space));
        textAppend(out, prStringView(&attribute->name));
        textAppend(out, "=\"");
        textAppend(out, prStringView(&attribute->value));
        textAppend(out, "\"\n");
    }
    order.reset();
}

void writeNode(TextBuffer* out, const PrDomNode* node, uint32_t depth);

void writeChildren(TextBuffer* out, const PrDomNode* node, uint32_t depth) {
    for (const PrDomNode* child = node->first; child != nullptr; child = child->next)
        writeNode(out, child, depth);
}

void writeNode(TextBuffer* out, const PrDomNode* node, uint32_t depth) {
    switch (node->type) {
        case PrDomNodeType_Element: {
            const PrDomElement* element = static_cast<const PrDomElement*>(node);
            writeIndent(out, depth);
            textAppendChar(out, '<');
            textAppend(out, namespacePrefix(element->space));
            textAppend(out, domElementName(element));
            textAppend(out, ">\n");

            writeAttributes(out, element, depth + 1);
            const PrDomNode* contents = domElementContents(element);
            if (contents != nullptr) {
                writeIndent(out, depth + 1);
                textAppend(out, "content\n");
                writeChildren(out, contents, depth + 2);
            }
            writeChildren(out, node, depth + 1);
            return;
        }

        case PrDomNodeType_Text:
            writeIndent(out, depth);
            textAppendChar(out, '"');
            textAppend(out, prStringView(&static_cast<const PrDomText*>(node)->data));
            textAppend(out, "\"\n");
            return;

        case PrDomNodeType_Comment:
            writeIndent(out, depth);
            textAppend(out, "<!-- ");
            textAppend(out, prStringView(&static_cast<const PrDomComment*>(node)->data));
            textAppend(out, " -->\n");
            return;

        case PrDomNodeType_ProcessingInstruction: {
            const PrDomProcessingInstruction* instruction =
                static_cast<const PrDomProcessingInstruction*>(node);
            writeIndent(out, depth);
            textAppend(out, "<?");
            textAppend(out, prStringView(&instruction->target));
            textAppendChar(out, ' ');
            textAppend(out, prStringView(&instruction->data));
            textAppend(out, "?>\n");
            return;
        }

        case PrDomNodeType_DocumentType: {
            const PrDomDocumentType* doctype = static_cast<const PrDomDocumentType*>(node);
            writeIndent(out, depth);
            textAppend(out, "<!DOCTYPE ");
            textAppend(out, prStringView(&doctype->name));
            if (prStringView(&doctype->publicId).size != 0
                || prStringView(&doctype->systemId).size != 0) {
                textAppend(out, " \"");
                textAppend(out, prStringView(&doctype->publicId));
                textAppend(out, "\" \"");
                textAppend(out, prStringView(&doctype->systemId));
                textAppendChar(out, '"');
            }
            textAppend(out, ">\n");
            return;
        }

        default:
            writeChildren(out, node, depth);
            return;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Cases

DomNamespaceId namespaceFromName(PrStringView text) {
    if (text == "svg")
        return PrDomNamespace_Svg;
    if (text == "math")
        return PrDomNamespace_MathMl;
    return PrDomNamespace_Html;
}

// A case's fields are its scripting flag, "on" or "off", and its context element's namespace and name, which are "-"
// for a document parse.
void run(TextBuffer* out, const HarnessCase& test) {
    PrStringView space = test.fields[1];
    PrStringView name = test.fields[2];

    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    HtmlParser parser = {};
    htmlParserInit(&parser, document, test.fields[0] == "on" ? HtmlParseFlag_Scripting : 0);

    PrStringView input = textView(&test.input);
    const PrDomNode* printed = document;
    if (space == "-") {
        htmlParse(&parser, input);
    } else {
        PrDomElement* context =
            domElementCreate(document, namespaceFromName(space), domTagFromText(name), name);
        printed = htmlParseFragment(&parser, context, input);
    }

    TextBuffer tree;
    writeChildren(&tree, printed, 0);

    textAppend(out, "{\"document\": ");
    harnessWriteString(out, textView(&tree));
    textAppend(out, ", \"errors\": ");
    harnessWriteNumber(out, parser.errors.count);
    textAppendChar(out, '}');

    textReset(&tree);
    htmlParserReset(&parser);
    domNodeRelease(document);
}

} // namespace

int main() {
    prCrashHandler();
    HarnessCase test = {};
    TextBuffer out;

    while (harnessReadCase(&test)) {
        if (test.fieldCount != 3) {
            fprintf(stderr, "a case names a scripting flag and a context element\n");
            return 2;
        }
        textClear(&out);
        run(&out, test);
        harnessWriteLine(textView(&out));
    }

    harnessCaseReset(&test);
    textReset(&out);
    return 0;
}
