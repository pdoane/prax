#include "pr/dom_traversal.h"

#include "dom/src/dom_node.h"

#include "doctest/doctest.h"

namespace pr_impl {

namespace {

// A root with the children a, b and c, and a child of b.
struct Tree {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* root = element();
    PrDomElement* a = element();
    PrDomElement* b = element();
    PrDomElement* inner = element();
    PrDomElement* c = element();

    Tree() {
        domNodeRetain(document);
        domAttach(document, root, nullptr);
        domAttach(root, a, nullptr);
        domAttach(root, b, nullptr);
        domAttach(b, inner, nullptr);
        domAttach(root, c, nullptr);
    }

    ~Tree() { domNodeRelease(document); }

    PrDomElement* element() {
        return domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    }
};

PrDomNode* next(PrDomNodeIterator* iterator) {
    PrDomNode* node = nullptr;
    CHECK(prDomNodeIteratorNextNode(iterator, &node) == PrStatus_Ok);
    return node;
}

PrDomNodeFilterResult rejectB(void* context, PrDomNode* node) {
    return node == static_cast<PrDomNode*>(context) ? PrDomNodeFilterResult_Reject : PrDomNodeFilterResult_Accept;
}

// Removes `context` while the filter is called with it, and accepts every node.
PrDomNodeFilterResult removeCandidate(void* context, PrDomNode* node) {
    if (node == static_cast<PrDomNode*>(context))
        domRemoveFromParent(node);
    return PrDomNodeFilterResult_Accept;
}

PrDomNodeFilterResult abandon(void*, PrDomNode*) {
    return PrDomNodeFilterResult_Abandon;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("traversal: an iterator visits in tree order, and only the types that it shows") {
    Tree tree;
    PrDomNodeIterator* iterator = prDomNodeIteratorCreate(tree.root, PrDomNodeShowFlag_Element, nullptr);
    CHECK(next(iterator) == tree.root);
    CHECK(next(iterator) == tree.a);
    CHECK(next(iterator) == tree.b);
    CHECK(next(iterator) == tree.inner);
    CHECK(next(iterator) == tree.c);
    CHECK(next(iterator) == nullptr);
    PrDomNode* node = nullptr;
    CHECK(prDomNodeIteratorPreviousNode(iterator, &node) == PrStatus_Ok);
    CHECK(node == tree.c);
    prDomNodeIteratorDestroy(iterator);
}

TEST_CASE("traversal: removing the reference node moves an iterator to what follows or precedes it") {
    Tree tree;
    PrDomNodeIterator* iterator = prDomNodeIteratorCreate(tree.root, kPrDomNodeShowAll, nullptr);
    CHECK(next(iterator) == tree.root);
    CHECK(next(iterator) == tree.a);
    CHECK(next(iterator) == tree.b);
    CHECK(next(iterator) == tree.inner);
    // The pointer is after the reference, so the reference moves to the last node before the removed subtree.
    domRemoveFromParent(tree.b);
    CHECK(prDomNodeIteratorReferenceNode(iterator) == tree.a);
    CHECK(!prDomNodeIteratorPointerBeforeReferenceNode(iterator));
    CHECK(next(iterator) == tree.c);
    prDomNodeIteratorDestroy(iterator);
}

TEST_CASE("traversal: removing an ancestor of an iterator's root leaves the iterator where it is") {
    Tree tree;
    PrDomNodeIterator* iterator = prDomNodeIteratorCreate(tree.inner, kPrDomNodeShowAll, nullptr);
    CHECK(next(iterator) == tree.inner);
    domRemoveFromParent(tree.b);
    CHECK(prDomNodeIteratorReferenceNode(iterator) == tree.inner);
    prDomNodeIteratorDestroy(iterator);
}

TEST_CASE("traversal: a filter that removes the candidate node moves the position, and the move returns the node") {
    Tree tree;
    domNodeRetain(tree.a);
    PrDomNodeFilter filter{.invoke = removeCandidate, .context = tree.a};
    PrDomNodeIterator* iterator = prDomNodeIteratorCreate(tree.root, kPrDomNodeShowAll, &filter);
    CHECK(next(iterator) == tree.root);
    CHECK(next(iterator) == tree.a);
    CHECK(prDomNodeIteratorReferenceNode(iterator) == tree.root);
    CHECK(next(iterator) == tree.b);
    prDomNodeIteratorDestroy(iterator);
    domNodeRelease(tree.a);
}

TEST_CASE("traversal: a walker skips a rejected subtree and stays at its node past the end") {
    Tree tree;
    PrDomNodeFilter filter{.invoke = rejectB, .context = tree.b};
    PrDomTreeWalker* walker = prDomTreeWalkerCreate(tree.root, kPrDomNodeShowAll, &filter);
    PrDomNode* node = nullptr;
    CHECK(prDomTreeWalkerNextNode(walker, &node) == PrStatus_Ok);
    CHECK(node == tree.a);
    CHECK(prDomTreeWalkerNextNode(walker, &node) == PrStatus_Ok);
    CHECK(node == tree.c);
    CHECK(prDomTreeWalkerNextNode(walker, &node) == PrStatus_Ok);
    CHECK(node == nullptr);
    CHECK(prDomTreeWalkerCurrentNode(walker) == tree.c);
    prDomTreeWalkerDestroy(walker);
}

TEST_CASE("traversal: a walker whose current node is outside the root ends at the top of that tree") {
    Tree tree;
    PrDomTreeWalker* walker = prDomTreeWalkerCreate(tree.a, kPrDomNodeShowAll, nullptr);
    domNodeRetain(tree.c);
    domRemoveFromParent(tree.c);
    prDomTreeWalkerSetCurrentNode(walker, tree.c);
    PrDomNode* node = nullptr;
    CHECK(prDomTreeWalkerNextNode(walker, &node) == PrStatus_Ok);
    CHECK(node == nullptr);
    prDomTreeWalkerDestroy(walker);
    domNodeRelease(tree.c);
}

TEST_CASE("traversal: a filter that abandons ends the move, and the position stays") {
    Tree tree;
    PrDomNodeFilter filter{.invoke = abandon};
    PrDomTreeWalker* walker = prDomTreeWalkerCreate(tree.root, kPrDomNodeShowAll, &filter);
    PrDomNode* node = nullptr;
    CHECK(prDomTreeWalkerFirstChild(walker, &node) == PrStatus_NotAllowed);
    CHECK(node == nullptr);
    CHECK(prDomTreeWalkerCurrentNode(walker) == tree.root);
    prDomTreeWalkerDestroy(walker);
}

TEST_CASE("traversal: an iterator follows its root to the document that adopts it") {
    Tree tree;
    PrDomDocument* other = domDocumentCreate(DomDocumentKind_Html);
    domNodeRetain(other);
    PrDomNodeIterator* iterator = prDomNodeIteratorCreate(tree.inner, kPrDomNodeShowAll, nullptr);
    CHECK(prDomDocumentAdoptNode(other, tree.b) == PrStatus_Ok);
    CHECK(other->iterators == iterator);
    CHECK(tree.document->iterators == nullptr);
    prDomNodeIteratorDestroy(iterator);
    domNodeRelease(other);
}

TEST_CASE("namespace lookups read the elements and the declarations in scope") {
    Tree tree;
    PrDomNode* node = tree.inner;
    CHECK(prDomNodeLookupNamespaceUri(node, "xml") == "http://www.w3.org/XML/1998/namespace");
    CHECK(prDomNodeLookupNamespaceUri(node, "") == "http://www.w3.org/1999/xhtml");
    CHECK(prDomNodeIsDefaultNamespace(node, "http://www.w3.org/1999/xhtml"));
    CHECK(prDomNodeLookupPrefix(node, "urn:missing").data == nullptr);
    CHECK(prDomNodeLookupNamespaceUri(tree.document, "missing").data == nullptr);
}

TEST_CASE("namespace lookups read an xmlns declaration of an ancestor") {
    Tree tree;
    CHECK(prDomElementSetAttributeNs(tree.b, "http://www.w3.org/2000/xmlns/", "xmlns:p", "urn:p") == PrStatus_Ok);
    CHECK(prDomNodeLookupNamespaceUri(tree.inner, "p") == "urn:p");
    CHECK(prDomNodeLookupPrefix(tree.inner, "urn:p") == "p");
    CHECK(prDomNodeLookupNamespaceUri(tree.a, "p").data == nullptr);
}
