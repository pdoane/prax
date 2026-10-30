#include "pr/http.h"
#include "pr/net_sockets.h"
#include "pr/websocket.h"

#include "examples/alchemy/comfy_client.h"
#include "tools/harness/net_peer.h"

#include "doctest/doctest.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

using namespace alchemy;
using namespace pr;
using namespace harness;

namespace {

// A client of the peer's fake ComfyUI server, and the events that it delivered.
struct Fixture {
    NetPeer peer{};
    PrNet* net = nullptr;
    PrHttp* http = nullptr;
    PrWebSocketNetwork* websockets = nullptr;
    ComfyClient* client = nullptr;
    std::mutex mutex;
    std::condition_variable woken;

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
                std::lock_guard<std::mutex> lock(mutex);
                woken.notify_all();
            },
        });
        return true;
    }

    ~Fixture() {
        comfyClientDestroy(client);
        if (net != nullptr) {
            prNetPostBlocking(net, [](void* PR_NULLABLE context) {
                Fixture* f = static_cast<Fixture*>(context);
                prWebSocketNetworkDestroy(f->websockets);
                prHttpDestroy(f->http);
            }, this);
            prNetDestroy(net);
        }
        netPeerReset(&peer);
    }

    std::string origin() const {
        return "http://127.0.0.1:" + std::to_string(peer.comfy);
    }

    // Waits for the next event of `kind`, and returns it. Events of other kinds before it are kept in `skipped`.
    ComfyEvent next(ComfyEventKind kind) {
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            ComfyEvent event;
            if (comfyPoll(client, &event)) {
                if (event.kind == kind)
                    return event;
                skipped.push_back(event.kind);
                continue;
            }
            std::unique_lock<std::mutex> lock(mutex);
            if (woken.wait_until(lock, deadline) == std::cv_status::timeout) {
                FAIL("no event of the kind waited for");
                return ComfyEvent{};
            }
        }
    }

    std::vector<ComfyEventKind> skipped;
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
    REQUIRE(catalog.catalog != nullptr);
    CHECK(catalog.catalog->checkpoints == std::vector<std::string>{"sd_xl_base_1.0.safetensors"});
    CHECK(catalog.catalog->samplers == std::vector<std::string>{"euler", "dpmpp_2m"});
    CHECK(catalog.catalog->schedulers == std::vector<std::string>{"normal", "karras"});
    CHECK(catalog.catalog->embeddings == std::vector<std::string>{"easynegative"});
    CHECK(catalog.catalog->detectors.empty());

    comfySubmit(f.client, 8, "{\"sampler\":{\"class_type\":\"KSampler\",\"inputs\":{}}}");
    ComfyEvent submitted = f.next(ComfyEventKind_Replied);
    CHECK(submitted.tag == 8);
    REQUIRE(!submitted.promptId.empty());
    ComfyEvent progress = f.next(ComfyEventKind_Progress);
    CHECK(progress.promptId == submitted.promptId);
    CHECK(progress.max == 2);
    CHECK(progress.node == "sampler");
    CHECK(!f.next(ComfyEventKind_Preview).bytes.empty());
    ComfyEvent image = f.next(ComfyEventKind_ImageReady);
    CHECK(image.filename == "Alchemy_00001_.png");
    CHECK(image.promptId == submitted.promptId);
    CHECK(image.bytes.size() > 8);
    CHECK(f.next(ComfyEventKind_Completed).promptId == submitted.promptId);

    comfySubmit(f.client, 9, "{}");
    ComfyEvent refused = f.next(ComfyEventKind_Refused);
    CHECK(refused.tag == 9);
    CHECK(refused.message.find("HTTP 400") != std::string::npos);
}

TEST_CASE("a client uploads images and reads folders, and refuses requests without a connection") {
    Fixture f;
    REQUIRE(f.open());
    comfyUpload(f.client, 1, "early.png", {1, 2, 3});
    CHECK(f.next(ComfyEventKind_Refused).tag == 1);

    comfyConnect(f.client, f.origin());
    // A request made while the socket connects waits for it.
    comfyUpload(f.client, 2, "source \"one\".png", {1, 2, 3, 4});
    ComfyEvent uploaded = f.next(ComfyEventKind_Replied);
    CHECK(uploaded.tag == 2);
    CHECK(uploaded.name == "source _one_.png");
    comfyFetchFolderPaths(f.client, 3, "checkpoints");
    CHECK(f.next(ComfyEventKind_Replied).names == std::vector<std::string>{"/models/checkpoints", "/more/checkpoints"});
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
    comfyConnect(f.client, "http://127.0.0.1:" + std::to_string(f.peer.closed));
    comfyFetchCatalog(f.client, 1);
    f.next(ComfyEventKind_Disconnected);
    CHECK(f.next(ComfyEventKind_Refused).tag == 1);
}
