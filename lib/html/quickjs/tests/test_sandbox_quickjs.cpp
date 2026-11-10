// Sandboxed iframes, whose scripts an engine runs inside a document that runs none.

#include "pr/dom.h"
#include "pr/html.h"
#include "pr/html_page.h"
#include "pr/html_quickjs.h"
#include "pr/raw_string.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

// A page that runs no script, whose iframes' scripts an engine runs, and the messages that its window receives, as
// JSON, with their origins.
struct Host {
    PrJsHtmlEngine* engine;
    PrDomAgent* agent;
    PrHtmlPage* page = nullptr;
    std::vector<std::string> messages;
    std::vector<std::string> origins;

    Host() : engine(prJsHtmlEngineCreate(&kDesc)), agent(prDomAgentCreate(nullptr)) {
        PrHtmlHost host = prHtmlHostDefault();
        host.agent = agent;
        host.context = this;
        host.frame = frame;
        page = prHtmlPageCreate(&host);
    }

    ~Host() {
        prJsHtmlEngineDestroy(engine);
        prHtmlPageDestroy(page);
        prDomAgentDestroy(agent);
    }

    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    void load(PrStringView markup) {
        REQUIRE(prHtmlPageLoad(page, markup, "https://host.test/", nullptr) == PrStatus_Ok);
        PrDomEventListener listener{
            .invoke = [](void* PR_NULLABLE context, PrDomEvent* event) {
                Host* host = static_cast<Host*>(context);
                PrHtmlMessageEvent* message = prHtmlMessageEventQuery(event);
                PrRawString json{};
                if (message == nullptr || !prJsHtmlEngineMessageJson(host->engine,
                        prHtmlMessageEventSerializedData(message), &json))
                    return;
                PrStringView text = prRawStringView(&json);
                host->messages.emplace_back(text.data, text.size);
                PrStringView origin = prHtmlMessageEventOrigin(message);
                host->origins.emplace_back(origin.data, origin.size);
                prRawStringReset(&json);
            },
            .context = this,
            .identity = 1,
        };
        prDomEventTargetAddEventListener(prHtmlWindowAsEventTarget(prHtmlPageWindow(page)), "message", &listener, nullptr);
        settle();
    }

    void settle() {
        for (uint32_t i = 0; i < 100 && prDomAgentNextTaskDelay(agent) == 0; ++i) {
            prDomAgentRunTask(agent);
            prJsHtmlEngineCheckpoint(engine);
        }
    }

    // Every iframe, at any depth, has a host whose scripts the engine runs.
    static void frame(void* PR_NULLABLE context, PrHtmlPage*, PrHtmlHost* child) {
        child->context = context;
        child->frame = frame;
        *child = prJsHtmlEngineHost(static_cast<Host*>(context)->engine, child, PrJsHtmlScripting_Enabled);
    }

    static constexpr PrJsHtmlEngineDesc kDesc{};
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a sandboxed iframe that allows scripts runs them in a page that runs none, and posts to it as JSON") {
    Host host;
    host.load(R"(<iframe sandbox=allow-scripts srcdoc="<script>parent.postMessage({kind: 'hello', n: [1, 2]}, '*')</script>"></iframe>)");
    REQUIRE(host.messages.size() == 1);
    CHECK(host.messages[0] == R"({"kind":"hello","n":[1,2]})");
    // Without allow-same-origin, the frame's origin is opaque.
    CHECK(host.origins[0] == "null");
}

TEST_CASE("an iframe sandboxed from scripts runs none, though its host would run them") {
    Host host;
    host.load(R"(<iframe sandbox srcdoc="<script>parent.postMessage('ran', '*')</script>"></iframe>)"
        R"(<iframe sandbox=allow-same-origin srcdoc="<script>parent.postMessage('ran', '*')</script>"></iframe>)");
    CHECK(host.messages.empty());
}

TEST_CASE("an iframe sandboxed with allow-same-origin keeps its container's origin") {
    Host host;
    host.load(R"(<iframe sandbox="allow-scripts allow-same-origin" srcdoc="<script>parent.postMessage(origin, '*')</script>"></iframe>)");
    REQUIRE(host.messages.size() == 1);
    CHECK(host.messages[0] == R"("https://host.test")");
    CHECK(host.origins[0] == "https://host.test");
}

TEST_CASE("the sandbox of an iframe holds for the iframes inside it") {
    Host host;
    host.load(R"(<iframe sandbox=allow-scripts srcdoc="<iframe sandbox='allow-scripts allow-same-origin' srcdoc='<script>top.postMessage(origin, &quot;*&quot;)</script>'></iframe>"></iframe>)");
    REQUIRE(host.messages.size() == 1);
    CHECK(host.messages[0] == R"("null")");
}

TEST_CASE("a message whose value has no JSON reads as none") {
    Host host;
    host.load(R"(<iframe sandbox=allow-scripts srcdoc="<script>parent.postMessage(undefined, '*'); parent.postMessage(7, '*')</script>"></iframe>)");
    REQUIRE(host.messages.size() == 1);
    CHECK(host.messages[0] == "7");
}

TEST_CASE("a frame of another origin reaches its parent only through the members that every origin may use") {
    Host host;
    host.load(R"(<title>host</title><iframe sandbox=allow-scripts srcdoc="<script>
        const tries = [];
        for (const read of [() => parent.document, () => parent.location.href, () => parent.name, () => top.document])
            try { read(); tries.push('read'); } catch (e) { tries.push(e.name); }
        tries.push(parent === top, parent.self === parent, parent.length, parent.closed, parent.opener, parent.then);
        try { parent.foo = 1; tries.push('set'); } catch (e) { tries.push(e.name); }
        parent.postMessage(tries, '*');
    </script>"></iframe>)");
    REQUIRE(host.messages.size() == 1);
    CHECK(host.messages[0] ==
        R"(["SecurityError","SecurityError","SecurityError","SecurityError",true,true,1,false,null,null,"SecurityError"])");
}

TEST_CASE("an iframe's sandbox is a token list of the attribute, which supports its keywords") {
    Host host;
    host.load(R"(<iframe sandbox="allow-scripts allow-same-origin" srcdoc="<script>
        const frame = parent.document.querySelector('iframe');
        const list = frame.sandbox;
        list.add('allow-forms');
        parent.postMessage([list === frame.sandbox, frame.getAttribute('sandbox'), list.supports('allow-popups'),
            list.supports('allow-everything')], '*');
    </script>"></iframe>)");
    REQUIRE(host.messages.size() == 1);
    CHECK(host.messages[0] == R"([true,"allow-scripts allow-same-origin allow-forms",true,false])");
}

TEST_CASE("a frame of the same origin reaches all of its parent") {
    Host host;
    host.load(R"(<title>host</title><iframe sandbox="allow-scripts allow-same-origin" srcdoc="<script>
        parent.postMessage([parent.document.title, top.document.title], '*');
    </script>"></iframe>)");
    REQUIRE(host.messages.size() == 1);
    CHECK(host.messages[0] == R"(["host","host"])");
}
