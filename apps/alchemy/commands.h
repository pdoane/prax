// Alchemy's commands and key bindings. Every verb of the application is a command, which a button, a menu item, the
// palette, a chord and automation run alike.

#pragma once

#include "pr/app.h"
#include "pr/base.h"
#include "pr/command.h"
#include "pr/command_keymap.h"
#include "pr/string_view.h"
#include "pr/ui.h"

#include "apps/alchemy/state.h"

PR_ASSUME_NONNULL_BEGIN

namespace alchemy {

using namespace pr;

// The contexts of the key bindings, which the view puts on its panels: the application's own, which applies everywhere,
// the parameters panel, the panels that show the gallery, and the dialog of an import.
constexpr PrStringView kContextParameters = "parameters";
constexpr PrStringView kContextGallery = "gallery";
constexpr PrStringView kContextImport = "import";

// What the commands act on besides the state: the window's context, and the requests that the view takes up.
struct Shell {
    AppState* state;
    PrUi* PR_NULLABLE ui = nullptr;
    PrApp* PR_NULLABLE app = nullptr;

    // The prompt fields as the last build made them, which the weight commands edit while one has focus.
    PrLayoutNode promptFields[2] = {kPrLayoutNodeNone, kPrLayoutNodeNone};

    // Whether each prompt field has held focus, and so has a caret that the user placed.
    bool promptTouched[2] = {false, false};

    bool paletteRequested = false;
};

// What the registry passes to a command, one for each command.
struct CommandContext;

// The commands and bindings of one shell.
struct Commands {
    PrCommandRegistry* registry;
    PrCommandKeymap* keymap;
    Shell* shell;
    CommandContext* contexts;
};

// Creates the commands of `shell`, binding each one's chords, or frees them.
Commands* commandsCreate(Shell* shell);
void commandsDestroy(Commands* PR_NULLABLE commands);

// Writes the token of the embedding `filename` into prompt field `field`, 0 for the prompt and 1 for the negative: over
// the selection of a field that the user has placed a caret in, and at the end of any other. The caret lands after it.
void insertEmbedding(Shell& shell, int field, PrStringView filename);

// Runs the command `id`, as a button does.
void runCommand(const Commands& commands, PrStringView id);

} // namespace alchemy

PR_ASSUME_NONNULL_END
