// Every feature drawing, in the order that they are shown, and the release of what the drawings created.

#include "examples/draw_features/features.h"

#include "examples/draw_features/drawing.h"
#include "examples/draw_features/features_boxes.h"
#include "examples/draw_features/features_compose.h"
#include "examples/draw_features/features_filters.h"
#include "examples/draw_features/features_fonts.h"
#include "examples/draw_features/features_paint.h"
#include "examples/draw_features/features_shapes.h"
#include "examples/draw_features/features_text.h"

namespace drawings {

using namespace pr;

const Array<Drawing>& allFeatures() {
    static const Array<Drawing> features = [] {
        const Array<Drawing>* groups[] = {
            &shapeFeatures(), &paintFeatures(), &composeFeatures(), &boxFeatures(), &filterFeatures(), &textFeatures(),
            &fontFeatures(),
        };
        Array<Drawing> all;
        for (const Array<Drawing>* group : groups)
            all.append(group->span());
        return all;
    }();
    return features;
}

void releaseDrawings() {
    releaseDrawingImages();
    releaseDrawingText();
    releaseDrawingFonts();
}

} // namespace drawings
