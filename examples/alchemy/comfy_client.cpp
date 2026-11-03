// A client of a ComfyUI server, over the HTTP exchanges and WebSockets of one network.

#include "examples/alchemy/comfy_client.h"

#include "pr/json.h"
#include "pr/random.h"
#include "pr/string_ops.h"

#include "examples/alchemy/text.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace alchemy {

using namespace pr;

// One REST exchange in flight, and what to do with its response. `done` receives the response's status, or zero if the
// exchange failed, and its body.
struct ComfyRequest {
    ComfyClient* client;
    PrHttpExchange* PR_NULLABLE exchange;
    std::string method;
    std::string url;
    std::string contentType;
    std::string body;
    std::string response;
    std::function<void(uint16_t status, const std::string& body)> done;
};

struct ComfyClient {
    PrNet* net;
    PrHttp* http;
    PrWebSocketNetwork* websockets;
    std::function<void()> wake;

    // The events for the application, which any thread may take.
    std::mutex mutex;
    std::deque<ComfyEvent> events;

    // The state of the connection, which only the loop reads and writes.
    std::string origin;         // the server's HTTP origin, such as "http://127.0.0.1:8188"
    std::string clientId;       // the id that the server sends this client's runs' events under
    PrWebSocket* PR_NULLABLE socket;
    bool open;

    // The requests made while the socket connects, by the tags that their replies carry.
    std::vector<std::pair<uint64_t, std::function<void()>>> waiting;

    // The REST exchanges in flight.
    std::unordered_set<ComfyRequest*> requests;

    // The image fetches in flight by prompt, and the prompts whose end waits for them, so that a prompt's images are
    // delivered before its end.
    std::unordered_map<std::string, uint32_t> fetching;
    std::unordered_set<std::string> ending;
};

namespace {

// Runs `fn` on the client's loop, after the tasks posted before it.
void post(ComfyClient* client, std::function<void()> fn) {
    std::function<void()>* task = new std::function<void()>(std::move(fn));
    prNetPost(client->net, [](void* PR_NULLABLE context) {
        std::function<void()>* run = static_cast<std::function<void()>*>(context);
        (*run)();
        delete run;
    }, task);
}

void deliver(ComfyClient* client, ComfyEvent event) {
    {
        std::lock_guard<std::mutex> lock(client->mutex);
        client->events.push_back(std::move(event));
    }
    if (client->wake)
        client->wake();
}

void refuse(ComfyClient* client, uint64_t tag, std::string message) {
    ComfyEvent event;
    event.kind = ComfyEventKind_Refused;
    event.tag = tag;
    event.message = std::move(message);
    deliver(client, std::move(event));
}

std::string randomHex(size_t bytes) {
    std::vector<uint8_t> random(bytes);
    prRandomBytes(random.data(), random.size());
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (uint8_t byte : random) {
        out += kHex[byte >> 4];
        out += kHex[byte & 15];
    }
    return out;
}

// Returns `text` percent-encoded for a URL's query, with every byte but the unreserved characters encoded.
std::string percentEncode(PrStringView text) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (char c : text) {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '.' || c == '_' || c == '~';
        if (unreserved) {
            out += c;
        } else {
            out += '%';
            out += kHex[uint8_t(c) >> 4];
            out += kHex[uint8_t(c) & 15];
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
    std::string string(PrJsonValueIndex object, const char* key) const {
        return stringOf(prJsonString(d, prJsonMember(d, object, key)));
    }
    double number(PrJsonValueIndex object, const char* key, double fallback) const {
        PrJsonValueIndex value = prJsonMember(d, object, key);
        return prJsonKind(d, value) == PrJsonKind_Number ? prJsonNumber(d, value, fallback) : fallback;
    }
    PrJsonValueIndex member(PrJsonValueIndex object, const char* key) const {
        return prJsonMember(d, object, key);
    }
};

void appendStrings(const Document& doc, PrJsonValueIndex array, std::vector<std::string>* out) {
    for (PrJsonValueIndex i = prJsonFirst(doc.d, array); i != kPrJsonMissing; i = prJsonAfter(doc.d, array, i)) {
        if (prJsonKind(doc.d, i) == PrJsonKind_String)
            out->push_back(stringOf(prJsonString(doc.d, i)));
    }
}

// Appends the options of a node's combo input, which the server reports as `[[options...], {...}]` or as `["COMBO",
// {"options": [...]}]`. A node or an input that the server does not report appends nothing.
void collectCombo(const Document& doc, const char* nodeClass, const char* input, std::vector<std::string>* out) {
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
        request->response.append(chunk, got);
    if (event != PrHttpEvent_End && event != PrHttpEvent_Fail)
        return;
    uint16_t status = event == PrHttpEvent_End ? prHttpExchangeStatus(exchange) : 0;
    prHttpExchangeDestroy(exchange);
    request->client->requests.erase(request);
    request->done(status, request->response);
    delete request;
}

// Starts a REST exchange on the loop. `done` runs on the loop with the response.
void exchange(ComfyClient* client, std::string method, std::string path, std::string contentType, std::string body,
        std::function<void(uint16_t, const std::string&)> done) {
    ComfyRequest* request = new ComfyRequest{
        .client = client,
        .exchange = nullptr,
        .method = std::move(method),
        .url = client->origin + path,
        .contentType = std::move(contentType),
        .body = std::move(body),
        .done = std::move(done),
    };
    PrHttpHeader header{"Content-Type", request->contentType};
    PrHttpRequestDesc desc{
        .method = request->method,
        .url = request->url,
        .headers = request->contentType.empty() ? nullptr : &header,
        .headerCount = request->contentType.empty() ? 0u : 1u,
        .body = PrByteView{reinterpret_cast<const uint8_t*>(request->body.data()), request->body.size()},
        .event = requestEvent,
        .context = request,
    };
    if (prHttpExchangeCreate(client->http, &desc, &request->exchange) != PrStatus_Ok) {
        request->done(0, std::string());
        delete request;
        return;
    }
    client->requests.insert(request);
}

// Runs `start` on the loop once the connection is open, or refuses the request `tag` if there is no connection.
void whenOpen(ComfyClient* client, uint64_t tag, std::function<void()> start) {
    post(client, [client, tag, start = std::move(start)]() mutable {
        if (client->open)
            start();
        else if (client->socket != nullptr)
            client->waiting.push_back({tag, std::move(start)});
        else
            refuse(client, tag, "Not connected to a server.");
    });
}

// Returns the function that delivers the reply to the request `tag`: Replied with what `fill` reads from a 200
// response, or Refused with `failure`.
std::function<void(uint16_t, const std::string&)> replyTo(ComfyClient* client, uint64_t tag, std::string failure,
        std::function<bool(const std::string&, ComfyEvent*)> fill) {
    return [client, tag, failure = std::move(failure), fill = std::move(fill)](uint16_t status, const std::string& body) {
        ComfyEvent event;
        event.kind = ComfyEventKind_Replied;
        event.tag = tag;
        if (status == 200 && fill(body, &event))
            deliver(client, std::move(event));
        else
            refuse(client, tag, failure + (status != 0 ? " (HTTP " + std::to_string(status) + ")" : ""));
    };
}

// Delivers the end of `promptId`, or holds it until the prompt's image fetches finish.
void endPrompt(ComfyClient* client, const std::string& promptId) {
    if (client->fetching.count(promptId) != 0) {
        client->ending.insert(promptId);
        return;
    }
    ComfyEvent event;
    event.kind = ComfyEventKind_Completed;
    event.promptId = promptId;
    deliver(client, std::move(event));
}

void fetchImage(ComfyClient* client, std::string promptId, std::string filename, std::string subfolder, std::string type) {
    ++client->fetching[promptId];
    std::string path = "/view?filename=" + percentEncode(filename);
    if (!subfolder.empty())
        path += "&subfolder=" + percentEncode(subfolder);
    path += "&type=" + percentEncode(type);
    exchange(client, "GET", path, "", "", [client, promptId, filename](uint16_t status, const std::string& body) {
        ComfyEvent event;
        event.kind = status == 200 ? ComfyEventKind_ImageReady : ComfyEventKind_ImageFailed;
        event.promptId = promptId;
        event.filename = filename;
        if (status == 200)
            event.bytes.assign(body.begin(), body.end());
        deliver(client, std::move(event));
        if (--client->fetching[promptId] != 0)
            return;
        client->fetching.erase(promptId);
        if (client->ending.erase(promptId) != 0)
            endPrompt(client, promptId);
    });
}

// Reads a message of the server's event stream (the "type" and "data" of a run's progress) into events.
void handleMessage(ComfyClient* client, PrStringView text) {
    Document doc;
    if (!doc.parse(text))
        return;
    std::string type = doc.string(0, "type");
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
            if (!event.promptId.empty())
                endPrompt(client, event.promptId);
            return;
        }
        event.kind = ComfyEventKind_Executing;
        event.node = stringOf(prJsonString(doc.d, node));
    } else if (type == "executed") {
        PrJsonValueIndex images = doc.member(doc.member(data, "output"), "images");
        for (PrJsonValueIndex i = prJsonFirst(doc.d, images); i != kPrJsonMissing; i = prJsonAfter(doc.d, images, i)) {
            std::string filename = doc.string(i, "filename");
            std::string kind = doc.string(i, "type");
            if (!filename.empty())
                fetchImage(client, event.promptId, filename, doc.string(i, "subfolder"), kind.empty() ? "output" : kind);
        }
        return;
    } else if (type == "execution_error") {
        event.kind = ComfyEventKind_Failed;
        event.message = doc.string(data, "exception_message");
        if (event.message.empty())
            event.message = "The server could not run the prompt.";
    } else {
        return;
    }
    deliver(client, std::move(event));
}

void closeSocket(ComfyClient* client, std::string reason) {
    if (client->socket == nullptr)
        return;
    prWebSocketDestroy(client->socket);
    client->socket = nullptr;
    client->open = false;
    std::vector<std::pair<uint64_t, std::function<void()>>> waiting = std::move(client->waiting);
    client->waiting.clear();
    ComfyEvent event;
    event.kind = ComfyEventKind_Disconnected;
    event.message = std::move(reason);
    deliver(client, std::move(event));
    for (const std::pair<uint64_t, std::function<void()>>& request : waiting)
        refuse(client, request.first, "The connection to the server closed before the request was sent.");
}

void socketEvent(void* PR_NULLABLE context, PrWebSocket* socket, PrWebSocketEvent event) {
    ComfyClient* client = static_cast<ComfyClient*>(context);
    switch (event) {
        case PrWebSocketEvent_Open: {
            client->open = true;
            ComfyEvent connected;
            connected.kind = ComfyEventKind_Connected;
            deliver(client, std::move(connected));
            std::vector<std::pair<uint64_t, std::function<void()>>> waiting = std::move(client->waiting);
            client->waiting.clear();
            for (std::pair<uint64_t, std::function<void()>>& request : waiting)
                request.second();
            break;
        }
        case PrWebSocketEvent_Message: {
            PrWebSocketMessage message = prWebSocketMessage(socket);
            if (message.kind == PrWebSocketMessageKind_Text) {
                handleMessage(client, PrStringView(reinterpret_cast<const char*>(message.data.data),
                    message.data.size));
            } else if (message.data.size > 8) {
                // A preview frame: a 4-byte event type and a 4-byte image format, then the image.
                ComfyEvent preview;
                preview.kind = ComfyEventKind_Preview;
                preview.bytes.assign(message.data.data + 8, message.data.data + message.data.size);
                deliver(client, std::move(preview));
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

} // namespace

ComfyClient* comfyClientCreate(const ComfyClientDesc& desc) {
    return new ComfyClient{
        .net = desc.net,
        .http = desc.http,
        .websockets = desc.websockets,
        .wake = desc.wake,
        .clientId = randomHex(16),
        .socket = nullptr,
        .open = false,
    };
}

void comfyClientDestroy(ComfyClient* PR_NULLABLE client) {
    if (client == nullptr)
        return;
    // The loop lets go of the client's exchanges and socket, which its callbacks name, before the client is freed.
    std::mutex mutex;
    std::condition_variable released;
    bool done = false;
    post(client, [&] {
        for (ComfyRequest* request : client->requests) {
            prHttpExchangeDestroy(request->exchange);
            delete request;
        }
        client->requests.clear();
        prWebSocketDestroy(client->socket);
        client->socket = nullptr;
        std::lock_guard<std::mutex> lock(mutex);
        done = true;
        released.notify_one();
    });
    std::unique_lock<std::mutex> lock(mutex);
    released.wait(lock, [&] { return done; });
    delete client;
}

void comfyConnect(ComfyClient* client, const std::string& origin) {
    post(client, [client, origin] {
        closeSocket(client, "Connecting to another server.");
        client->origin = origin;
        while (!client->origin.empty() && client->origin.back() == '/')
            client->origin.pop_back();
        std::string url = client->origin;
        if (url.rfind("http", 0) == 0)
            url.replace(0, 4, "ws");
        url += "/ws?clientId=" + client->clientId;
        PrWebSocketDesc desc{.url = url, .event = socketEvent, .context = client};
        if (prWebSocketCreate(client->websockets, &desc, &client->socket) != PrStatus_Ok) {
            ComfyEvent event;
            event.kind = ComfyEventKind_Disconnected;
            event.message = "The server's address is not one that alchemy can connect to.";
            deliver(client, std::move(event));
        }
    });
}

void comfyDisconnect(ComfyClient* client) {
    post(client, [client] { closeSocket(client, "Disconnected."); });
}

void comfySubmit(ComfyClient* client, uint64_t tag, std::string graph) {
    whenOpen(client, tag, [client, tag, graph = std::move(graph)] {
        // A server streams previews only for a prompt that asks for them, whatever its own default.
        std::string body = "{\"prompt\":" + graph + ",\"client_id\":\"" + client->clientId +
            "\",\"extra_data\":{\"preview_method\":\"auto\"}}";
        exchange(client, "POST", "/prompt", "application/json", std::move(body),
            replyTo(client, tag, "The server did not accept the prompt.", [](const std::string& response, ComfyEvent* event) {
                Document doc;
                event->promptId = doc.parse(response) ? doc.string(0, "prompt_id") : std::string();
                return !event->promptId.empty();
            }));
    });
}

void comfyInterrupt(ComfyClient* client) {
    post(client, [client] {
        if (client->open)
            exchange(client, "POST", "/interrupt", "application/json", "{}", [](uint16_t, const std::string&) {});
    });
}

void comfyUpload(ComfyClient* client, uint64_t tag, std::string filename, std::vector<uint8_t> bytes) {
    whenOpen(client, tag, [client, tag, filename = std::move(filename), bytes = std::move(bytes)] {
        PrStringView payload(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        // The boundary must not occur in the payload, or a part would end early.
        std::string boundary;
        do {
            boundary = "----AlchemyComfyUi" + randomHex(8);
        } while (prStringContains(payload, boundary));
        std::string safeName;
        for (char c : filename)
            safeName += c == '"' || c == '\\' || uint8_t(c) < 0x20 || c == 0x7f ? '_' : c;
        std::string body = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"image\"; filename=\"" +
            safeName + "\"\r\nContent-Type: application/octet-stream\r\n\r\n";
        body.append(payload.begin(), payload.end());
        body += "\r\n--" + boundary + "\r\nContent-Disposition: form-data; name=\"overwrite\"\r\n\r\ntrue\r\n--" +
            boundary + "--\r\n";
        exchange(client, "POST", "/upload/image", "multipart/form-data; boundary=" + boundary, std::move(body),
            replyTo(client, tag, "The server did not accept the image.", [](const std::string& response, ComfyEvent* event) {
                Document doc;
                event->name = doc.parse(response) ? doc.string(0, "name") : std::string();
                return !event->name.empty();
            }));
    });
}

void comfyFetchCatalog(ComfyClient* client, uint64_t tag) {
    whenOpen(client, tag, [client, tag] {
        exchange(client, "GET", "/object_info", "", "", [client, tag](uint16_t status, const std::string& body) {
            Document doc;
            if (status != 200 || !doc.parse(body)) {
                refuse(client, tag, "The server did not report its models.");
                return;
            }
            std::shared_ptr<ComfyCatalog> catalog = std::make_shared<ComfyCatalog>();
            collectCombo(doc, "CheckpointLoaderSimple", "ckpt_name", &catalog->checkpoints);
            collectCombo(doc, "LoraLoader", "lora_name", &catalog->loras);
            collectCombo(doc, "KSampler", "sampler_name", &catalog->samplers);
            collectCombo(doc, "KSampler", "scheduler", &catalog->schedulers);
            collectCombo(doc, "UpscaleModelLoader", "model_name", &catalog->upscaleModels);
            collectCombo(doc, "UltralyticsDetectorProvider", "model_name", &catalog->detectors);
            // The embeddings are a separate request, and a server that answers none still has a catalog.
            exchange(client, "GET", "/embeddings", "", "",
                [client, tag, catalog](uint16_t embeddingStatus, const std::string& embeddings) {
                    Document list;
                    if (embeddingStatus == 200 && list.parse(embeddings))
                        appendStrings(list, 0, &catalog->embeddings);
                    ComfyEvent event;
                    event.kind = ComfyEventKind_Replied;
                    event.tag = tag;
                    event.catalog = catalog;
                    deliver(client, std::move(event));
                });
        });
    });
}

void comfyFetchFolderPaths(ComfyClient* client, uint64_t tag, std::string folder) {
    whenOpen(client, tag, [client, tag, folder = std::move(folder)] {
        exchange(client, "GET", "/internal/folder_paths", "", "", replyTo(client, tag, "The server did not report its folders.",
            [folder](const std::string& response, ComfyEvent* event) {
                Document doc;
                if (!doc.parse(response) || prJsonKind(doc.d, 0) != PrJsonKind_Object)
                    return false;
                appendStrings(doc, doc.member(0, folder.c_str()), &event->names);
                return true;
            }));
    });
}

void comfyFetchManagerVersion(ComfyClient* client, uint64_t tag) {
    whenOpen(client, tag, [client, tag] {
        exchange(client, "GET", "/v2/manager/version", "", "",
            replyTo(client, tag, "The server has no ComfyUI-Manager.", [](const std::string& response, ComfyEvent* event) {
                size_t start = response.find_first_not_of(" \t\r\n\"");
                size_t end = response.find_last_not_of(" \t\r\n\"");
                if (start == std::string::npos)
                    return false;
                event->name = response.substr(start, end - start + 1);
                return true;
            }));
    });
}

void comfyInstallModel(ComfyClient* client, uint64_t tag, ComfyModelInstall install) {
    whenOpen(client, tag, [client, tag, install = std::move(install)] {
        PrJsonWriter* w = prJsonWriterCreate();
        prJsonWriterBeginObject(w);
        const std::pair<const char*, std::string> members[] = {
            {"name", install.name},         {"type", install.type},           {"base", install.base},
            {"save_path", "default"},       {"url", install.url},             {"filename", install.filename},
            {"ui_id", randomHex(16)},       {"client_id", client->clientId},
        };
        for (const std::pair<const char*, std::string>& member : members) {
            prJsonWriterKey(w, member.first);
            prJsonWriterString(w, member.second);
        }
        prJsonWriterEndObject(w);
        std::string body = stringOf(prJsonWriterText(w));
        prJsonWriterDestroy(w);
        exchange(client, "POST", "/v2/manager/queue/install_model", "application/json", std::move(body),
            [client, tag](uint16_t status, const std::string&) {
                if (status != 200) {
                    refuse(client, tag, "ComfyUI-Manager did not queue the download.");
                    return;
                }
                     // A worker that is already running answers 201, which leaves the task queued as a start would.
                exchange(client, "POST", "/v2/manager/queue/start", "application/json", "{}",
                    [client, tag](uint16_t started, const std::string&) {
                        if (started != 200 && started != 201) {
                            refuse(client, tag, "ComfyUI-Manager did not start its queue.");
                            return;
                        }
                        ComfyEvent event;
                        event.kind = ComfyEventKind_Replied;
                        event.tag = tag;
                        deliver(client, std::move(event));
                    });
            });
    });
}

void comfyFetchManagerStatus(ComfyClient* client, uint64_t tag) {
    whenOpen(client, tag, [client, tag] {
        exchange(client, "GET", "/v2/manager/queue/status", "", "", replyTo(client, tag, "ComfyUI-Manager did not report its queue.",
            [](const std::string& response, ComfyEvent* event) {
                Document doc;
                if (!doc.parse(response))
                    return false;
                event->manager.total = clampCount(doc.number(0, "total_count", 0));
                event->manager.done = clampCount(doc.number(0, "done_count", 0));
                event->manager.inProgress = clampCount(doc.number(0, "in_progress_count", 0));
                event->manager.pending = clampCount(doc.number(0, "pending_count", 0));
                event->manager.processing = prJsonKind(doc.d, doc.member(0, "is_processing")) == PrJsonKind_True;
                return true;
            }));
    });
}

bool comfyPoll(ComfyClient* client, ComfyEvent* out) {
    std::lock_guard<std::mutex> lock(client->mutex);
    if (client->events.empty())
        return false;
    *out = std::move(client->events.front());
    client->events.pop_front();
    return true;
}

} // namespace alchemy
