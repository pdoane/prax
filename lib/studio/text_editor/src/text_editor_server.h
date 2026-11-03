// The language servers that the editor runs: a process of the server's program for a workspace, and the protocol's
// client over its pipes.

#pragma once

#include "pr/app.h"
#include "pr/array.h"
#include "pr/base.h"
#include "pr/lsp.h"
#include "pr/process.h"
#include "pr/string.h"
#include "pr/text_editor.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A server that the application added: the languages that it serves, and the program that runs it.
struct TextEditorServerConfig {
    Array<PrString> languages;
    PrString program;
    Array<PrString> arguments;
};

// A server that runs for one workspace.
struct TextEditorServer {
    uint32_t config;                // by index of the editor's configs
    PrString root;                  // the workspace's folder
    PrProcess* PR_NULLABLE process; // null if the program could not start
    PrLspClient* client;
    PrString message;              // what the server last asked the user to see (window/showMessage), or empty
    Array<uint8_t> chunk;
};

// Starts the server of `config` for the workspace at `root` by `launcher`, whose threads wake `app`. A program that
// cannot start, or no launcher, leaves a server whose state is Failed.
TextEditorServer* textEditorServerCreate(const TextEditorServerConfig& config, uint32_t index, PrStringView root,
    PrProcessLauncher* PR_NULLABLE launcher, PrApp* PR_NULLABLE app);

// Asks the server to exit, waits a short time for it to, and frees it. A server that has not exited by then ends.
void textEditorServerDestroy(TextEditorServer* server);

// Gives the client what the process sent. Returns true if anything arrived.
bool textEditorServerPump(TextEditorServer* server);

// Returns where the server stands.
PrTextEditorServerState textEditorServerState(const TextEditorServer* server);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
