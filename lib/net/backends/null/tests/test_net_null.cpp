#include "pr/net_null.h"
#include "pr/string.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

struct Ran {
    PrNet* net;
    String order;
};

void second(void* PR_NULLABLE context) {
    static_cast<Ran*>(context)->order.append('2');
}

void first(void* PR_NULLABLE context) {
    Ran* ran = static_cast<Ran*>(context);
    ran->order.append('1');
    prNetPost(ran->net, second, ran);
}

void connected(void*, PrNetStream*, PrNetStreamEvent) {}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the null network runs its tasks only when run, in order, with the tasks that they post") {
    PrNet* net = prNetNullCreate();
    Ran ran{net, ""};
    prNetPost(net, first, &ran);
    CHECK(ran.order.isEmpty());
    CHECK(prNetNullRun(net));
    CHECK(ran.order == "12");
    CHECK(!prNetNullRun(net));
    prNetDestroy(net);
}

TEST_CASE("the null network runs what is still posted when it is destroyed") {
    PrNet* net = prNetNullCreate();
    Ran ran{net, ""};
    prNetPost(net, first, &ran);
    prNetDestroy(net);
    CHECK(ran.order == "12");
}

TEST_CASE("the null network connects to nothing") {
    PrNet* net = prNetNullCreate();
    PrNetStream* stream = nullptr;
    PrNetConnectDesc desc{.host = "localhost", .port = 80, .event = connected};
    CHECK(prNetConnect(net, &desc, &stream) == PrStatus_NotSupported);
    CHECK(stream == nullptr);
    prNetDestroy(net);
}

