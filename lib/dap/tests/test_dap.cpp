#include "pr/array.h"
#include "pr/dap.h"
#include "pr/integer.h"
#include "pr/string.h"
#include "pr/string_ops.h"

#include "tools/harness/test.h"

extern "C" int prTestDapCApi(void);

namespace pr_impl {

using namespace pr;

namespace {

// An adapter that a test plays: what the client wrote to it, as messages, and the events that reached the host.
struct Adapter {
    String written;
    Array<String> events;
    PrJson* json = prJsonCreate();
    uint32_t seq = 1000;

    ~Adapter() { prJsonDestroy(json); }

    Array<String> take() {
        Array<String> bodies;
        PrStringView text = written;
        size_t at = 0;
        while (at < text.size) {
            PrStringView rest = prStringSlice(text, at, text.size - at);
            size_t headerEnd = prStringFind(rest, "\r\n\r\n");
            REQUIRE(headerEnd != rest.size);
            uint64_t length = 0;
            REQUIRE(prU64Parse(rest.data + 16, headerEnd - 16, &length, 10));
            bodies.push(prStringSlice(rest, headerEnd + 4, size_t(length)));
            at += headerEnd + 4 + size_t(length);
        }
        written.clear();
        return bodies;
    }

    // Removes what the client wrote, and returns the commands of its requests in order.
    Array<String> commands() {
        Array<String> names;
        for (const String& body : take()) {
            REQUIRE(prJsonParse(json, body, nullptr));
            PrStringView command = prJsonString(json, prJsonMember(json, 0, "command"));
            names.push(command);
        }
        return names;
    }
};

void writeToAdapter(void* PR_NULLABLE context, PrByteView bytes) {
    static_cast<Adapter*>(context)->written.append(prStringFromBytes(bytes));
}

void noteEvent(void* PR_NULLABLE context, const PrDapEvent* event) {
    static_cast<Adapter*>(context)->events.push(event->event);
}

void receive(PrDapClient* client, PrStringView body) {
    String framed = format("Content-Length: {}\r\n\r\n{}", body.size, body);
    prDapClientReceive(client, prStringBytes(framed));
}

// Answers the request with sequence number `seq`, with `body`.
void answer(PrDapClient* client, uint32_t seq, PrStringView command, PrStringView body = "{}") {
    receive(client, format("{{\"seq\":1,\"type\":\"response\",\"request_seq\":{},\"success\":true,\"command\":\"{}\","
        "\"body\":{}}}", seq, command, body));
}

void event(PrDapClient* client, PrStringView name, PrStringView body = "{}") {
    receive(client, format("{{\"seq\":1,\"type\":\"event\",\"event\":\"{}\",\"body\":{}}}", name, body));
}

PrDapClient* create(Adapter& adapter) {
    PrDapClientDesc desc{
        .adapterId = "test",
        .write = writeToAdapter,
        .writeContext = &adapter,
        .event = noteEvent,
        .eventContext = &adapter,
    };
    return prDapClientCreate(&desc);
}

// A body that a test's callback keeps.
struct Kept {
    bool called = false;
    bool success = false;
    Array<PrDapFrame> frames;
    Array<String> names;
};

void keepFrames(void* PR_NULLABLE context, const PrDapResponse* response) {
    Kept& kept = *static_cast<Kept*>(context);
    kept.called = true;
    kept.success = response->success;
    kept.frames.resize(prDapFrames(response->json, response->body, nullptr, 0));
    prDapFrames(response->json, response->body, kept.frames.data(), kept.frames.count());
    for (const PrDapFrame& frame : kept.frames)
        kept.names.push(frame.name);
}

void keepVariables(void* PR_NULLABLE context, const PrDapResponse* response) {
    Kept& kept = *static_cast<Kept*>(context);
    kept.called = true;
    Array<PrDapVariable> variables(prDapVariables(response->json, response->body, nullptr, 0));
    prDapVariables(response->json, response->body, variables.data(), variables.count());
    for (const PrDapVariable& variable : variables)
        kept.names.push(format("{}={}", variable.name, variable.value));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("dap: the public headers compile as C") {
    CHECK(prTestDapCApi() == 0);
}

TEST_CASE("dap: a session initializes, launches, configures its breakpoints, and stops at them") {
    Adapter adapter;
    PrDapClient* client = create(adapter);
    CHECK(adapter.commands() == Array<String>({"initialize"}));
    // A launch asked for while the session initializes waits for it.
    uint32_t lines[] = {4, 9};
    prDapClientSetBreakpoints(client, "/src/main.c", lines, 2);
    CHECK(prDapClientLaunch(client, "{\"program\":\"/bin/a\"}"));
    CHECK(adapter.commands().isEmpty());
    answer(client, 1, "initialize", "{\"supportsConfigurationDoneRequest\":true}");
    CHECK(prDapClientState(client) == PrDapClientState_Ready);
    Array<String> bodies = adapter.take();
    REQUIRE(bodies.count() == 1);
    CHECK(prStringContains(bodies[0], "\"command\":\"launch\""));
    CHECK(prStringContains(bodies[0], "\"program\":\"/bin/a\""));

    event(client, "initialized");
    bodies = adapter.take();
    REQUIRE(bodies.count() == 2);
    CHECK(prStringContains(bodies[0], "\"command\":\"setBreakpoints\""));
    // The adapter counts lines from one.
    CHECK(prStringContains(bodies[0], "{\"line\":5},{\"line\":10}"));
    CHECK(prStringContains(bodies[1], "\"command\":\"configurationDone\""));
    CHECK(prDapClientState(client) == PrDapClientState_Running);
    REQUIRE(prJsonParse(adapter.json, bodies[0], nullptr));
    uint32_t breakpointsSeq = uint32_t(prJsonNumber(adapter.json, prJsonMember(adapter.json, 0, "seq"), 0));
    answer(client, breakpointsSeq, "setBreakpoints",
        "{\"breakpoints\":[{\"verified\":true,\"line\":5},{\"verified\":false,\"line\":11}]}");
    REQUIRE(prDapClientBreakpointCount(client, "/src/main.c") == 2);
    const PrDapBreakpoint* breakpoints = prDapClientBreakpoints(client, "/src/main.c");
    CHECK(breakpoints[0].verified);
    CHECK(breakpoints[0].line == 4);
    CHECK_FALSE(breakpoints[1].verified);
    CHECK(breakpoints[1].line == 10);

    event(client, "stopped", "{\"reason\":\"breakpoint\",\"threadId\":7}");
    CHECK(prDapClientState(client) == PrDapClientState_Stopped);
    CHECK(prDapClientStoppedThread(client) == 7);
    CHECK(prDapClientStopReason(client) == "breakpoint");
    CHECK(adapter.events.last() == "stopped");

    // A breakpoint set while the session runs reaches the adapter at once.
    uint32_t more[] = {2};
    prDapClientSetBreakpoints(client, "/src/other.c", more, 1);
    CHECK(adapter.commands() == Array<String>({"setBreakpoints"}));

    CHECK(prDapClientStep(client, 7, PrDapStep_Over) != kPrDapRequestNone);
    CHECK(adapter.commands() == Array<String>({"next"}));
    CHECK(prDapClientState(client) == PrDapClientState_Running);
    CHECK(prDapClientStep(client, 7, PrDapStep_Over) == kPrDapRequestNone);

    event(client, "exited", "{\"exitCode\":3}");
    event(client, "terminated");
    CHECK(prDapClientExitCode(client) == 3);
    CHECK(prDapClientState(client) == PrDapClientState_Terminated);
    prDapClientDestroy(client);
}

TEST_CASE("dap: stacks and variables are read from their answers") {
    Adapter adapter;
    PrDapClient* client = create(adapter);
    answer(client, 1, "initialize");
    CHECK(prDapClientLaunch(client, "{}"));
    adapter.take();
    event(client, "stopped", "{\"reason\":\"step\",\"threadId\":1}");
    Kept stack;
    PrDapRequest request = prDapClientStackTrace(client, 1, keepFrames, &stack);
    REQUIRE(request != kPrDapRequestNone);
    adapter.take();
    answer(client, request, "stackTrace", "{\"stackFrames\":[{\"id\":11,\"name\":\"main\",\"line\":3,\"column\":5,"
        "\"source\":{\"path\":\"/src/main.c\"}},{\"id\":12,\"name\":\"start\",\"line\":1,\"column\":1}]}");
    REQUIRE(stack.called);
    REQUIRE(stack.frames.count() == 2);
    CHECK(stack.names == Array<String>({"main", "start"}));
    CHECK(stack.frames[0].id == 11);
    CHECK(stack.frames[0].line == 2);
    CHECK(stack.frames[0].path == "/src/main.c");
    CHECK(stack.frames[1].path == "");

    Kept locals;
    request = prDapClientVariables(client, 5, keepVariables, &locals);
    adapter.take();
    answer(client, request, "variables", "{\"variables\":[{\"name\":\"x\",\"value\":\"1\",\"type\":\"int\","
        "\"variablesReference\":0},{\"name\":\"p\",\"value\":\"0x10\",\"variablesReference\":6}]}");
    CHECK(locals.names == Array<String>({"x=1", "p=0x10"}));
    prDapClientDestroy(client);
}

TEST_CASE("dap: a refused launch fails the session, and the adapter's own requests are refused") {
    Adapter adapter;
    PrDapClient* client = create(adapter);
    adapter.take();
    answer(client, 1, "initialize");
    receive(client, "{\"seq\":5,\"type\":\"request\",\"command\":\"runInTerminal\",\"arguments\":{}}");
    Array<String> bodies = adapter.take();
    REQUIRE(bodies.count() == 1);
    CHECK(prStringContains(bodies[0], "\"success\":false"));
    CHECK(prStringContains(bodies[0], "\"request_seq\":5"));
    CHECK(prDapClientLaunch(client, "{\"program\":\"/missing\"}"));
    bodies = adapter.take();
    REQUIRE(bodies.count() == 1);
    REQUIRE(prJsonParse(adapter.json, bodies[0], nullptr));
    uint32_t seq = uint32_t(prJsonNumber(adapter.json, prJsonMember(adapter.json, 0, "seq"), 0));
    receive(client, format("{{\"seq\":2,\"type\":\"response\",\"request_seq\":{},\"success\":false,"
        "\"command\":\"launch\",\"message\":\"no such program\"}}", seq));
    CHECK(prDapClientState(client) == PrDapClientState_Failed);
    CHECK_FALSE(prDapClientLaunch(client, "{}"));
    prDapClientDestroy(client);
    PrDapClient* other = create(adapter);
    CHECK_FALSE(prDapClientLaunch(other, "not json"));
    prDapClientDestroy(other);
}
