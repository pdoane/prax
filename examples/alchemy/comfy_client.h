// A client of a ComfyUI server: its REST endpoints over HTTP, and the events of its runs over a WebSocket.
//
// Every request is asynchronous. A call may be made from any thread: it posts the request to the network's loop, and
// the reply arrives later as an event, which the application takes from the client's queue with poll(). A request
// carries a tag that the caller chooses, which its reply repeats, so that the caller matches replies to what asked for
// them. The client calls its wake function, from the loop, whenever an event is queued.

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/function.h"
#include "pr/http.h"
#include "pr/net.h"
#include "pr/string.h"
#include "pr/websocket.h"

#include <stdint.h>

PR_ASSUME_NONNULL_BEGIN

namespace alchemy {

using namespace pr;

// The kinds of a client's events.
enum {
    ComfyEventKind_Connected,       // the WebSocket opened
    ComfyEventKind_Disconnected,    // the WebSocket closed or could not open; `message` says why
    ComfyEventKind_QueueStatus,     // `count` prompts wait in the server's queue
    ComfyEventKind_Progress,        // the prompt `promptId` sampled step `value` of `max` in node `node`
    ComfyEventKind_Executing,       // the prompt `promptId` reached node `node`
    ComfyEventKind_Preview,         // a frame of a run's latent preview, in `bytes`
    ComfyEventKind_ImageReady,      // the prompt `promptId` saved `filename`, whose bytes are `bytes`
    ComfyEventKind_ImageFailed,     // the image `filename` of the prompt `promptId` could not be fetched
    ComfyEventKind_Completed,       // the prompt `promptId` finished
    ComfyEventKind_Failed,          // the prompt `promptId` failed, as `message` says
    ComfyEventKind_Replied,         // the request `tag` succeeded; its reply is in the event's other members
    ComfyEventKind_Refused,         // the request `tag` failed, as `message` says
};
typedef uint8_t ComfyEventKind;

// The vocabularies that a server reports in /object_info and /embeddings. A vocabulary that the server does not report
// is empty.
struct ComfyCatalog {
    Array<String> checkpoints;
    Array<String> loras;
    Array<String> samplers;
    Array<String> schedulers;
    Array<String> upscaleModels;
    Array<String> detectors;
    Array<String> embeddings;
};

// The depth of ComfyUI-Manager's queue of tasks, across every client.
struct ComfyManagerStatus {
    uint32_t total = 0;
    uint32_t done = 0;
    uint32_t inProgress = 0;
    uint32_t pending = 0;
    bool processing = false;
};

// An event of a client. Each kind fills the members that its comment names. A reply fills what its request asked for:
// `promptId` for a submission, `name` for an upload, `catalog` for a catalog, `names` for a folder's directories,
// `name` for the manager's version, and `manager` for its queue.
struct ComfyEvent {
    ComfyEventKind kind = ComfyEventKind_Connected;
    uint64_t tag = 0;
    String promptId;
    String node;
    String filename;
    String message;
    String name;
    uint32_t value = 0;
    uint32_t max = 0;
    uint32_t count = 0;
    Array<uint8_t> bytes;
    Array<String> names;
    ComfyCatalog catalog;
    ComfyManagerStatus manager;
};

// A model download that ComfyUI-Manager queues: the file at `url`, stored as `filename` in the folder that `type`
// selects. `name` and `base` describe it in the manager's reports.
struct ComfyModelInstall {
    String name;
    String type;
    String base;
    String url;
    String filename;
};

// What a client connects with: the network whose loop runs it, and the sources of HTTP exchanges and WebSockets on that
// network, which must outlive the client.
struct ComfyClientDesc {
    PrNet* net;
    PrHttp* http;
    PrWebSocketNetwork* websockets;
    Function<void()> wake;
};

// A client of one server at a time.
struct ComfyClient;

// Creates a client, or frees one. Freeing waits for the network's loop to let go of the client, so it is not called on
// that loop.
ComfyClient* comfyClientCreate(const ComfyClientDesc& desc);
void comfyClientDestroy(ComfyClient* PR_NULLABLE client);

// Connects to the server at `origin`, such as "http://127.0.0.1:8188", replacing any connection. Delivers Connected or
// Disconnected. A request made before Connected waits for it.
void comfyConnect(ComfyClient* client, PrStringView origin);

// Closes the connection. Delivers Disconnected.
void comfyDisconnect(ComfyClient* client);

// Queues the API-format prompt `graph`, asking for latent previews. The reply's `promptId` names the prompt, which the
// run's events carry.
void comfySubmit(ComfyClient* client, uint64_t tag, String graph);

// Stops the prompt that the server is running.
void comfyInterrupt(ComfyClient* client);

// Uploads `bytes` as an input image named `filename`, replacing any of that name. The reply's `name` is what a
// LoadImage node's image input takes.
void comfyUpload(ComfyClient* client, uint64_t tag, PrStringView filename, Array<uint8_t> bytes);

// Reads the server's vocabularies into the reply's `catalog`.
void comfyFetchCatalog(ComfyClient* client, uint64_t tag);

// Reads the directories that the server searches for the model folder `folder` into the reply's `names`. They are the
// server's own paths, which a client on another machine cannot open.
void comfyFetchFolderPaths(ComfyClient* client, uint64_t tag, PrStringView folder);

// Asks for ComfyUI-Manager's version, into the reply's `name`. A refusal means that the server has no manager.
void comfyFetchManagerVersion(ComfyClient* client, uint64_t tag);

// Queues a model download with ComfyUI-Manager and starts its queue. The reply comes once the download is queued.
void comfyInstallModel(ComfyClient* client, uint64_t tag, ComfyModelInstall install);

// Reads ComfyUI-Manager's queue into the reply's `manager`.
void comfyFetchManagerStatus(ComfyClient* client, uint64_t tag);

// Moves the oldest queued event to `out`. Returns false if none is queued. May be called from any thread.
bool comfyPoll(ComfyClient* client, ComfyEvent* out);

} // namespace alchemy

PR_ASSUME_NONNULL_END
