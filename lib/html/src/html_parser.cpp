// Tree construction
//
// html/#tree-construction - https://html.spec.whatwg.org/multipage/parsing.html#tree-construction

#include "html/src/html_parser.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/memory.h"
#include "pr/string_ops.h"

#include "css/src/css_root.h"
#include "dom/src/dom_control.h"
#include "dom/src/dom_custom.h"
#include "dom/src/dom_element_state.h"
#include "dom/src/dom_select.h"
#include "dom/src/dom_use.h"
#include "html/src/html_adjust.h"
#include "html/src/html_charset.h"
#include "html/src/html_link.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Element categories (html/#the-stack-of-open-elements)

bool isMathMlTextIntegrationPoint(const PrDomElement* element) {
    PrDomTag tag = domElementTag(element);
    return element->space == PrDomNamespace_MathMl &&
        (tag == PrDomTag_Mi || tag == PrDomTag_Mo || tag == PrDomTag_Mn || tag == PrDomTag_Ms || tag == PrDomTag_Mtext);
}

// Returns true if the element is an SVG element that is an HTML integration point regardless of its attributes.
bool isSvgIntegrationPoint(const PrDomElement* element) {
    PrDomTag tag = domElementTag(element);
    return element->space == PrDomNamespace_Svg &&
        (tag == PrDomTag_ForeignObject || tag == PrDomTag_Desc || tag == PrDomTag_Title);
}

// Returns true if the element is a foreign element that is special and ends every scope except table scope.
bool isForeignBoundary(const PrDomElement* element) {
    return isMathMlTextIntegrationPoint(element) || isSvgIntegrationPoint(element) ||
        (element->space == PrDomNamespace_MathMl && element->tag == PrDomTag_AnnotationXml);
}

// Returns true if the element has its own parsing rules, so that an ordinary end tag stops at it.
bool isSpecial(const PrDomElement* element) {
    if (element->space != PrDomNamespace_Html)
        return isForeignBoundary(element);
    switch (element->tag) {
        case PrDomTag_Address: case PrDomTag_Applet: case PrDomTag_Area: case PrDomTag_Article:
        case PrDomTag_Aside: case PrDomTag_Base: case PrDomTag_Basefont: case PrDomTag_Bgsound:
        case PrDomTag_Blockquote: case PrDomTag_Body: case PrDomTag_Br: case PrDomTag_Button:
        case PrDomTag_Caption: case PrDomTag_Center: case PrDomTag_Col: case PrDomTag_Colgroup:
        case PrDomTag_Dd: case PrDomTag_Details: case PrDomTag_Dir: case PrDomTag_Div:
        case PrDomTag_Dl: case PrDomTag_Dt: case PrDomTag_Embed: case PrDomTag_Fieldset:
        case PrDomTag_Figcaption: case PrDomTag_Figure: case PrDomTag_Footer: case PrDomTag_Form:
        case PrDomTag_Frame: case PrDomTag_Frameset: case PrDomTag_H1: case PrDomTag_H2:
        case PrDomTag_H3: case PrDomTag_H4: case PrDomTag_H5: case PrDomTag_H6:
        case PrDomTag_Head: case PrDomTag_Header: case PrDomTag_Hgroup: case PrDomTag_Hr:
        case PrDomTag_Html: case PrDomTag_Iframe: case PrDomTag_Img: case PrDomTag_Input:
        case PrDomTag_Keygen: case PrDomTag_Li: case PrDomTag_Link: case PrDomTag_Listing:
        case PrDomTag_Main: case PrDomTag_Marquee: case PrDomTag_Menu: case PrDomTag_Meta:
        case PrDomTag_Nav: case PrDomTag_Noembed: case PrDomTag_Noframes: case PrDomTag_Noscript:
        case PrDomTag_Object: case PrDomTag_Ol: case PrDomTag_P: case PrDomTag_Param:
        case PrDomTag_Plaintext: case PrDomTag_Pre: case PrDomTag_Script: case PrDomTag_Search:
        case PrDomTag_Section: case PrDomTag_Select: case PrDomTag_Source: case PrDomTag_Style:
        case PrDomTag_Summary: case PrDomTag_Table: case PrDomTag_Tbody: case PrDomTag_Td:
        case PrDomTag_Template: case PrDomTag_Textarea: case PrDomTag_Tfoot: case PrDomTag_Th:
        case PrDomTag_Thead: case PrDomTag_Title: case PrDomTag_Tr: case PrDomTag_Track:
        case PrDomTag_Ul: case PrDomTag_Wbr: case PrDomTag_Xmp:
            return true;
        default:
            return false;
    }
}

// The elements at which a scope search stops (html/#the-stack-of-open-elements).
enum {
    Scope_Default,
    Scope_ListItem,
    Scope_Button,
    Scope_Table,
};
typedef uint8_t Scope;

bool endsScope(const PrDomElement* element, Scope scope) {
    PrDomTag tag = domElementTag(element);
    if (scope == Scope_Table) {
        return element->space == PrDomNamespace_Html &&
            (tag == PrDomTag_Html || tag == PrDomTag_Table || tag == PrDomTag_Template);
    }
    if (element->space != PrDomNamespace_Html)
        return isForeignBoundary(element);
    switch (tag) {
        case PrDomTag_Applet: case PrDomTag_Caption: case PrDomTag_Html: case PrDomTag_Table:
        case PrDomTag_Td: case PrDomTag_Th: case PrDomTag_Marquee: case PrDomTag_Object:
        case PrDomTag_Select: case PrDomTag_Template:
            return true;
        case PrDomTag_Ol: case PrDomTag_Ul:
            return scope == Scope_ListItem;
        case PrDomTag_Button:
            return scope == Scope_Button;
        default:
            return false;
    }
}

// Returns true if the element is one that an implied end tag closes
// (html/#closing-elements-that-have-implied-end-tags), and is not `except`.
bool isImpliedEnd(const PrDomElement* element, PrDomTag except) {
    if (element->space != PrDomNamespace_Html || element->tag == except)
        return false;
    switch (element->tag) {
        case PrDomTag_Dd: case PrDomTag_Dt: case PrDomTag_Li: case PrDomTag_Optgroup:
        case PrDomTag_Option: case PrDomTag_P: case PrDomTag_Rb: case PrDomTag_Rp:
        case PrDomTag_Rt: case PrDomTag_Rtc:
            return true;
        default:
            return false;
    }
}

bool isThoroughImpliedEnd(const PrDomElement* element) {
    if (element->space != PrDomNamespace_Html)
        return false;
    switch (element->tag) {
        case PrDomTag_Caption: case PrDomTag_Colgroup: case PrDomTag_Dd: case PrDomTag_Dt:
        case PrDomTag_Li: case PrDomTag_Optgroup: case PrDomTag_Option: case PrDomTag_P:
        case PrDomTag_Rb: case PrDomTag_Rp: case PrDomTag_Rt: case PrDomTag_Rtc:
        case PrDomTag_Tbody: case PrDomTag_Td: case PrDomTag_Tfoot: case PrDomTag_Th:
        case PrDomTag_Thead: case PrDomTag_Tr:
            return true;
        default:
            return false;
    }
}

// Returns true if the end of the input may leave the element open in "in body" without a parse error
// (html/#parsing-main-inbody).
bool mayEndOpen(const PrDomElement* element) {
    if (element->space != PrDomNamespace_Html)
        return false;
    switch (element->tag) {
        case PrDomTag_Dd: case PrDomTag_Dt: case PrDomTag_Li: case PrDomTag_Optgroup:
        case PrDomTag_Option: case PrDomTag_P: case PrDomTag_Rb: case PrDomTag_Rp:
        case PrDomTag_Rt: case PrDomTag_Rtc: case PrDomTag_Tbody: case PrDomTag_Td:
        case PrDomTag_Tfoot: case PrDomTag_Th: case PrDomTag_Thead: case PrDomTag_Tr:
        case PrDomTag_Body: case PrDomTag_Html:
            return true;
        default:
            return false;
    }
}

bool isHeading(const PrDomElement* element) {
    PrDomTag tag = domElementTag(element);
    return element->space == PrDomNamespace_Html &&
        (tag == PrDomTag_H1 || tag == PrDomTag_H2 || tag == PrDomTag_H3 || tag == PrDomTag_H4 ||
            tag == PrDomTag_H5 || tag == PrDomTag_H6);
}

// Returns true if the start tag leaves foreign content for HTML (html/#parsing-main-inforeign). A font start tag also
// leaves it if it has a color, face or size attribute.
bool breaksOutOfForeign(PrDomTag tag) {
    switch (tag) {
        case PrDomTag_B:
        case PrDomTag_Big:
        case PrDomTag_Blockquote:
        case PrDomTag_Body:
        case PrDomTag_Br:
        case PrDomTag_Center: case PrDomTag_Code: case PrDomTag_Dd: case PrDomTag_Div: case PrDomTag_Dl:
        case PrDomTag_Dt: case PrDomTag_Em: case PrDomTag_Embed: case PrDomTag_H1: case PrDomTag_H2:
        case PrDomTag_H3: case PrDomTag_H4: case PrDomTag_H5: case PrDomTag_H6: case PrDomTag_Head:
        case PrDomTag_Hr: case PrDomTag_I: case PrDomTag_Img: case PrDomTag_Li: case PrDomTag_Listing:
        case PrDomTag_Menu: case PrDomTag_Meta: case PrDomTag_Nobr: case PrDomTag_Ol: case PrDomTag_P:
        case PrDomTag_Pre: case PrDomTag_Ruby: case PrDomTag_S: case PrDomTag_Small: case PrDomTag_Span:
        case PrDomTag_Strong:
        case PrDomTag_Strike:
        case PrDomTag_Sub:
        case PrDomTag_Sup:
        case PrDomTag_Table:
        case PrDomTag_Tt: case PrDomTag_U: case PrDomTag_Ul: case PrDomTag_Var:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Quirks (html/#the-initial-insertion-mode)

constexpr PrStringView kQuirksPrefix[] = {
    "+//Silmaril//dtd html Pro v0r11 19970101//",
    "-//AS//DTD HTML 3.0 asWedit + extensions//",
    "-//AdvaSoft Ltd//DTD HTML 3.0 asWedit + extensions//",
    "-//IETF//DTD HTML 2.0 Level 1//",
    "-//IETF//DTD HTML 2.0 Level 2//",
    "-//IETF//DTD HTML 2.0 Strict Level 1//",
    "-//IETF//DTD HTML 2.0 Strict Level 2//",
    "-//IETF//DTD HTML 2.0 Strict//",
    "-//IETF//DTD HTML 2.0//",
    "-//IETF//DTD HTML 2.1E//",
    "-//IETF//DTD HTML 3.0//",
    "-//IETF//DTD HTML 3.2 Final//",
    "-//IETF//DTD HTML 3.2//",
    "-//IETF//DTD HTML 3//",
    "-//IETF//DTD HTML Level 0//",
    "-//IETF//DTD HTML Level 1//",
    "-//IETF//DTD HTML Level 2//",
    "-//IETF//DTD HTML Level 3//",
    "-//IETF//DTD HTML Strict Level 0//",
    "-//IETF//DTD HTML Strict Level 1//",
    "-//IETF//DTD HTML Strict Level 2//",
    "-//IETF//DTD HTML Strict Level 3//",
    "-//IETF//DTD HTML Strict//",
    "-//IETF//DTD HTML//",
    "-//Metrius//DTD Metrius Presentational//",
    "-//Microsoft//DTD Internet Explorer 2.0 HTML Strict//",
    "-//Microsoft//DTD Internet Explorer 2.0 HTML//",
    "-//Microsoft//DTD Internet Explorer 2.0 Tables//",
    "-//Microsoft//DTD Internet Explorer 3.0 HTML Strict//",
    "-//Microsoft//DTD Internet Explorer 3.0 HTML//",
    "-//Microsoft//DTD Internet Explorer 3.0 Tables//",
    "-//Netscape Comm. Corp.//DTD HTML//",
    "-//Netscape Comm. Corp.//DTD Strict HTML//",
    "-//O'Reilly and Associates//DTD HTML 2.0//",
    "-//O'Reilly and Associates//DTD HTML Extended 1.0//",
    "-//O'Reilly and Associates//DTD HTML Extended Relaxed 1.0//",
    "-//SQ//DTD HTML 2.0 HoTMetaL + extensions//",
    "-//SoftQuad Software//DTD HoTMetaL PRO 6.0::19990601::extensions to HTML 4.0//",
    "-//SoftQuad//DTD HoTMetaL PRO 4.0::19971010::extensions to HTML 4.0//",
    "-//Spyglass//DTD HTML 2.0 Extended//",
    "-//Sun Microsystems Corp.//DTD HotJava HTML//",
    "-//Sun Microsystems Corp.//DTD HotJava Strict HTML//",
    "-//W3C//DTD HTML 3 1995-03-24//",
    "-//W3C//DTD HTML 3.2 Draft//",
    "-//W3C//DTD HTML 3.2 Final//",
    "-//W3C//DTD HTML 3.2//",
    "-//W3C//DTD HTML 3.2S Draft//",
    "-//W3C//DTD HTML 4.0 Frameset//",
    "-//W3C//DTD HTML 4.0 Transitional//",
    "-//W3C//DTD HTML Experimental 19960712//",
    "-//W3C//DTD HTML Experimental 970421//",
    "-//W3C//DTD W3 HTML//",
    "-//W3O//DTD W3 HTML 3.0//",
    "-//WebTechs//DTD Mozilla HTML 2.0//",
    "-//WebTechs//DTD Mozilla HTML//",
};

DomQuirks quirksOf(PrStringView name, PrStringView publicId, PrStringView systemId,
        bool forceQuirks, bool hasPublicId, bool hasSystemId) {
    if (forceQuirks || name != "html")
        return DomQuirks_Yes;

    if (hasPublicId) {
        if (asciiEqualsFold(publicId, "-//W3O//DTD W3 HTML Strict 3.0//EN//") ||
                asciiEqualsFold(publicId, "-/W3C/DTD HTML 4.0 Transitional/EN") || asciiEqualsFold(publicId, "HTML"))
            return DomQuirks_Yes;
        for (PrStringView prefix : kQuirksPrefix) {
            if (asciiStartsWithFold(publicId, prefix))
                return DomQuirks_Yes;
        }
    }
    if (hasSystemId && asciiEqualsFold(systemId, "http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd"))
        return DomQuirks_Yes;

    bool emptySystem = !hasSystemId || systemId.size == 0;
    if (hasPublicId && (asciiStartsWithFold(publicId, "-//W3C//DTD HTML 4.01 Frameset//") ||
            asciiStartsWithFold(publicId, "-//W3C//DTD HTML 4.01 Transitional//")))
        return emptySystem ? DomQuirks_Yes : DomQuirks_Limited;

    if (hasPublicId && (asciiStartsWithFold(publicId, "-//W3C//DTD XHTML 1.0 Frameset//") ||
            asciiStartsWithFold(publicId, "-//W3C//DTD XHTML 1.0 Transitional//")))
        return DomQuirks_Limited;

    return DomQuirks_No;
}

// ---------------------------------------------------------------------------------------------------------------------
// One token's pass through the tree builder

struct Tree {
    HtmlParser* p;
    HtmlToken token = {};             // the token being processed
    PrDomTag tag = PrDomTag_Unknown;  // its tag name as a tag, or Unknown for a name with no tag
    PrStringView name = {nullptr, 0}; // its tag name as written
};

// ---------------------------------------------------------------------------------------------------------------------
// Reading the token

HtmlTokenKind kind(const Tree* t) { return t->token.kind; }

PrStringView text(const Tree* t, HtmlSpan span) { return htmlSpanText(&t->p->tokenizer, span); }

void error(Tree* t) { t->p->errors.push(HtmlError{HtmlErrorCode_Unnamed, t->token.span.start}); }

// ---------------------------------------------------------------------------------------------------------------------
// What the parser holds
//
// The stack of open elements, the list of active formatting elements and the element pointers each hold a reference to
// every element that they name. Otherwise a script that runs during parsing could remove and free those elements.

void hold(PrDomElement* PR_NULLABLE element) {
    if (element != nullptr)
        domNodeRetain(element);
}

void pushHeld(Array<PrDomElement* PR_NULLABLE>* list, PrDomElement* PR_NULLABLE element) {
    hold(element);
    list->push(element);
}

void insertHeld(Array<PrDomElement* PR_NULLABLE>* list, uint32_t index, PrDomElement* element) {
    hold(element);
    list->insert(index, element);
}

void eraseHeld(Array<PrDomElement* PR_NULLABLE>* list, uint32_t index) {
    PrDomElement* PR_NULLABLE element = list->data[index];
    list->erase(index);
    domNodeRelease(element);
}

void replaceHeld(Array<PrDomElement* PR_NULLABLE>* list, uint32_t index, PrDomElement* element) {
    hold(element);
    domNodeRelease(list->data[index]);
    list->data[index] = element;
}

// Pops the stack of open elements down to `count` elements. A popped option that is its select's selected option is
// cloned into the select's selectedcontent (html/#maybe-clone-an-option-into-selectedcontent). A popped SVG style
// element updates its style block, which the parser held back while it added the element's text
// (SVG2/styling.html#StyleElement): an HTML style element's end tag updates its own in the text insertion mode.
void truncateOpen(HtmlParser* p, uint32_t count) {
    while (p->open.count > count) {
        PrDomElement* element = p->open.data[--p->open.count];
        if (domIsHtml(element, PrDomTag_Option))
            domOptionPopped(element);
        if (element->space == PrDomNamespace_Svg && element->tag == PrDomTag_Style)
            cssStyleBlockUpdate(element, DomInsertedBy_Parser);
        domNodeRelease(element);
    }
}

// Points an element pointer at `element`.
void setPointer(PrDomElement* PR_NULLABLE* PR_NONNULL pointer, PrDomElement* PR_NULLABLE element) {
    hold(element);
    PrDomElement* PR_NULLABLE old = *pointer;
    *pointer = element;
    domNodeRelease(old);
}

// ---------------------------------------------------------------------------------------------------------------------
// The stack of open elements (html/#the-stack-of-open-elements)

PrDomElement* current(const Tree* t) { return t->p->open.last(); }

PrDomElement* adjustedCurrent(const Tree* t) {
    return t->p->context != nullptr && t->p->open.count == 1 ? t->p->context : current(t);
}

void pop(Tree* t) { truncateOpen(t->p, t->p->open.count - 1); }

void popThrough(Tree* t, PrDomTag which) {
    while (t->p->open.count != 0) {
        bool found = domIsHtml(current(t), which);
        pop(t);
        if (found)
            return;
    }
}

// Returns true if the stack holds an element of this name anywhere. Several rules test this rather than a scope.
bool hasOpen(const Tree* t, PrDomTag which) {
    for (uint32_t i = t->p->open.count; i-- != 0;) {
        if (domIsHtml(t->p->open.data[i], which))
            return true;
    }
    return false;
}

// Returns true if the parser is parsing template contents (html/#the-element-pointers). The form rules test this rather
// than the stack, because a fragment whose context is a template is inside a template without holding it open.
bool parsingTemplateContents(const Tree* t) {
    return hasOpen(t, PrDomTag_Template) || (domIsHtml(t->p->context, PrDomTag_Template));
}

bool inScope(const Tree* t, PrDomTag which, Scope scope) {
    for (uint32_t i = t->p->open.count; i-- != 0;) {
        PrDomElement* element = t->p->open.data[i];
        if (domIsHtml(element, which))
            return true;
        if (endsScope(element, scope))
            return false;
    }
    return false;
}

bool inScopeHeading(const Tree* t) {
    for (uint32_t i = t->p->open.count; i-- != 0;) {
        PrDomElement* element = t->p->open.data[i];
        if (isHeading(element))
            return true;
        if (endsScope(element, Scope_Default))
            return false;
    }
    return false;
}

// html/#closing-elements-that-have-implied-end-tags, in its second form, which names the one element that it leaves
// open.
void generateImpliedEndTagsExcept(Tree* t, PrDomTag except) {
    while (t->p->open.count != 0 && isImpliedEnd(current(t), except))
        pop(t);
}

void generateImpliedEndTags(Tree* t) {
    generateImpliedEndTagsExcept(t, PrDomTag_Unknown);
}

void generateImpliedEndTagsThoroughly(Tree* t) {
    while (t->p->open.count != 0 && isThoroughImpliedEnd(current(t)))
        pop(t);
}

void closeParagraph(Tree* t) {
    generateImpliedEndTagsExcept(t, PrDomTag_P);
    if (!domIsHtml(current(t), PrDomTag_P))
        error(t);
    popThrough(t, PrDomTag_P);
}

// Runs the steps of an end tag that closes the element `which` if it is in `scope`. Generates the implied end tags
// except for `except`, and pops the stack through the element. Returns false, with a parse error, if the element is not
// in scope.
bool closeElement(Tree* t, PrDomTag which, Scope scope, PrDomTag except = PrDomTag_Unknown) {
    if (!inScope(t, which, scope)) {
        error(t);
        return false;
    }
    generateImpliedEndTagsExcept(t, except);
    if (!domIsHtml(current(t), which))
        error(t);
    popThrough(t, which);
    return true;
}

// Returns true if the current node is a template or the html element. Clearing the stack back to a table, table body or
// row context stops at these elements.
bool atContextRoot(const Tree* t) {
    return domIsHtml(current(t), PrDomTag_Template) || domIsHtml(current(t), PrDomTag_Html);
}

// html/#clear-the-stack-back-to-a-table-context
void clearToTableContext(Tree* t) {
    while (t->p->open.count != 0 && !domIsHtml(current(t), PrDomTag_Table) && !atContextRoot(t))
        pop(t);
}

// html/#clear-the-stack-back-to-a-table-body-context
void clearToTableBodyContext(Tree* t) {
    while (t->p->open.count != 0 && !domIsHtml(current(t), PrDomTag_Tbody) &&
            !domIsHtml(current(t), PrDomTag_Tfoot) && !domIsHtml(current(t), PrDomTag_Thead) && !atContextRoot(t))
        pop(t);
}

// html/#clear-the-stack-back-to-a-table-row-context
void clearToRowContext(Tree* t) {
    while (t->p->open.count != 0 && !domIsHtml(current(t), PrDomTag_Tr) && !atContextRoot(t))
        pop(t);
}

// ---------------------------------------------------------------------------------------------------------------------
// The list of active formatting elements (html/#the-list-of-active-formatting-elements)

// Removes the entry at `index` from the list, releasing its element and its attributes.
void eraseFormatting(HtmlParser* p, uint32_t index) {
    HtmlFormatting entry = p->formatting.data[index];
    p->formatting.erase(index);
    for (uint32_t i = 0; i < entry.attributeCount; ++i) {
        prStringReset(&entry.attributes[i].name);
        prStringReset(&entry.attributes[i].value);
    }
    prMemoryFree(entry.attributes);
    domNodeRelease(entry.element);
}

// Replaces the element of the entry at `index` with one created for the same start tag.
void replaceFormatting(HtmlParser* p, uint32_t index, PrDomElement* element) {
    hold(element);
    domNodeRelease(p->formatting.data[index].element);
    p->formatting.data[index].element = element;
}

void pushMarker(HtmlParser* p) {
    p->formatting.push(HtmlFormatting{nullptr, nullptr, 0});
}

// Returns true if the start tags of two entries had the same attributes, in any order.
bool attributesEquals(const HtmlFormatting& a, const HtmlFormatting& b) {
    if (a.attributeCount != b.attributeCount)
        return false;
    for (uint32_t i = 0; i < a.attributeCount; ++i) {
        const HtmlTokenAttribute& one = a.attributes[i];
        bool found = false;
        for (uint32_t j = 0; j < b.attributeCount && !found; ++j) {
            const HtmlTokenAttribute& two = b.attributes[j];
            found = prStringView(&one.name) == prStringView(&two.name) &&
                prStringView(&one.value) == prStringView(&two.value);
        }
        if (!found)
            return false;
    }
    return true;
}

// html/#push-onto-the-list-of-active-formatting-elements, for the element that the current start tag created, with that
// tag's attributes. The Noah's Ark clause allows at most three entries with the same name, namespace and attributes.
void pushFormatting(Tree* t, PrDomElement* element) {
    const Array<HtmlAttribute>& given = t->p->tokenizer.attributes;
    HtmlFormatting entry = {element, nullptr, given.count};
    if (given.count != 0) {
        entry.attributes = static_cast<HtmlTokenAttribute*>(prMemoryAlloc(sizeof(HtmlTokenAttribute) * given.count));
        for (uint32_t i = 0; i < given.count; ++i)
            entry.attributes[i] = HtmlTokenAttribute{
                prStringCopy(text(t, given.data[i].name)),
                prStringCopy(text(t, given.data[i].value)),
            };
    }
    uint32_t seen = 0;
    for (uint32_t i = t->p->formatting.count; i-- != 0;) {
        const HtmlFormatting& other = t->p->formatting.data[i];
        if (other.element == nullptr)
            break;
        if (other.element->tag != element->tag || other.element->space != element->space || !attributesEquals(other, entry))
            continue;
        if (++seen == 3) {
            eraseFormatting(t->p, i);
            break;
        }
    }
    hold(element);
    t->p->formatting.push(entry);
}

uint32_t formattingIndex(const Tree* t, const PrDomElement* element) {
    for (uint32_t i = t->p->formatting.count; i-- != 0;) {
        if (t->p->formatting.data[i].element == element)
            return i;
    }
    return t->p->formatting.count;
}

bool isOpen(const Tree* t, const PrDomElement* element) {
    for (uint32_t i = 0; i < t->p->open.count; ++i) {
        if (t->p->open.data[i] == element)
            return true;
    }
    return false;
}

// Returns the formatting element named `which` after the last marker, or null if there is none.
PrDomElement* PR_NULLABLE activeFormatting(const Tree* t, PrDomTag which) {
    for (uint32_t i = t->p->formatting.count; i-- != 0;) {
        PrDomElement* element = t->p->formatting.data[i].element;
        if (element == nullptr)
            return nullptr;
        if (domIsHtml(element, which))
            return element;
    }
    return nullptr;
}

void clearFormattingToMarker(Tree* t) {
    while (t->p->formatting.count != 0) {
        bool marker = t->p->formatting.last().element == nullptr;
        eraseFormatting(t->p, t->p->formatting.count - 1);
        if (marker)
            return;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Inserting nodes (html/#creating-and-inserting-nodes)

PrDomNode* templateContents(PrDomElement* element) {
    PrDomNode* contents = domElementContents(element);
    PR_ASSERT(contents != nullptr);
    return contents;
}

// Writes the place where a node is inserted, as a parent and the child to insert it before.
void placeIn(Tree* t, PrDomElement* target, PrDomNode** parent, PrDomNode* PR_NULLABLE* before) {
    *before = nullptr;

    if (t->p->fosterParenting && target->space == PrDomNamespace_Html &&
            (target->tag == PrDomTag_Table || target->tag == PrDomTag_Tbody ||
                target->tag == PrDomTag_Tfoot || target->tag == PrDomTag_Thead || target->tag == PrDomTag_Tr)) {
        PrDomElement* PR_NULLABLE lastTable = nullptr;
        uint32_t tableIndex = 0;
        for (uint32_t i = t->p->open.count; i-- != 0;) {
            if (domIsHtml(t->p->open.data[i], PrDomTag_Template)) {
                *parent = templateContents(t->p->open.data[i]);
                return;
            }
            if (domIsHtml(t->p->open.data[i], PrDomTag_Table)) {
                lastTable = t->p->open.data[i];
                tableIndex = i;
                break;
            }
        }
        if (lastTable == nullptr) {
            *parent = t->p->open.data[0];
            return;
        }
        if (lastTable->parent != nullptr) {
            *parent = lastTable->parent;
            *before = lastTable;
            return;
        }
        *parent = t->p->open.data[tableIndex - 1];
        return;
    }

    if (domIsHtml(target, PrDomTag_Template)) {
        *parent = templateContents(target);
        return;
    }
    *parent = target;
}

// Returns the node that takes what is inserted into `parent`. This is the parse's root target if `parent` is the root
// of a fragment parse that has one, and `parent` otherwise.
PrDomNode* redirected(const Tree* t, PrDomNode* parent) {
    const HtmlParser* p = t->p;
    if (p->rootTarget != nullptr && p->open.count != 0 && parent == p->open.data[0])
        return p->rootTarget;
    return parent;
}

// Writes the appropriate place for inserting a node (html/#appropriate-place-for-inserting-a-node), as a parent and the
// child to insert it before.
void insertionPlaceIn(Tree* t, PrDomElement* target, PrDomNode** parent, PrDomNode* PR_NULLABLE* before) {
    placeIn(t, target, parent, before);
    *parent = redirected(t, *parent);
}

// Inserts `node` at the end of `into`, or, if that is null, at the appropriate place for inserting a node.
void insertNode(Tree* t, PrDomNode* node, PrDomNode* PR_NULLABLE into) {
    PrDomNode* parent = into != nullptr ? redirected(t, into) : nullptr;
    PrDomNode* before = nullptr;
    if (parent == nullptr)
        insertionPlaceIn(t, current(t), &parent, &before);
    domAttach(parent, node, before);
}

// Adds the token's attributes that the element does not already have. This is what html/#parsing-main-inbody does to
// the html and body elements if a second start tag names them.
void mergeAttributes(Tree* t, PrDomElement* element) {
    for (uint32_t i = 0; i < t->p->tokenizer.attributes.count; ++i) {
        const HtmlAttribute& attribute = t->p->tokenizer.attributes.data[i];
        PrStringView local = text(t, attribute.name);
        if (domAttribute(element, PrDomNamespace_None, local) == nullptr)
            domSetAttribute(element, PrDomNamespace_None, local, text(t, attribute.value));
    }
}

// Returns the value of the token's attribute `name`, or the null view if it has none.
PrStringView tokenAttribute(Tree* t, PrStringView name) {
    for (uint32_t i = 0; i < t->p->tokenizer.attributes.count; ++i) {
        const HtmlAttribute& attribute = t->p->tokenizer.attributes.data[i];
        if (text(t, attribute.name) == name)
            return text(t, attribute.value);
    }
    return PrStringView{};
}

// Adds the token's attributes, respelled as html/#parsing-main-inforeign specifies for a foreign element. The tokenizer
// dropped duplicates, so a new element takes them as they are.
void addAttributes(Tree* t, PrDomElement* element, DomNamespaceId space) {
    for (uint32_t i = 0; i < t->p->tokenizer.attributes.count; ++i) {
        const HtmlAttribute& attribute = t->p->tokenizer.attributes.data[i];
        PrStringView local = text(t, attribute.name);
        PrStringView value = text(t, attribute.value);
        DomNamespaceId attributeSpace = PrDomNamespace_None;

        if (space != PrDomNamespace_Html) {
            PrStringView prefixed = {nullptr, 0};
            DomNamespaceId found = htmlForeignAttribute(local, &prefixed);
            if (found != PrDomNamespace_None) {
                attributeSpace = found;
                local = prefixed;
            } else {
                PrStringView spelled = htmlAdjustedAttribute(space, local);
                if (spelled.data != nullptr)
                    local = spelled;
            }
        }
        domSetAttribute(element, attributeSpace, local, value);
    }
}

// Inserts `node` in `parent` before `before` if pre-insertion validity allows it
// (dom/#concept-node-ensure-pre-insertion-validity). Otherwise the node is dropped, and removed from its current place.
// A script can cause this by moving nodes that the parser holds: a node that is a host-including inclusive ancestor of
// `parent`, which an empty node cannot be, or a second element for a document.
void insertAt(PrDomNode* parent, PrDomNode* node, PrDomNode* PR_NULLABLE before) {
    const PrDomElement* element = domElementFromNode(node);
    bool holds = node->first != nullptr || (element != nullptr &&
        (domElementContents(element) != nullptr || domShadowRootOfHost(element) != nullptr));
    bool possible;
    if (parent->type == PrDomNodeType_Document)
        possible = domValidateInsertion(parent, node, before) == PrStatus_Ok;
    else
        possible = !holds || !domIsHostIncludingInclusiveAncestor(node, parent);
    if (!possible) {
        domDetach(node);
        return;
    }
    domAttach(parent, node, before);
}

// Recreates an element for the entry's start tag (html/#create-an-element-for-the-token), with the namespace and name
// of the entry's element and the tag's attributes.
PrDomElement* remake(HtmlParser* p, const HtmlFormatting& entry) {
    const PrDomElement* like = entry.element;
    PrDomElement* element = domElementCreate(p->document, like->space, domElementTag(like), domElementName(like));
    for (uint32_t i = 0; i < entry.attributeCount; ++i)
        domSetAttribute(element, PrDomNamespace_None, prStringView(&entry.attributes[i].name),
            prStringView(&entry.attributes[i].value));
    return element;
}

// Creates an element for the token, or for the start tag of `entry` if one is being recreated
// (html/#creating-and-inserting-nodes). The element is held until the stack holds it.
PrDomElement* insertElement(Tree* t, DomNamespaceId space, PrDomTag tag, PrStringView local,
        const HtmlFormatting* PR_NULLABLE entry) {
    PrDomElement* element = nullptr;
    bool constructed = false;
    if (entry != nullptr) {
        element = remake(t->p, *entry);
        domNodeRetain(element);
    } else if (space == PrDomNamespace_Html) {
        // html/#create-an-element-for-the-token: an element that a definition creates outside a fragment parse is
        // constructed immediately, and its reactions run before the parser continues.
        PrStringView is = tokenAttribute(t, "is");
        const PrDomCustomElementRegistry* registry = t->p->document->customElements;
        bool script = registry != nullptr && t->p->context == nullptr && registry->lookup(registry->context,
            local.data != nullptr ? local : domTagName(tag), is) != nullptr;
        if (script) {
            ++t->p->document->throwOnDynamicMarkupInsertion;
            domCustomReactionsPush(registry->agent);
        }
        element = domElementCreateHtml(t->p->document, tag, local.data != nullptr ? local : domTagName(tag), is, script,
            &constructed);
        domNodeRetain(element);
        addAttributes(t, element, space);
        if (script) {
            domCustomReactionsPop(registry->agent);
            --t->p->document->throwOnDynamicMarkupInsertion;
        }
    } else {
        element = domElementCreate(t->p->document, space, tag, local);
        domNodeRetain(element);
        addAttributes(t, element, space);
    }
    // html/#parsing-main-inhead: a script that a fragment parse creates never runs (html/#scripting-mode-inert).
    domScriptParserCreated(element, t->p->context != nullptr);

    PrDomNode* parent = nullptr;
    PrDomNode* before = nullptr;
    insertionPlaceIn(t, current(t), &parent, &before);
    // html/#create-an-element-for-the-token: a form-associated element takes the form element pointer as its form owner
    // if no template is open, it has no form attribute to name another form, and it goes into the form's tree.
    PrDomElement* form = t->p->form;
    if (form != nullptr && domIsFormAssociated(element) && !hasOpen(t, PrDomTag_Template) &&
            (!domIsListed(element) || domAttributeValue(element, "form").data == nullptr) &&
            domNodeRoot(parent) == domNodeRoot(form))
        domFormAssociateParserInserted(element, form);
    insertAt(parent, element, before);
    // A constructed element is held until its tree holds its object.
    if (constructed)
        domNodeUnhold(element);
    // A link's insertion steps obtain its sheet, a meta's insertion steps set its document's default language, embedded
    // content loads, a selectedcontent element reflects its select, a use element joins its document's uses, and an
    // element with an autofocus attribute becomes an autofocus candidate. The parser runs no other element's insertion
    // steps during parsing.
    if (tag == PrDomTag_Link)
        htmlLinkUpdate(element, DomInsertedBy_Parser);
    else if (tag == PrDomTag_Meta)
        domMetaConnected(element);
    else
        domEmbedChanged(element, {});
    domSelectedContentConnected(element);
    if (domIsConnected(element))
        domUseConnectionChanged(element);
    domAutofocusInserted(element);
    pushHeld(&t->p->open, element);
    domNodeRelease(element);
    return element;
}

PrDomElement* insertHtmlElement(Tree* t) {
    return insertElement(t, PrDomNamespace_Html, t->tag, t->name, nullptr);
}

// html/#parsing-main-inhead, for a meta element that declares the document's charset in its charset attribute, or in a
// Content-Type pragma. The host changes the encoding if the parse is not yet certain of it. Past the first kilobyte,
// which the prescan reads, only a meta element in the head declares a charset: one that the in head insertion mode
// inserts itself, not on behalf of another mode or a template. One that document.write() wrote declares none. Both are
// as whatwg/html#6962 proposes.
void declareCharset(Tree* t, PrDomElement* meta) {
    HtmlParser* p = t->p;
    uint32_t at = t->token.span.start;
    if (p->host.changeEncoding == nullptr || p->mode != HtmlInsertionMode_InHead ||
            (at >= p->writtenStart && at < p->writtenEnd))
        return;
    PrCharset charset = PrCharset_None;
    PrStringView label = domAttributeValue(meta, "charset");
    if (label.data != nullptr) {
        charset = prCharsetFromLabel(label);
    } else if (asciiEqualsFold(domAttributeValue(meta, "http-equiv"), "content-type")) {
        PrStringView content = domAttributeValue(meta, "content");
        if (content.data != nullptr)
            charset = htmlCharsetFromMetaContent(content);
    }
    if (charset != PrCharset_None && p->host.changeEncoding(p->host.context, charset))
        p->abandoned = true;
}

// html/#parsing-main-inhead, for a template start tag whose shadowrootmode attaches a declarative shadow root to the
// adjusted current node. The template is put on the stack of open elements but not in the tree, and its contents are
// the shadow root. Returns false if the template should be inserted like any other element.
bool attachDeclarativeShadow(Tree* t) {
    PrStringView mode = tokenAttribute(t, "shadowrootmode");
    PrDomElement* host = adjustedCurrent(t);
    if ((t->p->flags & HtmlParseFlag_DeclarativeShadowRoots) == 0 || mode.data == nullptr || host == t->p->open.data[0])
        return false;
    PrDomShadowRootInit init = prDomShadowRootInitDefault(PrDomShadowRootMode_Open);
    if (asciiEqualsFold(mode, "closed"))
        init.mode = PrDomShadowRootMode_Closed;
    else if (!asciiEqualsFold(mode, "open"))
        return false;
    init.clonable = tokenAttribute(t, "shadowrootclonable").data != nullptr;
    init.serializable = tokenAttribute(t, "shadowrootserializable").data != nullptr;
    init.delegatesFocus = tokenAttribute(t, "shadowrootdelegatesfocus").data != nullptr;
    // html/#attr-template-shadowrootslotassignment. The missing value default and the invalid value default are both
    // named.
    if (asciiEqualsFold(tokenAttribute(t, "shadowrootslotassignment"), "manual"))
        init.slotAssignment = PrDomSlotAssignmentMode_Manual;
    PrDomShadowRoot* shadow = nullptr;
    if (domAttachShadow(host, &init, &shadow) != PrStatus_Ok) {
        error(t);
        return false;
    }
    shadow->declarative = true;
    PrDomElement* element = domElementCreate(t->p->document, PrDomNamespace_Html, PrDomTag_Template, PrStringView{});
    addAttributes(t, element, PrDomNamespace_Html);
    domTemplateNameShadow(element, shadow);
    pushHeld(&t->p->open, element);
    return true;
}

void reconstructFormatting(Tree* t) {
    const Array<HtmlFormatting>& list = t->p->formatting;
    if (list.count == 0 || list.last().element == nullptr || isOpen(t, list.last().element))
        return;

    uint32_t entry = list.count - 1;
    while (entry != 0) {
        --entry;
        if (list.data[entry].element == nullptr || isOpen(t, list.data[entry].element)) {
            ++entry;
            break;
        }
    }
    for (; entry < list.count; ++entry) {
        const PrDomElement* held = list.data[entry].element;
        PrDomElement* made = insertElement(t, PrDomNamespace_Html, domElementTag(held),
            domElementName(held), &list.data[entry]);
        replaceFormatting(t->p, entry, made);
    }
}

// Inserts the element that a foreign start tag names, with the spelling that html/#parsing-main-inforeign gives it, and
// pops it at once if the token closes itself. Returns the element. A respelled name differs from the name that the
// tokenizer lowercased, so this is the one case that looks up a tag rather than using the token's tag.
PrDomElement* insertForeignElement(Tree* t, DomNamespaceId space) {
    PrStringView spelled = htmlAdjustedTag(space, t->name);
    PrDomElement* element;
    if (spelled.data == nullptr)
        element = insertElement(t, space, t->tag, t->name, nullptr);
    else
        element = insertElement(t, space, domTagFromText(spelled), spelled, nullptr);
    if ((t->token.flags & HtmlTokenFlag_SelfClosing) != 0)
        pop(t);
    return element;
}

// Processes an SVG script element that the parser has popped (html/#parsing-main-inforeign,
// SVG2/interact.html#ScriptElement). The host runs it as HTML prepares a script, with its href in place of src.
void processSvgScript(Tree* t, PrDomElement* script) {
    if (t->p->host.runScript != nullptr)
        t->p->status = t->p->host.runScript(t->p->host.context, script);
}

// An element that the tree builder creates itself. It has no attributes, because no token named it.
PrDomElement* insertBare(Tree* t, PrDomTag tag) {
    PrDomElement* element = domElementCreate(t->p->document, PrDomNamespace_Html, tag, PrStringView{});
    insertNode(t, element, nullptr);
    pushHeld(&t->p->open, element);
    return element;
}

void insertText(Tree* t, PrStringView data) {
    PrDomNode* parent = nullptr;
    PrDomNode* before = nullptr;
    insertionPlaceIn(t, current(t), &parent, &before);

    PrDomNode* neighbour = before != nullptr ? before->previous : parent->last;
    if (neighbour != nullptr && neighbour->type == PrDomNodeType_Text) {
        domAppendData(static_cast<PrDomText*>(neighbour), data);
        return;
    }
    if (parent->type == PrDomNodeType_Document)
        return;
    domAttach(parent, domTextCreate(t->p->document, data), before);
}

// What a mode does with the whitespace at the start of a character token (html/#parsing-main-inhtml).
enum {
    SpaceRule_Ignore,
    SpaceRule_Insert,
    SpaceRule_Reconstruct,   // inserted after reconstructing the formatting elements, the way "in body" inserts text
    SpaceRule_Count,
};
typedef uint8_t SpaceRule;

// Removes the whitespace at the start of a character token and handles it by `rule`. Returns true if nothing is left of
// the token. Otherwise the token holds the rest, for the mode to read next, if the rule stops at the first
// non-whitespace character.
bool takeWhitespace(Tree* t, SpaceRule rule) {
    uint32_t count = htmlTokenRun(&t->p->tokenizer, &t->token, HtmlTokenFlag_Whitespace);
    if (count != 0 && rule != SpaceRule_Ignore) {
        if (rule == SpaceRule_Reconstruct)
            reconstructFormatting(t);
        insertText(t, stringSlice(text(t, t->token.data), 0, count));
    }
    htmlTokenAdvance(&t->p->tokenizer, &t->token, count);
    return htmlSpanEmpty(t->token.data);
}

// For a mode that accepts only whitespace: handles every whitespace character of the token by `rule`, and reports a
// parse error for every other character (html/#parsing-main-inframeset, html/#parsing-main-afterframeset and
// html/#the-after-after-frameset-insertion-mode).
void takeSpaceOnly(Tree* t, SpaceRule rule) {
    while (!takeWhitespace(t, rule)) {
        HtmlTokenFlags other = HtmlTokenFlags(t->token.flags & ~HtmlTokenFlag_Whitespace);
        uint32_t count = htmlTokenRun(&t->p->tokenizer, &t->token, other);
        for (uint32_t i = 0; i < count; ++i)
            error(t);
        htmlTokenAdvance(&t->p->tokenizer, &t->token, count);
    }
}

void insertComment(Tree* t, PrDomNode* PR_NULLABLE into) {
    insertNode(t, domCommentCreate(t->p->document, text(t, t->token.data)), into);
}

void insertProcessingInstruction(Tree* t, PrDomNode* PR_NULLABLE into) {
    insertNode(t, domProcessingInstructionCreate(t->p->document, text(t, t->token.name), text(t, t->token.data)), into);
}

// ---------------------------------------------------------------------------------------------------------------------
// Text and the modes that return from it (html/#parsing-elements-that-contain-only-text)

void parseText(Tree* t, HtmlState state) {
    insertHtmlElement(t);
    if (state == HtmlState_ScriptData)
        t->p->scriptStart = t->token.span.end;
    htmlTokenizerSetState(&t->p->tokenizer, state, t->name);
    t->p->original = t->p->mode;
    t->p->mode = HtmlInsertionMode_Text;
}

void resetInsertionMode(Tree* t) {
    for (uint32_t i = t->p->open.count; i-- != 0;) {
        PrDomElement* element = t->p->open.data[i];
        bool last = i == 0;
        if (last && t->p->context != nullptr)
            element = t->p->context;

        if (element->space == PrDomNamespace_Html) {
            switch (element->tag) {
                case PrDomTag_Select: {
                    if (!last) {
                        for (uint32_t j = i; j-- != 0;) {
                            if (domIsHtml(t->p->open.data[j], PrDomTag_Template))
                                break;
                            if (domIsHtml(t->p->open.data[j], PrDomTag_Table)) {
                                t->p->mode = HtmlInsertionMode_InTable;
                                return;
                            }
                        }
                    }
                    t->p->mode = HtmlInsertionMode_InBody;
                    return;
                }
                case PrDomTag_Td: case PrDomTag_Th:
                    if (!last) {
                        t->p->mode = HtmlInsertionMode_InCell;
                        return;
                    }
                    break;
                case PrDomTag_Tr:      t->p->mode = HtmlInsertionMode_InRow; return;
                case PrDomTag_Tbody: case PrDomTag_Thead: case PrDomTag_Tfoot:
                    t->p->mode = HtmlInsertionMode_InTableBody; return;
                case PrDomTag_Caption: t->p->mode = HtmlInsertionMode_InCaption; return;
                case PrDomTag_Colgroup: t->p->mode = HtmlInsertionMode_InColumnGroup; return;
                case PrDomTag_Table:   t->p->mode = HtmlInsertionMode_InTable; return;
                case PrDomTag_Template:
                    t->p->mode = t->p->templateModes.count != 0 ? t->p->templateModes.last() : HtmlInsertionMode_InBody;
                    return;
                case PrDomTag_Head:
                    t->p->mode = last ? HtmlInsertionMode_InBody : HtmlInsertionMode_InHead;
                    return;
                case PrDomTag_Body:    t->p->mode = HtmlInsertionMode_InBody; return;
                case PrDomTag_Frameset: t->p->mode = HtmlInsertionMode_InFrameset; return;
                case PrDomTag_Html:
                    t->p->mode = t->p->head == nullptr ? HtmlInsertionMode_BeforeHead : HtmlInsertionMode_AfterHead;
                    return;
                default:
                    break;
            }
        }
        if (last) {
            t->p->mode = HtmlInsertionMode_InBody;
            return;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Distinguishing foreign content (html/#parsing-main-inforeign)

bool isHtmlIntegrationPoint(const PrDomElement* element) {
    if (element->space == PrDomNamespace_MathMl && element->tag == PrDomTag_AnnotationXml) {
        const PrDomAttr* encoding = domAttribute(element, PrDomNamespace_None, "encoding");
        return encoding != nullptr && (asciiEqualsFold(prStringView(&encoding->value), "text/html") ||
            asciiEqualsFold(prStringView(&encoding->value), "application/xhtml+xml"));
    }
    return isSvgIntegrationPoint(element);
}

bool inForeignContent(const Tree* t) {
    if (t->p->open.count == 0 || kind(t) == HtmlTokenKind_Eof)
        return false;

    const PrDomElement* adjusted = adjustedCurrent(t);
    if (adjusted->space == PrDomNamespace_Html)
        return false;
    if (isMathMlTextIntegrationPoint(adjusted) && (kind(t) == HtmlTokenKind_Character ||
            (kind(t) == HtmlTokenKind_StartTag && t->tag != PrDomTag_Mglyph && t->tag != PrDomTag_Malignmark)))
        return false;
    if (adjusted->space == PrDomNamespace_MathMl &&
            adjusted->tag == PrDomTag_AnnotationXml && kind(t) == HtmlTokenKind_StartTag && t->tag == PrDomTag_Svg)
        return false;
    if (isHtmlIntegrationPoint(adjusted) && (kind(t) == HtmlTokenKind_StartTag || kind(t) == HtmlTokenKind_Character))
        return false;
    return true;
}

// Pops until the current node is one against which an HTML token can be handled (html/#parsing-main-inforeign).
void breakOutOfForeign(Tree* t) {
    while (t->p->open.count != 0 && current(t)->space != PrDomNamespace_Html &&
            !isMathMlTextIntegrationPoint(current(t)) && !isHtmlIntegrationPoint(current(t)))
        pop(t);
}

// Processes the token by the rules that `mode` gives. In html/#parsing-main-inhtml, several modes process a token with
// another mode's rules. That leaves the parser in its current mode unless those rules change it, so the mode is an
// argument rather than a place to write to. Those modes are defined below, and call back through this function.
bool processIn(Tree* t, HtmlInsertionMode mode);

bool processMode(Tree* t) { return processIn(t, t->p->mode); }

// ---------------------------------------------------------------------------------------------------------------------
// The adoption agency algorithm (html/#parsing-main-inbody)

// The "any other end tag" steps. The adoption agency falls back on them, and they call the adoption agency in turn.
bool inBodyEnd(Tree* t);

void adoptionAgency(Tree* t, PrDomTag subject) {
    if (domIsHtml(current(t), subject) && formattingIndex(t, current(t)) == t->p->formatting.count) {
        pop(t);
        return;
    }

    for (uint32_t outer = 0; outer < 8; ++outer) {
        PrDomElement* formatting = activeFormatting(t, subject);
        if (formatting == nullptr) {
            inBodyEnd(t);
            return;
        }

        uint32_t bookmark = formattingIndex(t, formatting);
        if (!isOpen(t, formatting)) {
            error(t);
            eraseFormatting(t->p, bookmark);
            return;
        }
        if (!inScope(t, subject, Scope_Default)) {
            error(t);
            return;
        }
        if (formatting != current(t))
            error(t);

        uint32_t stackIndex = 0;
        while (t->p->open.data[stackIndex] != formatting)
            ++stackIndex;

        PrDomElement* PR_NULLABLE furthest = nullptr;
        uint32_t furthestIndex = 0;
        for (uint32_t i = stackIndex + 1; i < t->p->open.count; ++i) {
            if (isSpecial(t->p->open.data[i])) {
                furthest = t->p->open.data[i];
                furthestIndex = i;
                break;
            }
        }

        if (furthest == nullptr) {
            truncateOpen(t->p, stackIndex);
            eraseFormatting(t->p, bookmark);
            return;
        }

        PrDomElement* ancestor = t->p->open.data[stackIndex - 1];
        PrDomElement* node = furthest;
        PrDomElement* lastNode = furthest;
        uint32_t nodeIndex = furthestIndex;

        for (uint32_t inner = 1;; ++inner) {
            --nodeIndex;
            node = t->p->open.data[nodeIndex];
            if (node == formatting)
                break;

            uint32_t entry = formattingIndex(t, node);
            if (inner > 3 && entry != t->p->formatting.count) {
                eraseFormatting(t->p, entry);
                if (entry < bookmark)
                    --bookmark;
                entry = t->p->formatting.count;
            }
            if (entry == t->p->formatting.count) {
                eraseHeld(&t->p->open, nodeIndex);
                if (nodeIndex < furthestIndex)
                    --furthestIndex;
                continue;
            }

            PrDomElement* made = remake(t->p, t->p->formatting.data[entry]);
            replaceFormatting(t->p, entry, made);
            replaceHeld(&t->p->open, nodeIndex, made);
            node = made;

            if (lastNode == furthest)
                bookmark = entry + 1;
            insertAt(node, lastNode, nullptr);
            lastNode = node;
        }

        PrDomNode* parent = nullptr;
        PrDomNode* before = nullptr;
        insertionPlaceIn(t, ancestor, &parent, &before);
        insertAt(parent, lastNode, before);

        uint32_t entry = formattingIndex(t, formatting);
        PrDomElement* made = remake(t->p, t->p->formatting.data[entry]);
        while (furthest->first != nullptr)
            insertAt(made, furthest->first, nullptr);
        insertAt(furthest, made, nullptr);

        // The new element is held here until the stack and the list hold it. Its entry, for the same start tag, moves
        // to the bookmark.
        hold(made);
        HtmlFormatting moved = t->p->formatting.data[entry];
        t->p->formatting.erase(entry);
        domNodeRelease(moved.element);
        if (entry < bookmark)
            --bookmark;
        if (bookmark > t->p->formatting.count)
            bookmark = t->p->formatting.count;
        moved.element = made;
        hold(made);
        t->p->formatting.insert(bookmark, moved);

        for (uint32_t i = 0; i < t->p->open.count; ++i) {
            if (t->p->open.data[i] == formatting) {
                eraseHeld(&t->p->open, i);
                if (i < furthestIndex)
                    --furthestIndex;
                break;
            }
        }
        insertHeld(&t->p->open, furthestIndex + 1, made);
        domNodeRelease(made);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// In body (html/#parsing-main-inbody)

// The end tag entries that have no tag of their own: the "any other end tag" step.
bool inBodyEnd(Tree* t) {
    for (uint32_t i = t->p->open.count; i-- != 0;) {
        PrDomElement* node = t->p->open.data[i];
        if (node->space == PrDomNamespace_Html && domElementTag(node) == t->tag && domElementName(node) == t->name) {
            generateImpliedEndTagsExcept(t, t->tag);
            if (node != current(t))
                error(t);
            truncateOpen(t->p, i);
            return false;
        }
        if (isSpecial(node)) {
            error(t);
            return false;
        }
    }
    return false;
}

// Returns true if the open element ends the search that the li, dd and dt start tags make for an element to close. Such
// an element is a special element other than address, div and p.
bool endsListItemSearch(const PrDomElement* element) {
    return isSpecial(element) && !domIsHtml(element, PrDomTag_Address) &&
        !domIsHtml(element, PrDomTag_Div) && !domIsHtml(element, PrDomTag_P);
}

bool inBodyStart(Tree* t) {
    switch (t->tag) {
        case PrDomTag_Html:
            error(t);
            if (hasOpen(t, PrDomTag_Template))
                return false;
            mergeAttributes(t, t->p->open.data[0]);
            return false;

        case PrDomTag_Base: case PrDomTag_Basefont: case PrDomTag_Bgsound: case PrDomTag_Link:
        case PrDomTag_Meta: case PrDomTag_Noframes: case PrDomTag_Script: case PrDomTag_Style:
        case PrDomTag_Template: case PrDomTag_Title:
            return processIn(t, HtmlInsertionMode_InHead);

        case PrDomTag_Body: {
            error(t);
            if (t->p->open.count < 2 || !domIsHtml(t->p->open.data[1], PrDomTag_Body) || hasOpen(t, PrDomTag_Template))
                return false;
            t->p->framesetOk = false;
            mergeAttributes(t, t->p->open.data[1]);
            return false;
        }

        case PrDomTag_Frameset: {
            error(t);
            if (t->p->open.count < 2 || !domIsHtml(t->p->open.data[1], PrDomTag_Body) || !t->p->framesetOk)
                return false;
            PrDomNode* body = t->p->open.data[1];
            if (body->parent != nullptr)
                domDetach(body);
            truncateOpen(t->p, 1);
            insertHtmlElement(t);
            t->p->mode = HtmlInsertionMode_InFrameset;
            return false;
        }

        case PrDomTag_Address: case PrDomTag_Article: case PrDomTag_Aside: case PrDomTag_Blockquote:
        case PrDomTag_Center: case PrDomTag_Details: case PrDomTag_Dialog: case PrDomTag_Dir:
        case PrDomTag_Div: case PrDomTag_Dl: case PrDomTag_Fieldset: case PrDomTag_Figcaption:
        case PrDomTag_Figure: case PrDomTag_Footer: case PrDomTag_Header: case PrDomTag_Hgroup:
        case PrDomTag_Main: case PrDomTag_Menu: case PrDomTag_Nav: case PrDomTag_Ol:
        case PrDomTag_P: case PrDomTag_Search: case PrDomTag_Section: case PrDomTag_Summary:
        case PrDomTag_Ul:
            if (inScope(t, PrDomTag_P, Scope_Button))
                closeParagraph(t);
            insertHtmlElement(t);
            return false;

        case PrDomTag_H1: case PrDomTag_H2: case PrDomTag_H3:
        case PrDomTag_H4: case PrDomTag_H5: case PrDomTag_H6:
            if (inScope(t, PrDomTag_P, Scope_Button))
                closeParagraph(t);
            if (isHeading(current(t))) {
                error(t);
                pop(t);
            }
            insertHtmlElement(t);
            return false;

        case PrDomTag_Pre: case PrDomTag_Listing:
            if (inScope(t, PrDomTag_P, Scope_Button))
                closeParagraph(t);
            insertHtmlElement(t);
            t->p->framesetOk = false;
            t->p->skipNewline = true;
            return false;

        case PrDomTag_Form: {
            bool hasTemplate = parsingTemplateContents(t);
            if (t->p->form != nullptr && !hasTemplate) {
                error(t);
                return false;
            }
            if (inScope(t, PrDomTag_P, Scope_Button))
                closeParagraph(t);
            PrDomElement* form = insertHtmlElement(t);
            if (!hasTemplate)
                setPointer(&t->p->form, form);
            return false;
        }

        case PrDomTag_Li: {
            t->p->framesetOk = false;
            for (uint32_t i = t->p->open.count; i-- != 0;) {
                PrDomElement* node = t->p->open.data[i];
                if (domIsHtml(node, PrDomTag_Li)) {
                    generateImpliedEndTagsExcept(t, PrDomTag_Li);
                    if (!domIsHtml(current(t), PrDomTag_Li))
                        error(t);
                    popThrough(t, PrDomTag_Li);
                    break;
                }
                if (endsListItemSearch(node))
                    break;
            }
            if (inScope(t, PrDomTag_P, Scope_Button))
                closeParagraph(t);
            insertHtmlElement(t);
            return false;
        }

        case PrDomTag_Dd: case PrDomTag_Dt: {
            t->p->framesetOk = false;
            for (uint32_t i = t->p->open.count; i-- != 0;) {
                PrDomElement* node = t->p->open.data[i];
                if (domIsHtml(node, PrDomTag_Dd) || domIsHtml(node, PrDomTag_Dt)) {
                    PrDomTag which = domElementTag(node);
                    generateImpliedEndTagsExcept(t, which);
                    if (!domIsHtml(current(t), which))
                        error(t);
                    popThrough(t, which);
                    break;
                }
                if (endsListItemSearch(node))
                    break;
            }
            if (inScope(t, PrDomTag_P, Scope_Button))
                closeParagraph(t);
            insertHtmlElement(t);
            return false;
        }

        case PrDomTag_Plaintext:
            if (inScope(t, PrDomTag_P, Scope_Button))
                closeParagraph(t);
            insertHtmlElement(t);
            htmlTokenizerSetState(&t->p->tokenizer, HtmlState_Plaintext, PrStringView{});
            return false;

        case PrDomTag_Button:
            if (inScope(t, PrDomTag_Button, Scope_Default)) {
                error(t);
                generateImpliedEndTags(t);
                popThrough(t, PrDomTag_Button);
            }
            reconstructFormatting(t);
            insertHtmlElement(t);
            t->p->framesetOk = false;
            return false;

        case PrDomTag_A: {
            PrDomElement* open = activeFormatting(t, PrDomTag_A);
            if (open != nullptr) {
                error(t);
                adoptionAgency(t, PrDomTag_A);
                uint32_t entry = formattingIndex(t, open);
                if (entry != t->p->formatting.count)
                    eraseFormatting(t->p, entry);
                for (uint32_t i = 0; i < t->p->open.count; ++i) {
                    if (t->p->open.data[i] == open) {
                        eraseHeld(&t->p->open, i);
                        break;
                    }
                }
            }
            reconstructFormatting(t);
            pushFormatting(t, insertHtmlElement(t));
            return false;
        }

        case PrDomTag_B: case PrDomTag_Big: case PrDomTag_Code: case PrDomTag_Em:
        case PrDomTag_Font: case PrDomTag_I: case PrDomTag_S: case PrDomTag_Small:
        case PrDomTag_Strike: case PrDomTag_Strong: case PrDomTag_Tt: case PrDomTag_U:
            reconstructFormatting(t);
            pushFormatting(t, insertHtmlElement(t));
            return false;

        case PrDomTag_Nobr:
            reconstructFormatting(t);
            if (inScope(t, PrDomTag_Nobr, Scope_Default)) {
                error(t);
                adoptionAgency(t, PrDomTag_Nobr);
                reconstructFormatting(t);
            }
            pushFormatting(t, insertHtmlElement(t));
            return false;

        case PrDomTag_Applet: case PrDomTag_Marquee: case PrDomTag_Object:
            reconstructFormatting(t);
            insertHtmlElement(t);
            pushMarker(t->p);
            t->p->framesetOk = false;
            return false;

        case PrDomTag_Table:
            if (t->p->document->quirks != DomQuirks_Yes && inScope(t, PrDomTag_P, Scope_Button))
                closeParagraph(t);
            insertHtmlElement(t);
            t->p->framesetOk = false;
            t->p->mode = HtmlInsertionMode_InTable;
            return false;

        case PrDomTag_Area: case PrDomTag_Br: case PrDomTag_Embed: case PrDomTag_Img:
        case PrDomTag_Keygen: case PrDomTag_Wbr:
            reconstructFormatting(t);
            insertHtmlElement(t);
            pop(t);
            t->p->framesetOk = false;
            return false;

        case PrDomTag_Input: {
            if (domIsHtml(t->p->context, PrDomTag_Select)) {
                error(t);
                return false;
            }
            if (inScope(t, PrDomTag_Select, Scope_Default)) {
                error(t);
                popThrough(t, PrDomTag_Select);
            }
            reconstructFormatting(t);
            PrDomElement* input = insertHtmlElement(t);
            pop(t);
            const PrDomAttr* type = domAttribute(input, PrDomNamespace_None, "type");
            if (type == nullptr || !asciiEqualsFold(prStringView(&type->value), "hidden"))
                t->p->framesetOk = false;
            return false;
        }

        case PrDomTag_Param: case PrDomTag_Source: case PrDomTag_Track:
            insertHtmlElement(t);
            pop(t);
            return false;

        case PrDomTag_Hr:
            if (inScope(t, PrDomTag_P, Scope_Button))
                closeParagraph(t);
            if (inScope(t, PrDomTag_Select, Scope_Default)) {
                generateImpliedEndTags(t);
                if (inScope(t, PrDomTag_Option, Scope_Default) || inScope(t, PrDomTag_Optgroup, Scope_Default))
                    error(t);
            }
            insertHtmlElement(t);
            pop(t);
            t->p->framesetOk = false;
            return false;

        case PrDomTag_Image:
            error(t);
            t->name = "img";
            t->tag = PrDomTag_Img;
            return true;

        case PrDomTag_Textarea:
            insertHtmlElement(t);
            t->p->skipNewline = true;
            htmlTokenizerSetState(&t->p->tokenizer, HtmlState_Rcdata, t->name);
            t->p->framesetOk = false;
            t->p->original = t->p->mode;
            t->p->mode = HtmlInsertionMode_Text;
            return false;

        case PrDomTag_Xmp:
            if (inScope(t, PrDomTag_P, Scope_Button))
                closeParagraph(t);
            reconstructFormatting(t);
            t->p->framesetOk = false;
            parseText(t, HtmlState_Rawtext);
            return false;

        case PrDomTag_Iframe:
            t->p->framesetOk = false;
            parseText(t, HtmlState_Rawtext);
            return false;

        case PrDomTag_Noembed:
            parseText(t, HtmlState_Rawtext);
            return false;

        case PrDomTag_Noscript:
            if ((t->p->flags & HtmlParseFlag_Scripting) != 0) {
                parseText(t, HtmlState_Rawtext);
                return false;
            }
            reconstructFormatting(t);
            insertHtmlElement(t);
            return false;

        case PrDomTag_Select:
            if (domIsHtml(t->p->context, PrDomTag_Select)) {
                error(t);
                return false;
            }
            if (inScope(t, PrDomTag_Select, Scope_Default)) {
                error(t);
                popThrough(t, PrDomTag_Select);
                return false;
            }
            reconstructFormatting(t);
            insertHtmlElement(t);
            t->p->framesetOk = false;
            return false;

        case PrDomTag_Option:
            if (inScope(t, PrDomTag_Select, Scope_Default)) {
                generateImpliedEndTagsExcept(t, PrDomTag_Optgroup);
                if (inScope(t, PrDomTag_Option, Scope_Default))
                    error(t);
            } else if (domIsHtml(current(t), PrDomTag_Option)) {
                pop(t);
            }
            reconstructFormatting(t);
            insertHtmlElement(t);
            return false;

        case PrDomTag_Optgroup:
            if (inScope(t, PrDomTag_Select, Scope_Default)) {
                generateImpliedEndTags(t);
                if (inScope(t, PrDomTag_Option, Scope_Default) || inScope(t, PrDomTag_Optgroup, Scope_Default))
                    error(t);
            } else if (domIsHtml(current(t), PrDomTag_Option)) {
                pop(t);
            }
            reconstructFormatting(t);
            insertHtmlElement(t);
            return false;

        case PrDomTag_Rb: case PrDomTag_Rtc:
            if (inScope(t, PrDomTag_Ruby, Scope_Default)) {
                generateImpliedEndTags(t);
                if (!domIsHtml(current(t), PrDomTag_Ruby))
                    error(t);
            }
            insertHtmlElement(t);
            return false;

        case PrDomTag_Rp: case PrDomTag_Rt:
            if (inScope(t, PrDomTag_Ruby, Scope_Default)) {
                generateImpliedEndTagsExcept(t, PrDomTag_Rtc);
                if (!domIsHtml(current(t), PrDomTag_Rtc) && !domIsHtml(current(t), PrDomTag_Ruby))
                    error(t);
            }
            insertHtmlElement(t);
            return false;

        case PrDomTag_Math: case PrDomTag_Svg: {
            reconstructFormatting(t);
            DomNamespaceId space = t->tag == PrDomTag_Math ? PrDomNamespace_MathMl : PrDomNamespace_Svg;
            insertForeignElement(t, space);
            return false;
        }

        case PrDomTag_Caption: case PrDomTag_Col: case PrDomTag_Colgroup: case PrDomTag_Frame:
        case PrDomTag_Head: case PrDomTag_Tbody: case PrDomTag_Td: case PrDomTag_Tfoot:
        case PrDomTag_Th: case PrDomTag_Thead: case PrDomTag_Tr:
            error(t);
            return false;

        default:
            reconstructFormatting(t);
            insertHtmlElement(t);
            return false;
    }
}

bool inBody(Tree* t) {
    switch (kind(t)) {
        case HtmlTokenKind_Character:
            if ((t->token.flags & HtmlTokenFlag_Null) != 0) {
                error(t);
                return false;
            }
            reconstructFormatting(t);
            insertText(t, text(t, t->token.data));
            if ((t->token.flags & HtmlTokenFlag_Text) != 0)
                t->p->framesetOk = false;
            return false;

        case HtmlTokenKind_Comment:
            insertComment(t, nullptr);
            return false;

        case HtmlTokenKind_ProcessingInstruction:
            insertProcessingInstruction(t, nullptr);
            return false;

        case HtmlTokenKind_Doctype:
            error(t);
            return false;

        case HtmlTokenKind_Eof:
            if (t->p->templateModes.count != 0) {
                t->p->mode = HtmlInsertionMode_InTemplate;
                return true;
            }
            for (uint32_t i = 0; i < t->p->open.count; ++i) {
                if (!mayEndOpen(t->p->open.data[i])) {
                    error(t);
                    break;
                }
            }
            return false;

        case HtmlTokenKind_StartTag:
            return inBodyStart(t);

        case HtmlTokenKind_EndTag:
            break;

        default:
            return false;
    }

    switch (t->tag) {
        case PrDomTag_Template:
            return processIn(t, HtmlInsertionMode_InHead);

        case PrDomTag_Body:
            if (!inScope(t, PrDomTag_Body, Scope_Default)) {
                error(t);
                return false;
            }
            t->p->mode = HtmlInsertionMode_AfterBody;
            return false;

        case PrDomTag_Html:
            if (!inScope(t, PrDomTag_Body, Scope_Default)) {
                error(t);
                return false;
            }
            t->p->mode = HtmlInsertionMode_AfterBody;
            return true;

        case PrDomTag_Address: case PrDomTag_Article: case PrDomTag_Aside: case PrDomTag_Blockquote:
        case PrDomTag_Button: case PrDomTag_Center: case PrDomTag_Details: case PrDomTag_Dialog:
        case PrDomTag_Dir: case PrDomTag_Div: case PrDomTag_Dl: case PrDomTag_Fieldset:
        case PrDomTag_Figcaption: case PrDomTag_Figure: case PrDomTag_Footer: case PrDomTag_Header:
        case PrDomTag_Hgroup: case PrDomTag_Listing: case PrDomTag_Main: case PrDomTag_Menu:
        case PrDomTag_Nav: case PrDomTag_Ol: case PrDomTag_Pre: case PrDomTag_Search:
        case PrDomTag_Section: case PrDomTag_Summary: case PrDomTag_Ul:
            closeElement(t, t->tag, Scope_Default);
            return false;

        case PrDomTag_Form: {
            if (!parsingTemplateContents(t)) {
                // The pointer's reference is held here until the form leaves the stack.
                PrDomElement* PR_NULLABLE form = t->p->form;
                t->p->form = nullptr;
                if (form == nullptr || !isOpen(t, form)) {
                    error(t);
                    domNodeRelease(form);
                    return false;
                }
                generateImpliedEndTags(t);
                if (current(t) != form)
                    error(t);
                for (uint32_t i = t->p->open.count; i-- != 0;) {
                    if (t->p->open.data[i] == form) {
                        eraseHeld(&t->p->open, i);
                        break;
                    }
                }
                domNodeRelease(form);
                return false;
            }
            closeElement(t, PrDomTag_Form, Scope_Default);
            return false;
        }

        case PrDomTag_P:
            if (!inScope(t, PrDomTag_P, Scope_Button)) {
                error(t);
                insertBare(t, PrDomTag_P);
            }
            closeParagraph(t);
            return false;

        case PrDomTag_Li:
            closeElement(t, PrDomTag_Li, Scope_ListItem, PrDomTag_Li);
            return false;

        case PrDomTag_Dd: case PrDomTag_Dt:
            closeElement(t, t->tag, Scope_Default, t->tag);
            return false;

        case PrDomTag_H1: case PrDomTag_H2: case PrDomTag_H3:
        case PrDomTag_H4: case PrDomTag_H5: case PrDomTag_H6: {
            if (!inScopeHeading(t)) {
                error(t);
                return false;
            }
            generateImpliedEndTags(t);
            if (!domIsHtml(current(t), t->tag))
                error(t);
            while (t->p->open.count != 0) {
                bool found = isHeading(current(t));
                pop(t);
                if (found)
                    break;
            }
            return false;
        }

        case PrDomTag_A: case PrDomTag_B: case PrDomTag_Big: case PrDomTag_Code:
        case PrDomTag_Em: case PrDomTag_Font: case PrDomTag_I: case PrDomTag_Nobr:
        case PrDomTag_S: case PrDomTag_Small: case PrDomTag_Strike: case PrDomTag_Strong:
        case PrDomTag_Tt: case PrDomTag_U:
            adoptionAgency(t, t->tag);
            return false;

        case PrDomTag_Applet: case PrDomTag_Marquee: case PrDomTag_Object:
            if (closeElement(t, t->tag, Scope_Default))
                clearFormattingToMarker(t);
            return false;

        case PrDomTag_Br:
            error(t);
            reconstructFormatting(t);
            insertBare(t, PrDomTag_Br);
            pop(t);
            t->p->framesetOk = false;
            return false;

        default:
            return inBodyEnd(t);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Tables (html/#parsing-main-intable to html/#parsing-main-intemplate)

// Closes the cell `which`, leaving the parser in its row. Returns false, with a parse error, if the cell is not in
// table scope.
bool closeCell(Tree* t, PrDomTag which) {
    if (!closeElement(t, which, Scope_Table))
        return false;
    clearFormattingToMarker(t);
    t->p->mode = HtmlInsertionMode_InRow;
    return true;
}

// Closes the caption, leaving the parser in its table. Returns false, with a parse error, if the caption is not in
// table scope.
bool closeCaption(Tree* t) {
    if (!closeElement(t, PrDomTag_Caption, Scope_Table))
        return false;
    clearFormattingToMarker(t);
    t->p->mode = HtmlInsertionMode_InTable;
    return true;
}

// Closes the row that the stack holds in table scope, leaving the parser in its table body.
void closeRow(Tree* t) {
    clearToRowContext(t);
    pop(t);
    t->p->mode = HtmlInsertionMode_InTableBody;
}

// Closes the table body that the stack holds in table scope, leaving the parser in its table.
void closeTableBody(Tree* t) {
    clearToTableBodyContext(t);
    pop(t);
    t->p->mode = HtmlInsertionMode_InTable;
}

// Sets the current template insertion mode to `mode`, and reprocesses the token in it (html/#parsing-main-intemplate).
bool switchTemplateMode(Tree* t, HtmlInsertionMode mode) {
    if (t->p->templateModes.count != 0)
        t->p->templateModes.pop();
    t->p->templateModes.push(mode);
    t->p->mode = mode;
    return true;
}

// Closes the template that the stack holds, and leaves its template insertion mode.
void closeTemplate(Tree* t) {
    popThrough(t, PrDomTag_Template);
    clearFormattingToMarker(t);
    if (t->p->templateModes.count != 0)
        t->p->templateModes.pop();
    resetInsertionMode(t);
}

// The "in table" rules, which "in table body" and "in row" fall back on.
bool inTable(Tree* t) {
    switch (kind(t)) {
        case HtmlTokenKind_Character:
            if (domIsHtml(current(t), PrDomTag_Table) || domIsHtml(current(t), PrDomTag_Tbody) ||
                    domIsHtml(current(t), PrDomTag_Template) || domIsHtml(current(t), PrDomTag_Tfoot) ||
                    domIsHtml(current(t), PrDomTag_Thead) || domIsHtml(current(t), PrDomTag_Tr)) {
                textClear(&t->p->pending);
                t->p->pendingText = false;
                t->p->original = t->p->mode;
                t->p->mode = HtmlInsertionMode_InTableText;
                return true;
            }
            break;
        case HtmlTokenKind_Comment:
            insertComment(t, nullptr);
            return false;
        case HtmlTokenKind_ProcessingInstruction:
            insertProcessingInstruction(t, nullptr);
            return false;
        case HtmlTokenKind_Doctype:
            error(t);
            return false;
        case HtmlTokenKind_StartTag:
            switch (t->tag) {
                case PrDomTag_Caption:
                    clearToTableContext(t);
                    pushMarker(t->p);
                    insertHtmlElement(t);
                    t->p->mode = HtmlInsertionMode_InCaption;
                    return false;
                case PrDomTag_Colgroup:
                    clearToTableContext(t);
                    insertHtmlElement(t);
                    t->p->mode = HtmlInsertionMode_InColumnGroup;
                    return false;
                case PrDomTag_Col:
                    clearToTableContext(t);
                    insertBare(t, PrDomTag_Colgroup);
                    t->p->mode = HtmlInsertionMode_InColumnGroup;
                    return true;
                case PrDomTag_Tbody: case PrDomTag_Tfoot: case PrDomTag_Thead:
                    clearToTableContext(t);
                    insertHtmlElement(t);
                    t->p->mode = HtmlInsertionMode_InTableBody;
                    return false;
                case PrDomTag_Td: case PrDomTag_Th: case PrDomTag_Tr:
                    clearToTableContext(t);
                    insertBare(t, PrDomTag_Tbody);
                    t->p->mode = HtmlInsertionMode_InTableBody;
                    return true;
                case PrDomTag_Table:
                    error(t);
                    if (!inScope(t, PrDomTag_Table, Scope_Table))
                        return false;
                    popThrough(t, PrDomTag_Table);
                    resetInsertionMode(t);
                    return true;
                case PrDomTag_Style: case PrDomTag_Script: case PrDomTag_Template:
                    return processIn(t, HtmlInsertionMode_InHead);
                case PrDomTag_Input: {
                    const HtmlAttribute* PR_NULLABLE type = nullptr;
                    for (uint32_t i = 0; i < t->p->tokenizer.attributes.count; ++i) {
                        if (text(t, t->p->tokenizer.attributes.data[i].name) == "type")
                            type = &t->p->tokenizer.attributes.data[i];
                    }
                    if (type == nullptr || !asciiEqualsFold(text(t, type->value), "hidden"))
                        break;
                    error(t);
                    insertHtmlElement(t);
                    pop(t);
                    return false;
                }
                case PrDomTag_Form: {
                    error(t);
                    if (t->p->form != nullptr && !parsingTemplateContents(t))
                        return false;
                    PrDomElement* form = insertHtmlElement(t);
                    if (!parsingTemplateContents(t))
                        setPointer(&t->p->form, form);
                    pop(t);
                    return false;
                }
                default:
                    break;
            }
            break;
        case HtmlTokenKind_EndTag:
            switch (t->tag) {
                case PrDomTag_Table:
                    if (!inScope(t, PrDomTag_Table, Scope_Table)) {
                        error(t);
                        return false;
                    }
                    popThrough(t, PrDomTag_Table);
                    resetInsertionMode(t);
                    return false;
                case PrDomTag_Body: case PrDomTag_Caption: case PrDomTag_Col: case PrDomTag_Colgroup:
                case PrDomTag_Html: case PrDomTag_Tbody: case PrDomTag_Td: case PrDomTag_Tfoot:
                case PrDomTag_Th: case PrDomTag_Thead: case PrDomTag_Tr:
                    error(t);
                    return false;
                case PrDomTag_Template:
                    return processIn(t, HtmlInsertionMode_InHead);
                default:
                    break;
            }
            break;
        case HtmlTokenKind_Eof:
            return inBody(t);
        default:
            break;
    }
    error(t);
    t->p->fosterParenting = true;
    {
        bool again = inBody(t);
        t->p->fosterParenting = false;
        return again;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Foreign content (html/#parsing-main-inforeign)

bool processForeign(Tree* t) {
    switch (kind(t)) {
        case HtmlTokenKind_Character: {
            if ((t->token.flags & HtmlTokenFlag_Null) != 0) {
                PrStringView nulls = text(t, t->token.data);
                for (size_t i = 0; i < nulls.size; ++i) {
                    error(t);
                    insertText(t, "\xef\xbf\xbd");
                }
                return false;
            }
            if ((t->token.flags & HtmlTokenFlag_Text) != 0)
                t->p->framesetOk = false;
            insertText(t, text(t, t->token.data));
            return false;
        }

        case HtmlTokenKind_Comment:
            insertComment(t, nullptr);
            return false;

        case HtmlTokenKind_ProcessingInstruction:
            insertProcessingInstruction(t, nullptr);
            return false;

        case HtmlTokenKind_Doctype:
            error(t);
            return false;

        case HtmlTokenKind_StartTag: {
            bool breakOut = breaksOutOfForeign(t->tag);
            if (!breakOut && t->tag == PrDomTag_Font) {
                for (uint32_t i = 0; i < t->p->tokenizer.attributes.count; ++i) {
                    PrStringView attribute = text(t, t->p->tokenizer.attributes.data[i].name);
                    breakOut = breakOut || attribute == "color" || attribute == "face" || attribute == "size";
                }
            }

            if (breakOut) {
                error(t);
                breakOutOfForeign(t);
                return processMode(t);
            }

            DomNamespaceId space = adjustedCurrent(t)->space;
            bool script = t->tag == PrDomTag_Script && space == PrDomNamespace_Svg;
            if (script)
                t->p->scriptStart = t->token.span.end;
            PrDomElement* element = insertForeignElement(t, space);
            // A script that closes itself is processed as its end tag would process it.
            if (script && (t->token.flags & HtmlTokenFlag_SelfClosing) != 0)
                processSvgScript(t, element);
            return false;
        }

        case HtmlTokenKind_EndTag: {
            if (t->tag == PrDomTag_Br || t->tag == PrDomTag_P) {
                error(t);
                breakOutOfForeign(t);
                return processMode(t);
            }

            PrDomElement* currentNode = current(t);
            if (t->tag == PrDomTag_Script && currentNode->space == PrDomNamespace_Svg &&
                    currentNode->tag == PrDomTag_Script) {
                pop(t);
                processSvgScript(t, currentNode);
                return false;
            }

            uint32_t i = t->p->open.count - 1;
            if (!asciiEqualsFold(domElementName(t->p->open.data[i]), t->name))
                error(t);
            for (;;) {
                if (i == 0)
                    return false;
                if (asciiEqualsFold(domElementName(t->p->open.data[i]), t->name)) {
                    truncateOpen(t->p, i);
                    return false;
                }
                --i;
                if (t->p->open.data[i]->space == PrDomNamespace_Html)
                    return processMode(t);
            }
        }

        default:
            return false;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Selecting the mode a token is read in (html/#parsing-main-inhtml)

bool processIn(Tree* t, HtmlInsertionMode mode) {
    switch (mode) {
        case HtmlInsertionMode_Initial:
            switch (kind(t)) {
                case HtmlTokenKind_Character:
                    if (takeWhitespace(t, SpaceRule_Ignore))
                        return false;
                    break;
                case HtmlTokenKind_Comment:
                    insertComment(t, t->p->document);
                    return false;
                case HtmlTokenKind_ProcessingInstruction:
                    insertProcessingInstruction(t, t->p->document);
                    return false;
                case HtmlTokenKind_Doctype: {
                    PrStringView doctypeName =
                        (t->token.flags & HtmlTokenFlag_NoName) != 0 ? PrStringView{""} : text(t, t->token.name);
                    bool hasPublic = (t->token.flags & HtmlTokenFlag_NoPublicId) == 0;
                    bool hasSystem = (t->token.flags & HtmlTokenFlag_NoSystemId) == 0;
                    PrStringView publicId = hasPublic ? text(t, t->token.publicId) : PrStringView{""};
                    PrStringView systemId = hasSystem ? text(t, t->token.systemId) : PrStringView{""};
                    domAttach(t->p->document, domDocumentTypeCreate(t->p->document, doctypeName, publicId, systemId), nullptr);
                    t->p->document->quirks = quirksOf(doctypeName, publicId, systemId,
                        (t->token.flags & HtmlTokenFlag_ForceQuirks) != 0, hasPublic, hasSystem);
                    t->p->mode = HtmlInsertionMode_BeforeHtml;
                    return false;
                }
                default:
                    break;
            }
            t->p->document->quirks = DomQuirks_Yes;
            t->p->mode = HtmlInsertionMode_BeforeHtml;
            return true;

        case HtmlInsertionMode_BeforeHtml:
            switch (kind(t)) {
                case HtmlTokenKind_Doctype:
                    error(t);
                    return false;
                case HtmlTokenKind_Comment:
                    insertComment(t, t->p->document);
                    return false;
                case HtmlTokenKind_ProcessingInstruction:
                    insertProcessingInstruction(t, t->p->document);
                    return false;
                case HtmlTokenKind_Character:
                    if (takeWhitespace(t, SpaceRule_Ignore))
                        return false;
                    break;
                case HtmlTokenKind_StartTag:
                    if (t->tag == PrDomTag_Html) {
                        PrDomElement* html = domElementCreate(t->p->document, PrDomNamespace_Html, t->tag, t->name);
                        addAttributes(t, html, PrDomNamespace_Html);
                        domAttach(t->p->document, html, nullptr);
                        pushHeld(&t->p->open, html);
                        t->p->mode = HtmlInsertionMode_BeforeHead;
                        return false;
                    }
                    break;
                case HtmlTokenKind_EndTag:
                    if (t->tag != PrDomTag_Head && t->tag != PrDomTag_Body && t->tag != PrDomTag_Html &&
                            t->tag != PrDomTag_Br) {
                        error(t);
                        return false;
                    }
                    break;
                default:
                    break;
            }
            {
                PrDomElement* html =
                    domElementCreate(t->p->document, PrDomNamespace_Html, PrDomTag_Html, PrStringView{});
                domAttach(t->p->document, html, nullptr);
                pushHeld(&t->p->open, html);
            }
            t->p->mode = HtmlInsertionMode_BeforeHead;
            return true;

        case HtmlInsertionMode_BeforeHead:
            switch (kind(t)) {
                case HtmlTokenKind_Character:
                    if (takeWhitespace(t, SpaceRule_Ignore))
                        return false;
                    break;
                case HtmlTokenKind_Comment:
                    insertComment(t, nullptr);
                    return false;
                case HtmlTokenKind_ProcessingInstruction:
                    insertProcessingInstruction(t, nullptr);
                    return false;
                case HtmlTokenKind_Doctype:
                    error(t);
                    return false;
                case HtmlTokenKind_StartTag:
                    if (t->tag == PrDomTag_Html)
                        return inBody(t);
                    if (t->tag == PrDomTag_Head) {
                        setPointer(&t->p->head, insertHtmlElement(t));
                        t->p->mode = HtmlInsertionMode_InHead;
                        return false;
                    }
                    break;
                case HtmlTokenKind_EndTag:
                    if (t->tag != PrDomTag_Head && t->tag != PrDomTag_Body && t->tag != PrDomTag_Html &&
                            t->tag != PrDomTag_Br) {
                        error(t);
                        return false;
                    }
                    break;
                default:
                    break;
            }
            setPointer(&t->p->head, insertBare(t, PrDomTag_Head));
            t->p->mode = HtmlInsertionMode_InHead;
            return true;

        case HtmlInsertionMode_InHead:
            switch (kind(t)) {
                case HtmlTokenKind_Character:
                    if (takeWhitespace(t, SpaceRule_Insert))
                        return false;
                    break;
                case HtmlTokenKind_Comment:
                    insertComment(t, nullptr);
                    return false;
                case HtmlTokenKind_ProcessingInstruction:
                    insertProcessingInstruction(t, nullptr);
                    return false;
                case HtmlTokenKind_Doctype:
                    error(t);
                    return false;
                case HtmlTokenKind_StartTag:
                    switch (t->tag) {
                        case PrDomTag_Html:
                            return inBody(t);
                        case PrDomTag_Base: case PrDomTag_Basefont: case PrDomTag_Bgsound: case PrDomTag_Link:
                            insertHtmlElement(t);
                            pop(t);
                            return false;
                        case PrDomTag_Meta:
                            declareCharset(t, insertHtmlElement(t));
                            pop(t);
                            return false;
                        case PrDomTag_Title:
                            parseText(t, HtmlState_Rcdata);
                            return false;
                        case PrDomTag_Noscript:
                            if ((t->p->flags & HtmlParseFlag_Scripting) != 0) {
                                parseText(t, HtmlState_Rawtext);
                                return false;
                            }
                            insertHtmlElement(t);
                            t->p->mode = HtmlInsertionMode_InHeadNoscript;
                            return false;
                        case PrDomTag_Noframes: case PrDomTag_Style:
                            parseText(t, HtmlState_Rawtext);
                            return false;
                        case PrDomTag_Script:
                            parseText(t, HtmlState_ScriptData);
                            return false;
                        case PrDomTag_Template:
                            if (!attachDeclarativeShadow(t))
                                insertHtmlElement(t);
                            pushMarker(t->p);
                            t->p->framesetOk = false;
                            t->p->mode = HtmlInsertionMode_InTemplate;
                            t->p->templateModes.push(HtmlInsertionMode_InTemplate);
                            return false;
                        case PrDomTag_Head:
                            error(t);
                            return false;
                        default:
                            break;
                    }
                    break;
                case HtmlTokenKind_EndTag:
                    switch (t->tag) {
                        case PrDomTag_Head:
                            pop(t);
                            t->p->mode = HtmlInsertionMode_AfterHead;
                            return false;
                        case PrDomTag_Body: case PrDomTag_Html: case PrDomTag_Br:
                            break;
                        case PrDomTag_Template: {
                            if (!hasOpen(t, PrDomTag_Template)) {
                                error(t);
                                return false;
                            }
                            generateImpliedEndTagsThoroughly(t);
                            if (!domIsHtml(current(t), PrDomTag_Template))
                                error(t);
                            closeTemplate(t);
                            return false;
                        }
                        default:
                            error(t);
                            return false;
                    }
                    break;
                default:
                    break;
            }
            pop(t);
            t->p->mode = HtmlInsertionMode_AfterHead;
            return true;

        case HtmlInsertionMode_InHeadNoscript:
            switch (kind(t)) {
                case HtmlTokenKind_Doctype:
                    error(t);
                    return false;
                case HtmlTokenKind_StartTag:
                    if (t->tag == PrDomTag_Html)
                        return inBody(t);
                    if (t->tag == PrDomTag_Basefont || t->tag == PrDomTag_Bgsound || t->tag == PrDomTag_Link ||
                            t->tag == PrDomTag_Meta || t->tag == PrDomTag_Noframes || t->tag == PrDomTag_Style) {
                        return processIn(t, HtmlInsertionMode_InHead);
                    }
                    if (t->tag == PrDomTag_Head || t->tag == PrDomTag_Noscript) {
                        error(t);
                        return false;
                    }
                    break;
                case HtmlTokenKind_EndTag:
                    if (t->tag == PrDomTag_Noscript) {
                        pop(t);
                        t->p->mode = HtmlInsertionMode_InHead;
                        return false;
                    }
                    if (t->tag != PrDomTag_Br) {
                        error(t);
                        return false;
                    }
                    break;
                case HtmlTokenKind_Character:
                    if (takeWhitespace(t, SpaceRule_Insert))
                        return false;
                    break;
                case HtmlTokenKind_Comment:
                case HtmlTokenKind_ProcessingInstruction:
                    return processIn(t, HtmlInsertionMode_InHead);
                default:
                    break;
            }
            error(t);
            pop(t);
            t->p->mode = HtmlInsertionMode_InHead;
            return true;

        case HtmlInsertionMode_AfterHead:
            switch (kind(t)) {
                case HtmlTokenKind_Character:
                    if (takeWhitespace(t, SpaceRule_Insert))
                        return false;
                    break;
                case HtmlTokenKind_Comment:
                    insertComment(t, nullptr);
                    return false;
                case HtmlTokenKind_ProcessingInstruction:
                    insertProcessingInstruction(t, nullptr);
                    return false;
                case HtmlTokenKind_Doctype:
                    error(t);
                    return false;
                case HtmlTokenKind_StartTag:
                    switch (t->tag) {
                        case PrDomTag_Html:
                            return inBody(t);
                        case PrDomTag_Body:
                            insertHtmlElement(t);
                            t->p->framesetOk = false;
                            t->p->mode = HtmlInsertionMode_InBody;
                            return false;
                        case PrDomTag_Frameset:
                            insertHtmlElement(t);
                            t->p->mode = HtmlInsertionMode_InFrameset;
                            return false;
                        case PrDomTag_Base: case PrDomTag_Basefont: case PrDomTag_Bgsound: case PrDomTag_Link:
                        case PrDomTag_Meta: case PrDomTag_Noframes: case PrDomTag_Script: case PrDomTag_Style:
                        case PrDomTag_Template: case PrDomTag_Title: {
                            error(t);
                            if (t->p->head != nullptr)
                                pushHeld(&t->p->open, t->p->head);
                            bool again = processIn(t, HtmlInsertionMode_InHead);
                            for (uint32_t i = t->p->open.count; i-- != 0;) {
                                if (t->p->open.data[i] == t->p->head) {
                                    eraseHeld(&t->p->open, i);
                                    break;
                                }
                            }
                            return again;
                        }
                        case PrDomTag_Head:
                            error(t);
                            return false;
                        default:
                            break;
                    }
                    break;
                case HtmlTokenKind_EndTag:
                    if (t->tag == PrDomTag_Template) {
                        return processIn(t, HtmlInsertionMode_InHead);
                    }
                    if (t->tag != PrDomTag_Body && t->tag != PrDomTag_Html && t->tag != PrDomTag_Br) {
                        error(t);
                        return false;
                    }
                    break;
                default:
                    break;
            }
            insertBare(t, PrDomTag_Body);
            t->p->mode = HtmlInsertionMode_InBody;
            return true;

        case HtmlInsertionMode_InBody:
            return inBody(t);

        case HtmlInsertionMode_Text:
            switch (kind(t)) {
                case HtmlTokenKind_Character:
                    insertText(t, text(t, t->token.data));
                    return false;
                case HtmlTokenKind_Eof:
                    error(t);
                    cssStyleBlockUpdate(current(t), DomInsertedBy_Parser);
                    pop(t);
                    t->p->mode = t->p->original;
                    return true;
                case HtmlTokenKind_EndTag: {
                    PrDomElement* open = current(t);
                    pop(t);
                    t->p->mode = t->p->original;
                    cssStyleBlockUpdate(open, DomInsertedBy_Parser);
                    if (t->tag == PrDomTag_Script && t->p->host.runScript != nullptr)
                        t->p->status = t->p->host.runScript(t->p->host.context, open);
                    return false;
                }
                default:
                    return false;
            }

        case HtmlInsertionMode_InTable:
            return inTable(t);

        case HtmlInsertionMode_InTableText:
            if (kind(t) == HtmlTokenKind_Character) {
                if ((t->token.flags & HtmlTokenFlag_Null) != 0) {
                    error(t);
                    return false;
                }
                if ((t->token.flags & HtmlTokenFlag_Text) != 0)
                    t->p->pendingText = true;
                textAppend(&t->p->pending, text(t, t->token.data));
                return false;
            }
            {
                PrStringView pending = textView(&t->p->pending);
                t->p->mode = t->p->original;
                if (!t->p->pendingText) {
                    insertText(t, pending);
                    return true;
                }
                error(t);
                t->p->fosterParenting = true;
                reconstructFormatting(t);
                insertText(t, pending);
                t->p->framesetOk = false;
                t->p->fosterParenting = false;
                return true;
            }

        case HtmlInsertionMode_InCaption:
            if (kind(t) == HtmlTokenKind_EndTag && t->tag == PrDomTag_Caption) {
                closeCaption(t);
                return false;
            }
            if ((kind(t) == HtmlTokenKind_StartTag &&
                    (t->tag == PrDomTag_Caption || t->tag == PrDomTag_Col || t->tag == PrDomTag_Colgroup ||
                        t->tag == PrDomTag_Tbody || t->tag == PrDomTag_Td || t->tag == PrDomTag_Tfoot ||
                        t->tag == PrDomTag_Th || t->tag == PrDomTag_Thead || t->tag == PrDomTag_Tr)) ||
                    (kind(t) == HtmlTokenKind_EndTag && t->tag == PrDomTag_Table))
                return closeCaption(t);
            if (kind(t) == HtmlTokenKind_EndTag &&
                    (t->tag == PrDomTag_Body || t->tag == PrDomTag_Col || t->tag == PrDomTag_Colgroup ||
                        t->tag == PrDomTag_Html || t->tag == PrDomTag_Tbody || t->tag == PrDomTag_Td ||
                        t->tag == PrDomTag_Tfoot || t->tag == PrDomTag_Th || t->tag == PrDomTag_Thead ||
                        t->tag == PrDomTag_Tr)) {
                error(t);
                return false;
            }
            return inBody(t);

        case HtmlInsertionMode_InColumnGroup:
            switch (kind(t)) {
                case HtmlTokenKind_Character:
                    if (takeWhitespace(t, SpaceRule_Insert))
                        return false;
                    break;
                case HtmlTokenKind_Comment:
                    insertComment(t, nullptr);
                    return false;
                case HtmlTokenKind_ProcessingInstruction:
                    insertProcessingInstruction(t, nullptr);
                    return false;
                case HtmlTokenKind_Doctype:
                    error(t);
                    return false;
                case HtmlTokenKind_StartTag:
                    if (t->tag == PrDomTag_Html)
                        return inBody(t);
                    if (t->tag == PrDomTag_Col) {
                        insertHtmlElement(t);
                        pop(t);
                        return false;
                    }
                    if (t->tag == PrDomTag_Template) {
                        return processIn(t, HtmlInsertionMode_InHead);
                    }
                    break;
                case HtmlTokenKind_EndTag:
                    if (t->tag == PrDomTag_Colgroup) {
                        if (!domIsHtml(current(t), PrDomTag_Colgroup)) {
                            error(t);
                            return false;
                        }
                        pop(t);
                        t->p->mode = HtmlInsertionMode_InTable;
                        return false;
                    }
                    if (t->tag == PrDomTag_Col) {
                        error(t);
                        return false;
                    }
                    if (t->tag == PrDomTag_Template) {
                        return processIn(t, HtmlInsertionMode_InHead);
                    }
                    break;
                case HtmlTokenKind_Eof:
                    return inBody(t);
                default:
                    break;
            }
            if (!domIsHtml(current(t), PrDomTag_Colgroup)) {
                error(t);
                return false;
            }
            pop(t);
            t->p->mode = HtmlInsertionMode_InTable;
            return true;

        case HtmlInsertionMode_InTableBody:
            switch (kind(t)) {
                case HtmlTokenKind_StartTag:
                    if (t->tag == PrDomTag_Tr) {
                        clearToTableBodyContext(t);
                        insertHtmlElement(t);
                        t->p->mode = HtmlInsertionMode_InRow;
                        return false;
                    }
                    if (t->tag == PrDomTag_Th || t->tag == PrDomTag_Td) {
                        error(t);
                        clearToTableBodyContext(t);
                        insertBare(t, PrDomTag_Tr);
                        t->p->mode = HtmlInsertionMode_InRow;
                        return true;
                    }
                    if (t->tag == PrDomTag_Caption || t->tag == PrDomTag_Col || t->tag == PrDomTag_Colgroup ||
                            t->tag == PrDomTag_Tbody || t->tag == PrDomTag_Tfoot || t->tag == PrDomTag_Thead)
                        break;
                    return inTable(t);
                case HtmlTokenKind_EndTag:
                    if (t->tag == PrDomTag_Tbody || t->tag == PrDomTag_Tfoot || t->tag == PrDomTag_Thead) {
                        if (!inScope(t, t->tag, Scope_Table)) {
                            error(t);
                            return false;
                        }
                        closeTableBody(t);
                        return false;
                    }
                    if (t->tag == PrDomTag_Table)
                        break;
                    if (t->tag == PrDomTag_Body || t->tag == PrDomTag_Caption || t->tag == PrDomTag_Col ||
                            t->tag == PrDomTag_Colgroup || t->tag == PrDomTag_Html || t->tag == PrDomTag_Td ||
                            t->tag == PrDomTag_Th || t->tag == PrDomTag_Tr) {
                        error(t);
                        return false;
                    }
                    return inTable(t);
                default:
                    return inTable(t);
            }
            if (!inScope(t, PrDomTag_Tbody, Scope_Table) && !inScope(t, PrDomTag_Tfoot, Scope_Table) &&
                    !inScope(t, PrDomTag_Thead, Scope_Table)) {
                error(t);
                return false;
            }
            closeTableBody(t);
            return true;

        case HtmlInsertionMode_InRow:
            switch (kind(t)) {
                case HtmlTokenKind_StartTag:
                    if (t->tag == PrDomTag_Th || t->tag == PrDomTag_Td) {
                        clearToRowContext(t);
                        insertHtmlElement(t);
                        t->p->mode = HtmlInsertionMode_InCell;
                        pushMarker(t->p);
                        return false;
                    }
                    if (t->tag == PrDomTag_Caption || t->tag == PrDomTag_Col || t->tag == PrDomTag_Colgroup ||
                            t->tag == PrDomTag_Tbody || t->tag == PrDomTag_Tfoot || t->tag == PrDomTag_Thead ||
                            t->tag == PrDomTag_Tr)
                        break;
                    return inTable(t);
                case HtmlTokenKind_EndTag:
                    if (t->tag == PrDomTag_Tr) {
                        if (!inScope(t, PrDomTag_Tr, Scope_Table)) {
                            error(t);
                            return false;
                        }
                        closeRow(t);
                        return false;
                    }
                    if (t->tag == PrDomTag_Table)
                        break;
                    if (t->tag == PrDomTag_Tbody || t->tag == PrDomTag_Tfoot || t->tag == PrDomTag_Thead) {
                        if (!inScope(t, t->tag, Scope_Table)) {
                            error(t);
                            return false;
                        }
                        if (!inScope(t, PrDomTag_Tr, Scope_Table))
                            return false;
                        closeRow(t);
                        return true;
                    }
                    if (t->tag == PrDomTag_Body || t->tag == PrDomTag_Caption || t->tag == PrDomTag_Col ||
                            t->tag == PrDomTag_Colgroup || t->tag == PrDomTag_Html || t->tag == PrDomTag_Td ||
                            t->tag == PrDomTag_Th) {
                        error(t);
                        return false;
                    }
                    return inTable(t);
                default:
                    return inTable(t);
            }
            if (!inScope(t, PrDomTag_Tr, Scope_Table)) {
                error(t);
                return false;
            }
            closeRow(t);
            return true;

        case HtmlInsertionMode_InCell:
            if (kind(t) == HtmlTokenKind_EndTag && (t->tag == PrDomTag_Td || t->tag == PrDomTag_Th)) {
                closeCell(t, t->tag);
                return false;
            }
            if (kind(t) == HtmlTokenKind_StartTag &&
                    (t->tag == PrDomTag_Caption || t->tag == PrDomTag_Col || t->tag == PrDomTag_Colgroup ||
                        t->tag == PrDomTag_Tbody || t->tag == PrDomTag_Td || t->tag == PrDomTag_Tfoot ||
                        t->tag == PrDomTag_Th || t->tag == PrDomTag_Thead || t->tag == PrDomTag_Tr)) {
                if (!inScope(t, PrDomTag_Td, Scope_Table) && !inScope(t, PrDomTag_Th, Scope_Table)) {
                    error(t);
                    return false;
                }
                return closeCell(t, inScope(t, PrDomTag_Td, Scope_Table) ? PrDomTag_Td : PrDomTag_Th);
            }
            if (kind(t) == HtmlTokenKind_EndTag &&
                    (t->tag == PrDomTag_Body || t->tag == PrDomTag_Caption || t->tag == PrDomTag_Col ||
                        t->tag == PrDomTag_Colgroup || t->tag == PrDomTag_Html)) {
                error(t);
                return false;
            }
            if (kind(t) == HtmlTokenKind_EndTag &&
                    (t->tag == PrDomTag_Table || t->tag == PrDomTag_Tbody || t->tag == PrDomTag_Tfoot ||
                        t->tag == PrDomTag_Thead || t->tag == PrDomTag_Tr)) {
                if (!inScope(t, t->tag, Scope_Table)) {
                    error(t);
                    return false;
                }
                return closeCell(t, inScope(t, PrDomTag_Td, Scope_Table) ? PrDomTag_Td : PrDomTag_Th);
            }
            return inBody(t);

        case HtmlInsertionMode_InTemplate:
            switch (kind(t)) {
                case HtmlTokenKind_Character:
                case HtmlTokenKind_Comment:
                case HtmlTokenKind_ProcessingInstruction:
                case HtmlTokenKind_Doctype:
                    return inBody(t);
                case HtmlTokenKind_StartTag:
                    switch (t->tag) {
                        case PrDomTag_Base: case PrDomTag_Basefont: case PrDomTag_Bgsound: case PrDomTag_Link:
                        case PrDomTag_Meta: case PrDomTag_Noframes: case PrDomTag_Script: case PrDomTag_Style:
                        case PrDomTag_Template: case PrDomTag_Title:
                            return processIn(t, HtmlInsertionMode_InHead);
                        case PrDomTag_Caption: case PrDomTag_Colgroup: case PrDomTag_Tbody: case PrDomTag_Tfoot:
                        case PrDomTag_Thead:
                            return switchTemplateMode(t, HtmlInsertionMode_InTable);
                        case PrDomTag_Col:
                            return switchTemplateMode(t, HtmlInsertionMode_InColumnGroup);
                        case PrDomTag_Tr:
                            return switchTemplateMode(t, HtmlInsertionMode_InTableBody);
                        case PrDomTag_Td: case PrDomTag_Th:
                            return switchTemplateMode(t, HtmlInsertionMode_InRow);
                        default:
                            return switchTemplateMode(t, HtmlInsertionMode_InBody);
                    }
                case HtmlTokenKind_EndTag:
                    if (t->tag == PrDomTag_Template) {
                        return processIn(t, HtmlInsertionMode_InHead);
                    }
                    error(t);
                    return false;
                case HtmlTokenKind_Eof:
                    if (!hasOpen(t, PrDomTag_Template))
                        return false;
                    error(t);
                    closeTemplate(t);
                    return true;
                default:
                    return false;
            }

        case HtmlInsertionMode_AfterBody:
            switch (kind(t)) {
                case HtmlTokenKind_Character:
                    if (takeWhitespace(t, SpaceRule_Reconstruct))
                        return false;
                    break;
                case HtmlTokenKind_Comment:
                    insertComment(t, t->p->open.data[0]);
                    return false;
                case HtmlTokenKind_ProcessingInstruction:
                    insertProcessingInstruction(t, t->p->open.data[0]);
                    return false;
                case HtmlTokenKind_Doctype:
                    error(t);
                    return false;
                case HtmlTokenKind_StartTag:
                    if (t->tag == PrDomTag_Html)
                        return inBody(t);
                    break;
                case HtmlTokenKind_EndTag:
                    if (t->tag == PrDomTag_Html) {
                        if (t->p->context != nullptr) {
                            error(t);
                            return false;
                        }
                        t->p->mode = HtmlInsertionMode_AfterAfterBody;
                        return false;
                    }
                    break;
                case HtmlTokenKind_Eof:
                    return false;
                default:
                    break;
            }
            error(t);
            t->p->mode = HtmlInsertionMode_InBody;
            return true;

        case HtmlInsertionMode_InFrameset:
            switch (kind(t)) {
                case HtmlTokenKind_Character:
                    takeSpaceOnly(t, SpaceRule_Insert);
                    return false;
                case HtmlTokenKind_Comment:
                    insertComment(t, nullptr);
                    return false;
                case HtmlTokenKind_ProcessingInstruction:
                    insertProcessingInstruction(t, nullptr);
                    return false;
                case HtmlTokenKind_Doctype:
                    error(t);
                    return false;
                case HtmlTokenKind_StartTag:
                    if (t->tag == PrDomTag_Html)
                        return inBody(t);
                    if (t->tag == PrDomTag_Frameset) {
                        insertHtmlElement(t);
                        return false;
                    }
                    if (t->tag == PrDomTag_Frame) {
                        insertHtmlElement(t);
                        pop(t);
                        return false;
                    }
                    if (t->tag == PrDomTag_Noframes) {
                        return processIn(t, HtmlInsertionMode_InHead);
                    }
                    break;
                case HtmlTokenKind_EndTag:
                    if (t->tag == PrDomTag_Frameset) {
                        if (domIsHtml(current(t), PrDomTag_Html)) {
                            error(t);
                            return false;
                        }
                        pop(t);
                        if (t->p->context == nullptr && !domIsHtml(current(t), PrDomTag_Frameset))
                            t->p->mode = HtmlInsertionMode_AfterFrameset;
                        return false;
                    }
                    break;
                case HtmlTokenKind_Eof:
                    if (!domIsHtml(current(t), PrDomTag_Html))
                        error(t);
                    return false;
                default:
                    break;
            }
            error(t);
            return false;

        case HtmlInsertionMode_AfterFrameset:
            switch (kind(t)) {
                case HtmlTokenKind_Character:
                    takeSpaceOnly(t, SpaceRule_Insert);
                    return false;
                case HtmlTokenKind_Comment:
                    insertComment(t, nullptr);
                    return false;
                case HtmlTokenKind_ProcessingInstruction:
                    insertProcessingInstruction(t, nullptr);
                    return false;
                case HtmlTokenKind_Doctype:
                    error(t);
                    return false;
                case HtmlTokenKind_StartTag:
                    if (t->tag == PrDomTag_Html)
                        return inBody(t);
                    if (t->tag == PrDomTag_Noframes) {
                        return processIn(t, HtmlInsertionMode_InHead);
                    }
                    break;
                case HtmlTokenKind_EndTag:
                    if (t->tag == PrDomTag_Html) {
                        t->p->mode = HtmlInsertionMode_AfterAfterFrameset;
                        return false;
                    }
                    break;
                case HtmlTokenKind_Eof:
                    return false;
                default:
                    break;
            }
            error(t);
            return false;

        case HtmlInsertionMode_AfterAfterBody:
        case HtmlInsertionMode_AfterAfterFrameset:
            switch (kind(t)) {
                case HtmlTokenKind_Comment:
                    insertComment(t, t->p->document);
                    return false;
                case HtmlTokenKind_ProcessingInstruction:
                    insertProcessingInstruction(t, t->p->document);
                    return false;
                case HtmlTokenKind_Doctype:
                    return inBody(t);
                case HtmlTokenKind_Character:
                    if (mode == HtmlInsertionMode_AfterAfterFrameset) {
                        takeSpaceOnly(t, SpaceRule_Reconstruct);
                        return false;
                    }
                    if (takeWhitespace(t, SpaceRule_Reconstruct))
                        return false;
                    break;
                case HtmlTokenKind_StartTag:
                    if (t->tag == PrDomTag_Html)
                        return inBody(t);
                    if (mode == HtmlInsertionMode_AfterAfterFrameset && t->tag == PrDomTag_Noframes) {
                        return processIn(t, HtmlInsertionMode_InHead);
                    }
                    break;
                case HtmlTokenKind_Eof:
                    return false;
                default:
                    break;
            }
            if (mode == HtmlInsertionMode_AfterAfterFrameset) {
                error(t);
                return false;
            }
            error(t);
            t->p->mode = HtmlInsertionMode_InBody;
            return true;

        default:
            return false;
    }
}

bool process(Tree* t) {
    if (inForeignContent(t))
        return processForeign(t);
    return processMode(t);
}

// Reads tokens until the end of the input, passing each to the tree builder. An abandoned parse reads nothing.
void run(HtmlParser* p) {
    while (!p->abandoned) {
        PrDomElement* PR_NULLABLE adjusted = nullptr;
        if (p->context != nullptr && p->open.count == 1)
            adjusted = p->context;
        else if (p->open.count != 0)
            adjusted = p->open.last();
        p->tokenizer.foreign = adjusted != nullptr && adjusted->space != PrDomNamespace_Html;

        // If an open input ends before a complete token, the parser waits for more.
        if (!htmlTokenizerNext(&p->tokenizer))
            return;
        htmlParserProcess(p);
        if (p->tokenizer.token.kind == HtmlTokenKind_Eof || p->status != PrStatus_Ok || p->paused || p->abandoned)
            return;
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// The parse (html/#parsing)

void htmlParserInit(HtmlParser* parser, PrDomDocument* document, HtmlParseFlags flags) {
    parser->document = document;
    parser->flags = flags;
    parser->mode = HtmlInsertionMode_Initial;
    parser->original = HtmlInsertionMode_Initial;
    parser->framesetOk = true;
}

void htmlParserReset(HtmlParser* parser) {
    htmlTokenizerReset(&parser->tokenizer);
    parser->errors.reset();
    while (parser->open.count != 0)
        domNodeRelease(parser->open.data[--parser->open.count]);
    parser->open.reset();
    while (parser->formatting.count != 0)
        eraseFormatting(parser, parser->formatting.count - 1);
    parser->formatting.reset();
    setPointer(&parser->head, nullptr);
    setPointer(&parser->form, nullptr);
    setPointer(&parser->context, nullptr);
    parser->templateModes.reset();
    textReset(&parser->pending);

    PrDomDocument* document = parser->document;
    HtmlParseFlags flags = parser->flags;
    HtmlParserHost host = parser->host;
    *parser = HtmlParser{};
    parser->document = document;
    parser->flags = flags;
    parser->host = host;
    parser->framesetOk = true;
}

void htmlParse(HtmlParser* parser, PrStringView text) {
    htmlTokenizerStart(&parser->tokenizer, text, &parser->errors);
    run(parser);
}

PrDomNode* htmlParseFragment(HtmlParser* parser, PrDomElement* context, PrStringView text) {
    PrDomNode* fragment = domDocumentFragmentCreate(parser->document);
    domNodeRetain(fragment);

    htmlTokenizerStart(&parser->tokenizer, text, &parser->errors);
    PrDomElement* html = htmlParserBeginFragment(parser, context);
    domAttach(fragment, html, nullptr);
    run(parser);

    while (html->first != nullptr)
        domAttach(fragment, html->first, nullptr);
    domDetach(html);
    domDropTree(html);
    return fragment;
}

PrDomElement* htmlParserBeginFragment(HtmlParser* parser, PrDomElement* context) {
    setPointer(&parser->context, context);

    if (context->space == PrDomNamespace_Html) {
        switch (context->tag) {
            case PrDomTag_Title: case PrDomTag_Textarea:
                htmlTokenizerSetState(&parser->tokenizer, HtmlState_Rcdata, PrStringView{});
                break;
            case PrDomTag_Style: case PrDomTag_Xmp: case PrDomTag_Iframe: case PrDomTag_Noembed:
            case PrDomTag_Noframes:
                htmlTokenizerSetState(&parser->tokenizer, HtmlState_Rawtext, PrStringView{});
                break;
            case PrDomTag_Script:
                htmlTokenizerSetState(&parser->tokenizer, HtmlState_ScriptData, PrStringView{});
                break;
            case PrDomTag_Noscript:
                if ((parser->flags & HtmlParseFlag_Scripting) != 0)
                    htmlTokenizerSetState(&parser->tokenizer, HtmlState_Rawtext, PrStringView{});
                break;
            case PrDomTag_Plaintext:
                htmlTokenizerSetState(&parser->tokenizer, HtmlState_Plaintext, PrStringView{});
                break;
            default:
                break;
        }
    }

    PrDomElement* html = domElementCreate(parser->document, PrDomNamespace_Html, PrDomTag_Html, PrStringView{});
    pushHeld(&parser->open, html);

    if (domIsHtml(context, PrDomTag_Template))
        parser->templateModes.push(HtmlInsertionMode_InTemplate);

    for (PrDomNode* node = context; node != nullptr; node = node->parent) {
        PrDomElement* element = domElementFromNode(node);
        if (domIsHtml(element, PrDomTag_Form)) {
            setPointer(&parser->form, element);
            break;
        }
    }

    {
        Tree tree(parser);
        resetInsertionMode(&tree);
    }
    return html;
}

void htmlParserProcess(HtmlParser* parser) {
    HtmlParser* p = parser;
    Tree tree(p);
    tree.token = p->tokenizer.token;
    if (tree.token.kind == HtmlTokenKind_StartTag || tree.token.kind == HtmlTokenKind_EndTag) {
        tree.name = htmlSpanText(&p->tokenizer, tree.token.name);
        tree.tag = domTagFromText(tree.name);
    }

    if (p->skipNewline) {
        p->skipNewline = false;
        PrStringView opening;
        if (tree.token.kind == HtmlTokenKind_Character)
            opening = htmlSpanText(&p->tokenizer, tree.token.data);
        else
            opening = PrStringView{};
        if (opening.size != 0 && opening.data[0] == '\n') {
            htmlTokenAdvance(&p->tokenizer, &tree.token, 1);
            if (htmlSpanEmpty(tree.token.data))
                return;
        }
    }

    while (process(&tree)) {
    }
    // html/#stop-parsing pops every node off the stack of open elements.
    if (tree.token.kind == HtmlTokenKind_Eof) {
        truncateOpen(p, 0);
        p->stopped = true;
    }
}

HtmlParserRest htmlParserRest(const HtmlParser* parser) {
    return HtmlParserRest{
        .depth = parser->open.count,
        .templates = parser->templateModes.count,
        .mode = parser->mode,
        .form = parser->form,
        .framesetOk = parser->framesetOk,
    };
}

bool htmlParserIsAtRest(const HtmlParser* parser, const HtmlParserRest& rest) {
    return parser->open.count == rest.depth && parser->templateModes.count == rest.templates &&
        parser->mode == rest.mode && parser->form == rest.form && parser->formatting.count == 0 &&
        parser->pending.text.count == 0 && !parser->skipNewline && !parser->fosterParenting && !parser->paused &&
        !parser->stopped && htmlTokenizerIsIdle(&parser->tokenizer);
}

void htmlParserRestore(HtmlParser* parser, const HtmlParserRest& rest) {
    PR_ASSERT(parser->open.count >= rest.depth && parser->templateModes.count >= rest.templates);
    truncateOpen(parser, rest.depth);
    while (parser->formatting.count != 0)
        eraseFormatting(parser, parser->formatting.count - 1);
    parser->templateModes.resize(rest.templates);
    setPointer(&parser->form, rest.form);
    textClear(&parser->pending);
    parser->pendingText = false;
    parser->mode = rest.mode;
    parser->original = rest.mode;
    parser->framesetOk = rest.framesetOk;
    parser->fosterParenting = false;
    parser->skipNewline = false;
    parser->errors.clear();
    htmlTokenizerStart(&parser->tokenizer, PrStringView{}, &parser->errors);
    parser->tokenizer.open = true;
}

void htmlParserWrite(HtmlParser* parser, PrStringView text) {
    uint32_t at = parser->tokenizer.input.pos;
    uint32_t before = parser->tokenizer.input.text.count;
    htmlTokenizerInsert(&parser->tokenizer, text);
    uint32_t size = parser->tokenizer.input.text.count - before;
    // A write within the text that the last write inserted extends it.
    if (parser->writtenEnd > parser->writtenStart && at >= parser->writtenStart && at <= parser->writtenEnd) {
        parser->writtenEnd += size;
    } else {
        parser->writtenStart = at;
        parser->writtenEnd = at + size;
    }
}

void htmlParserOpen(HtmlParser* parser) {
    htmlTokenizerStart(&parser->tokenizer, PrStringView{}, &parser->errors);
    parser->tokenizer.open = true;
}

void htmlParserAppend(HtmlParser* parser, PrStringView text) {
    htmlTokenizerAppend(&parser->tokenizer, text);
    if (!parser->paused)
        run(parser);
}

void htmlParserClose(HtmlParser* parser) {
    parser->tokenizer.open = false;
    if (!parser->paused)
        run(parser);
}

void htmlParserResume(HtmlParser* parser) {
    parser->paused = false;
    if (!parser->stopped)
        run(parser);
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
