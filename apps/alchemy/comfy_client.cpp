// A client of a ComfyUI server, over the HTTP exchanges and WebSockets of one network.

#include "apps/alchemy/comfy_client.h"

#include "pr/array.h"
#include "pr/ascii.h"
#include "pr/function.h"
#include "pr/json.h"
#include "pr/lock.h"
#include "pr/map.h"
#include "pr/random.h"
#include "pr/set.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/thread.h"

namespace alchemy {

using namespace pr;

// One REST exchange in flight, and what to do with its response. `done` receives the response's status, or zero if the
// exchange failed, and its body.
struct ComfyRequest {
    ComfyClient* client;
    PrHttpExchange* PR_NULLABLE exchange;
    String method;
    String url;
    String contentType;
    String body;
    String response;
    Function<void(uint16_t status, PrStringView body)> done;
};

// A request made while the socket connects: the tag that its reply carries, and what starts it once the socket opens.
struct WaitingRequest {
    uint64_t tag;
    Function<void()> start;
};

struct ComfyClient {
    PrNet* net;
    PrHttp* http;
    PrWebSocketNetwork* websockets;
    Function<void()> wake;

    // The events for the application, which any thread may take. The events before `taken` have been taken.
    PrMutex mutex;
    Array<ComfyEvent> events;
    uint32_t taken = 0;

    // The state of the connection, which only the loop reads and writes.
    String origin;              // the server's HTTP origin, such as "http://127.0.0.1:8188"
    String clientId;            // the id that the server sends this client's runs' events under
    PrWebSocket* PR_NULLABLE socket = nullptr;
    bool open = false;

    // The requests made while the socket connects.
    Array<WaitingRequest> waiting;

    // The REST exchanges in flight.
    Set<ComfyRequest*> requests;

    // The image fetches in flight by prompt, and the prompts whose end waits for them, so that a prompt's images are
    // delivered before its end.
    Map<String, uint32_t> fetching;
    Set<String> ending;
};

namespace {

// Runs `fn` on the client's loop, after the tasks posted before it.
void post(ComfyClient* client, Function<void()> fn) {
    Function<void()>* task = new Function<void()>(move(fn));
    prNetPost(client->net, [](void* PR_NULLABLE context) {
        Function<void()>* run = static_cast<Function<void()>*>(context);
        (*run)();
        delete run;
    }, task);
}

void deliver(ComfyClient* client, ComfyEvent event) {
    {
        Lock lock(&client->mutex);
        client->events.push(move(event));
    }
    if (client->wake)
        client->wake();
}

void refuse(ComfyClient* client, uint64_t tag, PrStringView message) {
    ComfyEvent event;
    event.kind = ComfyEventKind_Refused;
    event.tag = tag;
    event.message = message;
    deliver(client, move(event));
}

String randomHex(uint32_t bytes) {
    Array<uint8_t> random(bytes);
    prRandomBytes(random.data(), random.count());
    static const char kHex[] = "0123456789abcdef";
    String out;
    for (uint8_t byte : random) {
        out.append(kHex[byte >> 4]);
        out.append(kHex[byte & 15]);
    }
    return out;
}

// Returns `text` percent-encoded for a URL's query, with every byte but the unreserved characters encoded.
String percentEncode(PrStringView text) {
    static const char kHex[] = "0123456789ABCDEF";
    String out;
    for (char c : text) {
        bool unreserved = prAsciiIsLetter(c) || prAsciiIsDigit(c) || c == '-' || c == '.' || c == '_' || c == '~';
        if (unreserved) {
            out.append(c);
        } else {
            out.append('%');
            out.append(kHex[uint8_t(c) >> 4]);
            out.append(kHex[uint8_t(c) & 15]);
        }
    }
    return out;
}

uint32_t clampCount(double value) {
    if (!(value > 0.0))
        return 0;
    return value >= 4294967295.0 ? 0xffffffffu : uint32_t(value);
}

// A JSON document that frees itself.
struct Document {
    PrJson* d = prJsonCreate();
    ~Document() { prJsonDestroy(d); }
    bool parse(PrStringView text) { return prJsonParse(d, text, nullptr); }
    String string(PrJsonValueIndex object, const char* key) const {
        return prJsonString(d, prJsonMember(d, object, key));
    }
    double number(PrJsonValueIndex object, const char* key, double fallback) const {
        PrJsonValueIndex value = prJsonMember(d, object, key);
        return prJsonKind(d, value) == PrJsonKind_Number ? prJsonNumber(d, value, fallback) : fallback;
    }
    PrJsonValueIndex member(PrJsonValueIndex object, const char* key) const {
        return prJsonMember(d, object, key);
    }
};

void appendStrings(const Document& doc, PrJsonValueIndex array, Array<String>* out) {
    for (PrJsonValueIndex i = prJsonFirst(doc.d, array); i != kPrJsonMissing; i = prJsonAfter(doc.d, array, i)) {
        if (prJsonKind(doc.d, i) == PrJsonKind_String)
            out->push(prJsonString(doc.d, i));
    }
}

// Appends the options of a node's combo input, which the server reports as `[[options...], {...}]` or as `["COMBO",
// {"options": [...]}]`. A node or an input that the server does not report appends nothing.
void collectCombo(const Document& doc, const char* nodeClass, const char* input, Array<String>* out) {
    PrJsonValueIndex required = doc.member(doc.member(doc.member(0, nodeClass), "input"), "required");
    PrJsonValueIndex entry = doc.member(required, input);
    PrJsonValueIndex options = prJsonElement(doc.d, entry, 0);
    if (prJsonKind(doc.d, options) != PrJsonKind_Array)
        options = doc.member(prJsonElement(doc.d, entry, 1), "options");
    appendStrings(doc, options, out);
}

void requestEvent(void* PR_NULLABLE context, PrHttpExchange* exchange, PrHttpEvent event) {
    ComfyRequest* request = static_cast<ComfyRequest*>(context);
    char chunk[16 * 1024];
    for (size_t got = prHttpExchangeRead(exchange, reinterpret_cast<uint8_t*>(chunk), sizeof(chunk)); got != 0;
            got = prHttpExchangeRead(exchange, reinterpret_cast<uint8_t*>(chunk), sizeof(chunk)))
        request->response.append(PrStringView{chunk, got});
    if (event != PrHttpEvent_End && event != PrHttpEvent_Fail)
        return;
    uint16_t status = event == PrHttpEvent_End ? prHttpExchangeStatus(exchange) : 0;
    prHttpExchangeDestroy(exchange);
    request->client->requests.erase(request);
    request->done(status, request->response);
    delete request;
}

// Starts a REST exchange on the loop. `done` runs on the loop with the response.
void exchange(ComfyClient* client, PrStringView method, PrStringView path, PrStringView contentType, String body,
        Function<void(uint16_t, PrStringView)> done) {
    ComfyRequest* request = new ComfyRequest{
        .client = client,
        .exchange = nullptr,
        .method = method,
        .url = format("{}{}", client->origin, path),
        .contentType = contentType,
        .body = move(body),
        .done = move(done),
    };
    PrHttpHeader header{"Content-Type", request->contentType};
    bool typed = !request->contentType.isEmpty();
    PrHttpRequestDesc desc{
        .method = request->method,
        .url = request->url,
        .headers = typed ? &header : nullptr,
        .headerCount = typed ? 1u : 0u,
        .body = prStringBytes(request->body),
        .event = requestEvent,
        .context = request,
    };
    if (prHttpExchangeCreate(client->http, &desc, &request->exchange) != PrStatus_Ok) {
        request->done(0, "");
        delete request;
        return;
    }
    client->requests.add(request);
}

// Runs `start` on the loop once the connection is open, or refuses the request `tag` if there is no connection.
void whenOpen(ComfyClient* client, uint64_t tag, Function<void()> start) {
    post(client, [client, tag, start = move(start)]() mutable {
        if (client->open)
            start();
        else if (client->socket != nullptr)
            client->waiting.push(WaitingRequest{.tag = tag, .start = move(start)});
        else
            refuse(client, tag, "Not connected to a server.");
    });
}

// Returns the function that delivers the reply to the request `tag`: Replied with what `fill` reads from a 200
// response, or Refused with `failure`.
Function<void(uint16_t, PrStringView)> replyTo(ComfyClient* client, uint64_t tag, PrStringView failure,
        Function<bool(PrStringView, ComfyEvent*)> fill) {
    return [client, tag, failure = String(failure), fill = move(fill)](uint16_t status, PrStringView body) {
        ComfyEvent event;
        event.kind = ComfyEventKind_Replied;
        event.tag = tag;
        if (status == 200 && fill(body, &event))
            deliver(client, move(event));
        else if (status != 0)
            refuse(client, tag, format("{} (HTTP {})", failure, status));
        else
            refuse(client, tag, failure);
    };
}

// Delivers the end of `promptId`, or holds it until the prompt's image fetches finish.
void endPrompt(ComfyClient* client, PrStringView promptId) {
    if (client->fetching.contains(promptId)) {
        client->ending.add(promptId);
        return;
    }
    ComfyEvent event;
    event.kind = ComfyEventKind_Completed;
    event.promptId = promptId;
    deliver(client, move(event));
}

void fetchImage(ComfyClient* client, PrStringView promptId, PrStringView filename, PrStringView subfolder, PrStringView type) {
    ++client->fetching[promptId];
    String path = format("/view?filename={}", percentEncode(filename));
    if (!subfolder.isEmpty())
        formatTo(path, "&subfolder={}", percentEncode(subfolder));
    formatTo(path, "&type={}", percentEncode(type));
    exchange(client, "GET", path, "", String(),
        [client, promptId = String(promptId), filename = String(filename)](uint16_t status, PrStringView body) {
            ComfyEvent event;
            event.kind = status == 200 ? ComfyEventKind_ImageReady : ComfyEventKind_ImageFailed;
            event.promptId = promptId;
            event.filename = filename;
            if (status == 200)
                event.bytes.append(prStringBytes(body));
            deliver(client, move(event));
            uint32_t* fetching = client->fetching.get(promptId);
            if (fetching == nullptr || --*fetching != 0)
                return;
            client->fetching.erase(promptId);
            if (client->ending.erase(promptId))
                endPrompt(client, promptId);
        });
}

// Reads a message of the server's event stream (the "type" and "data" of a run's progress) into events.
void handleMessage(ComfyClient* client, PrStringView text) {
    Document doc;
    if (!doc.parse(text))
        return;
    String type = doc.string(0, "type");
    PrJsonValueIndex data = doc.member(0, "data");
    ComfyEvent event;
    event.promptId = doc.string(data, "prompt_id");
    if (type == "status") {
        double remaining = doc.number(doc.member(doc.member(data, "status"), "exec_info"), "queue_remaining", -1);
        if (remaining < 0)
            return;
        event.kind = ComfyEventKind_QueueStatus;
        event.count = clampCount(remaining);
    } else if (type == "progress") {
        event.kind = ComfyEventKind_Progress;
        event.value = clampCount(doc.number(data, "value", 0));
        event.max = clampCount(doc.number(data, "max", 0));
        event.node = doc.string(data, "node");
    } else if (type == "executing") {
        // The server reports the end of a prompt as executing no node. Newer servers also send execution_success, which
        // the client leaves aside so that a prompt ends once.
        PrJsonValueIndex node = doc.member(data, "node");
        if (prJsonKind(doc.d, node) == PrJsonKind_Null) {
            if (!event.promptId.isEmpty())
                endPrompt(client, event.promptId);
            return;
        }
        event.kind = ComfyEventKind_Executing;
        event.node = prJsonString(doc.d, node);
    } else if (type == "executed") {
        PrJsonValueIndex images = doc.member(doc.member(data, "output"), "images");
        for (PrJsonValueIndex i = prJsonFirst(doc.d, images); i != kPrJsonMissing; i = prJsonAfter(doc.d, images, i)) {
            String filename = doc.string(i, "filename");
            String kind = doc.string(i, "type");
            if (kind.isEmpty())
                kind = "output";
            if (!filename.isEmpty())
                fetchImage(client, event.promptId, filename, doc.string(i, "subfolder"), kind);
        }
        return;
    } else if (type == "execution_error") {
        event.kind = ComfyEventKind_Failed;
        event.message = doc.string(data, "exception_message");
        if (event.message.isEmpty())
            event.message = "The server could not run the prompt.";
    } else {
        return;
    }
    deliver(client, move(event));
}

void closeSocket(ComfyClient* client, PrStringView reason) {
    if (client->socket == nullptr)
        return;
    prWebSocketDestroy(client->socket);
    client->socket = nullptr;
    client->open = false;
    Array<WaitingRequest> waiting = move(client->waiting);
    ComfyEvent event;
    event.kind = ComfyEventKind_Disconnected;
    event.message = reason;
    deliver(client, move(event));
    for (const WaitingRequest& request : waiting)
        refuse(client, request.tag, "The connection to the server closed before the request was sent.");
}

void socketEvent(void* PR_NULLABLE context, PrWebSocket* socket, PrWebSocketEvent event) {
    ComfyClient* client = static_cast<ComfyClient*>(context);
    switch (event) {
        case PrWebSocketEvent_Open: {
            client->open = true;
            ComfyEvent connected;
            connected.kind = ComfyEventKind_Connected;
            deliver(client, move(connected));
            Array<WaitingRequest> waiting = move(client->waiting);
            for (WaitingRequest& request : waiting)
                request.start();
            break;
        }
        case PrWebSocketEvent_Message: {
            PrWebSocketMessage message = prWebSocketMessage(socket);
            if (message.kind == PrWebSocketMessageKind_Text) {
                handleMessage(client, prStringFromBytes(message.data));
            } else if (message.data.size > 8) {
                // A preview frame: a 4-byte event type and a 4-byte image format, then the image.
                ComfyEvent preview;
                preview.kind = ComfyEventKind_Preview;
                preview.bytes.append(message.data.data + 8, uint32_t(message.data.size - 8));
                deliver(client, move(preview));
            }
            break;
        }
        case PrWebSocketEvent_Close: {
            PrWebSocketCloseInfo info = prWebSocketCloseInfo(socket);
            closeSocket(client, info.wasClean ? "The server closed the connection." : "The connection to the server failed.");
            break;
        }
    }
}

// Returns `text` without the spaces, line breaks and quotes at either end.
PrStringView unquoted(PrStringView text) {
    auto trimmed = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"'; };
    size_t start = 0;
    size_t end = text.size;
    while (start < end && trimmed(text.data[start]))
        ++start;
    while (end > start && trimmed(text.data[end - 1]))
        --end;
    return prStringSlice(text, start, end - start);
}

// A member of a request's JSON body: its key and its value.
struct BodyMember {
    const char* key;
    String value;
};

} // namespace

ComfyClient* comfyClientCreate(const ComfyClientDesc& desc) {
    ComfyClient* client = new ComfyClient;
    client->net = desc.net;
    client->http = desc.http;
    client->websockets = desc.websockets;
    client->wake = desc.wake;
    client->clientId = randomHex(16);
    prMutexInit(&client->mutex);
    return client;
}

void comfyClientDestroy(ComfyClient* PR_NULLABLE client) {
    if (client == nullptr)
        return;
    // The loop lets go of the client's exchanges and socket, which its callbacks name, before the client is freed.
    PrMutex mutex;
    PrCondition released;
    prMutexInit(&mutex);
    prConditionInit(&released);
    bool done = false;
    post(client, [&] {
        for (ComfyRequest* request : client->requests) {
            prHttpExchangeDestroy(request->exchange);
            delete request;
        }
        client->requests.clear();
        prWebSocketDestroy(client->socket);
        client->socket = nullptr;
        Lock lock(&mutex);
        done = true;
        prConditionSignal(&released);
    });
    {
        Lock lock(&mutex);
        while (!done)
            prConditionWait(&released, &mutex);
    }
    prConditionReset(&released);
    prMutexReset(&mutex);
    prMutexReset(&client->mutex);
    delete client;
}

void comfyConnect(ComfyClient* client, PrStringView origin) {
    post(client, [client, origin = String(origin)] {
        closeSocket(client, "Connecting to another server.");
        PrStringView trimmed = origin;
        while (prStringEndsWith(trimmed, "/"))
            trimmed = prStringSlice(trimmed, 0, trimmed.size - 1);
        client->origin = trimmed;
        PrStringView rest = client->origin;
        PrStringView scheme = "";
        if (prStringStartsWith(rest, "http")) {
            scheme = "ws";
            rest = prStringFrom(rest, 4);
        }
        String url = format("{}{}/ws?clientId={}", scheme, rest, client->clientId);
        PrWebSocketDesc desc{.url = url, .event = socketEvent, .context = client};
        if (prWebSocketCreate(client->websockets, &desc, &client->socket) != PrStatus_Ok) {
            ComfyEvent event;
            event.kind = ComfyEventKind_Disconnected;
            event.message = "The server's address is not one that alchemy can connect to.";
            deliver(client, move(event));
        }
    });
}

void comfyDisconnect(ComfyClient* client) {
    post(client, [client] { closeSocket(client, "Disconnected."); });
}

void comfySubmit(ComfyClient* client, uint64_t tag, String graph) {
    whenOpen(client, tag, [client, tag, graph = move(graph)] {
        // A server streams previews only for a prompt that asks for them, whatever its own default.
        String body = format("{{\"prompt\":{},\"client_id\":\"{}\",\"extra_data\":{{\"preview_method\":\"auto\"}}}}",
            graph, client->clientId);
        exchange(client, "POST", "/prompt", "application/json", move(body),
            replyTo(client, tag, "The server did not accept the prompt.", [](PrStringView response, ComfyEvent* event) {
                Document doc;
                if (doc.parse(response))
                    event->promptId = doc.string(0, "prompt_id");
                return !event->promptId.isEmpty();
            }));
    });
}

void comfyInterrupt(ComfyClient* client) {
    post(client, [client] {
        if (client->open)
            exchange(client, "POST", "/interrupt", "application/json", "{}", [](uint16_t, PrStringView) {});
    });
}

void comfyUpload(ComfyClient* client, uint64_t tag, PrStringView filename, Array<uint8_t> bytes) {
    whenOpen(client, tag, [client, tag, filename = String(filename), bytes = move(bytes)] {
        PrStringView payload(reinterpret_cast<const char*>(bytes.data()), bytes.count());
        // The boundary must not occur in the payload, or a part would end early.
        String boundary;
        do {
            boundary = format("----AlchemyComfyUi{}", randomHex(8));
        } while (prStringContains(payload, boundary));
        String safeName;
        for (char c : filename)
            safeName.append(c == '"' || c == '\\' || uint8_t(c) < 0x20 || c == 0x7f ? '_' : c);
        String body = format("--{}\r\nContent-Disposition: form-data; name=\"image\"; filename=\"{}\"\r\n"
            "Content-Type: application/octet-stream\r\n\r\n", boundary, safeName);
        body.append(payload);
        formatTo(body, "\r\n--{}\r\nContent-Disposition: form-data; name=\"overwrite\"\r\n\r\ntrue\r\n--{}--\r\n", boundary,
            boundary);
        String contentType = format("multipart/form-data; boundary={}", boundary);
        exchange(client, "POST", "/upload/image", contentType, move(body),
            replyTo(client, tag, "The server did not accept the image.", [](PrStringView response, ComfyEvent* event) {
                Document doc;
                if (doc.parse(response))
                    event->name = doc.string(0, "name");
                return !event->name.isEmpty();
            }));
    });
}

void comfyFetchCatalog(ComfyClient* client, uint64_t tag) {
    whenOpen(client, tag, [client, tag] {
        exchange(client, "GET", "/object_info", "", String(), [client, tag](uint16_t status, PrStringView body) {
            Document doc;
            if (status != 200 || !doc.parse(body)) {
                refuse(client, tag, "The server did not report its models.");
                return;
            }
            ComfyCatalog catalog;
            collectCombo(doc, "CheckpointLoaderSimple", "ckpt_name", &catalog.checkpoints);
            collectCombo(doc, "LoraLoader", "lora_name", &catalog.loras);
            collectCombo(doc, "KSampler", "sampler_name", &catalog.samplers);
            collectCombo(doc, "KSampler", "scheduler", &catalog.schedulers);
            collectCombo(doc, "UpscaleModelLoader", "model_name", &catalog.upscaleModels);
            collectCombo(doc, "UltralyticsDetectorProvider", "model_name", &catalog.detectors);
            // The embeddings are a separate request, and a server that answers none still has a catalog.
            exchange(client, "GET", "/embeddings", "", String(),
                [client, tag, catalog = move(catalog)](uint16_t embeddingStatus, PrStringView embeddings) mutable {
                    Document list;
                    if (embeddingStatus == 200 && list.parse(embeddings))
                        appendStrings(list, 0, &catalog.embeddings);
                    ComfyEvent event;
                    event.kind = ComfyEventKind_Replied;
                    event.tag = tag;
                    event.catalog = move(catalog);
                    deliver(client, move(event));
                });
        });
    });
}

void comfyFetchFolderPaths(ComfyClient* client, uint64_t tag, PrStringView folder) {
    whenOpen(client, tag, [client, tag, folder = String(folder)] {
        Function<bool(PrStringView, ComfyEvent*)> fill = [folder](PrStringView response, ComfyEvent* event) {
            Document doc;
            if (!doc.parse(response) || prJsonKind(doc.d, 0) != PrJsonKind_Object)
                return false;
            PrJsonValueIndex paths = prJsonMember(doc.d, 0, folder);
            appendStrings(doc, paths, &event->names);
            return true;
        };
        exchange(client, "GET", "/internal/folder_paths", "", String(),
            replyTo(client, tag, "The server did not report its folders.", move(fill)));
    });
}

void comfyFetchManagerVersion(ComfyClient* client, uint64_t tag) {
    whenOpen(client, tag, [client, tag] {
        exchange(client, "GET", "/v2/manager/version", "", String(),
            replyTo(client, tag, "The server has no ComfyUI-Manager.", [](PrStringView response, ComfyEvent* event) {
                PrStringView version = unquoted(response);
                if (version.isEmpty())
                    return false;
                event->name = version;
                return true;
            }));
    });
}

void comfyInstallModel(ComfyClient* client, uint64_t tag, ComfyModelInstall install) {
    whenOpen(client, tag, [client, tag, install = move(install)] {
        PrJsonWriter* w = prJsonWriterCreate();
        prJsonWriterBeginObject(w);
        const BodyMember members[] = {
            {"name", install.name},
            {"type", install.type},
            {"base", install.base},
            {"save_path", "default"},
            {"url", install.url},
            {"filename", install.filename},
            {"ui_id", randomHex(16)},
            {"client_id", client->clientId},
        };
        for (const BodyMember& member : members) {
            prJsonWriterKey(w, member.key);
            prJsonWriterString(w, member.value);
        }
        prJsonWriterEndObject(w);
        String body(prJsonWriterText(w));
        prJsonWriterDestroy(w);
        exchange(client, "POST", "/v2/manager/queue/install_model", "application/json", move(body),
            [client, tag](uint16_t status, PrStringView) {
                if (status != 200) {
                    refuse(client, tag, "ComfyUI-Manager did not queue the download.");
                    return;
                }
                // A worker that is already running answers 201, which leaves the task queued as a start would.
                exchange(client, "POST", "/v2/manager/queue/start", "application/json", "{}",
                    [client, tag](uint16_t started, PrStringView) {
                        if (started != 200 && started != 201) {
                            refuse(client, tag, "ComfyUI-Manager did not start its queue.");
                            return;
                        }
                        ComfyEvent event;
                        event.kind = ComfyEventKind_Replied;
                        event.tag = tag;
                        deliver(client, move(event));
                    });
            });
    });
}

void comfyFetchManagerStatus(ComfyClient* client, uint64_t tag) {
    whenOpen(client, tag, [client, tag] {
        Function<bool(PrStringView, ComfyEvent*)> fill = [](PrStringView response, ComfyEvent* event) {
            Document doc;
            if (!doc.parse(response))
                return false;
            event->manager.total = clampCount(doc.number(0, "total_count", 0));
            event->manager.done = clampCount(doc.number(0, "done_count", 0));
            event->manager.inProgress = clampCount(doc.number(0, "in_progress_count", 0));
            event->manager.pending = clampCount(doc.number(0, "pending_count", 0));
            event->manager.processing = prJsonKind(doc.d, doc.member(0, "is_processing")) == PrJsonKind_True;
            return true;
        };
        exchange(client, "GET", "/v2/manager/queue/status", "", String(),
            replyTo(client, tag, "ComfyUI-Manager did not report its queue.", move(fill)));
    });
}

bool comfyPoll(ComfyClient* client, ComfyEvent* out) {
    Lock lock(&client->mutex);
    if (client->taken == client->events.count()) {
        client->events.clear();
        client->taken = 0;
        return false;
    }
    *out = move(client->events[client->taken++]);
    return true;
}

} // namespace alchemy
