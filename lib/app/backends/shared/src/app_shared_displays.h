// The displays that a backend has reported, which keep the memory of what PrAppDisplayInfo holds by view.

#pragma once

#include "pr/app_display.h"
#include "pr/base.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A display that the backend has reported, with its name as the system shows it.
struct AppKnownDisplay {
    PrAppDisplayId id;
    PrRawString name;
};

// The displays that the backend has reported since the list was last reset. A backend resets it as it delivers
// DisplaysChanged, which may change the displays, and when it is destroyed, so that the views of the names it reported
// stay valid until then. Each display is in memory of its own, which the list's growth does not move.
struct AppDisplayList {
    RawArray<AppKnownDisplay*> displays;
};

// Writes the name kept for the display `id` to `out`, and returns true, or returns false if the list holds no such
// display.
bool appDisplayListFind(const AppDisplayList& list, PrAppDisplayId id, PrStringView* out);

// Keeps a copy of `name` for the display `id`, and returns the copy. A display already in the list keeps the name it
// has.
PrStringView appDisplayListAdd(AppDisplayList& list, PrAppDisplayId id, PrStringView name);

// Frees the displays and their names.
void appDisplayListReset(AppDisplayList& list);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
