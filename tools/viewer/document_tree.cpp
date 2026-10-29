// The tree of a collection's documents (document_tree.h).

#include "tools/viewer/document_tree.h"

namespace viewer {

namespace {

std::string folded(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    }
    return out;
}

} // namespace

DocumentTree::DocumentTree(const std::vector<Document>& documents) {
    nodes_.push_back(Node{});
    nodes_[0].expanded = true;
    for (uint32_t d = 0; d < uint32_t(documents.size()); ++d) {
        const std::string& name = documents[d].name;
        names_.push_back(folded(name));
        std::vector<std::string_view> parts;
        size_t start = 0;
        for (size_t slash = name.find('/'); slash != std::string::npos; slash = name.find('/', start)) {
            parts.push_back(std::string_view(name).substr(start, slash - start));
            start = slash + 1;
        }
        if (!documents[d].folders.empty())
            parts.assign(documents[d].folders.begin(), documents[d].folders.end());
        uint32_t parent = 0;
        for (std::string_view part : parts) {
            if (part.empty())
                continue;
            uint32_t found = 0;
            for (uint32_t child : nodes_[parent].children) {
                if (nodes_[child].document < 0 && nodes_[child].name == part)
                    found = child;
            }
            if (found != 0) {
                parent = found;
                continue;
            }
            Node directory;
            directory.name = std::string(part);
            directory.parent = parent;
            uint32_t index = uint32_t(nodes_.size());
            nodes_.push_back(std::move(directory));
            nodes_[parent].children.push_back(index);
            parent = index;
        }
        Node file;
        file.name = name.substr(start);
        file.parent = parent;
        file.document = int32_t(d);
        uint32_t index = uint32_t(nodes_.size());
        nodes_.push_back(std::move(file));
        nodes_[parent].children.push_back(index);
        nodeOfDocument_.push_back(index);
    }
    count(0);
    relist();
}

void DocumentTree::count(uint32_t node) {
    Node& n = nodes_[node];
    if (n.document >= 0) {
        n.matches = names_[uint32_t(n.document)].find(filter_) != std::string::npos ? 1 : 0;
        return;
    }
    n.matches = 0;
    for (uint32_t child : n.children) {
        count(child);
        n.matches += nodes_[child].matches;
    }
}

void DocumentTree::list(uint32_t node, uint32_t depth) {
    for (uint32_t child : nodes_[node].children) {
        const Node& c = nodes_[child];
        if (c.matches == 0)
            continue;
        rows_.push_back(TreeRow{child, depth});
        if (c.document < 0 && c.expanded)
            list(child, depth + 1);
    }
}

void DocumentTree::relist() {
    rows_.clear();
    list(0, 0);
}

void DocumentTree::filter(std::string_view filter) {
    std::string next = folded(filter);
    if (next == filter_)
        return;
    filter_ = std::move(next);
    count(0);
    for (size_t i = 1; i < nodes_.size(); ++i) {
        if (nodes_[i].document < 0)
            nodes_[i].expanded = !filter_.empty() && nodes_[i].matches != 0;
    }
    relist();
}

void DocumentTree::setExpanded(uint32_t node, bool expanded) {
    if (nodes_[node].document >= 0 || nodes_[node].expanded == expanded)
        return;
    nodes_[node].expanded = expanded;
    relist();
}

void DocumentTree::reveal(uint32_t document) {
    bool changed = false;
    for (uint32_t node = nodes_[nodeOf(document)].parent; node != 0; node = nodes_[node].parent) {
        changed = changed || !nodes_[node].expanded;
        nodes_[node].expanded = true;
    }
    if (changed)
        relist();
}

} // namespace viewer
