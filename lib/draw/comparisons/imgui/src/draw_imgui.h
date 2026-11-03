// The part of the Dear ImGui draw backend that every platform shares: the Dear ImGui context, the fonts in its atlas,
// and the recording of a list into a Dear ImGui frame. Each platform's source creates the renderer's state, uploads its
// images, and draws a frame with Dear ImGui's renderer for its device, through the functions that its header declares.

#pragma once

#include "pr/base.h"
#include "pr/draw_image.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/font.h"
#include "pr/pixels.h"
#include "pr/raw_array.h"

#include "imgui.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// One of the list's fonts in Dear ImGui's atlas. The atlas reads the font's bytes for as long as the font is in it.
struct ImGuiCachedFont {
    uint64_t id;
    const PrFont* PR_NULLABLE font;     // retained
    ImFont* PR_NULLABLE added;
    uint64_t lastUsed;
};

// The state of a renderer that its platform's state begins with.
struct ImGuiBackend : PrDrawRenderer {
    ImGuiContext* context;
    RawArray<ImGuiCachedFont> fonts;
    uint64_t render;                    // the prepares so far, which mark what each one used
};

// A frame: its draw data, with its own copies of the draw lists, because the next prepare rebuilds the context's draw
// lists. A platform's frame begins with it.
struct ImGuiFrame : PrDrawFrame {
    ImDrawData data;
};

// Writes the image's pixels as Dear ImGui's renderer blends them: Rgba8 with straight alpha.
void imguiImagePixels(const PrDrawImage* image, PrPixels pixels);

// Deletes the draw lists that the frame holds. The backend's context is current.
void imguiFrameClear(ImGuiFrame* frame);

// Runs a Dear ImGui frame over the target's extent that records the list into the context's background draw list, and
// copies the frame's draw data into `frame`, whose draw lists are clear. Then drops the fonts that the frame did not
// use. The backend's context is current, and `render` already counts this prepare.
void imguiRecord(ImGuiBackend* imgui, ImGuiFrame* frame, const PrDrawList* list, const PrDrawTarget* target);

// Destroys the backend's context, then releases its fonts, whose bytes the atlas read until then. The platform's
// renderer has shut down.
void imguiContextDestroy(ImGuiBackend* imgui);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
