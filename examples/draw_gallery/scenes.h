// Scenes drawn with lib/draw alone: animated pictures that combine its shapes, paths, strokes, gradients, images,
// shadows, clips, transforms, color spaces and text.

#pragma once

#include "pr/draw_list.h"
#include "pr/text.h"

PR_ASSUME_NONNULL_BEGIN

namespace scenes {

// The height in logical pixels of the band along the bottom of a drawing that holds the caption. A scene fits its
// picture above the band, and its backdrop fills the band too.
constexpr float kCaptionBand = 68;

// The pointer over a drawing, in its logical pixels. `hold` eases from 0 to 1 while the primary button is held, and
// back while it is not.
struct SceneInput {
    PrVec2 pointer;
    bool over;                  // the pointer is over the drawing; a scene moves a pointer of its own when it is not
    float hold;
};

// A scene: its name, a line that says what it shows, and the function that draws it at `width` by `height` logical
// pixels at `time` in seconds, with the pointer in `input`.
struct Scene {
    const char* name;
    const char* description;
    void (*draw)(PrDrawList* list, float width, float height, double time, const SceneInput& input);
};

// Gets the scenes ready to draw, with their text shaped by `shaper` in the fonts of the Noto bundle under `fonts`.
// `shaper` must outlive the scenes' use.
void scenesOpen(PrTextShaper* shaper, const char* fonts);

// Frees the images and fonts that the scenes made.
void scenesClose();

// Returns the scenes, and their number in `count`.
const Scene* allScenes(uint32_t* count);

// Draws the caption in the band along the bottom of a drawing of `width` by `height` logical pixels: the name of
// `scene` and its place among the scenes, and the keys that turn through them and pause them.
void drawCaption(PrDrawList* list, float width, float height, uint32_t scene, bool paused);

} // namespace scenes

PR_ASSUME_NONNULL_END
