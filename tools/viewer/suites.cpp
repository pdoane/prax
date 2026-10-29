// The drawings' collections (suites.h).

#include "tools/viewer/suites.h"

#include "tools/viewer/drawing.h"
#include "tools/viewer/features.h"
#include "tools/viewer/workloads.h"

namespace viewer {

namespace {

// Returns a collection of drawings, laid out for `width` by `height`.
Collection drawings(const char* name, float width, float height, const std::vector<Drawing>& (*all)()) {
    return Collection{name, width, height, 0, [all] {
        std::vector<Document> documents;
        for (const Drawing& drawing : all()) {
            Document d{drawing.name, drawing.description, Source{}};
            d.source.drawing = &drawing;
            documents.push_back(std::move(d));
        }
        return documents;
    }};
}

} // namespace

std::vector<Collection> drawingCollections() {
    return {drawings("Features", 1100, 800, allFeatures), drawings("Workloads", 1100, 800, workloadDrawings)};
}

} // namespace viewer
