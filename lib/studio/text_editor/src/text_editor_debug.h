// A debugging session of the editor: a process of the debug adapter, the protocol's client over its pipes, and what the
// editor shows of a stop, the stack of the stopped thread and the variables of its top frame.

#pragma once

#include "pr/app.h"
#include "pr/array.h"
#include "pr/base.h"
#include "pr/dap.h"
#include "pr/process.h"
#include "pr/string.h"
#include "pr/text_editor.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The debugger that the application set.
struct TextEditorDebugConfig {
    PrString program;
    Array<PrString> arguments;
    PrString adapterId;
    PrString launch;
};

// A frame of the stopped thread's stack.
struct TextEditorDebugFrame {
    int64_t id;
    PrString name;
    PrString path;                  // empty for a frame with no file
    uint32_t line;
};

// A variable of the top frame: its name, and its value as the adapter shows it.
struct TextEditorDebugVariable {
    PrString name;
    PrString value;
};

// A session, and what the editor shows of it.
struct TextEditorDebugSession {
    PrProcess* PR_NULLABLE process;     // null if the adapter could not start
    PrDapClient* client;
    Array<uint8_t> chunk;

    // What the last stop showed, which arrives in answers after the stop. `stops` counts the stops whose stacks
    // arrived, so the editor knows when to show a stop's frame.
    Array<TextEditorDebugFrame> frames;
    Array<TextEditorDebugVariable> variables;
    uint64_t stops;
    bool asked;                     // the stack of the current stop was asked for

    Array<PrString> output;         // the debuggee's last lines of output, oldest first
    PrString partial;               // output after its last line feed
};

// Starts the adapter of `config` by `launcher`, whose threads wake `app`, and launches the debuggee, with breakpoints
// set later. An adapter that cannot start, or no launcher, leaves a session whose state is Failed.
TextEditorDebugSession* textEditorDebugCreate(const TextEditorDebugConfig& config,
    PrProcessLauncher* PR_NULLABLE launcher, PrApp* PR_NULLABLE app);

// Asks the adapter to end the debuggee and to exit, waits a short time for it to, and frees the session.
void textEditorDebugDestroy(TextEditorDebugSession* session);

// Gives the client what the adapter sent, and asks for what a new stop shows. Returns true if anything arrived.
bool textEditorDebugPump(TextEditorDebugSession* session);

// Returns where the session stands.
PrTextEditorDebugState textEditorDebugState(const TextEditorDebugSession* session);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
