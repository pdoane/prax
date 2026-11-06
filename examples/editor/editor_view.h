// The editor's view: its menu bar, toolbar, dock of panels and status bar, declared from its model every round. The
// view composes two component libraries in one tree: Studio Kit, and the viewport's HUD (hud.h), which the model holds
// for the context that the view runs in.

#pragma once

#include "pr/kit.h"
#include "pr/layout_tree.h"

#include "examples/editor/editor_state.h"

PR_ASSUME_NONNULL_BEGIN

namespace editor {

// Declares the editor under `root`, a column that fills the window, and applies the round's input to the model.
void buildEditor(PrKit* kit, PrLayoutNode root, EditorState& editor);

} // namespace editor

PR_ASSUME_NONNULL_END
