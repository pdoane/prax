#include "html/src/html_serialize.h"

#include "pr/string_ops.h"

#include "dom/src/dom_node.h"

namespace pr_impl {
namespace {

// Returns true if serialization skips the element's children. These are the void elements (html/#void-elements), and
// the legacy elements whose children serialization also skips.
bool skipsChildren(const PrDomElement* element) {
    if (element->space != PrDomNamespace_Html)
        return false;
    switch (element->tag) {
        case PrDomTag_Area:
        case PrDomTag_Base:
        case PrDomTag_Basefont:
        case PrDomTag_Bgsound:
        case PrDomTag_Br:
        case PrDomTag_Col:
        case PrDomTag_Embed:
        case PrDomTag_Frame:
        case PrDomTag_Hr:
        case PrDomTag_Img:
        case PrDomTag_Input:
        case PrDomTag_Keygen:
        case PrDomTag_Link:
        case PrDomTag_Meta:
        case PrDomTag_Param:
        case PrDomTag_Source: case PrDomTag_Track: case PrDomTag_Wbr:
            return true;
        default:
            return false;
    }
}

// Returns true if text in `parent` is written unescaped. This holds for the raw text elements, and for the legacy
// elements that the parser reads as raw text. noscript is among them if scripting is enabled, which no serialization
// here assumes.
bool isRawText(const PrDomNode* PR_NULLABLE parent) {
    if (parent == nullptr || parent->type != PrDomNodeType_Element
        || parent->space != PrDomNamespace_Html)
        return false;
    switch (static_cast<const PrDomElement*>(parent)->tag) {
        case PrDomTag_Style:
        case PrDomTag_Script:
        case PrDomTag_Xmp:
        case PrDomTag_Iframe:
        case PrDomTag_Noembed:
        case PrDomTag_Noframes: case PrDomTag_Plaintext:
            return true;
        default:
            return false;
    }
}

// Appends `text` escaped by html/#escapingString, in attribute mode if `attribute` is set.
void appendEscaped(TextBuffer* out, PrStringView text, bool attribute) {
    size_t run = 0;
    for (size_t i = 0; i < text.size; ++i) {
        PrStringView replacement = {};
        size_t width = 1;
        char c = text.data[i];
        if (c == '&')
            replacement = "&amp;";
        else if (c == '<')
            replacement = "&lt;";
        else if (c == '>')
            replacement = "&gt;";
        else if (c == '"' && attribute)
            replacement = "&quot;";
        else if (c == '\xc2' && i + 1 < text.size && text.data[i + 1] == '\xa0') {
            replacement = "&nbsp;";
            width = 2;
        }
        if (replacement.size == 0)
            continue;
        textAppend(out, text.data + run, uint32_t(i - run));
        textAppend(out, replacement);
        i += width - 1;
        run = i + 1;
    }
    textAppend(out, text.data + run, uint32_t(text.size - run));
}

// Appends an attribute's serialized name, as html/#html-fragment-serialisation-algorithm writes it.
void appendAttributeName(TextBuffer* out, const PrDomAttr* attribute) {
    PrStringView name = prStringView(&attribute->name);
    switch (attribute->space) {
        case PrDomNamespace_Xml:
            textAppend(out, "xml:");
            break;
        case PrDomNamespace_XmlNs:
            if (name != "xmlns")
                textAppend(out, "xmlns:");
            break;
        case PrDomNamespace_XLink:
            textAppend(out, "xlink:");
            break;
        default:
            break;
    }
    textAppend(out, name);
}

} // namespace

void htmlSerializeNode(TextBuffer* out, const PrDomNode* node) {
    switch (node->type) {
        case PrDomNodeType_Element: {
            const PrDomElement* element = static_cast<const PrDomElement*>(node);
            PrStringView name = domElementName(element);
            textAppend(out, "<");
            textAppend(out, name);
            for (const PrDomAttr* attribute = element->firstAttribute; attribute != nullptr;
                 attribute = static_cast<const PrDomAttr*>(attribute->next)) {
                textAppend(out, " ");
                appendAttributeName(out, attribute);
                textAppend(out, "=\"");
                appendEscaped(out, prStringView(&attribute->value), true);
                textAppend(out, "\"");
            }
            textAppend(out, ">");
            if (skipsChildren(element))
                return;
            htmlSerializeChildren(out, node);
            textAppend(out, "</");
            textAppend(out, name);
            textAppend(out, ">");
            return;
        }
        case PrDomNodeType_Text:
        case PrDomNodeType_CdataSection: {
            PrStringView data = prStringView(&static_cast<const PrDomCharacterData*>(node)->data);
            if (isRawText(node->parent))
                textAppend(out, data);
            else
                appendEscaped(out, data, false);
            return;
        }
        case PrDomNodeType_Comment:
            textAppend(out, "<!--");
            textAppend(out, prStringView(&static_cast<const PrDomCharacterData*>(node)->data));
            textAppend(out, "-->");
            return;
        case PrDomNodeType_ProcessingInstruction: {
            const PrDomProcessingInstruction* instruction = static_cast<const PrDomProcessingInstruction*>(node);
            textAppend(out, "<?");
            textAppend(out, prStringView(&instruction->target));
            textAppend(out, " ");
            textAppend(out, prStringView(&instruction->data));
            textAppend(out, ">");
            return;
        }
        case PrDomNodeType_DocumentType:
            textAppend(out, "<!DOCTYPE ");
            textAppend(out, prStringView(&static_cast<const PrDomDocumentType*>(node)->name));
            textAppend(out, ">");
            return;
        default:
            htmlSerializeChildren(out, node);
            return;
    }
}

void htmlSerializeChildren(TextBuffer* out, const PrDomNode* node) {
    if (domIsHtml(domElementFromNode(node), PrDomTag_Template)) {
        const PrDomNode* contents = domElementContents(static_cast<const PrDomElement*>(node));
        if (contents == nullptr)
            return;
        node = contents;
    }
    for (const PrDomNode* child = node->first; child != nullptr; child = child->next)
        htmlSerializeNode(out, child);
}

} // namespace pr_impl
