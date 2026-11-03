// Debuggers (dap.h): the base protocol's framing, the protocol's requests, answers and events, and the lifecycle of a
// session, with the breakpoints that it configures.

#include "pr/dap.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/integer.h"
#include "pr/string.h"
#include "pr/string_ops.h"

namespace pr_impl {

namespace {

// The longest header of a message that the client reads. An adapter whose header runs longer is not speaking the
// protocol.
constexpr uint32_t kHeaderMaxBytes = 4096;

// The longest message that the client reads.
constexpr uint64_t kMessageMaxBytes = 1ull << 30;

// What the answer to a request does besides reaching its callback.
enum {
    PendingKind_User,
    PendingKind_Initialize,
    PendingKind_Start,              // launch or attach
    PendingKind_Breakpoints,
    PendingKind_Configured,
};
typedef uint8_t PendingKind;

struct DapPending {
    PrDapRequest request;
    PendingKind kind;
    uint32_t source;                // by index of the client's sources, for Breakpoints
    PrDapResponseFn PR_NULLABLE fn;
    void* PR_NULLABLE context;
};

// The breakpoints of one file: the lines that the host set, and what the adapter reported of them.
struct DapSource {
    PrString path;
    Array<uint32_t> lines;
    Array<PrDapBreakpoint> breakpoints;
};

struct DapClient {
    PrDapWriteFn write;
    void* PR_NULLABLE writeContext;
    PrDapEventFn PR_NULLABLE event;
    void* PR_NULLABLE eventContext;

    PrDapClientState state;
    bool configurationDone;         // the adapter takes the configurationDone request
    bool configured;                // the adapter asked for its configuration, which the client sent
    PrDapRequest nextRequest;
    Array<DapPending> pending;
    Array<DapSource> sources;

    // A launch or attach that the host asked for while the session initialized, as its command and arguments.
    PrString startCommand;
    PrString startArguments;

    int64_t stoppedThread;
    PrString stopReason;
    int64_t exitCode;

    Array<uint8_t> received;        // what arrived and was not handled, from `receivedStart`
    uint32_t receivedStart;
    PrJson* json;                   // the message being handled
    PrJson* arguments;              // the arguments that a host gave as text
    PrJsonWriter* writer;
    Array<uint8_t> frame;
};

// Sends the writer's text as a message, after its header.
void sendWritten(DapClient* client) {
    PrStringView body = prJsonWriterText(client->writer);
    char digits[kPrU64TextMaxBytes];
    size_t length = prU64Format(body.size, digits, 10);
    client->frame.clear();
    const char header[] = "Content-Length: ";
    client->frame.append(reinterpret_cast<const uint8_t*>(header), sizeof(header) - 1);
    client->frame.append(reinterpret_cast<const uint8_t*>(digits), uint32_t(length));
    client->frame.append(reinterpret_cast<const uint8_t*>("\r\n\r\n"), 4);
    client->frame.append(reinterpret_cast<const uint8_t*>(body.data), uint32_t(body.size));
    client->write(client->writeContext, PrByteView{client->frame.data, client->frame.count});
}

// Begins a request up to the value of its arguments, which the caller writes before it ends the message, and records
// what its answer does.
PrDapRequest beginRequest(DapClient* client, PrStringView command, PendingKind kind, PrDapResponseFn PR_NULLABLE fn,
        void* PR_NULLABLE context, uint32_t source = 0) {
    PrDapRequest request = ++client->nextRequest;
    prJsonWriterClear(client->writer);
    prJsonWriterBeginObject(client->writer);
    prJsonWriterKey(client->writer, "seq");
    prJsonWriterNumber(client->writer, double(request));
    prJsonWriterKey(client->writer, "type");
    prJsonWriterString(client->writer, "request");
    prJsonWriterKey(client->writer, "command");
    prJsonWriterString(client->writer, command);
    prJsonWriterKey(client->writer, "arguments");
    client->pending.push(DapPending{request, kind, source, fn, context});
    return request;
}

void endRequest(DapClient* client) {
    prJsonWriterEndObject(client->writer);
    sendWritten(client);
}

void writeKeyString(PrJsonWriter* writer, PrStringView key, PrStringView value) {
    prJsonWriterKey(writer, key);
    prJsonWriterString(writer, value);
}

void writeKeyBool(PrJsonWriter* writer, PrStringView key, bool value) {
    prJsonWriterKey(writer, key);
    prJsonWriterBool(writer, value);
}

void writeKeyNumber(PrJsonWriter* writer, PrStringView key, double value) {
    prJsonWriterKey(writer, key);
    prJsonWriterNumber(writer, value);
}

PrJsonValueIndex member(const PrJson* json, PrJsonValueIndex object, PrStringView key) {
    return object != kPrJsonMissing ? prJsonMember(json, object, key) : kPrJsonMissing;
}

int64_t integerMember(const PrJson* json, PrJsonValueIndex object, PrStringView key, int64_t fallback) {
    PrJsonValueIndex value = member(json, object, key);
    if (prJsonKind(json, value) != PrJsonKind_Number)
        return fallback;
    double number = prJsonNumber(json, value, 0);
    return number > -9.0e18 && number < 9.0e18 ? int64_t(number) : fallback;
}

// Returns a line of the adapter's, which counts from one, as the API counts lines, from zero.
uint32_t lineFromAdapter(int64_t line) {
    return line > 1 && line <= int64_t(UINT32_MAX) ? uint32_t(line - 1) : 0;
}

PrStringView stringMember(const PrJson* json, PrJsonValueIndex object, PrStringView key) {
    PrStringView text = prJsonString(json, member(json, object, key));
    return text.data != nullptr ? text : PrStringView("");
}

void fail(DapClient* client) {
    client->state = PrDapClientState_Failed;
    client->pending.clear();
}

DapSource* PR_NULLABLE sourceOf(const DapClient* client, PrStringView path) {
    for (uint32_t i = 0; i < client->sources.count; ++i) {
        if (prStringView(&client->sources.data[i].path) == path)
            return &client->sources.data[i];
    }
    return nullptr;
}

void sendBreakpoints(DapClient* client, uint32_t index) {
    const DapSource& source = client->sources.data[index];
    beginRequest(client, "setBreakpoints", PendingKind_Breakpoints, nullptr, nullptr, index);
    PrJsonWriter* writer = client->writer;
    prJsonWriterBeginObject(writer);
    prJsonWriterKey(writer, "source");
    prJsonWriterBeginObject(writer);
    writeKeyString(writer, "path", prStringView(&source.path));
    prJsonWriterEndObject(writer);
    prJsonWriterKey(writer, "breakpoints");
    prJsonWriterBeginArray(writer);
    for (uint32_t i = 0; i < source.lines.count; ++i) {
        prJsonWriterBeginObject(writer);
        writeKeyNumber(writer, "line", double(source.lines.data[i]) + 1);
        prJsonWriterEndObject(writer);
    }
    prJsonWriterEndArray(writer);
    prJsonWriterEndObject(writer);
    endRequest(client);
}

// Sends a launch or attach with arguments that a host gave, parsed.
bool sendStart(DapClient* client, PrStringView command, PrStringView arguments) {
    if (!prJsonParse(client->arguments, arguments, nullptr) || prJsonKind(client->arguments, 0) != PrJsonKind_Object)
        return false;
    beginRequest(client, command, PendingKind_Start, nullptr, nullptr);
    prJsonWriterValue(client->writer, client->arguments, 0);
    endRequest(client);
    return true;
}

bool start(DapClient* client, PrStringView command, PrStringView arguments) {
    if (client->state == PrDapClientState_Initializing) {
        if (!prJsonParse(client->arguments, arguments, nullptr) ||
                prJsonKind(client->arguments, 0) != PrJsonKind_Object)
            return false;
        prStringReplace(&client->startCommand, command);
        prStringReplace(&client->startArguments, arguments);
        return true;
    }
    if (client->state != PrDapClientState_Ready)
        return false;
    return sendStart(client, command, arguments);
}

// Sends the configuration that the adapter asked for: each file's breakpoints, then its end.
void configure(DapClient* client) {
    client->configured = true;
    for (uint32_t i = 0; i < client->sources.count; ++i)
        sendBreakpoints(client, i);
    if (client->configurationDone) {
        beginRequest(client, "configurationDone", PendingKind_Configured, nullptr, nullptr);
        prJsonWriterBeginObject(client->writer);
        prJsonWriterEndObject(client->writer);
        endRequest(client);
    }
    if (client->state == PrDapClientState_Configuring || client->state == PrDapClientState_Ready)
        client->state = PrDapClientState_Running;
}

void handleResponse(DapClient* client, const PrJson* json) {
    double number = prJsonNumber(json, member(json, 0, "request_seq"), 0);
    PrDapRequest request = number >= 1 && number <= double(UINT32_MAX) ? PrDapRequest(number) : kPrDapRequestNone;
    bool success = prJsonKind(json, member(json, 0, "success")) == PrJsonKind_True;
    PrJsonValueIndex body = member(json, 0, "body");
    for (uint32_t i = 0; i < client->pending.count; ++i) {
        if (client->pending.data[i].request != request)
            continue;
        DapPending waiting = client->pending.data[i];
        client->pending.erase(i);
        switch (waiting.kind) {
            case PendingKind_Initialize:
                if (!success) {
                    fail(client);
                    return;
                }
                client->configurationDone =
                    prJsonKind(json, member(json, body, "supportsConfigurationDoneRequest")) == PrJsonKind_True;
                client->state = PrDapClientState_Ready;
                if (!prStringIsNull(&client->startCommand)) {
                    sendStart(client, prStringView(&client->startCommand), prStringView(&client->startArguments));
                    prStringReset(&client->startCommand);
                    prStringReset(&client->startArguments);
                }
                break;
            case PendingKind_Start:
                if (!success)
                    fail(client);
                break;
            case PendingKind_Breakpoints: {
                if (!success || waiting.source >= client->sources.count)
                    break;
                DapSource& source = client->sources.data[waiting.source];
                PrJsonValueIndex list = member(json, body, "breakpoints");
                source.breakpoints.clear();
                for (PrJsonValueIndex item = prJsonFirst(json, list); item != kPrJsonMissing;
                        item = prJsonAfter(json, list, item)) {
                    uint32_t at = source.breakpoints.count;
                    uint32_t asked = at < source.lines.count ? source.lines.data[at] : 0;
                    int64_t line = integerMember(json, item, "line", int64_t(asked) + 1);
                    bool verified = prJsonKind(json, member(json, item, "verified")) == PrJsonKind_True;
                    source.breakpoints.push(PrDapBreakpoint{lineFromAdapter(line), verified});
                }
                break;
            }
            default: break;
        }
        if (waiting.fn != nullptr) {
            PrDapResponse response{
                .request = request,
                .success = success,
                .json = json,
                .body = body,
                .message = stringMember(json, 0, "message"),
            };
            waiting.fn(waiting.context, &response);
        }
        return;
    }
}

void handleEvent(DapClient* client, const PrJson* json) {
    PrStringView event = stringMember(json, 0, "event");
    PrJsonValueIndex body = member(json, 0, "body");
    if (event == "initialized") {
        if (client->state != PrDapClientState_Failed && client->state != PrDapClientState_Terminated) {
            client->state = PrDapClientState_Configuring;
            configure(client);
        }
    } else if (event == "stopped") {
        client->state = PrDapClientState_Stopped;
        client->stoppedThread = integerMember(json, body, "threadId", 0);
        prStringReplace(&client->stopReason, stringMember(json, body, "reason"));
    } else if (event == "continued") {
        client->state = PrDapClientState_Running;
        client->stoppedThread = 0;
        prStringReplace(&client->stopReason, "");
    } else if (event == "exited") {
        client->exitCode = integerMember(json, body, "exitCode", -1);
    } else if (event == "terminated") {
        client->state = PrDapClientState_Terminated;
    }
    if (client->event != nullptr) {
        PrDapEvent shown{.event = event, .json = json, .body = body};
        client->event(client->eventContext, &shown);
    }
}

// Refuses a request of the adapter, such as runInTerminal, which the client does not take.
void refuseRequest(DapClient* client, const PrJson* json) {
    PrDapRequest request = ++client->nextRequest;
    prJsonWriterClear(client->writer);
    prJsonWriterBeginObject(client->writer);
    writeKeyNumber(client->writer, "seq", double(request));
    writeKeyString(client->writer, "type", "response");
    prJsonWriterKey(client->writer, "request_seq");
    prJsonWriterValue(client->writer, json, member(json, 0, "seq"));
    writeKeyBool(client->writer, "success", false);
    writeKeyString(client->writer, "command", stringMember(json, 0, "command"));
    writeKeyString(client->writer, "message", "not supported");
    prJsonWriterEndObject(client->writer);
    sendWritten(client);
}

void handleMessage(DapClient* client, PrStringView body) {
    PrJson* json = client->json;
    if (!prJsonParse(json, body, nullptr) || prJsonKind(json, 0) != PrJsonKind_Object) {
        fail(client);
        return;
    }
    PrStringView type = stringMember(json, 0, "type");
    if (type == "response")
        handleResponse(client, json);
    else if (type == "event")
        handleEvent(client, json);
    else if (type == "request")
        refuseRequest(client, json);
}

// Reads the length of a message's body from its header. Returns false if the header has none.
bool contentLength(PrStringView header, uint64_t* out) {
    size_t at = 0;
    while (at < header.size) {
        size_t end = prStringFind(prStringSlice(header, at, header.size - at), "\r\n");
        end = end == header.size - at ? header.size : at + end;
        PrStringView line = prStringSlice(header, at, end - at);
        size_t colon = prStringFindByte(line, ':');
        if (colon != line.size && prAsciiEqualsFold(prStringSlice(line, 0, colon), "content-length")) {
            PrStringView value = prStringTrim(prStringSlice(line, colon + 1, line.size - colon - 1));
            return prU64Parse(value.data, value.size, out, 10);
        }
        at = end + 2;
    }
    return false;
}

void handleReceived(DapClient* client) {
    while (client->state != PrDapClientState_Failed) {
        PrStringView waiting{
            reinterpret_cast<const char*>(client->received.data + client->receivedStart),
            client->received.count - client->receivedStart,
        };
        size_t headerEnd = prStringFind(waiting, "\r\n\r\n");
        if (headerEnd == waiting.size) {
            if (waiting.size > kHeaderMaxBytes)
                fail(client);
            return;
        }
        uint64_t length = 0;
        if (!contentLength(prStringSlice(waiting, 0, headerEnd), &length) || length > kMessageMaxBytes) {
            fail(client);
            return;
        }
        if (waiting.size - headerEnd - 4 < length)
            return;
        handleMessage(client, prStringSlice(waiting, headerEnd + 4, size_t(length)));
        client->receivedStart += uint32_t(headerEnd + 4 + length);
    }
}

// Returns true if the session can take requests.
bool isLive(const DapClient* client) {
    return client->state != PrDapClientState_Initializing && client->state != PrDapClientState_Terminated &&
        client->state != PrDapClientState_Failed;
}

PrDapRequest requestOf(DapClient* client, PrStringView command, PrStringView key, int64_t value, PrDapResponseFn fn,
        void* PR_NULLABLE context) {
    if (!isLive(client))
        return kPrDapRequestNone;
    PrDapRequest request = beginRequest(client, command, PendingKind_User, fn, context);
    prJsonWriterBeginObject(client->writer);
    if (key.size != 0)
        writeKeyNumber(client->writer, key, double(value));
    prJsonWriterEndObject(client->writer);
    endRequest(client);
    return request;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrDapClient : DapClient {};

PrDapClient* prDapClientCreate(const PrDapClientDesc* desc) {
    PrDapClient* client = allocZeroed<PrDapClient>();
    client->write = desc->write;
    client->writeContext = desc->writeContext;
    client->event = desc->event;
    client->eventContext = desc->eventContext;
    client->json = prJsonCreate();
    client->arguments = prJsonCreate();
    client->writer = prJsonWriterCreate();
    client->startCommand = prStringNull();
    client->startArguments = prStringNull();
    client->stopReason = prStringEmpty();
    client->exitCode = -1;
    beginRequest(client, "initialize", PendingKind_Initialize, nullptr, nullptr);
    PrJsonWriter* writer = client->writer;
    prJsonWriterBeginObject(writer);
    writeKeyString(writer, "clientID", "prax");
    writeKeyString(writer, "clientName", desc->clientName.data != nullptr ? desc->clientName : PrStringView("Prax"));
    writeKeyString(writer, "adapterID", desc->adapterId);
    writeKeyString(writer, "pathFormat", "path");
    writeKeyBool(writer, "linesStartAt1", true);
    writeKeyBool(writer, "columnsStartAt1", true);
    writeKeyBool(writer, "supportsVariableType", true);
    writeKeyBool(writer, "supportsRunInTerminalRequest", false);
    prJsonWriterEndObject(writer);
    endRequest(client);
    return client;
}

void prDapClientDestroy(PrDapClient* PR_NULLABLE client) {
    if (client == nullptr)
        return;
    for (uint32_t i = 0; i < client->sources.count; ++i) {
        prStringReset(&client->sources.data[i].path);
        client->sources.data[i].lines.reset();
        client->sources.data[i].breakpoints.reset();
    }
    client->sources.reset();
    client->pending.reset();
    client->received.reset();
    client->frame.reset();
    prJsonDestroy(client->json);
    prJsonDestroy(client->arguments);
    prJsonWriterDestroy(client->writer);
    prStringReset(&client->startCommand);
    prStringReset(&client->startArguments);
    prStringReset(&client->stopReason);
    prMemoryFree(client);
}

void prDapClientReceive(PrDapClient* client, PrByteView bytes) {
    if (client->state == PrDapClientState_Failed || bytes.size == 0)
        return;
    if (client->receivedStart == client->received.count) {
        client->received.clear();
        client->receivedStart = 0;
    }
    PR_ASSERT(bytes.size <= UINT32_MAX - client->received.count);
    client->received.append(bytes.data, uint32_t(bytes.size));
    handleReceived(client);
    if (client->receivedStart != 0 && client->receivedStart * 2 >= client->received.count) {
        client->received.erase(0, client->receivedStart);
        client->receivedStart = 0;
    }
}

PrDapClientState prDapClientState(const PrDapClient* client) {
    return client->state;
}

int64_t prDapClientStoppedThread(const PrDapClient* client) {
    return client->state == PrDapClientState_Stopped ? client->stoppedThread : 0;
}

PrStringView prDapClientStopReason(const PrDapClient* client) {
    return client->state == PrDapClientState_Stopped ? prStringView(&client->stopReason) : PrStringView("");
}

int64_t prDapClientExitCode(const PrDapClient* client) {
    return client->exitCode;
}

bool prDapClientLaunch(PrDapClient* client, PrStringView arguments) {
    return start(client, "launch", arguments);
}

bool prDapClientAttach(PrDapClient* client, PrStringView arguments) {
    return start(client, "attach", arguments);
}

void prDapClientDisconnect(PrDapClient* client) {
    if (!isLive(client))
        return;
    beginRequest(client, "disconnect", PendingKind_User, nullptr, nullptr);
    prJsonWriterBeginObject(client->writer);
    writeKeyBool(client->writer, "terminateDebuggee", true);
    prJsonWriterEndObject(client->writer);
    endRequest(client);
}

void prDapClientSetBreakpoints(PrDapClient* client, PrStringView path, const uint32_t* PR_NULLABLE lines, uint32_t count) {
    DapSource* source = sourceOf(client, path);
    if (source == nullptr) {
        client->sources.push(DapSource{.path = prStringCopy(path)});
        source = &client->sources.last();
    }
    source->lines.clear();
    source->breakpoints.clear();
    for (uint32_t i = 0; i < count; ++i) {
        source->lines.push(lines[i]);
        source->breakpoints.push(PrDapBreakpoint{lines[i], false});
    }
    if (client->configured && isLive(client))
        sendBreakpoints(client, uint32_t(source - client->sources.data));
}

uint32_t prDapClientBreakpointCount(const PrDapClient* client, PrStringView path) {
    const DapSource* source = sourceOf(client, path);
    return source != nullptr ? source->breakpoints.count : 0;
}

const PrDapBreakpoint* PR_NULLABLE prDapClientBreakpoints(const PrDapClient* client, PrStringView path) {
    const DapSource* source = sourceOf(client, path);
    return source != nullptr ? source->breakpoints.data : nullptr;
}

PrDapRequest prDapClientRequest(PrDapClient* client, PrStringView command, PrStringView arguments,
        PrDapResponseFn PR_NULLABLE fn, void* PR_NULLABLE context) {
    if (!isLive(client))
        return kPrDapRequestNone;
    if (arguments.data != nullptr && !prJsonParse(client->arguments, arguments, nullptr))
        return kPrDapRequestNone;
    PrDapRequest request = beginRequest(client, command, PendingKind_User, fn, context);
    if (arguments.data != nullptr) {
        prJsonWriterValue(client->writer, client->arguments, 0);
    } else {
        prJsonWriterBeginObject(client->writer);
        prJsonWriterEndObject(client->writer);
    }
    endRequest(client);
    return request;
}

PrDapRequest prDapClientStep(PrDapClient* client, int64_t thread, PrDapStep step) {
    if (client->state != PrDapClientState_Stopped)
        return kPrDapRequestNone;
    const char* commands[] = {"continue", "next", "stepIn", "stepOut"};
    PrDapRequest request = requestOf(client, commands[step], "threadId", thread, nullptr, nullptr);
    client->state = PrDapClientState_Running;
    client->stoppedThread = 0;
    prStringReplace(&client->stopReason, "");
    return request;
}

PrDapRequest prDapClientThreads(PrDapClient* client, PrDapResponseFn fn, void* PR_NULLABLE context) {
    return requestOf(client, "threads", "", 0, fn, context);
}

PrDapRequest prDapClientStackTrace(PrDapClient* client, int64_t thread, PrDapResponseFn fn, void* PR_NULLABLE context) {
    return requestOf(client, "stackTrace", "threadId", thread, fn, context);
}

PrDapRequest prDapClientScopes(PrDapClient* client, int64_t frame, PrDapResponseFn fn, void* PR_NULLABLE context) {
    return requestOf(client, "scopes", "frameId", frame, fn, context);
}

PrDapRequest prDapClientVariables(PrDapClient* client, int64_t reference, PrDapResponseFn fn, void* PR_NULLABLE context) {
    return requestOf(client, "variables", "variablesReference", reference, fn, context);
}

uint32_t prDapFrames(const PrJson* json, PrJsonValueIndex body, PrDapFrame* PR_NULLABLE out, uint32_t capacity) {
    PrJsonValueIndex list = member(json, body, "stackFrames");
    uint32_t count = 0;
    for (PrJsonValueIndex item = prJsonFirst(json, list); item != kPrJsonMissing; item = prJsonAfter(json, list, item)) {
        if (count < capacity) {
            PrJsonValueIndex source = member(json, item, "source");
            out[count] = PrDapFrame{
                .id = integerMember(json, item, "id", 0),
                .name = stringMember(json, item, "name"),
                .path = stringMember(json, source, "path"),
                .line = lineFromAdapter(integerMember(json, item, "line", 1)),
                .column = uint32_t(integerMember(json, item, "column", 0)),
            };
        }
        ++count;
    }
    return count;
}

uint32_t prDapScopes(const PrJson* json, PrJsonValueIndex body, PrDapVariable* PR_NULLABLE out, uint32_t capacity) {
    PrJsonValueIndex list = member(json, body, "scopes");
    uint32_t count = 0;
    for (PrJsonValueIndex item = prJsonFirst(json, list); item != kPrJsonMissing; item = prJsonAfter(json, list, item)) {
        if (count < capacity) {
            out[count] = PrDapVariable{
                .name = stringMember(json, item, "name"),
                .value = "",
                .type = "",
                .reference = integerMember(json, item, "variablesReference", 0),
            };
        }
        ++count;
    }
    return count;
}

uint32_t prDapVariables(const PrJson* json, PrJsonValueIndex body, PrDapVariable* PR_NULLABLE out, uint32_t capacity) {
    PrJsonValueIndex list = member(json, body, "variables");
    uint32_t count = 0;
    for (PrJsonValueIndex item = prJsonFirst(json, list); item != kPrJsonMissing; item = prJsonAfter(json, list, item)) {
        if (count < capacity) {
            out[count] = PrDapVariable{
                .name = stringMember(json, item, "name"),
                .value = stringMember(json, item, "value"),
                .type = stringMember(json, item, "type"),
                .reference = integerMember(json, item, "variablesReference", 0),
            };
        }
        ++count;
    }
    return count;
}

uint32_t prDapThreads(const PrJson* json, PrJsonValueIndex body, PrDapThread* PR_NULLABLE out, uint32_t capacity) {
    PrJsonValueIndex list = member(json, body, "threads");
    uint32_t count = 0;
    for (PrJsonValueIndex item = prJsonFirst(json, list); item != kPrJsonMissing; item = prJsonAfter(json, list, item)) {
        if (count < capacity)
            out[count] = PrDapThread{integerMember(json, item, "id", 0), stringMember(json, item, "name")};
        ++count;
    }
    return count;
}
