#include "pr/array.h"
#include "pr/callback.h"
#include "pr/http.h"
#include "pr/lock.h"
#include "pr/net_sockets.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/thread.h"
#include "pr/time.h"
#include "pr/websocket.h"

#include "apps/alchemy/comfy_client.h"
#include "tools/harness/net_peer.h"

#include "tools/harness/test.h"

using namespace alchemy;
using namespace pr;
using namespace harness;

namespace {

// Returns the strings of `items`, for comparing with what a client read.
template <uint32_t N>
Array<String> strings(const char* const (&items)[N]) {
    Array<String> out;
    for (const char* item : items)
        out.push(item);
    return out;
}

// A client of the peer's fake ComfyUI server, and the events that it delivered.
struct Fixture {
    NetPeer peer{};
    PrNet* net = nullptr;
    PrHttp* http = nullptr;
    PrWebSocketNetwork* websockets = nullptr;
    ComfyClient* client = nullptr;
    PrMutex mutex;
    PrCondition woken;

    Fixture() {
        prMutexInit(&mutex);
        prConditionInit(&woken);
    }

    bool open() {
        if (!netPeerOpen(&peer)) {
            FAIL("the tests of alchemy's client need tools/harness/net_peer.py's servers: ./dev.sh test alchemy_comfy");
            return false;
        }
        net = prNetSocketsCreate();
        PrHttpDesc httpDesc{.net = net};
        http = prHttpCreate(PrHttpBackend_Default, &httpDesc);
        PrWebSocketNetworkDesc socketDesc{.net = net};
        websockets = prWebSocketNetworkCreate(PrWebSocketNetworkBackend_Default, &socketDesc);
        client = comfyClientCreate({
            .net = net,
            .http = http,
            .websockets = websockets,
            .wake = [this] {
                Lock lock(&mutex);
                prConditionBroadcast(&woken);
            },
        });
        return true;
    }

    ~Fixture() {
        comfyClientDestroy(client);
        if (net != nullptr) {
            auto destroyOnLoop = [&] {
                prWebSocketNetworkDestroy(websockets);
                prHttpDestroy(http);
            };
            prNetPostBlocking(net, PR_SCOPED_CALLBACK(destroyOnLoop));
            prNetDestroy(net);
        }
        netPeerReset(&peer);
        prConditionReset(&woken);
        prMutexReset(&mutex);
    }

    String origin() const {
        return format("http://127.0.0.1:{}", peer.comfy);
    }

    // Waits for the next event of `kind`, and returns it. Events of other kinds before it are kept in `skipped`.
    ComfyEvent next(ComfyEventKind kind) {
        double deadline = prTimeMonotonicMilliseconds() + 10000;
        for (;;) {
            ComfyEvent event;
            if (comfyPoll(client, &event)) {
                if (event.kind == kind)
                    return event;
                skipped.push(event.kind);
                continue;
            }
            Lock lock(&mutex);
            if (!prConditionWaitFor(&woken, &mutex, deadline - prTimeMonotonicMilliseconds())) {
                FAIL("no event of the kind waited for");
                return ComfyEvent{};
            }
        }
    }

    Array<ComfyEventKind> skipped;
};

} // namespace

TEST_CASE("a client connects, reads the catalog and runs a prompt to its saved image") {
    Fixture f;
    REQUIRE(f.open());
    comfyConnect(f.client, f.origin());
    f.next(ComfyEventKind_Connected);
    CHECK(f.next(ComfyEventKind_QueueStatus).count == 0);

    comfyFetchCatalog(f.client, 7);
    ComfyEvent catalog = f.next(ComfyEventKind_Replied);
    CHECK(catalog.tag == 7);
    CHECK(catalog.catalog.checkpoints == strings({"sd_xl_base_1.0.safetensors"}));
    CHECK(catalog.catalog.samplers == strings({"euler", "dpmpp_2m"}));
    CHECK(catalog.catalog.schedulers == strings({"normal", "karras"}));
    CHECK(catalog.catalog.embeddings == strings({"easynegative"}));
    CHECK(catalog.catalog.detectors.isEmpty());

    comfySubmit(f.client, 8, "{\"sampler\":{\"class_type\":\"KSampler\",\"inputs\":{}}}");
    ComfyEvent submitted = f.next(ComfyEventKind_Replied);
    CHECK(submitted.tag == 8);
    REQUIRE(!submitted.promptId.isEmpty());
    ComfyEvent progress = f.next(ComfyEventKind_Progress);
    CHECK(progress.promptId == submitted.promptId);
    CHECK(progress.max == 2);
    CHECK(progress.node == "sampler");
    CHECK(!f.next(ComfyEventKind_Preview).bytes.isEmpty());
    ComfyEvent image = f.next(ComfyEventKind_ImageReady);
    CHECK(image.filename == "Alchemy_00001_.png");
    CHECK(image.promptId == submitted.promptId);
    CHECK(image.bytes.count() > 8);
    CHECK(f.next(ComfyEventKind_Completed).promptId == submitted.promptId);

    comfySubmit(f.client, 9, "{}");
    ComfyEvent refused = f.next(ComfyEventKind_Refused);
    CHECK(refused.tag == 9);
    CHECK(prStringContains(refused.message, "HTTP 400"));
}

TEST_CASE("a client uploads images and reads folders, and refuses requests without a connection") {
    Fixture f;
    REQUIRE(f.open());
    comfyUpload(f.client, 1, "early.png", Array<uint8_t>({1, 2, 3}));
    CHECK(f.next(ComfyEventKind_Refused).tag == 1);

    comfyConnect(f.client, f.origin());
    // A request made while the socket connects waits for it.
    comfyUpload(f.client, 2, "source \"one\".png", Array<uint8_t>({1, 2, 3, 4}));
    ComfyEvent uploaded = f.next(ComfyEventKind_Replied);
    CHECK(uploaded.tag == 2);
    CHECK(uploaded.name == "source _one_.png");
    comfyFetchFolderPaths(f.client, 3, "checkpoints");
    CHECK(f.next(ComfyEventKind_Replied).names == strings({"/models/checkpoints", "/more/checkpoints"}));
    comfyFetchManagerVersion(f.client, 4);
    CHECK(f.next(ComfyEventKind_Refused).tag == 4);

    comfyDisconnect(f.client);
    f.next(ComfyEventKind_Disconnected);
    comfyFetchCatalog(f.client, 5);
    CHECK(f.next(ComfyEventKind_Refused).tag == 5);
}

TEST_CASE("a client that cannot reach its server reports the disconnection") {
    Fixture f;
    REQUIRE(f.open());
    comfyConnect(f.client, format("http://127.0.0.1:{}", f.peer.closed));
    comfyFetchCatalog(f.client, 1);
    f.next(ComfyEventKind_Disconnected);
    CHECK(f.next(ComfyEventKind_Refused).tag == 1);
}
