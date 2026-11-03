// The fixture of the layout tree tests: a tree over Arimo, and helpers that build and measure boxes.

#pragma once

#include "pr/draw_commands.h"
#include "pr/draw_cpu.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/draw_transform.h"
#include "pr/layout_query.h"
#include "pr/layout_scene.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/mem.h"
#include "pr/string_view.h"
#include "pr/text_harfbuzz.h"

#include "doctest/doctest.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Verifies the tree's incremental work, and reports the first difference if there is one.
inline bool verified(PrLayoutTree* tree) {
    PrLayoutTreeDifference difference{};
    bool same = prLayoutTreeVerify(tree, &difference);
    if (!same) {
        PrStringView what = prRawStringView(&difference.what);
        MESSAGE("node " << uint32_t(difference.node) << ": " << std::string(what.data, what.size));
        prRawStringReset(&difference.what);
    }
    return same;
}

struct Fixture;

// Returns the most recently made fixture that is still live. The helpers that build styles outside a test's fixture
// read it.
inline Fixture* PR_NULLABLE& currentFixture() {
    static Fixture* PR_NULLABLE current = nullptr;
    return current;
}

// A tree over Arimo, the default family. Arimo is aliased (prFontIsAliased) if `aliased` is set.
struct Fixture {
    Fixture* PR_NULLABLE previous = currentFixture();
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextHarfBuzzCreate();
    PrLayoutTree* PR_NULLABLE tree = nullptr;
    PrLayoutNode root = kPrLayoutNodeNone;  // the last element created with no parent, which layout lays out

    // The text fonts that textFont made, each made once for its inputs, so that equal fonts are one font group.
    struct Made {
        std::string key;
        PrTextFont* font;
    };
    std::vector<Made> made;

    explicit Fixture(bool aliased = false) {
        std::string path = std::string(PR_FONTS_DIR) + "/Arimo.ttf";
        const PrFont* PR_NULLABLE font = nullptr;
        PrFontDerivation derivation = prFontDerivationDefault();
        derivation.aliased = aliased;
        derivation.overrides = PrFontOverrideFlag_Aliased;
        REQUIRE(prFontCreateFromFile(path, 0, &derivation, &font) == PrStatus_Ok);
        PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
        prFontSetAdd(fonts, &descriptor, font);
        prFontRelease(font);
        prFontSetSetDefaultFamily(fonts, prFontFamilyFromName("Arimo"));
        PrTextFontDesc defaultFontDesc{.set = fonts, .style = prFontStyleDefault()};
        PrTextFont* defaultFont = prTextFontCreate(&defaultFontDesc);
        PrLayoutTreeDesc desc{.font = defaultFont, .shaper = shaper, .targetPixelRatio = 1};
        tree = prLayoutTreeCreate(&desc);
        // The default font is textFont's font of its size and nothing else, so that text in the default style is set in
        // one font.
        made.push_back(Made{keyOf(defaultFontDesc.style.size, {}, 0, kPrFontWeightNormal), defaultFont});
        currentFixture() = this;
    }

    ~Fixture() {
        currentFixture() = previous;
        prLayoutTreeDestroy(tree);
        for (const Made& m : made)
            prTextFontRelease(m.font);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
    }

    // Adds the faces of the font file `file` to the set, as the family that the file names, and returns that family,
    // whose name is `name`.
    PrFontFamily family(const char* file, const char* name) {
        std::string path = std::string(PR_FONTS_DIR) + "/" + file;
        const PrFont* PR_NULLABLE font = nullptr;
        REQUIRE(prFontCreateFromFile(path, 0, nullptr, &font) == PrStatus_Ok);
        PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
        prFontSetAdd(fonts, &descriptor, font);
        prFontRelease(font);
        return prFontFamilyFromName(name);
    }

    // Returns the key by which textFont keeps a text font.
    static std::string keyOf(float size, const std::vector<PrFontFamily>& families, PrLocale locale, float weight) {
        std::string key = std::to_string(size) + "/" + std::to_string(weight) + "/" + std::to_string(locale);
        for (const PrFontFamily& family : families)
            key += "/" + std::to_string(family.generic) + ":" + std::string(family.name.data, family.name.size);
        return key;
    }

    // Returns the text font of `families`, then the set's default family, at `size` px, `weight` and `locale`. It is
    // made once for its inputs and held until the fixture ends.
    PrTextFont* textFont(float size, std::vector<PrFontFamily> families = {}, PrLocale locale = 0,
            float weight = kPrFontWeightNormal) {
        std::string key = keyOf(size, families, locale, weight);
        for (const Made& m : made) {
            if (m.key == key)
                return m.font;
        }
        PrFontStyle style = prFontStyleDefault();
        style.size = size;
        style.weight = weight;
        PrTextFontDesc desc{
            .set = fonts,
            .families = families.data(),
            .familyCount = uint32_t(families.size()),
            .style = style,
            .locale = locale,
        };
        made.push_back(Made{key, prTextFontCreate(&desc)});
        return made.back().font;
    }

    uint32_t style(const PrLayoutBoxStyle& box, const PrLayoutDecorationStyle* PR_NULLABLE decoration = nullptr,
            const PrLayoutEffectStyle* PR_NULLABLE effects = nullptr, const PrLayoutFontStyle* PR_NULLABLE font = nullptr,
            const PrLayoutFlexStyle* PR_NULLABLE flex = nullptr) {
        PrLayoutStyle record = prLayoutStyleDefault();
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
        if (decoration != nullptr)
            record.groups[PrLayoutGroup_Decoration] = prLayoutDecorationStyleIntern(tree, decoration);
        if (effects != nullptr)
            record.groups[PrLayoutGroup_Effect] = prLayoutEffectStyleIntern(tree, effects);
        if (font != nullptr)
            record.groups[PrLayoutGroup_Font] = prLayoutFontStyleIntern(tree, font);
        if (flex != nullptr)
            record.groups[PrLayoutGroup_Flex] = prLayoutFlexStyleIntern(tree, flex);
        return prLayoutStyleIntern(tree, &record);
    }

    PrLayoutNode element(PrLayoutNode parent, const PrLayoutBoxStyle& box,
            const PrLayoutDecorationStyle* PR_NULLABLE decoration = nullptr,
            const PrLayoutEffectStyle* PR_NULLABLE effects = nullptr, const PrLayoutFontStyle* PR_NULLABLE font = nullptr,
            const PrLayoutFlexStyle* PR_NULLABLE flex = nullptr) {
        PrLayoutNode node = prLayoutNodeCreateElement(tree, parent, style(box, decoration, effects, font, flex));
        if (parent == kPrLayoutNodeNone)
            root = node;
        return node;
    }

    // Lays out the root in a viewport of `extent`.
    void layout(PrLayoutVec extent) {
        PrLayoutRoot viewport{root, extent};
        prLayoutTreeLayout(tree, &viewport, 1);
    }

    PrLayoutNode text(PrLayoutNode parent, const char* value) {
        return prLayoutNodeCreateText(tree, parent, value);
    }

    PrRect bounds(PrLayoutNode node) {
        return prLayoutRectToPx(prLayoutNodeBounds(tree, node, PrLayoutCoordinates_Viewport));
    }

    // Returns the topmost node at `point`, or none.
    PrLayoutNode hit(PrVec2 point) {
        PrLayoutNode node = kPrLayoutNodeNone;
        prLayoutTreeHitTest(tree, prLayoutVecFromPx(point), &node, 1);
        return node;
    }
};

// Returns the current fixture's text font of `families` at `size` px, `weight` and `locale` (Fixture::textFont).
inline PrTextFont* textFontOf(float size, std::vector<PrFontFamily> families = {}, PrLocale locale = 0,
        float weight = kPrFontWeightNormal) {
    Fixture* PR_NULLABLE f = currentFixture();
    REQUIRE(f != nullptr);
    return f->textFont(size, std::move(families), locale, weight);
}

// Returns a block box, of `height` if one is given, or an inline box.
inline PrLayoutBoxStyle block() {
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_Block;
    return box;
}
inline PrLayoutBoxStyle block(float height) {
    PrLayoutBoxStyle box = block();
    box.height = prLayoutLengthPx(height);
    return box;
}
inline PrLayoutBoxStyle inlineBox() {
    return prLayoutBoxStyleDefault();
}

// Sets the box's block-start and block-end margins.
inline void setMargins(PrLayoutBoxStyle& box, float top, float bottom) {
    box.margin[PrRectSide_Top] = prLayoutLengthPx(top);
    box.margin[PrRectSide_Bottom] = prLayoutLengthPx(bottom);
}

// Returns a decoration with a background of `color`.
inline PrLayoutDecorationStyle background(PrColor color) {
    PrLayoutDecorationStyle decoration = prLayoutDecorationStyleDefault();
    decoration.background = prColor16FromColor(color);
    return decoration;
}

// Returns a vector of pixels in units.
inline PrLayoutVec px(float x, float y) {
    return prLayoutVecFromPx(PrVec2{x, y});
}

// Returns natural dimensions in pixels, each negative for none.
inline PrLayoutNaturalSize natural(float width, float height, float ratio = 0) {
    PrLayoutNaturalSize size = prLayoutNaturalSizeNone();
    if (width >= 0)
        size.width = prLayoutUnitFromPx(width);
    if (height >= 0)
        size.height = prLayoutUnitFromPx(height);
    size.ratio = ratio;
    return size;
}

// Creates a replaced element under `parent` styled by `box`, with the natural dimensions `size`.
inline PrLayoutNode replaced(Fixture& f, PrLayoutNode parent, const PrLayoutBoxStyle& box, PrLayoutNaturalSize size,
        const PrLayoutDecorationStyle* PR_NULLABLE decoration = nullptr) {
    PrLayoutNode node = prLayoutNodeCreateReplaced(f.tree, parent, f.style(box, decoration));
    prLayoutNodeSetNaturalSize(f.tree, node, size);
    return node;
}

// Returns content of `color` filling a natural space of `width` by `height` pixels.
inline PrLayoutContent solid(PrColor color, float width, float height) {
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, width, height}, {.color = color});
    PrLayoutContent content{};
    content.kind = PrLayoutContentKind_DrawList;
    content.list = list;
    return content;
}

// Checks each edge of `actual`.
inline void checkRect(PrRect actual, double left, double top, double right, double bottom) {
    CHECK(double(actual.left) == doctest::Approx(left));
    CHECK(double(actual.top) == doctest::Approx(top));
    CHECK(double(actual.right) == doctest::Approx(right));
    CHECK(double(actual.bottom) == doctest::Approx(bottom));
}

// A glyph that a list draws: its id, and its origin in the list's space.
struct DrawnGlyph {
    uint16_t id;
    double x;
    double y;
};

// Appends the glyphs that `list` draws under `transform`, in the order it draws them, including through the lists it
// embeds. Transforms compose as a backend's walk composes them, snapping included (prDrawTransformPush).
inline void appendGlyphs(const PrDrawList* list, PrDrawTransform transform, std::vector<DrawnGlyph>& out) {
    std::vector<PrDrawTransform> stack{transform};
    const PrDrawCommand* commands = prDrawListCommands(list);
    for (uint32_t i = 0; i < prDrawListCommandCount(list); ++i) {
        const PrDrawCommand& command = commands[i];
        const PrDAffine2& t = stack.back().daffine;
        switch (command.kind) {
            case PrDrawCommandKind_Embed:
                appendGlyphs(command.embed.list, prDrawTransformMoved(&stack.back(), command.embed.offset), out);
                break;
            case PrDrawCommandKind_PushTransformAffine2:
                stack.push_back(prDrawTransformPush(&stack.back(), &command));
                break;
            case PrDrawCommandKind_PopTransform: stack.pop_back(); break;
            case PrDrawCommandKind_Glyphs:
                for (uint32_t g = 0; g < command.glyphs->glyphCount; ++g) {
                    PrVec2 p = command.glyphs->glyphs[g].position;
                    double x = double(p.x);
                    double y = double(p.y);
                    out.push_back(DrawnGlyph{
                        command.glyphs->glyphs[g].id, t.a * x + t.c * y + t.e,
                        t.b * x + t.d * y + t.f,
                    });
                }
                break;
            default: break;
        }
    }
}

// Returns the ids of the glyphs that the tree paints, in visual order: by line from the top, and from the left along
// each line. A line paints its fragments in tree order, so on a bidi line the painting order is not the visual order.
inline std::vector<uint16_t> paintedGlyphs(PrLayoutTree* tree) {
    PrDrawList* list = prDrawListCreate();
    prLayoutTreePaint(tree, list);
    std::vector<DrawnGlyph> drawn;
    appendGlyphs(list, prDrawTransformFromDAffine2(PrDAffine2{1, 0, 0, 1, 0, 0}), drawn);
    prDrawListRelease(list);
    std::stable_sort(drawn.begin(), drawn.end(), [](const DrawnGlyph& a, const DrawnGlyph& b) {
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    });
    std::vector<uint16_t> ids;
    for (const DrawnGlyph& glyph : drawn)
        ids.push_back(glyph.id);
    return ids;
}

// Returns the tree's offsets for the scroll containers of its scene.
inline std::vector<PrLayoutSceneScroll> treeScrolls(const PrLayoutTree* tree, const PrLayoutScene* scene) {
    std::vector<PrLayoutSceneScroll> scrolls(prLayoutSceneScrolls(tree, scene, nullptr, 0));
    prLayoutSceneScrolls(tree, scene, scrolls.data(), uint32_t(scrolls.size()));
    return scrolls;
}

// Renders the tree onto `width` by `height` RGBA pixels.
inline std::vector<uint8_t> render(PrLayoutTree* tree, uint32_t width, uint32_t height) {
    PrDrawList* list = prDrawListCreate();
    prLayoutTreePaint(tree, list);
    std::vector<uint8_t> bytes(size_t(width) * height * 4, 0);
    PrDrawRenderer* renderer = prDrawCpuCreate(nullptr);
    PrPixels into{bytes.data(), size_t(width) * 4, {width, height}};
    REQUIRE(prDrawRendererRenderPixels(renderer, list, into, PrPixelFormat_Rgba8) == PrStatus_Ok);
    prDrawRendererDestroy(renderer);
    prDrawListRelease(list);
    return bytes;
}

// Returns the RGBA bytes of the pixel at `x`, `y` of `bytes`, which is `width` pixels wide, as text.
inline std::string pixel(const std::vector<uint8_t>& bytes, uint32_t width, uint32_t x, uint32_t y) {
    const uint8_t* p = bytes.data() + (size_t(y) * width + x) * 4;
    return std::to_string(p[0]) + " " + std::to_string(p[1]) + " " + std::to_string(p[2]) + " " + std::to_string(p[3]);
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
