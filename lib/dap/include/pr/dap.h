// Debuggers: a client of the Debug Adapter Protocol, over a transport that its host provides.
//
// dap - https://microsoft.github.io/debug-adapter-protocol/specification
//
// The client writes what it sends through a function of its host, and its host gives it what the adapter sends, so the
// client does no I/O of its own, as a language server's client does (pr/lsp.h). The client parses what it receives on
// the thread that gives it. One thread uses a client at a time.
//
// A session initializes the adapter, then launches or attaches to a debuggee as the host asks. Once the adapter says it
// is ready for its configuration, the client sends the breakpoints that the host set, and ends the configuration. Lines
// are counted from zero everywhere in the API, and the client converts for the adapter, which counts from one. Columns
// are the adapter's, as it reports them.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/json.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Clients

// A session with one debug adapter.
typedef struct PrDapClient PrDapClient;

// Called with bytes for the adapter, in the order that they are sent. The bytes are valid only during the call.
typedef void (*PrDapWriteFn)(void* PR_NULLABLE context, PrByteView bytes);

// An event of the adapter, such as `stopped` or `output`, after the client has recorded what it says of the session.
// The JSON is valid only during the call.
typedef struct PrDapEvent {
    PrStringView event;
    const PrJson* json;
    PrJsonValueIndex body;      // kPrJsonMissing if the event has none
} PrDapEvent;

// Called with each event of the adapter.
typedef void (*PrDapEventFn)(void* PR_NULLABLE context, const PrDapEvent* event);

// What a client connects as.
typedef struct PrDapClientDesc {
    PrStringView adapterId;     // the kind of adapter, as the protocol's initialize request names it, such as "lldb"
    PrStringView clientName;    // the name that the adapter is told, or the null view for none

    PrDapWriteFn write;
    void* PR_NULLABLE writeContext;

    // Called with the adapter's events, or null to drop them.
    PrDapEventFn PR_NULLABLE event;
    void* PR_NULLABLE eventContext;
} PrDapClientDesc;

// Where a session stands. A session initializes, then is ready to launch or attach, then configures once the adapter is
// ready, then runs, and stops at a breakpoint or a step, until the debuggee ends. It fails if the adapter sends what is
// not the protocol, refuses to initialize, or refuses to launch.
enum {
    PrDapClientState_Initializing,
    PrDapClientState_Ready,
    PrDapClientState_Configuring,
    PrDapClientState_Running,
    PrDapClientState_Stopped,
    PrDapClientState_Terminated,
    PrDapClientState_Failed,
};
typedef uint8_t PrDapClientState;

// Creates a client and writes its initialize request, or frees one. Freeing a client drops the callbacks of the
// requests that it has not had answers to.
PrDapClient* prDapClientCreate(const PrDapClientDesc* desc);
void prDapClientDestroy(PrDapClient* PR_NULLABLE client);

// Reads bytes that the adapter sent, which may end anywhere in a message, and handles each message that they complete.
// A client that fails reads nothing more.
void prDapClientReceive(PrDapClient* client, PrByteView bytes);

// The client's state, and the thread that the debuggee last stopped on, or zero, with why it stopped, such as
// "breakpoint" or "step", or empty, until it runs again.
PrDapClientState prDapClientState(const PrDapClient* client);
int64_t prDapClientStoppedThread(const PrDapClient* client);
PrStringView prDapClientStopReason(const PrDapClient* client);

// The exit code of the debuggee, once the adapter reported it, or -1.
int64_t prDapClientExitCode(const PrDapClient* client);

// Launches or attaches to the debuggee, with the arguments that the adapter takes as the JSON text `arguments`, such as
// {"program": "/path/to/program"}. Returns false, and sends nothing, if the session is not ready or the arguments are
// not a JSON object. A session that is still initializing launches once it is ready.
bool prDapClientLaunch(PrDapClient* client, PrStringView arguments);
bool prDapClientAttach(PrDapClient* client, PrStringView arguments);

// Ends the session: asks the adapter to disconnect, which ends a debuggee that it launched.
void prDapClientDisconnect(PrDapClient* client);

// ---------------------------------------------------------------------------------------------------------------------
// Breakpoints

// A breakpoint as the adapter reports it: whether it holds, and the line that it moved to, which may differ from the
// line it was set on.
typedef struct PrDapBreakpoint {
    uint32_t line;
    bool verified;
} PrDapBreakpoint;

// Replaces the breakpoints of the file at `path` with breakpoints on `lines`. They reach the adapter once it is ready
// for its configuration, and at once after that.
void prDapClientSetBreakpoints(PrDapClient* client, PrStringView path, const uint32_t* PR_NULLABLE lines, uint32_t count);

// The breakpoints of the file at `path`, as the adapter last reported them, or as they were set until it reports them.
// The pointer is valid until the client changes.
uint32_t prDapClientBreakpointCount(const PrDapClient* client, PrStringView path);
const PrDapBreakpoint* PR_NULLABLE prDapClientBreakpoints(const PrDapClient* client, PrStringView path);

// ---------------------------------------------------------------------------------------------------------------------
// Requests

// A request that the client sent, by the number that names it in the protocol. kPrDapRequestNone is no request.
typedef uint32_t PrDapRequest;
enum { kPrDapRequestNone = 0 };

// The answer to a request: its body on success, which may be missing, or the adapter's message on failure. The JSON is
// valid only during the call.
typedef struct PrDapResponse {
    PrDapRequest request;
    bool success;
    const PrJson* json;
    PrJsonValueIndex body;      // kPrJsonMissing if the answer has none
    PrStringView message;
} PrDapResponse;

// Called with the answer to a request.
typedef void (*PrDapResponseFn)(void* PR_NULLABLE context, const PrDapResponse* response);

// Sends `command` with the JSON text `arguments`, or none for the null view, and returns it. Returns kPrDapRequestNone,
// and sends nothing, if the session has not initialized, has ended, or `arguments` is not JSON. A null `fn` takes no
// answer.
PrDapRequest prDapClientRequest(PrDapClient* client, PrStringView command, PrStringView arguments,
    PrDapResponseFn PR_NULLABLE fn, void* PR_NULLABLE context);

// The motions of a stopped debuggee: run until the next stop, the next line, into a call, out of the current function.
enum {
    PrDapStep_Continue,
    PrDapStep_Over,
    PrDapStep_Into,
    PrDapStep_Out,
};
typedef uint8_t PrDapStep;

// Moves `thread` as `step` says, and the session runs. Returns kPrDapRequestNone unless the session is stopped.
PrDapRequest prDapClientStep(PrDapClient* client, int64_t thread, PrDapStep step);

// Ask for the threads, the frames of `thread`'s stack, the scopes of `frame`, or the variables of `reference`, as the
// readers below read the answers.
PrDapRequest prDapClientThreads(PrDapClient* client, PrDapResponseFn fn, void* PR_NULLABLE context);
PrDapRequest prDapClientStackTrace(PrDapClient* client, int64_t thread, PrDapResponseFn fn, void* PR_NULLABLE context);
PrDapRequest prDapClientScopes(PrDapClient* client, int64_t frame, PrDapResponseFn fn, void* PR_NULLABLE context);
PrDapRequest prDapClientVariables(PrDapClient* client, int64_t reference, PrDapResponseFn fn, void* PR_NULLABLE context);

// ---------------------------------------------------------------------------------------------------------------------
// Results

// A frame of a stack. Its strings are valid as long as the JSON that it was read from.
typedef struct PrDapFrame {
    int64_t id;
    PrStringView name;
    PrStringView path;          // the frame's source file, or empty if it has none
    uint32_t line;
    uint32_t column;
} PrDapFrame;

// A scope or a variable: a name, and for a variable its value and type, and the reference of what it holds, which is
// zero if it holds nothing. Its strings are valid as long as the JSON that it was read from.
typedef struct PrDapVariable {
    PrStringView name;
    PrStringView value;
    PrStringView type;
    int64_t reference;
} PrDapVariable;

// A thread of the debuggee. Its name is valid as long as the JSON that it was read from.
typedef struct PrDapThread {
    int64_t id;
    PrStringView name;
} PrDapThread;

// Write up to `capacity` of the frames of a stackTrace body, the scopes of a scopes body, the variables of a variables
// body, or the threads of a threads body to `out`, and return their number.
uint32_t prDapFrames(const PrJson* json, PrJsonValueIndex body, PrDapFrame* PR_NULLABLE out, uint32_t capacity);
uint32_t prDapScopes(const PrJson* json, PrJsonValueIndex body, PrDapVariable* PR_NULLABLE out, uint32_t capacity);
uint32_t prDapVariables(const PrJson* json, PrJsonValueIndex body, PrDapVariable* PR_NULLABLE out, uint32_t capacity);
uint32_t prDapThreads(const PrJson* json, PrJsonValueIndex body, PrDapThread* PR_NULLABLE out, uint32_t capacity);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
