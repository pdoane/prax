#include "pr/layout_frame.h"
#include "pr/layout_query.h"
#include "pr/layout_scene.h"
#include "pr/layout_tree.h"
#include "pr/span.h"

#include "layout/tests/layout_fixture.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// Returns the committed tree under `node` as text: each text node's text, and each element's children in brackets.
String shape(PrLayoutTree* tree, PrLayoutNode node) {
    if (prLayoutNodeKind(tree, node) == PrLayoutNodeKind_Text)
        return String(prLayoutNodeText(tree, node));
    String out = "[";
    for (PrLayoutNode child = prLayoutNodeFirstChild(tree, node); child != kPrLayoutNodeNone;
            child = prLayoutNodeNextSibling(tree, child)) {
        if (out.count() > 1)
            out.append(' ');
        out.append(shape(tree, child));
    }
    out.append(']');
    return out;
}

String shape(PrLayoutTree* tree) {
    PrLayoutNode root = prLayoutTreeRoot(tree);
    return root == kPrLayoutNodeNone ? "" : shape(tree, root);
}

// One item of a list that a frame declares: its key, or 0 to be identified by its position, and its text.
struct Item {
    uint64_t key;
    PrStringView text;
};

// The nodes that a frame of a list declared.
struct ListFrame {
    PrLayoutNode root;
    PrLayoutNode list;
    Array<PrLayoutNode> rows;     // in the order of the items
};

// Declares, as one frame, a root holding a heading and a list, with a block for each item holding its text.
ListFrame declareList(Fixture& f, Span<const Item> items) {
    uint32_t box = f.style(block());
    ListFrame out{};
    prLayoutTreeBeginFrame(f.tree);
    out.root = prLayoutTreeFrameElement(f.tree, kPrLayoutNodeNone, 0, box);
    f.root = out.root;
    prLayoutTreeFrameText(f.tree, out.root, 0, "heading");
    out.list = prLayoutTreeFrameElement(f.tree, out.root, 0, box);
    for (const Item& item : items) {
        PrLayoutNode row = prLayoutTreeFrameElement(f.tree, out.list, item.key, box);
        prLayoutTreeFrameText(f.tree, row, 0, item.text);
        out.rows.push(row);
    }
    return out;
}

void layOut(Fixture& f) {
    f.layout(px(200, 400));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a frame that creates the committed tree again keeps every node and changes nothing") {
    Fixture f;
    const Item firstItems[] = {{0, "a"}, {0, "b"}, {0, "c"}};
    ListFrame first = declareList(f, firstItems);
    layOut(f);
    CHECK(prLayoutTreeStats(f.tree).frameNodesMade == 9);
    CHECK(shape(f.tree) == "[heading [[a] [b] [c]]]");
    const PrLayoutScene* scene = prLayoutTreeScene(f.tree);

    const Item againItems[] = {{0, "a"}, {0, "b"}, {0, "c"}};
    ListFrame again = declareList(f, againItems);
    CHECK(again.root == first.root);
    CHECK(again.list == first.list);
    CHECK(again.rows == first.rows);
    layOut(f);
    PrLayoutTreeStats stats = prLayoutTreeStats(f.tree);
    CHECK(stats.frameNodesKept == 9);
    CHECK(stats.frameNodesMade == 0);
    CHECK(stats.frameNodesJumped == 0);
    CHECK(stats.frameNodesGone == 0);
    CHECK(stats.frameParentsRelinked == 0);
    CHECK(stats.blocksLaidOut == 0);
    const PrLayoutScene* repainted = prLayoutTreeScene(f.tree);
    CHECK(repainted == scene);
    CHECK(verified(f.tree));
    prLayoutSceneRelease(repainted);
    prLayoutSceneRelease(scene);
}

TEST_CASE("a frame's insertion makes one node, and every node after it keeps its handle") {
    Fixture f;
    const Item firstItems[] = {{0, "a"}, {0, "c"}};
    ListFrame first = declareList(f, firstItems);
    layOut(f);
    // Unkeyed rows continue by position: the second row continues the row that held "c" and changes its text, and the
    // third is new.
    const Item byPlaceItems[] = {{0, "a"}, {0, "b"}, {0, "c"}};
    ListFrame byPlace = declareList(f, byPlaceItems);
    layOut(f);
    CHECK(byPlace.rows[0] == first.rows[0]);
    CHECK(byPlace.rows[1] == first.rows[1]);
    CHECK(prLayoutTreeStats(f.tree).frameNodesMade == 2);
    CHECK(shape(f.tree) == "[heading [[a] [b] [c]]]");
    CHECK(verified(f.tree));
}

TEST_CASE("keyed rows follow their keys through an insertion, a removal and a reorder") {
    Fixture f;
    const Item firstItems[] = {{1, "a"}, {2, "b"}, {3, "c"}};
    ListFrame first = declareList(f, firstItems);
    layOut(f);

    const Item insertedItems[] = {{1, "a"}, {4, "new"}, {2, "b"}, {3, "c"}};
    ListFrame inserted = declareList(f, insertedItems);
    layOut(f);
    CHECK(inserted.rows[0] == first.rows[0]);
    CHECK(inserted.rows[2] == first.rows[1]);
    CHECK(inserted.rows[3] == first.rows[2]);
    CHECK(prLayoutTreeStats(f.tree).frameNodesMade == 2);
    CHECK(prLayoutTreeStats(f.tree).frameParentsRelinked == 1);
    CHECK(shape(f.tree) == "[heading [[a] [new] [b] [c]]]");
    CHECK(verified(f.tree));

    const Item removedItems[] = {{1, "a"}, {4, "new"}, {3, "c"}};
    ListFrame removed = declareList(f, removedItems);
    layOut(f);
    CHECK(removed.rows[2] == first.rows[2]);
    CHECK(prLayoutTreeStats(f.tree).frameNodesGone == 1);
    CHECK_FALSE(prLayoutNodeIsValid(f.tree, first.rows[1]));
    CHECK(shape(f.tree) == "[heading [[a] [new] [c]]]");
    CHECK(verified(f.tree));

    const Item reorderedItems[] = {{3, "c"}, {1, "a"}, {4, "new"}};
    ListFrame reordered = declareList(f, reorderedItems);
    layOut(f);
    CHECK(reordered.rows[0] == first.rows[2]);
    CHECK(reordered.rows[1] == first.rows[0]);
    CHECK(reordered.rows[2] == inserted.rows[1]);
    CHECK(prLayoutTreeStats(f.tree).frameNodesMade == 0);
    CHECK(shape(f.tree) == "[heading [[c] [a] [new]]]");
    CHECK(verified(f.tree));
}

TEST_CASE("a key that a frame gives twice makes the second node without it, and the commit reports both nodes") {
    Fixture f;
    const Item firstItems[] = {{1, "a"}, {2, "b"}, {1, "again"}};
    ListFrame first = declareList(f, firstItems);
    CHECK(first.rows[2] != first.rows[0]);
    CHECK(prLayoutNodeKey(f.tree, first.rows[2]) == 0);
    layOut(f);
    CHECK(shape(f.tree) == "[heading [[a] [b] [again]]]");
    PrLayoutDuplicateKey duplicate{};
    REQUIRE(prLayoutTreeDuplicateKeys(f.tree, &duplicate, 1) == 1);
    CHECK(duplicate.key == 1);
    CHECK(duplicate.first == first.rows[0]);
    CHECK(duplicate.second == first.rows[2]);
    CHECK(verified(f.tree));

    // The report holds while the next frame is built, and a frame without the duplicate reports none.
    const Item withoutDuplicate[] = {{1, "a"}, {2, "b"}};
    declareList(f, withoutDuplicate);
    CHECK(prLayoutTreeDuplicateKeys(f.tree, nullptr, 0) == 1);
    layOut(f);
    CHECK(prLayoutTreeDuplicateKeys(f.tree, nullptr, 0) == 0);
    CHECK(shape(f.tree) == "[heading [[a] [b]]]");
    CHECK(verified(f.tree));
}

TEST_CASE("an unkeyed sibling keeps its node when a keyed one is inserted before it") {
    Fixture f;
    uint32_t box = f.style(block());
    auto declare = [&](bool keyed) {
        prLayoutTreeBeginFrame(f.tree);
        PrLayoutNode root = prLayoutTreeFrameElement(f.tree, kPrLayoutNodeNone, 0, box);
        f.root = root;
        if (keyed)
            prLayoutTreeFrameText(f.tree, root, 7, "keyed ");
        return prLayoutTreeFrameText(f.tree, root, 0, "place");
    };
    PrLayoutNode before = declare(false);
    layOut(f);
    PrLayoutNode after = declare(true);
    layOut(f);
    CHECK(after == before);
    CHECK(shape(f.tree) == "[keyed  place]");
    CHECK(verified(f.tree));
}

TEST_CASE("a node keyed apart from its parent moves between parents as itself") {
    Fixture f;
    uint32_t box = f.style(block());
    auto declare = [&](bool left) {
        prLayoutTreeBeginFrame(f.tree);
        PrLayoutNode root = prLayoutTreeFrameElement(f.tree, kPrLayoutNodeNone, 0, box);
        f.root = root;
        PrLayoutNode a = prLayoutTreeFrameElement(f.tree, root, 0, box);
        PrLayoutNode b = prLayoutTreeFrameElement(f.tree, root, 0, box);
        PrLayoutNode moved = prLayoutTreeFrameElement(f.tree, left ? a : b, 99, box);
        prLayoutTreeFrameText(f.tree, moved, 0, "moved");
        prLayoutTreeFrameText(f.tree, left ? b : a, 0, "stays");
        return moved;
    };
    PrLayoutNode first = declare(true);
    layOut(f);
    CHECK(shape(f.tree) == "[[[moved]] [stays]]");
    PrLayoutNode second = declare(false);
    layOut(f);
    CHECK(second == first);
    CHECK(shape(f.tree) == "[[stays] [[moved]]]");
    CHECK(prLayoutTreeStats(f.tree).frameNodesGone == 1);
    CHECK(verified(f.tree));
}

TEST_CASE("creations out of preorder give the tree creations in order give") {
    Fixture f;
    uint32_t box = f.style(block());
    auto declare = [&](bool inOrder) {
        prLayoutTreeBeginFrame(f.tree);
        PrLayoutNode root = prLayoutTreeFrameElement(f.tree, kPrLayoutNodeNone, 0, box);
        f.root = root;
        PrLayoutNode body = prLayoutTreeFrameElement(f.tree, root, 0, box);
        PrLayoutNode overlay = prLayoutTreeFrameElement(f.tree, root, 0, box);
        if (inOrder) {
            prLayoutTreeFrameText(f.tree, body, 0, "body");
            prLayoutTreeFrameText(f.tree, overlay, 0, "popover");
        } else {
            // A popover declared from deep in the body, into the overlay created before it.
            prLayoutTreeFrameText(f.tree, overlay, 0, "popover");
            prLayoutTreeFrameText(f.tree, body, 0, "body");
        }
        return root;
    };
    declare(true);
    layOut(f);
    CHECK(shape(f.tree) == "[[body] [popover]]");
    declare(false);
    layOut(f);
    // The popover's text and the body's are each found away from the next node in preorder.
    CHECK(prLayoutTreeStats(f.tree).frameNodesJumped == 2);
    CHECK(prLayoutTreeStats(f.tree).frameNodesMade == 0);
    CHECK(prLayoutTreeStats(f.tree).frameParentsRelinked == 0);
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);
    CHECK(shape(f.tree) == "[[body] [popover]]");
    CHECK(verified(f.tree));
}

TEST_CASE("a change in a long list relinks only the list, and the frame follows the preorder around it") {
    Fixture f;
    Array<String> texts;
    for (uint32_t i = 0; i < 20; ++i)
        texts.push(format("row {}", i));
    auto items = [&](uint64_t skip, uint64_t insertAfter) {
        Array<Item> out;
        for (uint32_t i = 0; i < 20; ++i) {
            if (i + 1 != skip)
                out.push(Item{i + 1, texts[i]});
            if (i + 1 == insertAfter)
                out.push(Item{100, "inserted"});
        }
        return out;
    };
    ListFrame first = declareList(f, items(0, 0).span());
    layOut(f);

    // The inserted row is made; every row after it is the next node in preorder.
    ListFrame inserted = declareList(f, items(0, 10).span());
    layOut(f);
    PrLayoutTreeStats stats = prLayoutTreeStats(f.tree);
    CHECK(stats.frameNodesMade == 2);
    CHECK(stats.frameNodesJumped == 0);
    CHECK(stats.frameParentsRelinked == 1);
    CHECK(inserted.rows[10] != first.rows[10]);
    CHECK(inserted.rows[11] == first.rows[10]);
    CHECK(verified(f.tree));

    // The row after the removed one is found by its key, and the frame goes on in preorder from it.
    ListFrame removed = declareList(f, items(5, 10).span());
    layOut(f);
    stats = prLayoutTreeStats(f.tree);
    CHECK(stats.frameNodesMade == 0);
    CHECK(stats.frameNodesJumped == 1);
    CHECK(stats.frameNodesGone == 1);
    CHECK(stats.frameParentsRelinked == 1);
    CHECK(removed.rows[4] == first.rows[5]);
    CHECK_FALSE(prLayoutNodeIsValid(f.tree, first.rows[4]));
    CHECK(verified(f.tree));
}

TEST_CASE("a frame after retained edits continues the tree as the edits left it") {
    Fixture f;
    uint32_t box = f.style(block());
    const Item firstItems[] = {{0, "a"}, {0, "b"}};
    ListFrame first = declareList(f, firstItems);
    layOut(f);
    // A retained edit restyles a row and adds a node to the list between frames.
    prLayoutNodeSetStyle(f.tree, first.rows[0], f.style(block(50)));
    PrLayoutNode extra = prLayoutNodeCreateElement(f.tree, kPrLayoutNodeNone, box);
    prLayoutTreeInsert(f.tree, first.list, extra, kPrLayoutNodeNone);
    layOut(f);
    CHECK(f.bounds(first.rows[0]).bottom - f.bounds(first.rows[0]).top == 50.0f);

    // The frame restates the rows' record and leaves out the added node.
    const Item againItems[] = {{0, "a"}, {0, "b"}};
    ListFrame again = declareList(f, againItems);
    layOut(f);
    CHECK(again.rows == first.rows);
    CHECK(prLayoutTreeStats(f.tree).frameNodesGone == 1);
    CHECK_FALSE(prLayoutNodeIsValid(f.tree, extra));
    CHECK(prLayoutNodeStyle(f.tree, first.rows[0]) == box);
    CHECK(shape(f.tree) == "[heading [[a] [b]]]");
    CHECK(verified(f.tree));
}

TEST_CASE("a record set after a component's own is compared at the commit, so restating last frame's changes nothing") {
    Fixture f;
    uint32_t plain = f.style(block());
    uint32_t tall = f.style(block(50));
    auto declare = [&](uint32_t restyle) {
        prLayoutTreeBeginFrame(f.tree);
        PrLayoutNode root = prLayoutTreeFrameElement(f.tree, kPrLayoutNodeNone, 0, plain);
        f.root = root;
        // A component makes its node plain, and its caller restyles it.
        PrLayoutNode node = prLayoutTreeFrameElement(f.tree, root, 0, plain);
        prLayoutTreeFrameSetStyle(f.tree, node, restyle);
        CHECK(prLayoutTreeFrameStyle(f.tree, node) == restyle);
        return node;
    };
    PrLayoutNode node = declare(tall);
    layOut(f);
    CHECK(f.bounds(node).bottom == 50.0f);
    declare(tall);
    layOut(f);
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);
    declare(plain);
    layOut(f);
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut != 0);
    CHECK(f.bounds(node).bottom == 0.0f);
    CHECK(verified(f.tree));
}

TEST_CASE("queries during a frame read the committed tree") {
    Fixture f;
    uint32_t plain = f.style(block());
    uint32_t tall = f.style(block(50));
    prLayoutTreeBeginFrame(f.tree);
    PrLayoutNode root = prLayoutTreeFrameElement(f.tree, kPrLayoutNodeNone, 0, plain);
    f.root = root;
    PrLayoutNode node = prLayoutTreeFrameElement(f.tree, root, 0, tall);
    layOut(f);

    prLayoutTreeBeginFrame(f.tree);
    root = prLayoutTreeFrameElement(f.tree, kPrLayoutNodeNone, 0, plain);
    f.root = root;
    PrLayoutNode made = prLayoutTreeFrameElement(f.tree, root, 0, plain);
    CHECK(made == node);
    PrLayoutNode added = prLayoutTreeFrameElement(f.tree, root, 0, tall);
    CHECK(prLayoutTreeIsInFrame(f.tree));
    CHECK(f.bounds(node).bottom == 50.0f);
    CHECK(prLayoutNodeStyle(f.tree, node) == tall);
    CHECK(prLayoutNodeParent(f.tree, added) == kPrLayoutNodeNone);
    CHECK(f.hit(PrVec2{10, 10}) == node);
    layOut(f);
    CHECK_FALSE(prLayoutTreeIsInFrame(f.tree));
    CHECK(f.bounds(node).bottom == 0.0f);
    CHECK(f.bounds(added).bottom == 50.0f);
    CHECK(verified(f.tree));
}

TEST_CASE("a node of another kind at a place is made new, and the one there is gone") {
    Fixture f;
    uint32_t box = f.style(block());
    auto declare = [&](bool text) {
        prLayoutTreeBeginFrame(f.tree);
        PrLayoutNode root = prLayoutTreeFrameElement(f.tree, kPrLayoutNodeNone, 0, box);
        f.root = root;
        return text ? prLayoutTreeFrameText(f.tree, root, 0, "text") : prLayoutTreeFrameElement(f.tree, root, 0, box);
    };
    PrLayoutNode text = declare(true);
    layOut(f);
    PrLayoutNode element = declare(false);
    layOut(f);
    CHECK(element != text);
    CHECK_FALSE(prLayoutNodeIsValid(f.tree, text));
    CHECK(shape(f.tree) == "[[]]");
    CHECK(verified(f.tree));
}

TEST_CASE("a text changed in a frame lays out its own line") {
    Fixture f;
    uint32_t box = f.style(block());
    auto declare = [&](const char* middle) {
        prLayoutTreeBeginFrame(f.tree);
        PrLayoutNode root = prLayoutTreeFrameElement(f.tree, kPrLayoutNodeNone, 0, box);
        f.root = root;
        const char* const texts[] = {"a", middle, "c"};
        for (const char* text : texts)
            prLayoutTreeFrameText(f.tree, prLayoutTreeFrameElement(f.tree, root, 0, box), 0, text);
    };
    declare("b");
    layOut(f);
    declare("changed");
    layOut(f);
    PrLayoutTreeStats stats = prLayoutTreeStats(f.tree);
    CHECK(stats.frameNodesMade == 0);
    CHECK(stats.linesLaidOut == 1);
    CHECK(shape(f.tree) == "[[a] [changed] [c]]");
    CHECK(verified(f.tree));
}

TEST_CASE("a frame's roots replace the committed roots, and a frame with none empties the tree") {
    Fixture f;
    uint32_t box = f.style(block());
    prLayoutTreeBeginFrame(f.tree);
    PrLayoutNode root = prLayoutTreeFrameElement(f.tree, kPrLayoutNodeNone, 0, box);
    f.root = root;
    prLayoutTreeFrameText(f.tree, root, 0, "one");
    layOut(f);

    prLayoutTreeBeginFrame(f.tree);
    PrLayoutNode other = prLayoutTreeFrameElement(f.tree, kPrLayoutNodeNone, 5, box);
    f.root = other;
    prLayoutTreeFrameText(f.tree, other, 0, "two");
    layOut(f);
    CHECK(other != root);
    CHECK_FALSE(prLayoutNodeIsValid(f.tree, root));
    CHECK(shape(f.tree) == "[two]");
    CHECK(verified(f.tree));

    prLayoutTreeBeginFrame(f.tree);
    f.root = kPrLayoutNodeNone;
    layOut(f);
    CHECK(prLayoutTreeRoot(f.tree) == kPrLayoutNodeNone);
    CHECK_FALSE(prLayoutNodeIsValid(f.tree, other));
}

TEST_CASE("a frame and the retained tree it ends in lay out alike") {
    Fixture framed;
    const Item frameItems[] = {{1, "one"}, {2, "two"}, {0, "three"}};
    ListFrame frame = declareList(framed, frameItems);
    layOut(framed);
    const Item rotated[] = {{2, "two"}, {0, "three"}, {1, "one"}};
    declareList(framed, rotated);
    layOut(framed);

    Fixture retained;
    uint32_t box = retained.style(block());
    PrLayoutNode root = retained.element(kPrLayoutNodeNone, block());
    retained.text(root, "heading");
    PrLayoutNode list = prLayoutNodeCreateElement(retained.tree, root, box);
    Array<PrLayoutNode> rows;
    const char* const texts[] = {"two", "three", "one"};
    for (const char* text : texts) {
        PrLayoutNode row = prLayoutNodeCreateElement(retained.tree, list, box);
        retained.text(row, text);
        rows.push(row);
    }
    layOut(retained);
    CHECK(shape(framed.tree) == shape(retained.tree));
    PrLayoutNode framedRow = prLayoutNodeFirstChild(framed.tree, frame.list);
    for (PrLayoutNode row : rows) {
        PrRect a = framed.bounds(framedRow);
        PrRect b = retained.bounds(row);
        CHECK(a.top == b.top);
        CHECK(a.bottom == b.bottom);
        CHECK(a.right == b.right);
        framedRow = prLayoutNodeNextSibling(framed.tree, framedRow);
    }
    CHECK(verified(framed.tree));
}

TEST_CASE("a run of a stacking list that a cached part replays paints in its own container, whatever the last run ended in") {
    // Two clipping bands of positioned boxes, enough of them that the root's stacking list paints in runs. A box that a
    // later frame inserts at the front of the first band moves the runs' boundaries, so that cached runs are replayed
    // after runs that ended in the other band.
    Fixture f;
    constexpr uint32_t kBoxes = 24;
    constexpr uint32_t kWidth = kBoxes * 10;
    PrLayoutBoxStyle bandBox = block(10);
    bandBox.position = PrLayoutPosition_Relative;
    bandBox.overflowX = bandBox.overflowY = PrLayoutOverflow_Clip;
    uint32_t bandStyle = f.style(bandBox);
    uint32_t rootStyle = f.style(block());
    auto box = [&](float left, PrColor color) {
        PrLayoutBoxStyle b = block(10);
        b.position = PrLayoutPosition_Absolute;
        b.inset[PrRectSide_Left] = prLayoutLengthPx(left);
        b.inset[PrRectSide_Top] = prLayoutLengthPx(0);
        b.width = prLayoutLengthPx(10);
        PrLayoutDecorationStyle fill = background(color);
        return f.style(b, &fill);
    };
    // The first frame names no red box, so the record is kept by a reference of its own.
    uint32_t red = box(0, PrColor{1, 0, 0, 1});
    prLayoutStyleRetain(f.tree, red);
    auto declare = [&](uint32_t inserted) {
        prLayoutTreeBeginFrame(f.tree);
        PrLayoutNode root = prLayoutTreeFrameElement(f.tree, kPrLayoutNodeNone, 0, rootStyle);
        f.root = root;
        for (uint32_t band = 0; band < 2; ++band) {
            PrLayoutNode node = prLayoutTreeFrameElement(f.tree, root, 0, bandStyle);
            if (band == 0) {
                for (uint32_t i = 0; i < inserted; ++i)
                    prLayoutTreeFrameElement(f.tree, node, 0, red);
            }
            for (uint32_t i = 0; i < kBoxes; ++i) {
                PrColor color = band == 0 ? PrColor{0, 0, 1, 1} : PrColor{0, 1, 0, 1};
                prLayoutTreeFrameElement(f.tree, node, 0, box(float(i) * 10, color));
            }
        }
    };
    Array<Array<uint8_t>> renders;
    const uint32_t insertions[] = {0u, 1u, 3u, 0u};
    for (uint32_t inserted : insertions) {
        declare(inserted);
        f.layout(px(kWidth, 20));
        renders.push(render(f.tree, kWidth, 20));
        CHECK(verified(f.tree));
    }
    // The second band is green throughout, whatever the first band holds.
    for (const Array<uint8_t>& pixels : renders) {
        for (uint32_t x = 5; x < kWidth; x += 10)
            CHECK(pixel(pixels, kWidth, x, 15) == "0 255 0 255");
    }
    CHECK(renders[3] == renders[0]);
}
