// Language servers (lsp.h): the base protocol's framing, JSON-RPC's requests and answers, the lifecycle of a client,
// and documents that the edit stream keeps in step with the server.

#include "pr/lsp.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/string_ops.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

// The longest header of a message that the client reads. A server whose header runs longer is not speaking the
// protocol.
constexpr uint32_t kHeaderMaxBytes = 4096;

// The records that one take of a document's edit stream reads.
constexpr uint32_t kRecordBatch = 32;

// The error of JSON-RPC that answers a method that the client does not know, as the protocol's ErrorCodes number it.
constexpr int32_t kMethodNotFound = -32601;

// How a server asks for a document's changes, as the protocol's TextDocumentSyncKind numbers them.
enum {
    SyncKind_None,
    SyncKind_Full,
    SyncKind_Incremental,
};
typedef uint8_t SyncKind;

struct LspClient;

// A request that waits for its answer.
struct LspPending {
    PrLspRequest request;
    PrLspResponseFn fn;
    void* PR_NULLABLE context;
};

// A diagnostic as a document keeps it: anchors that follow the buffer, and its strings as ranges of the document's
// `strings`.
struct LspDiagnosticRecord {
    PrTextEditAnchor start;
    PrTextEditAnchor end;
    PrLspSeverity severity;
    uint32_t message;
    uint32_t messageSize;
    uint32_t source;
    uint32_t sourceSize;
    uint32_t code;
    uint32_t codeSize;
};

struct LspDocument {
    LspClient* client;
    PrTextEditBuffer* buffer;
    PrTextEditStream* stream;

    // The text that the server holds, for a server that counts in units other than bytes and takes changes in ranges. A
    // change's old end is a position in the text before it, which only this copy still has.
    PrTextEditBuffer* PR_NULLABLE replica;

    PrString uri;
    PrString languageId;
    int32_t version;
    bool opened;                    // the server was told of the document
    Array<LspDiagnosticRecord> diagnostics;
    Array<char> strings;
    uint64_t diagnosticChanges;
};

struct LspClient {
    PrLspWriteFn write;
    void* PR_NULLABLE writeContext;
    PrLspMessageFn PR_NULLABLE message;
    void* PR_NULLABLE messageContext;
    PrString rootUri;               // null if the client has no workspace
    PrString rootName;
    PrString clientName;

    PrLspClientState state;
    bool shutdownAsked;             // the host asked to shut down while the client initialized
    bool receiving;                 // a call of prLspClientReceive is handling messages
    PrLspRequest nextRequest;
    PrLspRequest initializeRequest;
    PrLspRequest shutdownRequest;
    Array<LspPending> pending;
    Array<LspDocument*> documents;

    PrString serverName;
    PrLspCapabilityFlags capabilities;
    PrLspEncoding encoding;
    SyncKind syncKind;
    bool openClose;                 // the server is told when documents open and close
    bool save;                      // the server is told when documents are saved
    bool saveText;                  // and is sent their text then

    Array<uint8_t> received;        // what arrived and was not handled, from `receivedStart`
    uint32_t receivedStart;
    PrJson* json;                   // the message being handled
    PrJson* params;                 // the params that a host gave as text
    PrJsonWriter* writer;
    Array<uint8_t> frame;
    Array<char> text;               // a document's text, copied whole
};

// Writing

// Sends the writer's text as a message, after its header.
void sendWritten(LspClient* client) {
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

// Begins a message, which the caller ends with an object's end and sendWritten.
void beginMessage(LspClient* client) {
    prJsonWriterClear(client->writer);
    prJsonWriterBeginObject(client->writer);
    prJsonWriterKey(client->writer, "jsonrpc");
    prJsonWriterString(client->writer, "2.0");
}

// Begins a request or a notification up to the value of its params, which the caller writes before it ends the message.
// A request is given the next id, which this returns.
PrLspRequest beginCall(LspClient* client, PrStringView method, bool request) {
    beginMessage(client);
    PrLspRequest id = kPrLspRequestNone;
    if (request) {
        id = ++client->nextRequest;
        prJsonWriterKey(client->writer, "id");
        prJsonWriterNumber(client->writer, double(id));
    }
    prJsonWriterKey(client->writer, "method");
    prJsonWriterString(client->writer, method);
    prJsonWriterKey(client->writer, "params");
    return id;
}

void endCall(LspClient* client) {
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

// Writes the encodings that the client takes, UTF-8 first, which needs no conversion.
void writeEncodings(PrJsonWriter* writer) {
    prJsonWriterBeginArray(writer);
    prJsonWriterString(writer, "utf-8");
    prJsonWriterString(writer, "utf-16");
    prJsonWriterEndArray(writer);
}

void writePosition(PrJsonWriter* writer, PrLspPosition position) {
    prJsonWriterBeginObject(writer);
    prJsonWriterKey(writer, "line");
    prJsonWriterNumber(writer, double(position.line));
    prJsonWriterKey(writer, "character");
    prJsonWriterNumber(writer, double(position.character));
    prJsonWriterEndObject(writer);
}

void writeDocumentId(PrJsonWriter* writer, const LspDocument* document) {
    prJsonWriterKey(writer, "textDocument");
    prJsonWriterBeginObject(writer);
    writeKeyString(writer, "uri", prStringView(&document->uri));
    prJsonWriterEndObject(writer);
}

// Positions

// Counts the units of `encoding` in the bytes of `buffer` from `start` to `end`, which lie in one line.
uint32_t unitsBetween(const PrTextEditBuffer* buffer, uint32_t start, uint32_t end, PrLspEncoding encoding) {
    if (encoding == PrLspEncoding_Utf8)
        return end - start;
    uint32_t units = 0;
    for (uint32_t at = start; at < end;) {
        PrStringView chunk = prTextEditBufferChunk(buffer, at);
        size_t size = chunk.size < end - at ? chunk.size : end - at;
        for (size_t i = 0; i < size; ++i) {
            uint8_t byte = uint8_t(chunk.data[i]);
            // A code point counts at its lead byte: one unit, or two in UTF-16 for one past the BMP.
            if ((byte & 0xc0) != 0x80)
                units += encoding == PrLspEncoding_Utf16 && byte >= 0xf0 ? 2 : 1;
        }
        at += uint32_t(size);
    }
    return units;
}

PrLspPosition positionIn(const PrTextEditBuffer* buffer, uint32_t offset, PrLspEncoding encoding) {
    PrTextEditPoint point = prTextEditBufferPointFromOffset(buffer, offset);
    uint32_t lineStart = prTextEditBufferLineStart(buffer, point.line);
    return PrLspPosition{point.line, unitsBetween(buffer, lineStart, offset, encoding)};
}

uint32_t offsetIn(const PrTextEditBuffer* buffer, PrLspPosition position, PrLspEncoding encoding) {
    uint32_t lines = prTextEditBufferLineCount(buffer);
    if (position.line >= lines)
        return prTextEditBufferSize(buffer);
    uint32_t start = prTextEditBufferLineStart(buffer, position.line);
    uint32_t end = prTextEditBufferLineEnd(buffer, position.line);
    if (encoding == PrLspEncoding_Utf8)
        return position.character < end - start ? start + position.character : end;
    uint32_t units = 0;
    uint32_t at = start;
    while (at < end && units < position.character) {
        PrStringView chunk = prTextEditBufferChunk(buffer, at);
        size_t size = chunk.size < end - at ? chunk.size : end - at;
        size_t i = 0;
        for (; i < size && units < position.character; ++i) {
            uint8_t byte = uint8_t(chunk.data[i]);
            if ((byte & 0xc0) != 0x80)
                units += encoding == PrLspEncoding_Utf16 && byte >= 0xf0 ? 2 : 1;
        }
        at += uint32_t(i);
    }
    // The bytes after the lead byte of the last code point counted belong to it.
    while (at < end) {
        PrStringView chunk = prTextEditBufferChunk(buffer, at);
        if ((uint8_t(chunk.data[0]) & 0xc0) != 0x80)
            break;
        ++at;
    }
    return at;
}

// Reading

PrJsonValueIndex member(const PrJson* json, PrJsonValueIndex object, PrStringView key) {
    return object != kPrJsonMissing ? prJsonMember(json, object, key) : kPrJsonMissing;
}

uint32_t numberMember(const PrJson* json, PrJsonValueIndex object, PrStringView key) {
    double value = prJsonNumber(json, member(json, object, key), 0);
    return value > 0 && value < 4294967295.0 ? uint32_t(value) : 0;
}

PrLspPosition readPosition(const PrJson* json, PrJsonValueIndex index) {
    return PrLspPosition{numberMember(json, index, "line"), numberMember(json, index, "character")};
}

PrLspRange readRange(const PrJson* json, PrJsonValueIndex index) {
    return PrLspRange{readPosition(json, member(json, index, "start")), readPosition(json, member(json, index, "end"))};
}

// Returns true if a capability is offered: present, and not false or null.
bool offers(const PrJson* json, PrJsonValueIndex capabilities, PrStringView key) {
    PrJsonValueIndex value = member(json, capabilities, key);
    PrJsonKind kind = prJsonKind(json, value);
    return value != kPrJsonMissing && kind != PrJsonKind_False && kind != PrJsonKind_Null;
}

// Documents

PrStringView documentStrings(const LspDocument* document, uint32_t start, uint32_t size) {
    return PrStringView{document->strings.data + start, size};
}

void clearDiagnostics(LspDocument* document) {
    for (uint32_t i = 0; i < document->diagnostics.count; ++i) {
        prTextEditAnchorRemove(document->buffer, document->diagnostics.data[i].start);
        prTextEditAnchorRemove(document->buffer, document->diagnostics.data[i].end);
    }
    document->diagnostics.clear();
    document->strings.clear();
}

// Copies the document's whole text into the client's scratch text.
PrStringView copyText(LspClient* client, const PrTextEditBuffer* buffer) {
    uint32_t size = prTextEditBufferSize(buffer);
    client->text.resize(size);
    prTextEditBufferCopy(buffer, 0, size, client->text.data);
    return PrStringView{client->text.data, size};
}

// Reads every record of the document's stream and drops it, for text that the server is sent whole.
void dropRecords(LspDocument* document) {
    PrTextEditRecord records[kRecordBatch];
    while (prTextEditStreamTake(document->stream, records, kRecordBatch) != 0) {
    }
}

// Tells the server of the document, with its whole text, which the server holds from then on.
void openDocument(LspDocument* document) {
    LspClient* client = document->client;
    dropRecords(document);
    document->opened = true;
    document->version = 1;
    if (client->syncKind == SyncKind_Incremental && client->encoding != PrLspEncoding_Utf8)
        document->replica = prTextEditBufferCreate(copyText(client, document->buffer));
    if (!client->openClose)
        return;
    beginCall(client, "textDocument/didOpen", false);
    prJsonWriterBeginObject(client->writer);
    prJsonWriterKey(client->writer, "textDocument");
    prJsonWriterBeginObject(client->writer);
    writeKeyString(client->writer, "uri", prStringView(&document->uri));
    writeKeyString(client->writer, "languageId", prStringView(&document->languageId));
    prJsonWriterKey(client->writer, "version");
    prJsonWriterNumber(client->writer, double(document->version));
    writeKeyString(client->writer, "text", copyText(client, document->buffer));
    prJsonWriterEndObject(client->writer);
    prJsonWriterEndObject(client->writer);
    endCall(client);
}

// Writes the start of a didChange, up to its first content change.
void beginChange(LspDocument* document) {
    LspClient* client = document->client;
    ++document->version;
    beginCall(client, "textDocument/didChange", false);
    prJsonWriterBeginObject(client->writer);
    prJsonWriterKey(client->writer, "textDocument");
    prJsonWriterBeginObject(client->writer);
    writeKeyString(client->writer, "uri", prStringView(&document->uri));
    prJsonWriterKey(client->writer, "version");
    prJsonWriterNumber(client->writer, double(document->version));
    prJsonWriterEndObject(client->writer);
    prJsonWriterKey(client->writer, "contentChanges");
    prJsonWriterBeginArray(client->writer);
}

void endChange(LspDocument* document) {
    prJsonWriterEndArray(document->client->writer);
    prJsonWriterEndObject(document->client->writer);
    endCall(document->client);
}

void syncDocument(LspDocument* document) {
    LspClient* client = document->client;
    if (!document->opened)
        return;
    if (client->syncKind != SyncKind_Incremental) {
        bool changed = false;
        PrTextEditRecord records[kRecordBatch];
        while (prTextEditStreamTake(document->stream, records, kRecordBatch) != 0)
            changed = true;
        if (!changed || client->syncKind == SyncKind_None)
            return;
        beginChange(document);
        prJsonWriterBeginObject(client->writer);
        writeKeyString(client->writer, "text", copyText(client, document->buffer));
        prJsonWriterEndObject(client->writer);
        endChange(document);
        return;
    }
    bool begun = false;
    PrTextEditRecord records[kRecordBatch];
    while (uint32_t count = prTextEditStreamTake(document->stream, records, kRecordBatch)) {
        if (!begun)
            beginChange(document);
        begun = true;
        for (uint32_t i = 0; i < count; ++i) {
            const PrTextEditRecord& record = records[i];
            PrLspPosition start{record.startPoint.line, record.startPoint.column};
            PrLspPosition end{record.oldEndPoint.line, record.oldEndPoint.column};
            if (document->replica != nullptr) {
                start = positionIn(document->replica, record.start, client->encoding);
                end = positionIn(document->replica, record.oldEnd, client->encoding);
                PrTextEditChange change{.start = record.start, .end = record.oldEnd, .text = record.text};
                prTextEditBufferApply(document->replica, &change, 1, PrTextEditOrigin_Remote);
            }
            prJsonWriterBeginObject(client->writer);
            prJsonWriterKey(client->writer, "range");
            prJsonWriterBeginObject(client->writer);
            prJsonWriterKey(client->writer, "start");
            writePosition(client->writer, start);
            prJsonWriterKey(client->writer, "end");
            writePosition(client->writer, end);
            prJsonWriterEndObject(client->writer);
            writeKeyString(client->writer, "text", record.text);
            prJsonWriterEndObject(client->writer);
        }
    }
    if (begun)
        endChange(document);
}

// Writes the params of a request at a position of the document, synchronized first.
PrLspRequest sendPositionRequest(LspDocument* document, PrStringView method, uint32_t offset, PrLspResponseFn fn,
        void* PR_NULLABLE context) {
    LspClient* client = document->client;
    if (client->state != PrLspClientState_Running || !document->opened)
        return kPrLspRequestNone;
    syncDocument(document);
    PrLspPosition position = positionIn(document->buffer, offset, client->encoding);
    PrLspRequest request = beginCall(client, method, true);
    prJsonWriterBeginObject(client->writer);
    writeDocumentId(client->writer, document);
    prJsonWriterKey(client->writer, "position");
    writePosition(client->writer, position);
    prJsonWriterEndObject(client->writer);
    endCall(client);
    client->pending.push(LspPending{request, fn, context});
    return request;
}

// URIs

// Returns true if a byte stands for itself in a path of a URI.
bool isPathByte(char c) {
    return prAsciiIsLetter(c) || prAsciiIsDigit(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/' ||
        c == ':' || c == '@' || c == '!' || c == '$' || c == '&' || c == '\'' || c == '(' || c == ')' || c == '*' ||
        c == '+' || c == ',' || c == ';' || c == '=';
}

bool sameDocument(PrStringView a, PrStringView b) {
    if (a == b)
        return true;
    PrString first = prLspPathFromUri(a);
    PrString second = prLspPathFromUri(b);
    bool same = !prStringIsNull(&first) && prStringView(&first) == prStringView(&second);
    prStringReset(&first);
    prStringReset(&second);
    return same;
}

LspDocument* PR_NULLABLE documentFromUri(const LspClient* client, PrStringView uri) {
    for (uint32_t i = 0; i < client->documents.count; ++i) {
        if (sameDocument(prStringView(&client->documents.data[i]->uri), uri))
            return client->documents.data[i];
    }
    return nullptr;
}

// Messages from the server

void fail(LspClient* client) {
    client->state = PrLspClientState_Failed;
    client->pending.clear();
}

void sendShutdown(LspClient* client) {
    client->shutdownRequest = beginCall(client, "shutdown", true);
    prJsonWriterNull(client->writer);
    endCall(client);
    client->state = PrLspClientState_ShuttingDown;
}

void initialized(LspClient* client, const PrJson* json, PrJsonValueIndex result) {
    PrJsonValueIndex capabilities = member(json, result, "capabilities");
    PrStringView encoding = prJsonString(json, member(json, capabilities, "positionEncoding"));
    // clangd answered with offsetEncoding before the protocol had positionEncoding.
    if (encoding.data == nullptr)
        encoding = prJsonString(json, member(json, result, "offsetEncoding"));
    client->encoding = PrLspEncoding_Utf16;
    if (encoding == "utf-8")
        client->encoding = PrLspEncoding_Utf8;
    else if (encoding == "utf-32")
        client->encoding = PrLspEncoding_Utf32;

    PrJsonValueIndex sync = member(json, capabilities, "textDocumentSync");
    if (prJsonKind(json, sync) == PrJsonKind_Number) {
        client->syncKind = SyncKind(numberMember(json, capabilities, "textDocumentSync"));
        client->openClose = true;
    } else if (prJsonKind(json, sync) == PrJsonKind_Object) {
        client->syncKind = SyncKind(numberMember(json, sync, "change"));
        client->openClose = prJsonKind(json, member(json, sync, "openClose")) == PrJsonKind_True;
        PrJsonValueIndex save = member(json, sync, "save");
        client->save = offers(json, sync, "save");
        client->saveText = prJsonKind(json, member(json, save, "includeText")) == PrJsonKind_True;
    }
    if (client->syncKind > SyncKind_Incremental)
        client->syncKind = SyncKind_Full;

    struct Provider {
        const char* key;
        PrLspCapabilityFlags flag;
    };
    static const Provider kProviders[] = {
        {"hoverProvider", PrLspCapabilityFlag_Hover},
        {"definitionProvider", PrLspCapabilityFlag_Definition},
        {"declarationProvider", PrLspCapabilityFlag_Declaration},
        {"referencesProvider", PrLspCapabilityFlag_References},
        {"completionProvider", PrLspCapabilityFlag_Completion},
        {"signatureHelpProvider", PrLspCapabilityFlag_SignatureHelp},
        {"documentFormattingProvider", PrLspCapabilityFlag_Formatting},
        {"renameProvider", PrLspCapabilityFlag_Rename},
        {"documentSymbolProvider", PrLspCapabilityFlag_DocumentSymbol},
    };
    for (const Provider& provider : kProviders) {
        if (offers(json, capabilities, provider.key))
            client->capabilities |= provider.flag;
    }
    prStringReplace(&client->serverName, prJsonString(json, member(json, member(json, result, "serverInfo"), "name")));

    client->state = PrLspClientState_Running;
    beginCall(client, "initialized", false);
    prJsonWriterBeginObject(client->writer);
    prJsonWriterEndObject(client->writer);
    endCall(client);
    for (uint32_t i = 0; i < client->documents.count; ++i)
        openDocument(client->documents.data[i]);
    if (client->shutdownAsked)
        sendShutdown(client);
}

void handleResponse(LspClient* client, const PrJson* json, PrJsonValueIndex id) {
    double number = prJsonNumber(json, id, -1);
    PrLspRequest request = number >= 1 && number <= double(UINT32_MAX) ? PrLspRequest(number) : kPrLspRequestNone;
    PrJsonValueIndex error = member(json, 0, "error");
    PrJsonValueIndex result = error == kPrJsonMissing ? member(json, 0, "result") : kPrJsonMissing;
    if (request == kPrLspRequestNone)
        return;
    if (request == client->initializeRequest) {
        if (result == kPrJsonMissing)
            fail(client);
        else
            initialized(client, json, result);
        return;
    }
    if (request == client->shutdownRequest) {
        beginMessage(client);
        prJsonWriterKey(client->writer, "method");
        prJsonWriterString(client->writer, "exit");
        prJsonWriterEndObject(client->writer);
        sendWritten(client);
        client->state = PrLspClientState_Stopped;
        return;
    }
    for (uint32_t i = 0; i < client->pending.count; ++i) {
        if (client->pending.data[i].request != request)
            continue;
        LspPending waiting = client->pending.data[i];
        client->pending.erase(i);
        double code = prJsonNumber(json, member(json, error, "code"), 0);
        PrLspResponse response{
            .request = request,
            .json = json,
            .result = result,
            .errorCode = error != kPrJsonMissing ? int32_t(code) : 0,
            .errorMessage = prJsonString(json, member(json, error, "message")),
        };
        waiting.fn(waiting.context, &response);
        return;
    }
}

// Answers a request of the server. The caller writes the result's value and ends the message.
void beginAnswer(LspClient* client, const PrJson* json, PrJsonValueIndex id) {
    beginMessage(client);
    prJsonWriterKey(client->writer, "id");
    prJsonWriterValue(client->writer, json, id);
    prJsonWriterKey(client->writer, "result");
}

void handleServerRequest(LspClient* client, const PrJson* json, PrJsonValueIndex id, PrStringView method) {
    PrJsonValueIndex params = member(json, 0, "params");
    if (method == "workspace/configuration") {
        // No setting is known to the client, so every item reads as null, which a server takes as its default.
        beginAnswer(client, json, id);
        prJsonWriterBeginArray(client->writer);
        uint32_t items = prJsonCount(json, member(json, params, "items"));
        for (uint32_t i = 0; i < items; ++i)
            prJsonWriterNull(client->writer);
        prJsonWriterEndArray(client->writer);
    } else if (method == "client/registerCapability" || method == "client/unregisterCapability" ||
            method == "window/workDoneProgress/create" || method == "window/showMessageRequest") {
        if (method == "window/showMessageRequest" && client->message != nullptr) {
            PrLspMessage message{.method = method, .json = json, .params = params};
            client->message(client->messageContext, &message);
        }
        beginAnswer(client, json, id);
        prJsonWriterNull(client->writer);
    } else if (method == "workspace/workspaceFolders") {
        beginAnswer(client, json, id);
        if (prStringIsNull(&client->rootUri)) {
            prJsonWriterNull(client->writer);
        } else {
            prJsonWriterBeginArray(client->writer);
            prJsonWriterBeginObject(client->writer);
            writeKeyString(client->writer, "uri", prStringView(&client->rootUri));
            writeKeyString(client->writer, "name", prStringView(&client->rootName));
            prJsonWriterEndObject(client->writer);
            prJsonWriterEndArray(client->writer);
        }
    } else if (method == "workspace/applyEdit") {
        // A host applies edits through its own commands, so a server's edit is refused.
        beginAnswer(client, json, id);
        prJsonWriterBeginObject(client->writer);
        writeKeyBool(client->writer, "applied", false);
        prJsonWriterEndObject(client->writer);
    } else {
        beginMessage(client);
        prJsonWriterKey(client->writer, "id");
        prJsonWriterValue(client->writer, json, id);
        prJsonWriterKey(client->writer, "error");
        prJsonWriterBeginObject(client->writer);
        prJsonWriterKey(client->writer, "code");
        prJsonWriterNumber(client->writer, kMethodNotFound);
        writeKeyString(client->writer, "message", "method not found");
        prJsonWriterEndObject(client->writer);
    }
    prJsonWriterEndObject(client->writer);
    sendWritten(client);
}

uint32_t keepString(LspDocument* document, PrStringView text) {
    uint32_t at = document->strings.count;
    if (text.size != 0)
        document->strings.append(text.data, uint32_t(text.size));
    return at;
}

void publishDiagnostics(LspClient* client, const PrJson* json, PrJsonValueIndex params) {
    LspDocument* document = documentFromUri(client, prJsonString(json, member(json, params, "uri")));
    if (document == nullptr)
        return;
    clearDiagnostics(document);
    PrJsonValueIndex list = member(json, params, "diagnostics");
    for (PrJsonValueIndex item = prJsonFirst(json, list); item != kPrJsonMissing; item = prJsonAfter(json, list, item)) {
        PrLspRange range = readRange(json, member(json, item, "range"));
        uint32_t start = offsetIn(document->buffer, range.start, client->encoding);
        uint32_t end = offsetIn(document->buffer, range.end, client->encoding);
        LspDiagnosticRecord record{
            .start = prTextEditAnchorCreate(document->buffer, start, PrTextEditBias_Right),
            .end = prTextEditAnchorCreate(document->buffer, end > start ? end : start, PrTextEditBias_Left),
        };
        uint32_t severity = numberMember(json, item, "severity");
        record.severity = severity <= PrLspSeverity_Hint ? PrLspSeverity(severity) : PrLspSeverity_None;
        PrStringView message = prJsonString(json, member(json, item, "message"));
        record.message = keepString(document, message);
        record.messageSize = uint32_t(message.size);
        PrStringView source = prJsonString(json, member(json, item, "source"));
        record.source = keepString(document, source);
        record.sourceSize = uint32_t(source.size);
        PrJsonValueIndex code = member(json, item, "code");
        PrStringView codeText = prJsonString(json, code);
        char digits[kPrU64TextMaxBytes + 1];
        if (prJsonKind(json, code) == PrJsonKind_Number) {
            double value = prJsonNumber(json, code, 0);
            size_t length = 0;
            if (value < 0)
                digits[length++] = '-';
            length += prU64Format(uint64_t(value < 0 ? -value : value), digits + length, 10);
            codeText = PrStringView{digits, length};
        }
        record.code = keepString(document, codeText);
        record.codeSize = uint32_t(codeText.size);
        document->diagnostics.push(record);
    }
    ++document->diagnosticChanges;
}

void handleMessage(LspClient* client, PrStringView body) {
    PrJson* json = client->json;
    if (!prJsonParse(json, body, nullptr) || prJsonKind(json, 0) != PrJsonKind_Object) {
        fail(client);
        return;
    }
    PrJsonValueIndex id = member(json, 0, "id");
    PrStringView method = prJsonString(json, member(json, 0, "method"));
    if (method.data == nullptr) {
        if (id != kPrJsonMissing)
            handleResponse(client, json, id);
        return;
    }
    if (id != kPrJsonMissing) {
        handleServerRequest(client, json, id, method);
        return;
    }
    PrJsonValueIndex params = member(json, 0, "params");
    if (method == "textDocument/publishDiagnostics") {
        publishDiagnostics(client, json, params);
    } else if (client->message != nullptr) {
        PrLspMessage message{.method = method, .json = json, .params = params};
        client->message(client->messageContext, &message);
    }
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

// Handles each whole message that arrived.
void handleReceived(LspClient* client) {
    while (client->state != PrLspClientState_Failed) {
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
        if (!contentLength(prStringSlice(waiting, 0, headerEnd), &length) || length > kPrTextEditBufferMaxBytes) {
            fail(client);
            return;
        }
        if (waiting.size - headerEnd - 4 < length)
            return;
        handleMessage(client, prStringSlice(waiting, headerEnd + 4, size_t(length)));
        client->receivedStart += uint32_t(headerEnd + 4 + length);
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrLspClient : LspClient {};
struct PrLspDocument : LspDocument {};

PrLspClient* prLspClientCreate(const PrLspClientDesc* desc) {
    PrLspClient* client = allocZeroed<PrLspClient>();
    client->write = desc->write;
    client->writeContext = desc->writeContext;
    client->message = desc->message;
    client->messageContext = desc->messageContext;
    client->json = prJsonCreate();
    client->params = prJsonCreate();
    client->writer = prJsonWriterCreate();
    client->clientName = prStringCopy(desc->clientName.data != nullptr ? desc->clientName : PrStringView("prax"));
    if (desc->rootPath.data != nullptr) {
        client->rootUri = prLspUriFromPath(desc->rootPath);
        PrStringView root = desc->rootPath;
        while (root.size > 1 && (root.data[root.size - 1] == '/' || root.data[root.size - 1] == '\\'))
            --root.size;
        size_t slash = root.size;
        while (slash > 0 && root.data[slash - 1] != '/' && root.data[slash - 1] != '\\')
            --slash;
        client->rootName = prStringCopy(prStringSlice(root, slash, root.size - slash));
    }

    PrJsonWriter* writer = client->writer;
    client->initializeRequest = beginCall(client, "initialize", true);
    prJsonWriterBeginObject(writer);
    prJsonWriterKey(writer, "processId");
    prJsonWriterNull(writer);
    prJsonWriterKey(writer, "clientInfo");
    prJsonWriterBeginObject(writer);
    writeKeyString(writer, "name", prStringView(&client->clientName));
    prJsonWriterEndObject(writer);
    prJsonWriterKey(writer, "rootUri");
    if (prStringIsNull(&client->rootUri))
        prJsonWriterNull(writer);
    else
        prJsonWriterString(writer, prStringView(&client->rootUri));
    prJsonWriterKey(writer, "workspaceFolders");
    if (prStringIsNull(&client->rootUri)) {
        prJsonWriterNull(writer);
    } else {
        prJsonWriterBeginArray(writer);
        prJsonWriterBeginObject(writer);
        writeKeyString(writer, "uri", prStringView(&client->rootUri));
        writeKeyString(writer, "name", prStringView(&client->rootName));
        prJsonWriterEndObject(writer);
        prJsonWriterEndArray(writer);
    }
    prJsonWriterKey(writer, "capabilities");
    prJsonWriterBeginObject(writer);
    // The encodings in the protocol's form, and in clangd's older one.
    prJsonWriterKey(writer, "general");
    prJsonWriterBeginObject(writer);
    prJsonWriterKey(writer, "positionEncodings");
    writeEncodings(writer);
    prJsonWriterEndObject(writer);
    prJsonWriterKey(writer, "offsetEncoding");
    writeEncodings(writer);
    prJsonWriterKey(writer, "textDocument");
    prJsonWriterBeginObject(writer);
    prJsonWriterKey(writer, "synchronization");
    prJsonWriterBeginObject(writer);
    writeKeyBool(writer, "didSave", true);
    prJsonWriterEndObject(writer);
    prJsonWriterKey(writer, "publishDiagnostics");
    prJsonWriterBeginObject(writer);
    writeKeyBool(writer, "versionSupport", true);
    prJsonWriterEndObject(writer);
    prJsonWriterKey(writer, "hover");
    prJsonWriterBeginObject(writer);
    prJsonWriterKey(writer, "contentFormat");
    prJsonWriterBeginArray(writer);
    prJsonWriterString(writer, "plaintext");
    prJsonWriterString(writer, "markdown");
    prJsonWriterEndArray(writer);
    prJsonWriterEndObject(writer);
    const char* const linked[] = {"definition", "declaration"};
    for (const char* key : linked) {
        prJsonWriterKey(writer, key);
        prJsonWriterBeginObject(writer);
        writeKeyBool(writer, "linkSupport", true);
        prJsonWriterEndObject(writer);
    }
    prJsonWriterKey(writer, "completion");
    prJsonWriterBeginObject(writer);
    prJsonWriterKey(writer, "completionItem");
    prJsonWriterBeginObject(writer);
    writeKeyBool(writer, "snippetSupport", false);
    prJsonWriterEndObject(writer);
    prJsonWriterEndObject(writer);
    prJsonWriterEndObject(writer);
    prJsonWriterKey(writer, "window");
    prJsonWriterBeginObject(writer);
    writeKeyBool(writer, "workDoneProgress", true);
    prJsonWriterEndObject(writer);
    prJsonWriterKey(writer, "workspace");
    prJsonWriterBeginObject(writer);
    writeKeyBool(writer, "configuration", true);
    writeKeyBool(writer, "workspaceFolders", true);
    prJsonWriterEndObject(writer);
    prJsonWriterEndObject(writer);
    prJsonWriterEndObject(writer);
    endCall(client);
    return client;
}

void prLspClientDestroy(PrLspClient* PR_NULLABLE client) {
    if (client == nullptr)
        return;
    PR_ASSERT(client->documents.count == 0);
    client->documents.reset();
    client->pending.reset();
    client->received.reset();
    client->frame.reset();
    client->text.reset();
    prJsonDestroy(client->json);
    prJsonDestroy(client->params);
    prJsonWriterDestroy(client->writer);
    prStringReset(&client->rootUri);
    prStringReset(&client->rootName);
    prStringReset(&client->clientName);
    prStringReset(&client->serverName);
    prMemoryFree(client);
}

void prLspClientReceive(PrLspClient* client, PrByteView bytes) {
    PR_ASSERT(!client->receiving);
    if (client->state == PrLspClientState_Failed || bytes.size == 0)
        return;
    if (client->state == PrLspClientState_Running) {
        for (uint32_t i = 0; i < client->documents.count; ++i)
            syncDocument(client->documents.data[i]);
    }
    if (client->receivedStart == client->received.count) {
        client->received.clear();
        client->receivedStart = 0;
    }
    PR_ASSERT(bytes.size <= UINT32_MAX - client->received.count);
    client->received.append(bytes.data, uint32_t(bytes.size));
    client->receiving = true;
    handleReceived(client);
    client->receiving = false;
    if (client->receivedStart != 0 && client->receivedStart * 2 >= client->received.count) {
        client->received.erase(0, client->receivedStart);
        client->receivedStart = 0;
    }
}

void prLspClientShutdown(PrLspClient* client) {
    if (client->state == PrLspClientState_Initializing)
        client->shutdownAsked = true;
    else if (client->state == PrLspClientState_Running)
        sendShutdown(client);
}

PrLspClientState prLspClientState(const PrLspClient* client) {
    return client->state;
}

PrStringView prLspClientServerName(const PrLspClient* client) {
    PrStringView name = prStringView(&client->serverName);
    return name.data != nullptr ? name : PrStringView("");
}

PrLspCapabilityFlags prLspClientCapabilities(const PrLspClient* client) {
    return client->capabilities;
}

PrLspEncoding prLspClientEncoding(const PrLspClient* client) {
    return client->encoding;
}

PrLspRequest prLspClientRequest(PrLspClient* client, PrStringView method, PrStringView params, PrLspResponseFn fn,
        void* PR_NULLABLE context) {
    if (client->state != PrLspClientState_Running)
        return kPrLspRequestNone;
    if (params.data != nullptr && !prJsonParse(client->params, params, nullptr))
        return kPrLspRequestNone;
    PrLspRequest request = beginCall(client, method, true);
    if (params.data != nullptr)
        prJsonWriterValue(client->writer, client->params, 0);
    else
        prJsonWriterNull(client->writer);
    endCall(client);
    client->pending.push(LspPending{request, fn, context});
    return request;
}

bool prLspClientNotify(PrLspClient* client, PrStringView method, PrStringView params) {
    if (client->state != PrLspClientState_Running)
        return false;
    if (params.data != nullptr && !prJsonParse(client->params, params, nullptr))
        return false;
    beginCall(client, method, false);
    if (params.data != nullptr)
        prJsonWriterValue(client->writer, client->params, 0);
    else
        prJsonWriterNull(client->writer);
    endCall(client);
    return true;
}

void prLspClientCancel(PrLspClient* client, PrLspRequest request) {
    for (uint32_t i = 0; i < client->pending.count; ++i) {
        if (client->pending.data[i].request != request)
            continue;
        client->pending.erase(i);
        beginCall(client, "$/cancelRequest", false);
        prJsonWriterBeginObject(client->writer);
        prJsonWriterKey(client->writer, "id");
        prJsonWriterNumber(client->writer, double(request));
        prJsonWriterEndObject(client->writer);
        endCall(client);
        return;
    }
}

PrLspDocument* prLspDocumentCreate(PrLspClient* client, PrTextEditBuffer* buffer, PrStringView path,
        PrStringView languageId) {
    PrLspDocument* document = allocZeroed<PrLspDocument>();
    document->client = client;
    document->buffer = buffer;
    document->stream = prTextEditStreamCreate(buffer);
    document->uri = prLspUriFromPath(path);
    document->languageId = prStringCopy(languageId);
    client->documents.push(document);
    if (client->state == PrLspClientState_Running)
        openDocument(document);
    return document;
}

void prLspDocumentDestroy(PrLspDocument* PR_NULLABLE document) {
    if (document == nullptr)
        return;
    LspClient* client = document->client;
    if (document->opened && client->openClose && client->state == PrLspClientState_Running) {
        beginCall(client, "textDocument/didClose", false);
        prJsonWriterBeginObject(client->writer);
        writeDocumentId(client->writer, document);
        prJsonWriterEndObject(client->writer);
        endCall(client);
    }
    client->documents.removeFirst(document);
    clearDiagnostics(document);
    document->diagnostics.reset();
    document->strings.reset();
    prTextEditStreamDestroy(document->stream);
    prTextEditBufferDestroy(document->replica);
    prStringReset(&document->uri);
    prStringReset(&document->languageId);
    prMemoryFree(document);
}

void prLspDocumentSync(PrLspDocument* document) {
    if (document->client->state == PrLspClientState_Running)
        syncDocument(document);
}

void prLspDocumentSave(PrLspDocument* document) {
    LspClient* client = document->client;
    if (client->state != PrLspClientState_Running || !document->opened || !client->save)
        return;
    syncDocument(document);
    beginCall(client, "textDocument/didSave", false);
    prJsonWriterBeginObject(client->writer);
    writeDocumentId(client->writer, document);
    if (client->saveText)
        writeKeyString(client->writer, "text", copyText(client, document->buffer));
    prJsonWriterEndObject(client->writer);
    endCall(client);
}

PrStringView prLspDocumentUri(const PrLspDocument* document) {
    return prStringView(&document->uri);
}

PrLspDocument* PR_NULLABLE prLspClientDocumentFromUri(const PrLspClient* client, PrStringView uri) {
    return static_cast<PrLspDocument*>(documentFromUri(client, uri));
}

PrLspPosition prLspDocumentPosition(const PrLspDocument* document, uint32_t offset) {
    return positionIn(document->buffer, offset, document->client->encoding);
}

uint32_t prLspDocumentOffset(const PrLspDocument* document, PrLspPosition position) {
    return offsetIn(document->buffer, position, document->client->encoding);
}

PrLspPosition prLspPositionFromOffset(const PrTextEditBuffer* buffer, uint32_t offset, PrLspEncoding encoding) {
    return positionIn(buffer, offset, encoding);
}

uint32_t prLspOffsetFromPosition(const PrTextEditBuffer* buffer, PrLspPosition position, PrLspEncoding encoding) {
    return offsetIn(buffer, position, encoding);
}

uint32_t prLspDocumentDiagnosticCount(const PrLspDocument* document) {
    return document->diagnostics.count;
}

PrLspDiagnostic prLspDocumentDiagnostic(const PrLspDocument* document, uint32_t index) {
    PR_ASSERT(index < document->diagnostics.count);
    const LspDiagnosticRecord& record = document->diagnostics.data[index];
    uint32_t start = prTextEditAnchorOffset(document->buffer, record.start);
    uint32_t end = prTextEditAnchorOffset(document->buffer, record.end);
    return PrLspDiagnostic{
        .start = start < end ? start : end,
        .end = start < end ? end : start,
        .severity = record.severity,
        .message = documentStrings(document, record.message, record.messageSize),
        .source = documentStrings(document, record.source, record.sourceSize),
        .code = documentStrings(document, record.code, record.codeSize),
    };
}

uint64_t prLspDocumentDiagnosticChanges(const PrLspDocument* document) {
    return document->diagnosticChanges;
}

PrLspRequest prLspDocumentHover(PrLspDocument* document, uint32_t offset, PrLspResponseFn fn, void* PR_NULLABLE context) {
    return sendPositionRequest(document, "textDocument/hover", offset, fn, context);
}

PrLspRequest prLspDocumentDefinition(PrLspDocument* document, uint32_t offset, PrLspResponseFn fn, void* PR_NULLABLE context) {
    return sendPositionRequest(document, "textDocument/definition", offset, fn, context);
}

PrLspRequest prLspDocumentDeclaration(PrLspDocument* document, uint32_t offset, PrLspResponseFn fn, void* PR_NULLABLE context) {
    return sendPositionRequest(document, "textDocument/declaration", offset, fn, context);
}

PrLspRequest prLspDocumentCompletion(PrLspDocument* document, uint32_t offset, PrLspResponseFn fn, void* PR_NULLABLE context) {
    return sendPositionRequest(document, "textDocument/completion", offset, fn, context);
}

PrString prLspHoverText(const PrJson* json, PrJsonValueIndex result) {
    PrString text = prStringEmpty();
    PrJsonValueIndex contents = member(json, result, "contents");
    // MarkupContent, a MarkedString or an array of MarkedString, each a string or an object with a value.
    PrJsonValueIndex first = contents;
    bool list = prJsonKind(json, contents) == PrJsonKind_Array;
    if (list)
        first = prJsonFirst(json, contents);
    for (PrJsonValueIndex part = first; part != kPrJsonMissing;
            part = list ? prJsonAfter(json, contents, part) : kPrJsonMissing) {
        PrStringView value = prJsonString(json, part);
        if (value.data == nullptr)
            value = prJsonString(json, member(json, part, "value"));
        if (value.size == 0)
            continue;
        if (!prStringIsEmpty(&text))
            prStringAppend(&text, "\n\n");
        prStringAppend(&text, value);
    }
    return text;
}

uint32_t prLspLocations(const PrJson* json, PrJsonValueIndex result, PrLspLocation* PR_NULLABLE out, uint32_t capacity) {
    PrJsonKind kind = prJsonKind(json, result);
    if (result == kPrJsonMissing || (kind != PrJsonKind_Array && kind != PrJsonKind_Object))
        return 0;
    bool list = kind == PrJsonKind_Array;
    uint32_t count = 0;
    for (PrJsonValueIndex item = list ? prJsonFirst(json, result) : result; item != kPrJsonMissing;
            item = list ? prJsonAfter(json, result, item) : kPrJsonMissing) {
        PrLspLocation location{};
        PrJsonValueIndex target = member(json, item, "targetUri");
        if (target != kPrJsonMissing) {
            location.uri = prJsonString(json, target);
            PrJsonValueIndex range = member(json, item, "targetSelectionRange");
            location.range = readRange(json, range != kPrJsonMissing ? range : member(json, item, "targetRange"));
        } else {
            location.uri = prJsonString(json, member(json, item, "uri"));
            location.range = readRange(json, member(json, item, "range"));
        }
        if (location.uri.data == nullptr)
            continue;
        if (count < capacity)
            out[count] = location;
        ++count;
    }
    return count;
}

uint32_t prLspCompletions(const PrJson* json, PrJsonValueIndex result, PrLspCompletion* PR_NULLABLE out,
        uint32_t capacity) {
    PrJsonValueIndex items = prJsonKind(json, result) == PrJsonKind_Array ? result : member(json, result, "items");
    uint32_t count = 0;
    for (PrJsonValueIndex item = prJsonFirst(json, items); item != kPrJsonMissing; item = prJsonAfter(json, items, item)) {
        PrStringView label = prJsonString(json, member(json, item, "label"));
        if (label.data == nullptr)
            continue;
        if (count < capacity) {
            PrLspCompletion completion{.label = label};
            completion.detail = prJsonString(json, member(json, item, "detail"));
            completion.kind = uint8_t(numberMember(json, item, "kind"));
            PrJsonValueIndex edit = member(json, item, "textEdit");
            PrStringView text = prJsonString(json, member(json, edit, "newText"));
            if (text.data != nullptr) {
                // A plain edit has a range, and an edit that inserts or replaces has both; it inserts here.
                PrJsonValueIndex range = member(json, edit, "range");
                completion.range = readRange(json, range != kPrJsonMissing ? range : member(json, edit, "insert"));
                completion.hasRange = true;
            } else {
                text = prJsonString(json, member(json, item, "insertText"));
            }
            completion.text = text.data != nullptr ? text : label;
            PrStringView filter = prJsonString(json, member(json, item, "filterText"));
            completion.filterText = filter.data != nullptr ? filter : label;
            PrStringView sort = prJsonString(json, member(json, item, "sortText"));
            completion.sortText = sort.data != nullptr ? sort : label;
            if (completion.detail.data == nullptr)
                completion.detail = "";
            out[count] = completion;
        }
        ++count;
    }
    return count;
}

PrString prLspUriFromPath(PrStringView path) {
    PrString uri = prStringCopy("file://");
    // A drive letter's path, as Windows spells one, is a path below the URI's root.
    bool drive = path.size >= 2 && prAsciiIsLetter(path.data[0]) && path.data[1] == ':';
    if (drive)
        prStringAppend(&uri, "/");
    const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < path.size; ++i) {
        char c = path.data[i] == '\\' ? '/' : path.data[i];
        if (isPathByte(c)) {
            prStringAppend(&uri, PrStringView{&c, 1});
        } else {
            char escaped[3] = {'%', hex[uint8_t(c) >> 4], hex[uint8_t(c) & 15]};
            prStringAppend(&uri, PrStringView{escaped, 3});
        }
    }
    return uri;
}

PrString prLspPathFromUri(PrStringView uri) {
    if (uri.size < 7 || !prAsciiEqualsFold(prStringSlice(uri, 0, 7), "file://"))
        return prStringNull();
    PrStringView rest = prStringSlice(uri, 7, uri.size - 7);
    size_t slash = prStringFindByte(rest, '/');
    PrStringView authority = prStringSlice(rest, 0, slash);
    PrStringView path = prStringSlice(rest, slash, rest.size - slash);
    PrString decoded = prStringEmpty();
    // A host other than the local one names a share, as a UNC path does.
    if (authority.size != 0 && !prAsciiEqualsFold(authority, "localhost")) {
        prStringAppend(&decoded, "//");
        prStringAppend(&decoded, authority);
    }
    for (size_t i = 0; i < path.size; ++i) {
        char c = path.data[i];
        if (c == '%' && i + 2 < path.size && prAsciiIsHexDigit(path.data[i + 1]) && prAsciiIsHexDigit(path.data[i + 2])) {
            c = char(prAsciiHexValue(path.data[i + 1]) << 4 | prAsciiHexValue(path.data[i + 2]));
            i += 2;
        }
        prStringAppend(&decoded, PrStringView{&c, 1});
    }
#if defined(_WIN32)
    PrStringView view = prStringView(&decoded);
    if (view.size >= 3 && view.data[0] == '/' && prAsciiIsLetter(view.data[1]) && view.data[2] == ':') {
        PrString drive = prStringCopy(prStringSlice(view, 1, view.size - 1));
        prStringReset(&decoded);
        decoded = drive;
    }
#endif
    return decoded;
}
