// The thread's current Dear ImGui context, which imgui_config.h defines as GImGui.

#include "draw/comparisons/imgui/config/imgui_config.h"

namespace pr {

thread_local ImGuiContext* PR_NULLABLE gImGuiContext = nullptr;

} // namespace pr
