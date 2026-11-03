// Language servers: a client of the Language Server Protocol, over a transport that its host provides.
//
// lsp - https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/
//
// The client writes what it sends through a function of its host, and its host gives it what the server sends, so the
// client does no I/O of its own. A host usually runs the server as a process (pr/process.h), whose pipes threads of
// their own serve. The client parses what it receives on the thread that gives it. One thread uses a client and its
// documents at a time.
//
// A document is a buffer that the server knows of. Its changes reach the server from the buffer's edit stream, and the
// diagnostics that the server publishes for it are anchored ranges of the buffer, which follow its edits until the
// server publishes again. Positions are bytes of the buffer everywhere in the API. The client requests positions in
// UTF-8, and converts for a server that counts only UTF-16 code units, as the protocol's default does. How a host
// presents what the server says is its own decision.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/json.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"
#include "pr/text_edit.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Clients

// A connection to one language server.
typedef struct PrLspClient PrLspClient;

// Called with bytes for the server, in the order that they are sent. The bytes are valid only during the call.
typedef void (*PrLspWriteFn)(void* PR_NULLABLE context, PrByteView bytes);

// A notification from the server that the client does not handle itself, such as window/logMessage. The JSON is valid
// only during the call.
typedef struct PrLspMessage {
    PrStringView method;
    const PrJson* json;
    PrJsonValueIndex params;    // kPrJsonMissing if the message has none
} PrLspMessage;

// Called with each message that the client does not handle itself.
typedef void (*PrLspMessageFn)(void* PR_NULLABLE context, const PrLspMessage* message);

// What a client connects as.
typedef struct PrLspClientDesc {
    PrStringView rootPath;      // the workspace's folder, or the null view for none
    PrStringView clientName;    // the name that the server is told, or the null view for none

    PrLspWriteFn write;
    void* PR_NULLABLE writeContext;

    // Called with notifications that the client does not handle, or null to drop them.
    PrLspMessageFn PR_NULLABLE message;
    void* PR_NULLABLE messageContext;
} PrLspClientDesc;

// Where a client stands. A client starts by initializing, runs once the server answers, and stops once it has asked the
// server to exit. It fails if the server sends what is not the protocol, or refuses to initialize.
enum {
    PrLspClientState_Initializing,
    PrLspClientState_Running,
    PrLspClientState_ShuttingDown,
    PrLspClientState_Stopped,
    PrLspClientState_Failed,
};
typedef uint8_t PrLspClientState;

// How the server counts the characters of a position.
enum {
    PrLspEncoding_Utf16,
    PrLspEncoding_Utf8,
    PrLspEncoding_Utf32,
};
typedef uint8_t PrLspEncoding;

// The requests that the server says it answers.
enum {
    PrLspCapabilityFlag_Hover = 1u << 0,
    PrLspCapabilityFlag_Definition = 1u << 1,
    PrLspCapabilityFlag_Declaration = 1u << 2,
    PrLspCapabilityFlag_References = 1u << 3,
    PrLspCapabilityFlag_Completion = 1u << 4,
    PrLspCapabilityFlag_SignatureHelp = 1u << 5,
    PrLspCapabilityFlag_Formatting = 1u << 6,
    PrLspCapabilityFlag_Rename = 1u << 7,
    PrLspCapabilityFlag_DocumentSymbol = 1u << 8,
};
typedef uint32_t PrLspCapabilityFlags;

// Creates a client and writes its initialize request, or frees one. Destroying a client drops the callbacks of the
// requests that it has not had answers to, and its documents must be destroyed first.
PrLspClient* prLspClientCreate(const PrLspClientDesc* desc);
void prLspClientDestroy(PrLspClient* PR_NULLABLE client);

// Reads bytes that the server sent, which may end anywhere in a message, and handles each message that they complete.
// Every document is synchronized first, so that positions in what arrives are read against the text that the server was
// sent. A client that fails reads nothing more.
void prLspClientReceive(PrLspClient* client, PrByteView bytes);

// Asks the server to shut down, and to exit once it has. A client that is still initializing shuts down once the server
// answers.
void prLspClientShutdown(PrLspClient* client);

// The client's state.
PrLspClientState prLspClientState(const PrLspClient* client);

// The name that the server gave, empty until it answers or if it gave none.
PrStringView prLspClientServerName(const PrLspClient* client);

// The requests that the server answers, and how it counts characters, once it runs.
PrLspCapabilityFlags prLspClientCapabilities(const PrLspClient* client);
PrLspEncoding prLspClientEncoding(const PrLspClient* client);

// ---------------------------------------------------------------------------------------------------------------------
// Requests

// A request that the client sent, by the number that names it in the protocol. kPrLspRequestNone is no request.
typedef uint32_t PrLspRequest;
enum { kPrLspRequestNone = 0 };

// The answer to a request. A request that succeeded has `result`, which may be JSON's null, and an error code of zero.
// One that failed has the server's error code and message, and no result. The JSON is valid only during the call.
typedef struct PrLspResponse {
    PrLspRequest request;
    const PrJson* json;
    PrJsonValueIndex result;    // kPrJsonMissing if the request failed
    int32_t errorCode;
    PrStringView errorMessage;
} PrLspResponse;

// Called with the answer to a request.
typedef void (*PrLspResponseFn)(void* PR_NULLABLE context, const PrLspResponse* response);

// Sends a request whose params are the JSON text `params`, or none for the null view, and returns it. Returns
// kPrLspRequestNone, and sends nothing, if the client is not running or `params` is not JSON. A request's positions are
// the server's, and a host that sends them synchronizes their document first (prLspDocumentSync).
PrLspRequest prLspClientRequest(PrLspClient* client, PrStringView method, PrStringView params, PrLspResponseFn fn,
    void* PR_NULLABLE context);

// Sends a notification, as prLspClientRequest sends a request. Returns false if nothing was sent.
bool prLspClientNotify(PrLspClient* client, PrStringView method, PrStringView params);

// Asks the server to cancel `request`, whose callback is not called. A request that was answered is left as it is.
void prLspClientCancel(PrLspClient* client, PrLspRequest request);

// ---------------------------------------------------------------------------------------------------------------------
// Documents

// A buffer that the server knows of, by a file's URI.
typedef struct PrLspDocument PrLspDocument;

// A place in a document as the server counts it: a line, and a count of characters in the client's encoding from the
// line's start.
typedef struct PrLspPosition {
    uint32_t line;
    uint32_t character;
} PrLspPosition;

// The text between two places of a document.
typedef struct PrLspRange {
    PrLspPosition start;
    PrLspPosition end;
} PrLspRange;

// Creates a document of `buffer`, which must outlive it, for the file at `path`, in the language that the protocol
// names `languageId`, such as "cpp". The server is told of it once the client runs. Destroying a document tells the
// server that it closed.
PrLspDocument* prLspDocumentCreate(PrLspClient* client, PrTextEditBuffer* buffer, PrStringView path,
    PrStringView languageId);
void prLspDocumentDestroy(PrLspDocument* PR_NULLABLE document);

// Sends the server the buffer's changes since the last synchronization, as the server asks for them: in ranges, as the
// whole text, or not at all.
void prLspDocumentSync(PrLspDocument* document);

// Tells the server that the document was saved to its file.
void prLspDocumentSave(PrLspDocument* document);

// The document's file URI.
PrStringView prLspDocumentUri(const PrLspDocument* document);

// Returns the client's document of `uri`, or null if it has none.
PrLspDocument* PR_NULLABLE prLspClientDocumentFromUri(const PrLspClient* client, PrStringView uri);

// Converts between a byte of the buffer and a position in the client's encoding. A position past the end of its line
// stands at that end, and one past the last line at the end of the text.
PrLspPosition prLspDocumentPosition(const PrLspDocument* document, uint32_t offset);
uint32_t prLspDocumentOffset(const PrLspDocument* document, PrLspPosition position);

// Convert as prLspDocumentPosition and prLspDocumentOffset do, for any buffer and encoding, as a host does for a file
// that a server names before it is a document.
PrLspPosition prLspPositionFromOffset(const PrTextEditBuffer* buffer, uint32_t offset, PrLspEncoding encoding);
uint32_t prLspOffsetFromPosition(const PrTextEditBuffer* buffer, PrLspPosition position, PrLspEncoding encoding);

// How much a diagnostic matters. None is a diagnostic whose server gave no severity.
enum {
    PrLspSeverity_None,
    PrLspSeverity_Error,
    PrLspSeverity_Warning,
    PrLspSeverity_Information,
    PrLspSeverity_Hint,
};
typedef uint8_t PrLspSeverity;

// A problem that the server found in the document, at bytes of the buffer that follow its edits. Its strings are valid
// until the server publishes the document's diagnostics again.
typedef struct PrLspDiagnostic {
    uint32_t start;
    uint32_t end;
    PrLspSeverity severity;
    PrStringView message;
    PrStringView source;        // what found the problem, such as a compiler, or empty
    PrStringView code;          // the problem's code, as text, or empty
} PrLspDiagnostic;

// The document's diagnostics, in the order that the server gave them.
uint32_t prLspDocumentDiagnosticCount(const PrLspDocument* document);
PrLspDiagnostic prLspDocumentDiagnostic(const PrLspDocument* document, uint32_t index);

// The number of times that the server published the document's diagnostics, which a host compares to know when to show
// them again.
uint64_t prLspDocumentDiagnosticChanges(const PrLspDocument* document);

// Synchronize the document, and send a request for the hover, the definitions, the declarations or the completions at
// `offset`. Each returns kPrLspRequestNone, and sends nothing, if the client is not running.
PrLspRequest prLspDocumentHover(PrLspDocument* document, uint32_t offset, PrLspResponseFn fn, void* PR_NULLABLE context);
PrLspRequest prLspDocumentDefinition(PrLspDocument* document, uint32_t offset, PrLspResponseFn fn, void* PR_NULLABLE context);
PrLspRequest prLspDocumentDeclaration(PrLspDocument* document, uint32_t offset, PrLspResponseFn fn, void* PR_NULLABLE context);
PrLspRequest prLspDocumentCompletion(PrLspDocument* document, uint32_t offset, PrLspResponseFn fn, void* PR_NULLABLE context);

// ---------------------------------------------------------------------------------------------------------------------
// Results

// Returns the text of a hover result, its parts separated by a blank line, or the empty string if it has none. The
// caller releases it with prRawStringReset.
PrRawString prLspHoverText(const PrJson* json, PrJsonValueIndex result);

// A range of a file. Its URI is valid as long as the JSON that it was read from.
typedef struct PrLspLocation {
    PrStringView uri;
    PrLspRange range;
} PrLspLocation;

// Writes up to `capacity` locations of a definition or declaration result to `out`, and returns their number. A link
// gives the range of its target's name.
uint32_t prLspLocations(const PrJson* json, PrJsonValueIndex result, PrLspLocation* PR_NULLABLE out, uint32_t capacity);

// A completion. Its strings are valid as long as the JSON that it was read from.
typedef struct PrLspCompletion {
    PrStringView label;         // what a list shows
    PrStringView detail;        // more about it, such as a type, or empty
    PrStringView text;          // what it inserts
    PrStringView filterText;    // what a typed prefix is matched against
    PrStringView sortText;      // what the list is sorted by
    uint8_t kind;               // the protocol's CompletionItemKind, or zero
    bool hasRange;              // the completion replaces `range` rather than the word at the position
    PrLspRange range;
} PrLspCompletion;

// Writes up to `capacity` completions of a completion result to `out`, and returns their number.
uint32_t prLspCompletions(const PrJson* json, PrJsonValueIndex result, PrLspCompletion* PR_NULLABLE out, uint32_t capacity);

// ---------------------------------------------------------------------------------------------------------------------
// URIs

// Returns the file URI of `path`, an absolute path. The caller releases it with prRawStringReset.
PrRawString prLspUriFromPath(PrStringView path);

// Returns the path of a file URI, or the null string if `uri` is not one. The caller releases it with prRawStringReset.
PrRawString prLspPathFromUri(PrStringView uri);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
