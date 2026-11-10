// The editor's view: its menu bar, toolbar, panels and status bar, declared from its model every round. The view
// composes two component libraries in one tree: Studio Kit, and the viewport's HUD (hud.h).

#pragma once

#include "pr/kit.h"
#include "pr/kit_panel.h"

#include "tools/bench/editor_bench/editor_state.h"
#include "tools/bench/editor_bench/host.h"

PR_ASSUME_NONNULL_BEGIN

namespace editor {

// Declares the editor under `root`, the screen's panel, and applies the round's input to the model. The editor's HUD
// must exist (editorAttach).
void buildEditor(PrKit* kit, PrLayoutNode root, EditorState& editor);

// Make and destroy the HUD of the editor whose state is `context`, for the host's context (Host::attach and
// Host::detach).
void editorAttach(void* context, editor_bench::Host& host);
void editorDetach(void* context, editor_bench::Host& host);

} // namespace editor

PR_ASSUME_NONNULL_END
