#include "pr/memory.h"
#include "pr/string_ops.h"

#include "dom/src/dom_node.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

size_t treeCount(PrDomDocument* document, PrDomNode* node) {
    // The contents of a template are in the inert document of the template's document.
    CHECK((node->document == document || node->document == document->templateDocument));
    size_t count = 1;
    PrDomNode* previous = nullptr;
    for (PrDomNode* child = node->first; child != nullptr; child = child->next) {
        CHECK(child->parent == node);
        CHECK(child->previous == previous);
        count += treeCount(document, child);
        previous = child;
    }
    CHECK(node->last == previous);
    if (PrDomElement* element = domElementFromNode(node)) {
        for (PrDomAttr* attribute = element->firstAttribute; attribute != nullptr;
                attribute = static_cast<PrDomAttr*>(attribute->next)) {
            CHECK(attribute->document == document);
            CHECK(attribute->parent == node);
            ++count;
        }
        if (domElementContents(element) != nullptr && domNodeOwner(domElementContents(element)) == element)
            count += treeCount(document, domElementContents(element));
    }
    return count;
}

size_t ownedCount(PrDomDocument* document) {
    size_t count = treeCount(document, document) - 1;
    PrDomNode* previous = nullptr;
    for (PrDomNode* node = document->detached; node != nullptr; node = node->next) {
        CHECK(node->parent == nullptr);
        CHECK(node->previous == previous);
        count += treeCount(document, node);
        previous = node;
    }
    return count;
}

// Returns the text of a view. The null view reads as the empty string.
std::string textOf(PrStringView text) {
    return std::string(text.size != 0 ? text.data : "", text.size);
}

std::string taken(PrString text) {
    PrStringView view = prStringView(&text);
    std::string result(view.data != nullptr ? view.data : "", view.size);
    prStringReset(&text);
    return result;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a removed node with no references is freed with its tree") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* html = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Html, "");
    domAttach(document, html, nullptr);
    // The inert document of the template below lasts as long as the document.
    domDocumentTemplateDocument(document);
    size_t before = prMemoryLiveCount();
    PrDomElement* div = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    domSetAttribute(div, PrDomNamespace_None, "id", "an id longer than inline string storage");
    domAttach(div, domTextCreate(document, "text"), nullptr);
    domAttach(div, domElementCreate(document, PrDomNamespace_Html, PrDomTag_Template, ""), nullptr);
    domInsert(html, div, nullptr);
    domRemove(div);
    CHECK(prMemoryLiveCount() == before);
    CHECK(document->detached == nullptr);
    domNodeRelease(document);
}

TEST_CASE("a reference to any node keeps its tree, and the tree its document") {
    size_t before = prMemoryLiveCount();
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* html = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Html, "");
    domAttach(document, html, nullptr);
    PrDomElement* div = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    PrDomText* text = domTextCreate(document, "text");
    domAttach(div, text, nullptr);
    domInsert(html, div, nullptr);
    domNodeRetain(text);
    domRemove(div);
    CHECK(div->retained == 1);
    CHECK(text->parent == div);
    domNodeRelease(document);
    CHECK(document->retained == 1);
    domNodeRelease(text);
    CHECK(prMemoryLiveCount() == before);
}

TEST_CASE("a detached tree left with no references by a removal inside it is freed") {
    size_t before = prMemoryLiveCount();
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* outer = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    PrDomElement* inner = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Span, "");
    domAttach(outer, inner, nullptr);
    domNodeRetain(inner);
    CHECK(outer->retained == 1);
    domRemove(inner);
    CHECK(inner->retained == 1);
    CHECK(document->detached == inner);
    domNodeRelease(inner);
    CHECK(document->detached == nullptr);
    domNodeRelease(document);
    CHECK(prMemoryLiveCount() == before);
}

TEST_CASE("a node moved within its tree keeps the references where they are") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* html = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Html, "");
    domAttach(document, html, nullptr);
    PrDomElement* a = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    PrDomElement* b = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    domAttach(html, a, nullptr);
    domAttach(html, b, nullptr);
    domNodeRetain(a);
    uint32_t retained = document->retained;
    domInsert(b, a, nullptr);
    CHECK(a->parent == b);
    CHECK(domIsConnected(a));
    CHECK(document->retained == retained);
    domNodeRelease(a);
    domNodeRelease(document);
}

TEST_CASE("the connected flag follows insertion and removal, into shadow trees") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* html = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Html, "");
    PrDomElement* host = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    PrDomShadowRootInit init = prDomShadowRootInitDefault(PrDomShadowRootMode_Open);
    PrDomShadowRoot* shadow = nullptr;
    REQUIRE(domAttachShadow(host, &init, &shadow) == PrStatus_Ok);
    PrDomText* text = domTextCreate(document, "x");
    domAttach(shadow, text, nullptr);
    domAttach(html, host, nullptr);
    CHECK(!domIsConnected(text));
    domAttach(document, html, nullptr);
    CHECK(domIsConnected(host));
    CHECK(domIsConnected(shadow));
    CHECK(domIsConnected(text));
    CHECK(domTreeRoot(text) == document);
    domNodeRetain(host);
    domRemove(html);
    CHECK(!domIsConnected(text));
    CHECK(domTreeRoot(text) == html);
    domNodeRelease(host);
    domNodeRelease(document);
}

TEST_CASE("a node that leaves the contents of a template for its host's tree is adopted") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* html = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Html, "");
    domAttach(document, html, nullptr);
    PrDomElement* element = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Template, "");
    domAttach(html, element, nullptr);
    PrDomNode* contents = domElementContents(element);
    CHECK(contents->document == domDocumentTemplateDocument(document));
    PrDomText* text = domTextCreate(contents->document, "moved");
    domAttach(contents, text, nullptr);
    domInsert(html, contents, nullptr);
    CHECK(text->parent == html);
    CHECK(text->document == document);
    domNodeRelease(document);
}

TEST_CASE("an adopted subtree keeps its addresses after its old document is gone") {
    PrDomDocument* source = domDocumentCreate(DomDocumentKind_Html);
    PrDomDocument* target = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* element = domElementCreate(source, PrDomNamespace_Html, PrDomTag_Template, "");
    PrDomNode* contents = domElementContents(element);
    REQUIRE(contents != nullptr);
    PrDomText* text = domTextCreate(contents->document, "a text payload longer than inline string storage");
    domAttach(contents, text, nullptr);
    domSetAttribute(element, PrDomNamespace_None, "name", "a value longer than inline string storage");
    const PrDomAttr* attribute = domAttribute(element, PrDomNamespace_None, "name");
    REQUIRE(attribute != nullptr);
    PrDomComment* comment = domCommentCreate(source, "child");
    domAttach(element, comment, nullptr);
    domTextCreate(source, "detached node left in the source document");

    domAdopt(target, element);
    CHECK(ownedCount(source) == 1);
    CHECK(ownedCount(target) == 5);
    PrDomDocument* inert = domDocumentTemplateDocument(target);
    CHECK(inert != target);
    CHECK(contents->document == inert);
    domNodeRelease(source);

    CHECK(element->document == target);
    CHECK(domElementContents(element) == contents);
    CHECK(contents->document == inert);
    CHECK(contents->first == text);
    CHECK(text->document == inert);
    CHECK(prStringView(&text->data) == "a text payload longer than inline string storage");
    CHECK(domAttribute(element, PrDomNamespace_None, "name") == attribute);
    CHECK(attribute->document == target);
    CHECK(attribute->parent == element);
    CHECK(prStringView(&attribute->value) == "a value longer than inline string storage");
    CHECK(element->first == comment);
    CHECK(comment->parent == element);
    domNodeRelease(target);
}

TEST_CASE("a node inserted into another document moves to it and closes the gap it left") {
    PrDomDocument* source = domDocumentCreate(DomDocumentKind_Html);
    PrDomDocument* target = domDocumentCreate(DomDocumentKind_Html);
    PrDomNode* left = domDocumentFragmentCreate(source);
    PrDomNode* moved = domDocumentFragmentCreate(source);
    PrDomNode* right = domDocumentFragmentCreate(source);
    domAttach(source, left, nullptr);
    domAttach(source, moved, nullptr);
    domAttach(source, right, nullptr);
    PrDomNode* before = domDocumentFragmentCreate(target);
    domAttach(target, before, nullptr);

    domAttach(target, moved, before);
    CHECK(source->first == left);
    CHECK(source->last == right);
    CHECK(left->next == right);
    CHECK(right->previous == left);
    CHECK(target->first == moved);
    CHECK(moved->next == before);
    CHECK(before->previous == moved);
    CHECK(moved->document == target);
    CHECK(ownedCount(source) == 2);
    CHECK(ownedCount(target) == 2);
    domNodeRelease(target);
    CHECK(left->next == right);
    domNodeRelease(source);
}

TEST_CASE("a node adopted back and forth is the same node") {
    PrDomDocument* source = domDocumentCreate(DomDocumentKind_Html);
    PrDomDocument* target = domDocumentCreate(DomDocumentKind_Html);
    PrDomNode* node = domDocumentFragmentCreate(source);
    for (int i = 0; i < 3; ++i) {
        domAdopt(target, node);
        domAdopt(target, node);
        CHECK(ownedCount(source) == 0);
        CHECK(ownedCount(target) == 1);
        CHECK(target->detached == node);
        domAdopt(source, node);
        CHECK(ownedCount(source) == 1);
        CHECK(ownedCount(target) == 0);
        CHECK(source->detached == node);
    }
    domNodeRelease(target);
    domDropTree(node);
    CHECK(ownedCount(source) == 0);
    domNodeRelease(source);
}

TEST_CASE("a reference to an attribute keeps its element's tree") {
    size_t before = prMemoryLiveCount();
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* element = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Template, "");
    domSetAttribute(element, PrDomNamespace_None, "first", "one");
    PrDomAttr* first = element->firstAttribute;
    domNodeRetain(first);
    CHECK(element->retained == 1);
    domNodeRelease(document);
    CHECK(first->parent == element);
    CHECK(prStringView(&first->value) == "one");
    domNodeRelease(first);
    CHECK(prMemoryLiveCount() == before);
}

TEST_CASE("an attribute taken off its element is freed where nothing references it") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* element = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    domSetAttribute(element, PrDomNamespace_None, "kept", "1");
    domSetAttribute(element, PrDomNamespace_None, "dropped", "2");
    PrDomAttr* kept = element->firstAttribute;
    domNodeRetain(kept);
    size_t before = prMemoryLiveCount();
    domRemoveAttribute(element, PrDomNamespace_None, "dropped");
    CHECK(prMemoryLiveCount() < before);
    domRemoveAttributeNode(element, kept);
    CHECK(kept->parent == nullptr);
    CHECK(kept->retained == 1);
    CHECK(element->retained == 0);
    domNodeRelease(kept);
    domDropTree(element);
    domNodeRelease(document);
}

TEST_CASE("a document takes its deep trees with it, attached or not") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomNode* attached = document;
    PrDomNode* detached = domDocumentFragmentCreate(document);
    for (int i = 0; i < 10000; ++i) {
        PrDomNode* child = domDocumentFragmentCreate(document);
        domAttach(attached, child, nullptr);
        attached = child;
        child = domDocumentFragmentCreate(document);
        domAttach(detached, child, nullptr);
        detached = child;
    }
    PrDomDocumentType* doctype = domDocumentTypeCreate(document, "html", "a public identifier longer than inline storage",
        "system");
    domAttach(attached, doctype, nullptr);
    PrDomProcessingInstruction* instruction = domProcessingInstructionCreate(
        document, "target", "processing instruction data longer than inline storage");
    domAttach(detached, instruction, nullptr);
    CHECK(attached->first->type == PrDomNodeType_DocumentType);
    CHECK(detached->first->type == PrDomNodeType_ProcessingInstruction);
    domNodeRelease(document);
}

TEST_CASE("a DOM destroy takes null and does nothing") {
    domNodeRelease(nullptr);
    domDropTree(nullptr);
    prDomDocumentRelease(nullptr);
    prDomEventRelease(nullptr);
    prDomEventTargetRelease(nullptr);
}

TEST_CASE("an attribute or a detached node has no siblings") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* element = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    domSetAttribute(element, PrDomNamespace_None, "first", "1");
    domSetAttribute(element, PrDomNamespace_None, "second", "2");
    REQUIRE(element->firstAttribute->next != nullptr);
    CHECK(prDomNodeNextSibling(element->firstAttribute) == nullptr);
    CHECK(prDomNodeFirstChild(element->firstAttribute) == nullptr);

    PrDomNode* detached = domDocumentFragmentCreate(document);
    REQUIRE(detached->next != nullptr);
    CHECK(prDomNodeNextSibling(detached) == nullptr);
    PrDomNode* first = domTextCreate(document, "first");
    PrDomNode* second = domTextCreate(document, "second");
    domAttach(element, first, nullptr);
    domAttach(element, second, nullptr);
    CHECK(prDomNodeNextSibling(first) == second);
    CHECK(prDomNodeType(first) == PrDomNodeType_Text);
    domNodeRelease(document);
}

TEST_CASE("an HTML element's attribute names fold case and a foreign element's do not") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* html = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    domSetAttribute(html, PrDomNamespace_None, "data", "value");
    const PrDomAttr* lower = domAttribute(html, PrDomNamespace_None, "data");
    REQUIRE(lower != nullptr);
    CHECK(domAttribute(html, PrDomNamespace_None, "DATA") == lower);
    PrStringView value = prDomElementGetAttribute(html, "DaTa");
    CHECK(value == "value");
    domSetAttribute(html, PrDomNamespace_Xml, "Mixed", "xml");
    CHECK(domAttribute(html, PrDomNamespace_Xml, "Mixed") != nullptr);
    CHECK(domAttribute(html, PrDomNamespace_Xml, "mixed") == nullptr);

    PrDomElement* svg = domElementCreate(document, PrDomNamespace_Svg, PrDomTag_Unknown, "svg");
    domSetAttribute(svg, PrDomNamespace_None, "viewBox", "0 0 1 1");
    CHECK(domAttribute(svg, PrDomNamespace_None, "viewBox") != nullptr);
    CHECK(domAttribute(svg, PrDomNamespace_None, "viewbox") == nullptr);
    domNodeRelease(document);
}

TEST_CASE("head and body are children of the html element, and nowhere else") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    CHECK(domDocumentHead(document) == nullptr);

    PrDomElement* html = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Html, "");
    PrDomElement* head = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Head, "");
    PrDomElement* frameset = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Frameset, "");
    PrDomElement* body = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Body, "");
    PrDomElement* nested = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Head, "");
    domAttach(document, html, nullptr);
    domAttach(body, nested, nullptr);
    domAttach(html, frameset, nullptr);
    domAttach(html, head, nullptr);
    domAttach(html, body, nullptr);

    CHECK(domDocumentElement(document) == html);
    CHECK(domDocumentHead(document) == head);
    CHECK(domDocumentBody(document) == frameset);   // the first body or frameset
    CHECK(prDomDocumentHead(document) == head);
    domNodeRelease(document);
}

TEST_CASE("an element found by ID is the first in tree order, and the empty ID finds none") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* outer = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    PrDomElement* inner = domElementCreate(document, PrDomNamespace_Html, PrDomTag_P, "");
    PrDomElement* later = domElementCreate(document, PrDomNamespace_Html, PrDomTag_P, "");
    domAttach(document, outer, nullptr);
    domAttach(outer, inner, nullptr);
    domAttach(outer, later, nullptr);
    domSetAttribute(inner, PrDomNamespace_None, "id", "x");
    prDomElementSetId(later, "x");

    CHECK(domElementById(document, "x") == inner);
    CHECK(domElementById(document, "y") == nullptr);
    CHECK(domElementById(document, "") == nullptr);

    PrStringView id = prDomElementId(later);
    CHECK(id == "x");
    id = prDomElementId(outer);
    CHECK(id.size == 0);
    CHECK(id.data != nullptr);
    domNodeRelease(document);
}

TEST_CASE("an element's classes are a list of its document's, shared by every element of the same text") {
    size_t before = prMemoryLiveCount();
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* html = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Html, "");
    domAttach(document, html, nullptr);
    PrDomElement* a = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    PrDomElement* b = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    domAttach(html, a, nullptr);
    domNodeRetain(b);
    auto classes = [](const PrDomElement* element) {
        std::string out;
        if (element->classes == kDomClassesNone)
            return out;
        DomClassList list = domClassesList(element->document, element->classes);
        for (uint32_t i = 0; i < list.count; ++i) {
            PrStringView text = domClassesName(element->document, list.atoms[i]);
            out += (i != 0 ? " " : "") + std::string(text.data, text.size);
        }
        return out;
    };

    domSetAttribute(a, PrDomNamespace_None, "class", " x\ty x  z ");
    domSetAttribute(b, PrDomNamespace_None, "class", "y");
    CHECK(classes(a) == "x y z");
    CHECK(domClassesFind(document, "Y") == kAtomNone);
    CHECK(domClassesHas(document, a->classes, domClassesFind(document, "y")));

    // Elements with the same text share its list, and one with no word has no list.
    PrDomElement* c = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    domSetAttribute(c, PrDomNamespace_None, "class", "y");
    CHECK(c->classes == b->classes);
    domSetAttribute(c, PrDomNamespace_None, "class", "  ");
    CHECK(c->classes == kDomClassesNone);

    // Adoption takes an element's classes to the table of the document that adopts it.
    PrDomDocument* other = domDocumentCreate(DomDocumentKind_Html);
    domNodeRetain(a);
    domRemove(a);
    domAdopt(other, a);
    CHECK(classes(a) == "x y z");
    CHECK(domClassesFind(other, "x") != kAtomNone);

    domNodeRelease(a);
    domNodeRelease(b);
    domNodeRelease(document);
    domNodeRelease(other);
    CHECK(prMemoryLiveCount() == before);
}

TEST_CASE("a local name no tag names is an atom of the document's, which elements of it share") {
    size_t before = prMemoryLiveCount();
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* a = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Unknown, "my-el");
    PrDomElement* b = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Unknown, "my-el");
    PrDomElement* c = domElementCreate(document, PrDomNamespace_Svg, PrDomTag_Unknown, "my-el");
    PrDomElement* div = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "div");
    domNodeRetain(a);
    CHECK(domTagIsAtom(a->tag));
    CHECK(a->tag == b->tag);
    CHECK(a->tag == c->tag);
    CHECK(!domTagIsAtom(div->tag));
    CHECK(domElementTag(a) == PrDomTag_Unknown);
    CHECK(domElementName(a) == "my-el");

    // Adoption and cloning move the name to the atoms of the document that the element is then in.
    PrDomDocument* other = domDocumentCreate(DomDocumentKind_Html);
    domAdopt(other, a);
    CHECK(domElementName(a) == "my-el");
    CHECK(domElementName(a).data != domElementName(b).data);
    PrDomElement* copy = static_cast<PrDomElement*>(domClone(a, other, false));
    CHECK(copy->tag == a->tag);

    domNodeRelease(a);
    domNodeRelease(document);
    domNodeRelease(other);
    CHECK(prMemoryLiveCount() == before);
}

TEST_CASE("references counted together are counted as each would be alone") {
    size_t before = prMemoryLiveCount();
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* html = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Html, "");
    domAttach(document, html, nullptr);
    std::vector<PrDomNode*> all = {document, html};
    std::vector<PrDomNode*> groups;
    for (int i = 0; i < 3; ++i) {
        PrDomElement* group = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
        domAttach(html, group, nullptr);
        all.push_back(group);
        for (int j = 0; j < 3; ++j) {
            PrDomElement* leaf = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Span, "");
            domAttach(group, leaf, nullptr);
            all.push_back(leaf);
        }
    }
    PrDomElement* detached = domElementCreate(document, PrDomNamespace_Html, PrDomTag_P, "");
    PrDomElement* inside = domElementCreate(document, PrDomNamespace_Html, PrDomTag_B, "");
    domAttach(detached, inside, nullptr);
    domNodeRetain(detached);
    all.push_back(detached);
    all.push_back(inside);

    // In tree order, out of it, across trees, and with a node twice.
    std::vector<PrDomNode*> nodes = {all[3], all[4], all[5], all[2], all[9], inside, all[7], all[1], inside, all[4]};
    auto counts = [&] {
        std::vector<std::pair<uint32_t, uint32_t>> out;
        for (PrDomNode* node : all)
            out.push_back({node->references, node->retained});
        return out;
    };
    domNodeRetainEach(nodes.data(), uint32_t(nodes.size()));
    std::vector<std::pair<uint32_t, uint32_t>> together = counts();
    for (PrDomNode* node : all)
        CHECK((node->flags & DomNodeFlag_Counting) == 0);
    domNodeReleaseEach(nodes.data(), uint32_t(nodes.size()));
    for (PrDomNode* node : nodes)
        domNodeRetain(node);
    CHECK(counts() == together);
    for (PrDomNode* node : nodes)
        domNodeRelease(node);

    // A tree left with no references is freed when the last one is released.
    PrDomNode* last = inside;
    domNodeRetainEach(&last, 1);
    domNodeRelease(detached);
    domNodeReleaseEach(&last, 1);
    CHECK(document->detached == nullptr);
    domNodeRelease(document);
    CHECK(prMemoryLiveCount() == before);
}

TEST_CASE("a tree keeps its elements by ID as they connect, move, change and leave") {
    size_t before = prMemoryLiveCount();
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* html = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Html, "");
    domAttach(document, html, nullptr);
    PrDomElement* a = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    PrDomElement* b = domElementCreate(document, PrDomNamespace_Html, PrDomTag_P, "");
    domSetAttribute(a, PrDomNamespace_None, "id", "x");
    domSetAttribute(b, PrDomNamespace_None, "id", "x");
    domAttach(a, b, nullptr);
    CHECK(domElementById(document, "x") == nullptr);

    // Connecting a subtree adds its IDs, and the first of two is the first in tree order.
    domInsert(html, a, nullptr);
    PrDomElement* first = nullptr;
    CHECK(domElementsById(document, "x", &first) == 2);
    CHECK(first == a);
    CHECK(domElementById(a, "x") == b);

    // Moving the later one before the other makes it the first.
    domInsert(html, b, a);
    CHECK(domElementById(document, "x") == b);

    domSetAttribute(a, PrDomNamespace_None, "id", "y");
    CHECK(domElementsById(document, "x", &first) == 1);
    CHECK(first == b);
    CHECK(domElementById(document, "y") == a);
    domRemoveAttribute(a, PrDomNamespace_None, "id");
    CHECK(domElementById(document, "y") == nullptr);

    // A shadow tree keeps its own IDs. A connected move between it and the document's tree moves them between the two.
    PrDomShadowRootInit init = prDomShadowRootInitDefault(PrDomShadowRootMode_Open);
    PrDomShadowRoot* shadow = nullptr;
    REQUIRE(domAttachShadow(a, &init, &shadow) == PrStatus_Ok);
    CHECK(domKeepsIds(shadow));
    domInsert(shadow, b, nullptr);
    CHECK(domElementById(document, "x") == nullptr);
    CHECK(domElementById(shadow, "x") == b);
    domInsert(html, b, nullptr);
    CHECK(domElementById(shadow, "x") == nullptr);
    CHECK(domElementById(document, "x") == b);

    // A shadow tree of a host that is not connected keeps no IDs, and is searched instead.
    domInsert(shadow, b, nullptr);
    domNodeRetain(a);
    domRemove(a);
    CHECK(!domKeepsIds(shadow));
    CHECK(domElementById(shadow, "x") == b);
    CHECK(document->ids == nullptr);
    domNodeRelease(a);
    domNodeRelease(document);
    CHECK(prMemoryLiveCount() == before);
}

TEST_CASE("an HTML element's tag name is uppercase, and a foreign one's is as written") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* div = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    PrDomElement* custom = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Unknown, "my-el");
    PrDomElement* path = domElementCreate(document, PrDomNamespace_Svg, PrDomTag_Unknown, "textPath");

    PrString name = domElementTagName(div);
    CHECK(prStringView(&name) == "DIV");
    prStringReset(&name);
    name = domElementTagName(custom);
    CHECK(prStringView(&name) == "MY-EL");
    prStringReset(&name);
    name = domElementTagName(path);
    CHECK(prStringView(&name) == "textPath");
    prStringReset(&name);
    domNodeRelease(document);
}

TEST_CASE("character data is measured in UTF-16 code units") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    // a, é, ☃ and 😀
    PrDomText* text = domTextCreate(document, "a\xc3\xa9\xe2\x98\x83\xf0\x9f\x98\x80");
    CHECK(domCharacterDataLength(text) == 5);

    PrDomCharacterData* handle = text;
    prDomCharacterDataSetData(handle, PrStringView{});
    CHECK(domCharacterDataLength(text) == 0);
    PrStringView data = prDomCharacterDataData(handle);
    CHECK(data.size == 0);
    CHECK(data.data != nullptr);
    domNodeRelease(document);
}

TEST_CASE("a query answers the node a target is, and null for any other kind") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* element = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    PrDomText* text = domTextCreate(document, "x");
    PrDomEventTarget* bare = prDomEventTargetCreate();

    PrDomEventTarget* elementTarget = element;
    PrDomEventTarget* textTarget = text;
    PrDomEventTarget* documentTarget = document;

    CHECK(prDomElementQuery(elementTarget) == element);
    CHECK(prDomNodeQuery(elementTarget) != nullptr);
    CHECK(prDomTextQuery(elementTarget) == nullptr);
    CHECK(prDomTextQuery(textTarget) != nullptr);
    CHECK(prDomCharacterDataQuery(textTarget) != nullptr);
    CHECK(prDomCommentQuery(textTarget) == nullptr);
    CHECK(prDomDocumentQuery(documentTarget) == document);

    CHECK(prDomNodeQuery(bare) == nullptr);
    CHECK(prDomElementQuery(bare) == nullptr);
    CHECK(prDomNodeQuery(nullptr) == nullptr);

    prDomEventTargetRelease(bare);
    domNodeRelease(document);
}

TEST_CASE("pre-insertion refuses what would break the tree and finds the reference child") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* html = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Html, "html");
    PrDomElement* body = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Body, "body");
    CHECK(domPreInsert(document, html, nullptr) == PrStatus_Ok);
    CHECK(domPreInsert(html, body, nullptr) == PrStatus_Ok);

    // An ancestor, a second document element, and text or a doctype in the wrong place.
    CHECK(domPreInsert(body, html, nullptr) == PrStatus_NotAllowed);
    CHECK(domPreInsert(body, body, nullptr) == PrStatus_NotAllowed);
    PrDomElement* other = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "div");
    CHECK(domPreInsert(document, other, nullptr) == PrStatus_NotAllowed);
    CHECK(domPreInsert(document, domTextCreate(document, "x"), nullptr) == PrStatus_NotAllowed);
    PrDomDocumentType* doctype = domDocumentTypeCreate(document, "html", "", "");
    CHECK(domPreInsert(body, doctype, nullptr) == PrStatus_NotAllowed);
    CHECK(domPreInsert(document, doctype, nullptr) == PrStatus_NotAllowed);
    CHECK(domPreInsert(document, doctype, html) == PrStatus_Ok);

    // A reference child that is not a child of the parent.
    CHECK(domPreInsert(html, other, body->first) == PrStatus_Ok);
    CHECK(domPreInsert(body, domTextCreate(document, "x"), other) == PrStatus_NotFound);
    CHECK(domPreRemove(body, other) == PrStatus_NotFound);
    domNodeRetain(other);
    CHECK(domPreRemove(html, other) == PrStatus_Ok);
    CHECK(other->parent == nullptr);
    CHECK(ownedCount(document) == 6);
    domNodeRelease(other);
    CHECK(ownedCount(document) == 5);
    domNodeRelease(document);
}

TEST_CASE("a fragment inserts its children in order and is left empty") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* parent = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "div");
    PrDomNode* last = domTextCreate(document, "c");
    domAttach(parent, last, nullptr);
    PrDomNode* fragment = domDocumentFragmentCreate(document);
    PrDomNode* a = domTextCreate(document, "a");
    PrDomNode* b = domTextCreate(document, "b");
    domAttach(fragment, a, nullptr);
    domAttach(fragment, b, nullptr);
    domNodeRetain(fragment);
    CHECK(domPreInsert(parent, fragment, last) == PrStatus_Ok);
    CHECK(fragment->first == nullptr);
    CHECK(parent->first == a);
    CHECK(a->next == b);
    CHECK(b->next == last);
    PrString text = domChildTextContent(parent);
    CHECK(prStringView(&text) == "abc");
    prStringReset(&text);
    CHECK(ownedCount(document) == 5);
    // Once released, the fragment is freed.
    domNodeRelease(fragment);
    CHECK(ownedCount(document) == 4);
    domNodeRelease(document);
}

TEST_CASE("replacing a child and replacing all children leave the rest detached") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* parent = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "div");
    domNodeRetain(parent);
    PrDomNode* a = domTextCreate(document, "a");
    PrDomNode* b = domTextCreate(document, "b");
    domAttach(parent, a, nullptr);
    domNodeRetain(a);
    domNodeRetain(b);
    CHECK(domReplace(parent, b, a) == PrStatus_Ok);
    CHECK(parent->first == b);
    CHECK(a->parent == nullptr);

    domSetTextContent(parent, "text");
    REQUIRE(parent->first != nullptr);
    CHECK(parent->first == parent->last);
    CHECK(b->parent == nullptr);
    domSetTextContent(parent, "");
    CHECK(parent->first == nullptr);
    CHECK(ownedCount(document) == 3);
    domNodeRelease(a);
    domNodeRelease(b);
    CHECK(ownedCount(document) == 1);
    domNodeRelease(parent);
    domNodeRelease(document);
}

TEST_CASE("element and attribute local names are validated by their own rules") {
    CHECK(domIsValidElementLocalName("div"));
    CHECK(domIsValidElementLocalName("my-element"));
    CHECK(domIsValidElementLocalName("aé"));
    CHECK(domIsValidElementLocalName("_x.y:z"));
    CHECK(domIsValidElementLocalName("é"));
    CHECK(!domIsValidElementLocalName(""));
    CHECK(!domIsValidElementLocalName("1a"));
    CHECK(!domIsValidElementLocalName("a b"));
    CHECK(!domIsValidElementLocalName("a>"));
    CHECK(!domIsValidElementLocalName("_a b"));
    CHECK(!domIsValidElementLocalName("-a"));
    CHECK(domIsValidAttributeLocalName("data-x"));
    CHECK(domIsValidAttributeLocalName("1"));
    CHECK(!domIsValidAttributeLocalName(""));
    CHECK(!domIsValidAttributeLocalName("a=b"));
    CHECK(!domIsValidAttributeLocalName("a/"));
}

TEST_CASE("a removed attribute with no references is freed") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* element = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "div");
    domSetAttribute(element, PrDomNamespace_None, "a", "1");
    domSetAttribute(element, PrDomNamespace_None, "b", "2");
    domRemoveAttribute(element, PrDomNamespace_None, "A");
    CHECK(domAttribute(element, PrDomNamespace_None, "a") == nullptr);
    CHECK(domElementAttributeCount(element) == 1);
    CHECK(element->firstAttribute == element->lastAttribute);
    CHECK(ownedCount(document) == 2);
    domNodeRelease(document);
}

TEST_CASE("nodes and strings are converted into one node and placed around a child") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* parent = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "div");
    domNodeRetain(parent);
    PrDomElement* middle = domElementCreate(document, PrDomNamespace_Html, PrDomTag_B, "b");
    domAttach(parent, middle, nullptr);
    domNodeRetain(middle);
    PrDomNodeOrString around[] = {{nullptr, "a"}, {nullptr, "c"}};
    CHECK(domBefore(middle, around, 1) == PrStatus_Ok);
    CHECK(domAfter(middle, around + 1, 1) == PrStatus_Ok);
    CHECK(domAppend(parent, around, 2) == PrStatus_Ok);
    PrString text = domChildTextContent(parent);
    CHECK(prStringView(&text) == "acac");
    prStringReset(&text);
    CHECK(middle->previous != nullptr);

    // A node among the arguments is not its own viable sibling.
    PrDomNodeOrString self[] = {{middle, PrStringView{}}};
    CHECK(domReplaceWith(middle, self, 1) == PrStatus_Ok);
    CHECK(middle->parent == parent);

    PrDomNodeOrString doc[] = {{document, PrStringView{}}};
    CHECK(domPrepend(parent, doc, 1) == PrStatus_NotAllowed);
    CHECK(domReplaceChildren(parent, nullptr, 0) == PrStatus_Ok);
    CHECK(parent->first == nullptr);
    domRemoveFromParent(middle);
    CHECK(middle->parent == nullptr);
    domNodeRelease(middle);
    domNodeRelease(parent);
    domNodeRelease(document);
}

TEST_CASE("character data is edited in UTF-16 code units") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    // U+1F600 is two code units, so the data is five long.
    PrDomText* text = domTextCreate(document, "a\xF0\x9F\x98\x80" "bc");
    PrDomCharacterData* data = text;
    CHECK(prDomCharacterDataLength(data) == 5);
    PrString part = {};
    CHECK(prDomCharacterDataSubstringData(data, 1, 2, &part) == PrStatus_Ok);
    CHECK(taken(part) == "\xF0\x9F\x98\x80");
    CHECK(prDomCharacterDataSubstringData(data, 2, 1, &part) == PrStatus_Ok);
    CHECK(taken(part) == "\xEF\xBF\xBD");
    CHECK(prDomCharacterDataSubstringData(data, 6, 1, &part) == PrStatus_OutOfRange);
    CHECK(prDomCharacterDataInsertData(data, 5, "d") == PrStatus_Ok);
    CHECK(prDomCharacterDataDeleteData(data, 0, 1) == PrStatus_Ok);
    CHECK(prDomCharacterDataReplaceData(data, 2, 1, "X") == PrStatus_Ok);
    prDomCharacterDataAppendData(data, "!");
    CHECK(textOf(prDomCharacterDataData(data)) == "\xF0\x9F\x98\x80Xcd!");
    domNodeRelease(document);
}

TEST_CASE("splitting a text node leaves the rest in a sibling, and wholeText joins them") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* parent = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "div");
    PrDomText* text = domTextCreate(document, "hello");
    domAttach(document, parent, nullptr);
    domAttach(parent, text, nullptr);
    PrDomText* split = nullptr;
    CHECK(prDomTextSplitText(text, 2, &split) == PrStatus_Ok);
    CHECK(taken(prStringClone(&text->data)) == "he");
    REQUIRE(split != nullptr);
    CHECK(text->next == split);
    CHECK(taken(prDomTextWholeText(split)) == "hello");
    CHECK(prDomTextSplitText(text, 3, &split) == PrStatus_OutOfRange);
    domNodeRelease(document);
}

TEST_CASE("a document's title is its title element's text, stripped and collapsed") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomDocument* handle = document;
    PrDomElement* html = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Html, "html");
    PrDomElement* head = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Head, "head");
    domAttach(document, html, nullptr);
    CHECK(taken(prDomDocumentTitle(handle)) == "");
    prDomDocumentSetTitle(handle, "ignored");
    CHECK(taken(prDomDocumentTitle(handle)) == "");
    domAttach(html, head, nullptr);
    prDomDocumentSetTitle(handle, "  a \n b  ");
    CHECK(taken(prDomDocumentTitle(handle)) == "a b");
    CHECK(head->first != nullptr);

    PrDomElement* body = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Body, "body");
    CHECK(prDomDocumentSetBody(handle, head) == PrStatus_NotAllowed);
    CHECK(prDomDocumentSetBody(handle, body) == PrStatus_Ok);
    CHECK(prDomDocumentBody(handle) == body);
    domNodeRelease(document);
}

TEST_CASE("a processing instruction's target is an XML name and its data holds no ?>") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomDocument* handle = document;
    PrDomProcessingInstruction* instruction = nullptr;
    CHECK(prDomProcessingInstructionCreate(handle, "xml-stylesheet", "a", &instruction) == PrStatus_Ok);
    CHECK(instruction != nullptr);
    CHECK(prDomProcessingInstructionCreate(handle, "1x", "a", &instruction) == PrStatus_InvalidData);
    CHECK(prDomProcessingInstructionCreate(handle, "x", "a?>b", &instruction) == PrStatus_InvalidData);
    domNodeRelease(document);
}
