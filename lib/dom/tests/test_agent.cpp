#include "pr/atomic.h"
#include "pr/dom.h"
#include "pr/string.h"
#include "pr/thread.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

struct Posts {
    PrDomAgent* agent;
    String ran;
    Atomic<uint32_t> wakes{};
};

void appendA(void* PR_NULLABLE context) {
    static_cast<Posts*>(context)->ran.append('a');
}

void appendB(void* PR_NULLABLE context) {
    static_cast<Posts*>(context)->ran.append('b');
}

void woken(void* PR_NULLABLE context) {
    static_cast<Posts*>(context)->wakes.fetchAdd(1);
}

void postFromThread(void* PR_NULLABLE context) {
    Posts* posts = static_cast<Posts*>(context);
    prDomAgentPost(posts->agent, appendA, posts);
    prDomAgentPost(posts->agent, appendB, posts);
}

void checkpoint(void* PR_NULLABLE context) {
    static_cast<Posts*>(context)->ran.append('c');
}

void update(void* PR_NULLABLE context) {
    static_cast<Posts*>(context)->ran.append('u');
}

void stopFromThread(void* PR_NULLABLE context) {
    prDomAgentStop(static_cast<Posts*>(context)->agent);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a task posted from another thread wakes the host and runs on the agent's thread in the order posted" *
        doctest::skip(!prThreadSupported())) {
    PrDomAgent* agent = prDomAgentCreate(nullptr);
    Posts posts{agent, ""};
    prDomAgentSetWake(agent, woken, &posts);
    CHECK(prDomAgentIsIdle(agent));
    PrThread thread{};
    prThreadStart(&thread, postFromThread, &posts);
    prThreadJoin(&thread);
    CHECK(posts.wakes.load() == 2);
    CHECK(posts.ran.isEmpty());
    CHECK(!prDomAgentIsIdle(agent));
    CHECK(prDomAgentNextTaskDelay(agent) == 0);
    CHECK(prDomAgentRunTask(agent));
    CHECK(posts.ran == "a");
    CHECK(prDomAgentRunTask(agent));
    CHECK(posts.ran == "ab");
    CHECK(!prDomAgentRunTask(agent));
    CHECK(prDomAgentIsIdle(agent));
    CHECK(prDomAgentNextTaskDelay(agent) == -1);
    prDomAgentDestroy(agent);
}

TEST_CASE("an agent runs the tasks posted to it that have not run when it is destroyed") {
    PrDomAgent* agent = prDomAgentCreate(nullptr);
    Posts posts{agent, ""};
    prDomAgentPost(agent, appendA, &posts);
    prDomAgentDestroy(agent);
    CHECK(posts.ran == "a");
}

TEST_CASE("an agent's loop waits for a posted task, runs it with its checkpoints, then updates, until it stops" *
        doctest::skip(!prThreadSupported())) {
    Posts posts{};
    posts.agent = prDomAgentCreate(nullptr);
    prDomAgentAddCheckpoint(posts.agent, checkpoint, &posts);
    prDomAgentAddUpdate(posts.agent, update, &posts);
    // A wait with nothing to run lasts its time.
    CHECK(prDomAgentWait(posts.agent, 5));
    PrThread thread{};
    prThreadStart(&thread, postFromThread, &posts);
    prThreadJoin(&thread);
    CHECK(prDomAgentWait(posts.agent, -1));
    prDomAgentRun(posts.agent, 1000);
    CHECK(posts.ran == "acbcu");
    // A removed hook is no longer called, and a run with no task still updates.
    prDomAgentRemoveCheckpoint(posts.agent, checkpoint, &posts);
    posts.ran.clear();
    prDomAgentRun(posts.agent, 1000);
    CHECK(posts.ran == "u");
    prThreadStart(&thread, stopFromThread, &posts);
    prThreadJoin(&thread);
    CHECK_FALSE(prDomAgentWait(posts.agent, -1));
    CHECK_FALSE(prDomAgentWait(posts.agent, 0));
    prDomAgentRemoveUpdate(posts.agent, update, &posts);
    prDomAgentDestroy(posts.agent);
}
