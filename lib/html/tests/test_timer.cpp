#include "pr/html.h"
#include "pr/html_page.h"

#include "doctest/doctest.h"

namespace pr_impl {

namespace {

struct State {
    int references = 0;
    int calls = 0;
};

PrHtmlTimerHandler handlerFor(State* state) {
    return PrHtmlTimerHandler{
        [](void* context) { ++static_cast<State*>(context)->calls; },
        state,
        [](void* context) { ++static_cast<State*>(context)->references; },
        [](void* context) { --static_cast<State*>(context)->references; },
    };
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an interval runs again with its id until it is cleared, by its own handler or from outside") {
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    PrHtmlWindow* window = prHtmlPageWindow(page);
    PrDomAgent* agent = prHtmlPageAgent(page);
    State state;
    PrHtmlTimerHandler handler = handlerFor(&state);
    int32_t outside = prHtmlWindowSetTimeout(window, &handler, 0, true);
    for (int i = 0; i < 3; ++i)
        CHECK(prDomAgentRunTask(agent));
    CHECK(state.calls == 3);
    CHECK(state.references == 1);
    prHtmlWindowClearTimeout(window, outside);
    CHECK(state.references == 0);
    CHECK_FALSE(prDomAgentRunTask(agent));

    // An interval whose handler clears it on its second run runs twice.
    struct Clearing {
        PrHtmlWindow* window;
        int32_t id;
        int calls;
    } clearing{window, 0, 0};
    auto clear = [](void* data) {
        Clearing* self = static_cast<Clearing*>(data);
        if (++self->calls == 2)
            prHtmlWindowClearTimeout(self->window, self->id);
    };
    PrHtmlTimerHandler clearer{clear, &clearing, nullptr, nullptr};
    clearing.id = prHtmlWindowSetTimeout(window, &clearer, 0, true);
    while (prDomAgentRunTask(agent)) {
    }
    CHECK(clearing.calls == 2);
    prHtmlPageDestroy(page);
}

TEST_CASE("a timer holds its handler until it fires or is cleared, and fires only when due and run") {
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    PrHtmlWindow* window = prHtmlPageWindow(page);
    State state;
    PrHtmlTimerHandler handler = handlerFor(&state);
    CHECK(prDomAgentNextTaskDelay(prHtmlPageAgent(page)) == -1);
    CHECK_FALSE(prDomAgentRunTask(prHtmlPageAgent(page)));
    int32_t future = prHtmlWindowSetTimeout(window, &handler, 100000, false);
    CHECK(future > 0);
    CHECK(prDomAgentNextTaskDelay(prHtmlPageAgent(page)) > 0);
    CHECK(prDomAgentNextTaskDelay(prHtmlPageAgent(page)) <= 100000);
    CHECK_FALSE(prDomAgentRunTask(prHtmlPageAgent(page)));
    int32_t zero = prHtmlWindowSetTimeout(window, &handler, 0, false);
    int32_t negative = prHtmlWindowSetTimeout(window, &handler, -10, false);
    CHECK(zero > 0);
    CHECK(negative > 0);
    CHECK(zero != future);
    CHECK(negative != zero);
    CHECK(state.references == 3);
    CHECK(state.calls == 0);
    CHECK(prDomAgentNextTaskDelay(prHtmlPageAgent(page)) == 0);
    prHtmlWindowClearTimeout(window, 0);
    prHtmlWindowClearTimeout(window, -1);
    prHtmlWindowClearTimeout(window, zero);
    prHtmlWindowClearTimeout(window, zero);
    CHECK(state.references == 2);
    CHECK(prDomAgentRunTask(prHtmlPageAgent(page)));
    CHECK(state.calls == 1);
    CHECK(state.references == 1);
    CHECK_FALSE(prDomAgentRunTask(prHtmlPageAgent(page)));
    prHtmlPageDestroy(page);
    CHECK(state.references == 0);
}

TEST_CASE("a timer's handler can clear and set timers, and cannot run one itself") {
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    PrHtmlWindow* window = prHtmlPageWindow(page);
    State state;
    PrHtmlTimerHandler callback = handlerFor(&state);
    struct Mutation {
        PrHtmlPage* page;
        PrHtmlWindow* window;
        PrHtmlTimerHandler* callback;
        int32_t cancelled;
    } mutation{page, window, &callback, 0};
    PrHtmlTimerHandler mutator{
        [](void* data) {
            Mutation* value = static_cast<Mutation*>(data);
            prHtmlWindowClearTimeout(value->window, value->cancelled);
            prHtmlWindowSetTimeout(value->window, value->callback, 0, false);
            CHECK_FALSE(prDomAgentRunTask(prHtmlPageAgent(value->page)));
        },
        &mutation,
        nullptr,
        nullptr,
    };
    prHtmlWindowSetTimeout(window, &mutator, 0, false);
    mutation.cancelled = prHtmlWindowSetTimeout(window, &callback, 0, false);
    CHECK(prDomAgentRunTask(prHtmlPageAgent(page)));
    CHECK(state.references == 1);
    CHECK(state.calls == 0);
    CHECK(prDomAgentRunTask(prHtmlPageAgent(page)));
    CHECK(state.references == 0);
    CHECK(state.calls == 1);
    CHECK_FALSE(prDomAgentRunTask(prHtmlPageAgent(page)));
    CHECK(prDomAgentNextTaskDelay(prHtmlPageAgent(page)) == -1);
    prHtmlPageDestroy(page);
}

TEST_CASE("a Window clears only its own timers, and a new document releases the old one's") {
    PrHtmlPage* first = prHtmlPageCreate(nullptr);
    PrHtmlPage* second = prHtmlPageCreate(nullptr);
    CHECK(prHtmlPageLoad(first, "<p>first", "about:blank", nullptr) == PrStatus_Ok);
    State state;
    PrHtmlTimerHandler handler = handlerFor(&state);
    PrHtmlWindow* window = prHtmlPageWindow(first);
    int32_t id = prHtmlWindowSetTimeout(window, &handler, 0, false);
    prHtmlWindowSetTimeout(prHtmlPageWindow(second), &handler, 0, false);
    CHECK(state.references == 2);
    prHtmlWindowClearTimeout(window, id);
    CHECK(state.references == 1);
    CHECK(prDomAgentRunTask(prHtmlPageAgent(second)));
    CHECK(state.references == 0);
    CHECK(state.calls == 1);
    prHtmlWindowSetTimeout(window, &handler, 100000, false);
    CHECK(state.references == 1);
    CHECK(prHtmlPageLoad(first, "<p>second", "about:blank", nullptr) == PrStatus_Ok);
    CHECK(state.references == 0);
    // What remains is the new document's reveal, at the next rendering opportunity.
    double delay = prDomAgentNextTaskDelay(prHtmlPageAgent(first));
    CHECK(delay >= 0);
    CHECK(delay <= 1000.0 / 60.0);
    prHtmlPageDestroy(first);
    prHtmlPageDestroy(second);
}

TEST_CASE("no timer runs while the parser is running a script") {
    State state;
    PrHtmlHost scripting = prHtmlHostDefault();
    scripting.context = &state;
    scripting.execute = [](void* data, PrHtmlPage* page, const PrHtmlScript*) -> PrStatus {
        State* value = static_cast<State*>(data);
        PrHtmlTimerHandler handler = handlerFor(value);
        prHtmlWindowSetTimeout(prHtmlPageWindow(page), &handler, 0, false);
        CHECK_FALSE(prDomAgentRunTask(prHtmlPageAgent(page)));
        CHECK(value->calls == 0);
        return PrStatus_Ok;
    };
    PrHtmlPage* page = prHtmlPageCreate(&scripting);
    CHECK(prHtmlPageLoad(page, "<script>0</script>", "about:blank", nullptr) == PrStatus_Ok);
    CHECK(prDomAgentRunTask(prHtmlPageAgent(page)));
    CHECK(state.calls == 1);
    CHECK(state.references == 0);
    prHtmlPageDestroy(page);
}

TEST_CASE("frame requests run together at a rendering opportunity, and a canceled one does not run") {
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    PrHtmlWindow* window = prHtmlPageWindow(page);
    struct Frame {
        State state;
        double time = -1;
    } first, second;
    auto callbackFor = [](Frame* frame) {
        return PrHtmlFrameRequestCallback{
            [](void* context, double now) {
                ++static_cast<Frame*>(context)->state.calls;
                static_cast<Frame*>(context)->time = now;
            },
            frame,
            [](void* context) { ++static_cast<Frame*>(context)->state.references; },
            [](void* context) { --static_cast<Frame*>(context)->state.references; },
        };
    };
    PrHtmlFrameRequestCallback a = callbackFor(&first);
    PrHtmlFrameRequestCallback b = callbackFor(&second);
    uint32_t kept = prHtmlWindowRequestAnimationFrame(window, &a);
    uint32_t canceled = prHtmlWindowRequestAnimationFrame(window, &b);
    CHECK(kept != canceled);
    CHECK(prDomAgentNextTaskDelay(prHtmlPageAgent(page)) >= 0);
    prHtmlWindowCancelAnimationFrame(window, canceled);
    CHECK(second.state.references == 0);
    while (!prDomAgentRunTask(prHtmlPageAgent(page))) {
    }
    CHECK(first.state.calls == 1);
    CHECK(first.time > 0);
    CHECK(second.state.calls == 0);
    CHECK(first.state.references == 0);
    CHECK(prDomAgentNextTaskDelay(prHtmlPageAgent(page)) == -1);
    prHtmlPageDestroy(page);
}

TEST_CASE("an iframe's tasks run on its parent's agent, until the iframe is removed") {
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    PrDomAgent* agent = prHtmlPageAgent(page);
    REQUIRE(prHtmlPageLoad(page, "<iframe id=f srcdoc='<p>inner'></iframe>", "about:blank", nullptr) == PrStatus_Ok);
    while (prDomAgentNextTaskDelay(agent) == 0 && prDomAgentRunTask(agent)) {
    }
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrDomElement* frame = prDomNodeGetElementById(prDomDocumentAsNode(document), "f");
    REQUIRE(frame != nullptr);
    PrHtmlWindow* inner =
        prHtmlIFrameElementContentWindow(prHtmlIFrameElementQuery(prDomNodeAsEventTarget(prDomElementAsNode(frame))));
    REQUIRE(inner != nullptr);

    // A timer of the iframe's window runs when the agent runs the parent's tasks.
    State state;
    PrHtmlTimerHandler handler = handlerFor(&state);
    prHtmlWindowSetTimeout(inner, &handler, 0, false);
    CHECK(prDomAgentNextTaskDelay(agent) == 0);
    CHECK_FALSE(prDomAgentIsIdle(agent));
    CHECK(prDomAgentRunTask(agent));
    CHECK(state.calls == 1);

    // A removed iframe's window is discarded, and its tasks no longer run.
    prHtmlWindowSetTimeout(inner, &handler, 0, false);
    prDomNodeRemoveChild(prDomNodeParentNode(prDomElementAsNode(frame)), prDomElementAsNode(frame));
    while (prDomAgentNextTaskDelay(agent) == 0 && prDomAgentRunTask(agent)) {
    }
    CHECK(state.calls == 1);
    prHtmlPageDestroy(page);
    CHECK(state.references == 0);
}
