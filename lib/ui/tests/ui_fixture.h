// The fixture for the interaction tests: a tree with no text, a context for it, and helpers that declare boxes and
// queue input.

#pragma once

#include "pr/layout_frame.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/ui.h"

#include "doctest/doctest.h"

#include <functional>

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A context for a tree of boxes in an 800 by 600 px viewport. Each frame declares the tree that the test's build
// function gives.
struct UiFixture {
    PrLayoutTree* tree;
    PrUi* ui;
    double time = 0;
    PrLayoutNode root = kPrLayoutNodeNone;
    std::function<void()> build;

    UiFixture() {
        PrLayoutTreeDesc desc{.targetPixelRatio = 1};
        tree = prLayoutTreeCreate(&desc);
        ui = prUiCreate(tree);
    }

    ~UiFixture() {
        prUiDestroy(ui);
        prLayoutTreeDestroy(tree);
    }

    // Runs a frame of at most `rounds` rounds, `delta` seconds after the previous frame. Each round declares a root and
    // calls the test's build function. Returns the number of rounds run.
    uint32_t frame(double delta = 1.0 / 60, uint32_t rounds = 4) {
        time += delta;
        PrUiFrameDesc desc{.time = time,
                           .delta = delta,
                           .extent = PrVec2{800, 600},
                           .maxRounds = rounds,
                           .display = prAppDisplayDefault()};
        return prUiFrame(
            ui, &desc,
            [](void* context, PrUi*, const PrUiRound*) {
                UiFixture* f = static_cast<UiFixture*>(context);
                f->root = prLayoutTreeFrameElement(f->tree, kPrLayoutNodeNone, 0, f->rootStyle());
                if (f->build)
                    f->build();
            },
            this);
    }

    PrLayoutStyleIndex rootStyle() {
        PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
        box.display = PrLayoutDisplay_Block;
        box.position = PrLayoutPosition_Relative;
        box.width = prLayoutLengthPx(800);
        box.height = prLayoutLengthPx(600);
        return styleOf(box);
    }

    PrLayoutStyleIndex styleOf(const PrLayoutBoxStyle& box, int32_t zIndex = 0, bool zIndexed = false) {
        PrLayoutStyle record = prLayoutStyleDefault();
        PrLayoutBoxStyle b = box;
        b.zIndexed = zIndexed;
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &b);
        if (zIndexed) {
            PrLayoutEffectStyle effects = prLayoutEffectStyleDefault();
            effects.zIndex = zIndex;
            record.groups[PrLayoutGroup_Effect] = prLayoutEffectStyleIntern(tree, &effects);
        }
        return prLayoutStyleIntern(tree, &record);
    }

    // Creates an absolutely positioned box of `w` by `h` px at `x`, `y` in its parent, with `flags`.
    PrLayoutNode box(PrLayoutNode parent, float x, float y, float w, float h, PrUiFlags flags = 0, uint64_t key = 0,
                     int32_t zIndex = 0, bool zIndexed = false) {
        PrLayoutBoxStyle b = prLayoutBoxStyleDefault();
        b.display = PrLayoutDisplay_Block;
        b.position = PrLayoutPosition_Absolute;
        b.inset[PrRectSide_Left] = prLayoutLengthPx(x);
        b.inset[PrRectSide_Top] = prLayoutLengthPx(y);
        b.width = prLayoutLengthPx(w);
        b.height = prLayoutLengthPx(h);
        return prUiElement(ui, parent, key, styleOf(b, zIndex, zIndexed), flags);
    }

    // Creates a scroll container of `w` by `h` px at `x`, `y` that holds a block of `contentW` by `contentH` px.
    PrLayoutNode scroller(PrLayoutNode parent, float x, float y, float w, float h, float contentW, float contentH,
                          PrLayoutNode* PR_NULLABLE content = nullptr) {
        PrLayoutBoxStyle b = prLayoutBoxStyleDefault();
        b.display = PrLayoutDisplay_Block;
        b.position = PrLayoutPosition_Absolute;
        b.inset[PrRectSide_Left] = prLayoutLengthPx(x);
        b.inset[PrRectSide_Top] = prLayoutLengthPx(y);
        b.width = prLayoutLengthPx(w);
        b.height = prLayoutLengthPx(h);
        b.overflowX = PrLayoutOverflow_Scroll;
        b.overflowY = PrLayoutOverflow_Scroll;
        PrLayoutNode node = prUiElement(ui, parent, 0, styleOf(b), PrUiFlag_Scrollable);
        PrLayoutBoxStyle c = prLayoutBoxStyleDefault();
        c.display = PrLayoutDisplay_Block;
        c.width = prLayoutLengthPx(contentW);
        c.height = prLayoutLengthPx(contentH);
        PrLayoutNode inner = prUiElement(ui, node, 0, styleOf(c), 0);
        if (content != nullptr)
            *content = inner;
        return node;
    }

    void push(PrAppEvent event) {
        event.time = time;
        prUiPushEvent(ui, &event);
    }
    void move(float x, float y) {
        push(PrAppEvent{.kind = PrAppEventKind_PointerMove, .position = PrVec2{x, y}});
    }
    void down(float x, float y, uint8_t button = 0) {
        push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .button = button, .position = PrVec2{x, y}});
    }
    void up(float x, float y, uint8_t button = 0) {
        push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .button = button, .position = PrVec2{x, y}});
    }
    void press(float x, float y, uint8_t button = 0) {
        down(x, y, button);
        up(x, y, button);
    }
    void key(PrAppKey key, PrAppModifierFlags modifiers = 0) {
        push(PrAppEvent{.kind = PrAppEventKind_KeyDown, .modifiers = modifiers, .key = key});
        push(PrAppEvent{.kind = PrAppEventKind_KeyUp, .modifiers = modifiers, .key = key});
    }
    void text(const char* text) {
        push(PrAppEvent{.kind = PrAppEventKind_Text, .text = text});
    }
    void wheel(float x, float y, float dx, float dy) {
        push(PrAppEvent{.kind = PrAppEventKind_Scroll, .position = PrVec2{x, y}, .delta = PrVec2{dx, dy}});
    }
};

} // namespace pr_impl

PR_ASSUME_NONNULL_END
