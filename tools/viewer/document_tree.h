// A collection's documents as a tree of the directories that their names hold, which the controls list. A name's
// slashes separate its directories, unless the document gives its own folders. The tree keeps the collection's order,
// so a directory comes where its first document does.

#pragma once

#include "tools/viewer/documents.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// A row of the tree as the controls list it: a node, and its depth under the root.
struct TreeRow {
    uint32_t node;
    uint32_t depth;
};

// The tree of a collection's documents, with the rows that a filter and the expanded directories list.
class DocumentTree {
public:
    // A directory, or a document of the collection. Node 0 is the root, which is not listed.
    struct Node {
        std::string name;               // the last part of the path
        uint32_t parent = 0;
        std::vector<uint32_t> children;
        int32_t document = -1;          // the document's index, or -1 for a directory
        bool expanded = false;          // a directory: true if its children are listed
        uint32_t matches = 0;           // the number of documents under it, or itself, that the filter admits
    };

    explicit DocumentTree(const std::vector<Document>& documents);

    const std::vector<Node>& nodes() const { return nodes_; }

    // Returns the rows listed: each node whose parent is expanded and listed, and that holds a document that the filter
    // admits.
    const std::vector<TreeRow>& rows() const { return rows_; }

    // Returns the number of documents that the filter admits.
    uint32_t matching() const { return nodes_[0].matches; }

    // Returns the node of the document.
    uint32_t nodeOf(uint32_t document) const { return nodeOfDocument_[document]; }

    // Admits only the documents whose names contain `filter`, ignoring case. An empty filter admits every document. A
    // new filter expands the directories that hold an admitted document, and collapses the others.
    void filter(std::string_view filter);

    // Expands or collapses a directory.
    void setExpanded(uint32_t node, bool expanded);

    // Expands the directories that hold the document, so that it is listed if the filter admits it.
    void reveal(uint32_t document);

private:
    void count(uint32_t node);
    void list(uint32_t node, uint32_t depth);
    void relist();

    std::vector<Node> nodes_;
    std::vector<uint32_t> nodeOfDocument_;
    std::vector<std::string> names_;    // each document's name, folded to lowercase
    std::string filter_;
    std::vector<TreeRow> rows_;
};

} // namespace viewer

PR_ASSUME_NONNULL_END
