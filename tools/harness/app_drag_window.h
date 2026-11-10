// A window for the tests of the libraries that start drags (prAppWindowDragStart). It implements the start of a drag
// alone, which it records, and a test gives it to a context that drags (prUiSetWindow).

#pragma once

#include "pr/app.h"
#include "pr/app_backend.h"
#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace harness {

// A window whose only function is the start of a drag: the number of drags started, and the description of the last.
// Its application implements nothing.
struct DragWindow : PrAppWindow {
    int starts = 0;
    PrAppDragDesc started{};
    mutable PrApp app{};

    DragWindow() : PrAppWindow{} {
        appFn = [](const PrAppWindow* window) -> PrApp* { return &static_cast<const DragWindow*>(window)->app; };
        dragStartFn = [](PrAppWindow* window, const PrAppDragDesc* desc) {
            DragWindow& self = static_cast<DragWindow&>(*window);
            ++self.starts;
            self.started = *desc;
        };
    }
};

} // namespace harness

PR_ASSUME_NONNULL_END
