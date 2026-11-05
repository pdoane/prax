#include "pr/draw_commands.h"
#include "pr/draw_list.h"
#include "pr/draw_structure.h"
#include "pr/draw_writer.h"

#include "tools/harness/test.h"

#include <string>

namespace pr_impl {

namespace {

std::string text(PrRawString raw) {
    PrStringView view = prRawStringView(&raw);
    std::string result(view.data != nullptr ? view.data : "", view.size);
    prRawStringReset(&raw);
    return result;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("writer backends are named as a command line spells them") {
    for (PrDrawWriterBackend backend = 0; backend < PrDrawWriterBackend_Count; ++backend)
        CHECK(prDrawWriterBackendFromName(prDrawWriterBackendName(backend)) == backend);
    CHECK(prDrawWriterBackendFromName("SVG") == PrDrawWriterBackend_Svg);
    CHECK(prDrawWriterBackendFromName("postscript") == PrDrawWriterBackend_Count);
    bool any =
        prDrawWriterBackendIsBuilt(PrDrawWriterBackend_Svg) || prDrawWriterBackendIsBuilt(PrDrawWriterBackend_Pdf);
    CHECK(prDrawWriterBackendIsBuilt(PrDrawWriterBackend_Default) == any);
    PrDrawWriter* writer = nullptr;
    PrStatus status = prDrawWriterCreate(PrDrawWriterBackend_Default, nullptr, &writer);
    CHECK((status == PrStatus_Ok) == any);
    CHECK((writer != nullptr) == any);
    prDrawWriterDestroy(writer);
}

TEST_CASE("marks nest with the other pushes and carry their keys") {
    PrDrawList* list = prDrawListCreate();
    prDrawListPushMark(list, 7);
    prDrawListPushTransformAffine2(list, prAffine2Translate(PrVec2{1, 2}));
    prDrawListRect(list, PrRect{0, 0, 4, 4}, {.color = PrColor{1, 0, 0, 1}});
    prDrawListPopTransform(list);
    CHECK(prDrawListDepth(list) == 1);
    prDrawListPopMark(list);
    CHECK(prDrawListDepth(list) == 0);
    CHECK(text(prDrawListDump(list)) == "push-mark 7\n"
        "push-transform-affine 1 0 0 1 1 2\n"
        "rect 0 0 4 4 color 1 0 0 1\n"
        "pop-transform\n"
        "pop-mark\n");
    // Marks cover nothing.
    PrDrawList* empty = prDrawListCreate();
    prDrawListPushMark(empty, 1);
    prDrawListPopMark(empty);
    CHECK(prRectIsEmpty(prDrawListBounds(empty)));
    prDrawListRelease(empty);
    prDrawListRelease(list);
}

TEST_CASE("commands of any list are bounded as a list of them is") {
    PrDrawList* source = prDrawListCreate();
    prDrawListPushTransformAffine2(source, prAffine2Translate(PrVec2{40, 0}));
    prDrawListRect(source, PrRect{0, 0, 10, 10}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPushClipRect(source, PrRect{0, 0, 4, 20}, PrDrawEdges_Snapped);
    prDrawListRect(source, PrRect{2, 2, 6, 6}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopClip(source);
    prDrawListPopTransform(source);
    PrDrawList* alone = prDrawListCreate();
    prDrawListPushClipRect(alone, PrRect{0, 0, 4, 20}, PrDrawEdges_Snapped);
    prDrawListRect(alone, PrRect{2, 2, 6, 6}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopClip(alone);
    PrRect part = prDrawCommandsBounds(prDrawListCommands(source) + 2, 3);
    PrRect whole = prDrawListBounds(alone);
    CHECK(part.left == whole.left);
    CHECK(part.top == whole.top);
    CHECK(part.right == whole.right);
    CHECK(part.bottom == whole.bottom);
    CHECK(prRectIsEmpty(prDrawCommandsBounds(nullptr, 0)));
    prDrawListRelease(source);
    prDrawListRelease(alone);
}

TEST_CASE("a structure holds its nodes and keys in reading order") {
    PrDrawStructureNodeDesc root = prDrawStructureNodeDescDefault(PrDrawStructureType_Document);
    root.language = "en";
    PrDrawStructure* structure = prDrawStructureCreate(&root);
    PrDrawStructureNodeDesc heading = prDrawStructureNodeDescDefault(PrDrawStructureType_H1);
    heading.title = "Intro";
    PrDrawStructureNode h1 = prDrawStructureAppendNode(structure, kPrDrawStructureRoot, &heading);
    prDrawStructureAppendKey(structure, h1, 10);
    PrDrawStructureNodeDesc paragraph = prDrawStructureNodeDescDefault(PrDrawStructureType_P);
    PrDrawStructureNode p = prDrawStructureAppendNode(structure, kPrDrawStructureRoot, &paragraph);
    prDrawStructureAppendKey(structure, p, 11);
    PrDrawStructureNodeDesc link = prDrawStructureNodeDescDefault(PrDrawStructureType_Link);
    link.uri = "https://example.com/";
    PrDrawStructureNode a = prDrawStructureAppendNode(structure, p, &link);
    prDrawStructureAppendKey(structure, a, 12);
    prDrawStructureAppendKey(structure, p, 13);
    PrDrawStructureNodeDesc figure = prDrawStructureNodeDescDefault(PrDrawStructureType_Figure);
    figure.alt = "A chart";
    prDrawStructureAppendNode(structure, p, &figure);
    CHECK(prDrawStructureNodeCount(structure) == 5);
    CHECK(prDrawStructureNodeParent(structure, a) == p);
    CHECK(prDrawStructureNodeParent(structure, kPrDrawStructureRoot) == kPrDrawStructureNodeNone);
    CHECK(prDrawStructureItemCount(structure, p) == 4);
    CHECK(prDrawStructureNodeDesc(structure, a).uri == "https://example.com/");
    CHECK(prDrawStructureTypeHeadingLevel(PrDrawStructureType_H3) == 3);
    CHECK(prDrawStructureTypeHeadingLevel(PrDrawStructureType_P) == 0);
    CHECK(prDrawStructureTypeIsLegacy(PrDrawStructureType_Code));
    CHECK(!prDrawStructureTypeIsLegacy(PrDrawStructureType_Aside));
    CHECK(text(prDrawStructureDump(structure)) == "Document lang=\"en\"\n"
        "  H1 title=\"Intro\"\n"
        "    key 10\n"
        "  P\n"
        "    key 11\n"
        "    Link uri=\"https://example.com/\"\n"
        "      key 12\n"
        "    key 13\n"
        "    Figure alt=\"A chart\"\n");
    prDrawStructureDestroy(structure);
}
