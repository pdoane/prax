// Taffy's adapter of the flex benchmark (taffy_engine.h).

#include "tools/bench/flex_bench/taffy_engine.h"

#include <cstddef>

// The C interface of Taffy's Rust library, by the names that it exports.
struct TaffyTree;
extern "C" TaffyTree* prFlexTaffyTreeNew();
extern "C" void prFlexTaffyTreeFree(TaffyTree* tree);
extern "C" uint64_t prFlexTaffyNodeNew(TaffyTree* tree, const FlexStyle* style, const uint64_t* children, size_t count);
extern "C" void prFlexTaffyNodeSetStyle(TaffyTree* tree, uint64_t node, const FlexStyle* style);
extern "C" void prFlexTaffyLayout(TaffyTree* tree, uint64_t root, float width, float height);
extern "C" uint32_t prFlexTaffyLayoutCounted(TaffyTree* tree, uint64_t root, float width, float height);

namespace flex_bench {
namespace {

class Taffy final : public Engine {
public:
    ~Taffy() override { destroy(); }

    const char* name() const override { return "taffy"; }

    // The benchmark's styles are already in the values that Taffy builds a node's style from.
    void prepare(const Tree&) override {}

    void build(const Tree& tree) override {
        destroy();
        tree_ = prFlexTaffyTreeNew();
        built_ = &tree;
        nodes_.assign(tree.styles.size(), 0);
        root_ = make(tree, tree.root);
    }

    void destroy() override {
        if (tree_ != nullptr)
            prFlexTaffyTreeFree(tree_);
        tree_ = nullptr;
    }

    void setWidth(uint32_t index, float px) override {
        FlexStyle style = built_->styles[index];
        style.width = FlexLength{.kind = FlexLengthKind_Px, .value = px};
        prFlexTaffyNodeSetStyle(tree_, nodes_[index], &style);
    }

    void layout(float width, float height) override {
        if (counting_)
            measured_ = prFlexTaffyLayoutCounted(tree_, root_, width, height);
        else
            prFlexTaffyLayout(tree_, root_, width, height);
    }

    void startCounting() override { counting_ = true; }

    Shape stopCounting() override {
        counting_ = false;
        return Shape{.measured = measured_};
    }

private:
    // Taffy creates a node together with its children, so the children are made first. Their nodes are gathered after
    // all of them are made, so one buffer serves every level.
    uint64_t make(const Tree& tree, uint32_t index) {
        for (uint32_t child : tree.children[index])
            make(tree, child);
        children_.clear();
        for (uint32_t child : tree.children[index])
            children_.push_back(nodes_[child]);
        nodes_[index] = prFlexTaffyNodeNew(tree_, &tree.styles[index], children_.data(), children_.size());
        return nodes_[index];
    }

    TaffyTree* tree_ = nullptr;
    uint64_t root_ = 0;
    const Tree* built_ = nullptr;
    std::vector<uint64_t> nodes_;
    std::vector<uint64_t> children_;
    bool counting_ = false;
    int64_t measured_ = -1;
};

} // namespace

std::unique_ptr<Engine> makeTaffy() {
    return std::make_unique<Taffy>();
}

} // namespace flex_bench
