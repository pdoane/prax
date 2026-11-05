// Document structure: what a document's content means, as a structure tree whose keys are the document's nodes.
//
// Roles follow WAI-ARIA 1.2 and HTML-AAM's element role mappings, names follow accname 1.2, and the structure types are
// PDF 2.0's (ISO 32000-2, 14.8.4).

#include "pr/html_structure.h"

#include "pr/ascii.h"
#include "pr/css_presentation.h"
#include "pr/dom.h"
#include "pr/html.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/string_ops.h"
#include "pr/url.h"

#include "css/src/css_presentation.h"
#include "css/src/css_presentation_page.h"

namespace pr_impl {

namespace {

// What an element adds to the structure.
enum {
    StructureKind_Contents,     // no node: its content joins its parent's
    StructureKind_Node,         // a node of its type, which holds its content
    StructureKind_Leaf,         // a node of its type whose content is the element itself, as a replaced element's
    StructureKind_Skipped,      // nothing, and nothing of its content
};
typedef uint8_t StructureKind;

struct StructureRole {
    StructureKind kind;
    PrDrawStructureType type;
};

// An element of aria-owns, which the walk moves to its owner.
struct Owned {
    const PrDomElement* owner;
    const PrDomElement* element;
};

struct StructurePass {
    PrDrawStructure* structure;
    PrCssPresentation* presentation;
    PrDomDocument* document;
    PrRawString documentUrl;            // without its fragment
    RawArray<Owned> owned;
    RawArray<const PrDomElement*> walked;   // the owned elements already walked, each walked once
};

uint64_t keyOf(const PrDomNode* node) {
    return uint64_t(uintptr_t(node));
}

PrDomNode* asNode(const PrDomElement* element) {
    return prDomElementAsNode(const_cast<PrDomElement*>(element));
}

PrDomElement* PR_NULLABLE asElement(PrDomNode* node) {
    return prDomElementQuery(prDomNodeAsEventTarget(node));
}

bool isHtml(const PrDomElement* element) {
    return prDomElementNamespace(element) == PrDomNamespace_Html;
}

// Returns the text with each run of ASCII white space as one space, and none at either end.
void collapse(PrStringView text, RawArray<char>& out) {
    out.clear();
    bool space = false;
    for (size_t i = 0; i < text.size; ++i) {
        char c = text.data[i];
        if (prAsciiIsSpace(c)) {
            space = out.count != 0;
            continue;
        }
        if (space)
            out.push(' ');
        space = false;
        out.push(c);
    }
}

bool isBlank(PrStringView text) {
    for (size_t i = 0; i < text.size; ++i) {
        if (!prAsciiIsSpace(text.data[i]))
            return false;
    }
    return true;
}

// Returns true if the structure leaves out the element's whole subtree. An element is left out if aria-hidden hides it
// from a reader, or if its subtree renders nothing where it is. A subtree renders nothing if its element has no box and
// is not an element whose children render without one, as an element of display contents is. A running element renders
// only as copies in page-margin boxes, whose content is an artifact (css-gcpm-3/#running-elements), so it is left out
// too.
bool isHidden(const StructurePass& pass, const PrDomElement* element) {
    PrStringView hidden = prDomElementGetAttribute(element, "aria-hidden");
    if (hidden.data != nullptr && prAsciiEqualsFold(hidden, "true"))
        return true;
    if (pass.presentation == nullptr)
        return false;
    const CssComputedStyle* style = cssPresentationStyle(pass.presentation, element);
    CssValueRun position = style != nullptr ? cssComputedValue(style, PrCssProperty_Position) : CssValueRun{};
    if (position.count != 0 && cssValueIsFunction(position.values, PrCssKeyword_Running))
        return true;
    if (prCssPresentationLayoutNode(pass.presentation, asNode(element)) != kPrLayoutNodeNone)
        return false;
    return !cssPresentationIsInteractive(pass.presentation, element);
}

// Returns the role that an explicit role token names, or Count if the token names none that maps to a type.
StructureRole explicitRole(PrStringView token, const PrDomElement* element) {
    struct Entry {
        PrStringView name;
        StructureKind kind;
        PrDrawStructureType type;
    };
    static const Entry kRoles[] = {
        {"article", StructureKind_Node, PrDrawStructureType_Sect},
        {"banner", StructureKind_Node, PrDrawStructureType_Sect},
        {"blockquote", StructureKind_Node, PrDrawStructureType_BlockQuote},
        {"caption", StructureKind_Node, PrDrawStructureType_Caption},
        {"cell", StructureKind_Node, PrDrawStructureType_TD},
        {"code", StructureKind_Node, PrDrawStructureType_Code},
        {"columnheader", StructureKind_Node, PrDrawStructureType_TH},
        {"complementary", StructureKind_Node, PrDrawStructureType_Aside},
        {"contentinfo", StructureKind_Node, PrDrawStructureType_Sect},
        {"definition", StructureKind_Node, PrDrawStructureType_LBody},
        {"document", StructureKind_Node, PrDrawStructureType_Part},
        {"emphasis", StructureKind_Node, PrDrawStructureType_Em},
        {"figure", StructureKind_Node, PrDrawStructureType_Figure},
        {"form", StructureKind_Node, PrDrawStructureType_Sect},
        {"generic", StructureKind_Contents, PrDrawStructureType_Div},
        {"grid", StructureKind_Node, PrDrawStructureType_Table},
        {"gridcell", StructureKind_Node, PrDrawStructureType_TD},
        {"group", StructureKind_Node, PrDrawStructureType_Div},
        {"image", StructureKind_Leaf, PrDrawStructureType_Figure},
        {"img", StructureKind_Leaf, PrDrawStructureType_Figure},
        {"link", StructureKind_Node, PrDrawStructureType_Link},
        {"list", StructureKind_Node, PrDrawStructureType_L},
        {"listitem", StructureKind_Node, PrDrawStructureType_LI},
        {"main", StructureKind_Node, PrDrawStructureType_Sect},
        {"math", StructureKind_Leaf, PrDrawStructureType_Formula},
        {"navigation", StructureKind_Node, PrDrawStructureType_Sect},
        {"none", StructureKind_Contents, PrDrawStructureType_Div},
        {"note", StructureKind_Node, PrDrawStructureType_Note},
        {"paragraph", StructureKind_Node, PrDrawStructureType_P},
        {"presentation", StructureKind_Contents, PrDrawStructureType_Div},
        {"region", StructureKind_Node, PrDrawStructureType_Sect},
        {"row", StructureKind_Node, PrDrawStructureType_TR},
        {"rowgroup", StructureKind_Node, PrDrawStructureType_TBody},
        {"rowheader", StructureKind_Node, PrDrawStructureType_TH},
        {"search", StructureKind_Node, PrDrawStructureType_Sect},
        {"separator", StructureKind_Skipped, PrDrawStructureType_Artifact},
        {"strong", StructureKind_Node, PrDrawStructureType_Strong},
        {"table", StructureKind_Node, PrDrawStructureType_Table},
        {"term", StructureKind_Node, PrDrawStructureType_Lbl},
    };
    if (token == "heading") {
        // A heading's level is its aria-level, or the rank of an h1 to h6, or 2 (wai-aria-1.2/#heading).
        uint32_t level = 2;
        PrDomTag tag = prDomElementTag(element);
        if (tag >= PrDomTag_H1 && tag <= PrDomTag_H6)
            level = uint32_t(tag - PrDomTag_H1) + 1;
        PrStringView value = prDomElementGetAttribute(element, "aria-level");
        if (value.size == 1 && value.data[0] >= '1' && value.data[0] <= '9')
            level = uint32_t(value.data[0] - '0');
        level = level > 6 ? 6 : level;
        return StructureRole{StructureKind_Node, PrDrawStructureType(PrDrawStructureType_H1 + level - 1)};
    }
    for (const Entry& entry : kRoles) {
        if (entry.name == token)
            return StructureRole{entry.kind, entry.type};
    }
    return StructureRole{StructureKind_Contents, PrDrawStructureType_Count};
}

// Returns the role of an HTML element that states none, as HTML-AAM maps it.
StructureRole implicitRole(const PrDomElement* element) {
    auto node = [](PrDrawStructureType type) { return StructureRole{StructureKind_Node, type}; };
    switch (prDomElementTag(element)) {
        case PrDomTag_A: {
            PrStringView href = prDomElementGetAttribute(element, "href");
            if (href.data == nullptr)
                return StructureRole{StructureKind_Contents, PrDrawStructureType_Span};
            return node(PrDrawStructureType_Link);
        }
        case PrDomTag_Abbr: return node(PrDrawStructureType_Span);
        case PrDomTag_Article:
        case PrDomTag_Footer:
        case PrDomTag_Form:
        case PrDomTag_Header:
        case PrDomTag_Main:
        case PrDomTag_Nav:
        case PrDomTag_Search:
        case PrDomTag_Section:
            return node(PrDrawStructureType_Sect);
        case PrDomTag_Aside: return node(PrDrawStructureType_Aside);
        case PrDomTag_Blockquote: return node(PrDrawStructureType_BlockQuote);
        case PrDomTag_Caption:
        case PrDomTag_Figcaption:
            return node(PrDrawStructureType_Caption);
        case PrDomTag_Code: return node(PrDrawStructureType_Code);
        case PrDomTag_Dl:
        case PrDomTag_Menu:
        case PrDomTag_Ol:
        case PrDomTag_Ul:
            return node(PrDrawStructureType_L);
        case PrDomTag_Dd:
        case PrDomTag_Dt:
        case PrDomTag_Li:
            return node(PrDrawStructureType_LI);
        case PrDomTag_Em: return node(PrDrawStructureType_Em);
        case PrDomTag_Figure: return node(PrDrawStructureType_Figure);
        case PrDomTag_H1:
        case PrDomTag_H2:
        case PrDomTag_H3:
        case PrDomTag_H4:
        case PrDomTag_H5:
        case PrDomTag_H6:
            return node(PrDrawStructureType(PrDrawStructureType_H1 + (prDomElementTag(element) - PrDomTag_H1)));
        case PrDomTag_Img:
        case PrDomTag_Svg:
        case PrDomTag_Canvas:
            return StructureRole{StructureKind_Leaf, PrDrawStructureType_Figure};
        case PrDomTag_Math: return StructureRole{StructureKind_Leaf, PrDrawStructureType_Formula};
        case PrDomTag_P:
        case PrDomTag_Pre:
            return node(PrDrawStructureType_P);
        case PrDomTag_Q: return node(PrDrawStructureType_Quote);
        case PrDomTag_Rb: return node(PrDrawStructureType_RB);
        case PrDomTag_Rp: return node(PrDrawStructureType_RP);
        case PrDomTag_Rt: return node(PrDrawStructureType_RT);
        case PrDomTag_Ruby: return node(PrDrawStructureType_Ruby);
        case PrDomTag_Strong: return node(PrDrawStructureType_Strong);
        case PrDomTag_Table: return node(PrDrawStructureType_Table);
        case PrDomTag_Tbody: return node(PrDrawStructureType_TBody);
        case PrDomTag_Td: return node(PrDrawStructureType_TD);
        case PrDomTag_Tfoot: return node(PrDrawStructureType_TFoot);
        case PrDomTag_Th: return node(PrDrawStructureType_TH);
        case PrDomTag_Thead: return node(PrDrawStructureType_THead);
        case PrDomTag_Tr: return node(PrDrawStructureType_TR);
        // Elements whose content a reader never reads, and separators, which are decoration.
        case PrDomTag_Head:
        case PrDomTag_Hr:
        case PrDomTag_Noscript:
        case PrDomTag_Script:
        case PrDomTag_Style:
        case PrDomTag_Template:
        case PrDomTag_Title:
            return StructureRole{StructureKind_Skipped, PrDrawStructureType_Artifact};
        default: return StructureRole{StructureKind_Contents, PrDrawStructureType_Div};
    }
}

StructureRole roleOf(const PrDomElement* element) {
    PrStringView roles = prDomElementGetAttribute(element, "role");
    for (size_t at = 0; roles.data != nullptr && at < roles.size;) {
        while (at < roles.size && prAsciiIsSpace(roles.data[at]))
            ++at;
        size_t start = at;
        while (at < roles.size && !prAsciiIsSpace(roles.data[at]))
            ++at;
        if (at == start)
            break;
        StructureRole role = explicitRole(PrStringView{roles.data + start, at - start}, element);
        if (role.type != PrDrawStructureType_Count)
            return role;
    }
    if (!isHtml(element)) {
        // The outermost svg element of HTML content is an image, and so is a math element.
        PrDomTag tag = prDomElementTag(element);
        if (tag == PrDomTag_Svg || tag == PrDomTag_Math)
            return implicitRole(element);
        return StructureRole{StructureKind_Contents, PrDrawStructureType_Div};
    }
    return implicitRole(element);
}

// Returns true if the element's box is a block in its parent's flow, by the display that HTML's rendering gives it.
bool isBlockLevel(const PrDomElement* element) {
    switch (prDomElementTag(element)) {
        case PrDomTag_Address:
        case PrDomTag_Article:
        case PrDomTag_Aside:
        case PrDomTag_Blockquote:
        case PrDomTag_Center:
        case PrDomTag_Dd:
        case PrDomTag_Details:
        case PrDomTag_Dialog:
        case PrDomTag_Div:
        case PrDomTag_Dl:
        case PrDomTag_Dt:
        case PrDomTag_Fieldset:
        case PrDomTag_Figcaption:
        case PrDomTag_Figure:
        case PrDomTag_Footer:
        case PrDomTag_Form:
        case PrDomTag_H1:
        case PrDomTag_H2:
        case PrDomTag_H3:
        case PrDomTag_H4:
        case PrDomTag_H5:
        case PrDomTag_H6:
        case PrDomTag_Header:
        case PrDomTag_Hgroup:
        case PrDomTag_Hr:
        case PrDomTag_Legend:
        case PrDomTag_Li:
        case PrDomTag_Main:
        case PrDomTag_Menu:
        case PrDomTag_Nav:
        case PrDomTag_Ol:
        case PrDomTag_P:
        case PrDomTag_Pre:
        case PrDomTag_Search:
        case PrDomTag_Section:
        case PrDomTag_Summary:
        case PrDomTag_Table:
        case PrDomTag_Ul:
            return isHtml(element);
        default:
            return false;
    }
}

// Returns true if content of the type holds paragraphs, so that text directly in it takes a paragraph of its own.
bool isGrouping(PrDrawStructureType type) {
    switch (type) {
        case PrDrawStructureType_Document:
        case PrDrawStructureType_DocumentFragment:
        case PrDrawStructureType_Part:
        case PrDrawStructureType_Sect:
        case PrDrawStructureType_Div:
        case PrDrawStructureType_Aside:
        case PrDrawStructureType_BlockQuote:
        case PrDrawStructureType_Note:
            return true;
        default:
            return false;
    }
}

// Appends the collapsed text of the elements that the ids of `ids` name, in order (accname-1.2/#computation-steps, step
// 2B).
void labelledBy(const StructurePass& pass, PrStringView ids, RawArray<char>& out) {
    RawArray<char> part;
    for (size_t at = 0; at < ids.size;) {
        while (at < ids.size && prAsciiIsSpace(ids.data[at]))
            ++at;
        size_t start = at;
        while (at < ids.size && !prAsciiIsSpace(ids.data[at]))
            ++at;
        if (at == start)
            break;
        PrDomElement* labelling = prDomNodeGetElementById(prDomDocumentAsNode(pass.document),
            PrStringView{ids.data + start, at - start});
        if (labelling == nullptr)
            continue;
        PrRawString text = prDomNodeTextContent(asNode(labelling));
        collapse(prRawStringView(&text), part);
        prRawStringReset(&text);
        if (part.count == 0)
            continue;
        if (out.count != 0)
            out.push(' ');
        out.append(part.data, part.count);
    }
    part.reset();
}

// Writes the element's accessible name to `out`: from aria-labelledby, aria-label, the alternative text of an image, or
// its title, in that order (accname-1.2/#computation-steps). Content is the name of no element here, since the content
// of a node that holds content is in the structure.
void nameOf(const StructurePass& pass, const PrDomElement* element, RawArray<char>& out) {
    out.clear();
    PrStringView ids = prDomElementGetAttribute(element, "aria-labelledby");
    if (ids.data != nullptr)
        labelledBy(pass, ids, out);
    if (out.count != 0)
        return;
    static const PrStringView kAttributes[] = {"aria-label", "alt", "title"};
    for (PrStringView attribute : kAttributes) {
        PrStringView value = prDomElementGetAttribute(element, attribute);
        if (value.data != nullptr && !isBlank(value)) {
            collapse(value, out);
            return;
        }
    }
}

// Returns the key of the first text in the element's subtree, in tree order, that renders, or zero.
uint64_t firstTextKey(const StructurePass& pass, PrDomNode* node) {
    for (PrDomNode* child = prDomNodeFirstChild(node); child != nullptr; child = prDomNodeNextSibling(child)) {
        if (prDomNodeType(child) == PrDomNodeType_Text) {
            PrDomCharacterData* text = prDomCharacterDataQuery(prDomNodeAsEventTarget(child));
            if (text != nullptr && !isBlank(prDomCharacterDataData(text)) && (pass.presentation == nullptr ||
                    prCssPresentationLayoutNode(pass.presentation, child) != kPrLayoutNodeNone))
                return keyOf(child);
            continue;
        }
        if (uint64_t key = firstTextKey(pass, child))
            return key;
    }
    return 0;
}

// Fills in what a node of the element's type carries beyond its content.
void describe(StructurePass& pass, const PrDomElement* element, PrDrawStructureNodeDesc& desc, RawArray<char>& name,
        RawArray<char>& title, PrRawString& uri) {
    if (desc.type == PrDrawStructureType_Link && isHtml(element)) {
        uri = prHtmlHyperlinkComponent(reinterpret_cast<const PrHtmlElement*>(element), PrUrlComponent_Href);
        PrStringView href = prRawStringView(&uri);
        // A link to a fragment of the document leads to the first text of the element that the fragment indicates.
        size_t hash = 0;
        while (hash < href.size && href.data[hash] != '#')
            ++hash;
        PrStringView base = prRawStringView(&pass.documentUrl);
        if (hash < href.size && hash == base.size && prMemEquals(href.data, base.data, hash)) {
            PrStringView fragment{href.data + hash + 1, href.size - hash - 1};
            PrDomElement* target = prDomNodeGetElementById(prDomDocumentAsNode(pass.document), fragment);
            if (target != nullptr)
                desc.destination = firstTextKey(pass, asNode(target));
            if (desc.destination != 0)
                prRawStringClear(&uri);
        }
        desc.uri = prRawStringView(&uri);
    }
    nameOf(pass, element, name);
    if (name.count != 0) {
        if (desc.type == PrDrawStructureType_Figure || desc.type == PrDrawStructureType_Formula ||
                desc.type == PrDrawStructureType_Link)
            desc.alt = PrStringView{name.data, name.count};
    }
    if (prDomElementTag(element) == PrDomTag_Abbr && isHtml(element)) {
        PrStringView expansion = prDomElementGetAttribute(element, "title");
        if (expansion.data != nullptr)
            desc.expansion = expansion;
    }
    if (prDrawStructureTypeHeadingLevel(desc.type) != 0) {
        PrRawString text = prDomNodeTextContent(asNode(element));
        collapse(prRawStringView(&text), title);
        prRawStringReset(&text);
        desc.title = PrStringView{title.data, title.count};
    }
    if (desc.type == PrDrawStructureType_TH || desc.type == PrDrawStructureType_TD) {
        auto number = [&](PrStringView attribute) {
            PrStringView value = prDomElementGetAttribute(element, attribute);
            uint32_t n = 0;
            for (size_t i = 0; value.data != nullptr && i < value.size && prAsciiIsDigit(value.data[i]) && n < 10000; ++i)
                n = n * 10 + uint32_t(value.data[i] - '0');
            return n;
        };
        desc.columnSpan = number("colspan");
        desc.rowSpan = number("rowspan");
        PrStringView scope = prDomElementGetAttribute(element, "scope");
        if (scope.data != nullptr) {
            if (prAsciiEqualsFold(scope, "row") || prAsciiEqualsFold(scope, "rowgroup"))
                desc.scope = PrDrawStructureScope_Row;
            else if (prAsciiEqualsFold(scope, "col") || prAsciiEqualsFold(scope, "colgroup"))
                desc.scope = PrDrawStructureScope_Column;
        }
    }
}

bool isOwned(const StructurePass& pass, const PrDomElement* element) {
    for (uint32_t i = 0; i < pass.owned.count; ++i) {
        if (pass.owned.data[i].element == element)
            return true;
    }
    return false;
}

void walkElement(StructurePass& pass, const PrDomElement* element, PrDrawStructureNode parent, PrStringView language,
    PrDrawStructureNode* PR_NULLABLE paragraph);

// Returns the paragraph that `slot` holds under `parent`, which it starts if there is none.
PrDrawStructureNode paragraphOf(StructurePass& pass, PrDrawStructureNode parent, PrDrawStructureNode* slot) {
    if (*slot == kPrDrawStructureNodeNone) {
        PrDrawStructureNodeDesc desc = prDrawStructureNodeDescDefault(PrDrawStructureType_P);
        *slot = prDrawStructureAppendNode(pass.structure, parent, &desc);
    }
    return *slot;
}

// Appends the content of `node`'s children, then of the elements that it owns, to `parent`. Text and inline content
// directly in a node that holds paragraphs is gathered into a paragraph, which starts when its first content does. The
// paragraph is in `shared` for the content of an element that adds no node of its own, so that it continues past the
// element's ends.
void walkChildren(StructurePass& pass, PrDomNode* node, PrDrawStructureNode parent, PrStringView language,
        PrDrawStructureNode* PR_NULLABLE shared = nullptr) {
    bool grouping = isGrouping(prDrawStructureNodeDesc(pass.structure, parent).type);
    PrDrawStructureNode own = kPrDrawStructureNodeNone;
    PrDrawStructureNode& paragraph = shared != nullptr ? *shared : own;
    auto target = [&]() { return grouping ? paragraphOf(pass, parent, &paragraph) : parent; };
    auto walkOne = [&](PrDomNode* child) {
        PrDomNodeType type = prDomNodeType(child);
        if (type == PrDomNodeType_Text) {
            PrDomCharacterData* text = prDomCharacterDataQuery(prDomNodeAsEventTarget(child));
            if (text == nullptr)
                return;
            if (pass.presentation != nullptr &&
                    prCssPresentationLayoutNode(pass.presentation, child) == kPrLayoutNodeNone)
                return;
            // White space between words belongs to their paragraph, and white space alone starts none.
            if (isBlank(prDomCharacterDataData(text)) && grouping && paragraph == kPrDrawStructureNodeNone)
                return;
            prDrawStructureAppendKey(pass.structure, target(), keyOf(child));
            return;
        }
        PrDomElement* element = asElement(child);
        if (element == nullptr || isOwned(pass, element))
            return;
        if (isBlockLevel(element)) {
            paragraph = kPrDrawStructureNodeNone;
            walkElement(pass, element, parent, language, nullptr);
            paragraph = kPrDrawStructureNodeNone;
            return;
        }
        // Inline content in a node that holds paragraphs goes into the paragraph, which an inline element may start.
        if (grouping) {
            walkElement(pass, element, parent, language, &paragraph);
            return;
        }
        walkElement(pass, element, parent, language, nullptr);
    };
    for (PrDomNode* child = prDomNodeFirstChild(node); child != nullptr; child = prDomNodeNextSibling(child))
        walkOne(child);
    PrDomElement* owner = asElement(node);
    for (uint32_t i = 0; owner != nullptr && i < pass.owned.count; ++i) {
        if (pass.owned.data[i].owner != owner || pass.walked.find(pass.owned.data[i].element) != kArrayMissing)
            continue;
        pass.walked.push(pass.owned.data[i].element);
        PrDomElement* owned = const_cast<PrDomElement*>(pass.owned.data[i].element);
        if (isBlockLevel(owned))
            paragraph = kPrDrawStructureNodeNone;
        walkElement(pass, owned, parent, language, isBlockLevel(owned) || !grouping ? nullptr : &paragraph);
    }
}

// Appends the element and its content to `parent`. An inline element in a node that holds paragraphs appends to the
// paragraph that `paragraph` names, which it starts if there is none.
void walkElement(StructurePass& pass, const PrDomElement* element, PrDrawStructureNode parent, PrStringView language,
        PrDrawStructureNode* paragraph) {
    if (isHidden(pass, element))
        return;
    StructureRole role = roleOf(element);
    if (role.kind == StructureKind_Skipped)
        return;
    PrStringView lang = prDomElementGetAttribute(element, "lang");
    bool newLanguage = lang.data != nullptr && !(lang == language);
    PrStringView inherited = lang.data != nullptr ? lang : language;
    if (role.kind == StructureKind_Contents) {
        // Its content continues the paragraph that holds it, if it is inline content in one.
        walkChildren(pass, asNode(element), parent, inherited, paragraph);
        return;
    }
    if (paragraph != nullptr && role.kind == StructureKind_Node && !isGrouping(role.type))
        parent = paragraphOf(pass, parent, paragraph);
    PrDrawStructureNodeDesc desc = prDrawStructureNodeDescDefault(role.type);
    RawArray<char> name;
    RawArray<char> title;
    PrRawString uri = prRawStringNull();
    describe(pass, element, desc, name, title, uri);
    if (newLanguage)
        desc.language = lang;
    if (role.kind == StructureKind_Leaf) {
        // An image without a name is decoration (html/#alt), and its content an artifact.
        PrStringView alt = prDomElementGetAttribute(element, "alt");
        bool decorative = name.count == 0 && alt.data != nullptr && alt.size == 0;
        if (!decorative) {
            if (paragraph != nullptr)
                parent = paragraphOf(pass, parent, paragraph);
            PrDrawStructureNode leaf = prDrawStructureAppendNode(pass.structure, parent, &desc);
            prDrawStructureAppendKey(pass.structure, leaf, keyOf(asNode(element)));
        }
        name.reset();
        title.reset();
        prRawStringReset(&uri);
        return;
    }
    PrDrawStructureNode node = prDrawStructureAppendNode(pass.structure, parent, &desc);
    name.reset();
    title.reset();
    prRawStringReset(&uri);
    // A list item's content is its body, beside the marker, which is generated content and an artifact.
    PrDomTag tag = prDomElementTag(element);
    if (role.type == PrDrawStructureType_LI) {
        bool term = tag == PrDomTag_Dt && isHtml(element);
        PrDrawStructureNodeDesc body =
            prDrawStructureNodeDescDefault(term ? PrDrawStructureType_Lbl : PrDrawStructureType_LBody);
        node = prDrawStructureAppendNode(pass.structure, node, &body);
    }
    walkChildren(pass, asNode(element), node, inherited);
}

// Collects the elements that aria-owns attributes name, each owned by the first element that names it.
void collectOwned(StructurePass& pass, PrDomNode* node) {
    for (PrDomNode* child = prDomNodeFirstChild(node); child != nullptr; child = prDomNodeNextSibling(child)) {
        PrDomElement* element = asElement(child);
        if (element == nullptr)
            continue;
        PrStringView ids = prDomElementGetAttribute(element, "aria-owns");
        for (size_t at = 0; ids.data != nullptr && at < ids.size;) {
            while (at < ids.size && prAsciiIsSpace(ids.data[at]))
                ++at;
            size_t start = at;
            while (at < ids.size && !prAsciiIsSpace(ids.data[at]))
                ++at;
            if (at == start)
                break;
            PrDomElement* owned = prDomNodeGetElementById(prDomDocumentAsNode(pass.document),
                PrStringView{ids.data + start, at - start});
            // An element cannot own itself or one of its ancestors.
            bool ancestor = false;
            for (PrDomNode* up = child; up != nullptr && owned != nullptr && !ancestor; up = prDomNodeParentNode(up))
                ancestor = up == asNode(owned);
            if (owned != nullptr && !ancestor && !isOwned(pass, owned))
                pass.owned.push(Owned{element, owned});
        }
        collectOwned(pass, child);
    }
}

// Appends a link for each copy of a link in the page-margin boxes, which a running element's copies hold
// (css-gcpm-3/#running-elements), after the document's content. The rest of the margin boxes is an artifact
// (css-page-3/#margin-boxes), which no node names.
void appendMarginLinks(StructurePass& pass) {
    if (pass.presentation == nullptr)
        return;
    CssPageMarginLinks links = cssPresentationPageMarginLinks(pass.presentation);
    RawArray<char> name;
    RawArray<char> title;
    PrRawString uri{};
    PrDrawStructureNode node = kPrDrawStructureRoot;
    for (uint32_t i = 0; i < links.count; ++i) {
        const CssPageMarginLink& link = links.links[i];
        bool joins = i != 0 && links.links[i - 1].link == link.link && links.links[i - 1].copy == link.copy;
        if (!joins) {
            PrDrawStructureNodeDesc desc = prDrawStructureNodeDescDefault(PrDrawStructureType_Link);
            prRawStringReset(&uri);
            describe(pass, link.link, desc, name, title, uri);
            node = prDrawStructureAppendNode(pass.structure, kPrDrawStructureRoot, &desc);
        }
        prDrawStructureAppendKey(pass.structure, node, link.key);
    }
    prRawStringReset(&uri);
    name.reset();
    title.reset();
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

void prHtmlStructureAppend(PrHtmlPage* page, PrDrawStructure* structure) {
    StructurePass pass{};
    pass.structure = structure;
    pass.presentation = prHtmlPagePresentation(page);
    pass.document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrRawString url = prDomDocumentUrl(pass.document);
    PrStringView view = prRawStringView(&url);
    size_t hash = 0;
    while (hash < view.size && view.data[hash] != '#')
        ++hash;
    pass.documentUrl = prRawStringCopy(PrStringView{view.data, hash});
    prRawStringReset(&url);
    PrDomElement* root = prDomDocumentElement(pass.document);
    if (root != nullptr) {
        collectOwned(pass, prDomDocumentAsNode(pass.document));
        PrStringView language = prDomElementGetAttribute(root, "lang");
        if (language.data != nullptr && language.size != 0) {
            PrDrawStructureNodeDesc desc = prDrawStructureNodeDesc(structure, kPrDrawStructureRoot);
            desc.language = language;
            prDrawStructureNodeSetDesc(structure, kPrDrawStructureRoot, &desc);
        }
        PrStringView inherited = language.data != nullptr ? language : PrStringView{};
        walkChildren(pass, asNode(root), kPrDrawStructureRoot, inherited);
        appendMarginLinks(pass);
    }
    prRawStringReset(&pass.documentUrl);
    pass.owned.reset();
    pass.walked.reset();
}
