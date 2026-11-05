// Marks: a paint that marks content labels each text fragment's glyphs and each replaced box's content with its node's
// key, and gives each glyph run its text.

#include "layout/tests/layout_fixture.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

// A glyph run as a paint recorded it, with the key of the mark around it.
struct MarkedRun {
    uint64_t key;
    std::string text;
    std::vector<uint32_t> clusters;
};

void appendRuns(const PrDrawList* list, std::vector<uint64_t>& keys, std::vector<MarkedRun>& out, uint32_t& marks) {
    const PrDrawCommand* commands = prDrawListCommands(list);
    for (uint32_t i = 0; i < prDrawListCommandCount(list); ++i) {
        const PrDrawCommand& c = commands[i];
        if (c.kind == PrDrawCommandKind_Embed) {
            appendRuns(c.embed.list, keys, out, marks);
        } else if (c.kind == PrDrawCommandKind_PushMark) {
            keys.push_back(c.key);
            ++marks;
        } else if (c.kind == PrDrawCommandKind_PopMark) {
            keys.pop_back();
        } else if (c.kind == PrDrawCommandKind_Glyphs) {
            MarkedRun run{keys.empty() ? 0 : keys.back(), {}, {}};
            if (c.glyphs->text.data != nullptr)
                run.text.assign(c.glyphs->text.data, c.glyphs->text.size);
            if (c.glyphs->clusters != nullptr)
                run.clusters.assign(c.glyphs->clusters, c.glyphs->clusters + c.glyphs->glyphCount);
            out.push_back(run);
        }
    }
}

std::vector<MarkedRun> paintedRuns(Fixture& f, uint32_t* marks) {
    PrDrawList* list = prDrawListCreate();
    prLayoutTreePaint(f.tree, list);
    std::vector<uint64_t> keys;
    std::vector<MarkedRun> runs;
    *marks = 0;
    appendRuns(list, keys, runs, *marks);
    prDrawListRelease(list);
    return runs;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a paint marks text by its node's key only when the tree marks content") {
    Fixture f;
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    font.font = f.textFont(20);
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font);
    PrLayoutNode text = f.text(root, "Hello\nworld");
    prLayoutNodeSetKey(f.tree, text, 42);
    f.layout(px(800, 600));
    uint32_t marks = 0;
    std::vector<MarkedRun> runs = paintedRuns(f, &marks);
    REQUIRE(!runs.empty());
    CHECK(marks == 0);
    CHECK(runs[0].text.empty());
    CHECK(runs[0].clusters.empty());

    prLayoutTreeSetMarking(f.tree, PrLayoutMarking_Content);
    CHECK(prLayoutTreeMarking(f.tree) == PrLayoutMarking_Content);
    runs = paintedRuns(f, &marks);
    REQUIRE(runs.size() == 1);
    CHECK(marks == 1);
    CHECK(runs[0].key == 42);
    CHECK(runs[0].text == "Hello world");
    CHECK(runs[0].clusters == std::vector<uint32_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
    CHECK(verified(f.tree));

    prLayoutTreeSetMarking(f.tree, PrLayoutMarking_None);
    runs = paintedRuns(f, &marks);
    CHECK(marks == 0);
}

TEST_CASE("each line of a marked text node is its own mark, with the text of that line") {
    Fixture f;
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    font.font = f.textFont(20);
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font);
    PrLayoutNode text = f.text(root, "alpha beta gamma delta");
    prLayoutNodeSetKey(f.tree, text, 7);
    prLayoutTreeSetMarking(f.tree, PrLayoutMarking_Content);
    f.layout(px(80, 600));
    uint32_t marks = 0;
    std::vector<MarkedRun> runs = paintedRuns(f, &marks);
    REQUIRE(runs.size() >= 2);
    CHECK(marks == runs.size());
    std::string joined;
    for (const MarkedRun& run : runs) {
        CHECK(run.key == 7);
        joined += run.text;
    }
    CHECK(joined.find("alpha") == 0);
    CHECK(joined.find("delta") != std::string::npos);
}
