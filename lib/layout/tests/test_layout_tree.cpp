#include "pr/draw_cpu.h"
#include "pr/draw_image.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/layout_animation.h"
#include "pr/layout_compositor.h"
#include "pr/layout_query.h"
#include "pr/layout_scene.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/layout_worker.h"
#include "pr/memory.h"
#include "pr/text_harfbuzz.h"

#include "layout/src/layout_tree.h"
#include "layout/tests/layout_fixture.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

// Returns a transform that turns a box by `radians` about its y axis, or about its x axis if `aboutX` is set.
PrLayoutTransform turned(float radians, bool aboutX = false) {
    PrLayoutTransformList list{};
    list.count = 1;
    list.functions[0].kind = PrLayoutTransformKind_Rotate;
    list.functions[aboutX ? 0 : 0].values[aboutX ? 0 : 1] = 1;
    list.functions[0].values[3] = radians;
    return prLayoutTransformListTransform(&list);
}

// Returns a translation of a box by `z` pixels along its z axis.
PrLayoutTransform translatedZ(float z) {
    PrLayoutTransformList list{};
    list.count = 1;
    list.functions[0].kind = PrLayoutTransformKind_Translate;
    list.functions[0].values[2] = z;
    return prLayoutTransformListTransform(&list);
}

// Returns a 40px square block that gives its children a perspective of `distance` about its center.
PrLayoutBoxStyle square() {
    PrLayoutBoxStyle box = block(40);
    box.width = prLayoutLengthPx(40);
    return box;
}

PrLayoutEffectStyle perspective(float distance) {
    PrLayoutEffectStyle effects = prLayoutEffectStyleDefault();
    effects.perspective = distance;
    return effects;
}

// Returns a box style with a perspective, which is a containing block for all descendants.
PrLayoutBoxStyle perspectiveBox() {
    PrLayoutBoxStyle box = square();
    box.transformed = true;
    return box;
}

// Returns the number of projected groups in the tree's painting.
uint32_t projectedGroups(PrLayoutTree* tree) {
    PrDrawList* list = prDrawListCreate();
    prLayoutTreePaint(tree, list);
    PrString dump = prDrawListDump(list);
    std::string text(prStringView(&dump).data, prStringView(&dump).size);
    prStringReset(&dump);
    prDrawListRelease(list);
    uint32_t count = 0;
    for (size_t at = text.find(" projection"); at != std::string::npos; at = text.find(" projection", at + 1))
        ++count;
    return count;
}

// A clock that the test steps, and the frames that a compositor presented to it, rendered.
struct Host {
    double now = 0;
    uint32_t presented = 0;
    uint32_t wanted = 0;            // the times the compositor came to want a frame
    std::vector<uint8_t> pixels;
};

double hostNow(void* PR_NULLABLE context) {
    return static_cast<Host*>(context)->now;
}

void hostWantFrame(void* PR_NULLABLE context) {
    ++static_cast<Host*>(context)->wanted;
}

void hostPresent(void* PR_NULLABLE context, PrDrawList* list, PrColor16) {
    Host* host = static_cast<Host*>(context);
    ++host->presented;
    host->pixels.assign(size_t(20) * 20 * 4, 0);
    PrDrawRenderer* renderer = prDrawCpuCreate(nullptr);
    PrPixels into{host->pixels.data(), size_t(20) * 4, {20, 20}};
    prDrawRendererRenderPixels(renderer, list, into, PrPixelFormat_Rgba8);
    prDrawRendererDestroy(renderer);
}

// Commits the tree's scene for `embed`, with `tracks` and the tree's offsets, which include the compositor's offsets up
// to `applied`.
void commitTree(PrLayoutCompositor* compositor, const PrLayoutTree* tree, const PrLayoutScene* scene, uint64_t embed,
                uint32_t applied, const PrLayoutTrack* PR_NULLABLE tracks = nullptr, uint32_t trackCount = 0) {
    std::vector<PrLayoutSceneScroll> scrolls = treeScrolls(tree, scene);
    PrLayoutCommit commit{.embed = embed,
                          .scene = scene,
                          .transform = prAffine2Identity(),
                          .tracks = tracks,
                          .trackCount = trackCount,
                          .scrolls = scrolls.data(),
                          .scrollCount = uint32_t(scrolls.size()),
                          .scrollsApplied = applied};
    prLayoutCompositorCommit(compositor, &commit);
}

// Returns a font file of the checkout.
const PrFont* fontFile(const char* name) {
    std::string path = std::string(PR_FONTS_DIR) + "/" + name;
    const PrFont* font = nullptr;
    REQUIRE(prFontCreateFromFile(path, 0, nullptr, &font) == PrStatus_Ok);
    return font;
}

// Adds a face of the family Web with `key`, loaded with `font`, as a document's font rule would.
void addWeb(Fixture& f, uint64_t key, const PrFont* font) {
    PrFontFaceDescriptor d = prFontFaceDescriptorDefault();
    d.family = "Web";
    prFontSetAddPending(f.fonts, &d, key);
    prFontSetLoaded(f.fonts, key, font);
}

// A block of text in the family Web on lines `lineHeight` px tall, and one in the default family, under a root.
struct WebAndDefault {
    PrLayoutNode web;
    PrLayoutNode webTaller;
    PrLayoutNode plain;
};

WebAndDefault webAndDefault(Fixture& f) {
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    font.font = textFontOf(16, {prFontFamilyFromName("Web")});
    PrLayoutFontStyle taller = font;
    taller.lineHeight = prLayoutLineHeightPx(40);
    WebAndDefault nodes{f.element(root, block(), nullptr, nullptr, &font),
                        f.element(root, block(), nullptr, nullptr, &taller), f.element(root, block())};
    f.text(nodes.web, "Hamburgefonstiv");
    f.text(nodes.webTaller, "Hamburgefonstiv");
    f.text(nodes.plain, "Hamburgefonstiv");
    return nodes;
}

// Returns a gradient layer over `stops`, which must outlive the call that interns it.
PrLayoutBackgroundLayer gradientLayer(const PrLayoutColorStop* stops, uint32_t count) {
    PrLayoutBackgroundLayer layer = prLayoutBackgroundLayerDefault();
    layer.kind = PrLayoutImageKind_Gradient;
    layer.gradient = prLayoutGradientDefault();
    layer.gradient.space = PrColorSpace_Srgb;
    layer.gradient.stops = stops;
    layer.gradient.stopCount = count;
    return layer;
}

PrLayoutColorStop colorStop(PrColor color, PrLayoutLength position = prLayoutLengthAuto()) {
    return PrLayoutColorStop{.position = position, .color = prColor16FromColor(color)};
}

// Returns a box `width` by `height` with the background layers given.
PrLayoutNode backgroundBox(Fixture& f, PrLayoutNode parent, float width, float height,
                           const PrLayoutBackgroundLayer* layers, uint32_t count, PrColor color = PrColor{}) {
    PrLayoutBoxStyle box = block();
    box.width = prLayoutLengthPx(width);
    box.height = prLayoutLengthPx(height);
    PrLayoutDecorationStyle decoration = background(color);
    decoration.backgroundLayers = prLayoutBackgroundIntern(f.tree, layers, count);
    return f.element(parent, box, &decoration);
}

// Returns an absolutely positioned box at `left` and `top`, `width` by `height` px.
PrLayoutBoxStyle placedAt(float left, float top, float width, float height) {
    PrLayoutBoxStyle box = block(height);
    box.position = PrLayoutPosition_Absolute;
    box.inset[PrRectSide_Left] = prLayoutLengthPx(left);
    box.inset[PrRectSide_Top] = prLayoutLengthPx(top);
    box.width = prLayoutLengthPx(width);
    return box;
}

// Renders the tree onto `width` by `height` RGBA pixels, at `ratio` pixels per px.
std::vector<uint8_t> renderAt(PrLayoutTree* tree, uint32_t width, uint32_t height, float ratio) {
    PrDrawList* painted = prDrawListCreate();
    prLayoutTreePaint(tree, painted);
    PrDrawList* list = prDrawListCreate();
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{ratio, ratio}));
    prDrawListEmbed(list, painted, PrDVec2{0, 0});
    prDrawListPopTransform(list);
    std::vector<uint8_t> bytes(size_t(width) * height * 4, 0);
    PrDrawRenderer* renderer = prDrawCpuCreate(nullptr);
    PrPixels into{bytes.data(), size_t(width) * 4, {width, height}};
    REQUIRE(prDrawRendererRenderPixels(renderer, list, into, PrPixelFormat_Rgba8) == PrStatus_Ok);
    prDrawRendererDestroy(renderer);
    prDrawListRelease(list);
    prDrawListRelease(painted);
    return bytes;
}

const char* const kBlack = "0 0 0 255";
const char* const kClear = "0 0 0 0";

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a node erased is named by nothing, even once another takes its place") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode erased = f.text(root, "erased");
    prLayoutTreeErase(f.tree, erased);
    CHECK_FALSE(prLayoutNodeIsValid(f.tree, erased));
    PrLayoutNode made = f.text(root, "made");
    CHECK(made != erased);
    CHECK(prLayoutNodeIsValid(f.tree, made));
    CHECK_FALSE(prLayoutNodeIsValid(f.tree, erased));
    CHECK_FALSE(prLayoutNodeIsValid(f.tree, kPrLayoutNodeNone));
    f.layout(px(100, 100));
    CHECK(prLayoutNodeBoxes(f.tree, erased, PrLayoutCoordinates_Layout, nullptr, 0) == 0);
}

TEST_CASE("a node is found by its key, until the key moves or the node is erased") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode a = f.text(root, "a");
    PrLayoutNode b = f.text(root, "b");
    CHECK(prLayoutNodeFromKey(f.tree, 7) == kPrLayoutNodeNone);
    prLayoutNodeSetKey(f.tree, a, 7);
    prLayoutNodeSetKey(f.tree, b, 8);
    CHECK(prLayoutNodeFromKey(f.tree, 7) == a);
    CHECK(prLayoutNodeFromKey(f.tree, 8) == b);
    CHECK(prLayoutNodeFromKey(f.tree, 0) == kPrLayoutNodeNone);
    // A key the node gives up is free for another.
    prLayoutNodeSetKey(f.tree, a, 0);
    CHECK(prLayoutNodeFromKey(f.tree, 7) == kPrLayoutNodeNone);
    prLayoutNodeSetKey(f.tree, b, 7);
    CHECK(prLayoutNodeFromKey(f.tree, 7) == b);
    CHECK(prLayoutNodeFromKey(f.tree, 8) == kPrLayoutNodeNone);
    prLayoutTreeErase(f.tree, b);
    CHECK(prLayoutNodeFromKey(f.tree, 7) == kPrLayoutNodeNone);
}

TEST_CASE("a tree with no font and no shaper lays out boxes, and atomic inlines on lines of no strut") {
    PrLayoutTreeDesc desc{.font = nullptr, .shaper = nullptr, .targetPixelRatio = 1};
    PrLayoutTree* tree = prLayoutTreeCreate(&desc);
    CHECK(prLayoutTreeDefaultFont(tree) == nullptr);
    auto styled = [&](PrLayoutDisplay display, float width, float height) {
        PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
        box.display = display;
        box.width = width < 0 ? prLayoutLengthAuto() : prLayoutLengthPx(width);
        box.height = height < 0 ? prLayoutLengthAuto() : prLayoutLengthPx(height);
        PrLayoutStyle style = prLayoutStyleDefault();
        style.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
        return prLayoutStyleIntern(tree, &style);
    };
    PrLayoutNode root = prLayoutNodeCreateElement(tree, kPrLayoutNodeNone, styled(PrLayoutDisplay_Block, -1, -1));
    prLayoutNodeCreateElement(tree, root, styled(PrLayoutDisplay_Block, -1, 10));
    PrLayoutNode line = prLayoutNodeCreateElement(tree, root, styled(PrLayoutDisplay_Block, -1, -1));
    PrLayoutNode atomic = prLayoutNodeCreateElement(tree, line, styled(PrLayoutDisplay_InlineBlock, 30, 20));
    PrLayoutNode image = prLayoutNodeCreateReplaced(tree, line, styled(PrLayoutDisplay_Inline, -1, -1));
    prLayoutNodeSetNaturalSize(tree, image, PrLayoutNaturalSize{prLayoutUnitFromPx(40), prLayoutUnitFromPx(15), 0});
    PrLayoutRoot viewport{root, px(200, 100)};
    prLayoutTreeLayout(tree, &viewport, 1);
    auto bounds = [&](PrLayoutNode node) { return prLayoutNodeBounds(tree, node, PrLayoutCoordinates_Layout); };
    // The line is as tall as its tallest atomic inline, on a baseline with no strut, at their bottom margin edges.
    CHECK(prLayoutUnitToPx(bounds(line).top) == 10.0f);
    CHECK(prLayoutUnitToPx(bounds(line).bottom - bounds(line).top) == 20.0f);
    CHECK(prLayoutUnitToPx(bounds(image).bottom) == prLayoutUnitToPx(bounds(atomic).bottom));
    CHECK(prLayoutUnitToPx(bounds(image).left) == 30.0f);
    CHECK(verified(tree));
    prLayoutTreeDestroy(tree);
}

TEST_CASE("a scene's canvas is the root's background over the tree's canvas color, which the application may fill") {
    PrLayoutTreeDesc desc{.font = nullptr, .shaper = nullptr, .targetPixelRatio = 1};
    PrLayoutTree* tree = prLayoutTreeCreate(&desc);
    CHECK(prColorFromColor16(prLayoutTreeCanvasColor(tree)).a == 0.0f);
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_Block;
    box.height = prLayoutLengthPx(10);
    PrLayoutDecorationStyle decoration = prLayoutDecorationStyleDefault();
    decoration.background = prColor16FromColor(PrColor{1, 0, 0, 0.5f});
    PrLayoutStyle style = prLayoutStyleDefault();
    style.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
    style.groups[PrLayoutGroup_Decoration] = prLayoutDecorationStyleIntern(tree, &decoration);
    PrLayoutNode root = prLayoutNodeCreateElement(tree, kPrLayoutNodeNone, prLayoutStyleIntern(tree, &style));
    prLayoutTreeSetCanvasColor(tree, prColor16FromColor(PrColor{0, 0, 1, 1}));
    PrLayoutRoot viewport{root, px(20, 20)};
    prLayoutTreeLayout(tree, &viewport, 1);
    const PrLayoutScene* scene = prLayoutTreeScene(tree);
    // Half red over opaque blue, blended on encoded values.
    PrColor canvas = prColorFromColor16(prLayoutSceneCanvasColor(scene));
    CHECK(canvas.r == doctest::Approx(0.5).epsilon(0.01));
    CHECK(canvas.g == 0.0f);
    CHECK(canvas.b == doctest::Approx(0.5).epsilon(0.01));
    CHECK(canvas.a == 1.0f);
    // The canvas covers the viewport past the root's box. An application that filled it gets no fill from the player.
    auto pixel = [&](bool filled) {
        PrDrawList* list = prDrawListCreate();
        PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
        options.canvasFilled = filled;
        prLayoutScenePlay(scene, list, &options);
        std::vector<uint8_t> bytes(size_t(20) * 20 * 4, 0);
        PrDrawRenderer* renderer = prDrawCpuCreate(nullptr);
        PrPixels into{bytes.data(), size_t(20) * 4, {20, 20}};
        REQUIRE(prDrawRendererRenderPixels(renderer, list, into, PrPixelFormat_Rgba8) == PrStatus_Ok);
        prDrawRendererDestroy(renderer);
        prDrawListRelease(list);
        const uint8_t* p = &bytes[(size_t(15) * 20 + 10) * 4];
        return std::vector<uint8_t>{p[0], p[1], p[2], p[3]};
    };
    std::vector<uint8_t> played = pixel(false);
    CHECK(played[3] == 255);
    CHECK(played[0] > 120);
    CHECK(played[2] > 120);
    CHECK(pixel(true)[3] == 0);
    prLayoutSceneRelease(scene);
    // A new canvas color makes a new scene.
    prLayoutTreeSetCanvasColor(tree, prColor16FromColor(PrColor{0, 0, 0, 0}));
    CHECK_FALSE(prLayoutTreeSceneIsCurrent(tree));
    scene = prLayoutTreeScene(tree);
    CHECK(prColorFromColor16(prLayoutSceneCanvasColor(scene)).a == doctest::Approx(0.5).epsilon(0.01));
    prLayoutSceneRelease(scene);
    prLayoutTreeDestroy(tree);
}

TEST_CASE("block boxes stack in the block axis and fill the inline axis") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode a = f.element(root, block(50));
    PrLayoutNode b = f.element(root, block(30));
    f.layout(px(200, 100));
    checkRect(f.bounds(root), 0, 0, 200, 80);
    checkRect(f.bounds(a), 0, 0, 200, 50);
    checkRect(f.bounds(b), 0, 50, 200, 80);
}

TEST_CASE("a given width leaves auto margins to share what is left") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle centered = block(10);
    centered.width = prLayoutLengthPx(100);
    centered.margin[PrRectSide_Left] = prLayoutLengthAuto();
    centered.margin[PrRectSide_Right] = prLayoutLengthAuto();
    PrLayoutNode c = f.element(root, centered);
    PrLayoutBoxStyle right = block(10);
    right.width = prLayoutLengthPercent(25);
    right.margin[PrRectSide_Left] = prLayoutLengthAuto();
    PrLayoutNode r = f.element(root, right);
    f.layout(px(400, 100));
    checkRect(f.bounds(c), 150, 0, 250, 10);
    checkRect(f.bounds(r), 300, 10, 400, 20);
}

TEST_CASE("box-sizing, padding, borders and limits size the border box") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle content = block(10);
    content.width = prLayoutLengthPx(100);
    content.padding[PrRectSide_Left] = prLayoutLengthPx(5);
    content.border[PrRectSide_Right] = px(3, 0).x;
    PrLayoutNode a = f.element(root, content);
    PrLayoutBoxStyle border = content;
    border.boxSizing = PrLayoutBoxSizing_BorderBox;
    PrLayoutNode b = f.element(root, border);
    PrLayoutBoxStyle limited = block(10);
    limited.maxWidth = prLayoutLengthPx(50);
    PrLayoutNode c = f.element(root, limited);
    PrLayoutBoxStyle least = block();
    least.minHeight = prLayoutLengthPx(7);
    PrLayoutNode d = f.element(root, least);
    f.layout(px(400, 100));
    checkRect(f.bounds(a), 0, 0, 108, 10);
    checkRect(f.bounds(b), 0, 10, 100, 20);
    checkRect(f.bounds(c), 0, 20, 50, 30);
    checkRect(f.bounds(d), 0, 30, 400, 37);
}

TEST_CASE("stretch fills the containing block less the margins, and behaves as auto where that is indefinite") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(100));
    PrLayoutBoxStyle filled = block();
    filled.width = prLayoutLengthStretch();
    filled.height = prLayoutLengthStretch();
    filled.margin[PrRectSide_Left] = prLayoutLengthPx(10);
    filled.margin[PrRectSide_Right] = prLayoutLengthAuto();
    setMargins(filled, 5, 15);
    PrLayoutNode a = f.element(root, filled);
    f.layout(px(200, 300));
    // Auto margins count as zero, and the root's definite height is the space in the block axis.
    checkRect(f.bounds(a), 10, 5, 200, 85);

    // Under a parent whose height depends on its content, a stretch height is the content's height.
    PrLayoutNode parent = f.element(root, block());
    PrLayoutNode child = f.element(parent, filled);
    f.element(child, block(30));
    f.layout(px(200, 300));
    CHECK(f.bounds(child).bottom - f.bounds(child).top == 30.0f);

    // As a limit, stretch is the stretch-fit size.
    PrLayoutBoxStyle limited = block();
    limited.width = prLayoutLengthPx(500);
    limited.maxWidth = prLayoutLengthStretch();
    limited.height = prLayoutLengthPx(10);
    limited.minHeight = prLayoutLengthStretch();
    PrLayoutNode c = f.element(root, limited);
    f.layout(px(200, 300));
    CHECK(f.bounds(c).right - f.bounds(c).left == 200.0f);
    CHECK(f.bounds(c).bottom - f.bounds(c).top == 100.0f);
    CHECK(verified(f.tree));
}

TEST_CASE("adjoining margins collapse") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle first = block(10);
    setMargins(first, 0, 20);
    PrLayoutNode a = f.element(root, first);
    PrLayoutBoxStyle second = block(10);
    setMargins(second, 30, 0);
    PrLayoutNode b = f.element(root, second);

    // A parent with no border or padding shares its first child's margin, and an empty box's margins collapse through
    // it.
    PrLayoutBoxStyle parent = block();
    setMargins(parent, 5, 0);
    PrLayoutNode p = f.element(root, parent);
    PrLayoutBoxStyle empty = block();
    setMargins(empty, 15, 25);
    f.element(p, empty);
    PrLayoutBoxStyle child = block(10);
    setMargins(child, 10, 0);
    PrLayoutNode c = f.element(p, child);

    // A negative margin adds to the largest positive one it adjoins.
    PrLayoutBoxStyle negative = block(10);
    setMargins(negative, -10, 0);
    PrLayoutNode n = f.element(root, negative);
    f.layout(px(100, 100));
    checkRect(f.bounds(a), 0, 0, 100, 10);
    checkRect(f.bounds(b), 0, 40, 100, 50);
    checkRect(f.bounds(p), 0, 75, 100, 85);
    checkRect(f.bounds(c), 0, 75, 100, 85);
    checkRect(f.bounds(n), 0, 75, 100, 85);
}

TEST_CASE("a scroll container keeps its children's margins") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle clip = block();
    clip.overflowY = PrLayoutOverflow_Hidden;
    PrLayoutNode p = f.element(root, clip);
    PrLayoutBoxStyle child = block(10);
    setMargins(child, 10, 10);
    PrLayoutNode c = f.element(p, child);
    f.layout(px(100, 100));
    checkRect(f.bounds(p), 0, 0, 100, 30);
    checkRect(f.bounds(c), 0, 10, 100, 20);
}

TEST_CASE("overflow clip clips in its own axis, without scrolling, and passes margins through") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle clip = block(20);
    clip.width = prLayoutLengthPx(20);
    clip.overflowY = PrLayoutOverflow_Clip;
    PrLayoutNode box = f.element(root, clip);
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    PrLayoutBoxStyle child = block(40);
    child.width = prLayoutLengthPx(40);
    setMargins(child, 5, 0);
    PrLayoutNode inner = f.element(box, child, &red);
    f.layout(px(60, 60));
    CHECK(verified(f.tree));
    // The child's margin collapses through the box, which establishes no formatting context.
    checkRect(f.bounds(box), 0, 5, 20, 25);
    std::vector<uint8_t> pixels = render(f.tree, 60, 60);
    CHECK(pixel(pixels, 60, 30, 10) == "255 0 0 255");     // past it in the axis it does not clip
    CHECK(pixel(pixels, 60, 10, 30) == "0 0 0 0");         // past it in the axis it clips
    CHECK(f.hit(PrVec2{30, 10}) == inner);
    CHECK(f.hit(PrVec2{10, 30}) != inner);
    PrLayoutScrollInfo info{};
    REQUIRE(prLayoutNodeScrollInfo(f.tree, box, &info));
    CHECK(!info.scrollContainer);
}

TEST_CASE("a box that only clips scrolls by no offset, the tree's or a player's, and is no scroll container") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle scroller = block(20);
    scroller.width = prLayoutLengthPx(20);
    scroller.overflowX = PrLayoutOverflow_Hidden;
    scroller.overflowY = PrLayoutOverflow_Hidden;
    PrLayoutNode box = f.element(root, scroller);
    PrLayoutDecorationStyle green = background(PrColor{0, 1, 0, 1});
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    PrLayoutNode top = f.element(box, block(20), &green);
    PrLayoutNode below = f.element(box, block(40), &red);
    f.layout(px(20, 40));

    // Scrolled while it is a scroll container, its content moves.
    prLayoutNodeSetScroll(f.tree, box, px(0, 20));
    CHECK(pixel(render(f.tree, 20, 40), 20, 10, 10) == "255 0 0 255");
    CHECK(f.hit(PrVec2{10, 10}) == below);

    // Once it only clips, the offset the tree keeps for it moves nothing, in painting or in hit testing.
    PrLayoutBoxStyle clip = scroller;
    clip.overflowX = PrLayoutOverflow_Clip;
    clip.overflowY = PrLayoutOverflow_Clip;
    prLayoutNodeSetStyle(f.tree, box, f.style(clip));
    f.layout(px(20, 40));
    std::vector<uint8_t> pixels = render(f.tree, 20, 40);
    CHECK(pixel(pixels, 20, 10, 10) == "0 255 0 255");
    CHECK(pixel(pixels, 20, 10, 30) == "0 0 0 0");
    CHECK(f.hit(PrVec2{10, 10}) == top);
    CHECK(f.hit(PrVec2{10, 30}) != below);
    CHECK(f.bounds(top).top == 0.0f);
    CHECK(verified(f.tree));

    // A new offset moves nothing either, and the scene names no scroll container for it.
    prLayoutNodeSetScroll(f.tree, box, px(0, 20));
    CHECK(pixel(render(f.tree, 20, 40), 20, 10, 10) == "0 255 0 255");
    PrLayoutScrollInfo info{};
    REQUIRE(prLayoutNodeScrollInfo(f.tree, box, &info));
    CHECK(!info.scrollContainer);
    const PrLayoutScene* scene = prLayoutTreeScene(f.tree);
    CHECK(!prLayoutSceneHasScroll(scene, box));

    // A player's offset for it moves nothing.
    PrLayoutSceneScroll offset{box, px(0, 20)};
    PrDrawList* list = prDrawListCreate();
    PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
    options.scrolls = &offset;
    options.scrollCount = 1;
    prLayoutScenePlay(scene, list, &options);
    std::vector<uint8_t> played(size_t(20) * 40 * 4, 0);
    PrDrawRenderer* renderer = prDrawCpuCreate(nullptr);
    PrPixels into{played.data(), size_t(20) * 4, {20, 40}};
    REQUIRE(prDrawRendererRenderPixels(renderer, list, into, PrPixelFormat_Rgba8) == PrStatus_Ok);
    prDrawRendererDestroy(renderer);
    prDrawListRelease(list);
    prLayoutSceneRelease(scene);
    CHECK(pixel(played, 20, 10, 10) == "0 255 0 255");
    CHECK(pixel(played, 20, 10, 30) == "0 0 0 0");
}

TEST_CASE("a scroll container clips both axes, whatever overflow it has in the other") {
    for (bool vertical : {true, false}) {
        CAPTURE(vertical);
        Fixture f;
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
        PrLayoutBoxStyle parent = block(20);
        parent.width = prLayoutLengthPx(20);
        if (vertical)
            parent.overflowY = PrLayoutOverflow_Hidden;
        else
            parent.overflowX = PrLayoutOverflow_Hidden;
        PrLayoutNode box = f.element(root, parent);
        PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
        PrLayoutBoxStyle child = block(40);
        child.width = prLayoutLengthPx(40);
        PrLayoutNode inner = f.element(box, child, &red);
        f.layout(px(60, 60));
        CHECK(verified(f.tree));
        std::vector<uint8_t> pixels = render(f.tree, 60, 60);
        CHECK(pixel(pixels, 60, 30, 10) == "0 0 0 0");
        CHECK(pixel(pixels, 60, 10, 30) == "0 0 0 0");
        CHECK(pixel(pixels, 60, 10, 10) == "255 0 0 255");
        CHECK(f.hit(PrVec2{30, 10}) != inner);
        CHECK(f.hit(PrVec2{10, 30}) != inner);
        CHECK(f.hit(PrVec2{10, 10}) == inner);
    }
}

TEST_CASE("text in a tree with no fonts is set in no face, and takes no room") {
    PrLayoutTreeDesc desc{.targetPixelRatio = 1};
    PrLayoutTree* tree = prLayoutTreeCreate(&desc);
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_Block;
    PrLayoutStyle record = prLayoutStyleDefault();
    record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
    PrLayoutNode root = prLayoutNodeCreateElement(tree, kPrLayoutNodeNone, prLayoutStyleIntern(tree, &record));
    PrLayoutNode text = prLayoutNodeCreateText(tree, root, "no face sets this");
    PrLayoutRoot viewport{root, px(200, 100)};
    prLayoutTreeLayout(tree, &viewport, 1);
    CHECK(verified(tree));
    PrLayoutRect bounds = prLayoutNodeBounds(tree, text, PrLayoutCoordinates_Layout);
    CHECK(bounds.right.raw - bounds.left.raw == 0);
    prLayoutTreeDestroy(tree);
}

TEST_CASE("a box shows where the boxes that clip it and the viewport leave it, and not when scrolled away") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    // A scroll container 40 high holds children 30 high: the first shows, the second shows in part, and the fourth not
    // at all, until the container scrolls to it.
    PrLayoutBoxStyle port = block(40);
    port.width = prLayoutLengthPx(50);
    port.overflowY = PrLayoutOverflow_Scroll;
    PrLayoutNode view = f.element(root, port);
    PrLayoutNode rows[4];
    for (PrLayoutNode& row : rows)
        row = f.element(view, block(30));
    // A box past the viewport's bottom does not show.
    f.element(root, block(30));
    PrLayoutNode below = f.element(root, block(30));
    f.layout(px(60, 60));
    CHECK(verified(f.tree));
    auto shown = [&](PrLayoutNode node) { return prLayoutRectToPx(prLayoutNodeShownBounds(f.tree, node)); };
    CHECK(shown(rows[0]).bottom == 30.0f);
    CHECK(shown(rows[1]).top == 30.0f);
    CHECK(shown(rows[1]).bottom == 40.0f);
    CHECK(shown(rows[3]).bottom == 0.0f);
    CHECK(shown(below).bottom == 0.0f);
    prLayoutNodeSetScroll(f.tree, view, PrLayoutVec{prLayoutUnitFromPx(0), prLayoutUnitFromPx(80)});
    f.layout(px(60, 60));
    CHECK(shown(rows[0]).bottom == 0.0f);
    CHECK(shown(rows[3]).top == 10.0f);
    CHECK(shown(rows[3]).bottom == 40.0f);
}

TEST_CASE("a change to what a box paints inside an inline box with effects records its block again") {
    // An inline box paints on its lines, whatever its effects, and so does every box inside it, which inline layout
    // also treats as an inline box. Neither has a layer of its own.
    for (bool nested : {false, true}) {
        CAPTURE(nested);
        Fixture f;
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
        PrLayoutNode paragraph = f.element(root, block());
        PrLayoutEffectStyle faded = prLayoutEffectStyleDefault();
        faded.opacity = 0.5f;
        PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
        PrLayoutDecorationStyle green = background(PrColor{0, 1, 0, 1});
        PrLayoutNode changed = kPrLayoutNodeNone;
        if (nested) {
            PrLayoutNode span = f.element(paragraph, inlineBox());
            PrLayoutNode layered = f.element(span, block(), nullptr, &faded);
            changed = f.element(layered, block(10), &red);
        } else {
            changed = f.element(paragraph, inlineBox(), &red, &faded);
            f.text(changed, "text");
        }
        f.layout(px(100, 100));
        prLayoutSceneRelease(prLayoutTreeScene(f.tree));
        uint32_t restyled = nested ? f.style(block(10), &green) : f.style(inlineBox(), &green, &faded);
        prLayoutNodeSetStyle(f.tree, changed, restyled);
        f.layout(px(100, 100));
        prLayoutSceneRelease(prLayoutTreeScene(f.tree));
        CHECK(verified(f.tree));
    }
}

TEST_CASE("display none generates no box, and relative positioning moves one") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle none = block(10);
    none.display = PrLayoutDisplay_None;
    PrLayoutNode gone = f.element(root, none);
    PrLayoutBoxStyle moved = block(10);
    moved.position = PrLayoutPosition_Relative;
    moved.inset[PrRectSide_Left] = prLayoutLengthPx(5);
    moved.inset[PrRectSide_Bottom] = prLayoutLengthPx(3);
    PrLayoutNode m = f.element(root, moved);
    PrLayoutNode after = f.element(root, block(10));
    f.layout(px(100, 100));
    CHECK(prLayoutNodeBoxes(f.tree, gone, PrLayoutCoordinates_Viewport, nullptr, 0) == 0);
    checkRect(f.bounds(m), 5, -3, 105, 7);
    checkRect(f.bounds(after), 0, 10, 100, 20);
}

TEST_CASE("text is set on lines of the line height, and breaks between words") {
    Fixture f;
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    font.font = textFontOf(20);
    font.lineHeight = prLayoutLineHeightPx(30);
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font);
    PrLayoutNode text = f.text(root, "  hello   world  ");
    f.layout(px(1000, 100));
    PrRect one = f.bounds(text);
    CHECK(double(f.bounds(root).bottom) == doctest::Approx(30.0));
    CHECK(double(one.left) == doctest::Approx(0.0));
    float width = one.right - one.left;
    CHECK(width > 80.0f);
    CHECK(width < 160.0f);

    // Narrower than the two words, the text takes two lines.
    f.layout(px(width * 0.75f, 100));
    CHECK(double(f.bounds(root).bottom) == doctest::Approx(60.0));
    PrLayoutRect lines[4];
    CHECK(prLayoutNodeBoxes(f.tree, text, PrLayoutCoordinates_Viewport, lines, 4) == 2);
    CHECK(prLayoutUnitToPx(lines[0].left) == 0.0f);
    CHECK(prLayoutUnitToPx(lines[1].left) == 0.0f);
    CHECK(prLayoutUnitToPx(lines[1].top) > prLayoutUnitToPx(lines[0].top) + 25.0f);
}

TEST_CASE("a line height below the font's content area makes lines that tall") {
    Fixture f;
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    font.font = textFontOf(20);
    font.lineHeight = prLayoutLineHeightPx(4);
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font);
    f.text(root, "short");
    f.layout(px(1000, 100));
    // The strut's leading is negative, and one of its sides reaches less than nothing past the baseline.
    CHECK(double(f.bounds(root).bottom) == doctest::Approx(4.0));
    CHECK(verified(f.tree));
}

TEST_CASE("inline-level siblings of block boxes are wrapped in anonymous blocks") {
    Fixture f;
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    font.lineHeight = prLayoutLineHeightPx(20);
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font);
    PrLayoutNode before = f.text(root, "before");
    PrLayoutNode middle = f.element(root, block(10));
    f.text(root, "   ");
    PrLayoutNode last = f.element(root, block(10));
    f.layout(px(300, 100));
    CHECK(f.bounds(before).top >= 0.0f);
    checkRect(f.bounds(middle), 0, 20, 300, 30);
    // White space alone makes no line.
    checkRect(f.bounds(last), 0, 30, 300, 40);
}

TEST_CASE("an inline box's edges take room on the line") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode plain = f.text(root, "ab");
    PrLayoutBoxStyle padded = inlineBox();
    padded.padding[PrRectSide_Left] = prLayoutLengthPx(10);
    padded.margin[PrRectSide_Left] = prLayoutLengthPx(4);
    padded.border[PrRectSide_Right] = px(2, 0).x;
    PrLayoutNode span = f.element(root, padded);
    PrLayoutNode inside = f.text(span, "cd");
    f.layout(px(300, 100));
    PrRect a = f.bounds(plain);
    PrRect s = f.bounds(span);
    PrRect i = f.bounds(inside);
    CHECK(double(s.left) == doctest::Approx(double(a.right + 4)));
    CHECK(double(i.left) == doctest::Approx(double(s.left + 10)));
    CHECK(double(s.right) == doctest::Approx(double(i.right + 2)));
}

TEST_CASE("hit testing finds what is painted on top") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode a = f.element(root, block(50));
    PrLayoutBoxStyle lifted = block(50);
    lifted.position = PrLayoutPosition_Relative;
    lifted.inset[PrRectSide_Top] = prLayoutLengthPx(-25);
    PrLayoutNode b = f.element(root, lifted);
    PrLayoutNode c = f.element(root, block(50));
    f.layout(px(100, 200));
    CHECK(f.hit(PrVec2{10, 10}) == a);
    // The positioned box paints over the box it overlaps, which follows it in the tree.
    CHECK(f.hit(PrVec2{10, 30}) == b);
    CHECK(f.hit(PrVec2{10, 90}) == root);
    CHECK(f.hit(PrVec2{10, 140}) == c);
    CHECK(f.hit(PrVec2{10, 190}) == kPrLayoutNodeNone);
}

TEST_CASE("a transform moves a box's geometry and hit testing") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle box = block(20);
    box.width = prLayoutLengthPx(20);
    PrLayoutEffectStyle effects = prLayoutEffectStyleDefault();
    effects.transform = prLayoutTransformFromAffine2(prAffine2Scale(PrVec2{2, 2}));
    PrLayoutNode scaled = f.element(root, box, nullptr, &effects);
    f.layout(px(100, 100));
    checkRect(f.bounds(scaled), -10, -10, 30, 30);
    CHECK(f.hit(PrVec2{25, 25}) == scaled);
}

TEST_CASE("a transform resolves its percentages and origin against its reference box") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle box = block(20);
    box.width = prLayoutLengthPx(40);
    PrLayoutTransformList half{};
    half.count = 1;
    half.functions[0].kind = PrLayoutTransformKind_Translate;
    half.functions[0].values[3] = 0.5f;
    PrLayoutEffectStyle effects = prLayoutEffectStyleDefault();
    effects.transform = prLayoutTransformListTransform(&half);
    PrLayoutNode moved = f.element(root, box, nullptr, &effects);
    // With the content box as the reference box, the percentage and the origin are of the content box.
    PrLayoutBoxStyle padded = box;
    padded.padding[PrRectSide_Left] = prLayoutLengthPx(10);
    PrLayoutEffectStyle content = prLayoutEffectStyleDefault();
    content.transformBox = PrLayoutTransformBox_ContentBox;
    content.transform = prLayoutTransformListTransform(&half);
    PrLayoutNode inContent = f.element(root, padded, nullptr, &content);
    content.transform = prLayoutTransformFromAffine2(prAffine2Scale(PrVec2{2, 1}));
    PrLayoutNode scaled = f.element(root, padded, nullptr, &content);
    f.layout(px(100, 100));
    checkRect(f.bounds(moved), 20, 0, 60, 20);
    checkRect(f.bounds(inContent), 20, 20, 70, 40);
    // The content box spans 10 to 50, so the border box scales about 30.
    checkRect(f.bounds(scaled), -30, 40, 70, 60);
}

TEST_CASE("a 3D transform that is affine on the box's plane paints and maps flat") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle box = block(20);
    box.width = prLayoutLengthPx(40);
    // rotateY(60deg) maps the plane affinely: it halves x about the origin.
    PrLayoutEffectStyle effects = prLayoutEffectStyleDefault();
    effects.transform = turned(1.0471976f);
    PrLayoutNode flat = f.element(root, box, nullptr, &effects);
    f.layout(px(100, 100));
    checkRect(f.bounds(flat), 10, 0, 30, 20);
    CHECK(projectedGroups(f.tree) == 0);
}

TEST_CASE("a parent's perspective projects a child's 3D transform, which paints as a projected group") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutEffectStyle near = perspective(40);
    PrLayoutNode parent = f.element(root, perspectiveBox(), nullptr, &near);
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    PrLayoutEffectStyle card = prLayoutEffectStyleDefault();
    card.transform = turned(0.5235988f);
    PrLayoutNode child = f.element(parent, square(), &red, &card);
    f.layout(px(40, 40));
    // At 30deg, the left edge comes toward the eye at w = 0.75 and the right edge goes away at w = 1.25, about the
    // perspective origin at the parent's center.
    PrRect bounds = f.bounds(child);
    CHECK(double(bounds.left) == doctest::Approx(20 - 17.3205 / 0.75).epsilon(0.01));
    CHECK(double(bounds.right) == doctest::Approx(20 + 17.3205 / 1.25).epsilon(0.01));
    CHECK(double(bounds.top) == doctest::Approx(20 - 20 / 0.75).epsilon(0.01));
    CHECK(projectedGroups(f.tree) == 1);
    std::vector<uint8_t> pixels = render(f.tree, 40, 40);
    CHECK(pixel(pixels, 40, 2, 1) == "255 0 0 255");
    CHECK(pixel(pixels, 40, 2, 20) == "255 0 0 255");
    CHECK(pixel(pixels, 40, 36, 20) == "0 0 0 0");
    CHECK(pixel(pixels, 40, 30, 2) == "0 0 0 0");
    // Hit testing follows the projection: the far side is not hit where it no longer reaches.
    CHECK(f.hit(PrVec2{30, 20}) == child);
    CHECK(f.hit(PrVec2{36, 20}) == parent);
    // Without the perspective, the turn is affine and covers x up to 37.3.
    prLayoutNodeSetStyle(f.tree, parent, f.style(perspectiveBox()));
    f.layout(px(40, 40));
    CHECK(projectedGroups(f.tree) == 0);
    CHECK(pixel(render(f.tree, 40, 40), 40, 36, 20) == "255 0 0 255");
}

TEST_CASE("a perspective applies to a box's children, not to their descendants, about its perspective origin") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutEffectStyle near = perspective(40);
    PrLayoutNode parent = f.element(root, perspectiveBox(), nullptr, &near);
    PrLayoutEffectStyle back = prLayoutEffectStyleDefault();
    back.transform = translatedZ(-40);
    // A box 40px behind the plane, at a perspective of 40px, is half its size about the perspective origin.
    PrLayoutNode child = f.element(parent, square(), nullptr, &back);
    PrLayoutNode middle = f.element(parent, square());
    PrLayoutNode grandchild = f.element(middle, square(), nullptr, &back);
    f.layout(px(100, 100));
    checkRect(f.bounds(child), 10, 10, 30, 30);
    checkRect(f.bounds(grandchild), 0, 40, 40, 80);
    // The origin at the parent's top left halves the box toward that corner.
    near.perspectiveOriginX = prLayoutLengthPercent(0);
    near.perspectiveOriginY = prLayoutLengthPercent(0);
    prLayoutNodeSetStyle(f.tree, parent, f.style(perspectiveBox(), nullptr, &near));
    f.layout(px(100, 100));
    checkRect(f.bounds(child), 0, 0, 20, 20);
}

TEST_CASE("a box that shows the back that it hides is neither painted nor hit") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutEffectStyle near = perspective(100);
    PrLayoutNode parent = f.element(root, perspectiveBox(), nullptr, &near);
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    PrLayoutEffectStyle flipped = prLayoutEffectStyleDefault();
    flipped.transform = turned(3.1415927f);
    PrLayoutNode card = f.element(parent, square(), &red, &flipped);
    f.layout(px(40, 40));
    // A visible back face shows the card mirrored.
    CHECK(pixel(render(f.tree, 40, 40), 40, 20, 20) == "255 0 0 255");
    CHECK(f.hit(PrVec2{20, 20}) == card);
    flipped.backfaceHidden = true;
    prLayoutNodeSetStyle(f.tree, card, f.style(square(), &red, &flipped));
    f.layout(px(40, 40));
    CHECK(pixel(render(f.tree, 40, 40), 40, 20, 20) == "0 0 0 0");
    CHECK(f.hit(PrVec2{20, 20}) == parent);
    // A mirror in the plane shows the front, and paints.
    flipped.transform = prLayoutTransformFromAffine2(prAffine2Scale(PrVec2{-1, 1}));
    prLayoutNodeSetStyle(f.tree, card, f.style(square(), &red, &flipped));
    f.layout(px(40, 40));
    CHECK(pixel(render(f.tree, 40, 40), 40, 20, 20) == "255 0 0 255");
    CHECK(f.hit(PrVec2{20, 20}) == card);
}

TEST_CASE("a projected plane inside another flattens into the outer plane") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutEffectStyle near = perspective(40);
    PrLayoutNode parent = f.element(root, perspectiveBox(), nullptr, &near);
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    PrLayoutEffectStyle outer = perspective(40);
    outer.transform = turned(0.5235988f);
    PrLayoutNode card = f.element(parent, perspectiveBox(), &red, &outer);
    PrLayoutDecorationStyle green = background(PrColor{0, 1, 0, 1});
    PrLayoutBoxStyle small = block(20);
    small.width = prLayoutLengthPx(20);
    small.margin[PrRectSide_Left] = prLayoutLengthPx(10);
    small.margin[PrRectSide_Top] = prLayoutLengthPx(10);
    PrLayoutEffectStyle inner = prLayoutEffectStyleDefault();
    inner.transform = turned(0.5235988f, true);
    f.element(card, small, &green, &inner);
    f.layout(px(40, 40));
    CHECK(projectedGroups(f.tree) == 2);
    // The centers of both planes stay at the parent's center, and the outer plane's near side stays red.
    std::vector<uint8_t> pixels = render(f.tree, 40, 40);
    CHECK(pixel(pixels, 40, 20, 20) == "0 255 0 255");
    CHECK(pixel(pixels, 40, 3, 20) == "255 0 0 255");
}

TEST_CASE("a box partly behind the eye has bounds cut where drawing cuts it, and is hit where it lands") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutEffectStyle near = perspective(10);
    PrLayoutNode parent = f.element(root, perspectiveBox(), nullptr, &near);
    // At 45deg and a perspective of 10px, w runs from 1 - 1.414 at the left edge to 1 + 1.414 at the right edge, so the
    // left part of the card is behind the eye.
    PrLayoutEffectStyle card = prLayoutEffectStyleDefault();
    card.transform = turned(0.7853982f);
    PrLayoutNode child = f.element(parent, square(), nullptr, &card);
    f.layout(px(40, 40));
    PrRect bounds = f.bounds(child);
    // The right edge is at 20 + 14.142 / 2.414. The cut toward the eye reaches far, but not to the unbounded rectangle.
    CHECK(double(bounds.right) == doctest::Approx(20 + 14.1421 / 2.4142).epsilon(0.01));
    CHECK(bounds.left < -1000.0f);
    CHECK(bounds.left > -1e6f);
    // The right half of the card lands where it is drawn, and maps back into the card's plane.
    CHECK(f.hit(PrVec2{24, 20}) == child);
    PrLayoutVec local{};
    REQUIRE(prLayoutNodeLocalPoint(f.tree, child, prLayoutVecFromPx(PrVec2{24, 20}), &local));
    // 4px right of the center, x cos(45deg) / (1 + x sin(45deg) / 10) = 4 gives x = 9.43.
    CHECK(double(prLayoutUnitToPx(local.x)) == doctest::Approx(20 + 9.4281).epsilon(0.01));
    CHECK(double(prLayoutUnitToPx(local.y)) == doctest::Approx(20).epsilon(0.01));
    // A card wholly behind the eye has empty bounds, is not hit, and no point reaches its plane.
    card.transform = translatedZ(20);
    prLayoutNodeSetStyle(f.tree, child, f.style(square(), nullptr, &card));
    f.layout(px(40, 40));
    checkRect(f.bounds(child), 0, 0, 0, 0);
    CHECK(f.hit(PrVec2{20, 20}) == parent);
    CHECK(!prLayoutNodeLocalPoint(f.tree, child, prLayoutVecFromPx(PrVec2{20, 20}), &local));
}

TEST_CASE("a viewport point maps into a box's own coordinates through transforms and projections") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutEffectStyle near = perspective(40);
    PrLayoutNode parent = f.element(root, perspectiveBox(), nullptr, &near);
    PrLayoutEffectStyle card = prLayoutEffectStyleDefault();
    card.transform = turned(0.5235988f);
    PrLayoutNode child = f.element(parent, square(), nullptr, &card);
    PrLayoutBoxStyle box = block(20);
    box.width = prLayoutLengthPx(20);
    PrLayoutEffectStyle scaled = prLayoutEffectStyleDefault();
    scaled.transform = prLayoutTransformFromAffine2(prAffine2Scale(PrVec2{2, 2}));
    PrLayoutNode flat = f.element(root, box, nullptr, &scaled);
    f.layout(px(100, 100));
    // The point 10px from the card's left edge, on its middle line, comes toward the eye to w = 0.875 and lands at x =
    // 20 - 8.660 / 0.875.
    PrLayoutVec local{};
    REQUIRE(prLayoutNodeLocalPoint(f.tree, child, prLayoutVecFromPx(PrVec2{20 - 8.6603f / 0.875f, 20}), &local));
    CHECK(double(prLayoutUnitToPx(local.x)) == doctest::Approx(10).epsilon(0.01));
    CHECK(double(prLayoutUnitToPx(local.y)) == doctest::Approx(20).epsilon(0.01));
    // The flat box is 40px down and scaled twice about its center (10, 50).
    REQUIRE(prLayoutNodeLocalPoint(f.tree, flat, prLayoutVecFromPx(PrVec2{25, 45}), &local));
    CHECK(prLayoutUnitToPx(local.x) == 17.5f);
    CHECK(prLayoutUnitToPx(local.y) == 7.5f);
    // A node with no box maps nowhere.
    CHECK(!prLayoutNodeLocalPoint(f.tree, kPrLayoutNodeNone, prLayoutVecFromPx(PrVec2{0, 0}), &local));
}

TEST_CASE("a point on the edge of a transformed box maps onto that edge") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle box = block(10);
    box.width = prLayoutLengthPx(10);
    // Scaled to a fifth about its center, the box covers 4 to 6, and 6 is past its bottom edge.
    PrLayoutEffectStyle effects = prLayoutEffectStyleDefault();
    effects.transform = prLayoutTransformFromAffine2(prAffine2Scale(PrVec2{0.2f, 0.2f}));
    PrLayoutNode small = f.element(root, box, nullptr, &effects);
    f.layout(px(100, 100));
    CHECK(f.hit(PrVec2{5, 5.9f}) == small);
    CHECK(f.hit(PrVec2{5, 6}) != small);
}

TEST_CASE("a compositor's 3D transform plays through its layer's projection, with the parent's perspective") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutEffectStyle near = perspective(40);
    PrLayoutNode parent = f.element(root, perspectiveBox(), nullptr, &near);
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    PrLayoutEffectStyle animated = prLayoutEffectStyleDefault();
    animated.layer = true;
    PrLayoutNode card = f.element(parent, square(), &red, &animated);
    f.layout(px(40, 40));
    const PrLayoutScene* scene = prLayoutTreeScene(f.tree);
    PrLayoutSceneEffect effect{};
    effect.node = card;
    effect.flags = PrLayoutSceneEffectFlag_Transform;
    effect.opacity = 1;
    PrLayoutTransform turn = turned(0.5235988f);
    effect.transform = prLayoutTransformResolve(&turn, PrVec2{40, 40});
    PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
    options.effects = &effect;
    options.effectCount = 1;
    PrDrawList* list = prDrawListCreate();
    prLayoutScenePlay(scene, list, &options);
    PrString dump = prDrawListDump(list);
    CHECK(std::string(prStringView(&dump).data, prStringView(&dump).size).find(" projection") != std::string::npos);
    prStringReset(&dump);
    prDrawListRelease(list);
    prLayoutSceneRelease(scene);
}

TEST_CASE("hit testing follows a transform that changes without a layout") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle box = block(20);
    box.width = prLayoutLengthPx(20);
    // The first hit test caches the wrapper's area, and the transform reaches past it.
    PrLayoutNode wrapper = f.element(root, box);
    PrLayoutNode moved = f.element(wrapper, box);
    f.layout(px(100, 100));
    CHECK(f.hit(PrVec2{45, 10}) != moved);
    CHECK(f.hit(PrVec2{10, 10}) == moved);
    PrLayoutEffectStyle effects = prLayoutEffectStyleDefault();
    effects.transform = prLayoutTransformFromAffine2(prAffine2Translate(PrVec2{40, 0}));
    prLayoutNodeSetStyle(f.tree, moved, f.style(box, nullptr, &effects));
    f.layout(px(100, 100));
    CHECK(f.hit(PrVec2{45, 10}) == moved);
    CHECK(f.hit(PrVec2{10, 10}) != moved);
}

TEST_CASE("painting draws the canvas, backgrounds, borders and opacity") {
    Fixture f;
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), &red);
    PrLayoutDecorationStyle blue = background(PrColor{0, 0, 1, 1});
    PrLayoutBoxStyle bordered = block(20);
    bordered.width = prLayoutLengthPx(20);
    bordered.border[PrRectSide_Top] = px(4, 0).x;
    blue.borderStyle[PrRectSide_Top] = PrDrawBorderStyle_Solid;
    blue.borderColor[PrRectSide_Top] = PR_COLOR16(0, 1, 0, 1);
    f.element(root, bordered, &blue);
    PrLayoutDecorationStyle white = background(PrColor{1, 1, 1, 1});
    PrLayoutEffectStyle half = prLayoutEffectStyleDefault();
    half.opacity = 0.5f;
    f.element(root, block(10), &white, &half);
    f.layout(px(40, 40));
    std::vector<uint8_t> pixels = render(f.tree, 40, 40);
    // The root's background covers the canvas past the root's box.
    CHECK(pixel(pixels, 40, 30, 35) == "255 0 0 255");
    CHECK(pixel(pixels, 40, 10, 2) == "0 255 0 255");
    CHECK(pixel(pixels, 40, 10, 10) == "0 0 255 255");
    CHECK(pixel(pixels, 40, 10, 28) == "255 128 128 255");
}

TEST_CASE("a replaced box takes its natural size and ratio where its style leaves a size auto") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode both = replaced(f, root, block(), natural(100, 50));
    PrLayoutBoxStyle wide = block();
    wide.width = prLayoutLengthPx(200);
    PrLayoutNode scaled = replaced(f, root, wide, natural(100, 50));
    PrLayoutNode ratio = replaced(f, root, block(), natural(-1, -1, 2));
    PrLayoutNode none = replaced(f, root, block(), natural(-1, -1));
    PrLayoutBoxStyle limited = block();
    limited.maxWidth = prLayoutLengthPx(50);
    PrLayoutNode kept = replaced(f, root, limited, natural(100, 50));
    PrLayoutBoxStyle centered = block();
    centered.margin[PrRectSide_Left] = prLayoutLengthAuto();
    centered.margin[PrRectSide_Right] = prLayoutLengthAuto();
    PrLayoutNode middle = replaced(f, root, centered, natural(100, 10));
    f.layout(px(400, 300));
    checkRect(f.bounds(both), 0, 0, 100, 50);
    checkRect(f.bounds(scaled), 0, 50, 200, 150);
    // A ratio alone fills the containing block's width.
    checkRect(f.bounds(ratio), 0, 150, 400, 350);
    // With no natural dimensions, the size is the default object size.
    checkRect(f.bounds(none), 0, 350, 300, 500);
    // A limit keeps the ratio.
    checkRect(f.bounds(kept), 0, 500, 50, 525);
    checkRect(f.bounds(middle), 150, 525, 250, 535);
    CHECK(verified(f.tree));

    // New natural dimensions lay it out again.
    prLayoutNodeSetNaturalSize(f.tree, both, natural(10, 10));
    f.layout(px(400, 300));
    checkRect(f.bounds(both), 0, 0, 10, 10);
    CHECK(verified(f.tree));
}

TEST_CASE("a replaced box's ratio transfers its used size, its limits applied, to its auto size") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    // height: 20px; min-height: 30px: 30 tall, so 60 wide.
    PrLayoutBoxStyle raised = block();
    raised.height = prLayoutLengthPx(20);
    raised.minHeight = prLayoutLengthPx(30);
    PrLayoutNode tall = replaced(f, root, raised, natural(80, 40));
    // width: 100px; max-width: 50px: 50 wide, so 25 tall.
    PrLayoutBoxStyle capped = block();
    capped.width = prLayoutLengthPx(100);
    capped.maxWidth = prLayoutLengthPx(50);
    PrLayoutNode narrow = replaced(f, root, capped, natural(80, 40));
    f.layout(px(400, 300));
    checkRect(f.bounds(tall), 0, 0, 60, 30);
    checkRect(f.bounds(narrow), 0, 30, 50, 55);
    CHECK(verified(f.tree));
}

TEST_CASE("a block's aspect ratio gives an auto size from the other, its limits applied first") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle wide = block();
    wide.aspectRatio = 2;
    // Both sizes auto: the width fills the containing block, and the height follows it.
    PrLayoutNode filled = f.element(root, wide);
    // A height of 100px gives the width, which no longer fills.
    PrLayoutBoxStyle tall = wide;
    tall.height = prLayoutLengthPx(100);
    PrLayoutNode given = f.element(root, tall);
    // A max-height of 50px clamps the height before it transfers.
    PrLayoutBoxStyle capped = tall;
    capped.maxHeight = prLayoutLengthPx(50);
    PrLayoutNode clamped = f.element(root, capped);
    // A max-width of 100px clamps the width, which the height follows.
    PrLayoutBoxStyle narrow = block();
    narrow.aspectRatio = 1;
    narrow.maxWidth = prLayoutLengthPx(100);
    PrLayoutNode limited = f.element(root, narrow);
    // Auto margins share the space that a width from the ratio leaves, as they do for a given width.
    PrLayoutBoxStyle centered = tall;
    centered.margin[PrRectSide_Left] = prLayoutLengthAuto();
    centered.margin[PrRectSide_Right] = prLayoutLengthAuto();
    PrLayoutNode middle = f.element(root, centered);
    f.layout(px(600, 400));
    checkRect(f.bounds(filled), 0, 0, 600, 300);
    checkRect(f.bounds(given), 0, 300, 200, 400);
    checkRect(f.bounds(clamped), 0, 400, 100, 450);
    checkRect(f.bounds(limited), 0, 450, 100, 550);
    checkRect(f.bounds(middle), 200, 550, 400, 650);
    CHECK(verified(f.tree));
}

TEST_CASE("a block's block size from its aspect ratio grows to its content, unless it scrolls or has a minimum") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle square = block();
    square.width = prLayoutLengthPx(100);
    square.aspectRatio = 1;
    PrLayoutNode grows = f.element(root, square);
    f.element(grows, block(300));
    PrLayoutBoxStyle hidden = square;
    hidden.overflowX = PrLayoutOverflow_Hidden;
    hidden.overflowY = PrLayoutOverflow_Hidden;
    PrLayoutNode scrolls = f.element(root, hidden);
    f.element(scrolls, block(300));
    PrLayoutBoxStyle least = square;
    least.minHeight = prLayoutLengthPx(0);
    PrLayoutNode minimum = f.element(root, least);
    f.element(minimum, block(300));
    f.layout(px(600, 400));
    CHECK(f.bounds(grows).bottom - f.bounds(grows).top == 300.0f);
    CHECK(f.bounds(scrolls).bottom - f.bounds(scrolls).top == 100.0f);
    CHECK(f.bounds(minimum).bottom - f.bounds(minimum).top == 100.0f);
    CHECK(verified(f.tree));
}

TEST_CASE("a block's block size from its aspect ratio is definite, and is not auto for margin collapsing") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle strip = block();
    strip.width = prLayoutLengthPx(100);
    strip.aspectRatio = 4;
    PrLayoutNode box = f.element(root, strip);
    // A child's percentage height resolves against the 25px the ratio gives.
    PrLayoutBoxStyle half = block();
    half.height = prLayoutLengthPercent(40);
    PrLayoutNode child = f.element(box, half);
    // The last child's margin neither escapes the box nor grows it: its content ends at the child.
    PrLayoutBoxStyle spaced = block(5);
    spaced.margin[PrRectSide_Bottom] = prLayoutLengthPx(20);
    f.element(box, spaced);
    PrLayoutNode next = f.element(root, block(10));
    f.layout(px(600, 400));
    checkRect(f.bounds(box), 0, 0, 100, 25);
    checkRect(f.bounds(child), 0, 0, 100, 10);
    CHECK(f.bounds(next).top == 25.0f);
    CHECK(verified(f.tree));
}

TEST_CASE("an aspect ratio relates the boxes box-sizing names, or content boxes where auto is given with it") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle border = block();
    border.width = prLayoutLengthPx(100);
    border.padding[PrRectSide_Top] = prLayoutLengthPx(10);
    border.padding[PrRectSide_Bottom] = prLayoutLengthPx(10);
    border.boxSizing = PrLayoutBoxSizing_BorderBox;
    border.aspectRatio = 1;
    PrLayoutNode borderBoxes = f.element(root, border);
    PrLayoutBoxStyle withAuto = border;
    withAuto.aspectRatioAuto = 1;
    PrLayoutNode contentBoxes = f.element(root, withAuto);
    PrLayoutBoxStyle content = border;
    content.boxSizing = PrLayoutBoxSizing_ContentBox;
    PrLayoutNode contentSizing = f.element(root, content);
    f.layout(px(600, 400));
    CHECK(f.bounds(borderBoxes).bottom - f.bounds(borderBoxes).top == 100.0f);
    CHECK(f.bounds(contentBoxes).bottom - f.bounds(contentBoxes).top == 120.0f);
    CHECK(f.bounds(contentSizing).bottom - f.bounds(contentSizing).top == 120.0f);
    CHECK(verified(f.tree));
}

TEST_CASE("a replaced box's aspect ratio replaces its natural one, unless auto is given with the ratio") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle square = block();
    square.aspectRatio = 1;
    // The natural width stays, and the ratio gives the height.
    PrLayoutNode own = replaced(f, root, square, natural(100, 50));
    PrLayoutBoxStyle yields = square;
    yields.aspectRatioAuto = 1;
    PrLayoutNode yielded = replaced(f, root, yields, natural(100, 50));
    // With no natural ratio, auto given with the ratio takes the ratio: the width fills, and the height follows.
    PrLayoutNode none = replaced(f, root, yields, natural(-1, -1));
    f.layout(px(400, 300));
    checkRect(f.bounds(own), 0, 0, 100, 100);
    checkRect(f.bounds(yielded), 0, 100, 100, 150);
    checkRect(f.bounds(none), 0, 150, 400, 550);
    CHECK(verified(f.tree));
}

TEST_CASE("a box out of flow sizes the axis its aspect ratio depends on through the ratio, not by stretching") {
    Fixture f;
    PrLayoutBoxStyle holder = block(100);
    holder.width = prLayoutLengthPx(200);
    holder.position = PrLayoutPosition_Relative;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, holder);
    PrLayoutBoxStyle placed = block();
    placed.position = PrLayoutPosition_Absolute;
    placed.aspectRatio = 1;
    // Insets in the block axis alone: the width depends on the stretched height.
    PrLayoutBoxStyle columnar = placed;
    columnar.inset[PrRectSide_Top] = prLayoutLengthPx(0);
    columnar.inset[PrRectSide_Bottom] = prLayoutLengthPx(0);
    columnar.inset[PrRectSide_Left] = prLayoutLengthPx(0);
    PrLayoutNode fromHeight = f.element(root, columnar);
    // Insets in both axes: the block axis depends on the stretched width.
    PrLayoutBoxStyle all = placed;
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        all.inset[side] = prLayoutLengthPx(0);
    PrLayoutNode fromWidth = f.element(root, all);
    // Its content grows it past the ratio, as the automatic minimum of a box in flow does.
    PrLayoutBoxStyle wide = placed;
    wide.width = prLayoutLengthPx(100);
    wide.aspectRatio = 2;
    PrLayoutNode grown = f.element(root, wide);
    f.element(grown, block(100));
    f.layout(px(400, 300));
    checkRect(f.bounds(fromHeight), 0, 0, 100, 100);
    checkRect(f.bounds(fromWidth), 0, 0, 200, 200);
    checkRect(f.bounds(grown), 0, 0, 100, 100);
    CHECK(verified(f.tree));
}

TEST_CASE("a box's aspect ratio and definite block size give its intrinsic sizes, floored by its content's minimum") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle floating = block();
    floating.floating = PrLayoutFloat_Left;
    PrLayoutNode wide = f.element(root, floating);
    PrLayoutBoxStyle ratio = block(50);
    ratio.aspectRatio = 2;
    f.element(wide, ratio);
    PrLayoutNode narrow = f.element(root, floating);
    PrLayoutBoxStyle small = block(20);
    small.aspectRatio = 1;
    PrLayoutNode inner = f.element(narrow, small);
    PrLayoutBoxStyle word = block(10);
    word.width = prLayoutLengthPx(60);
    f.element(inner, word);
    f.layout(px(600, 400));
    CHECK(f.bounds(wide).right - f.bounds(wide).left == 100.0f);
    CHECK(f.bounds(narrow).right - f.bounds(narrow).left == 60.0f);
    CHECK(verified(f.tree));
}

TEST_CASE("an inline replaced box sits on the baseline at its bottom margin edge, and lines break around it") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    f.text(root, "a");
    PrLayoutNode image = replaced(f, root, inlineBox(), natural(20, 40));
    PrLayoutNode after = f.text(root, "b");
    f.layout(px(400, 300));
    PrRect box = f.bounds(image);
    PrRect text = f.bounds(after);
    CHECK(box.right - box.left == 20.0f);
    CHECK(box.top == 0.0f);
    // The text's baseline is the box's bottom.
    CHECK(text.top < box.bottom);
    CHECK(text.bottom > box.bottom);
    CHECK(text.left >= box.right);

    // Too narrow for the text and the box together, the box goes to a line of its own.
    f.layout(px(25, 300));
    PrRect wrapped = f.bounds(image);
    CHECK(wrapped.left == 0.0f);
    CHECK(wrapped.top > 0.0f);
    CHECK(verified(f.tree));
    CHECK(f.hit(PrVec2{5, wrapped.top + 5}) == image);
}

TEST_CASE("a replaced box's content fits its content box as object-fit says, and changing it lays out nothing") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle box = block();
    box.width = prLayoutLengthPx(40);
    box.height = prLayoutLengthPx(20);
    PrLayoutDecorationStyle contain = prLayoutDecorationStyleDefault();
    contain.objectFit = PrLayoutObjectFit_Contain;
    PrLayoutNode filled = replaced(f, root, box, natural(10, 10));
    PrLayoutNode fitted = replaced(f, root, box, natural(10, 10), &contain);
    PrLayoutContent red = solid(PrColor{1, 0, 0, 1}, 10, 10);
    prLayoutNodeSetContent(f.tree, filled, &red);
    prLayoutNodeSetContent(f.tree, fitted, &red);
    prDrawListRelease(red.list);
    f.layout(px(40, 40));
    std::vector<uint8_t> pixels = render(f.tree, 40, 40);
    // Fill stretches the content over the box; contain keeps its ratio, centered.
    CHECK(pixel(pixels, 40, 2, 10) == "255 0 0 255");
    CHECK(pixel(pixels, 40, 38, 10) == "255 0 0 255");
    CHECK(pixel(pixels, 40, 5, 30) == "0 0 0 0");
    CHECK(pixel(pixels, 40, 20, 30) == "255 0 0 255");
    CHECK(pixel(pixels, 40, 35, 30) == "0 0 0 0");

    // New content records again and lays out nothing.
    PrLayoutContent blue = solid(PrColor{0, 0, 1, 1}, 10, 10);
    prLayoutNodeSetContent(f.tree, filled, &blue);
    prDrawListRelease(blue.list);
    CHECK(prLayoutNodeContent(f.tree, filled).list == blue.list);
    f.layout(px(40, 40));
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);
    pixels = render(f.tree, 40, 40);
    CHECK(pixel(pixels, 40, 2, 10) == "0 0 255 255");
    CHECK(verified(f.tree));
}

TEST_CASE("an image's content covers its natural size, which object-fit and object-position place") {
    Fixture f;
    // An image of a red texel beside a blue one, in a natural size twice as wide as it is tall.
    const uint8_t texels[8] = {255, 0, 0, 255, 0, 0, 255, 255};
    PrDrawImageDesc desc{PrPixelView{texels, 8, {2, 1}}, PrPixelFormat_Rgba8, PrDrawAlpha_Straight, PrColorSpace_Srgb};
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&desc, &image) == PrStatus_Ok);
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle box = block();
    box.width = prLayoutLengthPx(40);
    box.height = prLayoutLengthPx(40);
    PrLayoutDecorationStyle start = prLayoutDecorationStyleDefault();
    start.objectFit = PrLayoutObjectFit_Cover;
    start.objectPositionX = prLayoutLengthPercent(0);
    PrLayoutDecorationStyle end = start;
    end.objectPositionX = prLayoutLengthPercent(100);
    PrLayoutNode left = replaced(f, root, box, natural(20, 10), &start);
    PrLayoutNode right = replaced(f, root, box, natural(20, 10), &end);
    PrLayoutContent content{};
    content.kind = PrLayoutContentKind_Image;
    content.image = image;
    prLayoutNodeSetContent(f.tree, left, &content);
    prLayoutNodeSetContent(f.tree, right, &content);
    prDrawImageRelease(image);
    CHECK(prLayoutNodeContent(f.tree, left).image == image);
    f.layout(px(40, 80));
    std::vector<uint8_t> pixels = render(f.tree, 40, 80);
    // Cover scales the image to 80 by 40, which shows its red half at the start and its blue half at the end.
    CHECK(pixel(pixels, 40, 10, 20) == "255 0 0 255");
    CHECK(pixel(pixels, 40, 30, 60) == "0 0 255 255");
    CHECK(verified(f.tree));
}

TEST_CASE("a background is painted within the box background-clip names, and an outline outside the border box") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    // A box 20 by 20 at 10,10, with 5px of padding, its background clipped to its content box, and an outline 2px wide
    // 1px outside its border box.
    PrLayoutBoxStyle box = block();
    box.width = prLayoutLengthPx(10);
    box.height = prLayoutLengthPx(10);
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        box.margin[side] = prLayoutLengthPx(10);
        box.padding[side] = prLayoutLengthPx(5);
    }
    PrLayoutDecorationStyle decoration = background(PrColor{1, 0, 0, 1});
    decoration.backgroundClip = PrLayoutBackgroundBox_ContentBox;
    decoration.outlineStyle = PrDrawBorderStyle_Solid;
    decoration.outlineWidth = prLayoutUnitFromPx(2);
    decoration.outlineOffset = prLayoutUnitFromPx(1);
    decoration.outlineColor = prColor16FromColor(PrColor{0, 0, 1, 1});
    f.element(root, box, &decoration);
    f.layout(px(40, 40));
    std::vector<uint8_t> pixels = render(f.tree, 40, 40);
    CHECK(pixel(pixels, 40, 20, 20) == "255 0 0 255");
    // The padding is not painted, nor is the gap that the offset leaves; the outline is.
    CHECK(pixel(pixels, 40, 12, 20) == "0 0 0 0");
    CHECK(pixel(pixels, 40, 9, 20) == "0 0 0 0");
    CHECK(pixel(pixels, 40, 7, 20) == "0 0 255 255");
    CHECK(pixel(pixels, 40, 4, 20) == "0 0 0 0");
    CHECK(verified(f.tree));
}

TEST_CASE("a background layer fixed to the viewport stays with it as its box scrolls, and fills a new viewport") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle scroller = block(40);
    scroller.width = prLayoutLengthPx(20);
    scroller.overflowX = PrLayoutOverflow_Hidden;
    scroller.overflowY = PrLayoutOverflow_Scroll;
    PrLayoutNode box = f.element(root, scroller);
    // Red over the viewport's top half and blue over its bottom half, wherever the box that paints it is.
    PrLayoutColorStop stops[] = {colorStop(PrColor{1, 0, 0, 1}, prLayoutLengthPercent(50)),
                                 colorStop(PrColor{0, 0, 1, 1}, prLayoutLengthPercent(50))};
    PrLayoutBackgroundLayer layer = gradientLayer(stops, 2);
    layer.attachment = PrLayoutBackgroundAttachment_Fixed;
    backgroundBox(f, box, 20, 100, &layer, 1);
    f.layout(px(20, 40));
    std::vector<uint8_t> pixels = render(f.tree, 20, 40);
    CHECK(pixel(pixels, 20, 10, 10) == "255 0 0 255");
    CHECK(pixel(pixels, 20, 10, 30) == "0 0 255 255");
    // Scrolled, the box moves and the layer does not.
    prLayoutNodeSetScroll(f.tree, box, px(0, 30));
    pixels = render(f.tree, 20, 40);
    CHECK(pixel(pixels, 20, 10, 10) == "255 0 0 255");
    CHECK(pixel(pixels, 20, 10, 30) == "0 0 255 255");
    // A smaller viewport records the layer again in its extent.
    f.layout(px(20, 20));
    pixels = render(f.tree, 20, 20);
    CHECK(pixel(pixels, 20, 10, 5) == "255 0 0 255");
    CHECK(pixel(pixels, 20, 10, 15) == "0 0 255 255");
    CHECK(verified(f.tree));
}

TEST_CASE("a background layer fixed to the viewport scrolls with its box under a transform") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle scroller = block(40);
    scroller.width = prLayoutLengthPx(20);
    scroller.overflowX = PrLayoutOverflow_Hidden;
    scroller.overflowY = PrLayoutOverflow_Scroll;
    scroller.transformed = true;
    PrLayoutNode box = f.element(root, scroller);
    PrLayoutColorStop stops[] = {colorStop(PrColor{1, 0, 0, 1}, prLayoutLengthPercent(50)),
                                 colorStop(PrColor{0, 0, 1, 1}, prLayoutLengthPercent(50))};
    PrLayoutBackgroundLayer layer = gradientLayer(stops, 2);
    layer.attachment = PrLayoutBackgroundAttachment_Fixed;
    backgroundBox(f, box, 20, 100, &layer, 1);
    f.layout(px(20, 40));
    // Placed as a scrolling layer is, in its box 100 tall, whose top half is red.
    prLayoutNodeSetScroll(f.tree, box, px(0, 40));
    std::vector<uint8_t> pixels = render(f.tree, 20, 40);
    CHECK(pixel(pixels, 20, 10, 5) == "255 0 0 255");
    CHECK(pixel(pixels, 20, 10, 15) == "0 0 255 255");
    CHECK(verified(f.tree));
}

TEST_CASE("background layers intern by value, and a layout frees the layers that no group names") {
    Fixture f;
    PrLayoutColorStop stops[] = {colorStop(PrColor{1, 0, 0, 1}), colorStop(PrColor{0, 0, 1, 1})};
    PrLayoutColorStop copy[] = {stops[0], stops[1]};
    PrLayoutBackgroundLayer layer = gradientLayer(stops, 2);
    PrLayoutBackgroundLayer same = gradientLayer(copy, 2);
    PrLayoutBackgroundIndex index = prLayoutBackgroundIntern(f.tree, &layer, 1);
    CHECK(index != kPrLayoutBackgroundNone);
    CHECK(prLayoutBackgroundIntern(f.tree, &same, 1) == index);
    CHECK(prLayoutBackgroundIntern(f.tree, &layer, 0) == kPrLayoutBackgroundNone);
    copy[1] = colorStop(PrColor{0, 1, 0, 1});
    CHECK(prLayoutBackgroundIntern(f.tree, &same, 1) != index);
    // The tree keeps its own copy of the stops.
    stops[0] = colorStop(PrColor{0, 0, 0, 1});
    CHECK(prLayoutBackgroundLayer(f.tree, index, 0).gradient.stops[0].color.r == PR_COLOR16(1, 0, 0, 1).r);
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBackgroundLayer named = gradientLayer(copy, 2);
    backgroundBox(f, root, 10, 10, &named, 1);
    f.layout(px(40, 40));
    CHECK(verified(f.tree));
}

TEST_CASE("a linear gradient runs along its line, which a corner points") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutColorStop stops[] = {colorStop(PrColor{1, 0, 0, 1}, prLayoutLengthPercent(50)),
                                 colorStop(PrColor{0, 0, 1, 1}, prLayoutLengthPercent(50))};
    // To the right, with a hard edge at the middle.
    PrLayoutBackgroundLayer right = gradientLayer(stops, 2);
    right.gradient.angle = 90;
    backgroundBox(f, root, 40, 10, &right, 1);
    // To the bottom right of a square, whose line is its diagonal: the edge runs from the top right corner.
    PrLayoutBackgroundLayer corner = gradientLayer(stops, 2);
    corner.gradient.corner = PrLayoutGradientCorner_BottomRight;
    backgroundBox(f, root, 20, 20, &corner, 1);
    f.layout(px(40, 40));
    std::vector<uint8_t> pixels = render(f.tree, 40, 40);
    CHECK(pixel(pixels, 40, 18, 5) == "255 0 0 255");
    CHECK(pixel(pixels, 40, 22, 5) == "0 0 255 255");
    CHECK(pixel(pixels, 40, 3, 13) == "255 0 0 255");
    CHECK(pixel(pixels, 40, 17, 27) == "0 0 255 255");
    CHECK(verified(f.tree));
}

TEST_CASE("color stops without positions spread evenly, and a stop before an earlier one moves to it") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    // red, lime, blue 50%, white 10%: lime at 25%, and white at 50% beside blue.
    PrLayoutColorStop stops[] = {colorStop(PrColor{1, 0, 0, 1}), colorStop(PrColor{0, 1, 0, 1}),
                                 colorStop(PrColor{0, 0, 1, 1}, prLayoutLengthPercent(50)),
                                 colorStop(PrColor{1, 1, 1, 1}, prLayoutLengthPercent(10))};
    PrLayoutBackgroundLayer layer = gradientLayer(stops, 4);
    layer.gradient.angle = 90;
    backgroundBox(f, root, 40, 10, &layer, 1);
    f.layout(px(40, 40));
    std::vector<uint8_t> pixels = render(f.tree, 40, 40);
    // Lime at 10px, a quarter of the way.
    CHECK(pixels[(5 * 40 + 10) * 4 + 1] > 230);
    CHECK(pixels[(5 * 40 + 10) * 4] < 20);
    CHECK(pixel(pixels, 40, 19, 5) != "255 255 255 255");
    CHECK(pixel(pixels, 40, 21, 5) == "255 255 255 255");
    CHECK(pixel(pixels, 40, 38, 5) == "255 255 255 255");
}

TEST_CASE("a background image is sized, positioned and repeated in its positioning area") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutColorStop stops[] = {colorStop(PrColor{1, 0, 0, 1}), colorStop(PrColor{1, 0, 0, 1})};
    // A 10px red square at the bottom right, once, over a blue background.
    PrLayoutBackgroundLayer corner = gradientLayer(stops, 2);
    corner.sizeX = prLayoutLengthPx(10);
    corner.sizeY = prLayoutLengthPx(10);
    corner.positionX = prLayoutLengthPercent(100);
    corner.positionY = prLayoutLengthPercent(100);
    corner.repeatX = PrLayoutBackgroundRepeat_NoRepeat;
    corner.repeatY = PrLayoutBackgroundRepeat_NoRepeat;
    backgroundBox(f, root, 40, 20, &corner, 1, PrColor{0, 0, 1, 1});
    // Squares of 10px repeated across only, with a gap: 5px at 0, 15, 25 and 35... each its own tile.
    PrLayoutBackgroundLayer row = corner;
    row.positionX = prLayoutLengthPx(0);
    row.positionY = prLayoutLengthPx(0);
    row.sizeX = prLayoutLengthPx(5);
    row.sizeY = prLayoutLengthPx(5);
    row.repeatX = PrLayoutBackgroundRepeat_Space;
    backgroundBox(f, root, 40, 20, &row, 1, PrColor{0, 0, 1, 1});
    f.layout(px(40, 40));
    std::vector<uint8_t> pixels = render(f.tree, 40, 40);
    CHECK(pixel(pixels, 40, 35, 15) == "255 0 0 255");
    CHECK(pixel(pixels, 40, 25, 15) == "0 0 255 255");
    CHECK(pixel(pixels, 40, 35, 5) == "0 0 255 255");
    // Eight copies of 5px fit in 40px exactly, so space leaves no gap; the row is red, and below it blue.
    CHECK(pixel(pixels, 40, 2, 22) == "255 0 0 255");
    CHECK(pixel(pixels, 40, 37, 22) == "255 0 0 255");
    CHECK(pixel(pixels, 40, 20, 30) == "0 0 255 255");
    CHECK(verified(f.tree));
}

TEST_CASE("space spreads whole copies, and round scales them to fill the area") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutColorStop stops[] = {colorStop(PrColor{1, 0, 0, 1}), colorStop(PrColor{1, 0, 0, 1})};
    // 12px squares in 40px: space fits three, with gaps of 2px; round fits three of 13.33px.
    PrLayoutBackgroundLayer spaced = gradientLayer(stops, 2);
    spaced.sizeX = prLayoutLengthPx(12);
    spaced.sizeY = prLayoutLengthPx(10);
    spaced.repeatX = PrLayoutBackgroundRepeat_Space;
    spaced.repeatY = PrLayoutBackgroundRepeat_NoRepeat;
    backgroundBox(f, root, 40, 10, &spaced, 1, PrColor{0, 0, 1, 1});
    PrLayoutBackgroundLayer rounded = spaced;
    rounded.repeatX = PrLayoutBackgroundRepeat_Round;
    // Half red, half blue in each copy, to see the copies' width.
    PrLayoutColorStop halves[] = {colorStop(PrColor{1, 0, 0, 1}, prLayoutLengthPercent(50)),
                                  colorStop(PrColor{0, 1, 0, 1}, prLayoutLengthPercent(50))};
    rounded.gradient.stops = halves;
    rounded.gradient.angle = 90;
    backgroundBox(f, root, 40, 10, &rounded, 1, PrColor{0, 0, 1, 1});
    f.layout(px(40, 40));
    std::vector<uint8_t> pixels = render(f.tree, 40, 40);
    CHECK(pixel(pixels, 40, 11, 5) == "255 0 0 255");
    CHECK(pixel(pixels, 40, 13, 5) == "0 0 255 255");
    CHECK(pixel(pixels, 40, 15, 5) == "255 0 0 255");
    CHECK(pixel(pixels, 40, 39, 5) == "255 0 0 255");
    // Copies 13.33px wide: green from 6.67 to 13.33, red from 13.33 to 20.
    CHECK(pixel(pixels, 40, 12, 15) == "0 255 0 255");
    CHECK(pixel(pixels, 40, 14, 15) == "255 0 0 255");
    CHECK(pixel(pixels, 40, 39, 15) == "0 255 0 255");
}

TEST_CASE("a repeating conic gradient repeats its stops around the turn") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    // A checkerboard of 10px squares: white, then gray, a quarter turn each, from the top, repeated each half turn.
    PrLayoutColorStop stops[] = {colorStop(PrColor{1, 1, 1, 1}, prLayoutLengthPercent(0)),
                                 colorStop(PrColor{1, 1, 1, 1}, prLayoutLengthPercent(25)),
                                 colorStop(PrColor{0, 0, 0, 1}, prLayoutLengthPercent(0)),
                                 colorStop(PrColor{0, 0, 0, 1}, prLayoutLengthPercent(50))};
    PrLayoutBackgroundLayer layer = gradientLayer(stops, 4);
    layer.gradient.kind = PrLayoutGradientKind_Conic;
    layer.gradient.angle = 0;
    layer.gradient.repeating = true;
    layer.sizeX = prLayoutLengthPx(20);
    layer.sizeY = prLayoutLengthPx(20);
    backgroundBox(f, root, 40, 40, &layer, 1);
    f.layout(px(40, 40));
    std::vector<uint8_t> pixels = render(f.tree, 40, 40);
    CHECK(pixel(pixels, 40, 15, 5) == "255 255 255 255");
    CHECK(pixel(pixels, 40, 15, 15) == "0 0 0 255");
    CHECK(pixel(pixels, 40, 5, 15) == "255 255 255 255");
    CHECK(pixel(pixels, 40, 5, 5) == "0 0 0 255");
    CHECK(pixel(pixels, 40, 25, 5) == "0 0 0 255");
    CHECK(pixel(pixels, 40, 35, 25) == "255 255 255 255");
}

TEST_CASE("a radial gradient's ellipse meets the farthest corner by default") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutColorStop stops[] = {colorStop(PrColor{1, 0, 0, 1}, prLayoutLengthPercent(70)),
                                 colorStop(PrColor{0, 0, 1, 1}, prLayoutLengthPercent(70))};
    PrLayoutBackgroundLayer layer = gradientLayer(stops, 2);
    layer.gradient.kind = PrLayoutGradientKind_Radial;
    backgroundBox(f, root, 40, 20, &layer, 1);
    PrLayoutBackgroundLayer circle = layer;
    circle.gradient.shape = PrLayoutRadialShape_Circle;
    circle.gradient.extent = PrLayoutRadialExtent_Radii;
    circle.gradient.radiusX = prLayoutLengthPx(10);
    backgroundBox(f, root, 40, 20, &circle, 1);
    f.layout(px(40, 40));
    std::vector<uint8_t> pixels = render(f.tree, 40, 40);
    // The ellipse through the corners has radii of 20 and 10 times the square root of 2, so 70% of it reaches about
    // 19.8px across and 9.9px down.
    CHECK(pixel(pixels, 40, 38, 10) == "255 0 0 255");
    CHECK(pixel(pixels, 40, 39, 1) == "0 0 255 255");
    CHECK(pixel(pixels, 40, 20, 1) == "255 0 0 255");
    // The circle's 70% is 7px.
    CHECK(pixel(pixels, 40, 26, 30) == "255 0 0 255");
    CHECK(pixel(pixels, 40, 28, 30) == "0 0 255 255");
}

TEST_CASE("the root's background layers cover the canvas, placed by the root's box") {
    Fixture f;
    PrLayoutColorStop stops[] = {colorStop(PrColor{1, 0, 0, 1}), colorStop(PrColor{1, 0, 0, 1})};
    PrLayoutBackgroundLayer layer = gradientLayer(stops, 2);
    layer.sizeX = prLayoutLengthPx(10);
    layer.sizeY = prLayoutLengthPx(10);
    layer.repeatY = PrLayoutBackgroundRepeat_NoRepeat;
    PrLayoutBoxStyle box = block();
    box.height = prLayoutLengthPx(10);
    box.margin[PrRectSide_Top] = prLayoutLengthPx(20);
    PrLayoutDecorationStyle decoration = background(PrColor{0, 0, 1, 1});
    decoration.backgroundLayers = prLayoutBackgroundIntern(f.tree, &layer, 1);
    f.element(kPrLayoutNodeNone, box, &decoration);
    f.layout(px(40, 40));
    std::vector<uint8_t> pixels = render(f.tree, 40, 40);
    // The row of squares is at the root's top, past which the color covers the canvas.
    CHECK(pixel(pixels, 40, 35, 25) == "255 0 0 255");
    CHECK(pixel(pixels, 40, 35, 5) == "0 0 255 255");
    CHECK(pixel(pixels, 40, 35, 35) == "0 0 255 255");
    CHECK(verified(f.tree));
}

TEST_CASE("an image background repeats its texels at its natural size") {
    Fixture f;
    const uint8_t texels[8] = {255, 0, 0, 255, 0, 0, 255, 255};
    PrDrawImageDesc desc{PrPixelView{texels, 8, {2, 1}}, PrPixelFormat_Rgba8, PrDrawAlpha_Straight, PrColorSpace_Srgb};
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&desc, &image) == PrStatus_Ok);
    PrLayoutBackgroundLayer layer = prLayoutBackgroundLayerDefault();
    layer.kind = PrLayoutImageKind_Image;
    layer.image = image;
    layer.rendering = PrLayoutImageRendering_Pixelated;
    layer.natural = natural(20, 10);
    layer.repeatY = PrLayoutBackgroundRepeat_NoRepeat;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    backgroundBox(f, root, 40, 20, &layer, 1);
    prDrawImageRelease(image);
    f.layout(px(40, 40));
    std::vector<uint8_t> pixels = render(f.tree, 40, 40);
    CHECK(pixel(pixels, 40, 5, 5) == "255 0 0 255");
    CHECK(pixel(pixels, 40, 15, 5) == "0 0 255 255");
    CHECK(pixel(pixels, 40, 25, 5) == "255 0 0 255");
    CHECK(pixel(pixels, 40, 35, 5) == "0 0 255 255");
    CHECK(pixel(pixels, 40, 5, 15) == "0 0 0 0");
    CHECK(verified(f.tree));
}

TEST_CASE("an inline replaced box with effects paints whole, as a layer of its own") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle inlineBox = prLayoutBoxStyleDefault();
    inlineBox.display = PrLayoutDisplay_Inline;
    PrLayoutEffectStyle scaled = prLayoutEffectStyleDefault();
    scaled.transform = prLayoutTransformFromAffine2(prAffine2Scale(PrVec2{2, 2}));
    scaled.originX = prLayoutLengthPx(0);
    scaled.originY = prLayoutLengthPx(0);
    PrLayoutNode image = prLayoutNodeCreateReplaced(f.tree, root, f.style(inlineBox, nullptr, &scaled));
    prLayoutNodeSetNaturalSize(f.tree, image, natural(10, 10));
    PrLayoutContent red = solid(PrColor{1, 0, 0, 1}, 10, 10);
    prLayoutNodeSetContent(f.tree, image, &red);
    prDrawListRelease(red.list);
    f.layout(px(40, 40));
    const PrLayoutScene* scene = prLayoutTreeScene(f.tree);
    CHECK(prLayoutSceneHasLayer(scene, image));
    prLayoutSceneRelease(scene);
    // The box is 10 by 10 on its line, drawn twice that.
    std::vector<uint8_t> pixels = render(f.tree, 40, 40);
    CHECK(pixel(pixels, 40, 15, 15) == "255 0 0 255");
    CHECK(pixel(pixels, 40, 25, 25) == "0 0 0 0");
    CHECK(verified(f.tree));
}

TEST_CASE("a painter draws a replaced box's content for its content box's size, which the tree keeps") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle box = block();
    box.width = prLayoutLengthPx(40);
    box.height = prLayoutLengthPx(20);
    PrLayoutNode node = replaced(f, root, box, prLayoutNaturalSizeNone());
    // It fills the left half of whatever box it is given, and counts its calls.
    struct Painted {
        uint32_t calls = 0;
        PrVec2 extent{};
    } painted;
    PrLayoutContent content{};
    content.kind = PrLayoutContentKind_Painter;
    content.painter.paint = [](void* PR_NULLABLE context, PrVec2 extent, PrDrawList* list) {
        Painted* p = static_cast<Painted*>(context);
        ++p->calls;
        p->extent = extent;
        prDrawListRect(list, PrRect{0, 0, extent.x / 2, extent.y}, {.color = PrColor{0, 1, 0, 1}});
    };
    content.painter.context = &painted;
    prLayoutNodeSetContent(f.tree, node, &content);
    f.layout(px(40, 40));
    std::vector<uint8_t> pixels = render(f.tree, 40, 40);
    CHECK(painted.calls == 1);
    CHECK(painted.extent.x == 40.0f);
    CHECK(painted.extent.y == 20.0f);
    CHECK(pixel(pixels, 40, 10, 10) == "0 255 0 255");
    CHECK(pixel(pixels, 40, 30, 10) == "0 0 0 0");

    // Painting again keeps what it drew, and a new size draws again.
    render(f.tree, 40, 40);
    CHECK(painted.calls == 1);
    box.width = prLayoutLengthPx(20);
    prLayoutNodeSetStyle(f.tree, node, f.style(box));
    f.layout(px(40, 40));
    pixels = render(f.tree, 40, 40);
    CHECK(painted.calls == 2);
    CHECK(pixel(pixels, 40, 15, 10) == "0 0 0 0");

    // Invalidating draws again at the next paint, and lays out nothing.
    prLayoutNodeInvalidateContent(f.tree, node);
    f.layout(px(40, 40));
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);
    render(f.tree, 40, 40);
    CHECK(painted.calls == 3);
    // Verifying the tree's caches draws again from scratch, and finds the same drawing.
    CHECK(verified(f.tree));
}

TEST_CASE("a root other than the viewport's is laid out in a containing block of its extent, which holds its boxes") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(10));
    PrLayoutBoxStyle whole = block();
    whole.height = prLayoutLengthPercent(100);
    PrLayoutNode other = f.element(kPrLayoutNodeNone, whole);
    f.root = root;
    PrLayoutBoxStyle half = block();
    half.height = prLayoutLengthPercent(50);
    PrLayoutNode child = f.element(other, half);
    PrLayoutBoxStyle corner = block(10);
    corner.width = prLayoutLengthPx(20);
    corner.position = PrLayoutPosition_Absolute;
    corner.inset[PrRectSide_Right] = prLayoutLengthPx(0);
    corner.inset[PrRectSide_Bottom] = prLayoutLengthPx(0);
    PrLayoutNode placed = f.element(other, corner);
    PrLayoutRoot roots[2] = {{root, px(100, 100)}, {other, px(60, 40)}};
    prLayoutTreeLayout(f.tree, roots, 2);
    auto bounds = [&](PrLayoutNode node) {
        return prLayoutRectToPx(prLayoutNodeBounds(f.tree, node, PrLayoutCoordinates_Layout));
    };
    // Percentages and insets are of the root's containing block, and geometry is measured from its top left.
    CHECK(bounds(child).right == 60.0f);
    CHECK(bounds(child).bottom == 20.0f);
    CHECK(bounds(placed).left == 40.0f);
    CHECK(bounds(placed).top == 30.0f);
    CHECK(prLayoutTreeRoot(f.tree) == root);
    CHECK(verified(f.tree));
    // The viewport shows its own root alone.
    CHECK(f.hit(PrVec2{50, 35}) == kPrLayoutNodeNone);

    // A change in the other root lays out that root again, and a new extent lays it out at that extent.
    roots[1].extent = px(80, 40);
    prLayoutTreeLayout(f.tree, roots, 2);
    CHECK(bounds(child).right == 80.0f);
    CHECK(bounds(placed).left == 60.0f);
    CHECK(verified(f.tree));

    // A root that a layout is not given is laid out no more.
    prLayoutTreeLayout(f.tree, roots, 1);
    CHECK(bounds(child).right == 0.0f);
    CHECK(verified(f.tree));
}

TEST_CASE("a painter draws another root in a replaced box's content, and draws again when that root changes") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle box = block(20);
    box.width = prLayoutLengthPx(40);
    PrLayoutNode node = replaced(f, root, box, prLayoutNaturalSizeNone());
    PrLayoutDecorationStyle green = background(PrColor{0, 1, 0, 1});
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    PrLayoutNode hosted = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode fill = f.element(hosted, block(10), &green);
    // A box past the root's containing block is drawn, for the painter to clip if it clips.
    f.element(hosted, block(10), &green);
    f.root = root;
    // The painter draws the other root at its content's top left, and counts its calls.
    struct Host {
        PrLayoutTree* tree;
        PrLayoutNode root;
        uint32_t calls;
    } host{f.tree, hosted, 0};
    PrLayoutContent content{};
    content.kind = PrLayoutContentKind_Painter;
    content.painter.paint = [](void* PR_NULLABLE context, PrVec2, PrDrawList* list) {
        Host* h = static_cast<Host*>(context);
        ++h->calls;
        prLayoutNodeDraw(h->tree, h->root, list);
    };
    content.painter.context = &host;
    prLayoutNodeSetContent(f.tree, node, &content);
    PrLayoutRoot roots[2] = {{root, px(40, 40)}, {hosted, px(20, 10)}};
    prLayoutTreeLayout(f.tree, roots, 2);
    std::vector<uint8_t> pixels = render(f.tree, 40, 40);
    CHECK(host.calls == 1);
    CHECK(pixel(pixels, 40, 5, 5) == "0 255 0 255");
    CHECK(pixel(pixels, 40, 25, 5) == "0 0 0 0");
    CHECK(pixel(pixels, 40, 5, 15) == "0 255 0 255");

    // Painting again draws nothing again. A change in the other root draws the box again.
    render(f.tree, 40, 40);
    CHECK(host.calls == 1);
    prLayoutNodeSetStyle(f.tree, fill, f.style(block(10), &red));
    prLayoutTreeLayout(f.tree, roots, 2);
    pixels = render(f.tree, 40, 40);
    CHECK(host.calls == 2);
    CHECK(pixel(pixels, 40, 5, 5) == "255 0 0 255");
    render(f.tree, 40, 40);
    CHECK(host.calls == 2);
    CHECK(verified(f.tree));
}

TEST_CASE("every root that a frame creates is committed, and a root that it does not create again is erased") {
    Fixture f;
    uint32_t box = f.style(block(10));
    prLayoutTreeBeginFrame(f.tree);
    PrLayoutNode first = prLayoutTreeFrameElement(f.tree, kPrLayoutNodeNone, 0, box);
    PrLayoutNode second = prLayoutTreeFrameElement(f.tree, kPrLayoutNodeNone, 0, box);
    PrLayoutNode roots[2] = {};
    CHECK(prLayoutTreeFrameRoots(f.tree, roots, 2) == 2);
    CHECK(roots[0] == first);
    CHECK(roots[1] == second);
    PrLayoutRoot laid[2] = {{first, px(40, 40)}, {second, px(20, 10)}};
    prLayoutTreeLayout(f.tree, laid, 2);
    CHECK(prLayoutTreeFrameRoots(f.tree, nullptr, 0) == 2);
    CHECK(verified(f.tree));

    prLayoutTreeBeginFrame(f.tree);
    PrLayoutNode again = prLayoutTreeFrameElement(f.tree, kPrLayoutNodeNone, 0, box);
    CHECK(again == first);
    prLayoutTreeLayout(f.tree, laid, 1);
    CHECK_FALSE(prLayoutNodeIsValid(f.tree, second));
    CHECK(prLayoutTreeFrameRoots(f.tree, nullptr, 0) == 1);
    CHECK(verified(f.tree));
}

TEST_CASE("text in an aliased font covers each pixel of a glyph whole or not at all") {
    for (bool aliased : {false, true}) {
        CAPTURE(aliased);
        Fixture f(aliased);
        PrLayoutFontStyle font = prLayoutFontStyleDefault();
        font.font = textFontOf(20);
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font);
        f.text(root, "sow");
        f.layout(px(60, 30));
        std::vector<uint8_t> pixels = render(f.tree, 60, 30);
        uint32_t covered = 0;
        uint32_t partial = 0;
        for (size_t i = 3; i < pixels.size(); i += 4) {
            covered += pixels[i] == 255;
            partial += pixels[i] != 0 && pixels[i] != 255;
        }
        CHECK(covered > 0);
        CHECK((partial == 0) == aliased);
    }
}

TEST_CASE("a scroll container clips and scrolls its content") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle scroller = block(20);
    scroller.overflowY = PrLayoutOverflow_Scroll;
    PrLayoutNode s = f.element(root, scroller);
    PrLayoutDecorationStyle green = background(PrColor{0, 1, 0, 1});
    f.element(s, block(20));
    f.element(s, block(20), &green);
    f.layout(px(20, 40));
    std::vector<uint8_t> before = render(f.tree, 20, 40);
    CHECK(pixel(before, 20, 10, 10) == "0 0 0 0");
    CHECK(pixel(before, 20, 10, 30) == "0 0 0 0");
    prLayoutNodeSetScroll(f.tree, s, px(0, 100));
    std::vector<uint8_t> after = render(f.tree, 20, 40);
    CHECK(pixel(after, 20, 10, 10) == "0 255 0 255");
    CHECK(pixel(after, 20, 10, 30) == "0 0 0 0");
}

TEST_CASE("a scene scrolls a container's content as it plays, and a scroll records nothing") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle scroller = block(20);
    scroller.overflowY = PrLayoutOverflow_Scroll;
    PrLayoutNode s = f.element(root, scroller);
    PrLayoutDecorationStyle green = background(PrColor{0, 1, 0, 1});
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    f.element(s, block(20), &red);
    f.element(s, block(20), &green);
    // A positioned box in the container scrolls with it, as a layer of its own.
    PrLayoutBoxStyle lifted = block(10);
    lifted.position = PrLayoutPosition_Relative;
    f.element(s, lifted, &red);
    f.layout(px(20, 40));
    std::vector<uint8_t> before = render(f.tree, 20, 40);
    CHECK(pixel(before, 20, 10, 10) == "255 0 0 255");

    PrLayoutScrollInfo info{};
    REQUIRE(prLayoutNodeScrollInfo(f.tree, s, &info));
    CHECK(info.scrollContainer);
    CHECK(prLayoutUnitToPx(info.range.bottom) == 30.0f);
    CHECK(info.userAxes == PrLayoutScrollFlag_Y);

    // The offset is clamped to the range, and the scene, which holds no offset, is the same one.
    const PrLayoutScene* last = prLayoutTreeScene(f.tree);
    prLayoutNodeSetScroll(f.tree, s, px(0, 100));
    const PrLayoutScene* scrolled = prLayoutTreeScene(f.tree);
    CHECK(scrolled == last);
    prLayoutSceneRelease(scrolled);
    prLayoutSceneRelease(last);
    std::vector<uint8_t> after = render(f.tree, 20, 40);
    CHECK(prLayoutTreeStats(f.tree).layersRecorded == 0);
    CHECK(pixel(after, 20, 10, 2) == "0 255 0 255");
    CHECK(pixel(after, 20, 10, 15) == "255 0 0 255");
    CHECK(pixel(after, 20, 10, 30) == "0 0 0 0");
    CHECK(verified(f.tree));
    REQUIRE(prLayoutNodeScrollInfo(f.tree, s, &info));
    CHECK(prLayoutUnitToPx(info.offset.y) == 30.0f);

    // A scene holds no offsets: a player plays it at the player's offsets, and the tree's offsets are the tree's own.
    const PrLayoutScene* scene = prLayoutTreeScene(f.tree);
    CHECK(prLayoutSceneHasScroll(scene, s));
    std::vector<PrLayoutSceneScroll> scrolls = treeScrolls(f.tree, scene);
    REQUIRE(scrolls.size() == 2);
    CHECK(scrolls[0].node == kPrLayoutNodeNone);
    CHECK(scrolls[1].node == s);
    CHECK(scrolls[1].offset == px(0, 30));
    PrLayoutSceneScroll offset{s, PrLayoutVec{}};
    PrDrawList* list = prDrawListCreate();
    PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
    options.scrolls = &offset;
    options.scrollCount = 1;
    prLayoutScenePlay(scene, list, &options);
    std::vector<uint8_t> played(size_t(20) * 40 * 4, 0);
    PrDrawRenderer* renderer = prDrawCpuCreate(nullptr);
    PrPixels into{played.data(), size_t(20) * 4, {20, 40}};
    REQUIRE(prDrawRendererRenderPixels(renderer, list, into, PrPixelFormat_Rgba8) == PrStatus_Ok);
    prDrawRendererDestroy(renderer);
    prDrawListRelease(list);
    prLayoutSceneRelease(scene);
    CHECK(pixel(played, 20, 10, 10) == "255 0 0 255");
}

TEST_CASE("the viewport scrolls the document, as the root's overflow lets it") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutDecorationStyle green = background(PrColor{0, 1, 0, 1});
    f.element(root, block(50));
    PrLayoutNode last = f.element(root, block(50), &green);
    f.layout(px(20, 40));
    PrLayoutScrollInfo info{};
    REQUIRE(prLayoutNodeScrollInfo(f.tree, kPrLayoutNodeNone, &info));
    CHECK(prLayoutUnitToPx(info.range.bottom) == 60.0f);
    CHECK(info.userAxes == (PrLayoutScrollFlag_X | PrLayoutScrollFlag_Y));
    // The root itself is no scroll container: its overflow is the viewport's.
    REQUIRE(prLayoutNodeScrollInfo(f.tree, root, &info));
    CHECK_FALSE(info.scrollContainer);

    render(f.tree, 20, 40);
    prLayoutNodeSetScroll(f.tree, kPrLayoutNodeNone, px(0, 60));
    std::vector<uint8_t> pixels = render(f.tree, 20, 40);
    CHECK(pixel(pixels, 10, 10, 10) == "0 255 0 255");
    CHECK(prLayoutTreeStats(f.tree).layersRecorded == 0);
    // Geometry as painted is scrolled; as laid out it is not.
    CHECK(prLayoutRectToPx(prLayoutNodeBounds(f.tree, last, PrLayoutCoordinates_Viewport)).top == -10.0f);
    CHECK(prLayoutRectToPx(prLayoutNodeBounds(f.tree, last, PrLayoutCoordinates_Layout)).top == 50.0f);
    CHECK(f.hit(PrVec2{10, 10}) == last);

    // A root that clips lets script scroll the viewport, but not a user.
    PrLayoutBoxStyle hidden = block();
    hidden.overflowY = PrLayoutOverflow_Hidden;
    prLayoutNodeSetStyle(f.tree, root, f.style(hidden));
    f.layout(px(20, 40));
    REQUIRE(prLayoutNodeScrollInfo(f.tree, kPrLayoutNodeNone, &info));
    CHECK(info.userAxes == PrLayoutScrollFlag_X);
    CHECK(prLayoutUnitToPx(info.offset.y) == 60.0f);
}

TEST_CASE("geometry follows the boxes as they move, whether a query looks for one node or reads every box placed") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    std::vector<PrLayoutNode> blocks;
    for (int i = 0; i < 40; ++i)
        blocks.push_back(f.element(root, block(10)));
    PrLayoutNode last = blocks.back();
    // The first queries look up one node's boxes each, and the rest read every placed box once, when that costs less.
    auto checkAll = [&](double first) {
        for (PrLayoutCoordinates coordinates : {PrLayoutCoordinates_Viewport, PrLayoutCoordinates_Layout}) {
            for (size_t i = 0; i < blocks.size(); ++i) {
                double top = i == 0 ? 0 : first + 10 * double(i - 1);
                double bottom = i == 0 ? first : top + 10;
                checkRect(prLayoutRectToPx(prLayoutNodeBounds(f.tree, blocks[i], coordinates)), 0, top, 100, bottom);
            }
        }
    };
    f.layout(px(100, 100));
    checkAll(10);
    CHECK(verified(f.tree));

    // A layout that moves boxes moves what queries answer, and one that moves none keeps it.
    prLayoutNodeSetStyle(f.tree, blocks[0], f.style(block(30)));
    f.layout(px(100, 100));
    checkAll(30);
    f.layout(px(100, 100));
    checkAll(30);
    CHECK(verified(f.tree));

    // A paint changes no box, and a transform moves one in the viewport without a layout.
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    prLayoutNodeSetStyle(f.tree, last, f.style(block(10), &red));
    checkAll(30);
    PrLayoutEffectStyle moved = prLayoutEffectStyleDefault();
    moved.transform = prLayoutTransformFromAffine2(prAffine2Translate(PrVec2{5, 0}));
    prLayoutNodeSetStyle(f.tree, last, f.style(block(10), &red, &moved));
    checkRect(f.bounds(last), 5, 410, 105, 420);
    checkRect(prLayoutRectToPx(prLayoutNodeBounds(f.tree, last, PrLayoutCoordinates_Layout)), 0, 410, 100, 420);
    CHECK(verified(f.tree));
    for (PrLayoutNode node : blocks)
        f.bounds(node);

    // A scroll moves boxes in the viewport, and not as laid out.
    prLayoutNodeSetScroll(f.tree, kPrLayoutNodeNone, px(0, 50));
    checkRect(f.bounds(last), 5, 360, 105, 370);
    checkRect(prLayoutRectToPx(prLayoutNodeBounds(f.tree, last, PrLayoutCoordinates_Layout)), 0, 410, 100, 420);
    CHECK(verified(f.tree));
}

TEST_CASE("geometry as laid out is before transforms, and hit testing lists every node at a point") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle box = block(20);
    box.width = prLayoutLengthPx(20);
    PrLayoutEffectStyle effects = prLayoutEffectStyleDefault();
    effects.transform = prLayoutTransformFromAffine2(prAffine2Translate(PrVec2{5, 0}));
    PrLayoutNode moved = f.element(root, box, nullptr, &effects);
    PrLayoutNode inner = f.element(moved, block(10));
    f.layout(px(100, 100));
    checkRect(prLayoutRectToPx(prLayoutNodeBounds(f.tree, moved, PrLayoutCoordinates_Viewport)), 5, 0, 25, 20);
    checkRect(prLayoutRectToPx(prLayoutNodeBounds(f.tree, moved, PrLayoutCoordinates_Layout)), 0, 0, 20, 20);
    PrLayoutNode hits[4] = {};
    REQUIRE(prLayoutTreeHitTest(f.tree, px(10, 5), hits, 4) == 3);
    CHECK(hits[0] == inner);
    CHECK(hits[1] == moved);
    CHECK(hits[2] == root);
    CHECK(prLayoutTreeHitTest(f.tree, px(2, 5), nullptr, 0) == 1);
}

TEST_CASE("a box whose pointer events are none is not hit, nor is its text, and a box inside it that takes them is") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutColorStyle none = prLayoutColorStyleDefault();
    none.pointerEvents = PrLayoutPointerEvents_None;
    PrLayoutBoxStyle box = block(40);
    PrLayoutStyle record = prLayoutStyleDefault();
    record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(f.tree, &box);
    record.groups[PrLayoutGroup_Color] = prLayoutColorStyleIntern(f.tree, &none);
    PrLayoutNode outer = prLayoutNodeCreateElement(f.tree, root, prLayoutStyleIntern(f.tree, &record));
    PrLayoutNode inner = f.element(outer, block(10));
    PrLayoutNode words = f.text(outer, "words");
    f.layout(px(100, 100));
    PrRect text = f.bounds(words);
    REQUIRE(text.top >= 10);
    CHECK(f.hit(PrVec2{5, 5}) == inner);
    CHECK(f.hit(PrVec2{text.left + 1, text.top + 1}) == root);
    CHECK(f.hit(PrVec2{90, 35}) == root);
}

TEST_CASE("a box with inline-size containment shrink-wraps to its contained size or characters, not its content") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    auto contained = [&](float intrinsic, uint16_t characters, float width) {
        PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
        box.display = PrLayoutDisplay_InlineBlock;
        box.containInline = 1;
        box.containIntrinsicWidth = intrinsic >= 0 ? prLayoutLengthPx(intrinsic) : prLayoutLengthNone();
        box.intrinsicCharacters = characters;
        if (width >= 0)
            box.width = prLayoutLengthPx(width);
        PrLayoutNode node = f.element(root, box);
        f.text(node, "a long run of content that the containment ignores");
        return node;
    };
    PrLayoutNode sized = contained(30, 0, -1);
    PrLayoutNode none = contained(-1, 0, -1);
    PrLayoutNode one = contained(-1, 1, -1);
    PrLayoutNode twenty = contained(-1, 20, -1);
    PrLayoutNode authored = contained(30, 20, 70);
    f.layout(px(2000, 400));
    auto width = [&](PrLayoutNode node) { return f.bounds(node).right - f.bounds(node).left; };
    CHECK(width(sized) == doctest::Approx(30));
    CHECK(width(none) == doctest::Approx(0));
    // n characters are n - 1 average advances and one maximum advance.
    CHECK(width(one) > 0);
    PrTextFont* font = textFontOf(prFontStyleDefault().size);
    PrFontMetrics metrics = prFontMetrics(prTextFontMatchedFont(font, prTextFontFirst(font)));
    float size = prTextFontStyle(font).size;
    CHECK(width(twenty) - width(one) == doctest::Approx(19 * metrics.averageAdvance * size).epsilon(0.01));
    CHECK(width(authored) == doctest::Approx(70));
}

TEST_CASE("text runs on either side of an inline box meet it") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode before = f.text(root, "with ");
    PrLayoutNode span = f.element(root, inlineBox());
    PrLayoutNode inside = f.text(span, "bold");
    PrLayoutNode after = f.text(root, ", and");
    f.layout(px(300, 100));
    PrRect a = f.bounds(before);
    PrRect s = f.bounds(span);
    PrRect i = f.bounds(inside);
    PrRect b = f.bounds(after);
    CHECK(double(s.left) == doctest::Approx(double(a.right)));
    CHECK(double(i.right) == doctest::Approx(double(s.right)));
    CHECK(double(b.left) == doctest::Approx(double(s.right)));
}

TEST_CASE("a layout keeps the results of what has not changed") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode a = f.element(root, block(10));
    PrLayoutNode b = f.element(root, block());
    PrLayoutNode text = f.text(b, "some words");
    PrLayoutNode c = f.element(root, block(30));
    f.layout(px(200, 100));
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 4);
    CHECK(verified(f.tree));

    // Nothing changed: the root is clean, so the layout reaches no box and the last result stands.
    f.layout(px(200, 100));
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);
    CHECK(prLayoutTreeStats(f.tree).blocksReused == 0);
    CHECK(verified(f.tree));

    // A leaf's size: the leaf and the root are laid out again, and its siblings are kept.
    prLayoutNodeSetStyle(f.tree, a, f.style(block(20)));
    f.layout(px(200, 100));
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 2);
    CHECK(prLayoutTreeStats(f.tree).blocksReused == 2);
    CHECK(f.bounds(c).top == 20.0f + f.bounds(b).bottom - f.bounds(b).top);
    CHECK(verified(f.tree));

    // A background alone changes no layout.
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    prLayoutNodeSetStyle(f.tree, c, f.style(block(30), &red));
    f.layout(px(200, 100));
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);

    // Text lays out its block container again, and the lines with it.
    prLayoutNodeSetText(f.tree, text, "other words");
    f.layout(px(200, 100));
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 2);
    CHECK(prLayoutTreeStats(f.tree).linesLaidOut == 1);
    CHECK(verified(f.tree));

    // A new child lays out its parent, and a new width lays out every box whose space it changes.
    PrLayoutNode d = f.element(root, block(5));
    f.layout(px(200, 100));
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 2);
    CHECK(f.bounds(d).bottom == f.bounds(root).bottom);
    f.layout(px(150, 100));
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 5);
    CHECK(verified(f.tree));
}

TEST_CASE("a box keeps its result through its container's resize where it resolves to the same inline size") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    // A box of fixed width, and one whose auto width its max-width caps, each centred by auto margins.
    PrLayoutBoxStyle fixed = block();
    fixed.width = prLayoutLengthPx(50);
    PrLayoutNode a = f.element(root, fixed);
    f.text(a, "words");
    PrLayoutBoxStyle capped = block();
    capped.maxWidth = prLayoutLengthPx(60);
    capped.margin[PrRectSide_Left] = prLayoutLengthAuto();
    capped.margin[PrRectSide_Right] = prLayoutLengthAuto();
    PrLayoutNode b = f.element(root, capped);
    f.text(b, "words");
    // A box whose margins leave it no width at either size.
    PrLayoutBoxStyle squeezed = block(10);
    squeezed.margin[PrRectSide_Left] = prLayoutLengthPx(150);
    squeezed.margin[PrRectSide_Right] = prLayoutLengthPx(150);
    PrLayoutNode c = f.element(root, squeezed);
    f.element(c, block(5));
    // A box whose padding is a percentage of its containing block's width, and one of auto width.
    PrLayoutBoxStyle padded = block(10);
    padded.width = prLayoutLengthPx(50);
    padded.padding[PrRectSide_Left] = prLayoutLengthPercent(10);
    PrLayoutNode d = f.element(root, padded);
    PrLayoutNode e = f.element(root, block(10));
    f.layout(px(200, 100));
    CHECK(verified(f.tree));

    // The root and the boxes whose width follows it are laid out again; the others are kept.
    f.layout(px(250, 100));
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 3);
    CHECK(prLayoutTreeStats(f.tree).blocksReused == 3);
    CHECK(f.bounds(a).right - f.bounds(a).left == 50.0f);
    CHECK(f.bounds(b).left == 95.0f);
    CHECK(f.bounds(c).right - f.bounds(c).left == 0.0f);
    CHECK(f.bounds(d).right - f.bounds(d).left == 75.0f);
    CHECK(f.bounds(e).right - f.bounds(e).left == 250.0f);
    CHECK(verified(f.tree));

    // Back again, from the results kept at the new size.
    f.layout(px(200, 100));
    CHECK(prLayoutTreeStats(f.tree).blocksReused == 3);
    CHECK(f.bounds(b).left == 70.0f);
    CHECK(verified(f.tree));
}

TEST_CASE("verifying a tree publishes nothing: its placements, queries and recordings stand, whatever it finds") {
    Fixture f;
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), &red);
    PrLayoutBoxStyle relative = block(30);
    relative.position = PrLayoutPosition_Relative;
    relative.padding[PrRectSide_Left] = prLayoutLengthPercent(10);
    PrLayoutNode holder = f.element(root, relative, &red);
    PrLayoutBoxStyle absolute = block(10);
    absolute.position = PrLayoutPosition_Absolute;
    absolute.width = prLayoutLengthPx(10);
    f.element(holder, absolute, &red);
    PrLayoutBoxStyle fixed = absolute;
    fixed.position = PrLayoutPosition_Fixed;
    f.element(root, fixed, &red);
    PrLayoutNode paragraph = f.element(root, block());
    f.text(paragraph, "some words ");
    PrLayoutBoxStyle atomic = block(10);
    atomic.display = PrLayoutDisplay_InlineBlock;
    atomic.width = prLayoutLengthPx(20);
    f.element(paragraph, atomic, &red);
    f.layout(px(100, 100));
    render(f.tree, 100, 100);

    // Each node's placement, the fragment it holds, and what queries answer of it.
    struct Published {
        LayoutPlacement placement;
        PrLayoutRect bounds;
        PrLayoutVec content;
    };
    LayoutTree* t = f.tree;
    auto published = [&] {
        std::vector<Published> out;
        for (LayoutNodeIndex node = 0; node < t->nodes.count; ++node) {
            if (!t->nodes.data[node].live)
                continue;
            PrLayoutNode name = layoutNodeName(t, node);
            out.push_back(Published{layoutCachePlacement(t->cache, node),
                                    prLayoutNodeBounds(f.tree, name, PrLayoutCoordinates_Viewport),
                                    prLayoutNodeContentExtent(f.tree, name)});
        }
        return out;
    };
    auto same = [](const std::vector<Published>& a, const std::vector<Published>& b) {
        bool equal = a.size() == b.size();
        for (size_t i = 0; i < a.size() && equal; ++i) {
            const LayoutPlacement& x = a[i].placement;
            const LayoutPlacement& y = b[i].placement;
            equal = x.parent == y.parent && x.offset == y.offset && x.fragment == y.fragment
                    && memcmp(&a[i].bounds, &b[i].bounds, sizeof(PrLayoutRect)) == 0 && a[i].content == b[i].content;
        }
        return equal;
    };
    std::vector<Published> before = published();
    CHECK(verified(f.tree));
    CHECK(same(before, published()));
    render(f.tree, 100, 100);
    CHECK(prLayoutTreeStats(f.tree).layersRecorded == 0);

    // A placement made wrong fails verification, which leaves the placement as it found it.
    LayoutPlacement& placement = t->cache.placements.data[layoutNodeIndex(t, holder)];
    placement.offset.x = placement.offset.x + PrLayoutUnit{1};
    before = published();
    CHECK(!prLayoutTreeVerify(f.tree, nullptr));
    CHECK(same(before, published()));
}

TEST_CASE("an inline context keeps its shaped text while only its width or what it paints changes") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode paragraph = f.element(root, block());
    PrLayoutNode words = f.text(paragraph, "some words that break across lines ");
    PrLayoutBoxStyle padded = inlineBox();
    padded.padding[PrRectSide_Left] = prLayoutLengthPercent(10);
    PrLayoutNode span = f.element(paragraph, padded);
    f.text(span, "and a span");
    f.layout(px(200, 100));
    CHECK(prLayoutTreeStats(f.tree).inlinesShaped == 1);

    // A new width breaks the lines again and resolves the box's percentage, from the shaped text kept.
    f.layout(px(90, 100));
    CHECK(prLayoutTreeStats(f.tree).inlinesShaped == 0);
    CHECK(prLayoutTreeStats(f.tree).linesLaidOut > 1);
    CHECK(verified(f.tree));

    // A color is no change to the text. New text, or a removed node, shapes it again.
    PrLayoutDecorationStyle green = background(PrColor{0, 1, 0, 1});
    prLayoutNodeSetStyle(f.tree, span, f.style(padded, &green));
    f.layout(px(100, 100));
    CHECK(prLayoutTreeStats(f.tree).inlinesShaped == 0);
    CHECK(verified(f.tree));
    prLayoutNodeSetText(f.tree, words, "other words ");
    f.layout(px(100, 100));
    CHECK(prLayoutTreeStats(f.tree).inlinesShaped == 1);
    prLayoutTreeErase(f.tree, span);
    f.layout(px(100, 100));
    CHECK(prLayoutTreeStats(f.tree).inlinesShaped == 1);
    CHECK(verified(f.tree));
}

TEST_CASE("a paint keeps the recordings of the boxes with layers of their own where nothing they paint changed") {
    Fixture f;
    PrLayoutDecorationStyle white = background(PrColor{1, 1, 1, 1});
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), &white);
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    PrLayoutNode plain = f.element(root, block(10), &red);
    PrLayoutBoxStyle relative = block(10);
    relative.position = PrLayoutPosition_Relative;
    PrLayoutNode positioned = f.element(root, relative, &red);
    PrLayoutEffectStyle half = prLayoutEffectStyleDefault();
    half.opacity = 0.5f;
    PrLayoutNode faded = f.element(root, block(10), &red, &half);
    f.text(faded, "words");
    auto paint = [&](uint32_t width = 40, uint32_t height = 40) {
        f.layout(px(float(width), float(height)));
        std::vector<uint8_t> pixels = render(f.tree, width, height);
        CHECK(verified(f.tree));
        return pixels;
    };

    // The root and the box with opacity each record once, and then are cached. The positioned box paints whole in the
    // root's recording.
    paint();
    CHECK(prLayoutTreeStats(f.tree).layersRecorded == 2);
    paint();
    CHECK(prLayoutTreeStats(f.tree).layersRecorded == 0);
    CHECK(prLayoutTreeStats(f.tree).layersReused == 2);

    // A background in the root's flow records the root's layer again, as a background of the positioned box does.
    PrLayoutDecorationStyle green = background(PrColor{0, 1, 0, 1});
    prLayoutNodeSetStyle(f.tree, plain, f.style(block(10), &green));
    std::vector<uint8_t> pixels = paint();
    CHECK(prLayoutTreeStats(f.tree).layersRecorded == 1);
    CHECK(pixel(pixels, 40, 5, 5) == "0 255 0 255");
    prLayoutNodeSetStyle(f.tree, positioned, f.style(relative, &green));
    pixels = paint();
    CHECK(prLayoutTreeStats(f.tree).layersRecorded == 1);
    CHECK(pixel(pixels, 40, 5, 15) == "0 255 0 255");

    // Opacity applies around the recording, and records nothing again.
    PrLayoutEffectStyle most = prLayoutEffectStyleDefault();
    most.opacity = 0.75f;
    prLayoutNodeSetStyle(f.tree, faded, f.style(block(10), &red, &most));
    pixels = paint();
    CHECK(prLayoutTreeStats(f.tree).layersRecorded == 0);
    CHECK(pixel(pixels, 40, 39, 22) == "255 64 64 255");

    // A layout that moves a box with a layer keeps its recording, which is in its own space.
    prLayoutNodeSetStyle(f.tree, plain, f.style(block(12), &green));
    paint();
    CHECK(prLayoutTreeStats(f.tree).layersRecorded == 1);
    CHECK(prLayoutTreeStats(f.tree).layersReused == 1);

    // Full opacity paints the box in its parent's flow, which records the parent's again.
    prLayoutNodeSetStyle(f.tree, faded, f.style(block(10), &red));
    paint();
    CHECK(prLayoutTreeStats(f.tree).layersRecorded == 1);
    CHECK(prLayoutTreeStats(f.tree).layersReused == 0);
}

TEST_CASE("recording a box that paints whole again embeds the recordings of its block subtrees that did not change") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    PrLayoutNode first = kPrLayoutNodeNone;
    for (int s = 0; s < 3; ++s) {
        PrLayoutNode section = f.element(root, block());
        for (int b = 0; b < 40; ++b) {
            PrLayoutNode leaf = f.element(section, block(1), &red);
            if (s == 0 && b == 0)
                first = leaf;
        }
    }
    auto paint = [&]() {
        f.layout(px(40, 130));
        std::vector<uint8_t> pixels = render(f.tree, 40, 130);
        CHECK(verified(f.tree));
        return pixels;
    };
    // Each section is cached for each of its 2 phases, its blocks and its inline content, as it holds no floats. Its 40
    // leaves are in two runs, of 32 and 8, each cached for each phase, whether it draws or not. Each leaf draws a
    // background, so its block phase is cached too.
    paint();
    CHECK(prLayoutTreeStats(f.tree).subtreesRecorded == 3 * (2 + 2 * 2) + 3 * 40);

    // Changing one leaf's background re-records the root's layer, the leaf, its section's 2 phases, and the first run
    // of each phase. The other sections, the section's second run in each phase, and the 31 other leaves of the first
    // run are reused.
    PrLayoutDecorationStyle green = background(PrColor{0, 1, 0, 1});
    prLayoutNodeSetStyle(f.tree, first, f.style(block(1), &green));
    std::vector<uint8_t> pixels = paint();
    CHECK(prLayoutTreeStats(f.tree).layersRecorded == 1);
    CHECK(prLayoutTreeStats(f.tree).subtreesRecorded == 1 + 2 + 2);
    CHECK(prLayoutTreeStats(f.tree).subtreesReused == 2 * 2 + 2 + 31);
    CHECK(pixel(pixels, 40, 5, 0) == "0 255 0 255");
    CHECK(pixel(pixels, 40, 5, 60) == "255 0 0 255");

    // Sections that a change above them moves reuse their recordings at their new position, and so do the second runs
    // of the section that changed, whose leaves all moved.
    prLayoutNodeSetStyle(f.tree, first, f.style(block(3), &green));
    pixels = paint();
    CHECK(prLayoutTreeStats(f.tree).subtreesReused == 2 * 2 + 2 + 31);
    CHECK(pixel(pixels, 40, 5, 2) == "0 255 0 255");
    CHECK(pixel(pixels, 40, 5, 121) == "255 0 0 255");
    CHECK(pixel(pixels, 40, 5, 123) == "0 0 0 0");
}

TEST_CASE("a positioned box without a layer reuses its cached recording when a sibling changes") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    PrLayoutBoxStyle relative = block(10);
    relative.position = PrLayoutPosition_Relative;
    std::vector<PrLayoutNode> cards;
    for (int i = 0; i < 4; ++i)
        cards.push_back(f.element(root, relative, &red));
    auto paint = [&]() {
        f.layout(px(40, 40));
        std::vector<uint8_t> pixels = render(f.tree, 40, 40);
        CHECK(verified(f.tree));
        return pixels;
    };
    paint();
    CHECK(prLayoutTreeStats(f.tree).subtreesRecorded == 4);

    // Changing one card re-records the root's layer and that card. The other three replay from cache.
    PrLayoutDecorationStyle green = background(PrColor{0, 1, 0, 1});
    prLayoutNodeSetStyle(f.tree, cards[1], f.style(relative, &green));
    std::vector<uint8_t> pixels = paint();
    CHECK(prLayoutTreeStats(f.tree).layersRecorded == 1);
    CHECK(prLayoutTreeStats(f.tree).subtreesRecorded == 1);
    CHECK(prLayoutTreeStats(f.tree).subtreesReused == 3);
    CHECK(pixel(pixels, 40, 5, 15) == "0 255 0 255");
    CHECK(pixel(pixels, 40, 5, 25) == "255 0 0 255");
}

TEST_CASE("a layout shapes the inline formatting contexts that changed, and nothing else") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle span = prLayoutBoxStyleDefault();
    span.display = PrLayoutDisplay_Inline;
    std::vector<PrLayoutNode> texts;
    for (int p = 0; p < 64; ++p) {
        PrLayoutNode paragraph = f.element(root, block());
        texts.push_back(f.text(paragraph, "Some words that break across lines "));
        PrLayoutNode inner = f.element(paragraph, span);
        f.text(inner, "and more words in a span");
    }
    f.layout(px(120, 600));
    CHECK(prLayoutTreeStats(f.tree).inlinesShaped == 64);
    CHECK(verified(f.tree));

    // What changed is shaped again, and nothing else.
    for (size_t i = 0; i < texts.size(); i += 2)
        prLayoutNodeSetText(f.tree, texts[i], "Other words, which break otherwise ");
    f.layout(px(120, 600));
    CHECK(prLayoutTreeStats(f.tree).inlinesShaped == 32);
    CHECK(verified(f.tree));

    // A new width shapes nothing.
    f.layout(px(90, 600));
    CHECK(prLayoutTreeStats(f.tree).inlinesShaped == 0);
    CHECK(verified(f.tree));

    // A paragraph's font is its own text's, which is shaped again, once.
    PrLayoutFontStyle large = prLayoutFontStyleDefault();
    large.font = textFontOf(40);
    PrLayoutNode paragraph = prLayoutNodeParent(f.tree, texts[1]);
    prLayoutNodeSetStyle(f.tree, paragraph, f.style(block(), nullptr, nullptr, &large));
    f.layout(px(90, 600));
    CHECK(prLayoutTreeStats(f.tree).inlinesShaped == 1);
    CHECK(verified(f.tree));
}

TEST_CASE("what paints and hit tests keep of the fragments a layout replaces is freed with them") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    PrLayoutNode leaf = kPrLayoutNodeNone;
    for (int s = 0; s < 3; ++s) {
        PrLayoutNode section = f.element(root, block());
        for (int b = 0; b < 40; ++b) {
            leaf = f.element(section, block(1), &red);
            f.text(leaf, "words");
        }
    }
    // The last section is laid out and recorded again each time, in every phase, and hit tested.
    auto cycle = [&](float height) {
        prLayoutNodeSetStyle(f.tree, leaf, f.style(block(height), &red));
        f.layout(px(40, 200));
        prLayoutSceneRelease(prLayoutTreeScene(f.tree));
        PrLayoutNode hit = kPrLayoutNodeNone;
        prLayoutTreeHitTest(f.tree, px(5, 100), &hit, 1);
    };
    cycle(2);
    cycle(1);
    size_t live = prMemoryLiveCount();
    for (int i = 0; i < 4; ++i) {
        cycle(2);
        cycle(1);
    }
    CHECK(prMemoryLiveCount() == live);
    CHECK(verified(f.tree));
}

TEST_CASE("a worker lays out a tree and records its scene while the caller's thread goes on") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutDecorationStyle green = background(PrColor{0, 1, 0, 1});
    PrLayoutNode box = f.element(root, block(10), &green);
    f.text(root, "words to set");
    PrLayoutWorker* worker = prLayoutWorkerCreate();
    PrDrawList* list = prDrawListCreate();
    for (uint32_t frame = 0; frame < 20; ++frame) {
        // The caller changes the tree between jobs, and the worker lays it out and records it during a job. The scene
        // of the last job plays while the next one runs.
        prLayoutNodeSetStyle(f.tree, box, f.style(block(float(10 + frame)), &green));
        const PrLayoutScene* scene = prLayoutWorkerTakeScene(worker, f.tree);
        CHECK((scene != nullptr) == (frame != 0));
        PrLayoutRoot viewport{f.root, px(100, 100)};
        prLayoutWorkerPost(worker, f.tree, &viewport, 1);
        if (scene != nullptr) {
            prDrawListClear(list);
            prLayoutScenePlay(scene, list, nullptr);
            prLayoutSceneRelease(scene);
        }
        prLayoutWorkerWait(worker);
        CHECK_FALSE(prLayoutWorkerIsBusy(worker));
        CHECK(f.bounds(box).bottom == float(10 + frame));
        CHECK(verified(f.tree));
    }
    CHECK(prDrawListBounds(list).right > 0.0f);
    prDrawListRelease(list);
    prLayoutWorkerDestroy(worker);
}

TEST_CASE("one worker lays out the trees posted to it in turn, and keeps each tree's scene") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    f.element(root, block(10));
    Fixture g;
    PrLayoutTree* other = g.tree;
    PrLayoutNode otherRoot = prLayoutNodeCreateElement(other, kPrLayoutNodeNone, g.style(block(30)));
    PrLayoutWorker* worker = prLayoutWorkerCreate();
    PrLayoutRoot viewport{root, px(100, 100)};
    PrLayoutRoot otherViewport{otherRoot, px(50, 50)};
    prLayoutWorkerPost(worker, f.tree, &viewport, 1);
    prLayoutWorkerPost(worker, other, &otherViewport, 1);
    prLayoutWorkerWait(worker);
    CHECK_FALSE(prLayoutWorkerIsBusy(worker));
    CHECK(f.bounds(root).bottom == 10.0f);
    CHECK(prLayoutNodeBounds(other, otherRoot, PrLayoutCoordinates_Layout).bottom.raw == 30 * kPrLayoutUnitsPerPx);
    const PrLayoutScene* scene = prLayoutWorkerTakeScene(worker, f.tree);
    const PrLayoutScene* otherScene = prLayoutWorkerTakeScene(worker, other);
    CHECK(scene != nullptr);
    CHECK(otherScene != nullptr);
    CHECK(scene != otherScene);
    CHECK(prLayoutWorkerTakeScene(worker, f.tree) == nullptr);
    prLayoutSceneRelease(scene);
    prLayoutSceneRelease(otherScene);
    // What the worker keeps for a tree that it is told to forget is dropped with it.
    prLayoutWorkerPost(worker, other, &otherViewport, 1);
    prLayoutWorkerForget(worker, other);
    prLayoutWorkerDestroy(worker);
}

TEST_CASE("a compositor plays a scene with its tracks' effects, and no paint") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    PrLayoutEffectStyle layer = prLayoutEffectStyleDefault();
    layer.layer = true;
    PrLayoutNode box = f.element(root, block(10), &red, &layer);
    f.layout(px(20, 20));
    const PrLayoutScene* scene = prLayoutTreeScene(f.tree);
    CHECK(prLayoutSceneLayerCount(scene) == 2);
    CHECK(prLayoutSceneHasLayer(scene, box));

    // The box fades out over a second.
    PrLayoutKeyframe frames[2] = {};
    frames[0].offset = 0;
    frames[0].opacity = 1;
    frames[0].easing = prLayoutEasingLinear();
    frames[1].offset = 1;
    frames[1].opacity = 0;
    frames[1].easing = prLayoutEasingLinear();
    PrLayoutTrack track{};
    track.node = box;
    track.effect = PrLayoutEffect_Opacity;
    track.timing = prLayoutTimingDefault();
    track.timing.duration = 1000;
    track.keyframes = frames;
    track.keyframeCount = 2;

    Host host;
    PrLayoutCompositorDesc desc{.context = &host, .now = hostNow, .present = hostPresent};
    PrLayoutCompositor* compositor = prLayoutCompositorCreate(&desc);
    prLayoutCompositorFrame(compositor);
    CHECK(host.presented == 0);
    CHECK_FALSE(prLayoutCompositorWantsFrame(compositor));
    commitTree(compositor, f.tree, scene, 0, 0, &track, 1);
    prLayoutSceneRelease(scene);
    // A commit wants a frame, and so does a track that moves as the clock advances.
    CHECK(prLayoutCompositorWantsFrame(compositor));
    prLayoutCompositorFrame(compositor);
    CHECK(pixel(host.pixels, 20, 5, 5) == "255 0 0 255");
    CHECK(prLayoutCompositorWantsFrame(compositor));
    host.now = 500;
    prLayoutCompositorFrame(compositor);
    CHECK(pixel(host.pixels, 20, 5, 5) == "128 0 0 128");
    CHECK(prLayoutCompositorStats(compositor).sampled == 1);
    // After the track ends, the scene's own opacity applies, and nothing moves.
    host.now = 2000;
    prLayoutCompositorFrame(compositor);
    CHECK(pixel(host.pixels, 20, 5, 5) == "255 0 0 255");
    CHECK(prLayoutCompositorStats(compositor).frames == 3);
    CHECK_FALSE(prLayoutCompositorWantsFrame(compositor));
    prLayoutCompositorDestroy(compositor);
}

TEST_CASE("a compositor with a thread presents frames while the caller's thread commits") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutDecorationStyle green = background(PrColor{0, 1, 0, 1});
    PrLayoutNode box = f.element(root, block(10), &green);
    Host host;
    PrLayoutCompositorDesc desc{
        .context = &host, .now = hostNow, .present = [](void* PR_NULLABLE, PrDrawList*, PrColor16) {}, .interval = 1};
    PrLayoutCompositor* compositor = prLayoutCompositorCreate(&desc);
    PrLayoutScrollFn apply = [](void* PR_NULLABLE context, const PrLayoutScroll* scroll) {
        prLayoutNodeSetScroll(static_cast<PrLayoutTree*>(context), scroll->node, scroll->offset);
    };
    uint32_t applied = 0;
    for (uint32_t i = 0; i < 20; ++i) {
        // The page is taller than the viewport, and the caller's thread scrolls it as a user would.
        prLayoutNodeSetStyle(f.tree, box, f.style(block(float(25 + i)), &green));
        f.layout(px(20, 20));
        const PrLayoutScene* scene = prLayoutTreeScene(f.tree);
        commitTree(compositor, f.tree, scene, 0, applied);
        prLayoutSceneRelease(scene);
        prLayoutCompositorScrollAt(compositor, px(5, 5), px(0, 1));
        applied = prLayoutCompositorTakeScrolls(compositor, apply, f.tree);
    }
    CHECK(prLayoutUnitToPx(prLayoutNodeScroll(f.tree, kPrLayoutNodeNone).y) > 0.0f);
    while (prLayoutCompositorStats(compositor).frames < 3) {
    }
    CHECK(prLayoutCompositorStats(compositor).commits == 20);
    prLayoutCompositorDestroy(compositor);
}

TEST_CASE("a compositor's own scroll moves with the scene that scroll anchoring adjusted, so the user's scroll stays") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode first = f.element(root, block(10));
    for (uint32_t i = 0; i < 10; ++i)
        f.element(root, block(10));
    f.layout(px(20, 20));
    Host host;
    PrLayoutCompositorDesc desc{.context = &host, .now = hostNow, .present = hostPresent, .wantFrame = hostWantFrame};
    PrLayoutCompositor* compositor = prLayoutCompositorCreate(&desc);
    PrLayoutScrollFn apply = [](void* PR_NULLABLE context, const PrLayoutScroll* scroll) {
        prLayoutNodeSetScroll(static_cast<PrLayoutTree*>(context), scroll->node, scroll->offset);
    };
    auto commit = [&](uint32_t applied) {
        const PrLayoutScene* scene = prLayoutTreeScene(f.tree);
        commitTree(compositor, f.tree, scene, 0, applied);
        prLayoutSceneRelease(scene);
    };
    commit(0);
    // The client applies the user's first scroll, and the user scrolls on before the client takes it.
    prLayoutCompositorScrollAt(compositor, px(5, 5), px(0, 30));
    uint32_t applied = prLayoutCompositorTakeScrolls(compositor, apply, f.tree);
    f.layout(px(20, 20));
    commit(applied);
    prLayoutCompositorScrollAt(compositor, px(5, 5), px(0, 10));
    // The block above grows, and anchoring moves the client's offset with what it shows.
    prLayoutNodeSetStyle(f.tree, first, f.style(block(15)));
    f.layout(px(20, 20));
    CHECK(prLayoutUnitToPx(prLayoutNodeScroll(f.tree, kPrLayoutNodeNone).y) == 35);
    commit(applied);
    // The compositor's own offset, which the client's gives way to, moves by as much.
    prLayoutCompositorTakeScrolls(compositor, apply, f.tree);
    CHECK(prLayoutUnitToPx(prLayoutNodeScroll(f.tree, kPrLayoutNodeNone).y) == 45);
    prLayoutCompositorDestroy(compositor);
}

TEST_CASE("the wheel waits over a box whose wheel blocks and what its descendants reach, one sequence at a time") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutColorStyle blocking = prLayoutColorStyleDefault();
    blocking.wheelEvents = PrLayoutWheelEvents_Blocking;
    PrLayoutBoxStyle box = block(10);
    PrLayoutStyle record = prLayoutStyleDefault();
    record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(f.tree, &box);
    record.groups[PrLayoutGroup_Color] = prLayoutColorStyleIntern(f.tree, &blocking);
    PrLayoutNode waits = prLayoutNodeCreateElement(f.tree, root, prLayoutStyleIntern(f.tree, &record));
    // The child overflows its parent, and the region holds it.
    f.element(waits, block(14));
    f.element(root, block(20));
    f.layout(px(20, 20));
    Host host;
    PrLayoutCompositorDesc desc{.context = &host, .now = hostNow, .present = hostPresent};
    PrLayoutCompositor* compositor = prLayoutCompositorCreate(&desc);
    auto commit = [&] {
        const PrLayoutScene* scene = prLayoutTreeScene(f.tree);
        commitTree(compositor, f.tree, scene, 0, 0);
        prLayoutSceneRelease(scene);
    };
    commit();

    // A sequence keeps what its first wheel decided until the client releases it, and a pause starts a new one.
    CHECK(prLayoutCompositorWheelBlocks(compositor, px(5, 5)));
    host.now = 100;
    CHECK(prLayoutCompositorWheelBlocks(compositor, px(5, 5)));
    prLayoutCompositorReleaseWheel(compositor);
    host.now = 200;
    CHECK_FALSE(prLayoutCompositorWheelBlocks(compositor, px(5, 5)));
    host.now = 200 + kPrLayoutWheelSequenceGap + 1;
    CHECK(prLayoutCompositorWheelBlocks(compositor, px(5, 5)));
    // A wheel at another point starts a new sequence. The child's overflow is in the region, and past it is not.
    CHECK(prLayoutCompositorWheelBlocks(compositor, px(5, 12)));
    CHECK_FALSE(prLayoutCompositorWheelBlocks(compositor, px(5, 17)));

    // The region scrolls with its container.
    CHECK(prLayoutCompositorScrollAt(compositor, px(5, 5), px(0, 10)));
    CHECK_FALSE(prLayoutCompositorWheelBlocks(compositor, px(5, 12)));
    CHECK(prLayoutCompositorWheelBlocks(compositor, px(5, 2)));

    // A tree whose wheel blocks over the whole viewport blocks everywhere in it.
    prLayoutTreeSetViewportWheelEvents(f.tree, PrLayoutWheelEvents_Blocking);
    commit();
    CHECK(prLayoutCompositorWheelBlocks(compositor, px(5, 18)));
    prLayoutCompositorDestroy(compositor);
}

TEST_CASE("a scroll container whose overscroll behavior contains a scroll keeps what it cannot take from the viewport") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle scroller = block(10);
    scroller.overflowY = PrLayoutOverflow_Scroll;
    scroller.overscrollY = PrLayoutOverscroll_Contain;
    PrLayoutNode s = f.element(root, scroller);
    f.element(s, block(20));
    f.element(root, block(40));
    f.layout(px(20, 20));
    PrLayoutScrollInfo info{};
    REQUIRE(prLayoutNodeScrollInfo(f.tree, s, &info));
    CHECK(info.chainAxes == PrLayoutScrollFlag_X);
    Host host;
    PrLayoutCompositorDesc desc{.context = &host, .now = hostNow, .present = hostPresent};
    PrLayoutCompositor* compositor = prLayoutCompositorCreate(&desc);
    const PrLayoutScene* scene = prLayoutTreeScene(f.tree);
    commitTree(compositor, f.tree, scene, 0, 0);
    prLayoutSceneRelease(scene);
    // The container takes 10 of the 30 and keeps the rest from the viewport. Past the container, the viewport scrolls.
    CHECK(prLayoutCompositorScrollAt(compositor, px(5, 5), px(0, 30)));
    std::vector<PrLayoutScroll> taken;
    PrLayoutScrollFn record = [](void* PR_NULLABLE context, const PrLayoutScroll* scroll) {
        static_cast<std::vector<PrLayoutScroll>*>(context)->push_back(*scroll);
    };
    prLayoutCompositorTakeScrolls(compositor, record, &taken);
    REQUIRE(taken.size() == 1);
    CHECK(taken[0].node == s);
    CHECK(prLayoutUnitToPx(taken[0].offset.y) == 10);
    CHECK(prLayoutCompositorScrollAt(compositor, px(5, 15), px(0, 30)));
    taken.clear();
    prLayoutCompositorTakeScrolls(compositor, record, &taken);
    REQUIRE(taken.size() == 1);
    CHECK(taken[0].node == kPrLayoutNodeNone);
    prLayoutCompositorDestroy(compositor);
}

TEST_CASE("a compositor scrolls what a user scrolls, and hands the offsets back") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle scroller = block(10);
    scroller.overflowY = PrLayoutOverflow_Scroll;
    PrLayoutNode s = f.element(root, scroller);
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    PrLayoutDecorationStyle green = background(PrColor{0, 1, 0, 1});
    f.element(s, block(10), &red);
    f.element(s, block(10), &green);
    f.element(root, block(40));
    f.layout(px(20, 20));
    Host host;
    PrLayoutCompositorDesc desc{.context = &host, .now = hostNow, .present = hostPresent, .wantFrame = hostWantFrame};
    PrLayoutCompositor* compositor = prLayoutCompositorCreate(&desc);
    auto commit = [&](uint32_t applied) {
        const PrLayoutScene* scene = prLayoutTreeScene(f.tree);
        commitTree(compositor, f.tree, scene, 0, applied);
        prLayoutSceneRelease(scene);
    };
    commit(0);
    // The host hears once that a frame is wanted, however much changes before the frame.
    CHECK(host.wanted == 1);
    commit(0);
    CHECK(host.wanted == 1);
    prLayoutCompositorFrame(compositor);
    CHECK(pixel(host.pixels, 20, 5, 5) == "255 0 0 255");
    CHECK_FALSE(prLayoutCompositorWantsFrame(compositor));

    // The wheel over the container scrolls it, and past its range, scrolls the viewport around it. The scroll wants a
    // frame to show it, and a scroll that moves nothing does not.
    CHECK_FALSE(prLayoutCompositorScrollAt(compositor, px(5, 5), px(0, -15)));
    CHECK(host.wanted == 1);
    CHECK(prLayoutCompositorScrollAt(compositor, px(5, 5), px(0, 15)));
    CHECK(prLayoutCompositorWantsFrame(compositor));
    CHECK(host.wanted == 2);
    prLayoutCompositorFrame(compositor);
    CHECK_FALSE(prLayoutCompositorWantsFrame(compositor));
    CHECK(pixel(host.pixels, 20, 5, 5) == "0 0 0 0");
    CHECK(prLayoutCompositorStats(compositor).scrolled == 2);
    // A commit of offsets read before the client applied the compositor's offsets undoes none of them.
    commit(0);
    prLayoutCompositorFrame(compositor);
    CHECK(pixel(host.pixels, 20, 5, 0) == "0 255 0 255");
    CHECK(prLayoutCompositorStats(compositor).scrolled == 2);
    struct Taken {
        PrLayoutTree* tree;
        uint32_t count;
    } taken{f.tree, 0};
    PrLayoutScrollFn apply = [](void* PR_NULLABLE context, const PrLayoutScroll* scroll) {
        Taken* t = static_cast<Taken*>(context);
        prLayoutNodeSetScroll(t->tree, scroll->node, scroll->offset);
        ++t->count;
    };
    uint32_t applied = prLayoutCompositorTakeScrolls(compositor, apply, &taken);
    CHECK(taken.count == 2);
    CHECK(prLayoutUnitToPx(prLayoutNodeScroll(f.tree, s).y) == 10.0f);
    CHECK(prLayoutUnitToPx(prLayoutNodeScroll(f.tree, kPrLayoutNodeNone).y) == 5.0f);

    // A commit after the client applied them gives the client's offsets, which are the same.
    commit(applied);
    prLayoutCompositorFrame(compositor);
    CHECK(prLayoutCompositorStats(compositor).scrolled == 0);
    CHECK(pixel(host.pixels, 20, 5, 0) == "0 255 0 255");

    // The client's own scroll shows with no scene committed: the container is back at the top.
    prLayoutNodeSetScroll(f.tree, s, px(0, 0));
    PrLayoutSceneScroll back{s, px(0, 0)};
    prLayoutCompositorSetScrolls(compositor, 0, &back, 1, applied);
    uint32_t commits = prLayoutCompositorStats(compositor).commits;
    prLayoutCompositorFrame(compositor);
    CHECK(pixel(host.pixels, 20, 5, 0) == "255 0 0 255");
    CHECK(prLayoutCompositorStats(compositor).commits == commits);
    prLayoutCompositorDestroy(compositor);
}

TEST_CASE("content far down a document paints, places and hits as it does at the top") {
    // A box with text a quarter pixel below `spacer`, and room below it, with the viewport scrolled to the spacer's
    // end.
    struct Page {
        Fixture f;
        PrLayoutNode content = kPrLayoutNodeNone;
        explicit Page(float spacer) {
            PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
            f.element(root, block(spacer));
            PrLayoutBoxStyle box = block();
            setMargins(box, 0.25f, 0);
            PrLayoutDecorationStyle green = background(PrColor{0, 1, 0, 1});
            content = f.element(root, box, &green);
            f.text(content, "Ag");
            f.element(root, block(40));
            f.layout(px(40, 40));
            prLayoutNodeSetScroll(f.tree, kPrLayoutNodeNone, px(0, spacer));
        }
    };
    Page top(0);
    Page far(8000000);
    checkRect(far.f.bounds(far.content), 0, 0.25, 40, double(top.f.bounds(top.content).bottom));
    CHECK(far.f.hit(PrVec2{30, 1}) == far.content);
    CHECK(render(far.f.tree, 40, 40) == render(top.f.tree, 40, 40));
}

TEST_CASE("a compositor shows an embedded scene in its slot, and scrolls into it before the scene around it") {
    Fixture f;
    PrLayoutDecorationStyle blue = background(PrColor{0, 0, 1, 1});
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    f.element(root, block(4), &blue);
    PrLayoutBoxStyle slotBox = block();
    slotBox.width = prLayoutLengthPx(20);
    slotBox.height = prLayoutLengthPx(10);
    PrLayoutNode slot = replaced(f, root, slotBox, prLayoutNaturalSizeNone());
    PrLayoutContent embedded{};
    embedded.kind = PrLayoutContentKind_Embed;
    embedded.embed = 7;
    prLayoutNodeSetContent(f.tree, slot, &embedded);
    f.element(root, block(20));
    f.layout(px(20, 20));
    const PrLayoutScene* outer = prLayoutTreeScene(f.tree);
    PrLayoutSceneEmbed embeds[2] = {};
    REQUIRE(prLayoutSceneEmbeds(outer, embeds, 2) == 1);
    CHECK(embeds[0].embed == 7);
    CHECK(embeds[0].node == slot);
    CHECK(embeds[0].extent == px(20, 10));

    // The embedded document: red over green, twice as tall as its slot.
    Fixture document;
    PrLayoutTree* inner = document.tree;
    PrLayoutNode innerRoot = prLayoutNodeCreateElement(inner, kPrLayoutNodeNone, document.style(block()));
    PrLayoutDecorationStyle redFill = background(PrColor{1, 0, 0, 1});
    PrLayoutDecorationStyle greenFill = background(PrColor{0, 1, 0, 1});
    prLayoutNodeCreateElement(inner, innerRoot, document.style(block(10), &redFill));
    prLayoutNodeCreateElement(inner, innerRoot, document.style(block(10), &greenFill));
    PrLayoutRoot innerViewport{innerRoot, px(20, 10)};
    prLayoutTreeLayout(inner, &innerViewport, 1);
    const PrLayoutScene* innerScene = prLayoutTreeScene(inner);

    Host host;
    PrLayoutCompositorDesc desc{.context = &host, .now = hostNow, .present = hostPresent};
    PrLayoutCompositor* compositor = prLayoutCompositorCreate(&desc);
    commitTree(compositor, f.tree, outer, 0, 0);
    commitTree(compositor, inner, innerScene, 7, 0);
    prLayoutCompositorFrame(compositor);
    CHECK(pixel(host.pixels, 20, 5, 2) == "0 0 255 255");
    CHECK(pixel(host.pixels, 20, 5, 8) == "255 0 0 255");

    // A scroll over the slot scrolls the embedded document, and what it cannot take scrolls the document around it.
    CHECK(prLayoutCompositorScrollAt(compositor, px(5, 8), px(0, 10)));
    prLayoutCompositorFrame(compositor);
    CHECK(pixel(host.pixels, 20, 5, 8) == "0 255 0 255");
    CHECK(prLayoutCompositorScrollAt(compositor, px(5, 8), px(0, 4)));
    struct Taken {
        PrLayoutVec outer;
        PrLayoutVec inner;
    } taken{};
    prLayoutCompositorTakeScrolls(
        compositor,
        [](void* PR_NULLABLE context, const PrLayoutScroll* scroll) {
            Taken* t = static_cast<Taken*>(context);
            (scroll->embed == 7 ? t->inner : t->outer) = scroll->offset;
        },
        &taken);
    CHECK(taken.inner == px(0, 10));
    CHECK(taken.outer == px(0, 4));

    // A slot whose content is dropped shows nothing.
    prLayoutCompositorRemove(compositor, 7);
    prLayoutCompositorFrame(compositor);
    CHECK(pixel(host.pixels, 20, 5, 5) == "0 0 0 0");
    prLayoutCompositorDestroy(compositor);
    prLayoutSceneRelease(outer);
    prLayoutSceneRelease(innerScene);
}

TEST_CASE("a font set change that changes no answer shapes and lays out nothing") {
    Fixture f;
    // Two rules of one source, which share its font.
    const PrFont* cousine = fontFile("Cousine.ttf");
    addWeb(f, 1, cousine);
    addWeb(f, 2, cousine);
    prFontRelease(cousine);
    WebAndDefault nodes = webAndDefault(f);
    f.layout(px(400, 400));
    float width = prRectWidth(f.bounds(nodes.web));
    // The serving rule is removed, and the other stays: the same font sets the text.
    prFontSetRemove(f.fonts, 2);
    f.layout(px(400, 400));
    CHECK(prLayoutTreeStats(f.tree).inlinesShaped == 0);
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);
    CHECK(prRectWidth(f.bounds(nodes.web)) == width);
    CHECK(verified(f.tree));
}

TEST_CASE("a change of a text font's answers shapes and lays out the text its groups set, and nothing else") {
    Fixture f;
    const PrFont* cousine = fontFile("Cousine.ttf");
    const PrFont* tinos = fontFile("Tinos.ttf");
    addWeb(f, 1, cousine);
    webAndDefault(f);
    f.layout(px(400, 400));
    // A face of the family added after the first, which serves the text first. Both groups of the text font change,
    // though they share it, and the block of the default family keeps its shaping.
    addWeb(f, 2, tinos);
    f.layout(px(400, 400));
    CHECK(prLayoutTreeStats(f.tree).inlinesShaped == 2);
    CHECK(verified(f.tree));
    prFontRelease(tinos);
    prFontRelease(cousine);
}

TEST_CASE("positioned boxes after the first that share a scroll container are hit where the scroll draws them") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle port = block();
    port.height = prLayoutLengthPx(100);
    port.overflowY = PrLayoutOverflow_Scroll;
    port.overflowX = PrLayoutOverflow_Hidden;
    PrLayoutNode view = f.element(root, port);
    PrLayoutBoxStyle spacer = block();
    spacer.height = prLayoutLengthPx(200);
    f.element(view, spacer);
    PrLayoutBoxStyle card = block();
    card.overflowX = card.overflowY = PrLayoutOverflow_Clip;
    PrLayoutNode clip = f.element(view, card);
    PrLayoutBoxStyle rowStyle = block();
    rowStyle.display = PrLayoutDisplay_Flex;
    rowStyle.height = prLayoutLengthPx(20);
    PrLayoutNode row = f.element(clip, rowStyle);
    PrLayoutBoxStyle name = block();
    name.width = prLayoutLengthPx(50);
    f.element(row, name);
    PrLayoutBoxStyle lane = block();
    lane.position = PrLayoutPosition_Relative;
    lane.overflowX = lane.overflowY = PrLayoutOverflow_Clip;
    PrLayoutFlexStyle grow = prLayoutFlexStyleDefault();
    grow.grow = 1;
    grow.basis = prLayoutLengthPx(0);
    PrLayoutNode positioned = f.element(row, lane, nullptr, nullptr, nullptr, &grow);
    // The relative lane and the absolute box in it are listed in the root's stacking context one after the other, and
    // both are under the scroll container.
    PrLayoutBoxStyle spanStyle = block();
    spanStyle.position = PrLayoutPosition_Absolute;
    spanStyle.inset[PrRectSide_Left] = prLayoutLengthPercent(50);
    spanStyle.inset[PrRectSide_Top] = prLayoutLengthPx(3);
    spanStyle.inset[PrRectSide_Bottom] = prLayoutLengthPx(3);
    spanStyle.width = prLayoutLengthPercent(20);
    PrLayoutNode inner = f.element(positioned, spanStyle);
    f.layout(px(400, 300));
    render(f.tree, 400, 300);
    prLayoutNodeSetScroll(f.tree, view, PrLayoutVec{prLayoutUnitFromPx(0), prLayoutUnitFromPx(120)});
    render(f.tree, 400, 300);
    CHECK(f.hit(PrVec2{100, 90}) == positioned);
    CHECK(f.hit(PrVec2{250, 90}) == inner);
}

TEST_CASE("a box's edges snap to the nearest target pixel of their position, half up, however the box is nested") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutDecorationStyle black = background(PrColor{0, 0, 0, 1});
    f.element(root, placedAt(10.25f, 0, 10, 4), &black);
    f.element(root, placedAt(10.5f, 5, 10, 4), &black);
    // A box at 0.25 in a box at 10.25 is at 10.5, which rounds up, where each offset alone would round down.
    PrLayoutNode outer = f.element(root, placedAt(10.25f, 10, 20, 4));
    f.element(outer, placedAt(0.25f, 0, 10, 4), &black);
    f.layout(px(40, 20));
    std::vector<uint8_t> pixels = render(f.tree, 40, 20);
    CHECK(verified(f.tree));
    CHECK(pixel(pixels, 40, 9, 1) == kClear);
    CHECK(pixel(pixels, 40, 10, 1) == kBlack);
    CHECK(pixel(pixels, 40, 19, 1) == kBlack);
    CHECK(pixel(pixels, 40, 20, 1) == kClear);
    for (uint32_t y : {6u, 11u}) {
        CHECK(pixel(pixels, 40, 10, y) == kClear);
        CHECK(pixel(pixels, 40, 11, y) == kBlack);
        CHECK(pixel(pixels, 40, 20, y) == kBlack);
        CHECK(pixel(pixels, 40, 21, y) == kClear);
    }
}

TEST_CASE("boxes that abut at a fraction of a pixel meet on one snapped edge, with no seam") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle rowStyle = placedAt(10.5f, 0, 100, 4);
    rowStyle.display = PrLayoutDisplay_Flex;
    PrLayoutNode row = f.element(root, rowStyle);
    PrLayoutFlexStyle grow = prLayoutFlexStyleDefault();
    grow.grow = 1;
    grow.basis = prLayoutLengthPx(0);
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    PrLayoutDecorationStyle green = background(PrColor{0, 1, 0, 1});
    PrLayoutDecorationStyle blue = background(PrColor{0, 0, 1, 1});
    f.element(row, block(), &red, nullptr, nullptr, &grow);
    f.element(row, block(), &green, nullptr, nullptr, &grow);
    f.element(row, block(), &blue, nullptr, nullptr, &grow);
    f.layout(px(120, 4));
    std::vector<uint8_t> pixels = render(f.tree, 120, 4);
    // The first item ends at 10.5 + 33.33, which rounds to 44.
    CHECK(pixel(pixels, 120, 10, 1) == kClear);
    CHECK(pixel(pixels, 120, 11, 1) == "255 0 0 255");
    CHECK(pixel(pixels, 120, 43, 1) == "255 0 0 255");
    CHECK(pixel(pixels, 120, 44, 1) == "0 255 0 255");
}

TEST_CASE("at a ratio that is not whole, edges snap to target pixels and borders are whole target pixels") {
    Fixture f;
    prLayoutTreeSetTargetPixelRatio(f.tree, 1.5f);
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutDecorationStyle black = background(PrColor{0, 0, 0, 1});
    // 10.25px is 15.375 target pixels, and 20.25px is 30.375.
    f.element(root, placedAt(10.25f, 0, 10, 4), &black);
    // A border's width snaps as a border width: under a target pixel is one, and a wider one is floored.
    PrLayoutBoxStyle bordered = placedAt(10, 10, 20, 6);
    PrLayoutDecorationStyle decoration = background(PrColor{1, 1, 1, 1});
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        bordered.border[side] = prLayoutUnitFromPx(1);
        decoration.borderStyle[side] = PrDrawBorderStyle_Solid;
        decoration.borderColor[side] = prColor16FromColor(PrColor{0, 0, 0, 1});
    }
    PrLayoutNode framed = f.element(root, bordered, &decoration);
    PrLayoutBoxStyle widths = placedAt(0, 20, 10, 4);
    widths.border[PrRectSide_Top] = prLayoutUnitFromPx(0.1f);
    widths.border[PrRectSide_Right] = prLayoutUnitFromPx(1.7f);
    widths.border[PrRectSide_Bottom] = prLayoutUnitFromPx(2);
    PrLayoutNode measured = f.element(root, widths);
    f.layout(px(40, 30));
    std::vector<uint8_t> pixels = renderAt(f.tree, 60, 45, 1.5f);
    CHECK(verified(f.tree));
    CHECK(pixel(pixels, 60, 14, 1) == kClear);
    CHECK(pixel(pixels, 60, 15, 1) == kBlack);
    CHECK(pixel(pixels, 60, 29, 1) == kBlack);
    CHECK(pixel(pixels, 60, 30, 1) == kClear);
    // The 1px border is one target pixel, 2/3 px, on every side. The border box spans 20px of content and the two
    // borders, so it ends at 31.3125px, 46.97 target pixels, which rounds to 47.
    CHECK(prLayoutNodeBorderWidth(f.tree, framed, PrRectSide_Left) == prLayoutUnitFromPxFloorDouble(1 / 1.5));
    CHECK(pixel(pixels, 60, 15, 20) == kBlack);
    CHECK(pixel(pixels, 60, 16, 20) == "255 255 255 255");
    CHECK(pixel(pixels, 60, 45, 20) == "255 255 255 255");
    CHECK(pixel(pixels, 60, 46, 20) == kBlack);
    CHECK(pixel(pixels, 60, 47, 20) == kClear);
    CHECK(prLayoutNodeBorderWidth(f.tree, measured, PrRectSide_Top) == prLayoutUnitFromPxFloorDouble(1 / 1.5));
    CHECK(prLayoutNodeBorderWidth(f.tree, measured, PrRectSide_Right) == prLayoutUnitFromPxFloorDouble(2 / 1.5));
    CHECK(prLayoutNodeBorderWidth(f.tree, measured, PrRectSide_Bottom) == prLayoutUnitFromPx(2));
    CHECK(prLayoutNodeBorderWidth(f.tree, measured, PrRectSide_Left) == kLayoutUnitZero);
}

TEST_CASE("text's baseline lands on a whole target pixel, and its glyphs keep their place along the line") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    f.element(root, block(4.25f));
    PrLayoutBoxStyle indented = block();
    indented.padding[PrRectSide_Left] = prLayoutLengthPx(0.25f);
    PrLayoutNode line = f.element(root, indented);
    f.text(line, "Ab");
    f.layout(px(100, 40));
    PrDrawList* list = prDrawListCreate();
    prLayoutTreePaint(f.tree, list);
    std::vector<DrawnGlyph> drawn;
    appendGlyphs(list, prDrawTransformFromDAffine2(PrDAffine2{1, 0, 0, 1, 0, 0}), drawn);
    prDrawListRelease(list);
    REQUIRE(drawn.size() == 2);
    CHECK(drawn[0].x == doctest::Approx(0.25));
    for (const DrawnGlyph& glyph : drawn)
        CHECK(glyph.y == doctest::Approx(double(int64_t(glyph.y + 0.5))));
}

TEST_CASE("a recording is reused wherever it moves, and its edges snap where it is played") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    PrLayoutNode spacer = f.element(root, block(1));
    PrLayoutNode section = f.element(root, block());
    for (int i = 0; i < 4; ++i)
        f.element(section, block(1), &red);
    auto paint = [&]() {
        f.layout(px(10, 20));
        std::vector<uint8_t> pixels = render(f.tree, 10, 20);
        CHECK(verified(f.tree));
        return pixels;
    };
    paint();
    prLayoutNodeSetStyle(f.tree, spacer, f.style(block(3)));
    paint();
    uint32_t whole = prLayoutTreeStats(f.tree).subtreesReused;
    CHECK(whole != 0);
    // A move by a fraction of a pixel reuses as much as a move by whole pixels: snapping happens as the list plays.
    prLayoutNodeSetStyle(f.tree, spacer, f.style(block(3.5f)));
    std::vector<uint8_t> pixels = paint();
    CHECK(prLayoutTreeStats(f.tree).subtreesReused == whole);
    // The section's first leaf, from 3.5 to 4.5, snaps to the pixel from 4 to 5.
    CHECK(pixel(pixels, 10, 5, 3) == kClear);
    CHECK(pixel(pixels, 10, 5, 4) == "255 0 0 255");
    CHECK(pixel(pixels, 10, 5, 7) == "255 0 0 255");
    CHECK(pixel(pixels, 10, 5, 8) == kClear);
}

TEST_CASE("a scroll offset plays rounded to whole target pixels, and records nothing") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle scroller = block(20);
    scroller.overflowY = PrLayoutOverflow_Scroll;
    PrLayoutNode s = f.element(root, scroller);
    PrLayoutDecorationStyle red = background(PrColor{1, 0, 0, 1});
    PrLayoutDecorationStyle green = background(PrColor{0, 1, 0, 1});
    f.element(s, block(10), &red);
    f.element(s, block(30), &green);
    f.layout(px(10, 20));
    render(f.tree, 10, 20);
    // 2.5 rounds to 3, so the red box covers the pixels above 7 whole.
    prLayoutNodeSetScroll(f.tree, s, px(0, 2.5f));
    std::vector<uint8_t> pixels = render(f.tree, 10, 20);
    CHECK(prLayoutTreeStats(f.tree).layersRecorded == 0);
    CHECK(pixel(pixels, 10, 5, 6) == "255 0 0 255");
    CHECK(pixel(pixels, 10, 5, 7) == "0 255 0 255");
    PrLayoutScrollInfo info{};
    REQUIRE(prLayoutNodeScrollInfo(f.tree, s, &info));
    CHECK(info.offset == px(0, 2.5f));
}

TEST_CASE("replaced content fills its content box's snapped edges") {
    Fixture f;
    PrLayoutNode root = f.element(kPrLayoutNodeNone, block());
    PrLayoutNode image = replaced(f, root, placedAt(10.25f, 0.5f, 10, 4), natural(10, 4));
    PrLayoutContent content = solid(PrColor{0, 0, 0, 1}, 10, 4);
    prLayoutNodeSetContent(f.tree, image, &content);
    prDrawListRelease(content.list);
    f.layout(px(40, 10));
    std::vector<uint8_t> pixels = render(f.tree, 40, 10);
    CHECK(verified(f.tree));
    CHECK(pixel(pixels, 40, 9, 2) == kClear);
    CHECK(pixel(pixels, 40, 10, 2) == kBlack);
    CHECK(pixel(pixels, 40, 19, 2) == kBlack);
    CHECK(pixel(pixels, 40, 20, 2) == kClear);
    CHECK(pixel(pixels, 40, 15, 0) == kClear);
    CHECK(pixel(pixels, 40, 15, 1) == kBlack);
    CHECK(pixel(pixels, 40, 15, 4) == kBlack);
    CHECK(pixel(pixels, 40, 15, 5) == kClear);
}
