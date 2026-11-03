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

void writeIndent(PrTextBuffer* out, uint32_t depth) {
    prTextBufferAppend(out, "| ");
    for (uint32_t i = 0; i < depth; ++i)
        prTextBufferAppend(out, "  ");
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
void writeAttributes(PrTextBuffer* out, const PrDomElement* element, uint32_t depth) {
    RawArray<const PrDomAttr* PR_NONNULL> order;
    for (const PrDomAttr* attribute = element->firstAttribute; attribute != nullptr;
            attribute = static_cast<const PrDomAttr*>(attribute->next)) {
        uint32_t at = order.count;
        while (at != 0) {
            const PrDomAttr* before = order.data[at - 1];
            int side = prStringCompare(prRawStringView(&before->name), prRawStringView(&attribute->name));
            if (side < 0 || (side == 0 && before->space <= attribute->space))
                break;
            --at;
        }
        order.insert(at, attribute);
    }

    for (uint32_t i = 0; i < order.count; ++i) {
        const PrDomAttr* attribute = order.data[i];
        writeIndent(out, depth);
        prTextBufferAppend(out, namespacePrefix(attribute->space));
        prTextBufferAppend(out, prRawStringView(&attribute->name));
        prTextBufferAppend(out, "=\"");
        prTextBufferAppend(out, prRawStringView(&attribute->value));
        prTextBufferAppend(out, "\"\n");
    }
    order.reset();
}

void writeNode(PrTextBuffer* out, const PrDomNode* node, uint32_t depth);

void writeChildren(PrTextBuffer* out, const PrDomNode* node, uint32_t depth) {
    for (const PrDomNode* child = node->first; child != nullptr; child = child->next)
        writeNode(out, child, depth);
}

void writeNode(PrTextBuffer* out, const PrDomNode* node, uint32_t depth) {
    switch (node->type) {
        case PrDomNodeType_Element: {
            const PrDomElement* element = static_cast<const PrDomElement*>(node);
            writeIndent(out, depth);
            prTextBufferAppendChar(out, '<');
            prTextBufferAppend(out, namespacePrefix(element->space));
            prTextBufferAppend(out, domElementName(element));
            prTextBufferAppend(out, ">\n");

            writeAttributes(out, element, depth + 1);
            const PrDomNode* contents = domElementContents(element);
            if (contents != nullptr) {
                writeIndent(out, depth + 1);
                prTextBufferAppend(out, "content\n");
                writeChildren(out, contents, depth + 2);
            }
            writeChildren(out, node, depth + 1);
            return;
        }

        case PrDomNodeType_Text:
            writeIndent(out, depth);
            prTextBufferAppendChar(out, '"');
            prTextBufferAppend(out, prRawStringView(&static_cast<const PrDomText*>(node)->data));
            prTextBufferAppend(out, "\"\n");
            return;

        case PrDomNodeType_Comment:
            writeIndent(out, depth);
            prTextBufferAppend(out, "<!-- ");
            prTextBufferAppend(out, prRawStringView(&static_cast<const PrDomComment*>(node)->data));
            prTextBufferAppend(out, " -->\n");
            return;

        case PrDomNodeType_ProcessingInstruction: {
            const PrDomProcessingInstruction* instruction = static_cast<const PrDomProcessingInstruction*>(node);
            writeIndent(out, depth);
            prTextBufferAppend(out, "<?");
            prTextBufferAppend(out, prRawStringView(&instruction->target));
            prTextBufferAppendChar(out, ' ');
            prTextBufferAppend(out, prRawStringView(&instruction->data));
            prTextBufferAppend(out, "?>\n");
            return;
        }

        case PrDomNodeType_DocumentType: {
            const PrDomDocumentType* doctype = static_cast<const PrDomDocumentType*>(node);
            writeIndent(out, depth);
            prTextBufferAppend(out, "<!DOCTYPE ");
            prTextBufferAppend(out, prRawStringView(&doctype->name));
            if (prRawStringView(&doctype->publicId).size != 0 || prRawStringView(&doctype->systemId).size != 0) {
                prTextBufferAppend(out, " \"");
                prTextBufferAppend(out, prRawStringView(&doctype->publicId));
                prTextBufferAppend(out, "\" \"");
                prTextBufferAppend(out, prRawStringView(&doctype->systemId));
                prTextBufferAppendChar(out, '"');
            }
            prTextBufferAppend(out, ">\n");
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
void run(PrTextBuffer* out, const HarnessCase& test) {
    PrStringView space = test.fields[1];
    PrStringView name = test.fields[2];

    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    HtmlParser parser = {};
    htmlParserInit(&parser, document, test.fields[0] == "on" ? HtmlParseFlag_Scripting : 0);

    PrStringView input = prTextBufferView(&test.input);
    const PrDomNode* printed = document;
    if (space == "-") {
        htmlParse(&parser, input);
    } else {
        PrDomElement* context = domElementCreate(document, namespaceFromName(space), domTagFromText(name), name);
        printed = htmlParseFragment(&parser, context, input);
    }

    PrTextBuffer tree = {};
    writeChildren(&tree, printed, 0);

    prTextBufferAppend(out, "{\"document\": ");
    harnessWriteString(out, prTextBufferView(&tree));
    prTextBufferAppend(out, ", \"errors\": ");
    harnessWriteNumber(out, parser.errors.count);
    prTextBufferAppendChar(out, '}');

    prTextBufferReset(&tree);
    htmlParserReset(&parser);
    domNodeRelease(document);
}

} // namespace

int main() {
    prCrashHandler();
    HarnessCase test = {};
    PrTextBuffer out = {};

    while (harnessReadCase(&test)) {
        if (test.fieldCount != 3) {
            fprintf(stderr, "a case names a scripting flag and a context element\n");
            return 2;
        }
        prTextBufferClear(&out);
        run(&out, test);
        harnessWriteLine(prTextBufferView(&out));
    }

    harnessCaseReset(&test);
    prTextBufferReset(&out);
    return 0;
}
