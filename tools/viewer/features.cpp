// Every feature drawing, in the order that the viewer turns through them, and the release of what the drawings created.

#include "tools/viewer/features.h"

#include "tools/viewer/drawing.h"
#include "tools/viewer/features_boxes.h"
#include "tools/viewer/features_compose.h"
#include "tools/viewer/features_filters.h"
#include "tools/viewer/features_fonts.h"
#include "tools/viewer/features_paint.h"
#include "tools/viewer/features_shapes.h"
#include "tools/viewer/features_text.h"

namespace viewer {

const std::vector<Drawing>& allFeatures() {
    static const std::vector<Drawing> features = [] {
        std::vector<Drawing> all;
        for (const std::vector<Drawing>* group :
             {&shapeFeatures(), &paintFeatures(), &composeFeatures(), &boxFeatures(), &filterFeatures(),
              &textFeatures(), &fontFeatures()})
            all.insert(all.end(), group->begin(), group->end());
        return all;
    }();
    return features;
}

void releaseDrawings() {
    releaseDrawingImages();
    releaseDrawingText();
    releaseDrawingFonts();
}

} // namespace viewer
