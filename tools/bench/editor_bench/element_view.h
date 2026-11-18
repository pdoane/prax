// The editor built with lib/ui's elements and memos (pr/ui_element.h) instead of Studio Kit's widgets: the same model,
// panels and controls as editor_view.h, with the widgets of element_widgets.h in a look of their own. Its icons are
// drawn by the host's kit.
//
// Each round walks the model as the view shows it, as immediate mode does: the scene tree's rows in the window that its
// scroll shows, the log, and the inspected entity's properties through the model's reflection. What the walk creates is
// memoized on the values that it read, so a round keeps every part whose values are unchanged and that no input
// reached, and builds the rest.

#pragma once

#include "pr/ui_element.h"

#include "tools/bench/editor_bench/editor_state.h"
#include "tools/bench/editor_bench/host.h"

#include <stdint.h>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

using namespace pr;

namespace editor {

// The editor of elements: the model, what the view keeps of its own, and the builder of its elements.
struct ElementEditor {
    EditorState state;

    // The entities that the scene tree shows, as indices of the model's entities, which each round computes.
    std::vector<uint32_t> rows;

    // The dock's shares: the left and right columns' of the width, and the scene's and the viewport's of their columns'
    // heights.
    float left = 0.22f;
    float right = 0.22f;
    float scene = 0.62f;
    float viewport = 0.72f;

    uint32_t consoleTab = 0;

    // What the editor draws with, which outlives the builders of the host's contexts, and the builder of the current
    // context.
    UiResources* PR_NULLABLE resources = nullptr;
    UiBuilder* PR_NULLABLE ui = nullptr;

    ElementEditor() = default;
    ~ElementEditor();
    ElementEditor(const ElementEditor&) = delete;
    ElementEditor& operator=(const ElementEditor&) = delete;
};

// Declares the editor under `root`, which fills the viewport, and applies the round's input to the model.
void buildElementEditor(ElementEditor& editor, PrLayoutNode root);

// Make and destroy the builder of the editor whose state is `context`, for the host's context (Host::attach and
// Host::detach).
void elementAttach(void* context, editor_bench::Host& host);
void elementDetach(void* context, editor_bench::Host& host);

} // namespace editor

PR_ASSUME_NONNULL_END
