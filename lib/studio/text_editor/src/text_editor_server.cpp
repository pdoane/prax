// The language servers that the editor runs (text_editor_server.h): the process's bytes into the client, and the
// client's into the process.

#include "studio/text_editor/src/text_editor_server.h"

#include "pr/alloc.h"
#include "pr/thread.h"
#include "pr/time.h"

namespace pr_impl {

namespace {

// How long a server has to exit once it is asked to, in milliseconds.
constexpr double kExitPatience = 300;

// The bytes that one read of the process takes at most.
constexpr uint32_t kChunkBytes = 64 * 1024;

void wakeApp(void* PR_NULLABLE context) {
    PrApp* app = static_cast<PrApp*>(context);
    if (app != nullptr)
        prAppWake(app);
}

void writeToProcess(void* PR_NULLABLE context, PrByteView bytes) {
    TextEditorServer* server = static_cast<TextEditorServer*>(context);
    if (server->process != nullptr)
        prProcessWrite(server->process, bytes);
}

void keepMessage(void* PR_NULLABLE context, const PrLspMessage* message) {
    TextEditorServer* server = static_cast<TextEditorServer*>(context);
    if (message->method != "window/showMessage")
        return;
    PrJsonValueIndex text = prJsonMember(message->json, message->params, "message");
    prRawStringReplace(&server->message, prJsonString(message->json, text));
}

} // namespace

TextEditorServer* textEditorServerCreate(const TextEditorServerConfig& config, uint32_t index, PrStringView root,
        PrProcessLauncher* PR_NULLABLE launcher, PrApp* PR_NULLABLE app) {
    TextEditorServer* server = allocZeroed<TextEditorServer>();
    server->config = index;
    server->root = prRawStringCopy(root);
    server->message = prRawStringEmpty();
    RawArray<PrStringView> arguments;
    for (uint32_t i = 0; i < config.arguments.count; ++i)
        arguments.push(prRawStringView(&config.arguments.data[i]));
    PrProcessDesc process{
        .program = prRawStringView(&config.program),
        .arguments = arguments.data,
        .argumentCount = arguments.count,
        .directory = root,
        .errors = PrProcessErrors_Discard,
        .wake = wakeApp,
        .wakeContext = app,
    };
    PrProcess* started = nullptr;
    if (launcher != nullptr && prProcessLaunch(launcher, &process, &started) == PrStatus_Ok)
        server->process = started;
    arguments.reset();
    PrLspClientDesc client{
        .rootPath = root,
        .clientName = "Prax Studio",
        .write = writeToProcess,
        .writeContext = server,
        .message = keepMessage,
        .messageContext = server,
    };
    server->client = prLspClientCreate(&client);
    return server;
}

void textEditorServerDestroy(TextEditorServer* server) {
    if (server->process != nullptr) {
        prLspClientShutdown(server->client);
        double end = prTimeMonotonicMilliseconds() + kExitPatience;
        while (!prProcessInfo(server->process).exited && prTimeMonotonicMilliseconds() < end) {
            if (!textEditorServerPump(server))
                prThreadSleep(1);
            // A server that stopped answering is asked to exit all the same, by the end of its input.
            if (prLspClientState(server->client) == PrLspClientState_Stopped)
                prProcessCloseInput(server->process);
        }
    }
    prLspClientDestroy(server->client);
    prProcessDestroy(server->process);
    prRawStringReset(&server->root);
    prRawStringReset(&server->message);
    server->chunk.reset();
    prMemoryFree(server);
}

bool textEditorServerPump(TextEditorServer* server) {
    if (server->process == nullptr)
        return false;
    server->chunk.resize(kChunkBytes);
    bool arrived = false;
    while (size_t got = prProcessRead(server->process, PrProcessStream_Output, server->chunk.data, kChunkBytes)) {
        prLspClientReceive(server->client, PrByteView{server->chunk.data, got});
        arrived = true;
    }
    return arrived;
}

PrTextEditorServerState textEditorServerState(const TextEditorServer* server) {
    if (server->process == nullptr || prProcessInfo(server->process).exited)
        return PrTextEditorServerState_Failed;
    switch (prLspClientState(server->client)) {
        case PrLspClientState_Initializing: return PrTextEditorServerState_Starting;
        case PrLspClientState_Running: return PrTextEditorServerState_Running;
        default: return PrTextEditorServerState_Failed;
    }
}

} // namespace pr_impl
