#include "pr/lsp.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

extern "C" int prTestLspCApi(void);

namespace pr_impl {

namespace {

// A server that a test plays: what the client wrote to it, as messages, and what the test sends back.
struct Server {
    std::string written;
    std::vector<std::string> notified;  // the methods of the messages that the client passed to its host
    PrJson* json = prJsonCreate();

    ~Server() {
        prJsonDestroy(json);
    }

    // Removes the messages that the client wrote, and returns their bodies.
    std::vector<std::string> take() {
        std::vector<std::string> bodies;
        size_t at = 0;
        while (at < written.size()) {
            size_t headerEnd = written.find("\r\n\r\n", at);
            REQUIRE(headerEnd != std::string::npos);
            std::string header = written.substr(at, headerEnd - at);
            REQUIRE(header.rfind("Content-Length: ", 0) == 0);
            size_t length = std::stoul(header.substr(16));
            bodies.push_back(written.substr(headerEnd + 4, length));
            at = headerEnd + 4 + length;
        }
        written.clear();
        return bodies;
    }

    // Removes the messages that the client wrote, and returns the one message among them, parsed.
    PrJson* takeOne() {
        std::vector<std::string> bodies = take();
        REQUIRE(bodies.size() == 1);
        REQUIRE(prJsonParse(json, bodies[0], nullptr));
        return json;
    }
};

void writeToServer(void* PR_NULLABLE context, PrByteView bytes) {
    static_cast<Server*>(context)->written.append(reinterpret_cast<const char*>(bytes.data), bytes.size);
}

void noteMessage(void* PR_NULLABLE context, const PrLspMessage* message) {
    static_cast<Server*>(context)->notified.emplace_back(message->method.data, message->method.size);
}

std::string frame(const std::string& body) {
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

void receive(PrLspClient* client, const std::string& body) {
    std::string framed = frame(body);
    prLspClientReceive(client, PrByteView{reinterpret_cast<const uint8_t*>(framed.data()), framed.size()});
}

PrStringView stringAt(const PrJson* json, std::initializer_list<const char*> path) {
    PrJsonValueIndex at = 0;
    for (const char* key : path)
        at = at != kPrJsonMissing ? prJsonMember(json, at, key) : kPrJsonMissing;
    return prJsonString(json, at);
}

double numberAt(const PrJson* json, std::initializer_list<const char*> path) {
    PrJsonValueIndex at = 0;
    for (const char* key : path)
        at = at != kPrJsonMissing ? prJsonMember(json, at, key) : kPrJsonMissing;
    return prJsonNumber(json, at, -1);
}

// A client that the server has initialized, with the capabilities that `capabilities` holds as JSON.
PrLspClient* running(Server& server, const std::string& capabilities) {
    PrLspClientDesc desc{
        .rootPath = "/work/project",
        .clientName = "tests",
        .write = writeToServer,
        .writeContext = &server,
        .message = noteMessage,
        .messageContext = &server,
    };
    PrLspClient* client = prLspClientCreate(&desc);
    PrJson* initialize = server.takeOne();
    REQUIRE(stringAt(initialize, {"method"}) == "initialize");
    receive(client, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"capabilities\":" + capabilities +
        ",\"serverInfo\":{\"name\":\"fake\"}}}");
    std::vector<std::string> sent = server.take();
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].find("\"method\":\"initialized\"") != std::string::npos);
    REQUIRE(prLspClientState(client) == PrLspClientState_Running);
    return client;
}

// Replaces the bytes from `start` to `end` of `buffer` with `text`.
void edit(PrTextEditBuffer* buffer, uint32_t start, uint32_t end, PrStringView text) {
    PrTextEditChange change{.start = start, .end = end, .text = text};
    REQUIRE(prTextEditBufferApply(buffer, &change, 1, PrTextEditOrigin_Local));
}

// A response's text, for a test's callback to keep.
struct Answer {
    bool called = false;
    std::string text;
    int32_t errorCode = 0;
};

void keepHover(void* PR_NULLABLE context, const PrLspResponse* response) {
    Answer& answer = *static_cast<Answer*>(context);
    answer.called = true;
    answer.errorCode = response->errorCode;
    PrRawString text = prLspHoverText(response->json, response->result);
    PrStringView view = prRawStringView(&text);
    answer.text.assign(view.data, view.size);
    prRawStringReset(&text);
}

constexpr const char* kIncremental = "{\"positionEncoding\":\"utf-8\",\"textDocumentSync\":{\"openClose\":true,"
    "\"change\":2,\"save\":{\"includeText\":false}},\"hoverProvider\":true,\"definitionProvider\":{}}";

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("lsp: the public headers compile as C") {
    CHECK(prTestLspCApi() == 0);
}

TEST_CASE("lsp: a client initializes, takes the server's capabilities and tells it that it is initialized") {
    Server server;
    PrLspClient* client = running(server, kIncremental);
    CHECK(prLspClientServerName(client) == "fake");
    CHECK(prLspClientEncoding(client) == PrLspEncoding_Utf8);
    PrLspCapabilityFlags flags = prLspClientCapabilities(client);
    CHECK((flags & PrLspCapabilityFlag_Hover) != 0);
    CHECK((flags & PrLspCapabilityFlag_Definition) != 0);
    CHECK((flags & PrLspCapabilityFlag_Completion) == 0);
    prLspClientDestroy(client);
}

TEST_CASE("lsp: the initialize request names the workspace and asks for UTF-8 first") {
    Server server;
    PrLspClientDesc desc{.rootPath = "/work/my project", .write = writeToServer, .writeContext = &server};
    PrLspClient* client = prLspClientCreate(&desc);
    PrJson* json = server.takeOne();
    CHECK(stringAt(json, {"params", "rootUri"}) == "file:///work/my%20project");
    PrJsonValueIndex encodings = prJsonMember(json, prJsonMember(json, prJsonMember(json, prJsonMember(json, 0,
        "params"), "capabilities"), "general"), "positionEncodings");
    CHECK(prJsonString(json, prJsonElement(json, encodings, 0)) == "utf-8");
    prLspClientDestroy(client);
}

TEST_CASE("lsp: a document opens with its text and sends its edits in ranges") {
    Server server;
    PrLspClient* client = running(server, kIncremental);
    PrTextEditBuffer* buffer = prTextEditBufferCreate("int a;\nint b;\n");
    PrLspDocument* document = prLspDocumentCreate(client, buffer, "/work/project/a.cpp", "cpp");
    PrJson* open = server.takeOne();
    CHECK(stringAt(open, {"method"}) == "textDocument/didOpen");
    CHECK(stringAt(open, {"params", "textDocument", "uri"}) == "file:///work/project/a.cpp");
    CHECK(stringAt(open, {"params", "textDocument", "languageId"}) == "cpp");
    CHECK(stringAt(open, {"params", "textDocument", "text"}) == "int a;\nint b;\n");

    edit(buffer, 11, 11, "c");
    edit(buffer, 0, 3, "long");
    prLspDocumentSync(document);
    PrJson* change = server.takeOne();
    CHECK(stringAt(change, {"method"}) == "textDocument/didChange");
    CHECK(numberAt(change, {"params", "textDocument", "version"}) == 2);
    PrJsonValueIndex changes = prJsonMember(change, prJsonMember(change, 0, "params"), "contentChanges");
    REQUIRE(prJsonCount(change, changes) == 2);
    PrJsonValueIndex first = prJsonElement(change, changes, 0);
    PrJsonValueIndex range = prJsonMember(change, first, "range");
    CHECK(prJsonNumber(change, prJsonMember(change, prJsonMember(change, range, "start"), "line"), -1) == 1);
    CHECK(prJsonNumber(change, prJsonMember(change, prJsonMember(change, range, "start"), "character"), -1) == 4);
    CHECK(prJsonString(change, prJsonMember(change, first, "text")) == "c");
    PrJsonValueIndex second = prJsonMember(change, prJsonElement(change, changes, 1), "range");
    CHECK(prJsonNumber(change, prJsonMember(change, prJsonMember(change, second, "end"), "character"), -1) == 3);

    // Nothing changed since, so nothing is sent.
    prLspDocumentSync(document);
    CHECK(server.take().empty());

    prLspDocumentSave(document);
    CHECK(stringAt(server.takeOne(), {"method"}) == "textDocument/didSave");
    prLspDocumentDestroy(document);
    CHECK(stringAt(server.takeOne(), {"method"}) == "textDocument/didClose");
    prTextEditBufferDestroy(buffer);
    prLspClientDestroy(client);
}

TEST_CASE("lsp: a document opened while the client initializes is opened with the text that it has then") {
    Server server;
    PrLspClientDesc desc{.write = writeToServer, .writeContext = &server};
    PrLspClient* client = prLspClientCreate(&desc);
    server.take();
    PrTextEditBuffer* buffer = prTextEditBufferCreate("one");
    PrLspDocument* document = prLspDocumentCreate(client, buffer, "/a.txt", "plaintext");
    edit(buffer, 3, 3, " two");
    CHECK(server.take().empty());
    receive(client, std::string("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"capabilities\":") + kIncremental + "}}");
    std::vector<std::string> sent = server.take();
    REQUIRE(sent.size() == 2);
    CHECK(sent[1].find("\"text\":\"one two\"") != std::string::npos);
    prLspDocumentSync(document);
    CHECK(server.take().empty());
    prLspDocumentDestroy(document);
    prTextEditBufferDestroy(buffer);
    prLspClientDestroy(client);
}

TEST_CASE("lsp: a server that counts UTF-16 is sent positions in UTF-16, old ends as the text was before") {
    Server server;
    PrLspClient* client = running(server, "{\"textDocumentSync\":2}");
    CHECK(prLspClientEncoding(client) == PrLspEncoding_Utf16);
    // é is one unit and two bytes, and 😀 is two units and four bytes.
    PrTextEditBuffer* buffer = prTextEditBufferCreate("é😀x\n");
    PrLspDocument* document = prLspDocumentCreate(client, buffer, "/a.txt", "plaintext");
    server.take();
    // Remove the emoji, then insert after the é: the second edit's position is in the text that the first left.
    edit(buffer, 2, 6, "");
    edit(buffer, 2, 2, "y");
    prLspDocumentSync(document);
    PrJson* change = server.takeOne();
    PrJsonValueIndex changes = prJsonMember(change, prJsonMember(change, 0, "params"), "contentChanges");
    REQUIRE(prJsonCount(change, changes) == 2);
    PrJsonValueIndex first = prJsonMember(change, prJsonElement(change, changes, 0), "range");
    CHECK(prJsonNumber(change, prJsonMember(change, prJsonMember(change, first, "start"), "character"), -1) == 1);
    CHECK(prJsonNumber(change, prJsonMember(change, prJsonMember(change, first, "end"), "character"), -1) == 3);
    PrJsonValueIndex second = prJsonMember(change, prJsonElement(change, changes, 1), "range");
    CHECK(prJsonNumber(change, prJsonMember(change, prJsonMember(change, second, "end"), "character"), -1) == 1);

    CHECK(prLspDocumentPosition(document, 3).character == 2);
    CHECK(prLspDocumentOffset(document, PrLspPosition{0, 2}) == 3);
    CHECK(prLspDocumentOffset(document, PrLspPosition{0, 99}) == 4);
    CHECK(prLspDocumentOffset(document, PrLspPosition{7, 0}) == prTextEditBufferSize(buffer));
    prLspDocumentDestroy(document);
    prTextEditBufferDestroy(buffer);
    prLspClientDestroy(client);
}

TEST_CASE("lsp: a server that takes whole text is sent the whole text") {
    Server server;
    PrLspClient* client = running(server, "{\"textDocumentSync\":1}");
    PrTextEditBuffer* buffer = prTextEditBufferCreate("ab");
    PrLspDocument* document = prLspDocumentCreate(client, buffer, "/a.txt", "plaintext");
    server.take();
    edit(buffer, 1, 1, "x");
    edit(buffer, 0, 0, "y");
    prLspDocumentSync(document);
    PrJson* change = server.takeOne();
    PrJsonValueIndex changes = prJsonMember(change, prJsonMember(change, 0, "params"), "contentChanges");
    REQUIRE(prJsonCount(change, changes) == 1);
    CHECK(prJsonString(change, prJsonMember(change, prJsonElement(change, changes, 0), "text")) == "yaxb");
    prLspDocumentDestroy(document);
    prTextEditBufferDestroy(buffer);
    prLspClientDestroy(client);
}

TEST_CASE("lsp: diagnostics are anchored ranges that follow the buffer's edits") {
    Server server;
    PrLspClient* client = running(server, kIncremental);
    PrTextEditBuffer* buffer = prTextEditBufferCreate("int a = b;\n");
    PrLspDocument* document = prLspDocumentCreate(client, buffer, "/work/project/a.cpp", "cpp");
    server.take();
    receive(client, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{"
        "\"uri\":\"file:///work/project/a.cpp\",\"diagnostics\":[{\"range\":{\"start\":{\"line\":0,\"character\":8},"
        "\"end\":{\"line\":0,\"character\":9}},\"severity\":1,\"source\":\"clang\",\"code\":\"undeclared\","
        "\"message\":\"use of undeclared identifier 'b'\"},{\"range\":{\"start\":{\"line\":0,\"character\":4},"
        "\"end\":{\"line\":0,\"character\":5}},\"severity\":2,\"code\":42,\"message\":\"unused\"}]}}");
    REQUIRE(prLspDocumentDiagnosticCount(document) == 2);
    CHECK(prLspDocumentDiagnosticChanges(document) == 1);
    PrLspDiagnostic first = prLspDocumentDiagnostic(document, 0);
    CHECK(first.start == 8);
    CHECK(first.end == 9);
    CHECK(first.severity == PrLspSeverity_Error);
    CHECK(first.message == "use of undeclared identifier 'b'");
    CHECK(first.source == "clang");
    CHECK(first.code == "undeclared");
    CHECK(prLspDocumentDiagnostic(document, 1).code == "42");

    edit(buffer, 0, 0, "const ");
    first = prLspDocumentDiagnostic(document, 0);
    CHECK(first.start == 14);
    CHECK(first.end == 15);
    // Text typed at either end of a diagnostic stays outside it.
    edit(buffer, 15, 15, "c");
    edit(buffer, 14, 14, "a");
    first = prLspDocumentDiagnostic(document, 0);
    CHECK(first.start == 15);
    CHECK(first.end == 16);

    receive(client, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{"
        "\"uri\":\"file:///work/project/a.cpp\",\"diagnostics\":[]}}");
    CHECK(prLspDocumentDiagnosticCount(document) == 0);
    CHECK(prLspDocumentDiagnosticChanges(document) == 2);
    prLspDocumentDestroy(document);
    prTextEditBufferDestroy(buffer);
    prLspClientDestroy(client);
}

TEST_CASE("lsp: a hover request is answered through its callback") {
    Server server;
    PrLspClient* client = running(server, kIncremental);
    PrTextEditBuffer* buffer = prTextEditBufferCreate("int a;\n");
    PrLspDocument* document = prLspDocumentCreate(client, buffer, "/a.cpp", "cpp");
    server.take();
    Answer answer;
    PrLspRequest request = prLspDocumentHover(document, 4, keepHover, &answer);
    CHECK(request != kPrLspRequestNone);
    PrJson* json = server.takeOne();
    CHECK(stringAt(json, {"method"}) == "textDocument/hover");
    CHECK(numberAt(json, {"params", "position", "character"}) == 4);
    std::string id = std::to_string(uint32_t(numberAt(json, {"id"})));
    receive(client, "{\"jsonrpc\":\"2.0\",\"id\":" + id +
        ",\"result\":{\"contents\":{\"kind\":\"markdown\",\"value\":\"`int a`\"}}}");
    CHECK(answer.called);
    CHECK(answer.text == "`int a`");

    Answer failed;
    prLspDocumentHover(document, 0, keepHover, &failed);
    id = std::to_string(uint32_t(numberAt(server.takeOne(), {"id"})));
    receive(client, "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":{\"code\":-32603,\"message\":\"no\"}}");
    CHECK(failed.called);
    CHECK(failed.errorCode == -32603);
    prLspDocumentDestroy(document);
    prTextEditBufferDestroy(buffer);
    prLspClientDestroy(client);
}

TEST_CASE("lsp: a cancelled request's callback is not called") {
    Server server;
    PrLspClient* client = running(server, kIncremental);
    Answer answer;
    PrLspRequest request = prLspClientRequest(client, "custom/thing", "{\"a\":1}", keepHover, &answer);
    PrJson* json = server.takeOne();
    CHECK(numberAt(json, {"params", "a"}) == 1);
    prLspClientCancel(client, request);
    PrJson* cancel = server.takeOne();
    CHECK(stringAt(cancel, {"method"}) == "$/cancelRequest");
    CHECK(numberAt(cancel, {"params", "id"}) == double(request));
    receive(client, "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(request) + ",\"result\":null}");
    CHECK(!answer.called);
    CHECK(prLspClientRequest(client, "custom/thing", "{not json", keepHover, &answer) == kPrLspRequestNone);
    prLspClientDestroy(client);
}

TEST_CASE("lsp: hover text, locations and completions read from each form a result may take") {
    PrJson* json = prJsonCreate();
    REQUIRE(prJsonParse(json, "{\"contents\":[\"first\",{\"language\":\"cpp\",\"value\":\"int a\"}]}", nullptr));
    PrRawString text = prLspHoverText(json, 0);
    CHECK(prRawStringView(&text) == "first\n\nint a");
    prRawStringReset(&text);

    REQUIRE(prJsonParse(json, "[{\"targetUri\":\"file:///b.h\",\"targetRange\":{\"start\":{\"line\":1,\"character\":0},"
        "\"end\":{\"line\":3,\"character\":1}},\"targetSelectionRange\":{\"start\":{\"line\":1,\"character\":4},"
        "\"end\":{\"line\":1,\"character\":7}}},{\"uri\":\"file:///c.h\",\"range\":{\"start\":{\"line\":9,"
        "\"character\":2},\"end\":{\"line\":9,\"character\":3}}}]", nullptr));
    PrLspLocation locations[4];
    REQUIRE(prLspLocations(json, 0, locations, 4) == 2);
    CHECK(locations[0].uri == "file:///b.h");
    CHECK(locations[0].range.start.character == 4);
    CHECK(locations[1].range.start.line == 9);

    REQUIRE(prJsonParse(json, "{\"isIncomplete\":false,\"items\":[{\"label\":\"push_back\",\"kind\":2,"
        "\"textEdit\":{\"range\":{\"start\":{\"line\":0,\"character\":2},\"end\":{\"line\":0,\"character\":4}},"
        "\"newText\":\"push_back\"}},{\"label\":\"size\",\"insertText\":\"size()\"}]}", nullptr));
    PrLspCompletion completions[4];
    REQUIRE(prLspCompletions(json, 0, completions, 4) == 2);
    CHECK(completions[0].hasRange);
    CHECK(completions[0].range.end.character == 4);
    CHECK(completions[0].kind == 2);
    CHECK(completions[1].text == "size()");
    CHECK(completions[1].filterText == "size");
    CHECK(!completions[1].hasRange);
    prJsonDestroy(json);
}

TEST_CASE("lsp: requests of the server are answered, and notifications reach the host") {
    Server server;
    PrLspClient* client = running(server, kIncremental);
    receive(client, "{\"jsonrpc\":\"2.0\",\"id\":\"c1\",\"method\":\"workspace/configuration\",\"params\":{"
        "\"items\":[{\"section\":\"a\"},{\"section\":\"b\"}]}}");
    PrJson* answer = server.takeOne();
    CHECK(stringAt(answer, {"id"}) == "c1");
    CHECK(prJsonCount(answer, prJsonMember(answer, 0, "result")) == 2);
    receive(client, "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"something/unknown\"}");
    CHECK(numberAt(server.takeOne(), {"error", "code"}) == -32601);
    receive(client, "{\"jsonrpc\":\"2.0\",\"method\":\"window/logMessage\",\"params\":{\"type\":3,\"message\":\"hi\"}}");
    REQUIRE(server.notified.size() == 1);
    CHECK(server.notified[0] == "window/logMessage");
    prLspClientDestroy(client);
}

TEST_CASE("lsp: messages split anywhere in the bytes are read whole") {
    Server server;
    PrLspClientDesc desc{
        .write = writeToServer,
        .writeContext = &server,
        .message = noteMessage,
        .messageContext = &server,
    };
    PrLspClient* client = prLspClientCreate(&desc);
    server.take();
    std::string bytes = frame(std::string("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"capabilities\":{}}}")) +
        frame("{\"jsonrpc\":\"2.0\",\"method\":\"window/logMessage\",\"params\":{}}");
    for (char c : bytes)
        prLspClientReceive(client, PrByteView{reinterpret_cast<const uint8_t*>(&c), 1});
    CHECK(prLspClientState(client) == PrLspClientState_Running);
    CHECK(server.notified.size() == 1);
    prLspClientDestroy(client);
}

TEST_CASE("lsp: what is not the protocol fails the client") {
    Server server;
    PrLspClientDesc desc{.write = writeToServer, .writeContext = &server};
    PrLspClient* client = prLspClientCreate(&desc);
    std::string bytes = "Content-Type: nothing\r\n\r\n{}";
    prLspClientReceive(client, PrByteView{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
    CHECK(prLspClientState(client) == PrLspClientState_Failed);
    prLspClientDestroy(client);

    client = prLspClientCreate(&desc);
    receive(client, "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32603,\"message\":\"no\"}}");
    CHECK(prLspClientState(client) == PrLspClientState_Failed);
    prLspClientDestroy(client);
}

TEST_CASE("lsp: shutting down asks the server to shut down, then to exit") {
    Server server;
    PrLspClient* client = running(server, kIncremental);
    prLspClientShutdown(client);
    CHECK(prLspClientState(client) == PrLspClientState_ShuttingDown);
    PrJson* shutdown = server.takeOne();
    CHECK(stringAt(shutdown, {"method"}) == "shutdown");
    std::string id = std::to_string(uint32_t(numberAt(shutdown, {"id"})));
    receive(client, "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":null}");
    CHECK(stringAt(server.takeOne(), {"method"}) == "exit");
    CHECK(prLspClientState(client) == PrLspClientState_Stopped);
    prLspClientDestroy(client);
}

TEST_CASE("lsp: a path and its file URI convert both ways") {
    PrRawString uri = prLspUriFromPath("/a b/c%d/é.cpp");
    CHECK(prRawStringView(&uri) == "file:///a%20b/c%25d/%C3%A9.cpp");
    PrRawString path = prLspPathFromUri(prRawStringView(&uri));
    CHECK(prRawStringView(&path) == "/a b/c%d/é.cpp");
    prRawStringReset(&uri);
    prRawStringReset(&path);
    path = prLspPathFromUri("file://localhost/x");
    CHECK(prRawStringView(&path) == "/x");
    prRawStringReset(&path);
    path = prLspPathFromUri("https://example.com/x");
    CHECK(prRawStringIsNull(&path));
}
