// A debugging session of the editor (text_editor_debug.h): the adapter's bytes into the client and the client's into
// the adapter, and the requests that follow a stop: the stack, the scopes of its top frame, and their first scope's
// variables.

#include "studio/text_editor/src/text_editor_debug.h"

#include "pr/alloc.h"
#include "pr/string_ops.h"
#include "pr/thread.h"
#include "pr/time.h"

namespace pr_impl {

namespace {

// How long an adapter has to exit once it is asked to, in milliseconds.
constexpr double kExitPatience = 300;

// The bytes that one read of the adapter takes at most.
constexpr uint32_t kChunkBytes = 64 * 1024;

// The most lines of output, and of variables, that a session keeps.
constexpr uint32_t kOutputMaxLines = 200;
constexpr uint32_t kVariablesMax = 200;

void wakeApp(void* PR_NULLABLE context) {
    PrApp* app = static_cast<PrApp*>(context);
    if (app != nullptr)
        prAppWake(app);
}

void writeToAdapter(void* PR_NULLABLE context, PrByteView bytes) {
    TextEditorDebugSession* session = static_cast<TextEditorDebugSession*>(context);
    if (session->process != nullptr)
        prProcessWrite(session->process, bytes);
}

void clearFrames(TextEditorDebugSession* session) {
    for (uint32_t i = 0; i < session->frames.count; ++i) {
        prRawStringReset(&session->frames.data[i].name);
        prRawStringReset(&session->frames.data[i].path);
    }
    session->frames.clear();
}

void clearVariables(TextEditorDebugSession* session) {
    for (uint32_t i = 0; i < session->variables.count; ++i) {
        prRawStringReset(&session->variables.data[i].name);
        prRawStringReset(&session->variables.data[i].value);
    }
    session->variables.clear();
}

// Adds the debuggee's output, kept line by line.
void keepOutput(TextEditorDebugSession* session, PrStringView text) {
    for (size_t i = 0; i < text.size; ++i) {
        if (text.data[i] != '\n') {
            prRawStringAppend(&session->partial, PrStringView{text.data + i, 1});
            continue;
        }
        if (session->output.count == kOutputMaxLines) {
            prRawStringReset(&session->output.data[0]);
            session->output.erase(0);
        }
        session->output.push(session->partial);
        session->partial = prRawStringEmpty();
    }
}

void noteEvent(void* PR_NULLABLE context, const PrDapEvent* event) {
    TextEditorDebugSession* session = static_cast<TextEditorDebugSession*>(context);
    if (event->event == "output") {
        PrStringView category = prJsonString(event->json, prJsonMember(event->json, event->body, "category"));
        if (category != "telemetry")
            keepOutput(session, prJsonString(event->json, prJsonMember(event->json, event->body, "output")));
    } else if (event->event == "stopped") {
        session->asked = false;
    }
}

void variablesAnswered(void* PR_NULLABLE context, const PrDapResponse* response) {
    TextEditorDebugSession* session = static_cast<TextEditorDebugSession*>(context);
    clearVariables(session);
    if (!response->success)
        return;
    PrDapVariable variables[kVariablesMax];
    uint32_t count = prDapVariables(response->json, response->body, variables, kVariablesMax);
    count = count < kVariablesMax ? count : kVariablesMax;
    for (uint32_t i = 0; i < count; ++i)
        session->variables.push(TextEditorDebugVariable{
            prRawStringCopy(variables[i].name),
            prRawStringCopy(variables[i].value),
        });
}

void scopesAnswered(void* PR_NULLABLE context, const PrDapResponse* response) {
    TextEditorDebugSession* session = static_cast<TextEditorDebugSession*>(context);
    PrDapVariable scope{};
    if (response->success && prDapScopes(response->json, response->body, &scope, 1) != 0 && scope.reference != 0)
        prDapClientVariables(session->client, scope.reference, variablesAnswered, session);
}

void stackAnswered(void* PR_NULLABLE context, const PrDapResponse* response) {
    TextEditorDebugSession* session = static_cast<TextEditorDebugSession*>(context);
    clearFrames(session);
    clearVariables(session);
    if (response->success) {
        uint32_t count = prDapFrames(response->json, response->body, nullptr, 0);
        RawArray<PrDapFrame> frames;
        frames.resize(count);
        prDapFrames(response->json, response->body, frames.data, count);
        for (uint32_t i = 0; i < count; ++i) {
            const PrDapFrame& frame = frames.data[i];
            session->frames.push(TextEditorDebugFrame{
                .id = frame.id,
                .name = prRawStringCopy(frame.name),
                .path = prRawStringCopy(frame.path),
                .line = frame.line,
            });
        }
        frames.reset();
        if (session->frames.count != 0)
            prDapClientScopes(session->client, session->frames.data[0].id, scopesAnswered, session);
    }
    ++session->stops;
}

} // namespace

TextEditorDebugSession* textEditorDebugCreate(const TextEditorDebugConfig& config,
        PrProcessLauncher* PR_NULLABLE launcher, PrApp* PR_NULLABLE app) {
    TextEditorDebugSession* session = allocZeroed<TextEditorDebugSession>();
    session->partial = prRawStringEmpty();
    RawArray<PrStringView> arguments;
    for (uint32_t i = 0; i < config.arguments.count; ++i)
        arguments.push(prRawStringView(&config.arguments.data[i]));
    PrProcessDesc process{
        .program = prRawStringView(&config.program),
        .arguments = arguments.data,
        .argumentCount = arguments.count,
        .errors = PrProcessErrors_Discard,
        .wake = wakeApp,
        .wakeContext = app,
    };
    PrProcess* started = nullptr;
    if (launcher != nullptr && prProcessLaunch(launcher, &process, &started) == PrStatus_Ok)
        session->process = started;
    arguments.reset();
    PrDapClientDesc client{
        .adapterId = prRawStringView(&config.adapterId),
        .clientName = "Prax Studio",
        .write = writeToAdapter,
        .writeContext = session,
        .event = noteEvent,
        .eventContext = session,
    };
    session->client = prDapClientCreate(&client);
    prDapClientLaunch(session->client, prRawStringView(&config.launch));
    return session;
}

void textEditorDebugDestroy(TextEditorDebugSession* session) {
    if (session->process != nullptr) {
        prDapClientDisconnect(session->client);
        double end = prTimeMonotonicMilliseconds() + kExitPatience;
        while (!prProcessInfo(session->process).exited && prTimeMonotonicMilliseconds() < end) {
            if (!textEditorDebugPump(session))
                prThreadSleep(1);
        }
    }
    prDapClientDestroy(session->client);
    prProcessDestroy(session->process);
    clearFrames(session);
    clearVariables(session);
    session->frames.reset();
    session->variables.reset();
    for (uint32_t i = 0; i < session->output.count; ++i)
        prRawStringReset(&session->output.data[i]);
    session->output.reset();
    prRawStringReset(&session->partial);
    session->chunk.reset();
    prMemoryFree(session);
}

bool textEditorDebugPump(TextEditorDebugSession* session) {
    if (session->process == nullptr)
        return false;
    session->chunk.resize(kChunkBytes);
    bool arrived = false;
    while (size_t got = prProcessRead(session->process, PrProcessStream_Output, session->chunk.data, kChunkBytes)) {
        prDapClientReceive(session->client, PrByteView{session->chunk.data, got});
        arrived = true;
    }
    if (prDapClientState(session->client) == PrDapClientState_Stopped && !session->asked) {
        session->asked = true;
        prDapClientStackTrace(session->client, prDapClientStoppedThread(session->client), stackAnswered, session);
    }
    return arrived;
}

PrTextEditorDebugState textEditorDebugState(const TextEditorDebugSession* session) {
    if (session->process == nullptr)
        return PrTextEditorDebugState_Failed;
    PrDapClientState state = prDapClientState(session->client);
    // An adapter that exits has ended its session, whatever the client last heard.
    if (prProcessInfo(session->process).exited && state != PrDapClientState_Failed)
        return state == PrDapClientState_Terminated ? PrTextEditorDebugState_Ended : PrTextEditorDebugState_Failed;
    switch (state) {
        case PrDapClientState_Initializing:
        case PrDapClientState_Ready:
        case PrDapClientState_Configuring: return PrTextEditorDebugState_Starting;
        case PrDapClientState_Running: return PrTextEditorDebugState_Running;
        case PrDapClientState_Stopped: return PrTextEditorDebugState_Stopped;
        case PrDapClientState_Terminated: return PrTextEditorDebugState_Ended;
        default: break;
    }
    return PrTextEditorDebugState_Failed;
}

} // namespace pr_impl
