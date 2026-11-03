#include "pr/dap.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

extern "C" int prTestDapCApi(void);

namespace pr_impl {

namespace {

// An adapter that a test plays: what the client wrote to it, as messages, and the events that reached the host.
struct Adapter {
    std::string written;
    std::vector<std::string> events;
    PrJson* json = prJsonCreate();
    uint32_t seq = 1000;

    ~Adapter() { prJsonDestroy(json); }

    std::vector<std::string> take() {
        std::vector<std::string> bodies;
        size_t at = 0;
        while (at < written.size()) {
            size_t headerEnd = written.find("\r\n\r\n", at);
            REQUIRE(headerEnd != std::string::npos);
            size_t length = std::stoul(written.substr(at + 16, headerEnd - at - 16));
            bodies.push_back(written.substr(headerEnd + 4, length));
            at = headerEnd + 4 + length;
        }
        written.clear();
        return bodies;
    }

    // Removes what the client wrote, and returns the commands of its requests in order.
    std::vector<std::string> commands() {
        std::vector<std::string> names;
        for (const std::string& body : take()) {
            REQUIRE(prJsonParse(json, body, nullptr));
            PrStringView command = prJsonString(json, prJsonMember(json, 0, "command"));
            names.emplace_back(command.data, command.size);
        }
        return names;
    }
};

void writeToAdapter(void* PR_NULLABLE context, PrByteView bytes) {
    static_cast<Adapter*>(context)->written.append(reinterpret_cast<const char*>(bytes.data), bytes.size);
}

void noteEvent(void* PR_NULLABLE context, const PrDapEvent* event) {
    static_cast<Adapter*>(context)->events.emplace_back(event->event.data, event->event.size);
}

void receive(PrDapClient* client, const std::string& body) {
    std::string framed = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    prDapClientReceive(client, PrByteView{reinterpret_cast<const uint8_t*>(framed.data()), framed.size()});
}

// Answers the request with sequence number `seq`, with `body`.
void answer(PrDapClient* client, uint32_t seq, const std::string& command, const std::string& body = "{}") {
    receive(client, "{\"seq\":1,\"type\":\"response\",\"request_seq\":" + std::to_string(seq) +
        ",\"success\":true,\"command\":\"" + command + "\",\"body\":" + body + "}");
}

void event(PrDapClient* client, const std::string& name, const std::string& body = "{}") {
    receive(client, "{\"seq\":1,\"type\":\"event\",\"event\":\"" + name + "\",\"body\":" + body + "}");
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
    std::vector<PrDapFrame> frames;
    std::vector<std::string> names;
};

void keepFrames(void* PR_NULLABLE context, const PrDapResponse* response) {
    Kept& kept = *static_cast<Kept*>(context);
    kept.called = true;
    kept.success = response->success;
    kept.frames.resize(prDapFrames(response->json, response->body, nullptr, 0));
    prDapFrames(response->json, response->body, kept.frames.data(), uint32_t(kept.frames.size()));
    for (const PrDapFrame& frame : kept.frames)
        kept.names.emplace_back(frame.name.data, frame.name.size);
}

void keepVariables(void* PR_NULLABLE context, const PrDapResponse* response) {
    Kept& kept = *static_cast<Kept*>(context);
    kept.called = true;
    std::vector<PrDapVariable> variables(prDapVariables(response->json, response->body, nullptr, 0));
    prDapVariables(response->json, response->body, variables.data(), uint32_t(variables.size()));
    for (const PrDapVariable& variable : variables)
        kept.names.push_back(std::string(variable.name.data, variable.name.size) + "=" +
            std::string(variable.value.data, variable.value.size));
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
    CHECK(adapter.commands() == std::vector<std::string>{"initialize"});
    // A launch asked for while the session initializes waits for it.
    uint32_t lines[] = {4, 9};
    prDapClientSetBreakpoints(client, "/src/main.c", lines, 2);
    CHECK(prDapClientLaunch(client, "{\"program\":\"/bin/a\"}"));
    CHECK(adapter.commands().empty());
    answer(client, 1, "initialize", "{\"supportsConfigurationDoneRequest\":true}");
    CHECK(prDapClientState(client) == PrDapClientState_Ready);
    std::vector<std::string> bodies = adapter.take();
    REQUIRE(bodies.size() == 1);
    CHECK(bodies[0].find("\"command\":\"launch\"") != std::string::npos);
    CHECK(bodies[0].find("\"program\":\"/bin/a\"") != std::string::npos);

    event(client, "initialized");
    bodies = adapter.take();
    REQUIRE(bodies.size() == 2);
    CHECK(bodies[0].find("\"command\":\"setBreakpoints\"") != std::string::npos);
    // The adapter counts lines from one.
    CHECK(bodies[0].find("{\"line\":5},{\"line\":10}") != std::string::npos);
    CHECK(bodies[1].find("\"command\":\"configurationDone\"") != std::string::npos);
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
    CHECK(adapter.events.back() == "stopped");

    // A breakpoint set while the session runs reaches the adapter at once.
    uint32_t more[] = {2};
    prDapClientSetBreakpoints(client, "/src/other.c", more, 1);
    CHECK(adapter.commands() == std::vector<std::string>{"setBreakpoints"});

    CHECK(prDapClientStep(client, 7, PrDapStep_Over) != kPrDapRequestNone);
    CHECK(adapter.commands() == std::vector<std::string>{"next"});
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
    REQUIRE(stack.frames.size() == 2);
    CHECK(stack.names == std::vector<std::string>{"main", "start"});
    CHECK(stack.frames[0].id == 11);
    CHECK(stack.frames[0].line == 2);
    CHECK(stack.frames[0].path == "/src/main.c");
    CHECK(stack.frames[1].path == "");

    Kept locals;
    request = prDapClientVariables(client, 5, keepVariables, &locals);
    adapter.take();
    answer(client, request, "variables", "{\"variables\":[{\"name\":\"x\",\"value\":\"1\",\"type\":\"int\","
        "\"variablesReference\":0},{\"name\":\"p\",\"value\":\"0x10\",\"variablesReference\":6}]}");
    CHECK(locals.names == std::vector<std::string>{"x=1", "p=0x10"});
    prDapClientDestroy(client);
}

TEST_CASE("dap: a refused launch fails the session, and the adapter's own requests are refused") {
    Adapter adapter;
    PrDapClient* client = create(adapter);
    adapter.take();
    answer(client, 1, "initialize");
    receive(client, "{\"seq\":5,\"type\":\"request\",\"command\":\"runInTerminal\",\"arguments\":{}}");
    std::vector<std::string> bodies = adapter.take();
    REQUIRE(bodies.size() == 1);
    CHECK(bodies[0].find("\"success\":false") != std::string::npos);
    CHECK(bodies[0].find("\"request_seq\":5") != std::string::npos);
    CHECK(prDapClientLaunch(client, "{\"program\":\"/missing\"}"));
    bodies = adapter.take();
    REQUIRE(bodies.size() == 1);
    REQUIRE(prJsonParse(adapter.json, bodies[0], nullptr));
    std::string seq = std::to_string(uint32_t(prJsonNumber(adapter.json, prJsonMember(adapter.json, 0, "seq"), 0)));
    receive(client, "{\"seq\":2,\"type\":\"response\",\"request_seq\":" + seq + ",\"success\":false,"
        "\"command\":\"launch\",\"message\":\"no such program\"}");
    CHECK(prDapClientState(client) == PrDapClientState_Failed);
    CHECK_FALSE(prDapClientLaunch(client, "{}"));
    prDapClientDestroy(client);
    PrDapClient* other = create(adapter);
    CHECK_FALSE(prDapClientLaunch(other, "not json"));
    prDapClientDestroy(other);
}
