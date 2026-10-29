// Dear ImGui's configuration for this build (IMGUI_USER_CONFIG).
//
// Dear ImGui's current context is per thread, so that the imgui backend can render on one thread while another thread
// runs its own user interface through Dear ImGui.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

struct ImGuiContext;

namespace pr {

// The current Dear ImGui context of the calling thread.
extern thread_local ImGuiContext* PR_NULLABLE gImGuiContext;

} // namespace pr

PR_ASSUME_NONNULL_END

// The name through which Dear ImGui reads and sets its current context.
#define GImGui pr::gImGuiContext
