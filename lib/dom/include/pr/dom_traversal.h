// Traversal: NodeFilter, NodeIterator and TreeWalker (dom/#traversal).

#pragma once

#include "pr/base.h"
#include "pr/dom.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// NodeFilter

// A filter's answer for a node. The traversal steps compare the answer with these values, so a filter may also return
// any other value below PrDomNodeFilterResult_Abandon (dom/#concept-node-filter).
enum {
    PrDomNodeFilterResult_Accept = 1,
    PrDomNodeFilterResult_Reject = 2,
    PrDomNodeFilterResult_Skip = 3,
    PrDomNodeFilterResult_Abandon = 1 << 16,    // ends the traversal with PrStatus_NotAllowed
};
typedef uint32_t PrDomNodeFilterResult;

// The node types that a traversal shows, as bits of whatToShow (dom/#interface-nodefilter). The flag of a node type
// with value `n` is bit `n - 1`.
enum {
    PrDomNodeShowFlag_Element = 1u << 0,
    PrDomNodeShowFlag_Attribute = 1u << 1,
    PrDomNodeShowFlag_Text = 1u << 2,
    PrDomNodeShowFlag_CdataSection = 1u << 3,
    PrDomNodeShowFlag_EntityReference = 1u << 4,
    PrDomNodeShowFlag_Entity = 1u << 5,
    PrDomNodeShowFlag_ProcessingInstruction = 1u << 6,
    PrDomNodeShowFlag_Comment = 1u << 7,
    PrDomNodeShowFlag_Document = 1u << 8,
    PrDomNodeShowFlag_DocumentType = 1u << 9,
    PrDomNodeShowFlag_DocumentFragment = 1u << 10,
    PrDomNodeShowFlag_Notation = 1u << 11,
};
typedef uint32_t PrDomNodeShowFlags;

// Shows every node.
static const PrDomNodeShowFlags kPrDomNodeShowAll = 0xFFFFFFFFu;

// Returns the filter's answer for `node`, which is a node of a type that whatToShow includes.
typedef PrDomNodeFilterResult (*PrDomNodeFilterFn)(void* PR_NULLABLE context, PrDomNode* node);

// A node filter. A traversal calls `retain` when it takes the filter and `release` when it is destroyed. A traversal
// does not read `identity`, which identifies the filter to its creator.
typedef struct PrDomNodeFilter {
    PrDomNodeFilterFn invoke;
    void* PR_NULLABLE context;
    uint64_t identity;
    void (*PR_NULLABLE retain)(void* PR_NULLABLE context);
    void (*PR_NULLABLE release)(void* PR_NULLABLE context);
} PrDomNodeFilter;

// ---------------------------------------------------------------------------------------------------------------------
// NodeIterator (dom/#interface-nodeiterator)

// A position between two nodes among the inclusive descendants of a root. A removal in the root's document adjusts the
// position so that it stays valid.
typedef struct PrDomNodeIterator PrDomNodeIterator;

// Creates an iterator positioned before `root`. A null `filter` accepts every node of a type that `whatToShow`
// includes.
PrDomNodeIterator* prDomNodeIteratorCreate(PrDomNode* root, uint32_t whatToShow, const PrDomNodeFilter* PR_NULLABLE filter);

// Frees the iterator.
void prDomNodeIteratorDestroy(PrDomNodeIterator* PR_NULLABLE iterator);

// The iterator's attributes. The reference node and whether the pointer is before it give the iterator's position.
PrDomNode* prDomNodeIteratorRoot(PrDomNodeIterator* iterator);
PrDomNode* prDomNodeIteratorReferenceNode(PrDomNodeIterator* iterator);
bool prDomNodeIteratorPointerBeforeReferenceNode(const PrDomNodeIterator* iterator);
uint32_t prDomNodeIteratorWhatToShow(const PrDomNodeIterator* iterator);
const PrDomNodeFilter* PR_NULLABLE prDomNodeIteratorFilter(const PrDomNodeIterator* iterator);

// Moves to the next or previous node that the filter accepts, and sets `*out` to that node, or to null if there is
// none. The iterator keeps a reference to the node. Returns InvalidState if the iterator's filter is running, and
// NotAllowed if the filter abandons the traversal.
PrStatus prDomNodeIteratorNextNode(PrDomNodeIterator* iterator, PrDomNode* PR_NULLABLE* PR_NONNULL out);
PrStatus prDomNodeIteratorPreviousNode(PrDomNodeIterator* iterator, PrDomNode* PR_NULLABLE* PR_NONNULL out);

// ---------------------------------------------------------------------------------------------------------------------
// TreeWalker (dom/#interface-treewalker)

// A walk over the inclusive descendants of a root, which moves from a current node.
typedef struct PrDomTreeWalker PrDomTreeWalker;

// Creates a walker whose current node is `root`. A null `filter` accepts every node of a type that `whatToShow`
// includes.
PrDomTreeWalker* prDomTreeWalkerCreate(PrDomNode* root, uint32_t whatToShow, const PrDomNodeFilter* PR_NULLABLE filter);

// Frees the walker.
void prDomTreeWalkerDestroy(PrDomTreeWalker* PR_NULLABLE walker);

// The walker's fixed attributes.
PrDomNode* prDomTreeWalkerRoot(PrDomTreeWalker* walker);
uint32_t prDomTreeWalkerWhatToShow(const PrDomTreeWalker* walker);
const PrDomNodeFilter* PR_NULLABLE prDomTreeWalkerFilter(const PrDomTreeWalker* walker);

// The current node, which is the node that the last move reached or the node last set. It may be outside the root.
PrDomNode* prDomTreeWalkerCurrentNode(PrDomTreeWalker* walker);
void prDomTreeWalkerSetCurrentNode(PrDomTreeWalker* walker, PrDomNode* node);

// Move to the named node that the filter accepts, and set `*out` to that node, or to null if there is none. The walker
// keeps a reference to the node. These return InvalidState if the walker's filter is running, and NotAllowed if the
// filter abandons the traversal.
PrStatus prDomTreeWalkerParentNode(PrDomTreeWalker* walker, PrDomNode* PR_NULLABLE* PR_NONNULL out);
PrStatus prDomTreeWalkerFirstChild(PrDomTreeWalker* walker, PrDomNode* PR_NULLABLE* PR_NONNULL out);
PrStatus prDomTreeWalkerLastChild(PrDomTreeWalker* walker, PrDomNode* PR_NULLABLE* PR_NONNULL out);
PrStatus prDomTreeWalkerPreviousSibling(PrDomTreeWalker* walker, PrDomNode* PR_NULLABLE* PR_NONNULL out);
PrStatus prDomTreeWalkerNextSibling(PrDomTreeWalker* walker, PrDomNode* PR_NULLABLE* PR_NONNULL out);
PrStatus prDomTreeWalkerPreviousNode(PrDomTreeWalker* walker, PrDomNode* PR_NULLABLE* PR_NONNULL out);
PrStatus prDomTreeWalkerNextNode(PrDomTreeWalker* walker, PrDomNode* PR_NULLABLE* PR_NONNULL out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
