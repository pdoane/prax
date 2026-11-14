// The data's pages: the clipboard in each form and the selection, drags in and out of the window with a file that a
// drag promises, and file dialogs, the file manager and the trash.

#pragma once

#include "pr/app.h"
#include "pr/studio_app.h"

#include "examples/app_features/features.h"

PR_ASSUME_NONNULL_BEGIN

namespace features {

// Shows a file dialog over the control window. The first file of its answer becomes the last path, and the log lists
// every file. A Save dialog's file gets a line of text.
void showDialog(Features& f, PrAppFileDialogKind kind, PrAppFileDialogFlags flags);

// Build the pages under `parent`, a column of the control window.
void pageClipboard(Features& f, PrStudioWindow* window, PrLayoutNode parent);
void pageDrags(Features& f, PrStudioWindow* window, PrLayoutNode parent);
void pageFiles(Features& f, PrStudioWindow* window, PrLayoutNode parent);

// Writes the file that a drop asks a promise for. Returns true for an event that it consumed.
bool dataWindowEvent(Features& f, const PrAppEvent& e);

// Counts the clipboard's changes. Returns true for an event that it took.
bool dataAppEvent(Features& f, const PrAppEvent& e);

} // namespace features

PR_ASSUME_NONNULL_END
